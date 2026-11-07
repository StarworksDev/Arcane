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

#undef ERROR

#include <cstdint>
#include <memory>

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

    // Neither dedicated class touches the overflow count: the planner never calls Allocate.
    RingLayout ring;
    ring.Init(capacity);
    CHECK(Arcane::Classify(ring.Capacity(), capacity + 512, 512) == Staging::Dedicated);
    CHECK(ring.OverflowCount() == 0u);
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
