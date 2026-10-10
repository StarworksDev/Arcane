#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>   // MaterialCensus -- the binding-readiness probe

#include <Arcane/Assets/Assets.hpp>       // Task 11: Assets::MeshArtifactFor/CookPending (golden prop)
#include <Arcane/Base/Engine.hpp>        // ExecutablePathUtf8 (the argv[0] replacement)
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Config/Config.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // kGamePluginABIVersion (the probe tripwire)
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (ReferenceProject material resolution)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>      // LoadMeshAsset (Task 11 -- ReferenceProject's mesh content)
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>       // MeshTable/MeshMaterialTable (the census's bound half)
#include <Arcane/Serialization/SceneAsset.hpp>   // kSceneJsonVersion, kSceneExt

// The scene-content case below stitches and compiles ReferenceProject's
// shipped .arcmat files -- no device involved, the same device-less
// [shadercompile] idiom MaterialSourceTest uses.
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>   // kVsEntry/kPsEntry + profiles
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include "Helpers/ReferenceProjectDir.hpp"   // FindReferenceProjectDir (was this file's own)
#include "Helpers/TestTypeContext.hpp"

#include <Astra/Reflection/Reflection.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>   // the mesh-table case compares bounds/colour vectors

#include <Json.hpp>

#include <catch2/catch_approx.hpp>   // the ReferenceProject pose assertion
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Platform/Platform.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // Locate the repo's real Arcane/ReferenceProject from wherever this test exe
    // happens to run. MOVED (F2c debts arc, Task E) to Helpers/
    // ReferenceProjectDir.hpp -- the thumbnail golden set needs the SAME walk to
    // reach Content/ and to bless SOURCE references, and two copies of a
    // filesystem search that must agree is exactly how they stop agreeing. The
    // reasoning that used to live here moved with it.
    using Arcane::Test::FindReferenceProjectDir;
}

// VerifySharedTypeContext, widened 2026-09-16 from a single Transform probe to the
// whole EngineComponentRoster (Arcane/Scene/EngineRoster.hpp).
//
// THE HAPPY PATH IS ALL THAT IS CONSTRUCTIBLE HERE, and deliberately so: the check
// compares THIS module's cached TypeID<T>::Value() against the registry's id for the
// same name hash, and this process has exactly one shared TypeContext (test_main
// installs it before Catch2 runs), so ids are process-wide first-touch and the test
// exe cannot be made to hold a second, wrong set. The negative case is a HOST bug by
// nature -- a module resolving a type before its SetTypeContext -- and its RED was
// the real one: with EditorApp's EditModeSchedule still a plain member, ArcaneEditor
// .exe refused boot here naming Arcane::WorldTransform with two different ids. What
// this case pins is that the widened check does not false-positive on a correct
// module, for every one of the twelve types rather than just Transform.
TEST_CASE("VerifySharedTypeContext accepts a module on the shared context, for the whole engine roster",
          "[host][typecontext]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    CHECK(Arcane::ProjectHost::VerifySharedTypeContext(rt.Registry(), "ArcaneTests.exe"));
    // The primitive the roster version expands, still callable for one named type.
    CHECK(Arcane::ProjectHost::VerifySharedTypeContextFor<Arcane::Transform>(rt.Registry(), "ArcaneTests.exe"));
    CHECK(Arcane::ProjectHost::VerifySharedTypeContextFor<Arcane::WorldTransform>(rt.Registry(), "ArcaneTests.exe"));
    // An UNREGISTERED type is not a contradiction -- unchanged from the one-probe
    // version, and what lets the check run before a game module has registered.
    struct NeverRegistered { int x = 0; };
    CHECK(Arcane::ProjectHost::VerifySharedTypeContextFor<NeverRegistered>(rt.Registry(), "ArcaneTests.exe"));
}

TEST_CASE("HostConfig parses --project", "[host]")
{
    const char* argv[] = { "ArcaneRuntime", "--project", "MyGame" };
    auto out = Arcane::HostConfig::Parse(3, const_cast<char**>(argv));
    REQUIRE(out.config.has_value());
    REQUIRE(out.config->projectPath == "MyGame");
}

TEST_CASE("HostConfig defaults --project to empty", "[host]")
{
    const char* argv[] = { "ArcaneRuntime" };
    auto out = Arcane::HostConfig::Parse(1, const_cast<char**>(argv));
    REQUIRE(out.config.has_value());
    REQUIRE(out.config->projectPath.empty());
}

TEST_CASE("HostBoot::GameModule falls back with no/empty gameModule", "[host]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_gm";
    std::error_code ec; fs::remove_all(dir, ec);
    REQUIRE(Arcane::Project::Create(dir, "G").has_value());   // Create writes gameModule ""
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "Fallback.dll") == "Fallback.dll");
    REQUIRE(Arcane::HostBoot::GameModule(nullptr, "Fallback.dll") == "Fallback.dll");
    // Empty fallback + no project -> empty: the editor's "start with no game" signal
    // (bare ArcaneEditor leaves pluginPath empty, so nothing is loaded).
    REQUIRE(Arcane::HostBoot::GameModule(nullptr, "").empty());
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "").empty());   // content-only project, no --plugin
    fs::remove_all(dir, ec);
}

TEST_CASE("HostBoot::GameModule returns the manifest gameModule when set", "[host]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_gm2";
    std::error_code ec; fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);
    std::ofstream(dir / "P.arcproj", std::ios::binary) <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":5},)"
        R"("gameModule":"Foo.dll","plugins":[],"bootScene":""})";
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    // The authored name, in the running platform's spelling (Foo.so on ELF;
    // exactly "Foo.dll" on Windows -- Platform::NativeModuleFileName).
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "Fallback.dll") == Arcane::Platform::ModuleFileName("Foo"));
    fs::remove_all(dir, ec);
}

TEST_CASE("HostBoot::GameModule resolves the project's Binaries/ copy when built", "[host]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_gm3";
    std::error_code ec; fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);
    fs::create_directories(dir / "Binaries", ec);
    std::ofstream(dir / "P.arcproj", std::ios::binary) <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":5},)"
        R"("gameModule":"Aphelyon.dll","plugins":[],"bootScene":""})";
    // The project has built its own module -> the host must load THIS copy, not a
    // same-named DLL beside the exe.
    std::ofstream(dir / "Binaries" / Arcane::Platform::ModuleFileName("Aphelyon"), std::ios::binary) << "MZ";  // presence is what matters

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(fs::path(Arcane::HostBoot::GameModule(&*proj, "Fallback.dll"))
            == dir / "Binaries" / Arcane::Platform::ModuleFileName("Aphelyon"));

    // Without the built copy, it stays a bare name (borrowing path, resolved beside exe).
    fs::remove(dir / "Binaries" / Arcane::Platform::ModuleFileName("Aphelyon"), ec);
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "Fallback.dll") == Arcane::Platform::ModuleFileName("Aphelyon"));
    fs::remove_all(dir, ec);
}

