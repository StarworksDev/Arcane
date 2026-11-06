// THE RULE (input-seam spec 2026-10-02 s6.3): GAME-FACING CODE SPELLS
// Arcane:: ONLY. The standalone libraries keep their namespaces underneath;
// Arcane/Ecs.hpp, Arcane/EcsFwd.hpp and Arcane/Reflection.hpp re-export what
// game code needs. This test fails when a library spelling -- Astra::,
// ASTRA_*, Manifold2D, Mosaic:: -- appears in game-facing code where the
// spec forbids it. The exemptions differ by surface (s6.3):
//
//   - ReferenceProject's game sources and every editor C++ template render:
//     OUTSIDE COMMENTS only (ScanMode::CommentsOnly). An #include of a
//     library header, a #define, a string literal or an ARC_INTERNAL fence
//     in game code is exactly the leak the guard exists to catch.
//   - The 21 headers a game module reads (kGameFacingHeaders): PUBLIC
//     DECLARATIONS only (ScanMode::PublicDeclarations) -- comments, string
//     literals, preprocessor directives (incl. #define continuations) and
//     `// ARC_INTERNAL_BEGIN: <why>` ... `// ARC_INTERNAL_END` fences are
//     exempt, since the engine includes and implements on the libraries.

#include <catch2/catch_test_macros.hpp>

#include <Project/ClassTemplates.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "Helpers/ReferenceProjectDir.hpp"

namespace
{
    constexpr const char* kGameFacingHeaders[] = {
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/GameModule.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/GameComponents.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/GameSystems.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/SystemFactory.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/PluginABI.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Base/ProcessContext.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.hpp",
        "Source/Runtime/ArcaneCore/src/Arcane/Base/Assert.hpp",
        "Source/Runtime/ArcaneCore/src/Arcane/Base/Log.hpp",
        "Source/Client/ArcaneClient/src/Arcane/Client/ClientRuntime.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneResources.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCacheRecords.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/Components.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/TransformSystems.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsComponents.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsSystem2D.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsEvents2D.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Physics2D.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Physics2DFwd.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DWorld.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DHandles.hpp",
    };

    struct Hit { std::string where; int line; std::string text; };

    enum class ScanMode
    {
        CommentsOnly,       // game sources + template renders: only comments are exempt
        PublicDeclarations, // the 21 headers: comments, strings, preprocessor lines and fences are exempt
    };

    // Line-oriented scan. Comments are always stripped; PublicDeclarations also
    // strips string literals, preprocessor directives (+ continuations) and
    // ARC_INTERNAL fences. CommentsOnly still parses string literals (so a
    // "//" inside one is not read as a comment) but keeps their contents.
    std::vector<Hit> Scan(const std::string& where, const std::string& text, ScanMode mode,
                          const char* pattern = R"((\bAstra::|\bASTRA_[A-Z_]+|\bManifold2D\b|\bMosaic::))")
    {
        const bool decl = mode == ScanMode::PublicDeclarations;
        const std::regex library(pattern);
        std::vector<Hit> hits;
        std::istringstream in(text);
        std::string raw;
        int lineNo = 0;
        bool inBlock = false, inFence = false, inDefine = false;
        while (std::getline(in, raw))
        {
            ++lineNo;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            if (decl)
            {
                if (raw.find("ARC_INTERNAL_BEGIN") != std::string::npos) { inFence = true;  continue; }
                if (raw.find("ARC_INTERNAL_END")   != std::string::npos) { inFence = false; continue; }
                if (inFence) continue;
                if (inDefine) { inDefine = !raw.empty() && raw.back() == '\\'; continue; }
            }

            std::string code;
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                if (inBlock)
                {
                    if (raw.compare(i, 2, "*/") == 0) { inBlock = false; ++i; }
                    continue;
                }
                if (raw.compare(i, 2, "/*") == 0) { inBlock = true; ++i; continue; }
                if (raw.compare(i, 2, "//") == 0) break;
                if (raw[i] == '"')
                {
                    // R"( ... )" raw strings on one line, and ordinary strings
                    const bool rawStr = i > 0 && raw[i - 1] == 'R';
                    const std::string close = rawStr ? ")\"" : "\"";
                    std::size_t j = i + 1;
                    while (j < raw.size())
                    {
                        if (!rawStr && raw[j] == '\\') { j += 2; continue; }
                        if (raw.compare(j, close.size(), close) == 0) break;
                        ++j;
                    }
                    const std::size_t end = std::min(j + close.size(), raw.size());
                    code += decl ? std::string("\"\"") : raw.substr(i, end - i);
                    i = end - 1;
                    continue;
                }
                code += raw[i];
            }
            const auto first = code.find_first_not_of(" \t");
            if (decl && first != std::string::npos && code[first] == '#')
            {
                inDefine = !raw.empty() && raw.back() == '\\';
                continue;
            }
            if (std::regex_search(code, library))
                hits.push_back({ where, lineNo, raw });
        }
        return hits;
    }

    std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf(); return ss.str();
    }

    void Report(const std::vector<Hit>& hits)
    {
        for (const Hit& h : hits)
            UNSCOPED_INFO(h.where << ":" << h.line << ": " << h.text);
        CHECK(hits.empty());
    }
}

