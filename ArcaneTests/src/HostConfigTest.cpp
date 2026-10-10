// HostConfig::Parse round-trip. PRESENTATION-FREE. HostConfig now lives in the
// engine DLL (Arcane/Host/HostConfig.hpp, ARC_API) so this test exe links
// it via the "Arcane" link, not a source-compile.
#include <vector>
#include <Arcane/Platform/Platform.hpp>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Arcane/Host/HostConfig.hpp>

// --- stderr capture (see StderrCapture below for WHY this is here) ----------
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#if defined(_WIN32)
    #include <io.h>
    #define ARCTEST_DUP    _dup
    #define ARCTEST_DUP2   _dup2
    #define ARCTEST_CLOSE  _close
    #define ARCTEST_FILENO _fileno
#else
    #include <unistd.h>
    #define ARCTEST_DUP    dup
    #define ARCTEST_DUP2   dup2
    #define ARCTEST_CLOSE  close
    #define ARCTEST_FILENO fileno
#endif
namespace {
    Arcane::HostConfig::ParseOutcome Run(std::vector<std::string> args) {
        std::vector<char*> argv; argv.push_back(const_cast<char*>("ArcaneRuntime"));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        return Arcane::HostConfig::Parse(static_cast<int>(argv.size()), argv.data());
    }
}
TEST_CASE("HostConfig: --set repeats", "[host]") {
    const auto o = Run({ "--set", "game.speed=4", "--set", "cheats=1" });
    REQUIRE(o.config.has_value());
    REQUIRE(o.config->cvarSets.size() == 2);
    REQUIRE(o.config->cvarSets[0] == "game.speed=4");
    REQUIRE(o.config->cvarSets[1] == "cheats=1");
}

TEST_CASE("HostConfig: defaults", "[host]") {
    const auto o = Run({});
    REQUIRE(o.config.has_value());
    // D3D12 on Windows; Vulkan elsewhere, the only backend a non-Windows
    // build has (Arcane::kDefaultGraphicsBackend).
#if ARC_PLATFORM_WINDOWS
    REQUIRE(o.config->backend == Arcane::GraphicsBackend::D3D12);
#else
    REQUIRE(o.config->backend == Arcane::GraphicsBackend::Vulkan);
#endif
    REQUIRE(o.config->maxFrames == 0u);
    REQUIRE(o.config->vsync);
    REQUIRE_FALSE(o.config->perf);
    // --plugin defaults to EMPTY (was "Sandbox.dll"): each host supplies its own
    // fallback -- ArcaneRuntime -> Sandbox.dll, the editor -> no game loaded.
    REQUIRE(o.config->pluginPath.empty());
}
TEST_CASE("HostConfig: every flag maps", "[host]") {
    const auto o = Run({"--backend", "vulkan", "--frames", "180", "--no-vsync", "--perf", "--plugin", "Game.dll"});
    REQUIRE(o.config.has_value());
    REQUIRE(o.config->backend == Arcane::GraphicsBackend::Vulkan);
    REQUIRE(o.config->maxFrames == 180u);
    REQUIRE_FALSE(o.config->vsync);
    REQUIRE(o.config->perf);
    REQUIRE(o.config->pluginPath == "Game.dll");
}
TEST_CASE("HostConfig: --help exits 0, no config", "[host]") {
    const auto o = Run({"--help"});
    REQUIRE_FALSE(o.config.has_value()); REQUIRE(o.exitCode == 0);
}
TEST_CASE("HostConfig: --view-mode perspective parses (editor viewport seed, F4 plan 1 T7)", "[host]") {
    const auto o = Run({"--view-mode", "perspective"});
    REQUIRE(o.config.has_value());
    REQUIRE(o.config->viewMode == "perspective");
    const auto two = Run({"--view-mode", "2d"});
    REQUIRE(two.config.has_value());
    REQUIRE(two.config->viewMode == "2d");
    // Absent = no seed: the persisted [EditorViewport][Camera] mode stands.
    const auto absent = Run({});
    REQUIRE(absent.config.has_value());
    REQUIRE(absent.config->viewMode.empty());
}
TEST_CASE("HostConfig: --view-mode refuses any other spelling", "[host]") {
    const auto o = Run({"--view-mode", "sideways"});
    REQUIRE_FALSE(o.config.has_value()); REQUIRE(o.exitCode != 0);
}
TEST_CASE("HostConfig: bad arg exits 2, no config", "[host]") {
    const auto o = Run({"--backend", "metal"});
    REQUIRE_FALSE(o.config.has_value()); REQUIRE(o.exitCode == 2);
}
TEST_CASE("HostConfig parses --scene as a guid override", "[host]") {
    const auto o = Run({"--scene", "a5e0c1de-1111-4222-8333-444455556666"});
    REQUIRE(o.config.has_value());
    CHECK(o.config->sceneOverride == "a5e0c1de-1111-4222-8333-444455556666");
    // Absent flag = empty = manifest bootScene wins (today's behavior).
    const auto def = Run({});
    REQUIRE(def.config.has_value());
    CHECK(def.config->sceneOverride.empty());
}
#if !defined(ARC_BUILD_DIST)
// --nri-graph is a retired no-op (the NRI frame graph is the only render path,
// unconditionally) and is registered unconditionally, so the every-configuration
// claim belongs to the last case in this file. What THIS one pins, still inside
// the Dist guard, is the part that was never Dist-specific: a command line
// pairing --nri-graph with the rest of the run vocabulary parses cleanly.
TEST_CASE("host config: --nri-graph still composes with the run vocabulary", "[host][nri]") {
    const auto paired = Run({"--nri-graph", "--frames", "120", "--screenshot", "nri-graph.png",
                             "--backend", "vulkan", "--no-vsync"});
    REQUIRE(paired.config.has_value());
    CHECK(paired.config->maxFrames == 120u);
    CHECK(paired.config->screenshotPath == "nri-graph.png");
    CHECK(paired.config->backend == Arcane::GraphicsBackend::Vulkan);
    CHECK_FALSE(paired.config->vsync);
}
// HostConfig::Parse is shared by both hosts, so the obligation pinned here is
// that a SCRIPTED EDITOR RUN is a legal command line: the editor's own launch
// vocabulary (--project / --plugin / --scene) is disjoint from the runtime's
// usual one, and this is what proves the two compose.
//
// As with the case above, this is the only coverage available: the
// editor's boot lives in EditorApp.cpp, which is not compiled into this exe,
// and everything past the flag needs a window and a real device.
TEST_CASE("host config: --nri-graph composes with the EDITOR's launch vocabulary", "[host][nri]") {
    const auto editorRun = Run({"--nri-graph", "--project", "ReferenceProject",
                                "--scene", "a5e0c1de-1111-4222-8333-444455556666",
                                "--frames", "120", "--backend", "vulkan"});
    REQUIRE(editorRun.config.has_value());
    CHECK(editorRun.config->projectPath == "ReferenceProject");
    CHECK(editorRun.config->sceneOverride == "a5e0c1de-1111-4222-8333-444455556666");
    CHECK(editorRun.config->maxFrames == 120u);
    CHECK(editorRun.config->backend == Arcane::GraphicsBackend::Vulkan);

    // The engine-dev path (host a plugin with no project) is equally legal --
    // the editor tolerates having nothing to host, and the flag changes only
    // which recorder draws.
    const auto pluginRun = Run({"--nri-graph", "--plugin", "Game.dll"});
    REQUIRE(pluginRun.config.has_value());
    CHECK(pluginRun.config->pluginPath == "Game.dll");
    CHECK(pluginRun.config->projectPath.empty());

    // And --crash-gpu, which the editor now honours on BOTH of its render arms
    // (FireDeliberateGpuFault dispatches through NriDiagnostics::FireFault on
    // the graph flavor), still pairs with it.
    const auto faultRun = Run({"--nri-graph", "--project", "ReferenceProject",
                               "--crash-gpu", "30", "--frames", "60"});
    REQUIRE(faultRun.config.has_value());
    CHECK(faultRun.config->crashGpuFrame == 30u);
}

