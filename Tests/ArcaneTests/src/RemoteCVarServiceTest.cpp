// Settings arc S7 (spec s9, s3.2): the transport-agnostic admin surface.
// ServerAdmin context; Protected is never readable through it; every set is
// handed to the injected audit sink with who, old, new and the outcome.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/RemoteCVarService.hpp>
#include <Json.hpp>

#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    struct AuditLog
    {
        std::vector<CVarAuditRecord> records;
        static void Sink(const CVarAuditRecord& r, void* user) { static_cast<AuditLog*>(user)->records.push_back(r); }
    };

    void Knob(CVarRegistry& reg, std::string_view name, CVarValue def, Audience audience, CVarFlags flags = CVarFlags::None)
    {
        CVarDesc d;
        d.name = name;
        d.type = def.type;
        d.defaultValue = def;
        d.flags = flags;
        d.help = "S7 remote knob.";
        d.module = "test-s7-remote";
        d.audience = audience;
        const CVarHandle h = reg.Register(d);
        INFO(reg.LastError());
        REQUIRE_FALSE(h.IsStale());
    }

    RemoteCVarRequest Req(std::string op, std::string name, std::string value = {})
    {
        return RemoteCVarRequest{ std::move(op), std::move(name), std::move(value), "admin-1" };
    }

    struct Fixture
    {
        CVarRegistry reg;
        AuditLog audit;
        RemoteCVarService service{ reg, &AuditLog::Sink, &audit };
        Fixture()
        {
            Knob(reg, "test.server.tick", CVarValue::Int32(60), Audience::Server);
            Knob(reg, "test.server.password", CVarValue::String("hunter2"), Audience::Server, CVarFlags::Protected);
            Knob(reg, "test.server.secretKnob", CVarValue::Bool(false), Audience::Server, CVarFlags::Hidden);
            Knob(reg, "test.serverx.other", CVarValue::Bool(true), Audience::Server);
            Knob(reg, "test.game.speed", CVarValue::Float32(1.0f), Audience::Game);
        }
    };
}

TEST_CASE("RemoteCVarService: get and set a Server setting as ServerAdmin; the set publishes and is audited", "[remote-cvar]")
{
    Fixture f;
    const RemoteCVarResponse got = f.service.Handle(Req("get", "test.server.tick"));
    CHECK(got.ok);
    CHECK(got.text == "test.server.tick = 60");

    const RemoteCVarResponse set = f.service.Handle(Req("set", "test.server.tick", "30"));
    CHECK(set.ok);
    CHECK(set.text == "test.server.tick = 30");
    CHECK(f.reg.Get(f.reg.Find("test.server.tick"))->AsInt32() == 30);   // published, not pending

    REQUIRE(f.audit.records.size() == 1);
    const CVarAuditRecord& r = f.audit.records.front();
    CHECK(r.name == "test.server.tick");
    CHECK(r.callerId == "admin-1");
    CHECK(r.oldValue == "60");
    CHECK(r.newValue == "30");
    CHECK(r.context == CVarContext::ServerAdmin);
    CHECK(r.verdict == PolicyVerdict::Allow);
    CHECK(r.unixMs > 0);
}

TEST_CASE("RemoteCVarService: a refused set changes nothing and is still audited as denied", "[remote-cvar]")
{
    Fixture f;
    const RemoteCVarResponse set = f.service.Handle(Req("set", "test.game.speed", "4"));   // Game, not Cheat
    CHECK_FALSE(set.ok);
    CHECK(f.reg.Get(f.reg.Find("test.game.speed"))->AsFloat32() == 1.0f);
    REQUIRE(f.audit.records.size() == 1);
    CHECK(f.audit.records.front().verdict == PolicyVerdict::Deny);
    CHECK(f.audit.records.front().newValue == "4");   // the attempted value
}

