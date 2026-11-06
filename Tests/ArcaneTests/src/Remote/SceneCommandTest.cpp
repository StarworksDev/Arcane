#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/EcsCommands.hpp> // SetEcsArtifactDir: the session artifact directory
#include <Arcane/Remote/Commands/SceneCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Scene/Components.hpp>
#include "Scene/SelectionContext.hpp"
#include "Remote/SelectionCommand.hpp"
#include <Arcane/Remote/HostSession.hpp>   // StopEditorRemote's session (round 4, N3-3)
#include <memory>
#include <Astra/Registry/Registry.hpp>
#include <Servitor/Verbs.hpp>
#include "RemoteTestSupport.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

// The one-row world table every world-taking command reads (Task 19, ruling F16).
static std::vector<Arcane::Remote::WorldInfo> RemoteTestWorld(void* user) {
    Arcane::Remote::WorldInfo world;
    world.name = "main";
    world.role = "Standalone";
    world.registry = static_cast<Astra::Registry*>(user);
    return {world};
}

static Arcane::Editor::SelectionContext* g_sel = nullptr;
static std::size_t SelCount(void*) { return g_sel ? g_sel->Count() : 0; }
static Astra::Entity SelAt(std::size_t i, void*) { return g_sel->Entities()[i]; }
static Astra::Entity SelPrimary(void*) {
    return g_sel ? g_sel->Primary() : Astra::Entity::Invalid();
}

TEST_CASE("scene.tree roots are parentless and siblings sort by guid", "[remote][scene]") {
    Astra::Registry reg;
    const auto first = reg.CreateEntity();
    const auto second = reg.CreateEntity();
    const auto child = reg.CreateEntity();
    REQUIRE(reg.SetParent(child, second));
    Arcane::Identity a;
    a.id = Arcane::Guid{1, 9};
    a.name = "later";
    Arcane::Identity b;
    b.id = Arcane::Guid{1, 2};
    b.name = "earlier";
    Arcane::Identity c;
    c.id = Arcane::Guid{1, 3};
    c.name = "child";
    reg.AddComponent<Arcane::Identity>(first, a);
    reg.AddComponent<Arcane::Identity>(second, b);
    reg.AddComponent<Arcane::Identity>(child, c);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};   // one-row table (ruling F16)
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterSceneCommands();
    RemoteTest::ScopedHostKind editorKind(Arcane::Remote::HostKind::Editor);
    Arcane::Remote::Call call;
    call.command = "scene.tree";
    call.args = nlohmann::json::object();
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(env.ok);
    REQUIRE(env.data["roots"].size() == 2);
    CHECK(env.data["roots"][0]["name"] == "earlier");
    CHECK(env.data["roots"][1]["name"] == "later");
    REQUIRE(env.data["roots"][0]["children"].size() == 1);
    CHECK(env.data["roots"][0]["children"][0]["name"] == "child");
}

TEST_CASE("scene.tree on a server is command.host-kind", "[remote][scene]") {
    Astra::Registry reg;
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};   // one-row table (ruling F16)
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterSceneCommands();
    RemoteTest::ScopedHostKind serverKind(Arcane::Remote::HostKind::Server);   // ruling F8
    Arcane::Remote::Call call;
    call.command = "scene.tree";
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "command.host-kind");
    CHECK(Arcane::Remote::ExitFromHost(env) == 2);
}