// --open-asset <guid> (hygiene pass 2026-09-28): the scripted document open.
TEST_CASE("host config: --open-asset round-trips and defaults empty", "[host]") {
    const auto plain = Run({"--project", "P", "--headless", "--frames", "1"});
    REQUIRE(plain.config.has_value());
    CHECK(plain.config->openAsset.empty());

    const auto opened = Run({"--project", "P", "--headless", "--frames", "1",
                             "--open-asset", "97260310-8b35-4b29-b12f-1fd6f8e99071"});
    REQUIRE(opened.config.has_value());
    CHECK(opened.config->openAsset == "97260310-8b35-4b29-b12f-1fd6f8e99071");
}

// --select-asset <guid> (inspector filters s6): the scripted Asset Browser
// selection -- its page shows in the Assets Inspector.
TEST_CASE("host config: --select-asset round-trips and defaults empty", "[host]") {
    const auto plain = Run({"--project", "P", "--headless", "--frames", "1"});
    REQUIRE(plain.config.has_value());
    CHECK(plain.config->selectAsset.empty());

    const auto selected = Run({"--project", "P", "--headless", "--frames", "1",
                               "--select-asset", "d7f389fd-f687-407d-b9d7-9753eb6b0258"});
    REQUIRE(selected.config.has_value());
    CHECK(selected.config->selectAsset == "d7f389fd-f687-407d-b9d7-9753eb6b0258");
}

// --select-in-document (inspector-ownership arc, spec A s4): the scripted
// in-document selection a golden needs to show a document page.
TEST_CASE("host config: --select-in-document round-trips and requires --open-asset", "[host]") {
    const auto plain = Run({"--project", "P", "--headless", "--frames", "1"});
    REQUIRE(plain.config.has_value());
    CHECK(plain.config->selectInDocument.empty());
    const auto both = Run({"--project", "P", "--headless", "--frames", "1",
                           "--open-asset", "97260310-8b35-4b29-b12f-1fd6f8e99071",
                           "--select-in-document", "Player/Jump"});
    REQUIRE(both.config.has_value());
    CHECK(both.config->selectInDocument == "Player/Jump");
    const auto orphan = Run({"--project", "P", "--headless", "--frames", "1",
                             "--select-in-document", "Player/Jump"});
    CHECK_FALSE(orphan.config.has_value());   // refused at parse: nothing to select inside
    CHECK(orphan.exitCode == 2);
}

// --hang-main N (crash window plan 2, D10): the witness hang lane's trigger.
// Parsed the same way --crash-gpu is, and honoured by both hosts.
TEST_CASE("host config: --hang-main round-trips and kHangMainSeconds is 15", "[host]") {
    const auto hangRun = Run({"--project", "P", "--headless",
                              "--hang-main", "30", "--frames", "90"});
    REQUIRE(hangRun.config.has_value());
    CHECK(hangRun.config->hangMainFrame == 30u);
    CHECK(Arcane::kHangMainSeconds == 15u);

    const auto off = Run({});
    REQUIRE(off.config.has_value());
    CHECK(off.config->hangMainFrame == 0u);
}
// The pick/outline probe. Guarded like --nri-graph
// (both are DEV scaffolding registered inside HostConfig.cpp's
// `#if !defined(ARC_BUILD_DIST)` block), and, like it, the parse round-trip is the
// ONLY coverage the flag can have: everything past it needs a window,
// a device and a scene.
//
// THE POINT of testing the refusals rather than just the happy path: the flag's
// whole value is being SCRIPTABLE, so a malformed coordinate that parsed to
// something plausible would report a confident id for a pixel nobody asked
// about -- worse than an error.
TEST_CASE("host config: --pick-probe round-trips x,y and defaults off", "[host][nri]") {
    const auto o = Run({"--nri-graph", "--frames", "120", "--pick-probe", "640,360"});
    REQUIRE(o.config.has_value());
    CHECK(o.config->pickProbe);
    CHECK(o.config->pickProbeX == 640);
    CHECK(o.config->pickProbeY == 360);

    // Absent flag = today's frame exactly: no pick node, no outline chain.
    const auto def = Run({"--nri-graph"});
    REQUIRE(def.config.has_value());
    CHECK_FALSE(def.config->pickProbe);
    CHECK(def.config->pickProbeX == 0);
    CHECK(def.config->pickProbeY == 0);

    // 0,0 is a legal probe (the canvas origin) and must not read as "absent".
    const auto origin = Run({"--nri-graph", "--frames", "10", "--pick-probe", "0,0"});
    REQUIRE(origin.config.has_value());
    CHECK(origin.config->pickProbe);
    CHECK(origin.config->pickProbeX == 0);
    CHECK(origin.config->pickProbeY == 0);

    // It composes with the rest of the vehicle's vocabulary, deliberately --
    // a probe run is an ordinary --nri-graph run with two extra nodes.
    const auto paired = Run({"--nri-graph", "--frames", "120", "--pick-probe", "12,34",
                             "--backend", "vulkan", "--no-vsync", "--screenshot", "probe.png"});
    REQUIRE(paired.config.has_value());
    CHECK(paired.config->pickProbe);
    CHECK(paired.config->pickProbeX == 12);
    CHECK(paired.config->pickProbeY == 34);
    CHECK(paired.config->backend == Arcane::GraphicsBackend::Vulkan);
    CHECK(paired.config->screenshotPath == "probe.png");
}
TEST_CASE("host config: --pick-probe refuses bad syntax at parse time", "[host][nri]") {
    const std::vector<std::string> bad[] = {
        {"--pick-probe", "640"},        // no comma at all
        {"--pick-probe", "640,"},       // nothing after it
        {"--pick-probe", ",360"},       // nothing before it
        {"--pick-probe", "640,360,1"},  // a third component is not a pixel
        {"--pick-probe", "a,b"},        // not numbers
        {"--pick-probe", "640, 360"},   // a space is not a digit -- refuse, do not trim
        {"--pick-probe", "-1,360"},     // negative: a typo, not a coordinate
        {"--pick-probe", "640,-1"},
        {"--pick-probe", "6.5,360"},    // fractional pixels are not a thing here
        {"--pick-probe", "99999999999,0"},   // past int32
    };
    for (const auto& args : bad) {
        std::vector<std::string> full{"--nri-graph", "--frames", "60"};
        full.insert(full.end(), args.begin(), args.end());
        const auto o = Run(full);
        REQUIRE_FALSE(o.config.has_value());
        CHECK(o.exitCode == 2);
    }
}
// --pick-probe does NOT require --nri-graph: the pick/outline nodes are always
// present, because the NRI frame graph is the only render path. It DOES still
// require --frames -- the readback lands a couple of frames after the pass that
// wrote it, so an open-ended run would exit on a window close having reported
// nothing at all.
TEST_CASE("host config: --pick-probe still requires --frames, no longer requires --nri-graph", "[host][nri]") {
    const auto noGraph = Run({"--frames", "60", "--pick-probe", "640,360"});
    REQUIRE(noGraph.config.has_value());
    CHECK(noGraph.config->pickProbe);
    CHECK(noGraph.config->pickProbeX == 640);
    CHECK(noGraph.config->pickProbeY == 360);

    const auto noFrames = Run({"--nri-graph", "--pick-probe", "640,360"});
    REQUIRE_FALSE(noFrames.config.has_value());
    CHECK(noFrames.exitCode == 2);

    // ...but neither refusal touches a run that simply did not pass the flag.
    const auto plain = Run({"--nri-graph"});
    REQUIRE(plain.config.has_value());
    CHECK_FALSE(plain.config->pickProbe);
}

