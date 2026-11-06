#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/EcsCommands.hpp>
#include <Arcane/Remote/Commands/ResourceCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Reflection.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Servitor/Verbs.hpp>
#include "RemoteTestSupport.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

struct RemoteProbeResource { int value = 0; };
ARC_REFLECT_TYPE(RemoteProbeResource)
    ARC_REFLECT_FIELD(RemoteProbeResource, value)
ARC_REFLECT_TYPE_END()

TEST_CASE("resource.get returns the resource the fixture stored", "[remote]") {
    Astra::Registry reg;
    RemoteProbeResource probe;
    probe.value = 9;
    REQUIRE(reg.SetResource(std::move(probe)) != nullptr);
    const auto listed = reg.InspectResources();
    REQUIRE_FALSE(listed.empty());
    REQUIRE(listed[0].descriptor != nullptr);
    REQUIRE(listed[0].descriptor->name != nullptr);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};   // one-row table (ruling F16)
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterResourceCommands();
    Arcane::Remote::Call get;
    get.command = "resource.get";
    get.args = nlohmann::json{{"name", listed[0].descriptor->name}};
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(get).envelope;
    CHECK(env.ok);
    CHECK(env.data["value"] == 9);
    Arcane::Remote::Call missing;
    missing.command = "resource.get";
    missing.args = nlohmann::json{{"name", "NoSuchResource"}};
    const auto absent = Arcane::Remote::CommandRegistry::Get().Invoke(missing).envelope;
    CHECK_FALSE(absent.ok);
    CHECK(absent.errors[0].code == "resource.not-found");
    CHECK(Arcane::Remote::ExitFromHost(absent) == 6);
}

// Two resources whose short names collide, a resource with a Protected field,
// one whose whole type is Protected, and one over the 256 KiB inline cap.
namespace ResourceTestA {
struct Knob { int turns = 0; };
ARC_REFLECT_TYPE(Knob)
    ARC_REFLECT_FIELD(Knob, turns)
ARC_REFLECT_TYPE_END()
} // namespace ResourceTestA
namespace ResourceTestB {
struct Knob { int turns = 0; };
ARC_REFLECT_TYPE(Knob)
    ARC_REFLECT_FIELD(Knob, turns)
ARC_REFLECT_TYPE_END()
} // namespace ResourceTestB

// Non-trivially-copyable resources serialize through a member (Astra's
// HasSerializeMethod seam), as Identity does.
struct RemoteSecretResource {
    int port = 0;
    std::string token;
    template <typename Archive>
    void Serialize(Archive& ar) {
        ar(port);
        ar(token);
    }
};
ARC_REFLECT_TYPE(RemoteSecretResource)
    ARC_REFLECT_FIELD(RemoteSecretResource, port)
    ARC_REFLECT_FIELD(RemoteSecretResource, token) ARC_REFLECT_ATTR(Flags, Arcane::CVarFlags::Protected)
ARC_REFLECT_TYPE_END()

struct RemoteSecretHolder {
    RemoteSecretResource inner;
    int visible = 0;
    template <typename Archive>
    void Serialize(Archive& ar) {
        inner.Serialize(ar);
        ar(visible);
    }
};
ARC_REFLECT_TYPE(RemoteSecretHolder)
    ARC_REFLECT_FIELD(RemoteSecretHolder, inner)
    ARC_REFLECT_FIELD(RemoteSecretHolder, visible)
ARC_REFLECT_TYPE_END()

// Fix round 1: Astra records no element type for std::array, so a Protected
// field inside its element must still never be written. (A C-array member does
// not reflect at all: ARC_REFLECT_FIELD's accessors assign the member, and an
// array is not assignable -- so there is no C-array case to test.)
struct RemoteSecretArrays {
    std::array<RemoteSecretResource, 2> keys;
    int visible = 0;
    template <typename Archive>
    void Serialize(Archive& ar) {
        for (RemoteSecretResource& k : keys)
            k.Serialize(ar);
        ar(visible);
    }
};
ARC_REFLECT_TYPE(RemoteSecretArrays)
    ARC_REFLECT_FIELD(RemoteSecretArrays, keys)
    ARC_REFLECT_FIELD(RemoteSecretArrays, visible)
ARC_REFLECT_TYPE_END()

struct RemoteSealedResource { int code = 0; };
ARC_REFLECT_TYPE(RemoteSealedResource)
    ARC_REFLECT_TYPE_ATTR(Flags, Arcane::CVarFlags::Protected)
    ARC_REFLECT_FIELD(RemoteSealedResource, code)
ARC_REFLECT_TYPE_END()

struct RemoteBulkResource {
    std::string blob;
    template <typename Archive>
    void Serialize(Archive& ar) {
        ar(blob);
    }
};
ARC_REFLECT_TYPE(RemoteBulkResource)
    ARC_REFLECT_FIELD(RemoteBulkResource, blob)
ARC_REFLECT_TYPE_END()

