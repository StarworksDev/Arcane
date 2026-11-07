// Bindless slot recycling (3D-readiness arc, Plan C Task 2, spec 3.5): the
// device-wide table hands out an index, a released index is recycled only
// after EVERY context that sampled it has completed that submit on its own
// timeline, and a write to a slot reaches every context's descriptor set.
//
// The device-less cases ([nri][bindless]) run on the NONE backend
// (NriDevice::CreateNoneForTests) with two MOCK TIMELINES: a context's "fence
// completed" answer is a number the test sets. ImplNONE's Destroy* entries are
// no-ops that never touch their argument, so a fabricated nri::Descriptor* is
// safe. The descriptor-set writes go through BindlessTable::SetHooks, which
// records them per set, so "both contexts' sets contain the descriptor at that
// index" is an executable CHECK.
//
// The [gpu] case runs the two-context shape on real D3D12 and Vulkan devices
// and holds RenderErrorCount() at 0.
//
// Include order: NRI headers first -- see NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIHelper.h>
#include <Arcane/Render/Nri/NriDevice.hpp>   // pulls Extensions/NRIDeviceCreation.h: before anything that reaches <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialTypes.hpp>
#include <Arcane/Render/MeshMaterialCache.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/Nri/BindlessTable.hpp>
#include <Arcane/Render/Nri/DeviceResourceLifetime.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/Generated/BindlessCapacity.gen.hpp>

#include "Helpers/GpuCapability.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <map>
#include <memory>
#include <vector>

namespace
{
    using Arcane::BindlessTable;
    using Arcane::DeviceResourceLifetime;

    constexpr BindlessTable::ColorSpace kSrgb    = BindlessTable::ColorSpace::Srgb;
    constexpr BindlessTable::ColorSpace kDisplay = BindlessTable::ColorSpace::Display;

    // Two sampling contexts' identities. Never dereferenced: the last-use record
    // only compares them, and the completion query below is a map lookup.
    const void* ContextA() { static int a; return &a; }
    const void* ContextB() { static int b; return &b; }
    const void* ContextC() { static int c; return &c; }

    nri::Descriptor* MockSrv(std::uintptr_t tag)
    {
        return reinterpret_cast<nri::Descriptor*>(0x1000u + tag * 16u);
    }
    nri::DescriptorSet* MockSet(std::uintptr_t tag)
    {
        return reinterpret_cast<nri::DescriptorSet*>(0x9000u + tag * 16u);
    }

    // The device-less device: a NONE NriDevice, ONE DeviceResourceLifetime the
    // table holds a reference to (so `life.NoteSample` IS what the table
    // recycles against), two mock timelines, and recording hooks.
    struct DeviceFixture
    {
        std::unique_ptr<Arcane::NriDevice>        device;
        DeviceResourceLifetime                    life;
        std::map<const void*, std::uint64_t>      completed;   // context -> highest completed fence
        std::map<nri::DescriptorSet*, std::map<std::uint32_t, nri::Descriptor*>> sets;   // recorded writes
        int                                       idleCalls = 0;
        std::unique_ptr<BindlessTable>            table;

        static bool Completed(const void* context, std::uint64_t fence, void* user)
        {
            const auto& f = *static_cast<DeviceFixture*>(user);
            const auto it = f.completed.find(context);
            return it != f.completed.end() && it->second >= fence;
        }
        static void Write(void* user, nri::DescriptorSet* set, std::uint32_t slot, nri::Descriptor* srv)
        {
            static_cast<DeviceFixture*>(user)->sets[set][slot] = srv;
        }
        static bool Idle(void* user)
        {
            ++static_cast<DeviceFixture*>(user)->idleCalls;
            return true;
        }

        explicit DeviceFixture(std::uint32_t capacity = Arcane::kBindlessCapacity)
        {
            device = Arcane::NriDevice::CreateNoneForTests();
            REQUIRE(device != nullptr);
            table = BindlessTable::Create(*device, capacity, life);
            REQUIRE(table != nullptr);
            table->SetCompletionQuery(&Completed, this);
            table->SetHooks(&Write, &Idle, this);
        }
        ~DeviceFixture()
        {
            if (table)
                table->DestroyAllNow(true);   // quiet: the NONE device destroys nothing
        }

        [[nodiscard]] BindlessTable& Table() { return *table; }

        void CompleteFence(const void* context, std::uint64_t fence)
        {
            std::uint64_t& c = completed[context];
            c = std::max(c, fence);
        }
        void CompleteEveryFence() { CompleteFence(ContextA(), 100); CompleteFence(ContextB(), 100); CompleteFence(ContextC(), 100); }

        [[nodiscard]] bool SetContains(nri::DescriptorSet* set, std::uint32_t slot, nri::Descriptor* srv) const
        {
            const auto s = sets.find(set);
            if (s == sets.end())
                return false;
            const auto d = s->second.find(slot);
            return d != s->second.end() && d->second == srv;
        }
    };

    // The four callers of the mapping-level release (spec 3.5 "Who calls Release"):
    //   0 material destruction  -- the last material reference drops
    //   1 texture invalidation  -- both colour spaces of the Guid
    //   2 project switch        -- every mapping
    //   3 texture eviction      -- the texture's own key, before the object dies
    enum Caller : int { MaterialDestruction = 0, TextureInvalidation = 1, ProjectSwitch = 2, TextureEviction = 3 };

    Arcane::Guid KeyFor(int caller) { return Arcane::Guid{ 40u + static_cast<std::uint64_t>(caller), 1u }; }
}

