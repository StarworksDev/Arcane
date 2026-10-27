// One residency account per device (3D-readiness arc, Plan C Task 4, spec 3.x).
//
// Meshes and textures are two slices of ONE account; every cache on a device charges
// it. The policy is Admit (device-less), so the locked numbers and every tie-break
// are plain arithmetic. The last case drives the real caches on the NONE backend to
// prove an evicted texture waits on the context that sampled it. The locked case
// passes ResidencyCaps explicitly (512 MiB, 512 MiB, 50 percent): those are the
// spec's provisional numbers, not a second production default.
//
// Include order: NRI headers first -- see NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIHelper.h>
#include <Arcane/Render/Nri/NriDevice.hpp>   // pulls Extensions/NRIDeviceCreation.h: before anything that reaches <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Nri/BindlessTable.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/Nri/ResidencyAccount.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace
{
    using Arcane::Admit;
    using Arcane::Guid;
    using Arcane::ResidencyAccount;
    using Arcane::ResidencyCaps;
    using Arcane::ResidencyEntry;
    using Arcane::ResidencyEviction;
    using Arcane::ResidencySlice;
    using Space = Arcane::NriTextureCache::ColorSpace;

    constexpr std::uint64_t MiB(std::uint64_t n) { return n << 20; }

    // The spec's provisional case, passed explicitly so a later default change cannot move it.
    ResidencyCaps LockedCaps()
    {
        ResidencyCaps caps;
        caps.meshCap = MiB(512);
        caps.textureCap = MiB(512);
        caps.reservationPercent = 50;
        return caps;
    }

    ResidencyEntry Tex(std::uint64_t guid, std::uint64_t mib, std::uint64_t frame, Space space = Space::Srgb)
    {
        return ResidencyEntry{ Guid{ guid, 0 }, ResidencySlice::Texture, space, MiB(mib), frame };
    }
    ResidencyEntry Mesh(std::uint64_t guid, std::uint64_t mib, std::uint64_t frame)
    {
        return ResidencyEntry{ Guid{ guid, 0 }, ResidencySlice::Mesh, Space::Srgb, MiB(mib), frame };
    }

    std::uint64_t Used(const std::vector<ResidencyEntry>& entries, ResidencySlice slice)
    {
        std::uint64_t n = 0;
        for (const ResidencyEntry& e : entries)
            if (e.slice == slice)
                n += e.bytes;
        return n;
    }
    bool Has(const std::vector<ResidencyEntry>& entries, std::uint64_t guid, ResidencySlice slice,
             Space space = Space::Srgb)
    {
        for (const ResidencyEntry& e : entries)
            if (e.id == Guid{ guid, 0 } && e.slice == slice && (slice == ResidencySlice::Mesh || e.space == space))
                return true;
        return false;
    }

    // One vehicle's view of the shared account: it charges onto the SAME entry list.
    struct MeshCacheFacade
    {
        std::vector<ResidencyEntry>* shared;
        ResidencyCaps caps;
        std::vector<ResidencyEviction> Charge(std::uint64_t guid, std::uint64_t mib, std::uint64_t frame)
        {
            return Admit(*shared, Mesh(guid, mib, 0), ResidencySlice::Mesh, caps, frame);
        }
    };

    nri::Descriptor* MockSrv(std::uintptr_t tag)
    {
        return reinterpret_cast<nri::Descriptor*>(0x1000u + tag * 16u);
    }
}