TEST_CASE("selection.get keeps user order and a null store is permission.denied", "[remote][scene]") {
    Astra::Registry reg;
    const auto e1 = reg.CreateEntity();
    const auto e2 = reg.CreateEntity();
    Arcane::Identity a;
    a.id = Arcane::Guid{2, 1};
    a.name = "first";
    Arcane::Identity b;
    b.id = Arcane::Guid{2, 2};
    b.name = "second";
    reg.AddComponent<Arcane::Identity>(e1, a);
    reg.AddComponent<Arcane::Identity>(e2, b);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};   // one-row table (ruling F16)
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Editor::SelectionContext sel;
    sel.Select(e1);
    sel.Toggle(e2);
    g_sel = &sel;
    Arcane::Editor::SelectionView view;
    view.count = &SelCount;
    view.at = &SelAt;
    view.primary = &SelPrimary;
    // Cleared on scope exit even if a REQUIRE aborts (ruling S3-9).
    RemoteTest::ScopedRemotePointer<const Arcane::Editor::SelectionView> viewGuard(
        [](const Arcane::Editor::SelectionView* v) {
            Arcane::Editor::SetSelectionView(v ? *v : Arcane::Editor::SelectionView{});
        },
        &view);
    Arcane::Editor::RegisterSelectionCommand();
    Arcane::Remote::Call call;
    call.command = "editor.selection.get";
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(env.ok);
    REQUIRE(env.data["entities"].size() == 2);
    CHECK(env.data["entities"][0]["name"] == "first");
    CHECK(env.data["entities"][1]["name"] == "second");
    CHECK(env.data["primary"]["lo"] == 2);
    g_sel = nullptr;
    Arcane::Editor::SetSelectionView({});
    const auto denied = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(denied.ok);
    CHECK(denied.errors[0].code == "permission.denied");
    CHECK(Arcane::Remote::ExitFromHost(denied) == 3);
}

TEST_CASE("scene.tree --depth 0 is roots only and an entity without Identity sorts last", "[remote][scene]") {
    Astra::Registry reg;
    const auto anonymous = reg.CreateEntity();   // created first, still sorts last
    const auto named = reg.CreateEntity();
    const auto child = reg.CreateEntity();
    REQUIRE(reg.SetParent(child, named));
    Arcane::Identity n;
    n.id = Arcane::Guid{5, 5};
    n.name = "named";
    reg.AddComponent<Arcane::Identity>(named, n);
    Arcane::Identity c;
    c.id = Arcane::Guid{5, 6};
    c.name = "kid";
    reg.AddComponent<Arcane::Identity>(child, c);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterSceneCommands();
    RemoteTest::ScopedHostKind runtimeKind(Arcane::Remote::HostKind::Runtime);
    Arcane::Remote::Call call;
    call.command = "scene.tree";

    // The whole tree: the anonymous root is last, guid null, handle kept, one warning.
    auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(env.ok);
    REQUIRE(env.data["roots"].size() == 2);
    CHECK(env.data["roots"][0]["name"] == "named");
    CHECK(env.data["roots"][0]["ref"] == "guid:" + n.id.ToString());
    CHECK(env.data["roots"][0]["guid"]["hi"] == 5);
    CHECK(env.data["roots"][1]["guid"].is_null());
    CHECK(env.data["roots"][1]["handle"] == static_cast<std::uint64_t>(anonymous.GetValue()));
    REQUIRE(env.warnings.size() == 1);
    CHECK(env.warnings[0].code == "entity.no-identity");
    REQUIRE(env.data["roots"][0]["children"].size() == 1);

    // depth 0: roots only; the cut node says how many children it had.
    call.args = {{"depth", 0}};
    env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(env.ok);
    REQUIRE(env.data["roots"].size() == 2);
    CHECK(env.data["roots"][0]["children"].empty());
    CHECK(env.data["roots"][0]["childrenOmitted"] == 1);

    // depth -1 (the schema default) is the whole tree.
    call.args = {{"depth", -1}};
    env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(env.ok);
    REQUIRE(env.data["roots"][0]["children"].size() == 1);
    CHECK_FALSE(env.data["roots"][0].contains("childrenOmitted"));

    // Bad arguments are usage refusals, never a silent default.
    call.args = {{"depth", -2}};
    env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "arg.invalid");
    call.args = {{"deep", 1}};
    env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "arg.unknown");
    CHECK(Arcane::Remote::ExitFromHost(env) == 2);
}

