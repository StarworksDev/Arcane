#pragma once

// MeshDocument: the .arcmesh editor (F2a, Task 9) -- gives the procedural mesh
// asset its own document with a live preview, so a user can author one
// without hand-writing JSON. Two halves, deliberately unequal in weight:
//
//   DATA + UNDO   follows SpriteDocument.{hpp,cpp} almost exactly: a flat
//                 MeshAssetData held by value, dirty tracking, whole-data undo
//                 steps built from an activation-time COPY compared against
//                 the live data (PushDataEdit -- SpriteDocument.cpp:123-134),
//                 and a doc-identity anchor so a step left on the shared
//                 CommandStack after this document closes goes inert instead
//                 of dereferencing a dead `this`.
//   PREVIEW       follows ShaderEditorDocument's offscreen-NriGraphContext
//                 pattern ONLY for that one mechanism (its own small vehicle
//                 over the process's one device, built lazily, retired
//                 through the app's one-frame hand-off) -- NOT its scale,
//                 its multi-panel structure, or its async compile machinery.
//                 A mesh needs none of that: BuildMeshData is synchronous and
//                 pure, so there is no compile step to coalesce and no
//                 in-flight job to route a result back to.
//
// TWO CACHES, AND ONLY ONE OF THEM IS THIS WINDOW'S PREVIEW. The preview
// image reads `m_data` DIRECTLY every rebuild (RebuildPreviewMesh), so there
// is nothing in front of THAT to invalidate: an edit or an undo shows up in
// this window the instant it lands. The SCENE is the other story, and it is
// not this document's preview at all: SceneRenderResolver owns a live
// MeshCache (Host/SceneRenderResolver.cpp) whose published MeshTable both
// hosts publish every frame through GpuSceneSync inside ExtractFrame
// (EditorAppFrame.cpp / RuntimeFrame.cpp), and MeshCache::Request memoises per Guid --
// entries leave only via Invalidate/Clear. So a save or an undo that
// invalidates nothing leaves every MeshRenderer in the open scene drawing the
// PRE-edit geometry until the project is switched.
//
// Hence Services::invalidateMesh, called from BOTH Save() and ApplyMeshData()
// -- the same two sites SpriteDocument calls its own invalidateSprite from
// (SpriteDocument.cpp:119-120, :144-145), for the reason stated there: "an
// undo the user cannot see in the scene is indistinguishable from an undo
// that did not happen."
//
// This block used to argue the opposite -- that the mesh sweep (then
// CollectMeshInstances) had no call site outside tests and no live cache existed. Both were true when
// Task 9 wrote it and Task 10 falsified both; the note stays so the old
// conclusion is not re-derived from the same (now wrong) premise.
//
// IMPORTED MESHES (F2c; final-review fix I4, 2026-09-11): every companion
// .arcmesh the import wave mints (source == MeshSource::Imported) opens in
// this same document. Its posture there is deliberately narrower than a
// generated mesh's: no Source combo (the source IS its model), no topology
// fields (the geometry is the cooked artifact's), no procedural preview
// (BuildMeshData has nothing to build -- the panel says "preview in the
// viewport" instead), and the material picker's clear NEVER erases slot 0
// (ClearPrimarySlotMaterial below: the slot array mirrors the artifact's
// slot table by position, so erasing would shift every later slot).
//
// Implements EditorDocument's five pure virtuals (EditorDocument.hpp:15-44);
// DocumentHost owns the open-document list, the asset-type -> factory
// routing, and the unsaved-close confirm modal.

#include "Scene/EditGesture.hpp"
#include "Scene/UndoGate.hpp"
#include "Documents/DocumentPageSelection.hpp"   // the page's one key + open/click epoch
#include "Documents/EditorDocument.hpp"
#include "Documents/PreviewStatus.hpp"
#include "Settings/DocumentSettings.hpp"   // editor.mesh.previewResolution, read once per document

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>          // MeshAssetData; also brings MeshBuilder.hpp (MeshData)

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// CommandStack arrives complete via EditGesture.hpp. The preview vehicle is
// forward-declared only -- see the block below for why (same reasoning
// ShaderEditorDocument.hpp states at its own forward-declare block).
namespace Arcane
{
    class CommandStack;

