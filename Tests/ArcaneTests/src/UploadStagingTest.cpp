// Plan C Task 5 -- the `upload` node's device-less pins: staging arithmetic,
// ring fit, the three staging classes, and the deferral contract of both
// residency caches (a Resolve of a fresh payload creates the device object,
// queues the upload and answers not-ready; the upload node declares a SNAPSHOT
// of the queue; an entry is ready only after its upload was recorded, so its
// first consumer is a LATER frame's declaration).
//
// Everything here runs on the NONE backend (NriDevice::CreateNoneForTests).
// NriUploadRing::Init fails on NONE (MapBuffer returns null), so the ring half
// drives a standalone RingLayout -- the math BeginFrame resets.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>
#include <Arcane/Render/Nri/NriUploadStaging.hpp>
#include <Arcane/Render/Nri/ResidencyAccount.hpp>

#undef ERROR

#include <cstdint>
#include <memory>
#include <vector>

#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>

namespace
{
    using Arcane::Guid;
    using Arcane::MeshData;
    using Arcane::MeshResolveState;
    using Arcane::NriMeshBufferCache;
    using Arcane::NriTextureCache;
    using Arcane::RingLayout;
    using Arcane::Staging;

    const Guid kMeshA = *Guid::FromString("a1a1a1a1-0000-0000-0000-000000000001");
    const Guid kMeshB = *Guid::FromString("a1a1a1a1-0000-0000-0000-000000000002");
    const Guid kTexA  = *Guid::FromString("b2b2b2b2-0000-0000-0000-000000000001");
    const Guid kTexB  = *Guid::FromString("b2b2b2b2-0000-0000-0000-000000000002");

    Arcane::PixelData MakePixels(std::uint32_t w, std::uint32_t h)
    {
        Arcane::PixelData p;
        p.width  = w;
        p.height = h;
        p.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0x7F);
        return p;
    }

    struct MeshFixture
    {
        std::unique_ptr<Arcane::NriDevice>         device = Arcane::NriDevice::CreateNoneForTests();
        std::unique_ptr<NriMeshBufferCache>        cache;
        MeshData                                   cube = Arcane::BuildCube(1.0f);
        MeshFixture()
        {
            REQUIRE(device != nullptr);
            cache = NriMeshBufferCache::Create(*device);
            REQUIRE(cache != nullptr);
            cache->SetMeshSupply([this](const Guid&) {
                return NriMeshBufferCache::SupplyResult{ &cube, MeshResolveState::Ready };
            });
        }
        ~MeshFixture()
        {
            cache->Release(device->Graves(), 1);
            device->Graves().Reap(1);
        }
    };

    struct TextureFixture
    {
        std::unique_ptr<Arcane::NriDevice>   device = Arcane::NriDevice::CreateNoneForTests();
        std::unique_ptr<NriTextureCache>     cache;
        Arcane::PixelData                    pixels = MakePixels(4, 4);
        TextureFixture()
        {
            REQUIRE(device != nullptr);
            cache = NriTextureCache::Create(*device);
            REQUIRE(cache != nullptr);
            cache->SetPixelSupply([this](const Guid&) -> const Arcane::PixelData* { return &pixels; });
        }
        ~TextureFixture()
        {
            cache->Release(device->Graves(), 1);
            device->Graves().Reap(1);
        }
    };
}

TEST_CASE("texture footprint matches the helper at 256 and 512", "[nri][upload]")
{
    CHECK(Arcane::FootprintRgba8(1, 1, 256, 512) == 512u);
    CHECK(Arcane::FootprintRgba8(5, 5, 256, 512) == 1536u);
    CHECK(Arcane::FootprintBc7(5, 5, 256, 512) == 512u);
    CHECK(Arcane::FootprintBc7(2, 2, 256, 512) == 512u);
    CHECK(Arcane::FootprintBc7(1, 1, 256, 512) == 512u);
    CHECK(Arcane::FootprintBc7Chain(5, 5, 256, 512) == 1536u);
    CHECK(Arcane::FootprintRgba8Chain(5, 5, 256, 512) == 2560u);
}

TEST_CASE("ring fit agrees with Allocate on a non-aligned capacity", "[nri][upload]")
{
    RingLayout ring;
    ring.Init((4ull << 20) + 511ull);   // Init sets the cursor to 0
    // A legal prefix: AlignUp(0, 1) + size <= capacity, leaving the cursor at 4MiB - 16.
    const auto prefix = ring.Allocate((4ull << 20) - 16ull, 1ull);
    REQUIRE(prefix.ok);
    CHECK(ring.OverflowCount() == 0u);
    const std::uint64_t before = ring.OverflowCount();
    CHECK_FALSE(ring.Fits(512, 512));
    CHECK(ring.OverflowCount() == before);          // Fits does not bump
    CHECK_FALSE(ring.Allocate(512, 512).ok);
    CHECK(ring.OverflowCount() == before + 1);      // Allocate bumps by exactly 1
}

