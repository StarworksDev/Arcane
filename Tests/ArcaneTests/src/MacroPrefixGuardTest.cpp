// One macro prefix (user, 2026-10-03): no C++ (or HLSL) macro is spelled ARCANE_*.
// Environment variables (getenv names) keep ARCANE_ and are allow-listed here.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "Helpers/ReferenceProjectDir.hpp"

namespace
{
    const std::set<std::string> kEnvNames = {
        "ARCANE_SDK", "ARCANE_BUILD_MACHINE", "ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE", "ARCANE_IDE_DESK",
        "ARCANE_IDE_DESK_FILE", "ARCANE_DIAG_DESK", "ARCANE_BUILD_DESK", "ARCANE_THUMBS_BLESS",
        "ARCANE_TEST_CAPTURE_DIR", "ARCANE_TESTS_NO_D3D12_DEBUG", "ARCANE_RESAVE_SCENES",
        "ARCANE_RECORD_TRAJECTORY", "ARCANE_SHADER_DIR", "ARCANE_CAPTURE_PHYSICS_BASELINE",
        "ARCANE_TEST_NO_PHYSICS_HOOK" };
}

TEST_CASE("Macro prefix guard: no ARCANE_* token outside the environment-variable allow-list", "[guard]")
{
    namespace fs = std::filesystem;
    // The repo root, found the way ArcaneSpellingGuardTest finds it (this suite has no source-root define).
    const fs::path root = Arcane::Test::FindReferenceProjectDir().parent_path();
    REQUIRE_FALSE(root.empty());
    const std::regex token(R"(\bARCANE_[A-Z0-9_]+)");
    std::vector<std::string> hits;
    // Every module and program lives under Source/<Group>/ (engine modularization P3).
    for (const char* dir : { "Source", "Tests/ArcaneTests", "ReferenceProject/Source", "data/shaders" })
    {
        for (const auto& e : fs::recursive_directory_iterator(root / dir))
        {
            const auto ext = e.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".inl" && ext != ".hlsl" && ext != ".hlsli")
                continue;
            if (e.path().filename() == "MacroPrefixGuardTest.cpp") continue;
            std::ifstream in(e.path());
            std::string line; int n = 0;
            while (std::getline(in, line))
            {
                ++n;
                for (std::sregex_iterator it(line.begin(), line.end(), token), end; it != end; ++it)
                    if (!kEnvNames.contains(it->str()))
                        hits.push_back(e.path().string() + ":" + std::to_string(n) + " " + it->str());
            }
        }
    }
    INFO("hits: " << hits.size() << ", first: " << (hits.empty() ? std::string{} : hits.front()));
    CHECK(hits.empty());
}
