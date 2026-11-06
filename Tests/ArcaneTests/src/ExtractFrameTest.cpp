// ExtractFrameTest.cpp -- the one extract stage (F5 plan 1 Task 5, spec s4.1):
// ExtractFrame runs the sprite submission into the host's batcher and COPIES
// the drained batch into the packet, so a later registry edit cannot reach
// the packet. Device-free, under ~[gpu]; the GPU witness that a registered
// sprite material id survives the copy is in NriGraphPixelTest.cpp.
#include <Arcane/Base/ExtractAccounting.hpp>
#include <Arcane/Base/Log.hpp>                    // the degenerate-basis warning's logger
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/ExtractFrame.hpp>
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/Render/Grid2D.hpp>
#include <Arcane/Render/GpuSceneSync.hpp>         // the World fixture's GpuSceneSync
#include <Arcane/Render/GpuSceneTypes.hpp>        // the World fixture's GpuSceneMirror / GpuSceneStage
#include <Arcane/Render/RenderFramePacket.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderViewSettings.hpp>   // Ortho2DView
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>        // PhysicsWorld2D, Detail::Physics2D::Adopt / Access
#include <Arcane/Scene/SceneCamera.hpp>          // ActiveSceneCamera, ActivePerspectiveSceneCamera
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include "Helpers/ConstantScan.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // GpuSceneSyncTest.cpp's World (that copy is not visible to this TU), plus
    // AddSprite.
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry>            meshes;
        std::unordered_map<Arcane::Guid, Arcane::ResolvedMeshMaterial> materials;
        std::uint64_t meshGen = 1;   // the fixture plays MeshCache: MeshTable::generation, bumped on a re-publish
        Astra::Entity root{};
        Arcane::GpuSceneMirror mirror;
        Arcane::GpuSceneStage  stage;
        std::uint64_t          deviceGen = 0;   // what the device side last synced; 0 = never

        World()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes, &meshGen });
            reg.SetResource<Arcane::MeshMaterialTable>(Arcane::MeshMaterialTable{ &materials });
        }
        Arcane::Guid Mesh(std::uint32_t sections = 1, Arcane::Guid slotMaterial = {})
        {
            const Arcane::Guid id = Arcane::Guid::Generate();
            Arcane::MeshEntry entry;
            entry.data   = Arcane::BuildCube(2.0f);
            entry.bounds = Arcane::ComputeMeshBounds(entry.data);
            const std::uint32_t per = static_cast<std::uint32_t>(entry.data.indices.size()) / sections;
            entry.data.sections.clear();
            for (std::uint32_t s = 0; s < sections; ++s)
            {
                entry.data.sections.push_back(Arcane::MeshSection{ std::string(), s * per, per, s });
                entry.slots.push_back(Arcane::MeshSlot{ std::string(), slotMaterial });
            }
            meshes.emplace(id, entry);
            return id;
        }
        Astra::Entity Spawn(glm::vec3 pos, Arcane::Guid mesh, Arcane::Guid overrideMat = {})
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t; t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ mesh, overrideMat });
            return e;
        }
        // One host frame: propagate, bounds, sync (the device side "applies" by stamping the generation).
        void Frame()
        {
            Arcane::TransformPropagationSystem{}(reg);
            Arcane::BoundsSystem{}(reg);
            Arcane::GpuSceneSync(reg, mirror, deviceGen, stage);
            deviceGen = mirror.generation;
        }
        const Arcane::GpuInstance* Staged(std::uint32_t row) const
        {
            for (std::size_t i = 0; i < stage.rows.size(); ++i)
                if (stage.rows[i] == row) return &stage.values[i];
            return nullptr;
        }
        std::vector<const Arcane::GpuInstance*> StagedAll(std::uint32_t row) const
        {
            std::vector<const Arcane::GpuInstance*> out;
            for (std::size_t i = 0; i < stage.rows.size(); ++i)
                if (stage.rows[i] == row) out.push_back(&stage.values[i]);
            return out;
        }
        std::uint32_t RowOf(Astra::Entity e, std::uint32_t section = 0) const
        {
            const Arcane::GpuSceneMirror::Rows* r = mirror.slots.TryGet(e);
            REQUIRE(r);
            return r->first + section;
        }

        Astra::Entity sprite{};
        Astra::Entity AddSprite(glm::vec3 pos)
        {
            sprite = reg.CreateEntity();
            Arcane::Transform t;
            t.position = pos;
            reg.AddComponent<Arcane::Transform>(sprite, t);
            reg.SetParent(sprite, root);
            reg.AddComponent<Arcane::SpriteRenderer>(sprite, Arcane::SpriteRenderer{});
            return sprite;
        }
        void AddOrthoCamera()
        {
            Arcane::Camera cam;
            cam.active = true;
            cam.projection = Arcane::CameraProjection::Orthographic;
            cam.orthographicSize = 2.0f;
            reg.AddComponent<Arcane::Camera>(root, cam);
        }
        void AddPerspectiveCamera()
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t;
            t.position = { 0.0f, 0.0f, 5.0f };
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            Arcane::Camera cam;
            cam.active = true;
            cam.projection = Arcane::CameraProjection::Perspective;
            cam.fovYDegrees = 50.0f;
            cam.nearZ = 0.05f;
            cam.farZ = 500.0f;
            reg.AddComponent<Arcane::Camera>(e, cam);
        }
    };

    // MeshCacheTest's warn counter, narrowed to the degenerate-basis line.
    // The callback sink is the seam those camera-adjacent warn tests use:
    // attach to Log::Engine(), count warn payloads, detach on the way out.
    struct DegenerateBasisWarns
    {
        int count = 0;
        std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;

        DegenerateBasisWarns()
        {
            sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
                [this](const spdlog::details::log_msg& m)
                {
                    if (m.level != spdlog::level::warn)
                        return;
                    const std::string text(m.payload.data(), m.payload.size());
                    if (text.find("degenerate basis") != std::string::npos)
                        ++count;
                });
            Arcane::Log::Engine()->sinks().push_back(sink);
        }
        ~DegenerateBasisWarns()
        {
            auto& sinks = Arcane::Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
        }
        DegenerateBasisWarns(const DegenerateBasisWarns&) = delete;
        DegenerateBasisWarns& operator=(const DegenerateBasisWarns&) = delete;
    };
}

