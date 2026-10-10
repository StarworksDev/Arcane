// Spec 2026-09-15 s4 + s9: two Runtimes, ONE loaded module, role-masked
// instantiation; ListenServer takes both; HasAuthority across the four modes; and
// the net-mode/launch-flag INDEPENDENCE the spec names as a bug class (UE's
// IsRunningDedicatedServer vs NetMode).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/ModuleNames.hpp"   // fixture module file names per platform
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/GameSystems.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>
#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"
#include <Astra/Component/ComponentRegistry.hpp>   // GetComponentDescriptor (the shared-registry proof)
#include <Astra/Core/TypeID.hpp>
#include <filesystem>
#include <type_traits>
using namespace Arcane::HotReloadTest;

namespace
{
    struct AutoUpdateProbe
    {
        void operator()(Astra::Registry&) {}
    };

    void Step(Arcane::Runtime& rt, Arcane::PluginHost& host, int k)
    { for (int i = 0; i < k; ++i) rt.Loop().Advance(1.0 / 60.0, [&](double dt){ host.FixedUpdateAll(dt); }, [&](double,double){}); }
    RoleCounters Read(Arcane::Runtime& rt)
    { RoleCounters out; rt.Registry().CreateView<RoleCounters>().ForEach([&](Astra::Entity, RoleCounters& c){ out = c; }); return out; }
}

ARC_SYSTEM(AutoUpdateProbe,
              Arcane::RoleMask::Client,
              Arcane::SystemPhase::Update)

TEST_CASE("automatic system registrars add a role-masked factory to the selected phase",
          "[runtime][netmode][systems]")
{
    STATIC_REQUIRE(std::is_trivially_destructible_v<Arcane::Game::SystemRegistrar>);

    Arcane::SystemFactoryTable table;
    const int owner = 1;
    table.BeginOwner(&owner);
    CHECK(Arcane::Game::RegisterSystems(table) == 1);
    table.EndOwner();

    Arcane::SystemSchedulers client(nullptr);
    CHECK(table.InstantiateInto(client, Arcane::NetMode::Client) == 1);
    CHECK(client.update.HasSystem<AutoUpdateProbe>());
    CHECK_FALSE(client.fixedUpdate.HasSystem<AutoUpdateProbe>());
    CHECK_FALSE(client.render.HasSystem<AutoUpdateProbe>());

    Arcane::SystemSchedulers server(nullptr);
    CHECK(table.InstantiateInto(server, Arcane::NetMode::DedicatedServer) == 0);
    CHECK_FALSE(server.update.HasSystem<AutoUpdateProbe>());
}

TEST_CASE("one module cannot register the same system twice in one phase",
          "[runtime][netmode][systems]")
{
    Arcane::SystemFactoryTable table;
    const int owner = 2;
    const auto entry = [](Arcane::RoleMask mask, Arcane::SystemPhase phase)
    {
        return Arcane::SystemFactoryEntry{
            "DuplicateProbe", mask, phase,
            [](Astra::SystemScheduler&) {}, nullptr };
    };

    table.BeginOwner(&owner);
    table.Add(entry(Arcane::RoleMask::Server, Arcane::SystemPhase::FixedUpdate));
    table.Add(entry(Arcane::RoleMask::Client, Arcane::SystemPhase::FixedUpdate));
    CHECK(table.Size() == 1);

    table.Add(entry(Arcane::RoleMask::Client, Arcane::SystemPhase::Update));
    CHECK(table.Size() == 2);
    table.EndOwner();
}

TEST_CASE("HasAuthority: every mode but Client", "[runtime][netmode]")
{
    using Arcane::NetMode;
    CHECK(Arcane::Runtime(Arcane::Test::Process(), NetMode::Standalone).HasAuthority());
    CHECK(Arcane::Runtime(Arcane::Test::Process(), NetMode::DedicatedServer).HasAuthority());
    CHECK(Arcane::Runtime(Arcane::Test::Process(), NetMode::ListenServer).HasAuthority());
    CHECK_FALSE(Arcane::Runtime(Arcane::Test::Process(), NetMode::Client).HasAuthority());
}

