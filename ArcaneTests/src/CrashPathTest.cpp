// The crash path (crash window plan 1, task 5): a report runs on a DEDICATED
// crash thread, in UE's order -- backlog frozen, minimal envelope, minidump,
// module+offset text, GPU provider, full envelope, backlog dump, hand-off --
// while the submitting thread only signals, waits and (for a fatal report)
// exits.
//
// Device-free on purpose: everything asserted here is CPU-side and observable
// from the files the report leaves behind, so the freeze this whole arc exists
// to remove is covered in the ordinary [diag] gate rather than on a desk with a
// wedged GPU.

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Platform.hpp>   // module/exe spellings per platform (Diagnostics POSIX port)
#include <Arcane/Project/ProjectPaths.hpp>   // kDistBuild

#include "Helpers/HostWitness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Same RAII discipline as DiagnosticsTest's ArmedDiagnostics: Install() is
    // idempotent, so a case that does not disarm leaks its config into the next
    // one under Catch2's random ordering.
    struct Armed
    {
        explicit Armed(const std::filesystem::path& dir)
        {
            // R16: Diagnostics::Install does NOT bootstrap the engine logger
            // (Log::Init is call_once and the FIRST caller's level wins, so
            // bootstrapping there would pin the whole process to the default
            // level). Hosts call Log::Init() before Install -- so the tests
            // do too, which is also what makes the backlog assertion below
            // order-independent: no logger means no file sink, and no file
            // sink means no backlog sink to record into. Init is a once-latch,
            // so this is harmless when an earlier case already ran it.
            Arcane::Log::Init();

            Arcane::Diagnostics::Config cfg;
            cfg.appName = "CrashPathTest";
            cfg.dumpDir = dir.string();
            cfg.unattended = true;
            cfg.spawnReporter = false;
            cfg.startHangWatchdog = false;
            // Never hijack the suite's own fault handling (controller note R2;
            // every existing [diag] case does the same). The crash thread and
            // its events are created regardless -- they are the report engine,
            // not the handler.
            cfg.installCrashHandler = false;
            Arcane::Diagnostics::Install(cfg);
        }
        ~Armed() { Arcane::Diagnostics::Shutdown(); }

        Armed(const Armed&)            = delete;
        Armed& operator=(const Armed&) = delete;
    };

    std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    // RAII for the process-global GPU-section provider slot -- same reasoning
    // as DiagnosticsTest's ArmedGpuProvider: the suite runs in random order,
    // so a case that installs one must clear it even if an assertion unwinds
    // first, or the NEXT report (or the watchdog, on its own thread) runs a
    // provider pointing at this case's dead stack.
    struct ArmedProvider
    {
        ArmedProvider(Arcane::Diagnostics::GpuSectionProvider fn, void* user)
        {
            Arcane::Diagnostics::SetGpuSectionProvider(fn, user);
        }
        ~ArmedProvider() { Arcane::Diagnostics::ClearGpuSectionProvider(); }

        ArmedProvider(const ArmedProvider&)            = delete;
        ArmedProvider& operator=(const ArmedProvider&) = delete;
    };

    // A provider that returns far more than the envelope's arena reserve can
    // hold: ~400 queues of ~600 bytes each, plus 200 long activeLayers, is
    // several hundred KiB against a 64 KiB reserve. Nothing here is a GPU
    // call -- the point is purely the SIZE the provider hands back, which is
    // the one input to this path a backend controls and Core cannot bound.
    void FloodingGpuSectionProvider(Arcane::Diag::Envelope& envelope, std::string& humanText,
                                    const std::filesystem::path&, void* user)
    {
        if (auto* calls = static_cast<int*>(user)) ++(*calls);

        for (int i = 0; i < 400; ++i)
        {
            envelope.queues.push_back({ std::string(200, 'q'),
                                        std::string(200, 'c'),
                                        { std::string(200, 'f') } });
        }
        envelope.activeLayers.assign(200, std::string(200, 'L'));
        envelope.fault = { "page-fault", "0xDEADBEEF0000", "FloodResource" };

        humanText = "flooded";
    }
}

