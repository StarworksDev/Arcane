// NriMeshBufferCache -- F2c Plan 2 Task 2. Split the way NriTextureCacheArtifactTest
// splits: device-less policy on NONE ([render] / [nri], part of ~[gpu]), then
// [gpu][meshcache] upload cases on a real adapter.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/OffscreenVehicle.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>

#undef ERROR

#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include "Helpers/GpuCapability.hpp"

namespace
{
    using Arcane::Guid;
    using Arcane::MeshData;
    using Arcane::MeshResolveState;
    using Arcane::NriMeshBufferCache;
    using SupplyResult = NriMeshBufferCache::SupplyResult;

    Guid GuidA() { return Guid{1, 0}; }
    Guid GuidB() { return Guid{2, 0}; }
    Guid GuidC() { return Guid{3, 0}; }

    std::unique_ptr<NriMeshBufferCache> MakeDevicelessCache(Arcane::NriDevice& device)
    {
        auto cache = NriMeshBufferCache::Create(device);
        REQUIRE(cache != nullptr);
        return cache;
    }
}

TEST_CASE("mesh buffer cache: a pending mesh resolves to nothing, QUIETLY and retryably",
          "[render]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = MakeDevicelessCache(*device);

    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        return SupplyResult{ nullptr, MeshResolveState::PendingCook };
    });
    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    CHECK(cache->Resolve(GuidA(), 2) == nullptr);
    CHECK(asks == 2);                        // retried, not memoized
    CHECK(cache->ResidentCount() == 0u);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

TEST_CASE("mesh buffer cache: a FAILED supply is memoized exactly once", "[render]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = MakeDevicelessCache(*device);

    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        return SupplyResult{ nullptr, MeshResolveState::Failed };
    });
    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    CHECK(cache->Resolve(GuidA(), 2) == nullptr);
    CHECK(asks == 1);                        // attempted once, not once per frame

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

TEST_CASE("mesh buffer cache: Invalidate un-latches a memoized failure", "[render]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = MakeDevicelessCache(*device);

    int asks = 0;
    MeshData cube = Arcane::BuildCube(1.0f);
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        if (asks == 1)
            return SupplyResult{ nullptr, MeshResolveState::Failed };
        return SupplyResult{ &cube, MeshResolveState::Ready };
    });
    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    CHECK(asks == 1);

    cache->Invalidate(GuidA(), device->Graves(), 1);
    const NriMeshBufferCache::Resident* r = cache->Resolve(GuidA(), 2);
    CHECK(asks == 2);
    REQUIRE(r != nullptr);
    CHECK(r->indexCount == 36u);

    cache->Release(device->Graves(), 2);
    device->Graves().Reap(2);
}

TEST_CASE("mesh buffer cache: no supply installed is a quiet, memoized miss", "[render]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = MakeDevicelessCache(*device);

    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    CHECK(cache->Resolve(Guid{}, 1) == nullptr);   // a nil guid is the ordinary
                                                    // untextured case, silent
    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

namespace
{
    struct GpuVehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner> native;
        std::unique_ptr<Arcane::NriDevice>         nri;
    };

    GpuVehicle MakeGpuVehicle(Arcane::GraphicsBackend backend)
    {
        GpuVehicle v;
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
#if defined(ARC_BUILD_DEBUG)
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;
#endif
        v.native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(v.native != nullptr);
        v.nri = Arcane::NriDevice::Wrap(*v.native);
        REQUIRE(v.nri != nullptr);
        return v;
    }
}

TEST_CASE("pixel: a mesh uploads once and stays resident across frames",
          "[gpu][meshcache]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    auto v = MakeGpuVehicle(Arcane::Test::kNativeBackend);
    auto cache = NriMeshBufferCache::Create(*v.nri);
    REQUIRE(cache != nullptr);

    MeshData cube = Arcane::BuildCube(1.0f);
    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        return SupplyResult{ &cube, MeshResolveState::Ready };
    });

    const NriMeshBufferCache::Resident* first = cache->Resolve(GuidA(), 1);
    REQUIRE(first != nullptr);
    CHECK(first->indexCount == 36u);
    CHECK(cache->ResidentCount() == 1u);
    const std::uint64_t expectBytes = Arcane::MeshResidencyBytes(
        cube.vertices.size() * sizeof(Arcane::MeshVertex),
        cube.indices.size() * sizeof(std::uint32_t),
        cube.sections.size() * sizeof(Arcane::MeshSection));
    CHECK(cache->ResidentBytes() == expectBytes);

    for (std::uint64_t frame = 2; frame <= 5; ++frame)
    {
        const NriMeshBufferCache::Resident* again = cache->Resolve(GuidA(), frame);
        CHECK(again == first);
        CHECK(cache->ResidentCount() == 1u);
    }
    CHECK(asks == 1);

    cache->Release(v.nri->Graves(), 1);
    v.nri->Graves().Reap(1);
}

