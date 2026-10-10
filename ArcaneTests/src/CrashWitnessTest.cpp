// Crash-window witness lanes (spec s10): the REAL staged ArcaneRuntime, headless,
// with the reporter staged beside it -- the copy WitnessScratch makes carries
// ArcaneCrashReporter.exe, so the default reporterPath resolves inside the copy.
#include "Helpers/HostWitness.hpp"
#include <Arcane/Platform/Platform.hpp>   // ExecutableFileName: .exe on Windows only
#include <Arcane/Base/DiagEnvelope.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace Arcane::Test;

namespace
{
    std::filesystem::path StagedRuntimeDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneRuntime");
        REQUIRE(std::filesystem::exists(p / Arcane::Platform::ExecutableFileName("ArcaneRuntime")));
        REQUIRE(std::filesystem::exists(p / Arcane::Platform::ExecutableFileName("ArcaneCrashReporter")));   // staged by the host's postbuild (task 4)
        return p;
    }
    WitnessInvocation HostInv(const WitnessScratch& scratch, std::vector<std::string> extraArgs)
    {
        WitnessInvocation inv;
        inv.exePath    = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneRuntime");
        inv.workingDir = scratch.Dir();
        inv.reportPath = scratch.Dir() / "witness-report.json";
        inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "vulkan",
                     "--report", inv.reportPath.generic_string() };
        inv.args.insert(inv.args.end(), extraArgs.begin(), extraArgs.end());
        inv.hardCapMs = 120000;
        return inv;
    }
    // R105 (controller ruling): the directory iterator's yield order is NOT
    // time-ordered, and the diagnostics dir can also hold the REPORTER's own
    // report (Diagnostics installs itself with dumpDir = the report dir --
    // see this file's header comment -- so an ArcaneCrashReporter-*.arcdiag
    // can land right beside the host's). Consider only the HOST's files
    // (the "ArcaneRuntime-" stem prefix Diagnostics.cpp mints) and pick the
    // newest by last_write_time, never iteration order.
    std::filesystem::path NewestStem(const std::filesystem::path& dir)
    {
        std::filesystem::path found;
        std::filesystem::file_time_type foundTime{};
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        {
            if (e.path().extension() != ".arcdiag") continue;
            if (e.path().filename().string().rfind("ArcaneRuntime-", 0) != 0) continue;
            std::error_code tec;
            const auto t = std::filesystem::last_write_time(e.path(), tec);
            if (tec) continue;
            if (found.empty() || t > foundTime)
            {
                found = e.path().parent_path() / e.path().stem();
                foundTime = t;
            }
        }
        return found;
    }
    bool WaitForFile(const std::filesystem::path& p, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!std::filesystem::exists(p) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return std::filesystem::exists(p);
    }
    // R107 (controller ruling, fix round 1): mirrors
    // CrashPathTest.cpp:225-229's SkipIfBuildMachine EXACTLY --
    // global-constraints.md bullet 4 is binding for every test that needs a
    // SPAWNED reporter, witness siblings included: skip only when CI or
    // ARCANE_BUILD_MACHINE is set AND ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE
    // is NOT, so Jenkins can opt these lanes in later without the override
    // going silently unhonoured. Kept file-local, this suite's convention
    // (CrashPathTest.cpp does not export its own copy either).
    void SkipIfBuildMachine()
    {
        if ((std::getenv("CI") || std::getenv("ARCANE_BUILD_MACHINE")) && !std::getenv("ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE"))
            SKIP("CI/ARCANE_BUILD_MACHINE set -- the reporter is never spawned on a build machine (spec §6; "
                 "ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE overrides)");
    }
}

// H1: the main thread stops beating for kHangMainSeconds (15 s) at frame 30 --
// past the 12 s default threshold -- so the watchdog writes a SURVIVABLE hang
// report into the project's Saved/Diagnostics, hands off to the reporter, and
// the host then finishes its frames and exits 0. The reporter's sibling lands
// while the host is still alive (unattended: symbolize and exit).
TEST_CASE("H1: a scripted main-thread hang yields a hang report, the reporter sibling, and a clean exit", "[witness][gpu]")
{
    WitnessScratch scratch(StagedRuntimeDir(), "h1-hang-main");
    WitnessRun run = RunWitness(HostInv(scratch, { "--frames", "90", "--hang-main", "30" }));
    INFO("host stderr: " << run.stderrPath.string());
    CHECK_FALSE(run.timedOut);
    CHECK(run.exitCode == 0);
    CHECK(run.wallMs >= 15000);   // the sleep really happened

    const auto diagDir = scratch.Dir() / "ReferenceProject" / "Saved" / "Diagnostics";   // RuntimeApp retargets here on project open
    const auto stem = NewestStem(diagDir);
    REQUIRE_FALSE(stem.empty());
    const auto env = Arcane::Diag::ReadFile(stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "hang");
    CHECK(env->exitCode == 0);
    CHECK(std::filesystem::exists(stem.string() + ".dmp"));
    SkipIfBuildMachine();
    REQUIRE(WaitForFile(stem.string() + ".symbolized.txt", std::chrono::seconds(20)));
}

// G1: the deliberate GPU fault. Desk-only (a TDR resets the driver): gated on
// ARCANE_DIAG_DESK like the arcbuild desk probes. Exit 1 is the hosts'
// device-loss code (Diagnostics.cpp's gpu-crash submit), not 10.
//
// R106 (controller ruling, binding): this lane must NEVER be run with
// ARCANE_DIAG_DESK=1 by an agent -- a deliberate GPU fault triggers a
// machine-wide TDR that resets the display driver while the user is working
// on this desk. Task 10 proves only that the lane SKIPS cleanly without the
// env var; the live TDR run is deferred to Task 11's desk proof, where the
// user is warned first.
TEST_CASE("G1: a deliberate GPU fault yields a gpu-crash report with the reporter sibling", "[witness][gpu]")
{
    if (!std::getenv("ARCANE_DIAG_DESK")) SKIP("ARCANE_DIAG_DESK not set -- desk-only TDR lane");
    WitnessScratch scratch(StagedRuntimeDir(), "g1-crash-gpu");
    WitnessRun run = RunWitness(HostInv(scratch, { "--frames", "60", "--crash-gpu", "30" }));
    CHECK_FALSE(run.timedOut);
    const auto stem = NewestStem(scratch.Dir() / "ReferenceProject" / "Saved" / "Diagnostics");
    REQUIRE_FALSE(stem.empty());
    const auto env = Arcane::Diag::ReadFile(stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "gpu-crash");
    REQUIRE(WaitForFile(stem.string() + ".symbolized.txt", std::chrono::seconds(20)));
}
