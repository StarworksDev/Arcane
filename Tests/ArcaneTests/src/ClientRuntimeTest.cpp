// The Runtime/ClientRuntime split (spec 2026-09-15 s2): a headless Runtime has NO
// presentation -- no client, no hooks, an EMPTY render scheduler; a ClientRuntime
// owns one and attaches itself as the client hooks. RenderSubmissionSystem is no
// longer installed on either (F5 plan 1 Task 5): sprites are submitted once per
// frame by ExtractFrame, and that stays true across ClearSystems (which
// PluginHost -- now Core -- calls on every module unload/reload).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Input/LocalInputUser.hpp>
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Plugin/ClientHooks.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // EngineContext (the hooks' fill target)
#include <Arcane/Render/RenderSystems.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <filesystem>
#include <fstream>

#include "Helpers/TestTypeContext.hpp"

TEST_CASE("a headless Runtime carries no client and installs only the two headless engine systems", "[runtime][client]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    CHECK(rt.Client() == nullptr);
    CHECK(rt.ClientHooks() == nullptr);
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>());
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    CHECK_FALSE(rt.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
    rt.ClearSystems();
    CHECK_FALSE(rt.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
}

TEST_CASE("ClientRuntime owns a Runtime, attaches as its client, and leaves render submission uninstalled", "[runtime][client]")
{
    Arcane::ClientRuntime crt(Arcane::Test::Process());
    Arcane::Runtime& core = crt.Core();
    CHECK(core.Client() == &crt);
    CHECK(core.ClientHooks() != nullptr);
    CHECK_FALSE(core.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
    core.ClearSystems();                                   // the hook path PluginHost takes
    CHECK_FALSE(core.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
    CHECK(core.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>());
    // the aliases (P5) are the same objects
    CHECK(&crt.Registry() == &core.Registry());
    CHECK(&crt.Loop()     == &core.Loop());
}

TEST_CASE("ClientRuntime's ImGui handoff reaches an EngineContext only through the hooks", "[runtime][client]")
{
    Arcane::ClientRuntime crt(Arcane::Test::Process());
    int a = 0, b = 0, c = 0, d = 0;
    crt.SetImGui(&a, &b, &c, &d);
    Arcane::EngineContext ctx{};
    crt.Core().ClientHooks()->FillEngineContext(ctx);
    CHECK(ctx.imguiContext == &a); CHECK(ctx.imguiAlloc == &b); CHECK(ctx.imguiFree == &c); CHECK(ctx.imguiUserData == &d);
    Arcane::Runtime bare(Arcane::Test::Process());
    CHECK(bare.ClientHooks() == nullptr);                  // a headless host hands the module null ImGui, as before
}

namespace
{
    Arcane::InputActionAsset RuntimeInputAsset()
    {
        return *Arcane::InputActionAsset::FromJson(nlohmann::json::parse(R"JSON({
            "version":1,"id":"11111111-1111-4111-8111-111111111111",
            "defaultMap":"22222222-2222-4222-8222-222222222222","controlSchemes":[],
            "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
              {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[
                {"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space"}]}
            ]}]
        })JSON"));
    }

    // Three revisions of one asset, as the editor's Save republishes them:
    // A = the session's original (Jump = Space, maps gameplay + menu),
    // B = Jump re-authored to K, C = B with the "menu" map removed.
    Arcane::InputActionAsset SessionAsset(const char* jumpPath, bool withMenu)
    {
        nlohmann::json doc = nlohmann::json::parse(R"JSON({
            "version":1,"id":"11111111-1111-4111-8111-111111111111",
            "defaultMap":"22222222-2222-4222-8222-222222222222",
            "controlSchemes":[{"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","name":"Gamepad","bindingGroup":"Gamepad"}],
            "actionMaps":[
              {"id":"22222222-2222-4222-8222-222222222222","name":"gameplay","actions":[
                {"id":"44444444-4444-4444-8444-444444444444","name":"Jump","type":"Button","bindings":[
                  {"id":"55555555-5555-4555-8555-555555555555","path":"<Keyboard>/space"}]}]},
              {"id":"33333333-3333-4333-8333-333333333333","name":"menu","actions":[
                {"id":"66666666-6666-4666-8666-666666666666","name":"Back","type":"Button","bindings":[
                  {"id":"77777777-7777-4777-8777-777777777777","path":"<Keyboard>/escape"}]}]}
            ]})JSON");
        doc["actionMaps"][0]["actions"][0]["bindings"][0]["path"] = jumpPath;
        if (!withMenu) doc["actionMaps"].erase(1);
        return *Arcane::InputActionAsset::FromJson(doc);
    }
}

