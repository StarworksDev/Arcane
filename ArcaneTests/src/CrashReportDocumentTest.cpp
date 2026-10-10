// CrashReportDocument (GPU crash diagnostics arc, Task 10): the PURE model
// half -- construction from an already-loaded Diag::Envelope, the
// CPU-report-noise filtering rules (IsEmptyQueueTimeline/IsNoiseFault), and
// the .gpudump sibling's parsed section inventory -- driven headless (Draw
// is never called; ImGui is untouched by this file). Also pins the
// DocumentHost .arcdiag routing (mirrors EditorDocumentHostTest.cpp's
// .arcmat routing test) and the Asset Browser's Diagnostic classification
// (mirrors AssetBrowserTest.cpp's kind cases).

#include <catch2/catch_test_macros.hpp>
#include <Arcane/Platform/Platform.hpp>

#include "Documents/CrashReportDocument.hpp"
#include "Documents/DocumentHost.hpp"
#include "Panels/AssetPanelModel.hpp"
#include "SymbolizedText.hpp"
#include "FileText.hpp"

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

using Arcane::Diag::Envelope;
using Arcane::Editor::CrashReportDocument;
using Arcane::Editor::DocumentHost;
using Arcane::Editor::EditorDocument;

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d =
            std::filesystem::temp_directory_path() / "arcane_crashreportdoc_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }
}

TEST_CASE("CrashReportDocument headless construction from a Diag::WriteFile fixture exposes parsed fields",
          "[editor][diag]")
{
    const auto dir  = TempDir("construct");
    const auto path = dir / "ArcaneEditor-20260811-120000-pid4242.arcdiag";

    Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "gpu-crash";
    e.timestampUtc = "2026-08-11T12:00:00Z";
    e.appName = "ArcaneEditor";
    e.phase = "editor frame loop";
    e.buildInfo = "Debug@deadbeef";
    e.cpuThreadSummary = "Main thread: RenderSceneToViewport\n";
    // One real queue, plus Task 5's deferred CPU-report-noise shape (an
    // empty timeline on an otherwise-unrelated queue) -- both round-trip;
    // VisibleQueues() is what tells them apart.
    e.queues.push_back({ "direct", "pass:tonemap", { "pass:outline" } });
    e.queues.push_back({ "graphics", "", {} });
    e.fault = { "page-fault", "0xDEADBEEF0000", "ParticleIndices" };
    e.siblingTxt = "r.txt";
    e.siblingDmp = "r.dmp";
    e.activeLayers = { "breadcrumbs:pass", "dred:full" };

    REQUIRE(Arcane::Diag::WriteFile(e, path));
    const auto loaded = Arcane::Diag::ReadFile(path);
    REQUIRE(loaded.has_value());

    CrashReportDocument doc(path, *loaded);

    CHECK(doc.AssetGuid() == e.guid);
    CHECK(doc.Title().rfind("Arcane Editor the GPU device was lost -- ", 0) == 0);   // headline + local stamp (s8.1 item 7)
    CHECK_FALSE(doc.Dirty());
    CHECK(doc.Save());   // read-only: unreachable via Dirty()-gated paths, trivially "succeeds"
    CHECK(doc.Envelope().kind == "gpu-crash");
    CHECK(doc.Envelope().phase == "editor frame loop");
    CHECK(doc.Envelope().buildInfo == "Debug@deadbeef");
    CHECK(doc.Envelope().activeLayers.size() == 2);

    const auto visible = doc.VisibleQueues();
    REQUIRE(visible.size() == 1);
    CHECK(visible[0]->name == "direct");
    CHECK(visible[0]->lastCompleted == "pass:tonemap");

    CHECK(doc.HasVisibleFault());
    CHECK(doc.Envelope().fault.resource == "ParticleIndices");

    // No .gpudump sibling recorded on this fixture -> no parsed inventory.
    CHECK(doc.GpuDumpSectionTags().empty());

    // "r.txt" is recorded but no file exists at either resolved location
    // (not the literal string relative to cwd, not beside this .arcdiag) --
    // a "recorded but missing" verdict, distinct from "never recorded" (see
    // the dedicated sibling-resolution tests below for both cases in
    // isolation).
    CHECK(doc.ResolvedSiblingTxt().empty());
}