    // Forward-declared, never included here: NriGraphContext.hpp pulls
    // <NRI.h> plus every render-graph node header, and this header is
    // included by the whole editor AND source-compiled into ArcaneTests. The
    // unique_ptr member below is legal against an incomplete type because
    // ~MeshDocument is out of line (MeshDocument.cpp).
    struct HostConfig;
    class RenderOverlayNode;
    class NriGraphContext;
}

namespace Arcane::Editor
{
    struct AssetRefServices;   // Panels/AssetReferenceField.hpp (Services::assetRefs)
    struct AssetRefEdit;       // Panels/AssetReferenceField.hpp (ApplySlotMaterialEdit)

    class MeshDocument final : public EditorDocument
    {
    public:
        // Everything the document borrows from the app. Two shapes glued
        // together: the small "services struct" SpriteDocument::Services
        // uses (undo + invalidate), plus the three-borrow preview seam
        // DocServices carries for ShaderEditorDocument (ShaderEditorDocument.
        // hpp:120-172) -- read there for the full mechanism; restated only
        // briefly below.
        struct Services
        {
            // Resolves to the SAME shared editor CommandStack every other
            // surface pushes to (EditorApp::DocumentUndo, the resolver
            // MakeDocServices hands DocServices::undo) -- one global history,
            // so a mesh param edit undoes alongside everything else in the
            // order it happened. Asked per edit (UndoStack()); unset or null
            // (Play) = no undo coverage (the EditGesture bracket no-ops
            // whole); the document still edits and saves.
            UndoResolver undo;   // the ONE history, resolved per edit; returns null in Play (s3.3b)

            // Fired with this asset's Guid after a successful Save AND after
            // every undo/redo apply -- routed to SceneRenderResolver::
            // InvalidateMesh, which evicts the scene's cached resolve and
            // re-reads the saved file synchronously.
            //
            // NOT SPECULATIVE PLUMBING: MeshCache::Request is a once-per-Guid
            // cache and the resolver's MeshTable is what every MeshRenderer in
            // the open scene draws through, so this callback IS the whole
            // mechanism that makes a mesh edit (or an undo of one) visible in
            // the viewport -- exactly what SpriteDocument::Services::
            // invalidateSprite is for its own asset. See the file-top block.
            //
            // Null (the headless tests; any host with no resolver) means no
            // scene invalidation -- the document still edits, previews and
            // saves, so an unwired hook degrades to "the viewport lags the
            // file", never to a lost edit.
            std::function<void(const Arcane::Guid&)> invalidateMesh;

            // ===== THE PREVIEW SEAM (late-bound; node page + editor upgrades s3.2) =====
            // Borrowed from EditorApp, which outlives the document list.
            //   chromeGraph -- resolves the CHROME context at each use (its
            //                  Device() for CreateOffscreen). Late-bound because a
            //                  document opened during boot (--open-asset opens
            //                  inside StageFinalize) exists BEFORE
            //                  CreateGraphVehicles makes that context; Tick
            //                  retries the vehicle until it resolves (the
            //                  material-preview harvester's precedent).
            //   hostConfig  -- the knobs CreateOffscreen reads.
            // The chrome ImGuiNriNode is NOT re-resolved at teardown: the
            // document records the one it bound through (m_previewHud),
            // because ChromeGraph() is null after ShutdownGraphPath.
            // Both unset in the headless tests: EnsurePreviewContext is then a
            // null check that never latches.
            std::function<Arcane::NriGraphContext*()> chromeGraph;
            const Arcane::HostConfig* hostConfig = nullptr;

            // ===== AND THE ONE-FRAME RETIRE, NOT OPTIONAL =====
            // A document can be destroyed INSIDE the editor's ImGui pass
            // while a still-to-be-recorded chrome frame names this preview's
            // output by raw pointer -- see DocServices::retireGraphPreview
            // (ShaderEditorDocument.hpp:149-172) for the full ordering
            // argument; it applies here unchanged. Null in the headless
            // tests and at shutdown; a document with no sink destroys its
            // vehicle inline (see DestroyPreviewContext).
            std::function<void(std::unique_ptr<Arcane::NriGraphContext>)> retireGraphPreview;