TEST_CASE("700 MiB of textures plus a 512 MiB mesh sums to 1024", "[residency][account]")
{
    std::vector<ResidencyEntry> entries;
    entries.push_back(Tex(1, 188, 1));   // T1
    entries.push_back(Tex(2, 512, 2));   // T2

    const std::vector<ResidencyEviction> evicted =
        Admit(entries, Mesh(3, 512, 0), ResidencySlice::Mesh, LockedCaps(), /*currentFrame=*/10);

    const std::uint64_t meshUsed = Used(entries, ResidencySlice::Mesh);
    const std::uint64_t textureUsed = Used(entries, ResidencySlice::Texture);
    const std::uint64_t sum = meshUsed + textureUsed;
    CHECK(sum == (1024ull << 20));
    CHECK(meshUsed == (512ull << 20));
    CHECK(textureUsed == (512ull << 20));
    CHECK(evicted.size() == 1);
    REQUIRE_FALSE(evicted.empty());
    CHECK(evicted[0].id == Arcane::Guid{1, 0}); // T1
    CHECK(evicted[0].slice == Arcane::ResidencySlice::Texture);
    CHECK(Has(entries, 2, ResidencySlice::Texture));   // T2 stays
    CHECK(Has(entries, 3, ResidencySlice::Mesh));      // M stays

    // The arithmetic the policy leans on: multiply, then divide.
    CHECK(Arcane::Reservation(MiB(512), 50) == MiB(256));
    CHECK(Arcane::Reservation(7, 50) == 3ull);            // cap / 100 * percent would be 0
    CHECK(Arcane::UnusedSurplus(MiB(700), MiB(512), MiB(256)) == 0ull);
    CHECK(Arcane::UnusedSurplus(MiB(100), MiB(512), MiB(256)) == MiB(256));
    CHECK(Arcane::UnusedSurplus(MiB(400), MiB(512), MiB(256)) == MiB(112));
}

TEST_CASE("a second vehicle charges the same account", "[residency][account]")
{
    // Two mesh-cache facades over ONE entry list. Neither has a private 512.
    std::vector<ResidencyEntry> shared;
    MeshCacheFacade first{ &shared, LockedCaps() };
    MeshCacheFacade second{ &shared, LockedCaps() };

    CHECK(first.Charge(1, 400, 1).empty());
    CHECK(shared.size() == 1);

    // 800 MiB of meshes does not fit the shared mesh cap even with the texture
    // slice's whole unused surplus lent out (512 + 256 = 768). A private second 512
    // would have kept both; the shared account evicts the first vehicle's mesh.
    const std::vector<ResidencyEviction> evicted = second.Charge(2, 400, 2);
    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0].id == Guid{ 1, 0 });
    CHECK(evicted[0].slice == ResidencySlice::Mesh);
    CHECK(shared.size() == 1);
    CHECK(Used(shared, ResidencySlice::Mesh) == MiB(400));

    // The same shape on the real caches: two caches on ONE device share ONE account.
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cacheA = Arcane::NriMeshBufferCache::Create(*device);
    auto cacheB = Arcane::NriMeshBufferCache::Create(*device);
    REQUIRE(cacheA != nullptr);
    REQUIRE(cacheB != nullptr);
    CHECK(&device->Residency() == &device->Residency());
    CHECK(cacheA->Budget() == device->Residency().Caps().meshCap);
    CHECK(cacheB->Budget() == device->Residency().Caps().meshCap);

    Arcane::MeshData cube = Arcane::BuildCube(1.0f);
    const auto supply = [&](const Guid&)
    { return Arcane::NriMeshBufferCache::SupplyResult{ &cube, Arcane::MeshResolveState::Ready }; };
    cacheA->SetMeshSupply(supply);
    cacheB->SetMeshSupply(supply);

    const Arcane::NriMeshBufferCache::Resident* a = cacheA->Resolve(Guid{ 11, 0 }, 1);
    const Arcane::NriMeshBufferCache::Resident* b = cacheB->Resolve(Guid{ 12, 0 }, 1);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(device->Residency().Used(ResidencySlice::Mesh) == a->bytes + b->bytes);   // one account, both charged
    CHECK(device->Residency().EntryCount() == 2u);

    // The same guid resident in both vehicles is ONE entry whose bytes are the SUM of the copies.
    REQUIRE(cacheB->Resolve(Guid{ 11, 0 }, 2) != nullptr);
    CHECK(device->Residency().EntryCount() == 2u);
    CHECK(device->Residency().EntryBytes(Guid{ 11, 0 }, ResidencySlice::Mesh, Space::Srgb) == 2u * a->bytes);

    // A cache that drops a key on its own uncharges ITS copy only.
    Arcane::Graveyard lane;
    cacheB->Invalidate(Guid{ 11, 0 }, lane, 1);
    CHECK(device->Residency().EntryBytes(Guid{ 11, 0 }, ResidencySlice::Mesh, Space::Srgb) == a->bytes);
    cacheA->Release(lane, 1);
    cacheB->Release(lane, 1);
    CHECK(device->Residency().EntryCount() == 0u);
    lane.Reap(1);
}

