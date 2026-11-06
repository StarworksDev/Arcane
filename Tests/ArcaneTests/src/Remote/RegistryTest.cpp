#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Registry.hpp>
#include "RemoteDemoArgs.hpp"
#include "RemoteTestSupport.hpp"
#include <algorithm>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

static Arcane::Remote::CommandResult DemoFn(const Arcane::Remote::Call& call, void*) {
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeOk(call.command, call.args, Arcane::Remote::HostBlock{});
    return r;
}

static Arcane::Remote::Desc Base() {
    Arcane::Remote::Desc d;
    d.name = "demo.ok";
    d.summary = "does a thing";
    d.argsMeta = Astra::GetMeta<RemoteDemoArgs>();
    d.fn = &DemoFn;
    return d;
}

TEST_CASE("registry rejects an empty summary, a bad name, and Drive with mutates None", "[remote][registry]") {
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    Arcane::Remote::Desc bad = Base();
    bad.summary = "";
    CHECK_FALSE(reg.Register(bad, &err));
    bad.summary = "does a thing";
    bad.name = "Demo.Ok";
    CHECK_FALSE(reg.Register(bad, &err));
    bad.name = "demo..ok";
    CHECK_FALSE(reg.Register(bad, &err));
    bad.name = "demo.drive";
    bad.access = Arcane::Remote::Access::Drive;
    bad.mutates = Arcane::Remote::Mutates::None;
    CHECK_FALSE(reg.Register(bad, &err));
}

TEST_CASE("schema rewrites vec3 and stamps ReflectionJsonWriter defaults", "[remote][registry]") {
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    Arcane::Remote::Desc ok = Base();
    REQUIRE(reg.Register(ok, &err));
    const auto schema = reg.SchemaOf("demo.ok");
    const auto& pos = schema["properties"]["position"];
    CHECK(pos["type"] == "array");
    CHECK(pos["minItems"] == 3);
    CHECK(pos["maxItems"] == 3);
    CHECK(pos["default"].is_array());
    CHECK(pos["default"].size() == 3);
    CHECK(pos["default"][0] == 0);
    CHECK(pos["default"][1] == 0);
    CHECK(pos["default"][2] == 0);
    CHECK(schema["properties"]["n"]["default"] == 3);
    CHECK_FALSE(reg.Register(ok, &err));
    reg.UnregisterModule("");
    CHECK(reg.Find("demo.ok") != nullptr);
    ok.module = "fixture";
    ok.name = "demo.mod";
    REQUIRE(reg.Register(ok, &err));
    const auto gen = reg.Generation();
    reg.UnregisterModule("fixture");
    CHECK(reg.Find("demo.mod") == nullptr);
    CHECK(reg.Generation() == gen + 1);
    CHECK(reg.Find("demo.ok") != nullptr);
    Arcane::Remote::Call call;
    call.command = "no.such";
    const auto missing = reg.Invoke(call);
    CHECK(missing.envelope.errors[0].code == "command.unknown");
    CHECK(Arcane::Remote::ExitFromHost(missing.envelope) == 2);
}

TEST_CASE("an editor command on a runtime host is command.host-kind", "[remote][registry]") {
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    // A name no task ever registers (ruling F7): Task 25 registers the real
    // editor.selection.get in this same process singleton.
    Arcane::Remote::Call call;
    call.command = "editor.nosuch.get";
    {
        RemoteTest::ScopedHostKind runtime(Arcane::Remote::HostKind::Runtime);
        const auto offEditor = reg.Invoke(call);
        CHECK(offEditor.envelope.errors[0].code == "command.host-kind");
        CHECK(Arcane::Remote::ExitFromHost(offEditor.envelope) == 2);
        Arcane::Remote::Call other;
        other.command = "no.such";
        CHECK(reg.Invoke(other).envelope.errors[0].code == "command.unknown");
    }
    {
        RemoteTest::ScopedHostKind editor(Arcane::Remote::HostKind::Editor);
        CHECK(reg.Invoke(call).envelope.errors[0].code == "command.unknown");
    }
}