TEST_CASE("crash path: a manual report runs on the crash thread, writes envelope before dump, "
          "a module+offset stack, and the log backlog", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-crash-path-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    Armed armed(dir);

    ARC_WARN("a line the backlog must carry");
    // exitCode 0: survivable. A fatal report would TerminateProcess here.
    Arcane::Diagnostics::SubmitReport({ "hang (test)", nullptr, false, 0 });

    const std::string stem = Arcane::Diagnostics::LastReportStem();
    REQUIRE_FALSE(stem.empty());
    REQUIRE(std::filesystem::exists(stem + ".arcdiag"));
    REQUIRE(std::filesystem::exists(stem + ".dmp"));
    REQUIRE(std::filesystem::exists(stem + ".txt"));
    REQUIRE(std::filesystem::exists(stem + ".log.txt"));

    const auto env = Arcane::Diag::ReadFile(stem + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "hang");
    // Plan 2 (D2): the reason rides in the envelope so the reporter reads one file.
    CHECK(env->reason == "hang (test)");
    CHECK(env->siblingDmp.empty() == false);

    const std::string txt = Slurp(stem + ".txt");
    // Portable frames name modules, and nothing on this path symbolizes any
    // more -- no DbgHelp, so no "module!function" line can appear.
    CHECK(txt.find(Arcane::Platform::SharedLibraryFileName("ArcaneCore") + " + 0x") != std::string::npos);
    CHECK(txt.find("!") == std::string::npos);
    CHECK(Slurp(stem + ".log.txt").find("a line the backlog must carry") != std::string::npos);
}

TEST_CASE("crash path: a lightweight (ensure) report writes envelope and text only and returns", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-crash-path-test-ensure";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    Armed armed(dir);

    Arcane::Diagnostics::SubmitReport({ "ensure: index < count", nullptr, /*lightweight*/true, 0 });

    const std::string stem = Arcane::Diagnostics::LastReportStem();
    REQUIRE_FALSE(stem.empty());
    CHECK(std::filesystem::exists(stem + ".arcdiag"));
    CHECK_FALSE(std::filesystem::exists(stem + ".dmp"));

    const auto env = Arcane::Diag::ReadFile(stem + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "ensure");
}

TEST_CASE("crash path: an envelope that cannot fit the arena is elided or withheld, never left "
          "truncated on disk", "[diag]")
{
    // THE FAILURE THIS PINS. The envelope is hand-written into a fixed arena
    // span, and CrashArena::Builder::Append copies min(remaining, size) and
    // only flags the arena -- so an overrun is SILENTLY TRUNCATED,
    // syntactically invalid JSON. The full envelope is then moved over the
    // minimal one with MOVEFILE_REPLACE_EXISTING, which would destroy the
    // only parseable report on disk. The GPU-section provider is the one
    // input to that path Core cannot bound (a real backend returns however
    // many queues, in-flight passes and layers the device has), so that is
    // where the overrun is driven from here.
    const auto dir = std::filesystem::temp_directory_path() / "arcane-crash-path-test-flood";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    Armed armed(dir);

    int providerCalls = 0;
    ArmedProvider provider(&FloodingGpuSectionProvider, &providerCalls);

    Arcane::Diagnostics::SubmitReport({ "hang (envelope flood)", nullptr, false, 0 });
    CHECK(providerCalls == 1);

    const std::string stem = Arcane::Diagnostics::LastReportStem();
    REQUIRE_FALSE(stem.empty());
    REQUIRE(std::filesystem::exists(stem + ".arcdiag"));

    // THE ASSERTION: whatever landed, it PARSES. Either the full envelope was
    // withheld and the minimal one stands, or the rebuild with the unbounded
    // fields elided replaced it -- both are valid JSON, and a truncated file
    // is neither.
    const auto env = Arcane::Diag::ReadFile(stem + ".arcdiag");
    REQUIRE(env.has_value());

    // ...and it is still the RIGHT report: kind and the sibling paths survive
    // elision, because they are what a reader acts on.
    CHECK(env->kind == "hang");
    CHECK(env->guid.IsValid());
    CHECK(env->siblingDmp.empty() == false);

    // The stack is never lost by eliding -- it lives in the .txt sibling the
    // envelope names, which is written before any of this.
    const std::string txt = Slurp(stem + ".txt");
    CHECK(txt.find(" + 0x") != std::string::npos);

    // No unparseable temp is left lying around for a reporter to pick up.
    CHECK_FALSE(std::filesystem::exists(stem + ".arcdiag.tmp"));
}