TEST_CASE("HostBoot::LoadInputConfig loads the input category", "[host]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_input";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir, ec);
    std::ofstream(dir / "input.json", std::ios::binary) <<
        R"({"actionMaps":[{"name":"demo","actions":[{"name":"quit","type":"Button",)"
        R"("bindings":[{"path":"<Keyboard>/escape"}]}]}]})";

    Arcane::Config config;
    config.LoadEngineDefaults(dir);   // temp dir stands in as the engine-default config layer

    auto input = Arcane::InputActions::Create();
    REQUIRE(Arcane::HostBoot::LoadInputConfig(*input, config) == true);
    REQUIRE(input->ActiveContext() == "demo");
    fs::remove_all(dir, ec);
}

// --- Engine probe (Arcane Hub slice 1) -----------------------------------------
// The Hub stamps `engine.abi` into every .arcproj it creates. If that number is
// ever sourced from anywhere but the engine itself, New Project silently mints
// stale-ABI projects and they crash on open -- the exact failure recorded in the
// shader-editor arc's ABI LESSON.
//
// WHAT THIS SUITE CANNOT DO (corrected after the 2026-07-26 review found two
// reviewers flagging it independently): it does NOT catch an ABI bump.
// EngineInfoJson is an inline header function reading kGamePluginABIVersion and
// the assertion below compares against kGamePluginABIVersion, so both sides
// expand to the same constant in this one TU -- bumping it moves both and the
// test stays green by construction.
//
// What it DOES catch: a refactor that hardcodes a literal instead of the
// constant, a renamed/dropped JSON key, a type change, a payload that stops
// being one parseable line. What it structurally CANNOT catch is a STALE BUILT
// BINARY, which is the failure the design actually cares about -- only spawning
// the built exe and parsing its stdout catches that, which is what the Hub does
// at registration time.

TEST_CASE("EngineInfoJson sources the ABI from the engine constant", "[host]")
{
    const std::string s = Arcane::HostBoot::EngineInfoJson("C:/some/ArcaneEditor.exe");
    const nlohmann::json j = nlohmann::json::parse(s);

    REQUIRE(j.contains("engineAbi"));
    REQUIRE(j["engineAbi"].is_number_unsigned());
    // Pins the key, the type, and the fact that the value comes from the constant
    // rather than a literal. NOT an ABI-bump tripwire -- see the note above.
    CHECK(j["engineAbi"].get<std::uint32_t>() == Arcane::kGamePluginABIVersion);
}

TEST_CASE("EngineInfoJson carries build + exe path and parses cleanly", "[host]")
{
    // Fed BACKSLASHES on purpose: the previous fixture passed an
    // already-forward-slashed path, so the normalisation assertion below held
    // trivially and could not have failed. This is the Win32 shape
    // Arcane::ExecutablePathUtf8 actually produces before normalisation.
    const std::string s = Arcane::HostBoot::EngineInfoJson("C:\\some dir\\ArcaneEditor.exe");
    const nlohmann::json j = nlohmann::json::parse(s);   // must not throw

    REQUIRE(j.contains("build"));
    CHECK(j["build"].is_string());
    CHECK_FALSE(j["build"].get<std::string>().empty());

    REQUIRE(j.contains("exePath"));
    // Forward slashes so the Hub never has to unescape backslashes out of JSON.
    CHECK(j["exePath"].get<std::string>() == "C:/some dir/ArcaneEditor.exe");
}

TEST_CASE("EngineInfoJson survives a non-ASCII install path", "[host]")
{
    // Regression for MAJOR 5 of the 2026-07-26 review. The probe used to be fed
    // argv[0], which MSVC hands to main() in the ACTIVE ANSI CODEPAGE. Under an
    // install path like C:\Users\Jose\... those bytes are not valid UTF-8, and
    // nlohmann's dump() is strict-UTF-8 with no JSON_NOEXCEPTION -- it threw
    // type_error.316 straight out of main and terminated. The probe failing in
    // exactly the way it exists to prevent.
    //
    // Two belts are under test: callers now pass Arcane::ExecutablePathUtf8()
    // (well-formed UTF-8 from the wide Win32 path), and dump() runs with
    // error_handler_t::replace so even malformed input degrades instead of
    // throwing.
    SECTION("well-formed UTF-8 round-trips byte for byte")
    {
        // "C:/Users/José/Arcane/ArcaneEditor.exe" -- e-acute as UTF-8 (C3 A9).
        const std::string path = "C:/Users/Jos\xC3\xA9/Arcane/ArcaneEditor.exe";
        std::string s;
        REQUIRE_NOTHROW(s = Arcane::HostBoot::EngineInfoJson(path));
        const nlohmann::json j = nlohmann::json::parse(s);
        CHECK(j["exePath"].get<std::string>() == path);
        CHECK(s.find('\n') == std::string::npos);
    }

    SECTION("a raw ANSI byte does not throw out of the probe")
    {
        // 0xE9 alone is e-acute in CP-1252 and an INVALID UTF-8 lead byte -- i.e.
        // literally what argv[0] used to hand over. Must degrade, never throw.
        const std::string ansi = "C:/Users/Jos\xE9/ArcaneEditor.exe";
        std::string s;
        REQUIRE_NOTHROW(s = Arcane::HostBoot::EngineInfoJson(ansi));
        REQUIRE_NOTHROW(nlohmann::json::parse(s));   // still valid JSON
        CHECK(s.find('\n') == std::string::npos);
    }
}

TEST_CASE("ExecutablePathUtf8 reports this test exe, absolute and forward-slashed", "[host]")
{
    // The replacement for argv[0]. Asserted against the real running process, so
    // it also proves the export crosses the Arcane.dll boundary.
    const std::string exe = Arcane::ExecutablePathUtf8();
    REQUIRE_FALSE(exe.empty());
    CHECK(exe.find('\\') == std::string::npos);        // normalised
    CHECK(std::filesystem::path(exe).is_absolute());   // NOT a bare argv[0] name
    // Case-insensitively ends with the test exe's name (Win32 paths vary in case).
    std::string lower = exe;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(lower.find(Arcane::Platform::ExecutableFileName("arcanetests")) != std::string::npos);   // .exe on Windows only
}

TEST_CASE("EngineInfoJson is a single line", "[host]")
{
    // The Hub reads one line from stdout. A pretty-printed payload would make it
    // guess where the object ends.
    const std::string s = Arcane::HostBoot::EngineInfoJson("x.exe");
    CHECK(s.find('\n') == std::string::npos);
}

TEST_CASE("HostConfig parses --print-engine-info and defaults it off", "[host]")
{
    {
        const char* argv[] = { "ArcaneEditor.exe" };
        auto out = Arcane::HostConfig::Parse(1, const_cast<char**>(argv));
        REQUIRE(out.config.has_value());
        CHECK_FALSE(out.config->printEngineInfo);
    }
    {
        const char* argv[] = { "ArcaneEditor.exe", "--print-engine-info" };
        auto out = Arcane::HostConfig::Parse(2, const_cast<char**>(argv));
        REQUIRE(out.config.has_value());
        CHECK(out.config->printEngineInfo);
    }
}

// --- Task 7: HostBoot::BootSceneFile / BootScene ---------------------------------
// A project opens into its boot scene. BootSceneFile is the pure Guid -> file
// resolution (the part with the interesting failure modes: no boot scene, or an
// id this project's AssetRegistry does not contain); BootScene composes it with
// Scene::ReadSceneFile/ApplySceneDocument/Runtime::ResetRegistry and hands the
// resolved file + the scene's asset id BACK to the caller so SceneSession::Adopt
// does not have to re-open and re-parse the same file just to recover the Guid.