TEST_CASE("catalogue is sorted and command.unknown lists same-group candidates", "[remote][registry]") {
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    const auto before = reg.Generation();
    Arcane::Remote::Desc d = Base();
    d.module = "cattest";
    d.name = "cattest.zeta";
    Arcane::Remote::Desc noArgs = d;
    noArgs.argsMeta = nullptr;
    CHECK_FALSE(reg.Register(noArgs, &err));
    Arcane::Remote::Desc noFn = d;
    noFn.fn = nullptr;
    CHECK_FALSE(reg.Register(noFn, &err));
    CHECK(reg.Generation() == before);
    REQUIRE(reg.Register(d, &err));
    d.name = "cattest.alpha";
    d.access = Arcane::Remote::Access::Drive;
    d.mutates = Arcane::Remote::Mutates::Scene;
    REQUIRE(reg.Register(d, &err));
    CHECK(reg.Generation() == before + 2);

    const auto catalogue = reg.Catalogue();
    CHECK(catalogue["generation"] == reg.Generation());
    std::vector<std::string> names;
    for (const auto& row : catalogue["commands"])
        names.push_back(row["name"].get<std::string>());
    CHECK(std::is_sorted(names.begin(), names.end()));
    const auto alpha = std::find(names.begin(), names.end(), "cattest.alpha");
    REQUIRE(alpha != names.end());
    const auto& row = catalogue["commands"][static_cast<std::size_t>(alpha - names.begin())];
    CHECK(row["access"] == "drive");
    CHECK(row["mutates"] == "scene");
    CHECK(row["module"] == "cattest");

    Arcane::Remote::Call call;
    call.command = "cattest.nosuch";
    const auto missing = reg.Invoke(call);
    REQUIRE(missing.envelope.errors.size() == 1);
    CHECK(missing.envelope.errors[0].code == "command.unknown");
    CHECK(missing.envelope.errors[0].remediation == "servitor list");
    CHECK(missing.envelope.errors[0].candidates == std::vector<std::string>{"cattest.alpha", "cattest.zeta"});

    reg.UnregisterModule("cattest");
    CHECK(reg.Find("cattest.alpha") == nullptr);
    CHECK(reg.Find("cattest.zeta") == nullptr);
}

struct RemoteSchemaInner {
    int n = 4;
};
ARC_REFLECT_TYPE(RemoteSchemaInner)
    ARC_REFLECT_FIELD(RemoteSchemaInner, n)
ARC_REFLECT_TYPE_END()

struct RemoteSchemaOuter {
    int plain = 7;
    RemoteSchemaInner child{};
};
ARC_REFLECT_TYPE(RemoteSchemaOuter)
    ARC_REFLECT_FIELD(RemoteSchemaOuter, plain)
    ARC_REFLECT_FIELD(RemoteSchemaOuter, child)
ARC_REFLECT_TYPE_END()

struct RemoteSchemaNoDefault {
    int n;
    RemoteSchemaNoDefault() = delete;
    explicit RemoteSchemaNoDefault(int v) : n(v) {}
};
ARC_REFLECT_TYPE(RemoteSchemaNoDefault)
    ARC_REFLECT_FIELD(RemoteSchemaNoDefault, n)
ARC_REFLECT_TYPE_END()

TEST_CASE("schema defaults come from ReflectionJsonWriter and keep a nested initializer", "[remote][registry]") {
    const Astra::TypeMeta* outer = Astra::GetMeta<RemoteSchemaOuter>();
    REQUIRE(outer != nullptr);
    const auto schema = Arcane::Remote::SchemaFor(*outer);
    CHECK(schema["properties"]["plain"]["default"] == 7);
    CHECK(schema["properties"]["child"]["default"]["n"] == 4);
    const Astra::TypeMeta* bare = Astra::GetMeta<RemoteSchemaNoDefault>();
    REQUIRE(bare != nullptr);
    CHECK(bare->isDefaultConstructible == false);
    const auto noDefault = Arcane::Remote::SchemaFor(*bare);
    CHECK_FALSE(noDefault.dump().find("\"default\"") != std::string::npos);
}

