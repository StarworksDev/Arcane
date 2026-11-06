// F5 plan 1 Task 3: the post-tonemap overlay pass. Device-less declaration
// cases ([nri][overlay]), a source scan of overlay.hlsl / tonemap.hlsl /
// tonemap_operator.hlsl / compile-shaders.bat, a CPU pin that the tonemap
// operator is nonlinear, and GPU witnesses ([gpu][overlay]) on both backends:
// the depth class, the colour space, the pass order relative to the post
// chain, and the perspective depth compare (fragD = SV_Position.z).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/DepthConventionMap.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/GpuSceneSync.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Scene/SceneCamera.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/GpuScene.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/PostChainCache.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Edit/Gizmo.hpp>                 // the real gizmo, for the order witness
#include <Arcane/Scene/RenderViewSettings.hpp>     // Ortho2DView
#include <Arcane/Render/Nri/nodes/OverlayNode.hpp> // OverlayNode::AppendTriangles
#include "Settings/EditorViewportSettings.hpp"     // ToGizmoTuning: the editor.gizmo.* defaults

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Helpers/ConstantScan.hpp"
#include "Helpers/GpuCapability.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // ---- pasted unchanged from NriGraphPixelTest.cpp:140-235 ----------------
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

    // ---- pasted unchanged from NriGraphPixelTest.cpp:880-927 ----------------
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

    // ---- pasted unchanged from NriGraphPixelTest.cpp:2150-2188 --------------
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

    // A C++ copy of data/shaders/tonemap_operator.hlsl, same constants.
    glm::vec3 AcesFilmic(glm::vec3 x)
    {
        const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        glm::vec3 y = (x * (a * x + b)) / (x * (c * x + d) + e);
        return glm::clamp(y, glm::vec3(0.0f), glm::vec3(1.0f));
    }
    float LinearToSrgb1(float lin)
    {
        return std::min(lin * 12.92f, std::pow(std::max(lin, 0.0031308f), 1.0f / 2.4f) * 1.055f - 0.055f);
    }
    glm::vec3 TonemapOperator(glm::vec3 linear)
    {
        const glm::vec3 mapped = AcesFilmic(linear);
        return { LinearToSrgb1(mapped.x), LinearToSrgb1(mapped.y), LinearToSrgb1(mapped.z) };
    }

    std::size_t NodeIndexOrNpos(const Arcane::RenderGraph& graph, const char* name)
    {
        for (std::size_t i = 0; i < graph.NodeCount(); ++i)
            if (std::string(graph.NodeName(i)) == name) return i;
        return static_cast<std::size_t>(-1);
    }
    std::size_t NodeIndex(const Arcane::RenderGraph& graph, const char* name)
    {
        const std::size_t i = NodeIndexOrNpos(graph, name);
        REQUIRE(i != static_cast<std::size_t>(-1));
        return i;
    }

    Arcane::OverlayLine ScreenLine(float x0, float y0, float x1, float y1, float z,
                                   Arcane::OverlayDepthClass depth,
                                   Arcane::OverlayColorSpace space,
                                   glm::vec4 color)
    {
        Arcane::OverlayLine line{};
        line.a = { x0, y0, z };
        line.b = { x1, y1, z };
        line.thickness = 8.0f;
        line.depth = depth;
        line.space = space;
        line.color = color;
        line.screenSpace = true;
        return line;
    }
}

TEST_CASE("overlay node sits after tonemap and samples mesh depth", "[nri][overlay]")
{
    Arcane::MeshInstance one;
    one.mesh = Arcane::Guid{ 1, 1 };
    const Arcane::MeshInstance instances[] = { one };
    Arcane::MeshSceneDesc scene;
    scene.instances = instances;

    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth = 64;
    shape.canvasHeight = 64;
    shape.mesh = &scene;
    Arcane::OverlayPrimitiveList list;
    Arcane::OverlayLine line{};
    line.a = { 0.0f, 0.0f, 0.0f };
    line.b = { 1.0f, 0.0f, 0.0f };
    line.color = { 1.0f, 0.0f, 0.0f, 1.0f };
    line.depth = Arcane::OverlayDepthClass::DepthTested;
    line.space = Arcane::OverlayColorSpace::Linear;
    list.AddLine(line);
    shape.overlay = &list;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const std::size_t tonemap = NodeIndex(graph, "tonemap");
    const std::size_t overlay = NodeIndex(graph, "overlay");
    CHECK(overlay == tonemap + 1);
    CHECK(NodeIndexOrNpos(graph, "grid") == static_cast<std::size_t>(-1));
    CHECK(graph.DepthLoadOp(overlay) == nullptr);   // sampled, not attached
    REQUIRE(graph.IsHandleValid(handles.depth));
    CHECK(graph.WasRead(handles.depth));
    CHECK(graph.WasWritten(handles.depth));         // the mesh node still writes it
}

TEST_CASE("a runtime frame with no overlay does not declare one", "[nri][overlay]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth = 64;
    shape.canvasHeight = 64;
    Arcane::DeclareGraphFrame(graph, shape, nullptr);
    CHECK(NodeIndexOrNpos(graph, "overlay") == static_cast<std::size_t>(-1));
    CHECK(NodeIndexOrNpos(graph, "grid") == static_cast<std::size_t>(-1));
}