namespace
{
    // Spec §6: no reporter spawn on a build machine (Install forces
    // spawnReporter=false there), so every case that needs a SPAWNED reporter
    // skips -- the desk gate is where these run.
    void SkipIfBuildMachine()
    {
        if ((std::getenv("CI") || std::getenv("ARCANE_BUILD_MACHINE")) && !std::getenv("ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE"))
            SKIP("CI/ARCANE_BUILD_MACHINE set -- the reporter is never spawned on a build machine (spec §6; "
                 "ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE overrides)");
    }

    // The newest .arcdiag stem in `dir`, polling up to `timeout` -- a
    // detached reporter or monitor writes AFTER the fixture has exited.
    //
    // R69: "newest" means BY last_write_time, not "whichever entry
    // directory_iterator happened to yield last" -- directory order is not
    // timestamp order. This is not hypothetical: the reporter installs its
    // OWN Diagnostics into the host's report directory (D13), so two app
    // names can legitimately coexist there, and tasks 8/9 add a monitor that
    // writes a third.
    std::filesystem::path WaitForStem(const std::filesystem::path& dir, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            std::filesystem::path           found;
            std::filesystem::file_time_type newest{};
            std::error_code                 ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            {
                if (e.path().extension() != ".arcdiag") continue;
                std::error_code tec;
                const auto written = std::filesystem::last_write_time(e.path(), tec);
                if (tec) continue;
                if (found.empty() || written > newest)
                {
                    newest = written;
                    found  = e.path().parent_path() / e.path().stem();
                }
            }
            if (!found.empty() || std::chrono::steady_clock::now() >= deadline) return found;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool WaitForFile(const std::filesystem::path& p, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!std::filesystem::exists(p) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return std::filesystem::exists(p);
    }

    // Runs the fixture with `--die <mode>` into a fresh dir; returns the run,
    // the newest .arcdiag stem and the directory itself. The `tag` derivation
    // gives each FLAG VARIANT its own directory ("arcane-death-av-reporter"),
    // so a `--reporter` run can never read a plain run's stem -- before this,
    // the three `--die none` cases all shared "arcane-death-none".
    struct FixtureRun { Arcane::Test::WitnessRun run; std::filesystem::path stem; std::filesystem::path dir; };
    FixtureRun RunFixture(const char* mode, std::vector<std::string> extra = {})
    {
        const auto exe = std::filesystem::absolute("../death-fixture/" + Arcane::Platform::ExecutableFileName("death-fixture"));
        REQUIRE(std::filesystem::exists(exe));
        std::string tag = mode;
        for (const auto& e : extra) if (e.rfind("--", 0) == 0) tag += "-" + e.substr(2);
        const auto dir = std::filesystem::temp_directory_path() / ("arcane-death-" + tag);
        std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
        std::vector<std::string> args = { "--dir", dir.string(), "--die", mode };
        args.insert(args.end(), extra.begin(), extra.end());
        Arcane::Test::WitnessInvocation inv; inv.exePath = exe; inv.args = args; inv.hardCapMs = 30000;
        FixtureRun out{ Arcane::Test::RunWitness(inv), {}, dir };
        out.stem = WaitForStem(dir, std::chrono::milliseconds(0));
        return out;
    }

    // The pid the report came from, read off its own stem
    // ("<app>-<stamp>-pid<n>", Diagnostics.cpp's spelling).
    //
    // The brief for task 5 passed `--pid 0` here; R65 (task 4 fix round 1)
    // made that a parse refusal in both modes afterwards, and rightly -- 0
    // names the System Idle Process. The stem's pid is the honest value: the
    // fixture that wrote the report has already exited, which IS the crash
    // case this reporter run models, and no LIVE process can be named by
    // accident from a file the fixture itself wrote.
    std::uint32_t PidFromStem(const std::filesystem::path& stem)
    {
        const std::string name = stem.filename().string();
        const std::size_t at   = name.rfind("-pid");
        if (at == std::string::npos) return 0;
        std::uint32_t    pid = 0;
        const char* const b   = name.data() + at + 4;
        const auto        r   = std::from_chars(b, name.data() + name.size(), pid);
        return r.ec == std::errc{} ? pid : 0;
    }

    // Runs the staged reporter DIRECTLY on a report the fixture already wrote
    // -- no host spawn, so this is the symbolizer under test and nothing else.
    Arcane::Test::WitnessRun RunReporter(const std::filesystem::path& stem, std::vector<std::string> extra)
    {
        const auto exe = std::filesystem::absolute("../ArcaneCrashReporter/ArcaneCrashReporter.exe");
        REQUIRE(std::filesystem::exists(exe));
        const std::uint32_t pid = PidFromStem(stem);
        REQUIRE(pid != 0u);
        std::vector<std::string> args = { stem.string() + ".arcdiag", "--pid", std::to_string(pid),
                                          "--kind", "crash", "--product", "DeathFixture",
                                          "--unattended", "--deadline", "40" };
        args.insert(args.end(), extra.begin(), extra.end());
        Arcane::Test::WitnessInvocation inv; inv.exePath = exe; inv.args = args; inv.hardCapMs = 60000;
        return Arcane::Test::RunWitness(inv);
    }
}

// Spec §6 "Symbolization": once with PDBs present (names resolve) and once
// with them hidden (module+offset). --symbol-path REPLACES the search and sets
// SYMOPT_IGNORE_CVREC (D5), which is the only way to hide a PDB on the desk
// that built it -- the linker embedded an absolute path to it in the image.
//
// This is the case that proves the engine is REAL: a pure formatter cannot
// tell you whether dbgeng resolved a name, only how a resolved one prints.
TEST_CASE("reporter: symbolizes the death fixture's minidump -- names with PDBs, module+offset with them hidden", "[diag]")
{
    const FixtureRun crash = RunFixture("av");
    REQUIRE(crash.run.exitCode == 10);
    REQUIRE_FALSE(crash.stem.empty());
    const std::string sibling = crash.stem.string() + ".symbolized.txt";

    // death-fixture keeps symbols on in Dist (premake5.lua), so dbgeng resolves
    // death-fixture.pdb in every configuration. The symbol-less half below
    // still runs with an empty search path.
    const std::string fixtureDir = std::filesystem::absolute("../death-fixture").string();
    const std::string coreDir    = std::filesystem::absolute("../ArcaneCore").string();
    {
        const auto run = RunReporter(crash.stem, { "--symbol-path", fixtureDir + ";" + coreDir });
        INFO("reporter stderr: " << run.stderrPath.string());
        CHECK_FALSE(run.timedOut);
        CHECK(run.exitCode == 0);
        REQUIRE(std::filesystem::exists(sibling));
        const std::string text = Slurp(sibling);
        INFO("symbolized:\n" << text);
        CHECK(text.find("engine      : dbgeng") != std::string::npos);
        CHECK(text.find("(faulting)") != std::string::npos);
        CHECK(text.find("!main") != std::string::npos);            // death-fixture.pdb resolved
        CHECK(text.find("DeathFixtureMain.cpp") != std::string::npos);
    }
    std::filesystem::remove(sibling);
    {
        const auto hidden = std::filesystem::temp_directory_path() / "arcane-no-symbols";
        std::filesystem::remove_all(hidden);
        std::filesystem::create_directories(hidden);
        const auto run = RunReporter(crash.stem, { "--symbol-path", hidden.string() });
        CHECK(run.exitCode == 0);
        REQUIRE(std::filesystem::exists(sibling));
        const std::string text = Slurp(sibling);
        INFO("symbolized (hidden):\n" << text);
        CHECK(text.find("engine      : dbgeng") != std::string::npos);
        CHECK(text.find("!main") == std::string::npos);
        CHECK(text.find("death-fixture.exe+0x") != std::string::npos);   // the symbol-less form names the IMAGE
    }
}

// The hand-off, end to end (spec §6): the fixture crashes, the crash thread
// spawns the STAGED reporter beside the fixture, the reporter reads the
// envelope and writes <stem>.symbolized.txt after the host is already dead.
TEST_CASE("death fixture --reporter: a crash report gains a .symbolized.txt from the detached reporter", "[diag]")
{
    SkipIfBuildMachine();
    REQUIRE(std::filesystem::exists(std::filesystem::absolute("../death-fixture/ArcaneCrashReporter.exe")));
    const FixtureRun r = RunFixture("av", { "--reporter" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 10);
    REQUIRE_FALSE(r.stem.empty());
    const auto sibling = r.stem.string() + ".symbolized.txt";
    REQUIRE(WaitForFile(sibling, std::chrono::seconds(20)));
    const std::string text = Slurp(sibling);
    CHECK(text.find("symbolized by ArcaneCrashReporter") != std::string::npos);
    CHECK(text.find("thread") != std::string::npos);
}

TEST_CASE("death fixture: an access violation yields a crash report and exit code 10 within the cap, with no dialog", "[diag]")
{
    const FixtureRun r = RunFixture("av");
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 10);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(std::filesystem::exists(r.stem.string() + ".dmp"));
    // Plan 2 (D9): a FATAL report's echo reaches the log file WITHOUT spdlog.
    // The fixture's logDir is derived: <dumpDir>/../Logs/<appName>.log.
    const auto log = std::filesystem::temp_directory_path() / "Logs" / "DeathFixture.log";
    REQUIRE(std::filesystem::exists(log));
    const std::string logText = Slurp(log);
    // The direct append lands in the same file spdlog is writing. This much
    // was green before D9 too -- ARC_ERROR used to carry the line...
    CHECK(logText.find("-- report written") != std::string::npos);
    // ...so the assertion that actually PINS D9 is that the echo is
    // UNPREFIXED. spdlog stamps every line it emits with
    // "[<ts>] [Arcane] [<level>] "; FatalEcho's WriteFile does not. An echo
    // that regressed to ARC_ERROR fails both of these -- the line would no
    // longer start at a newline, and an [error]-prefixed copy would appear.
#if ARC_PLATFORM_WINDOWS
    CHECK(logText.find("\nDiagnostics: crash (unhandled exception) -- report written")
          != std::string::npos);
#else
    // POSIX names the signal (Base/Posix/DiagnosticsPosix.cpp's OnFatalSignal).
    CHECK(logText.find("\nDiagnostics: crash (fatal signal SIGSEGV) -- report written")
          != std::string::npos);
#endif
    CHECK(logText.find("[error] Diagnostics: crash") == std::string::npos);
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "crash");
    CHECK(r.run.wallMs < 15000);
}

