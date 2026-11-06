#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/CvarCommands.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Discover.hpp>
#include <Servitor/FanOut.hpp>
#include <Servitor/Verbs.hpp>
#include <Json.hpp>
#include "RemoteTestHttp.hpp"
#include "RemoteTestSupport.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir

#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

static void AddBool(Arcane::CVarRegistry& reg, const char* name, bool def,
                     Arcane::Audience audience, Arcane::ApplyMode apply,
                     Arcane::CVarFlags flags = Arcane::CVarFlags::None) {
    Arcane::CVarDesc d;
    d.name = name;
    d.type = Arcane::CVarType::Bool;
    d.defaultValue = Arcane::CVarValue::Bool(def);
    d.audience = audience;
    d.apply = apply;
    d.flags = flags;
    d.help = name;
    REQUIRE_FALSE(reg.Register(d).IsStale());
}

TEST_CASE("Protected get in the Editor context is cvar.protected exit 3", "[remote][cvar]") {
    Arcane::CVarRegistry reg;
    Arcane::CVarDesc secret;
    secret.name = "editor.secret";
    secret.type = Arcane::CVarType::String;
    secret.defaultValue = Arcane::CVarValue::String("editorhorse");
    secret.audience = Arcane::Audience::Editor;
    secret.flags = Arcane::CVarFlags::Protected;
    secret.help = "editor.secret";
    REQUIRE_FALSE(reg.Register(secret).IsStale());
    Arcane::Remote::CvarCommandUser user;
    user.registry = &reg;
    user.context = Arcane::CVarContext::Editor;
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetCvarCommandUser, &user);
    Arcane::Remote::RegisterCvarCommands();
    Arcane::Remote::Call call;
    call.command = "cvar.get";
    call.args = nlohmann::json{{"name", "editor.secret"}};
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(env.ok);
    CHECK(env.data["value"].is_null());
    CHECK(env.data["masked"] == "<protected>");
    REQUIRE_FALSE(env.errors.empty());
    CHECK(env.errors[0].code == "cvar.protected");
    CHECK(env.errors[0].message.find("protected") != std::string::npos);
    CHECK(env.errors[0].message.find("editorhorse") == std::string::npos);
    CHECK(Arcane::Remote::ExitFromHost(env) == 3);
    CHECK_FALSE(env.data.contains("result"));
}

TEST_CASE("explain of render.vsync reports Restart and PlayerSafe", "[remote][cvar]") {
    Arcane::CVarRegistry reg;
    AddBool(reg, "render.vsync", true, Arcane::Audience::PlayerSafe, Arcane::ApplyMode::Restart);
    Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::LocalHost};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetCvarCommandUser, &user);
    Arcane::Remote::RegisterCvarCommands();
    // The case's name is what it tests (ruling F18): a real cvar.explain.
    Arcane::Remote::Call explain;
    explain.command = "cvar.explain";
    explain.args = nlohmann::json{{"name", "render.vsync"}};
    const auto explained = Arcane::Remote::CommandRegistry::Get().Invoke(explain).envelope;
    REQUIRE(explained.ok);
    CHECK(explained.data["apply"] == "Restart");
    CHECK(explained.data["audience"] == "PlayerSafe");
    CHECK(explained.data["setBy"] == "Default");
    CHECK(explained.data["published"] == true);
    REQUIRE(explained.data["history"].is_array());
    CHECK(explained.data["history"].front()["by"] == "Default");
    Arcane::Remote::Call set;
    set.command = "cvar.set";
    set.args = nlohmann::json{{"name", "render.vsync"}, {"value", false}};
    const auto wrote = Arcane::Remote::CommandRegistry::Get().Invoke(set).envelope;
    CHECK(wrote.ok);
    CHECK(wrote.data["previous"] == true);     // from the leading get
    CHECK(wrote.data["value"] == false);       // from the trailing explain
    CHECK(wrote.data["type"] == "bool");
    CHECK(wrote.data["apply"] == "Restart");
    CHECK(wrote.data["audience"] == "PlayerSafe");
    CHECK(wrote.data["setBy"] == "Console");
    CHECK(wrote.data["context"] == "LocalHost");
    CHECK(wrote.undo->undoable == false);
    CHECK(wrote.undo->reason == "not-undoable-command");
    const auto dumped = Arcane::Remote::ToJson(wrote);
    CHECK(dumped["undo"]["reason"] == "not-undoable-command");
    CHECK_FALSE(dumped.contains("hostFrame"));
    Arcane::Remote::Call bad;
    bad.command = "cvar.set";
    bad.args = nlohmann::json{{"name", "render.vsync"}, {"value", "nope"}};
    const auto mismatch = Arcane::Remote::CommandRegistry::Get().Invoke(bad).envelope;
    CHECK(mismatch.errors[0].code == "cvar.type-mismatch");
    CHECK(Arcane::Remote::ExitFromHost(mismatch) == 6);
    Arcane::Remote::Call missing;
    missing.command = "cvar.get";
    missing.args = nlohmann::json{{"name", "no.such.cvar"}};
    const auto unknown = Arcane::Remote::CommandRegistry::Get().Invoke(missing).envelope;
    CHECK(unknown.errors[0].code == "cvar.unknown");
    CHECK(Arcane::Remote::ExitFromHost(unknown) == 6);
}