TEST_CASE("capacity plus 16 and capacity plus 512 are dedicated", "[nri][upload]")
{
    const std::uint64_t capacity = 4ull << 20;
    CHECK(Arcane::Classify(capacity, capacity + 16, 16) == Staging::Dedicated);
    CHECK(Arcane::Classify(capacity, capacity + 512, 512) == Staging::Dedicated);
    CHECK(Arcane::Classify(capacity, 1024, 16) == Staging::Ring);          // fits an empty slot
    // remaining short, payload fits an empty slot:
    CHECK(Arcane::ClassifyAtCursor(capacity, capacity - 16, 4096, 16) == Staging::Retry);

    // The pinned oversized payloads, built from the real byte functions.
    CHECK(Arcane::MeshStagingBytes(capacity, 16) == capacity + 16);
    CHECK(Arcane::Classify(capacity, Arcane::MeshStagingBytes(capacity, 16), 16) == Staging::Dedicated);
}

TEST_CASE("mesh cache: Resolve of a fresh mesh queues the upload and answers not-ready", "[nri][upload]")
{
    MeshFixture f;
    CHECK_FALSE(f.cache->HasPendingUploads());

    CHECK(f.cache->Resolve(kMeshA, 1) == nullptr);   // created + queued, NOT bindable
    CHECK(f.cache->Resolve(kMeshA, 1) == nullptr);   // still not ready on the next ask
    CHECK(f.cache->ResidentCount() == 0u);           // ready entries only
    CHECK(f.cache->QueuedUploadCount() == 1u);       // queued once, not once per ask
    CHECK(f.cache->HasPendingUploads());
}

TEST_CASE("mesh cache: the upload node's snapshot excludes entries queued during declaration", "[nri][upload]")
{
    MeshFixture f;
    CHECK(f.cache->Resolve(kMeshA, 1) == nullptr);

    // The upload node is declared FIRST: its setup takes the snapshot...
    const auto snapshot = f.cache->BeginUploadFrame();
    REQUIRE(snapshot.size() == 1u);
    CHECK(snapshot[0].id == kMeshA);
    CHECK(snapshot[0].vertexBuffer != nullptr);
    CHECK(snapshot[0].indexBuffer != nullptr);

    // ...and a Resolve that happens LATER in the same declaration queues for the NEXT frame.
    CHECK(f.cache->Resolve(kMeshB, 1) == nullptr);
    CHECK(f.cache->QueuedUploadCount() == 1u);

    // The frame's exec recorded the snapshot; it submits.
    f.cache->CommitUploads(true, f.device->Graves(), 1);
    CHECK(f.cache->ResidentCount() == 1u);
    CHECK(f.cache->Resolve(kMeshB, 2) == nullptr);   // B waits one more frame
    const NriMeshBufferCache::Resident* a = f.cache->Resolve(kMeshA, 2);
    REQUIRE(a != nullptr);                           // A is drawn by the NEXT frame's declaration
    CHECK(a->ready);

    const auto next = f.cache->BeginUploadFrame();
    REQUIRE(next.size() == 1u);
    CHECK(next[0].id == kMeshB);                     // and A is not uploaded twice
}

TEST_CASE("mesh cache: a failed Execute puts the snapshot back in the queue", "[nri][upload]")
{
    MeshFixture f;
    CHECK(f.cache->Resolve(kMeshA, 1) == nullptr);
    REQUIRE(f.cache->BeginUploadFrame().size() == 1u);
    CHECK(f.cache->QueuedUploadCount() == 0u);
    f.cache->CommitUploads(false, f.device->Graves(), 1);   // Execute failed before submit
    CHECK(f.cache->QueuedUploadCount() == 1u);
    CHECK(f.cache->ResidentCount() == 0u);
    CHECK(f.cache->HasPendingUploads());
    CHECK(f.cache->BeginUploadFrame().size() == 1u);
}

