#include <Arcane/Render/ExtractFrame.hpp>

// Before Log.hpp: this header includes NRI.h, and windows.h (pulled by the
// logger) #defines ERROR. GetRenderOverlayHooks is how extract copies ImGui
// draw data without including an ImGui header (ruling PR-1).
#include <Arcane/Render/Nri/RenderOverlayNode.hpp>

#include <Arcane/Base/ExtractAccounting.hpp>
#include <Arcane/Base/Log.hpp>                   // ARC_WARN (no-camera / multi-camera, once)
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/GpuSceneSync.hpp>        // GpuSceneSync, BuildGpuSceneFrame, MeshSectionRanges
#include <Arcane/Render/Grid2D.hpp>
#include <Arcane/Render/OverlayBatchCollector.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/RenderSystems.hpp>       // RenderSubmissionSystem
#include <Arcane/Render/SpriteGeometry.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsQuery.hpp>         // FillPhysicsDebug2D, FillColliderSilhouettes2D
#include <Arcane/Scene/SceneCamera.hpp>          // ActiveSceneCamera, ActivePerspectiveSceneCamera
#include <Arcane/Scene/SceneResources.hpp>       // RenderContext2D, MeshTable

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Arcane
{
    // Defined in Nri/GpuScene.cpp. Declared here, not via GpuScene.hpp: that
    // header includes NRI.h, and this TU already pulled windows.h through
    // Astra (the ERROR macro would corrupt nri::Message::ERROR).
    std::uint64_t GpuSceneSyncedGeneration(const GpuScene* device) noexcept;

    namespace
    {
        // PublishUiSnapshot warns once per process per list. Tests reset
        // these; ArcaneTests runs in random order.
        bool g_warnedUiSnapshotImgui = false;
        bool g_warnedUiSnapshotGameUi = false;

        // GpuSceneHost.hpp's SameView. That header is ArcaneClient's;
        // ArcaneRender does not include it.
        [[nodiscard]] bool SameView(const ViewTransform& a, const ViewTransform& b) noexcept
        {
            return a.view == b.view && a.projection == b.projection && a.viewport == b.viewport;
        }

        // False from the snapshot hook means a UserCallback command was skipped
        // and the rest of the draw data was kept. Warn once per process per list.
        void PublishUiSnapshot(bool (*snapshot)(const ImDrawData*, ImDrawSnapshot&),
                               const ImDrawData* src, ImDrawSnapshot& dest,
                               const char* listName, bool& warned)
        {
            if (snapshot(src, dest))
                return;
            if (warned)
                return;
            warned = true;
            ARC_WARN("extract skipped an ImGui UserCallback command in {}; the rest of that draw data was kept",
                     listName);
        }

        // The sprite batch, copied. The host already called Begin, SetGlobals
        // and SetRenderContext on its persistent batcher, and the sprite
        // materials are registered on that batcher, so extract submits into
        // it rather than a fresh one: Begin would clear the host's geometry
        // and reset its view-projection, SetGlobals(GlobalParams{}) would wipe
        // Time/DeltaTime, and QuadWorld on a fresh batcher rewrites every
        // registered id to kMaterialSprite. A null batcher submits nothing
        // and leaves the packet's batch empty.
        void ExtractSprites(Astra::Registry& reg, RenderFramePacket& packet)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            Batcher2D* batcher = ctx ? ctx->batcher : nullptr;
            if (!batcher)
                return;

            // The sprite view the caller published: scene camera, editor view,
            // or the host's stored view. Identity only when none of those exist.
            batcher->SetViewProjection(packet.spriteView
                ? packet.spriteView->view.ViewProjection()
                : glm::mat4{ 1.0f });   // does not clear geometry
            RenderSubmissionSystem{}(reg);
            const Batch2DDrained drained = batcher->Drain();
            packet.batchVertices.assign(drained.vertices.begin(), drained.vertices.end());
            packet.batchIndices.assign(drained.indices.begin(), drained.indices.end());
            packet.batchSpans.assign(drained.spans.begin(), drained.spans.end());
            if (drained.globals)
                packet.globals = *drained.globals;
            packet.viewProjection = drained.viewProjection;
        }

        // passes.size() after this is the live count. resize keeps a pass
        // element when the chain length does not change, and inputs.assign
        // reuses that element's buffer. A shorter chain drops the tail.
        void CopyPostInPlace(PostChainDesc& dst, const PostChainDesc& src)
        {
            dst.templ = src.templ;
            dst.instance = src.instance;
            dst.chainInputSlots = src.chainInputSlots;
            dst.passes.resize(src.passes.size());
            for (std::size_t i = 0; i < src.passes.size(); ++i)
            {
                dst.passes[i].vsBytes = src.passes[i].vsBytes;
                dst.passes[i].psBytes = src.passes[i].psBytes;
                dst.passes[i].inputs.assign(src.passes[i].inputs.begin(), src.passes[i].inputs.end());
            }
        }
    }

    namespace Detail
    {
        void ResetExtractUiSnapshotWarnForTest() noexcept
        {
            g_warnedUiSnapshotImgui = false;
            g_warnedUiSnapshotGameUi = false;
        }
    }

    void CollectPickables(Astra::Registry& registry, std::vector<PickDrawable>& out)
    {
        // ---- PASS 1: sprites -------------------------------------------------
        // The DRAWN set, through the DRAWN corner rule. The view filter is
        // RenderSystems.hpp's own (a Hidden sprite is not drawn, so it is not
        // pickable either), and base size + pivot come from the SpriteTable
        // exactly as submission resolves them -- an unresolved or non-Rect
        // sprite is a 1x1 m quad at the centre pivot -- so the silhouette
        // keeps matching the drawn quad.
        {
            const SpriteTable* spriteTable = registry.GetResource<SpriteTable>();
            const VisibleSet* vis = MainVisibleSet(registry);   // nullptr: cull nothing (spec s4)
            auto spriteView = registry.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>();
            spriteView.ForEach([&](Astra::Entity e, const WorldTransform& xf, const SpriteRenderer& sp)
            {
                if (vis && !vis->Contains(e))
                    return;   // off-screen this frame (the CPU coarse stage)

                const SpriteEntry* entry =
                    (sp.shape == SpriteShape::Rect && spriteTable)
                        ? spriteTable->Resolve(sp.sprite)
                        : nullptr;
                const glm::vec2 baseSize = entry ? entry->sizeMeters : glm::vec2(1.0f);
                const glm::vec2 pivot    = entry ? entry->pivot      : glm::vec2(0.5f);

                // THE ONE CORNER RULE (SpriteGeometry.hpp): the same four world
                // points the sprite submission draws, so the silhouette matches
                // the drawn quad under ANY projection -- a mirrored or off-centre
                // pivot included (the retired "centre + angle" derivation placed
                // a mirrored off-centre pivot on the wrong side).
                PickDrawable d;
                d.entity  = e;
                d.kind    = PickDrawable::Kind::Quad;
                d.corners = SpriteWorldQuad(xf.matrix, baseSize, pivot).corners;
                out.push_back(d);
            });
        }

        // ---- PASS 2: physics colliders ---------------------------------------
        // One PickDrawable per collider fixture on every live tracked body, in
        // the order the physics query proxy yields them. The drawable index IS
        // the hit-proxy id (id = index+1), so that order must be DETERMINISTIC:
        // FillColliderSilhouettes2D walks an archetype-stable View of
        // Collider2D + PhysicsBodyRef2D, never the world's entity-to-body hash
        // map. Render names no solver type here: the proxy reads the body pose
        // and applies the baked appliedScale, and hands back world-space
        // shapes (metres, world-sense angle, the entity's world z). No physics
        // module registered, or no physics world on this registry, yields no
        // silhouettes -- NOT an early return: the meshes below are collected
        // regardless.
        {
            std::vector<PhysicsColliderSilhouette2D> silhouettes;
            FillColliderSilhouettes2D(registry, silhouettes);
            for (const PhysicsColliderSilhouette2D& s : silhouettes)
            {
                PickDrawable d;
                d.entity = s.entity;
                d.center = s.center;
                d.angle  = s.angle;
                switch (s.kind)
                {
                case PhysicsColliderSilhouette2D::Kind::Circle:
                    d.kind   = PickDrawable::Kind::Circle;
                    d.radius = s.radius;
                    break;
                case PhysicsColliderSilhouette2D::Kind::Capsule:
                    d.kind    = PickDrawable::Kind::Capsule;
                    d.halfLen = s.halfLen;
                    d.radius  = s.radius;
                    break;
                case PhysicsColliderSilhouette2D::Kind::Box:
                    d.kind        = PickDrawable::Kind::Box;
                    d.halfExtents = s.halfExtents;
                    break;
                }
                out.push_back(d);
            }
        }

        // ---- PASS 3: meshes -------------------------------------------------
        // One drawable per MeshRenderer entity that GpuSceneSync would stage
        // (Render/GpuSceneSync.hpp: WorldTransform + MeshRenderer, not Hidden,
        // resolved through the MeshTable to a mesh with sections). The whole mesh is
        // one silhouette -- sections carry materials, not identity -- so an entity
        // gets ONE id whatever its section count.
        {
            const MeshTable* meshTable = registry.GetResource<MeshTable>();
            const VisibleSet* vis = MainVisibleSet(registry);   // nullptr: cull nothing (spec s4)
            auto meshView = registry.CreateView<const WorldTransform, const MeshRenderer, Astra::Not<Hidden>>();
            meshView.ForEach([&](Astra::Entity e, const WorldTransform& xf, const MeshRenderer& mr)
            {
                if (vis && !vis->Contains(e))
                    return;   // off-screen this frame (the CPU coarse stage)

                const MeshEntry* entry = meshTable ? meshTable->Resolve(mr.mesh) : nullptr;
                if (!entry || entry->data.sections.empty())
                    return;
                PickDrawable d;
                d.entity = e;
                d.kind   = PickDrawable::Kind::Mesh;
                d.world  = xf.matrix;
                d.mesh   = mr.mesh;
                out.push_back(d);
            });
        }
    }

    ResolvedFrameViews ResolveFrameViews(Astra::Registry& reg, const ExtractRequest& req, bool warn)
    {
        ResolvedFrameViews out;
        // Edit uses the one editor camera for both. Otherwise the orthographic
        // scene camera is the sprite view and the perspective scene camera is
        // the mesh view.
        if (req.editorView)
        {
            out.spriteView = SceneCameraView{};
            out.spriteView->view = *req.editorView;
            out.meshView = *req.editorView;
            return out;
        }

        // Once per process, and only when `warn` is set, the two warnings
        // PushSceneCamera used to print. The same flag is passed into both
        // camera sweeps. The perspective sweep's degenerate-basis line warns
        // on every call that asks, so a host that resolves again before
        // DrawUI passes false and a frame that also extracts warns once.
        int camCount = 0;
        out.spriteView = ActiveSceneCamera(reg, req.viewport, &camCount, warn);
        out.spriteViewFromScene = out.spriteView.has_value();
        out.meshView = ActivePerspectiveSceneCamera(reg, req.viewport, nullptr, warn);
        if (warn)
        {
            static bool warnedNoSceneCamera = false;
            static bool warnedMultiSceneCamera = false;
            if (!out.spriteView && !warnedNoSceneCamera)
            {
                warnedNoSceneCamera = true;
                int total = 0;
                reg.CreateView<const Camera>().ForEach([&](Astra::Entity, const Camera&) { ++total; });
                if (total == 0)
                    ARC_WARN("scene has no Camera component at all -- nothing sets the view. "
                             "Add a Camera component to an entity (a New Scene ships one).");
                else
                    ARC_WARN("scene carries {} Camera component(s) but none is usable "
                             "as the sprite camera (active == false, or orthographicSize <= 0).", total);
            }
            if (camCount > 1 && !warnedMultiSceneCamera)
            {
                warnedMultiSceneCamera = true;
                ARC_WARN("scene carries {} active Camera entities -- the first found wins", camCount);
            }
        }
        // No scene camera: keep the host's stored view, as PushSceneCamera
        // does -- a plugin driving ClientRuntime::SetView still works.
        // RenderContext2D::view is that stored view (ClientRuntime.cpp).
        if (!out.spriteView)
            if (const RenderContext2D* rc = reg.GetResource<RenderContext2D>())
            {
                out.spriteView = SceneCameraView{};
                out.spriteView->view = rc->view;
            }
        return out;
    }

    RenderFramePacket& ExtractFrame(Astra::Registry& reg, const ExtractRequest& req, PacketRing& ring)
    {
        NoteExtractFrameCall();
        RenderFramePacket& packet = ring.BeginExtract();
        packet.viewport = req.viewport;
        packet.origin.translation = { 0.0f, 0.0f, 0.0f };

        // The two views, from the one resolver. warn is true here and false
        // on a host's pre-DrawUI call, so the diagnostics fire once. The cull
        // below still reads mainView and meshView, with the cull viewport
        // forced to req.viewport.
        const ResolvedFrameViews views = ResolveFrameViews(reg, req, true);
        packet.spriteView = views.spriteView;
        packet.spriteViewFromScene = views.spriteViewFromScene;
        packet.meshView = views.meshView;
        const std::optional<ViewTransform> mainView =
            packet.spriteView ? std::optional<ViewTransform>(packet.spriteView->view) : std::nullopt;
        const std::optional<ViewTransform>& meshView = packet.meshView;
        ViewTransform cullView = mainView.value_or(ViewTransform{});
        cullView.viewport = req.viewport;

        SceneVisibility* sv = reg.GetResource<SceneVisibility>();
        if (!sv)
            sv = reg.EmplaceResource<SceneVisibility>();
        const bool twoViews = meshView && !SameView(*meshView, cullView);
        sv->views.resize(twoViews ? 2 : 1);
        BuildVisibleSet(reg, cullView, sv->views[0]);
        if (twoViews)
            BuildVisibleSet(reg, *meshView, sv->views[1]);
        packet.visible = sv->views[0];
        if (twoViews)
            packet.meshVisible = sv->views[1];

        GpuSceneMirror* mirror = reg.GetResource<GpuSceneMirror>();
        if (!mirror)
            mirror = reg.EmplaceResource<GpuSceneMirror>();
        GpuSceneStage stage;
        GpuSceneSync(reg, *mirror, GpuSceneSyncedGeneration(req.device), stage);
        if (meshView)
        {
            const std::vector<MeshSectionRange> sections = MeshSectionRanges(reg.GetResource<MeshTable>());
            BuildGpuSceneFrame(*mirror, twoViews ? &packet.meshVisible : &packet.visible, sections, *meshView, packet.mesh);
        }
        else
        {
            // PrepareSceneForRender's no-mesh-view arm (GpuSceneHost.hpp): the
            // stage still lands; only the batch table is empty.
            packet.mesh.batches.clear();
            packet.mesh.args.clear();
            packet.mesh.visibleIndices.clear();
            packet.mesh.rowCount = stage.rowCapacity;
            packet.mesh.stats = {};
        }
        packet.mesh.stage = stage;

        ExtractSprites(reg, packet);
        CollectPickables(reg, packet.pickDrawables);

        // Overlays, in submission order: the 2D grid, the 3D grid request,
        // physics debug, the camera rect, then the gizmo. The gizmo arrives
        // already tagged; Append keeps its own line/fill order.
        if (req.showGrid2d && packet.spriteView)
        {
            Grid2DStyle style;
            style.minorAlpha = req.gridMinorAlpha;
            style.majorAlpha = req.gridMajorAlpha;
            style.lineRgb = req.gridLineRgb;
            style.lineThicknessPx = req.gridLineThicknessPx;
            style.maxLinesPerAxis = req.gridMaxLinesPerAxis;
            style.axisX = req.gridAxisX;
            style.axisY = req.gridAxisY;
            const float ppm = GridPixelsPerMetre(packet.spriteView->view);
            EmitGrid2D(packet.spriteView->view,
                       PlanGrid2D(ppm, req.gridMinorAlpha, req.gridMajorAlpha,
                                  req.gridFadeInPx, req.gridFadeFullPx),
                       style, packet.overlay);
        }
        if (req.showGrid3d)
        {
            packet.overlay.hasGrid3d = true;
            packet.overlay.grid3d = req.grid3d;
        }
        if (req.physicsDebug && packet.spriteView)
        {
            // The ordinary debug path: one proxy request, then the Line / Circle
            // loop into the overlay. A perspective view has no 2D affine and
            // draws nothing; so does a registry with no physics world.
            PhysicsDebugExtractRequest2D physicsRequest;
            physicsRequest.viewport = req.viewport;
            physicsRequest.view = packet.spriteView->view.AsAffine2D();
            if (const RenderContext2D* rc = reg.GetResource<RenderContext2D>())
                physicsRequest.alpha = rc->alpha;
            physicsRequest.selected = req.physicsSelectedBody;
            physicsRequest.physicsWholeWorld = req.physicsWholeWorld;
            physicsRequest.playMode = req.playMode;
            const PhysicsDebugGeometry geometry = FillPhysicsDebug2D(reg, physicsRequest);
            if (!geometry.primitives.empty())
            {
                OverlayBatchCollector collector(packet.overlay);
                collector.Begin(req.viewport.x, req.viewport.y);
                PaintPhysicsDebug(collector, geometry);
            }
        }
        if (req.cameraRect && req.editorView && !req.playMode)
        {
            if (const auto cam = ActiveSceneCamera(reg, req.viewport))
            {
                const float aspect = (req.viewport.y > 0u)
                    ? (float(req.viewport.x) / float(req.viewport.y)) : 1.0f;
                const float hw = cam->halfHeight * aspect;
                const float hh = cam->halfHeight;
                const glm::vec2 c = cam->worldCenter;
                const ViewTransform& ev = *req.editorView;
                const glm::vec3 p[4] = {
                    ev.WorldToScreen(glm::vec3(c.x - hw, c.y - hh, 0.0f)),
                    ev.WorldToScreen(glm::vec3(c.x + hw, c.y - hh, 0.0f)),
                    ev.WorldToScreen(glm::vec3(c.x + hw, c.y + hh, 0.0f)),
                    ev.WorldToScreen(glm::vec3(c.x - hw, c.y + hh, 0.0f)),
                };
                bool finite = true;
                for (const glm::vec3& q : p)
                    finite = finite && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
                if (finite)
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        OverlayLine line{};
                        line.a = glm::vec3(glm::vec2(p[i]), 0.0f);
                        line.b = glm::vec3(glm::vec2(p[(i + 1) % 4]), 0.0f);
                        line.thickness = 1.0f;
                        line.color = req.cameraFrameColor;
                        line.depth = OverlayDepthClass::AlwaysOnTop;
                        line.space = OverlayColorSpace::Linear;
                        line.screenSpace = true;
                        packet.overlay.AddLine(line);
                    }
                }
            }
        }
        packet.overlay.Append(req.gizmoPrims);

        // Ruling PR-1: ArcaneRender cannot call ArcaneImGui directly; the copy is the
        // overlay hook. No hook (a host without ImGui) leaves both snapshots at the
        // empty state Clear() already published: listCount 0. Retained list
        // elements are not live.
        const RenderOverlayHooks overlayHooks = GetRenderOverlayHooks();
        if (overlayHooks.snapshot)
        {
            PublishUiSnapshot(overlayHooks.snapshot, req.imgui, packet.imgui, "imgui", g_warnedUiSnapshotImgui);
            PublishUiSnapshot(overlayHooks.snapshot, req.gameUi, packet.gameUi, "gameUi", g_warnedUiSnapshotGameUi);
        }
        packet.hasPost = req.post != nullptr;
        if (req.post)
            CopyPostInPlace(packet.post, *req.post);
        else
            packet.post.passes.clear();
        packet.lightDirection = req.lightDirection;
        packet.lightColor = req.lightColor;
        packet.ambient = req.ambient;
        return packet;
    }
}