TEST_CASE("RemoteCVarService: Protected is never readable remotely -- get, explain and list hide it; a set is allowed and audited masked", "[remote-cvar]")
{
    Fixture f;
    const RemoteCVarResponse got = f.service.Handle(Req("get", "test.server.password"));
    CHECK_FALSE(got.ok);
    CHECK(got.text.find("protected") != std::string::npos);
    CHECK(got.text.find("hunter2") == std::string::npos);
    CHECK_FALSE(f.service.Handle(Req("explain", "test.server.password")).ok);

    const RemoteCVarResponse set = f.service.Handle(Req("set", "test.server.password", "correcthorse"));
    CHECK(set.ok);
    CHECK(set.text.find("correcthorse") == std::string::npos);
    CHECK(f.reg.Get(f.reg.Find("test.server.password"))->AsString() == "correcthorse");
    REQUIRE(f.audit.records.size() == 1);
    CHECK(f.audit.records.front().oldValue == "<protected>");
    CHECK(f.audit.records.front().newValue == "<protected>");

    const RemoteCVarResponse list = f.service.Handle(Req("list", "test.server"));
    CHECK(list.text.find("test.server.password = <protected>") != std::string::npos);
    CHECK(list.text.find("correcthorse") == std::string::npos);
}

TEST_CASE("RemoteCVarService: Hidden and unknown names answer unknown and are not audited", "[remote-cvar]")
{
    Fixture f;
    CHECK(f.service.Handle(Req("get", "test.server.secretKnob")).text == "unknown cvar 'test.server.secretKnob'");
    CHECK_FALSE(f.service.Handle(Req("set", "test.server.secretKnob", "true")).ok);
    CHECK_FALSE(f.service.Handle(Req("set", "no.such.cvar", "1")).ok);
    CHECK(f.audit.records.empty());
}

TEST_CASE("RemoteCVarService: list honours the dotted prefix boundary and is sorted; explain shows the history", "[remote-cvar]")
{
    Fixture f;
    const RemoteCVarResponse list = f.service.Handle(Req("list", "test.server"));
    CHECK(list.ok);
    CHECK(list.text.find("test.server.tick = 60") != std::string::npos);
    CHECK(list.text.find("test.serverx.other") == std::string::npos);
    CHECK(list.text.find("test.server.secretKnob") == std::string::npos);
    CHECK(list.text.find("test.server.password") < list.text.find("test.server.tick"));

    REQUIRE(f.service.Handle(Req("set", "test.server.tick", "45")).ok);
    const RemoteCVarResponse ex = f.service.Handle(Req("explain", "test.server.tick"));
    CHECK(ex.ok);
    CHECK(ex.text.find("Console") != std::string::npos);
    CHECK(f.service.Handle(Req("list", "nothing.here")).text == "(no cvars match 'nothing.here')");
}

TEST_CASE("RemoteCVarService: a command runs remotely only with ServerCanExecute; an unknown op is refused", "[remote-cvar]")
{
    Fixture f;
    REQUIRE(f.reg.RegisterCommand("test.flush", CVarFlags::ServerCanExecute, "Flush.", "test-s7-remote",
        [](std::string_view, void*) -> CommandResult { return { true, "flushed" }; }, nullptr));
    REQUIRE(f.reg.RegisterCommand("test.local", CVarFlags::None, "Local only.", "test-s7-remote",
        [](std::string_view, void*) -> CommandResult { return { true, "ran" }; }, nullptr));

    const RemoteCVarResponse ok = f.service.Handle(Req("set", "test.flush"));
    CHECK(ok.ok);
    CHECK(ok.text == "flushed");
    const RemoteCVarResponse refused = f.service.Handle(Req("get", "test.local"));
    CHECK_FALSE(refused.ok);
    CHECK(refused.text.find("ServerCanExecute") != std::string::npos);
    CHECK(f.audit.records.size() == 1);   // the command run; the refusal named no value

    CHECK_FALSE(f.service.Handle(Req("drop", "test.server.tick")).ok);
    CHECK_FALSE(f.service.Handle(Req("get", "")).ok);
}

TEST_CASE("RemoteCVarService: cvar_explain via the command path does not leak a Protected value or its history", "[remote-cvar]")
{
    Fixture f;
    REQUIRE(f.service.Handle(Req("set", "test.server.password", "correcthorse")).ok);

    const RemoteCVarResponse leaked = f.service.Handle(Req("set", "cvar_explain", "test.server.password"));
    CHECK_FALSE(leaked.ok);
    CHECK(leaked.text.find("hunter2") == std::string::npos);
    CHECK(leaked.text.find("correcthorse") == std::string::npos);
    CHECK(leaked.text.find("Default") == std::string::npos);
    CHECK(leaked.text.find("Console") == std::string::npos);
}

