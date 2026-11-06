#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Arcane/Remote/Commands/CatalogCommands.hpp>
#include <Arcane/Remote/Commands/EcsCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Arcane/Reflection.hpp>
#include <Servitor/Verbs.hpp>
#include "RemoteTestSupport.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

using Catch::Approx;

// A reflected plain struct (never registered as a component) whose short name
// is Transform: schema.type's short-name resolver must not count it.
namespace EcsTestShadow {
struct Transform {
    float x = 0.0f;
};
ARC_REFLECT_TYPE(Transform)
    ARC_REFLECT_FIELD(Transform, x)
ARC_REFLECT_TYPE_END()
} // namespace EcsTestShadow

// The one-row world table every world-taking command reads (Task 19, ruling F16).
static std::vector<Arcane::Remote::WorldInfo> RemoteTestWorld(void* user) {
    Arcane::Remote::WorldInfo world;
    world.name = "main";
    world.role = "Standalone";
    world.registry = static_cast<Astra::Registry*>(user);
    return {world};
}

TEST_CASE("short names collide as component.ambiguous", "[remote][ecs]") {
    const auto many = Arcane::Remote::PickComponent({"GameA::Health", "GameB::Health"}, "Health");
    CHECK(many.pick == Arcane::Remote::NamePick::Many);
    REQUIRE(many.candidates.size() == 2);
    CHECK(many.candidates[0] == "GameA::Health");
    CHECK(many.candidates[1] == "GameB::Health");
    const auto one = Arcane::Remote::PickComponent({"Arcane::Transform"}, "Transform");
    CHECK(one.pick == Arcane::Remote::NamePick::One);
    CHECK(one.qualified == "Arcane::Transform");
    CHECK(Arcane::Remote::ShortName("Arcane::Transform") == "Transform");
    const auto none = Arcane::Remote::PickComponent({"Arcane::Transform"}, "Nope");
    CHECK(none.pick == Arcane::Remote::NamePick::None);
}

TEST_CASE("ecs.query --fields keeps one member and entity.get reads Transform", "[remote][ecs]") {
    Astra::Registry reg;
    const auto entity = reg.CreateEntity();
    Arcane::Identity id;
    id.id = Arcane::Guid{1, 2};
    id.name = "MeshCube";
    reg.AddComponent<Arcane::Identity>(entity, id);
    Arcane::Transform xf;
    xf.position = glm::vec3(0.4f, -0.3f, 0.0f);
    xf.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    xf.scale = glm::vec3(4.0f, 5.0f, 6.0f);
    reg.AddComponent<Arcane::Transform>(entity, xf);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};   // one-row table (ruling F16)
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterEcsCommands();

    Arcane::Remote::Call query;
    query.command = "ecs.query";
    query.args = nlohmann::json{
        {"with", nlohmann::json::array({"Transform"})},
        {"fields", nlohmann::json::array({"Transform.position"})},
        {"limit", 100}};
    const auto rows = Arcane::Remote::CommandRegistry::Get().Invoke(query).envelope;
    CHECK(rows.ok);
    REQUIRE(rows.data["rows"].size() == 1);
    const auto& comp = rows.data["rows"][0]["components"]["Transform"];
    CHECK(comp["position"][0].get<double>() == Approx(0.4).margin(1e-4));
    CHECK(comp["position"][1].get<double>() == Approx(-0.3).margin(1e-4));
    CHECK_FALSE(comp.contains("scale"));
    CHECK_FALSE(comp.contains("rotation"));
    CHECK(rows.data["rows"][0]["name"] == "MeshCube");

    Arcane::Remote::Call get;
    get.command = "entity.get";
    get.args = nlohmann::json{{"entity", "name:MeshCube"}, {"components", nlohmann::json::array({"Transform"})}};
    const auto got = Arcane::Remote::CommandRegistry::Get().Invoke(get).envelope;
    CHECK(got.ok);
    CHECK(got.data["components"]["Transform"]["rotation"][0] == 0);
    CHECK(got.data["components"]["Transform"]["rotation"][3] == 1);
    CHECK(got.data["entity"]["guid"]["hi"] == 1);
    CHECK(got.data["entity"]["guid"]["lo"] == 2);

    Arcane::Remote::Call loose;
    loose.command = "ecs.query";
    loose.args = nlohmann::json{{"with", nlohmann::json::array({"NoSuchComponent"})}, {"strict", false}};
    const auto warned = Arcane::Remote::CommandRegistry::Get().Invoke(loose).envelope;
    CHECK(warned.ok);
    CHECK(warned.errors.empty());
    CHECK(warned.warnings[0].code == "component.unknown");
    CHECK(warned.data["rows"].empty());

    Arcane::Remote::Call strict;
    strict.command = "ecs.query";
    strict.args = nlohmann::json{{"with", nlohmann::json::array({"NoSuchComponent"})}};
    const auto failed = Arcane::Remote::CommandRegistry::Get().Invoke(strict).envelope;
    CHECK_FALSE(failed.ok);
    CHECK(failed.errors[0].code == "component.unknown");
    CHECK(Arcane::Remote::ExitFromHost(failed) == 6);

    Arcane::Remote::Call missing;
    missing.command = "entity.get";
    missing.args = nlohmann::json{{"entity", "name:NoSuchEntity"}};
    const auto absent = Arcane::Remote::CommandRegistry::Get().Invoke(missing).envelope;
    CHECK(absent.errors[0].code == "entity.not-found");
    CHECK(Arcane::Remote::ExitFromHost(absent) == 6);

    Arcane::Remote::Call wrongWorld;
    wrongWorld.command = "ecs.stats";
    wrongWorld.world = "nope";                          // --world names no world
    const auto unknownWorld = Arcane::Remote::CommandRegistry::Get().Invoke(wrongWorld).envelope;
    CHECK(unknownWorld.errors[0].code == "arg.unknown");
    CHECK(Arcane::Remote::ExitFromHost(unknownWorld) == 2);
}

