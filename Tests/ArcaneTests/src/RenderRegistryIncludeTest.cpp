#include <catch2/catch_test_macros.hpp>
#include "Helpers/ConstantScan.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <cstdio>

static std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

#if defined(_WIN32)
static std::filesystem::path ModuleProject(const char* type, const char* module)
{
    return Arcane::Test::RepoRoot() / "Source" / type / module / (std::string(module) + ".vcxproj");
}

static std::string Capture(const std::string& command)
{
    FILE* pipe = _popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string output;
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        output += buf;
    _pclose(pipe);
    return output;
}

struct ClTool
{
    std::string vcvars;
    std::string cl;
};

static ClTool ResolveCl()
{
    const char* pfx86 = std::getenv("ProgramFiles(x86)");
    REQUIRE(pfx86 != nullptr);
    const std::filesystem::path vswhere = std::filesystem::path(pfx86)
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    REQUIRE(std::filesystem::exists(vswhere));
    const std::string vs = Capture(
        "\"" + vswhere.string() + "\" -latest -prerelease -products * "
        "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath");
    std::string install = vs;
    while (!install.empty() && (install.back() == '\n' || install.back() == '\r'))
        install.pop_back();
    REQUIRE(!install.empty());
    ClTool tool;
    tool.vcvars = (std::filesystem::path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat").string();
    REQUIRE(std::filesystem::exists(tool.vcvars));
    const std::filesystem::path msvc = std::filesystem::path(install) / "VC" / "Tools" / "MSVC";
    std::string best;
    for (const auto& entry : std::filesystem::directory_iterator(msvc))
        if (entry.is_directory() && entry.path().filename().string() > best)
            best = entry.path().filename().string();
    REQUIRE(!best.empty());
    tool.cl = (msvc / best / "bin" / "Hostx64" / "x64" / "cl.exe").string();
    REQUIRE(std::filesystem::exists(tool.cl));
    return tool;
}

static std::string GrabTag(const std::string& xml, const char* tag)
{
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const std::size_t a = xml.find(open);
    const std::size_t b = xml.find(close, a);
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    return xml.substr(a + open.size(), b - (a + open.size()));
}

// premake5.lua configurations: Debug / Release / Dist define exactly one of these.
static const char* BuiltConfiguration()
{
#if defined(ARC_BUILD_DEBUG)
    return "Debug";
#elif defined(ARC_BUILD_RELEASE)
    return "Release";
#elif defined(ARC_BUILD_DIST)
    return "Dist";
#else
#error Arcane premake defines one of ARC_BUILD_DEBUG, ARC_BUILD_RELEASE, ARC_BUILD_DIST
#endif
}

// MultiThreadedDebugDLL -> /MDd, MultiThreadedDLL -> /MD. Anything else is empty
// so the caller fails instead of inventing a flag.
static std::string RuntimeLibraryFlag(const std::string& runtimeLibrary)
{
    if (runtimeLibrary == "MultiThreadedDebugDLL")
        return "/MDd";
    if (runtimeLibrary == "MultiThreadedDLL")
        return "/MD";
    return {};
}

// The ItemDefinitionGroup whose Condition is
// '$(Configuration)|$(Platform)'=='<cfg>|x64'. Empty when that group is
// absent: the caller fails, and does not use another configuration's group.
// PropertyGroups carry the same Condition and are not compile settings.
static std::string ItemDefinitionGroupFor(const std::string& xml, const std::string& cfg)
{
    const std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "|x64'";
    const std::string openTag = "<ItemDefinitionGroup";
    const std::string closeTag = "</ItemDefinitionGroup>";
    std::size_t search = 0;
    while (search < xml.size())
    {
        const std::size_t open = xml.find(openTag, search);
        if (open == std::string::npos)
            return {};
        const std::size_t openEnd = xml.find('>', open);
        if (openEnd == std::string::npos)
            return {};
        const std::string opening = xml.substr(open, openEnd - open);
        if (opening.find(condition) != std::string::npos)
        {
            const std::size_t close = xml.find(closeTag, openEnd);
            if (close == std::string::npos)
                return {};
            return xml.substr(open, close + closeTag.size() - open);
        }
        search = openEnd + 1;
    }
    return {};
}

TEST_CASE("the ItemDefinitionGroup helper picks the named configuration", "[registry-gate]")
{
    // Release's PropertyGroup precedes both compile groups, matching the vcxproj
    // shape: the first condition hit is not the ItemDefinitionGroup.
    const char* xml =
        "<PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">"
        "<OutDir>release-out</OutDir></PropertyGroup>"
        "<ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">"
        "<PreprocessorDefinitions>FROM_DEBUG</PreprocessorDefinitions>"
        "<AdditionalIncludeDirectories>inc-debug</AdditionalIncludeDirectories>"
        "<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>"
        "</ItemDefinitionGroup>"
        "<ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">"
        "<PreprocessorDefinitions>FROM_RELEASE</PreprocessorDefinitions>"
        "<AdditionalIncludeDirectories>inc-release</AdditionalIncludeDirectories>"
        "<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>"
        "</ItemDefinitionGroup>";

    const std::string debug = ItemDefinitionGroupFor(xml, "Debug");
    REQUIRE_FALSE(debug.empty());
    CHECK(GrabTag(debug, "PreprocessorDefinitions") == "FROM_DEBUG");
    CHECK(GrabTag(debug, "AdditionalIncludeDirectories") == "inc-debug");
    CHECK(RuntimeLibraryFlag(GrabTag(debug, "RuntimeLibrary")) == "/MDd");

    const std::string release = ItemDefinitionGroupFor(xml, "Release");
    REQUIRE_FALSE(release.empty());
    CHECK(GrabTag(release, "PreprocessorDefinitions") == "FROM_RELEASE");
    CHECK(GrabTag(release, "AdditionalIncludeDirectories") == "inc-release");
    CHECK(RuntimeLibraryFlag(GrabTag(release, "RuntimeLibrary")) == "/MD");

    // A missing configuration is a miss. It must not come back as Debug.
    CHECK(ItemDefinitionGroupFor(xml, "Dist").empty());

    const std::string built = ItemDefinitionGroupFor(xml, BuiltConfiguration());
#if defined(ARC_BUILD_DEBUG)
    REQUIRE_FALSE(built.empty());
    CHECK(GrabTag(built, "PreprocessorDefinitions") == "FROM_DEBUG");
    CHECK(RuntimeLibraryFlag(GrabTag(built, "RuntimeLibrary")) == "/MDd");
#elif defined(ARC_BUILD_RELEASE)
    REQUIRE_FALSE(built.empty());
    CHECK(GrabTag(built, "PreprocessorDefinitions") == "FROM_RELEASE");
    CHECK(RuntimeLibraryFlag(GrabTag(built, "RuntimeLibrary")) == "/MD");
#else
    CHECK(built.empty());
#endif
}

static int ShowIncludes(const ClTool& tool, const std::filesystem::path& tu, std::string& output,
                        const std::filesystem::path& vcx = ModuleProject("Client", "ArcaneRender"))
{
    const std::filesystem::path projectDir = vcx.parent_path();
    const std::string xml = ReadAll(vcx);
    const char* cfg = BuiltConfiguration();
    const std::string group = ItemDefinitionGroupFor(xml, cfg);
    if (group.empty())
    {
        FAIL("no ItemDefinitionGroup with Condition '$(Configuration)|$(Platform)'=='"
             << cfg << "|x64' in " << vcx.string()
             << "; refusing to use another configuration");
        return 1;
    }
    const std::string includes = GrabTag(group, "AdditionalIncludeDirectories");
    const std::string defines = GrabTag(group, "PreprocessorDefinitions");
    const std::string runtimeFlag = RuntimeLibraryFlag(GrabTag(group, "RuntimeLibrary"));
    if (runtimeFlag.empty())
    {
        FAIL("RuntimeLibrary in " << vcx.string() << " (" << cfg
             << ") is not MultiThreadedDebugDLL (/MDd) or MultiThreadedDLL (/MD)");
        return 1;
    }

    // AdditionalOptions from the module vcxproj. fmt static_asserts without /utf-8.
    // RuntimeLibrary is the built configuration's flag, read above, not a fixed /MD.
    std::string cmd = "cmd /c \"call \"" + tool.vcvars + "\" >nul && \"" + tool.cl
        + "\" /nologo /showIncludes /Zs /std:c++latest /EHsc " + runtimeFlag
        + " /utf-8 /arch:AVX2 /Zc:__cplusplus ";
    std::stringstream def(defines);
    std::string item;
    while (std::getline(def, item, ';'))
    {
        if (item.empty() || item.find('%') != std::string::npos)
            continue;
        cmd += "/D" + item + " ";
    }
    std::stringstream inc(includes);
    while (std::getline(inc, item, ';'))
    {
        if (item.empty() || item.find("%(") != std::string::npos)
            continue;   // premake appends %(AdditionalIncludeDirectories)
        const std::string projectToken = "$(ProjectDir)";
        const std::string solutionToken = "$(SolutionDir)";
        if (item.find(projectToken) != std::string::npos)
            item.replace(item.find(projectToken), projectToken.size(), projectDir.string() + "\\");
        if (item.find(solutionToken) != std::string::npos)
            item.replace(item.find(solutionToken), solutionToken.size(),
                         Arcane::Test::RepoRoot().string() + "\\");
        REQUIRE(item.find("$(") == std::string::npos);
        std::filesystem::path path(item);
        if (path.is_relative())
            path = projectDir / path;
        cmd += "/I\"" + path.string() + "\" ";
    }
    cmd += "\"" + tu.string() + "\" 2>&1\"";
    FILE* pipe = _popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        output += buf;
    return _pclose(pipe);
}

static bool RegistryHit(const std::string& output)
{
    return output.find("Astra\\Registry\\Registry.hpp") != std::string::npos
        || output.find("Astra/Registry/Registry.hpp") != std::string::npos;
}

static bool EcsHit(const std::string& output)
{
    return output.find("Arcane\\Ecs.hpp") != std::string::npos
        || output.find("Arcane/Ecs.hpp") != std::string::npos;
}

TEST_CASE("render TUs preprocess Registry.hpp only from ExtractFrame.cpp", "[registry-gate]")
{
    const ClTool tool = ResolveCl();
    const auto root = Arcane::Test::RepoRoot() / "Source/Client/ArcaneRender/src/Arcane/Render";
    bool sawExtract = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (entry.path().extension() != ".cpp")
            continue;
        std::string output;
        const int code = ShowIncludes(tool, entry.path(), output);
        INFO(entry.path().string());
        INFO(output);
        CHECK(code == 0);
        const bool extract = entry.path().filename() == "ExtractFrame.cpp";
        if (extract)
            sawExtract = true;
        if (extract)
            CHECK(RegistryHit(output));
        else
            CHECK_FALSE(RegistryHit(output));
        if (!extract)
            CHECK(ReadAll(entry.path()).find("Astra::Registry") == std::string::npos);
    }
    CHECK(sawExtract);
}

