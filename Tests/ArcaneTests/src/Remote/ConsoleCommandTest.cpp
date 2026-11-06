#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/ConsoleCommands.hpp>
#include <Arcane/Remote/Commands/CvarCommands.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/HostSession.hpp>
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
#include <string>
#include <thread>
#include <vector>

#if !defined(ARC_BUILD_DIST)

static int g_flush = 0;
static Arcane::CommandResult FlushCmd(std::string_view, void*) {
    ++g_flush;
    return {true, "flushed"};
}

TEST_CASE("console.exec runs test.flush and refuses cvar_explain", "[remote]") {
    g_flush = 0;
    Arcane::CVarRegistry reg;
    REQUIRE(reg.RegisterCommand("test.flush", Arcane::CVarFlags::ServerCanExecute,
                                "flush", "remote-test", &FlushCmd, nullptr));
    Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::ServerAdmin};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, &user);
    Arcane::Remote::RegisterConsoleCommand();
    Arcane::Remote::Call ok;
    ok.command = "console.exec";
    ok.callerId = "remote-test";
    ok.args = nlohmann::json{{"line", "test.flush"}};
    const auto ran = Arcane::Remote::CommandRegistry::Get().Invoke(ok).envelope;
    CHECK(ran.ok);
    CHECK(ran.data["text"] == "flushed");
    CHECK(g_flush == 1);
    CHECK(ran.undo->reason == "not-undoable-command");
    Arcane::Remote::Call denied;
    denied.command = "console.exec";
    denied.args = nlohmann::json{{"line", "cvar_explain render.vsync"}};
    const auto refused = Arcane::Remote::CommandRegistry::Get().Invoke(denied).envelope;
    CHECK_FALSE(refused.ok);
    CHECK(refused.errors[0].code == "command.unavailable");
    CHECK(g_flush == 1);
}

static int g_secret = 0;
static Arcane::CommandResult SecretCmd(std::string_view, void*) {
    ++g_secret;
    return {true, "ran"};
}

TEST_CASE("console.exec refuses a command without ServerCanExecute", "[remote]") {
    g_secret = 0;
    Arcane::CVarRegistry reg;
    REQUIRE(reg.RegisterCommand("test.secret", Arcane::CVarFlags::None,
                                "not remote", "remote-test", &SecretCmd, nullptr));
    Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::ServerAdmin};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, &user);
    Arcane::Remote::RegisterConsoleCommand();
    Arcane::Remote::Call call;
    call.command = "console.exec";
    call.callerId = "remote-test";
    call.args = nlohmann::json{{"line", "test.secret now"}};
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "permission.denied");
    CHECK(env.errors[0].message.find("not remotely executable") != std::string::npos);
    CHECK(Arcane::Remote::ExitFromHost(env) == 3);
    CHECK(g_secret == 0);
}

// --- Task 21: the rest of the console.exec contract ---

static Arcane::Remote::Envelope Exec(const std::string& line, const std::string& callerId = "remote-test") {
    Arcane::Remote::Call call;
    call.command = "console.exec";
    call.callerId = callerId;
    call.args = nlohmann::json{{"line", line}};
    return Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
}

static std::string g_seenCaller;
static Arcane::CVarContext g_seenContext = Arcane::CVarContext::Editor;
static std::string g_seenArgs;
static Arcane::CommandResult EchoCmd(std::string_view args, void*) {
    g_seenArgs = std::string(args);
    return {true, "echo " + std::string(args)};
}
static int g_failed = 0;
static Arcane::CommandResult FailingCmd(std::string_view, void*) {
    ++g_failed;
    return {false, "usage: test.fail <n>"};
}
static int g_cheat = 0;
static Arcane::CommandResult CheatCmd(std::string_view, void*) {
    ++g_cheat;
    return {true, "cheated"};
}
static int g_hidden = 0;
static Arcane::CommandResult HiddenCmd(std::string_view, void*) {
    ++g_hidden;
    return {true, "hidden ran"};
}

// The caller the command's Execute is dispatched for: the policy sees it.
static Arcane::PolicyVerdict RecordCaller(const Arcane::CVarInfo&, const Arcane::CVarRequest& request, void*) {
    if (request.caller != nullptr)
        g_seenCaller = std::string(request.caller->id);
    g_seenContext = request.context;
    return Arcane::PolicyVerdict::Default;
}