TEST_CASE("BootSceneFile resolves the manifest's bootScene Guid to a file", "[host][project]")
{
    // The pure half: Guid -> physical file, through the AssetRegistry the
    // project rebuilt at open. Empty when there is no boot scene, or when the
    // id names nothing this project contains.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_resolve";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content" / "scenes", ec);

    const Arcane::Guid id = Arcane::Guid::Generate();
    std::ofstream(dir / "Content" / "scenes" / "main.arcscene")
        << R"({"id":")" << id.ToString() << R"(","version":)"
        << Arcane::Scene::kSceneJsonVersion << R"(,"entities":[]})";

    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":")" << id.ToString() << R"("})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    const fs::path file = Arcane::HostBoot::BootSceneFile(*proj);
    REQUIRE_FALSE(file.empty());
    CHECK(file.filename() == "main.arcscene");

    fs::remove_all(dir, ec);
}

TEST_CASE("BootSceneFile is empty for no boot scene and for an unknown id", "[host][project]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_absent";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);

    SECTION("empty bootScene")
    {
        std::ofstream(dir / "P.arcproj") <<
            R"({"formatVersion":1,"name":"P","engine":{"abi":)"
            << static_cast<int>(Arcane::kGamePluginABIVersion)
            << R"(},"gameModule":"","plugins":[],"bootScene":""})";
        auto proj = Arcane::Project::Open(dir);
        REQUIRE(proj.has_value());
        CHECK(Arcane::HostBoot::BootSceneFile(*proj).empty());
    }
    SECTION("a Guid this project does not contain")
    {
        std::ofstream(dir / "P.arcproj") <<
            R"({"formatVersion":1,"name":"P","engine":{"abi":)"
            << static_cast<int>(Arcane::kGamePluginABIVersion)
            << R"(},"gameModule":"","plugins":[],"bootScene":")"
            << Arcane::Guid::Generate().ToString() << R"("})";
        auto proj = Arcane::Project::Open(dir);
        REQUIRE(proj.has_value());
        CHECK(Arcane::HostBoot::BootSceneFile(*proj).empty());
    }

    fs::remove_all(dir, ec);
}

TEST_CASE("BootScene loads the resolved scene into the runtime and reports the file + id back",
          "[host][project]")
{
    // The integration the brief's editor snippet got wrong: BootScene must hand
    // the caller enough to Adopt() the scene WITHOUT a second ReadSceneFile of
    // the same path -- this pins that the returned file/id actually match what
    // was applied, not just that loading succeeded.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_load";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content" / "scenes", ec);

    const Arcane::Guid id = Arcane::Guid::Generate();
    const std::string ltName(Astra::GetMeta<Arcane::Transform>()->typeName);

    nlohmann::json e0;
    e0["components"][ltName]["position"] = { 3.0, 4.0, 0.0 };   // Task 3 (F1): position is a vec3
    nlohmann::json doc;
    doc["id"] = id.ToString();
    doc["version"] = Arcane::Scene::kSceneJsonVersion;
    doc["entities"] = nlohmann::json::array({ e0 });
    std::ofstream(dir / "Content" / "scenes" / "main.arcscene") << doc.dump();

    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":")" << id.ToString() << R"("})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    Arcane::Runtime rt(Arcane::Test::Process());
    // Stands in for whatever the plugin's Init built before the boot scene
    // loads -- BootScene must DISCARD this, not merge into it.
    rt.Registry().CreateEntity();
    REQUIRE(rt.Registry().Size() == 1);

    const auto result = Arcane::HostBoot::BootScene(rt, *proj);
    REQUIRE(result.has_value());
    CHECK(result->file.filename() == "main.arcscene");
    CHECK(result->id == id);

    // The sentinel is gone, replaced by the one boot-scene entity -- proves
    // ResetRegistry ran BEFORE ApplySceneDocument, not merged after it.
    REQUIRE(rt.Registry().Size() == 1);
    const Arcane::Transform* loaded = nullptr;
    rt.Registry().CreateView<Arcane::Transform>().ForEach(
        [&](Astra::Entity, Arcane::Transform& t) { loaded = &t; });
    REQUIRE(loaded != nullptr);
    CHECK(loaded->position.x == 3.0f);
    CHECK(loaded->position.y == 4.0f);

    fs::remove_all(dir, ec);
}

TEST_CASE("BootScene leaves the registry untouched when there is no boot scene", "[host][project]")
{
    // No boot scene configured: a project with none keeps whatever the plugin's
    // Init already built (EditorApp::Init's own comment) -- BootScene must not
    // reset anything in this branch.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_none";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);
    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":""})";
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Registry().CreateEntity();
    REQUIRE(rt.Registry().Size() == 1);

    CHECK_FALSE(Arcane::HostBoot::BootScene(rt, *proj).has_value());
    CHECK(rt.Registry().Size() == 1);   // untouched -- nothing to load

    fs::remove_all(dir, ec);
}

TEST_CASE("BootScene leaves the registry untouched when the resolved file fails to parse",
          "[host][project]")
{
    // A boot scene IS configured and DOES resolve to a file, but the file
    // itself is bad (wrong schema version here). Distinct from the "unknown
    // id" BootSceneFile case above: this drives BootScene's OWN
    // ReadSceneFile-failure branch, which must run BEFORE ResetRegistry, same
    // ordering rule as the editor's Open Scene.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_badfile";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content" / "scenes", ec);

    const Arcane::Guid id = Arcane::Guid::Generate();
    std::ofstream(dir / "Content" / "scenes" / "main.arcscene")
        << R"({"id":")" << id.ToString() << R"(","version":1,"entities":[]})";   // wrong version

    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":")" << id.ToString() << R"("})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Registry().CreateEntity();
    REQUIRE(rt.Registry().Size() == 1);

    CHECK_FALSE(Arcane::HostBoot::BootScene(rt, *proj).has_value());
    CHECK(rt.Registry().Size() == 1);   // untouched -- read failed before any reset

    fs::remove_all(dir, ec);
}

// --- Guid-taking BootSceneFile/BootScene overloads (runtime host --scene) ---
// The runtime host's `--scene` override (HostConfig::sceneOverride) already
// has a parsed Guid in hand and should not have to round-trip it back through
// the manifest's bootScene text -- these overloads take the Guid directly.
// Same resolution/failure modes as the manifest-path overloads above (shared
// via ProjectBoot.hpp's Detail::ApplySceneFile), just fed an explicit id
// instead of project.Manifest().bootScene.

TEST_CASE("BootSceneFile(project, id) resolves an explicit Guid to a file", "[host][project]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_guid_resolve";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content" / "scenes", ec);

    const Arcane::Guid id = Arcane::Guid::Generate();
    std::ofstream(dir / "Content" / "scenes" / "main.arcscene")
        << R"({"id":")" << id.ToString() << R"(","version":)"
        << Arcane::Scene::kSceneJsonVersion << R"(,"entities":[]})";

    // bootScene left EMPTY on purpose -- this exercises the Guid overload, not
    // the manifest path (BootSceneFile(project) would return empty here).
    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":""})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(Arcane::HostBoot::BootSceneFile(*proj).empty());   // manifest path has nothing

    const fs::path file = Arcane::HostBoot::BootSceneFile(*proj, id);
    REQUIRE_FALSE(file.empty());
    CHECK(file.filename() == "main.arcscene");

    fs::remove_all(dir, ec);
}