TEST_CASE("overlay.hlsl uses the shared operator and has no exposure term", "[nri][overlay]")
{
    auto read = [](const char* rel) {
        std::ifstream in(Arcane::Test::RepoRoot() / rel);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    const std::string overlay = read("data/shaders/overlay.hlsl");
    const std::string tonemap = read("data/shaders/tonemap.hlsl");
    const std::string shared  = read("data/shaders/tonemap_operator.hlsl");
    CHECK(overlay.find("exposure") == std::string::npos);
    CHECK(overlay.find("#include \"tonemap_operator.hlsl\"") != std::string::npos);
    CHECK(tonemap.find("#include \"tonemap_operator.hlsl\"") != std::string::npos);
    CHECK(shared.find("float3 ACESFilmic(float3 x)") != std::string::npos);
    CHECK(shared.find("float3 LinearToSrgb(float3 lin)") != std::string::npos);
    // The constants stay the Narkowicz values. A drift here moves world pixels.
    CHECK(shared.find("const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;")
          != std::string::npos);
    // Screen-space clip.z is the vertex z. A hardcoded 0 is the near plane, so a
    // depth-tested overlay could never sit behind a mesh.
    CHECK(overlay.find("float4(ndc, input.pos.z, 1.0)") != std::string::npos);
    CHECK(overlay.find("float4(ndc, 0.0, 1.0)") == std::string::npos);
    CHECK(overlay.find("g_HasDepth > 0.5 && input.depthTest > 0.5") != std::string::npos);
    // SV_Position.z is already post-divide. A second divide by .w must fail this scan.
    CHECK(overlay.find("input.clip.z / input.clip.w") == std::string::npos);
    CHECK(overlay.find("clip.z / clip.w") == std::string::npos);
    const auto fragD = overlay.find("float fragD = input.clip.z;");
    CHECK(fragD != std::string::npos);
    CHECK(overlay.find("float fragD = input.clip.z;", fragD + 1) != std::string::npos);
    // compile-shaders.bat is an explicit list; compile-shaders.sh reads its lines.
    const std::string bat = read("data/shaders/compile-shaders.bat");
    CHECK(bat.find("call :compile overlay vs_main vs_6_5 overlay_vs") != std::string::npos);
    CHECK(bat.find("call :compile overlay ps_main ps_6_5 overlay_ps") != std::string::npos);
    CHECK(bat.find("call :compile overlay grid_vs_main vs_6_5 overlay_grid_vs") != std::string::npos);
    CHECK(bat.find("call :compile overlay grid_ps_main ps_6_5 overlay_grid_ps") != std::string::npos);
}

// The placeholder depth node (NriGraphContext.cpp:1358-1370) stays unconsumed.
// Reading it would make WasRead true. Today's placeholder case already requires
// WasWritten(depth) == false (RenderGraphTest.cpp:6673-6695).
TEST_CASE("overlay with no mesh does not read the placeholder depth", "[nri][overlay]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth = 64;
    shape.canvasHeight = 64;
    shape.depth = true;
    Arcane::OverlayPrimitiveList list;
    Arcane::OverlayLine line{};
    line.a = { 4.0f, 10.0f, 1.0f };
    line.b = { 16.0f, 10.0f, 1.0f };
    line.thickness = 8.0f;
    line.depth = Arcane::OverlayDepthClass::DepthTested;
    line.space = Arcane::OverlayColorSpace::DisplayReferred;
    line.color = { 0.0f, 0.0f, 1.0f, 1.0f };
    line.screenSpace = true;
    list.AddLine(line);
    shape.overlay = &list;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const std::size_t tonemap = NodeIndex(graph, "tonemap");
    const std::size_t overlay = NodeIndex(graph, "overlay");
    CHECK(overlay == tonemap + 1);
    CHECK(NodeIndexOrNpos(graph, "depth") != static_cast<std::size_t>(-1));
    REQUIRE(graph.IsHandleValid(handles.depth));
    CHECK_FALSE(graph.WasRead(handles.depth));
    CHECK_FALSE(graph.WasWritten(handles.depth));
}

// Review Focus 3, CPU half. The operator is nonlinear, so the two composites differ
// even when the post chain is empty. This fails if someone "proves" identity by
// algebra that assumes a linear tonemap.
TEST_CASE("tonemap-then-blend differs from blend-then-tonemap", "[nri][overlay]")
{
    const glm::vec3 scene{ 0.02f, 0.02f, 0.04f };
    const glm::vec3 prim{ 0.8f, 0.8f, 0.2f };
    const float a = 0.5f;
    const glm::vec3 blendThen = TonemapOperator(prim * a + scene * (1.0f - a));
    const glm::vec3 tonemapThen = TonemapOperator(prim) * a + TonemapOperator(scene) * (1.0f - a);
    CHECK(blendThen != tonemapThen);
}

void CheckOverlayClasses(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    // The cube sequence at NriGraphPixelTest.cpp:2202-2239. Centre pixel is the
    // lit red cube; pixel (10, 10) is background.
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
    Arcane::GpuSceneFrame gpu;
    Arcane::GpuSceneSync(w.reg, mirror, /*deviceSyncedGeneration*/ 0u, gpu.stage);
    Arcane::BuildGpuSceneFrame(mirror, &vis, Arcane::MeshSectionRanges(w.reg.GetResource<Arcane::MeshTable>()), view, gpu);
    REQUIRE(gpu.stage.rows.size() == 1u);
    for (Arcane::GpuInstance& row : gpu.stage.values)
        row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    PixelVehicle v = MakeVehicle(backend);
    v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
    scene.scene = &gpu;

    auto shoot = [&](const Arcane::OverlayPrimitiveList& list, bool withMesh) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.overlay = &list;
        if (withMesh)
            frame.mesh = &scene;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };

    // pos.z = 1 is the far plane (clip.w is 1). The cube's front face is nearer
    // (eye at z = 4, BuildCube(2) half-extent 1, near 0.1, far 100). Forward-Z
    // discards when fragD > sceneD, so this green line is hidden. A shader that
    // writes clip.z = 0 puts the line on the near plane and the centre goes green.
    Arcane::OverlayPrimitiveList hidden;
    hidden.AddLine(ScreenLine(72.0f, 48.0f, 88.0f, 48.0f, 1.0f,
        Arcane::OverlayDepthClass::DepthTested,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)));
    const std::vector<unsigned char> covered = shoot(hidden, true);
    const Rgba cubePixel = At(covered, kW, kW / 2u, kH / 2u);
    CHECK(cubePixel.r > cubePixel.g + 60);

    Arcane::OverlayPrimitiveList onTop = hidden;
    onTop.prims[0].line.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    const std::vector<unsigned char> top = shoot(onTop, true);
    const Rgba topPixel = At(top, kW, kW / 2u, kH / 2u);
    CHECK(topPixel.r == 0);
    CHECK(topPixel.g == 255);
    CHECK(topPixel.b == 0);

    // Display-referred (1,0,0) is not ACES(1,0,0). ACES(1) is about 0.8038,
    // which encodes to a byte near 231, not 255. Pixel (10, 10) is off the cube.
    Arcane::OverlayPrimitiveList red;
    red.AddLine(ScreenLine(4.0f, 10.0f, 16.0f, 10.0f, 0.0f,
        Arcane::OverlayDepthClass::AlwaysOnTop,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
    const std::vector<unsigned char> shown = shoot(red, true);
    const Rgba shownPixel = At(shown, kW, 10u, 10u);
    CHECK(shownPixel.r == 255);
    CHECK(shownPixel.g == 0);
    CHECK(shownPixel.b == 0);

    Arcane::OverlayPrimitiveList graded = red;
    graded.prims[0].line.space = Arcane::OverlayColorSpace::Linear;
    const std::vector<unsigned char> gradedPx = shoot(graded, true);
    const Rgba gradedPixel = At(gradedPx, kW, 10u, 10u);
    CHECK(gradedPixel.r > 200);
    CHECK(gradedPixel.r < 250);

    // No mesh: g_HasDepth is 0, so z = 1 is still drawn. The declaration test
    // above is what fails if this frame samples the placeholder depth.
    Arcane::OverlayPrimitiveList open;
    open.AddLine(ScreenLine(4.0f, 10.0f, 16.0f, 10.0f, 1.0f,
        Arcane::OverlayDepthClass::DepthTested,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)));
    const std::vector<unsigned char> bare = shoot(open, false);
    const Rgba blue = At(bare, kW, 10u, 10u);
    CHECK(blue.b == 255);
    CHECK(Arcane::RenderErrorCount() == before);
}