TEST_CASE("ClientRuntime keeps gameplay actions separate from the raw host snapshot", "[client][input]")
{
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    const auto project = *Arcane::Guid::FromString("55555555-5555-4555-8555-555555555555");
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), project));
    const auto jump = runtime.GameInput().FindAction("Player", "Jump");
    REQUIRE(jump);
    Arcane::InputSnapshot raw;
    raw.AddKeycode(32);
    runtime.SetInputSnapshot(raw);
    CHECK(runtime.Input().KeycodeDown(32));
    CHECK_FALSE(runtime.GameInput().Down(*jump));
    runtime.UpdateGameInput(1.0 / 60.0, raw);
    CHECK(runtime.GameInput().Down(*jump));
    CHECK(runtime.GameInput().Pressed(*jump));
    CHECK(runtime.GameInput().Started(*jump));
    CHECK(runtime.GameInput().Performed(*jump));
    CHECK(runtime.GameInput().ButtonDown(*jump) == true);
    runtime.BeginGameInputFixedStep();
    CHECK(runtime.GameInput().PressedThisFixedStep(*jump));
    runtime.BeginGameInputFixedStep();
    CHECK_FALSE(runtime.GameInput().PressedThisFixedStep(*jump));
    runtime.UpdateGameInput(1.0 / 60.0, {});
    CHECK(runtime.GameInput().Released(*jump));
}

TEST_CASE("ClientRuntime switches gameplay input between project identities", "[client][input]")
{
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    const auto a = *Arcane::Guid::FromString("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const auto b = *Arcane::Guid::FromString("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), a));
    const auto jump = runtime.GameInput().FindAction("Player", "Jump");
    REQUIRE(jump);
    const auto binding = *Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444");
    REQUIRE(runtime.GameInput().SetOverride(binding, "<Keyboard>/scancode/w"));
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), b));
    CHECK(runtime.GameInput().ProjectId() == b);
    CHECK(runtime.GameInput().Bindings(*jump)[0].effectivePath == "<Keyboard>/space");
}

TEST_CASE("project gameplay input boot reports unconfigured missing valid and invalid assets", "[client][input]")
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("arcane_gameplay_boot_" + Arcane::Guid::Generate().ToString());
    fs::create_directories(root / "Content" / "input");
    auto writeManifest = [&](const std::string& selection)
    {
        nlohmann::json manifest = {
            { "formatVersion", 2 }, { "name", "P" },
            { "engine", { { "abi", Arcane::kGamePluginABIVersion } } },
            { "inputActions", selection }
        };
        std::ofstream(root / "P.arcproj") << manifest.dump(2);
    };
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    writeManifest("");
    auto project = Arcane::Project::Open(root);
    REQUIRE(project);
    CHECK(Arcane::HostBoot::LoadGameplayInput(runtime, *project).status ==
          Arcane::HostBoot::GameplayInputLoadResult::Status::Unconfigured);
    writeManifest("99999999-9999-4999-8999-999999999999");
    project = Arcane::Project::Open(root);
    REQUIRE(project);
    CHECK(Arcane::HostBoot::LoadGameplayInput(runtime, *project).status ==
          Arcane::HostBoot::GameplayInputLoadResult::Status::Invalid);
    const auto asset = RuntimeInputAsset();
    std::ofstream(root / "Content" / "input" / "Player.arcinput") << asset.ToJson().dump(2);
    writeManifest(asset.id.ToString());
    project = Arcane::Project::Open(root);
    REQUIRE(project);
    CHECK(Arcane::HostBoot::LoadGameplayInput(runtime, *project).status ==
          Arcane::HostBoot::GameplayInputLoadResult::Status::Loaded);
    CHECK(runtime.GameInput().FindAction("Player", "Jump").has_value());
    std::ofstream(root / "Content" / "input" / "Player.arcinput") <<
        nlohmann::json{ { "id", asset.id.ToString() }, { "version", 99 } }.dump();
    project = Arcane::Project::Open(root);
    REQUIRE(project);
    CHECK(Arcane::HostBoot::LoadGameplayInput(runtime, *project).status ==
          Arcane::HostBoot::GameplayInputLoadResult::Status::Invalid);
    std::error_code error;
    fs::remove_all(root, error);
}

