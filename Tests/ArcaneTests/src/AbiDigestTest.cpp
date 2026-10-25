// The compiler-derived plugin ABI digest (3D-readiness Plan B, Task 1).
//
// build/abi-digest.txt holds AbiManifestMix() at the default Astra defines:
// FoldGroups() of the Engine, Input and Client group mixes (the manifests
// Plugin/AbiManifestEngine.hpp, Input/AbiManifestInput.hpp,
// Client/AbiManifestClient.hpp). kGamePluginABIDigest is the same value, and
// kGamePluginABIDigestInput / kGamePluginABIDigestClient the Input and Client
// mixes, compiled in. The tool
// arcane-abi-digest (Source/Programs/arcane-abi-digest) writes and checks the
// file. These cases pin the file, the constant, comment stability and the
// mutation witnesses (second translation units that compile a comment-appended
// copy of the manifest and shadow copies of listed headers with one mutation
// each), and the completeness scans that keep the manifest in step with the 21
// game-facing headers.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Core/AbiManifest.hpp>
#include <Arcane/Core/AbiVersion.hpp>
#include <Arcane/Plugin/Plugin.hpp>

#include "Helpers/ReferenceProjectDir.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define ABI_TEST_POPEN  _popen
#define ABI_TEST_PCLOSE _pclose
#else
#define ABI_TEST_POPEN  popen
#define ABI_TEST_PCLOSE pclose
#endif

namespace
{
    namespace fs = std::filesystem;