TEST_CASE("mesh cache: evicting a queued entry cancels its upload", "[nri][upload]")
{
    MeshFixture f;
    CHECK(f.cache->Resolve(kMeshA, 1) == nullptr);
    CHECK(f.cache->Resolve(kMeshB, 1) == nullptr);
    REQUIRE(f.cache->QueuedUploadCount() == 2u);

    f.cache->Invalidate(kMeshA, f.device->Graves(), 1);   // the one drop path an eviction takes
    CHECK(f.cache->QueuedUploadCount() == 1u);
    const auto snapshot = f.cache->BeginUploadFrame();
    REQUIRE(snapshot.size() == 1u);
    CHECK(snapshot[0].id == kMeshB);                      // never upload into an evicted object
}

TEST_CASE("mesh cache: one-shot query stays true until a frame after the entry became ready", "[nri][upload]")
{
    MeshFixture f;
    CHECK_FALSE(f.cache->HasPendingUploads());
    CHECK(f.cache->Resolve(kMeshA, 1) == nullptr);
    CHECK(f.cache->HasPendingUploads());                  // frame N queued

    REQUIRE(f.cache->BeginUploadFrame().size() == 1u);    // frame N+1 uploads...
    f.cache->CommitUploads(true, f.device->Graves(), 1);
    CHECK(f.cache->HasPendingUploads());                  // ...but its picture was drawn before the entry was ready

    f.cache->CommitUploads(true, f.device->Graves(), 2);  // frame N+2 draws, nothing new
    CHECK_FALSE(f.cache->HasPendingUploads());            // settled
}

TEST_CASE("texture cache: Resolve of a fresh image queues the upload and answers not-ready", "[nri][upload]")
{
    TextureFixture f;
    CHECK(f.cache->Resolve(kTexA) == nullptr);
    CHECK(f.cache->View(kTexA) == nullptr);               // no view until the upload is recorded
    CHECK(f.cache->ResidentCount() == 0u);
    CHECK(f.cache->QueuedUploadCount() == 1u);
    CHECK(f.cache->HasPendingUploads());

    const auto snapshot = f.cache->BeginUploadFrame();
    REQUIRE(snapshot.size() == 1u);
    CHECK(snapshot[0].id == kTexA);
    CHECK(snapshot[0].texture != nullptr);

    CHECK(f.cache->Resolve(kTexB) == nullptr);            // queued during declaration: next frame
    f.cache->CommitUploads(true, f.device->Graves(), 1);
    CHECK(f.cache->ResidentCount() == 1u);
    CHECK(f.cache->Resolve(kTexA) != nullptr);
    CHECK(f.cache->View(kTexA) != nullptr);
    CHECK(f.cache->Resolve(kTexB) == nullptr);
    CHECK(f.cache->QueuedUploadCount() == 1u);
}

TEST_CASE("texture cache: evicting a queued entry cancels its upload; a failed frame requeues", "[nri][upload]")
{
    TextureFixture f;
    CHECK(f.cache->Resolve(kTexA) == nullptr);
    CHECK(f.cache->Resolve(kTexB) == nullptr);
    REQUIRE(f.cache->QueuedUploadCount() == 2u);

    f.cache->Evict(kTexA, NriTextureCache::ColorSpace::Srgb, f.device->Graves(), 1);
    CHECK(f.cache->QueuedUploadCount() == 1u);

    REQUIRE(f.cache->BeginUploadFrame().size() == 1u);
    f.cache->CommitUploads(false, f.device->Graves(), 1);
    CHECK(f.cache->QueuedUploadCount() == 1u);
    const auto again = f.cache->BeginUploadFrame();
    REQUIRE(again.size() == 1u);
    CHECK(again[0].id == kTexB);
}

TEST_CASE("texture cache: one-shot query settles one frame after the entry became ready", "[nri][upload]")
{
    TextureFixture f;
    CHECK(f.cache->Resolve(kTexA) == nullptr);
    CHECK(f.cache->HasPendingUploads());
    REQUIRE(f.cache->BeginUploadFrame().size() == 1u);
    f.cache->CommitUploads(true, f.device->Graves(), 1);
    CHECK(f.cache->HasPendingUploads());
    f.cache->CommitUploads(true, f.device->Graves(), 2);
    CHECK_FALSE(f.cache->HasPendingUploads());
}

// ===========================================================================
// FIX ROUND 1 -- the per-frame ring BUDGET, queue stamping.
// ===========================================================================

