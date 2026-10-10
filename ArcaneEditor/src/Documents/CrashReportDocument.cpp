#include "Documents/CrashReportDocument.hpp"

#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"   // LinkText (s4.7)
#include "Widgets/EditorFonts.hpp"     // MonoFont (s4.8)
#include "Widgets/IconsLucide.h"
#include "Settings/EditorDocumentUiSettings.hpp"
#include "Project/OsShell.hpp"   // ShellOpen / ShowInExplorer -- the one shell route (s4.6)

#include "FileText.hpp"   // ArcaneCrashReporter/src: Slurp
#include "LogTail.hpp"    // ResolveLogPath / ReadLogTail

#include <Arcane/Base/DiagnosticsSettings.hpp>   // diagnostics.logTailLines -- one tail length with the reporter
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/UiSettings.hpp>             // ui.copyFlashSeconds -- one flash with the reporter
#include <Arcane/Config/Settings.hpp>

#include <Arcane/Render/IGpuCrashBackend.hpp>   // Diag::ReadGpuDump / ParseGpuDump

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <system_error>

namespace Arcane::Editor
{
    CrashReportDocument::CrashReportDocument(std::filesystem::path path, Arcane::Diag::Envelope envelope,
                                             Services services)
        : m_path(std::move(path)), m_envelope(std::move(envelope)), m_services(std::move(services))
    {
        // Resolved ONCE here, never in Draw() (post-review fix): a moved or
        // copied reports folder leaves the envelope's ABSOLUTE paths stale,
        // so every sibling is resolved against disk exactly once, at load,
        // and the verdict (existing path or empty = missing) is what both
        // Draw() and any headless test observe. See the class header
        // comment and ResolveSibling's own doc comment.
        m_siblingTxtResolved     = ResolveSibling(m_envelope.siblingTxt, m_path);
        m_siblingDmpResolved     = ResolveSibling(m_envelope.siblingDmp, m_path);
        m_siblingGpuDumpResolved = ResolveSibling(m_envelope.siblingGpuDump, m_path);

        // Parsed ONCE here, never in Draw(): the model half a headless test
        // observes (GpuDumpSectionTags) is exactly what gets rendered. Reads
        // the RESOLVED path (which may be the beside-.arcdiag fallback, not
        // the envelope's raw/possibly-stale one) -- a moved report folder
        // still yields its real section inventory.
        if (!m_siblingGpuDumpResolved.empty())
        {
            if (const auto dump = Arcane::Diag::ReadGpuDump(m_siblingGpuDumpResolved))
                for (const auto& section : dump->sections)
                    m_gpuDumpTags.push_back(section.tag);
        }

        // The title, the window label and the reporter model (s8.1) -- the
        // same call every later re-check (NoteMoved, NoteReopened, Tick) makes.
        LoadReport();
    }

    void CrashReportDocument::LoadReport()
    {
        namespace R = Arcane::Reporter;
        // R64: siblings are built with path +=, never string concatenation.
        const std::filesystem::path stem = m_path.parent_path() / m_path.stem();
        m_symbolizedPath = stem;
        m_symbolizedPath += ".symbolized.txt";

        std::error_code ec;
        m_symbolized.reset();
        if (std::filesystem::is_regular_file(m_symbolizedPath, ec))
            m_symbolized = R::ParseSymbolized(R::Slurp(m_symbolizedPath));

        // <stem>.log.txt, else the live log the envelope recorded, else empty:
        // ResolveLogPath is the one home of that order (ReadLogTail uses it too).
        const std::filesystem::path livePath(m_envelope.logPath);
        m_logResolved = R::ResolveLogPath(stem, livePath);

        R::Args args;
        args.product      = R::DisplayProduct(m_envelope.appName);
        args.envelopePath = m_path.string();
        m_view = R::BuildReportView(m_envelope, args, m_symbolized ? &m_symbolized->sym : nullptr,
                                    R::ReadLogTail(stem, livePath, Arcane::Settings<Arcane::DiagnosticsSettings>().logTailLines));

        m_frameFileExists.assign(m_view.threads.size(), {});
        for (std::size_t t = 0; t < m_view.threads.size(); ++t)
            for (const R::SymFrame& f : m_view.threads[t].frames)
            {
                std::error_code fe;
                m_frameFileExists[t].push_back(!f.file.empty() && std::filesystem::exists(f.file, fe));
            }
        if (m_threadIndex >= m_view.threads.size()) m_threadIndex = 0;

        const R::TimeZone* zone = nullptr;
#if defined(ARC_HAS_TZDB)
        try { zone = std::chrono::current_zone(); } catch (...) { zone = nullptr; }   // no tzdb: fall back to the stem
#endif
#if defined(ARC_HAS_TZDB)
        const std::string stamp = R::FormatLocalStamp(m_envelope.timestampUtc, zone);
#else
        // No tzdb (Apple libc++): the C library's local zone stands in.
        (void)zone;
        const std::string stamp = R::FormatSystemLocalStamp(m_envelope.timestampUtc);
#endif
        m_title = stamp.empty() ? m_path.stem().string() : m_view.headline + " -- " + stamp;
        m_windowLabel = m_title + "###crashdoc_" + m_envelope.guid.ToString();   // the id is unchanged
    }

