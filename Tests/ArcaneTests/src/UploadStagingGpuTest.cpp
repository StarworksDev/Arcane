// Plan C Task 5 -- the real `upload` node on D3D12 and on Vulkan. [gpu][upload]:
// run BY NAME, one GPU suite at a time on the desk. Every case asserts
// RenderErrorCount() unchanged; the D3D12 debug layer stays forced (test_main).
// The device-less half (arithmetic, queue, snapshot rule, budget) is UploadStagingTest.cpp.
//
// THE TIMELINE UNDER TEST (user ruling 2026-11-06): frame N queues, N+1
// uploads, N+2 draws. Each case renders a scene with other work in the frame (a
// rect) plus fresh textured quads and a fresh mesh, reads back EVERY frame, and
// compares the first frame that shows them byte-for-byte with the picture the
// OLD blocking UploadData path (NriUploadMode::Immediate -- a test-only mode)
// draws in its one frame. The device-wait-idle count is read around the RENDER
// calls only (ReadCapture idles by contract).
//
// Fix round 1 adds: the per-frame ring BUDGET (a 1024x1024 first use beside a real
// Batch2D stream must not starve it), the budget-deferred second texture, the
// injected retry, a many-payload burst (no object churn), a textured mesh first
// use, and the two-context case (B samples what A uploaded while A is in flight).
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Scene/SceneCamera.hpp>

#undef ERROR

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Helpers/GpuCapability.hpp"

namespace
{
    constexpr std::uint32_t kW = 160;
    constexpr std::uint32_t kH = 96;

    struct GpuVehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner> native;
        std::unique_ptr<Arcane::NriDevice>         nri;
        std::unique_ptr<Arcane::NriGraphContext>   ctx;
        std::unique_ptr<Arcane::NriGraphContext>   ctxB;   // the two-context case only
        ~GpuVehicle()
        {
            ctxB.reset();   // wrap before owner, as every other vehicle in the suite
            ctx.reset();
            nri.reset();
            native.reset();
        }
    };

    std::unique_ptr<GpuVehicle> MakeGpuVehicle(Arcane::GraphicsBackend backend, bool second = false)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