TEST_CASE("upload budget: FIFO admission against a per-frame cap", "[nri][upload]")
{
    Arcane::UploadBudget b = Arcane::UploadBudget::WithCap(100);
    CHECK(b.Admit(60));          // fits the empty budget
    CHECK_FALSE(b.Admit(50));    // 50 > the 40 left: it waits (and nothing behind it may overtake)
    CHECK(b.Admit(40));          // exactly what is left
    CHECK_FALSE(b.Admit(1));     // empty
    CHECK(b.Admit(101));         // over the cap: dedicated staging, costs no ring bytes
    CHECK_FALSE(b.Admit(101));   // and only one per frame: the next over-cap payload waits, FIFO
    CHECK_FALSE(b.Admit(5000));
    CHECK(b.remaining == 0u);    // neither touched the ring bytes
    Arcane::UploadBudget bigFirst = Arcane::UploadBudget::WithCap(100);
    CHECK(bigFirst.Admit(101));  // a first entry over the cap is always admitted, so progress holds
    CHECK(bigFirst.Admit(100));  // and it costs the small ones nothing
    CHECK(Arcane::UploadBudget::WithCap(100).Admit(100));   // a fresh budget always admits the first entry <= cap

    // The exec-time class with the cap: over the cap -> dedicated even when it would fit the slot.
    const std::uint64_t slot = 4ull << 20;
    const std::uint64_t cap  = slot / 2;
    CHECK(Arcane::ClassifyBudgeted(true, slot, cap, cap) == Staging::Ring);
    CHECK(Arcane::ClassifyBudgeted(true, slot, cap, cap + 1) == Staging::Dedicated);      // never the ring
    CHECK(Arcane::ClassifyBudgeted(false, slot, cap, cap) == Staging::Retry);             // the guard branch
    CHECK(Arcane::ClassifyBudgeted(false, slot, slot, slot + 16) == Staging::Dedicated);  // bigger than the slot
    // The pins of Classify itself are unchanged by the cap.
    CHECK(Arcane::Classify(slot, slot + 16, 16) == Staging::Dedicated);
    CHECK(Arcane::ClassifyAtCursor(slot, slot - 16, 4096, 16) == Staging::Retry);
}

TEST_CASE("the planner never moves a real ring's overflow count", "[nri][upload]")
{
    // The planner asks first and only Allocates what it classified as Ring; payloads routed to retry or
    // dedicated staging never call Allocate. Walked against a real RingLayout instance whose state the
    // classifier reads, so the count staying 0 is a property of the loop, not of an unused object.
    const std::uint64_t slot = 4ull << 20;
    RingLayout ring;
    ring.Init(slot);
    const std::uint64_t payloads[] = { 1ull << 20, 3ull << 20, 512, slot + 16, 2ull << 20, slot / 2 + 512 };
    std::size_t ringHits = 0, retries = 0, dedicated = 0;
    for (const std::uint64_t bytes : payloads)
    {
        const Staging cls = Arcane::ClassifyAtCursor(ring.Capacity(), ring.Cursor(), bytes, 16);
        if (cls == Staging::Ring)
        {
            REQUIRE(ring.Allocate(bytes, 16).ok);
            ++ringHits;
        }
        else if (cls == Staging::Retry)
            ++retries;
        else
            ++dedicated;
    }
    CHECK(ringHits >= 2u);
    CHECK(retries >= 1u);
    CHECK(dedicated >= 1u);
    CHECK(ring.OverflowCount() == 0u);
}

TEST_CASE("mesh cache: a burst drains FIFO under the budget with one object pair per payload", "[nri][upload]")
{
    MeshFixture f;
    const std::uint64_t cost = Arcane::MeshStagingBytes(f.cube.vertices.size() * sizeof(Arcane::MeshVertex),
                                                        f.cube.indices.size() * sizeof(std::uint32_t));
    constexpr int kPayloads = 40;
    std::vector<Guid> ids;
    for (int i = 0; i < kPayloads; ++i)
    {
        const Guid id{ 900u + static_cast<std::uint64_t>(i), 0 };
        ids.push_back(id);
        CHECK(f.cache->Resolve(id, 1) == nullptr);
    }
    CHECK(f.cache->ObjectCreateCount() == static_cast<std::uint64_t>(kPayloads));

    int frames = 0;
    std::size_t admitted = 0;
    while (f.cache->QueuedUploadCount() > 0u && frames < 64)
    {
        Arcane::UploadBudget budget = Arcane::UploadBudget::WithCap(4 * cost + 1);   // four payloads per frame
        const auto jobs = f.cache->BeginUploadFrame(budget);
        CHECK(jobs.size() == 4u);
        if (!jobs.empty())
            CHECK(jobs.front().id == ids[admitted]);                    // FIFO
        admitted += jobs.size();
        f.cache->CommitUploads(true, f.device->Graves(), 1);
        ++frames;
    }
    CHECK(admitted == static_cast<std::size_t>(kPayloads));
    CHECK(frames == kPayloads / 4);
    CHECK(f.cache->ResidentCount() == static_cast<std::size_t>(kPayloads));
    // No churn: nothing was destroyed or recreated while it waited.
    CHECK(f.cache->ObjectCreateCount() == static_cast<std::uint64_t>(kPayloads));
}