TEST_CASE("cvar commands with no user answer host.unavailable", "[remote][cvar]") {
    Arcane::Remote::RegisterCvarCommands();
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetCvarCommandUser, nullptr);
    Arcane::Remote::Call get;
    get.command = "cvar.get";
    get.args = nlohmann::json{{"name", "render.vsync"}};
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(get).envelope;
    CHECK(env.errors[0].code == "host.unavailable");
    CHECK(Arcane::Remote::ExitFromHost(env) == 5);
}

TEST_CASE("cvar.get, cvar.list and a protected set mask the secret; cvar.set is a drive", "[remote][cvar]") {
    Arcane::CVarRegistry reg;
    AddBool(reg, "render.vsync", true, Arcane::Audience::PlayerSafe, Arcane::ApplyMode::Restart);
    Arcane::CVarDesc secret;
    secret.name = "render.secret";
    secret.type = Arcane::CVarType::String;
    secret.defaultValue = Arcane::CVarValue::String("gamehorse");
    secret.audience = Arcane::Audience::Game;
    secret.flags = Arcane::CVarFlags::Protected;
    secret.help = "render.secret";
    REQUIRE_FALSE(reg.Register(secret).IsStale());
    Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::Editor};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetCvarCommandUser, &user);
    Arcane::Remote::RegisterCvarCommands();
    Arcane::Remote::RegisterCvarCommands();   // a second call is a no-op (ruling F11)

    auto& registry = Arcane::Remote::CommandRegistry::Get();
    const auto* setDesc = registry.Find("cvar.set");
    REQUIRE(setDesc != nullptr);
    CHECK(setDesc->access == Arcane::Remote::Access::Drive);
    CHECK(setDesc->mutates == Arcane::Remote::Mutates::Preferences);
    for (const char* observe : {"cvar.get", "cvar.list", "cvar.explain"}) {
        const auto* d = registry.Find(observe);
        REQUIRE(d != nullptr);
        CHECK(d->access == Arcane::Remote::Access::Observe);
        CHECK_FALSE(d->needsFrame);
    }

    Arcane::Remote::Call get;
    get.command = "cvar.get";
    get.args = nlohmann::json{{"name", "render.vsync"}};
    const auto got = registry.Invoke(get).envelope;
    REQUIRE(got.ok);
    CHECK(got.data["name"] == "render.vsync");
    CHECK(got.data["value"] == true);
    CHECK(got.data["type"] == "bool");

    Arcane::Remote::Call list;
    list.command = "cvar.list";
    list.args = nlohmann::json{{"prefix", "render"}};
    const auto listed = registry.Invoke(list).envelope;
    REQUIRE(listed.ok);
    REQUIRE(listed.data["cvars"].is_array());
    CHECK(listed.data["cvars"].size() == 2);
    CHECK(listed.data.dump().find("gamehorse") == std::string::npos);
    bool sawMasked = false;
    for (const auto& row : listed.data["cvars"]) {
        if (row["name"] == "render.secret") {
            sawMasked = row["masked"] == "<protected>" && row["text"].is_null();
        } else {
            CHECK(row["name"] == "render.vsync");
            CHECK(row["text"] == "true");
        }
    }
    CHECK(sawMasked);
    list.args = nlohmann::json{{"prefix", "nothing.here"}};
    const auto empty = registry.Invoke(list).envelope;
    REQUIRE(empty.ok);
    CHECK(empty.data["cvars"].empty());

    // A protected set proceeds, and nothing in the envelope carries either value.
    Arcane::Remote::Call set;
    set.command = "cvar.set";
    set.args = nlohmann::json{{"name", "render.secret"}, {"value", "newhorse"}};
    const auto wrote = registry.Invoke(set).envelope;
    CHECK(wrote.ok);
    CHECK(wrote.data["previous"].is_null());
    CHECK(wrote.data["value"].is_null());
    CHECK(wrote.data["masked"] == "<protected>");
    CHECK_FALSE(wrote.data.contains("setBy"));
    const std::string dumped = Arcane::Remote::ToJson(wrote).dump();
    CHECK(dumped.find("gamehorse") == std::string::npos);
    CHECK(dumped.find("newhorse") == std::string::npos);

    // Argument refusals are usage (exit 2).
    Arcane::Remote::Call extra;
    extra.command = "cvar.get";
    extra.args = nlohmann::json{{"name", "render.vsync"}, {"bogus", 1}};
    CHECK(registry.Invoke(extra).envelope.errors[0].code == "arg.unknown");
    Arcane::Remote::Call noName;
    noName.command = "cvar.explain";
    noName.args = nlohmann::json::object();
    const auto refused = registry.Invoke(noName).envelope;
    CHECK(refused.errors[0].code == "arg.missing");
    CHECK(Arcane::Remote::ExitFromHost(refused) == 2);
    Arcane::Remote::Call noValue;
    noValue.command = "cvar.set";
    noValue.args = nlohmann::json{{"name", "render.vsync"}};
    CHECK(registry.Invoke(noValue).envelope.errors[0].code == "arg.missing");
}