    // The 21 headers a game module reads: the SAME list as
    // ArcaneSpellingGuardTest.cpp's kGameFacingHeaders. Keep them equal.
    constexpr const char* kAbiHeaders[] = {
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

    // Headers outside the 21 that define by-value field types of the 21's structs.
    // Their struct names count as "structs" for the transitive-record case.
    constexpr const char* kAbiFieldTypeHeaders[] = {
        "Source/Runtime/ArcaneCore/src/Arcane/Guid.hpp",
        "Source/Runtime/ArcaneCore/src/Arcane/Math/Aabb.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/ViewTransform.hpp",
        "Source/Client/ArcaneInput/src/Arcane/Input/InputSnapshot.hpp",
        "Source/Client/ArcaneClient/src/Arcane/Client/RuntimePresentation.hpp",
        "Source/Runtime/ArcaneAssets/src/Arcane/Mesh/MeshAsset.hpp",
        "Source/Runtime/ArcaneAssets/src/Arcane/Mesh/MeshBuilder.hpp",
    };

    fs::path RepoRootDir() { return Arcane::Test::FindReferenceProjectDir().parent_path(); }

    std::string ReadFileBinary(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string Hex16(std::uint64_t v)
    {
        std::ostringstream hex;
        hex << std::hex << std::setfill('0') << std::setw(16) << v;
        return hex.str();
    }

    std::string TrimTrailingNewline(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }

    // ---- public-declaration stripping (ArcaneSpellingGuardTest ScanMode::PublicDeclarations)
    //
    // Comments, string literals (kept as ""), preprocessor lines (+ continuations)
    // and `ARC_INTERNAL_BEGIN` ... `ARC_INTERNAL_END` fences are removed. Line
    // structure is kept (one output line per input line). `commentsOnly` strips
    // comments and nothing else (used on the manifest, whose string literals are
    // the data).
    std::string StripPublic(const std::string& text, bool commentsOnly = false)
    {
        std::string out;
        std::istringstream in(text);
        std::string raw;
        bool inBlock = false, inFence = false, inDefine = false;
        while (std::getline(in, raw))
        {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            if (!commentsOnly)
            {
                if (raw.find("ARC_INTERNAL_BEGIN") != std::string::npos) { inFence = true;  out += '\n'; continue; }
                if (raw.find("ARC_INTERNAL_END")   != std::string::npos) { inFence = false; out += '\n'; continue; }
                if (inFence) { out += '\n'; continue; }
                if (inDefine) { inDefine = !raw.empty() && raw.back() == '\\'; out += '\n'; continue; }
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
                    code += commentsOnly ? raw.substr(i, end - i) : std::string("\"\"");
                    i = end - 1;
                    continue;
                }
                code += raw[i];
            }
            const auto first = code.find_first_not_of(" \t");
            if (!commentsOnly && first != std::string::npos && code[first] == '#')
            {
                inDefine = !raw.empty() && raw.back() == '\\';
                out += '\n';
                continue;
            }
            out += code;
            out += '\n';
        }
        return out;
    }

    std::string Trim(const std::string& s)
    {
        const auto a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        const auto b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // ---- type scan ------------------------------------------------------------------
    //
    // Finds every `struct` and `class` defined at NAMESPACE scope (not nested in a
    // class, a struct or a function) and records its non-static data members. A
    // struct's members are public until a `private:`/`protected:` label, a class's
    // private until `public:`. Functions, constructors, usings, friends, templates and
    // enums are not fields; brace initialisers and inline function bodies are skipped.
    // A type that declares `friend struct ...Detail::AbiManifest::Access;` (or
    // ClientAccess, the Client group's accessor) has handed
    // the manifest its private members too: ScannedType::Checked() then includes them.
    enum class Scope { Namespace, Struct, Other };

    struct ScannedField { std::string name; bool priv = false; };
    struct ScannedType
    {
        std::string name;
        bool friendAccess = false;
        std::vector<ScannedField> fields;

        std::vector<std::string> Checked() const
        {
            std::vector<std::string> out;
            for (const ScannedField& f : fields)
                if (!f.priv || friendAccess) out.push_back(f.name);
            return out;
        }
    };

    std::string StripAccess(std::string head, bool& priv)
    {
        static const std::regex label(R"(^\s*(public|private|protected)\s*:(?!:))");
        std::smatch m;
        while (std::regex_search(head, m, label))
        {
            priv = m[1].str() != "public";
            head = m.suffix().str();
        }
        return head;
    }

    // The declarator names of one member declaration (empty for non-fields).
    std::vector<std::string> FieldNames(const std::string& declIn)
    {
        std::string decl = Trim(declIn);
        if (decl.empty()) return {};
        static const std::regex skipLead(R"(^(static|using|typedef|friend|template|enum|struct\s+\w+\s*$|class\s+\w+\s*$|static_assert|namespace)\b)");
        if (std::regex_search(decl, skipLead)) return {};
        if (decl.find("operator") != std::string::npos) return {};
        static const std::regex fnPtr(R"(\(\s*\*\s*(\w+)\s*\))");
        std::smatch pm;
        if (decl.find('(') != std::string::npos)
        {
            // A function-pointer field is `ret (*name)(args)`; any other `(` is a function.
            if (std::regex_search(decl, pm, fnPtr)) return { pm[1].str() };
            return {};
        }
        decl = std::regex_replace(decl, std::regex(R"(\[\[[^\]]*\]\])"), " ");
        std::vector<std::string> parts;
        std::string cur;
        int depth = 0;
        for (char c : decl)
        {
            if (c == '<' || c == '[' || c == '(') ++depth;
            else if (c == '>' || c == ']' || c == ')') --depth;
            if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        parts.push_back(cur);
        std::vector<std::string> names;
        for (std::string part : parts)
        {
            const auto eq = part.find('=');
            if (eq != std::string::npos) part = part.substr(0, eq);
            part = std::regex_replace(part, std::regex(R"(:\s*\d+\s*$)"), "");    // bit-field width
            part = std::regex_replace(part, std::regex(R"(\[[^\]]*\]\s*$)"), ""); // array extent
            static const std::regex last(R"((\w+)\s*$)");
            std::smatch lm;
            if (std::regex_search(part, lm, last)) names.push_back(lm[1].str());
        }
        return names;
    }

    std::vector<ScannedType> ScanTypes(const std::string& source)
    {
        std::string text = StripPublic(source);
        text = std::regex_replace(text, std::regex(R"(\bARC_CHANGE_TRACKED\b)"), " ");
        text = std::regex_replace(text, std::regex(R"(\bARC_CONSTANT\s*\(\s*""\s*\))"), " ");

        std::vector<ScannedType> types;
        struct Frame { Scope kind; int type; bool isClass; };
        std::vector<Frame> stack;
        bool priv = false;
        std::string head;
        int skipDepth = 0;   // inside a skipped brace group (function body / initialiser)
        const auto top = [&]() { return stack.empty() ? Scope::Namespace : stack.back().kind; };
        static const std::regex friendRe(R"(\bfriend\s+struct\s+(?:::)?(?:Arcane::)?Detail::AbiManifest::(?:Client|Physics2D)?Access\b)");

        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (skipDepth > 0)
            {
                if (c == '{') ++skipDepth;
                else if (c == '}') --skipDepth;
                continue;
            }
            if (c == '{')
            {
                bool p = priv;
                const std::string h = Trim(StripAccess(head, p));
                static const std::regex nsRe(R"(^(inline\s+)?namespace\b|^extern\s*""$)");
                static const std::regex typeRe(R"(^(struct|class)\s+(?:(?:ARC_\w+|alignas\s*\([^)]*\))\s+)*(\w+)\s*(?:final\s*)?(?::[^{]*)?$)");
                std::smatch m;
                if (top() == Scope::Namespace && std::regex_search(h, nsRe))
                {
                    stack.push_back({ Scope::Namespace, -1, false });
                    head.clear();
                }
                else if (top() == Scope::Namespace && h.find("template") == std::string::npos &&
                         std::regex_search(h, m, typeRe))
                {
                    types.push_back({ m[2].str(), false, {} });
                    stack.push_back({ Scope::Struct, static_cast<int>(types.size()) - 1, m[1].str() == "class" });
                    priv = stack.back().isClass;
                    head.clear();
                }
                else if (top() == Scope::Struct)
                {
                    // A function body (the head ends in `)`/qualifiers) is discarded with
                    // its declaration; any other brace group is an initialiser, which
                    // belongs to the declaration and is simply skipped.
                    static const std::regex fnTail(R"(\)\s*(?:const|noexcept|override|final|volatile|&|&&|\s)*(?:->[^;{]*)?$)");
                    priv = p;
                    if (std::regex_search(h, fnTail)) head.clear();
                    else head = h + " ";
                    skipDepth = 1;
                }
                else
                {
                    stack.push_back({ Scope::Other, -1, false });
                    head.clear();
                }
                continue;
            }
            if (c == '}')
            {
                if (!stack.empty()) stack.pop_back();
                head.clear();
                priv = false;
                continue;
            }
            if (c == ';')
            {
                if (top() == Scope::Struct)
                {
                    bool p = priv;
                    const std::string h = StripAccess(head, p);
                    priv = p;
                    ScannedType& t = types[static_cast<std::size_t>(stack.back().type)];
                    if (std::regex_search(h, friendRe)) t.friendAccess = true;
                    for (const std::string& n : FieldNames(h)) t.fields.push_back({ n, priv });
                }
                head.clear();
                continue;
            }
            head += c;
        }
        return types;
    }

    std::vector<std::pair<std::string, std::string>> CheckedFields(const std::string& source)
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (const ScannedType& t : ScanTypes(source))
            for (const std::string& f : t.Checked()) out.push_back({ t.name, f });
        return out;
    }

    std::vector<std::string> TypeNames(const std::string& source)
    {
        std::vector<std::string> names;
        for (const ScannedType& t : ScanTypes(source)) names.push_back(t.name);
        return names;
    }

    // Struct types a declaration passes or returns BY VALUE (not a pointer, a
    // reference, or a template argument such as std::span<const X>).
    std::set<std::string> ByValueUses(const std::string& source, const std::set<std::string>& structs)
    {
        const std::string text = StripPublic(source);
        std::set<std::string> used;
        for (const std::string& name : structs)
        {
            const std::regex param("[(,]\\s*(?:const\\s+)?(?:\\w+::)*" + name + "\\s+\\w+\\s*(?=[,)=])");
            const std::regex ret("(?:^|[;{}:>]|\\bconstexpr|\\binline|\\bstatic|\\bvirtual|\\]\\]|\\bARC_\\w+)\\s*(?:const\\s+)?(?:\\w+::)*" +
                                 name + "\\s+\\w+\\s*\\(");
            if (std::regex_search(text, param) || std::regex_search(text, ret))
                used.insert(name);
        }
        return used;
    }

    // `manifest` is the manifest with comments removed (StripPublic(.., true)), so a
    // commented-out entry does not count.
    bool ManifestHasOffsetof(const std::string& manifest, const std::string& type, const std::string& field)
    {
        const std::regex re("offsetof\\(\\s*(?:\\w+::)*" + type + "\\s*,\\s*" + field + "\\s*\\)");
        return std::regex_search(manifest, re);
    }

    bool ManifestHasRoot(const std::string& manifest, const std::string& name)
    {
        const std::regex re("f\\.Type\\(\\s*\"(?:\\w+::)*" + name + "\"");
        return std::regex_search(manifest, re);
    }

    // The digest's four group manifests (Core/AbiManifest.hpp folds them). The
    // completeness scans read all four as one text.
    constexpr const char* kManifestFiles[] = {
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/AbiManifestEngine.hpp",
        "Source/Client/ArcaneInput/src/Arcane/Input/AbiManifestInput.hpp",
        "Source/Client/ArcaneClient/src/Arcane/Client/AbiManifestClient.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Physics2D/AbiManifestPhysics2D.hpp",
    };

    // The Engine group: the one the comment-stability witness copies.
    fs::path EngineManifestPath() { return RepoRootDir() / kManifestFiles[0]; }

    std::string ManifestCode()
    {
        std::string code;
        for (const char* file : kManifestFiles)
        {
            const std::string text = ReadFileBinary(RepoRootDir() / file);
            REQUIRE_FALSE(text.empty());
            code += StripPublic(text, true);
            code += '\n';
        }
        return code;
    }

    // Each group file's mix function, from its definition to the end of that file
    // (calls to the R_ accessors count only there).
    std::string MixBodies()
    {
        static const std::regex mixRe(R"(constexpr\s+std::uint64_t\s+AbiManifest\w+Mix\(\)\s*noexcept\s*\{)");
        std::string bodies;
        for (const char* file : kManifestFiles)
        {
            const std::string code = StripPublic(ReadFileBinary(RepoRootDir() / file), true);
            std::smatch m;
            REQUIRE(std::regex_search(code, m, mixRe));
            bodies += code.substr(static_cast<std::size_t>(m.position(0)));
            bodies += '\n';
        }
        return bodies;
    }

    // Runs `exe args` and returns its stdout. cmd.exe /c wants the whole line wrapped in
    // one more pair of quotes; /bin/sh wants the executable path quoted on its own.
    std::string RunCaptured(const fs::path& exe, const std::string& args)
    {
#if defined(_WIN32)
        const std::string line = "\"\"" + exe.string() + "\" " + args + "\"";
#else
        const std::string line = "'" + exe.string() + "' " + args;
#endif
        FILE* pipe = ABI_TEST_POPEN(line.c_str(), "r");
        REQUIRE(pipe != nullptr);
        std::string out;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
        const int status = ABI_TEST_PCLOSE(pipe);
        CHECK(status == 0);
        return out;
    }

    // Runs `exe args` with stderr folded into stdout and returns the exit status
    // (0 is success; any other value is a failure, on every platform) and output.
    struct ToolRun
    {
        int         status = -1;
        std::string output;
    };
    ToolRun RunWithStatus(const fs::path& exe, const std::string& args)
    {
#if defined(_WIN32)
        const std::string line = "\"\"" + exe.string() + "\" " + args + " 2>&1\"";
#else
        const std::string line = "'" + exe.string() + "' " + args + " 2>&1";
#endif
        ToolRun run;
        FILE* pipe = ABI_TEST_POPEN(line.c_str(), "r");
        REQUIRE(pipe != nullptr);
        char buf[256];
        while (std::fgets(buf, sizeof(buf), pipe)) run.output += buf;
        run.status = ABI_TEST_PCLOSE(pipe);
        return run;
    }

    fs::path SiblingTool(const char* dir, const char* exe)
    {
        const fs::path exeDir = fs::path(Arcane::ExecutablePathUtf8()).parent_path();
#if defined(_WIN32)
        return exeDir.parent_path() / dir / (std::string(exe) + ".exe");
#else
        return exeDir.parent_path() / dir / exe;
#endif
    }
}

TEST_CASE("abi digest matches build/abi-digest.txt", "[plugin][abi]")
{
    const auto path = RepoRootDir() / "build/abi-digest.txt";
    REQUIRE(fs::exists(path));
    std::string file = ReadFileBinary(path);
    file = TrimTrailingNewline(file);   // a CRLF checkout (autocrlf) ends in CRLF
    CHECK(file == Hex16(Arcane::AbiManifestMix()));
}

TEST_CASE("kGamePluginABIDigest equals the compiler-derived mix", "[plugin][abi]")
{
    CHECK(Arcane::kGamePluginABIDigest == Arcane::AbiManifestMix());
    // The committed sub-digests a game folds for a group it cannot see.
    CHECK(Arcane::kGamePluginABIDigestInput == Arcane::AbiManifestInputMix());
    CHECK(Arcane::kGamePluginABIDigestClient == Arcane::AbiManifestClientMix());
    CHECK(Arcane::kGamePluginABIDigestPhysics2D == Arcane::AbiManifestPhysics2DMix());
}

TEST_CASE("a game TU's digest equals the host's with or without the Input and Client headers", "[plugin][abi]")
{
    // ArcaneTests sees every module, so its AbiGameMix() (Plugin/AbiManifestGame.hpp,
    // the expression ARC_GAME_MODULE exports) hashes all three groups itself.
    STATIC_REQUIRE(ARC_ABI_GAME_HASHES_INPUT == 1);
    STATIC_REQUIRE(ARC_ABI_GAME_HASHES_CLIENT == 1);
    STATIC_REQUIRE(ARC_ABI_GAME_HASHES_PHYSICS2D == 1);
    CHECK(Arcane::AbiGameMix() == Arcane::AbiManifestMix());
    CHECK(Arcane::AbiGameMix() == Arcane::kGamePluginABIDigest);
    // An ArcaneEngine-only game hashes the Engine group and folds the committed
    // sub-digests: the same value. (The [hotreload] fixtures are such games and
    // load through the digest gate; AbiDigestBits64 static_asserts it is one.)
    using Arcane::Detail::AbiManifest::FoldGroups;
    CHECK(FoldGroups(Arcane::AbiManifestEngineMix(), Arcane::kGamePluginABIDigestInput,
                     Arcane::kGamePluginABIDigestClient, Arcane::kGamePluginABIDigestPhysics2D) == Arcane::kGamePluginABIDigest);
    // Each group moves the fold, and so does the group order.
    const std::uint64_t e = Arcane::AbiManifestEngineMix();
    const std::uint64_t i = Arcane::AbiManifestInputMix();
    const std::uint64_t c = Arcane::AbiManifestClientMix();
    const std::uint64_t p = Arcane::AbiManifestPhysics2DMix();
    CHECK(FoldGroups(e ^ 1, i, c, p) != FoldGroups(e, i, c, p));
    CHECK(FoldGroups(e, i ^ 1, c, p) != FoldGroups(e, i, c, p));
    CHECK(FoldGroups(e, i, c ^ 1, p) != FoldGroups(e, i, c, p));
    CHECK(FoldGroups(e, i, c, p ^ 1) != FoldGroups(e, i, c, p));
    CHECK(FoldGroups(e, c, i, p) != FoldGroups(e, i, c, p));
}

TEST_CASE("a comment-only change does not move the abi digest", "[plugin][abi]")
{
    // The tool compiles TWO translation units: one includes the manifest, the
    // other first includes a copy of the Engine group manifest
    // (Plugin/AbiManifestEngine.hpp, the bulk of the records) with a comment line
    // appended (generated by the tool's prebuild step into Intermediate/AbiDigest).
    // Each bakes its mix at compile time. A source-text hash of the copy differs;
    // the mix must not.
    const fs::path original = EngineManifestPath();
    const fs::path commented = RepoRootDir() / "Intermediate/AbiDigest/AbiManifestEngine.commented.hpp";
    REQUIRE(fs::exists(original));
    REQUIRE(fs::exists(commented));
    const std::string a = ReadFileBinary(original);
    const std::string b = ReadFileBinary(commented);
    CHECK(a != b);
    CHECK(b.size() > a.size());

    const fs::path tool = SiblingTool("arcane-abi-digest", "arcane-abi-digest");
    REQUIRE(fs::exists(tool));
    const std::string plain = TrimTrailingNewline(RunCaptured(tool, "--print"));
    const std::string other = TrimTrailingNewline(RunCaptured(tool, "--print-commented"));
    CHECK(plain == Hex16(Arcane::AbiManifestMix()));
    CHECK(other == plain);
}

TEST_CASE("a mutated listed header moves the abi digest, a comment in it does not", "[plugin][abi]")
{
    // arcane-abi-digest-mutants compiles shadow copies of ProcessContext.hpp,
    // Aabb.hpp and ViewTransform.hpp (premake writes them at generate time; see the
    // comment above that project). `base` carries only a comment line; each other
    // variant adds ONE mutation: a private int before ProcessContext's first
    // container, or a swap of two same-size fields.
    const fs::path tool = SiblingTool("arcane-abi-digest-mutants", "arcane-abi-digest-mutants");
    REQUIRE(fs::exists(tool));
    std::map<std::string, std::string> mix;
    std::istringstream lines(RunCaptured(tool, ""));
    for (std::string line; std::getline(lines, line);)
    {
        line = TrimTrailingNewline(line);
        const auto sp = line.find(' ');
        if (sp != std::string::npos) mix[line.substr(0, sp)] = line.substr(sp + 1);
    }
    REQUIRE(mix.size() == 6);
    // The shadow copies are the real headers plus a comment: the same mix.
    INFO("re-run premake: the shadow headers are stale (they are written at generate time; "
         "a listed header changed since)");
    CHECK(mix["base"] == Hex16(Arcane::AbiManifestMix()));
    CHECK(mix["process-private"] != mix["base"]);
    CHECK(mix["aabb-swap"] != mix["base"]);
    CHECK(mix["view-swap"] != mix["base"]);
    // ClientRuntime: a pointer before m_pres moves m_pres's offset; an int before
    // RuntimePresentation::gameInput moves gameInput's (inline GameInput() reads it).
    CHECK(mix["client-private"] != mix["base"]);
    CHECK(mix["pres-gameinput"] != mix["base"]);
}

TEST_CASE("every public struct field is in the abi manifest", "[plugin][abi]")
{
    // Self-checks for the scanner.
    {
        const auto probe = CheckedFields("struct Probe { int alpha; };");
        REQUIRE(probe.size() == 1);
        CHECK(probe[0].first == "Probe");
        CHECK(probe[0].second == "alpha");
        CHECK(CheckedFields("// int alpha;").empty());
        const auto two = CheckedFields("struct P { int a, b; static constexpr int k = 1; void F() { int local; } int c{ 3 }; private: int hidden; };");
        REQUIRE(two.size() == 3);
        CHECK(two[0].second == "a");
        CHECK(two[1].second == "b");
        CHECK(two[2].second == "c");
        // A class is private until public:, and its private members count only when it befriends Access (or ClientAccess).
        CHECK(CheckedFields("class K { int hidden; public: int shown; };").size() == 1);
        const auto fr = CheckedFields("class K { friend struct ::Arcane::Detail::AbiManifest::Access; int hidden; public: int shown; };");
        REQUIRE(fr.size() == 2);
        CHECK(fr[0].second == "hidden");
        CHECK(fr[1].second == "shown");
        // A commented-out manifest entry is not an entry.
        CHECK_FALSE(ManifestHasOffsetof(StripPublic("// f.Field(\"a\", offsetof(Probe, alpha), 1, 1, \"int\");", true), "Probe", "alpha"));
        CHECK(ManifestHasOffsetof(StripPublic("f.Field(\"a\", offsetof(Probe, alpha), 1, 1, \"int\"); // c", true), "Probe", "alpha"));
    }

    const std::string manifest = ManifestCode();
    REQUIRE_FALSE(manifest.empty());
    const fs::path root = RepoRootDir();
    std::size_t scanned = 0;
    for (const char* header : kAbiHeaders)
    {
        INFO(header);
        const std::string source = ReadFileBinary(root / header);
        REQUIRE_FALSE(source.empty());
        for (const auto& [type, field] : CheckedFields(source))
        {
            ++scanned;
            INFO(type << "::" << field);
            CHECK(ManifestHasOffsetof(manifest, type, field));
        }
    }
    CHECK(scanned > 100);   // the walk must actually have seen the fields

    // The transitive records (kAbiFieldTypeHeaders) too, for every type the manifest
    // roots: a field added to Aabb, ViewTransform, MeshSection ... without an entry
    // does not move the digest otherwise (spec 3.4). A type with no root is the
    // by-value-record case's business, not this one's.
    std::size_t transitive = 0;
    for (const char* header : kAbiFieldTypeHeaders)
    {
        INFO(header);
        for (const ScannedType& t : ScanTypes(ReadFileBinary(root / header)))
        {
            if (!ManifestHasRoot(manifest, t.name)) continue;
            for (const std::string& field : t.Checked())
            {
                ++transitive;
                INFO(t.name << "::" << field);
                CHECK(ManifestHasOffsetof(manifest, t.name, field));
            }
        }
    }
    CHECK(transitive > 15);
}

TEST_CASE("a private data member is hashed or allow-listed", "[plugin][abi]")
{
    // Game code compiles inline accessors that read private members, so a class with
    // private data must befriend the manifest's Access (and be recorded), unless it
    // is listed here with the reason its private state never reaches an inline
    // game-side reader. Empty on purpose.
    const std::set<std::string> allowList{};
    CHECK(ScanTypes("class K { int hidden; };").front().friendAccess == false);
    const fs::path root = RepoRootDir();
    std::size_t types = 0;
    const auto walk = [&](const char* header)
    {
        INFO(header);
        for (const ScannedType& t : ScanTypes(ReadFileBinary(root / header)))
        {
            ++types;
            bool hasPrivate = false;
            for (const ScannedField& f : t.fields) hasPrivate = hasPrivate || f.priv;
            if (!hasPrivate || t.friendAccess || allowList.count(t.name) != 0) continue;
            INFO(t.name << " has private data members and no `friend struct ...Detail::AbiManifest::Access;` (ClientAccess in the Client group)");
            CHECK(false);
        }
    };
    for (const char* header : kAbiHeaders) walk(header);
    for (const char* header : kAbiFieldTypeHeaders) walk(header);
    CHECK(types > 60);
}

TEST_CASE("every manifest accessor record is called", "[plugin][abi]")
{
    // Each befriended record's text lives in a `static constexpr void R_<X>(Fnv&)` body
    // of Access (ClientAccess in the Client group), and its group's mix function must
    // call `Access::R_<X>(f);` exactly once. Delete a call and the text scans still see
    // the entries while --write re-blesses a digest that has silently lost the record.
    const std::string manifest = ManifestCode();
    REQUIRE_FALSE(manifest.empty());
    // Calls count only inside the mix functions (a call from another R_ body is dead code).
    const std::string mixBody = MixBodies();
    static const std::regex defRe(R"(static\s+constexpr\s+void\s+(R_\w+)\s*\()");
    std::size_t defs = 0;
    for (std::sregex_iterator it(manifest.begin(), manifest.end(), defRe), end; it != end; ++it)
    {
        const std::string name = (*it)[1].str();
        ++defs;
        INFO(name);
        const std::regex callRe("Access::" + name + R"(\(\s*f\s*\)\s*;)");
        const auto calls = std::distance(std::sregex_iterator(mixBody.begin(), mixBody.end(), callRe), std::sregex_iterator());
        CHECK(calls == 1);
    }
    CHECK(defs >= 7);
}

TEST_CASE("every abi header struct and class is a manifest root", "[plugin][abi]")
{
    // A root with no public fields (Hidden, EnsureScope, ProcessContext, ...) is
    // invisible to the field scan; deleting its f.Type must still fail.
    const std::string manifest = ManifestCode();
    REQUIRE_FALSE(manifest.empty());
    const fs::path root = RepoRootDir();
    std::size_t seen = 0;
    for (const char* header : kAbiHeaders)
    {
        INFO(header);
        for (const std::string& name : TypeNames(ReadFileBinary(root / header)))
        {
            ++seen;
            INFO(name);
            CHECK(ManifestHasRoot(manifest, name));
        }
    }
    CHECK(seen > 40);
    CHECK_FALSE(ManifestHasRoot(StripPublic("// f.Type(\"Arcane::Probe\", 1, 1);", true), "Probe"));
}

TEST_CASE("every struct-typed manifest field has its own record", "[plugin][abi]")
{
    // Spec 3.4: field types reached by value are recorded transitively. A declared type
    // that names a struct or class defined in the headers (not std::, glm::, a pointer or
    // a template) must have an f.Type, so a same-size reorder inside it moves the digest.
    const fs::path root = RepoRootDir();
    std::set<std::string> known;
    for (const char* header : kAbiHeaders)
        for (const std::string& n : TypeNames(ReadFileBinary(root / header))) known.insert(n);
    for (const char* header : kAbiFieldTypeHeaders)
        for (const std::string& n : TypeNames(ReadFileBinary(root / header))) known.insert(n);
    REQUIRE(known.count("Aabb") == 1);
    REQUIRE(known.count("ViewTransform") == 1);
    REQUIRE(known.count("MeshData") == 1);

    const std::string manifest = ManifestCode();
    static const std::regex fieldRe(R"re(f\.Field\(\s*"[^"]*"\s*,[^;]*,\s*"([^"]*)"\s*\);)re");
    static const std::regex bareType(R"(^(?:const\s+)?(?:\w+::)*(\w+)$)");
    std::size_t checked = 0;
    for (std::sregex_iterator it(manifest.begin(), manifest.end(), fieldRe), end; it != end; ++it)
    {
        std::smatch bm;
        const std::string declared = (*it)[1].str();
        if (!std::regex_match(declared, bm, bareType)) continue;
        const std::string name = bm[1].str();
        if (known.count(name) == 0) continue;
        ++checked;
        INFO(declared);
        CHECK(ManifestHasRoot(manifest, name));
    }
    CHECK(checked > 10);
}