TEST_CASE("extract copies one sprite quad and ignores a later edit", "[nri][extract]")
{
    World world;
    Astra::Entity sprite = world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);
    // Task 6's BuildVisibleSet only inserts entities that already have
    // WorldBounds (VisibilitySystem.hpp:124-128). RenderSubmissionSystem
    // drops anything that set does not contain (RenderSystems.hpp:62-63).
    Arcane::BoundsSystem{}(world.reg);

    auto batcher = Arcane::Batcher2D::Create();
    batcher->Begin(128u, 128u);
    Arcane::RenderContext2D ctx;
    ctx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));

    Arcane::ResetExtractFrameCallCount();
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    // Ortho view so Task 6's visible set still contains this quad.
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    CHECK(packet.origin.translation == glm::vec3(0.0f));
    CHECK(packet.viewport == req.viewport);
    CHECK(packet.batchVertices.size() == 4u);
    CHECK(Arcane::ExtractFrameCallCount() == 1u);
    const glm::vec3 copied = packet.batchVertices[0].pos;

    world.reg.GetComponent<Arcane::Transform>(sprite)->position = { 50.0f, 50.0f, 0.0f };
    CHECK(packet.batchVertices[0].pos == copied);
    CHECK(Arcane::ExtractFrameCallCount() == 1u);
}

TEST_CASE("extract keeps a registered sprite material id", "[nri][extract]")
{
    World world;
    auto batcher = Arcane::Batcher2D::Create();
    REQUIRE(batcher != nullptr);

    auto templ = std::make_shared<Arcane::MaterialTemplate>(Arcane::MaterialTemplate::Build(
        "extract-id", 1, {}));
    auto instance = std::make_shared<Arcane::MaterialInstance>(
        std::shared_ptr<const Arcane::MaterialTemplate>(templ));
    Arcane::Material2DDesc desc;
    desc.templ = templ;
    desc.instance = instance;
    desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 5, 6, 7, 8 });
    const std::uint16_t id = batcher->RegisterMaterial(desc);
    REQUIRE(id != Arcane::Batcher2D::kInvalidMaterialId);
    REQUIRE(id >= 3u);
    REQUIRE(id != Arcane::Batcher2D::kMaterialSprite);
    REQUIRE(batcher->MaterialDesc(id) != nullptr);

    batcher->Begin(128u, 128u);
    Arcane::RenderContext2D ctx;
    ctx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));

    const Arcane::Guid matGuid = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, std::uint16_t> ids;
    ids.emplace(matGuid, id);
    world.reg.SetResource<Arcane::SpriteMaterialTable>(Arcane::SpriteMaterialTable{ &ids });

    Astra::Entity sprite = world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::SpriteRenderer& rend = *world.reg.GetComponent<Arcane::SpriteRenderer>(sprite);
    rend.material = matGuid;
    rend.tint = { 1.0f, 0.0f, 0.0f, 1.0f };
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.batchSpans.size() == 1u);
    CHECK(packet.batchSpans[0].material == id);
    CHECK(packet.batchSpans[0].material != Arcane::Batcher2D::kMaterialSprite);
    CHECK(packet.batchSpans[0].worldSpace);
    CHECK(packet.batchVertices.size() == 4u);
}

TEST_CASE("extract publishes one mesh row and ignores a later transform edit", "[nri][extract][gpuscene]")
{
    World world;
    const Arcane::Guid mesh = world.Mesh(1);
    Astra::Entity spawned = world.Spawn({ 0.0f, 0.0f, 0.0f }, mesh);
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.mesh.stats.total >= 1u);
    const std::uint32_t total = packet.mesh.stats.total;
    world.reg.GetComponent<Arcane::Transform>(spawned)->position = { 40.0f, 0.0f, 0.0f };
    CHECK(packet.mesh.stats.total == total);
    CHECK(packet.visible.entries.size() >= 1u);
}

TEST_CASE("meshes cull against the perspective mesh view, not the orthographic sprite view", "[nri][extract][gpuscene]")
{
    auto coarse = [](glm::vec3 position)
    {
        World world;
        world.AddOrthoCamera();
        world.AddPerspectiveCamera();
        world.Spawn(position, world.Mesh(1));
        Arcane::TransformPropagationSystem{}(world.reg);
        Arcane::BoundsSystem{}(world.reg);
        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { 200u, 100u };
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
        REQUIRE(packet.mesh.stats.total == 1u);
        return packet.mesh.stats.coarseVisible;
    };
    // x = 30 at depth 55: inside the perspective frustum (half-width about 51 m),
    // outside the orthographic view (half-width 4 m).
    CHECK(coarse({ 30.0f, 0.0f, -50.0f }) == 1u);
    // z = -600: inside the orthographic slab (+-render.ortho2D.depthRange, 1000 m),
    // 605 m from the perspective eye, past its far plane.
    CHECK(coarse({ 0.0f, 0.0f, -600.0f }) == 0u);
}

