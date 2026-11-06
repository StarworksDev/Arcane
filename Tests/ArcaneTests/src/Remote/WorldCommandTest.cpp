#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Remote/Commands/WorldCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Verbs.hpp>
#include <Arcane/Core/NetMode.hpp>
#include <Json.hpp>
#include "RemoteTestSupport.hpp"
#include <Arcane/Remote/HostSession.hpp>
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir

#include <set>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

static std::vector<Arcane::Remote::WorldInfo> TwoWorlds(void*) {
    Arcane::Remote::WorldInfo main;
    main.name = "main";
    main.role = Arcane::ToString(Arcane::NetMode::DedicatedServer);
    Arcane::Remote::WorldInfo other;
    other.name = "other";
    other.role = Arcane::ToString(Arcane::NetMode::Client);
    return {other, main};
}

TEST_CASE("world.list sorts by name and uses NetMode strings", "[remote][world]") {
    Arcane::Remote::WorldTable table;
    table.fn = &TwoWorlds;
    auto env = Arcane::Remote::WorldList(table);
    CHECK(env.ok);
    REQUIRE(env.data["worlds"].size() == 2);
    CHECK(env.data["worlds"][0]["name"] == "main");
    CHECK(env.data["worlds"][0]["role"] == "DedicatedServer");
    CHECK(env.data["worlds"][1]["role"] == "Client");
}

TEST_CASE("a missing world name is arg.missing when two worlds exist", "[remote][world]") {
    Arcane::Remote::WorldTable table;
    table.fn = &TwoWorlds;
    auto missing = Arcane::Remote::ResolveWorld(table, "");
    CHECK(missing.pick == Arcane::Remote::WorldPick::Missing);
    CHECK(missing.error.errors[0].code == "arg.missing");
    CHECK(Arcane::Remote::ExitFromHost(missing.error) == 2);
    CHECK(missing.error.data["worlds"].size() == 2);
    auto ok = Arcane::Remote::ResolveWorld(table, "main");
    CHECK(ok.pick == Arcane::Remote::WorldPick::Ok);
    CHECK(ok.world.name == "main");
    auto bad = Arcane::Remote::ResolveWorld(table, "nope");
    CHECK(bad.pick == Arcane::Remote::WorldPick::Unknown);
    CHECK(Arcane::Remote::ExitFromHost(bad.error) == 2);
}

static std::vector<Arcane::Remote::WorldInfo> OneWorld(void*) {
    Arcane::Remote::WorldInfo main;
    main.name = "main";
    main.role = "Standalone";
    return {main};
}

TEST_CASE("one world is the default", "[remote][world]") {
    Arcane::Remote::WorldTable table;
    table.fn = &OneWorld;
    auto picked = Arcane::Remote::ResolveWorld(table, "");
    CHECK(picked.pick == Arcane::Remote::WorldPick::Ok);
    CHECK(picked.world.name == "main");
    CHECK(picked.world.role == "Standalone");
    auto none = Arcane::Remote::ResolveWorld(Arcane::Remote::WorldTable{}, "");
    CHECK(none.pick == Arcane::Remote::WorldPick::None);
    CHECK(none.error.errors[0].code == "host.unavailable");
    CHECK(Arcane::Remote::ExitFromHost(none.error) == 5);
}

