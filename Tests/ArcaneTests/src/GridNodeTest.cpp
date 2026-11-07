// GridNode -- the 3D reference grid's PIXEL cases (F4 plan 1 Task 10, spec
// s5.2). [gpu][pixel], outside the ~[gpu] baseline set, on BOTH backends.
//
// WHAT IS ASSERTED, and structurally rather than by value (the same rule
// NriGraphPixelTest.cpp's banner states -- everything here has been through
// the tonemap, so a literal expectation would pin the curve by accident):
//
//   1. THE GRID IS THERE: a 256x256 frame, XZ plane, a perspective eye at
//      (0, 5, 10) looking at the origin, NO mesh -- at least one pixel in the
//      centre column is brighter than the clear colour (the world-Z axis line
//      projects exactly onto that column), and an OFF-axis column carries
//      both brighter pixels (minor lines crossing it) and clear-coloured
//      pixels between them (it is a pattern, not a fill);
//   2. THE SKY IS NOT COVERED: the top rows are the clear colour, because the
//      quad lies on the ground plane and the horizon sits well below them at
//      this eye (atan(5/10) = 26.6 degrees above the view axis, inside a
//      60-degree fov -- the horizon lands around row 17 of 256);
//   3. THE DEPTH TEST: with a 2 m cube resting on the plane at the origin,
//      the centre pixel is the CUBE's colour (red), not the grid's blue Z
//      axis blended over it -- the grid tests against the mesh pass's depth
//      transient. Without the test the axis line (alpha 0.9) would turn that
//      pixel blue.
//
// The vehicle is NriGraphPixelTest.cpp's (device -> NRI wrap -> offscreen
// graph context), restated here because that file's helpers live in an
// anonymous namespace; 256x256 rather than its 160x96 so the geometry above
// has room.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>            // BuildCube
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/nodes/GridNode.hpp>
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>
#include <Arcane/Scene/SceneCamera.hpp>           // PerspectiveProjection
#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>           // lookAtRH, translate

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include "Helpers/GpuCapability.hpp"
#include "Settings/EditorGridSettings.hpp"   // MakeGridScene -- the grid desc the editor builds

namespace
{
    constexpr std::uint32_t kW = 256;
    constexpr std::uint32_t kH = 256;