TEST_CASE("a full table recycles only after every sampling fence retires", "[nri][bindless]")
{
    DeviceFixture dev;
    BindlessTable& table = dev.Table();
    DeviceResourceLifetime& life = dev.life;

    std::vector<std::uint32_t> first;
    for (std::uint32_t i = 0; i < Arcane::kBindlessCapacity; ++i)
        first.push_back(table.Add(MockSrv(i)));
    for (std::uint32_t i = 0; i < Arcane::kBindlessCapacity; ++i)
        REQUIRE(first[i] == i);   // add-only traffic is dense

    // Both contexts sampled every slot in a submit that is still in flight.
    for (std::uint32_t slot : first)
    {
        life.NoteSample(slot, ContextA(), 1);
        life.NoteSample(slot, ContextB(), 1);
        table.Release(slot);
    }
    CHECK(table.PendingCount() == Arcane::kBindlessCapacity);
    // With any recorded fence still in flight nothing is recycled.
    CHECK(table.Add(MockSrv(0)) == BindlessTable::kInvalidSlot);

    // A retires, B does not yet: still nothing.
    dev.CompleteFence(ContextA(), 1);
    CHECK(table.Add(MockSrv(0)) == BindlessTable::kInvalidSlot);

    dev.CompleteEveryFence();
    std::vector<std::uint32_t> refilled;
    for (std::uint32_t i = 0; i < Arcane::kBindlessCapacity; ++i)
    {
        const std::uint32_t slot = table.Add(MockSrv(i));
        CHECK(slot < Arcane::kBindlessCapacity);
        refilled.push_back(slot);
    }
    // The table is full. Add returns kInvalidSlot and consumes nothing.
    CHECK(table.Add(MockSrv(0)) == BindlessTable::kInvalidSlot);
    // Free one refilled slot. It has no NoteSample, and a context with no
    // note is not a waiter, so AllRetired is already true and Release
    // returns that index to the free list. The in-flight slot stays out.
    const std::uint32_t spare = refilled[0];
    table.Release(spare);
    const std::uint32_t held = refilled[1];
    life.NoteSample(held, ContextA(), 101);   // A's submit 101 has not completed
    table.Release(held);
    const std::uint32_t next = table.Add(MockSrv(1));
    CHECK(next != BindlessTable::kInvalidSlot);
    CHECK(next == spare);
    CHECK(next != held);
}

TEST_CASE("context B does not recycle a slot context A is sampling", "[nri][bindless]")
{
    DeviceFixture dev;
    BindlessTable& table = dev.Table();
    table.RegisterSet(ContextA(), MockSet(1));
    table.RegisterSet(ContextB(), MockSet(2));

    nri::Descriptor* original = MockSrv(0);
    const std::uint32_t s = table.AddAndWrite(original, ContextA());
    REQUIRE(s != BindlessTable::kInvalidSlot);
    // One device-wide index, written into both contexts' descriptor sets.
    CHECK(dev.SetContains(MockSet(1), s, original));
    CHECK(dev.SetContains(MockSet(2), s, original));

    dev.life.NoteSample(s, ContextA(), 1);   // A stays in flight
    table.Release(s);                         // B (or anything else) releases it

    nri::Descriptor* second = MockSrv(1);
    const std::uint32_t b = table.AddAndWrite(second, ContextB());
    CHECK(b != s);
    CHECK(dev.SetContains(MockSet(1), b, second));
    CHECK(dev.SetContains(MockSet(2), b, second));

    dev.CompleteFence(ContextA(), 1);
    nri::Descriptor* third = MockSrv(2);
    const std::uint32_t later = table.AddAndWrite(third, ContextB());
    CHECK(later == s);
    // The recycled index now holds the new descriptor in BOTH sets.
    CHECK(dev.SetContains(MockSet(1), later, third));
    CHECK(dev.SetContains(MockSet(2), later, third));

    // A context created after the first materials still sees them.
    table.RegisterSet(ContextC(), MockSet(3));
    CHECK(dev.SetContains(MockSet(3), b, second));
    CHECK(dev.SetContains(MockSet(3), later, third));
}

TEST_CASE("add-only slots are 0..n-1", "[nri][bindless]")
{
    DeviceFixture dev;
    BindlessTable& table = dev.Table();
    CHECK(table.Add(MockSrv(0)) == 0u);
    CHECK(table.Add(MockSrv(1)) == 1u);
    CHECK(table.Add(MockSrv(2)) == 2u);
    CHECK(table.Add(MockSrv(3)) == 3u);
}