TEST_CASE("a clean include of DepthConvention passes and SceneCamera fails the same check", "[registry-gate]")
{
    const ClTool tool = ResolveCl();
    const auto dir = std::filesystem::temp_directory_path();
    const auto passTu = dir / "f5_depth_only.cpp";
    const auto failTu = dir / "f5_scene_camera.cpp";
    {
        std::ofstream o(passTu);
        o << "#include <Arcane/Scene/DepthConvention.hpp>\n";
    }
    {
        std::ofstream o(failTu);
        o << "#include <Arcane/Scene/SceneCamera.hpp>\n";
    }
    std::string passOut, failOut;
    CHECK(ShowIncludes(tool, passTu, passOut) == 0);
    CHECK_FALSE(RegistryHit(passOut));
    ShowIncludes(tool, failTu, failOut);
    CHECK(RegistryHit(failOut));
    std::filesystem::remove(passTu);
    std::filesystem::remove(failTu);
}

TEST_CASE("cache and debug-draw TUs preprocess neither Registry.hpp nor Ecs.hpp", "[registry-gate]")
{
    const ClTool tool = ResolveCl();
    const auto root = Arcane::Test::RepoRoot() / "Source/Client/ArcaneRender/src/Arcane/Render";
    const char* names[] = {
        "MeshCache.cpp", "MeshMaterialCache.cpp", "SpriteCache.cpp", "PhysicsDebugDraw.cpp" };
    for (const char* name : names)
    {
        std::string output;
        const int code = ShowIncludes(tool, root / name, output);
        INFO(name);
        INFO(output);
        CHECK(code == 0);
        CHECK_FALSE(RegistryHit(output));
        CHECK_FALSE(EcsHit(output));
    }
    // Ruling PR-2: the render node that left the render tree: ArcaneImGui's
    // ImGuiNriNode.cpp, compiled with ArcaneImGui's own flags.
    {
        std::string output;
        const auto node = Arcane::Test::RepoRoot() / "Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiNriNode.cpp";
        const int code = ShowIncludes(tool, node, output, ModuleProject("Client", "ArcaneImGui"));
        INFO(output);
        CHECK(code == 0);
        CHECK_FALSE(RegistryHit(output));
        CHECK_FALSE(EcsHit(output));
    }
}
#endif // _WIN32