TEST_CASE("CrashReportDocument hides CPU-report noise: empty queue timelines and a device-alive/empty fault",
          "[editor][diag]")
{
    const Envelope::Queue emptyQueue{ "graphics", "", {} };
    CHECK(CrashReportDocument::IsEmptyQueueTimeline(emptyQueue));

    const Envelope::Queue realQueue{ "direct", "", { "pass:shadow" } };
    CHECK_FALSE(CrashReportDocument::IsEmptyQueueTimeline(realQueue));

    const Envelope::Fault noiseFault{ "device-alive", "", "" };
    CHECK(CrashReportDocument::IsNoiseFault(noiseFault));

    const Envelope::Fault emptyFault{};
    CHECK(CrashReportDocument::IsNoiseFault(emptyFault));

    const Envelope::Fault realFault{ "page-fault", "0x0", "Foo" };
    CHECK_FALSE(CrashReportDocument::IsNoiseFault(realFault));
}

TEST_CASE("CrashReportDocument parses its .gpudump sibling's section inventory at construction",
          "[editor][diag]")
{
    const auto dir         = TempDir("gpudump");
    const auto gpuDumpPath = dir / "r.gpudump";

    Arcane::Diag::GpuDumpWriter raw;
    raw.Add("markers", "abcd", 4);
    raw.Add("dred", std::string_view("breadcrumb-bytes"));
    REQUIRE(raw.Write(gpuDumpPath));

    Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "gpu-crash";
    e.siblingGpuDump = gpuDumpPath.string();

    CrashReportDocument doc(dir / "r.arcdiag", e);
    const auto& tags = doc.GpuDumpSectionTags();
    REQUIRE(tags.size() == 2);
    CHECK(tags[0] == "markers");
    CHECK(tags[1] == "dred");
}

TEST_CASE("CrashReportDocument.gpudump inventory stays empty when the sibling file is missing",
          "[editor][diag]")
{
    const auto dir = TempDir("gpudump_missing");

    Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "gpu-crash";
    e.siblingGpuDump = (dir / "does-not-exist.gpudump").string();

    CrashReportDocument doc(dir / "r.arcdiag", e);
    CHECK(doc.GpuDumpSectionTags().empty());
}

TEST_CASE("CrashReportDocument resolves a sibling beside its own .arcdiag when the recorded absolute path is stale",
          "[editor][diag]")
{
    // Simulates a moved/copied reports folder: the envelope's own recorded
    // path points somewhere that no longer exists, but the actual sibling
    // file sits right beside this document's .arcdiag (exactly where
    // WriteReportImpl minted it in the first place -- Diagnostics.cpp:335-339).
    const auto dir       = TempDir("sibling_resolve_beside");
    const auto docPath   = dir / "r.arcdiag";
    const auto besideTxt = dir / "r.txt";
    {
        std::ofstream out(besideTxt, std::ios::binary);
        out << "symbolized stack text";
    }

    Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "hang";
    e.siblingTxt = (dir / "does-not-exist-anymore" / "r.txt").string();   // stale

    CrashReportDocument doc(docPath, e);
    CHECK(doc.ResolvedSiblingTxt() == besideTxt);
}

TEST_CASE("CrashReportDocument sibling resolution yields a missing verdict when neither location exists",
          "[editor][diag]")
{
    const auto dir     = TempDir("sibling_resolve_missing");
    const auto docPath = dir / "r.arcdiag";

    Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "hang";
    // Recorded (non-empty -- a button would appear), but nothing exists at
    // the stored path OR beside the .arcdiag.
    e.siblingDmp = (dir / "does-not-exist-anymore" / "r.dmp").string();

    CrashReportDocument doc(docPath, e);
    CHECK_FALSE(e.siblingDmp.empty());
    CHECK(doc.ResolvedSiblingDmp().empty());
}