TEST_CASE("each release caller drops the mapping and dirties the row", "[nri][bindless]")
{
    for (int caller = MaterialDestruction; caller <= TextureEviction; ++caller)
    {
        INFO("caller " << caller);
        DeviceFixture dev;
        BindlessTable& table = dev.Table();
        table.RegisterSet(ContextA(), MockSet(1));
        table.RegisterSet(ContextB(), MockSet(2));

        const Arcane::Guid key = KeyFor(caller);
        const std::uint32_t slot = table.AddMapped(key, kSrgb, MockSrv(100 + caller), ContextA());
        REQUIRE(slot != BindlessTable::kInvalidSlot);
        CHECK(table.Lookup(key, kSrgb) == slot);

        // The GPU scene's mirror, the real one: a live row last staged with `slot`,
        // and a bystander row holding another index.
        Arcane::GpuSceneMirror mirror;
        mirror.rows.resize(2);
        mirror.rows[0].live = true; mirror.rows[0].lastSlot = slot;
        mirror.rows[1].live = true; mirror.rows[1].lastSlot = slot + 50u;
        // And a resolved-material stand-in (the resolver's observer resets these).
        std::uint32_t resolvedMaterialSlot = slot;
        table.SetObserver(&mirror, [&](std::uint32_t released)
        {
            (void)mirror.ReleaseMaterialSlot(released);
            if (resolvedMaterialSlot == released)
                resolvedMaterialSlot = BindlessTable::kInvalidSlot;
        });

        // Context B's submit that sampled the slot is still in flight.
        dev.life.NoteSample(slot, ContextB(), 1);

        switch (caller)
        {
        case MaterialDestruction:
            table.AcquireRef(key, kSrgb);   // two live materials share the albedo
            table.AcquireRef(key, kSrgb);
            table.Release(key, kSrgb);      // the first goes: the slot lives on
            CHECK(table.Lookup(key, kSrgb) == slot);
            CHECK(table.IsLive(slot));
            CHECK(resolvedMaterialSlot == slot);
            table.Release(key, kSrgb);      // the last goes: queued for the frame boundary...
            CHECK(table.Lookup(key, kSrgb) == slot);
            table.FlushZeroRefs();          // ...and released there, still unreferenced
            break;
        case TextureInvalidation:
            table.ReleaseAll(key, kSrgb);
            table.ReleaseAll(key, kDisplay);   // both colour spaces; Display was never mapped
            break;
        case ProjectSwitch:
            table.ReleaseEveryMapping();
            break;
        case TextureEviction:
            table.ReleaseAll(key, kSrgb);
            break;
        }

        // The mapping is gone, the rows fell back to kInvalidSlot and were dirtied.
        CHECK(table.Lookup(key, kSrgb) == BindlessTable::kInvalidSlot);
        CHECK(resolvedMaterialSlot == BindlessTable::kInvalidSlot);
        CHECK(mirror.rows[0].lastSlot == BindlessTable::kInvalidSlot);
        CHECK(std::find(mirror.dirtyLastFrame.begin(), mirror.dirtyLastFrame.end(), 0u)
              != mirror.dirtyLastFrame.end());
        CHECK(mirror.rows[1].lastSlot == slot + 50u);   // untouched
        CHECK(std::find(mirror.dirtyLastFrame.begin(), mirror.dirtyLastFrame.end(), 1u)
              == mirror.dirtyLastFrame.end());

        // A later resolve of the same key does not receive the abandoned memo, and
        // while the other context is in flight it does not get the old index either.
        CHECK(table.IsLive(slot) == false);
        const std::uint32_t again = table.AddMapped(key, kSrgb, MockSrv(200 + caller), ContextA());
        CHECK(again != BindlessTable::kInvalidSlot);
        CHECK(again != slot);
        const std::uint32_t other = table.Add(MockSrv(300 + caller));
        CHECK(other != slot);

        // Complete that fence: the index returns.
        dev.CompleteFence(ContextB(), 1);
        CHECK(table.Add(MockSrv(400 + caller)) == slot);
    }
}

TEST_CASE("a context tearing down releases the slots whose view names its objects", "[nri][bindless]")
{
    DeviceFixture dev;
    BindlessTable& table = dev.Table();
    const std::uint32_t mine   = table.AddMapped(Arcane::Guid{ 7, 1 }, kSrgb, MockSrv(1), ContextA());
    const std::uint32_t theirs = table.AddMapped(Arcane::Guid{ 7, 2 }, kSrgb, MockSrv(2), ContextB());
    dev.life.NoteSample(mine, ContextB(), 5);   // B is mid-flight on A's slot (never completes here)

    // Device idle (the teardown's own DeviceWaitIdle): A's slot retires at once.
    table.ReleaseOwnedBy(ContextA(), true);
    CHECK(table.Lookup(Arcane::Guid{ 7, 1 }, kSrgb) == BindlessTable::kInvalidSlot);
    CHECK(table.Lookup(Arcane::Guid{ 7, 2 }, kSrgb) == theirs);
    CHECK(table.PendingCount() == 0u);
    CHECK(table.Add(MockSrv(3)) == mine);
}

TEST_CASE("bindless descriptor writes idle at most once per frame without update-after-set",
          "[nri][bindless]")
{
    SECTION("the fallback path idles once for four slots in one frame")
    {
        DeviceFixture dev;
        BindlessTable& table = dev.Table();
        table.RegisterSet(ContextA(), MockSet(1));
        table.SetUpdateAfterSet(false);
        table.NoteFrame();
        for (std::uintptr_t i = 0; i < 4; ++i)
            CHECK(table.AddAndWrite(MockSrv(i), ContextA()) == static_cast<std::uint32_t>(i));
        CHECK(dev.idleCalls == 1);

        table.NoteFrame();   // the next frame that rewrites a descriptor idles again, once
        CHECK(table.AddAndWrite(MockSrv(4), ContextA()) == 4u);
        CHECK(table.AddAndWrite(MockSrv(5), ContextA()) == 5u);
        CHECK(dev.idleCalls == 2);
    }
    SECTION("update-after-set never idles inside Add")
    {
        DeviceFixture dev;
        BindlessTable& table = dev.Table();
        table.RegisterSet(ContextA(), MockSet(1));
        table.SetUpdateAfterSet(true);
        table.NoteFrame();
        for (std::uintptr_t i = 0; i < 4; ++i)
            CHECK(table.AddAndWrite(MockSrv(i), ContextA()) == static_cast<std::uint32_t>(i));
        CHECK(dev.idleCalls == 0);
    }
}

