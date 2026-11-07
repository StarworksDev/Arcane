// Plan C Task 5 -- the real `upload` node on D3D12 and on Vulkan. [gpu][upload]:
// run BY NAME, one GPU suite at a time on the desk. Every case asserts
// RenderErrorCount() unchanged; the D3D12 debug layer stays forced (test_main).
// The device-less half (arithmetic, queue, snapshot rule) is UploadStagingTest.cpp.
//
// THE TIMELINE UNDER TEST (user ruling 2026-11-06): frame N queues, N+1 uploads,
// N+2 draws. Each case renders a scene with other work in the frame (a rect) plus
// fresh textured quads and a fresh mesh, reads back EVERY frame, and compares the
// first frame that shows them byte-for-byte with the picture the OLD blocking
// UploadData path (NriUploadMode::Immediate -- a test-only mode) draws in its one
// frame. The device-wait-idle count is read around the RENDER calls only
// (ReadCapture idles by contract).
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
        ~GpuVehicle()
        {
            ctx.reset();   // wrap before owner, as every other vehicle in the suite
            nri.reset();
            native.reset();
        }
    };

    std::unique_ptr<GpuVehicle> MakeGpuVehicle(Arcane::GraphicsBackend backend)
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
        return v;
    }

    struct SceneSpec
    {
        std::uint32_t textureSide  = 8;   // each texture is textureSide x textureSide RGBA8
        int           textureCount = 1;
        std::uint32_t sphereRings  = 0;   // 0 = a cube, else a UV sphere of that many rings (x rings*2 segments)
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
                texIds.push_back(*Arcane::Guid::FromString(
                    std::string("d4d4d4d4-0000-0000-0000-00000000000") + char('1' + i)));
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

        // One frame of the scene: a rect (other work in the frame), one textured quad per
        // texture, and the mesh. The batcher is re-filled every call (a frame drains it).
        Arcane::NriGraphContext::FrameOutcome Render(Arcane::NriGraphContext& ctx, Arcane::Batcher2D& batch)
        {
            batch.Begin(kW, kH);
            batch.SetLayer(0, 0);
            batch.Rect(glm::vec2(4.0f, 4.0f), glm::vec2(30.0f, 12.0f), glm::vec4(0.1f, 0.8f, 0.2f, 1.0f));
            for (std::size_t i = 0; i < texIds.size(); ++i)
                batch.QuadTextured(Arcane::Batcher2D::kMaterialSprite, texIds[i],
                                   glm::vec2(8.0f + 40.0f * float(i), 60.0f), glm::vec2(32.0f, 32.0f),
                                   glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));

            Arcane::MeshInstance instance;
            instance.mesh      = meshId;
            instance.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
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
        // ResidentCount of {textures, meshes} read right after each frame was rendered.
        std::vector<std::pair<std::size_t, std::size_t>> ready;
    };

    Timeline RunDeferred(Arcane::GraphicsBackend backend, const SceneSpec& spec, const Arcane::PixelData& reference)
    {
        auto v = MakeGpuVehicle(backend);
        v->nri->EnableDeviceWaitIdleCounting();
        GpuScene scene(spec);
        scene.Install(*v->ctx);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);

        Timeline t;
        constexpr int kMaxFrames = 8;
        for (int frame = 0; frame < kMaxFrames; ++frame)
        {
            const std::uint64_t idleBefore = Arcane::NriDevice::DeviceWaitIdleCount();
            REQUIRE(scene.Render(*v->ctx, *batch) == Arcane::NriGraphContext::FrameOutcome::Presented);
            // No blocking device wait inside the frame: the whole point of the node.
            CHECK(Arcane::NriDevice::DeviceWaitIdleCount() == idleBefore);
            ++t.framesRendered;
            t.ready.push_back({ v->ctx->Textures()->ResidentCount(), v->ctx->MeshBuffers()->ResidentCount() });
            const Arcane::PixelData picture = Capture(*v->ctx);
            std::uint32_t diff = 0;
            const bool same = SamePicture(picture, reference, &diff);
            t.lastDiff = diff;
            if (same && t.firstDrawnFrame < 0)
                t.firstDrawnFrame = frame;
            if (!v->ctx->HasPendingUploads())
                break;   // settled: this frame is final
        }
        return t;
    }

    void CheckFirstUse(Arcane::GraphicsBackend backend, const SceneSpec& spec, int expectedFirstDrawnFrame,
                       bool expectRetry)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        const Arcane::PixelData reference = ReferencePicture(backend, spec);

        const Timeline t = RunDeferred(backend, spec, reference);
        INFO("first drawn frame " << t.firstDrawnFrame << ", frames rendered " << t.framesRendered
             << ", last diffCount " << t.lastDiff);
        CHECK(t.firstDrawnFrame == expectedFirstDrawnFrame);
        CHECK(t.lastDiff == 0u);   // the settled picture is byte-identical to the blocking path's
        REQUIRE(t.ready.size() >= 2u);
        // Nothing is ready through the declaration of the frame that queued it...
        CHECK(t.ready[0].first == 0u);
        CHECK(t.ready[0].second == 0u);
        if (!expectRetry)
        {
            // ...and everything is ready after the single upload frame (an oversized payload
            // is NOT a retry: it uploads in that same frame through a dedicated staging buffer).
            CHECK(t.ready[1].first == static_cast<std::size_t>(spec.textureCount));
            CHECK(t.ready[1].second == 1u);
        }
        else
        {
            // One texture fits the empty slot, the second does not: it is retried a frame later.
            CHECK(t.ready[1].first == 1u);
            REQUIRE(t.ready.size() >= 3u);
            CHECK(t.ready[2].first == static_cast<std::size_t>(spec.textureCount));
        }
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }
}

