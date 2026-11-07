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
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/PostChainCache.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Scene/SceneCamera.hpp>

#undef ERROR

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
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
        std::uint32_t keyBase      = 0;      // distinguishes the texture keys of repeated runs (fresh keys)
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
                std::snprintf(text, sizeof(text), "d4d4d4d4-0000-0000-%04x-0000000000%02x", spec.keyBase, i + 1);
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
        // A heavy upload (a 2000x2000 texture, 16 MB through dedicated staging) so A's upload submit has real GPU
        // work to retire; only the mesh samples the texture, so B has no copy of its own to make.
        // WHAT A'S SUBMIT CARRIES, by 0-based repeat: each context has its own mesh cache, and A's frame resolves
        // A's copy of the mesh during declaration, after that frame's upload snapshot. So repeat 0 carries the
        // texture and leaves A's mesh queued, repeat 1 carries the texture AND A's mesh, and repeat 2 on carries
        // the texture alone. The per-repeat readings logged below give the queued count. The fence covers the
        // whole submit, not the texture copy.
        SceneSpec base;
        base.albedoMesh = true;
        base.textureCount = 1;
        base.textureSide = 2000;
        base.sphereRings = 300;
        base.quads = false;
        const Arcane::PixelData reference = ReferencePicture(backend, base);
        SceneSpec flatSpec = base;
        flatSpec.albedoMesh = false;   // B's warm-up frames: the mesh, with no slot (it never asks for the texture)

        auto v = MakeGpuVehicle(backend, /*second=*/true);
        v->nri->EnableDeviceWaitIdleCounting();
        GpuScene flat(flatSpec);
        flat.Install(*v->ctxB);
        auto batchA = Arcane::Batcher2D::Create();
        auto batchB = Arcane::Batcher2D::Create();
        REQUIRE(batchA != nullptr);
        REQUIRE(batchB != nullptr);

        // 1. B's MESH becomes resident first (it renders with no albedo slot until the mesh lands), so that
        //    later B needs no queue or upload frame of its own before it samples.
        for (int f = 0; f < 3; ++f)
            REQUIRE(flat.Render(*v->ctxB, *batchB) == Arcane::NriGraphContext::FrameOutcome::Presented);
        REQUIRE(v->ctxB->MeshBuffers()->ResidentCount() == 1u);
        CHECK_FALSE(v->ctxB->HasPendingUploads());

        // Bounded repeats with FRESH texture keys, until one repeat has A's upload still in flight when B records
        // (completed fence < the upload's submit value). Every repeat passes the picture / idle / error checks.
        constexpr int kMaxRepeats = 8;
        int repeats = 0;
        bool observedInFlight = false;
        std::uint32_t previousSlot = 0xFFFFFFFFu;
        std::ostringstream readings;
        for (int r = 0; r < kMaxRepeats && !observedInFlight; ++r)
        {
            ++repeats;
            SceneSpec spec = base;
            spec.keyBase = static_cast<std::uint32_t>(r + 1);
            GpuScene scene(spec);
            scene.Install(*v->ctx);
            scene.Install(*v->ctxB);

            // 2. A queues and UPLOADS the texture in its frame (the texture is asked for before that frame is
            //    declared, so the frame's snapshot carries it). No capture, no fence wait, no idle.
            const std::uint64_t idleBefore = Arcane::NriDevice::DeviceWaitIdleCount();
            const std::size_t aMeshQueued = v->ctx->MeshBuffers()->QueuedUploadCount();   // goes in A's snapshot too
            REQUIRE(scene.Render(*v->ctx, *batchA) == Arcane::NriGraphContext::FrameOutcome::Presented);
            const std::uint64_t uploadSubmit = v->ctx->Graph().DebugSubmitCount();
            const std::uint64_t completedAfterA = v->ctx->Graph().CompletedFence();   // read IMMEDIATELY after A's render, before the resolve
            //    ...then A maps the device-wide bindless slot with a plain resolve.
            const std::uint32_t slot = v->ctx->ResolveMeshAlbedoSlot(scene.texIds[0]);
            REQUIRE(slot != 0xFFFFFFFFu);
            // The pixels ignore keyBase, so the picture alone would also pass on the previous repeat's texture.
            CHECK(slot != previousSlot);
            previousSlot = slot;

            // 3. B's very next frame resolves the slot (a lookup hit on A's mapping) and samples A's texture,
            //    recorded straight after A's upload submit.
            const std::uint64_t completedAtRecord = v->ctx->Graph().CompletedFence();   // the START of B's frame
            REQUIRE(scene.Render(*v->ctxB, *batchB) == Arcane::NriGraphContext::FrameOutcome::Presented);
            const std::uint64_t completedAfterB = v->ctx->Graph().CompletedFence();     // before Capture: it idles the device
            CHECK(v->ctxB->ResolveMeshAlbedoSlot(scene.texIds[0]) == slot);
            observedInFlight = completedAtRecord < uploadSubmit;
            readings << "\nrepeat " << r << ": A mesh uploads queued " << aMeshQueued << ", A upload submit " << uploadSubmit
                     << ", A completed fence right after A's render " << completedAfterA << ", when B recorded "
                     << completedAtRecord << ", after B's render " << completedAfterB;

            CHECK(Arcane::NriDevice::DeviceWaitIdleCount() == idleBefore);   // the slot write needed no idle either
            CHECK(v->ctxB->Textures()->ObjectCreateCount() == 0u);           // B never made a copy: it sampled A's

            // 4. B's frame draws exactly the picture the blocking path draws.
            const Arcane::PixelData picture = Capture(*v->ctxB);
            std::uint32_t diff = 0;
            const bool samePicture = SamePicture(picture, reference, &diff);
            INFO("two-context repeat " << r << " diffCount " << diff);
            CHECK(samePicture);
            CHECK(v->ctx->UploadRingOverflowCount() == 0u);
            CHECK(v->ctxB->UploadRingOverflowCount() == 0u);
            CHECK(Arcane::RenderErrorCount() == errorsBefore);
        }

        // Reported through the test log either way; a fast GPU can legitimately never show A in flight.
        WARN("two-context: " << repeats << " repeat(s) ran; in-flight observed = " << (observedInFlight ? "yes" : "no")
             << readings.str()
             << (observedInFlight ? "" : "\nnever observed in flight: on this GPU the upload retired before B recorded, "
                                         "so the case proves ordering by submission order only"));
    }

    // ---- A MATERIAL WITH DECLARED TEXTURES: the first build holds, so first use needs no device idle ----
    struct MaterialKit
    {
        std::shared_ptr<const Arcane::MaterialTemplate> templ;
        std::shared_ptr<const Arcane::MaterialInstance> instance;
        std::shared_ptr<const std::vector<std::uint8_t>> vs, ps;
    };

    MaterialKit CompileDetailMaterial(Arcane::GraphicsBackend backend, const Arcane::Guid& texA, const Arcane::Guid& texB)
    {
        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("data/shaders");
        const auto templateText = provider.Get(Arcane::MaterialTemplateFile(Arcane::MaterialSurface::Sprite));
        REQUIRE(templateText.has_value());
        const Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
            *templateText,
            "//@param texture DetailA\n//@param texture DetailB\n"
            "float4 shade(Varyings v) { float4 a = DetailA.Sample(MaterialSampler, v.uv); "
            "float4 b = DetailB.Sample(MaterialSampler, v.uv); return float4((a.rgb + b.rgb) * 0.5, 1.0) * v.color; }",
            "upload_detail", Arcane::MaterialSurface::Sprite);
        REQUIRE(build.errors.empty());

        Arcane::ShaderCompiler compiler;
        REQUIRE(compiler.InitializeWithDebounce(0.0));
        const auto compileStage = [&](const char* entry, const char* profile)
        {
            Arcane::ShaderCompileRequest request;
            request.debugName = "upload_detail.hlsl";
            request.sourceUtf8 = build.hlsl;
            request.entry = entry;
            request.profile = profile;
            return compiler.CompileNow(request);
        };
        const Arcane::ShaderCompileResult vs = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
        const Arcane::ShaderCompileResult ps = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
        const bool vulkan = backend == Arcane::GraphicsBackend::Vulkan;
        const Arcane::ShaderTargetResult& vsTarget = vulkan ? vs.spirv : vs.dxil;
        const Arcane::ShaderTargetResult& psTarget = vulkan ? ps.spirv : ps.dxil;
        REQUIRE(vsTarget.succeeded);
        REQUIRE(psTarget.succeeded);

        MaterialKit kit;
        kit.templ = std::make_shared<const Arcane::MaterialTemplate>(build.templ);
        auto instance = std::make_shared<Arcane::MaterialInstance>(kit.templ);
        REQUIRE(instance->SetTexture("DetailA", texA));
        REQUIRE(instance->SetTexture("DetailB", texB));
        kit.instance = instance;
        kit.vs = std::make_shared<const std::vector<std::uint8_t>>(vsTarget.bytecode);
        kit.ps = std::make_shared<const std::vector<std::uint8_t>>(psTarget.bytecode);
        return kit;
    }

    std::uint16_t RegisterDetailMaterial(Arcane::Batcher2D& batch, const MaterialKit& kit)
    {
        Arcane::Material2DDesc desc;
        desc.templ = kit.templ;
        desc.instance = kit.instance;
        desc.vsBytes = kit.vs;
        desc.psBytes = kit.ps;
        const std::uint16_t id = batch.RegisterMaterial(std::move(desc));
        REQUIRE(id != Arcane::Batcher2D::kInvalidMaterialId);
        return id;
    }

    void CheckMaterialFirstUse(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        // Two 640x640 images (1.6 MB staging each): the 2 MiB per-frame cap takes one per upload frame, so the
        // material's two declared images are spread across two upload frames.
        const Arcane::Guid texA = *Arcane::Guid::FromString("e5e5e5e5-0000-0000-0000-000000000001");
        const Arcane::Guid texB = *Arcane::Guid::FromString("e5e5e5e5-0000-0000-0000-000000000002");
        Arcane::PixelData pixelsA, pixelsB;
        for (Arcane::PixelData* p : { &pixelsA, &pixelsB })
        {
            p->width = p->height = 640;
            p->rgba.resize(640u * 640u * 4u);
        }
        for (std::uint32_t y = 0; y < 640; ++y)
            for (std::uint32_t x = 0; x < 640; ++x)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * 640 + x) * 4;
                pixelsA.rgba[i] = static_cast<unsigned char>(x / 3); pixelsA.rgba[i + 1] = 40; pixelsA.rgba[i + 2] = 40; pixelsA.rgba[i + 3] = 255;
                pixelsB.rgba[i] = 40; pixelsB.rgba[i + 1] = static_cast<unsigned char>(y / 3); pixelsB.rgba[i + 2] = 200; pixelsB.rgba[i + 3] = 255;
            }
        const auto supply = [&](const Arcane::Guid& id) -> const Arcane::PixelData* {
            return id == texA ? &pixelsA : id == texB ? &pixelsB : nullptr;
        };
        const MaterialKit kit = CompileDetailMaterial(backend, texA, texB);

        const auto draw = [&](Arcane::Batcher2D& batch, std::uint16_t material) -> Arcane::NriGraphContext::FrameDesc
        {
            batch.Begin(kW, kH);
            batch.SetLayer(0, 0);
            batch.Rect(glm::vec2(4.0f, 4.0f), glm::vec2(30.0f, 12.0f), glm::vec4(0.1f, 0.8f, 0.2f, 1.0f));
            batch.QuadMaterial(material, glm::vec2(60.0f, 20.0f), glm::vec2(64.0f, 64.0f),
                               glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
            Arcane::NriGraphContext::FrameDesc frame;
            frame.batch = &batch;
            frame.capture = true;
            return frame;
        };

        // The reference: the blocking path draws the material in one frame.
        Arcane::PixelData reference;
        {
            auto v = MakeGpuVehicle(backend);
            v->ctx->SetPixelSupply(supply);
            v->ctx->Textures()->SetUploadMode(Arcane::NriUploadMode::Immediate);
            v->ctx->MeshBuffers()->SetUploadMode(Arcane::NriUploadMode::Immediate);
            auto batch = Arcane::Batcher2D::Create();
            REQUIRE(batch != nullptr);
            const std::uint16_t id = RegisterDetailMaterial(*batch, kit);
            REQUIRE(v->ctx->RenderFrameOffscreen(draw(*batch, id)) == Arcane::NriGraphContext::FrameOutcome::Presented);
            reference = Capture(*v->ctx);
        }

        auto v = MakeGpuVehicle(backend);
        v->nri->EnableDeviceWaitIdleCounting();
        v->ctx->SetPixelSupply(supply);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);
        const std::uint16_t id = RegisterDetailMaterial(*batch, kit);

        int firstDrawn = -1;
        bool sawHeldFrame = false;
        for (int frame = 0; frame < 10; ++frame)
        {
            const std::uint64_t idleBefore = Arcane::NriDevice::DeviceWaitIdleCount();
            REQUIRE(v->ctx->RenderFrameOffscreen(draw(*batch, id)) == Arcane::NriGraphContext::FrameOutcome::Presented);
            // Count 0 across first use: the held first build, the build itself, the draws.
            CHECK(Arcane::NriDevice::DeviceWaitIdleCount() == idleBefore);
            const Arcane::PixelData picture = Capture(*v->ctx);
            const bool same = SamePicture(picture, reference);
            if (same && firstDrawn < 0)
                firstDrawn = frame;
            sawHeldFrame = sawHeldFrame || !same;
            CHECK(BatchPresent(picture));
            if (!v->ctx->HasPendingUploads() && frame > 0)
                break;
        }
        INFO("material first drawn frame " << firstDrawn);
        CHECK(sawHeldFrame);                 // it was held (drawn through the plain sprite pipeline) meanwhile
        CHECK(firstDrawn == 3);              // queue, upload A, upload B (the budget spreads them), build + draw
        CHECK(v->ctx->UploadRingOverflowCount() == 0u);
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }

    // ---- A POST CHAIN WITH A DECLARED TEXTURE: the first build holds, so first use needs no device idle ----
    void CheckPostChainFirstUse(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        const Arcane::Guid detail = *Arcane::Guid::FromString("e6e6e6e6-0000-0000-0000-000000000001");
        Arcane::PixelData pixels;
        pixels.width = pixels.height = 640;   // 1.6 MB staging: one upload frame under the 2 MiB cap
        pixels.rgba.resize(640u * 640u * 4u);
        for (std::uint32_t y = 0; y < 640; ++y)
            for (std::uint32_t x = 0; x < 640; ++x)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * 640 + x) * 4;
                pixels.rgba[i] = static_cast<unsigned char>(40 + x / 4); pixels.rgba[i + 1] = static_cast<unsigned char>(40 + y / 4);
                pixels.rgba[i + 2] = 90; pixels.rgba[i + 3] = 255;
            }
        const auto supply = [&](const Arcane::Guid& id) -> const Arcane::PixelData* { return id == detail ? &pixels : nullptr; };

        // The compile path is OverlayWitnessTest.cpp CheckPostDoesNotGradeOverlay's, with a declared texture.
        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("data/shaders");
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const char* shade =
            "//@param texture Detail\n"
            "float4 shade(Varyings v) { return InputTexture.Sample(MaterialSampler, v.uv) * Detail.Sample(MaterialSampler, v.uv); }\n";
        const std::uint32_t sceneIn[] = { Arcane::kSceneInput };
        const Arcane::MaterialChainPassDesc authored[] = { { shade, sceneIn } };
        Arcane::MaterialChainBuildResult built = Arcane::BuildMaterialChainSource(
            *templateText, authored, "upload_post_detail", {}, /*externalInput=*/true);
        REQUIRE(built.Ok());

        Arcane::ShaderCompiler compiler;
        REQUIRE(compiler.InitializeWithDebounce(0.0));
        const auto compileStage = [&](const char* entry, const char* profile)
        {
            Arcane::ShaderCompileRequest req;
            req.debugName = "upload_post_detail.hlsl";
            req.sourceUtf8 = built.hlsl[0];
            req.entry = entry;
            req.profile = profile;
            const Arcane::ShaderCompileResult cr = compiler.CompileNow(req);
            REQUIRE(cr.AllSucceeded());
            const Arcane::ShaderTargetResult& target = backend == Arcane::GraphicsBackend::D3D12 ? cr.dxil : cr.spirv;
            REQUIRE(target.succeeded);
            return std::make_shared<const std::vector<std::uint8_t>>(target.bytecode);
        };
        auto templ = std::make_shared<Arcane::MaterialTemplate>(std::move(built.templ));
        auto instance = std::make_shared<Arcane::MaterialInstance>(std::shared_ptr<const Arcane::MaterialTemplate>(templ));
        REQUIRE(instance->SetTexture("Detail", detail));
        Arcane::PostChainDesc chain;
        chain.templ = templ;
        chain.instance = instance;
        chain.chainInputSlots = built.chainInputSlots;
        Arcane::PostChainPassDesc pass;
        pass.vsBytes = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
        pass.psBytes = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
        pass.inputs = built.passInputs[0];
        chain.passes.push_back(std::move(pass));
        compiler.Shutdown();

        const auto draw = [&](Arcane::Batcher2D& batch) -> Arcane::NriGraphContext::FrameDesc
        {
            batch.Begin(kW, kH);
            batch.SetLayer(0, 0);
            batch.Rect(glm::vec2(4.0f, 4.0f), glm::vec2(30.0f, 12.0f), glm::vec4(0.1f, 0.8f, 0.2f, 1.0f));
            batch.Rect(glm::vec2(50.0f, 20.0f), glm::vec2(80.0f, 50.0f), glm::vec4(0.9f, 0.9f, 0.3f, 1.0f));
            Arcane::NriGraphContext::FrameDesc frame;
            frame.batch = &batch;
            frame.post = &chain;
            frame.capture = true;
            return frame;
        };

        Arcane::PixelData reference;
        {
            auto v = MakeGpuVehicle(backend);
            v->ctx->SetPixelSupply(supply);
            v->ctx->Textures()->SetUploadMode(Arcane::NriUploadMode::Immediate);
            auto batch = Arcane::Batcher2D::Create();
            REQUIRE(batch != nullptr);
            REQUIRE(v->ctx->RenderFrameOffscreen(draw(*batch)) == Arcane::NriGraphContext::FrameOutcome::Presented);
            reference = Capture(*v->ctx);
        }

        auto v = MakeGpuVehicle(backend);
        v->nri->EnableDeviceWaitIdleCounting();
        v->ctx->SetPixelSupply(supply);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);
        int firstDrawn = -1;
        bool sawUnfinishedFrame = false;
        for (int frame = 0; frame < 10; ++frame)
        {
            const std::uint64_t idleBefore = Arcane::NriDevice::DeviceWaitIdleCount();
            REQUIRE(v->ctx->RenderFrameOffscreen(draw(*batch)) == Arcane::NriGraphContext::FrameOutcome::Presented);
            // THE WITNESS OF THE HOLD: unchanged across each render (not 0: the captures below idle), since a
            // chain built while its image is queued rebuilds behind a device idle once the image lands.
            CHECK(Arcane::NriDevice::DeviceWaitIdleCount() == idleBefore);
            const Arcane::PixelData picture = Capture(*v->ctx);
            const bool same = SamePicture(picture, reference);
            if (same && firstDrawn < 0)
                firstDrawn = frame;
            sawUnfinishedFrame = sawUnfinishedFrame || !same;   // (no BatchPresent probe: the chain tints every pixel once built)
            if (!v->ctx->HasPendingUploads() && frame > 0)
                break;
        }
        INFO("post chain first drawn frame " << firstDrawn);
        // Early frames differ from the finished picture. NOT proof of the hold: without it they would draw the chain
        // over the white texel, which differs too. The idle check above is the witness.
        CHECK(sawUnfinishedFrame);
        CHECK(firstDrawn == 2);     // queue, upload, build + draw
        CHECK(v->ctx->UploadRingOverflowCount() == 0u);
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
UPLOAD_GPU_CASE("a second context samples a texture the first uploaded with no wait straight after the upload submit (in flight when the timing allows)", CheckTwoContexts, D3D12, "d3d12")
UPLOAD_GPU_CASE("a second context samples a texture the first uploaded with no wait straight after the upload submit (in flight when the timing allows)", CheckTwoContexts, Vulkan, "vulkan")
UPLOAD_GPU_CASE("a material with declared textures holds its first build and needs no device idle", CheckMaterialFirstUse, D3D12, "d3d12")
UPLOAD_GPU_CASE("a material with declared textures holds its first build and needs no device idle", CheckMaterialFirstUse, Vulkan, "vulkan")
UPLOAD_GPU_CASE("a post chain with a declared texture holds its first build and needs no device idle", CheckPostChainFirstUse, D3D12, "d3d12")
UPLOAD_GPU_CASE("a post chain with a declared texture holds its first build and needs no device idle", CheckPostChainFirstUse, Vulkan, "vulkan")
