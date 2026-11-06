#include <Arcane/ImGui/ImGuiNriNode.hpp>
#include <Arcane/ImGui/ImDrawSnapshot.hpp>   // CopyImDrawSnapshot / ImDrawSnapshotRebuild (ruling PR-1)
// NODE-LEVEL PIXEL CORRECTNESS for the NRI frame graph -- [gpu], outside the
// ~[gpu] baseline set.
//
// ===== WHY THIS FILE EXISTS ==================================================
// Without it the suite's entire [gpu] coverage is TWO cases, both in
// NriSubstrateTest.cpp, and both device-WRAP smokes: they prove a native device
// survives the trip into NRI and say nothing whatsoever about pixels.
//
// A whole-frame image comparison could only say THAT the frame changed, never
// WHICH node changed it. Node-level cases are what turn a pixel regression from
// a desk session into a test run, and that difference compounds with every pass
// the renderer gains.
//
// ===== WHAT IS ASSERTED EXACTLY, AND WHAT IS ASSERTED STRUCTURALLY ===========
// This distinction is deliberate and load-bearing, so it is stated up front.
//
//   * THE PICK ID PASS IS EXACT. Its output is an R32_UINT hit-proxy id, not a
//     colour: no tonemap, no colour space, no filtering. `k+1 for the k-th
//     drawable, 0 for background` is an integer contract and is asserted as
//     one. This is where the value of the file is concentrated, and it is
//     exactly reproducible.
//
//   * COLOUR IS ASSERTED STRUCTURALLY. Everything reaching ReadCapture has been
//     through AddTonemapNode, so the canvas clear ({0.02, 0.02, 0.04, 1.0},
//     Batch2DNode.cpp) does NOT arrive as those bytes and a literal expectation
//     would be pinning the tonemap curve by accident. These cases assert
//     relations that survive any sane curve -- "the drawn rect is far brighter
//     than the background", "red dominates green and blue there", "the pixel
//     outside the rect matches the pixel in the far corner". Pinning the curve
//     itself is a different job and does not belong in these cases.
//
// ===== WHY AN OFFSCREEN VEHICLE ==============================================
// NriGraphContext::CreateOffscreen builds the real graph -- same nodes, same
// barriers, same RenderFrame path -- with NO window and NO swapchain, and hands
// back pixels through ReadCapture(). So these run headless on a machine with a
// GPU, without the windowed-run hazard that makes host runs a desk-only affair
// (see the [gpu] note below). The editor already uses this exact vehicle for
// its viewport, so the code under test is the code that ships.
//
// EVERYTHING GOES THROUGH EXPORTED Arcane CLASSES, never a raw nri* call. This
// exe links its OWN static copy of NRI, so a bare nri* function here would run
// against a different function table than the device it was handed. Both
// NativeDeviceOwner and NriDevice are ARC_API, so `Wrap` executes inside
// ArcaneClient.dll and the device that comes back is the DLL's -- which is the
// one CreateOffscreen must be given. NriSubstrateTest.cpp's wrap smoke carries
// the same rule for the same reason.
//
// ===== [gpu] MEANS "NEEDS AN ADAPTER, AND IS OUTSIDE THE ~[gpu] BASELINE" ====
// The desk-only ban was RETIRED on 2026-08-31: these cases have since run
// clean on both backends (62002 assertions / 25 cases), on the same driver
// build the old note blamed. They render OFFSCREEN -- no window, no swapchain --
// so the windowed-present hazard that note described was never reachable from
// here. CI runs the full unfiltered suite (Jenkinsfile).
//
// `~[gpu]` survives as a BASELINE-COMPARABILITY convention so recorded figures
// stay comparable -- never cite it as a safety gate. Whether a machine can
// actually run these is now answered by ARC_REQUIRE_BACKEND, which SKIPS with a
// stated reason rather than silently omitting.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/FramePacing.hpp>          // FramesInFlight() -- the probe latency
#include <Arcane/Render/GpuSceneSync.hpp>         // GpuSceneSync / BuildGpuSceneFrame -- the registry-backed draw (F3 plan 1 T7)
#include <Arcane/Render/VisibilitySystem.hpp>     // BuildVisibleSet / VisibleSet -- what the indirect path draws from
#include <Arcane/Scene/BoundsSystem.hpp>          // WorldBounds for the GPU scene's rows
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>           // RegisterSceneComponents
#include <Arcane/Scene/SceneResources.hpp>        // MeshTable / MeshEntry
#include <Arcane/Scene/TransformSystems.hpp>      // TransformPropagationSystem
#include <Arcane/Render/PickEmit.hpp>             // PickDrawable -- the id pass's input
#include <Arcane/Render/RenderErrorLatch.hpp>     // the shared 0/0 latch every case guards
#include <Arcane/Render/RenderFramePacket.hpp>    // ImDrawSnapshot -- the packet's ImGui copy (F5 plan 1 Task 4)
#include <Arcane/Render/ExtractFrame.hpp>         // ExtractFrame / ExtractRequest -- the registered-material witness (F5 plan 1 Task 5)
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>         // PhysicsWorld2D, Detail::Physics2D::Adopt / Access -- the extracted overlay witness
#include <Arcane/Material/MaterialSource.hpp>     // BuildMaterialShaderSource / MaterialTemplateFile
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>    // kVsEntry / kPsEntry / kVsProfile / kPsProfile
#include <Arcane/Mesh/MeshBuilder.hpp>          // BuildCube -- the opaque pass's geometry
#include <Arcane/Render/Nri/BindlessTable.hpp>    // kInvalidSlot -- the four-cube bindless proof
#include <Arcane/Render/Nri/GpuScene.hpp>         // GpuScene -- the instance-buffer round trip (F3 plan 1 T6)
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>  // ColorSpace -- Textures() is read directly, case 9
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>   // MeshInstance / MeshSceneDesc
#include <Arcane/Scene/SceneCamera.hpp>           // PerspectiveProjection -- the camera under test
#include <Arcane/Scene/ViewTransform.hpp>         // Orthographic -- the world-space batch case (3b)
#include <Arcane/Scene/RenderViewSettings.hpp>

// Extensions/NRIHelper.h: HelperInterface::UploadData, used by the four-cube
// bindless proof's own MakeSolidTexture (mirrors MeshNode::CreateWhiteTexel's
// CreateCommittedTexture + UploadData + CreateTextureView sequence). Placed
// beside the other NRI-adjacent includes above, all of which already pull in
// <NRI.h> first internally (NriDevice.hpp's own first include) -- so by the
// time the preprocessor reaches this line the ERROR/windows.h ordering this
// tree's NRI headers care about (NriCommon.hpp) is already settled.
#include <Extensions/NRIHelper.h>

// F2b Task 11's own case (9): cooks a real BC7 artifact through the pipeline
// lib in-test and reads it back through THIS engine's ArtifactReader -- the
// SAME cross-lib byte-contract path NriTextureCacheArtifactTest.cpp's own
// "PART 2" exercises (its CookFlatBc7Artifact is this case's direct
// ancestor; this file cannot include that one's anonymous-namespace helpers,
// so they are mirrored here rather than shared).
#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/TextureImporter.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Assets/ArtifactReader.hpp>

#include <stb_image_write.h>

// The ImGui pool-chain case (the last section): it builds its OWN context and
// draws through the vehicle's host HUD node. No ERROR clash -- the NRI-first
// order above is already settled by here.
#include <imgui.h>

#include <Astra/Component/ComponentRegistry.hpp>  // the registry the T7 draws-and-culls case builds
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>           // lookAtRH, translate

#include <array>        // the world quad's four corners (case 3b)
#include <cstdint>
#include <cmath>        // std::lround, std::abs -- the extracted-overlay sample
#include <cstdlib>      // std::abs over the integer luma difference
#include <cstring>      // std::memcmp -- the byte-exact instance-row check
#include <filesystem>   // case 9's own temp artifact dir
#include <memory>
#include <optional>
#include <span>         // FrameDesc::pickables / ::selectedIds are spans
#include <system_error> // case 9's own temp-dir cleanup
#include <unordered_map> // the T7 case's MeshTable backing store
#include <vector>
#include <utility>      // std::move -- the extract witness's resource and material desc

#include "Helpers/GpuCapability.hpp"

namespace
{
    // The capture target. Small on purpose: every assertion below is about a
    // handful of named texels, and a smaller target makes a desk run cheap.
    // Not square, so a transposed x/y anywhere in the chain shows up as an
    // out-of-range read rather than a plausible-looking wrong pixel.
    constexpr std::uint32_t kW = 160;
    constexpr std::uint32_t kH = 96;

    // The whole vehicle, in destruction order. Declaration order IS teardown
    // order here and it matters: the graph context borrows the NriDevice, and
    // the NriDevice wraps the native one (contract item 15 -- NRI device first,
    // native second), so the members are declared in the order they are built
    // and the compiler-generated destructor unwinds them correctly.
    struct PixelVehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner>  native;
        std::unique_ptr<Arcane::NriDevice>          nri;
        std::unique_ptr<Arcane::NriGraphContext>    ctx;

        [[nodiscard]] bool Ok() const noexcept { return ctx != nullptr; }
    };

    // Builds device -> NRI wrap -> offscreen graph context. REQUIREs rather
    // than returning a failure, because every case below is meaningless without
    // one and a null-check cascade would just move the failure further from its
    // cause. A machine with no usable adapter fails here, loudly, at the first
    // step -- which is the correct place for "this desk cannot run [gpu]".
    PixelVehicle MakeVehicle(Arcane::GraphicsBackend backend,
                             const Arcane::NriGraphContext::NodeSet& nodes = {})
    {
        PixelVehicle v;

        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
#if defined(ARC_BUILD_DEBUG)
        // Mirror OffscreenVehicle::Create / NriGraphContext.cpp's windowed
        // creation half EXACTLY (same three flags, same Debug-only gate).
        // These [gpu][pixel] cases are node-level frame-graph tests -- they
        // exercise hand- and graph-derived barrier placement more directly
        // than anything else in the tree, which is precisely the defect
        // class Vulkan sync validation catches and core validation does not
        // (RenderDeviceDesc.hpp). Leaving this vehicle at RenderDeviceDesc's
        // defaults would make the best-placed cases in the suite for finding
        // barrier hazards the ones running with the least validation.
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;   // VK-only; see RenderDeviceDesc.hpp
#endif
        v.native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(v.native != nullptr);

        v.nri = Arcane::NriDevice::Wrap(*v.native);
        REQUIRE(v.nri != nullptr);

        Arcane::HostConfig cfg;
        cfg.backend = backend;
        // vsync is meaningless with nothing to present to; CreateOffscreen
        // ignores it and the surface-owned knobs. Left at its default rather
        // than set, so this does not imply otherwise.

        v.ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *v.nri, kW, kH, nodes);
        REQUIRE(v.ctx != nullptr);
        REQUIRE(v.ctx->IsOffscreen());
        REQUIRE(v.ctx->SurfaceWidth()  == kW);
        REQUIRE(v.ctx->SurfaceHeight() == kH);
        return v;
    }

    struct Rgba
    {
        std::uint8_t r = 0, g = 0, b = 0, a = 0;
    };

    // ReadCapture hands back TIGHT RGBA8 (it swizzles from BGRA itself when the
    // target resolved that way), so the row stride is exactly width*4 and the
    // channel order is display-referred RGBA whatever NRI picked.
    Rgba At(const std::vector<unsigned char>& rgba, std::uint32_t w,
            std::uint32_t x, std::uint32_t y)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
        REQUIRE(i + 3u < rgba.size());
        return Rgba{ rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3] };
    }

    [[nodiscard]] int Luma(const Rgba& p) noexcept
    {
        // Deliberately crude and integer: this is only ever used for "much
        // brighter than", never for a colour-accurate comparison.
        return static_cast<int>(p.r) + static_cast<int>(p.g) + static_cast<int>(p.b);
    }

    // Renders one frame and REQUIREs it actually rendered. Skipped is a real
    // outcome for a collapsed panel, but this vehicle is created at a fixed
    // non-zero extent and never resized, so Skipped here is a defect and must
    // not be tolerated into a pixel assertion.
    void RenderOne(Arcane::NriGraphContext& ctx,
                   const Arcane::NriGraphContext::FrameDesc& frame)
    {
        const auto outcome = ctx.RenderFrameOffscreen(frame);
        REQUIRE(outcome == Arcane::NriGraphContext::FrameOutcome::Presented);
    }

    // A batcher carrying one axis-aligned rect, ready to hand to FrameDesc.
    // Begin() opens the batch against the canvas extent; the graph's Batch2DNode
    // is what Drains it, so this must NOT drain it here.
    std::unique_ptr<Arcane::Batcher2D> BatchOneRect(glm::vec2 pos, glm::vec2 size,
                                                    glm::vec4 color)
    {
        auto batcher = Arcane::Batcher2D::Create();
        REQUIRE(batcher != nullptr);
        batcher->Begin(kW, kH);
        batcher->SetLayer(0, 0);
        batcher->Rect(pos, size, color);
        return batcher;
    }
}