TEST_CASE("DeviceResourceLifetime waits for every noting context and ignores the rest", "[nri][bindless]")
{
    DeviceFixture dev;
    DeviceResourceLifetime& life = dev.life;
    CHECK(life.AllRetired(9, &DeviceFixture::Completed, &dev));   // no note, no waiter

    life.NoteSample(9, ContextA(), 3);
    life.NoteSample(9, ContextB(), 2);
    CHECK_FALSE(life.AllRetired(9, &DeviceFixture::Completed, &dev));
    dev.CompleteFence(ContextA(), 3);
    CHECK_FALSE(life.AllRetired(9, &DeviceFixture::Completed, &dev));   // B has not
    dev.CompleteFence(ContextB(), 2);
    CHECK(life.AllRetired(9, &DeviceFixture::Completed, &dev));

    // A fence from one timeline never retires another's: B at 100 says nothing about A.
    life.NoteSample(10, ContextA(), 200);
    dev.CompleteFence(ContextB(), 500);
    CHECK_FALSE(life.AllRetired(10, &DeviceFixture::Completed, &dev));

    life.ForgetContext(ContextA());   // the context went away
    CHECK(life.AllRetired(10, &DeviceFixture::Completed, &dev));
    life.Clear(9);
    CHECK(life.Uses(9) == nullptr);
}

// ---------------------------------------------------------------------------
// Fix round 1 (Task 2 review).
// ---------------------------------------------------------------------------

// C1: a pending (released, not yet retired) slot that a context records with
// is noted for that context, so it cannot retire under an in-flight use.
TEST_CASE("a pending slot sampled by a context does not retire until that context's fence completes",
          "[nri][bindless]")
{
    DeviceFixture dev;
    BindlessTable& table = dev.Table();

    const Arcane::Guid key{ 61, 1 };
    const std::uint32_t slot = table.AddMapped(key, kSrgb, MockSrv(1), ContextA());
    REQUIRE(slot != BindlessTable::kInvalidSlot);
    dev.life.NoteSample(slot, ContextA(), 1);   // A is in flight on it
    table.ReleaseAll(key, kSrgb);
    REQUIRE(table.PendingCount() == 1u);        // released, but A has not completed

    // B records a pass with the index AFTER it was released (a holder that had not
    // yet heard about the release): the table notes the pending slot for B too.
    table.NoteSampledBy(ContextB(), 7);
    REQUIRE(dev.life.Uses(slot) != nullptr);
    CHECK(dev.life.Uses(slot)->size() == 2u);

    dev.CompleteFence(ContextA(), 1);           // A retires; B's submit has not
    table.ProcessPending();
    CHECK(table.PendingCount() == 1u);
    CHECK(table.Add(MockSrv(2)) != slot);       // the index is not recycled

    dev.CompleteFence(ContextB(), 6);           // still short of 7
    table.ProcessPending();
    CHECK(table.PendingCount() == 1u);

    dev.CompleteFence(ContextB(), 7);
    table.ProcessPending();
    CHECK(table.PendingCount() == 0u);
    CHECK(table.Add(MockSrv(3)) == slot);       // now it is
}

// Round 2 (N1): a mesh pass records every frame. A pending slot is noted by a
// context only on its FIRST record after the release, and the record also runs
// the retire sweep -- so the slot retires with no Add in between.
TEST_CASE("a pending slot retires after repeated records once the first fence completes",
          "[nri][bindless]")
{
    DeviceFixture dev;
    BindlessTable& table = dev.Table();

    const Arcane::Guid key{ 63, 1 };
    const std::uint32_t slot = table.AddMapped(key, kSrgb, MockSrv(1), ContextA());
    REQUIRE(slot != BindlessTable::kInvalidSlot);
    table.NoteSampledBy(ContextA(), 1);          // live: noted every record
    table.ReleaseAll(key, kSrgb);
    REQUIRE(table.PendingCount() == 1u);

    table.NoteSampledBy(ContextA(), 2);          // the first record after the release
    table.NoteSampledBy(ContextA(), 3);          // later records do not move the wait
    table.NoteSampledBy(ContextA(), 4);
    REQUIRE(dev.life.Uses(slot) != nullptr);
    REQUIRE(dev.life.Uses(slot)->size() == 1u);
    CHECK(dev.life.Uses(slot)->front().fence == 2u);
    CHECK(table.PendingCount() == 1u);

    dev.CompleteFence(ContextA(), 2);
    table.NoteSampledBy(ContextA(), 5);          // no Add: the record's sweep retires it
    CHECK(table.PendingCount() == 0u);
    CHECK(table.FreeCount() == 1u);
    CHECK(table.Add(MockSrv(2)) == slot);
}

// Round 2 (N1b): a context whose first record comes after the release still
// blocks the retire until that one fence completes.
TEST_CASE("a context recording for the first time after the release blocks the retire",
          "[nri][bindless]")
{
    DeviceFixture dev;
    BindlessTable& table = dev.Table();

    const Arcane::Guid key{ 64, 1 };
    const std::uint32_t slot = table.AddMapped(key, kSrgb, MockSrv(1), ContextA());
    REQUIRE(slot != BindlessTable::kInvalidSlot);
    table.NoteSampledBy(ContextA(), 1);
    table.ReleaseAll(key, kSrgb);

    table.NoteSampledBy(ContextA(), 2);
    dev.CompleteFence(ContextA(), 2);
    table.NoteSampledBy(ContextB(), 9);          // B's first record: noted at 9
    table.NoteSampledBy(ContextA(), 3);
    CHECK(table.PendingCount() == 1u);           // A is done, B is not

    table.NoteSampledBy(ContextB(), 10);         // B's later records change nothing
    dev.CompleteFence(ContextB(), 8);
    table.NoteSampledBy(ContextA(), 4);
    CHECK(table.PendingCount() == 1u);

    dev.CompleteFence(ContextB(), 9);
    table.NoteSampledBy(ContextA(), 5);
    CHECK(table.PendingCount() == 0u);
    CHECK(table.FreeCount() == 1u);
}