TEST_CASE("RemoteCVarService: list applies ServerAdmin visibility; an Editor-only Protected name is absent", "[remote-cvar]")
{
    Fixture f;
    Knob(f.reg, "editor.secret", CVarValue::String("editorhorse"), Audience::Editor, CVarFlags::Protected);

    const RemoteCVarResponse list = f.service.Handle(Req("list", "editor"));
    CHECK(list.text.find("editor.secret") == std::string::npos);
    CHECK(list.text.find("editorhorse") == std::string::npos);

    const RemoteCVarResponse got = f.service.Handle(Req("get", "editor.secret"));
    CHECK_FALSE(got.ok);
    CHECK(got.text.find("editorhorse") == std::string::npos);
}

TEST_CASE("RemoteCVarService: a set with a missing value on a known name is audited as denied", "[remote-cvar]")
{
    Fixture f;
    const RemoteCVarResponse missing = f.service.Handle(Req("set", "test.server.tick"));
    CHECK_FALSE(missing.ok);
    REQUIRE(f.audit.records.size() == 1);
    CHECK(f.audit.records.front().name == "test.server.tick");
    CHECK(f.audit.records.front().callerId == "admin-1");
    CHECK(f.audit.records.front().oldValue == "60");
    CHECK(f.audit.records.front().newValue.empty());
    CHECK(f.audit.records.front().context == CVarContext::ServerAdmin);
    CHECK(f.audit.records.front().verdict == PolicyVerdict::Deny);

    const RemoteCVarResponse prot = f.service.Handle(Req("set", "test.server.password"));
    CHECK_FALSE(prot.ok);
    REQUIRE(f.audit.records.size() == 2);
    CHECK(f.audit.records.back().name == "test.server.password");
    CHECK(f.audit.records.back().oldValue == "<protected>");
    CHECK(f.audit.records.back().newValue.empty());
    CHECK(f.audit.records.back().verdict == PolicyVerdict::Deny);
}

TEST_CASE("RemoteCVarService: get and explain on a ServerCanExecute command do not run it", "[remote-cvar]")
{
    Fixture f;
    struct Flag { bool ran = false; } flag;
    REQUIRE(f.reg.RegisterCommand("test.ping", CVarFlags::ServerCanExecute, "Ping.", "test-s7-remote",
        [](std::string_view, void* user) -> CommandResult {
            static_cast<Flag*>(user)->ran = true;
            return { true, "pong" };
        }, &flag));

    const RemoteCVarResponse got = f.service.Handle(Req("get", "test.ping"));
    CHECK_FALSE(got.ok);
    CHECK_FALSE(flag.ran);
    CHECK(got.text.find("pong") == std::string::npos);

    const RemoteCVarResponse explained = f.service.Handle(Req("explain", "test.ping"));
    CHECK_FALSE(explained.ok);
    CHECK_FALSE(flag.ran);

    const RemoteCVarResponse set = f.service.Handle(Req("set", "test.ping"));
    CHECK(set.ok);
    CHECK(flag.ran);
    CHECK(set.text == "pong");
}

TEST_CASE("Editor context can read an Editor-audience cvar the ServerAdmin list hides", "[remote][cvar]") {
    CVarRegistry reg;
    Knob(reg, "editor.visible", CVarValue::Int32(3), Audience::Editor);
    Knob(reg, "editor.secret", CVarValue::String("editorhorse"), Audience::Editor, CVarFlags::Protected);
    RemoteCVarService svc(reg);
    RemoteCVarRequest req{"get", "editor.visible", "", "remote-test", CVarContext::Editor};
    const RemoteCVarResponse res = svc.Handle(req);
    CHECK(res.ok);
    CHECK(res.text.find('3') != std::string::npos);
    CHECK(res.valueJson == "3");
    RemoteCVarRequest hidden{"get", "editor.visible", "", "admin-1"};
    CHECK_FALSE(svc.Handle(hidden).ok);
}

TEST_CASE("Protected get is masked in the Editor context", "[remote][cvar]") {
    CVarRegistry reg;
    Knob(reg, "editor.secret", CVarValue::String("editorhorse"), Audience::Editor, CVarFlags::Protected);
    RemoteCVarService svc(reg);
    RemoteCVarRequest req{"get", "editor.secret", "", "remote-test", CVarContext::Editor};
    const RemoteCVarResponse res = svc.Handle(req);
    CHECK_FALSE(res.ok);
    CHECK(res.text.find("protected") != std::string::npos);
    CHECK(res.text.find("editorhorse") == std::string::npos);
    CHECK(res.valueJson == "null");
}