TEST_CASE("death fixture: a clean run exits 0 and writes nothing", "[diag]")
{
    const FixtureRun r = RunFixture("none");
    CHECK(r.run.exitCode == 0);
    CHECK(r.stem.empty());
}

// THE FAILURE THIS PINS (task 7). Every death that is NOT an SEH fault is
// invisible today: a failing ARC_ASSERT goes to Mosaic's abort(), an uncaught
// exception to std::terminate, a CRT contract violation to __fastfail -- none
// of them reach the crash thread, so none of them leave a report, and under a
// Debug CRT some of them stop on a modal box no unattended run can dismiss.
// The wall-time bound is part of the assertion, not decoration: a dialog is
// observable here ONLY as a run that burns the 30s cap.
TEST_CASE("death fixture: assert, terminate, abort, invalid parameter, pure call, stack overflow "
          "and OOM all yield a report with the right kind and exit 10, bounded", "[diag]")
{
    struct Row { const char* mode; const char* kind; };
#if ARC_PLATFORM_WINDOWS
    const Row rows[] = { {"assert","assert"}, {"terminate","terminate"}, {"abort","terminate"},
                         {"invalid-parameter","crash"}, {"purecall","crash"},
                         {"stack-overflow","crash"}, {"oom","out-of-memory"} };
#else
    // POSIX has no CRT invalid-parameter or purecall hook: glibc reports a
    // fortify violation by abort() and the Itanium C++ ABI a pure virtual
    // call by std::terminate(), so both arrive as `terminate` -- what the
    // process actually died of on this platform, not a weaker assertion.
    // macOS libc's fortify check traps instead (brk -> SIGTRAP on Apple
    // silicon), which the crash path files as `crash`.
#if ARC_PLATFORM_MACOS
    constexpr const char* kFortifyKind = "crash";
#else
    constexpr const char* kFortifyKind = "terminate";
#endif
    const Row rows[] = { {"assert","assert"}, {"terminate","terminate"}, {"abort","terminate"},
                         {"invalid-parameter",kFortifyKind}, {"purecall","terminate"},
                         {"stack-overflow","crash"}, {"oom","out-of-memory"} };
#endif
    for (const Row& row : rows)
    {
        INFO("mode " << row.mode);
        const FixtureRun r = RunFixture(row.mode);
        CHECK_FALSE(r.run.timedOut);
        CHECK(r.run.exitCode == 10);
        REQUIRE_FALSE(r.stem.empty());
        CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == row.kind);
        CHECK(r.run.wallMs < 20000);
    }
}