TEST_CASE("mesh buffer cache: eviction ERASES the entry -- the next Resolve re-asks the "
          "supply", "[render]")
{
    // Final-review I2, ruled: the budget counts CPU+GPU, so eviction must free BOTH
    // or ResidentBytes() reports a number the process is not honouring. The CPU copy
    // that makes the re-upload cheap lives in the SUPPLY (SceneRenderResolver's
    // in-memory MeshTable), which is why re-asking is a table lookup and not a disk
    // read -- pinned here device-less, on the NONE backend, because the rule is about
    // bookkeeping rather than about any real buffer.
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = MakeDevicelessCache(*device);

    MeshData cube = Arcane::BuildCube(1.0f);
    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        return SupplyResult{ &cube, MeshResolveState::Ready };
    });

    const NriMeshBufferCache::Resident* a = cache->Resolve(GuidA(), 1);
    REQUIRE(cache->Resolve(GuidB(), 2) != nullptr);
    REQUIRE(cache->Resolve(GuidC(), 3) != nullptr);
    REQUIRE(a != nullptr);
    CHECK(asks == 3);
    CHECK(cache->ResidentCount() == 3u);

    const std::uint64_t one = a->bytes;
    REQUIRE(one > 0u);
    CHECK(cache->ResidentBytes() == one * 3u);

    cache->EvictToBudget(4, device->Graves(), 1, /*budget=*/ one * 2 + 1);
    CHECK(cache->ResidentCount() == 2u);
    // EXACTLY the resident bytes: nothing hides behind a cold entry any more.
    CHECK(cache->ResidentBytes() == one * 2u);

    REQUIRE(cache->Resolve(GuidA(), 5) != nullptr);
    CHECK(asks == 4);                        // the supply IS asked again
    CHECK(cache->ResidentCount() == 3u);
    CHECK(cache->ResidentBytes() == one * 3u);

    cache->Release(device->Graves(), 2);
    CHECK(cache->ResidentCount() == 0u);
    CHECK(cache->ResidentBytes() == 0u);
    device->Graves().Reap(2);
}

TEST_CASE("mesh buffer cache: a zero-size mesh is refused ONCE and never retried",
          "[render]")
{
    // The device-less mirror of the [gpu][meshcache] zero-size case, which is the
    // one upload refusal reachable without a real adapter -- final-review I1's
    // never-retried half. A refusal that retried per frame is what leaked an
    // nri::Buffer per frame before the fix.
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = MakeDevicelessCache(*device);

    MeshData empty;
    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        return SupplyResult{ &empty, MeshResolveState::Ready };
    });
    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    for (std::uint64_t frame = 2; frame <= 6; ++frame)
        CHECK(cache->Resolve(GuidA(), frame) == nullptr);
    CHECK(asks == 1);                          // memoized, not once per frame
    CHECK(cache->ResidentCount() == 0u);
    CHECK(cache->ResidentBytes() == 0u);       // and nothing hidden in a CPU copy
    // Debt 12: the zero-size refusal is the SAME memo an Upload refusal sets, taken
    // before ever reaching CreateCommittedBuffer -- RefusedCount() counts it too.
    CHECK(cache->RefusedCount() == 1u);

    // Invalidate is the un-latch, exactly as for a Failed supply.
    MeshData cube = Arcane::BuildCube(1.0f);
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        return SupplyResult{ &cube, MeshResolveState::Ready };
    });
    cache->Invalidate(GuidA(), device->Graves(), 1);
    REQUIRE(cache->Resolve(GuidA(), 7) != nullptr);
    CHECK(asks == 2);
    CHECK(cache->RefusedCount() == 0u);        // the un-latch clears the refusal too

    cache->Release(device->Graves(), 2);
    device->Graves().Reap(2);
}