TEST_CASE("mesh cache: payloads over the cap are admitted one per frame, FIFO, with no object churn", "[nri][upload]")
{
    MeshFixture f;
    const std::uint64_t cost = Arcane::MeshStagingBytes(f.cube.vertices.size() * sizeof(Arcane::MeshVertex),
                                                        f.cube.indices.size() * sizeof(std::uint32_t));
    CHECK(f.cache->Resolve(kMeshA, 1) == nullptr);
    CHECK(f.cache->Resolve(kMeshB, 1) == nullptr);
    const auto frame = [&]()
    {
        Arcane::UploadBudget budget = Arcane::UploadBudget::WithCap(cost - 1);   // every cube is "over the cap": dedicated
        return f.cache->BeginUploadFrame(budget);
    };
    const auto first = frame();
    REQUIRE(first.size() == 1u);
    CHECK(first[0].id == kMeshA);
    CHECK(f.cache->QueuedUploadCount() == 1u);
    f.cache->CommitUploads(true, f.device->Graves(), 1);
    const auto second = frame();
    REQUIRE(second.size() == 1u);
    CHECK(second[0].id == kMeshB);
    f.cache->CommitUploads(true, f.device->Graves(), 1);
    CHECK(f.cache->ObjectCreateCount() == 2u);
}

TEST_CASE("texture cache: payloads over the cap are admitted one per frame, FIFO, with no object churn", "[nri][upload]")
{
    TextureFixture f;
    CHECK(f.cache->Resolve(kTexA) == nullptr);
    CHECK(f.cache->Resolve(kTexB) == nullptr);
    Arcane::UploadBudget budget = Arcane::UploadBudget::WithCap(1);
    const auto first = f.cache->BeginUploadFrame(budget);
    REQUIRE(first.size() == 1u);
    CHECK(first[0].id == kTexA);
    CHECK(f.cache->QueuedUploadCount() == 1u);
    f.cache->CommitUploads(true, f.device->Graves(), 1);
    Arcane::UploadBudget next = Arcane::UploadBudget::WithCap(1);
    const auto second = f.cache->BeginUploadFrame(next);
    REQUIRE(second.size() == 1u);
    CHECK(second[0].id == kTexB);
    f.cache->CommitUploads(true, f.device->Graves(), 1);
    CHECK(f.cache->ObjectCreateCount() == 2u);
}

// NONE's MapBuffer always refuses, which is exactly a dedicated staging failure, and a never-Init'd ring classifies
// over-cap payloads as dedicated without touching the command buffer.
TEST_CASE("texture cache: a dedicated staging failure requeues for a bounded number of tries, then is refused", "[nri][upload]")
{
    TextureFixture f;
    Arcane::NriUploadRing ring;
    char notACommandBuffer = 0;   // the failure path records nothing
    nri::CommandBuffer& noCommands = reinterpret_cast<nri::CommandBuffer&>(notACommandBuffer);
    CHECK(f.cache->Resolve(kTexA) == nullptr);
    for (std::uint32_t attempt = 1; attempt <= Arcane::kMaxStagingTries; ++attempt)
    {
        INFO("attempt " << attempt);
        REQUIRE(f.cache->UploadQueued(kTexA));
        Arcane::UploadBudget budget = Arcane::UploadBudget::WithCap(1);
        const auto jobs = f.cache->BeginUploadFrame(budget);
        REQUIRE(jobs.size() == 1u);
        f.cache->RecordUpload(jobs[0], noCommands, ring, budget.cap);
        f.cache->CommitUploads(true, f.device->Graves(), 1);
        CHECK(f.cache->Resolve(kTexA) == nullptr);
        CHECK(f.cache->ResidentCount() == 0u);
        CHECK(f.cache->QueuedUploadCount() == (attempt < Arcane::kMaxStagingTries ? 1u : 0u));   // a retry, then memoized
    }
    CHECK_FALSE(f.cache->UploadQueued(kTexA));   // refused: Resolve above answered from the memo, nothing is queued
    CHECK(f.cache->ObjectCreateCount() == Arcane::kMaxStagingTries);   // the original and one per retry
}

