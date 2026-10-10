#include "ReportView.hpp"
#include <Arcane/Base/ForeignModules.hpp>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <optional>

namespace Arcane::Reporter
{
    std::string PlainWordsKind(std::string_view kind)
    {
        if (kind == "crash")         return "crashed";
        if (kind == "hang")          return "stopped responding";
        if (kind == "gpu-stall")     return "the GPU stopped responding";
        if (kind == "gpu-crash")     return "the GPU device was lost";
        if (kind == "assert")        return "an assertion failed";
        if (kind == "terminate")     return "terminated";
        if (kind == "out-of-memory") return "ran out of memory";
        if (kind == "ensure")        return "hit a recoverable check";
        if (kind == "abnormal-exit") return "exited abnormally";
        return "stopped (" + std::string(kind) + ")";
    }

    std::string LastLines(std::string_view text, std::size_t n)
    {
        if (text.empty() || n == 0) return {};
        std::size_t end = text.size();
        if (text.back() == '\n') --end;             // a trailing newline does not count as a line
        std::size_t pos = end;
        for (std::size_t lines = 0; pos > 0; --pos)
        {
            if (text[pos - 1] == '\n' && ++lines == n) break;
        }
        return std::string(text.substr(pos));
    }

    namespace
    {
        std::string InjectedLines(const std::vector<std::string>& names)
        {
            std::string out;
            for (const std::string& name : names)
            {
                out += name;
                if (const auto m = ForeignModules::Classify(name))
                {
                    out += " -- " + m->product + " (tier " + std::to_string(m->tier) + ")";
                    if (!m->consequence.empty()) out += ": " + m->consequence;
                    if (!m->remedy.empty())      out += " -- " + m->remedy;
                }
                else
                {
                    out += " -- injected, uncatalogued";
                }
                out += "\n";
            }
            return out;
        }

        std::string GpuLines(const Diag::Envelope& e)
        {
            std::string out;
            for (const auto& q : e.queues)
            {
                out += q.name + ": last completed " + (q.lastCompleted.empty() ? "<none>" : q.lastCompleted);
                if (!q.inFlight.empty())
                {
                    out += ", in flight ";
                    for (std::size_t i = 0; i < q.inFlight.size(); ++i) out += (i ? ", " : "") + q.inFlight[i];
                }
                out += "\n";
            }
            if (!e.fault.type.empty())
                out += "fault: " + e.fault.type + " at " + e.fault.address + (e.fault.resource.empty() ? "" : " (" + e.fault.resource + ")") + "\n";
            if (!e.activeLayers.empty())
            {
                out += "layers: ";
                for (std::size_t i = 0; i < e.activeLayers.size(); ++i) out += (i ? ", " : "") + e.activeLayers[i];
                out += "\n";
            }
            return out;
        }

        // The portable stack's first line is "--- thread <id> (MAIN)"; the label is the rest of that line.
        ThreadView PortableThread(std::string_view summary)
        {
            ThreadView t;
            const std::size_t nl = summary.find('\n');
            const std::string_view first = summary.substr(0, nl);
            t.label = first.rfind("--- ", 0) == 0 ? std::string(first.substr(4)) : "thread";
            t.text  = nl == std::string_view::npos ? "" : std::string(summary.substr(nl + 1));
            return t;
        }
    }

    ReportView BuildReportView(const Diag::Envelope& e, const Args& a, const Symbolized* sym, std::string_view logTail)
    {
        ReportView v;
        const std::string product = a.product.empty() ? (e.appName.empty() ? "Arcane" : e.appName) : a.product;
        const std::string words = PlainWordsKind(e.kind);
        v.title     = product + " -- " + words;
        v.headline  = product + " " + words;
        v.whenLine  = e.timestampUtc + (e.phase.empty() ? "" : " | phase: " + e.phase) + (e.buildInfo.empty() ? "" : " | build: " + e.buildInfo);
        v.reasonText = e.reason.empty() ? "(no reason recorded)" : e.reason;
        v.injectedText = InjectedLines(e.foreignModules);
        v.gpuText   = GpuLines(e);
        v.logTail   = std::string(logTail);
        v.reportFolder = std::filesystem::path(a.envelopePath).parent_path().generic_string();
        v.relaunchLine = a.relaunch.empty() ? e.commandLine : a.relaunch;
        v.isHang    = (e.kind == "hang" || e.kind == "gpu-stall") && e.exitCode == 0;
        v.isAbnormalExit = (e.kind == "abnormal-exit");
        v.canRelaunch = !v.relaunchLine.empty() && !v.isHang;

        if (sym && sym->engineAvailable && !sym->threads.empty())
        {
            for (const SymThread& t : sym->threads)
            {
                ThreadView tv;
                tv.label = "thread " + std::to_string(t.systemId) + (t.faulting ? " (faulting)" : "");
                char idx[8];
                for (std::size_t i = 0; i < t.frames.size(); ++i)
                {
                    std::snprintf(idx, sizeof(idx), "%02zu ", i);
                    tv.text += idx + FormatFrame(t.frames[i]) + "\n";
                }
                tv.frames = t.frames;
                v.threads.push_back(std::move(tv));
            }
        }
        else if (!e.cpuThreadSummary.empty())
        {
            v.threads.push_back(PortableThread(e.cpuThreadSummary));
        }
        return v;
    }

