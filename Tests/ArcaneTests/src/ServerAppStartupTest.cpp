// ServerApp startup (review round 1, Task 17): a requested --remote whose Start
// fails ends the run through Finish (exit 2, report written), and a --report
// that cannot be written can never leave the exit code at 0. No test builds a
// ServerApp, so this is a source pin; the Servitor launch with an unwritable
// --remote-descriptor is the behavior witness.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace
{
    std::string ReadRepoFile(const char* relative)
    {
        std::ifstream in(Arcane::Test::RepoRoot() / relative);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
}

TEST_CASE("a requested remote Start failure returns Finish, and a failed report cannot exit 0", "[server]")
{
    const auto text = ReadRepoFile("Source/Programs/ArcaneServer/src/ServerApp.cpp");
    const auto start = text.find("if (!m_remote->Start())");
    REQUIRE(start != std::string::npos);
    const auto window = text.substr(start, 260);
    CHECK(window.find("return Finish") != std::string::npos);
    CHECK(window.find("m_remote.reset()") == std::string::npos);
    // Final fix pass (b6 minor): HostSession's last line says "this host runs
    // without remote", so the server says it exits 2 before it does.
    const auto said = window.find("exiting 2");
    CHECK(said != std::string::npos);
    CHECK(said < window.find("return Finish"));
    const auto finish = text.find("ServerApp::Finish");
    REQUIRE(finish != std::string::npos);
    const auto finishBody = text.substr(finish, 500);
    CHECK(finishBody.find("return 1") != std::string::npos);
}