TEST_CASE("the scene-save cover invalidates the GPU-scene mirror after its extract", "[nri][extract][gpuscene]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorApp.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::size_t fn = text.find("bool EditorApp::CaptureGraphViewportPng(");
    REQUIRE(fn != std::string::npos);
    const std::size_t end = text.find("\n    }\n", fn);   // four-space-indented member definitions
    REQUIRE(end != std::string::npos);
    const std::string body = text.substr(fn, end - fn);
    const std::size_t extract = body.find("ExtractFrame(");
    const std::size_t render = body.find("RenderFrameOffscreen(");
    const std::size_t invalidate = body.find("GpuSceneInvalidate(m_runtime->Registry())");
    REQUIRE(extract != std::string::npos);
    REQUIRE(render != std::string::npos);
    REQUIRE(invalidate != std::string::npos);
    CHECK(extract < render);
    CHECK(render < invalidate);
    // Before the early return on a non-Presented outcome, so every outcome invalidates.
    const std::size_t early = body.find("return false;", render);
    CHECK((early == std::string::npos || invalidate < early));
}

static void CheckView(const Arcane::ViewTransform& a, const Arcane::ViewTransform& b)
{
    CHECK(a.view == b.view);
    CHECK(a.projection == b.projection);
    CHECK(a.viewport == b.viewport);
}

TEST_CASE("runtime extract keeps the ortho sprite view and the perspective mesh view", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 200u, 100u };
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    const std::optional<Arcane::SceneCameraView> ortho = Arcane::ActiveSceneCamera(world.reg, req.viewport);
    const std::optional<Arcane::ViewTransform> persp = Arcane::ActivePerspectiveSceneCamera(world.reg, req.viewport);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    REQUIRE(ortho.has_value());
    REQUIRE(persp.has_value());
    CheckView(packet.spriteView->view, ortho->view);
    CHECK(packet.spriteView->worldCenter == ortho->worldCenter);
    CHECK(packet.spriteView->halfHeight == ortho->halfHeight);
    CheckView(*packet.meshView, *persp);
    CHECK(packet.spriteViewFromScene);
}

TEST_CASE("with no scene camera the sprite view is the host's stored view, not identity", "[nri][extract]")
{
    World world;   // no Camera component
    Arcane::RenderContext2D ctx;
    ctx.view = Arcane::Ortho2DView({ 3.0f, 1.0f }, 2.0f, { 64u, 64u });
    // SetResource takes T&&. With the template argument explicit, an lvalue
    // does not bind; the move leaves this trivial view intact for the check.
    world.reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 64u, 64u };
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    CheckView(packet.spriteView->view, ctx.view);
    CHECK_FALSE(packet.spriteViewFromScene);
}

TEST_CASE("an editorView is the world view and is not replaced by a scene camera", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    world.reg.GetComponent<Arcane::Transform>(world.root)->position = { 4.0f, 0.0f, 0.0f };
    const std::optional<Arcane::SceneCameraView> scene = Arcane::ActiveSceneCamera(world.reg, { 64u, 64u });
    REQUIRE(scene.has_value());
    Arcane::ViewTransform editor = scene->view;
    editor.view[3][0] += 3.0f;
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 64u, 64u };
    req.editorView = editor;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    CHECK(packet.spriteView->view.view == editor.view);
    CHECK(packet.spriteView->view.projection == editor.projection);
    CHECK(packet.meshView->view == editor.view);
    CHECK(packet.meshView->projection == editor.projection);
    CHECK(packet.spriteView->worldCenter == glm::vec2(0.0f));
    CHECK(scene->worldCenter != packet.spriteView->worldCenter);
}

TEST_CASE("extract publishes a pick drawable that survives a later edit", "[nri][extract]")
{
    World world;
    Astra::Entity sprite = world.AddSprite({ 1.0f, 2.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);
    // BuildVisibleSet only inserts entities that already have WorldBounds,
    // and CollectPickables culls sprites against that set.
    Arcane::BoundsSystem{}(world.reg);
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 4.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.pickDrawables.size() == 1u);
    CHECK(packet.pickDrawables[0].entity == sprite);
    const glm::vec3 corner = packet.pickDrawables[0].corners[0];
    world.reg.GetComponent<Arcane::Transform>(sprite)->position = { 30.0f, 30.0f, 0.0f };
    CHECK(packet.pickDrawables[0].corners[0] == corner);
}

// The buffer a vector owned after the first extract. The second extract of
// that same ring slot must keep it: data() stable and capacity not grown.
template <typename T>
struct KeptBuffer
{
    const T* data = nullptr;
    std::size_t capacity = 0;
    std::size_t size = 0;
};

template <typename T>
KeptBuffer<T> TakeBuffer(const std::vector<T>& v)
{
    return { v.data(), v.capacity(), v.size() };
}

template <typename T>
void CheckBufferKept(const std::vector<T>& v, const KeptBuffer<T>& before)
{
    REQUIRE(before.size > 0u);
    CHECK(v.size() == before.size);
    CHECK(v.capacity() >= before.capacity);
    CHECK(v.capacity() == before.capacity);
    CHECK(v.data() == before.data);
}