            // The shared asset-reference cell's services (spec 2026-09-30 s4.2):
            // EditorApp::m_assetRefServices, app-lifetime; its callables read state
            // at call time, so a document made during a boot stage is not stale.
            // Null in the headless tests (the cell's null services). T3's ports read it.
            const AssetRefServices* assetRefs = nullptr;
        };

        // `data` is already loaded (LoadMeshAsset happens in the factory,
        // which returns null on failure -- same split as SpriteDocument's
        // ctor / EditorApp.cpp's spriteFactory).
        MeshDocument(Services services, std::filesystem::path path,
                    Arcane::MeshAssetData data);
        ~MeshDocument() override;   // closes a parked edit gesture, retires the preview vehicle

        const std::string& Title() const override { return m_title; }
        Arcane::Guid AssetGuid() const override { return m_data.id; }
        bool Dirty() const override { return m_dirty; }
        bool Save() override;
        bool WindowFocused() const override { return m_windowFocused; }
        void Tick(double dt) override;
        void Draw(bool& requestClose) override;

        // ---- Inspector source (inspector filters spec s6a) ----------------
        // Kind "mesh". ONE page, the whole mesh form (source, topology,
        // material), under ONE key, "mesh": opening the document selects it
        // (m_pageSel starts at epoch 1) and a click in the document's content
        // re-selects it (Draw's NoteContentClick). Tab switches and focus never
        // do (the spec's one selection rule). The document window keeps its
        // toolbar and the preview.
        std::string_view Kind() const override { return "mesh"; }
        InspectorPage* Page() override { return &m_page; }
        InspectorPage* PageFor(std::string_view key) override { return m_pageSel.Resolves(key) ? &m_page : nullptr; }
        std::string SelectionKey() const override { return m_pageSel.SelectionKey(); }
        // True when it resolves; NO epoch bump -- a restore is not a click.
        bool RestoreSelection(std::string_view key) override { return m_pageSel.Resolves(key); }
        bool Resolves(std::string_view key) const override { return m_pageSel.Resolves(key); }
        std::uint64_t SelectionEpoch() const override { return m_pageSel.epoch; }
        void NoteReopened() override { m_pageSel.NoteReopened(); }
        void NoteMoved(const std::filesystem::path& p) override;   // T5 s7.11
        std::vector<Arcane::Guid> LiveReferences() const override;   // T5 s7.5
        void FlushGesture() override;

        // Undo plumbing (doc-identity commands, the same shape as
        // SpriteDocument::ApplySpriteData): swap the whole authored data in
        // and rebuild the preview mesh from it, so an undo shows up in the
        // preview rather than living only inside the ImGui form.
        void ApplyMeshData(const Arcane::MeshAssetData& data);

        // One undo step for a COMPLETED field gesture (or a single-frame
        // commit -- the source combo, a material drag-drop/clear): `before`
        // is the pre-edit copy, `after` is m_data as it stands now. Pushes
        // nothing when the two match, so a click that moved no value leaves
        // no step behind.
        void PushDataEdit(std::string label, const Arcane::MeshAssetData& before);

        // The live authored data. Exposed for the headless [editor] units --
        // the undo units never draw (Draw and the page's DrawFormBody are the
        // only ImGui methods), so this is how they observe what a command did.
        const Arcane::MeshAssetData& Data() const noexcept { return m_data; }