TEST_CASE("pixel: EvictToBudget drops the least-recently-drawn mesh and re-uploads it"
          " on the next ask", "[gpu][meshcache]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    auto v = MakeGpuVehicle(Arcane::Test::kNativeBackend);
    auto cache = NriMeshBufferCache::Create(*v.nri);
    REQUIRE(cache != nullptr);

    MeshData cube = Arcane::BuildCube(1.0f);
    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        return SupplyResult{ &cube, MeshResolveState::Ready };
    });

    const NriMeshBufferCache::Resident* a = cache->Resolve(GuidA(), 1);
    const NriMeshBufferCache::Resident* b = cache->Resolve(GuidB(), 2);
    const NriMeshBufferCache::Resident* c = cache->Resolve(GuidC(), 3);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(asks == 3);
    CHECK(cache->ResidentCount() == 3u);

    const std::uint64_t one = a->bytes;
    cache->EvictToBudget(4, v.nri->Graves(), 1, /*budget=*/ one * 2 + 1);
    CHECK(cache->ResidentCount() == 2u);
    CHECK(cache->ResidentBytes() == one * 2u);   // the CPU half went too (I2)

    const NriMeshBufferCache::Resident* a2 = cache->Resolve(GuidA(), 5);
    REQUIRE(a2 != nullptr);
    CHECK(a2->ready);
    // FLIPPED at final review (I2, ruled): eviction ERASES the entry, CPU copy
    // included, so this is a clean miss and the supply IS asked a fourth time. In
    // production that supply is SceneRenderResolver's in-memory MeshTable, so the
    // re-ask costs a table lookup rather than an artifact read -- which is what
    // makes "keep the CPU copy for cheap re-upload" (s7.2) true one layer up
    // instead of false down here.
    CHECK(asks == 4);
    CHECK(cache->ResidentCount() == 3u);
    CHECK(cache->ResidentBytes() == one * 3u);

    cache->Release(v.nri->Graves(), 2);
    v.nri->Graves().Reap(2);
}

TEST_CASE("pixel: Release buries everything and is idempotent", "[gpu][meshcache]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    auto v = MakeGpuVehicle(Arcane::Test::kNativeBackend);
    auto cache = NriMeshBufferCache::Create(*v.nri);
    REQUIRE(cache != nullptr);

    MeshData cube = Arcane::BuildCube(1.0f);
    cache->SetMeshSupply([&](const Guid&) {
        return SupplyResult{ &cube, MeshResolveState::Ready };
    });
    REQUIRE(cache->Resolve(GuidA(), 1) != nullptr);
    REQUIRE(cache->Resolve(GuidB(), 1) != nullptr);
    CHECK(cache->ResidentCount() == 2u);

    cache->Release(v.nri->Graves(), 1);
    CHECK(cache->ResidentCount() == 0u);
    cache->Release(v.nri->Graves(), 1);   // idempotent -- no crash, no double-bury
    v.nri->Graves().Reap(1);
}

TEST_CASE("pixel: a mesh with no indices or no vertices is refused, not uploaded",
          "[gpu][meshcache]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    auto v = MakeGpuVehicle(Arcane::Test::kNativeBackend);
    auto cache = NriMeshBufferCache::Create(*v.nri);
    REQUIRE(cache != nullptr);

    MeshData empty;
    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        return SupplyResult{ &empty, MeshResolveState::Ready };
    });
    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    CHECK(cache->Resolve(GuidA(), 2) == nullptr);
    CHECK(asks == 1);
    CHECK(cache->ResidentCount() == 0u);

    cache->Release(v.nri->Graves(), 1);
    v.nri->Graves().Reap(1);
}

