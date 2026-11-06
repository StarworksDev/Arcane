// 3D-readiness Plan B, Task 3: the one .arcart codec in ArcaneAssets. These gates read
// Source/ text (comments and strings stripped first, the same rule as the spelling guard's
// public-declaration scan) so a comment that mentions a name cannot satisfy or break them.
#include "Helpers/ArtifactV1Fixture.hpp"

#include <Arcane/Assets/ArtifactCodec.hpp>
#include <Arcane/Crypto/Crypto.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::Test;

namespace
{
    std::string Slurp(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // Blanks // and /* */ comments and "..." / '...' literals (a ' between a digit and a
    // hex digit is a digit separator and is kept), keeping newlines so offsets stay sane.
    std::string StripCommentsAndStrings(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
            {
                while (i < s.size() && s[i] != '\n') ++i;
                out.push_back('\n');
                continue;
            }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
                ++i;
                out.push_back(' ');
                continue;
            }
            const bool digitSep = c == '\'' && i > 0 && i + 1 < s.size()
                && std::isdigit(static_cast<unsigned char>(s[i - 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 1]));
            if ((c == '"' || c == '\'') && !digitSep)
            {
                for (++i; i < s.size() && s[i] != c; ++i)
                    if (s[i] == '\\') ++i;
                out.push_back(' ');
                continue;
            }
            out.push_back(c);
        }
        return out;
    }

    struct SourceFile
    {
        std::string name;      // filename only
        std::string rel;       // repo-relative, '/' separators
        std::string stripped;
    };

    std::vector<SourceFile> WalkSource()
    {
        std::vector<SourceFile> files;
        const fs::path root = RepoRoot();
        for (const auto& e : fs::recursive_directory_iterator(root / "Source"))
        {
            if (!e.is_regular_file()) continue;
            const std::string ext = e.path().extension().string();
            if (ext != ".hpp" && ext != ".cpp" && ext != ".h" && ext != ".inl") continue;
            files.push_back({ e.path().filename().string(),
                              fs::relative(e.path(), root).generic_string(),
                              StripCommentsAndStrings(Slurp(e.path())) });
        }
        return files;
    }

    struct HashDeclarators
    {
        int prototypes = 0;
        int definitions = 0;
        std::string prototypeFile, definitionFile;
    };

    // A DECLARATOR is `HashSourceBytes(` whose preceding token is a type (an identifier
    // character, '*' or '&') that is not `return`; a call follows '=', '(', ',', '.', '::'
    // after a non-type, ';', '{', and so on. A declarator with a body ('{' after the
    // parameter list and any specifiers) is a definition; one ending in ';' is a prototype.
    // A definition is NOT counted as a prototype.
    void ClassifyHashDeclarators(const SourceFile& f, HashDeclarators& out)
    {
        const std::string& t = f.stripped;
        const std::string name = "HashSourceBytes";
        for (std::size_t pos = t.find(name); pos != std::string::npos; pos = t.find(name, pos + name.size()))
        {
            if (pos > 0 && (std::isalnum(static_cast<unsigned char>(t[pos - 1])) || t[pos - 1] == '_')) continue;
            std::size_t after = pos + name.size();
            while (after < t.size() && std::isspace(static_cast<unsigned char>(t[after]))) ++after;
            if (after >= t.size() || t[after] != '(') continue;

            // Skip an optional `Qualifier::` before the name, then look at the previous token.
            std::size_t before = pos;
            while (before >= 2 && t[before - 1] == ':' && t[before - 2] == ':')
            {
                before -= 2;
                while (before > 0 && (std::isalnum(static_cast<unsigned char>(t[before - 1])) || t[before - 1] == '_')) --before;
            }
            while (before > 0 && std::isspace(static_cast<unsigned char>(t[before - 1]))) --before;
            if (before == 0) continue;
            const char prev = t[before - 1];
            const bool typeLike = std::isalnum(static_cast<unsigned char>(prev)) || prev == '_' || prev == '*' || prev == '&';
            if (!typeLike) continue;
            std::size_t wordStart = before;
            while (wordStart > 0 && (std::isalnum(static_cast<unsigned char>(t[wordStart - 1])) || t[wordStart - 1] == '_')) --wordStart;
            if (t.compare(wordStart, before - wordStart, "return") == 0) continue;

            // Match the parameter list.
            int depth = 0;
            std::size_t i = after;
            for (; i < t.size(); ++i)
            {
                if (t[i] == '(') ++depth;
                else if (t[i] == ')' && --depth == 0) break;
            }
            // Skip specifiers up to '{' or ';'.
            for (++i; i < t.size() && t[i] != '{' && t[i] != ';'; ++i) {}
            if (i >= t.size()) continue;
            if (t[i] == '{') { ++out.definitions; out.definitionFile = f.rel; }
            else { ++out.prototypes; out.prototypeFile = f.rel; }
        }
    }

    int CountMatches(const std::string& text, const std::regex& re)
    {
        return static_cast<int>(std::distance(std::sregex_iterator(text.begin(), text.end(), re),
                                              std::sregex_iterator()));
    }
}