TEST_CASE("HostSession serves cvar.* in its context and releases only its own user", "[remote][cvar]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetCvarCommandUser, nullptr);
    Arcane::Test::TempDir root("remote-cvar-session");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root.path;
    Arcane::Remote::Call list;
    list.command = "cvar.list";
    list.args = nlohmann::json{{"prefix", "no.such.prefix"}};
    {
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        const auto listed = Arcane::Remote::CommandRegistry::Get().Invoke(list).envelope;
        REQUIRE(listed.ok);
        CHECK(listed.data["context"] == "LocalHost");   // the session's {CVarRegistry::Get(), cvarContext}
        session.Stop("test");
        CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(list).envelope.errors[0].code == "host.unavailable");
    }
    // A user installed after the session started is not the session's to release.
    Arcane::CVarRegistry reg;
    Arcane::Remote::CvarCommandUser other{&reg, Arcane::CVarContext::ServerAdmin};
    {
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        Arcane::Remote::SetCvarCommandUser(&other);
        session.Stop("test");
    }
    const auto kept = Arcane::Remote::CommandRegistry::Get().Invoke(list).envelope;
    REQUIRE(kept.ok);
    CHECK(kept.data["context"] == "ServerAdmin");
}

TEST_CASE("servitor cvar verbs build their args and refuse unknown flags", "[servitor][cvar]") {
    std::size_t consumed = 0;
    const std::vector<std::string> getTokens = {"cvar", "get", "render.vsync"};
    const Servitor::VerbRow* get = Servitor::MatchVerb(getTokens, &consumed);
    REQUIRE(get != nullptr);
    CHECK(consumed == 2);
    CHECK(std::string(get->command) == "cvar.get");
    nlohmann::json args;
    std::string error;
    REQUIRE(get->args(std::vector<std::string>{"render.vsync"}, args, &error));
    CHECK(args == nlohmann::json{{"name", "render.vsync"}});
    CHECK_FALSE(get->args(std::vector<std::string>{}, args, &error));
    CHECK_FALSE(get->args(std::vector<std::string>{"a", "b"}, args, &error));

    const Servitor::VerbRow* set = Servitor::FindVerb("cvar set");
    REQUIRE(set != nullptr);
    CHECK(std::string(set->command) == "cvar.set");
    REQUIRE(set->args(std::vector<std::string>{"render.vsync", "false"}, args, &error));
    CHECK(args == nlohmann::json{{"name", "render.vsync"}, {"value", "false"}});   // console text, as typed
    REQUIRE(set->args(std::vector<std::string>{"physics.gravity", "-9.81"}, args, &error));
    CHECK(args["value"] == "-9.81");
    CHECK_FALSE(set->args(std::vector<std::string>{"render.vsync"}, args, &error));

    const Servitor::VerbRow* list = Servitor::FindVerb("cvar list");
    REQUIRE(list != nullptr);
    REQUIRE(list->args(std::vector<std::string>{}, args, &error));
    CHECK(args == nlohmann::json::object());
    REQUIRE(list->args(std::vector<std::string>{"render"}, args, &error));
    CHECK(args == nlohmann::json{{"prefix", "render"}});

    const Servitor::VerbRow* explain = Servitor::FindVerb("cvar explain");
    REQUIRE(explain != nullptr);
    REQUIRE(explain->args(std::vector<std::string>{"render.vsync"}, args, &error));
    CHECK(args == nlohmann::json{{"name", "render.vsync"}});

    // No cvar verb declares a flag: any "--x" after the verb is usage exit 2.
    for (const Servitor::VerbRow* row : {get, set, list, explain}) {
        const std::vector<std::string> rest = {"render.vsync", "--bogus"};
        CHECK(Servitor::SplitVerbTokens(*row, rest, nullptr).has_value());
        CHECK_FALSE(row->args(rest, args, &error));
    }
    const char* argv[] = {"servitor", "cvar", "get", "render.vsync", "--bogus"};
    CHECK(Servitor::Run(5, const_cast<char**>(argv)) == 2);

    // cvar verbs post a call, so the client forwards --world and --read-only
    // and the HOST decides (Rule 3): --read-only refuses cvar.set at the gate,
    // and --world is refused as arg.inapplicable because no cvar command is
    // world-scoped (both asserted over a live session in "a read-only session
    // refuses cvar.set ..." below).
    Servitor::GlobalOptions opt;
    opt.world = "main";
    opt.readOnly = true;
    for (const Servitor::VerbRow* row : {get, set, list, explain})
        CHECK_FALSE(Servitor::UnusedGlobal(*row, opt).has_value());
    auto& registry = Arcane::Remote::CommandRegistry::Get();
    Arcane::Remote::RegisterCvarCommands();
    for (const char* command : {"cvar.get", "cvar.set", "cvar.list", "cvar.explain"}) {
        const auto* d = registry.Find(command);
        REQUIRE(d != nullptr);
        CHECK_FALSE(d->worldScoped);
    }
}