TEST_CASE("console.exec passes the line's arguments and answers data {line, text, context}", "[remote][console]") {
    Arcane::CVarRegistry reg;
    REQUIRE(reg.RegisterCommand("test.echo", Arcane::CVarFlags::ServerCanExecute, "echo", "remote-test", &EchoCmd,
                                nullptr));
    Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::ServerAdmin};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, &user);
    Arcane::Remote::RegisterConsoleCommand();
    g_seenArgs.clear();
    const auto env = Exec("  test.echo a b  ");
    REQUIRE(env.ok);
    CHECK(env.errors.empty());
    CHECK(g_seenArgs == "a b");
    CHECK(env.data["text"] == "echo a b");
    CHECK(env.data["line"] == "test.echo a b");
    CHECK(env.data["context"] == "ServerAdmin");
    CHECK(Arcane::Remote::ExitFromHost(env) == 0);

    const auto* desc = Arcane::Remote::CommandRegistry::Get().Find("console.exec");
    REQUIRE(desc != nullptr);
    CHECK(desc->access == Arcane::Remote::Access::Drive);
    CHECK(desc->mutates == Arcane::Remote::Mutates::Preferences);
    CHECK_FALSE(desc->needsFrame);
    CHECK_FALSE(desc->worldScoped);   // process-global: --world is the dispatcher's arg.inapplicable
}

TEST_CASE("console.exec hands Execute the call's caller id", "[remote][console]") {
    Arcane::CVarRegistry reg;
    Arcane::CVarDesc d;
    d.name = "test.level";
    d.type = Arcane::CVarType::Int32;
    d.defaultValue = Arcane::CVarValue::Int32(1);
    d.audience = Arcane::Audience::Server;
    d.help = "level";
    REQUIRE_FALSE(reg.Register(d).IsStale());
    reg.Publish();
    reg.SetPolicy(&RecordCaller, nullptr, "remote-test");
    Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::ServerAdmin};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, &user);
    Arcane::Remote::RegisterConsoleCommand();
    g_seenCaller.clear();
    const auto env = Exec("test.level 3", "agent-7");
    REQUIRE(env.ok);
    CHECK(g_seenCaller == "agent-7");
    CHECK(g_seenContext == Arcane::CVarContext::ServerAdmin);
    const auto v = reg.Get(reg.Find("test.level"));
    REQUIRE(v.has_value());
    CHECK(v->AsInt32() == 3);   // the service publishes after a set, as cvar.set does
    const auto read = Exec("test.level");
    REQUIRE(read.ok);
    CHECK(read.data["text"] == "test.level = 3");
}

TEST_CASE("console.exec never prints a Protected value", "[remote][console]") {
    Arcane::CVarRegistry reg;
    Arcane::CVarDesc d;
    d.name = "test.secretKey";
    d.type = Arcane::CVarType::String;
    d.defaultValue = Arcane::CVarValue::String("hunter2");
    d.audience = Arcane::Audience::Server;
    d.flags = Arcane::CVarFlags::Protected;
    d.help = "secret";
    REQUIRE_FALSE(reg.Register(d).IsStale());
    reg.Publish();
    // ServerAdmin's console table may read a Protected value; the remote
    // surface never does (RemoteCVarService), so console.exec must not either.
    for (const Arcane::CVarContext context : {Arcane::CVarContext::ServerAdmin, Arcane::CVarContext::Editor}) {
        Arcane::Remote::CvarCommandUser user{&reg, context};
        RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, &user);
        Arcane::Remote::RegisterConsoleCommand();
        const auto env = Exec("test.secretKey");
        CHECK_FALSE(env.ok);
        REQUIRE_FALSE(env.errors.empty());
        CHECK(env.errors[0].code == "cvar.protected");
        CHECK(Arcane::Remote::ExitFromHost(env) == 3);
        CHECK(Arcane::Remote::ToJson(env).dump().find("hunter2") == std::string::npos);
    }
}