TEST_CASE("mesh cache: a dedicated staging failure requeues for a bounded number of tries, then is refused", "[nri][upload]")
{
    MeshFixture f;
    Arcane::NriUploadRing ring;
    char notACommandBuffer = 0;   // the failure path records nothing
    nri::CommandBuffer& noCommands = reinterpret_cast<nri::CommandBuffer&>(notACommandBuffer);
    CHECK(f.cache->Resolve(kMeshA, 1) == nullptr);
    for (std::uint32_t attempt = 1; attempt <= Arcane::kMaxStagingTries; ++attempt)
    {
        INFO("attempt " << attempt);
        REQUIRE(f.cache->QueuedUploadCount() == 1u);
        CHECK(f.cache->RefusedCount() == 0u);
        Arcane::UploadBudget budget = Arcane::UploadBudget::WithCap(1);
        const auto jobs = f.cache->BeginUploadFrame(budget);
        REQUIRE(jobs.size() == 1u);
        f.cache->RecordUpload(jobs[0], noCommands, ring, budget.cap);
        f.cache->CommitUploads(true, f.device->Graves(), 1);
    }
    CHECK(f.cache->QueuedUploadCount() == 0u);
    CHECK(f.cache->RefusedCount() == 1u);
    CHECK(f.cache->ResidentCount() == 0u);
}

TEST_CASE("the one-shot renderers' bound is on upload progress, not on frames", "[nri][upload]")
{
    // A paced queue of ten: never refused, however many frames it takes.
    Arcane::UploadSettleBound paced;
    paced.maxStalled = 3;
    paced.lowestQueued = 10;
    for (std::size_t queued = 10; queued-- > 0;)
        CHECK(paced.AllowAnotherFrame(queued));

    // No progress at all: exactly maxStalled more frames.
    Arcane::UploadSettleBound stuck;
    stuck.maxStalled = 3;
    stuck.lowestQueued = 2;
    CHECK(stuck.AllowAnotherFrame(2));
    CHECK(stuck.AllowAnotherFrame(2));
    CHECK(stuck.AllowAnotherFrame(2));
    CHECK_FALSE(stuck.AllowAnotherFrame(2));

    // The settle frames after the queue empties count as no progress, and fit the default.
    Arcane::UploadSettleBound settling;
    settling.maxStalled = 3;
    settling.lowestQueued = 0;
    CHECK(settling.AllowAnotherFrame(0));

    // A queue that climbs back does not earn new frames: only a new LOW does, so it terminates.
    Arcane::UploadSettleBound bouncing;
    bouncing.maxStalled = 1;
    bouncing.lowestQueued = 3;
    CHECK(bouncing.AllowAnotherFrame(2));         // a new low
    CHECK(bouncing.AllowAnotherFrame(5));         // the one stall allowed
    CHECK_FALSE(bouncing.AllowAnotherFrame(3));   // above the low of 2: still stalled
    CHECK(bouncing.AllowAnotherFrame(1));         // a new low again
}

TEST_CASE("texture cache: ViewSerial names the view, and a dropped key's replacement gets another", "[nri][upload]")
{
    TextureFixture f;
    f.cache->SetUploadMode(Arcane::NriUploadMode::Immediate);
    CHECK(f.cache->ViewSerial(kTexA) == 0u);   // nothing yet
    REQUIRE(f.cache->Resolve(kTexA) != nullptr);
    const std::uint64_t first = f.cache->ViewSerial(kTexA);
    CHECK(first != 0u);
    CHECK(f.cache->ViewSerial(kTexA) == first);   // stable while the view lives

    // NONE hands out one dummy handle, so the POINTER here is equal across the replacement: exactly the
    // recycled address a pointer compare could not see.
    nri::Descriptor* const firstView = f.cache->View(kTexA);
    f.cache->Invalidate(kTexA, f.device->Graves(), 1);
    CHECK(f.cache->ViewSerial(kTexA) == 0u);
    REQUIRE(f.cache->Resolve(kTexA) != nullptr);
    CHECK(f.cache->View(kTexA) == firstView);
    CHECK(f.cache->ViewSerial(kTexA) != 0u);
    CHECK(f.cache->ViewSerial(kTexA) != first);
}

TEST_CASE("texture cache: a queued key has no view serial until its upload commits", "[nri][upload]")
{
    TextureFixture f;
    CHECK(f.cache->Resolve(kTexA) == nullptr);
    CHECK(f.cache->ViewSerial(kTexA) == 0u);
    (void)f.cache->BeginUploadFrame();
    CHECK(f.cache->ViewSerial(kTexA) == 0u);
    f.cache->CommitUploads(true, f.device->Graves(), 1);
    CHECK(f.cache->ViewSerial(kTexA) != 0u);
}