// ---------------------------------------------------------------------------
// 1. THE VEHICLE ITSELF: a frame renders, captures, and comes back the size it
//    was asked for. Everything below depends on this, so it is its own case --
//    a failure here localises to the harness rather than to a node.
// ---------------------------------------------------------------------------
namespace
{
    void CheckCaptureRoundTrip(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();
        PixelVehicle v = MakeVehicle(backend);

        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        CHECK(w == kW);
        CHECK(h == kH);
        // TIGHT rgba8: exactly four bytes per texel, no row padding.
        CHECK(rgba.size() == static_cast<std::size_t>(kW) * kH * 4u);

        // The canvas clear is opaque ({..., 1.0}), so alpha must survive the
        // chain. A zero alpha here means the capture read an untouched buffer.
        const Rgba corner = At(rgba, w, 0, 0);
        CHECK(corner.a == 255);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pixel: an offscreen graph frame captures at the requested extent (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckCaptureRoundTrip(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: an offscreen graph frame captures at the requested extent (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckCaptureRoundTrip(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 2. ReadCapture REFUSES A FRAME THAT DID NOT ASK TO BE CAPTURED. This is the
//    contract that keeps a capture consumer from silently reading a stale
//    buffer -- the accessor's own comment states it, and nothing pinned it.
// ---------------------------------------------------------------------------
namespace
{
    void CheckCaptureRefusesWithoutACaptureFrame(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        PixelVehicle v = MakeVehicle(backend);

        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = false;                       // the whole point
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        // False, already logged. NOT a run failure -- the same exit-3 "capture
        // failed" class the caller is told not to escalate.
        CHECK_FALSE(v.ctx->ReadCapture(w, h, rgba));
    }
}

TEST_CASE("pixel: ReadCapture refuses when no capture node was recorded (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckCaptureRefusesWithoutACaptureFrame(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: ReadCapture refuses when no capture node was recorded (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckCaptureRefusesWithoutACaptureFrame(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 3. SPRITE/BATCHER PIXEL CORRECTNESS -- the coverage SpriteMaterialGpuTest and
//    MaterialGpuTest lost. A batched rect must land INSIDE its own canvas
//    rectangle and nowhere else.
//
//    Structural, not literal (see the file header): the rect is pure red at
//    full intensity against a near-black clear, so "red dominates" and "far
//    brighter than the corner" hold under any tonemap curve, while an exact
//    byte would be pinning the curve.
// ---------------------------------------------------------------------------
namespace
{
    void CheckBatchedRectLandsInItsRectangle(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();
        PixelVehicle v = MakeVehicle(backend);

        // A rect in the upper-left quadrant, inset from every edge so that
        // "inside" and "outside" samples are both unambiguous and no assertion
        // sits on the boundary texel (where filtering/rounding is a coin flip).
        constexpr float kX = 20.0f, kY = 16.0f, kWide = 40.0f, kTall = 24.0f;
        auto batcher = BatchOneRect(glm::vec2(kX, kY), glm::vec2(kWide, kTall),
                                    glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.batch   = batcher.get();
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));

        // Dead centre of the rect.
        const Rgba inside = At(rgba, w,
                               static_cast<std::uint32_t>(kX + kWide  * 0.5f),
                               static_cast<std::uint32_t>(kY + kTall * 0.5f));
        // Well outside it, and far from every edge of the canvas.
        const Rgba outside = At(rgba, w, kW - 10u, kH - 10u);

        // THE COLOUR: red dominates both other channels by a wide margin.
        CHECK(inside.r > inside.g + 60);
        CHECK(inside.r > inside.b + 60);
        // THE PLACEMENT: the rect is far brighter than the untouched clear, and
        // the outside sample is NOT -- which together are what "landed in its
        // own rectangle" means. Asserting only the first would pass a shader
        // that filled the whole canvas red.
        CHECK(Luma(inside) > Luma(outside) + 120);
        CHECK(outside.r < 96);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pixel: a batched rect lands inside its own canvas rectangle (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckBatchedRectLandsInItsRectangle(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: a batched rect lands inside its own canvas rectangle (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckBatchedRectLandsInItsRectangle(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 3b. THE WORLD-SPACE PATH (F4 plan 1, Task 4). A QuadWorld quad carries WORLD
//     vertices (metres, +Y up) and reaches clip space through the batch's
//     view-projection in the vertex shader; a Rect in the SAME frame stays on
//     the pixel path. One frame, both spans, so this is also the proof that the
//     per-span `worldSpace` root-constant re-issue lands: without it the second
//     span would be projected by the first span's mapping and miss its rectangle.
//
//     +Y UP IS ASSERTED: the world quad sits ABOVE the camera centre and must
//     land in the TOP half of the canvas. A shader (or a matrix) that mirrored
//     Y would put it in the bottom half, where the "inside" sample below reads
//     the clear.
//
//     THE CAMERA CENTRE IS OFF-ORIGIN (review round 1): with the centre at the
//     origin the ortho matrix is diagonal but for m[3][2], and every vertex has
//     z = 0, so a TRANSPOSED memcpy into the root constants renders pixel-
//     identical output and the shaders' "column-major, no transpose" claim
//     has no evidence. A translation makes the transpose visible: it moves
//     the translation into the w row (w = 1 + 0.5 x - 0.083 y here, negative
//     at every corner -> the quad is clipped away) and a matrix that merely
//     DROPPED the translation lands the quad 40 px left / 4 px up of where it
//     belongs -- a spot sampled below and required dark.
// ---------------------------------------------------------------------------
namespace
{
    void CheckWorldQuadAndScreenRectShareOneFrame(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();
        PixelVehicle v = MakeVehicle(backend);

        // The engine's own orthographic producer: half-height 4.8 m over 96 px
        // -> 10 px per metre, 16 m across, and the camera centred at world
        // (-4, 0.4) -- NOT the origin, so the view-projection carries a real
        // translation (see the banner: a transposed upload is invisible
        // without one). Pixel = (80 + (x + 4) * 10, 48 - (y - 0.4) * 10).
        constexpr glm::vec2 kCentre(-4.0f, 0.4f);
        const Arcane::ViewTransform view =
            Arcane::Ortho2DView(kCentre, 4.8f, glm::uvec2(kW, kH));

        auto batcher = Arcane::Batcher2D::Create();
        REQUIRE(batcher != nullptr);
        batcher->Begin(kW, kH);
        batcher->SetViewProjection(view.ViewProjection());
        batcher->SetLayer(0, 0);
        // World: x in [-6, -2], y in [1.6, 4] -> pixels x 60..100, y 12..36
        // (upper-middle; its centre is (80, 24)). Red. Without the camera's
        // translation it would sit at x 20..60, y 8..32 (centre (40, 20)).
        const std::array<glm::vec3, 4> corners{ glm::vec3(-6.0f, 4.0f, 0.0f),
                                                glm::vec3(-2.0f, 4.0f, 0.0f),
                                                glm::vec3(-2.0f, 1.6f, 0.0f),
                                                glm::vec3(-6.0f, 1.6f, 0.0f) };
        batcher->QuadWorld(Arcane::Batcher2D::kMaterialSprite, Arcane::Guid::Nil(), corners,
                           glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        // Screen: pixels x 100..140, y 56..80 (bottom-right quadrant). Green.
        constexpr float kSx = 100.0f, kSy = 56.0f, kSw = 40.0f, kSh = 24.0f;
        batcher->Rect(glm::vec2(kSx, kSy), glm::vec2(kSw, kSh), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));

        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.batch   = batcher.get();
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));

        const Rgba worldInside  = At(rgba, w, 80u, 24u);       // centre of the world quad
        const Rgba worldMirror  = At(rgba, w, 80u, kH - 24u);  // where a Y-mirrored quad would land (80, 72)
        const Rgba worldUntrans = At(rgba, w, 40u, 20u);       // where a translation-less matrix would land it
        const Rgba screenInside = At(rgba, w,
                                     static_cast<std::uint32_t>(kSx + kSw * 0.5f),
                                     static_cast<std::uint32_t>(kSy + kSh * 0.5f));
        const Rgba outside      = At(rgba, w, kW / 2u, kH / 2u);   // (80, 48): neither quad, no decoy

        // The world quad: red, bright, in the TOP half -- NOT mirrored, and
        // NOT where a matrix without the camera's translation would put it.
        CHECK(worldInside.r > worldInside.g + 60);
        CHECK(worldInside.r > worldInside.b + 60);
        CHECK(Luma(worldInside) > Luma(outside) + 120);
        CHECK(Luma(worldMirror) < Luma(outside) + 16);
        CHECK(Luma(worldUntrans) < Luma(outside) + 16);
        // The screen rect: green, bright, where the pixel path always put it.
        CHECK(screenInside.g > screenInside.r + 60);
        CHECK(screenInside.g > screenInside.b + 60);
        CHECK(Luma(screenInside) > Luma(outside) + 120);
        CHECK(outside.r < 96);
        CHECK(outside.g < 96);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pixel: a QuadWorld quad and a screen Rect land in their own rectangles in one frame (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckWorldQuadAndScreenRectShareOneFrame(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: a QuadWorld quad and a screen Rect land in their own rectangles in one frame (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckWorldQuadAndScreenRectShareOneFrame(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 4. THE PICK ID PASS -- the largest single loss (PickBufferTest), and the one
//    place in this file where the contract is an exact integer.
//
//    THE CONVENTION UNDER TEST: the k-th entry of FrameDesc::pickables IS
//    hit-proxy id k+1, and 0 is background. `PickDrawable::entity` is NOT read
//    by the id pass -- it is the host's own reverse-mapping label -- so these
//    cases need no registry and leave it default-constructed deliberately.
//
//    THE LATENCY IS PART OF THE CONTRACT: the readback rides
//    FramesInFlight() frames behind, so ProbeId() is nullopt until that
//    many frames have gone by. Rendering exactly FramesInFlight() + 1
//    frames and requiring a value on the last is what pins the latency at its
//    documented depth -- a regression that made it deeper would fail here
//    rather than silently return a stale id to a host.
// ---------------------------------------------------------------------------
namespace
{
    // A pixel-identity ORTHOGRAPHIC view over the vehicle's kW x kH canvas: world
    // (x, y, 0) lands on pixel (x, kH - y). The pick cases below author their
    // silhouettes in these units so the old canvas-pixel expectations read
    // through unchanged: a quad "at pixel (40, 24)" is a world quad centred on
    // (40, kH - 24).
    Arcane::ViewTransform PixelIdentityView()
    {
        return Arcane::Ortho2DView(glm::vec2(kW * 0.5f, kH * 0.5f), kH * 0.5f, { kW, kH });
    }

    Arcane::PickDrawable WorldQuadAtPixel(glm::vec2 pixelCentre, glm::vec2 halfPx)
    {
        Arcane::PickDrawable d;
        d.kind = Arcane::PickDrawable::Kind::Quad;
        const float cx = pixelCentre.x, cy = float(kH) - pixelCentre.y;
        d.corners = { glm::vec3(cx - halfPx.x, cy + halfPx.y, 0.0f), glm::vec3(cx + halfPx.x, cy + halfPx.y, 0.0f),
                      glm::vec3(cx + halfPx.x, cy - halfPx.y, 0.0f), glm::vec3(cx - halfPx.x, cy - halfPx.y, 0.0f) };
        return d;
    }

    // Renders the same pick frame repeatedly until the readback has had time to
    // land, then returns what the probe read. The drawables are WORLD-space
    // (F4 plan 2) and the frame's pickView is what puts them on pixels.
    std::optional<std::uint32_t> ProbeAt(PixelVehicle& v,
                                         std::span<const Arcane::PickDrawable> drawables,
                                         glm::ivec2 pixel,
                                         std::uint64_t ticket = 0)
    {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.pickOutline = true;          // declares the pick node + readback + JFA chain
        frame.pickables   = drawables;
        frame.pickView    = PixelIdentityView();
        frame.pickPixel   = pixel;
        frame.pickTicket  = ticket;

        for (std::uint32_t i = 0; i < Arcane::FramesInFlight(); ++i)
        {
            RenderOne(*v.ctx, frame);
            // Nothing has come back yet, by construction. Asserted rather than
            // assumed: this IS the latency contract.
            CHECK_FALSE(v.ctx->ProbeId().has_value());
        }
        RenderOne(*v.ctx, frame);
        return v.ctx->ProbeId();
    }

    void CheckPickIdPass(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.pickOutline = true;

        // Two well-separated quads. Index 0 -> id 1, index 1 -> id 2.
        const Arcane::PickDrawable a = WorldQuadAtPixel({ 40.0f, 24.0f }, { 16.0f, 10.0f });
        const Arcane::PickDrawable b = WorldQuadAtPixel({ 118.0f, 68.0f }, { 16.0f, 10.0f });

        const Arcane::PickDrawable drawables[] = { a, b };

        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            const auto id = ProbeAt(v, drawables, glm::ivec2(40, 24));
            REQUIRE(id.has_value());
            CHECK(*id == 1u);            // the 0th drawable IS id 1
        }
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            const auto id = ProbeAt(v, drawables, glm::ivec2(118, 68));
            REQUIRE(id.has_value());
            CHECK(*id == 2u);            // the 1st drawable IS id 2
        }
        {
            // Background. 0 is a legitimate VALUE, not a missing readback --
            // the accessor is explicit that the caller decides 0 means miss --
            // so this must come back engaged AND zero, and asserting both is
            // what separates "read background" from "read nothing".
            PixelVehicle v = MakeVehicle(backend, nodes);
            const auto id = ProbeAt(v, drawables, glm::ivec2(5, 90));
            REQUIRE(id.has_value());
            CHECK(*id == 0u);
        }

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pick: the id pass reads k+1 for the k-th drawable and 0 for background (d3d12)",
          "[gpu][pixel][pick][nri][d3d12]")
{
    CheckPickIdPass(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pick: the id pass reads k+1 for the k-th drawable and 0 for background (vulkan)",
          "[gpu][pixel][pick][nri][vulkan]")
{
    CheckPickIdPass(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 5. THE PROBE TICKET LABELS THE READBACK. A host that probes a different pixel
//    every frame cannot otherwise tell which request an answer belongs to, and
//    the editor's deferred pick depends on exactly this.
// ---------------------------------------------------------------------------
namespace
{
    void CheckProbeTicketRidesWithTheId(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        Arcane::NriGraphContext::NodeSet nodes;
        nodes.pickOutline = true;

        const Arcane::PickDrawable a = WorldQuadAtPixel({ 40.0f, 24.0f }, { 16.0f, 10.0f });
        const Arcane::PickDrawable drawables[] = { a };

        PixelVehicle v = MakeVehicle(backend, nodes);

        constexpr std::uint64_t kTicket = 4242u;
        const auto id = ProbeAt(v, drawables, glm::ivec2(40, 24), kTicket);
        REQUIRE(id.has_value());
        CHECK(*id == 1u);

        const auto result = v.ctx->ProbeResult();
        REQUIRE(result.has_value());
        CHECK(result->id     == 1u);
        CHECK(result->ticket == kTicket);   // the label the copy carried

        // IT DOES NOT SELF-CLEAR: reading twice gives the same answer, which is
        // the documented behaviour and the reason acting on it exactly once is
        // the host's job rather than the vehicle's.
        const auto again = v.ctx->ProbeResult();
        REQUIRE(again.has_value());
        CHECK(again->id     == 1u);
        CHECK(again->ticket == kTicket);
    }
}

TEST_CASE("pick: the readback carries the ticket its frame was armed with (d3d12)",
          "[gpu][pixel][pick][nri][d3d12]")
{
    CheckProbeTicketRidesWithTheId(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pick: the readback carries the ticket its frame was armed with (vulkan)",
          "[gpu][pixel][pick][nri][vulkan]")
{
    CheckProbeTicketRidesWithTheId(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 6. THE SELECTION-OUTLINE JFA -- SelectionOutlineTest's twelve cases were the
//    single largest block retired, and this is the property they existed for:
//    a SELECTED silhouette grows an outline, and an EMPTY selection grows none.
//
//    The two halves must be one case. "Outline pixels appeared" alone would
//    pass a chain that outlines everything; "no outline pixels" alone would
//    pass a chain that is simply switched off. Rendering the same scene twice,
//    differing ONLY in selectedIds, is what makes the difference attributable.
// ---------------------------------------------------------------------------
namespace
{
    // Renders one capture frame with the pick/outline chain armed and returns
    // the pixels. `selected` is the union of hit-proxy ids the outline traces.
    std::vector<unsigned char> CaptureWithOutline(
        PixelVehicle& v,
        std::span<const Arcane::PickDrawable> drawables,
        std::span<const std::uint32_t> selected,
        std::uint32_t& w, std::uint32_t& h)
    {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture     = true;
        frame.pickOutline = true;
        frame.pickables   = drawables;
        frame.pickView    = PixelIdentityView();
        frame.selectedIds = selected;
        // No hover: (-1,-1) is the "no hover" convention the outline seed
        // understands, so the ONLY thing that can produce an outline in these
        // frames is `selected`. Left unset would arm the config's probe pixel
        // and give the seed a cursor, which is precisely the confound.
        frame.hoverPixel  = glm::ivec2(-1, -1);
        RenderOne(*v.ctx, frame);

        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        return rgba;
    }

    // Counts texels that differ meaningfully between two captures of the same
    // scene. With only selectedIds varying, every difference IS outline.
    std::size_t DifferingTexels(const std::vector<unsigned char>& a,
                                const std::vector<unsigned char>& b,
                                std::uint32_t w, std::uint32_t h)
    {
        REQUIRE(a.size() == b.size());
        std::size_t n = 0;
        for (std::uint32_t y = 0; y < h; ++y)
        {
            for (std::uint32_t x = 0; x < w; ++x)
            {
                const Rgba pa = At(a, w, x, y);
                const Rgba pb = At(b, w, x, y);
                // A generous threshold: this counts OUTLINE, and an outline is
                // a strong colour, not a rounding difference between two runs.
                if (std::abs(Luma(pa) - Luma(pb)) > 40) ++n;
            }
        }
        return n;
    }

    void CheckSelectionOutlineTracesOnlyTheSelection(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.pickOutline = true;

        const Arcane::PickDrawable a = WorldQuadAtPixel({ 60.0f, 40.0f }, { 20.0f, 14.0f });
        const Arcane::PickDrawable drawables[] = { a };

        std::uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;

        // (a) NOTHING SELECTED: the chain is declared and runs, and discards
        //     every pixel. This is the Play-mode arming.
        std::vector<unsigned char> none;
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            none = CaptureWithOutline(v, drawables, {}, w0, h0);
        }

        // (b) THE ONE SILHOUETTE SELECTED (hit-proxy id 1 == drawables[0]).
        std::vector<unsigned char> one;
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            const std::uint32_t selected[] = { 1u };
            one = CaptureWithOutline(v, drawables, selected, w1, h1);
        }

        REQUIRE(w0 == w1);
        REQUIRE(h0 == h1);

        const std::size_t changed = DifferingTexels(none, one, w0, h0);

        // AN OUTLINE APPEARED. A border around a 40x28 silhouette is on the
        // order of a few hundred texels; the floor is deliberately far below
        // that so a thickness change does not fail the case -- the property is
        // "the selection is traced", not "the trace is N texels wide".
        CHECK(changed > 40u);

        // AND IT IS AN OUTLINE, NOT A FILL OR A FULL-SCREEN EFFECT. The
        // silhouette is 40x28 = 1120 texels of a 160x96 = 15360 canvas; an
        // outline plus its falloff must stay well under a fill of the whole
        // thing. Without this bound, a chain that tinted the entire canvas on
        // selection would pass the floor above.
        CHECK(changed < (static_cast<std::size_t>(w0) * h0) / 4u);

        CHECK(Arcane::RenderErrorCount() == before);
    }

    std::vector<unsigned char> CaptureBoundSelection(
        PixelVehicle& v, const Arcane::RenderFramePacket* packet,
        std::span<const Arcane::PickDrawable> drawables,
        std::span<const std::uint32_t> frameSelected,
        std::uint32_t& w, std::uint32_t& h)
    {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture     = true;
        frame.pickOutline = true;
        frame.pickables   = drawables;
        frame.pickView    = PixelIdentityView();
        frame.selectedIds = frameSelected;
        frame.hoverPixel  = glm::ivec2(-1, -1);
        if (packet)
            v.ctx->BindPacket(packet);
        RenderOne(*v.ctx, frame);
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        return rgba;
    }

    void CheckPacketSelectionDrawsTheOutline(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();
        Arcane::NriGraphContext::NodeSet nodes;
        nodes.pickOutline = true;

        const Arcane::PickDrawable quad = WorldQuadAtPixel({ 60.0f, 40.0f }, { 20.0f, 14.0f });
        Arcane::RenderFramePacket fromPacket;
        fromPacket.pickDrawables.push_back(quad);
        fromPacket.selectedIds = { 1u };
        Arcane::RenderFramePacket fromFrameIgnored;
        fromFrameIgnored.pickDrawables.push_back(quad);

        std::uint32_t wP = 0, hP = 0, wF = 0, hF = 0, wB = 0, hB = 0;
        std::vector<unsigned char> packetPx, framePx, baseline;
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            packetPx = CaptureBoundSelection(v, &fromPacket, fromPacket.pickDrawables, {}, wP, hP);
        }
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            const std::uint32_t selected[] = { 1u };
            framePx = CaptureBoundSelection(v, &fromFrameIgnored, fromFrameIgnored.pickDrawables, selected, wF, hF);
        }
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            const Arcane::PickDrawable drawables[] = { quad };
            baseline = CaptureBoundSelection(v, nullptr, drawables, {}, wB, hB);
        }
        REQUIRE((wP == wB && hP == hB && wF == wB && hF == hB));
        CHECK(DifferingTexels(packetPx, baseline, wB, hB) > 40u);
        CHECK(DifferingTexels(framePx, baseline, wB, hB) == 0u);
        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("outline: the JFA traces the selected silhouette and nothing when the selection is empty (d3d12)",
          "[gpu][pixel][outline][nri][d3d12]")
{
    CheckSelectionOutlineTracesOnlyTheSelection(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("outline: the JFA traces the selected silhouette and nothing when the selection is empty (vulkan)",
          "[gpu][pixel][outline][nri][vulkan]")
{
    CheckSelectionOutlineTracesOnlyTheSelection(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("packet selection draws the outline (d3d12)", "[gpu][pixel][pick][nri][d3d12]")
{
    CheckPacketSelectionDrawsTheOutline(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("packet selection draws the outline (vulkan)", "[gpu][pixel][pick][nri][vulkan]")
{
    CheckPacketSelectionDrawsTheOutline(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 7. THE SAME DECLARED FRAME IS BYTE-DETERMINISTIC. Two vehicles, same content,
//    same nodes -> identical pixels. Cheap, and it is the invariant every other
//    case in this file silently depends on: a comparison is only meaningful if
//    a re-run of the same frame reproduces itself exactly.
//
//    IT ALSO PINS THE FLAG SURFACE. `post`, `gameUi` and `imgui` are null here,
//    which is how a caller asks for the frame WITHOUT those passes (see
//    FrameDesc). If nulling them ever stopped meaning "omit the node", this
//    frame would grow passes and stop matching itself.
// ---------------------------------------------------------------------------
namespace
{
    void CheckFrameIsByteDeterministic(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        auto capture = [&](std::uint32_t& w, std::uint32_t& h)
        {
            PixelVehicle v = MakeVehicle(backend);
            auto batcher = BatchOneRect(glm::vec2(30.0f, 20.0f), glm::vec2(50.0f, 30.0f),
                                        glm::vec4(0.2f, 0.8f, 0.3f, 1.0f));
            Arcane::NriGraphContext::FrameDesc frame;
            frame.capture = true;
            frame.batch   = batcher.get();
            // post / gameUi / imgui all left null: no chain, no game HUD, no
            // host chrome. That IS the "batch slice" now.
            RenderOne(*v.ctx, frame);
            std::vector<unsigned char> rgba;
            REQUIRE(v.ctx->ReadCapture(w, h, rgba));
            return rgba;
        };

        std::uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;
        const std::vector<unsigned char> first  = capture(w0, h0);
        const std::vector<unsigned char> second = capture(w1, h1);

        REQUIRE(w0 == w1);
        REQUIRE(h0 == h1);
        // EXACT, not tolerant: two runs of the same nodes over the same content
        // on the same device are deterministic. A tolerance here would hide the
        // very drift the case exists to catch.
        CHECK(first == second);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pixel: the same declared frame reproduces itself byte-for-byte (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckFrameIsByteDeterministic(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: the same declared frame reproduces itself byte-for-byte (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckFrameIsByteDeterministic(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 8. THE OPAQUE MESH PASS (NRI Phase 4, Task 7) -- the first 3D content this
//    suite has ever put a pixel assertion on, and the ONLY place the DEPTH
//    TEST can be proven at all.
//
//    Everything below is structural, per the file header: the cubes are pure
//    single-channel colours under a white light, so "red dominates green" and
//    "the lit sample is far brighter than the ambient-only one" survive any
//    sane tonemap curve where a literal byte would be pinning it.
//
//    THE CAMERA is built here rather than through a Registry because
//    ActivePerspectiveSceneCamera needs one and these cases have no scene.
//    SceneCamera::PerspectiveProjection is an UNVALIDATED pure function (a zero
//    fov, aspectRatio <= 0 or nearZ >= farZ all silently produce NaN), so the
//    three preconditions it does not check are REQUIREd here before it is
//    called -- which is the same guard ActivePerspectiveSceneCamera applies,
//    stated at the only other call site in the tree.
// ---------------------------------------------------------------------------
namespace
{
    // Eye on +Z looking at the origin down -Z with +Y up -- the engine's
    // right-handed convention (SceneCamera.hpp, Task 5), and the vantage
    // MeshBuilder's CCW-from-outside winding is stated against.
    constexpr float kEyeZ        = 4.0f;
    constexpr float kFovYDegrees = 60.0f;
    constexpr float kNearZ       = 0.1f;
    constexpr float kFarZ        = 100.0f;

    // A flat ambient term big enough that an UNLIT surface is still clearly
    // above the canvas clear -- which is what lets the light's contribution be
    // asserted as a difference rather than as "the cube appeared".
    constexpr float kAmbient = 0.08f;

    void FillCamera(Arcane::MeshSceneDesc& scene)
    {
        const float aspect = static_cast<float>(kW) / static_cast<float>(kH);
        REQUIRE(aspect > 0.0f);
        REQUIRE(kNearZ > 0.0f);
        REQUIRE(kFarZ > kNearZ);
        REQUIRE(kFovYDegrees > 0.0f);

        scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, kEyeZ),
                                   glm::vec3(0.0f, 0.0f, 0.0f),
                                   glm::vec3(0.0f, 1.0f, 0.0f));
        scene.projection = Arcane::PerspectiveProjection(kFovYDegrees, aspect, kNearZ, kFarZ);

        // Pointing TOWARD the camera, so a cube's +Z face -- the one facing the
        // lens -- has N.L == 1 exactly. That makes the lit sample's value a
        // property of the lighting model rather than of the cube's orientation.
        scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
        scene.lightColor     = glm::vec3(1.0f, 1.0f, 1.0f);
        scene.ambient        = glm::vec3(kAmbient);
    }

    using MeshSupplyFn     = Arcane::NriMeshBufferCache::MeshSupplyFn;
    using MeshSupplyResult = Arcane::NriMeshBufferCache::SupplyResult;

    // The GUIDS are captured BY VALUE, the MeshData by reference, on purpose: a Guid
    // is two words, so copying it costs nothing and removes a dangling-reference
    // footgun from a helper other tests will copy. The MeshData is the big thing the
    // supply must hand back as a live pointer, and every caller keeps it alive across
    // the frames it drives.
    MeshSupplyFn SupplyOne(Arcane::Guid id, const Arcane::MeshData& data)
    {
        return [id, &data](const Arcane::Guid& g) -> MeshSupplyResult
        {
            if (g == id)
                return { &data, Arcane::MeshResolveState::Ready };
            return { nullptr, Arcane::MeshResolveState::Failed };
        };
    }

    MeshSupplyFn SupplyTwo(Arcane::Guid a, const Arcane::MeshData& da,
                           Arcane::Guid b, const Arcane::MeshData& db)
    {
        return [a, b, &da, &db](const Arcane::Guid& g) -> MeshSupplyResult
        {
            if (g == a)
                return { &da, Arcane::MeshResolveState::Ready };
            if (g == b)
                return { &db, Arcane::MeshResolveState::Ready };
            return { nullptr, Arcane::MeshResolveState::Failed };
        };
    }

    std::vector<unsigned char> CaptureMesh(Arcane::GraphicsBackend backend,
                                           const Arcane::MeshSceneDesc& scene,
                                           std::uint32_t& w, std::uint32_t& h,
                                           MeshSupplyFn supply)
    {
        PixelVehicle v = MakeVehicle(backend);
        v.ctx->SetMeshSupply(std::move(supply));

        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.mesh    = &scene;
        RenderOne(*v.ctx, frame);

        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        return rgba;
    }

    // ---- 8a: a lit cube covers the centre and the corners stay background ---
    void CheckMeshCubeCoversTheCentre(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        // 2 m on a side at the origin, seen from 4 m away through a 60-degree
        // lens: the visible height at the origin plane is 2*tan(30)*4 = 4.6 m,
        // so the cube covers the middle ~43% of the frame and leaves every
        // corner untouched.
        const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
        const Arcane::Guid cubeId{ 1, 1 };

        Arcane::MeshInstance one;
        one.mesh      = cubeId;
        one.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);   // pure red, so channels separate cleanly
        const Arcane::MeshInstance instances[] = { one };

        Arcane::MeshSceneDesc scene;
        scene.instances = instances;
        FillCamera(scene);

        std::uint32_t w = 0, h = 0;
        const std::vector<unsigned char> lit = CaptureMesh(backend, scene, w, h, SupplyOne(cubeId, cube));

        // The SAME scene with the directional light switched off. Only
        // `lightColor` differs, so every difference below IS the light.
        Arcane::MeshSceneDesc unlitScene = scene;
        unlitScene.lightColor = glm::vec3(0.0f);
        std::uint32_t uw = 0, uh = 0;
        const std::vector<unsigned char> unlit = CaptureMesh(backend, unlitScene, uw, uh, SupplyOne(cubeId, cube));
        REQUIRE(w == uw);
        REQUIRE(h == uh);

        const Rgba centre = At(lit,   w, w / 2u, h / 2u);
        const Rgba unlitC = At(unlit, w, w / 2u, h / 2u);
        const Rgba corner = At(lit,   w, 10u, 10u);
        // NOT named `far`: <windows.h> still #defines that (and `near`) to
        // nothing, so a local by either name is a syntax error here.
        const Rgba opposite = At(lit, w, w - 10u, h - 10u);

        // THE CUBE IS THERE, AND IT IS RED. Asserting the channel separation
        // as well as the brightness is what rules out "the whole canvas got
        // brighter" -- a clear-colour change would move all three together.
        CHECK(centre.r > centre.g + 60);
        CHECK(centre.r > centre.b + 60);
        CHECK(centre.r > corner.r + 120);
        CHECK(Luma(centre) > Luma(corner));

        // THE CORNERS ARE BACKGROUND, both of them -- the canvas clear, which
        // is near-black and blue-leaning (Batch2DNode's kCanvasClear).
        CHECK(corner.r < 96);
        CHECK(opposite.r < 96);
        CHECK(std::abs(Luma(corner) - Luma(opposite)) < 24);

        // THE LIGHT ACTUALLY LIT IT. Ambient alone still shows the cube (so
        // this is not "the mesh only renders when lit"), and the directional
        // term is a large addition on top -- which is the whole of the Lambert
        // model this pass implements.
        CHECK(unlitC.r > corner.r + 20);
        CHECK(centre.r > unlitC.r + 40);

        CHECK(Arcane::RenderErrorCount() == before);
    }

    // ---- 8b: THE DEPTH TEST -------------------------------------------------
    // The assertion nothing else in this suite can make. Two cubes, one NEARER
    // the camera and smaller on screen, one FARTHER and larger, in two
    // different submission orders:
    //
    //   * the centre pixel is the NEAR cube's colour in BOTH orders. Without a
    //     depth test the last-submitted instance wins, so one of the two orders
    //     would come back the far cube's colour -- which is exactly the failure
    //     this case exists to catch, and exactly what painter's order cannot
    //     be talked out of;
    //   * a pixel outside the near cube's silhouette but inside the far one's
    //     is the FAR cube's colour, which rules out the degenerate pass where
    //     the far cube simply never drew.
    //
    // Back-face culling cannot produce this result on its own: both cubes are
    // separate closed opaque solids, so culling says nothing about which of
    // the two owns a shared pixel.
    void CheckNearMeshOccludesFar(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        // Geometry chosen so the two silhouettes are nested with room to spare
        // at 160x96 (worked out against the projection above, in canvas px):
        //   near cube front face at z = +1.3 (2.7 m out) -> x in [71, 89]
        //   far  cube front face at z =  0.0 (4.0 m out) -> x in [49, 111]
        // so x = 96 on the centre row is unambiguously far-only, 7 px clear of
        // the near cube's edge and 15 px clear of the far cube's.
        const Arcane::MeshData nearCube = Arcane::BuildCube(0.6f);
        const Arcane::MeshData farCube  = Arcane::BuildCube(3.0f);
        const Arcane::Guid nearId{ 1, 1 };
        const Arcane::Guid farId{ 2, 2 };

        Arcane::MeshInstance nearInstance;
        nearInstance.mesh      = nearId;
        nearInstance.model     = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        nearInstance.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);   // RED = near

        Arcane::MeshInstance farInstance;
        farInstance.mesh      = farId;
        farInstance.model     = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.5f));
        farInstance.baseColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);    // GREEN = far

        const Arcane::MeshInstance nearFirst[] = { nearInstance, farInstance };
        const Arcane::MeshInstance farFirst[]  = { farInstance, nearInstance };

        Arcane::MeshSceneDesc nearFirstScene;
        nearFirstScene.instances = nearFirst;
        FillCamera(nearFirstScene);

        Arcane::MeshSceneDesc farFirstScene;
        farFirstScene.instances = farFirst;
        FillCamera(farFirstScene);

        std::uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;
        const std::vector<unsigned char> nearFirstPixels =
            CaptureMesh(backend, nearFirstScene, w0, h0, SupplyTwo(nearId, nearCube, farId, farCube));
        const std::vector<unsigned char> farFirstPixels =
            CaptureMesh(backend, farFirstScene, w1, h1, SupplyTwo(nearId, nearCube, farId, farCube));
        REQUIRE(w0 == w1);
        REQUIRE(h0 == h1);

        for (const std::vector<unsigned char>* pixels : { &nearFirstPixels, &farFirstPixels })
        {
            const Rgba centre = At(*pixels, w0, w0 / 2u, h0 / 2u);
            const Rgba onlyFar = At(*pixels, w0, 96u, h0 / 2u);
            const Rgba corner  = At(*pixels, w0, 10u, 10u);

            // THE OCCLUSION. Red wins the centre whichever order the two were
            // submitted in -- which is depth, and only depth.
            CHECK(centre.r > centre.g + 60);
            CHECK(centre.r > centre.b + 60);

            // ...and the far cube DID draw, outside the near one's silhouette.
            CHECK(onlyFar.g > onlyFar.r + 60);
            CHECK(onlyFar.g > onlyFar.b + 60);

            // ...and neither of them reached the corner.
            CHECK(corner.r < 96);
            CHECK(corner.g < 96);
        }

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("mesh: a lit cube covers the centre pixel and the corners stay background (d3d12)",
          "[gpu][pixel][mesh][nri][d3d12]")
{
    CheckMeshCubeCoversTheCentre(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("mesh: a lit cube covers the centre pixel and the corners stay background (vulkan)",
          "[gpu][pixel][mesh][nri][vulkan]")
{
    CheckMeshCubeCoversTheCentre(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("mesh: the nearer cube occludes the farther one whichever order they are submitted in "
          "(d3d12)", "[gpu][pixel][mesh][nri][d3d12]")
{
    CheckNearMeshOccludesFar(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("mesh: the nearer cube occludes the farther one whichever order they are submitted in "
          "(vulkan)", "[gpu][pixel][mesh][nri][vulkan]")
{
    CheckNearMeshOccludesFar(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 8b. MESH PICKING (F4 plan 2, spec s7.1) -- the id pass's SECOND pipeline.
//     Placed after the mesh cases because it borrows their supply helpers and
//     their nested-cube geometry.
// ---------------------------------------------------------------------------
namespace
{
    // THE PROPERTY THIS PLAN EXISTS FOR: a mesh drawable rasterises into the id
    // buffer through the frame's ViewTransform, depth-tested against the pick
    // pass's OWN depth -- the nearer of two overlapping cubes wins the centre
    // pixel whichever order they were emitted in, and a sprite quad under a
    // mesh loses the pixel to it (the main pass's order, reproduced).
    void CheckMeshPickThroughTheIdBuffer(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();
        Arcane::NriGraphContext::NodeSet nodes;
        nodes.pickOutline = true;

        const Arcane::MeshData nearCube = Arcane::BuildCube(0.6f);
        const Arcane::MeshData farCube  = Arcane::BuildCube(3.0f);
        const Arcane::Guid nearId{ 1, 1 }, farId{ 2, 2 };

        Arcane::ViewTransform view = Arcane::ViewTransform::Perspective(
            glm::vec3(0.0f, 0.0f, kEyeZ), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
            kFovYDegrees, { kW, kH }, kNearZ, kFarZ);

        Arcane::PickDrawable nearD; nearD.kind = Arcane::PickDrawable::Kind::Mesh; nearD.mesh = nearId;
        nearD.world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        Arcane::PickDrawable farD;  farD.kind  = Arcane::PickDrawable::Kind::Mesh; farD.mesh  = farId;
        farD.world  = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.5f));
        // A sprite quad covering the whole view at z = +2 (NEARER than both cubes):
        // depth-off 2D drawables never occlude a mesh, by the pass's order rule.
        Arcane::PickDrawable sprite; sprite.kind = Arcane::PickDrawable::Kind::Quad;
        sprite.corners = { glm::vec3(-10, 10, 2), glm::vec3(10, 10, 2), glm::vec3(10, -10, 2), glm::vec3(-10, -10, 2) };

        // Order 1: sprite(1), near(2), far(3). Order 2: sprite(1), far(2), near(3).
        const Arcane::PickDrawable order1[] = { sprite, nearD, farD };
        const Arcane::PickDrawable order2[] = { sprite, farD, nearD };

        auto probe = [&](std::span<const Arcane::PickDrawable> drawables, glm::ivec2 pixel)
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            v.ctx->SetMeshSupply(SupplyTwo(nearId, nearCube, farId, farCube));
            Arcane::NriGraphContext::FrameDesc frame;
            frame.pickOutline = true; frame.pickables = drawables; frame.pickPixel = pixel; frame.pickView = view;
            for (std::uint32_t i = 0; i < Arcane::FramesInFlight(); ++i) RenderOne(*v.ctx, frame);
            RenderOne(*v.ctx, frame);
            const auto id = v.ctx->ProbeId();
            REQUIRE(id.has_value());
            return *id;
        };

        // The centre pixel is the NEAR cube in both orders -- depth, not order.
        CHECK(probe(order1, glm::ivec2(kW / 2, kH / 2)) == 2u);
        CHECK(probe(order2, glm::ivec2(kW / 2, kH / 2)) == 3u);
        // x = 96 on the centre row is far-only (the mesh depth case's geometry).
        CHECK(probe(order1, glm::ivec2(96, kH / 2)) == 3u);
        // The corner is the SPRITE: no mesh there, and the sprite covers the view.
        CHECK(probe(order1, glm::ivec2(4, 4)) == 1u);
        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pick: a mesh drawable rasterises depth-tested into the id buffer, over any 2D silhouette (d3d12)",
          "[gpu][pixel][pick][nri][d3d12]")
{
    CheckMeshPickThroughTheIdBuffer(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pick: a mesh drawable rasterises depth-tested into the id buffer, over any 2D silhouette (vulkan)",
          "[gpu][pixel][pick][nri][vulkan]")
{
    CheckMeshPickThroughTheIdBuffer(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 9. THE NORMAL MATRIX (NRI Phase 4 Task 8, F2a) -- a non-uniformly-scaled
//    instance's normals must go through the INVERSE TRANSPOSE, not the upper
//    3x3 mesh.hlsl's vs_main applied before this task. MeshNodeTest.cpp pins
//    NormalMatrixFor's analytic answer device-lessly; this is the one place that
//    can prove the SHADER HALF actually consumes it.
//
//    A plain cube cannot exercise this at all: every one of its face normals
//    is AXIS-ALIGNED in local space, and an axis-aligned vector's DIRECTION
//    survives a diagonal scale identically whether or not you invert it first
//    (scaling a vector along its own single nonzero axis never turns it, only
//    stretches it) -- the naive upper-3x3 and the correct inverse transpose
//    agree on every cube face under a purely diagonal `model`. So this case
//    hand-builds a single flat quad with a deliberately OBLIQUE authored
//    normal (not aligned with the quad's own geometric plane) -- physically
//    unusual, but a MeshVertex's `normal` is just an attribute the shader
//    transforms and shades with; nothing checks it against the triangle's
//    winding, and isolating the shader's NORMAL-MATRIX MATH from its geometry
//    is exactly the point.
//
//    THE COMPARISON IS STRUCTURAL, per the file header: an UNSCALED reference
//    cube's front face (axis-aligned normal (0,0,1), so its shading is
//    correct under EITHER formula) lit by light=(0,0,1) gives a KNOWN-GOOD
//    N.L=1 rendering, independent of anything this task changed. The oblique
//    quad, scaled 8x non-uniformly along X, is lit by light = the CPU's own
//    NormalMatrixFor(model) applied to its authored normal -- i.e. the
//    direction the shader SHOULD compute if it is correct. If it is, N.L=1
//    there too and the two centre pixels should closely match. If the shader
//    had regressed to the upper-3x3 (this task's exact defect), the ACTUAL
//    shader normal would be a very different direction (dot product with the
//    intended one ~=0.25 at this scale ratio -- worked out below), so N.L
//    would drop to a fraction of 1 and the two centre pixels would visibly
//    diverge.
// ---------------------------------------------------------------------------
namespace
{
    // A flat quad in the Z=0 plane, 2m per side, CCW as seen from +Z (front-
    // facing toward this file's camera, which sits on +Z looking down -Z --
    // MeshBuilder.hpp's WINDING contract, worked out by hand for these four
    // vertices rather than inherited from a generator, because no generator
    // in MeshBuilder.hpp authors a normal independent of its geometry).
    //
    // THE NORMAL IS DELIBERATELY OBLIQUE: normalize(1,0,1), not the quad's
    // true geometric normal (0,0,1). See the section comment above for why an
    // axis-aligned normal (every cube face; this quad's own true geometric
    // one) cannot exercise this defect under a diagonal `model` at all.
    Arcane::MeshData BuildObliqueNormalQuad()
    {
        const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
        Arcane::MeshData quad;
        quad.vertices = {
            { glm::vec3(-1.0f, -1.0f, 0.0f), n, glm::vec2(0.0f, 0.0f) },
            { glm::vec3( 1.0f, -1.0f, 0.0f), n, glm::vec2(1.0f, 0.0f) },
            { glm::vec3( 1.0f,  1.0f, 0.0f), n, glm::vec2(1.0f, 1.0f) },
            { glm::vec3(-1.0f,  1.0f, 0.0f), n, glm::vec2(0.0f, 1.0f) },
        };
        quad.indices = { 0, 1, 2, 0, 2, 3 };
        return quad;
    }

    void CheckNonUniformScaleNormalMatchesAnalyticLambert(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        // ---- THE KNOWN-GOOD REFERENCE: an unscaled cube, axis-aligned
        //      normal, light dead-on. Correct under EITHER normal-transform
        //      formula, so it needs nothing from this task to be
        //      trustworthy. ----
        const Arcane::MeshData referenceCube = Arcane::BuildCube(2.0f);
        const Arcane::Guid referenceId{ 1, 1 };
        Arcane::MeshInstance referenceInstance;
        referenceInstance.mesh      = referenceId;
        referenceInstance.baseColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);   // white: pure N.L*light+ambient
        const Arcane::MeshInstance referenceInstances[] = { referenceInstance };

        Arcane::MeshSceneDesc referenceScene;
        referenceScene.instances = referenceInstances;
        FillCamera(referenceScene);   // lightDirection defaults to (0,0,1) -- exactly this face's normal

        std::uint32_t rw = 0, rh = 0;
        const std::vector<unsigned char> referencePixels =
            CaptureMesh(backend, referenceScene, rw, rh, SupplyOne(referenceId, referenceCube));
        const Rgba referenceCentre = At(referencePixels, rw, rw / 2u, rh / 2u);

        // ---- THE OBLIQUE, NON-UNIFORMLY-SCALED CASE. `model` scales X by 8x
        //      and leaves Y/Z alone -- extreme on purpose, so a regression to
        //      the upper-3x3 misses by a WIDE margin rather than a subtle
        //      one (worked out below, not just asserted). ----
        const Arcane::MeshData obliqueQuad = BuildObliqueNormalQuad();
        const glm::vec3 localNormal = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
        const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(8.0f, 1.0f, 1.0f));

        // THE LIGHT IS SET FROM THE SAME FUNCTION THE SHADER MUST MATCH. This
        // is deliberate, not circular: NormalMatrixFor is pinned analytically,
        // device-lessly, against hand-worked numbers in MeshNodeTest.cpp. What
        // THIS case adds is proof the shader's OWN computation (mesh.hlsl's
        // vs_main, run on a real GPU) agrees with the CPU's -- which no
        // device-less test can show.
        //
        //   correct = inverseTranspose(diag(8,1,1)) * normalize(1,0,1)
        //           = diag(1/8,1,1) * (0.7071, 0, 0.7071) = (0.0884, 0, 0.7071)
        //           normalized ~= (0.124, 0, 0.992)
        //   naive (upper 3x3) = diag(8,1,1) * (0.7071, 0, 0.7071) = (5.657, 0, 0.7071)
        //           normalized ~= (0.992, 0, 0.124)
        //   dot(correct, naive) ~= 0.246 -- so a shader still using the naive
        //   formula would land N.L ~= 0.25 here, not 1.0: a wide, unmistakable
        //   miss, not a rounding-sized one.
        const glm::vec3 correctNormal = glm::normalize(Arcane::NormalMatrixFor(model) * localNormal);

        const Arcane::Guid obliqueId{ 2, 2 };
        Arcane::MeshInstance obliqueInstance;
        obliqueInstance.mesh      = obliqueId;
        obliqueInstance.model     = model;
        obliqueInstance.baseColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        const Arcane::MeshInstance obliqueInstances[] = { obliqueInstance };

        Arcane::MeshSceneDesc obliqueScene;
        obliqueScene.instances      = obliqueInstances;
        FillCamera(obliqueScene);
        obliqueScene.lightDirection = correctNormal;   // aim the light at the CORRECT answer

        std::uint32_t ow = 0, oh = 0;
        const std::vector<unsigned char> obliquePixels =
            CaptureMesh(backend, obliqueScene, ow, oh, SupplyOne(obliqueId, obliqueQuad));
        const Rgba obliqueCentre = At(obliquePixels, ow, ow / 2u, oh / 2u);

        // ---- THE SAME OBLIQUE SCENE, AMBIENT ONLY -- proves the light is
        //      actually contributing something, so "matches the reference"
        //      cannot be explained away by "everything saturates to white
        //      regardless of the light". Mirrors CheckMeshCubeCoversTheCentre's
        //      own lit-vs-unlit idiom. ----
        Arcane::MeshSceneDesc obliqueUnlitScene = obliqueScene;
        obliqueUnlitScene.lightColor = glm::vec3(0.0f);
        std::uint32_t uw = 0, uh = 0;
        const std::vector<unsigned char> obliqueUnlitPixels =
            CaptureMesh(backend, obliqueUnlitScene, uw, uh, SupplyOne(obliqueId, obliqueQuad));
        REQUIRE(uw == ow);
        REQUIRE(uh == oh);
        const Rgba obliqueUnlitCentre = At(obliqueUnlitPixels, uw, uw / 2u, uh / 2u);

        REQUIRE(rw == ow);
        REQUIRE(rh == oh);

        // THE PROPERTY: the oblique instance's normal-matrix-corrected
        // shading matches the known-good reference's, closely -- both are
        // N.L=1 under the SAME ambient/light-color/albedo, so a CORRECT
        // shader lands these BIT-IDENTICAL (both N.L=1 -> linear 1.08 -> the
        // same tonemapped byte), making the true delta 0 and this margin free
        // to tighten. 12 still comfortably covers CPU/GPU float noise while
        // catching failures far short of the upper-3x3 regression this case
        // was designed for (N.L~=0.25, nowhere near this margin): an
        // all-identity normal matrix reaching the shader (e.g. Finding 2's
        // singular guard misfiring, or the push constants never arriving)
        // gives N.L=cos(angle between correctNormal and local +Z)~=0.789 ->
        // linear 0.869, whose tonemapped delta from the reference sits at or
        // just above 24 depending on the curve -- a margin of 24 could miss
        // that failure; 12 does not.
        CHECK(std::abs(Luma(referenceCentre) - Luma(obliqueCentre)) < 12);

        // AND THE LIGHT DEMONSTRABLY MATTERED: the lit oblique centre is far
        // brighter than its own ambient-only twin.
        CHECK(Luma(obliqueCentre) > Luma(obliqueUnlitCentre) + 60);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("mesh: a non-uniformly-scaled instance's lit-face brightness matches the analytic "
          "Lambert term (d3d12)", "[gpu][pixel][mesh][nri][d3d12]")
{
    CheckNonUniformScaleNormalMatchesAnalyticLambert(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("mesh: a non-uniformly-scaled instance's lit-face brightness matches the analytic "
          "Lambert term (vulkan)", "[gpu][pixel][mesh][nri][vulkan]")
{
    CheckNonUniformScaleNormalMatchesAnalyticLambert(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 10. THE BINDLESS MATERIAL TABLE (NRI Phase 4 Task 8 / F2b Task 10) -- FOUR
//     cubes, FOUR generated solid-colour textures, FOUR distinct material
//     slots, in ONE scene / ONE Record() call. This is the case that makes
//     bindless PROVEN rather than merely present: a binding bug that ignores
//     the per-instance index (a stale root constant, a wrong range write, an
//     off-by-one slot) makes every cube read the SAME texture -- or falls
//     back to the flat baseColor path -- and this fails; a correct
//     implementation makes each cube's centre pixel dominated by ITS OWN
//     texture's channel(s).
//
//     Reuses case 8's proven camera/geometry rather than inventing new
//     numbers: four SMALL cubes on the z=0 plane, offset in X/Y so their
//     on-screen footprints do not overlap, each instance's expected centre
//     pixel located by PROJECTING its world-space centre through the SAME
//     view/projection FillCamera built -- not a hand-derived pixel constant,
//     which is exactly the kind of magic number a later camera or geometry
//     change could silently desynchronise from reality. baseColor is left
//     WHITE (1,1,1,1) on every instance so a channel-dominance assertion is
//     entirely the BINDLESS TEXTURE's doing, never a tint faking it -- a
//     shader that wrongly fell back to the flat path shows up as an
//     undifferentiated white/grey cube, not an accidental channel match.
// ---------------------------------------------------------------------------
namespace
{
    // A 1x1 solid-colour RGBA8_UNORM texture + its SHADER_RESOURCE view,
    // created and uploaded DIRECTLY through the device -- the pixel-suite
    // way, mirroring MeshNode::CreateWhiteTexel's own CreateCommittedTexture
    // + HelperInterface::UploadData + CreateTextureView sequence
    // (MeshNode.cpp) rather than going through NriTextureCache, which this
    // proof deliberately does not exercise (Task 11's route, not Task 10's).
    //
    // OWNERSHIP: the CALLER's, both halves. MeshNode::AddMaterial takes
    // ownership of the VIEW only (BindlessTable::Add's own contract), never
    // the texture it names -- so the texture must be destroyed by the
    // caller, and only AFTER the vehicle holding the view has been torn
    // down (a view must not outlive the texture it names -- see this
    // case's own teardown below).
    struct SolidTexture
    {
        nri::Texture*    texture = nullptr;
        nri::Descriptor* view    = nullptr;
    };

    SolidTexture MakeSolidTexture(Arcane::NriDevice& device, std::uint8_t r, std::uint8_t g,
                                  std::uint8_t b, std::uint8_t a)
    {
        const nri::CoreInterface& core = device.Core();
        SolidTexture out;

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
        REQUIRE(out.texture != nullptr);

        nri::HelperInterface helper = {};
        REQUIRE(nriGetInterface(device.Device(), NRI_INTERFACE(nri::HelperInterface), &helper)
                == nri::Result::SUCCESS);

        // RGBA8_UNORM: byte 0 is R (this machine is little-endian, so the low
        // byte of this uint32 lands first in memory) -- the same packed-pixel
        // idiom MeshNode::CreateWhiteTexel uses (there, all four bytes 0xFF).
        const std::uint32_t pixel = (std::uint32_t)r | ((std::uint32_t)g << 8)
                                   | ((std::uint32_t)b << 16) | ((std::uint32_t)a << 24);
        nri::TextureSubresourceUploadDesc subresource = {};
        subresource.slices     = &pixel;
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
        REQUIRE(out.view != nullptr);
        return out;
    }

    // Projects `worldPos` through `view`/`projection` and returns its pixel
    // coordinate in a `width`x`height` target -- COMPUTED, not a hand-derived
    // constant, so a later camera or geometry change that shifts where a
    // cube lands on screen cannot silently desynchronise this case from
    // reality the way a magic pixel coordinate could.
    //
    // NDC +Y is the TOP of the target on both backends (MeshNode.cpp's
    // PipelineFor WINDING comment), hence the flip in the Y term below; NDC
    // X maps to pixel X with no flip.
    glm::ivec2 ProjectToPixel(const glm::mat4& view, const glm::mat4& projection,
                              const glm::vec3& worldPos, std::uint32_t width, std::uint32_t height)
    {
        const glm::vec4 clip = projection * view * glm::vec4(worldPos, 1.0f);
        REQUIRE(clip.w > 0.0f);   // in front of the camera -- a REQUIRE, not a CHECK: everything
                                  // below is meaningless against a point behind the eye
        const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
        const float u = (ndc.x + 1.0f) * 0.5f;
        const float v = (1.0f - ndc.y) * 0.5f;
        return glm::ivec2(static_cast<int>(u * static_cast<float>(width)),
                          static_cast<int>(v * static_cast<float>(height)));
    }

    void CheckBindlessTableIndexesFourDistinctMaterials(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        PixelVehicle v = MakeVehicle(backend);
        Arcane::MeshNode* meshNode = v.ctx->Mesh();
        REQUIRE(meshNode != nullptr);

        // FOUR generated solid-colour textures, created directly through the
        // device (not NriTextureCache -- Task 11's route, not this one's).
        // Channels chosen so each is unambiguously dominant against the
        // other three: pure red, pure green, pure blue, and cyan (green AND
        // blue both high, red low) rather than a second primary some other
        // texture's channel could plausibly be confused with.
        const SolidTexture red   = MakeSolidTexture(*v.nri, 255, 0,   0,   255);
        const SolidTexture green = MakeSolidTexture(*v.nri, 0,   255, 0,   255);
        const SolidTexture blue  = MakeSolidTexture(*v.nri, 0,   0,   255, 255);
        const SolidTexture cyan  = MakeSolidTexture(*v.nri, 0,   255, 255, 255);

        const std::uint32_t redSlot   = meshNode->AddMaterial(red.view);
        const std::uint32_t greenSlot = meshNode->AddMaterial(green.view);
        const std::uint32_t blueSlot  = meshNode->AddMaterial(blue.view);
        const std::uint32_t cyanSlot  = meshNode->AddMaterial(cyan.view);
        // FOUR DISTINCT slots -- the property the shader's indexing has to
        // preserve. Dense-from-0 is BindlessTable's own documented policy
        // (BindlessTable.hpp), so this also happens to pin the order this
        // node's table hands slots out in, but the assertions below do not
        // otherwise depend on the exact numbers.
        REQUIRE(redSlot   != Arcane::BindlessTable::kInvalidSlot);
        REQUIRE(greenSlot != Arcane::BindlessTable::kInvalidSlot);
        REQUIRE(blueSlot  != Arcane::BindlessTable::kInvalidSlot);
        REQUIRE(cyanSlot  != Arcane::BindlessTable::kInvalidSlot);
        CHECK(redSlot   != greenSlot);
        CHECK(redSlot   != blueSlot);
        CHECK(redSlot   != cyanSlot);
        CHECK(greenSlot != blueSlot);
        CHECK(greenSlot != cyanSlot);
        CHECK(blueSlot  != cyanSlot);

        // FOUR SMALL cubes, offset in X/Y from the origin so their on-screen
        // footprints do not overlap (worked out against case 8's own
        // derivation: at z=0 this camera's visible frame is ~7.67m wide by
        // ~4.6m tall) -- comfortably inside the frame, with clearance to
        // spare between neighbours along both axes.
        constexpr float kCubeSize = 1.0f;
        constexpr float kOffsetX  = 1.6f;
        constexpr float kOffsetY  = 0.9f;

        const Arcane::MeshData cube = Arcane::BuildCube(kCubeSize);

        const Arcane::Guid cubeId{ 1, 1 };
        const auto instanceAt = [&cube, cubeId](float x, float y, std::uint32_t slot)
        {
            Arcane::MeshInstance instance;
            instance.mesh         = cubeId;
            instance.model        = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f));
            instance.baseColor    = glm::vec4(1.0f);   // WHITE -- see this section's header comment
            instance.materialSlot = slot;
            return instance;
        };

        const Arcane::MeshInstance instances[] = {
            instanceAt(-kOffsetX,  kOffsetY, redSlot),     // top-left     -> red
            instanceAt( kOffsetX,  kOffsetY, greenSlot),   // top-right    -> green
            instanceAt(-kOffsetX, -kOffsetY, blueSlot),    // bottom-left  -> blue
            instanceAt( kOffsetX, -kOffsetY, cyanSlot),    // bottom-right -> cyan
        };

        Arcane::MeshSceneDesc scene;
        scene.instances = instances;
        FillCamera(scene);

        v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.mesh    = &scene;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> pixels;
        REQUIRE(v.ctx->ReadCapture(w, h, pixels));

        const auto centreOf = [&](float x, float y)
        {
            return ProjectToPixel(scene.view, scene.projection, glm::vec3(x, y, 0.0f), w, h);
        };
        const glm::ivec2 redPx   = centreOf(-kOffsetX,  kOffsetY);
        const glm::ivec2 greenPx = centreOf( kOffsetX,  kOffsetY);
        const glm::ivec2 bluePx  = centreOf(-kOffsetX, -kOffsetY);
        const glm::ivec2 cyanPx  = centreOf( kOffsetX, -kOffsetY);

        const Rgba redPixel   = At(pixels, w, (std::uint32_t)redPx.x,   (std::uint32_t)redPx.y);
        const Rgba greenPixel = At(pixels, w, (std::uint32_t)greenPx.x, (std::uint32_t)greenPx.y);
        const Rgba bluePixel  = At(pixels, w, (std::uint32_t)bluePx.x,  (std::uint32_t)bluePx.y);
        const Rgba cyanPixel  = At(pixels, w, (std::uint32_t)cyanPx.x,  (std::uint32_t)cyanPx.y);

        // EACH CUBE'S CENTRE IS DOMINATED BY ITS OWN TEXTURE'S CHANNEL(S) --
        // a binding bug that ignores the per-instance slot (reads the same
        // slot for every draw, or falls back to the flat WHITE baseColor
        // path for all four) makes these four indistinguishable and fails at
        // least three of the four blocks below.
        CHECK(redPixel.r > redPixel.g + 60);
        CHECK(redPixel.r > redPixel.b + 60);

        CHECK(greenPixel.g > greenPixel.r + 60);
        CHECK(greenPixel.g > greenPixel.b + 60);

        CHECK(bluePixel.b > bluePixel.r + 60);
        CHECK(bluePixel.b > bluePixel.g + 60);

        CHECK(cyanPixel.g > cyanPixel.r + 60);
        CHECK(cyanPixel.b > cyanPixel.r + 60);

        CHECK(Arcane::RenderErrorCount() == before);

        // ---- TEARDOWN, in the order the OWNERSHIP contract requires ----
        // The vehicle FIRST: ~NriGraphContext buries every node's objects
        // (including MeshNode::Release's burial of every bindless VIEW,
        // MeshNode.cpp) and then DRAINS the graveyard before returning (its
        // own "RUNS every burial above HERE rather than leaving it pending"
        // comment) -- so by the time this reset() call returns, the four
        // views above are genuinely destroyed, not merely queued. Only THEN
        // are the textures they named destroyed below: "a view must not
        // outlive the image it views" is not a Vulkan validation nicety
        // here, it is the actual hazard the wrong order would hit.
        v.ctx.reset();
        const nri::CoreInterface& core = v.nri->Core();
        core.DestroyTexture(red.texture);
        core.DestroyTexture(green.texture);
        core.DestroyTexture(blue.texture);
        core.DestroyTexture(cyan.texture);
    }
}

TEST_CASE("mesh: the bindless material table indexes four distinct textures by per-instance slot "
          "(d3d12)", "[gpu][pixel][mesh][bindless][nri][d3d12]")
{
    CheckBindlessTableIndexesFourDistinctMaterials(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("mesh: the bindless material table indexes four distinct textures by per-instance slot "
          "(vulkan)", "[gpu][pixel][mesh][bindless][nri][vulkan]")
{
    CheckBindlessTableIndexesFourDistinctMaterials(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 9. THE ARC'S PROOF (F2b Task 11) -- cooked albedo, end to end. A small
//    solid-colour PNG cooked through the REAL pipeline lib (bc7enc_rdo, the
//    same library arccook links -- exactly NriTextureCacheArtifactTest.cpp's
//    own "PART 2" proof), read back through THIS engine's ArtifactReader,
//    uploaded through NriTextureCache and Added to MeshNode's BindlessTable
//    via NriGraphContext::ResolveMeshAlbedoSlot -- the ACTUAL seam
//    SceneRenderResolver installs every frame (Host/SceneRenderResolver.cpp's
//    (1b) sweep -> MeshMaterialCache::Request -> Services::
//    resolveAlbedoSlot), not a raw AddMaterial call -- and a cube carrying
//    that slot renders the artifact's own colour. Case 8 above builds its
//    four textures directly through the device (MakeSolidTexture); this is
//    the one case that runs the WHOLE spine the arc exists to prove --
//    `.arcmat` parse -> ResolvedMeshMaterial -> the resolver -> NriTextureCache
//    -> BindlessTable -> MeshInstance.materialSlot -> the pixel -- in one
//    assertion.
// ---------------------------------------------------------------------------
namespace
{
    namespace fs = std::filesystem;

    fs::path AlbedoArtifactTempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_mesh_albedo_gpu_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    void CollectAlbedoPngBytes(void* ctx, void* data, int size)
    {
        auto* out = static_cast<std::vector<std::byte>*>(ctx);
        const auto* p = static_cast<std::byte*>(data);
        out->insert(out->end(), p, p + size);
    }

    // A FLAT single-colour PNG -- BC7 reproduces a solid-colour block
    // exactly (no gradient to lose), so this proof does not have to pin an
    // exact mip level, the same reasoning
    // NriTextureCacheArtifactTest.cpp's own EncodeFlatPng states in full.
    std::vector<std::byte> EncodeFlatAlbedoPng(int size, unsigned char r, unsigned char g,
                                                unsigned char b)
    {
        std::vector<unsigned char> rgba(static_cast<std::size_t>(size) * size * 4);
        for (std::size_t i = 0; i < rgba.size(); i += 4)
        {
            rgba[i + 0] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = 255;
        }
        std::vector<std::byte> out;
        REQUIRE(stbi_write_png_to_func(&CollectAlbedoPngBytes, &out, size, size, 4, rgba.data(),
                                        size * 4) != 0);
        return out;
    }

    // Cooks a flat-colour BC7 artifact through the REAL pipeline lib
    // (ImportTexture -> bc7enc_rdo -> WriteTextureArtifact, exactly what
    // arccook drives) and reads it back through ArtifactReader -- the same
    // cross-lib byte-contract path NriTextureCacheArtifactTest.cpp's own
    // CookFlatBc7Artifact exercises, carried one layer further downstream
    // here (through NriTextureCache AND the bindless table, not just the
    // texture cache alone).
    Arcane::LoadedClientArtifact CookFlatAlbedoArtifact(const fs::path& artifactPath,
                                                         const Arcane::Guid& guid,
                                                         unsigned char r, unsigned char g,
                                                         unsigned char b)
    {
        const std::vector<std::byte> src = EncodeFlatAlbedoPng(5, r, g, b);

        Arcane::AssetPipeline::TextureMetaSettings settings;
        settings.format       = Arcane::AssetPipeline::TextureMetaSettings::Format::Bc7;
        settings.srgb         = true;
        settings.generateMips = true;

        auto imported = Arcane::AssetPipeline::ImportTexture(src, guid, settings);
        REQUIRE(imported.has_value());
        REQUIRE(imported->desc.format == Arcane::AssetPipeline::ArtifactPixelFormat::BC7);

        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
            artifactPath, imported->desc, imported->payload, imported->thumbRgba));

        auto result = Arcane::ReadClientArtifact(artifactPath, src, guid);
        REQUIRE(result.refusal == Arcane::ArtifactRefusal::None);
        REQUIRE(result.artifact.has_value());
        return std::move(*result.artifact);
    }

    void CheckCookedAlbedoRendersThroughBindlessSlot(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        const fs::path dir = AlbedoArtifactTempDir("cooked_albedo");
        const Arcane::Guid albedoGuid = Arcane::Guid::Generate();
        // Magenta: red AND blue high, green low -- unambiguous against the
        // near-black canvas clear and against every other channel
        // combination this suite's mesh cases use.
        Arcane::LoadedClientArtifact artifact =
            CookFlatAlbedoArtifact(dir / "albedo.arcart", albedoGuid, 255, 0, 255);

        PixelVehicle v = MakeVehicle(backend);
        Arcane::MeshNode* meshNode = v.ctx->Mesh();
        REQUIRE(meshNode != nullptr);
        Arcane::NriTextureCache* textures = v.ctx->Textures();
        REQUIRE(textures != nullptr);

        // Task 7's own seam, installed exactly like
        // NriTextureCacheArtifactTest.cpp's GPU case: the content supply is
        // ARTIFACT-shaped, not raw pixels.
        v.ctx->SetArtifactSupply(
            [&](const Arcane::Guid& id) -> const Arcane::LoadedClientArtifact*
            {
                return id == albedoGuid ? &artifact : nullptr;
            });

        // THE SPINE: NriTextureCache resolve/view -> BindlessTable::Add,
        // through the ACTUAL device seam SceneRenderResolver wires every
        // frame (NriGraphContext::ResolveMeshAlbedoSlot) -- NOT a raw
        // meshNode->AddMaterial(view) call, which is case 8's route and
        // deliberately not this one's: this case's whole point is proving
        // the SEAM end to end, not re-proving BindlessTable's own mechanics.
        const std::uint32_t slot = v.ctx->ResolveMeshAlbedoSlot(albedoGuid);
        REQUIRE(slot != Arcane::BindlessTable::kInvalidSlot);
        // Memoized: a second ask for the same Guid returns the SAME slot
        // without re-touching the device (ResolveMeshAlbedoSlot's own doc
        // comment, NriGraphContext.hpp) -- the property that keeps N mesh
        // materials sharing one albedo cheap.
        CHECK(v.ctx->ResolveMeshAlbedoSlot(albedoGuid) == slot);

        const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
        const Arcane::Guid cubeId{ 1, 1 };
        Arcane::MeshInstance instance;
        instance.mesh         = cubeId;
        // WHITE -- the sampled albedo passes through unmultiplied
        // (mesh.hlsl's ps_main: albedo * baseColor, and 1.0 * x is exact).
        instance.baseColor    = glm::vec4(1.0f);
        instance.materialSlot = slot;
        const Arcane::MeshInstance instances[] = { instance };

        Arcane::MeshSceneDesc scene;
        scene.instances = instances;
        FillCamera(scene);

        v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.mesh    = &scene;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> pixels;
        REQUIRE(v.ctx->ReadCapture(w, h, pixels));

        const Rgba centre = At(pixels, w, w / 2u, h / 2u);
        const Rgba corner = At(pixels, w, 10u, 10u);

        // THE ARTIFACT'S OWN COLOUR: structural, not literal, per the file
        // header (the tonemap sits between the sampled texel and this
        // capture) -- magenta dominates the green channel on both counts.
        CHECK(centre.r > centre.g + 60);
        CHECK(centre.b > centre.g + 60);
        // ...and it is genuinely THERE, not a coincidence of the clear
        // colour: the cube is far brighter than the untouched corner.
        CHECK(Luma(centre) > Luma(corner) + 120);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("mesh: a cooked artifact resolves through NriTextureCache into a bindless slot "
          "and the cube renders its colour (d3d12)",
          "[gpu][pixel][mesh][bindless][nri][d3d12]")
{
    CheckCookedAlbedoRendersThroughBindlessSlot(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("mesh: a cooked artifact resolves through NriTextureCache into a bindless slot "
          "and the cube renders its colour (vulkan)",
          "[gpu][pixel][mesh][bindless][nri][vulkan]")
{
    CheckCookedAlbedoRendersThroughBindlessSlot(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 10. THE GPU SCENE'S DEVICE HALF (F3 plan 1 T6): staged GpuInstance rows land
//     BYTE-EXACT in the persistent instance buffer, through GpuSceneSyncNode's
//     upload-ring copies, and SURVIVE a growth -- the buffer doubling at
//     declaration time (GpuScene::Reserve) with the live rows copied old -> new
//     behind an explicit barrier at record time (GpuScene::Apply).
//
//     The frame carries a registry-backed scene (MeshSceneDesc::scene) with ONE
//     emitted batch and NO ad-hoc instances, so the mesh node is DECLARED
//     (reading the three imported buffers -- the copy -> read barriers the
//     validation layers judge) and records nothing but its depth clear (Task 7
//     rewrites the draw). The bytes come back through GpuScene's TEST-ONLY
//     readback: a HOST_READBACK buffer a Copy node after the mesh node fills
//     from the imported instances handle (the `pickreadback` idiom).
//
//     RenderErrorCount() is the validation gate: a missing barrier on the grown
//     buffer, or on the retired one the grow-copy reads, is a sync-validation /
//     debug-layer message -- and every one of those lands in the latch.
// ---------------------------------------------------------------------------
namespace
{
    // Every field non-default and distinct per seed, so a row copied to the
    // wrong offset, a partial copy, or a stale row from the previous frame all
    // fail the memcmp rather than passing on zeros.
    Arcane::GpuInstance MakeInstanceRow(float seed)
    {
        Arcane::GpuInstance v;
        v.model        = glm::translate(glm::mat4(1.0f), glm::vec3(seed, 2.0f * seed, 3.0f * seed));
        v.prevModel    = glm::translate(glm::mat4(1.0f), glm::vec3(-seed, seed, 0.5f * seed));
        v.normal0      = glm::vec4(seed, 0.1f, 0.2f, 0.3f);
        v.normal1      = glm::vec4(0.4f, seed, 0.5f, 0.6f);
        v.normal2      = glm::vec4(0.7f, 0.8f, seed, 0.9f);
        v.boundsMin    = glm::vec4(-seed, -2.0f * seed, -3.0f * seed, 0.0f);
        v.boundsMax    = glm::vec4(seed, 2.0f * seed, 3.0f * seed, 0.25f);
        v.baseColor    = glm::vec4(0.1f * seed, 0.2f * seed, 0.3f * seed, 1.0f);
        v.materialSlot = static_cast<std::uint32_t>(seed) * 7u;
        v.batch        = static_cast<std::uint32_t>(seed);
        v.flags        = Arcane::kGpuInstanceFlagTeleported;
        v.pad          = 0xA5A5A5A5u + static_cast<std::uint32_t>(seed);
        return v;
    }

    // One batch, whose args + visible indices ride the same sync node -- so the
    // args buffer's copy -> ARGUMENT_BUFFER/INDIRECT edge is judged too.
    void FillOneBatch(Arcane::GpuSceneFrame& frame, std::uint32_t rowCapacity,
                      std::span<const std::uint32_t> visibleRows)
    {
        frame.rowCount = rowCapacity;
        frame.visibleIndices.assign(rowCapacity, 0xFFFFFFFFu);
        for (std::size_t i = 0; i < visibleRows.size(); ++i)
            frame.visibleIndices[i] = visibleRows[i];
        Arcane::GpuBatchDraw batch;
        batch.mesh       = Arcane::Guid{ 1, 1 };
        batch.indexCount = 36;
        batch.capacity   = static_cast<std::uint32_t>(visibleRows.size());
        frame.batches.push_back(batch);
        frame.args.push_back(Arcane::DrawIndexedArgs{ 36u, static_cast<std::uint32_t>(visibleRows.size()), 0u, 0, 0u });
    }

    void CheckRowBytes(const std::vector<std::uint8_t>& bytes, std::uint32_t row, const Arcane::GpuInstance& expected)
    {
        constexpr std::size_t kRow = sizeof(Arcane::GpuInstance);
        INFO("instance row " << row);
        REQUIRE(bytes.size() >= (static_cast<std::size_t>(row) + 1u) * kRow);
        CHECK(std::memcmp(bytes.data() + static_cast<std::size_t>(row) * kRow, &expected, kRow) == 0);
    }

    void CheckGpuSceneRoundTrip(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();
        PixelVehicle v = MakeVehicle(backend);

        Arcane::GpuScene* device = v.ctx->Scene();
        REQUIRE(device != nullptr);
        // The latched render.gpuScene.initialRows (its 256 default in this process).
        const std::uint32_t kInitialRows = device->InitialRows();
        REQUIRE(device->RowCapacity() == kInitialRows);
        REQUIRE(device->EnableDebugReadback());
        const std::uint64_t generationBefore = device->InstanceBufferGeneration();
        CHECK(Arcane::GpuSceneSyncedGeneration(device) == 0u);
        CHECK(Arcane::GpuSceneSyncedGeneration(nullptr) == 0u);

        constexpr std::uint32_t kRowA = 0u;
        constexpr std::uint32_t kRowB = 5u;                               // NOT adjacent: per-row copies, not one span
        const std::uint32_t     kRowC = kInitialRows;   // the row that forces the growth
        const Arcane::GpuInstance rowA  = MakeInstanceRow(1.0f);
        const Arcane::GpuInstance rowB  = MakeInstanceRow(2.0f);
        const Arcane::GpuInstance rowB2 = MakeInstanceRow(4.0f);   // row B re-staged INSIDE the copied range, in the growth frame
        const Arcane::GpuInstance rowC  = MakeInstanceRow(3.0f);

        // ---- frame 1: two staged rows inside the initial capacity ----------
        {
            Arcane::GpuSceneFrame frame;
            frame.stage.rows        = { kRowA, kRowB };
            frame.stage.values      = { rowA, rowB };
            frame.stage.rowCapacity = kInitialRows;
            frame.stage.fullRebuild = true;
            frame.stage.generation  = 42u;
            const std::uint32_t visible[] = { kRowA, kRowB };
            FillOneBatch(frame, kInitialRows, visible);

            Arcane::MeshSceneDesc scene;
            scene.scene = &frame;
            FillCamera(scene);
            REQUIRE(scene.instances.empty());
            REQUIRE_FALSE(scene.Empty());

            Arcane::NriGraphContext::FrameDesc fd;
            fd.mesh = &scene;
            RenderOne(*v.ctx, fd);

            std::vector<std::uint8_t> bytes;
            REQUIRE(device->ReadDebugInstances(bytes));
            CHECK(bytes.size() == device->InstanceBytes());
            CheckRowBytes(bytes, kRowA, rowA);
            CheckRowBytes(bytes, kRowB, rowB);
            CHECK(device->RowCapacity() == kInitialRows);
            CHECK(device->InstanceBufferGeneration() == generationBefore);
            CHECK(device->SyncedGeneration() == 42u);
            CHECK(Arcane::GpuSceneSyncedGeneration(device) == 42u);
        }

        // ---- frame 2: GROWTH. The mirror's high water is kInitialRows + 1 and
        // the stage is NOT a full rebuild, so Reserve doubles the buffer and
        // Apply copies the live rows old -> new before writing the staged
        // rows. Row A must SURVIVE the move untouched; row B is RE-STAGED with
        // new bytes in the same frame -- a transfer write INSIDE the range the
        // grow-copy just wrote, the write-after-write the copy -> copy barrier
        // in Apply orders (sync validation would flag its absence); row C
        // lands past the old end.
        {
            Arcane::GpuSceneFrame frame;
            frame.stage.rows        = { kRowB, kRowC };
            frame.stage.values      = { rowB2, rowC };
            frame.stage.rowCapacity = kInitialRows + 1u;
            frame.stage.fullRebuild = false;
            frame.stage.generation  = 42u;
            const std::uint32_t visible[] = { kRowA, kRowB, kRowC };
            FillOneBatch(frame, kInitialRows + 1u, visible);

            Arcane::MeshSceneDesc scene;
            scene.scene = &frame;
            FillCamera(scene);

            Arcane::NriGraphContext::FrameDesc fd;
            fd.mesh = &scene;
            RenderOne(*v.ctx, fd);

            std::vector<std::uint8_t> bytes;
            REQUIRE(device->ReadDebugInstances(bytes));
            CHECK(device->RowCapacity() == 2u * kInitialRows);
            CHECK(bytes.size() == device->InstanceBytes());
            CheckRowBytes(bytes, kRowA, rowA);    // survived the grow-copy
            CheckRowBytes(bytes, kRowB, rowB2);   // the re-stage won over the grow-copy
            CheckRowBytes(bytes, kRowC, rowC);
            CHECK(device->InstanceBufferGeneration() == generationBefore + 1u);
            CHECK(device->SyncedGeneration() == 42u);
        }

        // ---- frame 3: a frame with NO mesh scene at all, so the retired
        // buffer's burial retires behind a real submit and the vehicle tears
        // down with nothing pending but what every frame leaves.
        {
            Arcane::NriGraphContext::FrameDesc fd;
            RenderOne(*v.ctx, fd);
        }

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("gpuscene: staged rows land byte-exact in the instance buffer and survive a growth (d3d12)",
          "[gpu][gpuscene][nri][d3d12]")
{
    CheckGpuSceneRoundTrip(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("gpuscene: staged rows land byte-exact in the instance buffer and survive a growth (vulkan)",
          "[gpu][gpuscene][nri][vulkan]")
{
    CheckGpuSceneRoundTrip(Arcane::GraphicsBackend::Vulkan);
}

// An Apply REFUSAL forgets the synced generation (F3 plan 1 review fix,
// Important #2). Before the fix a refused stage in steady state was lost for
// good: the host's next Sync saw synced == generation, and the dropped rows
// (a spawn, a material change -- only moved rows get the re-dirty) were never
// re-staged. The cheap forcing is a stage whose `rows` and `values` disagree,
// which CopyRows must refuse before it touches the ring (a plain ARC_ERROR:
// the 0/0 latch is not fed, and the mesh node draws nothing -- the frame
// carries no batches). The row that DID land in frame 1 must survive
// untouched, and the next frame's full-rebuild stage restores the stamp.
namespace
{
    void CheckGpuSceneRefusalForgetsTheSyncedGeneration(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();
        PixelVehicle v = MakeVehicle(backend);

        Arcane::GpuScene* device = v.ctx->Scene();
        REQUIRE(device != nullptr);
        const std::uint32_t kInitialRows = device->InitialRows();   // the latched render.gpuScene.initialRows
        REQUIRE(device->EnableDebugReadback());
        REQUIRE(device->SyncedGeneration() == 0u);

        constexpr std::uint32_t kRowA = 0u;
        const Arcane::GpuInstance rowA  = MakeInstanceRow(1.0f);
        const Arcane::GpuInstance rowA2 = MakeInstanceRow(7.0f);

        auto renderStage = [&](std::vector<std::uint32_t> rows, std::vector<Arcane::GpuInstance> values,
                               bool fullRebuild, std::uint64_t generation)
        {
            Arcane::GpuSceneFrame frame;
            frame.stage.rows        = std::move(rows);
            frame.stage.values      = std::move(values);
            frame.stage.rowCapacity = kInitialRows;
            frame.stage.fullRebuild = fullRebuild;
            frame.stage.generation  = generation;
            frame.rowCount          = kInitialRows;   // no batches: nothing draws
            Arcane::MeshSceneDesc scene;
            scene.scene = &frame;
            FillCamera(scene);
            REQUIRE_FALSE(scene.Empty());   // the stage alone keeps the sync node in the graph
            Arcane::NriGraphContext::FrameDesc fd;
            fd.mesh = &scene;
            RenderOne(*v.ctx, fd);
        };

        // Frame 1: a full rebuild lands and is acknowledged.
        renderStage({ kRowA }, { rowA }, /*fullRebuild*/ true, 42u);
        CHECK(device->SyncedGeneration() == 42u);

        // Frame 2: steady state, and the stage is malformed -- Apply refuses
        // and must FORGET the acknowledgement, or the next Sync (synced ==
        // generation) would stage nothing and the refused rows would be lost.
        renderStage({ kRowA }, { rowA2, rowA2 }, /*fullRebuild*/ false, 42u);
        CHECK(device->SyncedGeneration() == 0u);
        CHECK(Arcane::GpuSceneSyncedGeneration(device) == 0u);
        {
            std::vector<std::uint8_t> bytes;
            REQUIRE(device->ReadDebugInstances(bytes));
            CheckRowBytes(bytes, kRowA, rowA);   // the refusal wrote nothing
        }

        // Frame 3: what the host's GpuSceneSync does on 0 != generation -- a
        // full rebuild of every live row -- restores the stamp.
        renderStage({ kRowA }, { rowA2 }, /*fullRebuild*/ true, 42u);
        CHECK(device->SyncedGeneration() == 42u);
        {
            std::vector<std::uint8_t> bytes;
            REQUIRE(device->ReadDebugInstances(bytes));
            CheckRowBytes(bytes, kRowA, rowA2);
        }

        // Frame 4: no mesh scene, so the vehicle tears down with nothing pending.
        {
            Arcane::NriGraphContext::FrameDesc fd;
            RenderOne(*v.ctx, fd);
        }
        CHECK(Arcane::RenderErrorCount() == before);   // a refusal is an ARC_ERROR, not a latch error
    }
}

TEST_CASE("gpuscene: an Apply refusal resets the synced generation to 0 and the next full rebuild restores it (d3d12)",
          "[gpu][gpuscene][nri][d3d12]")
{
    CheckGpuSceneRefusalForgetsTheSyncedGeneration(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("gpuscene: an Apply refusal resets the synced generation to 0 and the next full rebuild restores it (vulkan)",
          "[gpu][gpuscene][nri][vulkan]")
{
    CheckGpuSceneRefusalForgetsTheSyncedGeneration(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 11. THE INDIRECT PATH DRAWS, AND DRAWS ONLY THE VISIBLE ROWS (F3 plan 1 T7):
//     a registry-backed cube (Transform + MeshRenderer -> TransformPropagation
//     -> Bounds -> BuildVisibleSet -> GpuSceneSync -> BuildGpuSceneFrame) goes
//     through GpuSceneSyncNode's copies and ONE CmdDrawIndexedIndirect whose
//     vertex shader reads row = g_VisibleIndices[firstOutput + SV_InstanceID].
//     Two cubes share the batch: one at the origin, one at (1000, 0, 0) that the
//     CPU coarse test rejects -- so the CPU's args carry instanceNum 0 (compute
//     is the sole counter writer, asserted below), the cull pass emits exactly
//     the origin cube's row, and the centre pixel is the origin cube, red and
//     lit.
//
//     Then a VisibleSet that admits ONLY the far cube: the instance buffer still
//     holds the origin cube's row (the stage is the same full rebuild), but the
//     visible-index list names only the far row, so the indirect draw's one
//     instance is off-screen and the centre is BACKGROUND. That is the pin that
//     the draw takes its rows from the visible-index list and not from the
//     instance buffer's order.
//
//     Finally an EMPTY VisibleSet (ruling R-D): nothing emitted, but the stage
//     still has rows, so the pass is declared (MeshSceneDesc::Empty() is false),
//     the rows upload, and the mesh node records only its depth clear -- no
//     draw, no error, centre background.
//
//     Every capture is a FRESH vehicle (CaptureMesh), so every frame is a full
//     rebuild against a device that synced nothing -- deviceSyncedGeneration 0.
// ---------------------------------------------------------------------------
namespace
{
    struct GpuSceneWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
        Astra::Entity root{};

        GpuSceneWorld()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
        }
        void AddMesh(Arcane::Guid id, const Arcane::MeshData& data)
        {
            Arcane::MeshEntry entry;
            entry.data   = data;
            entry.bounds = Arcane::ComputeMeshBounds(entry.data);
            entry.slots.push_back(Arcane::MeshSlot{});
            meshes.emplace(id, entry);
        }
        Astra::Entity Spawn(glm::vec3 pos, Arcane::Guid mesh)
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t;
            t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ mesh, Arcane::Guid{} });
            return e;
        }
        void Schedulers()
        {
            Arcane::TransformPropagationSystem{}(reg);
            Arcane::BoundsSystem{}(reg);
        }
    };

    // A refused registry stage must stop the registry-backed cull + mesh
    // consumers for THAT frame. Otherwise the sync node leaves the previous
    // slot buffers intact, mesh-cull dispatches with this frame's rowCount
    // against stale args/visible/cull-batch data, and MeshNode can draw a
    // ghost from the previous registry frame. Ad-hoc direct rows are
    // independent; this case has none, so the refused frame should be
    // background.
    void CheckGpuSceneApplyRefusalSuppressesRegistryDraws(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
        const Arcane::Guid cubeId{ 1, 1 };
        GpuSceneWorld w;
        w.AddMesh(cubeId, cube);
        w.Spawn(glm::vec3(0.0f), cubeId);
        w.Schedulers();

        Arcane::MeshSceneDesc scene;
        FillCamera(scene);
        Arcane::ViewTransform view;
        view.view       = scene.view;
        view.projection = scene.projection;
        view.viewport   = glm::uvec2{ kW, kH };

        Arcane::VisibleSet vis;
        Arcane::BuildVisibleSet(w.reg, view, vis);

        Arcane::GpuSceneMirror mirror;
        Arcane::GpuSceneFrame frame;
        Arcane::GpuSceneSync(w.reg, mirror, /*deviceSyncedGeneration*/ 0u, frame.stage);
        Arcane::BuildGpuSceneFrame(mirror, &vis, Arcane::MeshSectionRanges(w.reg.GetResource<Arcane::MeshTable>()), view, frame);
        REQUIRE(frame.stage.rows.size() == 1u);
        REQUIRE(frame.stage.values.size() == 1u);
        REQUIRE(frame.batches.size() == 1u);
        REQUIRE(frame.cullBatches.size() == 1u);
        REQUIRE(frame.args.size() == 1u);
        for (Arcane::GpuInstance& row : frame.stage.values)
            row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

        PixelVehicle v = MakeVehicle(backend);
        v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
        Arcane::GpuScene* device = v.ctx->Scene();
        REQUIRE(device != nullptr);

        scene.scene = &frame;
        Arcane::NriGraphContext::FrameDesc first;
        first.capture = true;
        first.mesh = &scene;
        RenderOne(*v.ctx, first);
        // Prime both frame slots, then reuse one. This exercises the per-frame
        // zero-args upload on a previously computed slot before the refusal
        // rotates to the other stale slot. Without execution-time readiness,
        // that refused slot draws the old row.
        RenderOne(*v.ctx, first);
        RenderOne(*v.ctx, first);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        const Rgba lit = At(rgba, width, width / 2u, height / 2u);
        const Rgba corner = At(rgba, width, 10u, 10u);
        CHECK(lit.r > lit.g + 60);
        CHECK(device->SyncedGeneration() == frame.stage.generation);

        Arcane::GpuSceneFrame refused = frame;
        refused.stage.values.push_back(frame.stage.values[0]);   // rows/values mismatch: Apply must refuse before uploads
        scene.scene = &refused;
        Arcane::NriGraphContext::FrameDesc second;
        second.capture = true;
        second.mesh = &scene;
        RenderOne(*v.ctx, second);
        rgba.clear();
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        const Rgba refusedCentre = At(rgba, width, width / 2u, height / 2u);
        CHECK(device->SyncedGeneration() == 0u);
        CHECK(refusedCentre.r < 96);
        CHECK(std::abs(Luma(refusedCentre) - Luma(corner)) < 24);

        CHECK(Arcane::RenderErrorCount() == before);
    }

    void CheckGpuSceneDrawsAndCulls(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
        const Arcane::Guid cubeId{ 1, 1 };
        GpuSceneWorld w;
        w.AddMesh(cubeId, cube);
        const Astra::Entity nearE = w.Spawn(glm::vec3(0.0f), cubeId);
        const Astra::Entity farE  = w.Spawn(glm::vec3(1000.0f, 0.0f, 0.0f), cubeId);   // outside the frustum
        w.Schedulers();

        // The camera the mesh pass renders with IS the camera the CPU coarse
        // test uses -- one ViewTransform built from FillCamera's matrices.
        Arcane::MeshSceneDesc scene;
        FillCamera(scene);
        Arcane::ViewTransform view;
        view.view       = scene.view;
        view.projection = scene.projection;
        view.viewport   = glm::uvec2{ kW, kH };

        Arcane::VisibleSet vis;
        Arcane::BuildVisibleSet(w.reg, view, vis);
        CHECK(vis.Contains(nearE));
        CHECK_FALSE(vis.Contains(farE));

        Arcane::GpuSceneMirror mirror;
        Arcane::GpuSceneFrame  frame;
        Arcane::GpuSceneSync(w.reg, mirror, /*deviceSyncedGeneration*/ 0u, frame.stage);
        Arcane::BuildGpuSceneFrame(mirror, &vis, Arcane::MeshSectionRanges(w.reg.GetResource<Arcane::MeshTable>()), view, frame);
        REQUIRE(frame.stage.fullRebuild);
        REQUIRE(frame.stage.rows.size() == 2);
        REQUIRE(frame.batches.size() == 1);
        REQUIRE(frame.args.size() == 1);
        CHECK(frame.stats.total == 2);
        CHECK(frame.stats.coarseVisible == 1);          // the (1000,0,0) cube is outside the frustum
        CHECK(frame.args[0].instanceNum == 0);           // compute is the sole counter writer
        CHECK(frame.args[0].indexNum == static_cast<std::uint32_t>(cube.indices.size()));
        CHECK(frame.batches[0].mesh == cubeId);
        REQUIRE(frame.cullBatches.size() == 1);
        CHECK(frame.cullBatches[0].emitted == 1u);
        CHECK(frame.cullBatches[0].capacity == 2u);
        const Arcane::GpuSceneMirror::Rows* nearRows = mirror.slots.TryGet(nearE);
        const Arcane::GpuSceneMirror::Rows* farRows  = mirror.slots.TryGet(farE);
        REQUIRE(nearRows != nullptr);
        REQUIRE(farRows != nullptr);
        CHECK(frame.visibleIndices[frame.batches[0].firstOutput] == 0xFFFFFFFFu);
        CHECK(frame.oracleVisibleIndices[frame.batches[0].firstOutput] == nearRows->first);

        // The rows are staged white (no material table) -- paint them red so the
        // channel assertions below separate the cube from the clear cleanly.
        for (Arcane::GpuInstance& row : frame.stage.values)
            row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

        scene.scene = &frame;
        REQUIRE(scene.instances.empty());
        REQUIRE_FALSE(scene.Empty());
        std::uint32_t w0 = 0, h0 = 0;
        const std::vector<unsigned char> lit = CaptureMesh(backend, scene, w0, h0, SupplyOne(cubeId, cube));
        const Rgba centre = At(lit, w0, w0 / 2u, h0 / 2u);
        const Rgba corner = At(lit, w0, 10u, 10u);
        CHECK(centre.r > centre.g + 60);
        CHECK(centre.r > centre.b + 60);
        CHECK(centre.r > corner.r + 120);
        CHECK(corner.r < 96);

        // ---- A single off-screen row whose batch is deliberately emitted by
        // the CPU coarse stage. The indirect args still start at zero; the
        // compute cull repeats the frustum test and leaves the draw empty.
        GpuSceneWorld offscreen;
        offscreen.AddMesh(cubeId, cube);
        const Astra::Entity offscreenE = offscreen.Spawn(glm::vec3(1000.0f, 0.0f, 0.0f), cubeId);
        offscreen.Schedulers();
        Arcane::GpuSceneMirror offscreenMirror;
        Arcane::GpuSceneFrame  offscreenFrame;
        Arcane::GpuSceneSync(offscreen.reg, offscreenMirror, /*deviceSyncedGeneration*/ 0u, offscreenFrame.stage);
        for (Arcane::GpuInstance& row : offscreenFrame.stage.values)
            row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        Arcane::VisibleSet coarseOffscreen;
        coarseOffscreen.view    = view;
        coarseOffscreen.frustum = vis.frustum;
        coarseOffscreen.Clear();
        const Arcane::WorldBounds* offscreenBounds = offscreen.reg.GetComponent<Arcane::WorldBounds>(offscreenE);
        REQUIRE(offscreenBounds != nullptr);
        coarseOffscreen.Insert(offscreenE, offscreenBounds->box, 0.0f);
        Arcane::GpuSceneFrame culled;
        Arcane::BuildGpuSceneFrame(offscreenMirror, &coarseOffscreen,
                                   Arcane::MeshSectionRanges(offscreen.reg.GetResource<Arcane::MeshTable>()), view, culled);
        REQUIRE(culled.batches.size() == 1);
        REQUIRE(culled.args.size() == 1);
        CHECK(culled.stats.coarseVisible == 1);
        CHECK(culled.args[0].instanceNum == 0);
        const Arcane::GpuSceneMirror::Rows* offscreenRows = offscreenMirror.slots.TryGet(offscreenE);
        REQUIRE(offscreenRows != nullptr);
        CHECK(culled.visibleIndices[culled.batches[0].firstOutput] == 0xFFFFFFFFu);
        CHECK(culled.oracleVisibleIndices[culled.batches[0].firstOutput] == offscreenRows->first);
        scene.scene = &culled;
        REQUIRE_FALSE(scene.Empty());
        std::uint32_t w1 = 0, h1 = 0;
        const std::vector<unsigned char> dark = CaptureMesh(backend, scene, w1, h1, SupplyOne(cubeId, cube));
        REQUIRE(w1 == w0);
        REQUIRE(h1 == h0);
        const Rgba centreDark = At(dark, w1, w1 / 2u, h1 / 2u);
        CHECK(centreDark.r < 96);
        CHECK(std::abs(Luma(centreDark) - Luma(corner)) < 24);   // background, same as a corner

        // ---- an EMPTY VisibleSet (R-D): nothing emitted, rows still staged.
        // Declared (not Empty()), uploaded, and the mesh node records only the
        // depth clear -- no draw, no error.
        Arcane::VisibleSet none;
        none.view    = view;
        none.frustum = vis.frustum;
        none.Clear();
        Arcane::GpuSceneFrame nothing;
        nothing.stage = frame.stage;
        Arcane::BuildGpuSceneFrame(mirror, &none, Arcane::MeshSectionRanges(w.reg.GetResource<Arcane::MeshTable>()), view, nothing);
        CHECK(nothing.batches.empty());
        CHECK(nothing.args.empty());
        CHECK_FALSE(nothing.HasDraws());
        scene.scene = &nothing;
        CHECK_FALSE(scene.Empty());   // R-D: staged rows keep the pass declared
        std::uint32_t w2 = 0, h2 = 0;
        const std::vector<unsigned char> empty = CaptureMesh(backend, scene, w2, h2, SupplyOne(cubeId, cube));
        const Rgba centreEmpty = At(empty, w2, w2 / 2u, h2 / 2u);
        CHECK(centreEmpty.r < 96);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("gpuscene: a registry-backed cube draws through the indirect path and a culled one does not (d3d12)",
          "[gpu][gpuscene][mesh][nri][d3d12]")
{
    CheckGpuSceneDrawsAndCulls(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("gpuscene: a registry-backed cube draws through the indirect path and a culled one does not (vulkan)",
          "[gpu][gpuscene][mesh][nri][vulkan]")
{
    CheckGpuSceneDrawsAndCulls(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("gpuscene: an Apply refusal suppresses registry cull and mesh draws for that frame (d3d12)",
          "[gpu][gpuscene][mesh][nri][d3d12]")
{
    CheckGpuSceneApplyRefusalSuppressesRegistryDraws(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("gpuscene: an Apply refusal suppresses registry cull and mesh draws for that frame (vulkan)",
          "[gpu][gpuscene][mesh][nri][vulkan]")
{
    CheckGpuSceneApplyRefusalSuppressesRegistryDraws(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// THE IMGUI POOL CHAIN, on a device. The desk crash: the editor's chrome drew
// more than 32 distinct ImGui textures (29 asset thumbnails + the font atlas +
// previews), ImGuiNri's single 32-set descriptor pool refused the rest, and a
// font atlas that then needed a set kept an Invalid TexID -- so the next draw
// naming it asserted in ImDrawCmd::GetTexID (imgui.h) and aborted the editor.
//
// This case reproduces THE DESK ORDER: 100 user textures take their sets FIRST
// (pre-warmed through EnsureUserTexture, the same EnsureEntry a draw makes),
// and only then does frame 1 ask for the atlas. Under the old cap the atlas
// could not register and the frame asserted; with the chain it lands in a
// later link. The draws then span SEVERAL links in one pass, which is what
// exercises the per-draw CmdSetDescriptorPool re-bind (SetDescriptorHeaps on
// D3D12) -- and the readback proves each cell sampled ITS OWN texture, i.e.
// every draw was bound, not merely recorded.
// ---------------------------------------------------------------------------
namespace
{
    void CheckImGuiDrawsPastOnePool(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        // OUR OWN context, created BEFORE the vehicle so the HUD node's Init
        // installs the backend flags on it (and adopted explicitly below).
        // Restored at the end: Catch2 runs cases in random order.
        ImGuiContext* const previous = ImGui::GetCurrentContext();
        ImGuiContext* const imgui    = ImGui::CreateContext();
        REQUIRE(imgui != nullptr);
        ImGui::SetCurrentContext(imgui);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2((float)kW, (float)kH);
        io.DeltaTime   = 1.0f / 60.0f;

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.hostHud = true;
        PixelVehicle v = MakeVehicle(backend, nodes);
        Arcane::ImGuiNriNode* const hud = dynamic_cast<Arcane::ImGuiNriNode*>(v.ctx->ImGuiHud());
        REQUIRE(hud != nullptr);
        hud->AdoptImGuiContext(imgui);
        Arcane::ImGuiNri& renderer = hud->Renderer();
        REQUIRE(renderer.PoolCount() == 1);

        // 100 distinct solid colours, one per 16x9 cell of the 160x96 target.
        constexpr std::uint32_t kCols  = 10;
        constexpr std::uint32_t kCount = 100;
        constexpr std::uint32_t kCellW = kW / kCols;   // 16
        constexpr std::uint32_t kCellH = kH / 10u;     // 9
        static_assert(kCount > 32u, "must exceed the old single-pool cap");
        REQUIRE(kCount > renderer.LinkCapacity(0));   // must spill past the chain's first link

        const auto colourOf = [](std::uint32_t i)
        {
            return Rgba{ (std::uint8_t)(40u + i * 2u), (std::uint8_t)(250u - i * 2u),
                         (std::uint8_t)((i % 10u) * 25u), 255 };
        };

        std::vector<SolidTexture> textures;
        textures.reserve(kCount);
        for (std::uint32_t i = 0; i < kCount; ++i)
        {
            const Rgba c = colourOf(i);
            textures.push_back(MakeSolidTexture(*v.nri, c.r, c.g, c.b, c.a));
        }

        // THE DESK ORDER: every user texture registered before the atlas.
        for (const SolidTexture& t : textures)
            REQUIRE(renderer.EnsureUserTexture(t.texture));
        CHECK(renderer.PoolCount() >= 2);
        CHECK(Arcane::RenderErrorCount() == before);

        // FramesInFlight() + 1 frames, so a steady-state frame (the
        // atlas already registered, every set reused) is recorded too; the
        // last one is captured.
        const std::uint32_t frames = Arcane::FramesInFlight() + 1u;
        for (std::uint32_t frameIndex = 0; frameIndex < frames; ++frameIndex)
        {
            ImGui::NewFrame();
            ImDrawList* const draw = ImGui::GetForegroundDrawList();
            for (std::uint32_t i = 0; i < kCount; ++i)
            {
                const float x0 = (float)((i % kCols) * kCellW);
                const float y0 = (float)((i / kCols) * kCellH);
                draw->AddImage((ImTextureID)(intptr_t)textures[i].texture, ImVec2(x0, y0),
                               ImVec2(x0 + (float)kCellW, y0 + (float)kCellH));
            }
            // The ATLAS, after every user texture -- top-left, over cells
            // 0-2 and 10-12, which the pixel checks below avoid.
            draw->AddText(ImVec2(2.0f, 2.0f), IM_COL32_WHITE, "imgui");
            ImGui::Render();

            Arcane::NriGraphContext::FrameDesc frame;
            frame.imgui   = ImGui::GetDrawData();
            frame.capture = frameIndex + 1u == frames;
            RenderOne(*v.ctx, frame);
        }

        // EVERY ImGui-managed texture (the atlas) registered: a valid TexID and
        // status OK -- the state whose absence made GetTexID assert.
        const ImVector<ImTextureData*>& managed = ImGui::GetPlatformIO().Textures;
        REQUIRE(managed.Size >= 1);
        for (const ImTextureData* tex : managed)
        {
            CHECK(tex->TexID != ImTextureID_Invalid);
            CHECK(tex->Status == ImTextureStatus_OK);
        }
        CHECK(renderer.LiveTextureCount() == (std::size_t)kCount + (std::size_t)managed.Size);
        CHECK(renderer.PoolCount() >= 2);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        REQUIRE(w == kW);
        REQUIRE(h == kH);

        // Cells from the FIRST link (5), past it (70, 99) and the middle --
        // each must carry its own texture's colour. A draw bound to the wrong
        // link's heap, or left on a stale set, fails these.
        for (const std::uint32_t i : { 5u, 37u, 55u, 70u, 99u })
        {
            const std::uint32_t cx = (i % kCols) * kCellW + kCellW / 2u;
            const std::uint32_t cy = (i / kCols) * kCellH + kCellH / 2u;
            const Rgba got  = At(rgba, w, cx, cy);
            const Rgba want = colourOf(i);
            INFO("cell " << i << " at (" << cx << ", " << cy << ")");
            CHECK(std::abs((int)got.r - (int)want.r) <= 2);
            CHECK(std::abs((int)got.g - (int)want.g) <= 2);
            CHECK(std::abs((int)got.b - (int)want.b) <= 2);
        }

        CHECK(Arcane::RenderErrorCount() == before);

        // ---- TEARDOWN: the vehicle first (it buries + drains every view the
        // HUD built over these textures, and walks the adopted ImGui context,
        // which must therefore still be alive), then our views and textures,
        // then the context.
        v.ctx.reset();
        const nri::CoreInterface& core = v.nri->Core();
        for (const SolidTexture& t : textures)
        {
            core.DestroyDescriptor(t.view);
            core.DestroyTexture(t.texture);
        }
        ImGui::DestroyContext(imgui);
        ImGui::SetCurrentContext(previous);
    }
}

TEST_CASE("imgui: 100 concurrent textures grow the HUD's descriptor-pool chain and every draw "
          "samples its own texture (d3d12)", "[gpu][pixel][imgui][nri][d3d12]")
{
    CheckImGuiDrawsPastOnePool(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("imgui: 100 concurrent textures grow the HUD's descriptor-pool chain and every draw "
          "samples its own texture (vulkan)", "[gpu][pixel][imgui][nri][vulkan]")
{
    CheckImGuiDrawsPastOnePool(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// F5 plan 1 Task 4 (ruling PR-1): a packet's ImDrawSnapshot, rebuilt into an
// ImDrawData, reaches ImGuiNri -- the rebuild carries Textures, declaration
// services the atlas before record, and the red rect composites after tonemap.
// ---------------------------------------------------------------------------
namespace
{
    void CheckImGuiSnapshotReachesRenderer(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        ImGuiContext* const previous = ImGui::GetCurrentContext();
        ImGuiContext* const imgui = ImGui::CreateContext();
        REQUIRE(imgui != nullptr);
        ImGui::SetCurrentContext(imgui);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
        io.DeltaTime = 1.0f / 60.0f;

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.hostHud = true;
        PixelVehicle v = MakeVehicle(backend, nodes);
        Arcane::RenderOverlayNode* const hud = v.ctx->ImGuiHud();
        REQUIRE(hud != nullptr);
        hud->AdoptImGuiContext(imgui);

        ImGui::NewFrame();
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(70.0f, 40.0f), ImVec2(90.0f, 56.0f), IM_COL32(255, 0, 0, 255));
        ImGui::Render();

        Arcane::ImDrawSnapshot snap;
        REQUIRE(Arcane::CopyImDrawSnapshot(ImGui::GetDrawData(), snap));
        REQUIRE(snap.hasTextures);
        Arcane::ImDrawSnapshotRebuild rebuilt(snap);
        REQUIRE(rebuilt.Data()->Textures != nullptr);

        Arcane::NriGraphContext::FrameDesc frame;
        frame.imgui = rebuilt.Data();
        frame.capture = true;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        REQUIRE(w == kW);
        REQUIRE(h == kH);
        const Rgba px = At(rgba, w, 80u, 48u);
        CHECK(px.r > px.g + 60);
        CHECK(px.r > px.b + 60);
        CHECK(Arcane::RenderErrorCount() == before);

        v.ctx.reset();
        ImGui::DestroyContext(imgui);
        ImGui::SetCurrentContext(previous);
    }
}

TEST_CASE("an ImDrawSnapshot rebuild reaches ImGuiNri (d3d12)",
          "[gpu][packet][imgui][nri][d3d12]")
{
    CheckImGuiSnapshotReachesRenderer(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("an ImDrawSnapshot rebuild reaches ImGuiNri (vulkan)",
          "[gpu][packet][imgui][nri][vulkan]")
{
    CheckImGuiSnapshotReachesRenderer(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// F5 plan 1 Task 10: a bound packet's gameUi snapshot is what the gameui
// node draws. The live FrameDesc::gameUi is empty and only declares the node.
// ---------------------------------------------------------------------------
namespace
{
    void CheckPacketGameUiReachesTheHud(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        ImGuiContext* const previous = ImGui::GetCurrentContext();
        ImGuiContext* const game = ImGui::CreateContext();
        REQUIRE(game != nullptr);
        ImGui::SetCurrentContext(game);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
        io.DeltaTime = 1.0f / 60.0f;

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.gameUi = true;
        PixelVehicle v = MakeVehicle(backend, nodes);
        Arcane::RenderOverlayNode* const hud = v.ctx->ImGuiGame();
        REQUIRE(hud != nullptr);
        hud->AdoptImGuiContext(game);

        // The Play HUD the editor built before extract.
        ImGui::NewFrame();
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(70.0f, 40.0f), ImVec2(90.0f, 56.0f), IM_COL32(255, 0, 0, 255));
        ImGui::Render();
        Arcane::RenderFramePacket packet;
        REQUIRE(Arcane::CopyImDrawSnapshot(ImGui::GetDrawData(), packet.gameUi));
        REQUIRE(packet.gameUi.hasTextures);

        // An empty live frame: it declares the gameui node and draws nothing itself.
        ImGui::NewFrame();
        ImGui::Render();
        Arcane::NriGraphContext::FrameDesc frame;
        frame.gameUi = ImGui::GetDrawData();
        frame.capture = true;
        v.ctx->BindPacket(&packet);
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        REQUIRE(w == kW);
        REQUIRE(h == kH);
        const Rgba px = At(rgba, w, 80u, 48u);
        CHECK(px.r > px.g + 60);
        CHECK(px.r > px.b + 60);
        CHECK(Arcane::RenderErrorCount() == before);

        v.ctx.reset();
        ImGui::DestroyContext(game);
        ImGui::SetCurrentContext(previous);
    }
}

TEST_CASE("a bound packet's game HUD reaches the gameui node (d3d12)", "[gpu][packet][imgui][nri][d3d12]")
{
    CheckPacketGameUiReachesTheHud(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("a bound packet's game HUD reaches the gameui node (vulkan)", "[gpu][packet][imgui][nri][vulkan]")
{
    CheckPacketGameUiReachesTheHud(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// EXTRACT KEEPS A REGISTERED SPRITE MATERIAL (F5 plan 1 Task 5). ExtractFrame
// submits into the batcher the material was registered on and copies the
// drained batch into the packet; the bound packet is what Batch2DNode draws.
// A constant-green shade proves the registered pipeline drew: had the id been
// rewritten to kMaterialSprite, the plain sprite pipeline would draw the red
// tint instead.
// ---------------------------------------------------------------------------
namespace
{
    void CheckExtractKeepsRegisteredMaterial(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("data/shaders");
        const auto templateText = provider.Get(
            Arcane::MaterialTemplateFile(Arcane::MaterialSurface::Sprite));
        REQUIRE(templateText.has_value());
        const Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
            *templateText,
            "float4 shade(Varyings v) { return float4(0.0, 1.0, 0.0, 1.0); }",
            "extract_green",
            Arcane::MaterialSurface::Sprite);
        REQUIRE(build.errors.empty());

        Arcane::ShaderCompiler compiler;
        REQUIRE(compiler.InitializeWithDebounce(0.0));
        auto compileStage = [&](const char* entry, const char* profile)
        {
            Arcane::ShaderCompileRequest compileReq;
            compileReq.debugName = "extract_green.hlsl";
            compileReq.sourceUtf8 = build.hlsl;
            compileReq.entry = entry;
            compileReq.profile = profile;
            return compiler.CompileNow(compileReq);
        };
        const Arcane::ShaderCompileResult vs = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
        const Arcane::ShaderCompileResult ps = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
        const bool vulkan = backend == Arcane::GraphicsBackend::Vulkan;
        const Arcane::ShaderTargetResult& vsTarget = vulkan ? vs.spirv : vs.dxil;
        const Arcane::ShaderTargetResult& psTarget = vulkan ? ps.spirv : ps.dxil;
        REQUIRE(vsTarget.succeeded);
        REQUIRE(psTarget.succeeded);

        auto batcher = Arcane::Batcher2D::Create();
        REQUIRE(batcher != nullptr);
        auto templ = std::make_shared<const Arcane::MaterialTemplate>(build.templ);
        auto instance = std::make_shared<const Arcane::MaterialInstance>(templ);
        Arcane::Material2DDesc desc;
        desc.templ = templ;
        desc.instance = instance;
        desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(vsTarget.bytecode);
        desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(psTarget.bytecode);
        const std::uint16_t id = batcher->RegisterMaterial(std::move(desc));
        REQUIRE(id != Arcane::Batcher2D::kInvalidMaterialId);
        REQUIRE(id != Arcane::Batcher2D::kMaterialSprite);
        batcher->Begin(kW, kH);

        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        Arcane::RegisterSceneComponents(reg);
        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });

        Arcane::RenderContext2D renderCtx;
        renderCtx.batcher = batcher.get();
        reg.SetResource<Arcane::RenderContext2D>(std::move(renderCtx));
        const Arcane::Guid matGuid = Arcane::Guid::Generate();
        std::unordered_map<Arcane::Guid, std::uint16_t> ids;
        ids.emplace(matGuid, id);
        reg.SetResource<Arcane::SpriteMaterialTable>(Arcane::SpriteMaterialTable{ &ids });

        Astra::Entity sprite = reg.CreateEntity();
        Arcane::Transform xform;
        xform.position = { 80.0f, 48.0f, 0.0f };
        xform.scale = { 16.0f, 16.0f, 1.0f };
        reg.AddComponent<Arcane::Transform>(sprite, xform);
        reg.SetParent(sprite, root);
        Arcane::SpriteRenderer rend;
        rend.tint = { 1.0f, 0.0f, 0.0f, 1.0f };
        rend.material = matGuid;
        reg.AddComponent<Arcane::SpriteRenderer>(sprite, rend);
        Arcane::TransformPropagationSystem{}(reg);
        Arcane::BoundsSystem{}(reg);

        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { kW, kH };
        req.editorView = Arcane::Ortho2DView(
            glm::vec2(static_cast<float>(kW) * 0.5f, static_cast<float>(kH) * 0.5f),
            static_cast<float>(kH) * 0.5f,
            req.viewport);
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(reg, req, ring);
        REQUIRE(packet.batchSpans.size() == 1u);
        CHECK(packet.batchSpans[0].material == id);
        CHECK(packet.batchSpans[0].worldSpace);

        PixelVehicle v = MakeVehicle(backend);
        v.ctx->BindPacket(&packet);
        Arcane::NriGraphContext::FrameDesc frame;
        frame.batch = batcher.get();
        frame.capture = true;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        const Rgba px = At(rgba, w, 80u, 48u);
        CHECK(px.g > px.r + 60);
        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("extract keeps a registered sprite material on screen (d3d12)",
          "[gpu][extract][shadercompile][nri][d3d12]")
{
    CheckExtractKeepsRegisteredMaterial(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("extract keeps a registered sprite material on screen (vulkan)",
          "[gpu][extract][shadercompile][nri][vulkan]")
{
    CheckExtractKeepsRegisteredMaterial(Arcane::GraphicsBackend::Vulkan);
}

namespace
{
    void CheckExtractedOverlayReachesTheBackbuffer(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        Arcane::RegisterSceneComponents(reg);

        Manifold2D::Physics::WorldDef wd;
        wd.gravityX = Manifold2D::Physics::Real(0);
        wd.gravityY = Manifold2D::Physics::Real(0);
        reg.SetResource(Arcane::Detail::Physics2D::Adopt(
            std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));
        Manifold2D::Physics::PhysicsWorld& solver =
            *Arcane::Detail::Physics2D::Access::Solver(*reg.GetResource<Arcane::PhysicsWorld2D>());
        Manifold2D::Physics::BodyDef bd;
        bd.type = Manifold2D::Physics::BodyType::Static;
        bd.position = Manifold2D::Physics::Vec2(
            Manifold2D::Physics::Real(15), Manifold2D::Physics::Real(6));
        bd.shape = Manifold2D::Physics::MakeAabb(
            Manifold2D::Physics::Real(1), Manifold2D::Physics::Real(1));
        solver.AddBody(bd);

        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { kW, kH };
        req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 16.0f, req.viewport);
        req.playMode = false;
        req.showGrid2d = true;
        req.gridMinorAlpha = 0.35f;
        req.gridMajorAlpha = 0.55f;
        req.gridLineRgb = { 0.5f, 0.5f, 0.5f };
        req.gridLineThicknessPx = 8.0f;
        req.gridMaxLinesPerAxis = 4096;
        req.physicsDebug = true;
        req.physicsWholeWorld = true;
        req.cameraRect = false;
        Arcane::OverlayLine gizmo{};
        gizmo.a = { 4.0f, 10.0f, 0.0f };
        gizmo.b = { 16.0f, 10.0f, 0.0f };
        gizmo.color = { 0.0f, 0.0f, 1.0f, 1.0f };
        gizmo.thickness = 8.0f;
        gizmo.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
        gizmo.space = Arcane::OverlayColorSpace::DisplayReferred;
        gizmo.screenSpace = true;
        req.gizmoPrims.AddLine(gizmo);

        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(reg, req, ring);
        REQUIRE(packet.spriteView.has_value());
        const glm::vec3 origin = packet.spriteView->view.WorldToScreen({ 0.0f, 0.0f, 0.0f });
        CHECK(std::abs(origin.x - 80.0f) < 0.5f);
        CHECK(std::abs(origin.y - 48.0f) < 0.5f);

        const Arcane::OverlayLine* physics = nullptr;
        float best = -1.0f;
        for (const Arcane::OverlayPrimitive& prim : packet.overlay.prims)
        {
            if (prim.tag != Arcane::OverlayPrimitive::Tag::Line)
                continue;
            const Arcane::OverlayLine& line = prim.line;
            if (!line.screenSpace || line.space != Arcane::OverlayColorSpace::Linear
                || line.depth != Arcane::OverlayDepthClass::AlwaysOnTop)
                continue;
            const float len = glm::length(glm::vec2(line.b) - glm::vec2(line.a));
            if (len > best)
            {
                best = len;
                physics = &line;
            }
        }
        REQUIRE(physics != nullptr);
        const int px = (int)std::lround(0.5f * (physics->a.x + physics->b.x));
        const int py = (int)std::lround(0.5f * (physics->a.y + physics->b.y));
        REQUIRE((std::abs(px - 80) > 8 || std::abs(py - 20) > 8));
        REQUIRE((std::abs(px - 10) > 8 || std::abs(py - 10) > 8));

        Arcane::RenderFramePacket cleared = packet;
        cleared.overlay.prims.clear();
        cleared.overlay.hasGrid3d = false;

        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;

        PixelVehicle v = MakeVehicle(backend);
        v.ctx->BindPacket(&packet);
        RenderOne(*v.ctx, frame);
        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> drawn;
        REQUIRE(v.ctx->ReadCapture(w, h, drawn));

        v.ctx->BindPacket(&cleared);
        RenderOne(*v.ctx, frame);
        std::uint32_t w2 = 0, h2 = 0;
        std::vector<unsigned char> bare;
        REQUIRE(v.ctx->ReadCapture(w2, h2, bare));
        REQUIRE((w == kW && h == kH && w2 == w && h2 == h));

        const Rgba gridOn = At(drawn, w, 80, 20);
        const Rgba gridOff = At(bare, w, 80, 20);
        CHECK((gridOn.r != gridOff.r || gridOn.g != gridOff.g || gridOn.b != gridOff.b));

        const Rgba gizmoOn = At(drawn, w, 10, 10);
        CHECK(gizmoOn.r == 0);
        CHECK(gizmoOn.g == 0);
        CHECK(gizmoOn.b == 255);
        const Rgba gizmoOff = At(bare, w, 10, 10);
        CHECK((!(gizmoOff.r == 0 && gizmoOff.g == 0 && gizmoOff.b == 255)));

        bool physicsDiff = false;
        const glm::ivec2 samples[5] = {
            { px, py }, { px - 1, py }, { px + 1, py }, { px, py - 1 }, { px, py + 1 } };
        for (const glm::ivec2 s : samples)
        {
            if (s.x < 0 || s.y < 0 || s.x >= (int)w || s.y >= (int)h)
                continue;
            const Rgba a = At(drawn, w, (std::uint32_t)s.x, (std::uint32_t)s.y);
            const Rgba b = At(bare, w, (std::uint32_t)s.x, (std::uint32_t)s.y);
            if (a.r != b.r || a.g != b.g || a.b != b.b)
                physicsDiff = true;
        }
        CHECK(physicsDiff);
        CHECK(Arcane::RenderErrorCount() == before);
    }

    // Packet-bound frames take both overlay matrices from the packet. Equal
    // sprite and mesh views depth-test a world line; differing views do not.
    void CheckPacketBoundDifferingViewWorldLine(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
        const Arcane::Guid cubeId{ 1, 1 };
        GpuSceneWorld w;
        w.AddMesh(cubeId, cube);
        w.Spawn(glm::vec3(0.0f), cubeId);
        w.Schedulers();

        Arcane::MeshSceneDesc scene;
        FillCamera(scene);
        Arcane::ViewTransform meshView;
        meshView.view       = scene.view;
        meshView.projection = scene.projection;
        meshView.viewport   = glm::uvec2{ kW, kH };

        Arcane::VisibleSet vis;
        Arcane::BuildVisibleSet(w.reg, meshView, vis);
        Arcane::GpuSceneMirror mirror;
        Arcane::GpuSceneFrame gpu;
        Arcane::GpuSceneSync(w.reg, mirror, /*deviceSyncedGeneration*/ 0u, gpu.stage);
        Arcane::BuildGpuSceneFrame(mirror, &vis, Arcane::MeshSectionRanges(w.reg.GetResource<Arcane::MeshTable>()), meshView, gpu);
        for (Arcane::GpuInstance& row : gpu.stage.values)
            row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        scene.scene = &gpu;

        Arcane::ViewTransform otherView = meshView;
        otherView.projection = Arcane::PerspectiveProjection(kFovYDegrees + 1.0f,
                                                             static_cast<float>(kW) / static_cast<float>(kH),
                                                             kNearZ, kFarZ);
        REQUIRE(meshView.ViewProjection() != otherView.ViewProjection());

        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { kW, kH };
        req.editorView = meshView;
        Arcane::ExtractFrame(w.reg, req, ring);
        Arcane::RenderFramePacket& packet = ring.Latest();
        REQUIRE(packet.spriteView.has_value());
        REQUIRE(packet.meshView.has_value());

        Arcane::OverlayLine line{};
        line.a = { -0.4f, 0.0f, -2.0f };
        line.b = {  0.4f, 0.0f, -2.0f };
        line.thickness = 0.5f;
        line.depth = Arcane::OverlayDepthClass::DepthTested;
        line.space = Arcane::OverlayColorSpace::DisplayReferred;
        line.color = { 0.0f, 1.0f, 0.0f, 1.0f };
        line.screenSpace = false;
        packet.overlay.AddLine(line);

        PixelVehicle v = MakeVehicle(backend);
        v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));

        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.mesh = &scene;

        auto centre = [&]() {
            v.ctx->BindPacket(&packet);
            RenderOne(*v.ctx, frame);
            std::uint32_t width = 0, height = 0;
            std::vector<unsigned char> rgba;
            REQUIRE(v.ctx->ReadCapture(width, height, rgba));
            REQUIRE(width == kW);
            REQUIRE(height == kH);
            return At(rgba, kW, kW / 2u, kH / 2u);
        };

        packet.spriteView->view = meshView;
        packet.meshView = meshView;
        const Rgba hidden = centre();
        CHECK(hidden.r > hidden.g + 60);

        packet.spriteView->view = otherView;
        packet.meshView = meshView;
        REQUIRE(packet.spriteView->view.ViewProjection() != packet.meshView->ViewProjection());
        const Rgba shown = centre();
        CHECK(shown.g == 255);
        CHECK(shown.r == 0);
        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("extracted overlay reaches the backbuffer (d3d12)", "[gpu][pixel][overlay][nri][d3d12]")
{
    CheckExtractedOverlayReachesTheBackbuffer(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("extracted overlay reaches the backbuffer (vulkan)", "[gpu][pixel][overlay][nri][vulkan]")
{
    CheckExtractedOverlayReachesTheBackbuffer(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("packet-bound overlay draws a differing-view world line untested (d3d12)", "[gpu][pixel][overlay][nri][d3d12]")
{
    CheckPacketBoundDifferingViewWorldLine(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("packet-bound overlay draws a differing-view world line untested (vulkan)", "[gpu][pixel][overlay][nri][vulkan]")
{
    CheckPacketBoundDifferingViewWorldLine(Arcane::GraphicsBackend::Vulkan);
}