TEST_CASE("one kArtifactVersion and one SectionTag", "[artifact][codec]")
{
    const std::regex version(R"(\bkArtifactVersion\s*(=(?!=)|\{))");
    const std::regex tagEnum(R"(enum\s+class\s+SectionTag\b)");
    const std::regex tangentsUse(R"(SectionTag\s*::\s*Tangents\b)");

    int versionDecls = 0, tagEnums = 0;
    std::string versionFile, tagFile;
    for (const SourceFile& f : WalkSource())
    {
        if (const int n = CountMatches(f.stripped, version)) { versionDecls += n; versionFile = f.rel; }
        if (const int n = CountMatches(f.stripped, tagEnum)) { tagEnums += n; tagFile = f.rel; }

        // Tag 7 is reserved and never written: no writer TU may name it.
        if (f.name == "ArtifactCodec.cpp" || f.name == "ArtifactFormat.cpp" || f.name == "MeshImporter.cpp")
            CHECK_FALSE(std::regex_search(f.stripped, tangentsUse));
    }

    CHECK(versionDecls == 1);
    CHECK(versionFile.ends_with("/ArtifactCodec.hpp"));
    CHECK(tagEnums == 1);
    CHECK(tagFile.ends_with("/ArtifactCodec.hpp"));
}

TEST_CASE("checked-in v1 bytes match a fresh codec write", "[artifact][codec]")
{
    const auto root = RepoRoot() / "Tests/ArcaneTests/data/artifacts";
    for (const char* name : { "v1-texture.arcart", "v1-mesh.arcart" })
    {
        const auto path = root / name;
        REQUIRE(fs::exists(path));
        const std::string disk = ReadFileBinary(path);
        std::string expect = ReadFileBinary(root / (std::string(name) + ".sha256"));
        if (!expect.empty() && expect.back() == '\n') expect.pop_back();
        CHECK(Arcane::Crypto::Sha256Hex(disk) == expect);
        const auto fresh = (std::string(name) == "v1-texture.arcart")
            ? V1Fixture::WriteV1TextureToTemp() : V1Fixture::WriteV1MeshToTemp();
        CHECK(fresh == disk);
    }
}

TEST_CASE("source hash is one function and matches the texture fixture", "[artifact][codec]")
{
    HashDeclarators d;
    for (const SourceFile& f : WalkSource())
        ClassifyHashDeclarators(f, d);
    // One prototype (ArtifactCodec.hpp) and one definition (ArtifactCodec.cpp). A definition
    // is also a declaration, so a single-declarator count could never pass on this layout.
    CHECK(d.prototypes == 1);
    CHECK(d.prototypeFile.ends_with("/ArtifactCodec.hpp"));
    CHECK(d.definitions == 1);
    CHECK(d.definitionFile.ends_with("/ArtifactCodec.cpp"));

    const auto png = V1Fixture::EncodePng(17, 9, V1Fixture::GradientPixels(17, 9));
    const auto hashed = Arcane::HashSourceBytes(png);
    const auto art = V1Fixture::ReadV1TextureSourceHash(RepoRoot() / "Tests/ArcaneTests/data/artifacts/v1-texture.arcart");
    CHECK(hashed == art);
}