TEST_CASE("a second extract keeps the packet vector capacity", "[nri][extract]")
{
    Arcane::LatchFramesInFlight();
    const std::uint32_t depth = Arcane::FramesInFlight();
    REQUIRE(depth >= 2u);

    World world;
    world.AddSprite({ 0.0f, 0.0f, 0.0f });
    world.Spawn({ 0.0f, 0.0f, 0.0f }, world.Mesh(1));
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    auto batcher = Arcane::Batcher2D::Create();
    REQUIRE(batcher != nullptr);
    Arcane::RenderContext2D ctx;
    ctx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* imgui = ImGui::CreateContext();
    REQUIRE(imgui != nullptr);
    ImGui::SetCurrentContext(imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(128.0f, 128.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(64.0f, 64.0f), ImGuiCond_Always);
    ImGui::Begin("capacity");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(3), ImVec2(0.0f, 0.0f), ImVec2(8.0f, 8.0f));
    ImGui::End();
    ImGui::Render();
    ImDrawData* draw = ImGui::GetDrawData();
    REQUIRE(draw != nullptr);
    REQUIRE(draw->CmdLists.Size >= 1);

    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    Arcane::PostChainDesc post;
    post.passes.push_back({});
    post.passes.back().vsBytes = bytes;
    post.passes.back().inputs.push_back(0u);

    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    req.imgui = draw;
    req.post = &post;
    Arcane::OverlayLine gizmo{};
    gizmo.a = { 0.0f, 0.0f, 0.0f };
    gizmo.b = { 8.0f, 0.0f, 0.0f };
    gizmo.color = { 1.0f, 0.0f, 0.0f, 1.0f };
    gizmo.thickness = 1.0f;
    gizmo.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    gizmo.space = Arcane::OverlayColorSpace::DisplayReferred;
    gizmo.screenSpace = true;
    req.gizmoPrims.AddLine(gizmo);

    Arcane::PacketRing ring;
    const Arcane::RenderFramePacket* slot = nullptr;
    KeptBuffer<Arcane::Batch2DVertex> batchVertices;
    KeptBuffer<std::uint32_t> batchIndices;
    KeptBuffer<Arcane::Batch2DDrawSpan> batchSpans;
    KeptBuffer<Arcane::OverlayPrimitive> prims;
    KeptBuffer<Arcane::PickDrawable> pick;
    KeptBuffer<Arcane::VisibleEntry> entries;
    KeptBuffer<std::uint64_t> members;
    KeptBuffer<Arcane::GpuBatchDraw> batches;
    KeptBuffer<Arcane::DrawIndexedArgs> args;
    KeptBuffer<Arcane::GpuCullBatch> cullBatches;
    KeptBuffer<std::uint32_t> visibleIndices;
    KeptBuffer<std::uint32_t> oracleIndices;
    KeptBuffer<std::uint32_t> stageRows;
    KeptBuffer<Arcane::GpuInstance> stageValues;
    KeptBuffer<Arcane::PostChainPassDesc> passes;
    KeptBuffer<std::uint32_t> postInputs;
    KeptBuffer<Arcane::ImDrawListSnapshot> imguiLists;
    KeptBuffer<Arcane::ImDrawSnapshotVert> imguiVertices;
    KeptBuffer<std::uint16_t> imguiIndices;
    KeptBuffer<Arcane::ImDrawListSnapshot::Cmd> imguiCmds;
    KeptBuffer<void*> imguiTextures;

    for (std::uint32_t i = 0; i < depth; ++i)
    {
        batcher->Begin(req.viewport.x, req.viewport.y);
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
        if (i != 0)
            continue;
        slot = &packet;
        batchVertices = TakeBuffer(packet.batchVertices);
        batchIndices = TakeBuffer(packet.batchIndices);
        batchSpans = TakeBuffer(packet.batchSpans);
        prims = TakeBuffer(packet.overlay.prims);
        pick = TakeBuffer(packet.pickDrawables);
        entries = TakeBuffer(packet.visible.entries);
        members = TakeBuffer(packet.visible.members);
        batches = TakeBuffer(packet.mesh.batches);
        args = TakeBuffer(packet.mesh.args);
        cullBatches = TakeBuffer(packet.mesh.cullBatches);
        visibleIndices = TakeBuffer(packet.mesh.visibleIndices);
        oracleIndices = TakeBuffer(packet.mesh.oracleVisibleIndices);
        stageRows = TakeBuffer(packet.mesh.stage.rows);
        stageValues = TakeBuffer(packet.mesh.stage.values);
        passes = TakeBuffer(packet.post.passes);
        REQUIRE(!packet.post.passes.empty());
        postInputs = TakeBuffer(packet.post.passes[0].inputs);
        imguiLists = TakeBuffer(packet.imgui.lists);
        REQUIRE(packet.imgui.listCount >= 1u);
        imguiVertices = TakeBuffer(packet.imgui.lists[0].vertices);
        imguiIndices = TakeBuffer(packet.imgui.lists[0].indices);
        imguiCmds = TakeBuffer(packet.imgui.lists[0].cmds);
        imguiTextures = TakeBuffer(packet.imgui.textures);
    }
    REQUIRE(slot != nullptr);

    batcher->Begin(req.viewport.x, req.viewport.y);
    const Arcane::RenderFramePacket& again = Arcane::ExtractFrame(world.reg, req, ring);
    CHECK(&again == slot);
    CheckBufferKept(again.batchVertices, batchVertices);
    CheckBufferKept(again.batchIndices, batchIndices);
    CheckBufferKept(again.batchSpans, batchSpans);
    CheckBufferKept(again.overlay.prims, prims);
    CheckBufferKept(again.pickDrawables, pick);
    CheckBufferKept(again.visible.entries, entries);
    CheckBufferKept(again.visible.members, members);
    CheckBufferKept(again.mesh.batches, batches);
    CheckBufferKept(again.mesh.args, args);
    CheckBufferKept(again.mesh.cullBatches, cullBatches);
    CheckBufferKept(again.mesh.visibleIndices, visibleIndices);
    CheckBufferKept(again.mesh.oracleVisibleIndices, oracleIndices);
    CheckBufferKept(again.mesh.stage.rows, stageRows);
    CheckBufferKept(again.mesh.stage.values, stageValues);
    CheckBufferKept(again.post.passes, passes);
    REQUIRE(!again.post.passes.empty());
    CheckBufferKept(again.post.passes[0].inputs, postInputs);
    CheckBufferKept(again.imgui.lists, imguiLists);
    REQUIRE(again.imgui.listCount >= 1u);
    CheckBufferKept(again.imgui.lists[0].vertices, imguiVertices);
    CheckBufferKept(again.imgui.lists[0].indices, imguiIndices);
    CheckBufferKept(again.imgui.lists[0].cmds, imguiCmds);
    CheckBufferKept(again.imgui.textures, imguiTextures);

    ImGui::DestroyContext(imgui);
    ImGui::SetCurrentContext(previous);
}