// Helpers for the cases below: one Invoke per call.
static Arcane::Remote::Envelope InvokeEcs(const char* command, nlohmann::json args) {
    Arcane::Remote::Call call;
    call.command = command;
    call.args = std::move(args);
    return Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
}

static Astra::Entity AddNamed(Astra::Registry& reg, Arcane::Guid guid, const std::string& name) {
    const auto e = reg.CreateEntity();
    Arcane::Identity id;
    id.id = guid;
    id.name = name;
    reg.AddComponent<Arcane::Identity>(e, id);
    reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    return e;
}

TEST_CASE("entity locators: guid, ambiguous name, raw handle, absent component", "[remote][ecs]") {
    Astra::Registry reg;
    AddNamed(reg, Arcane::Guid{7, 9}, "Twin");
    AddNamed(reg, Arcane::Guid{7, 10}, "Twin");
    const auto bare = reg.CreateEntity();               // no Identity
    reg.AddComponent<Arcane::Transform>(bare, Arcane::Transform{});
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterEcsCommands();
    Arcane::Remote::RegisterEcsCommands();              // a second call returns

    const auto byGuid = InvokeEcs("entity.get", {{"entity", "guid:" + Arcane::Guid{7, 10}.ToString()}});
    REQUIRE(byGuid.ok);
    CHECK(byGuid.data["entity"]["guid"]["lo"] == 10);
    CHECK(byGuid.data["entity"]["name"] == "Twin");
    CHECK(byGuid.data["entity"]["ref"] == "guid:" + Arcane::Guid{7, 10}.ToString());   // round-trips
    CHECK(byGuid.data["components"].contains("Identity"));   // empty components = every reflected one
    CHECK(byGuid.data["components"].contains("Transform"));
    CHECK(byGuid.warnings.empty());

    const auto twin = InvokeEcs("entity.get", {{"entity", "name:Twin"}});
    REQUIRE_FALSE(twin.ok);
    CHECK(twin.errors[0].code == "entity.ambiguous");
    CHECK(twin.errors[0].candidates.size() == 2);
    CHECK(Arcane::Remote::ExitFromHost(twin) == 2);

    const auto unknownGuid = InvokeEcs("entity.get", {{"entity", "guid:" + Arcane::Guid{3, 3}.ToString()}});
    CHECK(unknownGuid.errors[0].code == "entity.not-found");
    const auto badLocator = InvokeEcs("entity.get", {{"entity", "MeshCube"}});
    CHECK(badLocator.errors[0].code == "arg.invalid");
    CHECK(Arcane::Remote::ExitFromHost(badLocator) == 2);

    const auto handle = InvokeEcs("entity.get", {{"entity", static_cast<std::uint64_t>(bare.GetValue())}});
    REQUIRE(handle.ok);
    CHECK(handle.data["entity"]["guid"].is_null());
    CHECK(handle.data["entity"]["handle"] == static_cast<std::uint64_t>(bare.GetValue()));
    REQUIRE(handle.warnings.size() == 1);
    CHECK(handle.warnings[0].code == "entity.no-identity");

    // A handle outside the handle type's range never aliases onto a live entity.
    const std::uint64_t oversized = (std::uint64_t{1} << 40) + static_cast<std::uint64_t>(bare.GetValue());
    const auto aliased = InvokeEcs("entity.get", {{"entity", oversized}});
    CHECK(aliased.errors[0].code == "entity.not-found");
    CHECK(Arcane::Remote::ExitFromHost(aliased) == 6);

    const auto noIdentity = InvokeEcs("entity.get", {{"entity", std::to_string(bare.GetValue())},
                                                    {"components", nlohmann::json::array({"Identity"})}});
    CHECK(noIdentity.errors[0].code == "component.absent");
    CHECK(Arcane::Remote::ExitFromHost(noIdentity) == 6);

    // rows sort by guid.hi, then guid.lo; the entity without Identity is a guid:null row.
    const auto rows = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"Transform"})},
                                             {"without", nlohmann::json::array({"Identity"})}});
    REQUIRE(rows.ok);
    CHECK(rows.data["count"] == 1);
    CHECK(rows.data["rows"][0]["guid"].is_null());
    const auto both = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"Identity"})}, {"limit", 1}});
    REQUIRE(both.ok);
    CHECK(both.data["count"] == 2);
    CHECK(both.data["truncated"] == true);
    REQUIRE(both.data["rows"].size() == 1);
    CHECK(both.data["rows"][0]["guid"]["lo"] == 9);
    const auto again = InvokeEcs("entity.get", {{"entity", both.data["rows"][0]["ref"]}});
    REQUIRE(again.ok);
    CHECK(again.data["entity"]["guid"]["lo"] == 9);

    const auto stats = InvokeEcs("ecs.stats", nlohmann::json::object());
    REQUIRE(stats.ok);
    CHECK(stats.data["entityCount"] == 3);
    CHECK(stats.data["archetypeCount"].get<int>() >= 2);
    const auto archetypes = InvokeEcs("ecs.archetypes", nlohmann::json::object());
    REQUIRE(archetypes.ok);
    int entities = 0;
    for (const auto& row : archetypes.data["archetypes"])
        entities += row["entityCount"].get<int>();
    CHECK(entities == 3);

    const auto unknownArg = InvokeEcs("ecs.query", {{"wiht", nlohmann::json::array()}});
    CHECK(unknownArg.errors[0].code == "arg.unknown");

    // An unknown --fields member: strict refuses (arg.invalid, exit 2, the
    // members as candidates); non-strict warns and keeps the known member.
    const auto badField = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"Transform"})},
                                                 {"fields", nlohmann::json::array({"Transform.nope"})}});
    REQUIRE_FALSE(badField.ok);
    CHECK(badField.errors[0].code == "arg.invalid");
    CHECK(Arcane::Remote::ExitFromHost(badField) == 2);
    CHECK(std::find(badField.errors[0].candidates.begin(), badField.errors[0].candidates.end(), "position") !=
          badField.errors[0].candidates.end());
    const auto looseField = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"Transform"})},
                                                   {"fields", nlohmann::json::array({"Transform.nope", "Transform.scale"})},
                                                   {"strict", false}});
    REQUIRE(looseField.ok);
    REQUIRE(looseField.warnings.size() == 1);
    CHECK(looseField.warnings[0].code == "arg.invalid");
    CHECK(looseField.data["count"] == 3);
    const auto& firstTransform = looseField.data["rows"][0]["components"]["Transform"];
    CHECK(firstTransform.contains("scale"));
    CHECK_FALSE(firstTransform.contains("position"));
    CHECK_FALSE(firstTransform.contains("nope"));

    // A limit over the main-thread bound is clamped, and the clamp is reported.
    const auto clamped = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"Transform"})},
                                                {"limit", std::uint64_t{1} << 40}});
    REQUIRE(clamped.ok);
    REQUIRE(clamped.warnings.size() == 1);
    CHECK(clamped.warnings[0].code == "ecs.limit-clamped");
    CHECK(clamped.data["rows"].size() == 3);
}

