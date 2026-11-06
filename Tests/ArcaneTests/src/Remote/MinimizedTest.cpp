#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include "RemoteDemoArgs.hpp"
#include "RemoteTestSupport.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir

#if !defined(ARC_BUILD_DIST)

namespace
{
int g_get = 0;
int g_shot = 0;

Arcane::Remote::CommandResult CountGet(const Arcane::Remote::Call& call, void*)
{
    ++g_get;
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeOk(call.command, nlohmann::json::object(), {});
    return r;
}

Arcane::Remote::CommandResult CountShot(const Arcane::Remote::Call& call, void*)
{
    ++g_shot;
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeOk(call.command, nlohmann::json::object(), {});
    return r;
}
} // namespace

TEST_CASE("minimized: cvar.get runs, screenshot does not, and the waiter returns", "[remote][dispatch]")
{
    g_get = 0;
    g_shot = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    Arcane::Remote::Desc get;
    get.name = "min.cvar";
    get.summary = "read a cvar";
    get.access = Arcane::Remote::Access::Observe;
    get.module = "min-test";
    get.argsMeta = Astra::GetMeta<RemoteDemoArgs>();
    get.fn = &CountGet;
    get.needsFrame = false;
    Arcane::Remote::Desc shot = get;
    shot.name = "min.shot";
    shot.summary = "grab the viewport";
    shot.access = Arcane::Remote::Access::Drive;
    shot.mutates = Arcane::Remote::Mutates::Project;
    shot.fn = &CountShot;
    shot.needsFrame = true;
    REQUIRE(reg.Register(get, &err));
    REQUIRE(reg.Register(shot, &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.minimized = true;
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-min-audit" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call a;
    a.command = "min.cvar";
    Arcane::Remote::Call b;
    b.command = "min.shot";
    REQUIRE(queue.TryPush(1, a));
    REQUIRE(queue.TryPush(2, b));
    dispatcher.Drain(1000.0, 8);
    CHECK(g_get == 1);
    CHECK(g_shot == 0);
    Arcane::Remote::CommandResult got;
    Arcane::Remote::CommandResult denied;
    REQUIRE(queue.Wait(1, got, std::chrono::milliseconds(1000)));
    REQUIRE(queue.Wait(2, denied, std::chrono::milliseconds(1000)));
    CHECK(got.envelope.ok);
    REQUIRE_FALSE(denied.envelope.errors.empty());
    CHECK(denied.envelope.errors[0].code == "host.busy");
    CHECK(denied.envelope.errors[0].detail["state"] == "minimized");
    CHECK(Arcane::Remote::ExitFromHost(denied.envelope) == 5);
    reg.UnregisterModule("min-test");
}

TEST_CASE("minimized: HostSession::NoteMinimized sets and clears the gate flag", "[remote][host]")
{
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir root("remote-minimized-session");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Server;
    cfg.cvarContext = Arcane::CVarContext::ServerAdmin;
    cfg.projectRoot = root.path;
    cfg.capabilities = {"console", "cvar"};
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK_FALSE(session.Gate().minimized.load());
    session.NoteMinimized(true);
    CHECK(session.Gate().minimized.load());
    session.NoteMinimized(false);
    CHECK_FALSE(session.Gate().minimized.load());
    session.Stop("test");
}

#endif