TEST_CASE("LocalInputUser re-Configure for the same project replays the map stack, scheme and dirty overrides", "[client][input]")
{
    // Fresh project ids: SDL's pref path may hold a Default.json from an earlier
    // run under a fixed id, and a loaded profile would mask the dirty one.
    const auto projectId      = Arcane::Guid::Generate();
    const auto otherProjectId = Arcane::Guid::Generate();
    const auto gameplayId     = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto menuId         = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    const auto jumpActionId   = *Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444");
    const auto jumpBindingId  = *Arcane::Guid::FromString("55555555-5555-4555-8555-555555555555");
    Arcane::LocalInputUser user;
    CHECK_FALSE(user.ActiveMap().has_value());
    REQUIRE(user.Configure(SessionAsset("<Keyboard>/space", true), projectId));
    CHECK(user.ActiveMap() == gameplayId);                          // the base map
    REQUIRE(user.PushMap(menuId));
    CHECK(user.ActiveMap() == menuId);
    REQUIRE(user.SetControlScheme("Gamepad"));
    CHECK(user.ControlScheme() == "Gamepad");
    REQUIRE(user.SetOverride(jumpBindingId, "<Keyboard>/j"));
    REQUIRE(user.ProfileDirty());
    REQUIRE(user.Configure(SessionAsset("<Keyboard>/k", true), projectId));     // the save's republish
    CHECK(user.ActiveMap() == menuId);                              // pushed map survived
    CHECK(user.ControlScheme() == "Gamepad");                       // the scheme was replayed
    REQUIRE(user.Bindings(jumpActionId).size() == 1);
    CHECK(user.Bindings(jumpActionId)[0].authoredPath == "<Keyboard>/k");   // the edit is live
    CHECK(user.Bindings(jumpActionId)[0].effectivePath == "<Keyboard>/j");  // the unsaved override still sits on it
    CHECK(user.ProfileDirty());                                     // unsaved override kept
    CHECK(user.BindingDisplayString(jumpBindingId) == "Keyboard J");
    REQUIRE(user.Configure(SessionAsset("<Keyboard>/k", false), projectId));    // "menu" removed
    CHECK(user.ActiveMap() == gameplayId);                          // collapsed to the surviving base
    CHECK(user.ControlScheme() == "Gamepad");                       // schemes unchanged: still replayed
    CHECK(user.ProfileDirty());
    REQUIRE(user.Configure(SessionAsset("<Keyboard>/k", false), otherProjectId)); // a different project: cold start
    CHECK(user.ControlScheme().empty());                            // a cold start carries no scheme
    CHECK_FALSE(user.ProfileDirty());
    CHECK(user.Bindings(jumpActionId)[0].effectivePath == "<Keyboard>/k");
    CHECK(user.ProjectId() == otherProjectId);
}

TEST_CASE("ClientRuntime: a re-Configure does not re-fire a control held across it", "[client][input]")
{
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    const auto project = Arcane::Guid::Generate();
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), project));
    const auto jump = runtime.GameInput().FindAction("Player", "Jump");
    REQUIRE(jump);
    Arcane::InputSnapshot raw; raw.AddKeycode(32);                  // space held
    runtime.UpdateGameInput(1.0 / 60.0, raw);
    CHECK(runtime.GameInput().Pressed(*jump));                      // the real press
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), project));
    runtime.UpdateGameInput(1.0 / 60.0, raw);                       // first live tick, still held
    CHECK(runtime.GameInput().Down(*jump));
    CHECK_FALSE(runtime.GameInput().Pressed(*jump));
    CHECK_FALSE(runtime.GameInput().Started(*jump));
    runtime.BeginGameInputFixedStep();
    CHECK_FALSE(runtime.GameInput().PressedThisFixedStep(*jump));
    runtime.UpdateGameInput(1.0 / 60.0, {});
    CHECK(runtime.GameInput().Released(*jump));                     // the held state was carried, not zeroed
}

TEST_CASE("LocalInputUser::Generation moves on every Configure and Clear, never on a failed Configure", "[client][input]")
{
    Arcane::LocalInputUser user;
    const auto project = *Arcane::Guid::FromString("66666666-6666-4666-8666-666666666666");
    const std::uint64_t g0 = user.Generation();

    REQUIRE(user.Configure(RuntimeInputAsset(), project));          // cold
    const std::uint64_t g1 = user.Generation();
    CHECK(g1 > g0);

    REQUIRE(user.Configure(RuntimeInputAsset(), project));          // same-project re-entry
    const std::uint64_t g2 = user.Generation();
    CHECK(g2 > g1);

    CHECK_FALSE(user.Configure(RuntimeInputAsset(), Arcane::Guid::Nil()));   // refused
    CHECK(user.Generation() == g2);

    user.Clear();
    CHECK(user.Generation() > g2);
}