// Called on comment-stripped text (HostGateHits), so a brace inside a comment
// cannot end or extend the body; braces inside string and character literals
// are skipped the same way.
static std::string FunctionBody(const std::string& text, const std::string& signature)
{
    const std::size_t sig = text.find(signature);
    if (sig == std::string::npos) return {};
    const std::size_t open = text.find('{', sig);
    if (open == std::string::npos) return {};
    int depth = 0;
    char quote = 0;
    for (std::size_t i = open; i < text.size(); ++i)
    {
        const char c = text[i];
        if (quote)
        {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0)
            return text.substr(open, i - open + 1);
    }
    return {};
}

static std::string Strip(std::string row, const char* token)
{
    for (;;)
    {
        const std::size_t at = row.find(token);
        if (at == std::string::npos)
            return row;
        row.erase(at, std::char_traits<char>::length(token));
    }
}

static bool IdentChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_';
}

static std::string StripComments(const std::string& body)
{
    std::string out;
    out.reserve(body.size());
    enum class Mode { Code, Line, Block, String, Char };
    Mode mode = Mode::Code;
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        const char c = body[i];
        const char n = (i + 1 < body.size()) ? body[i + 1] : '\0';
        if (mode == Mode::Code)
        {
            if (c == '/' && n == '/')      { mode = Mode::Line;  out.push_back(' '); ++i; }
            else if (c == '/' && n == '*') { mode = Mode::Block; out.push_back(' '); ++i; }
            else if (c == '"')             { mode = Mode::String; out.push_back(c); }
            else if (c == '\'')            { mode = Mode::Char;   out.push_back(c); }
            else out.push_back(c);
        }
        else if (mode == Mode::Line)
        {
            if (c == '\n') { mode = Mode::Code; out.push_back('\n'); }
            else out.push_back(' ');
        }
        else if (mode == Mode::Block)
        {
            if (c == '*' && n == '/') { mode = Mode::Code; out.push_back(' '); ++i; }
            else if (c == '\n') out.push_back('\n');
            else out.push_back(' ');
        }
        else
        {
            out.push_back(c);
            if (c == '\\' && n) { out.push_back(n); ++i; }
            else if ((mode == Mode::String && c == '"') || (mode == Mode::Char && c == '\''))
                mode = Mode::Code;
        }
    }
    return out;
}