TEST_CASE("ecs.query over the inline cap writes the rows to one ecs artifact", "[remote][ecs]") {
    Astra::Registry reg;
    const std::string longName(1024, 'n');
    for (std::uint64_t i = 0; i < 300; ++i)
        AddNamed(reg, Arcane::Guid{1, 300 - i}, longName + std::to_string(i));
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Test::TempDir out("ecs_artifact");
    Arcane::Remote::SetEcsArtifactDir(out.path);
    Arcane::Remote::RegisterEcsCommands();

    const auto big = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"Identity"})},
                                            {"fields", nlohmann::json::array({"Identity.name"})},
                                            {"limit", 1000}});
    Arcane::Remote::ReleaseEcsArtifactDir(out.path);
    REQUIRE(big.ok);
    CHECK(big.data["count"] == 300);
    CHECK(big.data["truncated"] == true);
    CHECK(big.data["rows"].empty());
    REQUIRE(big.artifacts.size() == 1);
    CHECK(big.artifacts[0].kind == "ecs");
    CHECK(big.artifacts[0].sha256.size() == 64);
    const std::filesystem::path file = big.artifacts[0].path;
    REQUIRE(std::filesystem::exists(file));
    CHECK(std::filesystem::file_size(file) == big.artifacts[0].bytes);
    std::ifstream in(file, std::ios::binary);
    const nlohmann::json written = nlohmann::json::parse(in);
    REQUIRE(written["rows"].size() == 300);
    CHECK(written["rows"][0]["guid"]["lo"] == 1);   // sorted by guid
    CHECK(written["rows"][0]["components"]["Identity"].contains("name"));
    CHECK_FALSE(written["rows"][0]["components"]["Identity"].contains("id"));

    // An artifact directory that cannot be written is an error, never ok with empty rows.
    const std::filesystem::path notADir = out.path / "plain-file";
    { std::ofstream(notADir) << "x"; }
    Arcane::Remote::SetEcsArtifactDir(notADir);
    const auto failed = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"Identity"})}, {"limit", 1000}});
    Arcane::Remote::ReleaseEcsArtifactDir(notADir);
    REQUIRE_FALSE(failed.ok);
    CHECK(failed.errors[0].code == "ecs.artifact-failed");
    CHECK(Arcane::Remote::ExitFromHost(failed) == 6);
    CHECK(failed.artifacts.empty());

    // With no artifact directory the rows are dropped, not inlined, and a warning says so.
    const auto noDir = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"Identity"})}, {"limit", 1000}});
    REQUIRE(noDir.ok);
    CHECK(noDir.data["rows"].empty());
    CHECK(noDir.data["truncated"] == true);
    CHECK(noDir.artifacts.empty());
    REQUIRE_FALSE(noDir.warnings.empty());
    CHECK(noDir.warnings[0].code == "ecs.no-artifact-dir");
}