TEST_CASE("a by-value public parameter is in the abi root list", "[plugin][abi]")
{
    // Self-checks.
    {
        const std::set<std::string> probeSet{ "Probe" };
        CHECK(ByValueUses("Probe Foo(Probe value);", probeSet).count("Probe") == 1);
        CHECK(ByValueUses("Probe* Foo(const Probe& value);", probeSet).empty());
        CHECK(ByValueUses("void Foo(std::span<const Probe> view);", probeSet).empty());
    }

    const std::string manifest = ManifestCode();
    REQUIRE_FALSE(manifest.empty());
    const fs::path root = RepoRootDir();

    std::set<std::string> structs{ "Entity", "Guid" };   // by-value engine types the headers use
    std::vector<std::string> sources;
    for (const char* header : kAbiHeaders)
    {
        sources.push_back(ReadFileBinary(root / header));
        for (const std::string& n : TypeNames(sources.back())) structs.insert(n);
    }
    std::set<std::string> used;
    for (const std::string& source : sources)
        for (const std::string& n : ByValueUses(source, structs)) used.insert(n);

    CHECK_FALSE(used.empty());
    for (const std::string& name : used)
    {
        INFO(name);
        CHECK(ManifestHasRoot(manifest, name));
    }
}

// ---------------------------------------------------------------------------
// Task 2: the load gate. Capture-free function pointers only: a capturing
// lambda does not convert to uint64_t (*)() or to bool (*)(EngineContext*).
// ---------------------------------------------------------------------------
namespace
{
    std::uint32_t TestAbiEpoch() { return Arcane::kGamePluginABIVersion; }
    std::uint64_t TestAbiWrongDigest() { return Arcane::kGamePluginABIDigest ^ 0x1ull; }
    std::uint64_t TestAbiRightDigest() { return Arcane::kGamePluginABIDigest; }
    int g_initCalls = 0;
    bool TestAbiInit(Arcane::EngineContext*) { ++g_initCalls; return true; }