TEST_CASE("a texture admit borrows mesh surplus and does not evict a mesh inside the reservation",
          "[residency][account]")
{
    std::vector<ResidencyEntry> entries;
    entries.push_back(Mesh(1, 100, 1));   // inside the 256 MiB mesh reservation
    entries.push_back(Tex(2, 400, 2));

    // 700 MiB of textures against a 512 cap: it fits only by borrowing the mesh slice's
    // unused 256 MiB (cap 512 minus its reservation), so nothing is evicted.
    const std::vector<ResidencyEviction> borrowed =
        Admit(entries, Tex(3, 300, 0), ResidencySlice::Texture, LockedCaps(), 10);
    CHECK(borrowed.empty());
    CHECK(Used(entries, ResidencySlice::Texture) == MiB(700));
    CHECK(Has(entries, 1, ResidencySlice::Mesh));

    // 800 MiB is past the borrowable 768: the TEXTURE slice pays, oldest cold texture
    // first, and the mesh inside its reservation is never touched.
    const std::vector<ResidencyEviction> over =
        Admit(entries, Tex(4, 100, 0), ResidencySlice::Texture, LockedCaps(), 11);
    REQUIRE(over.size() == 1);
    CHECK(over[0].id == Guid{ 2, 0 });
    CHECK(over[0].slice == ResidencySlice::Texture);
    CHECK(Has(entries, 1, ResidencySlice::Mesh));
    CHECK(Used(entries, ResidencySlice::Texture) == MiB(400));
}

TEST_CASE("equal age evicts the texture before the mesh", "[residency][account]")
{
    std::vector<ResidencyEntry> entries;
    entries.push_back(Tex(1, 300, 3));    // Ta, oldest on the texture slice
    entries.push_back(Tex(2, 300, 5));    // Tb
    entries.push_back(Mesh(3, 300, 3));   // Ma, the same age as Ta

    // The mesh requester fits its own allowance (512 of 512), so the texture slice
    // (600 of 512) is the one over: the oldest cold entry on a slice above its
    // reservation. Ta and Ma are equally old; a texture goes before a mesh.
    const std::vector<ResidencyEviction> evicted =
        Admit(entries, Mesh(4, 212, 0), ResidencySlice::Mesh, LockedCaps(), 10);
    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0].id == Guid{ 1, 0 });
    CHECK(evicted[0].slice == ResidencySlice::Texture);
    CHECK(Has(entries, 3, ResidencySlice::Mesh));

    // Equal age, same slice: the smaller Guid first.
    std::vector<ResidencyEntry> tie;
    tie.push_back(Tex(9, 300, 4));
    tie.push_back(Tex(7, 300, 4));
    const std::vector<ResidencyEviction> byGuid =
        Admit(tie, Tex(8, 300, 0), ResidencySlice::Texture, LockedCaps(), 10);
    REQUIRE(byGuid.size() == 1);
    CHECK(byGuid[0].id == Guid{ 7, 0 });
}