// The refusal that keeps a CONFIDENTLY WRONG answer unreachable. An offscreen
// context declines to arm the fixed probe pixel by construction
// (NriGraphContext.cpp -- `m_pickArmed = config.pickProbe && !IsOffscreen()`),
// so the pick nodes are never built, no readback lands, and the run exits 1
// blaming a short run or an out-of-range pixel -- neither of which happened.
// Refuse the combination at parse time until the offscreen pick is driven per
// frame through FrameDesc::pickPixel.
TEST_CASE("host config: --pick-probe with --headless is refused at parse time", "[host][nri]") {
    const auto both = Run({"--headless", "--frames", "60", "--pick-probe", "640,360"});
    REQUIRE_FALSE(both.config.has_value());
    CHECK(both.exitCode == 2);

    // Neither flag alone is affected -- the refusal is about the PAIR, and a
    // refusal that leaked onto either one would silently retire a working
    // desk item.
    const auto probeOnly = Run({"--frames", "60", "--pick-probe", "640,360"});
    REQUIRE(probeOnly.config.has_value());
    CHECK(probeOnly.config->pickProbe);

    const auto headlessOnly = Run({"--headless", "--frames", "60"});
    REQUIRE(headlessOnly.config.has_value());
    CHECK(headlessOnly.config->headless);
    CHECK_FALSE(headlessOnly.config->pickProbe);
}
#endif
// UNGUARDED, deliberately: every case above this point that touches
// --nri-graph is Dist-excluded (several pair it with --pick-probe/--crash-gpu,
// which stay Dist-only), so none of it can show that --nri-graph itself
// parses in a Dist build. This case is the one that does: the flag is
// retired to a no-op rather than rejected -- scripts, the Hub's saved launch
// args and the desk batteries all still pass it, and a hard "unknown
// argument" refusal would turn a harmless no-op into a boot failure at the
// worst moment (a shipped Dist build, launched from a saved arg list nobody
// is watching). Proving it composes with real vocabulary (not just alone) is
// why --frames rides along rather than a bare flag.
TEST_CASE("host config: --nri-graph is accepted and ignored in every configuration", "[host][nri]") {
    const auto with    = Run({"--nri-graph", "--frames", "1"});
    const auto without = Run({"--frames", "1"});
    REQUIRE(with.config.has_value());
    REQUIRE(without.config.has_value());
    CHECK(with.exitCode == without.exitCode);
}

// --headless / --fixed-dt / --probe / --report. UNGUARDED (not behind
// ARC_BUILD_DIST): headless agent verification must work in Release and Dist,
// not just dev builds.
namespace {
    Arcane::HostConfig::ParseOutcome ParseArgs(std::vector<const char*> args)
    {
        return Arcane::HostConfig::Parse((int)args.size(), const_cast<char**>(args.data()));
    }
}

TEST_CASE("hostconfig: --headless collects probes and a report path", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--headless", "--frames", "5",
                                 "--probe", "luma@640,360", "--probe", "census",
                                 "--report", "r.json" });
    REQUIRE(out.exitCode == 0);
    REQUIRE(out.config.has_value());
    CHECK(out.config->headless);
    CHECK(out.config->reportPath == "r.json");
    REQUIRE(out.config->probes.size() == 2);
    CHECK(out.config->probes[0] == "luma@640,360");
}

TEST_CASE("hostconfig: --fixed-dt defaults to 1/60 under --headless", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--headless", "--frames", "1" });
    REQUIRE(out.exitCode == 0);
    CHECK(out.config->fixedDtSeconds == Catch::Approx(1.0 / 60.0));
}

TEST_CASE("hostconfig: --fixed-dt without --headless is REFUSED, not ignored", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--fixed-dt", "0.016" });
    CHECK(out.exitCode == 2);
    CHECK_FALSE(out.config.has_value());
}

// fixedDtSupplied (final fix wave, Fix 1): a caller-facing "was this flag
// typed" bit distinct from the resolved value, needed because the registered
// default (1/60) is itself a value a caller could pass on purpose --
// comparing the resolved double against its own default cannot tell the two
// apart. Consumed by ArcaneEditor/src/main.cpp to refuse --fixed-dt without
// misfiring on every ordinary run that never mentioned the flag.
TEST_CASE("hostconfig: fixedDtSupplied distinguishes an explicit default from an absent flag", "[hostconfig]")
{
    const auto absent = ParseArgs({ "h.exe", "--headless", "--frames", "1" });
    REQUIRE(absent.config.has_value());
    CHECK_FALSE(absent.config->fixedDtSupplied);

    const auto explicitDefault = ParseArgs({ "h.exe", "--headless", "--frames", "1",
                                              "--fixed-dt", "0.0166666666666666666" });
    REQUIRE(explicitDefault.config.has_value());
    CHECK(explicitDefault.config->fixedDtSupplied);
    CHECK(explicitDefault.config->fixedDtSeconds == Catch::Approx(1.0 / 60.0));
}

TEST_CASE("hostconfig: --probe without --frames is refused", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--headless", "--probe", "census" });
    CHECK(out.exitCode == 2);
}