    struct WarnCapture
    {
        std::vector<std::string> lines;
    };
    void WarnSink(const Arcane::LogRecord& record, void* user)
    {
        static_cast<WarnCapture*>(user)->lines.push_back(record.text);
    }

    struct DigestChildRun
    {
        int         exitCode = -1;
        std::string kind;
        std::string output;                          // the child's whole stdout
        std::map<std::string, std::string> fields;   // its `key=value` lines
        fs::path    sentinel;
    };

    // Spawns AbiDigestChild on the named fixture DLL. A missing fixture is a
    // failure of the task, not a skip.
    DigestChildRun RunDigestChild(const char* fixture, const char* extraArg = nullptr)
    {
        DigestChildRun run;
        const fs::path exe = SiblingTool("AbiDigestChild", "AbiDigestChild");
        const fs::path dll = SiblingTool(fixture, fixture).replace_extension(
#if defined(_WIN32)
            ".dll"
#elif defined(__APPLE__)
            ".dylib"
#else
            ".so"
#endif
        );
        REQUIRE(fs::exists(exe));
        REQUIRE(fs::exists(dll));
        run.sentinel = fs::temp_directory_path() / (std::string(fixture) + ".init.sentinel");
        std::error_code ec;
        fs::remove(run.sentinel, ec);
#if defined(_WIN32)
        const std::string extra = extraArg ? std::string(" ") + extraArg : std::string();
        const std::string line = "\"\"" + exe.string() + "\" \"" + dll.string() + "\" \""
                               + run.sentinel.string() + "\"" + extra + "\"";
#else
        const std::string extra = extraArg ? std::string(" ") + extraArg : std::string();
        const std::string line = "'" + exe.string() + "' '" + dll.string() + "' '"
                               + run.sentinel.string() + "'" + extra;
#endif
        FILE* pipe = ABI_TEST_POPEN(line.c_str(), "r");
        REQUIRE(pipe != nullptr);
        std::string out;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
        run.exitCode = ABI_TEST_PCLOSE(pipe);
        run.output = out;
        std::istringstream lines(out);
        for (std::string line; std::getline(lines, line);)
        {
            line = TrimTrailingNewline(line);
            const auto eq = line.find('=');
            if (eq != std::string::npos) run.fields[line.substr(0, eq)] = line.substr(eq + 1);
        }
        if (const auto it = run.fields.find("kind"); it != run.fields.end()) run.kind = it->second;
        return run;
    }
}