TEST_CASE("CrashReportDocument::ResolveSibling covers empty/stored/fallback/neither directly",
          "[editor][diag]")
{
    const auto dir     = TempDir("resolve_sibling_static");
    const auto docPath = dir / "report.arcdiag";

    // Never recorded -> always empty, regardless of docPath.
    CHECK(CrashReportDocument::ResolveSibling("", docPath).empty());

    // Recorded, and the stored path itself still exists -> used as-is.
    const auto storedPath = dir / "stored.txt";
    { std::ofstream out(storedPath, std::ios::binary); out << "x"; }
    CHECK(CrashReportDocument::ResolveSibling(storedPath.string(), docPath) == storedPath);

    // Recorded, stored path gone, but a same-named file sits beside docPath
    // -> the fallback.
    const auto besidePath = dir / "beside.txt";
    { std::ofstream out(besidePath, std::ios::binary); out << "x"; }
    const std::string staleStoredPath = (dir / "gone" / "beside.txt").string();
    CHECK(CrashReportDocument::ResolveSibling(staleStoredPath, docPath) == besidePath);

    // Recorded, neither location exists -> missing (empty).
    const std::string neitherPath = (dir / "gone" / "neither.txt").string();
    CHECK(CrashReportDocument::ResolveSibling(neitherPath, docPath).empty());
}

TEST_CASE("DocumentHost routes .arcdiag to CrashReportDocument and focuses instead of reopening",
          "[editor][diag]")
{
    const auto dir  = TempDir("host_route");
    const auto path = dir / "ArcaneEditor-20260811-120000-pid1.arcdiag";

    Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "hang";
    REQUIRE(Arcane::Diag::WriteFile(e, path));

    // Same factory+peek shape EditorApp.cpp registers for ".arcdiag" (beside
    // its ".arcmat" route) -- Diag::ReadFile stands in for LoadMaterialAsset.
    DocumentHost host;
    int factoryCalls = 0;
    const auto crashReportFactory =
        [&](const std::filesystem::path& p) -> std::unique_ptr<EditorDocument>
        {
            ++factoryCalls;
            auto envelope = Arcane::Diag::ReadFile(p);
            if (!envelope)
                return nullptr;
            return std::make_unique<CrashReportDocument>(p, std::move(*envelope));
        };
    const auto crashReportPeek =
        [](const std::filesystem::path& p) -> Arcane::Guid
        {
            const auto envelope = Arcane::Diag::ReadFile(p);
            return envelope ? envelope->guid : Arcane::Guid::Nil();
        };
    host.RegisterFactory(".arcdiag", crashReportFactory, crashReportPeek);

    EditorDocument* first = host.OpenPath(path);
    REQUIRE(first != nullptr);
    CHECK(host.Count() == 1);
    CHECK(first->AssetGuid() == e.guid);
    CHECK_FALSE(first->Dirty());
    CHECK(factoryCalls == 1);

    // Same file, uppercase extension (case-insensitive match, same as the
    // .arcmat routing test) -> the peek resolves the same guid and the open
    // document is focused instead of a second one being constructed.
    const auto upperExtPath = dir / (path.stem().string() + ".ARCDIAG");
#if !ARC_PLATFORM_WINDOWS
    // A case-SENSITIVE filesystem: on Windows the uppercase spelling names the
    // SAME file; here it is another name, so give it the same bytes (the same
    // guid) -- the routing + focus behaviour under test is unchanged. macOS's
    // default APFS is case-INSENSITIVE like Windows: the name already exists
    // (it is the same file) and copying a file onto itself is an error.
    if (!std::filesystem::exists(upperExtPath))
        std::filesystem::copy_file(path, upperExtPath, std::filesystem::copy_options::overwrite_existing);
#endif
    EditorDocument* again = host.OpenPath(upperExtPath);
    CHECK(again == first);
    CHECK(host.Count() == 1);

    // Unregistered extension -> null, nothing added.
    CHECK(host.OpenPath(dir / "not-a-report.txt") == nullptr);
    CHECK(host.Count() == 1);

    CHECK(host.FindByGuid(e.guid) == first);
    CHECK(host.FindByGuid(Arcane::Guid::Generate()) == nullptr);
}