// Fix 2 (final fix wave): a malformed --probe used to parse clean, run to
// completion, and exit 0 with the bad spec simply absent from the report --
// no error, no artifact. Refused HERE, at parse time, reusing ParseProbe's
// own error text (VerifyReport.hpp) rather than a second vocabulary for the
// same mistake. Caught before the --headless/--frames gates further down,
// so it fires even on a command line missing both of those too.
TEST_CASE("hostconfig: a malformed --probe is refused at parse time", "[hostconfig]")
{
    const std::vector<std::string> bad[] = {
        { "bogus@1,2" },        // unknown kind
        { "brightness" },       // positional kind missing its @x,y
        { "census@1,2" },       // argless kind given @x,y anyway
        { "luma@-1,2" },        // negative is a typo, not a coordinate
        { "luma@1" },           // no comma
        { "luma@1,2,3" },       // a third component is not a pixel
    };
    for (const auto& spec : bad)
    {
        const auto out = ParseArgs({ "h.exe", "--headless", "--frames", "5", "--probe", spec[0].c_str() });
        CHECK_FALSE(out.config.has_value());
        CHECK(out.exitCode == 2);
    }

    // Fires even without --headless/--frames -- a syntax error does not need
    // either flag to already be wrong.
    const auto bare = ParseArgs({ "h.exe", "--probe", "bogus@1,2" });
    CHECK_FALSE(bare.config.has_value());
    CHECK(bare.exitCode == 2);

    // A well-formed probe alongside a malformed one still refuses the whole
    // command line -- there is no partial acceptance.
    const auto mixed = ParseArgs({ "h.exe", "--headless", "--frames", "5",
                                    "--probe", "census", "--probe", "bogus@1,2" });
    CHECK_FALSE(mixed.config.has_value());
    CHECK(mixed.exitCode == 2);

    // A genuinely well-formed probe list is unaffected.
    const auto good = ParseArgs({ "h.exe", "--headless", "--frames", "5",
                                   "--probe", "luma@640,360", "--probe", "census" });
    REQUIRE(good.config.has_value());
    CHECK(good.exitCode == 0);
}

TEST_CASE("hostconfig: a non-positive --fixed-dt is refused", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--headless", "--frames", "1", "--fixed-dt", "0" });
    CHECK(out.exitCode == 2);
}

// --fixed-dt nan/inf is the SAME undefined-behaviour class as the
// --max-diff-pixel-ratio bug fixed in 92792847: Cli's NumericOk only asks
// whether from_chars can consume the token, and it accepts "nan" and "inf".
// The value then flows into the frame loop's clock arithmetic, where a NaN dt
// poisons every downstream accumulation silently -- the run exits 0 having
// simulated nothing coherent, which is the worst failure an agent can read.
TEST_CASE("host config: --fixed-dt refuses non-finite values", "[hostconfig]")
{
    for (const char* bad : { "nan", "inf", "-inf" })
    {
        const auto o = Run({ "--headless", "--frames", "60", "--fixed-dt", bad });
        CAPTURE(bad);
        REQUIRE_FALSE(o.config.has_value());
        CHECK(o.exitCode == 2);
    }
    // The existing positive-value contract still holds.
    const auto ok = Run({ "--headless", "--frames", "60", "--fixed-dt", "0.008" });
    REQUIRE(ok.config.has_value());
    CHECK(ok.config->fixedDtSeconds == Catch::Approx(0.008));
}

// THE UNSTOPPABLE-PROCESS REFUSAL. A windowed run has two exits the frame loop
// honours: the window's close button, and the `quit` input action -- which
// needs the window FOCUSED to deliver a key. An unmapped window has neither,
// so a bare --headless with maxFrames == 0 runs until something outside the
// process reaps it. In the agent workflow this mode exists for, that is a
// process the spawner cannot stop by any means it owns.
// --settle N (Task 10). UNGUARDED like the block above: headless agent
// verification must work in Release and Dist, not just dev builds.
TEST_CASE("hostconfig: --settle defaults to 0 (off)", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--headless", "--frames", "1", "--screenshot", "s.png" });
    REQUIRE(out.exitCode == 0);
    REQUIRE(out.config.has_value());
    CHECK(out.config->settleAttempts == 0u);

    // The bare defaults case (no --headless at all) carries the same 0.
    const auto bare = ParseArgs({ "h.exe" });
    REQUIRE(bare.config.has_value());
    CHECK(bare.config->settleAttempts == 0u);
}

TEST_CASE("hostconfig: --settle without --headless is refused, not ignored", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--settle", "10", "--frames", "5", "--screenshot", "s.png" });
    CHECK(out.exitCode == 2);
    CHECK_FALSE(out.config.has_value());
}

// Settle compares CAPTURED frames -- with neither --screenshot nor --report
// there is nowhere for that comparison to land, and nothing would ever arm
// the capture node convergence is measured against, so the loop would never
// know when to stop.
TEST_CASE("hostconfig: --settle requires --screenshot or --report", "[hostconfig]")
{
    const auto neither = ParseArgs({ "h.exe", "--headless", "--frames", "5", "--settle", "10" });
    CHECK(neither.exitCode == 2);
    CHECK_FALSE(neither.config.has_value());

    const auto withScreenshot = ParseArgs({ "h.exe", "--headless", "--frames", "5",
                                            "--settle", "10", "--screenshot", "s.png" });
    REQUIRE(withScreenshot.config.has_value());
    CHECK(withScreenshot.config->settleAttempts == 10u);

    const auto withReport = ParseArgs({ "h.exe", "--headless", "--frames", "5",
                                        "--settle", "10", "--report", "r.json" });
    REQUIRE(withReport.config.has_value());
    CHECK(withReport.config->settleAttempts == 10u);
}

// Explicitly typing the value that also means "off" is a mistake, not a way
// to turn the flag off -- same reasoning as --fixed-dt's own r.Supplied()
// check just above.
TEST_CASE("hostconfig: an explicit --settle 0 is refused", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--headless", "--frames", "5",
                                 "--settle", "0", "--report", "r.json" });
    CHECK(out.exitCode == 2);
    CHECK_FALSE(out.config.has_value());
}

// Fix round 1, item 3: --settle 1 parses as valid syntax but is a GUARANTEED
// failure -- the first attempt has no prior capture to compare against, so it
// can never converge on any scene. That is a worse trap than a refusal (the
// caller only discovers it after paying for a whole run), so it is refused at
// parse time exactly like the explicit-0 case just above -- but with its own
// message, since "0 means off" would be a wrong explanation for why 1 fails.
TEST_CASE("hostconfig: --settle 1 is refused -- one attempt has nothing to compare against", "[hostconfig]")
{
    const auto one = ParseArgs({ "h.exe", "--headless", "--frames", "5",
                                 "--settle", "1", "--report", "r.json" });
    CHECK(one.exitCode == 2);
    CHECK_FALSE(one.config.has_value());

    // 2 is the smallest legal value -- the boundary this refusal draws.
    const auto two = ParseArgs({ "h.exe", "--headless", "--frames", "5",
                                 "--settle", "2", "--report", "r.json" });
    REQUIRE(two.config.has_value());
    CHECK(two.config->settleAttempts == 2u);
}

TEST_CASE("hostconfig: --settle N parses and composes with the rest of the headless vocabulary", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--headless", "--frames", "30", "--backend", "vulkan",
                                 "--settle", "25", "--screenshot", "s.png", "--report", "r.json",
                                 "--probe", "census" });
    REQUIRE(out.exitCode == 0);
    REQUIRE(out.config.has_value());
    CHECK(out.config->settleAttempts == 25u);
    CHECK(out.config->maxFrames == 30u);
    CHECK(out.config->screenshotPath == "s.png");
    CHECK(out.config->reportPath == "r.json");
}