TEST_CASE("--all cvar set needs --yes; --all cvar get does not", "[servitor][cvar]") {
    Servitor::Scan scan;
    for (std::uint32_t pid : {1u, 2u}) {
        Servitor::LiveHost h;
        h.descriptor.pid = pid;
        h.descriptor.hostKind = pid == 1 ? Arcane::Remote::HostKind::Runtime : Arcane::Remote::HostKind::Server;
        h.descriptor.protocol = 1;
        scan.live.push_back(h);
    }
    struct Seen { std::mutex m; std::vector<std::string> commands; } seen;
    auto post = [](const Arcane::Remote::Descriptor&, const nlohmann::json& call, Arcane::Remote::Envelope* out,
                   void* user) {
        auto& s = *static_cast<Seen*>(user);
        std::lock_guard lock(s.m);
        s.commands.push_back(call.value("command", std::string{}));
        *out = Arcane::Remote::MakeOk(call.value("command", std::string{}), nlohmann::json::object(), {});
        return 0;
    };
    CHECK(Servitor::IsDriveCommand(nlohmann::json{{"command", "cvar.set"}}));
    Servitor::GlobalOptions opt;
    opt.all = true;
    Servitor::FanOutResult result;
    const std::vector<std::string> setRest = {"render.vsync", "false"};
    CHECK(Servitor::FanOutVerb(*Servitor::FindVerb("cvar set"), setRest, opt, scan, post, &seen, &result) == 2);
    CHECK(seen.commands.empty());
    CHECK(result.text.find("arg.inapplicable") != std::string::npos);
    opt.yes = true;
    CHECK(Servitor::FanOutVerb(*Servitor::FindVerb("cvar set"), setRest, opt, scan, post, &seen, &result) == 0);
    CHECK(seen.commands == std::vector<std::string>{"cvar.set", "cvar.set"});

    seen.commands.clear();
    opt.yes = false;
    const std::vector<std::string> getRest = {"render.vsync"};
    CHECK(Servitor::FanOutVerb(*Servitor::FindVerb("cvar get"), getRest, opt, scan, post, &seen, &result) == 0);
    CHECK(seen.commands == std::vector<std::string>{"cvar.get", "cvar.get"});
}