TEST_CASE("overlay tags: grid is depth-tested linear, physics camera and gizmo are always on top", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);

    Manifold2D::Physics::WorldDef wd;
    wd.gravityX = Manifold2D::Physics::Real(0);
    wd.gravityY = Manifold2D::Physics::Real(0);
    // PhysicsWorld2D owns the solver privately; tests adopt one (PhysicsPauseTest.cpp:25).
    world.reg.SetResource(Arcane::Detail::Physics2D::Adopt(
        std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));
    Manifold2D::Physics::PhysicsWorld& solver =
        *Arcane::Detail::Physics2D::Access::Solver(*world.reg.GetResource<Arcane::PhysicsWorld2D>());
    Manifold2D::Physics::BodyDef bd;
    bd.type = Manifold2D::Physics::BodyType::Static;
    bd.position = Manifold2D::Physics::Vec2(Manifold2D::Physics::Real(0), Manifold2D::Physics::Real(0));
    bd.shape = Manifold2D::Physics::MakeAabb(Manifold2D::Physics::Real(1), Manifold2D::Physics::Real(1));
    solver.AddBody(bd);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    req.showGrid2d = true;
    req.gridMinorAlpha = 0.35f;
    req.gridMajorAlpha = 0.55f;
    req.gridLineRgb = { 0.5f, 0.5f, 0.5f };
    req.gridLineThicknessPx = 1.0f;
    req.gridMaxLinesPerAxis = 4096;
    req.physicsDebug = true;
    req.physicsWholeWorld = true;
    req.cameraRect = true;
    Arcane::OverlayLine gizmo{};
    gizmo.a = { 0.0f, 0.0f, 0.0f };
    gizmo.b = { 10.0f, 0.0f, 0.0f };
    gizmo.color = { 1.0f, 0.0f, 0.0f, 1.0f };
    gizmo.thickness = 1.0f;
    gizmo.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    gizmo.space = Arcane::OverlayColorSpace::DisplayReferred;
    gizmo.screenSpace = true;
    req.gizmoPrims.AddLine(gizmo);

    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    bool sawGrid = false;
    bool sawGizmo = false;
    bool sawScreenLinear = false;
    for (const Arcane::OverlayPrimitive& prim : packet.overlay.prims)
    {
        if (prim.tag != Arcane::OverlayPrimitive::Tag::Line)
            continue;
        const Arcane::OverlayLine& line = prim.line;
        if (line.space == Arcane::OverlayColorSpace::Linear &&
            line.depth == Arcane::OverlayDepthClass::DepthTested &&
            line.screenSpace == false)
            sawGrid = true;
        if (line.space == Arcane::OverlayColorSpace::DisplayReferred)
        {
            sawGizmo = true;
            CHECK(line.depth == Arcane::OverlayDepthClass::AlwaysOnTop);
            CHECK(line.screenSpace == true);
        }
        if (line.space == Arcane::OverlayColorSpace::Linear &&
            line.depth == Arcane::OverlayDepthClass::AlwaysOnTop &&
            line.screenSpace == true)
            sawScreenLinear = true;
    }
    CHECK(sawGrid);
    CHECK(sawGizmo);
    CHECK(sawScreenLinear);
}

TEST_CASE("a selected body outlines only that body", "[nri][extract]")
{
    World world;
    Manifold2D::Physics::WorldDef wd;
    wd.gravityX = Manifold2D::Physics::Real(0);
    wd.gravityY = Manifold2D::Physics::Real(0);
    world.reg.SetResource(Arcane::Detail::Physics2D::Adopt(
        std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));
    Arcane::PhysicsWorld2D& physics = *world.reg.GetResource<Arcane::PhysicsWorld2D>();
    Manifold2D::Physics::PhysicsWorld& solver = *Arcane::Detail::Physics2D::Access::Solver(physics);
    auto addBox = [&](float x)
    {
        Manifold2D::Physics::BodyDef bd;
        bd.type = Manifold2D::Physics::BodyType::Static;
        bd.position = Manifold2D::Physics::Vec2(Manifold2D::Physics::Real(x), Manifold2D::Physics::Real(0));
        bd.shape = Manifold2D::Physics::MakeAabb(Manifold2D::Physics::Real(1), Manifold2D::Physics::Real(1));
        const Astra::Entity e = world.reg.CreateEntity();
        Arcane::Detail::Physics2D::Access::Entities(physics)[e] = solver.AddBody(bd);
        return e;
    };
    const Astra::Entity left = addBox(-6.0f);
    addBox(6.0f);
    // The Play rows render through the scene camera: the same 3 px/m framing
    // (orthographicSize is the half-height, SceneCamera.hpp:86).
    world.AddOrthoCamera();
    world.reg.GetComponent<Arcane::Camera>(world.root)->orthographicSize = 16.0f;

    // Physics lines are the screen-space, linear, always-on-top lines: no grid,
    // no camera rect, and no gizmo are requested here.
    auto sides = [&](std::optional<Astra::Entity> selected, bool wholeWorld, bool play = false)
    {
        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { 160u, 96u };
        req.playMode = play;
        if (!play)
            req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 16.0f, req.viewport);
        req.physicsDebug = true;
        req.physicsWholeWorld = wholeWorld;
        req.physicsSelectedBody = selected;
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
        int leftLines = 0;
        int rightLines = 0;
        for (const Arcane::OverlayPrimitive& prim : packet.overlay.prims)
        {
            if (prim.tag != Arcane::OverlayPrimitive::Tag::Line)
                continue;
            const Arcane::OverlayLine& line = prim.line;
            if (!line.screenSpace || line.space != Arcane::OverlayColorSpace::Linear
                || line.depth != Arcane::OverlayDepthClass::AlwaysOnTop)
                continue;
            if (0.5f * (line.a.x + line.b.x) < 80.0f)
                ++leftLines;
            else
                ++rightLines;
        }
        return std::pair<int, int>{ leftLines, rightLines };
    };

    const auto [selectedLeft, selectedRight] = sides(left, false);
    CHECK(selectedLeft > 0);
    CHECK(selectedRight == 0);
    const auto [noneLeft, noneRight] = sides(std::nullopt, false);
    CHECK(noneLeft == 0);
    CHECK(noneRight == 0);
    const auto [allLeft, allRight] = sides(std::nullopt, true);
    CHECK(allLeft > 0);
    CHECK(allRight > 0);
    // Play: the toggled whole-world overlay still draws, with no editor camera;
    // a selection alone draws nothing (PlanPhysicsOverlay's Edit-only outline).
    const auto [playAllLeft, playAllRight] = sides(std::nullopt, true, true);
    CHECK(playAllLeft > 0);
    CHECK(playAllRight > 0);
    const auto [playSelLeft, playSelRight] = sides(left, false, true);
    CHECK(playSelLeft == 0);
    CHECK(playSelRight == 0);
}