TEST_CASE("hostconfig: --headless without --frames is refused", "[hostconfig]")
{
    const auto bare = ParseArgs({ "h.exe", "--headless" });
    CHECK(bare.exitCode == 2);
    CHECK_FALSE(bare.config.has_value());

    // --frames 0 is the SAME state spelled explicitly (0 == "run until quit"),
    // and it must not slip past a check written against the default.
    const auto zero = ParseArgs({ "h.exe", "--headless", "--frames", "0" });
    CHECK(zero.exitCode == 2);
    CHECK_FALSE(zero.config.has_value());

    // ...and the refusal is about --headless ONLY. A windowed open-ended run
    // still has its close button, so it stays legal -- this is exactly the
    // pairing that would break if the check were written against maxFrames
    // alone.
    const auto windowed = ParseArgs({ "h.exe" });
    REQUIRE(windowed.config.has_value());
    CHECK(windowed.exitCode == 0);
    CHECK(windowed.config->maxFrames == 0);
    CHECK_FALSE(windowed.config->headless);
}

// --offscreen was RENAMED to --headless (2026-08-27), hard break, no alias.
// This case exists because the rename could otherwise HALF-LAND: --headless
// added, --offscreen never removed, every existing caller still working, and
// nothing to show the job was unfinished. It asserts the removal, which is
// the part with no other witness.
TEST_CASE("host config: --offscreen is gone and is refused as unknown", "[hostconfig]")
{
    const auto old = Run({"--offscreen", "--frames", "60"});
    REQUIRE_FALSE(old.config.has_value());
    CHECK(old.exitCode == 2);

    // ...and the replacement does what the old flag did.
    const auto now = Run({"--headless", "--frames", "60"});
    REQUIRE(now.config.has_value());
    CHECK(now.config->headless);
}

// ---- Task 8: --compare / --bless -----------------------------------------

TEST_CASE("host: --compare requires --headless and --settle", "[host]")
{
    // A comparison against an unconverged frame is a frame number, not a
    // verdict -- the same reasoning that already gates --settle behind
    // --screenshot/--report.
    {
        const char* argv[] = { "ArcaneRuntime", "--compare", "runtime-scene" };
        CHECK_FALSE(Arcane::HostConfig::Parse(3, const_cast<char**>(argv)).config.has_value());
    }
    {
        const char* argv[] = { "ArcaneRuntime", "--headless", "--frames", "10",
                               "--compare", "runtime-scene" };
        CHECK_FALSE(Arcane::HostConfig::Parse(6, const_cast<char**>(argv)).config.has_value());
    }
    {
        const char* argv[] = { "ArcaneRuntime", "--headless", "--frames", "10",
                               "--settle", "30", "--report", "r.json",
                               "--compare", "runtime-scene" };
        const auto outcome = Arcane::HostConfig::Parse(10, const_cast<char**>(argv));
        REQUIRE(outcome.config.has_value());
        CHECK(outcome.config->compareReference == "runtime-scene");
    }
}

TEST_CASE("host: --bless without --compare is refused, not ignored", "[host]")
{
    // Rule 3, silent inertness: a flag that does nothing must say so.
    const char* argv[] = { "ArcaneRuntime", "--headless", "--frames", "10",
                           "--settle", "30", "--report", "r.json", "--bless" };
    CHECK_FALSE(Arcane::HostConfig::Parse(9, const_cast<char**>(argv)).config.has_value());
}

// Finding 1 (Task 8 dispatch audit): every refusal above tests
// cfg.compareReference.empty() as the sentinel for "not supplied", which
// conflates "not supplied" with "supplied empty". `--compare ""` must be its
// OWN refusal, not a silent, wordless disabling of the comparison -- Rule 3,
// the same treatment an explicit `--settle 0` already gets one screen up in
// HostConfig.cpp.
TEST_CASE("host: --compare with an explicit empty value is refused, not silently disabled", "[host]")
{
    const char* argv[] = { "ArcaneRuntime", "--headless", "--frames", "10",
                           "--settle", "30", "--report", "r.json",
                           "--compare", "" };
    CHECK_FALSE(Arcane::HostConfig::Parse(10, const_cast<char**>(argv)).config.has_value());
}

// Finding 2: an unsafe reference name must be refused HERE, at PARSE time,
// with its own message -- deferring to ResolveReference's own refusal would
// have the run exit "compare-missing-reference", which is a LIE: the name
// was refused, not missing. Uses the exact traversal name
// ReferenceImagesTest.cpp's own "refused, not resolved" case already pins.
TEST_CASE("host: --compare refuses an unsafe reference name at parse time", "[host]")
{
    const char* argv[] = { "ArcaneRuntime", "--headless", "--frames", "10",
                           "--settle", "30", "--report", "r.json",
                           "--compare", "../evil" };
    CHECK_FALSE(Arcane::HostConfig::Parse(10, const_cast<char**>(argv)).config.has_value());
}

TEST_CASE("host: --max-diff-pixels/--max-diff-pixel-ratio require --compare", "[host]")
{
    const char* argv[] = { "ArcaneRuntime", "--headless", "--frames", "10",
                           "--settle", "30", "--report", "r.json",
                           "--max-diff-pixels", "5" };
    CHECK_FALSE(Arcane::HostConfig::Parse(9, const_cast<char**>(argv)).config.has_value());

    const char* argv2[] = { "ArcaneRuntime", "--headless", "--frames", "10",
                            "--settle", "30", "--report", "r.json",
                            "--max-diff-pixel-ratio", "0.01" };
    CHECK_FALSE(Arcane::HostConfig::Parse(9, const_cast<char**>(argv2)).config.has_value());
}

TEST_CASE("host: --compare/--bless/--max-diff-pixels/--max-diff-pixel-ratio round-trip", "[host]")
{
    const char* argv[] = { "ArcaneRuntime", "--headless", "--frames", "10",
                           "--settle", "30", "--report", "r.json",
                           "--compare", "runtime-scene", "--bless",
                           "--max-diff-pixels", "5", "--max-diff-pixel-ratio", "0.001" };
    const auto outcome = Arcane::HostConfig::Parse(15, const_cast<char**>(argv));
    REQUIRE(outcome.config.has_value());
    CHECK(outcome.config->compareReference == "runtime-scene");
    CHECK(outcome.config->bless);
    REQUIRE(outcome.config->maxDiffPixels.has_value());
    CHECK(*outcome.config->maxDiffPixels == 5u);
    REQUIRE(outcome.config->maxDiffPixelRatio.has_value());
    CHECK(*outcome.config->maxDiffPixelRatio == Catch::Approx(0.001));
}