TEST_CASE("a matching epoch with the wrong digest is refused before Init", "[plugin][abi]")
{
    g_initCalls = 0;
    Arcane::PluginVTable table{};
    table.ABIVersion = &TestAbiEpoch;
    table.ABIDigest = &TestAbiWrongDigest;
    table.Init = &TestAbiInit;
    Arcane::PluginResolveError err{};
    CHECK_FALSE(Arcane::AcceptPluginAbi(table, &err));
    CHECK(err.kind == Arcane::PluginResolveError::Kind::AbiMismatch);
    CHECK(g_initCalls == 0);

    table.ABIDigest = &TestAbiRightDigest;
    Arcane::PluginResolveError ok{};
    CHECK(Arcane::AcceptPluginAbi(table, &ok));
    CHECK(ok.kind == Arcane::PluginResolveError::Kind::None);

    table.ABIDigest = nullptr;   // a missing digest is refused too
    Arcane::PluginResolveError missing{};
    CHECK_FALSE(Arcane::AcceptPluginAbi(table, &missing));
    CHECK(missing.kind == Arcane::PluginResolveError::Kind::AbiMismatch);
    CHECK(g_initCalls == 0);
}

TEST_CASE("a static table with the wrong digest is refused before Init", "[plugin][abi]")
{
    g_initCalls = 0;
    Arcane::PluginVTable table{};
    table.ABIVersion = &TestAbiEpoch;
    table.ABIDigest = &TestAbiWrongDigest;
    table.Init = &TestAbiInit;
    Arcane::PluginResolveError err{};
    std::optional<Arcane::Plugin> adopted;
    CHECK_FALSE(Arcane::AdoptStaticGameModule(table, &err, &adopted, "abi-test"));
    CHECK_FALSE(adopted.has_value());
    CHECK(err.kind == Arcane::PluginResolveError::Kind::AbiMismatch);
    CHECK(g_initCalls == 0);
}