// --- Task 20 fix round 1 ---

TEST_CASE("cvar refusal texts map to their codes; refused-weaker and refused are exit 6", "[remote][cvar]") {
    using Arcane::Remote::CvarRefusalCode;
    CHECK(std::string(CvarRefusalCode("refused: a stronger source holds render.vsync")) == "cvar.refused-weaker");
    CHECK(std::string(CvarRefusalCode("rejected")) == "cvar.refused");
    CHECK(std::string(CvarRefusalCode("render.secret is protected: never readable remotely")) == "cvar.protected");
    CHECK(std::string(CvarRefusalCode("denied: 'render.secret' is protected")) == "cvar.protected");
    CHECK(std::string(CvarRefusalCode("denied: 'x' by the game's policy")) == "permission.denied");
    CHECK(std::string(CvarRefusalCode("denied")) == "permission.denied");
    CHECK(std::string(CvarRefusalCode("unknown cvar 'protected.thing'")) == "cvar.unknown");
    CHECK(std::string(CvarRefusalCode("command 'x' runs only via set")) == "cvar.unknown");
    CHECK(std::string(CvarRefusalCode("expected true or false")) == "cvar.type-mismatch");
    CHECK(std::string(CvarRefusalCode("set x: missing value")) == "arg.missing");
    for (const char* code : {"cvar.refused-weaker", "cvar.refused"}) {
        INFO(code);
        const auto env = Arcane::Remote::MakeError("cvar.set", code, "m", "r");
        CHECK(Arcane::Remote::ExitFromHost(env) == 6);
    }
}

static int g_consoleRuns = 0;
static Arcane::CommandResult CountRun(std::string_view, void*) {
    ++g_consoleRuns;
    return {true, "ran"};
}

TEST_CASE("cvar.set on a console command is cvar.unknown and never runs it", "[remote][cvar]") {
    Arcane::CVarRegistry reg;
    // ServerCanExecute: RemoteCVarService's op set WOULD run it; cvar.set must stop first.
    REQUIRE(reg.RegisterCommand("tests.remote.flush", Arcane::CVarFlags::ServerCanExecute, "Flush.", "cvar-test",
                                &CountRun, nullptr));
    for (const auto context : {Arcane::CVarContext::Editor, Arcane::CVarContext::ServerAdmin}) {
        g_consoleRuns = 0;
        Arcane::Remote::CvarCommandUser user{&reg, context};
        RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetCvarCommandUser, &user);
        Arcane::Remote::RegisterCvarCommands();
        Arcane::Remote::Call set;
        set.command = "cvar.set";
        set.args = nlohmann::json{{"name", "tests.remote.flush"}, {"value", "now"}};
        const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(set).envelope;
        CHECK_FALSE(env.ok);
        REQUIRE_FALSE(env.errors.empty());
        CHECK(env.errors[0].code == "cvar.unknown");
        CHECK(env.errors[0].message.find("console command") != std::string::npos);
        CHECK(Arcane::Remote::ExitFromHost(env) == 6);
        CHECK(g_consoleRuns == 0);
    }
}