TEST_CASE("console.exec refusals: unknown, hidden, cheat-gated, failed, empty, no user", "[remote][console]") {
    g_failed = 0;
    g_cheat = 0;
    g_hidden = 0;
    Arcane::CVarRegistry reg;
    REQUIRE(reg.RegisterCommand("test.fail", Arcane::CVarFlags::ServerCanExecute, "fails", "remote-test", &FailingCmd,
                                nullptr));
    REQUIRE(reg.RegisterCommand("test.cheat", Arcane::CVarFlags::ServerCanExecute | Arcane::CVarFlags::Cheat, "cheat",
                                "remote-test", &CheatCmd, nullptr));
    REQUIRE(reg.RegisterCommand("test.hidden", Arcane::CVarFlags::ServerCanExecute | Arcane::CVarFlags::Hidden,
                                "hidden", "remote-test", &HiddenCmd, nullptr));
    Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::LocalHost};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, &user);
    Arcane::Remote::RegisterConsoleCommand();

    const auto unknown = Exec("no.such.thing 1");
    CHECK_FALSE(unknown.ok);
    REQUIRE_FALSE(unknown.errors.empty());
    CHECK(unknown.errors[0].code == "cvar.unknown");
    CHECK(unknown.errors[0].message.find("no.such.thing") != std::string::npos);
    CHECK(Arcane::Remote::ExitFromHost(unknown) == 6);

    // A Hidden command is not listed, so the remote surface does not know it.
    const auto hidden = Exec("test.hidden");
    REQUIRE_FALSE(hidden.errors.empty());
    CHECK(hidden.errors[0].code == "cvar.unknown");
    CHECK(g_hidden == 0);

    // Execute's own gate: a Cheat command outside the Editor needs server.cheats.
    const auto cheat = Exec("test.cheat");
    CHECK_FALSE(cheat.ok);
    REQUIRE_FALSE(cheat.errors.empty());
    CHECK(cheat.errors[0].code == "permission.denied");
    CHECK(Arcane::Remote::ExitFromHost(cheat) == 3);
    CHECK(g_cheat == 0);

    // The command ran and said no: ok false, its text kept, exit 6.
    const auto failed = Exec("test.fail");
    CHECK_FALSE(failed.ok);
    REQUIRE_FALSE(failed.errors.empty());
    CHECK(failed.errors[0].code == "cvar.refused");
    CHECK(failed.errors[0].message == "usage: test.fail <n>");
    CHECK(failed.data["text"] == "usage: test.fail <n>");
    CHECK(Arcane::Remote::ExitFromHost(failed) == 6);
    CHECK(g_failed == 1);

    // cvar_explain is refused whatever follows it, with or without arguments.
    for (const char* line : {"cvar_explain", "  cvar_explain   test.fail"}) {
        const auto explain = Exec(line);
        REQUIRE_FALSE(explain.errors.empty());
        CHECK(explain.errors[0].code == "command.unavailable");
        CHECK(explain.errors[0].remediation.find("servitor cvar explain") != std::string::npos);
        CHECK(Arcane::Remote::ExitFromHost(explain) == 2);
    }

    // Usage: line missing, empty, not a string; an unknown key.
    Arcane::Remote::Call bad;
    bad.command = "console.exec";
    bad.args = nlohmann::json::object();
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(bad).envelope.errors[0].code == "arg.missing");
    bad.args = nlohmann::json{{"line", "   "}};
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(bad).envelope.errors[0].code == "arg.invalid");
    bad.args = nlohmann::json{{"line", 3}};
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(bad).envelope.errors[0].code == "arg.invalid");
    bad.args = nlohmann::json{{"line", "test.fail"}, {"extra", true}};
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(bad).envelope.errors[0].code == "arg.unknown");
    CHECK(g_failed == 1);

    // No user: host.unavailable (exit 5), nothing dereferenced.
    Arcane::Remote::SetConsoleCommandUser(nullptr);
    const auto none = Exec("test.fail");
    REQUIRE_FALSE(none.errors.empty());
    CHECK(none.errors[0].code == "host.unavailable");
    CHECK(Arcane::Remote::ExitFromHost(none) == 5);
    CHECK(g_failed == 1);
}