#if defined(ARC_BUILD_DEBUG)
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;
#endif
        auto v = std::make_unique<GpuVehicle>();
        v->native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(v->native != nullptr);
        v->nri = Arcane::NriDevice::Wrap(*v->native);
        REQUIRE(v->nri != nullptr);
        Arcane::HostConfig cfg;
        cfg.backend = backend;
        v->ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *v->nri, kW, kH, {});
        REQUIRE(v->ctx != nullptr);
        if (second)
        {
            v->ctxB = Arcane::NriGraphContext::CreateOffscreen(cfg, *v->nri, kW, kH, {});
            REQUIRE(v->ctxB != nullptr);
        }
        return v;
    }

    struct SceneSpec
    {
        std::uint32_t textureSide  = 8;   // each texture is textureSide x textureSide RGBA8
        int           textureCount = 1;
        std::uint32_t sphereRings  = 0;   // 0 = a cube, else a UV sphere of that many rings (x rings*2 segments)
        int           streamRects  = 0;   // a real Batch2D stream (rects across the bottom) sharing the ring
        bool          albedoMesh   = false;   // the mesh samples texture 0 through a bindless slot
        std::uint32_t forceRetries = 0;   // test injection: send this many ring-class textures down the retry branch
        bool          checkIdle    = true;
        bool          quads        = true;   // draw a textured sprite quad per texture
    };

    struct GpuScene
    {
        SceneSpec                      spec;
        std::vector<Arcane::Guid>      texIds;
        std::vector<Arcane::PixelData> pixels;
        Arcane::MeshData               mesh;
        Arcane::Guid                   meshId = *Arcane::Guid::FromString("c3c3c3c3-0000-0000-0000-000000000001");

        explicit GpuScene(const SceneSpec& s) : spec(s)
        {
            for (int i = 0; i < s.textureCount; ++i)
            {
                char text[64];
                std::snprintf(text, sizeof(text), "d4d4d4d4-0000-0000-0000-0000000000%02x", i + 1);
                texIds.push_back(*Arcane::Guid::FromString(text));
                Arcane::PixelData p;
                p.width = p.height = s.textureSide;
                p.rgba.resize(static_cast<std::size_t>(s.textureSide) * s.textureSide * 4);
                for (std::uint32_t y = 0; y < s.textureSide; ++y)
                    for (std::uint32_t x = 0; x < s.textureSide; ++x)
                    {
                        unsigned char* t = p.rgba.data() + (static_cast<std::size_t>(y) * s.textureSide + x) * 4;
                        t[0] = static_cast<unsigned char>((x * 255u) / s.textureSide);
                        t[1] = static_cast<unsigned char>((y * 255u) / s.textureSide);
                        t[2] = static_cast<unsigned char>(i == 0 ? 40 : 200);
                        t[3] = 255;
                    }
                pixels.push_back(std::move(p));
            }
            mesh = s.sphereRings == 0 ? Arcane::BuildCube(2.0f)
                                      : Arcane::BuildUvSphere(1.0f, s.sphereRings, s.sphereRings * 2u);
        }

        void Install(Arcane::NriGraphContext& ctx)
        {
            ctx.SetPixelSupply([this](const Arcane::Guid& id) -> const Arcane::PixelData* {
                for (std::size_t i = 0; i < texIds.size(); ++i)
                    if (texIds[i] == id)
                        return &pixels[i];
                return nullptr;
            });
            ctx.SetMeshSupply([this](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult {
                if (id == meshId)
                    return { &mesh, Arcane::MeshResolveState::Ready };
                return { nullptr, Arcane::MeshResolveState::Failed };
            });
        }

        // One frame of the scene: a rect (other work in the frame), an optional Batch2D stream,
        // one textured quad per texture, and the mesh. The batcher is re-filled every call.
        Arcane::NriGraphContext::FrameOutcome Render(Arcane::NriGraphContext& ctx, Arcane::Batcher2D& batch)
        {
            batch.Begin(kW, kH);
            batch.SetLayer(0, 0);
            batch.Rect(glm::vec2(4.0f, 4.0f), glm::vec2(30.0f, 12.0f), glm::vec4(0.1f, 0.8f, 0.2f, 1.0f));
            for (int k = 0; k < spec.streamRects; ++k)
                batch.Rect(glm::vec2(1.0f + float(k % 40) * 3.0f, 90.0f), glm::vec2(2.0f, 4.0f),
                           glm::vec4(0.8f, 0.2f, 0.2f, 1.0f));
            for (std::size_t i = 0; spec.quads && i < texIds.size(); ++i)
            {
                const float col = float(i % 8), row = float(i / 8);
                const bool grid = texIds.size() > 2;
                batch.QuadTextured(Arcane::Batcher2D::kMaterialSprite, texIds[i],
                                   grid ? glm::vec2(4.0f + 19.0f * col, 36.0f + 20.0f * row)
                                        : glm::vec2(8.0f + 40.0f * float(i), 60.0f),
                                   grid ? glm::vec2(16.0f) : glm::vec2(32.0f),
                                   glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
            }

            Arcane::MeshInstance instance;
            instance.mesh      = meshId;
            instance.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            if (spec.albedoMesh && !texIds.empty())
            {
                // What SceneRenderResolver does every frame: re-resolve the albedo slot. On a first use it
                // is invalid until the texture's upload has landed, and the mesh draws untextured meanwhile.
                const std::uint32_t slot = ctx.ResolveMeshAlbedoSlot(texIds[0]);
                instance.materialSlot = slot;
                if (slot != 0xFFFFFFFFu)
                    instance.baseColor = glm::vec4(1.0f);
            }
            const Arcane::MeshInstance instances[] = { instance };
            Arcane::MeshSceneDesc scene;
            scene.instances = instances;
            scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            scene.projection     = Arcane::PerspectiveProjection(60.0f, float(kW) / float(kH), 0.1f, 100.0f);
            scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
            scene.lightColor     = glm::vec3(1.0f);
            scene.ambient        = glm::vec3(0.08f);

            Arcane::NriGraphContext::FrameDesc frame;
            frame.batch   = &batch;
            frame.mesh    = &scene;
            frame.capture = true;
            return ctx.RenderFrameOffscreen(frame);
        }
    };

    Arcane::PixelData Capture(Arcane::NriGraphContext& ctx)
    {
        Arcane::PixelData out;
        REQUIRE(ctx.ReadCapture(out.width, out.height, out.rgba));
        return out;
    }

    bool SamePicture(const Arcane::PixelData& a, const Arcane::PixelData& b, std::uint32_t* diff = nullptr)
    {
        const auto result = Arcane::CompareImages(a, b);
        if (diff)
            *diff = static_cast<std::uint32_t>(result.diffCount);
        return result.diffCount == 0u;
    }

    // The green rect at (4,4)-(34,16): present means the frame's Batch2D stream was NOT dropped.
    bool BatchPresent(const Arcane::PixelData& picture)
    {
        const std::size_t i = (static_cast<std::size_t>(8) * picture.width + 12) * 4u;
        if (i + 3u >= picture.rgba.size())
            return false;
        const int r = picture.rgba[i], g = picture.rgba[i + 1], b = picture.rgba[i + 2];
        return g > r + 40 && g > b + 40;
    }

    // THE REFERENCE: the old blocking UploadData path draws the finished picture in ONE frame.
    Arcane::PixelData ReferencePicture(Arcane::GraphicsBackend backend, const SceneSpec& spec)
    {
        auto v = MakeGpuVehicle(backend);
        GpuScene scene(spec);
        scene.Install(*v->ctx);
        v->ctx->Textures()->SetUploadMode(Arcane::NriUploadMode::Immediate);
        v->ctx->MeshBuffers()->SetUploadMode(Arcane::NriUploadMode::Immediate);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);
        REQUIRE(scene.Render(*v->ctx, *batch) == Arcane::NriGraphContext::FrameOutcome::Presented);
        return Capture(*v->ctx);
    }

    struct Timeline
    {
        int           firstDrawnFrame = -1;   // index of the first frame whose picture equals the reference
        std::uint32_t lastDiff = 0;
        int           framesRendered = 0;
        bool          batchAlwaysPresent = true;
        std::uint64_t ringOverflow = 0;
        std::uint64_t textureCreates = 0;
        std::uint64_t meshCreates = 0;
        // ResidentCount of {textures, meshes} read right after each frame was rendered.
        std::vector<std::pair<std::size_t, std::size_t>> ready;
    };

    Timeline RunDeferred(Arcane::GraphicsBackend backend, const SceneSpec& spec, const Arcane::PixelData& reference)
    {
        auto v = MakeGpuVehicle(backend);
        v->nri->EnableDeviceWaitIdleCounting();
        GpuScene scene(spec);
        scene.Install(*v->ctx);
        if (spec.forceRetries > 0)
            v->ctx->Textures()->DebugForceRetry(spec.forceRetries);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);

        Timeline t;
        constexpr int kMaxFrames = 10;
        for (int frame = 0; frame < kMaxFrames; ++frame)
        {
            const std::uint64_t idleBefore = Arcane::NriDevice::DeviceWaitIdleCount();
            REQUIRE(scene.Render(*v->ctx, *batch) == Arcane::NriGraphContext::FrameOutcome::Presented);
            // No blocking device wait inside the frame: the whole point of the node.
            if (spec.checkIdle)
                CHECK(Arcane::NriDevice::DeviceWaitIdleCount() == idleBefore);
            ++t.framesRendered;
            t.ready.push_back({ v->ctx->Textures()->ResidentCount(), v->ctx->MeshBuffers()->ResidentCount() });
            const Arcane::PixelData picture = Capture(*v->ctx);
            t.batchAlwaysPresent = t.batchAlwaysPresent && BatchPresent(picture);
            std::uint32_t diff = 0;
            const bool same = SamePicture(picture, reference, &diff);
            t.lastDiff = diff;
            if (same && t.firstDrawnFrame < 0)
                t.firstDrawnFrame = frame;
            if (!v->ctx->HasPendingUploads())
                break;   // settled: this frame is final
        }
        t.ringOverflow   = v->ctx->UploadRingOverflowCount();
        t.textureCreates = v->ctx->Textures()->ObjectCreateCount();
        t.meshCreates    = v->ctx->MeshBuffers()->ObjectCreateCount();
        return t;
    }

    // `texReady[i]` = the texture ResidentCount expected right after frame i (-1 = unchecked);
    // `meshReadyAfter1` is the mesh count expected after frame 1.
    struct Expect
    {
        int               firstDrawn = 2;
        std::vector<long> texReady;
        long              meshReadyAfter1 = 1;
        std::uint64_t     textureCreates = 0;   // 0 = unchecked
    };

    void CheckFirstUse(Arcane::GraphicsBackend backend, const SceneSpec& spec, const Expect& expect)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        const Arcane::PixelData reference = ReferencePicture(backend, spec);

        const Timeline t = RunDeferred(backend, spec, reference);
        INFO("first drawn frame " << t.firstDrawnFrame << ", frames rendered " << t.framesRendered
             << ", last diffCount " << t.lastDiff);
        CHECK(t.firstDrawnFrame == expect.firstDrawn);
        CHECK(t.lastDiff == 0u);       // the settled picture is byte-identical to the blocking path's
        CHECK(t.batchAlwaysPresent);   // the frame's own Batch2D stream is never starved by an upload
        CHECK(t.ringOverflow == 0u);   // no Allocate ever overflowed: oversized / deferred / retried payloads never touch the count
        REQUIRE(t.ready.size() >= 2u);
        if (!spec.albedoMesh)   // (the albedo texture is asked for BEFORE the queuing frame is declared, so it is in that frame's snapshot)
            CHECK(t.ready[0].first == 0u);   // nothing is ready through the declaration of the queuing frame
        CHECK(t.ready[0].second == 0u);
        for (std::size_t i = 0; i < expect.texReady.size() && i < t.ready.size(); ++i)
            if (expect.texReady[i] >= 0)
                CHECK(t.ready[i].first == static_cast<std::size_t>(expect.texReady[i]));
        if (expect.meshReadyAfter1 >= 0)
            CHECK(t.ready[1].second == static_cast<std::size_t>(expect.meshReadyAfter1));
        if (expect.textureCreates != 0)
            CHECK(t.textureCreates == expect.textureCreates);   // no churn: one create per payload
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }

    void CheckAll(Arcane::GraphicsBackend backend)
    {
        // Fresh mesh + texture.
        CheckFirstUse(backend, SceneSpec{}, Expect{ 2, { 0, 1 }, 1, 0 });
        // Oversized texture and mesh: ready after their single upload frame, no retry, no overflow.
        SceneSpec big;
        big.textureSide = 1100;   // ~5.1 MB staging > the 4 MiB slot
        big.sphereRings = 300;    // 150801 vertices x 32 B + 3.6 MB of indices > the 4 MiB slot
        CheckFirstUse(backend, big, Expect{ 2, { 0, 1 }, 1, 0 });
    }

    void CheckBudget(Arcane::GraphicsBackend backend)
    {
        // 640x640 RGBA8 = 1.6 MB staging: one fits the 2 MiB per-frame cap, two do not -> the second waits a frame.
        SceneSpec spec;
        spec.textureSide = 640;
        spec.textureCount = 2;
        CheckFirstUse(backend, spec, Expect{ 3, { 0, 1, 2 }, 1, 2 });
    }

    void CheckInjectedRetry(Arcane::GraphicsBackend backend)
    {
        SceneSpec spec;
        spec.forceRetries = 1;   // the retry branch: bury + recreate, upload next frame
        CheckFirstUse(backend, spec, Expect{ 3, { 0, 0, 1 }, -1, 2 });
    }

    void CheckStarvationWitness(Arcane::GraphicsBackend backend)
    {
        // A 1024x1024 RGBA8 first use (exactly one whole 4 MiB ring slot) in the same frames as a real
        // Batch2D stream. Without the per-frame budget the upload took the entire slot and the frame's
        // own batch failed with a latched render error and vanished.
        SceneSpec spec;
        spec.textureSide = 1024;
        spec.streamRects = 120;
        CheckFirstUse(backend, spec, Expect{ 2, { 0, 1 }, 1, 1 });
    }

    void CheckBurst(Arcane::GraphicsBackend backend)
    {
        // 16 payloads of 256 KB each against a 2 MiB cap: 8 per upload frame (the texture cache takes the
        // budget before the mesh cache, so the mesh waits for frame 3 and the scene is drawn on frame 4);
        // exactly one device texture per payload ever created (no retry churn).
        SceneSpec spec;
        spec.textureSide = 256;
        spec.textureCount = 16;
        CheckFirstUse(backend, spec, Expect{ 4, { 0, 8, 16 }, 0, 16 });
    }

    void CheckTexturedMesh(Arcane::GraphicsBackend backend)
    {
        // The albedo slot flips invalid -> valid once the texture's upload has landed; the mesh must pick
        // it up and end up byte-identical to the blocking path's picture. (The bindless descriptor write may
        // idle on a device without update-after-set; that is the table's own, so the idle count is not asserted.)
        SceneSpec spec;
        spec.albedoMesh = true;
        spec.checkIdle = false;
        CheckFirstUse(backend, spec, Expect{ 2, { 1, 1 }, 1, 0 });
    }

    void CheckTwoContexts(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        SceneSpec spec;
        spec.albedoMesh = true;
        spec.textureCount = 1;
        spec.quads = false;   // only the mesh samples the texture, so B has no copy of its own to make
        const Arcane::PixelData reference = ReferencePicture(backend, spec);

        auto v = MakeGpuVehicle(backend, /*second=*/true);
        v->nri->EnableDeviceWaitIdleCounting();
        GpuScene scene(spec);
        scene.Install(*v->ctx);
        scene.Install(*v->ctxB);
        auto batchA = Arcane::Batcher2D::Create();
        auto batchB = Arcane::Batcher2D::Create();
        REQUIRE(batchA != nullptr);
        REQUIRE(batchB != nullptr);

        // A uploads the texture and maps its bindless slot (frame 2's pre-declaration resolve); A's last
        // submit is NOT waited on -- no capture, no idle, before B starts.
        for (int f = 0; f < 3; ++f)
            REQUIRE(scene.Render(*v->ctx, *batchA) == Arcane::NriGraphContext::FrameOutcome::Presented);
        REQUIRE(v->ctx->Textures()->ResidentCount() == 1u);

        // B first-samples the resource A uploaded, through the device-wide slot, while A is in flight.
        const std::uint64_t idleBefore = Arcane::NriDevice::DeviceWaitIdleCount();
        for (int f = 0; f < 3; ++f)
            REQUIRE(scene.Render(*v->ctxB, *batchB) == Arcane::NriGraphContext::FrameOutcome::Presented);
        CHECK(Arcane::NriDevice::DeviceWaitIdleCount() == idleBefore);
        CHECK(v->ctxB->Textures()->ObjectCreateCount() == 0u);   // B never made its own copy: it sampled A's

        const Arcane::PixelData picture = Capture(*v->ctxB);
        std::uint32_t diff = 0;
        CHECK(SamePicture(picture, reference, &diff));
        INFO("two-context diffCount " << diff);
        CHECK(v->ctx->UploadRingOverflowCount() == 0u);
        CHECK(v->ctxB->UploadRingOverflowCount() == 0u);
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }
}