namespace
{
    // Alpha of the vertical grey grid line at world x, or nullopt when that
    // line was not emitted. Axes are coloured, so they do not match.
    std::optional<float> VerticalGreyAlpha(const Arcane::RenderFramePacket& packet, float worldX)
    {
        std::optional<float> alpha;
        float best = 1.0e9f;
        for (const Arcane::OverlayPrimitive& prim : packet.overlay.prims)
        {
            if (prim.tag != Arcane::OverlayPrimitive::Tag::Line)
                continue;
            const Arcane::OverlayLine& line = prim.line;
            if (line.screenSpace)
                continue;
            if (std::abs(line.a.x - line.b.x) > 1.0e-4f)
                continue;
            if (std::abs(line.color.r - 0.5f) > 1.0e-3f || std::abs(line.color.g - 0.5f) > 1.0e-3f
                || std::abs(line.color.b - 0.5f) > 1.0e-3f)
                continue;
            const float d = std::abs(line.a.x - worldX);
            if (d < best)
            {
                best = d;
                alpha = line.color.a;
            }
        }
        if (best > 1.0e-3f)
            return std::nullopt;
        return alpha;
    }

    // The width the overlay will rasterise: screen-space thickness is already
    // pixels; a world line is expanded in metres and projected.
    float OnScreenWidth(const Arcane::ViewTransform& view, const Arcane::OverlayLine& line)
    {
        if (line.screenSpace)
            return line.thickness;
        const glm::vec2 d = glm::vec2(line.b) - glm::vec2(line.a);
        const float len = glm::length(d);
        if (!(len > 0.0f))
            return 0.0f;
        const glm::vec2 n = glm::vec2(-d.y, d.x) / len * (line.thickness * 0.5f);
        const glm::vec3 mid = (line.a + line.b) * 0.5f;
        const glm::vec2 p0 = glm::vec2(view.WorldToScreen(mid + glm::vec3(n, 0.0f)));
        const glm::vec2 p1 = glm::vec2(view.WorldToScreen(mid - glm::vec3(n, 0.0f)));
        return glm::length(p1 - p0);
    }
}

TEST_CASE("extract: a non-default grid fade changes minor and major line alphas", "[nri][extract]")
{
    // 200 px over 2 m is 100 px/m. The 0.1 m level sits at 10 px, inside the
    // default 8..24 window and fully in once the window is 4..10. x = 1 m is
    // the decade above it (the major).
    auto alphas = [](float fadeIn, float fadeFull)
    {
        World world;
        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { 200u, 200u };
        req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 1.0f, req.viewport);
        req.showGrid2d = true;
        req.gridFadeInPx = fadeIn;
        req.gridFadeFullPx = fadeFull;
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
        const std::optional<float> minor = VerticalGreyAlpha(packet, 0.1f);
        const std::optional<float> major = VerticalGreyAlpha(packet, 1.0f);
        REQUIRE(minor.has_value());
        REQUIRE(major.has_value());
        return std::pair<float, float>{ *minor, *major };
    };

    const auto [minorDefault, majorDefault] = alphas(8.0f, 24.0f);
    const auto [minorCustom, majorCustom] = alphas(4.0f, 10.0f);
    CHECK(minorCustom != minorDefault);
    CHECK(majorCustom != majorDefault);
}

TEST_CASE("extract: a grid line stays 1 px wide at ppm 8, 32 and 128", "[nri][extract]")
{
    for (const float ppmWanted : { 8.0f, 32.0f, 128.0f })
    {
        World world;
        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { 256u, 256u };
        const float halfHeight = static_cast<float>(req.viewport.y) / (2.0f * ppmWanted);
        req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, halfHeight, req.viewport);
        req.showGrid2d = true;
        REQUIRE(Arcane::GridPixelsPerMetre(*req.editorView) == Catch::Approx(ppmWanted).margin(1.0e-3));

        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
        int measured = 0;
        for (const Arcane::OverlayPrimitive& prim : packet.overlay.prims)
        {
            if (prim.tag != Arcane::OverlayPrimitive::Tag::Line)
                continue;
            const Arcane::OverlayLine& line = prim.line;
            if (line.depth != Arcane::OverlayDepthClass::DepthTested)
                continue;
            ++measured;
            CHECK(OnScreenWidth(*req.editorView, line) == Catch::Approx(1.0f).margin(1.0e-3));
        }
        CHECK(measured > 0);
    }
}