TEST_CASE("BootScene(runtime, project, id) boots an explicit Guid override into the runtime",
          "[host][project]")
{
    // The happy path a runtime host's --scene override drives: a Guid that
    // resolves to a scene file boots it exactly like the manifest path does,
    // reporting the file + id back the same way.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_guid_load";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content" / "scenes", ec);

    const Arcane::Guid id = Arcane::Guid::Generate();
    const std::string ltName(Astra::GetMeta<Arcane::Transform>()->typeName);

    nlohmann::json e0;
    e0["components"][ltName]["position"] = { 5.0, 6.0, 0.0 };   // Task 3 (F1): position is a vec3
    nlohmann::json doc;
    doc["id"] = id.ToString();
    doc["version"] = Arcane::Scene::kSceneJsonVersion;
    doc["entities"] = nlohmann::json::array({ e0 });
    std::ofstream(dir / "Content" / "scenes" / "main.arcscene") << doc.dump();

    // bootScene left EMPTY -- proves the override, not the manifest, drove this.
    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":""})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Registry().CreateEntity();
    REQUIRE(rt.Registry().Size() == 1);

    const auto result = Arcane::HostBoot::BootScene(rt, *proj, id);
    REQUIRE(result.has_value());
    CHECK(result->file.filename() == "main.arcscene");
    CHECK(result->id == id);

    // The sentinel is gone, replaced by the one boot-scene entity -- same
    // ResetRegistry-before-ApplySceneDocument ordering as the manifest path.
    REQUIRE(rt.Registry().Size() == 1);
    const Arcane::Transform* loaded = nullptr;
    rt.Registry().CreateView<Arcane::Transform>().ForEach(
        [&](Astra::Entity, Arcane::Transform& t) { loaded = &t; });
    REQUIRE(loaded != nullptr);
    CHECK(loaded->position.x == 5.0f);
    CHECK(loaded->position.y == 6.0f);

    fs::remove_all(dir, ec);
}

TEST_CASE("BootSceneFile/BootScene(project, id) fall into the manifest path's empty failure mode "
          "for an unresolvable Guid", "[host][project]")
{
    // An override Guid that parses but names nothing this project contains --
    // must hit the SAME "does not resolve to a file" ARC_WARN/empty path as the
    // manifest's unknown-id case ("BootSceneFile is empty for no boot scene and
    // for an unknown id" above), not a distinct failure mode of its own.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_guid_unresolvable";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);

    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":""})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    const Arcane::Guid unknown = Arcane::Guid::Generate();
    CHECK(Arcane::HostBoot::BootSceneFile(*proj, unknown).empty());

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Registry().CreateEntity();
    REQUIRE(rt.Registry().Size() == 1);

    CHECK_FALSE(Arcane::HostBoot::BootScene(rt, *proj, unknown).has_value());
    CHECK(rt.Registry().Size() == 1);   // untouched -- nothing resolved, nothing reset

    fs::remove_all(dir, ec);
}

// --- Task 9: ReferenceProject ships an authored scene and opens into it --------
// The end-to-end proof: the shipped Content/scenes/main.arcscene resolves
// through the real .arcproj's bootScene Guid and loads into a real Runtime.
// Opened IN PLACE, not a copy -- Project::Open only ever writes an asset file
// back when it is missing/invalid a native "id" (AssetRegistry.cpp's
// ResolveNativeId), and both ReferenceProject content files already carry one
// (main.arcscene's id was stamped by Scene::SaveSceneFile when it was
// generated), so this is a read-only pass over the real repo tree -- no
// mutation risk to source-controlled fixtures from running the test suite.