// Round 2 (N3): a queued zero-ref key does not outlive an unconditional release.
TEST_CASE("an unconditional release purges the queued zero-ref keys", "[nri][bindless]")
{
    DeviceFixture dev;
    BindlessTable& table = dev.Table();

    const Arcane::Guid k1{ 65, 1 };
    const Arcane::Guid k2{ 66, 1 };
    REQUIRE(table.AddMapped(k1, kSrgb, MockSrv(1), ContextA()) != BindlessTable::kInvalidSlot);
    REQUIRE(table.AddMapped(k2, kSrgb, MockSrv(2), ContextA()) != BindlessTable::kInvalidSlot);
    table.AcquireRef(k1, kSrgb);
    table.AcquireRef(k2, kSrgb);
    table.Release(k1, kSrgb);                    // both queued at zero
    table.Release(k2, kSrgb);

    table.ReleaseAll(k1, kSrgb);
    table.ReleaseEveryMapping();
    // Re-map both under the same keys: a stale queued key (zero refs) would release them.
    const std::uint32_t s1 = table.AddMapped(k1, kSrgb, MockSrv(3), ContextA());
    const std::uint32_t s2 = table.AddMapped(k2, kSrgb, MockSrv(4), ContextA());
    REQUIRE(s1 != BindlessTable::kInvalidSlot);
    REQUIRE(s2 != BindlessTable::kInvalidSlot);
    table.FlushZeroRefs();
    CHECK(table.Lookup(k1, kSrgb) == s1);
    CHECK(table.Lookup(k2, kSrgb) == s2);
}

// I1: texture invalidation is device-wide: every registered context's texture
// cache drops the guid and the device mapping is released, so no stale cache
// can publish its old view back to the other contexts.
TEST_CASE("a device-wide texture invalidation reaches every context's texture cache",
          "[nri][bindless][texcache]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    const Arcane::Guid id{ 62, 1 };
    Arcane::PixelData pixels;
    pixels.width = 2;
    pixels.height = 2;
    pixels.rgba.assign(16, 0xAB);
    auto supply = [&](const Arcane::Guid&) -> const Arcane::PixelData* { return &pixels; };

    // Two contexts, each with its OWN cache and its OWN graveyard (own timeline).
    struct Ctx
    {
        std::unique_ptr<Arcane::NriTextureCache> cache;
        Arcane::Graveyard graves;
        std::uint64_t fence = 1;
    };
    Ctx a, b;
    for (Ctx* c : { &a, &b })
    {
        c->cache = Arcane::NriTextureCache::Create(*device);
        REQUIRE(c->cache != nullptr);
        c->cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)
        c->cache->SetPixelSupply(supply);
        device->RegisterContext(c, [c](const Arcane::Guid& g) { c->cache->Invalidate(g, c->graves, c->fence); });
    }
    CHECK(device->RegisteredContextCount() == 2u);

    BindlessTable* table = device->Bindless();
    REQUIRE(table != nullptr);

    REQUIRE(a.cache->Resolve(id) != nullptr);
    REQUIRE(b.cache->Resolve(id) != nullptr);
    CHECK(a.cache->ResidentCount() == 1u);
    CHECK(b.cache->ResidentCount() == 1u);
    // N2: map the cache's REAL texture, so Invalidate's adoption branch runs.
    // Context A's submit (fence 5 on its timeline) still samples the slot.
    struct Done { std::uint64_t fence = 0; } done;
    table->SetCompletionQuery(
        [](const void*, std::uint64_t f, void* user) { return static_cast<Done*>(user)->fence >= f; }, &done);
    const nri::Texture* aTexture = a.cache->Resolve(id);
    const std::uint32_t first = table->AddMapped(id, kSrgb, MockSrv(1), &a, aTexture);
    REQUIRE(first != BindlessTable::kInvalidSlot);
    table->Lifetime().NoteSample(first, &a, 5);

    // The cook for `id` lands: ONE call, however many contexts exist.
    device->InvalidateTexture(id);

    CHECK(a.cache->ResidentCount() == 0u);   // neither cache returns the old texture
    CHECK(b.cache->ResidentCount() == 0u);
    CHECK(table->Lookup(id, kSrgb) == BindlessTable::kInvalidSlot);   // mapping released
    CHECK(table->BackingCount() >= 1u);      // the texture waits on the slot's retire (the NONE backend hands every cache the same fake handle, so b's drop may join it too)
    CHECK(table->PendingCount() == 1u);
    done.fence = 5;
    table->ProcessPending();                 // retired: descriptor, then the texture
    CHECK(table->BackingCount() == 0u);
    CHECK(table->PendingCount() == 0u);

    // The next resolve is a fresh upload in each cache and a fresh mapping.
    REQUIRE(a.cache->Resolve(id) != nullptr);
    REQUIRE(b.cache->Resolve(id) != nullptr);
    CHECK(a.cache->ResidentCount() == 1u);
    CHECK(b.cache->ResidentCount() == 1u);
    const std::uint32_t second = table->AddMapped(id, kSrgb, MockSrv(2), &a);
    CHECK(second != BindlessTable::kInvalidSlot);
    CHECK(table->Lookup(id, kSrgb) == second);

    // A context that unregisters is no longer reached.
    device->UnregisterContext(&b);
    device->InvalidateTexture(id);
    CHECK(a.cache->ResidentCount() == 0u);
    CHECK(b.cache->ResidentCount() == 1u);

    device->UnregisterContext(&a);
    for (Ctx* c : { &a, &b })
    {
        c->cache->Release(c->graves, c->fence + 1);
        c->graves.Reap(c->fence + 1);
    }
    table->DestroyAllNow(true);
}