// editor.selection.get on a runtime or server answers command.host-kind because
// those hosts never register it (ruling F-1); Task 4's never-registered-name
// case pins that registry rule, so it is not re-tested here (this process has
// registered it).
TEST_CASE("scene.tree and editor.selection.get are world-scoped Observe reads", "[remote][scene]") {
    Arcane::Remote::RegisterSceneCommands();
    Arcane::Editor::RegisterSelectionCommand();
    Arcane::Editor::RegisterSelectionCommand();   // safe to call twice
    const auto* tree = Arcane::Remote::CommandRegistry::Get().Find("scene.tree");
    const auto* selection = Arcane::Remote::CommandRegistry::Get().Find("editor.selection.get");
    REQUIRE(tree != nullptr);
    REQUIRE(selection != nullptr);
    CHECK(tree->worldScoped);
    CHECK(selection->worldScoped);
    CHECK(tree->access == Arcane::Remote::Access::Observe);
    CHECK(selection->access == Arcane::Remote::Access::Observe);
    CHECK(tree->mutates == Arcane::Remote::Mutates::None);
    CHECK(selection->mutates == Arcane::Remote::Mutates::None);
    CHECK_FALSE(tree->needsFrame);
    CHECK_FALSE(selection->needsFrame);
    CHECK(tree->module.empty());
    CHECK(selection->module.empty());
}

TEST_CASE("scene and selection verbs build their wire args", "[remote][scene][servitor]") {
    const Servitor::VerbRow* tree = Servitor::FindVerb("scene tree");
    const Servitor::VerbRow* selection = Servitor::FindVerb("selection");
    REQUIRE(tree != nullptr);
    REQUIRE(selection != nullptr);
    CHECK(std::string(tree->command) == "scene.tree");
    CHECK(std::string(selection->command) == "editor.selection.get");
    CHECK(selection->args == nullptr);   // Run refuses any token after `selection`
    nlohmann::json out;
    std::string error;
    std::vector<std::string> none;
    REQUIRE(tree->args(none, out, &error));
    CHECK(out == nlohmann::json::object());
    std::vector<std::string> depth{"--depth", "2"};
    REQUIRE(tree->args(depth, out, &error));
    CHECK(out == nlohmann::json{{"depth", 2}});
    for (const auto& bad : std::vector<std::vector<std::string>>{
             {"--depth", "x"}, {"--depth", "-1"}, {"--bogus"}, {"extra"}}) {
        error.clear();
        CHECK_FALSE(tree->args(bad, out, &error));
        CHECK_FALSE(error.empty());
    }
}

// --- Task 25 fix round 1: the bounded-output paths ---

static Arcane::Remote::Envelope InvokeTree(nlohmann::json args) {
    Arcane::Remote::Call call;
    call.command = "scene.tree";
    call.args = std::move(args);
    return Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
}

TEST_CASE("scene.tree over the inline cap writes one scene artifact", "[remote][scene]") {
    Astra::Registry reg;
    // 2000 named roots with 150-character names: ~0.5 MB of tree JSON.
    for (std::uint64_t i = 0; i < 2000; ++i) {
        const auto e = reg.CreateEntity();
        Arcane::Identity id;
        id.id = Arcane::Guid{7, i + 1};
        id.name = std::string(150, 'n') + std::to_string(i);
        reg.AddComponent<Arcane::Identity>(e, id);
    }
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterSceneCommands();
    RemoteTest::ScopedHostKind runtimeKind(Arcane::Remote::HostKind::Runtime);

    Arcane::Test::TempDir out("scene_artifact");
    Arcane::Remote::SetEcsArtifactDir(out.path);   // the session's <remote dir>/out, shared with ecs.query
    const auto big = InvokeTree(nlohmann::json::object());
    Arcane::Remote::ReleaseEcsArtifactDir(out.path);
    REQUIRE(big.ok);
    CHECK(big.data.is_null());
    REQUIRE(big.artifacts.size() == 1);
    CHECK(big.artifacts[0].kind == "scene");
    CHECK(big.artifacts[0].sha256.size() == 64);
    REQUIRE(big.warnings.size() == 1);
    CHECK(big.warnings[0].code == "scene.in-artifact");
    const std::filesystem::path file = big.artifacts[0].path;
    REQUIRE(std::filesystem::exists(file));
    CHECK(std::filesystem::file_size(file) == big.artifacts[0].bytes);
    CHECK(big.artifacts[0].bytes > 262144);
    std::ifstream in(file, std::ios::binary);
    const nlohmann::json written = nlohmann::json::parse(in);
    REQUIRE(written["roots"].size() == 2000);
    CHECK(written["roots"][0]["guid"]["lo"] == 1);   // sorted by guid

    // An artifact directory that cannot be written is an error, never ok with no tree.
    const std::filesystem::path notADir = out.path / "plain-file";
    { std::ofstream(notADir) << "x"; }
    Arcane::Remote::SetEcsArtifactDir(notADir);
    const auto failed = InvokeTree(nlohmann::json::object());
    Arcane::Remote::ReleaseEcsArtifactDir(notADir);
    REQUIRE_FALSE(failed.ok);
    CHECK(failed.errors[0].code == "scene.artifact-failed");
    CHECK(Arcane::Remote::ExitFromHost(failed) == 6);
    CHECK(failed.artifacts.empty());

    // With no artifact directory the tree is dropped, not inlined, and a warning says so.
    const auto noDir = InvokeTree(nlohmann::json::object());
    REQUIRE(noDir.ok);
    CHECK(noDir.data.is_null());
    CHECK(noDir.artifacts.empty());
    REQUIRE(noDir.warnings.size() == 1);
    CHECK(noDir.warnings[0].code == "scene.no-artifact-dir");
}