TEST_CASE("ReferenceProject opens into its authored boot scene end to end", "[host][project]")
{
    const fs::path dir = FindReferenceProjectDir();
    REQUIRE_FALSE(dir.empty());   // if this fails, FindReferenceProjectDir's search bound needs raising

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE_FALSE(proj->Manifest().bootScene.empty());

    const fs::path sceneFile = Arcane::HostBoot::BootSceneFile(*proj);
    REQUIRE_FALSE(sceneFile.empty());
    CHECK(sceneFile.filename() == "main.arcscene");

    Arcane::Runtime runtime(Arcane::Test::Process());
    const auto result = Arcane::HostBoot::BootScene(runtime, *proj);
    REQUIRE(result.has_value());
    CHECK(result->id.ToString() == proj->Manifest().bootScene);

    const Arcane::SceneRoot* sceneRoot = runtime.Registry().GetResource<Arcane::SceneRoot>();
    REQUIRE(sceneRoot != nullptr);

    const auto children = runtime.Registry().GetChildren(sceneRoot->entity);
    // Task 11 (F2a): the reference scene grows a mesh -- a MeshCube entity
    // (Arcane::MeshRenderer) and a PerspectiveCamera entity (Arcane::Camera
    // with projection == Perspective), both children of the scene root
    // alongside the four pre-existing sprites. 4 -> 6, deliberately: see the
    // brief's own warning that this count and the name list below WOULD
    // break, and that breaking them is the point (the same "make the format
    // break observable" spirit the pose assertions below already apply to
    // Ground/BoxA/BoxB).
    //
    // F2c Plan 2, Task 11: 6 -> 7. GoldenProp (the imported multi-section
    // prop) is the second MeshRenderer this scene carries -- see this same
    // discipline applied below.
    REQUIRE(children.size() == 7);

    std::vector<std::string> names;
    Astra::Entity pulseBox{};
    bool foundPulseBox = false;
    std::map<std::string, Astra::Entity> byName;
    for (Astra::Entity child : children)
    {
        const Arcane::Identity* info = runtime.Registry().GetComponent<Arcane::Identity>(child);
        REQUIRE(info != nullptr);
        if (info->name == "PulseBox")
        {
            pulseBox = child;
            foundPulseBox = true;
        }
        byName[info->name] = child;
        names.push_back(info->name);
    }
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"BoxA", "BoxB", "GoldenProp", "Ground", "MeshCube",
                                             "PerspectiveCamera", "PulseBox"});

    // THE POSE ASSERTION (Task 3, F1). Everything above this line asserts
    // NAMES and GUIDS, and that is exactly why the 2D->3D format change slipped
    // past it: a stale scene still produced four correctly-named children with
    // resolvable materials, while every Transform in the file silently read as
    // absent and defaulted to the origin at unit scale. Nothing in the suite
    // said so. These three assertions are what make the next format break
    // observable instead of green -- one per field of Transform, so no single
    // one of them can go on passing while its neighbour rots:
    //
    //   * Ground's SCALE, non-uniform and not 1, so a defaulted vec3 fails it.
    //   * BoxA's POSITION, non-zero on both axes, so a defaulted vec3 fails it.
    //   * BoxB's ROTATION, a real ~0.35 rad turn about +Z, so a defaulted
    //     identity quaternion fails it -- and it is the only assertion in the
    //     suite that reads an authored quaternion back out of a FILE, which is
    //     the path the norm guard and the arity guard both live on.
    {
        REQUIRE(byName.count("Ground") == 1);
        const Arcane::Transform* ground =
            runtime.Registry().GetComponent<Arcane::Transform>(byName["Ground"]);
        REQUIRE(ground != nullptr);
        CHECK(ground->scale.x == Catch::Approx(10.0f));
        CHECK(ground->scale.y == Catch::Approx(0.5f));
        CHECK(ground->scale.z == Catch::Approx(1.0f));

        REQUIRE(byName.count("BoxA") == 1);
        const Arcane::Transform* boxA =
            runtime.Registry().GetComponent<Arcane::Transform>(byName["BoxA"]);
        REQUIRE(boxA != nullptr);
        // Y and the Z rotation MIRRORED by the +Y flip (F4 plan 1 T2): the
        // scene file was re-saved at v6 with every Y negated, so the authored
        // values these pin are the mirror image of the +Y-down ones. The
        // magnitudes are unchanged, which is the point -- a defaulted vec3
        // still fails them.
        CHECK(boxA->position.x == Catch::Approx(-1.0f));
        CHECK(boxA->position.y == Catch::Approx(0.5f));
        CHECK(boxA->position.z == Catch::Approx(0.0f));

        REQUIRE(byName.count("BoxB") == 1);
        const Arcane::Transform* boxB =
            runtime.Registry().GetComponent<Arcane::Transform>(byName["BoxB"]);
        REQUIRE(boxB != nullptr);
        CHECK(Arcane::RotationZ(boxB->rotation) == Catch::Approx(-0.35f).margin(1e-4));

        // GoldenProp's ORIENTATION AS AUTHORED UNDER +Y UP (F4 plan 1 T12,
        // Ruling O). golden_prop.glb is three SINGLE-SIDED quads in the XZ
        // plane; the prop faces the +Z camera only when rotated +90 degrees
        // about X (local +Y -> world +Z). The v5 file authored exactly that,
        // and the +Y migration -- correctly -- conjugated it to -90 degrees
        // (a reflection across XZ negates a rotation about X), which turned
        // the prop's back to the camera and the mesh pass culled it out of
        // every golden. The FIXTURE was re-authored, not the migration: this
        // pin states the intent (+90 about X, visible bottom-right at world
        // (2.5, -1.6)) so no future migration can silently re-hide it. The
        // quaternion is read as its image of local +Y, the axis whose facing
        // decides visibility.
        REQUIRE(byName.count("GoldenProp") == 1);
        const Arcane::Transform* goldenProp =
            runtime.Registry().GetComponent<Arcane::Transform>(byName["GoldenProp"]);
        REQUIRE(goldenProp != nullptr);
        {
            const glm::vec3 localUp = goldenProp->rotation * glm::vec3(0.0f, 1.0f, 0.0f);
            CHECK(localUp.x == Catch::Approx(0.0f).margin(1e-4));
            CHECK(localUp.y == Catch::Approx(0.0f).margin(1e-4));
            CHECK(localUp.z == Catch::Approx(1.0f).margin(1e-4));   // faces the +Z camera
            CHECK(goldenProp->position.x == Catch::Approx(2.5f));
            CHECK(goldenProp->position.y == Catch::Approx(-1.6f));   // mirrored by the +Y flip (F4 plan 1 T2)
        }
    }

    // Task 11 (F2a): the reference scene's new mesh content. Same discipline
    // as the pose block above -- one assertion per field that a dropped or
    // defaulted component would silently zero out -- plus the two Guid
    // references (MeshRenderer::mesh, and the .arcmesh's own default
    // material) resolved through the real asset registry, mirroring the
    // sprite/post material resolution immediately below applied to the new
    // asset kind.
    {
        REQUIRE(byName.count("MeshCube") == 1);
        const Astra::Entity meshCube = byName["MeshCube"];

        const Arcane::Transform* meshCubeTransform =
            runtime.Registry().GetComponent<Arcane::Transform>(meshCube);
        REQUIRE(meshCubeTransform != nullptr);
        CHECK(meshCubeTransform->position.x == Catch::Approx(0.4f));
        CHECK(meshCubeTransform->position.y == Catch::Approx(-0.3f));   // mirrored by the +Y flip (F4 plan 1 T2)
        CHECK(meshCubeTransform->position.z == Catch::Approx(0.0f));

        const Arcane::MeshRenderer* meshRenderer =
            runtime.Registry().GetComponent<Arcane::MeshRenderer>(meshCube);
        REQUIRE(meshRenderer != nullptr);
        REQUIRE(meshRenderer->mesh.IsValid());
        // The asset's own default material is what should win here -- an
        // authored override would defeat the "mesh asset carries a default
        // material" path this fixture means to exercise.
        CHECK_FALSE(meshRenderer->materialOverride.IsValid());

        const auto meshAssetPath = proj->ResolveAsset(Arcane::AssetId::FromGuid(meshRenderer->mesh));
        REQUIRE(meshAssetPath.has_value());
        CHECK(meshAssetPath->filename() == "reference_cube.arcmesh");

        // Follow the .arcmesh's OWN material reference too -- the same
        // "resolve the referenced asset for real" rigor the sprite/post
        // materials get below, applied to this new asset kind.
        //
        // F2c Task 10: reference_cube.arcmesh on disk still carries the F2a
        // scalar "material" key (deliberately NOT rewritten -- see this
        // task's report), and NO "slots" key. This assertion is therefore no
        // longer just a boot check -- it is the tolerant legacy-key mapping's
        // END-TO-END PROOF: LoadMeshAsset's fallback path is what turns that
        // scalar into slots[0] here, against a real file on disk rather than
        // a hand-written test fixture.
        const auto meshData = Arcane::LoadMeshAsset(*meshAssetPath);
        REQUIRE(meshData.has_value());
        CHECK(meshData->source == Arcane::MeshSource::Cube);
        REQUIRE(meshData->slots.size() == 1u);
        REQUIRE(meshData->slots[0].material.IsValid());
        const auto meshMatPath =
            proj->ResolveAsset(Arcane::AssetId::FromGuid(meshData->slots[0].material));
        REQUIRE(meshMatPath.has_value());
        CHECK(meshMatPath->filename() == "reference_mesh.arcmat");

        REQUIRE(byName.count("PerspectiveCamera") == 1);
        const Astra::Entity perspectiveCamera = byName["PerspectiveCamera"];

        const Arcane::Transform* camTransform =
            runtime.Registry().GetComponent<Arcane::Transform>(perspectiveCamera);
        REQUIRE(camTransform != nullptr);
        CHECK(camTransform->position.z == Catch::Approx(5.0f));

        const Arcane::Camera* cam =
            runtime.Registry().GetComponent<Arcane::Camera>(perspectiveCamera);
        REQUIRE(cam != nullptr);
        CHECK(cam->active);
        // `projection` alone already has teeth -- Orthographic is the struct
        // default -- and the three lens fields below are each authored away
        // from THEIR OWN defaults (60 / 0.1 / 1000) so a dropped field fails
        // loud instead of silently reading back as a value nobody would
        // notice was wrong.
        CHECK(cam->projection == Arcane::CameraProjection::Perspective);
        CHECK(cam->fovYDegrees == Catch::Approx(50.0f));
        CHECK(cam->nearZ == Catch::Approx(0.05f));
        CHECK(cam->farZ == Catch::Approx(500.0f));
    }

    // ReferenceProject's scene is the engine's canonical render fixture, so
    // the two seams it carries are pinned here -- a sprite with a REGISTERED
    // sprite material (Batcher2D's per-material CB + binding-set path) and a
    // scene PostProcess assignment (the PostChainCache path). Both are Guid
    // references into this project's asset registry: a renamed/moved .arcmat
    // resolves to nothing and the seam silently stops being exercised, while
    // everything that runs over the fixture goes on passing and covering less
    // than it claims. Resolving them here is what makes that loud. (`.arcmat`
    // is a NATIVE asset -- the Guid lives in the file's own "id" -- so this is
    // the same resolution SceneRenderResolver performs.)
    REQUIRE(foundPulseBox);
    const Arcane::SpriteRenderer* pulse =
        runtime.Registry().GetComponent<Arcane::SpriteRenderer>(pulseBox);
    REQUIRE(pulse != nullptr);
    REQUIRE(pulse->material.IsValid());
    const auto spriteMatPath = proj->ResolveAsset(Arcane::AssetId::FromGuid(pulse->material));
    REQUIRE(spriteMatPath.has_value());
    CHECK(spriteMatPath->filename() == "pulse_sprite.arcmat");

    const Arcane::PostProcess* post =
        runtime.Registry().GetComponent<Arcane::PostProcess>(sceneRoot->entity);
    REQUIRE(post != nullptr);
    REQUIRE(post->material.IsValid());
    const auto postMatPath = proj->ResolveAsset(Arcane::AssetId::FromGuid(post->material));
    REQUIRE(postMatPath.has_value());
    CHECK(postMatPath->filename() == "reference_post.arcmat");
}