        // THE SLOT-CLEAR RULE (final-review fix I4, 2026-09-11) -- what the
        // material picker's "x" button does to `data.slots`, factored out PURE
        // so the headless [editor] units can pin it without ImGui:
        //   * a GENERATED mesh (Plane/Cube/...) ERASES slot 0 -- the F2a
        //     single-material UX expressed through the array: an unnamed slot
        //     with a nil material is a different state from "no slot at all"
        //     (MeshAsset.cpp's loader: a nil legacy material yields NO slot),
        //     and a generated mesh has no artifact whose slot table the
        //     array must stay aligned with;
        //   * an IMPORTED mesh NEVER erases -- it sets slots[0].material to nil
        //     and keeps the slot (its name included). The .arcmesh slot array
        //     mirrors the cooked artifact's slot table BY POSITION (slotIndex
        //     -> slots[slotIndex]; Plan 2's per-section draw resolves through
        //     exactly that index), so erasing slot 0 would shift every later
        //     slot down one and silently re-bind each section to the wrong
        //     material. Applied to EVERY imported slot count (a single-slot
        //     imported mesh keeps its one named-but-unassigned slot too --
        //     the correspondence rule has no size threshold).
        // A no-op on an empty slot array. = ClearSlotMaterial(data, 0).
        static void ClearPrimarySlotMaterial(Arcane::MeshAssetData& data);

        // s5.5: the I4 rule for any slot k -- imported meshes nil slots[k].material
        // and KEEP the slot (positional correspondence with the artifact);
        // generated meshes erase it. No-op when k is out of range.
        static void ClearSlotMaterial(Arcane::MeshAssetData& data, std::size_t slot);
        // One material row's AssetRefEdit (s5.5). Generated: Set creates slot 0
        // when absent, else writes slots[0]; Clear = ClearPrimarySlotMaterial.
        // Imported: Set/Clear on slots[k]. One step each ("Assign Material" /
        // "Clear Material"); None does nothing.
        void ApplySlotMaterialEdit(std::size_t slot, const AssetRefEdit& edit);

        // The CURRENT preview geometry, rebuilt every time m_data changes
        // (construction, ApplyMeshData, or a live field edit in the page).
        // nullopt exactly when ValidationReason() is set -- BuildMeshData's
        // own contract, restated here so a headless test can observe both
        // halves of "an invalid param set yields no geometry and surfaces
        // the reason" without needing ImGui at all.
        const std::optional<Arcane::MeshData>& PreviewMesh() const noexcept { return m_previewMesh; }

        // nullopt == the current data is valid. Otherwise ValidateMeshAsset's
        // human-readable reason, naming the offending field -- what the panel
        // shows in place of the preview image when a hand-edited file loads
        // outside the widgets' own bounds.
        const std::optional<std::string>& ValidationReason() const noexcept { return m_validationReason; }

        // The preview's offscreen output as an ImGui texture id, and 0 for
        // "nothing to draw" -- no vehicle at all (device-less services, or
        // one that failed to build), same convention as ShaderEditorDocument
        // ::GraphPreviewTextureId. Exposed so the headless units can pin
        // "device-less services allocate no preview resources" without
        // reaching into a private member.
        [[nodiscard]] std::uint64_t PreviewTextureId() const noexcept;

        // How many times this document has told its preview vehicle to drop the
        // resident geometry under kPreviewMeshGuid -- once per RebuildPreviewMesh,
        // vehicle or no vehicle.
        //
        // AN INSTRUMENT, not bookkeeping: since Plan 2 Task 4 the preview resolves
        // its geometry through NriMeshBufferCache, which caches by guid and only
        // consults the supply on a MISS, so a rebuild that does not invalidate
        // leaves the preview frozen on the first shape the document ever built
        // (final-review C2). That invalidate is the only thing standing between a
        // topology edit and a stale picture, and it is unobservable from outside
        // without a device; this counter is what lets a headless test prove every
        // rebuild path issues one. Same role PresentedFrames/RenderErrorCount play
        // for the graph.
        [[nodiscard]] std::uint64_t PreviewGeometryInvalidations() const noexcept
        {
            return m_previewGeometryInvalidations;
        }

        // The PreviewStatus inputs (s3.2): a validation reason -> Errors,
        // imported -> NotCompiledHere, no seam -> NoDevice, vehicle unavailable
        // -> VehicleFailed, a rendered image -> Ready. What the report's
        // documents[] carries; T3 reads it for the UI.
        [[nodiscard]] PreviewStatus ComputeStatus() const;