TEST_CASE("scene.tree cuts a chain deeper than the hard bound with scene.depth-limit", "[remote][scene]") {
    Astra::Registry reg;
    // A 100-level parent chain: the walk stops at level 64 whatever --depth says.
    std::vector<Astra::Entity> chain;
    for (int i = 0; i < 100; ++i) {
        const auto e = reg.CreateEntity();
        if (!chain.empty())
            REQUIRE(reg.SetParent(e, chain.back()));
        Arcane::Identity id;
        id.id = Arcane::Guid{9, static_cast<std::uint64_t>(i + 1)};
        id.name = "link" + std::to_string(i);
        reg.AddComponent<Arcane::Identity>(e, id);
        chain.push_back(e);
    }
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterSceneCommands();
    RemoteTest::ScopedHostKind runtimeKind(Arcane::Remote::HostKind::Runtime);

    const auto env = InvokeTree(nlohmann::json::object());
    REQUIRE(env.ok);
    REQUIRE(env.warnings.size() == 1);
    CHECK(env.warnings[0].code == "scene.depth-limit");
    CHECK(env.warnings[0].detail["limit"] == 64);
    // Walk down to the cut node: level 64 is link64, which keeps its child as childrenOmitted.
    const nlohmann::json* node = &env.data["roots"][0];
    for (int level = 0; level < 64; ++level) {
        REQUIRE(node->at("children").size() == 1);
        node = &node->at("children").at(0);
    }
    CHECK(node->at("name") == "link64");
    CHECK(node->at("children").empty());
    CHECK(node->at("childrenOmitted") == 1);
    // The listener's path (envelope JSON, then text) handles the 64-level tree
    // on the main-thread stack too.
    const std::string wire = Arcane::Remote::ToJson(env).dump();
    CHECK(wire.find("link64") != std::string::npos);
    CHECK(wire.find("link65") == std::string::npos);

    // A user --depth under the bound cuts there without the hard-bound warning.
    const auto shallow = InvokeTree({{"depth", 3}});
    REQUIRE(shallow.ok);
    CHECK(shallow.warnings.empty());
}