static bool WholeWord(const std::string& row, const char* token)
{
    const std::size_t n = std::char_traits<char>::length(token);
    for (std::size_t at = 0; (at = row.find(token, at)) != std::string::npos; at += n)
    {
        const bool left  = at > 0 && IdentChar(row[at - 1]);
        const bool right = at + n < row.size() && IdentChar(row[at + n]);
        if (!left && !right)
            return true;
    }
    return false;
}

static int HostGateHits(const std::string& text, const std::string& signature)
{
    // Strip comments over the whole file first, then find the body: a lone
    // brace in a comment must not truncate or extend the scanned function.
    const std::string body = FunctionBody(StripComments(text), signature);
    if (body.empty()) return -1;
    const char* tokens[] = {
        "CreateView", "GetComponent", "GetResource", "InspectEntity",
        "ActiveSceneCamera", "ActivePerspectiveSceneCamera", "CollectPickables",
        "PrepareSceneForRender", "BuildVisibleSet", "GpuSceneSync"
    };
    const bool stripInvalidate = signature == "RenderGraph("
        || signature == "EditorApp::RenderSceneToViewport(";
    const bool stripPick = signature == "EditorApp::ArmGraphViewportFrame(";
    int hits = 0;
    std::size_t line = 0;
    while (line < body.size())
    {
        const std::size_t end = body.find('\n', line);
        std::string row = body.substr(line, (end == std::string::npos ? body.size() : end) - line);
        if (stripInvalidate) row = Strip(row, "GpuSceneInvalidate");
        if (stripPick) row = Strip(row, "PickPassIdOf");
        for (const char* token : tokens)
            if (WholeWord(row, token))
                ++hits;
        if (end == std::string::npos) break;
        line = end + 1;
    }
    return hits;
}

