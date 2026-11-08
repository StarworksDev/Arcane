// Plan C N1: the editor's UI-phase texture drops wait for the drain. Device-less; the [gpu] half is
// ResidencyEvictionGpuTest's "a drop raised in the UI phase waits until the frame ... is submitted".
#include <catch2/catch_test_macros.hpp>

#include "App/DeferredTextureDrops.hpp"

#include <Arcane/Guid.hpp>

#include <vector>

using Arcane::Editor::DeferredTextureDrops;

namespace
{
    Arcane::Guid G(const char* text) { return *Arcane::Guid::FromString(text); }
}

TEST_CASE("a raised drop waits for the drain, which runs each held id once, in raise order", "[editor][textures]")
{
    const Arcane::Guid a = G("d1d1d1d1-0000-0000-0000-000000000001");
    const Arcane::Guid b = G("d1d1d1d1-0000-0000-0000-000000000002");
    std::vector<Arcane::Guid> dropped;
    DeferredTextureDrops drops([&](const Arcane::Guid& id) { dropped.push_back(id); });

    drops.Raise(b);
    drops.Raise(a);
    drops.Raise(b);              // a repeat is held once
    drops.Raise(Arcane::Guid{}); // nil is ignored
    CHECK(dropped.empty());      // nothing runs at the raise
    CHECK(drops.Pending() == 2u);

    drops.Drain();
    CHECK(dropped == std::vector<Arcane::Guid>{ b, a });
    CHECK(drops.Pending() == 0u);

    drops.Drain();               // empty: nothing again
    CHECK(dropped.size() == 2u);
}

TEST_CASE("a drop raised while the drain runs waits for the next drain", "[editor][textures]")
{
    const Arcane::Guid a = G("d1d1d1d1-0000-0000-0000-000000000003");
    const Arcane::Guid b = G("d1d1d1d1-0000-0000-0000-000000000004");
    std::vector<Arcane::Guid> dropped;
    DeferredTextureDrops drops;
    drops.SetDrop([&](const Arcane::Guid& id)
    {
        dropped.push_back(id);
        if (id == a)
            drops.Raise(b);
    });

    drops.Raise(a);
    drops.Drain();
    CHECK(dropped == std::vector<Arcane::Guid>{ a });
    CHECK(drops.Pending() == 1u);
    drops.Drain();
    CHECK(dropped == std::vector<Arcane::Guid>{ a, b });
}

TEST_CASE("with no drop installed the drain forgets what it held", "[editor][textures]")
{
    DeferredTextureDrops drops;
    drops.Raise(G("d1d1d1d1-0000-0000-0000-000000000005"));
    CHECK(drops.Pending() == 1u);
    drops.Drain();
    CHECK(drops.Pending() == 0u);
}