TEST_CASE("net mode and the launch flag are independent: a DedicatedServer Runtime in a non-server process has authority", "[runtime][netmode]")
{
    REQUIRE_FALSE(Arcane::Test::Process().IsDedicatedServerProcess());
    Arcane::Runtime rt(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    CHECK(rt.HasAuthority());
    CHECK(rt.Mode() == Arcane::NetMode::DedicatedServer);
}

TEST_CASE("two Runtimes, one module: the Server-masked system exists only in the server world and the Client-masked only in the client", "[runtime][netmode][hotreload]")
{
    Arcane::Runtime server(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    // The secondary world is built on the PRIMARY's ComponentRegistry (spec s4): a
    // module registers its component types ONCE, into the primary's registry, and
    // every world it serves shares that one -- so a single host-side registration
    // covers both worlds, and AttachRuntime refuses anything else.
    Arcane::Runtime client(Arcane::Test::Process(), Arcane::NetMode::Client, server.Components());
    REQUIRE(client.Components() == server.Components());
    server.Components()->RegisterComponent<Pulse>();
    server.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(server));
    REQUIRE(host.AttachRuntime(client));
    REQUIRE(host.Load());
    REQUIRE(host.Runtimes().size() == 2);

    CHECK(server.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK_FALSE(server.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK(client.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK_FALSE(client.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());

    server.Registry().CreateEntityWith(RoleCounters{});
    client.Registry().CreateEntityWith(RoleCounters{});
    Step(server, host, 3); Step(client, host, 3);
    CHECK(Read(server).serverTicks == 3); CHECK(Read(server).clientTicks == 0);
    CHECK(Read(client).clientTicks == 3); CHECK(Read(client).serverTicks == 0);
    host.Unload();
}

TEST_CASE("ListenServer instantiates BOTH masks in its one Runtime", "[runtime][netmode][hotreload]")
{
    Arcane::Runtime listen(Arcane::Test::Process(), Arcane::NetMode::ListenServer);
    listen.Components()->RegisterComponent<Pulse>(); listen.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(listen));
    REQUIRE(host.Load());
    CHECK(listen.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK(listen.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    host.Unload();
}

TEST_CASE("the factory table is the process's, cleared when the module unloads", "[runtime][netmode][hotreload]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>(); rt.Components()->RegisterComponent<RoleCounters>();
    const std::size_t before = Arcane::Test::Process().SystemFactories().Size();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(rt));
    REQUIRE(host.Load());
    CHECK(Arcane::Test::Process().SystemFactories().Size() == before + 2);
    host.Unload();
    CHECK(Arcane::Test::Process().SystemFactories().Size() == before);   // std::functions into the image are gone BEFORE the unmap
}

TEST_CASE("AttachRuntime refuses a secondary world that does not share the primary's ComponentRegistry", "[runtime][netmode][hotreload]")
{
    // The invariant spec s4 rests on: a module opens its ComponentModule on the
    // PRIMARY's registry alone, so a secondary with a registry of its own would
    // resolve NONE of the module's component types -- and a snapshot moved between
    // the two worlds would fail with UnknownComponent. Refused at attach, where the
    // mistake is still nameable.
    Arcane::Runtime primary(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    Arcane::Runtime stranger(Arcane::Test::Process(), Arcane::NetMode::Client);   // its OWN registry
    REQUIRE(stranger.Components() != primary.Components());

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    CHECK(host.AttachRuntime(primary));          // the FIRST attach always succeeds
    CHECK_FALSE(host.AttachRuntime(stranger));   // refused, and the host is unchanged
    CHECK(host.Runtimes().size() == 1);
    CHECK(host.Runtimes()[0] == &primary);
    CHECK(host.AttachRuntime(primary));          // re-attaching the same world is a no-op success
    CHECK(host.Runtimes().size() == 1);
}

TEST_CASE("the module's component descriptors are live in the secondary world too", "[runtime][netmode][hotreload]")
{
    // The other half of "one module image serves N worlds": role-masked SYSTEMS are
    // per-world, but COMPONENT TYPES are registered once (spec R1) into the primary's
    // registry -- which the secondary shares. What that buys the secondary is the
    // MODULE's descriptors, not merely the host's.
    Arcane::Runtime server(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    Arcane::Runtime client(Arcane::Test::Process(), Arcane::NetMode::Client, server.Components());
    // The host-side registration is the base owner every case in this file installs
    // (the module's ComponentModule shadows it and its unload pops back onto it), so
    // it cannot be what proves the point. The descriptor CONTENT is: the slot address
    // is pointer-stable for life, so a defaultConstruct that CHANGED across Load()
    // means the module's own registration -- made on the primary's registry -- is what
    // the CLIENT world now resolves.
    server.Components()->RegisterComponent<Pulse>();
    server.Components()->RegisterComponent<RoleCounters>();

    const Astra::ComponentID counterId = Astra::TypeID<RoleCounters>::Value();
    const Astra::ComponentDescriptor* slot = client.Components()->GetComponentDescriptor(counterId);
    REQUIRE(slot != nullptr);
    const auto hostDefaultConstruct = slot->defaultConstruct;
    REQUIRE(hostDefaultConstruct != nullptr);

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(server));
    REQUIRE(host.AttachRuntime(client));
    REQUIRE(host.Load());

    CHECK(client.Components()->GetComponentDescriptor(counterId) == slot);   // same slot...
    CHECK(slot->defaultConstruct != hostDefaultConstruct);                   // ...the MODULE's descriptor
    // ...and usable in that world, not merely present: the client world creates and
    // reads a component type only the module and the shared registry know about.
    client.Registry().CreateEntityWith(RoleCounters{7, 9});
    CHECK(Read(client).serverTicks == 7);
    CHECK(Read(client).clientTicks == 9);

    host.Unload();
    // Popped back to the host's base owner -- the evidence that the module's entry
    // was a shadow over it, and the reason the base owner has to exist at all.
    CHECK(client.Components()->GetComponentDescriptor(counterId)->defaultConstruct == hostDefaultConstruct);
}