TEST_CASE("HostSession serves console.exec in its context and releases only its own user", "[remote][console]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> cvarGuard(&Arcane::Remote::SetCvarCommandUser, nullptr);
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, nullptr);
    Arcane::Test::TempDir root("remote-console-session");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root.path;
    {
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        const auto listed = Exec("cvarlist");   // the registry's own read-only builtin
        REQUIRE(listed.ok);
        CHECK(listed.data["context"] == "LocalHost");   // the session's {CVarRegistry::Get(), cvarContext}
        session.Stop("test");
        CHECK(Exec("cvarlist").errors[0].code == "host.unavailable");
    }
    // A user installed after the session started is not the session's to release.
    Arcane::CVarRegistry reg;
    Arcane::Remote::CvarCommandUser other{&reg, Arcane::CVarContext::ServerAdmin};
    {
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        Arcane::Remote::SetConsoleCommandUser(&other);
        session.Stop("test");
    }
    const auto kept = Exec("cvarlist");
    REQUIRE(kept.ok);
    CHECK(kept.data["context"] == "ServerAdmin");
}

// One /call through a live session's listener while the "host loop" drains.
static Arcane::Remote::Envelope PostConsoleLive(Arcane::Remote::HostSession& session, const std::string& token,
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

TEST_CASE("a read-only session refuses console.exec; --world is inapplicable to it", "[remote][console]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> cvarGuard(&Arcane::Remote::SetCvarCommandUser, nullptr);
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, nullptr);
    Arcane::Test::TempDir root("remote-console-readonly");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root.path;
    cfg.readOnly = true;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);

    const auto refused = PostConsoleLive(session, desc->token,
        R"({"id":"c1","command":"console.exec","args":{"line":"cvarlist"}})");
    CHECK_FALSE(refused.ok);
    REQUIRE_FALSE(refused.errors.empty());
    CHECK(refused.errors[0].code == "permission.read-only");
    CHECK(Arcane::Remote::ExitFromHost(refused) == 3);
    CHECK(refused.timing.ranMs == 0.0);   // the gate refused it: never invoked
    session.Stop("test");

    cfg.readOnly = false;
    Arcane::Remote::HostSession writable(cfg);
    REQUIRE(writable.Start());
    const auto wdesc = Arcane::Remote::ReadDescriptor(writable.DescriptorFile());
    REQUIRE(wdesc);
    const auto world = PostConsoleLive(writable, wdesc->token,
        R"({"id":"c2","command":"console.exec","args":{"line":"cvarlist"},"options":{"world":"main"}})");
    CHECK_FALSE(world.ok);
    REQUIRE_FALSE(world.errors.empty());
    CHECK(world.errors[0].code == "arg.inapplicable");
    CHECK(world.command == "console.exec");
    const auto ran = PostConsoleLive(writable, wdesc->token,
        R"({"id":"c3","command":"console.exec","args":{"line":"cvarlist"}})");
    CHECK(ran.ok);
    CHECK(ran.command == "console.exec");
    CHECK(ran.data["text"].is_string());
    writable.Stop("test");
}