    struct GridVehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner> native;
        std::unique_ptr<Arcane::NriDevice>         nri;
        std::unique_ptr<Arcane::NriGraphContext>   ctx;
    };

    GridVehicle MakeVehicle(Arcane::GraphicsBackend backend)
    {
        GridVehicle v;
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
        Arcane::HostConfig cfg;
        cfg.backend = backend;
        v.ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *v.nri, kW, kH, {});
        REQUIRE(v.ctx != nullptr);
        v.ctx->SetUploadModeForTests(Arcane::NriUploadMode::Immediate);   // node-level pixel case: it renders ONE frame of a scene whose mesh/textures are first used there (Plan C Task 5: the deferred timeline is UploadStagingGpuTest's)
        REQUIRE(v.ctx->SurfaceWidth()  == kW);
        REQUIRE(v.ctx->SurfaceHeight() == kH);
        return v;
    }

    struct Rgba
    {
        std::uint8_t r = 0, g = 0, b = 0, a = 0;
    };

    Rgba At(const std::vector<unsigned char>& rgba, std::uint32_t w,
            std::uint32_t x, std::uint32_t y)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
        REQUIRE(i + 3u < rgba.size());
        return Rgba{ rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3] };
    }

    [[nodiscard]] int Luma(const Rgba& p) noexcept
    {
        return static_cast<int>(p.r) + static_cast<int>(p.g) + static_cast<int>(p.b);
    }

    // The editor's perspective vantage for these cases: 5 m up, 10 m back,
    // looking at the origin, +Y up (the engine's right-handed convention).
    Arcane::ViewTransform GridView()
    {
        Arcane::ViewTransform view;
        view.view = glm::lookAtRH(glm::vec3(0.0f, 5.0f, 10.0f),
                                 glm::vec3(0.0f, 0.0f, 0.0f),
                                 glm::vec3(0.0f, 1.0f, 0.0f));
        view.projection = Arcane::PerspectiveProjection(60.0f, 1.0f, 0.1f, 500.0f);
        view.viewport   = { kW, kH };
        return view;
    }

    std::vector<unsigned char> CaptureGrid(Arcane::GraphicsBackend backend,
                                           const Arcane::GridSceneDesc& grid,
                                           const Arcane::MeshSceneDesc* mesh,
                                           Arcane::NriMeshBufferCache::MeshSupplyFn supply,
                                           std::uint32_t& w, std::uint32_t& h)
    {
        GridVehicle v = MakeVehicle(backend);
        if (supply)
            v.ctx->SetMeshSupply(std::move(supply));

        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.grid    = &grid;
        frame.mesh    = mesh;
        REQUIRE(v.ctx->RenderFrameOffscreen(frame) == Arcane::NriGraphContext::FrameOutcome::Presented);

        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        return rgba;
    }

    void CheckGridDrawsAndSkyStaysClear(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        // The editor's own defaults (settings S6-21): the desc's tunables are
        // zero until MakeGridScene fills them.
        Arcane::GridSceneDesc grid = Arcane::Editor::MakeGridScene(Arcane::Editor::EditorGridSettings{},
                                                                   Arcane::Editor::EditorGrid3DSettings{});
        grid.view = GridView();
        grid.SetPlane(Arcane::GridSceneDesc::Plane::XZ);

        std::uint32_t w = 0, h = 0;
        const std::vector<unsigned char> px = CaptureGrid(backend, grid, nullptr, nullptr, w, h);
        REQUIRE(w == kW);
        REQUIRE(h == kH);

        // THE CLEAR COLOUR, as it arrives after the tonemap: the top-left
        // corner pixel, which the ground quad cannot reach (sky).
        const Rgba clear = At(px, w, 0u, 0u);

        // 1. THE CENTRE COLUMN carries the world-Z axis (x == 0 projects onto
        //    x == w/2 from an eye on the Z axis) -- at least one pixel in its
        //    lower half is well above the clear.
        int brightCentre = 0;
        for (std::uint32_t y = h / 2u; y < h; ++y)
            if (Luma(At(px, w, w / 2u, y)) > Luma(clear) + 30)
                ++brightCentre;
        CHECK(brightCentre > 0);

        // ...and an OFF-axis column (40 px right of centre) shows a PATTERN:
        //    minor lines crossing it (bright) with gaps between them (clear).
        int brightOff = 0, darkOff = 0;
        for (std::uint32_t y = h / 2u; y < h; ++y)
        {
            const Rgba p = At(px, w, w / 2u + 40u, y);
            if (Luma(p) > Luma(clear) + 30)
                ++brightOff;
            else if (std::abs(Luma(p) - Luma(clear)) < 12)
                ++darkOff;
        }
        CHECK(brightOff > 0);
        CHECK(darkOff > 0);

        // 2. THE SKY: the top four rows equal the clear at five columns each.
        for (std::uint32_t y = 0; y < 4u; ++y)
            for (std::uint32_t x : { 0u, w / 4u, w / 2u, 3u * w / 4u, w - 1u })
            {
                const Rgba p = At(px, w, x, y);
                CHECK(std::abs(Luma(p) - Luma(clear)) < 12);
            }

        CHECK(Arcane::RenderErrorCount() == before);
    }

    void CheckCubeOccludesTheGrid(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        // The editor's own defaults (settings S6-21): the desc's tunables are
        // zero until MakeGridScene fills them.
        Arcane::GridSceneDesc grid = Arcane::Editor::MakeGridScene(Arcane::Editor::EditorGridSettings{},
                                                                   Arcane::Editor::EditorGrid3DSettings{});
        grid.view = GridView();
        grid.SetPlane(Arcane::GridSceneDesc::Plane::XZ);

        // A 2 m red cube RESTING on the plane (y in [0, 2]) at the origin: the
        // centre ray hits its front (+Z) face before the ground.
        const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
        const Arcane::Guid cubeId{ 1, 1 };
        Arcane::MeshInstance one;
        one.mesh      = cubeId;
        one.model     = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        one.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        const Arcane::MeshInstance instances[] = { one };

        Arcane::MeshSceneDesc mesh;
        mesh.instances     = instances;
        mesh.view          = grid.view.view;
        mesh.projection    = grid.view.projection;
        mesh.lightDirection = glm::vec3(0.0f, 1.0f, 1.0f);
        mesh.lightColor    = glm::vec3(1.0f);
        mesh.ambient       = glm::vec3(0.2f);

        std::uint32_t w = 0, h = 0;
        const std::vector<unsigned char> px = CaptureGrid(
            backend, grid, &mesh,
            [cubeId, &cube](const Arcane::Guid& g) -> Arcane::NriMeshBufferCache::SupplyResult
            {
                if (g == cubeId)
                    return { &cube, Arcane::MeshResolveState::Ready };
                return { nullptr, Arcane::MeshResolveState::Failed };
            },
            w, h);
        REQUIRE(w == kW);
        REQUIRE(h == kH);

        // 3. THE CENTRE PIXEL IS THE CUBE'S, red and NOT blue: the Z axis
        //    line (0.3, 0.4, 0.9, alpha 0.9) runs straight through this
        //    column and would dominate the pixel if the grid ignored depth.
        const Rgba centre = At(px, w, w / 2u, h / 2u);
        CHECK(centre.r > centre.b + 60);
        CHECK(centre.r > centre.g + 60);

        // ...and the grid still drew OUTSIDE the cube: a pixel low in the
        //    frame, off to the side, below the cube's silhouette, is on the
        //    ground and some pixel in that column is a line.
        const Rgba clear = At(px, w, 0u, 0u);
        int bright = 0;
        for (std::uint32_t y = 3u * h / 4u; y < h; ++y)
            if (Luma(At(px, w, w / 8u, y)) > Luma(clear) + 30)
                ++bright;
        CHECK(bright > 0);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("grid: the 3D grid draws its lines on the ground and leaves the sky clear (d3d12)",
          "[gpu][pixel][grid][nri][d3d12]")
{
    CheckGridDrawsAndSkyStaysClear(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("grid: the 3D grid draws its lines on the ground and leaves the sky clear (vulkan)",
          "[gpu][pixel][grid][nri][vulkan]")
{
    CheckGridDrawsAndSkyStaysClear(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("grid: a cube on the plane occludes the grid -- the centre pixel is the cube's, "
          "not the axis line's (d3d12)", "[gpu][pixel][grid][nri][d3d12]")
{
    CheckCubeOccludesTheGrid(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("grid: a cube on the plane occludes the grid -- the centre pixel is the cube's, "
          "not the axis line's (vulkan)", "[gpu][pixel][grid][nri][vulkan]")
{
    CheckCubeOccludesTheGrid(Arcane::GraphicsBackend::Vulkan);
}