TEST_CASE("pixel: DebugFailNextUpload forces Upload's abandon() arm for real, "
          "refusing without leaking", "[gpu][meshcache]")
{
    // Debt 13: the real I1 leak path (create-into-locals / publish-on-full-success)
    // was correct by inspection only -- the re-review traced all three abandon()
    // arms, never exercised one. There is no honest way to make a REAL device fail
    // only the SECOND CreateCommittedBuffer (an over-limit index buffer needs a
    // multi-GB CPU vector to reach it), so DebugFailNextUpload injects the failure
    // instead. Two SECTIONs so BOTH create-call sites inside Upload run their own
    // abandon(): Index (vb real, ib injected -- abandon() has something to destroy)
    // and Vertex (nothing was ever created yet -- abandon() destroys nothing).
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    auto v = MakeGpuVehicle(Arcane::Test::kNativeBackend);
    auto cache = NriMeshBufferCache::Create(*v.nri);
    REQUIRE(cache != nullptr);

    MeshData cube = Arcane::BuildCube(1.0f);
    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) {
        ++asks;
        return SupplyResult{ &cube, MeshResolveState::Ready };
    });

    NriMeshBufferCache::UploadStage stage = NriMeshBufferCache::UploadStage::Index;
    SECTION("the index buffer's create call is the one that fails")
    {
        stage = NriMeshBufferCache::UploadStage::Index;
    }
    SECTION("the vertex buffer's create call is the one that fails")
    {
        stage = NriMeshBufferCache::UploadStage::Vertex;
    }

    const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
    cache->DebugFailNextUpload(stage);
    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    CHECK(cache->ResidentCount() == 0u);
    CHECK(cache->RefusedCount() == 1u);
    // The abandon() arm ran through real NRI calls (a real CreateCommittedBuffer for
    // the Index section, a real DestroyBuffer either way) with no validation
    // complaint -- this is what the fixture CAN observe about the leak path: no NRI
    // error was raised destroying what Upload created before the injected failure.
    // It does NOT prove the driver reclaimed the memory; RenderErrorCount() is the
    // instrument this codebase has, and no live-object-count seam exists here.
    CHECK(Arcane::RenderErrorCount() == errorsBefore);
    CHECK(asks == 1);                          // the supply was asked exactly once

    // Memoized refusal: no per-frame retry.
    CHECK(cache->Resolve(GuidA(), 2) == nullptr);
    CHECK(asks == 1);
    CHECK(cache->RefusedCount() == 1u);

    // Invalidate un-latches it; the next Resolve uploads for real.
    cache->Invalidate(GuidA(), v.nri->Graves(), 1);
    REQUIRE(cache->Resolve(GuidA(), 3) != nullptr);
    CHECK(asks == 2);
    CHECK(cache->ResidentCount() == 1u);
    CHECK(cache->RefusedCount() == 0u);
    CHECK(Arcane::RenderErrorCount() == errorsBefore);

    cache->Release(v.nri->Graves(), 2);
    CHECK(cache->ResidentCount() == 0u);
    v.nri->Graves().Reap(2);
    CHECK(Arcane::RenderErrorCount() == errorsBefore);   // buries cleanly
}

TEST_CASE("pixel: the vehicle owns one mesh buffer cache, released on teardown",
          "[gpu][meshcache]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    Arcane::HostConfig cfg;
    cfg.backend  = Arcane::Test::kNativeBackend;
    cfg.headless = true;
    auto v = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(v != nullptr);
    REQUIRE(v->Graph().MeshBuffers() != nullptr);

    MeshData cube = Arcane::BuildCube(1.0f);
    v->Graph().SetMeshSupply([&](const Guid&) {
        return SupplyResult{ &cube, MeshResolveState::Ready };
    });
    REQUIRE(v->Graph().MeshBuffers()->Resolve(GuidA(), 1) != nullptr);
    CHECK(v->Graph().MeshBuffers()->ResidentCount() == 1u);

    const std::uint64_t errors = Arcane::RenderErrorCount();
    v.reset();
    CHECK(Arcane::RenderErrorCount() == errors);
}

TEST_CASE("pixel: ResizeOffscreen does NOT release resident mesh buffers",
          "[gpu][meshcache]")
{
    // Resident meshes are PERSISTENT and are not pool tenants -- the same reason
    // m_textures is "deliberately NOT released on Resize" (its own declaration
    // comment). Re-uploading every mesh on a viewport drag would be the ring cliff
    // reintroduced through a different door.
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    Arcane::HostConfig cfg;
    cfg.backend  = Arcane::Test::kNativeBackend;
    cfg.headless = true;
    auto v = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(v != nullptr);

    MeshData cube = Arcane::BuildCube(1.0f);
    v->Graph().SetMeshSupply([&](const Guid&) {
        return SupplyResult{ &cube, MeshResolveState::Ready };
    });
    const NriMeshBufferCache::Resident* first =
        v->Graph().MeshBuffers()->Resolve(GuidA(), 1);
    REQUIRE(first != nullptr);
    CHECK(v->Graph().MeshBuffers()->ResidentCount() == 1u);

    v->Graph().ResizeOffscreen(320, 200);
    const NriMeshBufferCache::Resident* after =
        v->Graph().MeshBuffers()->Resolve(GuidA(), 2);
    CHECK(after == first);
    CHECK(v->Graph().MeshBuffers()->ResidentCount() == 1u);
}