TEST_CASE("AssetKindOf classifies .arcdiag as Diagnostic, case-insensitive", "[editor]")
{
    using namespace Arcane::Editor;
    CHECK(AssetKindOf("diag://reports/ArcaneEditor-20260811-120000-pid1234.arcdiag") == AssetKind::Diagnostic);
    CHECK(AssetKindOf("diag://reports/CRASH.ARCDIAG") == AssetKind::Diagnostic);
    CHECK(AssetKindOf("diag://reports/CRASH.ARCDIAG") != AssetKind::Other);
    CHECK(AssetKindOf("diag://reports/CRASH.ARCDIAG") != AssetKind::Data);
}

namespace
{
    std::filesystem::path Sibling(const std::filesystem::path& arcdiag, const char* suffix)
    {
        std::filesystem::path p = arcdiag.parent_path() / arcdiag.stem();
        p += suffix;
        return p;
    }

    Envelope CrashFixture(const std::filesystem::path& path)
    {
        Envelope e;
        e.guid = Arcane::Guid::Generate();
        e.kind = "crash";
        e.appName = "ArcaneEditor";
        e.timestampUtc = "2026-09-29T16:57:12Z";
        e.reason = "crash (unhandled exception)";
        e.cpuThreadSummary = "--- thread 77 (MAIN)\n00 ArcaneEditor.exe + 0x10\n";
        REQUIRE(Arcane::Diag::WriteFile(e, path));
        return e;
    }

    std::string SymbolizedFixture()
    {
        Arcane::Reporter::Symbolized s;
        s.engineAvailable = true;
        s.symbolPath = "srv*";
        s.threads.push_back({ 77, true, { Arcane::Reporter::SymFrame{ 0, "ArcaneEditor", "Boom", 0x1a, "D:\\dev\\Boom.cpp", 42 } } });
        s.threads.push_back({ 78, false, {} });
        return Arcane::Reporter::FormatSymbolized(s, "Debug@x", "");
    }
}

TEST_CASE("CrashReportDocument reads its .symbolized.txt and .log.txt siblings once, at construction", "[editor][diag]")
{
    const auto dir  = TempDir("symbolized");
    const auto path = dir / "ArcaneEditor-20260929-165712-pid77.arcdiag";
    CrashFixture(path);
    REQUIRE(Arcane::Reporter::WriteText(Sibling(path, ".symbolized.txt"), SymbolizedFixture()));
    REQUIRE(Arcane::Reporter::WriteText(Sibling(path, ".log.txt"), "first\nlast line\n"));

    CrashReportDocument doc(path, *Arcane::Diag::ReadFile(path));
    REQUIRE(doc.HasSymbolized());
    CHECK(doc.SymbolizedPath() == Sibling(path, ".symbolized.txt"));
    REQUIRE(doc.View().threads.size() == 2);
    CHECK(doc.View().threads[0].label == "thread 77 (faulting)");
    REQUIRE(doc.View().threads[0].frames.size() == 1);
    CHECK(doc.View().threads[0].frames[0].file == "D:\\dev\\Boom.cpp");
    CHECK(doc.View().threads[0].frames[0].line == 42);
    CHECK(doc.View().logTail.find("last line") != std::string::npos);
    CHECK(doc.LogPath() == Sibling(path, ".log.txt"));
    CHECK(doc.Title().rfind("Arcane Editor crashed -- ", 0) == 0);
}

TEST_CASE("CrashReportDocument falls back to the portable stack, then picks up a late .symbolized.txt on NoteReopened", "[editor][diag]")
{
    const auto dir  = TempDir("late_symbolized");
    const auto path = dir / "ArcaneEditor-20260929-165712-pid77.arcdiag";
    CrashFixture(path);

    CrashReportDocument doc(path, *Arcane::Diag::ReadFile(path));
    CHECK_FALSE(doc.HasSymbolized());
    REQUIRE(doc.View().threads.size() == 1);
    CHECK(doc.View().threads[0].label == "thread 77 (MAIN)");
    CHECK(doc.View().threads[0].frames.empty());
    CHECK(doc.LogPath().empty());

    REQUIRE(Arcane::Reporter::WriteText(Sibling(path, ".symbolized.txt"), SymbolizedFixture()));
    doc.NoteReopened();
    REQUIRE(doc.HasSymbolized());
    CHECK(doc.View().threads[0].label == "thread 77 (faulting)");
}