// I2: the texture and its descriptor wait on the last-use record together.
TEST_CASE("an invalidated texture is not destroyed until every context sampling its slot retires",
          "[nri][bindless]")
{
    DeviceFixture dev;
    BindlessTable& table = dev.Table();
    nri::Texture* const tex = reinterpret_cast<nri::Texture*>(0x7000u);   // context A's texture

    const Arcane::Guid key{ 63, 1 };
    const std::uint32_t slot = table.AddMapped(key, kSrgb, MockSrv(1), ContextA(), tex);
    REQUIRE(slot != BindlessTable::kInvalidSlot);
    dev.life.NoteSample(slot, ContextB(), 3);   // B samples a descriptor viewing A's texture

    // A invalidates: the mapping is released first (pending: B is in flight)...
    table.ReleaseAll(key, kSrgb);
    REQUIRE(table.PendingCount() == 1u);

    // ...then A's cache drops the texture, which joins the slot's retire.
    int destroyed = 0;
    bool descriptorFirst = false;
    const bool adopted = table.AdoptTextureBacking(tex, 5, [&]
    {
        ++destroyed;
        // By the time the texture goes, the slot's descriptor is already gone
        // (the index is back on the free list).
        descriptorFirst = !table.IsLive(slot) && table.FreeCount() == 1u;
    });
    REQUIRE(adopted);
    CHECK(table.BackingCount() == 1u);

    dev.CompleteFence(ContextA(), 5);           // A's own submit done, B's is not
    table.ProcessPending();
    CHECK(destroyed == 0);
    dev.CompleteFence(ContextB(), 2);           // B short of 3
    table.ProcessPending();
    CHECK(destroyed == 0);

    dev.CompleteFence(ContextB(), 3);
    table.ProcessPending();
    CHECK(destroyed == 1);
    CHECK(descriptorFirst);
    CHECK(table.BackingCount() == 0u);

    // A texture no slot views keeps the caller's own path.
    nri::Texture* const other = reinterpret_cast<nri::Texture*>(0x7100u);
    CHECK_FALSE(table.AdoptTextureBacking(other, 9, [] {}));
    // A slot that retired at once (no sampler in flight) is not waited on.
    const std::uint32_t quick = table.AddMapped(Arcane::Guid{ 63, 2 }, kSrgb, MockSrv(2), ContextA(), other);
    REQUIRE(quick != BindlessTable::kInvalidSlot);
    table.ReleaseAll(Arcane::Guid{ 63, 2 }, kSrgb);
    CHECK(table.PendingCount() == 0u);
    CHECK_FALSE(table.AdoptTextureBacking(other, 9, [] {}));
}

namespace
{
    namespace fs = std::filesystem;

    fs::path MakeAlbedoDir(const char* tag)
    {
        std::error_code ec;
        fs::path d = fs::temp_directory_path() / (std::string("arcane_bindless_fix1_") + tag);
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    // A mesh-kind .arcmat declaring `albedo`; returns the material's id.
    Arcane::Guid WriteAlbedoMaterial(const fs::path& file, const Arcane::Guid& albedo)
    {
        Arcane::MaterialAssetData data;
        data.id   = Arcane::Guid::Generate();
        data.name = "fix1-material";
        data.kind = "mesh";
        data.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
        data.params.emplace_back("albedo", Arcane::MatParamValue::MakeTexture(albedo));
        REQUIRE(Arcane::SaveMaterialAsset(file, data));
        return data.id;
    }

    // The resolver's wiring in miniature: resolve through the device table (Lookup
    // else a fresh mapping) and hold a reference; release drops one.
    struct TableBackedServices
    {
        BindlessTable* table = nullptr;
        std::uint32_t  minted = 0;

        std::uint32_t Acquire(const Arcane::Guid& g)
        {
            std::uint32_t slot = table->Lookup(g, kSrgb);
            if (slot == BindlessTable::kInvalidSlot)
            {
                ++minted;
                slot = table->AddMapped(g, kSrgb, MockSrv(minted), ContextA());
            }
            if (slot != BindlessTable::kInvalidSlot)
                table->AcquireRef(g, kSrgb);
            return slot;
        }
        void ReleaseRef(const Arcane::Guid& g) { table->Release(g, kSrgb); }
    };
}

// I3: a whole-cache Clear (every material save) followed by the same frame's
// re-resolve keeps every slot: no new index, nothing pending.
TEST_CASE("a Clear and re-resolve in one frame keeps every albedo slot", "[nri][bindless][mesh]")
{
    constexpr int N = 12;
    const fs::path dir = MakeAlbedoDir("clear");
    DeviceFixture dev;
    TableBackedServices dsvc;
    dsvc.table = &dev.Table();

    std::vector<Arcane::Guid> albedos, materials;
    for (int i = 0; i < N; ++i)
    {
        albedos.push_back(Arcane::Guid{ 70, static_cast<std::uint64_t>(i + 1) });
        materials.push_back(WriteAlbedoMaterial(dir / ("m" + std::to_string(i) + ".arcmat"), albedos.back()));
    }
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid& id) -> std::optional<fs::path>
    {
        for (int i = 0; i < N; ++i)
            if (materials[i] == id)
                return dir / ("m" + std::to_string(i) + ".arcmat");
        return std::nullopt;
    };
    s.resolveAlbedoSlot = [&](const Arcane::Guid& g) { return dsvc.Acquire(g); };
    s.releaseAlbedoSlot = [&](const Arcane::Guid& g) { dsvc.ReleaseRef(g); };
    Arcane::MeshMaterialCache cache(std::move(s));

