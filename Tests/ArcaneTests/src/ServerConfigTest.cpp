// ArcaneServer's CLI (spec 2026-09-15 s6: "mirrors the other hosts"). Core-side --
// HostConfig is Client (it names a GraphicsBackend), so the server owns its own
// vocabulary over the same Arcane::Cli. Source-compiled into the tests like
// arcbuild's Request.cpp.
#include <catch2/catch_test_macros.hpp>
#include "ServerConfig.hpp"
#include <string>
#include <vector>
namespace
{
    Arcane::Server::ServerConfig::ParseOutcome Parse(std::vector<const char*> args)
    { args.insert(args.begin(), "ArcaneServer"); return Arcane::Server::ServerConfig::Parse(static_cast<int>(args.size()), const_cast<char**>(args.data())); }
}
// Servitor P0 Task 10: the server --remote flags. Dist ServerConfig::Parse
// refuses them (the #else case), so the parse cases are Dist-guarded.
#if !defined(ARC_BUILD_DIST)
TEST_CASE("ServerConfig parses --remote and its relaunch line strips it", "[remote][host]") {
    const char* argv[] = {"ArcaneServer", "--project", "ReferenceProject", "--remote",
                          "--remote-descriptor", "C:/secret.json", "--remote-idle-exit", "0"};
    auto parsed = Arcane::Server::ServerConfig::Parse(8, const_cast<char**>(argv));
    REQUIRE(parsed.config);
    CHECK(parsed.exitCode == 0);
    CHECK(parsed.config->remote);
    CHECK(parsed.config->remoteDescriptor == "C:/secret.json");
    CHECK(parsed.config->remoteIdleExitSeconds == 0);
    CHECK(parsed.config->remoteIdleExitSupplied);
    const std::string line = Arcane::Server::SanitizedRelaunchLine("ArcaneServer.exe", 8, const_cast<char**>(argv));
    CHECK(line.find("--remote") == std::string::npos);
    CHECK(line.find("secret.json") == std::string::npos);
    CHECK(line.find("ReferenceProject") != std::string::npos);
    CHECK(line.find("--project") != std::string::npos);
}
TEST_CASE("ServerConfig: the remote defaults, and the same refusals as HostConfig", "[remote][host]") {
    auto plain = Parse({"--project", "P"});
    REQUIRE(plain.config);
    CHECK_FALSE(plain.config->remote);
    CHECK(plain.config->remoteDescriptor.empty());
    CHECK(plain.config->remoteIdleExitSeconds == 600);
    CHECK_FALSE(plain.config->remoteIdleExitSupplied);

    // --remote takes no value: the strip must not eat the --project after it.
    const char* bare[] = {"ArcaneServer", "--remote", "--project", "P", "--remote-idle-exit=9"};
    CHECK(Arcane::Server::SanitizedRelaunchLine("ArcaneServer.exe", 5, const_cast<char**>(bare))
          == "ArcaneServer.exe --project P");

    const std::vector<std::vector<const char*>> refused = {
        {"--project", "P", "--remote-descriptor"},
        {"--project", "P", "--remote-descriptor="},
        {"--project", "P", "--remote-descriptor", "--remote"},
        {"--project", "P", "--remote-idle-exit", "soon"},
        {"--project", "P", "--remote-idle-exit", "-1"},
        {"--project", "P", "--remote-idle-exit", "4294967296"},   // one past std::uint32_t
    };
    for (const auto& args : refused)
    {
        INFO("args[2] = " << args[2]);
        auto bad = Parse(args);
        CHECK_FALSE(bad.config.has_value());
        CHECK(bad.exitCode == 2);
    }
}

// Servitor P0 Task 29 (ruling B): the server's --remote help states the AM1 hold.
TEST_CASE("ServerConfig: --remote's help states the held start", "[remote][host]") {
    const std::string_view help = Arcane::Server::kRemoteFlagHelp;
    CHECK(help.starts_with("DEV: arm the loopback remote surface"));
    CHECK(help.find("an open-ended run starts held at frame 0; use servitor play/resume, or pass --frames N")
          != std::string_view::npos);
}
#else
TEST_CASE("ServerConfig: Dist refuses the remote flags at parse", "[server][dist]") {
    for (const auto& args : std::vector<std::vector<const char*>>{
             {"--project", "P", "--remote"}, {"--project", "P", "--remote-descriptor", "C:/d.json"},
             {"--project", "P", "--remote-idle-exit=0"}})
    {
        auto bad = Parse(args);
        CHECK_FALSE(bad.config.has_value());
        CHECK(bad.exitCode == 2);
    }
}
#endif
TEST_CASE("ServerConfig: --project is required; the defaults are the documented ones", "[server]")
{
    CHECK_FALSE(Parse({}).config.has_value());
    CHECK(Parse({}).exitCode == 2);
    auto ok = Parse({"--project", "ReferenceProject"});
    REQUIRE(ok.config);
    CHECK(ok.config->projectPath == "ReferenceProject");
    CHECK(ok.config->frames == 0);
    CHECK_FALSE(ok.config->fixedDtSupplied);   // no --fixed-dt: the tick is 1/server.tickHz (SweepSimTest)
    CHECK(ok.config->reportPath.empty());
}
TEST_CASE("ServerConfig: --frames, --fixed-dt, --report, --plugin parse; a non-positive --fixed-dt is refused", "[server]")
{
    auto ok = Parse({"--project", "P", "--frames", "30", "--fixed-dt", "0.02", "--report", "r.json", "--plugin", "X.dll"});
    REQUIRE(ok.config);
    CHECK(ok.config->frames == 30); CHECK(ok.config->fixedDtSeconds == 0.02);
    CHECK(ok.config->reportPath == "r.json"); CHECK(ok.config->pluginPath == "X.dll");
    CHECK_FALSE(Parse({"--project", "P", "--fixed-dt", "0"}).config.has_value());
    CHECK_FALSE(Parse({"--project", "P", "--fixed-dt", "-1"}).config.has_value());
}
TEST_CASE("ServerConfig: --print-engine-info needs no --project; --help exits 0", "[server]")
{
    CHECK(Parse({"--print-engine-info"}).config->printEngineInfo);
    auto h = Parse({"--help"}); CHECK_FALSE(h.config); CHECK(h.exitCode == 0);
}
TEST_CASE("ServerConfig: the stdin admin console is on by default and --no-admin-console turns it off", "[server]")
{
    auto on = Parse({"--project", "P"});
    REQUIRE(on.config);
    CHECK(on.config->adminConsole);
    auto off = Parse({"--project", "P", "--no-admin-console"});
    REQUIRE(off.config);
    CHECK_FALSE(off.config->adminConsole);
}