TEST_CASE("texture cache: the drop hook hears every drop, ahead of the burial, on the drop's own lane and fence", "[nri][upload]")
{
    TextureFixture f;
    f.cache->SetUploadMode(Arcane::NriUploadMode::Immediate);
    struct Heard
    {
        nri::Texture*  texture = nullptr;
        Arcane::Graveyard* graves = nullptr;
        std::uint64_t  fence = 0;
        int            count = 0;
    } heard;
    bool buriedWhenHeard = false;
    bool textureDestroyed = false;
    f.cache->SetDropHook([&](nri::Texture* texture, Arcane::Graveyard& graves, std::uint64_t fence)
    {
        heard = Heard{ texture, &graves, fence, heard.count + 1 };
        // The hook's own burial, as ImGuiNri makes one: it must run ahead of the texture's.
        graves.Bury(fence, [&, wasDestroyed = &textureDestroyed] { buriedWhenHeard = !*wasDestroyed; });
    });
    nri::Texture* const texture = f.cache->Resolve(kTexA);
    REQUIRE(texture != nullptr);
    Arcane::Graveyard lane;
    f.cache->Invalidate(kTexA, lane, 7);
    CHECK(heard.count == 1);
    CHECK(heard.texture == texture);
    CHECK(heard.graves == &lane);
    CHECK(heard.fence == 7u);
    lane.Bury(7, [&] { textureDestroyed = true; });   // after the cache's own view + texture burials
    lane.Reap(7);
    CHECK(buriedWhenHeard);

    // An eviction takes the same path; a key the cache does not hold hears nothing.
    REQUIRE(f.cache->Resolve(kTexA) != nullptr);
    f.cache->Evict(kTexA, NriTextureCache::ColorSpace::Srgb, lane, 8);
    CHECK(heard.count == 2);
    f.cache->Invalidate(kTexB, lane, 8);
    CHECK(heard.count == 2);
    lane.Reap(8);
}

TEST_CASE("texture cache: a later small entry never overtakes a waiting one", "[nri][upload]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    Arcane::PixelData small = MakePixels(4, 4), big = MakePixels(16, 16);
    const Guid idA = kTexA, idB = kTexB, idC = *Guid::FromString("b2b2b2b2-0000-0000-0000-000000000003");
    cache->SetPixelSupply([&](const Guid& id) -> const Arcane::PixelData* { return id == idB ? &big : &small; });

    const nri::DeviceDesc& dd = device->Core().GetDeviceDesc(device->Device());
    const auto cost = [&](std::uint32_t side) {
        return Arcane::UploadMipFootprint(side * 4u, side * side * 4u, 1u, dd.memoryAlignment.uploadBufferTextureRow,
                                          dd.memoryAlignment.uploadBufferTextureSlice).bytes;
    };
    REQUIRE(cost(16) >= 2u * cost(4));

    CHECK(cache->Resolve(idA) == nullptr);
    CHECK(cache->Resolve(idB) == nullptr);
    CHECK(cache->Resolve(idC) == nullptr);

    // cap == B's cost: A fits, then B (<= cap) does not fit what is left, so it WAITS and C (which would
    // fit) must not overtake it.
    Arcane::UploadBudget b1 = Arcane::UploadBudget::WithCap(cost(16));
    auto jobs = cache->BeginUploadFrame(b1);
    REQUIRE(jobs.size() == 1u);
    CHECK(jobs[0].id == idA);
    cache->CommitUploads(true, device->Graves(), 1);

    Arcane::UploadBudget b2 = Arcane::UploadBudget::WithCap(cost(16));   // a fresh budget always admits the first entry
    jobs = cache->BeginUploadFrame(b2);
    REQUIRE(jobs.size() == 1u);
    CHECK(jobs[0].id == idB);
    cache->CommitUploads(true, device->Graves(), 2);

    Arcane::UploadBudget b3 = Arcane::UploadBudget::WithCap(cost(16));
    jobs = cache->BeginUploadFrame(b3);
    REQUIRE(jobs.size() == 1u);
    CHECK(jobs[0].id == idC);
    cache->CommitUploads(true, device->Graves(), 3);
    CHECK(cache->ResidentCount() == 3u);
    CHECK(cache->ObjectCreateCount() == 3u);   // no churn while they waited

    cache->Release(device->Graves(), 4);
    device->Graves().Reap(4);
}