    std::vector<std::uint32_t> before;
    for (const Arcane::Guid& m : materials) cache.Request(m);
    for (const Arcane::Guid& m : materials) before.push_back(cache.Table().at(m).materialSlot);
    for (std::uint32_t slot : before) REQUIRE(slot != BindlessTable::kInvalidSlot);
    CHECK(dev.Table().LiveCount() == static_cast<std::uint32_t>(N));
    const std::uint32_t mintedBefore = dsvc.minted;

    // The material save: Clear, then the same frame's Refresh re-Requests every
    // referenced material, then the frame boundary.
    cache.Clear();
    for (const Arcane::Guid& m : materials) cache.Request(m);
    dev.Table().FlushZeroRefs();

    for (int i = 0; i < N; ++i)
        CHECK(cache.Table().at(materials[i]).materialSlot == before[i]);   // every index kept
    CHECK(dsvc.minted == mintedBefore);                                    // no new mint
    CHECK(dev.Table().PendingCount() == 0u);
    CHECK(dev.Table().LiveCount() == static_cast<std::uint32_t>(N));

    // A material that is NOT re-requested is released at the boundary.
    cache.Clear();
    for (int i = 0; i < N - 1; ++i) cache.Request(materials[i]);
    dev.Table().FlushZeroRefs();
    CHECK(dev.Table().LiveCount() == static_cast<std::uint32_t>(N - 1));
    CHECK(dev.Table().Lookup(albedos[N - 1], kSrgb) == BindlessTable::kInvalidSlot);

    std::error_code ec; fs::remove_all(dir, ec);
}

// I3: a Request that finds no free slot is queued for re-resolve, not stored flat.
TEST_CASE("a material that finds the table full is re-resolved once a slot frees", "[nri][bindless][mesh]")
{
    const fs::path dir = MakeAlbedoDir("pressure");
    DeviceFixture dev(2);   // capacity 2, three materials
    TableBackedServices dsvc;
    dsvc.table = &dev.Table();

    std::vector<Arcane::Guid> albedos, materials;
    for (int i = 0; i < 3; ++i)
    {
        albedos.push_back(Arcane::Guid{ 71, static_cast<std::uint64_t>(i + 1) });
        materials.push_back(WriteAlbedoMaterial(dir / ("m" + std::to_string(i) + ".arcmat"), albedos.back()));
    }
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid& id) -> std::optional<fs::path>
    {
        for (int i = 0; i < 3; ++i)
            if (materials[i] == id)
                return dir / ("m" + std::to_string(i) + ".arcmat");
        return std::nullopt;
    };
    s.resolveAlbedoSlot = [&](const Arcane::Guid& g) { return dsvc.Acquire(g); };
    s.releaseAlbedoSlot = [&](const Arcane::Guid& g) { dsvc.ReleaseRef(g); };
    Arcane::MeshMaterialCache cache(std::move(s));

    for (const Arcane::Guid& m : materials) cache.Request(m);
    CHECK(cache.Table().at(materials[0]).materialSlot != BindlessTable::kInvalidSlot);
    CHECK(cache.Table().at(materials[1]).materialSlot != BindlessTable::kInvalidSlot);
    CHECK(cache.Table().at(materials[2]).materialSlot == BindlessTable::kInvalidSlot);   // table full

    // Still full: a later Request retries and still gets nothing (no growth).
    cache.Request(materials[2]);
    CHECK(cache.Table().at(materials[2]).materialSlot == BindlessTable::kInvalidSlot);

    // Material 0 goes away: its slot frees at the frame boundary...
    cache.Invalidate(materials[0]);
    dev.Table().FlushZeroRefs();
    // ...and the starved material picks it up on its next Request.
    cache.Request(materials[2]);
    CHECK(cache.Table().at(materials[2]).materialSlot != BindlessTable::kInvalidSlot);

    std::error_code ec; fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// [gpu]: two contexts on ONE real device share the bindless index space. A
// slot context A and B both sampled is released, the new descriptor is written
// at a different index while both are in flight, and the old index is reused
// only after both have retired -- with the validation layers (D3D12 debug
// layer, Vulkan core + sync validation) quiet throughout.
// ---------------------------------------------------------------------------
namespace
{
    constexpr std::uint32_t kGpuW = 64;
    constexpr std::uint32_t kGpuH = 64;

    struct GpuSolid
    {
        nri::Texture*    texture = nullptr;
        nri::Descriptor* view    = nullptr;
    };

