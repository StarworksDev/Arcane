// ExtractHostTest.cpp -- behavioural extract-skip (F5 plan 1 Task 11):
// a rendering host prints how many times it extracted, and a host that
// never renders leaves the counter at 0. Behaviour only; no source scan.
#include "Helpers/HostWitness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/ExtractAccounting.hpp>   // ArcaneCore
#include <Arcane/Base/Runtime.hpp>             // ArcaneEngine
#include <Arcane/Platform/Platform.hpp>        // ExecutableFileName: .exe on Windows only
#include "Helpers/TestTypeContext.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("a non-rendering runtime leaves the extract counter at 0", "[nri][extract]")
{
    Arcane::ResetExtractFrameCallCount();
    Arcane::Runtime rt(Arcane::Test::Process());
    for (int i = 0; i < 4; ++i)
        rt.Loop().Advance(1.0 / 60.0);
    CHECK(Arcane::ExtractFrameCallCount() == 0u);
}

static std::string Slurp(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int CountExactLine(const std::string& text, const std::string& want)
{
    int n = 0;
    std::size_t i = 0;
    while (i < text.size())
    {
        std::size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(i, end - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == want) ++n;
        i = end + (end < text.size() ? 1 : 0);
        if (end == text.size()) break;
    }
    return n;
}

TEST_CASE("RuntimeApp --headless prints extract-frames equal to --frames", "[gpu][extract][witness]")
{
    const std::filesystem::path dir = std::filesystem::absolute("../ArcaneRuntime");
    REQUIRE(std::filesystem::exists(dir / Arcane::Platform::ExecutableFileName("ArcaneRuntime")));
    Arcane::Test::WitnessScratch scratch(dir, "extract-headless");
    Arcane::Test::WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneRuntime");
    inv.workingDir = scratch.Dir();
    inv.args = { "--project", "ReferenceProject", "--frames", "4", "--headless" };
    inv.hardCapMs = 60000;
    Arcane::Test::WitnessRun run = Arcane::Test::RunWitness(inv);
    INFO(Slurp(run.stdoutPath));
    CHECK(run.exitCode == 0);
    CHECK(CountExactLine(Slurp(run.stdoutPath), "extract-frames: 4") == 1);
}

TEST_CASE("ArcaneServer ticks N frames with extractFrameCalls 0", "[witness][server][extract]")
{
    const std::filesystem::path dir = std::filesystem::absolute("../ArcaneServer");
    REQUIRE(std::filesystem::exists(dir / Arcane::Platform::ExecutableFileName("ArcaneServer")));
    Arcane::Test::WitnessScratch scratch(dir, "extract-server");
    Arcane::Test::WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneServer");
    inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "ReferenceProject", "--frames", "4", "--report", inv.reportPath.generic_string() };
    inv.hardCapMs = 60000;
    Arcane::Test::WitnessRun run = Arcane::Test::RunWitness(inv);
    REQUIRE(run.exitCode == 0);
    CHECK(run.report.at("framesTicked") == 4);
    CHECK(run.report.at("extractFrameCalls") == 0);
    CHECK(run.report.at("systems").at("hasRenderSubmission") == false);
    CHECK(run.report.at("systems").at("render") == 0);
    CHECK(run.report.at("presentation").at("clientAttached") == false);
    CHECK(run.report.at("presentation").at("clientDllLoadedAtBoot") == false);
    CHECK(run.report.at("schemaVersion") == 1);
}