TEST_CASE("extract copies two ImGui lists and keeps the post bytecode", "[nri][extract]")
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    REQUIRE(ctx != nullptr);
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(32.0f, 32.0f);
    // Before NewFrame, matching the live backend (ImGuiNri.cpp:119):
    // without RendererHasTextures NewFrame demands a built font atlas
    // (imgui_draw.cpp:2778).
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    ImGui::NewFrame();
    // First-frame windows are hidden unless both sizes are API-set
    // (imgui.cpp:8093-8095, :6394-6395); see Task 4's case.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(32.0f, 32.0f), ImGuiCond_Always);
    ImGui::Begin("one");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(42), ImVec2(0.0f, 0.0f), ImVec2(8.0f, 8.0f));
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(32.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(32.0f, 32.0f), ImGuiCond_Always);
    ImGui::Begin("two");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(7), ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f));
    ImGui::End();
    ImGui::Render();
    ImDrawData* live = ImGui::GetDrawData();
    REQUIRE(live != nullptr);
    REQUIRE(live->CmdLists.Size >= 2);
    REQUIRE(live->Textures != nullptr);

    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    Arcane::PostChainDesc desc;
    desc.passes.push_back({});
    desc.passes.back().vsBytes = bytes;

    World world;
    auto batcher = Arcane::Batcher2D::Create();
    batcher->Begin(32u, 32u);
    Arcane::RenderContext2D renderCtx;
    renderCtx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(std::move(renderCtx));

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 32u, 32u };
    req.imgui = live;
    req.post = &desc;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.imgui.listCount == static_cast<std::uint32_t>(live->CmdLists.Size));
    REQUIRE(packet.imgui.listCount >= 2u);
    CHECK(packet.imgui.hasTextures);
    REQUIRE(packet.imgui.textures.size() == static_cast<std::size_t>(live->Textures->Size));
    bool saw42 = false;
    bool saw7 = false;
    for (std::uint32_t li = 0; li < packet.imgui.listCount; ++li)
    {
        CHECK(packet.imgui.lists[li].vertices.size() ==
              static_cast<std::size_t>(live->CmdLists[static_cast<int>(li)]->VtxBuffer.Size));
        for (const Arcane::ImDrawListSnapshot::Cmd& cmd : packet.imgui.lists[li].cmds)
        {
            if (cmd.managed == nullptr && cmd.texture == static_cast<ImTextureID>(42))
                saw42 = true;
            if (cmd.managed == nullptr && cmd.texture == static_cast<ImTextureID>(7))
                saw7 = true;
        }
    }
    CHECK(saw42);
    CHECK(saw7);
    CHECK(packet.hasPost);
    desc = {};
    bytes.reset();
    REQUIRE(packet.post.passes.size() == 1u);
    REQUIRE(packet.post.passes[0].vsBytes);
    CHECK(packet.post.passes[0].vsBytes->size() == 4u);

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
}

static std::string MemberBody(const std::string& text, const char* signature)
{
    const std::size_t fn = text.find(signature);
    REQUIRE(fn != std::string::npos);
    const std::size_t end = text.find("\n    }\n", fn);
    REQUIRE(end != std::string::npos);
    return text.substr(fn, end - fn);
}

TEST_CASE("the editor builds its game HUD before extract and hands it to the request", "[nri][extract]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::string render = MemberBody(text, "void EditorApp::RenderSceneToViewport(");
    const std::size_t hud = render.find("RenderToDrawData()");
    const std::size_t extract = render.find("ExtractFrame(");
    REQUIRE(hud != std::string::npos);
    REQUIRE(extract != std::string::npos);
    CHECK(hud < extract);
    CHECK(render.find("req.gameUi = m_gameUiDrawData;") != std::string::npos);
    const std::string arm = MemberBody(text, "void EditorApp::ArmGraphViewportFrame(");
    CHECK(arm.find("RenderToDrawData()") == std::string::npos);
    CHECK(arm.find("vp.gameUi = m_gameUiDrawData;") != std::string::npos);
}

// What the host does before DrawUI: resolve, and push only a scene camera.
// The stored view is ClientRuntime::View(). There is no editor or runtime
// DrawUI seam in this suite, so the push itself lives here and the source
// scan below checks that both hosts do the same thing.
static Arcane::ViewTransform PushResolvedSpriteView(Astra::Registry& reg,
                                                    const Arcane::ExtractRequest& req,
                                                    Arcane::ViewTransform stored)
{
    const Arcane::ResolvedFrameViews views = Arcane::ResolveFrameViews(reg, req, false);
    if (views.spriteViewFromScene && views.spriteView)
        stored = views.spriteView->view;
    return stored;
}

TEST_CASE("frame view before DrawUI: the observer reads this frame's scene camera", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.reg.GetComponent<Arcane::Transform>(world.root)->position = { 1.0f, 2.0f, 0.0f };
    Arcane::TransformPropagationSystem{}(world.reg);

    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.playMode = true;   // Play: no editorView, so the scene camera wins

    const std::optional<Arcane::SceneCameraView> frame0 = Arcane::ActiveSceneCamera(world.reg, req.viewport);
    REQUIRE(frame0.has_value());
    // Last frame already pushed its scene camera. That is what DrawUI would
    // still be reading if this frame did not push before the HUD.
    Arcane::ViewTransform runtimeView = frame0->view;

    world.reg.GetComponent<Arcane::Transform>(world.root)->position = { 6.0f, -2.0f, 0.0f };
    Arcane::TransformPropagationSystem{}(world.reg);
    const std::optional<Arcane::SceneCameraView> frame1 = Arcane::ActiveSceneCamera(world.reg, req.viewport);
    REQUIRE(frame1.has_value());
    CHECK(frame1->view.view != runtimeView.view);

    const Arcane::ViewTransform observed = PushResolvedSpriteView(world.reg, req, runtimeView);

    Arcane::PacketRing ring;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    CHECK(packet.spriteViewFromScene);
    CheckView(packet.spriteView->view, frame1->view);
    // DrawUI's read is this frame's scene camera, not the view frame 0 pushed.
    CheckView(observed, frame1->view);
    CheckView(observed, packet.spriteView->view);
}

TEST_CASE("ResolveFrameViews matches the views ExtractFrame publishes", "[nri][extract]")
{
    auto check = [](World& world, const Arcane::ExtractRequest& req)
    {
        const Arcane::ResolvedFrameViews views = Arcane::ResolveFrameViews(world.reg, req, false);
        Arcane::PacketRing ring;
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
        CHECK(views.spriteViewFromScene == packet.spriteViewFromScene);
        CHECK(views.spriteView.has_value() == packet.spriteView.has_value());
        if (views.spriteView && packet.spriteView)
        {
            CheckView(views.spriteView->view, packet.spriteView->view);
            CHECK(views.spriteView->worldCenter == packet.spriteView->worldCenter);
            CHECK(views.spriteView->halfHeight == packet.spriteView->halfHeight);
        }
        CHECK(views.meshView.has_value() == packet.meshView.has_value());
        if (views.meshView && packet.meshView)
            CheckView(*views.meshView, *packet.meshView);
    };

    {
        World world;
        world.AddOrthoCamera();
        world.AddPerspectiveCamera();
        Arcane::TransformPropagationSystem{}(world.reg);
        Arcane::ExtractRequest req;
        req.viewport = { 200u, 100u };
        req.playMode = true;
        check(world, req);
    }
    {
        World world;
        Arcane::RenderContext2D ctx;
        ctx.view = Arcane::Ortho2DView({ 3.0f, 1.0f }, 2.0f, { 64u, 64u });
        world.reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));
        Arcane::ExtractRequest req;
        req.viewport = { 64u, 64u };
        check(world, req);
    }
    {
        World world;
        world.AddOrthoCamera();
        world.AddPerspectiveCamera();
        Arcane::ExtractRequest req;
        req.viewport = { 64u, 64u };
        req.editorView = Arcane::Ortho2DView({ 1.0f, 2.0f }, 3.0f, req.viewport);
        check(world, req);
        const Arcane::ResolvedFrameViews views = Arcane::ResolveFrameViews(world.reg, req, false);
        CHECK_FALSE(views.spriteViewFromScene);
    }
}