TEST_CASE("CrashReportDocument carries the openSourceAtLine service", "[editor][diag]")
{
    const auto dir  = TempDir("services");
    const auto path = dir / "ArcaneEditor-20260929-165712-pid77.arcdiag";
    CrashFixture(path);
    std::filesystem::path seenFile;
    int seenLine = 0;
    CrashReportDocument::Services services;
    services.openSourceAtLine = [&](const std::filesystem::path& f, int l) { seenFile = f; seenLine = l; };
    CrashReportDocument doc(path, *Arcane::Diag::ReadFile(path), services);
    REQUIRE(doc.OpenSource("D:\\dev\\Boom.cpp", 42));
    CHECK(seenFile == std::filesystem::path("D:\\dev\\Boom.cpp"));
    CHECK(seenLine == 42);

    // No service wired: OpenSource reports false and calls nothing.
    CrashReportDocument bare(path, *Arcane::Diag::ReadFile(path));
    CHECK_FALSE(bare.OpenSource("D:\\dev\\Boom.cpp", 7));
    CHECK(seenLine == 42);
}

TEST_CASE("CrashReportDocument: an engine-unavailable .symbolized.txt counts as present and keeps the portable stack", "[editor][diag]")
{
    const auto dir  = TempDir("unavailable_symbolized");
    const auto path = dir / "ArcaneEditor-20260929-165712-pid77.arcdiag";
    const Envelope e = CrashFixture(path);
    Arcane::Reporter::Symbolized s;
    s.engineError = "LoadLibrary(dbgeng.dll) failed (126)";
    REQUIRE(Arcane::Reporter::WriteText(Sibling(path, ".symbolized.txt"),
                                        Arcane::Reporter::FormatSymbolized(s, "Debug@x", e.cpuThreadSummary)));
    CrashReportDocument doc(path, *Arcane::Diag::ReadFile(path));
    REQUIRE(doc.HasSymbolized());
    REQUIRE(doc.View().threads.size() == 1);
    CHECK(doc.View().threads[0].label == "thread 77 (MAIN)");
    CHECK(doc.View().threads[0].frames.empty());
    doc.NoteReopened();   // present: nothing to re-check, same view
    CHECK(doc.View().threads[0].label == "thread 77 (MAIN)");
}

TEST_CASE("CrashReportDocument::NoteMoved reloads the title and the symbolized sibling at the new stem", "[editor][diag]")
{
    // Controller ruling (carry T6-A5): NoteMoved is `m_path = p; LoadReport();`
    // so a moved report keeps the headline title and re-keys its siblings.
    const auto dir  = TempDir("moved");
    const auto path = dir / "ArcaneEditor-20260929-165712-pid77.arcdiag";
    CrashFixture(path);
    REQUIRE(Arcane::Reporter::WriteText(Sibling(path, ".symbolized.txt"), SymbolizedFixture()));

    CrashReportDocument doc(path, *Arcane::Diag::ReadFile(path));
    REQUIRE(doc.HasSymbolized());

    const auto moved = dir / "Renamed-pid77.arcdiag";
    std::filesystem::rename(path, moved);
    std::filesystem::rename(Sibling(path, ".symbolized.txt"), Sibling(moved, ".symbolized.txt"));
    doc.NoteMoved(moved);

    CHECK(doc.Path() == moved);
    CHECK(doc.Title().rfind("Arcane Editor crashed -- ", 0) == 0);
    REQUIRE(doc.HasSymbolized());
    CHECK(doc.SymbolizedPath() == Sibling(moved, ".symbolized.txt"));
}