// ---------------------------------------------------------------------------
// Final-review I-1. --max-diff-pixel-ratio is the one Rule-3 hole this otherwise
// exhaustive file had: its value flows unchecked into
// `static_cast<std::uint64_t>(expectedArea * ratio)` (ImageCompare.cpp:503),
// where a negative, non-finite, or out-of-uint64-range double is UNDEFINED
// BEHAVIOUR ([conv.fpint]) rather than a saturating clamp. The consequence is
// not abstract: `--max-diff-pixel-ratio 1e30` means "forgive everything" and can
// silently produce a budget of ZERO -- the strictest verdict from the loosest
// request, indistinguishable to an agent from a genuine regression.
//
// WOULD THESE FAIL IF THE THING THEY NAME WERE WRONG?
//   * Delete the range check   -> the refused half fails (Parse hands back a
//                                 config for -1 / 1e30 / 1.0001).
//   * CLAMP instead of refuse  -> the refused half fails for the same reason;
//                                 clamping returns a config with a fudged value.
//   * Write it as `> 0.0`      -> the accepted half fails on "0" (an explicit
//                                 zero budget is a legal request, and is what an
//                                 unset budget already means).
//   * Write it as `< 1.0`      -> the accepted half fails on "1"/"1.0", the
//                                 upper endpoint ("every pixel may differ").
// Non-vacuity of the out-of-range half is pinned by the --fixed-dt control
// below: it proves Cli's own NumericOk (Cli.cpp:69-89) hands "1e30" straight
// through, so the refusal being tested here can only be coming from
// HostConfig's new check and not from the numeric parse underneath it.
TEST_CASE("host: --max-diff-pixel-ratio is range-refused at parse time, not clamped", "[host]")
{
    auto withRatio = [](const std::string& value) {
        return Run({ "--headless", "--frames", "10", "--settle", "30",
                     "--report", "r.json", "--compare", "runtime-scene",
                     "--max-diff-pixel-ratio", value });
    };

    SECTION("out of range is REFUSED (exit 2), and no config comes back") {
        // Every one of these is a token std::from_chars consumes whole as a
        // double, so each reaches the new check rather than dying in Cli.
        for (const char* bad : { "-1", "-0.5", "-0.0001", "1.0001", "2", "1e30" }) {
            CAPTURE(bad);
            const auto o = withRatio(bad);
            CHECK_FALSE(o.config.has_value());
            CHECK(o.exitCode == 2);
        }
    }

    SECTION("non-finite is REFUSED (exit 2)") {
        // from_chars accepts the inf/nan spellings (and a leading '-'), so these
        // ARE numeric tokens as far as Cli is concerned -- MEASURED, not assumed:
        // running the built ArcaneRuntime.exe with `--max-diff-pixel-ratio nan`
        // prints HostConfig's OWN "wants a finite fraction in [0, 1]" message,
        // not Cli's "expects a number". So this section is not vacuous either:
        // without the new check these would sail through to the narrowing cast.
        for (const char* bad : { "nan", "inf", "-inf", "infinity" }) {
            CAPTURE(bad);
            const auto o = withRatio(bad);
            CHECK_FALSE(o.config.has_value());
            CHECK(o.exitCode == 2);
        }
    }

    SECTION("in range is ACCEPTED, BOTH endpoints included") {
        // 0 is a legal explicit request (it is also what an unset budget means)
        // and 1 is the upper endpoint -- the ratio is a fraction of the
        // reference's AREA, so 1 already means "every pixel may differ".
        for (const char* good : { "0", "0.0", "0.001", "0.5", "1", "1.0" }) {
            CAPTURE(good);
            const auto o = withRatio(good);
            REQUIRE(o.config.has_value());
            REQUIRE(o.config->maxDiffPixelRatio.has_value());
            CHECK(*o.config->maxDiffPixelRatio >= 0.0);
            CHECK(*o.config->maxDiffPixelRatio <= 1.0);
        }
    }

    SECTION("CONTROL: Cli's numeric parse itself accepts 1e30 on a Double option") {
        // --fixed-dt is the other CliType::Double option and carries no upper
        // bound. If this ever starts failing, the "out of range is REFUSED"
        // section above has gone vacuous -- the refusal would be coming from
        // NumericOk, not from the range check this case exists to pin.
        const auto o = Run({ "--headless", "--frames", "1", "--fixed-dt", "1e30" });
        REQUIRE(o.config.has_value());
        CHECK(o.config->fixedDtSeconds == Catch::Approx(1e30));
    }
}

// The budgets are refused WITHOUT --compare (that is the case above this block),
// so the range refusal must not be reachable ahead of that refusal -- otherwise
// `--max-diff-pixel-ratio -1` with no --compare would report the wrong mistake.
TEST_CASE("host: an out-of-range ratio without --compare still reports the --compare mistake",
          "[host]")
{
    // Both are exit 2; what this pins is that Parse does not stop caring about
    // the missing --compare just because the value is also out of range. The
    // ordering is only observable through stderr, so this asserts the weaker
    // fact that BOTH orderings agree on: still refused, still exit 2, still no
    // config -- i.e. the new check never turns a two-fault command line into a
    // parse SUCCESS.
    const auto o = Run({ "--headless", "--frames", "10", "--settle", "30",
                         "--report", "r.json", "--max-diff-pixel-ratio", "-1" });
    CHECK_FALSE(o.config.has_value());
    CHECK(o.exitCode == 2);
}

TEST_CASE("host: --compare/--bless default off, maxDiff budgets default unset", "[host]")
{
    const char* argv[] = { "ArcaneRuntime" };
    const auto o = Arcane::HostConfig::Parse(1, const_cast<char**>(argv));
    REQUIRE(o.config.has_value());
    CHECK(o.config->compareReference.empty());
    CHECK_FALSE(o.config->bless);
    CHECK_FALSE(o.config->maxDiffPixels.has_value());
    CHECK_FALSE(o.config->maxDiffPixelRatio.has_value());
}

TEST_CASE("host: the editor accepts the same verification flags the runtime does", "[host]")
{
    // HostConfig is SHARED, so this only pins that Parse itself accepts these
    // flags on an argv[0] of "ArcaneEditor" -- it is NOT a claim about
    // ArcaneEditor/src/main.cpp's own per-host refusal table, which this
    // test exe does not compile (EditorApp*.cpp / main.cpp are not part of
    // ArcaneTests) and therefore cannot observe either way. Task 9 lifts
    // --report/--settle (and, transitively, --compare/--bless) out of that
    // table while leaving --probe refused there -- see main.cpp's own
    // disposition comment for that split. The real proof the refusal was
    // actually lifted is running the ArcaneEditor.exe binary itself (this
    // task's Steps 5/6), not this test.
    const char* argv[] = { "ArcaneEditor", "--project", "ReferenceProject",
                           "--headless", "--frames", "60", "--settle", "30",
                           "--report", "r.json", "--compare", "editor-ui" };
    const auto outcome = Arcane::HostConfig::Parse(12, const_cast<char**>(argv));
    REQUIRE(outcome.config.has_value());
    CHECK(outcome.config->settleAttempts == 30);
    CHECK(outcome.config->compareReference == "editor-ui");
    CHECK(outcome.config->reportPath == "r.json");
}