struct RemoteOpaqueResource { int hidden = 0; };   // never reflected

static Arcane::Remote::Envelope InvokeResource(const char* command, nlohmann::json args) {
    Arcane::Remote::Call call;
    call.command = command;
    call.args = std::move(args);
    Arcane::Remote::Envelope env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    if (env.command.empty())
        env.command = command;   // the dispatcher stamps it on the wire path
    return env;
}

TEST_CASE("resource.list names every resource, sorted, and resource.get resolves short names", "[remote][resource]") {
    Astra::Registry reg;
    REQUIRE(reg.SetResource(ResourceTestA::Knob{3}) != nullptr);
    REQUIRE(reg.SetResource(ResourceTestB::Knob{4}) != nullptr);
    REQUIRE(reg.SetResource(RemoteProbeResource{5}) != nullptr);
    REQUIRE(reg.SetResource(RemoteOpaqueResource{6}) != nullptr);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterResourceCommands();
    Arcane::Remote::RegisterResourceCommands();   // a second call returns

    const auto list = InvokeResource("resource.list", nullptr);
    REQUIRE(list.ok);
    REQUIRE(list.data["resources"].is_array());
    std::vector<std::string> names;
    for (const nlohmann::json& row : list.data["resources"])
        names.push_back(row.at("name").get<std::string>());
    CHECK(names == std::vector<std::string>{"RemoteOpaqueResource", "RemoteProbeResource", "ResourceTestA::Knob",
                                            "ResourceTestB::Knob"});

    // A unique short name resolves; a qualified name always does.
    const auto shortName = InvokeResource("resource.get", {{"name", "RemoteProbeResource"}});
    REQUIRE(shortName.ok);
    CHECK(shortName.data["value"] == 5);
    const auto qualified = InvokeResource("resource.get", {{"name", "ResourceTestB::Knob"}});
    REQUIRE(qualified.ok);
    CHECK(qualified.data["turns"] == 4);

    // A colliding short name is never the first match: ambiguous, exit 2, both candidates.
    const auto ambiguous = InvokeResource("resource.get", {{"name", "Knob"}});
    REQUIRE_FALSE(ambiguous.ok);
    CHECK(ambiguous.errors[0].code == "resource.ambiguous");
    CHECK(Arcane::Remote::ExitFromHost(ambiguous) == 2);
    CHECK(ambiguous.errors[0].candidates == std::vector<std::string>{"ResourceTestA::Knob", "ResourceTestB::Knob"});

    // An unreflected resource is listed but cannot be read as JSON.
    const auto opaque = InvokeResource("resource.get", {{"name", "RemoteOpaqueResource"}});
    REQUIRE_FALSE(opaque.ok);
    CHECK(opaque.errors[0].code == "reflect.unreadable");
    CHECK(Arcane::Remote::ExitFromHost(opaque) == 6);

    // Argument refusals.
    const auto noName = InvokeResource("resource.get", nlohmann::json::object());
    CHECK(noName.errors[0].code == "arg.missing");
    const auto extra = InvokeResource("resource.get", {{"name", "Knob"}, {"bogus", 1}});
    CHECK(extra.errors[0].code == "arg.unknown");
    const auto notString = InvokeResource("resource.get", {{"name", 7}});
    CHECK(notString.errors[0].code == "arg.invalid");
    const auto listArgs = InvokeResource("resource.list", {{"bogus", 1}});
    CHECK(listArgs.errors[0].code == "arg.unknown");
}

TEST_CASE("resource.get masks Protected fields and Protected types", "[remote][resource]") {
    Astra::Registry reg;
    RemoteSecretResource secret;
    secret.port = 7777;
    secret.token = "hunter2";
    REQUIRE(reg.SetResource(std::move(secret)) != nullptr);
    RemoteSecretHolder holder;
    holder.inner.token = "hunter3";
    holder.visible = 1;
    REQUIRE(reg.SetResource(std::move(holder)) != nullptr);
    REQUIRE(reg.SetResource(RemoteSealedResource{42}) != nullptr);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterResourceCommands();

    const auto field = InvokeResource("resource.get", {{"name", "RemoteSecretResource"}});
    REQUIRE(field.ok);
    CHECK(field.data["port"] == 7777);
    CHECK(field.data["token"] == "<protected>");
    CHECK(field.data.dump().find("hunter2") == std::string::npos);

    const auto nested = InvokeResource("resource.get", {{"name", "RemoteSecretHolder"}});
    REQUIRE(nested.ok);
    CHECK(nested.data["visible"] == 1);
    CHECK(nested.data["inner"] == "<protected>");   // a nested struct holding a secret is masked whole
    CHECK(nested.data.dump().find("hunter3") == std::string::npos);

    RemoteSecretArrays arrays;
    arrays.keys[0].token = "array-secret-0";
    arrays.keys[1].token = "array-secret-1";
    arrays.visible = 5;
    REQUIRE(reg.SetResource(std::move(arrays)) != nullptr);
    const auto inArrays = InvokeResource("resource.get", {{"name", "RemoteSecretArrays"}});
    REQUIRE(inArrays.ok);
    CHECK(inArrays.data["visible"] == 5);
    CHECK(inArrays.data["keys"] == "<protected>");   // std::array: element type unresolvable, masked whole
    const std::string arraysDump = inArrays.data.dump();
    for (const char* leaked : {"array-secret-0", "array-secret-1", "hunter"})
        CHECK(arraysDump.find(leaked) == std::string::npos);

    const auto sealed = InvokeResource("resource.get", {{"name", "RemoteSealedResource"}});
    REQUIRE(sealed.ok);
    CHECK(sealed.data["code"] == "<protected>");
}

