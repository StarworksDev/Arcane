// The s6 witness: spawn the REAL staged ArcaneServer.exe and read its census. Same
// harness as the runtime witnesses (Helpers/HostWitness.hpp), same fresh-copy
// hygiene; [server], not [gpu] -- this host has no device (P14).
#include "Helpers/HostWitness.hpp"
#include <Arcane/Platform/Platform.hpp>   // ExecutableFileName: .exe on Windows only
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
using namespace Arcane::Test;
namespace
{
    std::filesystem::path StagedServerDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneServer");
        INFO("staged ArcaneServer not found -- build Arcane.slnx first: " << p.string());
        REQUIRE(std::filesystem::exists(p / Arcane::Platform::ExecutableFileName("ArcaneServer")));
        return p;
    }
    std::string SlurpServerLog(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
}
TEST_CASE("S1: ArcaneServer opens the project, loads the module, ticks N frames headless, and constructs no presentation", "[witness][server]")
{
    WitnessScratch scratch(StagedServerDir(), "s1-census");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneServer"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "ReferenceProject", "--frames", "30", "--report", inv.reportPath.generic_string() };
    inv.hardCapMs = 60000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    CHECK(run.exitCode == 0);
    const auto& r = run.report;
    CHECK(r.at("schemaVersion") == 1);
    CHECK(r.at("host") == "ArcaneServer");
    CHECK(r.at("netMode") == "DedicatedServer");
    CHECK(r.at("isDedicatedServerProcess") == true);
    CHECK(r.at("project").at("opened") == true);
    CHECK(r.at("project").at("name") == "ReferenceProject");
    CHECK(r.at("module").at("loaded") == true);
    CHECK(r.at("framesTicked") == 30);
    CHECK(r.at("systems").at("hasPhysics") == true);
    CHECK(r.at("systems").at("hasPropagation") == true);
    CHECK(r.at("systems").at("hasRenderSubmission") == false);
    CHECK(r.at("systems").at("render") == 0);
    CHECK(r.at("presentation").at("clientAttached") == false);
    CHECK(r.at("presentation").at("clientDllLoadedAtBoot") == false);   // the exe's link line has no ArcaneClient (spec s6)
    CHECK(r.at("exitReason") == "frames-complete");
}
TEST_CASE("S2: ArcaneServer refuses a missing project with a report that says so", "[witness][server]")
{
    WitnessScratch scratch(StagedServerDir(), "s2-no-project");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneServer"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "DoesNotExist", "--frames", "1", "--report", inv.reportPath.generic_string() };
    WitnessRun run = RunWitness(inv);
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    CHECK(run.exitCode != 0);
    CHECK(run.report.at("project").at("opened") == false);
    CHECK(run.report.at("exitReason") == "project-open-failed");
}
TEST_CASE("S3: ArcaneServer's --fixed-dt is REAL -- the census reports the ACTUAL step, not the requested value echoed blind", "[witness][server]")
{
    // Review round 1: the census's fixedDt is derived from
    // RunLoop::Loop().FixedHz() AFTER ServerApp::Run() calls SetFixedHz(1 /
    // cfg.fixedDtSeconds) -- this is the end-to-end proof that --fixed-dt 0.05
    // makes the loop's canonical step 0.05s, not merely a config echo (see
    // ServerFixedRateTest.cpp for the unit-level proof against RunLoop directly).
    WitnessScratch scratch(StagedServerDir(), "s3-fixed-dt");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneServer"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "ReferenceProject", "--frames", "10", "--fixed-dt", "0.05", "--report", inv.reportPath.generic_string() };
    inv.hardCapMs = 60000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    CHECK(run.exitCode == 0);
    const auto& r = run.report;
    CHECK(r.at("fixedDt") == 0.05);
    CHECK(r.at("framesTicked") == 10);
    CHECK(r.at("exitReason") == "frames-complete");
}
TEST_CASE("S4: ArcaneServer's stdin admin console answers get/set/list/explain as ServerAdmin, defaults cheats off, and audits the set", "[witness][server]")
{
    // Settings arc S7 (spec s9, s3.2). The script is the process's stdin: the
    // reader thread queues it at once, the tick loop drains it between ticks.
    WitnessScratch scratch(StagedServerDir(), "s4-admin-console");
    const std::filesystem::path script = scratch.Dir() / "admin.txt";
    {
        std::ofstream f(script, std::ios::binary);
        f << "get server.cheatsAllowed\n"
             "set server.allowClientSetServer true\n"
             "get server.allowClientSetServer\n"
             "list server.\n"
             "explain server.allowClientSetServer\n"
             "bogus.cvar.name\n";
    }
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneServer.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "ReferenceProject", "--frames", "60", "--report", inv.reportPath.generic_string() };
    inv.stdinPath = script;
    inv.hardCapMs = 60000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    CHECK(run.exitCode == 0);

    const std::string out = SlurpServerLog(run.stdoutPath);
    CHECK(out.find("OK server.cheatsAllowed = false") != std::string::npos);      // the dedicated default
    CHECK(out.find("OK server.allowClientSetServer = true") != std::string::npos); // set, then get
    CHECK(out.find("server.cheats = ") != std::string::npos);                     // in the list
    CHECK(out.find("Console") != std::string::npos);                              // explain's history
    CHECK(out.find("ERR unknown cvar 'bogus.cvar.name'") != std::string::npos);
    const std::string log = out + SlurpServerLog(run.stderrPath);
    CHECK(log.find("cvar-audit server.allowClientSetServer false -> true by stdin (applied)") != std::string::npos);
}