    GpuSolid MakeGpuSolid(Arcane::NriDevice& device, std::uint32_t rgba)
    {
        const nri::CoreInterface& core = device.Core();
        GpuSolid out;

        nri::TextureDesc textureDesc = {};
        textureDesc.type      = nri::TextureType::TEXTURE_2D;
        textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
        textureDesc.format    = nri::Format::RGBA8_UNORM;
        textureDesc.width     = 1;
        textureDesc.height    = 1;
        textureDesc.depth     = 1;
        textureDesc.mipNum    = 1;
        textureDesc.layerNum  = 1;
        textureDesc.sampleNum = 1;
        REQUIRE(core.CreateCommittedTexture(device.Device(), nri::MemoryLocation::DEVICE, 0.0f,
                                            textureDesc, out.texture) == nri::Result::SUCCESS);

        nri::HelperInterface helper = {};
        REQUIRE(nriGetInterface(device.Device(), NRI_INTERFACE(nri::HelperInterface), &helper)
                == nri::Result::SUCCESS);
        nri::TextureSubresourceUploadDesc subresource = {};
        subresource.slices     = &rgba;
        subresource.sliceNum   = 1;
        subresource.rowPitch   = 4;
        subresource.slicePitch = 4;
        nri::TextureUploadDesc upload = {};
        upload.subresources = &subresource;
        upload.texture      = out.texture;
        upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                nri::StageBits::FRAGMENT_SHADER };
        upload.planes       = nri::PlaneBits::ALL;
        REQUIRE(helper.UploadData(*device.GraphicsQueue(), &upload, 1, nullptr, 0) == nri::Result::SUCCESS);

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = out.texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        REQUIRE(core.CreateTextureView(viewDesc, out.view) == nri::Result::SUCCESS);
        return out;
    }

    void RunTwoContextRecycle(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();

        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
#if defined(ARC_BUILD_DEBUG)
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;   // VK-only
#endif
        auto native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(native != nullptr);
        auto nri = Arcane::NriDevice::Wrap(*native);
        REQUIRE(nri != nullptr);

        Arcane::HostConfig cfg;
        cfg.backend = backend;
        auto ctxA = Arcane::NriGraphContext::CreateOffscreen(cfg, *nri, kGpuW, kGpuH, {});
        auto ctxB = Arcane::NriGraphContext::CreateOffscreen(cfg, *nri, kGpuW, kGpuH, {});
        REQUIRE(ctxA != nullptr);
        REQUIRE(ctxB != nullptr);

        BindlessTable* table = nri->Bindless();
        REQUIRE(table != nullptr);

        const GpuSolid red   = MakeGpuSolid(*nri, 0xFF0000FFu);
        const GpuSolid green = MakeGpuSolid(*nri, 0xFF00FF00u);
        const GpuSolid blue  = MakeGpuSolid(*nri, 0xFFFF0000u);

        // One device-wide index, written into BOTH contexts' bindless sets.
        const std::uint32_t s = table->AddMapped(Arcane::Guid{ 90, 1 }, kSrgb, red.view, nullptr);
        REQUIRE(s != BindlessTable::kInvalidSlot);

        // Both contexts' next submit samples it.
        nri->ResourceLifetime().NoteSample(s, ctxA.get(), ctxA->CurrentFence());
        nri->ResourceLifetime().NoteSample(s, ctxB.get(), ctxB->CurrentFence());
        table->ReleaseAll(Arcane::Guid{ 90, 1 }, kSrgb);
        CHECK(table->PendingCount() == 1u);

        // While both are in flight the new descriptor lands on a different index.
        const std::uint32_t t = table->AddMapped(Arcane::Guid{ 90, 2 }, kSrgb, green.view, nullptr);
        REQUIRE(t != BindlessTable::kInvalidSlot);
        CHECK(t != s);

        // A submits; B has not yet: the index is still not recyclable.
        REQUIRE(ctxA->RenderFrameOffscreen({}) == Arcane::NriGraphContext::FrameOutcome::Presented);
        {
            const nri::CoreInterface& core = nri->Core();
            REQUIRE(core.DeviceWaitIdle(&nri->Device()) == nri::Result::SUCCESS);
        }
        table->ProcessPending();
        CHECK(table->PendingCount() == 1u);

        REQUIRE(ctxB->RenderFrameOffscreen({}) == Arcane::NriGraphContext::FrameOutcome::Presented);
        {
            const nri::CoreInterface& core = nri->Core();
            REQUIRE(core.DeviceWaitIdle(&nri->Device()) == nri::Result::SUCCESS);
        }
        table->ProcessPending();
        CHECK(table->PendingCount() == 0u);

        // Now the old index returns, and rewriting it in both sets is legal.
        const std::uint32_t u = table->AddMapped(Arcane::Guid{ 90, 3 }, kSrgb, blue.view, nullptr);
        CHECK(u == s);

        // Teardown: descriptors before the textures they view.
        table->ReleaseEveryMapping();
        {
            const nri::CoreInterface& core = nri->Core();
            REQUIRE(core.DeviceWaitIdle(&nri->Device()) == nri::Result::SUCCESS);
        }
        table->ProcessPending();
        CHECK(table->LiveCount() == 0u);
        CHECK(table->PendingCount() == 0u);

        ctxB.reset();
        ctxA.reset();
        const nri::CoreInterface& core = nri->Core();
        core.DestroyTexture(red.texture);
        core.DestroyTexture(green.texture);
        core.DestroyTexture(blue.texture);
        nri.reset();
        native.reset();

        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }
}

TEST_CASE("two contexts recycle a bindless slot without validation errors", "[gpu][nri][bindless]")
{
    const Arcane::GraphicsBackend backends[] = { Arcane::GraphicsBackend::D3D12, Arcane::GraphicsBackend::Vulkan };
    const bool any = Arcane::Test::BackendAvailable(backends[0]) || Arcane::Test::BackendAvailable(backends[1]);
    if (!any)
        SKIP("no graphics backend available on this machine");

    // Both backends in one case (the brief names one case); a backend this machine
    // lacks is skipped, never both.
    for (const Arcane::GraphicsBackend backend : backends)
    {
        if (!Arcane::Test::BackendAvailable(backend))
            continue;
        DYNAMIC_SECTION(Arcane::Test::BackendName(backend))
        {
            RunTwoContextRecycle(backend);
        }
    }
}