TEST_CASE("servitor console joins its tokens into one line; --all console needs --yes", "[servitor][console]") {
    std::size_t consumed = 0;
    const std::vector<std::string> tokens = {"console", "cvar_explain", "render.vsync"};
    const Servitor::VerbRow* row = Servitor::MatchVerb(tokens, &consumed);
    REQUIRE(row != nullptr);
    CHECK(consumed == 1);
    CHECK(std::string(row->command) == "console.exec");
    nlohmann::json args;
    std::string error;
    REQUIRE(row->args(std::vector<std::string>{"cvar_explain", "render.vsync"}, args, &error));
    CHECK(args == nlohmann::json{{"line", "cvar_explain render.vsync"}});
    REQUIRE(row->args(std::vector<std::string>{"test.echo a b"}, args, &error));   // one quoted token
    CHECK(args == nlohmann::json{{"line", "test.echo a b"}});
    CHECK_FALSE(row->args(std::vector<std::string>{}, args, &error));
    CHECK(error.find("console needs") != std::string::npos);
    CHECK_FALSE(row->args(std::vector<std::string>{"cvarlist", "--bogus"}, args, &error));
    const char* argv[] = {"servitor", "console"};
    CHECK(Servitor::Run(2, const_cast<char**>(argv)) == 2);

    // console posts a call, so --world and --read-only reach the host (Rule 3).
    Servitor::GlobalOptions forwarded;
    forwarded.world = "main";
    forwarded.readOnly = true;
    CHECK_FALSE(Servitor::UnusedGlobal(*row, forwarded).has_value());

    CHECK(Servitor::IsDriveCommand(nlohmann::json{{"command", "console.exec"}}));
    Servitor::Scan scan;
    for (std::uint32_t pid : {1u, 2u}) {
        Servitor::LiveHost h;
        h.descriptor.pid = pid;
        h.descriptor.hostKind = pid == 1 ? Arcane::Remote::HostKind::Runtime : Arcane::Remote::HostKind::Server;
        h.descriptor.protocol = 1;
        scan.live.push_back(h);
    }
    struct Seen { std::mutex m; std::vector<nlohmann::json> calls; } seen;
    auto post = [](const Arcane::Remote::Descriptor&, const nlohmann::json& call, Arcane::Remote::Envelope* out,
                   void* user) {
        auto& s = *static_cast<Seen*>(user);
        std::lock_guard lock(s.m);
        s.calls.push_back(call);
        *out = Arcane::Remote::MakeOk(call.value("command", std::string{}), nlohmann::json::object(), {});
        return 0;
    };
    Servitor::GlobalOptions opt;
    opt.all = true;
    Servitor::FanOutResult result;
    const std::vector<std::string> rest = {"cvarlist"};
    CHECK(Servitor::FanOutVerb(*row, rest, opt, scan, post, &seen, &result) == 2);
    CHECK(seen.calls.empty());
    opt.yes = true;
    CHECK(Servitor::FanOutVerb(*row, rest, opt, scan, post, &seen, &result) == 0);
    REQUIRE(seen.calls.size() == 2);
    for (const nlohmann::json& call : seen.calls) {
        CHECK(call["command"] == "console.exec");
        CHECK(call["args"] == nlohmann::json{{"line", "cvarlist"}});
    }
}

// --- Task 21 fix round 1 ---

TEST_CASE("console.exec set of a Protected cvar never echoes the new value", "[remote][console]") {
    Arcane::CVarRegistry reg;
    Arcane::CVarDesc text;
    text.name = "test.secretKey";
    text.type = Arcane::CVarType::String;
    text.defaultValue = Arcane::CVarValue::String("hunter2");
    text.audience = Arcane::Audience::Server;
    text.flags = Arcane::CVarFlags::Protected;
    text.help = "secret";
    REQUIRE_FALSE(reg.Register(text).IsStale());
    Arcane::CVarDesc pin;
    pin.name = "test.secretPin";
    pin.type = Arcane::CVarType::Int32;
    pin.defaultValue = Arcane::CVarValue::Int32(1234);
    pin.audience = Arcane::Audience::Server;
    pin.flags = Arcane::CVarFlags::Protected;
    pin.help = "secret pin";
    REQUIRE_FALSE(reg.Register(pin).IsStale());
    reg.Publish();
    for (const Arcane::CVarContext context : {Arcane::CVarContext::ServerAdmin, Arcane::CVarContext::Editor}) {
        Arcane::Remote::CvarCommandUser user{&reg, context};
        RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, &user);
        Arcane::Remote::RegisterConsoleCommand();
        // The success path: the set lands, and only the mask is echoed.
        const auto set = Exec("test.secretKey newSecretValue");
        REQUIRE(set.ok);
        CHECK(set.data["line"] == "test.secretKey <protected>");
        CHECK(Arcane::Remote::ToJson(set).dump().find("newSecretValue") == std::string::npos);
        const auto v = reg.Get(reg.Find("test.secretKey"));
        REQUIRE(v.has_value());
        CHECK(v->AsString() == "newSecretValue");
        // The failure path: a value the cvar's type refuses is not echoed either.
        const auto bad = Exec("test.secretPin s3cretXYZ");
        CHECK_FALSE(bad.ok);
        REQUIRE_FALSE(bad.errors.empty());
        CHECK(bad.errors[0].code == "cvar.type-mismatch");
        CHECK(bad.data["line"] == "test.secretPin <protected>");
        CHECK(Arcane::Remote::ToJson(bad).dump().find("s3cretXYZ") == std::string::npos);
    }
}