namespace {
    // Everything written to stderr while this object lives is captured.
    //
    // WHY: HostConfig::Parse returns exit 2 for EVERY refusal, so a case that
    // asserts only on the exit code passes whether or not the rule it names is
    // the one that actually fired. Two of the --settle-timeout cases below are
    // exactly that shape, and a test that cannot fail for the reason it names
    // is worse than no test -- it is a false green. The message is the only
    // thing that distinguishes one refusal from another, so the message is what
    // gets asserted.
    //
    // RAII, restoring on EVERY path (normal return, early return, exception):
    // ArcaneTests runs in RANDOM ORDER across 1200+ cases, so a redirect that
    // leaked would silently swallow the whole remaining suite's diagnostics.
    class StderrCapture
    {
    public:
        StderrCapture()
        {
            std::fflush(stderr);
            m_savedFd = ARCTEST_DUP(ARCTEST_FILENO(stderr));
            m_path    = std::filesystem::temp_directory_path() /
                        ("arcane_hostconfig_stderr_" + std::to_string(NextId()) + ".txt");
            FILE* redirected = nullptr;
#if defined(_WIN32)
            if (freopen_s(&redirected, m_path.string().c_str(), "w", stderr) != 0)
                redirected = nullptr;
#else
            redirected = std::freopen(m_path.string().c_str(), "w", stderr);
#endif
            m_ok = (m_savedFd != -1 && redirected != nullptr);
        }
        StderrCapture(const StderrCapture&)            = delete;
        StderrCapture& operator=(const StderrCapture&) = delete;
        ~StderrCapture()
        {
            Restore();
            std::error_code ec;
            std::filesystem::remove(m_path, ec);   // best effort; must not throw
        }
        [[nodiscard]] bool Ok() const noexcept { return m_ok; }
        // Restores FIRST, so the text is read -- and any CHECK on it reported --
        // with stderr already back to normal.
        std::string Text()
        {
            Restore();
            std::ifstream in(m_path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        }
    private:
        static int NextId() { static int n = 0; return ++n; }
        void Restore() noexcept
        {
            if (m_restored) return;
            m_restored = true;
            std::fflush(stderr);
            if (m_savedFd != -1)
            {
                ARCTEST_DUP2(m_savedFd, ARCTEST_FILENO(stderr));
                ARCTEST_CLOSE(m_savedFd);
                m_savedFd = -1;
            }
            std::clearerr(stderr);
        }
        int                   m_savedFd  = -1;
        bool                  m_ok       = false;
        bool                  m_restored = false;
        std::filesystem::path m_path;
    };

    struct RunWithStderr
    {
        Arcane::HostConfig::ParseOutcome outcome;
        std::string                      err;
        bool                             captured = false;
    };