TEST_CASE("schema.type resolves a short component name through PickComponent", "[remote][ecs]") {
    Astra::Registry reg;   // registers Arcane::Transform as a component in this context
    reg.AddComponent<Arcane::Transform>(reg.CreateEntity(), Arcane::Transform{});
    REQUIRE(Astra::GetMetaByName("EcsTestShadow::Transform") != nullptr);   // the same-short-name plain struct
    Arcane::Remote::RegisterCatalogCommands();
    for (const char* name : {"Transform", "Arcane::Transform"}) {
        const auto typed = InvokeEcs("schema.type", {{"kind", "component"}, {"name", name}});
        REQUIRE(typed.ok);
        CHECK(typed.data["properties"].contains("position"));
    }
    const auto unknown = InvokeEcs("schema.type", {{"kind", "component"}, {"name", "NoSuchComponent"}});
    CHECK(unknown.errors[0].code == "component.unknown");
}

TEST_CASE("entity get and ecs verbs build their wire args", "[remote][ecs][servitor]") {
    nlohmann::json out;
    std::string error;
    const Servitor::VerbRow* get = Servitor::FindVerb("entity get");
    const Servitor::VerbRow* query = Servitor::FindVerb("ecs query");
    REQUIRE(get != nullptr);
    REQUIRE(query != nullptr);
    REQUIRE(Servitor::FindVerb("ecs stats") != nullptr);
    REQUIRE(Servitor::FindVerb("ecs archetypes") != nullptr);
    CHECK(std::string(get->command) == "entity.get");
    CHECK(std::string(query->command) == "ecs.query");
    CHECK(std::string(Servitor::FindVerb("ecs stats")->command) == "ecs.stats");
    CHECK(std::string(Servitor::FindVerb("ecs archetypes")->command) == "ecs.archetypes");

    std::vector<std::string> t{"name:MeshCube", "--component", "Transform", "--component", "Identity"};
    REQUIRE(get->args(t, out, &error));
    CHECK(out == nlohmann::json{{"entity", "name:MeshCube"}, {"components", {"Transform", "Identity"}}});
    t = {"guid:x"};
    REQUIRE(get->args(t, out, &error));
    CHECK(out == nlohmann::json{{"entity", "guid:x"}});
    for (const auto& bad : std::vector<std::vector<std::string>>{{}, {"a", "b"}, {"a", "--bogus"}})
        CHECK_FALSE(get->args(bad, out, &error));

    t = {"--with", "Transform,Identity", "--without", "Camera", "--fields", "Transform.position,Identity",
         "--limit", "5", "--no-strict"};
    REQUIRE(query->args(t, out, &error));
    CHECK(out == nlohmann::json{{"with", {"Transform", "Identity"}}, {"without", {"Camera"}},
                                {"fields", {"Transform.position", "Identity"}}, {"limit", 5}, {"strict", false}});
    for (const auto& bad : std::vector<std::vector<std::string>>{{"Transform"}, {"--limit", "x"}, {"--limit", "-1"},
                                                                {"--strict"}})
        CHECK_FALSE(query->args(bad, out, &error));
}
// ---- Final fix pass (C-I5, C-M8) ---------------------------------------------