TEST_CASE("texture cache: a queued entry that is asked for is stamped, so it survives another key's Charge",
          "[nri][upload]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    Arcane::ResidencyCaps caps;
    caps.meshCap = 0;
    caps.textureCap = 2ull << 10;   // two 1 KiB images
    caps.reservationPercent = 0;
    Arcane::ResidencyAccount& account = device->ResidencyForTests(caps);
    auto cache = NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    Arcane::PixelData px = MakePixels(16, 16);   // 1 KiB
    cache->SetPixelSupply([&](const Guid&) -> const Arcane::PixelData* { return &px; });
    const Guid idC = *Guid::FromString("b2b2b2b2-0000-0000-0000-000000000003");

    // Frame 1: A and B are queued (and charged). The frame ends.
    CHECK(cache->Resolve(kTexA) == nullptr);
    CHECK(cache->Resolve(kTexB) == nullptr);
    cache->CloseFrame();

    // Frame 2: the scene still draws A and B (they are asked for again, still not ready), and a third key is
    // admitted over the cap. A and B were touched this frame, so neither is its victim.
    CHECK(cache->Resolve(kTexA) == nullptr);
    CHECK(cache->Resolve(kTexB) == nullptr);
    CHECK(cache->Resolve(idC) == nullptr);
    CHECK(account.TakePending(cache.get()).empty());
    CHECK(cache->UploadQueued(kTexA));
    CHECK(cache->UploadQueued(kTexB));

    // ...and the whole way to READY: the snapshot takes all three, the frame submits, A and B are bindable.
    const auto jobs = cache->BeginUploadFrame();
    CHECK(jobs.size() == 3u);
    cache->CommitUploads(true, device->Graves(), 1);
    CHECK(cache->Resolve(kTexA) != nullptr);
    CHECK(cache->Resolve(kTexB) != nullptr);
    CHECK(cache->View(kTexA) != nullptr);
    CHECK(cache->View(kTexB) != nullptr);

    cache->Release(device->Graves(), 2);
    device->Graves().Reap(2);
}

TEST_CASE("mesh cache: a queued entry that is asked for is stamped, so two queued meshes over budget both reach ready",
          "[nri][upload]")
{
    // The cost of one cube on the account, measured on a probe device.
    std::uint64_t cubeBytes = 0;
    {
        auto probe = Arcane::NriDevice::CreateNoneForTests();
        REQUIRE(probe != nullptr);
        auto cache = NriMeshBufferCache::Create(*probe);
        REQUIRE(cache != nullptr);
        cache->SetUploadMode(Arcane::NriUploadMode::Immediate);
        MeshData cube = Arcane::BuildCube(1.0f);
        cache->SetMeshSupply([&](const Guid&) { return NriMeshBufferCache::SupplyResult{ &cube, MeshResolveState::Ready }; });
        const NriMeshBufferCache::Resident* r = cache->Resolve(kMeshA, 1);
        REQUIRE(r != nullptr);
        cubeBytes = r->bytes;
        cache->Release(probe->Graves(), 1);
        probe->Graves().Reap(1);
    }
    REQUIRE(cubeBytes > 0u);

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    Arcane::ResidencyCaps caps;
    caps.meshCap = cubeBytes * 2;   // room for two cubes
    caps.textureCap = 0;
    caps.reservationPercent = 0;
    Arcane::ResidencyAccount& account = device->ResidencyForTests(caps);
    auto cache = NriMeshBufferCache::Create(*device);
    REQUIRE(cache != nullptr);
    MeshData cube = Arcane::BuildCube(1.0f);
    cache->SetMeshSupply([&](const Guid&) { return NriMeshBufferCache::SupplyResult{ &cube, MeshResolveState::Ready }; });
    const Guid kMeshC = *Guid::FromString("a1a1a1a1-0000-0000-0000-000000000003");

    // Frame 1: A and B are queued (and charged). The frame ends.
    CHECK(cache->Resolve(kMeshA, 1) == nullptr);
    CHECK(cache->Resolve(kMeshB, 1) == nullptr);
    cache->CloseFrame();

    // Frame 2: the scene still draws A and B (asked for again, still not ready) and a third mesh is charged over
    // the cap. A and B were touched this frame, so neither is its victim.
    CHECK(cache->Resolve(kMeshA, 2) == nullptr);
    CHECK(cache->Resolve(kMeshB, 2) == nullptr);
    CHECK(cache->Resolve(kMeshC, 2) == nullptr);
    CHECK(account.TakePending(cache.get()).empty());

    // ...and the whole way to ready.
    CHECK(cache->BeginUploadFrame().size() == 3u);
    cache->CommitUploads(true, device->Graves(), 1);
    CHECK(cache->Resolve(kMeshA, 3) != nullptr);
    CHECK(cache->Resolve(kMeshB, 3) != nullptr);

    cache->Release(device->Graves(), 2);
    device->Graves().Reap(2);
}