TEST_CASE("console.exec refuses a line holding a control character before anything runs", "[remote][console]") {
    g_flush = 0;
    g_secret = 0;
    Arcane::CVarRegistry reg;
    REQUIRE(reg.RegisterCommand("test.flush", Arcane::CVarFlags::ServerCanExecute, "flush", "remote-test", &FlushCmd,
                                nullptr));
    REQUIRE(reg.RegisterCommand("test.secret", Arcane::CVarFlags::None, "not remote", "remote-test", &SecretCmd,
                                nullptr));
    REQUIRE(reg.RegisterCommand("test.echo", Arcane::CVarFlags::ServerCanExecute, "echo", "remote-test", &EchoCmd,
                                nullptr));
    Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::ServerAdmin};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, &user);
    Arcane::Remote::RegisterConsoleCommand();
    for (const std::string line : {std::string("cvar_explain\r x"), std::string("test.secret\r now"),
                                   std::string("test.flush\n"), std::string("test.flush\r"),
                                   std::string("test.flush\x7f"), std::string("test.flush\x1b[2J"),
                                   std::string("test.flush") + '\0' + "x"}) {
        const auto env = Exec(line);
        CHECK_FALSE(env.ok);
        REQUIRE_FALSE(env.errors.empty());
        CHECK(env.errors[0].code == "arg.invalid");
        CHECK(env.errors[0].message.find("control characters") != std::string::npos);
        CHECK(Arcane::Remote::ExitFromHost(env) == 2);
    }
    CHECK(g_flush == 0);
    CHECK(g_secret == 0);
    // A tab is a word separator, not a control character to refuse.
    g_seenArgs.clear();
    const auto tabbed = Exec("test.echo\ta\tb");
    REQUIRE(tabbed.ok);
    CHECK(g_seenArgs == "a\tb");
}

static Arcane::CommandResult DeniedTextCmd(std::string_view, void*) {
    return {false, "denied: this text names no permission rule"};
}
static Arcane::CommandResult NotRemoteTextCmd(std::string_view, void*) {
    return {false, "command 'x' is not remotely executable"};
}

TEST_CASE("console.exec decides permission from the command's flags, never from its reply text", "[remote][console]") {
    g_cheat = 0;
    Arcane::CVarRegistry reg;
    REQUIRE(reg.RegisterCommand("test.deniedText", Arcane::CVarFlags::ServerCanExecute, "fails", "remote-test",
                                &DeniedTextCmd, nullptr));
    REQUIRE(reg.RegisterCommand("test.notRemoteText", Arcane::CVarFlags::ServerCanExecute, "fails", "remote-test",
                                &NotRemoteTextCmd, nullptr));
    REQUIRE(reg.RegisterCommand("test.cheat", Arcane::CVarFlags::ServerCanExecute | Arcane::CVarFlags::Cheat, "cheat",
                                "remote-test", &CheatCmd, nullptr));
    {
        Arcane::Remote::CvarCommandUser user{&reg, Arcane::CVarContext::LocalHost};
        RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> userGuard(&Arcane::Remote::SetConsoleCommandUser, &user);
        Arcane::Remote::RegisterConsoleCommand();
        // Allowed commands that ran and failed: their text is not a permission refusal.
        for (const char* line : {"test.deniedText", "test.notRemoteText"}) {
            const auto env = Exec(line);
            REQUIRE_FALSE(env.errors.empty());
            CHECK(env.errors[0].code == "cvar.refused");
            CHECK(Arcane::Remote::ExitFromHost(env) == 6);
        }
        // Cheat outside the Editor without server.cheats: refused from the flags, not run.
        const auto cheat = Exec("test.cheat");
        REQUIRE_FALSE(cheat.errors.empty());
        CHECK(cheat.errors[0].code == "permission.denied");
        CHECK(cheat.errors[0].message == "denied: 'test.cheat' needs server.cheats");
        CHECK(g_cheat == 0);
    }
    // In the Editor the Cheat gate does not apply (CVarRegistry::Execute's rule).
    Arcane::Remote::CvarCommandUser editor{&reg, Arcane::CVarContext::Editor};
    RemoteTest::ScopedRemotePointer<Arcane::Remote::CvarCommandUser> editorGuard(&Arcane::Remote::SetConsoleCommandUser, &editor);
    const auto ran = Exec("test.cheat");
    REQUIRE(ran.ok);
    CHECK(ran.data["text"] == "cheated");
    CHECK(g_cheat == 1);
}

#endif