    void CrashReportDocument::NoteReopened()
    {
        if (!m_symbolized) LoadReport();
    }

    void CrashReportDocument::Tick(double)
    {
        if (m_windowFocused && !m_wasFocused && !m_symbolized)
            LoadReport();
        m_wasFocused = m_windowFocused;
    }

    bool CrashReportDocument::OpenSource(const std::filesystem::path& file, int line) const
    {
        if (!m_services.openSourceAtLine) return false;
        m_services.openSourceAtLine(file, line);
        return true;
    }

    std::filesystem::path CrashReportDocument::ResolveSibling(const std::string& recorded,
                                                               const std::filesystem::path& docPath)
    {
        if (recorded.empty())
            return {};   // never produced this run -- not "missing", just absent

        const std::filesystem::path stored(recorded);
        std::error_code ec;
        if (std::filesystem::exists(stored, ec) && !ec)
            return stored;

        // Siblings are minted beside each other at the same stem
        // (Diagnostics.cpp:335-339 mints base = "<appName>-<stamp>-pid<N>"
        // and every sibling as `dir / (base + ext)`), so a whole reports
        // folder moved or copied together still has this file sitting right
        // beside the .arcdiag this document was opened from.
        const std::filesystem::path fallback = docPath.parent_path() / stored.filename();
        ec.clear();
        if (std::filesystem::exists(fallback, ec) && !ec)
            return fallback;

        return {};   // recorded, but neither location panned out
    }

    bool CrashReportDocument::IsEmptyQueueTimeline(const Arcane::Diag::Envelope::Queue& q) noexcept
    {
        return q.lastCompleted.empty() && q.inFlight.empty();
    }

    bool CrashReportDocument::IsNoiseFault(const Arcane::Diag::Envelope::Fault& f) noexcept
    {
        return f.type.empty() || f.type == "device-alive";
    }

    std::vector<const Arcane::Diag::Envelope::Queue*> CrashReportDocument::VisibleQueues() const
    {
        std::vector<const Arcane::Diag::Envelope::Queue*> out;
        for (const Arcane::Diag::Envelope::Queue& q : m_envelope.queues)
            if (!IsEmptyQueueTimeline(q))
                out.push_back(&q);
        return out;
    }

    // Controller ruling (s8.1 item 7 + s7.11): the title, the label (same id)
    // and the .symbolized.txt / .log.txt / view resolution move to the new stem
    // in one place.
    void CrashReportDocument::NoteMoved(const std::filesystem::path& p) { m_path = p; LoadReport(); }