TEST_CASE("upload: a fresh texture and a fresh mesh are drawn two frames after they were queued, "
          "byte-identical to the blocking path (d3d12)", "[gpu][upload]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);
    CheckFirstUse(Arcane::GraphicsBackend::D3D12, SceneSpec{}, 2, false);
}

TEST_CASE("upload: a fresh texture and a fresh mesh are drawn two frames after they were queued, "
          "byte-identical to the blocking path (vulkan)", "[gpu][upload]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::Vulkan);
    CheckFirstUse(Arcane::GraphicsBackend::Vulkan, SceneSpec{}, 2, false);
}

TEST_CASE("upload: an oversized texture and an oversized mesh upload in their single upload frame, "
          "no retry (d3d12)", "[gpu][upload]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);
    SceneSpec spec;
    spec.textureSide = 1100;   // 1100 rows x 4608 aligned = ~5.1 MB staging > the 4 MiB ring
    spec.sphereRings = 300;    // 150801 vertices x 32 B + 3.6 MB of indices > the 4 MiB ring
    CheckFirstUse(Arcane::GraphicsBackend::D3D12, spec, 2, false);
}

TEST_CASE("upload: an oversized texture and an oversized mesh upload in their single upload frame, "
          "no retry (vulkan)", "[gpu][upload]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::Vulkan);
    SceneSpec spec;
    spec.textureSide = 1100;
    spec.sphereRings = 300;
    CheckFirstUse(Arcane::GraphicsBackend::Vulkan, spec, 2, false);
}

TEST_CASE("upload: two textures that do not fit one ring slot together retry the second one frame "
          "later (d3d12)", "[gpu][upload]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);
    SceneSpec spec;
    spec.textureSide  = 900;   // ~3.5 MB each: one fits an empty 4 MiB slot, two do not
    spec.textureCount = 2;
    CheckFirstUse(Arcane::GraphicsBackend::D3D12, spec, 3, true);
}

TEST_CASE("upload: two textures that do not fit one ring slot together retry the second one frame "
          "later (vulkan)", "[gpu][upload]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::Vulkan);
    SceneSpec spec;
    spec.textureSide  = 900;
    spec.textureCount = 2;
    CheckFirstUse(Arcane::GraphicsBackend::Vulkan, spec, 3, true);
}