// A game component holding a secret: the same Attr::Flags{Protected} the
// resource walk honours. Non-trivially-copyable, so it serializes through a
// member, as Identity does.
struct RemoteSecretComponent {
    int port = 7;
    std::string token = "default-secret";
    template <typename Archive>
    void Serialize(Archive& ar) {
        ar(port);
        ar(token);
    }
};
ARC_REFLECT_TYPE(RemoteSecretComponent)
    ARC_REFLECT_FIELD(RemoteSecretComponent, port)
    ARC_REFLECT_FIELD(RemoteSecretComponent, token) ARC_REFLECT_ATTR(Flags, Arcane::CVarFlags::Protected)
ARC_REFLECT_TYPE_END()

// entity.get and ecs.query apply resource.get's Protected walk (one shared
// helper), and schema.type masks a Protected field's default the same way.
TEST_CASE("entity.get, ecs.query and schema.type never write a Protected component field", "[remote][ecs]") {
    Astra::Registry reg;
    const auto e = reg.CreateEntity();
    Arcane::Identity id;
    id.id = Arcane::Guid{9, 9};
    id.name = "Vault";
    reg.AddComponent<Arcane::Identity>(e, id);
    RemoteSecretComponent secret;
    secret.port = 4242;
    secret.token = "hunter2";
    reg.AddComponent<RemoteSecretComponent>(e, secret);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterEcsCommands();
    Arcane::Remote::RegisterCatalogCommands();

    auto got = InvokeEcs("entity.get", {{"entity", "name:Vault"},
                                              {"components", nlohmann::json::array({"RemoteSecretComponent"})}});
    REQUIRE(got.ok);
    const auto& comp = got.data["components"]["RemoteSecretComponent"];
    CHECK(comp["port"] == 4242);
    CHECK(comp["token"] == "<protected>");
    CHECK(Arcane::Remote::ToJson(got).dump().find("hunter2") == std::string::npos);

    auto rows = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"RemoteSecretComponent"})}});
    REQUIRE(rows.ok);
    REQUIRE(rows.data["rows"].size() == 1);
    CHECK(rows.data["rows"][0]["components"]["RemoteSecretComponent"]["token"] == "<protected>");
    auto field = InvokeEcs("ecs.query", {{"with", nlohmann::json::array({"RemoteSecretComponent"})},
                                               {"fields", nlohmann::json::array({"RemoteSecretComponent.token"})}});
    REQUIRE(field.ok);
    CHECK(field.data["rows"][0]["components"]["RemoteSecretComponent"]["token"] == "<protected>");
    CHECK(Arcane::Remote::ToJson(field).dump().find("hunter2") == std::string::npos);

    auto schema = InvokeEcs("schema.type", {{"kind", "component"}, {"name", "RemoteSecretComponent"}});
    REQUIRE(schema.ok);
    CHECK(schema.data["properties"]["port"]["default"] == 7);
    CHECK(schema.data["properties"]["token"]["default"] == "<protected>");
    CHECK(Arcane::Remote::ToJson(schema).dump().find("default-secret") == std::string::npos);
}

#endif