// --- the material census -------------------------------------------------
// SceneRenderResolver::Materials() reports which materials a scene DECLARES and
// which of them are BOUND -- the fact a caller waiting on a fully-rendered
// frame polls. The gate cannot exercise the binding half (that needs a device
// and a compile service) -- but the REFERENCED half is pure scene data, and it
// is the half that decays silently: drop the PostProcess component or repoint
// the sprite's material Guid and everything that runs over this fixture keeps
// passing while quietly covering less. Pinning it here means such an edit fails
// the gate instead.

TEST_CASE("ReferenceProject's scene census reports the sprite and post materials as referenced but unbound",
          "[host][project]")
{
    const fs::path dir = FindReferenceProjectDir();
    REQUIRE_FALSE(dir.empty());
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    // ClientRuntime: SceneRenderResolver::Services::runtime is a ClientRuntime*
    // since the Core-DLL split (the four Set*Table publishers are presentation).
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    REQUIRE(Arcane::HostBoot::BootScene(runtime.Core(), *proj).has_value());

    // Nested scope: the resolver's header contract is that it destructs BEFORE
    // the Runtime it publishes non-owning table pointers through.
    {
        // Deliberately device-less -- no batcher, no device, no compile service.
        // Those disable BINDING (Refresh's `materialsReady`), which is part of
        // what lets this case assert referenced-without-bound.
        //
        // THE OTHER PART, unstated until F2a's final review and worth naming
        // because it is stronger: this fixture calls Project::Open + BootScene
        // and never Runtime::OpenProject, so runtime.CurrentProject() is NULL
        // and the resolver's `resolveAsset` closure returns nullopt for every
        // Guid. NOTHING can resolve here, compiler or not. That is why the
        // mesh half below reads unbound too even though its caches are
        // ungated on the compiler entirely -- and why the case that proves the
        // caches genuinely consume this project's content has to open the
        // project properly (see the next case).
        Arcane::SceneRenderResolver::Services services;
        services.runtime = &runtime;
        Arcane::SceneRenderResolver resolver(std::move(services));

        // The census reads the LIVE scene, so it answers the same before and
        // after a sweep -- only the BOUND side can ever move. The warm-up polls
        // this around Refresh calls, so an answer that depended on sweep order
        // would be a trap (and was: the first draft of this case latched the
        // post half in Refresh and disagreed with itself here).
        const auto cold = resolver.Materials();
        CHECK(cold.spriteReferenced == 1);
        CHECK(cold.postReferenced);
        CHECK(cold.spriteBound == 0);
        CHECK_FALSE(cold.postBound);
        // The mesh half obeys the same cold/warm rule, and it is the half that
        // makes the rule visible: `meshReferenced` is pure scene data (a
        // MeshRenderer carrying a valid `mesh` Guid), so it already answers 1
        // before any sweep has run.
        //
        // F2c Plan 2, Task 11: 1 -> 2 -- GoldenProp is a second MeshRenderer
        // (MeshCube's sibling), counted here the same way.
        CHECK(cold.meshReferenced == 2);
        CHECK(cold.meshBound == 0);

        Arcane::SceneRenderResolver::FrameInfo frame;
        frame.viewportWidth = 1280.0f;
        frame.viewportHeight = 720.0f;
        resolver.Refresh(frame);

        const auto census = resolver.Materials();
        // PulseBox is the one sprite carrying a material; Ground/BoxA/BoxB are
        // deliberately plain, so this also pins that the enrichment did not
        // quietly spread to the pre-existing content.
        CHECK(census.spriteReferenced == 1);
        CHECK(census.postReferenced);            // the scene root's PostProcess
        // Nothing can bind: no compiler, no batcher and -- see the Services
        // note above -- no project open on the Runtime at all. Referenced, and
        // provably not yet bound, which is the state this case wants.
        CHECK(census.spriteBound == 0);
        CHECK_FALSE(census.postBound);
        CHECK(census.meshReferenced == 2);
        CHECK(census.meshBound == 0);
    }
}

