// Spec 2026-09-15 s5: snapshot ALL -> reload the DLL ONCE -> re-run registration ->
// restore ALL. Both worlds' state survives one swap and both are repopulated from
// the re-registered factories; a reload is REFUSED while any attached Runtime
// reports an active net driver (a test double until the replication arc).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/ModuleNames.hpp"   // fixture module file names per platform
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Sim/NetDriver.hpp>
#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"
#include <filesystem>
using namespace Arcane::HotReloadTest;

namespace
{
    struct FakeDriver final : Arcane::INetDriver { bool active = false; bool IsActive() const noexcept override { return active; } };
    int ReadPulse(Arcane::Runtime& rt) { int v = 0; rt.Registry().CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse& p){ v = p.ticks; }); return v; }
    // One sim step for EVERY attached world, with the module's FixedUpdate hook run
    // ONCE per step (it is bound to the primary's world; running it per Runtime would
    // double-count the primary's Pulse).
    void StepAll(Arcane::PluginHost& host, int k)
    {
        for (int i = 0; i < k; ++i)
        {
            bool first = true;
            for (Arcane::Runtime* rt : host.Runtimes())
            {
                rt->Loop().Advance(1.0/60.0, [&](double dt){ if (first) host.FixedUpdateAll(dt); }, [&](double,double){});
                first = false;
            }
        }
    }
}

TEST_CASE("snapshot-all / reload / restore-all across two live Runtimes", "[hotreload][netmode]")
{
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"), std::filesystem::copy_options::overwrite_existing);
    Arcane::Runtime server(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    // The client world shares the primary's ComponentRegistry (spec s4) -- which is
    // what makes the cross-world snapshot/restore below meaningful: a registry of its
    // own would answer UnknownComponent for every module-defined type.
    Arcane::Runtime client(Arcane::Test::Process(), Arcane::NetMode::Client, server.Components());
    server.Components()->RegisterComponent<Pulse>();
    server.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(server)); REQUIRE(host.AttachRuntime(client));
    REQUIRE(host.Load());
    // The module's OnInit creates its Pulse entity in the PRIMARY (server) world only;
    // give the client world its own state to prove the registry-only restore path.
    client.Registry().CreateEntityWith(Pulse{100});
    server.Registry().CreateEntityWith(RoleCounters{}); client.Registry().CreateEntityWith(RoleCounters{});
    StepAll(host, 2);
    const int serverPulse = ReadPulse(server);   // V1: +1 per step on the primary's own Pulse -> 2
    REQUIRE(serverPulse == 2);

    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV2"), Arcane::Test::ModuleFile("HotReloadPluginV1"), std::filesystem::copy_options::overwrite_existing);
    REQUIRE(host.ForceReload());
    CHECK(ReadPulse(server) == 2);          // primary: module SaveState/LoadState round-trip
    CHECK(ReadPulse(client) == 100);        // secondary: registry snapshot/restore, untouched by the module's OnInit
    CHECK(server.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());   // re-registered factories repopulated BOTH
    CHECK(client.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    StepAll(host, 1);
    CHECK(ReadPulse(server) == 12);         // V2's +10 ran on the restored primary world
    host.Unload();
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"), std::filesystem::copy_options::overwrite_existing);
}

TEST_CASE("hot reload is refused while any attached Runtime has an active net driver", "[hotreload][netmode]")
{
    Arcane::Runtime a(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    Arcane::Runtime b(Arcane::Test::Process(), Arcane::NetMode::Client, a.Components());
    a.Components()->RegisterComponent<Pulse>();
    a.Components()->RegisterComponent<RoleCounters>();
    FakeDriver drv; b.SetNetDriver(&drv);
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(a)); REQUIRE(host.AttachRuntime(b));
    REQUIRE(host.Load());
    const std::uint32_t gen = host.Generation();
    drv.active = true;
    CHECK_FALSE(host.ForceReload());        // refused: the diagnostic names Runtime #2 (Client)
    CHECK(host.IsLoaded());                 // and the live module is untouched
    CHECK(host.Generation() == gen);
    drv.active = false;
    CHECK(host.ForceReload());
    host.Unload();
}

TEST_CASE("DetachRuntime empties the leaving world: it drops out of every teardown path", "[hotreload][netmode]")
{
    // A world that leaves a loaded host keeps ENTITIES whose component descriptors
    // point into the module image, and the moment it is erased from Runtimes() it is
    // no longer covered by TeardownImage's ClearSystems/ResetRegistry loop. So the
    // detach itself has to do both, or those descriptors dangle at the unmap -- the
    // permanent-dangling-descriptor class diagnosed 2026-07-31.
    Arcane::Runtime server(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    Arcane::Runtime client(Arcane::Test::Process(), Arcane::NetMode::Client, server.Components());
    server.Components()->RegisterComponent<Pulse>();
    server.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(server)); REQUIRE(host.AttachRuntime(client));
    REQUIRE(host.Load());

    client.Registry().CreateEntityWith(Pulse{100});
    REQUIRE(ReadPulse(client) == 100);
    REQUIRE(client.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());

    host.DetachRuntime(client);
    CHECK(host.Runtimes().size() == 1);
    CHECK(host.Runtimes()[0] == &server);
    CHECK_FALSE(client.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());   // systems cleared...
    CHECK(ReadPulse(client) == 0);                                             // ...and the registry emptied

    // The staying world is untouched: same shared registry, module still live.
    CHECK(client.Components() == server.Components());
    CHECK(server.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK(host.IsLoaded());
    host.Unload();
}

TEST_CASE("DetachRuntime refuses the PRIMARY of a loaded host", "[hotreload][netmode]")
{
    // The primary IS the module's world (EngineContext::engine, and the SaveState/
    // LoadState half of a hot reload). Dropping it under a live module would leave the
    // module pointing at a world the host no longer drives; Unload() first.
    Arcane::Runtime server(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    Arcane::Runtime client(Arcane::Test::Process(), Arcane::NetMode::Client, server.Components());
    server.Components()->RegisterComponent<Pulse>();
    server.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(server)); REQUIRE(host.AttachRuntime(client));
    REQUIRE(host.Load());
    const std::uint32_t gen = host.Generation();

    host.DetachRuntime(server);                 // refused, no-op
    CHECK(host.Runtimes().size() == 2);
    CHECK(host.Runtimes()[0] == &server);
    CHECK(host.IsLoaded());
    CHECK(host.Generation() == gen);
    CHECK(server.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());   // its systems survive the refusal
    host.Unload();

    // With nothing loaded the primary detaches cleanly (the contract is about a LIVE
    // module, not about being first).
    host.DetachRuntime(server);
    CHECK(host.Runtimes().size() == 1);
}