TEST_CASE("scene.tree --depth N cuts a node at depth N that has children", "[remote][scene]") {
    Astra::Registry reg;
    const auto root = reg.CreateEntity();
    const auto mid = reg.CreateEntity();
    const auto leafA = reg.CreateEntity();
    const auto leafB = reg.CreateEntity();
    REQUIRE(reg.SetParent(mid, root));
    REQUIRE(reg.SetParent(leafA, mid));
    REQUIRE(reg.SetParent(leafB, mid));
    std::uint64_t lo = 1;
    for (const auto e : {root, mid, leafA, leafB}) {
        Arcane::Identity id;
        id.id = Arcane::Guid{4, lo};
        id.name = "e" + std::to_string(lo++);
        reg.AddComponent<Arcane::Identity>(e, id);
    }
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterSceneCommands();
    RemoteTest::ScopedHostKind editorKind(Arcane::Remote::HostKind::Editor);

    const auto env = InvokeTree({{"depth", 1}});
    REQUIRE(env.ok);
    REQUIRE(env.data["roots"].size() == 1);
    CHECK_FALSE(env.data["roots"][0].contains("childrenOmitted"));
    REQUIRE(env.data["roots"][0]["children"].size() == 1);
    const nlohmann::json& cut = env.data["roots"][0]["children"][0];
    CHECK(cut["name"] == "e2");
    CHECK(cut["children"].empty());
    CHECK(cut["childrenOmitted"] == 2);
    CHECK(env.warnings.empty());

    // depth 2 reaches the leaves; nothing is cut.
    const auto deeper = InvokeTree({{"depth", 2}});
    REQUIRE(deeper.ok);
    CHECK(deeper.data["roots"][0]["children"][0]["children"].size() == 2);
    CHECK_FALSE(deeper.data["roots"][0]["children"][0].contains("childrenOmitted"));

    // Above INT32_MAX is refused on both the signed and the unsigned branch, never truncated.
    const std::int64_t tooBig = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1;
    for (const nlohmann::json& bad : {nlohmann::json(tooBig), nlohmann::json(static_cast<std::uint64_t>(tooBig))}) {
        const auto refused = InvokeTree({{"depth", bad}});
        REQUIRE_FALSE(refused.ok);
        CHECK(refused.errors[0].code == "arg.invalid");
        CHECK(Arcane::Remote::ExitFromHost(refused) == 2);
    }
    // INT32_MAX itself is accepted (the whole tree in practice).
    const auto maxDepth = InvokeTree({{"depth", std::numeric_limits<std::int32_t>::max()}});
    CHECK(maxDepth.ok);
}

// ---- Final fix round 4 (N3-3) ---------------------------------------------

// The editor's remote teardown, shared by MainLoop's end and Shutdown(): a
// boot that exits before MainLoop (a splash close after finalize, a vehicle
// failure) after --play-as armed the session still stops it -- the descriptor
// goes, the session is reset, and the selection view naming the app's
// selection is cleared -- instead of waiting for ~HostSession.
TEST_CASE("StopEditorRemote stops the session, clears the selection view, and keeps the exit reason", "[remote][scene]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-editor-stop");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Editor;
    cfg.cvarContext = Arcane::CVarContext::Editor;
    cfg.projectRoot = tmp.path;
    auto session = std::make_unique<Arcane::Remote::HostSession>(cfg);
    REQUIRE(session->Start());
    const std::filesystem::path descriptor = session->DescriptorFile();
    REQUIRE(std::filesystem::exists(descriptor));
    Arcane::Editor::SelectionContext sel;
    g_sel = &sel;
    Arcane::Editor::SelectionView view;
    view.count = &SelCount;
    view.at = &SelAt;
    view.primary = &SelPrimary;
    RemoteTest::ScopedRemotePointer<const Arcane::Editor::SelectionView> viewGuard(
        [](const Arcane::Editor::SelectionView* v) {
            Arcane::Editor::SetSelectionView(v ? *v : Arcane::Editor::SelectionView{});
        },
        &view);
    Arcane::Editor::RegisterSelectionCommand();

    session->RequestQuit();
    CHECK(session->DrainFrame(0));   // the loop would break on remote-quit
    const char* reason = Arcane::Editor::StopEditorRemote(session, "exit");
    REQUIRE(reason != nullptr);
    CHECK(std::string(reason) == "remote-quit");
    CHECK(session == nullptr);
    CHECK_FALSE(std::filesystem::exists(descriptor));
    Arcane::Remote::Call call;
    call.command = "editor.selection.get";
    const auto denied = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE_FALSE(denied.errors.empty());
    CHECK(denied.errors[0].code == "permission.denied");   // the view was cleared
    g_sel = nullptr;
    // No session: a no-op that reports no reason.
    CHECK(Arcane::Editor::StopEditorRemote(session, "exit") == nullptr);
}

#endif