// --- the reference scene's mesh content, THROUGH the render caches ---------
// THE ONLY DEVICE-LESS PROOF THAT THE RENDER CACHES CONSUME THIS PROJECT'S
// CONTENT, rather than merely that its scene data parses. Task 11's
// assertions read the FILES (ids, component fields, LoadMeshAsset); the
// census case above reads the scene with no project open on the Runtime, so
// nothing there resolves. This one opens the project the way a real host
// does -- Runtime::OpenProject, which is what scans Content into the
// AssetRegistry and installs the resolver every cache resolves Guids through
// -- and then follows the whole chain the viewport follows: MeshRenderer::mesh
// -> MeshCache -> MeshEntry::material -> MeshMaterialCache -> a baseColor.
//
// Every link fails SILENTLY at the desk. An unresolvable mesh draws nothing;
// a mis-kinded, missing or renamed-param material draws WHITE -- which is also
// what a broken reference looks like. Task 6's find is the exact shape of
// regression this catches: `.arcmesh` was missing from AssetRegistry::AddFile's
// native-extension whitelist, making every mesh in every project unresolvable,
// with no warning at all.
TEST_CASE("ReferenceProject's mesh and its default material resolve into the render tables",
          "[host][project][mesh]")
{
    const fs::path dir = FindReferenceProjectDir();
    REQUIRE_FALSE(dir.empty());

    // ClientRuntime: SceneRenderResolver::Services::runtime is a ClientRuntime*
    // since the Core-DLL split (the four Set*Table publishers are presentation).
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    // Runtime::OpenProject, NOT Project::Open: only this path scans Content
    // into the AssetRegistry and installs the resolver behind
    // Runtime::CurrentProject(), which is what SceneRenderResolver's one
    // `resolveAsset` closure reads. With a bare Project::Open every Guid
    // resolves to nullopt (see the census case's Services note).
    REQUIRE(runtime.OpenProject(dir));
    const Arcane::Project* proj = runtime.CurrentProject();
    REQUIRE(proj != nullptr);
    REQUIRE(Arcane::HostBoot::BootScene(runtime.Core(), *proj).has_value());

    // Nested scope: the resolver publishes NON-OWNING pointers through the
    // Runtime and must destruct first (its own header contract).
    {
        Arcane::SceneRenderResolver::Services services;
        services.runtime = &runtime;
        Arcane::SceneRenderResolver resolver(std::move(services));

        Arcane::SceneRenderResolver::FrameInfo frame;
        frame.viewportWidth  = 1280.0f;
        frame.viewportHeight = 720.0f;
        // ONE Refresh, deliberately: the mesh's default material is only
        // knowable AFTER MeshCache resolves the mesh, and the sweep is
        // required to do both within the SAME call (SceneRenderResolver.cpp's
        // "ORDERING IS LOAD-BEARING"). A second Refresh here would hide a
        // regression that made the material one frame late.
        resolver.Refresh(frame);

        // The census, now against a project that can actually resolve: this is
        // the referenced==bound answer the device-less gate could never give
        // before, and it is available at all only because neither mesh cache
        // compiles anything (no device or compile service is needed).
        const auto census = resolver.Materials();
        // F2c Plan 2, Task 11: 1 -> 2 (both MeshCube and the golden-scene's
        // imported GoldenProp resolve -- golden_prop.arcmesh's cook artifact
        // is staged by ArcaneTests' own postbuild arccook step, same as
        // reference_cube's generated geometry needs no cook at all).
        CHECK(census.meshReferenced == 2);
        CHECK(census.meshBound == 2);

        // The MeshRenderer THIS CASE follows end to end is specifically
        // MeshCube's -- Task 11 (Plan 2) adds a second MeshRenderer
        // (GoldenProp) to the scene, so a bare "the one MeshRenderer" scan no
        // longer identifies a single entity; find MeshCube by name instead,
        // exactly as the boot-scene case above does.
        Astra::Entity meshCube = Astra::Entity::Invalid();
        int meshRenderers = 0;
        runtime.Registry().CreateView<Arcane::MeshRenderer>().ForEach(
            [&](Astra::Entity e, Arcane::MeshRenderer&)
        {
            ++meshRenderers;
            const Arcane::Identity* info = runtime.Registry().GetComponent<Arcane::Identity>(e);
            if (info && info->name == "MeshCube")
                meshCube = e;
        });
        REQUIRE(meshRenderers == 2);
        REQUIRE(meshCube != Astra::Entity::Invalid());

        const Arcane::MeshRenderer* meshCubeRenderer =
            runtime.Registry().GetComponent<Arcane::MeshRenderer>(meshCube);
        REQUIRE(meshCubeRenderer != nullptr);
        // Nil by design: the cube's colour comes from the MESH's own
        // default material, which is the link this case exists to follow.
        CHECK_FALSE(meshCubeRenderer->materialOverride.IsValid());
        const Arcane::Guid meshGuid = meshCubeRenderer->mesh;
        REQUIRE(meshGuid.IsValid());

        const Arcane::MeshTable* meshes = runtime.Registry().GetResource<Arcane::MeshTable>();
        REQUIRE(meshes != nullptr);
        const Arcane::MeshEntry* entry = meshes->Resolve(meshGuid);
        REQUIRE(entry != nullptr);
        // Real geometry, and a unit cube's: the generators emit UNIT shapes,
        // so size is the Transform's business and these bounds are fixed.
        CHECK_FALSE(entry->data.vertices.empty());
        CHECK(entry->bounds.min == glm::vec3(-0.5f, -0.5f, -0.5f));
        CHECK(entry->bounds.max == glm::vec3(0.5f, 0.5f, 0.5f));
        // F2c Task 10: the F2a scalar `material` retired into `slots[]`, so this
        // is reference_cube.arcmesh's legacy "material" key, mapped by
        // LoadMeshAsset's tolerant fallback into ONE unnamed slot.
        REQUIRE(entry->slots.size() == 1u);
        REQUIRE(entry->slots[0].material.IsValid());

        const Arcane::MeshMaterialTable* materials =
            runtime.Registry().GetResource<Arcane::MeshMaterialTable>();
        REQUIRE(materials != nullptr);
        const Arcane::ResolvedMeshMaterial* resolved = materials->Resolve(entry->slots[0].material);
        REQUIRE(resolved != nullptr);
        // The AUTHORED colour, not ResolvedMeshMaterial's (1,1,1,1) default --
        // so a mis-kinded .arcmat (MeshMaterialCache's kind gate) or a renamed
        // param fails HERE rather than rendering white at the desk.
        CHECK(resolved->baseColor != glm::vec4(1.0f));
    }
}

// --- the golden scene's imported prop (F2c Plan 2, Task 11) --------------
// The END-TO-END pin under the golden lane: golden_prop.arcmesh loads, names
// an importedSource, resolves through the artifact arccook produced during
// ArcaneTests' own postbuild staging, and comes back with THREE sections over
// TWO slots (multi.glb's A1 shape -- two "Metal" sections share slot 0, one
// "Paint" section is slot 1). A regression anywhere in Plan 1's cook chain,
// or in ResolveMeshData's Imported arm, surfaces here as a specific failure
// rather than as an unexplained golden diff two steps later.
TEST_CASE("host boot: the golden scene's imported prop resolves to sectioned geometry",
          "[host]")
{
    const fs::path dir = FindReferenceProjectDir();
    REQUIRE_FALSE(dir.empty());

    // Runtime::OpenProject, not a bare Project::Open: MeshArtifactFor below
    // resolves through the Assets facade's installed resolver, which only
    // exists once a project is actually opened on the Runtime (same
    // reasoning as the mesh/material render-table case above).
    Arcane::Runtime runtime(Arcane::Test::Process());
    REQUIRE(runtime.OpenProject(dir));
    const Arcane::Project* proj = runtime.CurrentProject();
    REQUIRE(proj != nullptr);
    REQUIRE(Arcane::HostBoot::BootScene(runtime, *proj).has_value());

    const Arcane::SceneRoot* sceneRoot = runtime.Registry().GetResource<Arcane::SceneRoot>();
    REQUIRE(sceneRoot != nullptr);

    Astra::Entity goldenProp = Astra::Entity::Invalid();
    bool foundGoldenProp = false;
    for (Astra::Entity child : runtime.Registry().GetChildren(sceneRoot->entity))
    {
        const Arcane::Identity* info = runtime.Registry().GetComponent<Arcane::Identity>(child);
        if (info && info->name == "GoldenProp")
        {
            goldenProp = child;
            foundGoldenProp = true;
            break;
        }
    }
    REQUIRE(foundGoldenProp);

    const Arcane::MeshRenderer* meshRenderer =
        runtime.Registry().GetComponent<Arcane::MeshRenderer>(goldenProp);
    REQUIRE(meshRenderer != nullptr);
    REQUIRE(meshRenderer->mesh.IsValid());

    const auto meshAssetPath = proj->ResolveAsset(Arcane::AssetId::FromGuid(meshRenderer->mesh));
    REQUIRE(meshAssetPath.has_value());
    CHECK(meshAssetPath->filename() == "golden_prop.arcmesh");

    const auto meshData = Arcane::LoadMeshAsset(*meshAssetPath);
    REQUIRE(meshData.has_value());
    REQUIRE(meshData->source == Arcane::MeshSource::Imported);
    REQUIRE(meshData->importedSource.IsValid());
    REQUIRE(meshData->slots.size() == 2u);

    // ResolveMeshData through the boot Assets facade -- production's own
    // wiring (MeshCache::Request forwards these same two closures from
    // Services), exercised here with no MeshCache in between so a broken
    // artifact resolution names itself precisely.
    Arcane::Assets& assets = runtime.AssetsFacade();
    const auto resolved = Arcane::ResolveMeshData(
        *meshData,
        [&assets](const Arcane::Guid& g) { return assets.MeshArtifactFor(g); },
        [&assets](const Arcane::Guid& g) { return assets.CookPending(g); });

    REQUIRE(resolved.state == Arcane::MeshResolveState::Ready);
    REQUIRE(resolved.mesh.has_value());
    REQUIRE(resolved.mesh->sections.size() == 3u);
    CHECK(resolved.mesh->sections[0].slotIndex == resolved.mesh->sections[1].slotIndex);
    CHECK(resolved.mesh->sections[2].slotIndex != resolved.mesh->sections[0].slotIndex);
}