#define UPLOAD_GPU_CASE(Name, Fn, Backend, BackendTag)                                                      \
    TEST_CASE("upload: " Name " (" BackendTag ")", "[gpu][upload]")                                         \
    {                                                                                                       \
        ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::Backend);                                              \
        Fn(Arcane::GraphicsBackend::Backend);                                                               \
    }

UPLOAD_GPU_CASE("a fresh texture and mesh, and an oversized pair, are drawn two frames after they were "
                "queued, byte-identical to the blocking path", CheckAll, D3D12, "d3d12")
UPLOAD_GPU_CASE("a fresh texture and mesh, and an oversized pair, are drawn two frames after they were "
                "queued, byte-identical to the blocking path", CheckAll, Vulkan, "vulkan")
UPLOAD_GPU_CASE("a second texture over the per-frame budget waits one frame", CheckBudget, D3D12, "d3d12")
UPLOAD_GPU_CASE("a second texture over the per-frame budget waits one frame", CheckBudget, Vulkan, "vulkan")
UPLOAD_GPU_CASE("the retry guard buries, recreates and uploads one frame later", CheckInjectedRetry, D3D12, "d3d12")
UPLOAD_GPU_CASE("the retry guard buries, recreates and uploads one frame later", CheckInjectedRetry, Vulkan, "vulkan")
UPLOAD_GPU_CASE("a 1024x1024 first use does not starve the same frames' Batch2D stream", CheckStarvationWitness, D3D12, "d3d12")
UPLOAD_GPU_CASE("a 1024x1024 first use does not starve the same frames' Batch2D stream", CheckStarvationWitness, Vulkan, "vulkan")
UPLOAD_GPU_CASE("a burst of sixteen payloads creates one texture each, no churn", CheckBurst, D3D12, "d3d12")
UPLOAD_GPU_CASE("a burst of sixteen payloads creates one texture each, no churn", CheckBurst, Vulkan, "vulkan")
UPLOAD_GPU_CASE("a textured mesh picks up its albedo slot once the texture has landed", CheckTexturedMesh, D3D12, "d3d12")
UPLOAD_GPU_CASE("a textured mesh picks up its albedo slot once the texture has landed", CheckTexturedMesh, Vulkan, "vulkan")
UPLOAD_GPU_CASE("a second context first-samples what the first uploaded while it is in flight", CheckTwoContexts, D3D12, "d3d12")
UPLOAD_GPU_CASE("a second context first-samples what the first uploaded while it is in flight", CheckTwoContexts, Vulkan, "vulkan")