TEST_CASE("fallback adapter drops Optional from required and emits one $defs object", "[remote][registry]") {
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    Arcane::Remote::Desc ok;
    ok.name = "demo.opt";
    ok.summary = "optional field";
    ok.module = "fallback";
    ok.argsMeta = Astra::GetMeta<RemoteDemoArgs>();
    ok.fn = &DemoFn;
    REQUIRE(reg.Register(ok, &err));
    const auto flat = reg.SchemaOf("demo.opt");
    REQUIRE(flat.contains("required")); // `position` is required, so the list exists (ruling S3-2)
    CHECK(flat["required"].dump().find("\"n\"") == std::string::npos);
    CHECK(flat["required"].dump().find("\"position\"") != std::string::npos);
    reg.UnregisterModule("fallback");
    CHECK(reg.Find("demo.opt") == nullptr);

    const auto schema = Arcane::Remote::SchemaFor(*Astra::GetMeta<RemoteSchemaOuter>());
    CHECK(schema["properties"]["child"]["$ref"] == "#/$defs/RemoteSchemaInner");
    CHECK(schema["$defs"]["RemoteSchemaInner"]["properties"]["n"]["default"] == 4);
    CHECK_FALSE(schema["$defs"]["RemoteSchemaInner"].contains("$defs"));
    const auto& innerReq = schema["$defs"]["RemoteSchemaInner"];
    REQUIRE(innerReq.contains("required")); // ruling S3-2: no `if`
    CHECK(innerReq["required"].dump().find("\"n\"") != std::string::npos);
}

struct RemoteSchemaList {
    std::vector<std::string> names;
    int count = 2;
};
ARC_REFLECT_TYPE(RemoteSchemaList)
    ARC_REFLECT_FIELD(RemoteSchemaList, names)
    ARC_REFLECT_FIELD(RemoteSchemaList, count)
ARC_REFLECT_TYPE_END()

TEST_CASE("schema repairs Astra's literal-to-bool keys and leaves property types alone", "[remote][registry]") {
    // Vendored Astra writes "$schema", the top-level "type" and array "items"
    // "type" as `true` (JsonSchema.hpp:50/:56/:276); SchemaFor restores them.
    for (const Astra::TypeMeta* meta : {Astra::GetMeta<RemoteDemoArgs>(), Astra::GetMeta<RemoteSchemaOuter>(),
                                        Astra::GetMeta<RemoteSchemaList>()}) {
        REQUIRE(meta != nullptr);
        const auto schema = Arcane::Remote::SchemaFor(*meta);
        INFO(schema.dump());
        REQUIRE(schema["$schema"].is_string());
        CHECK(schema["$schema"] == "https://json-schema.org/draft/2020-12/schema");
        REQUIRE(schema["type"].is_string());
        CHECK(schema["type"] == "object");
        CHECK(schema.dump().find(":true") == std::string::npos); // no boolean schema anywhere ("default":true aside, none here)
    }
    const auto demo = Arcane::Remote::SchemaFor(*Astra::GetMeta<RemoteDemoArgs>());
    CHECK(demo["properties"]["n"]["type"] == "integer");
    CHECK(demo["properties"]["position"]["type"] == "array");
    CHECK(demo["properties"]["position"]["items"]["type"] == "number");   // glm rewrite untouched
    const auto outer = Arcane::Remote::SchemaFor(*Astra::GetMeta<RemoteSchemaOuter>());
    CHECK(outer["properties"]["plain"]["type"] == "integer");
    CHECK(outer["$defs"]["RemoteSchemaInner"]["type"] == "object");      // the fallback's nested def
    CHECK(outer["$defs"]["RemoteSchemaInner"]["properties"]["n"]["default"] == 4);
    const auto list = Arcane::Remote::SchemaFor(*Astra::GetMeta<RemoteSchemaList>());
    CHECK(list["properties"]["names"]["type"] == "array");
    REQUIRE(list["properties"]["names"]["items"].is_object());
    CHECK_FALSE(list["properties"]["names"]["items"].contains("type"));  // any element, as :276 intends
    CHECK(list["properties"]["count"]["type"] == "integer");
}

#endif