        // CreateOffscreen calls this document has made -- the [gpu] test's
        // "the first non-null seam builds the vehicle exactly once" instrument.
        [[nodiscard]] std::uint32_t PreviewVehicleAttempts() const noexcept { return m_previewVehicleAttempts; }

    private:
        // The mesh page: the form, drawn by the Inspector instance showing it.
        // Carries its own EditGesture::ScopeGuard (the topology drags that
        // open gestures are submitted inside it) and no Begin/End -- the
        // Inspector window is its window. Sections Mesh / Material / Info (s5.5).
        void DrawFormBody(PropertyGrid& grid);

        // A single-frame commit (the Source combo, a material Set/Clear): marks
        // dirty, rebuilds the preview and pushes ONE step; nothing when unchanged.
        void CommitDataEdit(const char* label, const Arcane::MeshAssetData& before);

        // The one page this document contributes (kind "mesh", key "mesh").
        // The base MUST be public: Page() hands &m_page out as InspectorPage*,
        // and a private base makes that conversion inaccessible (MSVC C2243).
        class MeshInspectorPage final : public InspectorPage
        {
        public:
            explicit MeshInspectorPage(MeshDocument& doc) : m_doc(doc) {}
            std::vector<InspectorCrumb> Breadcrumb() const override
            {
                // One crumb; `select` is a no-op (the page IS the only level).
                return { InspectorCrumb{ m_doc.m_title, [] {}, std::string{ "mesh" } } };
            }
            void Draw(PropertyGrid& g) override { m_doc.DrawFormBody(g); }

        private:
            MeshDocument& m_doc;
        };

        // Recompute m_previewMesh/m_validationReason from the CURRENT
        // m_data. Called from the ctor and from every path that mutates
        // m_data (ApplyMeshData, and the page's live field edits) -- there is no
        // cache in front of the preview, so this is the whole of keeping it
        // in sync; see the file-top comment for why that is deliberately
        // simpler than SpriteDocument's cache-invalidate story.
        void RebuildPreviewMesh();

        // Build this document's own offscreen vehicle, once, when the chrome
        // context resolves. Called from the constructor AND retried from Tick
        // while there is no vehicle and neither latch is set (s3.2): a null
        // chromeGraph() costs one check and never latches; the first non-null
        // attempt builds the vehicle or latches m_previewVehicleFailed.
        void EnsurePreviewContext();

        // Render one frame of the preview: with a valid m_previewMesh, a
        // single instance framed from ComputeMeshBounds so the whole mesh is
        // in view regardless of source/topology. With NO valid preview mesh
        // (an invalid param set), the frame STILL RECORDS -- with an EMPTY
        // instance span, which NriGraphContext's `wantsMesh` gate reads as
        // "no mesh pass this frame" -- so the offscreen texture ends up
        // holding Batch2DNode's plain clear colour rather than whatever
        // undefined bytes CreateOffscreen left it with.
        //
        // Recording unconditionally (rather than returning early on no
        // preview mesh) is Task 9 fix-round Finding 2: OffscreenTextureId()
        // returns the raw texture pointer, non-zero the INSTANT
        // CreateOffscreen succeeds -- long before any frame renders into it
        // (NriGraphContext.cpp:582-588) -- so a Draw() that trusts a non-zero
        // texture id as "there is something to show" was, before this fix,
        // sampling creation-time-undefined contents for every invalid asset.
        // A no-op ONLY when there is no vehicle at all.
        void RenderPreview();

        // Hand the vehicle to the app's one-frame retire (or destroy it
        // inline with the cross-context invalidate when there is no sink) --
        // mirrors ShaderEditorDocument::DestroyGraphPreview exactly; see its
        // comment for the full ordering argument.
        void DestroyPreviewContext();