TEST_CASE("resource.get over the inline cap writes one resource artifact", "[remote][resource]") {
    Astra::Registry reg;
    RemoteBulkResource bulk;
    bulk.blob.assign(300000, 'b');   // ~300 KB of JSON
    REQUIRE(reg.SetResource(std::move(bulk)) != nullptr);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterResourceCommands();

    Arcane::Test::TempDir out("resource_artifact");
    Arcane::Remote::SetEcsArtifactDir(out.path);   // the session's <remote dir>/out, shared with ecs.query
    const auto big = InvokeResource("resource.get", {{"name", "RemoteBulkResource"}});
    Arcane::Remote::ReleaseEcsArtifactDir(out.path);
    REQUIRE(big.ok);
    CHECK(big.data.is_null());
    REQUIRE(big.artifacts.size() == 1);
    CHECK(big.artifacts[0].kind == "resource");
    CHECK(big.artifacts[0].sha256.size() == 64);
    REQUIRE(big.warnings.size() == 1);
    CHECK(big.warnings[0].code == "resource.in-artifact");
    const std::filesystem::path file = big.artifacts[0].path;
    REQUIRE(std::filesystem::exists(file));
    CHECK(std::filesystem::file_size(file) == big.artifacts[0].bytes);
    std::ifstream in(file, std::ios::binary);
    const nlohmann::json written = nlohmann::json::parse(in);
    REQUIRE(written["blob"].is_string());
    CHECK(written["blob"].get<std::string>().size() == 300000);

    // A directory that cannot be written is an error, never ok with no data.
    const std::filesystem::path notADir = out.path / "plain-file";
    { std::ofstream(notADir) << "x"; }
    Arcane::Remote::SetEcsArtifactDir(notADir);
    const auto failed = InvokeResource("resource.get", {{"name", "RemoteBulkResource"}});
    Arcane::Remote::ReleaseEcsArtifactDir(notADir);
    REQUIRE_FALSE(failed.ok);
    CHECK(failed.errors[0].code == "resource.artifact-failed");
    CHECK(Arcane::Remote::ExitFromHost(failed) == 6);

    // With no artifact directory the value is dropped, not inlined, and a warning says so.
    const auto noDir = InvokeResource("resource.get", {{"name", "RemoteBulkResource"}});
    REQUIRE(noDir.ok);
    CHECK(noDir.data.is_null());
    CHECK(noDir.artifacts.empty());
    REQUIRE(noDir.warnings.size() == 1);
    CHECK(noDir.warnings[0].code == "resource.no-artifact-dir");
}

TEST_CASE("resource.* answers host.unavailable with no world table", "[remote][resource]") {
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        nullptr);
    Arcane::Remote::RegisterResourceCommands();
    const auto list = InvokeResource("resource.list", nullptr);
    REQUIRE_FALSE(list.ok);
    CHECK(Arcane::Remote::ExitFromHost(list) == 5);
    const auto get = InvokeResource("resource.get", {{"name", "Anything"}});
    REQUIRE_FALSE(get.ok);
    CHECK(Arcane::Remote::ExitFromHost(get) == 5);
}

TEST_CASE("resource verbs build their wire args", "[remote][resource][servitor]") {
    nlohmann::json out;
    std::string error;
    const Servitor::VerbRow* list = Servitor::FindVerb("resource list");
    const Servitor::VerbRow* get = Servitor::FindVerb("resource get");
    REQUIRE(list != nullptr);
    REQUIRE(get != nullptr);
    CHECK(std::string(list->command) == "resource.list");
    CHECK(list->args == nullptr);   // Run refuses any token after `resource list`
    CHECK(std::string(get->command) == "resource.get");
    std::vector<std::string> t{"Arcane::Time"};
    REQUIRE(get->args(t, out, &error));
    CHECK(out == nlohmann::json{{"name", "Arcane::Time"}});
    for (const auto& bad : std::vector<std::vector<std::string>>{{}, {"a", "b"}, {"a", "--bogus"}}) {
        error.clear();
        CHECK_FALSE(get->args(bad, out, &error));
        CHECK_FALSE(error.empty());
    }
}

#endif