// Review Focus 3, GPU half. An opaque (alpha 1) display-referred pixel is
// identical with and without a post chain that tints the scene, because the
// overlay blends after the tonemap and alpha 1 replaces the destination. A
// background pixel the line does not cover changes. The CPU test above is the
// partial-alpha nonlinearity; this one is the pass order.
void CheckPostDoesNotGradeOverlay(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    // The compile path is MaterialSourceTest.cpp:624-672, in post mode
    // (externalInput true, the sentinel case at :434-450). Bytes are copied
    // into PostChainDesc the way PostChainCache.cpp:271-282 publishes them.
    // CompileNow runs in the test, before either RenderFrame. PrepareChain's
    // DeviceWaitIdle (FullscreenNodes.cpp:867-871) runs only when m_ready is
    // already true; this chain is bound once, so that branch does not run.
    Arcane::ShaderSourceProvider provider;
    provider.AddRoot(Arcane::Test::RepoRoot() / "data" / "shaders");
    const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
    REQUIRE(templateText.has_value());

    const char* shade =
        "float4 shade(Varyings v) { return InputTexture.Sample(MaterialSampler, v.uv) * 0.25; }\n";
    const std::uint32_t sceneIn[] = { Arcane::kSceneInput };
    const Arcane::MaterialChainPassDesc authored[] = { { shade, sceneIn } };
    Arcane::MaterialChainBuildResult built = Arcane::BuildMaterialChainSource(
        *templateText, authored, "tint_quarter", {}, /*externalInput=*/true);
    REQUIRE(built.Ok());
    REQUIRE(built.hlsl.size() == 1u);
    REQUIRE(built.passInputs.size() == 1u);
    REQUIRE(built.passInputs[0].size() == 1u);
    REQUIRE(built.passInputs[0][0] == Arcane::kSceneInput);

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(0.0));
    auto compileStage = [&](const char* entry, const char* profile) {
        Arcane::ShaderCompileRequest req;
        req.debugName = "tint_quarter.hlsl";
        req.sourceUtf8 = built.hlsl[0];
        req.entry = entry;
        req.profile = profile;
        const Arcane::ShaderCompileResult cr = compiler.CompileNow(req);
        REQUIRE(cr.AllSucceeded());
        const Arcane::ShaderTargetResult& target =
            backend == Arcane::GraphicsBackend::D3D12 ? cr.dxil : cr.spirv;
        REQUIRE(target.succeeded);
        REQUIRE_FALSE(target.bytecode.empty());
        return std::make_shared<const std::vector<std::uint8_t>>(target.bytecode);
    };
    auto templ = std::make_shared<Arcane::MaterialTemplate>(std::move(built.templ));
    Arcane::PostChainDesc chain;
    chain.templ = templ;
    chain.instance = std::make_shared<Arcane::MaterialInstance>(
        std::shared_ptr<const Arcane::MaterialTemplate>(templ));
    chain.chainInputSlots = built.chainInputSlots;
    Arcane::PostChainPassDesc pass;
    pass.vsBytes = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
    pass.psBytes = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
    pass.inputs = built.passInputs[0];
    chain.passes.push_back(std::move(pass));
    compiler.Shutdown();

    PixelVehicle v = MakeVehicle(backend);
    Arcane::OverlayPrimitiveList list;
    list.AddLine(ScreenLine(4.0f, 10.0f, 16.0f, 10.0f, 0.0f,
        Arcane::OverlayDepthClass::AlwaysOnTop,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)));

    auto shoot = [&](const Arcane::PostChainDesc* post) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.overlay = &list;
        frame.post = post;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };
    const std::vector<unsigned char> noPost = shoot(nullptr);
    const std::vector<unsigned char> withPost = shoot(&chain);

    // (10, 10) is inside the 8px-thick segment. (4, 4) is outside it
    // (the quad covers y in [6, 14]) and is the tonemapped clear colour.
    const Rgba overlayA = At(noPost, kW, 10u, 10u);
    const Rgba overlayB = At(withPost, kW, 10u, 10u);
    CHECK(overlayA.r == 0);
    CHECK(overlayA.g == 255);
    CHECK(overlayA.b == 0);
    CHECK(overlayA.r == overlayB.r);
    CHECK(overlayA.g == overlayB.g);
    CHECK(overlayA.b == overlayB.b);

    const Rgba worldA = At(noPost, kW, 4u, 4u);
    const Rgba worldB = At(withPost, kW, 4u, 4u);
    CHECK((worldA.r != worldB.r || worldA.g != worldB.g || worldA.b != worldB.b));
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("overlay depth class and colour space (d3d12)", "[gpu][overlay][d3d12]")
{
    CheckOverlayClasses(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("overlay depth class and colour space (vulkan)", "[gpu][overlay][vulkan]")
{
    CheckOverlayClasses(Arcane::GraphicsBackend::Vulkan);
}
TEST_CASE("post chain does not grade an opaque overlay pixel (d3d12)", "[gpu][overlay][shadercompile][d3d12]")
{
    CheckPostDoesNotGradeOverlay(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("post chain does not grade an opaque overlay pixel (vulkan)", "[gpu][overlay][shadercompile][vulkan]")
{
    CheckPostDoesNotGradeOverlay(Arcane::GraphicsBackend::Vulkan);
}

// Review Focus 5, perspective half. The screen-space lines above write clip.w = 1,
// so fragD = SV_Position.z / SV_Position.w still equals SV_Position.z and those
// checks stay green under the wrong formula. This one does not.
//
// FillCamera (eye z = 4, fovY 60, near 0.1, far 100, perspectiveRH_ZO) puts the
// cube's front face (world z = +1, view depth 3) at ndc.z ~ 0.968. A world point
// on z = 0 has view depth 4 and ndc.z ~ 0.976, farther than the front face and
// nearer than the depth clear of 1. The wrong formula divides that ndc.z by
// SV_Position.w (1/clip.w), producing ~ ndc.z * viewDepth ~ 3.9, which is past
// the clear and discards the unoccluded axis as well as the in-front line.
void CheckPerspectiveGridDepth(Arcane::GraphicsBackend backend)
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
    Arcane::GpuSceneFrame gpu;
    Arcane::GpuSceneSync(w.reg, mirror, /*deviceSyncedGeneration*/ 0u, gpu.stage);
    Arcane::BuildGpuSceneFrame(mirror, &vis, Arcane::MeshSectionRanges(w.reg.GetResource<Arcane::MeshTable>()), view, gpu);
    for (Arcane::GpuInstance& row : gpu.stage.values)
        row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    PixelVehicle v = MakeVehicle(backend);
    v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
    scene.scene = &gpu;

    auto shoot = [&](const Arcane::OverlayPrimitiveList& list) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.mesh = &scene;
        frame.overlay = &list;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };

    // Axis only. Camera at y = 0 looking down -Z, so the world-X axis projects
    // onto y = kH/2. The cube's front face covers about x in [52, 108]; x = 10
    // is beside it. fwidth is about one pixel, so accept the midline and its
    // two neighbours. The wrong divide discards all three.
    Arcane::OverlayPrimitiveList grid;
    grid.hasGrid3d = true;
    grid.grid3d.view = view;
    // XY, not XZ: FillCamera's eye is at (0, 0, 4) with +Y up, so the XZ plane
    // (y = 0) is edge-on and rasterises nothing. The XY plane is z = 0, view depth 4.
    grid.grid3d.SetPlane(Arcane::GridSceneDesc::Plane::XY);
    grid.grid3d.minorSpacing = 1.0f;
    grid.grid3d.majorEvery = 8.0f;
    grid.grid3d.fadeDistance = 1000.0f;
    grid.grid3d.minHalfExtent = 20.0f;
    grid.grid3d.extentPerAltitude = 1.0f;
    grid.grid3d.minorColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    grid.grid3d.majorColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    grid.grid3d.axisUColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    grid.grid3d.axisVColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    const std::vector<unsigned char> gridPx = shoot(grid);
    const Rgba centre = At(gridPx, kW, kW / 2u, kH / 2u);
    CHECK(centre.r > centre.g + 60);
    bool axis = false;
    for (std::uint32_t y = (kH / 2u) - 1u; y <= (kH / 2u) + 1u; ++y)
    {
        const Rgba px = At(gridPx, kW, 10u, y);
        if (px.g > 200 && px.g < 250 && px.r < 80)
            axis = true;
    }
    CHECK(axis);

    auto worldLine = [](float z) {
        Arcane::OverlayLine line{};
        line.a = { -0.4f, 0.0f, z };
        line.b = {  0.4f, 0.0f, z };
        line.thickness = 0.5f;
        line.depth = Arcane::OverlayDepthClass::DepthTested;
        line.space = Arcane::OverlayColorSpace::DisplayReferred;
        line.color = { 0.0f, 1.0f, 0.0f, 1.0f };
        line.screenSpace = false;
        return line;
    };
    Arcane::OverlayPrimitiveList inFront;
    inFront.AddLine(worldLine(2.0f));
    const Rgba front = At(shoot(inFront), kW, kW / 2u, kH / 2u);
    CHECK(front.r == 0);
    CHECK(front.g == 255);
    CHECK(front.b == 0);

    Arcane::OverlayPrimitiveList behind;
    behind.AddLine(worldLine(-2.0f));
    const Rgba back = At(shoot(behind), kW, kW / 2u, kH / 2u);
    CHECK(back.r > back.g + 60);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("a perspective grid axis is hidden on the cube and visible beside it (d3d12)", "[gpu][overlay][d3d12]")
{
    CheckPerspectiveGridDepth(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("a perspective grid axis is hidden on the cube and visible beside it (vulkan)", "[gpu][overlay][vulkan]")
{
    CheckPerspectiveGridDepth(Arcane::GraphicsBackend::Vulkan);
}

// =========================================================================
// Review round 1 (Task 3): I1 submission order, I2 the world camera, M5 the
// CPU expansion.
// =========================================================================
namespace
{
    Arcane::OverlayFill ScreenRect(glm::vec2 pos, glm::vec2 size, Arcane::OverlayDepthClass depth, glm::vec4 color)
    {
        Arcane::OverlayFill fill{};
        fill.kind        = Arcane::OverlayPrimKind::Rect;
        fill.a           = glm::vec3(pos, 0.0f);
        fill.size        = size;
        fill.color       = color;
        fill.depth       = depth;
        fill.space       = Arcane::OverlayColorSpace::DisplayReferred;
        fill.screenSpace = true;
        return fill;
    }

    // The OverlayGizmoSink in EditorAppFrame.cpp, restated: the gizmo
    // callbacks as AlwaysOnTop, DisplayReferred, screen-space primitives, in
    // the order the gizmo calls them.
    class TestGizmoSink final : public Arcane::GizmoDrawSink
    {
    public:
        explicit TestGizmoSink(Arcane::OverlayPrimitiveList& out) : m_out(out) {}
        void Line(glm::vec2 a, glm::vec2 b, float thickness, glm::vec4 rgba) override
        {
            Arcane::OverlayLine line{};
            line.a = glm::vec3(a, 0.0f);
            line.b = glm::vec3(b, 0.0f);
            line.color = rgba;
            line.thickness = thickness;
            line.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
            line.space = Arcane::OverlayColorSpace::DisplayReferred;
            line.screenSpace = true;
            m_out.AddLine(line);
        }
        void Triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec4 rgba) override
        {
            Arcane::OverlayFill fill = Fill(Arcane::OverlayPrimKind::Triangle, rgba);
            fill.a = glm::vec3(a, 0.0f);
            fill.b = glm::vec3(b, 0.0f);
            fill.c = glm::vec3(c, 0.0f);
            m_out.AddFill(fill);
        }
        void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 rgba) override
        {
            Arcane::OverlayFill fill = Fill(Arcane::OverlayPrimKind::Rect, rgba);
            fill.a = glm::vec3(pos, 0.0f);
            fill.size = size;
            m_out.AddFill(fill);
        }
        void Circle(glm::vec2 center, float radius, glm::vec4 rgba) override
        {
            Arcane::OverlayFill fill = Fill(Arcane::OverlayPrimKind::Circle, rgba);
            fill.a = glm::vec3(center, 0.0f);
            fill.radius = radius;
            m_out.AddFill(fill);
        }
    private:
        static Arcane::OverlayFill Fill(Arcane::OverlayPrimKind kind, glm::vec4 rgba)
        {
            Arcane::OverlayFill fill{};
            fill.kind = kind;
            fill.color = rgba;
            fill.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
            fill.space = Arcane::OverlayColorSpace::DisplayReferred;
            fill.screenSpace = true;
            return fill;
        }
        Arcane::OverlayPrimitiveList& m_out;
    };

    // The real translate gizmo with its XY plane HOT (so the plane square is
    // drawn), at the centre of a kW x kH ortho view.
    Arcane::OverlayPrimitiveList HotPlaneGizmo()
    {
        Arcane::OverlayPrimitiveList list;
        TestGizmoSink sink(list);
        const Arcane::ViewTransform view = Arcane::Ortho2DView({ 0.0f, 0.0f }, 1.0f, { kW, kH });
        const Arcane::GizmoTuning tuning = Arcane::Editor::ToGizmoTuning(Arcane::Editor::EditorGizmoSettings{});
        Arcane::Draw(sink, Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, Arcane::GizmoTransform{},
                     view, Arcane::GizmoHandleMask::All(), 1.0f, Arcane::GizmoAxis::XY, Arcane::GizmoAxis::None,
                     tuning);
        return list;
    }

    // The translucent prims are the hot plane square (hotFillAlpha < 1);
    // every bar, axis and head is opaque.
    bool IsTranslucentFill(const Arcane::OverlayPrimitive& p)
    {
        return p.tag == Arcane::OverlayPrimitive::Tag::Fill && p.fill.color.a < 1.0f;
    }
}

TEST_CASE("overlay expansion: vertex counts per kind, the class filter and degenerate input", "[nri][overlay]")
{
    using Arcane::OverlayDepthClass;
    const glm::vec4 white(1.0f);
    Arcane::OverlayPrimitiveList list;
    list.AddLine(ScreenLine(0, 0, 10, 0, 0, OverlayDepthClass::AlwaysOnTop,
                            Arcane::OverlayColorSpace::DisplayReferred, white));            // 6
    Arcane::OverlayLine thin = ScreenLine(0, 0, 10, 0, 0, OverlayDepthClass::AlwaysOnTop,
                                          Arcane::OverlayColorSpace::DisplayReferred, white);
    thin.thickness = 0.0f;
    list.AddLine(thin);                                                                       // 0
    list.AddLine(ScreenLine(5, 5, 5, 5, 0, OverlayDepthClass::AlwaysOnTop,
                            Arcane::OverlayColorSpace::DisplayReferred, white));            // 0: zero length
    list.AddFill(ScreenRect({ 0, 0 }, { 4, 4 }, OverlayDepthClass::AlwaysOnTop, white));   // 6
    Arcane::OverlayFill tri = ScreenRect({ 0, 0 }, { 0, 0 }, OverlayDepthClass::AlwaysOnTop, white);
    tri.kind = Arcane::OverlayPrimKind::Triangle;
    list.AddFill(tri);                                                                        // 3
    Arcane::OverlayFill circle = ScreenRect({ 8, 8 }, { 0, 0 }, OverlayDepthClass::AlwaysOnTop, white);
    circle.kind   = Arcane::OverlayPrimKind::Circle;
    circle.radius = 3.0f;
    list.AddFill(circle);                                                                     // 48
    Arcane::OverlayFill dot = circle;
    dot.radius = 0.0f;
    list.AddFill(dot);                                                                        // 0
    list.AddFill(ScreenRect({ 0, 0 }, { 4, 4 }, OverlayDepthClass::DepthTested, white));   // the other class

    std::vector<Arcane::OverlayNode::Vertex> onTop, tested;
    Arcane::OverlayNode::AppendTriangles(list, OverlayDepthClass::AlwaysOnTop, true, onTop);
    Arcane::OverlayNode::AppendTriangles(list, OverlayDepthClass::DepthTested, true, tested);
    CHECK(Arcane::OverlayNode::kCircleSegments == 16u);
    CHECK(onTop.size() == 6u + 6u + 3u + 3u * Arcane::OverlayNode::kCircleSegments);
    CHECK(tested.size() == 6u);
    for (const auto& v : onTop)
        CHECK(v.depthTest == 0.0f);
    for (const auto& v : tested)
        CHECK(v.depthTest == 1.0f);   // screen-space: samples whenever DepthTested
}

TEST_CASE("overlay expansion keeps submission order: a fill before a line stays under it", "[nri][overlay]")
{
    using Arcane::OverlayDepthClass;
    const glm::vec4 red{ 1, 0, 0, 1 }, green{ 0, 1, 0, 1 }, blue{ 0, 0, 1, 1 };
    Arcane::OverlayPrimitiveList list;
    list.AddFill(ScreenRect({ 0, 0 }, { 10, 10 }, OverlayDepthClass::AlwaysOnTop, red));
    list.AddLine(ScreenLine(0, 5, 10, 5, 0, OverlayDepthClass::AlwaysOnTop,
                            Arcane::OverlayColorSpace::DisplayReferred, green));
    list.AddFill(ScreenRect({ 2, 2 }, { 2, 2 }, OverlayDepthClass::AlwaysOnTop, blue));

    std::vector<Arcane::OverlayNode::Vertex> out;
    Arcane::OverlayNode::AppendTriangles(list, OverlayDepthClass::AlwaysOnTop, false, out);
    REQUIRE(out.size() == 18u);
    for (std::size_t i = 0; i < 6; ++i)   CHECK(out[i].color == red);
    for (std::size_t i = 6; i < 12; ++i)  CHECK(out[i].color == green);
    for (std::size_t i = 12; i < 18; ++i) CHECK(out[i].color == blue);

    // The real gizmo submits its hot plane square BEFORE the plane bars
    // (Gizmo.cpp, the DrawDualAxis block), and the list keeps that order.
    const Arcane::OverlayPrimitiveList gizmo = HotPlaneGizmo();
    std::size_t firstFill = gizmo.prims.size(), lastFill = 0;
    for (std::size_t i = 0; i < gizmo.prims.size(); ++i)
        if (IsTranslucentFill(gizmo.prims[i])) { firstFill = std::min(firstFill, i); lastFill = i; }
    REQUIRE(firstFill < gizmo.prims.size());
    REQUIRE(lastFill + 1 < gizmo.prims.size());
    CHECK(gizmo.prims[lastFill + 1].tag == Arcane::OverlayPrimitive::Tag::Line);   // a bar follows the square
}

TEST_CASE("overlay expansion: a world primitive samples depth only through the mesh camera", "[nri][overlay]")
{
    Arcane::OverlayLine world = ScreenLine(0, 0, 1, 0, 0, Arcane::OverlayDepthClass::DepthTested,
                                           Arcane::OverlayColorSpace::Linear, glm::vec4(1.0f));
    world.screenSpace = false;
    Arcane::OverlayPrimitiveList list;
    list.AddLine(world);
    std::vector<Arcane::OverlayNode::Vertex> same, other;
    Arcane::OverlayNode::AppendTriangles(list, Arcane::OverlayDepthClass::DepthTested, true, same);
    Arcane::OverlayNode::AppendTriangles(list, Arcane::OverlayDepthClass::DepthTested, false, other);
    REQUIRE(same.size() == 6u);
    REQUIRE(other.size() == 6u);
    CHECK(same[0].depthTest == 1.0f);
    CHECK(other[0].depthTest == 0.0f);
}

// I1, GPU half: submission order is paint order. A fill submitted before a
// line ends up UNDER it; the reverse order puts it over. And the real gizmo
// bars stay over its hot plane square: wherever moving the square to the END
// of the list changes a pixel, the submitted order shows the bar-only pixel.
void CheckOverlaySubmissionOrder(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();
    PixelVehicle v = MakeVehicle(backend);
    auto shoot = [&](const Arcane::OverlayPrimitiveList& list) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.overlay = &list;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };

    const Arcane::OverlayFill rect = ScreenRect({ 4, 4 }, { 32, 32 }, Arcane::OverlayDepthClass::AlwaysOnTop,
                                                glm::vec4(1, 0, 0, 1));
    const Arcane::OverlayLine line = ScreenLine(4, 20, 36, 20, 0, Arcane::OverlayDepthClass::AlwaysOnTop,
                                                Arcane::OverlayColorSpace::DisplayReferred, glm::vec4(0, 1, 0, 1));
    Arcane::OverlayPrimitiveList fillFirst;
    fillFirst.AddFill(rect);
    fillFirst.AddLine(line);
    Arcane::OverlayPrimitiveList lineFirst;
    lineFirst.AddLine(line);
    lineFirst.AddFill(rect);
    const Rgba under = At(shoot(fillFirst), kW, 20u, 20u);
    CHECK(under.r == 0);
    CHECK(under.g == 255);
    const Rgba over = At(shoot(lineFirst), kW, 20u, 20u);
    CHECK(over.r == 255);
    CHECK(over.g == 0);

    // The hot square and the hot bars are the SAME colour (Colors().hot), so
    // their order is invisible as drawn; the square is recoloured (blue, its
    // own alpha) so the order shows, and nothing else about the list changes.
    Arcane::OverlayPrimitiveList submitted = HotPlaneGizmo();
    for (Arcane::OverlayPrimitive& p : submitted.prims)
        if (IsTranslucentFill(p))
            p.fill.color = glm::vec4(0.0f, 0.0f, 1.0f, p.fill.color.a);
    Arcane::OverlayPrimitiveList fillLast, barsOnly;
    for (const Arcane::OverlayPrimitive& p : submitted.prims)
        if (!IsTranslucentFill(p)) { fillLast.prims.push_back(p); barsOnly.prims.push_back(p); }
    for (const Arcane::OverlayPrimitive& p : submitted.prims)
        if (IsTranslucentFill(p)) fillLast.prims.push_back(p);
    const std::vector<unsigned char> a = shoot(submitted);
    const std::vector<unsigned char> b = shoot(fillLast);
    const std::vector<unsigned char> c = shoot(barsOnly);
    std::size_t moved = 0, wrong = 0;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4)
    {
        if (a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2])
            continue;
        ++moved;
        if (a[i] != c[i] || a[i + 1] != c[i + 1] || a[i + 2] != c[i + 2])
            ++wrong;
    }
    CHECK(moved > 0);    // the square does overlap the bars
    CHECK(wrong == 0);   // and in submitted order the bars win every one of those pixels
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("overlay keeps submission order on the GPU, gizmo bars over the plane fill (d3d12)", "[gpu][overlay][d3d12]")
{
    CheckOverlaySubmissionOrder(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("overlay keeps submission order on the GPU, gizmo bars over the plane fill (vulkan)", "[gpu][overlay][vulkan]")
{
    CheckOverlaySubmissionOrder(Arcane::GraphicsBackend::Vulkan);
}

// I2, GPU half: the world camera.
//   * no meshes: the 3D grid and a world line still draw (nothing to test
//     against, and the world camera is given, so nothing projects through
//     identity);
//   * a world camera DIFFERENT from the mesh camera: a depth-tested world
//     line behind the cube, and the grid, draw UNTESTED over the cube;
//   * the SAME camera: the same line is hidden.
void CheckOverlayWorldCamera(Arcane::GraphicsBackend backend)
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

    PixelVehicle v = MakeVehicle(backend);
    v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
    scene.scene = &gpu;

    // A second camera: the same eye, a slightly wider lens. Not the mesh camera.
    Arcane::ViewTransform otherView = meshView;
    otherView.projection = Arcane::PerspectiveProjection(kFovYDegrees + 1.0f,
                                                         static_cast<float>(kW) / static_cast<float>(kH),
                                                         kNearZ, kFarZ);
    const glm::mat4 meshVp  = meshView.projection * meshView.view;
    const glm::mat4 otherVp = otherView.projection * otherView.view;

    auto shoot = [&](const Arcane::OverlayPrimitiveList& list, bool withMesh, const glm::mat4* worldVp) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.overlay = &list;
        if (withMesh)
            frame.mesh = &scene;
        if (worldVp)
            frame.overlayWorldViewProjection = *worldVp;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };
    auto axisNearCentreRow = [&](const std::vector<unsigned char>& px, std::uint32_t x) {
        for (std::uint32_t y = (kH / 2u) - 1u; y <= (kH / 2u) + 1u; ++y)
        {
            const Rgba p = At(px, kW, x, y);
            if (p.g > 200 && p.g < 250 && p.r < 80)
                return true;
        }
        return false;
    };
    auto worldLine = [](float z) {
        Arcane::OverlayLine line{};
        line.a = { -0.4f, 0.0f, z };
        line.b = {  0.4f, 0.0f, z };
        line.thickness = 0.5f;
        line.depth = Arcane::OverlayDepthClass::DepthTested;
        line.space = Arcane::OverlayColorSpace::DisplayReferred;
        line.color = { 0.0f, 1.0f, 0.0f, 1.0f };
        line.screenSpace = false;
        return line;
    };
    auto gridThrough = [&](const Arcane::ViewTransform& view) {
        Arcane::OverlayPrimitiveList grid;
        grid.hasGrid3d = true;
        grid.grid3d.view = view;
        grid.grid3d.SetPlane(Arcane::GridSceneDesc::Plane::XY);
        grid.grid3d.minorSpacing = 1.0f;
        grid.grid3d.majorEvery = 8.0f;
        grid.grid3d.fadeDistance = 1000.0f;
        grid.grid3d.minHalfExtent = 20.0f;
        grid.grid3d.extentPerAltitude = 1.0f;
        grid.grid3d.minorColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        grid.grid3d.majorColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        grid.grid3d.axisUColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
        grid.grid3d.axisVColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
        return grid;
    };

    // No meshes: the grid axis crosses the centre (no cube there), and a world
    // line through the given world camera lands on the centre too.
    CHECK(axisNearCentreRow(shoot(gridThrough(meshView), false, &meshVp), kW / 2u));
    Arcane::OverlayPrimitiveList behind;
    behind.AddLine(worldLine(-2.0f));
    const Rgba bare = At(shoot(behind, false, &meshVp), kW, kW / 2u, kH / 2u);
    CHECK(bare.g == 255);
    CHECK(bare.r == 0);

    // The world camera IS the mesh camera: the line behind the cube is hidden.
    const Rgba hidden = At(shoot(behind, true, &meshVp), kW, kW / 2u, kH / 2u);
    CHECK(hidden.r > hidden.g + 60);

    // A different world camera: untested, drawn over the cube.
    const Rgba shown = At(shoot(behind, true, &otherVp), kW, kW / 2u, kH / 2u);
    CHECK(shown.g == 255);
    CHECK(shown.r == 0);

    // The grid through a camera that is not the mesh camera: untested, its
    // axis crosses the cube at the centre.
    CHECK(axisNearCentreRow(shoot(gridThrough(otherView), true, &meshVp), kW / 2u));
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("overlay world camera: no mesh still draws, a non-mesh camera is untested (d3d12)", "[gpu][overlay][d3d12]")
{
    CheckOverlayWorldCamera(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("overlay world camera: no mesh still draws, a non-mesh camera is untested (vulkan)", "[gpu][overlay][vulkan]")
{
    CheckOverlayWorldCamera(Arcane::GraphicsBackend::Vulkan);
}