// The one member of the family that must NOT kill the process: an ensure is a
// report you walk away from (spec S5.3, UE's continuable report). Envelope
// only -- no minidump -- and the fixture goes on to return 0.
TEST_CASE("death fixture: an ensure writes a lightweight report and the process continues to exit 0", "[diag]")
{
    const FixtureRun r = RunFixture("ensure");
    CHECK(r.run.exitCode == 0);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "ensure");
    CHECK_FALSE(std::filesystem::exists(r.stem.string() + ".dmp"));
}

// Spec S5.4, as a whole process: a hang report is SURVIVABLE. The watchdog
// writes it and hands the host straight back -- the fixture then finishes its
// sleep and leaves by its own `return 0`, WITHOUT calling Shutdown(), which is
// also what proves the raw watchdog thread + atexit hook (task 8) let a main()
// return cleanly where a joinable std::thread used to call std::terminate.
TEST_CASE("death fixture: a hang writes a hang report and the process stays alive until it exits on its own with 0", "[diag]")
{
    const FixtureRun r = RunFixture("none", { "--hang", "5", "--hang-seconds", "1" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 0);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "hang");
}

// The hang hand-off as a whole process (spec S5.4, task 8): the watchdog
// spawns the reporter with the recovered event; unattended, it symbolizes the
// hang's minidump and exits while the host is STILL ALIVE (an unattended hang
// report waits on nothing -- nobody is there to choose), and the host then
// exits 0 on its own. The sibling is polled after the run: the fixture's 8 s
// sleep is normally long enough, but a slow symbol load is no reason to flake.
TEST_CASE("death fixture --reporter: a hang report gains its .symbolized.txt while the host lives", "[diag]")
{
    SkipIfBuildMachine();
    REQUIRE(std::filesystem::exists(std::filesystem::absolute("../death-fixture/ArcaneCrashReporter.exe")));
    const FixtureRun r = RunFixture("none", { "--hang", "8", "--hang-seconds", "1", "--reporter" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 0);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "hang");
    REQUIRE(WaitForFile(r.stem.string() + ".symbolized.txt", std::chrono::seconds(20)));
    CHECK(Slurp(r.stem.string() + ".symbolized.txt").find("thread") != std::string::npos);
}