TEST_CASE("frame view before DrawUI: hosts resolve before the HUD", "[nri][extract]")
{
    std::ifstream editorIn(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    REQUIRE(editorIn.good());
    std::ostringstream editorSs;
    editorSs << editorIn.rdbuf();
    const std::string editor = editorSs.str();
    const std::string render = MemberBody(editor, "void EditorApp::RenderSceneToViewport(");
    const std::size_t resolve = render.find("ResolveFrameViews(m_runtime->Registry(), req, false)");
    const std::size_t hud = render.find("BeginGameUiFrame()");
    const std::size_t extract = render.find("ExtractFrame(");
    REQUIRE(resolve != std::string::npos);
    REQUIRE(hud != std::string::npos);
    REQUIRE(extract != std::string::npos);
    CHECK(resolve < hud);
    CHECK(hud < extract);
    const std::size_t guard = render.rfind("if (InPlayMode())", resolve);
    REQUIRE(guard != std::string::npos);
    CHECK(resolve - guard < 400u);
    const std::string refresh = MemberBody(editor, "void EditorApp::RefreshSceneResolution(");
    CHECK(refresh.find("ActiveSceneCamera(") == std::string::npos);
    CHECK(refresh.find("ResolveFrameViews(") == std::string::npos);
    CHECK(refresh.find("m_camera.Resolve(ViewportSize())") != std::string::npos);

    std::ifstream runtimeIn(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp");
    REQUIRE(runtimeIn.good());
    std::ostringstream runtimeSs;
    runtimeSs << runtimeIn.rdbuf();
    const std::string runtime = runtimeSs.str();
    const std::size_t buildHud = runtime.find("void BuildHud(");
    const std::size_t prepare = runtime.find("void PrepareFrame(", buildHud);
    REQUIRE(buildHud != std::string::npos);
    REQUIRE(prepare != std::string::npos);
    const std::string hudBody = runtime.substr(buildHud, prepare - buildHud);
    const std::size_t runtimeResolve = hudBody.find("ResolveFrameViews(io.runtime->Registry(), viewReq, false)");
    const std::size_t drawUi = hudBody.find("DrawUIAll()");
    REQUIRE(runtimeResolve != std::string::npos);
    REQUIRE(drawUi != std::string::npos);
    CHECK(runtimeResolve < drawUi);

    std::ifstream extractIn(Arcane::Test::RepoRoot() / "Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp");
    REQUIRE(extractIn.good());
    std::ostringstream extractSs;
    extractSs << extractIn.rdbuf();
    const std::string extractCpp = extractSs.str();
    CHECK(extractCpp.find("ResolveFrameViews(reg, req, true)") != std::string::npos);
}

// Before the host resolved ahead of DrawUI, one Play frame called
// ActivePerspectiveSceneCamera once, so a persistent degenerate basis
// warned once. The early resolve and ExtractFrame both resolve that
// camera now. The early call passes warn false; the extract keeps the
// warning. Two frames still warn twice: the line is not latched for
// the process.
TEST_CASE("a Play frame warns once for a degenerate perspective basis", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    Astra::Entity cam = world.reg.CreateEntity();
    Arcane::Transform t;
    t.position = { 0.0f, 0.0f, 5.0f };
    t.scale = glm::vec3(0.0f);
    world.reg.AddComponent<Arcane::Transform>(cam, t);
    world.reg.SetParent(cam, world.root);
    Arcane::Camera c;
    c.active = true;
    c.projection = Arcane::CameraProjection::Perspective;
    world.reg.AddComponent<Arcane::Camera>(cam, c);
    Arcane::TransformPropagationSystem{}(world.reg);

    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.playMode = true;

    DegenerateBasisWarns warns;

    const Arcane::ResolvedFrameViews early = Arcane::ResolveFrameViews(world.reg, req, false);
    CHECK(early.meshView.has_value());
    CHECK(warns.count == 0);

    Arcane::PacketRing ring;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    CHECK(packet.meshView.has_value());
    CHECK(warns.count == 1);

    const Arcane::ResolvedFrameViews earlyAgain = Arcane::ResolveFrameViews(world.reg, req, false);
    CHECK(earlyAgain.meshView.has_value());
    CHECK(warns.count == 1);
    Arcane::PacketRing ring2;
    const Arcane::RenderFramePacket& packet2 = Arcane::ExtractFrame(world.reg, req, ring2);
    CHECK(packet2.meshView.has_value());
    CHECK(warns.count == 2);
}