        [[nodiscard]] Arcane::CommandStack* UndoStack() const { return m_services.undo ? m_services.undo() : nullptr; }
        Services                 m_services;
        std::filesystem::path    m_path;
        Arcane::MeshAssetData    m_data;
        std::string              m_title;         // display name (Title())
        std::string              m_windowLabel;    // "name (Mesh)###meshdoc_<guid>" (stable across rename)
        bool                     m_dirty = false;  // set by any field edit; cleared only by a SUCCESSFUL Save
        // Latched each Draw; read by DocumentHost::FocusedDoc so the app's
        // scene-level Ctrl+S stands down while this document is focused.
        bool                     m_windowFocused = false;

        // Kept in lockstep with m_data by RebuildPreviewMesh -- never written
        // anywhere else.
        std::optional<Arcane::MeshData> m_previewMesh;
        std::optional<std::string>      m_validationReason;

        // THE PREVIEW IS A STATIC IMAGE. Nothing in RenderPreview reads a
        // clock -- there is no time input anywhere in this document -- so the
        // rendered picture can only change when m_previewMesh does. Without
        // this flag Tick would re-record a 512x512 offscreen graph frame for
        // EVERY open mesh document on EVERY editor frame, because
        // DocumentHost::TickAll ticks them all unconditionally, with no
        // visibility or focus gate (DocumentHost.cpp:152-156) -- a collapsed
        // background tab pays the same as the focused one.
        //
        // ShaderEditorDocument's own per-tick render is NOT a precedent for
        // doing the same here: that preview genuinely animates (its
        // m_animTime += dt, ShaderEditorDocument.cpp:1819), so its picture
        // really is different every frame.
        //
        // Set by RebuildPreviewMesh -- the one place m_previewMesh and
        // m_validationReason ever move -- and cleared only once a frame has
        // actually landed in the texture (FrameOutcome::Presented). True at
        // construction so the opening image records on the first Tick.
        bool m_previewDirty = true;

        // Bumped beside m_previewDirty, by RebuildPreviewMesh and nowhere else --
        // see PreviewGeometryInvalidations() for why this counter exists.
        std::uint64_t m_previewGeometryInvalidations = 0;

        // The document's ONE edit-gesture bracket. Every topology drag shares
        // it -- same shape as SpriteDocument::m_gesture, including its TWO
        // ScopeGuards: Draw's (the document window, whose Begin can refuse on
        // a background tab) and DrawFormBody's (the Inspector page, where the
        // drags now live and which draws AFTER the documents).
        EditGesture::GestureState m_gesture;

        // Doc-identity handle for undo steps, mirroring SpriteDocument's
        // m_anchor: commands hold this WEAKLY and forward through the
        // pointee, so steps left on the shared stack after this document
        // closes go inert instead of dereferencing a dead `this`.
        std::shared_ptr<MeshDocument*> m_anchor;

        // This document's own offscreen preview vehicle -- null in every
        // device-less test (Services carries no chromeGraph there) and whenever
        // CreateOffscreen itself refuses (already logged).
        std::unique_ptr<Arcane::NriGraphContext> m_preview;

        // ===== The late-bound seam's state (s3.2) =====
        // The chrome ImGuiNriNode this vehicle's image was bound through,
        // captured at creation -- the no-sink destroy invalidates against IT.
        Arcane::RenderOverlayNode* m_previewHud = nullptr;
        bool          m_previewVehicleFailed = false;   // CreateOffscreen returned null: Tick stops retrying
        bool          m_previewFrameFailed   = false;   // a frame failed and dropped the vehicle (today's permanent drop)
        bool          m_previewPresented     = false;   // a frame has landed in THIS vehicle's texture
        std::uint32_t m_previewVehicleAttempts = 0;

        // Square (editor.mesh.previewResolution, default 512 like the shader
        // editor's preview), read ONCE when the document opens and fixed for
        // its life: the preview vehicle has no resize seam.
        std::uint32_t m_previewSize =
            static_cast<std::uint32_t>(Arcane::Settings<MeshDocSettings>().previewResolution);

        // The Inspector page's selection (key "mesh", epoch 1 = selected at
        // open) and the page itself. m_page holds a reference to *this;
        // documents live behind unique_ptr in DocumentHost and never move.
        DocumentPageSelection  m_pageSel{ "mesh" };
        MeshInspectorPage      m_page{ *this };
    };
}