namespace
{
    const std::string kScannerSample =
        "// Astra::Registry in a comment\n"
        "const char* s = \"Astra::Registry\";\n"
        "#include <Manifold2D/Physics/PhysicsWorld.hpp>\n"
        "#define M(x) \\\n"
        "    Astra::Thing(x)\n"
        "// ARC_INTERNAL_BEGIN: test\n"
        "Astra::Registry hidden;\n"
        "// ARC_INTERNAL_END\n"
        "Arcane::Registry fine;\n"
        "Astra::Registry leaked;\n";
}

TEST_CASE("guard: header mode skips comments, strings, preprocessor lines and fences", "[guard]")
{
    const auto hits = Scan("sample", kScannerSample, ScanMode::PublicDeclarations);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].line == 10);
}

TEST_CASE("guard: comments-only mode flags includes, #define continuations, strings and fenced lines", "[guard]")
{
    const auto hits = Scan("sample", kScannerSample, ScanMode::CommentsOnly);
    std::vector<int> lines;
    for (const Hit& h : hits) lines.push_back(h.line);
    // 2 = string, 3 = #include <Manifold2D/...>, 5 = #define continuation,
    // 7 = inside an ARC_INTERNAL fence, 10 = plain code. Comments (1, 6, 8) stay exempt.
    CHECK(lines == std::vector<int>{ 2, 3, 5, 7, 10 });

    // A "//" inside a string is not a comment start: the token after it still counts.
    const auto url = Scan("url", "const char* u = \"a//b\"; Mosaic::Thing t;\n", ScanMode::CommentsOnly);
    REQUIRE(url.size() == 1);
    CHECK(url[0].line == 1);
}

TEST_CASE("guard: ReferenceProject's game sources spell Arcane:: only (outside comments)", "[guard]")
{
    const auto root = Arcane::Test::FindReferenceProjectDir();
    REQUIRE_FALSE(root.empty());
    std::vector<Hit> hits;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "Source"))
    {
        const auto ext = entry.path().extension();
        if (ext != ".hpp" && ext != ".cpp") continue;
        auto h = Scan(entry.path().generic_string(), Slurp(entry.path()), ScanMode::CommentsOnly);
        hits.insert(hits.end(), h.begin(), h.end());
    }
    Report(hits);
}

TEST_CASE("guard: every editor C++ template render spells Arcane:: only (outside comments)", "[guard]")
{
    using namespace Arcane::Editor;
    std::vector<Hit> hits;
    auto scan = [&](const char* what, const ClassTemplates::Rendered& r)
    {
        auto a = Scan(std::string(what) + " header", r.header, ScanMode::CommentsOnly);
        auto b = Scan(std::string(what) + " source", r.source, ScanMode::CommentsOnly);
        hits.insert(hits.end(), a.begin(), a.end());
        hits.insert(hits.end(), b.begin(), b.end());
    };
    scan("component", ClassTemplates::Render(ClassTemplates::Kind::Component, "C", "P"));
    scan("plain", ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "C", "P"));
    for (int phase = 0; phase < ClassTemplates::kSystemPhaseChoiceCount; ++phase)
        for (int role = 0; role < ClassTemplates::kSystemRoleChoiceCount; ++role)
            scan("system", ClassTemplates::Render(ClassTemplates::Kind::System, "S", "P",
                                                  ClassTemplates::SystemOptionsForChoiceIndices(phase, role)));
    Report(hits);
}

TEST_CASE("guard: the 21 game-facing engine headers spell Arcane:: outside comments, preprocessor lines and fences", "[guard]")
{
    const auto root = Arcane::Test::FindReferenceProjectDir().parent_path();
    std::vector<Hit> hits;
    for (const char* rel : kGameFacingHeaders)
    {
        const auto path = root / rel;
        INFO(path.generic_string());
        REQUIRE(std::filesystem::exists(path));
        auto h = Scan(rel, Slurp(path), ScanMode::PublicDeclarations);
        hits.insert(hits.end(), h.begin(), h.end());
    }
    Report(hits);
}

// Namespace-facades spec s6 / SA6. Comments stripped, string literals kept,
// whole token. Phys:: must not match Physics::. Detail:: is engine-internal.
TEST_CASE("facade: game sources and class templates spell only Arcane names", "[facade]")
{
    constexpr const char* kTokens = R"((\bAstra::|\bManifold2D::|\bPhys::|\bMosaic::|\bDetail::))";
    const auto root = Arcane::Test::FindReferenceProjectDir().parent_path();
    std::vector<Hit> hits;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             Arcane::Test::FindReferenceProjectDir() / "Source"))
    {
        const auto ext = entry.path().extension();
        if (ext != ".hpp" && ext != ".cpp") continue;
        auto found = Scan(entry.path().generic_string(), Slurp(entry.path()),
                          ScanMode::CommentsOnly, kTokens);
        hits.insert(hits.end(), found.begin(), found.end());
    }
    const auto templates = root / "Source" / "Programs" / "ArcaneEditor" / "src" / "Project" / "ClassTemplates.cpp";
    REQUIRE(std::filesystem::exists(templates));
    auto found = Scan(templates.generic_string(), Slurp(templates), ScanMode::CommentsOnly, kTokens);
    hits.insert(hits.end(), found.begin(), found.end());
    Report(hits);
}
