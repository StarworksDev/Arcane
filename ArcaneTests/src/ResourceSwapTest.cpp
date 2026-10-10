// Input-seam spec 2026-10-02 s8 T3: Arcane::Time and Arcane::GameInput are
// present after EVERY registry swap a host performs -- Play, Stop, scene open
// (ResetRegistry) and hot reload -- because RunLoop and ClientRuntime
// republish them on the very next pass. The snapshot carries neither: both are
// transient resources (Astra AstraTransientResource, IN-8 ruling), so every
// swap starts without them and only the republish brings them back.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/ModuleNames.hpp"   // fixture module file names per platform

#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Input/GameInput.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Sim/Time.hpp>

#include <Astra/Registry/Registry.hpp>

#include <App/PlayMode.hpp>

#include <filesystem>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    void Frame(Arcane::ClientRuntime& c, Arcane::PluginHost* host = nullptr)
    {
        c.UpdateGameInput(1.0 / 60.0, {});
        c.Loop().Advance(1.0 / 60.0,
            [&](double dt)           { c.BeginGameInputFixedStep(); if (host) host->FixedUpdateAll(dt); },
            [&](double dt, double a) { if (host) host->UpdateAll(dt, a); });
    }
    bool HasBoth(Arcane::ClientRuntime& c)
    {
        return c.Registry().GetResource<Arcane::Time>() != nullptr &&
               c.Registry().GetResource<Arcane::GameInput>() != nullptr;
    }
}

TEST_CASE("Time and GameInput are back one frame after Play, Stop and a scene open", "[client][time][gameinput][editor]")
{
    Arcane::ClientRuntime client(Arcane::Test::Process());
    Arcane::RegisterSceneComponents(client.Registry());
    client.Registry().CreateEntity();
    Frame(client);
    REQUIRE(HasBoth(client));

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(client.Core()));
    Frame(client);
    CHECK(HasBoth(client));
    const std::uint64_t playSteps = client.Registry().GetResource<Arcane::Time>()->fixedStep;

    REQUIRE(play.Stop(client.Core()));             // RestoreRegistry: resources gone with the swap
    Frame(client);
    CHECK(HasBoth(client));
    CHECK(client.Registry().GetResource<Arcane::Time>()->fixedStep == 0);           // Rebind restarted the clock (and Stop re-paused, so no step since)
    CHECK(playSteps > 0);
    CHECK(client.Registry().GetResource<Arcane::Time>()->paused);                   // Stop re-paused

    client.ResetRegistry();                         // File > Open Scene's swap
    Frame(client);
    CHECK(HasBoth(client));
}

TEST_CASE("Time and GameInput are back one frame after a module hot reload", "[client][time][gameinput][hotreload]")
{
    std::error_code ec;
    std::filesystem::copy_file(Arcane::Test::ModuleFile("HotReloadPluginV1"), "ResourceSwapPlugin.dll",
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);

    Arcane::ClientRuntime client(Arcane::Test::Process());
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("ResourceSwapPlugin.dll"));
    REQUIRE(host.AttachRuntime(client.Core()));
    REQUIRE(host.Load());
    Frame(client, &host);
    REQUIRE(HasBoth(client));

    REQUIRE(host.ForceReload());                    // SaveState -> unload -> load -> LoadState
    Frame(client, &host);
    CHECK(HasBoth(client));

    host.Unload();
    std::filesystem::remove("ResourceSwapPlugin.dll", ec);
}

// IN-8 ruling (spec s4 amendment): Time and GameInput are TRANSIENT resources
// (Astra AstraTransientResource), never written into a registry snapshot. So
// the EmbeddedServer world, seeded from the client's snapshot, has no GameInput
// (its pointer would name the CLIENT's LocalInputUser), and a restore revives
// no stale Time: Rebind republishes it at step 0 before any Advance.
TEST_CASE("A server world seeded from a client snapshot has no GameInput", "[client][time][gameinput][editor][netmode]")
{
    Arcane::ClientRuntime client(Arcane::Test::Process());
    Arcane::RegisterSceneComponents(client.Registry());
    client.Registry().CreateEntity();
    for (int i = 0; i < 5; ++i) Frame(client);
    REQUIRE(HasBoth(client));
    REQUIRE(client.Registry().GetResource<Arcane::Time>()->fixedStep > 0);

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(client.Core(), nullptr, Arcane::Editor::PlayTopology::EmbeddedServer));
    Arcane::Runtime* server = play.ServerWorld();
    REQUIRE(server != nullptr);
    CHECK(server->Registry().GetResource<Arcane::GameInput>() == nullptr);
    const Arcane::Time* serverTime = server->Registry().GetResource<Arcane::Time>();
    REQUIRE(serverTime != nullptr);                  // Rebind republished it...
    CHECK(serverTime->fixedStep == 0);               // ...fresh, not the client's stale clock

    for (int i = 0; i < 3; ++i) play.TickServer(1.0 / 60.0);
    CHECK(server->Registry().GetResource<Arcane::GameInput>() == nullptr);   // and nothing on the server publishes one

    REQUIRE(play.Stop(client.Core()));
}

TEST_CASE("After a RestoreRegistry, Time reads step 0 before any Advance and GameInput waits for the next frame", "[client][time][gameinput]")
{
    Arcane::ClientRuntime client(Arcane::Test::Process());
    Arcane::RegisterSceneComponents(client.Registry());
    client.Registry().CreateEntity();
    for (int i = 0; i < 5; ++i) Frame(client);
    REQUIRE(client.Registry().GetResource<Arcane::Time>()->fixedStep > 0);

    auto snap = client.Core().SnapshotRegistry();
    REQUIRE(snap.IsOk());
    client.Loop().SetPaused(true);
    REQUIRE(client.RestoreRegistry(*snap.GetValue()));

    const Arcane::Time* t = client.Registry().GetResource<Arcane::Time>();
    REQUIRE(t != nullptr);
    CHECK(t->fixedStep == 0);
    CHECK(t->elapsed == 0.0);
    CHECK(t->paused);                                // the host's mode survives the rebind
    CHECK(client.Registry().GetResource<Arcane::GameInput>() == nullptr);    // no stale copy revived

    Frame(client);
    CHECK(HasBoth(client));
}