TEST_CASE("world.list reads the command world table and the guard clears it", "[remote][world]") {
    Arcane::Remote::RegisterWorldCommands();
    Arcane::Remote::Call call;
    call.command = "world.list";
    {
        Arcane::Remote::WorldTable table;
        table.fn = &TwoWorlds;
        RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worlds(
            [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
            &table);
        const auto listed = Arcane::Remote::CommandRegistry::Get().Invoke(call);
        REQUIRE(listed.envelope.ok);
        CHECK(listed.envelope.data["worlds"].size() == 2);
    }
    CHECK(Arcane::Remote::CommandWorlds().fn == nullptr);   // cleared on scope exit
    const auto empty = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(empty.envelope.errors[0].code == "host.unavailable");
}

TEST_CASE("--world is parsed and posted as options.world", "[remote][world]") {
    const char* argv[] = {"servitor", "--world", "main", "ecs", "stats"};
    Servitor::GlobalOptions opt;
    std::string err;
    REQUIRE(Servitor::ParseGlobals(5, const_cast<char**>(argv), opt, &err));
    CHECK(opt.world == "main");
    const char* bare[] = {"servitor", "ecs", "stats", "--world"};
    Servitor::GlobalOptions bareOpt;
    CHECK_FALSE(Servitor::ParseGlobals(4, const_cast<char**>(bare), bareOpt, &err));
    CHECK(err.find("world") != std::string::npos);
    static std::string posted;
    posted.clear();
    Servitor::CallRequest req;
    req.command = "ecs.stats";
    Servitor::Call(req, opt,
        [](const std::string& body, void*) {
            posted = body;
            Servitor::ExchangeResult r;
            r.envelope = Arcane::Remote::MakeOk("ecs.stats", nlohmann::json::object(), {});
            return r;
        }, nullptr, nullptr);
    CHECK(nlohmann::json::parse(posted)["options"]["world"] == "main");
}

TEST_CASE("world.list refuses a --world it does not run", "[remote][world]") {
    Arcane::Remote::RegisterWorldCommands();
    Arcane::Remote::WorldTable table;
    table.fn = &TwoWorlds;
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worlds(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &table);
    Arcane::Remote::Call call;
    call.command = "world.list";
    call.world = "nope";
    const auto bad = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    REQUIRE_FALSE(bad.envelope.ok);   // Invoke is below the dispatcher, which stamps the command
    CHECK(bad.envelope.errors[0].code == "arg.unknown");
    CHECK(bad.envelope.errors[0].candidates == std::vector<std::string>{"main", "other"});
    CHECK(Arcane::Remote::ExitFromHost(bad.envelope) == 2);
    call.world = "other";
    const auto named = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    REQUIRE(named.envelope.ok);
    CHECK(named.envelope.data["worlds"].size() == 2);   // --world validates; the list is still every world
    call.world.clear();
    call.args = {{"world", "main"}};
    const auto extra = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(extra.envelope.errors[0].code == "arg.unknown");
}

TEST_CASE("HostSession installs its worlds at Start and clears them at Stop", "[remote][world]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir root("remote-world-session");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root.path;
    cfg.worlds.fn = &OneWorld;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK(Arcane::Remote::CommandWorlds().fn == &OneWorld);
    CHECK(session.Gate().world == "main");               // the default world is the host block's world
    CHECK(session.Gate().Block().world == "main");
    Arcane::Remote::Call call;
    call.command = "world.list";
    REQUIRE(Arcane::Remote::CommandRegistry::Get().Find("world.list") != nullptr);
    const auto listed = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    REQUIRE(listed.envelope.ok);
    REQUIRE(listed.envelope.data["worlds"].size() == 1);
    CHECK(listed.envelope.data["worlds"][0]["role"] == "Standalone");
    session.Stop("test");
    CHECK(Arcane::Remote::CommandWorlds().fn == nullptr);
    const auto stopped = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(stopped.envelope.errors[0].code == "host.unavailable");
}

// --- Task 19 fix round 1: Rule 3 for the host-scoped globals ---

TEST_CASE("--world and --read-only are refused by verbs that post no call", "[remote][world]") {
    // The verbs that never post a /call, so would drop options.world / readOnly.
    const std::set<std::string> local = {"status", "launch", "commands", "list"};
    Servitor::GlobalOptions world;
    world.world = "main";
    Servitor::GlobalOptions readOnly;
    readOnly.readOnly = true;
    std::size_t refused = 0;
    for (const Servitor::VerbRow& row : Servitor::VerbTable()) {
        INFO(row.words);
        const bool drops = local.count(row.words) != 0;
        CHECK(Servitor::PostsCall(row) == !drops);
        const auto w = Servitor::UnusedGlobal(row, world);
        const auto r = Servitor::UnusedGlobal(row, readOnly);
        CHECK(w.has_value() == drops);
        CHECK(r.has_value() == drops);
        CHECK_FALSE(Servitor::UnusedGlobal(row, Servitor::GlobalOptions{}).has_value());
        if (drops) {
            ++refused;
            CHECK(*w == "--world does not apply to " + std::string(row.words));
            CHECK(*r == "--read-only does not apply to " + std::string(row.words));
        }
    }
    CHECK(refused == local.size());
    // The posting verbs keep both: world list, call, quit, describe, ...
    CHECK_FALSE(Servitor::UnusedGlobal(*Servitor::FindVerb("world list"), world).has_value());
    CHECK_FALSE(Servitor::UnusedGlobal(*Servitor::FindVerb("call"), readOnly).has_value());
}

#endif