TEST_CASE("OpenProject warns and still opens when engineAbi is stale", "[plugin][abi]")
{
    const fs::path dir = fs::temp_directory_path() / "arc-abi-stale-project";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content");
    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":)"
        << (static_cast<int>(Arcane::kGamePluginABIVersion) - 1) << "}}";

    WarnCapture capture;
    const auto sub = Arcane::Log::Subscribe(&WarnSink, &capture);
    bool opened = false;
    {
        Arcane::Runtime rt(Arcane::Test::Process());
        opened = rt.OpenProject(dir);
    }
    Arcane::Log::Unsubscribe(sub);
    fs::remove_all(dir, ec);

    CHECK(opened);
    bool warned = false;
    for (const std::string& line : capture.lines)
        if (line.find("opening") != std::string::npos && line.find("engine ABI") != std::string::npos)
            warned = true;
    CHECK(warned);
}

TEST_CASE("a 64-bit digest DLL and a stale digest DLL are refused before Init", "[plugin][abi]")
{
    // Refused at the DIGEST compare, not at the epoch: the child prints the
    // PluginResolveError's numbers, and only the digest branch leaves the epochs
    // equal with the digests different (a fixture left over from an older epoch
    // also prints kind=AbiMismatch, but with pluginAbi != engineAbi).
    const std::string engineAbi = std::to_string(Arcane::kGamePluginABIVersion);
    const std::string engineDigest = Hex16(Arcane::kGamePluginABIDigest);

    const DigestChildRun bits64 = RunDigestChild("AbiDigestBits64");
    INFO("child output: " << bits64.output);
    CHECK(bits64.exitCode == 0);
    CHECK(bits64.kind == "AbiMismatch");
    REQUIRE(bits64.fields.count("pluginAbi") == 1);
    REQUIRE(bits64.fields.count("engineAbi") == 1);
    REQUIRE(bits64.fields.count("pluginDigest") == 1);
    REQUIRE(bits64.fields.count("engineDigest") == 1);
    CHECK(bits64.fields.at("pluginAbi") == engineAbi);
    CHECK(bits64.fields.at("engineAbi") == engineAbi);
    CHECK(bits64.fields.at("engineDigest") == engineDigest);
    CHECK(bits64.fields.at("pluginDigest") != engineDigest);
    CHECK(bits64.fields.at("pluginDigest") != Hex16(0));   // it exported a digest of its own
    CHECK_FALSE(fs::exists(bits64.sentinel));

    const DigestChildRun stale = RunDigestChild("AbiDigestStale");
    INFO("child output: " << stale.output);
    CHECK(stale.exitCode == 0);
    CHECK(stale.kind == "AbiMismatch");
    REQUIRE(stale.fields.count("pluginAbi") == 1);
    REQUIRE(stale.fields.count("pluginDigest") == 1);
    REQUIRE(stale.fields.count("engineDigest") == 1);
    CHECK(stale.fields.at("pluginAbi") == engineAbi);
    CHECK(stale.fields.at("engineDigest") == engineDigest);
    CHECK(stale.fields.at("pluginDigest") == Hex16(1));
    CHECK_FALSE(fs::exists(stale.sentinel));
}