TEST_CASE("explain returns structured history and still masks Protected", "[remote][cvar]") {
    CVarRegistry reg;
    Knob(reg, "test.server.tick", CVarValue::Int32(60), Audience::Server);
    Knob(reg, "test.server.password", CVarValue::String("hunter2"), Audience::Server, CVarFlags::Protected);
    RemoteCVarService svc(reg);
    RemoteCVarRequest set{"set", "test.server.tick", "45", "remote-test", CVarContext::ServerAdmin};
    REQUIRE(svc.Handle(set).ok);
    RemoteCVarRequest exp{"explain", "test.server.tick", "", "remote-test", CVarContext::ServerAdmin};
    const RemoteCVarResponse res = svc.Handle(exp);
    CHECK(res.ok);
    CHECK(res.text.find("Console") != std::string::npos);
    CHECK(res.setBy == "Console");
    CHECK(res.valueJson == "45");
    const auto history = nlohmann::json::parse(res.historyJson);
    REQUIRE(history.is_array());
    REQUIRE(history.size() >= 2);
    CHECK(history.front()["by"] == "Default");
    CHECK(history.front()["value"] == 60);
    CHECK(history.front()["module"] == "");
    CHECK(history.back()["by"] == "Console");
    CHECK(history.back()["value"] == 45);
    CHECK(history.back()["module"] == "");
    CHECK(res.historyJson.find("remote-test") == std::string::npos);
    RemoteCVarRequest prot{"explain", "test.server.password", "", "remote-test", CVarContext::Editor};
    const RemoteCVarResponse masked = svc.Handle(prot);
    CHECK_FALSE(masked.ok);
    CHECK(masked.text.find("hunter2") == std::string::npos);
    CHECK(masked.valueJson == "null");
    CHECK(masked.historyJson.empty());
}

TEST_CASE("Editor context explains a non-protected Editor-audience cvar", "[remote][cvar]") {
    CVarRegistry reg;
    AuditLog audit;
    Knob(reg, "editor.visible", CVarValue::Int32(3), Audience::Editor);
    RemoteCVarService svc(reg, &AuditLog::Sink, &audit);
    RemoteCVarRequest set{"set", "editor.visible", "9", "remote-test", CVarContext::Editor};
    REQUIRE(svc.Handle(set).ok);
    REQUIRE(audit.records.size() == 1);
    CHECK(audit.records.front().context == CVarContext::Editor);
    RemoteCVarRequest exp{"explain", "editor.visible", "", "remote-test", CVarContext::Editor};
    const RemoteCVarResponse res = svc.Handle(exp);
    CHECK(res.ok);
    CHECK(res.text.find("unknown") == std::string::npos);
    CHECK(res.valueJson == "9");
    CHECK(res.setBy == "Console");
    CHECK(res.audience == "Editor");
}

TEST_CASE("omitted context stays ServerAdmin", "[remote][cvar]") {
    RemoteCVarRequest req{"get", "test.server.tick", "", "tester"};
    CHECK(req.context == CVarContext::ServerAdmin);
}

TEST_CASE("ToString(SetBy) names every rung", "[remote][cvar]") {
    CHECK(std::string(Arcane::ToString(SetBy::Default)) == "Default");
    CHECK(std::string(Arcane::ToString(SetBy::EngineConfig)) == "EngineConfig");
    CHECK(std::string(Arcane::ToString(SetBy::Plugin)) == "Plugin");
    CHECK(std::string(Arcane::ToString(SetBy::Project)) == "Project");
    CHECK(std::string(Arcane::ToString(SetBy::EditorUser)) == "EditorUser");
    CHECK(std::string(Arcane::ToString(SetBy::User)) == "User");
    CHECK(std::string(Arcane::ToString(SetBy::CommandLine)) == "CommandLine");
    CHECK(std::string(Arcane::ToString(SetBy::Code)) == "Code");
    CHECK(std::string(Arcane::ToString(SetBy::Console)) == "Console");
}