TEST_CASE("a mesh drawn this frame is not evicted for another vehicle", "[residency][account]")
{
    std::vector<ResidencyEntry> entries;
    entries.push_back(Mesh(1, 512, 10));   // drawn this frame by the first vehicle
    entries.push_back(Mesh(2, 100, 1));    // cold

    // The second vehicle's texture does not fit. The drawn mesh is protected on its slice
    // whichever vehicle asked; only the cold mesh can go, and the frame renders anyway.
    const std::vector<ResidencyEviction> evicted =
        Admit(entries, Tex(3, 600, 0), ResidencySlice::Texture, LockedCaps(), 10);
    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0].id == Guid{ 2, 0 });
    CHECK(Has(entries, 1, ResidencySlice::Mesh));
    CHECK(Has(entries, 3, ResidencySlice::Texture));

    // The device account's clock extends that to ANY context's open frame (R2): a mesh
    // context A drew in its not-yet-finished frame is protected when context B admits.
    int ownerA = 0;
    int ownerB = 0;
    ResidencyAccount account(LockedCaps());
    (void)account.Charge(&ownerA, Mesh(1, 512, 0));   // A draws mesh 1; A's frame is open
    account.EndFrame(&ownerB);                         // the clock moves on (B had no open frame)
    const auto first = account.Charge(&ownerB, Tex(2, 600, 0));
    CHECK(first.empty());                              // mesh 1 is A's, drawn in A's open frame
    CHECK(account.Holds(&ownerA, Guid{ 1, 0 }, ResidencySlice::Mesh, Space::Srgb));

    account.EndFrame(&ownerA);                         // A's frame finishes
    const auto second = account.Charge(&ownerB, Tex(3, 100, 0));
    REQUIRE(second.size() == 1);                       // now mesh 1 is cold, B's admit may take it
    CHECK(second[0].id == Guid{ 1, 0 });
    const auto queuedForA = account.TakePending(&ownerA);
    REQUIRE(queuedForA.size() == 1);                   // A's cache carries it out on A's lane
    CHECK(queuedForA[0].slice == ResidencySlice::Mesh);
    CHECK(account.TakePending(&ownerB).empty());

    // One key held by two caches is ONE entry; its bytes are the sum of the copies.
    ResidencyAccount shared(LockedCaps());
    (void)shared.Charge(&ownerA, Tex(5, 100, 0));
    (void)shared.Charge(&ownerB, Tex(5, 100, 0));
    CHECK(shared.EntryCount() == 1u);
    CHECK(shared.EntryBytes(Guid{ 5, 0 }, ResidencySlice::Texture, Space::Srgb) == MiB(200));
    shared.Uncharge(&ownerA, Guid{ 5, 0 }, ResidencySlice::Texture, Space::Srgb);
    CHECK(shared.EntryBytes(Guid{ 5, 0 }, ResidencySlice::Texture, Space::Srgb) == MiB(100));
    CHECK(shared.Holds(&ownerB, Guid{ 5, 0 }, ResidencySlice::Texture, Space::Srgb));
    shared.UnchargeOwner(&ownerB);
    CHECK(shared.EntryCount() == 0u);
}