TEST_CASE("a refused load leaves nothing registered from a dependency image it unmapped", "[plugin][abi][hook]")
{
    // 3D-readiness Plan A Task 8 fix round 2 (I1). AbiDigestStale imports
    // ArcanePhysics2D; AbiDigestChild links ArcaneEngine only, so the refused
    // raw Plugin::Load maps Physics2D (its statics register a module-attach
    // callback, cvars and log setters, a sim hook and the query pair into
    // Core and Scene) and unmaps it again. The child then constructs a fresh
    // Runtime (AttachAllModules), steps it and walks the log setters: any
    // registration the dependency left behind calls into freed code.
    // Linux (user decision 2026-10-24): the refused load PINS the fresh engine
    // dependency for the life of the process instead (Module::PinsForProcessLife),
    // so Physics2D stays mapped with its registrations intact -- exactly one
    // module-attach callback -- and the same Runtime / log walk runs live code.
#if !defined(__APPLE__)   // MappedModules lists nothing on macOS
    const DigestChildRun stale = RunDigestChild("AbiDigestStale", "--then-runtime");
    INFO("child output: " << stale.output);
    CHECK(stale.kind == "AbiMismatch");
    REQUIRE(stale.fields.count("physics2dMappedAfterRefusal") == 1);
    REQUIRE(stale.fields.count("physics2dAttachAfterRefusal") == 1);
#if defined(_WIN32)
    REQUIRE(stale.fields.count("fixturePhysics2dMapped") == 1);
    CHECK(stale.fields.at("fixturePhysics2dMapped") == "1");        // the load really mapped it
    CHECK(stale.fields.at("physics2dMappedAfterRefusal") == "0");   // and the refusal unmapped it
#else
    CHECK(stale.fields.at("physics2dMappedAfterRefusal") == "1");   // mapped by the load (the child does not link it), pinned
    CHECK(stale.fields.at("physics2dAttachAfterRefusal") == "1");   // its registrations intact, not purged
#endif
    CHECK(stale.fields.count("runtimeAfterRefusal") == 1);
    CHECK(stale.exitCode == 0);
    CHECK_FALSE(fs::exists(stale.sentinel));
#else
    SKIP("the dependency witness uses Module::MappedModules, which lists nothing on macOS");
#endif
}