TEST_CASE("cvar.set of a Game cvar outside the Editor is permission.denied exit 3", "[remote][cvar]") {
    Arcane::CVarRegistry reg;
    AddBool(reg, "physics.debugDraw", false, Arcane::Audience::Game, Arcane::ApplyMode::Live);
    Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::LocalHost};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetCvarCommandUser, &user);
    Arcane::Remote::RegisterCvarCommands();
    Arcane::Remote::Call set;
    set.command = "cvar.set";
    set.args = nlohmann::json{{"name", "physics.debugDraw"}, {"value", true}};
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(set).envelope;
    CHECK_FALSE(env.ok);
    REQUIRE_FALSE(env.errors.empty());
    CHECK(env.errors[0].code == "permission.denied");
    CHECK(Arcane::Remote::ExitFromHost(env) == 3);
    const auto v = reg.Get(reg.Find("physics.debugDraw"));
    REQUIRE(v.has_value());
    CHECK(v->AsBool() == false);   // nothing was written
}

static std::vector<Arcane::Remote::WorldInfo> MainWorld(void*) {
    Arcane::Remote::WorldInfo w;
    w.name = "main";
    w.role = "Standalone";
    return {w};
}

// One /call through a live session's listener while the "host loop" drains.
static Arcane::Remote::Envelope PostLive(Arcane::Remote::HostSession& session, const std::string& token,
                                         const std::string& body) {
    const std::uint16_t port = session.Port();
    auto reply = std::async(std::launch::async, [port, token, body] {
        return RemoteTest::HttpExchange(port, token, "", "POST /remote/v1/call HTTP/1.1", body);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (reply.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        session.DrainFrame(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(reply.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const std::string raw = reply.get();
    const auto split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    return Arcane::Remote::ParseEnvelope(nlohmann::json::parse(raw.substr(split + 4)));
}

TEST_CASE("a read-only session refuses cvar.set; --world is refused on cvar.* and kept on world.list", "[remote][cvar]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetCvarCommandUser, nullptr);
    Arcane::Test::TempDir root("remote-cvar-readonly");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root.path;
    cfg.readOnly = true;
    cfg.worlds.fn = &MainWorld;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);

    const auto set = PostLive(session, desc->token,
        R"({"id":"s1","command":"cvar.set","args":{"name":"render.vsync","value":"false"}})");
    CHECK_FALSE(set.ok);
    REQUIRE_FALSE(set.errors.empty());
    CHECK(set.errors[0].code == "permission.read-only");
    CHECK(Arcane::Remote::ExitFromHost(set) == 3);
    CHECK(set.timing.ranMs == 0.0);   // the gate refused it: never invoked

    const auto get = PostLive(session, desc->token,
        R"({"id":"g1","command":"cvar.get","args":{"name":"render.vsync"},"options":{"world":"main"}})");
    CHECK_FALSE(get.ok);
    REQUIRE_FALSE(get.errors.empty());
    CHECK(get.errors[0].code == "arg.inapplicable");
    CHECK(get.errors[0].remediation.find("not world-scoped") != std::string::npos);
    CHECK(Arcane::Remote::ExitFromHost(get) == 2);
    CHECK(get.command == "cvar.get");

    const auto worlds = PostLive(session, desc->token,
        R"({"id":"w1","command":"world.list","args":{},"options":{"world":"main"}})");
    CHECK(worlds.ok);
    CHECK(Arcane::Remote::ExitFromHost(worlds) == 0);
    session.Stop("test");
}

#endif
