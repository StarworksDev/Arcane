// HostWitness: the spawn helper Arc B's [witness][gpu] scenarios are built
// on (later tasks). This file is the [witness-unit] slice only -- process
// facts, the watchdog, report loading, and scratch hygiene -- against
// cmd.exe, so it needs no GPU and no host binaries. Task 3 brief,
// verbatim test code.
#include "Helpers/HostWitness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/Verdict.hpp>
#include <Arcane/Platform/Platform.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using Arcane::Test::WitnessInvocation;
using Arcane::Test::RunWitness;
using Arcane::Test::GradeProcessFacts;

// The platform's command interpreter: cmd.exe /c on Windows, /bin/sh -c on
// POSIX (Linux port). Each case below spells its command in both dialects.
static WitnessInvocation CmdInv(std::string cmdArg, std::filesystem::path reportPath = {})
{
    WitnessInvocation inv;
#if ARC_PLATFORM_WINDOWS
    inv.exePath    = "C:/Windows/System32/cmd.exe";
    inv.args       = { "/c", std::move(cmdArg) };
#else
    inv.exePath    = "/bin/sh";
    inv.args       = { "-c", std::move(cmdArg) };
#endif
    inv.workingDir = std::filesystem::temp_directory_path();
    inv.reportPath = std::move(reportPath);
    return inv;
}

TEST_CASE("witness: exit code is captured", "[witness-unit]")
{
    auto run = RunWitness(CmdInv("exit 3"));
    REQUIRE(run.exitCode == 3);
    REQUIRE_FALSE(run.timedOut);
}

TEST_CASE("witness: hard cap kills and reports timedOut", "[witness-unit]")
{
#if ARC_PLATFORM_WINDOWS
    auto inv = CmdInv("ping -n 30 127.0.0.1 >nul");
#else
    auto inv = CmdInv("sleep 30");
#endif
    inv.hardCapMs = 1500;
    auto run = RunWitness(inv);
    REQUIRE(run.timedOut);
    REQUIRE(GradeProcessFacts(run) == Arcane::Verdict::Errored);
}

TEST_CASE("witness: exit 0 with NO report is Indeterminate, never a pass", "[witness-unit]")
{
    // UE's cascade grades this Passed ("no report to parse"). We refuse that.
    auto inv = CmdInv("exit 0",
        std::filesystem::temp_directory_path() / "arc-witness-absent-report.json");
    std::filesystem::remove(inv.reportPath);
    auto run = RunWitness(inv);
    REQUIRE(run.exitCode == 0);
    REQUIRE_FALSE(run.reportFound);
    REQUIRE(GradeProcessFacts(run) == Arcane::Verdict::Indeterminate);
}

TEST_CASE("witness: malformed report is Indeterminate", "[witness-unit]")
{
    const auto rp = std::filesystem::temp_directory_path() / "arc-witness-junk.json";
    { std::ofstream f(rp); f << "{not json"; }
    auto run = RunWitness(CmdInv("exit 0", rp));
    REQUIRE(run.reportFound);
    REQUIRE_FALSE(run.reportParsed);
    REQUIRE(GradeProcessFacts(run) == Arcane::Verdict::Indeterminate);
    std::filesystem::remove(rp);
}

TEST_CASE("witness: a parsed report defers to the scenario", "[witness-unit]")
{
    const auto rp = std::filesystem::temp_directory_path() / "arc-witness-ok.json";
    { std::ofstream f(rp); f << R"({"schemaVersion":5})"; }
    auto run = RunWitness(CmdInv("exit 0", rp));
    REQUIRE(run.reportParsed);
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    std::filesystem::remove(rp);
}

TEST_CASE("witness: stdout is captured to a file", "[witness-unit]")
{
    auto run = RunWitness(CmdInv("echo witness-marker"));
    REQUIRE(std::filesystem::exists(run.stdoutPath));
    std::ifstream f(run.stdoutPath); std::string all((std::istreambuf_iterator<char>(f)), {});
    REQUIRE(all.find("witness-marker") != std::string::npos);
}

TEST_CASE("witness scratch: copies, and deletes on clean exit", "[witness-unit]")
{
    const auto src = std::filesystem::temp_directory_path() / "arc-scratch-src";
    std::filesystem::create_directories(src / "sub");
    { std::ofstream f(src / "sub" / "a.txt"); f << "x"; }
    std::filesystem::path copied;
    {
        Arcane::Test::WitnessScratch s(src, "unit");
        copied = s.Dir();
        REQUIRE(std::filesystem::exists(copied / "sub" / "a.txt"));
    }
    REQUIRE_FALSE(std::filesystem::exists(copied));
    std::filesystem::remove_all(src);
}