TEST_CASE("an older-epoch module fails at the epoch and a digest-less module at the export", "[plugin][abi]")
{
    // Plugin.cpp's resolve order: (2) the epoch is compared BEFORE the digest
    // export is resolved, so an old module without GamePlugin_ABIDigest still
    // reports the ABI error kind; (3) once the epoch matches, the export is
    // required, and its absence is the unresolved-symbol failure. Both fixtures
    // hand-write their exports and export no digest.
    const DigestChildRun old = RunDigestChild("AbiDigestOldEpoch");
    INFO("child output: " << old.output);
    CHECK(old.exitCode == 0);
    CHECK(old.kind == "AbiMismatch");
    REQUIRE(old.fields.count("pluginAbi") == 1);
    REQUIRE(old.fields.count("engineAbi") == 1);
    CHECK(old.fields.at("pluginAbi") == std::to_string(Arcane::kGamePluginABIVersion - 1));
    CHECK(old.fields.at("engineAbi") == std::to_string(Arcane::kGamePluginABIVersion));
    CHECK_FALSE(fs::exists(old.sentinel));

    const DigestChildRun none = RunDigestChild("AbiDigestNoExport");
    INFO("child output: " << none.output);
    CHECK(none.exitCode != 0);   // the child exits 0 only for AbiMismatch
    CHECK(none.kind == "MissingExport");
    REQUIRE(none.fields.count("symbol") == 1);
    CHECK(none.fields.at("symbol") == "GamePlugin_ABIDigest");
    CHECK_FALSE(fs::exists(none.sentinel));
}

TEST_CASE("a stale committed sub-digest fails arcane-abi-digest --check", "[plugin][abi]")
{
    // A game that cannot see the Input or Client headers folds the COMMITTED
    // sub-digest (kGamePluginABIDigestInput / kGamePluginABIDigestClient,
    // Core/AbiVersion.hpp) into its digest instead of hashing that group itself.
    // A stale committed sub-digest would make every such game disagree with the
    // host, so --check fails on it. --committed-input / --committed-client stand
    // in for the compiled-in constant (the witness for "the constant is stale").
    const fs::path tool = SiblingTool("arcane-abi-digest", "arcane-abi-digest");
    REQUIRE(fs::exists(tool));
    const std::string root = "--root \"" + RepoRootDir().string() + "\"";

    const ToolRun ok = RunWithStatus(tool, "--check " + root);
    INFO("--check: " << ok.output);
    CHECK(ok.status == 0);

    const ToolRun client = RunWithStatus(tool, "--check " + root + " --committed-client 0000000000000001");
    INFO("--check --committed-client: " << client.output);
    CHECK(client.status != 0);
    CHECK(client.output.find("kGamePluginABIDigestClient") != std::string::npos);

    const ToolRun input = RunWithStatus(tool, "--check " + root + " --committed-input 0000000000000001");
    INFO("--check --committed-input: " << input.output);
    CHECK(input.status != 0);
    CHECK(input.output.find("kGamePluginABIDigestInput") != std::string::npos);

    const ToolRun physics2d = RunWithStatus(tool, "--check " + root + " --committed-physics2d 0000000000000001");
    INFO("--check --committed-physics2d: " << physics2d.output);
    CHECK(physics2d.status != 0);
    CHECK(physics2d.output.find("kGamePluginABIDigestPhysics2D") != std::string::npos);
}