    void CrashReportDocument::Draw(bool& requestClose)
    {
        namespace R = Arcane::Reporter;
        bool open = true;
        const CVarVec2 firstSize = Arcane::Settings<CrashViewerSettings>().initialSize;   // editor.crash.initialSize
        ImGui::SetNextWindowSize(ImVec2(firstSize.x, firstSize.y), ImGuiCond_FirstUseEver);
        const bool visible = ImGui::Begin(m_windowLabel.c_str(), &open, 0);
        ImGui::SetItemTooltip("%s", m_path.stem().string().c_str());   // the tab (or title bar) is LastItemData after Begin
        if (!visible)
        {
            m_windowFocused = false;
            ImGui::End();
            requestClose = !open;
            return;
        }
        m_windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const R::ReportView& v = m_view;

        // ---- header (no separators between fields; s8.1 item 5) ----------------
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(v.headline.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::TextDisabled("%s", v.whenLine.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(v.reasonText.c_str());
        ImGui::PopTextWrapPos();
        if (m_envelope.exitCode != 0)
            ImGui::Text("Exit code: %d (0x%08X)", m_envelope.exitCode, static_cast<unsigned>(m_envelope.exitCode));
        ImGui::Spacing();

        // ---- action row -----------------------------------------------------
        const bool flash = ImGui::GetTime() < m_copyFlashUntil;
        const float copyW = ImGui::CalcTextSize("Copy Details").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const std::string copyLabel = std::string(R::CopyButtonLabel(flash ? R::CopyState::Copied : R::CopyState::Idle)) + "###crashcopy";
        if (ImGui::Button(copyLabel.c_str(), ImVec2(copyW, 0.0f)))
        {
            ImGui::SetClipboardText(R::DetailsText(v, m_threadIndex).c_str());
            m_copyFlashUntil = ImGui::GetTime() + Arcane::Settings<Arcane::UiSettings>().copyFlashSeconds;
        }
        const auto shellOpen = [](const std::filesystem::path& p)
        {
            const auto r = OsShell::ShellOpen(p);
            if (r != OsShell::ShellResult::Ok)
                ARC_WARN("Crash report: could not open {} -- {}", p.string(), OsShell::Describe(r));
        };
        // Recorded + resolved = live; recorded + unresolved = disabled "(missing)"; never recorded = nothing.
        const auto openButton = [&](const char* label, bool recorded, const std::filesystem::path& resolved)
        {
            if (!recorded && resolved.empty()) return;
            ImGui::SameLine();
            if (resolved.empty())
            {
                ImGui::BeginDisabled();
                ImGui::Button(label);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(missing)");
            }
            else if (ImGui::Button(label))
                shellOpen(resolved);
        };
        openButton("Open .txt", !m_envelope.siblingTxt.empty(), m_siblingTxtResolved);
        openButton("Open .log", !m_envelope.logPath.empty(), m_logResolved);
        openButton("Open .symbolized.txt", false, HasSymbolized() ? m_symbolizedPath : std::filesystem::path{});
        openButton("Open .dmp", !m_envelope.siblingDmp.empty(), m_siblingDmpResolved);
        ImGui::SameLine();
        if (ImGui::Button("Show in Explorer"))
            if (const auto r = OsShell::ShowInExplorer(m_path); r != OsShell::ShellResult::Ok)
                ARC_WARN("Crash report: could not show {} -- {}", m_path.string(), OsShell::Describe(r));
        ImGui::Spacing();

        // ---- injected modules (classified in InjectedLines) ---------------------
        if (!v.injectedText.empty())
        {
            std::size_t start = 0;
            while (start < v.injectedText.size())
            {
                const std::size_t nl = v.injectedText.find('\n', start);
                const std::string line = v.injectedText.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                start = nl == std::string::npos ? v.injectedText.size() : nl + 1;
                if (line.empty()) continue;
                ImGui::TextColored(Theme::kWarning, ICON_LC_TRIANGLE_ALERT);
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Spacing();
        }

        // ---- stack -----------------------------------------------------------
        if (v.threads.empty())
            ImGui::TextDisabled("No stack recorded");
        else
        {
            if (v.threads.size() > 1)
            {
                ImGui::SetNextItemWidth(320.0f);
                if (ImGui::BeginCombo("##thread", v.threads[m_threadIndex].label.c_str()))
                {
                    for (std::size_t i = 0; i < v.threads.size(); ++i)
                        if (ImGui::Selectable(v.threads[i].label.c_str(), i == m_threadIndex)) m_threadIndex = i;
                    ImGui::EndCombo();
                }
            }
            if (m_symbolized && m_symbolized->sym.engineAvailable && !m_symbolized->sym.threads.empty())
                ImGui::TextDisabled("Symbolized (dbgeng)");
            else if (m_symbolized && m_symbolized->sym.engineAvailable)   // dbgeng ran but recovered no thread: BuildReportView fell back
                ImGui::TextDisabled("Portable stack (module+offset) -- the symbolizer recovered no threads");
            else if (m_symbolized)   // file present, engine was unavailable (case the spec leaves open)
                ImGui::TextDisabled("Portable stack (module+offset) -- symbolizer unavailable (%s)",
                                    m_symbolized->sym.engineError.c_str());
            else
                ImGui::TextDisabled("Portable stack (module+offset) -- %s not found",
                                    m_symbolizedPath.filename().string().c_str());

            const R::ThreadView& t = v.threads[m_threadIndex];
            const float lineH = ImGui::GetTextLineHeightWithSpacing();
            const float maxFramesH = lineH * static_cast<float>(Arcane::Settings<CrashViewerSettings>().maxRows);
            ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, maxFramesH));
            if (ImGui::BeginChild("##frames", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY))
            {
                MonoFont mono;
                const auto row = [&](int i, const std::string& copyText, const auto& body)
                {
                    ImGui::PushID(i);
                    const ImVec2 min = ImGui::GetCursorScreenPos();
                    body();
                    const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, ImGui::GetItemRectMax().y);
                    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max) &&
                        ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                        ImGui::OpenPopup("##framectx");
                    if (ImGui::BeginPopup("##framectx"))
                    {
                        if (ImGui::MenuItem("Copy line")) ImGui::SetClipboardText(copyText.c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                };
                if (!t.frames.empty())
                {
                    for (std::size_t i = 0; i < t.frames.size(); ++i)
                    {
                        const R::SymFrame& f = t.frames[i];
                        R::SymFrame head = f;
                        head.file.clear();
                        char idx[16];
                        std::snprintf(idx, sizeof(idx), "%02zu ", i);
                        const std::string text = idx + R::FormatFrame(head);
                        row(static_cast<int>(i), idx + R::FormatFrame(f), [&]
                        {
                            ImGui::TextUnformatted(text.c_str());
                            if (f.file.empty()) return;
                            ImGui::SameLine();
                            const bool live = m_frameFileExists[m_threadIndex][i];
                            const std::string link = "[" + f.file + ":" + std::to_string(f.line) + "]";
                            if (LinkText(link.c_str(), live))
                                (void)OpenSource(f.file, static_cast<int>(f.line));
                            if (!live) ImGui::SetItemTooltip("Not on this machine");
                        });
                    }
                }
                else
                {
                    std::size_t start = 0;
                    int i = 0;
                    while (start < t.text.size())
                    {
                        const std::size_t nl = t.text.find('\n', start);
                        const std::string line = t.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                        start = nl == std::string::npos ? t.text.size() : nl + 1;
                        row(i++, line, [&] { ImGui::TextUnformatted(line.c_str()); });
                    }
                }
            }
            ImGui::EndChild();
        }
        ImGui::Spacing();

        // ---- GPU (only when something is worth showing) -----------------------
        const std::vector<const Arcane::Diag::Envelope::Queue*> queues = VisibleQueues();
        if ((!queues.empty() || HasVisibleFault()) && ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (const Arcane::Diag::Envelope::Queue* q : queues)
            {
                ImGui::TextUnformatted(q->name.empty() ? "(unnamed queue)" : q->name.c_str());
                if (!q->lastCompleted.empty()) ImGui::BulletText("last completed: %s", q->lastCompleted.c_str());
                for (const std::string& scope : q->inFlight)
                    ImGui::TextColored(Theme::kAmber, "  in flight: %s", scope.c_str());
            }
            if (HasVisibleFault())
            {
                ImGui::Text("Fault: %s", m_envelope.fault.type.c_str());
                if (!m_envelope.fault.address.empty())  ImGui::Text("Address: %s", m_envelope.fault.address.c_str());
                if (!m_envelope.fault.resource.empty()) ImGui::Text("Resource: %s", m_envelope.fault.resource.c_str());
            }
            if (!m_envelope.activeLayers.empty())
            {
                std::string line;
                for (const std::string& layer : m_envelope.activeLayers) line += (line.empty() ? "" : "   ") + layer;
                ImGui::TextWrapped("Layers: %s", line.c_str());   // moved here from top level (drafting pick, 9.28)
            }
            if (!m_envelope.siblingGpuDump.empty())
            {
                if (m_siblingGpuDumpResolved.empty())
                    ImGui::TextDisabled(".gpudump (missing)");
                else if (ImGui::Button("Show .gpudump"))
                    if (const auto r = OsShell::ShowInExplorer(m_siblingGpuDumpResolved); r != OsShell::ShellResult::Ok)
                        ARC_WARN("Crash report: could not show {} -- {}", m_siblingGpuDumpResolved.string(), OsShell::Describe(r));
                if (!m_gpuDumpTags.empty())
                {
                    std::string inventory;
                    for (const std::string& tag : m_gpuDumpTags) inventory += (inventory.empty() ? "" : ", ") + tag;
                    ImGui::TextDisabled("Sections: %s", inventory.c_str());
                }
            }
        }

        // ---- log tail (closed by default) -----------------------------------------
        if (!m_view.logTail.empty() && ImGui::CollapsingHeader("Log (last 200 lines)"))
        {
            MonoFont mono;
            ImGui::InputTextMultiline("##logtail", m_view.logTail.data(), m_view.logTail.size() + 1,
                                      ImVec2(-1.0f, ImGui::GetTextLineHeight() *
                                                        static_cast<float>(Arcane::Settings<CrashViewerSettings>().textRows)),
                                      ImGuiInputTextFlags_ReadOnly);
        }

        ImGui::End();
        requestClose = !open;
    }
}