TEST_CASE("stripping a call name does not exempt the rest of the line", "[registry-gate]")
{
    const std::string mixed =
        "void RenderGraph()\n{\n    GpuSceneInvalidate(reg); reg.CreateView<const Arcane::Transform>();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(mixed, "RenderGraph(") == 1);
    const std::string only =
        "void RenderGraph()\n{\n    GpuSceneInvalidate(reg);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(only, "RenderGraph(") == 0);
    const std::string pick =
        "void EditorApp::ArmGraphViewportFrame()\n{\n    PickPassIdOf(e); reg.GetComponent<Arcane::Transform>(e);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(pick, "EditorApp::ArmGraphViewportFrame(") == 1);
}

TEST_CASE("a brace inside a comment does not end the scanned body", "[registry-gate]")
{
    const std::string brace =
        "void RenderGraph()\n{\n    // } a stray brace\n    reg.CreateView<const Arcane::Transform>();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(brace, "RenderGraph(") == 1);
    const std::string literal =
        "void RenderGraph()\n{\n    const char* s = \"}\";\n    reg.GetResource<Arcane::MeshTable>();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(literal, "RenderGraph(") == 1);
}

TEST_CASE("comments and a longer identifier are not host-gate hits", "[registry-gate]")
{
    const std::string comments =
        "void RenderGraph()\n{\n"
        "    // CollectPickables\n"
        "    /* PrepareSceneForRender\n       GpuSceneSync */\n"
        "    GpuSceneSyncNode();\n"
        "}\nvoid Next(){}\n";
    CHECK(HostGateHits(comments, "RenderGraph(") == 0);
    const std::string inString =
        "void RenderGraph()\n{\n    const char* kept = \"// CreateView\";\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(inString, "RenderGraph(") == 1);
    const std::string call =
        "void RenderGraph()\n{\n    GpuSceneSync(reg);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(call, "RenderGraph(") == 1);
    const std::string onlyNode =
        "void RenderGraph()\n{\n    GpuSceneSyncNode();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(onlyNode, "RenderGraph(") == 0);
}

TEST_CASE("named frame builders do not read the registry", "[registry-gate]")
{
    const std::string runtime = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp");
    const std::string editor = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    const std::string harvest = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp");
    const std::string preview = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/Documents/ShaderEditorDocument.cpp");
    const std::string editorApp = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorApp.cpp");
    CHECK(HostGateHits(runtime, "RenderGraph(") == 0);
    CHECK(HostGateHits(runtime, "BuildHud(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::RefreshSceneResolution(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::RenderSceneToViewport(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::SubmitSceneToBatcher(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::ArmGraphViewportFrame(") == 0);
    CHECK(HostGateHits(harvest, "MaterialPreviewHarvester::Impl::Harvest(") == 0);
    CHECK(HostGateHits(preview, "RenderGraphPreview(") == 0);
    CHECK(HostGateHits(editorApp, "EditorApp::CaptureGraphViewportPng(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::BuildExtractRequest(") == 0);
}