    std::string DetailsHeader(const ReportView& v)
    {
        return v.headline + "\n" + v.whenLine + "\n\nreason: " + v.reasonText + "\n";
    }

    std::string DetailsBody(const ReportView& v, std::size_t threadIndex)
    {
        std::string d;
        if (!v.injectedText.empty()) d += "\ninjected modules:\n" + v.injectedText;
        if (threadIndex < v.threads.size()) d += "\n--- " + v.threads[threadIndex].label + "\n" + v.threads[threadIndex].text;
        if (!v.gpuText.empty()) d += "\n=== GPU ===\n" + v.gpuText;
        if (!v.logTail.empty()) d += "\n=== log (tail) ===\n" + v.logTail;
        d += "\nreport folder: " + v.reportFolder + "\n";
        return d;
    }

    std::string DetailsText(const ReportView& v, std::size_t threadIndex)
    {
        return DetailsHeader(v) + DetailsBody(v, threadIndex);
    }

    std::string DisplayProduct(std::string_view appName)
    {
        constexpr std::string_view kArcane = "Arcane";
        if (appName.size() > kArcane.size() && appName.starts_with(kArcane) && appName[kArcane.size()] != ' ')
            return std::string(kArcane) + " " + std::string(appName.substr(kArcane.size()));
        return std::string(appName);
    }

    std::string_view CopyButtonLabel(CopyState state) noexcept
    {
        switch (state)
        {
            case CopyState::Copied: return "Copied";
            case CopyState::Failed: return "Copy failed";
            case CopyState::Idle:   break;
        }
        return "Copy Details";
    }

    std::vector<ReporterButton> VisibleButtons(const ReportView& v)
    {
        std::vector<ReporterButton> out{ ReporterButton::OpenFolder, ReporterButton::Copy };
        if (v.isHang) { out.push_back(ReporterButton::KeepWaiting); out.push_back(ReporterButton::Terminate); }
        if (!v.relaunchLine.empty()) out.push_back(ReporterButton::Relaunch);
        out.push_back(ReporterButton::Close);
        return out;
    }

    namespace
    {
        // Exactly "YYYY-MM-DDTHH:MM:SSZ" (Diagnostics.cpp's stamp shape) ->
        // the UTC instant, or nothing.
        std::optional<std::chrono::sys_seconds> ParseIsoUtc(std::string_view s)
        {
            if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
                s[13] != ':' || s[16] != ':' || s[19] != 'Z')
                return std::nullopt;
            // Digits only: from_chars would accept a '-' sign inside a field.
            const auto num = [&](std::size_t at, std::size_t len, int& out)
            {
                const char* b = s.data() + at;
                if (*b < '0' || *b > '9') return false;
                const auto r = std::from_chars(b, b + len, out);
                return r.ec == std::errc{} && r.ptr == b + len;
            };
            int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
            if (!num(0, 4, y) || !num(5, 2, mo) || !num(8, 2, d) || !num(11, 2, h) || !num(14, 2, mi) || !num(17, 2, sec))
                return std::nullopt;
            using namespace std::chrono;
            const year_month_day ymd{ year{ y }, month{ static_cast<unsigned>(mo) }, day{ static_cast<unsigned>(d) } };
            if (!ymd.ok() || h > 23 || mi > 59 || sec > 60) return std::nullopt;
            return sys_days{ ymd } + hours{ h } + minutes{ mi } + seconds{ sec };
        }
    }

    std::string FormatLocalStamp(std::string_view s, const TimeZone* zone)
    {
#if !defined(ARC_HAS_TZDB)
        (void)s; (void)zone;
        return {};
#else
        if (!zone)
            return {};
        const auto utc = ParseIsoUtc(s);
        if (!utc)
            return {};
        return std::format("{:%Y-%m-%d %H:%M}", zone->to_local(*utc));
#endif
    }

    std::string FormatSystemLocalStamp(std::string_view s)
    {
        const auto utc = ParseIsoUtc(s);
        if (!utc)
            return {};
        const std::time_t t = static_cast<std::time_t>(utc->time_since_epoch().count());
        std::tm local{};
#if defined(_WIN32)
        if (::localtime_s(&local, &t) != 0)
            return {};
#else
        if (!::localtime_r(&t, &local))
            return {};
#endif
        char buf[32] = {};
        if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local) == 0)
            return {};
        return buf;
    }
}