// Spec S5.7, the other half: from RequestCleanExit() onward the watchdog's beat
// rule is replaced by an exit deadline, and a host that never gets out is NAMED
// ("hang at exit") rather than left wedged on somebody's desk. The fixture asks
// for a clean exit and then never exits, so only the sentinel can end it.
TEST_CASE("death fixture: a hang at exit is named by the sentinel and ends with exit code 12", "[diag]")
{
    const FixtureRun r = RunFixture("none", { "--hang-at-exit", "--exit-seconds", "2" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 12);
    REQUIRE_FALSE(r.stem.empty());
    const auto env = Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "hang");
    CHECK(env->exitCode == 12);
}

namespace
{
    // True once no "*.session" file remains in `dir` (the host deleted it on a
    // clean exit, or the monitor reaped it after deciding).
    bool WaitForNoSessionRecord(const std::filesystem::path& dir, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            bool any = false;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().extension() == ".session") any = true;
            if (!any) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // The stem of the first envelope of `kind` in `dir`, polled until one
    // appears or `timeout` passes -- the monitor writes AFTER the host is
    // gone, and a directory may legitimately hold the host's own survivable
    // report beside it.
    std::filesystem::path WaitForKind(const std::filesystem::path& dir, const std::string& kind, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            {
                if (e.path().extension() != ".arcdiag") continue;
                const auto env = Arcane::Diag::ReadFile(e.path());
                if (env && env->kind == kind) return e.path().parent_path() / e.path().stem();
            }
            if (std::chrono::steady_clock::now() >= deadline) return {};
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::size_t CountEnvelopes(const std::filesystem::path& dir)
    {
        std::size_t n = 0;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.path().extension() == ".arcdiag") ++n;
        return n;
    }
}