TEST_CASE("an evicted object waits on the sampling context", "[residency][account][nri][bindless]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    const Guid id{ 70, 1 };
    Arcane::PixelData pixels;
    pixels.width = 2;
    pixels.height = 2;
    pixels.rgba.assign(16, 0xAB);
    auto supply = [&](const Guid&) -> const Arcane::PixelData* { return &pixels; };

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
        c->cache->SetPixelSupply(supply);
    }
    Arcane::BindlessTable* table = device->Bindless();
    REQUIRE(table != nullptr);

    struct Done { std::uint64_t fence = 0; } done;   // context A's own completed fence
    table->SetCompletionQuery(
        [](const void*, std::uint64_t f, void* user) { return static_cast<Done*>(user)->fence >= f; }, &done);

    const nri::Texture* aTexture = a.cache->Resolve(id);
    REQUIRE(aTexture != nullptr);
    REQUIRE(b.cache->Resolve(id) != nullptr);
    // One key, two copies: ONE account entry, the sum of both (2x2 RGBA8 = 16 bytes each).
    CHECK(device->Residency().EntryCount() == 1u);
    CHECK(device->Residency().EntryBytes(id, ResidencySlice::Texture, Space::Srgb) == 32u);

    // Context A sampled the texture through a bindless slot and its submit (fence 5 on
    // A's timeline) is still in flight.
    const std::uint32_t slot = table->AddMapped(id, Arcane::BindlessTable::ColorSpace::Srgb, MockSrv(1), &a, aTexture);
    REQUIRE(slot != Arcane::BindlessTable::kInvalidSlot);
    table->Lifetime().NoteSample(slot, &a, 5);

    // Context B's cache carries out an account eviction of the key.
    b.cache->Evict(id, Space::Srgb, b.graves, b.fence);

    CHECK(b.cache->ResidentCount() == 0u);
    CHECK(a.cache->ResidentCount() == 1u);   // A's own copy is A's to evict
    CHECK(device->Residency().EntryBytes(id, ResidencySlice::Texture, Space::Srgb) == 16u);   // B uncharged its copy
    CHECK(table->Lookup(id, Arcane::BindlessTable::ColorSpace::Srgb) == Arcane::BindlessTable::kInvalidSlot);
    CHECK(table->PendingCount() == 1u);      // the slot waits on A's fence
    CHECK(table->BackingCount() == 1u);      // and the texture waits on it with the descriptor
    CHECK(b.graves.Pending() == 0u);         // never buried at B's fence, never in B's lane

    done.fence = 4;                          // A's submit has not completed
    table->ProcessPending();
    CHECK(table->BackingCount() == 1u);
    CHECK(table->PendingCount() == 1u);

    done.fence = 5;                          // A's fence completes: the destroy runs
    table->ProcessPending();
    CHECK(table->BackingCount() == 0u);
    CHECK(table->PendingCount() == 0u);

    for (Ctx* c : { &a, &b })
    {
        c->cache->Release(c->graves, c->fence + 1);
        c->graves.Reap(c->fence + 1);
    }
    table->DestroyAllNow(true);
}

TEST_CASE("one guid in Srgb and Display is two entries", "[residency][account]")
{
    std::vector<ResidencyEntry> entries;
    entries.push_back(Tex(1, 300, 1, Space::Srgb));
    entries.push_back(Tex(1, 300, 2, Space::Display));   // the same Guid, the other space

    // The account holds two entries for one Guid.
    CHECK(Has(entries, 1, ResidencySlice::Texture, Space::Srgb));
    CHECK(Has(entries, 1, ResidencySlice::Texture, Space::Display));
    CHECK(entries.size() == 2u);

    // 600 MiB of textures plus a 512 MiB mesh: the texture slice is over. Evicting the
    // older Srgb entry leaves the Display entry.
    const std::vector<ResidencyEviction> evicted =
        Admit(entries, Mesh(3, 512, 0), ResidencySlice::Mesh, LockedCaps(), 10);
    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0].id == Guid{ 1, 0 });
    CHECK(evicted[0].slice == ResidencySlice::Texture);
    CHECK(evicted[0].space == Space::Srgb);
    CHECK_FALSE(Has(entries, 1, ResidencySlice::Texture, Space::Srgb));
    CHECK(Has(entries, 1, ResidencySlice::Texture, Space::Display));

    // The account keeps them apart too.
    int owner = 0;
    ResidencyAccount account(LockedCaps());
    (void)account.Charge(&owner, Tex(1, 10, 0, Space::Srgb));
    (void)account.Charge(&owner, Tex(1, 10, 0, Space::Display));
    CHECK(account.EntryCount() == 2u);
    account.Uncharge(&owner, Guid{ 1, 0 }, ResidencySlice::Texture, Space::Srgb);
    CHECK(account.EntryCount() == 1u);
    CHECK(account.Holds(&owner, Guid{ 1, 0 }, ResidencySlice::Texture, Space::Display));
}