// --- the reference scene's material content ------------------------------
// The two .arcmat assets the scene references are AUTHORED CONTENT, and nothing
// else in the device-less suite would notice a hand-edit that broke them: a
// mistyped //@param or a snippet that no longer compiles surfaces as an
// unshaded quad / a missing post chain at the desk, one GPU round trip later.
// This case stitches both through the real engine templates and runs DXC over
// every stitched source -- CPU only, no device, so it belongs to the ~[gpu]
// gate.

TEST_CASE("ReferenceProject's materials stitch and compile on both targets",
          "[host][project][shadercompile]")
{
    const fs::path dir = FindReferenceProjectDir();
    REQUIRE_FALSE(dir.empty());
    const fs::path materials = dir / "Content" / "materials";

    Arcane::ShaderSourceProvider provider;
    provider.AddRoot("data/shaders");

    Arcane::ShaderCompiler sc;
    REQUIRE(sc.InitializeWithDebounce(0.0));

    // Both stages, both targets -- the sprite/post caches submit exactly this
    // pair per pass, and a source that compiles as DXIL but not SPIR-V binds on
    // one backend only -- which reads as a vulkan-specific renderer bug and is
    // not one.
    auto compileBoth = [&sc](const std::string& name, const std::string& hlsl)
    {
        for (const char* entry : { Arcane::kPsEntry, Arcane::kVsEntry })
        {
            Arcane::ShaderCompileRequest req;
            req.debugName = name;
            req.sourceUtf8 = hlsl;
            req.entry = entry;
            req.profile = entry == Arcane::kPsEntry ? Arcane::kPsProfile : Arcane::kVsProfile;
            const Arcane::ShaderCompileResult r = sc.CompileNow(req);
            for (const Arcane::ShaderDiag& d : r.dxil.diags)
                INFO("dxil " << name << " " << entry << " line " << d.line << ": " << d.message);
            for (const Arcane::ShaderDiag& d : r.spirv.diags)
                INFO("spirv " << name << " " << entry << " line " << d.line << ": " << d.message);
            CHECK(r.AllSucceeded());
        }
    };

    SECTION("the sprite material stitches the SPRITE surface with a CB and a texture")
    {
        const auto data = Arcane::LoadMaterialAsset(materials / "pulse_sprite.arcmat");
        REQUIRE(data.has_value());
        CHECK(Arcane::MaterialSurfaceForKind(data->kind) == Arcane::MaterialSurface::Sprite);

        const auto templateText =
            provider.Get(Arcane::MaterialTemplateFile(Arcane::MaterialSurface::Sprite));
        REQUIRE(templateText.has_value());

        const Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
            *templateText, data->snippet, data->name, Arcane::MaterialSurface::Sprite);
        REQUIRE(build.errors.empty());

        // WHY the scene needs this material at all: numeric params are what make
        // Batcher2D build the per-material volatile CB at b1, and a declared
        // texture is what puts the extra Texture_SRV(t1) into both the binding
        // layout and the per-(texture, material) binding-set cache. A snippet
        // that lost either decl would still compile and still draw -- and would
        // stop covering the seam this fixture exists to exercise.
        CHECK(build.templ.CbSize() > 0);
        CHECK(build.templ.TextureCount() == 1);

        // Every saved value must land. A name that no longer matches a decl (or
        // a type that changed under it) drops SILENTLY at bind, and on this
        // material that means the sprite quietly renders its identity defaults.
        Arcane::MaterialInstance instance(
            std::make_shared<const Arcane::MaterialTemplate>(build.templ));
        CHECK(Arcane::ApplyMaterialParams(*data, instance) == data->params.size());

        compileBoth("pulse_sprite.hlsl", build.hlsl);
    }

    SECTION("the post material stitches a two-pass chain that reads the scene")
    {
        const auto data = Arcane::LoadMaterialAsset(materials / "reference_post.arcmat");
        REQUIRE(data.has_value());
        CHECK(Arcane::MaterialSurfaceForKind(data->kind) == Arcane::MaterialSurface::Fullscreen);
        // Pass 0 reads the EXTERNAL scene colour; pass 1 reads pass 0. That
        // exact shape -- two passes, an intermediate, a scene input -- is the
        // post seam the cutover has to reproduce; a chain collapsed to one pass
        // would still render and would prove strictly less.
        REQUIRE((data->baseInputs == std::vector<std::uint32_t>{ Arcane::kSceneInput }));
        REQUIRE(data->passes.size() == 1);   // + the base snippet = 2 chain passes
        CHECK((data->passes[0].inputs == std::vector<std::uint32_t>{ 0u }));

        const auto templateText =
            provider.Get(Arcane::MaterialTemplateFile(Arcane::MaterialSurface::Fullscreen));
        REQUIRE(templateText.has_value());

        // Built exactly as PostChainCache builds it: base snippet first, POST
        // mode on (which is what makes the kSceneInput wire legal).
        std::vector<Arcane::MaterialChainPassDesc> descs;
        descs.push_back({ data->snippet, data->baseInputs });
        for (const Arcane::MaterialPass& p : data->passes)
            descs.push_back({ p.snippet, p.inputs });

        const Arcane::MaterialChainBuildResult build = Arcane::BuildMaterialChainSource(
            *templateText, descs, data->name, data->vertexSnippet, /*externalInput=*/true);
        REQUIRE(build.Ok());
        REQUIRE(build.hlsl.size() == 2);
        CHECK(build.templ.CbSize() > 0);   // the ONE merged CB both passes share

        Arcane::MaterialInstance instance(
            std::make_shared<const Arcane::MaterialTemplate>(build.templ));
        CHECK(Arcane::ApplyMaterialParams(*data, instance) == data->params.size());

        for (std::size_t i = 0; i < build.hlsl.size(); ++i)
            compileBoth("reference_post.p" + std::to_string(i) + ".hlsl", build.hlsl[i]);
    }

    sc.Shutdown();
}