// Spec s5.8, the row the in-process path cannot close: __fastfail never
// reaches the filter, so the host writes NOTHING and dies 0xC0000409. The
// pre-launched monitor sees the session record still present (D15), finds no
// report of the host's own, and synthesizes an abnormal-exit report with the
// log tail -- exactly UE's monitor shape.
TEST_CASE("death fixture --monitor: a fail-fast the host never sees becomes an abnormal-exit report from the monitor", "[diag]")
{
    SkipIfBuildMachine();
    const FixtureRun r = RunFixture("fastfail", { "--monitor" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(static_cast<std::uint32_t>(r.run.exitCode) == 0xC0000409u);
    const auto stem = WaitForKind(r.dir, "abnormal-exit", std::chrono::seconds(10));
    REQUIRE_FALSE(stem.empty());
    CHECK(CountEnvelopes(r.dir) == 1);   // the host wrote nothing at exit time: the monitor's is the only one
    const auto env = Arcane::Diag::ReadFile(stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->appName == "DeathFixture");
    CHECK(static_cast<std::uint32_t>(env->exitCode) == 0xC0000409u);
    CHECK(env->reason == "abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN");
    CHECK(std::filesystem::exists(stem.string() + ".txt"));
    // R32: the fixture logs its mode at WARN -- the file sink's flush_on
    // level -- so the line is on disk before __fastfail takes the process.
    CHECK(Slurp(stem.string() + ".log.txt").find("death fixture: mode fastfail") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(stem.string() + ".dmp"));   // no minidump: the monitor never saw the fault
    CHECK(WaitForNoSessionRecord(r.dir, std::chrono::seconds(5)));   // the monitor reaps the record (UE :1408)
}

// The other half of s5.8: a host that died THROUGH the crash path (exit 10,
// report on disk) leaves the monitor silent -- one window per incident. The
// session record is still present (a crash never reaches Shutdown), so this is
// the "fresh report" clause of the rule doing its job.
TEST_CASE("death fixture --monitor: a crash the host reported itself leaves the monitor silent", "[diag]")
{
    SkipIfBuildMachine();
    const FixtureRun r = RunFixture("av", { "--monitor" });
    CHECK(r.run.exitCode == 10);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(WaitForNoSessionRecord(r.dir, std::chrono::seconds(5)));   // the monitor ran, decided, and reaped
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(CountEnvelopes(r.dir) == 1);
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "crash");
}

// D15's reason for existing: an external kill. The harness kills the sleeping
// fixture at its cap (TerminateProcess with 1, a code inside the hosts'
// "known" range) -- no report, no Shutdown, record left behind -> the monitor
// names it. The exit code alone could never have said this was abnormal.
TEST_CASE("death fixture --monitor: an external kill leaves the session record behind and the monitor reports it", "[diag]")
{
    SkipIfBuildMachine();
    const auto exe = std::filesystem::absolute("../death-fixture/" + Arcane::Platform::ExecutableFileName("death-fixture"));
    const auto dir = std::filesystem::temp_directory_path() / "arcane-death-killed-monitor";
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    Arcane::Test::WitnessInvocation inv;
    inv.exePath = exe;
    inv.args = { "--dir", dir.string(), "--die", "none", "--monitor", "--hang", "60", "--hang-seconds", "100" };
    inv.hardCapMs = 3000;   // killed mid-sleep; hang-seconds 100 keeps the watchdog quiet
    const auto run = Arcane::Test::RunWitness(inv);
    REQUIRE(run.timedOut);
    const auto stem = WaitForKind(dir, "abnormal-exit", std::chrono::seconds(10));
    REQUIRE_FALSE(stem.empty());
    const auto env = Arcane::Diag::ReadFile(stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->exitCode == 1);
    CHECK(env->reason == "abnormal-exit: 0x00000001");
    CHECK(WaitForNoSessionRecord(dir, std::chrono::seconds(5)));
}

// The hang-then-kill decision (task 9): a SURVIVABLE report the host wrote
// earlier -- here a hang, reported unattended, so no hang window is up --
// says nothing about a death that came later. The monitor still names the
// kill; only a FATAL report of the host's own, or a live hang window that
// never saw its host recover, speaks for the exit. Under the brief's "any
// .arcdiag newer than the record" rule this case was silent.
TEST_CASE("death fixture --monitor: a hang report written earlier does not silence the monitor when the host is later killed", "[diag]")
{
    SkipIfBuildMachine();
    const auto exe = std::filesystem::absolute("../death-fixture/" + Arcane::Platform::ExecutableFileName("death-fixture"));
    const auto dir = std::filesystem::temp_directory_path() / "arcane-death-hang-then-killed-monitor";
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    Arcane::Test::WitnessInvocation inv;
    inv.exePath = exe;
    inv.args = { "--dir", dir.string(), "--die", "none", "--monitor", "--hang", "60", "--hang-seconds", "1" };
    inv.hardCapMs = 5000;   // the hang report lands at ~1-2 s; the kill at 5 s
    const auto run = Arcane::Test::RunWitness(inv);
    REQUIRE(run.timedOut);
    REQUIRE_FALSE(WaitForKind(dir, "hang", std::chrono::seconds(1)).empty());
    const auto stem = WaitForKind(dir, "abnormal-exit", std::chrono::seconds(10));
    REQUIRE_FALSE(stem.empty());
    CHECK(Arcane::Diag::ReadFile(stem.string() + ".arcdiag")->exitCode == 1);
    CHECK(WaitForNoSessionRecord(dir, std::chrono::seconds(5)));
}

// A clean run deletes its record in Shutdown, so the monitor has nothing to say.
TEST_CASE("death fixture --monitor: a clean exit deletes the session record and the monitor stays silent", "[diag]")
{
    SkipIfBuildMachine();
    const FixtureRun r = RunFixture("none", { "--monitor" });
    CHECK(r.run.exitCode == 0);
    // R104(a): a monitor was REALLY watching -- without this, the silence
    // below would pass just as well if none had ever been launched (the host
    // deletes its own record either way). The fixture's log is derived:
    // <dumpDir>/../Logs/DeathFixture.log, truncated per run by the sink.
    const std::string log = Slurp(std::filesystem::temp_directory_path() / "Logs" / "DeathFixture.log");
    CHECK(log.find("Diagnostics: crash monitor launched (pid ") != std::string::npos);
    CHECK(log.find("monitor on)") != std::string::npos);
    CHECK(WaitForNoSessionRecord(r.dir, std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(CountEnvelopes(r.dir) == 0);
}

// The console family (spec S5.7): Ctrl-C is two-step as in UE -- the first
// press requests the host's clean exit, the second terminates -- and a console
// close requests the SAME clean exit rather than a second one. Driven through
// the SimulateConsoleCtrl seam so the rule is testable without a console, a
// signal, or a process to kill; note that `Armed` runs with
// startHangWatchdog = false, so none of this may depend on the watchdog.
TEST_CASE("diagnostics: the console handler is two-step for Ctrl-C and requests a clean exit on close", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-console-ctrl-test";
    std::filesystem::create_directories(dir);
    Armed armed(dir);
    static int hookCalls = 0; hookCalls = 0;

    // WITH NO HOOK INSTALLED the press is DECLINED, not swallowed. A console
    // tool that never opted in has nothing for a first Ctrl-C to start, so
    // claiming the event would cost the user a press and then kill the process
    // with 0xC000013A instead of the exit they asked for. Returning false
    // hands it to Windows' default handler, which terminates exactly as it did
    // before this module existed.
    Arcane::Diagnostics::SetCleanExitHook(nullptr, nullptr);
    CHECK_FALSE(Arcane::Diagnostics::SimulateConsoleCtrl(0 /*CTRL_C_EVENT*/));
    CHECK(hookCalls == 0);

    Arcane::Diagnostics::SetCleanExitHook([](void*) { ++hookCalls; }, nullptr);
    CHECK(Arcane::Diagnostics::SimulateConsoleCtrl(0 /*CTRL_C_EVENT*/));
    CHECK(hookCalls == 1);
    // A second Ctrl-C would terminate: not simulated. Close requests the same clean exit once.
    CHECK(Arcane::Diagnostics::SimulateConsoleCtrl(2 /*CTRL_CLOSE_EVENT*/));
    CHECK(hookCalls == 1);   // idempotent

    // The slot is PROCESS-wide and outlives this case's Armed: leave it empty
    // rather than pointing every later RequestCleanExit at this case's counter.
    Arcane::Diagnostics::SetCleanExitHook(nullptr, nullptr);
}