    // A SECOND helper BESIDE Run(), deliberately not a change to Run() itself:
    // ~50 existing cases call that one and none of them want a redirect.
    RunWithStderr RunCapturingStderr(std::vector<std::string> args)
    {
        RunWithStderr r;
        StderrCapture cap;
        r.outcome  = Run(std::move(args));
        r.captured = cap.Ok();
        r.err      = cap.Text();
        return r;
    }
}

TEST_CASE("host config: --settle-timeout parses and defaults to 5000", "[hostconfig]")
{
    const auto def = Run({ "--headless", "--frames", "60", "--settle", "30",
                           "--screenshot", "s.png" });
    REQUIRE(def.config.has_value());
    // 5000ms is INHERITED from Playwright's expect timeout, not invented here.
    // The 50ms poll interval beside it (Arcane::kSettleIntervalMs) is OURS --
    // the two do NOT share a provenance, and must never be cited as if they do.
    CHECK(def.config->settleTimeoutMs == 5000u);

    const auto set = Run({ "--headless", "--frames", "60", "--settle", "30",
                           "--settle-timeout", "250", "--screenshot", "s.png" });
    REQUIRE(set.config.has_value());
    CHECK(set.config->settleTimeoutMs == 250u);
}

TEST_CASE("host config: --settle-timeout requires --settle", "[hostconfig]")
{
    // A timeout on a loop that never runs is a caller error, and this file
    // refuses those loudly rather than ignoring them.
    //
    // --headless is present, so wantsOffscreenOnly cannot be what rejects this
    // -- and the stderr assertion nails it down further, because every refusal
    // in HostConfig::Parse exits 2 and the exit code alone proves nothing.
    const auto o = RunCapturingStderr({ "--headless", "--frames", "60",
                                        "--settle-timeout", "250", "--screenshot", "s.png" });
    REQUIRE_FALSE(o.outcome.config.has_value());
    CHECK(o.outcome.exitCode == 2);
    REQUIRE(o.captured);
    CHECK(o.err.find("--settle-timeout requires --settle") != std::string::npos);
}

TEST_CASE("host config: --settle-timeout requires --headless", "[hostconfig]")
{
    // --settle-timeout joins --fixed-dt/--probe/--report/--settle/--compare in
    // the wantsOffscreenOnly set: a settle timeout in a windowed run is
    // meaningless for the same reason --settle already is.
    //
    // THERE IS DELIBERATELY NO --settle ON THIS COMMAND LINE. With one, --settle
    // would trip wantsOffscreenOnly all by itself and this case could not fail
    // even if the --settle-timeout clause were reverted -- it would be a false
    // green. Without it, ONLY the new clause can route this run to the
    // --headless refusal: revert the clause and Parse falls through to the
    // "requires --settle" refusal instead. Both exit 2, so the MESSAGE is the
    // only thing that can tell the two apart, which is what is asserted here.
    const auto o = RunCapturingStderr({ "--frames", "60", "--settle-timeout", "250",
                                        "--screenshot", "s.png" });
    REQUIRE_FALSE(o.outcome.config.has_value());
    CHECK(o.outcome.exitCode == 2);
    REQUIRE(o.captured);
    CHECK(o.err.find("require --headless")  != std::string::npos);   // the offscreen-only gate
    CHECK(o.err.find("--settle-timeout")    != std::string::npos);   // named in its flag list
    CHECK(o.err.find("requires --settle")   == std::string::npos);   // NOT the sibling refusal
}

TEST_CASE("host config: --settle-timeout 0 is ACCEPTED, unlike --settle 0", "[hostconfig]")
{
    // 0 means "no time bound": the conjunction degrades to the old
    // attempts-only behaviour rather than becoming unsatisfiable, so it is a
    // legal value carrying a real meaning. --settle 0 IS refused (0 there means
    // "off", which omitting the flag already says). The two zeroes must not be
    // collapsed into one "zero is refused" rule.
    const auto o = Run({ "--headless", "--frames", "60", "--settle", "30",
                         "--settle-timeout", "0", "--screenshot", "s.png" });
    REQUIRE(o.config.has_value());
    CHECK(o.config->settleTimeoutMs == 0u);
}

// ---- Task 4: --fixed-time, absolute scene-clock pinning -------------------

TEST_CASE("host config: --fixed-time is optional and validated like --fixed-dt", "[host]")
{
    // Absent by default -- the scene clock keeps accumulating as it always has.
    {
        const char* argv[] = { "ArcaneRuntime", "--project", "P", "--headless", "--frames", "1" };
        auto [cfg, code] = Arcane::HostConfig::Parse(6, const_cast<char**>(argv));
        REQUIRE(cfg.has_value());
        CHECK_FALSE(cfg->fixedTimeSeconds.has_value());
    }
    // Supplied and finite: accepted, including zero (a legitimate pin).
    {
        const char* argv[] = { "ArcaneRuntime", "--project", "P", "--headless",
                               "--frames", "1", "--fixed-time", "0" };
        auto [cfg, code] = Arcane::HostConfig::Parse(8, const_cast<char**>(argv));
        REQUIRE(cfg.has_value());
        REQUIRE(cfg->fixedTimeSeconds.has_value());
        CHECK(*cfg->fixedTimeSeconds == Catch::Approx(0.0));
    }
    // NaN is refused at the parse boundary, exactly as --fixed-dt already is
    // (HostConfig.cpp:191-195): `>= 0.0` alone would not reject it, because
    // every comparison against NaN is false.
    {
        const char* argv[] = { "ArcaneRuntime", "--project", "P", "--headless",
                               "--frames", "1", "--fixed-time", "nan" };
        auto [cfg, code] = Arcane::HostConfig::Parse(8, const_cast<char**>(argv));
        CHECK_FALSE(cfg.has_value());
        CHECK(code == 2);
    }
    // Negative is refused: a scene clock before zero is a typo, not a time.
    {
        const char* argv[] = { "ArcaneRuntime", "--project", "P", "--headless",
                               "--frames", "1", "--fixed-time", "-1" };
        auto [cfg, code] = Arcane::HostConfig::Parse(8, const_cast<char**>(argv));
        CHECK_FALSE(cfg.has_value());
        CHECK(code == 2);
    }
}

// ---- Core-DLL split, plan 1 Task 7: --play-as (EDITOR only) ----------------

TEST_CASE("host config: --play-as round-trips a known topology, refuses an unknown one, and defaults empty", "[host]")
{
    // NOT a Cli::Choices() list: the registered default is "" (absent = the
    // editor's ordinary Edit-mode boot), and an empty string is not one of the
    // choices -- so the refusal is a post-parse one, like --fixed-dt's.
    const auto ok = Run({ "--play-as", "embedded-server" });
    REQUIRE(ok.config.has_value());
    CHECK(ok.config->playAs == "embedded-server");

    const auto bad = RunCapturingStderr({ "--play-as", "bogus" });
    CHECK_FALSE(bad.outcome.config.has_value());
    CHECK(bad.outcome.exitCode == 2);
    if (bad.captured)
        CHECK(bad.err.find("--play-as must be one of") != std::string::npos);

    const auto absent = Run({});
    REQUIRE(absent.config.has_value());
    CHECK(absent.config->playAs.empty());
}

// ---- crash window plan 1, Task 9: the sanitized relaunch line --------------
//
// SanitizeRelaunchLine is what a host puts in Diagnostics::Config::commandLine,
// so the crash reporter's "restart" button re-runs the SESSION and not the
// capture harness that produced the crash. Two properties, and the second is
// the one a naive "drop --frames" implementation gets wrong: the line it
// returns must still PARSE, so a stripped flag takes its dependants with it
// (HostConfig::Parse refuses --probe without --frames, and the whole
// offscreen-only family without --headless).

TEST_CASE("SanitizeRelaunchLine strips the harness flags and keeps the session", "[host]")
{
    const std::vector<std::string> argv = {
        "ArcaneEditor.exe", "--project", "ReferenceProject", "--backend", "dx12",
        "--frames", "900", "--headless", "--settle", "4", "--report", "out.json",
        "--compare", "golden", "--screenshot", "shot.png", "--crash-gpu", "3",
        "--hang-main", "30",
    };
    const std::string line = Arcane::SanitizeRelaunchLine(argv);

    CHECK(line.find("--project ReferenceProject") != std::string::npos);
    CHECK(line.find("--backend dx12") != std::string::npos);
    CHECK(line.find("ArcaneEditor.exe") == 0u);

    for (const char* gone : { "--frames", "900", "--headless", "--settle", "--report",
                              "out.json", "--compare", "golden", "--screenshot",
                              "shot.png", "--crash-gpu", "--hang-main" })
        CHECK(line.find(gone) == std::string::npos);
}

TEST_CASE("SanitizeRelaunchLine handles --flag=value, dependants and spaces", "[host]")
{
    // --flag=value is the OTHER form Cli accepts (Cli.cpp:146) -- stripping
    // only the space-separated spelling would leave the flag in the line.
    const std::vector<std::string> inlineForm = {
        "ArcaneRuntime.exe", "--frames=120", "--project=P", "--report=r.json",
    };
    const std::string a = Arcane::SanitizeRelaunchLine(inlineForm);
    CHECK(a.find("--project=P") != std::string::npos);
    CHECK(a.find("--frames") == std::string::npos);
    CHECK(a.find("--report") == std::string::npos);

    // The dependants go with their parents: --bless without --compare and
    // --settle-timeout without --settle are both parse-time refusals, so a
    // line that kept them would not relaunch anything.
    const std::vector<std::string> dependants = {
        "ArcaneRuntime.exe", "--headless", "--frames", "8", "--settle", "4",
        "--settle-timeout", "9000", "--compare", "g", "--bless", "--probe", "census",
        "--fixed-dt", "0.016", "--project", "P",
    };
    const std::string b = Arcane::SanitizeRelaunchLine(dependants);
    CHECK(b == "ArcaneRuntime.exe --project P");

    // A path with spaces has to come back quoted or the relaunch splits it.
    const std::vector<std::string> spaced = { "ArcaneEditor.exe", "--project", "C:/My Games/P" };
    CHECK(Arcane::SanitizeRelaunchLine(spaced) == "ArcaneEditor.exe --project \"C:/My Games/P\"");
}

// --window-size WxH (T3-D6 fix round 1): the automation-only host extent a
// desk-geometry capture needs (1920x1080). Unset keeps the 1280x720 default
// every golden reference is captured at.
TEST_CASE("host config: --window-size parses WxH, is automation only, and refuses a malformed extent", "[host]") {
    const auto plain = Run({"--project", "P", "--headless", "--frames", "1"});
    REQUIRE(plain.config.has_value());
    CHECK(plain.config->windowWidth == 0u);   // unset: GpuContext's 1280x720 default
    CHECK(plain.config->windowHeight == 0u);

    const auto sized = Run({"--project", "P", "--headless", "--frames", "1",
                            "--window-size", "1920x1080"});
    REQUIRE(sized.config.has_value());
    CHECK(sized.config->windowWidth == 1920u);
    CHECK(sized.config->windowHeight == 1080u);

    const auto inlineForm = Run({"--project", "P", "--frames", "1", "--window-size=2560X1440"});
    REQUIRE(inlineForm.config.has_value());
    CHECK(inlineForm.config->windowWidth == 2560u);
    CHECK(inlineForm.config->windowHeight == 1440u);

    // Automation only: no --frames, no extent.
    const auto interactive = Run({"--project", "P", "--window-size", "1920x1080"});
    CHECK_FALSE(interactive.config.has_value());
    CHECK(interactive.exitCode == 2);

    for (const char* bad : { "1920", "1920x", "x1080", "1920x1080x2", "-1920x1080", "1920 x1080",
                             "63x1080", "1920x9000", "abcxdef", "" })
    {
        INFO("--window-size '" << bad << "'");
        const auto refused = Run({"--project", "P", "--frames", "1", "--window-size", bad});
        CHECK_FALSE(refused.config.has_value());
        CHECK(refused.exitCode == 2);
    }
}

TEST_CASE("SanitizeRelaunchLine strips --window-size (an automation extent, not the session)", "[host]")
{
    const std::vector<std::string> argv = {
        "ArcaneEditor.exe", "--project", "P", "--frames", "60", "--window-size", "1920x1080",
    };
    const std::string line = Arcane::SanitizeRelaunchLine(argv);
    CHECK(line.find("--project P") != std::string::npos);
    CHECK(line.find("--window-size") == std::string::npos);
    CHECK(line.find("1920x1080") == std::string::npos);

    const std::vector<std::string> inlineForm = {
        "ArcaneEditor.exe", "--project=P", "--frames=60", "--window-size=1920x1080",
    };
    CHECK(Arcane::SanitizeRelaunchLine(inlineForm).find("window-size") == std::string::npos);
}
