#pragma once
#include <Arcane/Modules/ArcaneClientApi.hpp>

// PrepareSceneForRender -- the three calls (F3, spec s4/s5): build the visible
// set(s), sync the GPU-scene mirror, build the frame. Header-only so
// ArcaneTests can drive it without a device. F5 plan 1 Task 6 moved the hosts'
// call into ExtractFrame; this function stays for the tests that already call it.
//
// TWO VIEWS when they differ: in Play the sprites draw with View() (the scene
// camera's orthographic view) while the mesh pass draws with packet.meshView,
// the perspective scene camera ExtractFrame resolved. Each pass
// culls against its own camera: views[0] = main, views[1] = mesh. F5 unifies
// the two passes under one view; until then the second set is the honest one.
//
// NRI-FREE, deliberately: the one device fact this needs -- the mirror
// generation the GpuScene last acknowledged -- comes through the exported
// free function below (defined in Render/Nri/GpuScene.cpp, which is where
// <NRI.h> is visible), so this header pulls neither GpuScene.hpp nor NRI and
// ArcaneTests' ~[gpu] cases include it as freely as GpuSceneSync.hpp.
#include <Arcane/Render/GpuSceneSync.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cstdint>
#include <optional>

namespace Arcane
{
    class GpuScene;   // Render/Nri/GpuScene.hpp; only SyncedGeneration() is read here, through the free function below

    // The mirror generation the device last synced (GpuScene::Apply stamps it
    // on success); 0 for a null device, which GpuSceneSync reads as "never
    // synced" -> full rebuild. Re-declared identically in GpuScene.hpp.
    [[nodiscard]] ARC_CLIENT_API std::uint64_t GpuSceneSyncedGeneration(const GpuScene* device) noexcept;

    // THE VISIBILITY OBSERVABILITY SEAM (F3 plan 2 T5), NRI-free for the same
    // reason the generation above is: the hosts read it, and neither
    // VerifyReport nor the runtime HUD may pull the device half in.
    //
    // Arm opts THIS device scene into the delayed readback ring -- a per-frame
    // copy of the cull pass's args + visible indices, published once the owning
    // frame's fence has retired (GpuScene.hpp's ring block states the whole
    // contract). False for a null device, and IDEMPOTENT: a host that arms the
    // same scene twice (the editor rebuilds its viewport context) keeps
    // whatever has already landed.
    //
    // WHO ARMS, and why the two hosts differ: the RUNTIME arms at boot on
    // EVERY run, because its HUD prints the count every frame whether or not a
    // report was asked for (RuntimeFrame.cpp's BuildHud, and spec s9.5's desk
    // pass reads exactly that line). The EDITOR arms only on a --report run:
    // it has no such line, so outside a report nothing would read the answer.
    // Every other consumer -- MeshDocument's preview, the thumbnail harvester,
    // the [gpu] cases that never ask -- leaves it unarmed, declares no copy
    // node, and pays nothing.
    //
    // GpuSceneVisibleRows answers the most recently COMPLETED readback's
    // GPU-visible row count -- every emitted batch's instanceNum, summed -- or
    // nullopt when the ring is unarmed or no result has landed yet. NULLOPT IS
    // THE HONEST ANSWER and must never be replaced by a CPU count: "the GPU
    // emitted N" and "the CPU expected N" are different facts, and a report
    // that conflates them tells a witness the cull ran when it may not have.
    ARC_CLIENT_API bool GpuSceneArmVisibilityReadback(GpuScene* device) noexcept;
    [[nodiscard]] ARC_CLIENT_API std::optional<std::uint32_t> GpuSceneVisibleRows(const GpuScene* device) noexcept;

    [[nodiscard]] inline bool SameView(const ViewTransform& a, const ViewTransform& b) noexcept
    {
        return a.view == b.view && a.projection == b.projection && a.viewport == b.viewport;
    }

    // `meshView` is the view the mesh pass will draw with (Edit: the editor
    // view; Play: the perspective scene camera, or nullopt = no mesh pass).
    // `device` may be null (a device-less host, a test): the mirror then
    // rebuilds fully every frame, which is the honest answer for "nothing has
    // ever been synced".
    //
    // ON THE NO-MESH-VIEW PATH the stage still lands in `out.stage`: Sync has
    // already advanced the mirror's lastModel history past the rows it staged,
    // so a host must still declare the mesh pass whenever the frame carries
    // staged rows (MeshSceneDesc::Empty() counts them -- ruling R-D); only the
    // batch table is cleared, since there is no camera to cull against.
    inline void PrepareSceneForRender(Astra::Registry& reg, const ViewTransform& mainView,
                                      const std::optional<ViewTransform>& meshView,
                                      const GpuScene* device, GpuSceneFrame& out)
    {
        SceneVisibility* sv = reg.GetResource<SceneVisibility>();
        if (!sv) sv = reg.EmplaceResource<SceneVisibility>();
        const bool twoViews = meshView && !SameView(*meshView, mainView);
        sv->views.resize(twoViews ? 2 : 1);
        BuildVisibleSet(reg, mainView, sv->views[0]);
        if (twoViews)
            BuildVisibleSet(reg, *meshView, sv->views[1]);

        GpuSceneMirror* mirror = reg.GetResource<GpuSceneMirror>();
        if (!mirror) mirror = reg.EmplaceResource<GpuSceneMirror>();
        GpuSceneSync(reg, *mirror, GpuSceneSyncedGeneration(device), out.stage);

        if (!meshView)
        {
            out.batches.clear(); out.args.clear(); out.visibleIndices.clear();
            out.rowCount = out.stage.rowCapacity; out.stats = {};
            return;   // no mesh pass this frame; the stage still lands (rows stay current for the frame that has one)
        }
        const VisibleSet* meshVis = twoViews ? &sv->views[1] : &sv->views[0];
        BuildGpuSceneFrame(*mirror, meshVis, MeshSectionRanges(reg.GetResource<MeshTable>()), *meshView, out);
    }

    // THE DROPPED-STAGE REMEDY (ruling R-F). GpuSceneSync records lastModel and
    // lastSyncTick in the same step it stages a row, and the device only
    // stamps the synced generation inside GpuScene::Apply -- so a frame whose
    // graph outcome was anything but Presented (Skipped: a collapsed viewport
    // panel, a zero-sized surface, an OUT_OF_DATE acquire; Failed) has a stage
    // that never reached the GPU and would never be re-staged: the next
    // frame's out.Clear() drops it, and the rows draw stale (or never-written)
    // bytes when they come into view. A host calls this on every such outcome
    // AFTER ExtractFrame ran for that frame. It hands the mirror a
    // NEW generation, so the next GpuSceneSync sees the device's stamp
    // mismatch and re-stages every live row with prev == model -- spans and
    // batch ids stay put (cheaper and safer than dropping the resource). A
    // registry with no mirror yet has nothing to lose: no-op.
    inline void GpuSceneInvalidate(Astra::Registry& reg)
    {
        if (GpuSceneMirror* m = reg.GetResource<GpuSceneMirror>())
            m->generation = GpuSceneMirror::NextGeneration();
    }
}
