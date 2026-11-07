#pragma once

// ShaderEditorDocument: the shader editor MVP (Slice 5) -- the first real
// EditorDocument. One window, three panels over one .arcmat asset:
//   snippet text (InputTextMultiline; the line-jump seam is dormant -- the
//                 errors panel was its only driver, see m_jumpToLine)
//   live preview (this document's own offscreen NriGraphContext, animating
//                 Time)
//   params (auto-widgets from the //@param decls; edits write the CB LIVE, no
//           recompile, and land on the shared CommandStack as undo steps)
// Diagnostics have NO panel of their own: canvas node badges still mark the
// offending nodes, and the structured rows are published under this
// document's key ("material:<guid>") to the engine-wide Diagnostics seam by
// PublishDiagnostics, every Tick -- publication groups make republishing an
// unchanged set idempotent, so there is no anti-spam gate to maintain. The
// editor's Problems panel is the presentation layer.
// Live loop: a text edit resubmits both stages through the app-shared
// ShaderCompiler (the service's per-key debounce coalesces keystrokes); the
// LAST-GOOD pipeline keeps rendering while a compile is in flight or failing.
// EditorApp drains the service once per frame and offers each result to every
// document via ConsumeResult.

#include "Scene/EditGesture.hpp"
#include "Scene/UndoGate.hpp"
#include "Documents/DocumentPageSelection.hpp"   // the page's one key + open/click epoch
#include "Documents/EditorDocument.hpp"
#include "Documents/ShaderNodeKey.hpp"   // NodeKey: the node page's "node:<pass>:<id>" key
#include "Documents/ShaderGraphPinTypes.hpp"   // GraphPinPaint: the canvas pin paint seam (T3-D1)
#include "Documents/PreviewStatus.hpp"
#include "Widgets/EditorWidgets.hpp"   // TextCommitState / StableTextEdit
// The grid's PURE half, and its ONLY half: the two phase members below are
// what the canvas grid IS. Held BY VALUE because a canvas keeps its pan/zoom
// history across view switches.
#include "Widgets/GraphGridPhase.hpp"
#include "Widgets/GraphFit.hpp"   // CanvasNavLatch -- the fit-on-open + focus latches, held by value
// NodeLOD + its zoom boundaries. Moved out of this header (2026-09-09) so the
// Assets panel's Graph lens reads the same table instead of copying one of its
// numbers into a bare float compare -- see that header.
#include "Widgets/GraphNodeLod.hpp"

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>   // MeshData: the mesh-surface preview sphere (T3-D6)
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include "Settings/DocumentSettings.hpp"   // editor.shader.previewResolution, read once per document

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>
// PostChainDesc -- the DEVICE-FREE description of a compiled fullscreen
// material (bytecode + merged template + instance + input wiring). Held BY
// VALUE below because it is what the preview renders from and what the
// headless tests read; see m_graphPost.
#include <Arcane/Render/PostChainCache.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Util/FunctionRef.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Arcane
{
    class CommandStack;
    class Runtime;

    // ---- The preview vehicle ----
    // Forward-declared, never included: NriGraphContext.hpp pulls <NRI.h> plus
    // every node header, and this header is included by the whole editor AND
    // source-compiled into ArcaneTests. The unique_ptr member below is legal
    // against an incomplete type because ~ShaderEditorDocument is out of line.
    class Batcher2D;
    struct HostConfig;
    class RenderOverlayNode;
    class NriGraphContext;
}

namespace ax::NodeEditor
{
    struct EditorContext;   // imgui-node-editor (opaque; graph canvas, Slice 9)
}

namespace Arcane::Editor
{
    struct AssetRefEdit;       // Panels/AssetReferenceField.hpp (ApplyParamRefEdit)
    struct AssetRefServices;   // Panels/AssetReferenceField.hpp (DocServices::assetRefs)

    // NodeLOD, the tier boundaries (editor.graph.lod.*) and NodeLODForScale live in
    // Widgets/GraphNodeLod.hpp (included above) so both node canvases read one
    // table; only the per-tier DEGRADATION -- which branches in DrawGraphNode
    // drop what -- is still this document's own.

    // Everything a document borrows from the app (all outlive the host's
    // document list -- see EditorApp member ordering).
    struct DocServices
    {
        // NO DEVICE AND NO SHADER LIBRARY travel in here: a document builds
        // its own offscreen vehicle from the three borrowed seams further
        // down, and everything else it needs is bytes.
        Arcane::ShaderCompiler*       compiler = nullptr;   // app-shared service
        Arcane::ShaderSourceProvider* sources = nullptr;    // template text
        Arcane::Runtime*              runtime = nullptr;    // Assets facade + open project (picker)
        UndoResolver                  undo;                 // the ONE history, resolved per edit; returns null in Play (s3.3b)
        const double*                 clock = nullptr;      // app compile clock (Poll's `now`)
        Arcane::GraphicsBackend       backend{};
        // Fired after a successful Save with the asset's Guid -- the app
        // invalidates the sprite-material cache so scene sprites pick up the
        // SAVED asset (Slice 8; scene sprites never render the working copy).
        std::function<void(const Arcane::Guid&)> onAssetSaved;
        // Fired by assisted param rename for each rewritten INSTANCE file: the
        // app patches any OPEN document for that asset in memory (re-key only;
        // unsaved edits stay). (guid, oldName, newName).
        std::function<void(const Arcane::Guid&, const std::string&, const std::string&)>
            onParamRenamed;

        // ===== THE PREVIEW SEAM (late-bound; node page + editor upgrades s3.2) =====
        // Both BORROWED from EditorApp (which outlives the document list):
        //
        //   chromeGraph -- resolves the CHROME context at each use. Its
        //                  Device() is the ONE device in the process; the
        //                  document builds its own small CreateOffscreen
        //                  vehicle over it (its own graveyard lane --
        //                  NriGraphContext.hpp, TWO CONTEXTS TWO LANES). Its
        //                  ImGuiHud() is the backend that CACHES this
        //                  document's preview texture by raw pointer; the
        //                  document records that node at vehicle creation and
        //                  owes it an InvalidateUserTextureNow before the
        //                  texture dies (~ShaderEditorDocument), never
        //                  re-resolving it, because ChromeGraph() is null after
        //                  ShutdownGraphPath.
        //                  LATE-BOUND because a document opened during boot
        //                  (--open-asset opens inside StageFinalize) exists
        //                  before CreateGraphVehicles makes the chrome context:
        //                  a copied pointer was null forever. Tick retries the
        //                  vehicle until it resolves.
        //   hostConfig  -- the backend/validation knobs CreateOffscreen reads.
        //
        // Both unset in the headless tests (no EditorApp at all).
        std::function<Arcane::NriGraphContext*()> chromeGraph;
        const Arcane::HostConfig*      hostConfig = nullptr;

        // ===== AND THE ONE-FRAME RETIRE, WHICH IS NOT OPTIONAL =============
        // A document is DESTROYED INSIDE the editor's ImGui pass:
        // DocumentHost::DrawAll collects requestClose and calls Close (which
        // erases the unique_ptr) right after its draw loop -- i.e. at PHASE 14.
        // The chrome frame that replays this frame's draw lists is recorded at
        // PHASE 19. Those lists still carry an ImGui::Image naming this
        // document's preview output BY RAW POINTER, so destroying the context
        // in ~ShaderEditorDocument would hand ImGuiNri::EnsureEntry a freed
        // nri::Texture* to build a view over.
        //
        // NRI DOES NOT REF-COUNT and defers nothing, so this ordering has to
        // be arranged rather than inherited from the API.
        //
        // So a closing document HANDS its vehicle over instead, and the app
        // destroys it at the TOP of the next frame -- after the chrome frame
        // that named it has been recorded AND submitted. The invalidate rides
        // with the destroy at that point (the pair is one operation) and
        // InvalidateUserTextureNow's own DeviceWaitIdle is what covers the
        // in-flight submission.
        //
        // Null in the headless tests; a document with no sink destroys its
        // vehicle inline, which is correct at shutdown (no
        // further frame is recorded) and unreachable anywhere else.
        std::function<void(std::unique_ptr<Arcane::NriGraphContext>)> retireGraphPreview;

        // The shared asset-reference cell's services (spec 2026-09-30 s4.2):
        // EditorApp::m_assetRefServices, app-lifetime; its callables read state
        // at call time, so a document made during a boot stage is not stale.
        // Null in the headless tests (the cell's null services). T3's ports read it.
        const AssetRefServices* assetRefs = nullptr;
    };

    class ShaderEditorDocument final : public EditorDocument
    {
    public:
        ShaderEditorDocument(DocServices services, std::filesystem::path path,
                             Arcane::MaterialAssetData data);
        // Closes a parked edit gesture, then destroys the node-editor contexts.
        ~ShaderEditorDocument() override;

        const std::string& Title() const override { return m_title; }
        Arcane::Guid AssetGuid() const override { return m_data.id; }
        // Text dirtiness is tracked directly; param dirtiness derives from the
        // instance's EffectiveSerial vs the saved baseline, so undo/redo of a
        // param edit flips the verdict correctly (review m1).
        bool Dirty() const override { return m_dirty || ParamsDirty(); }
        bool PreviewUploadsPending() const noexcept override;
        bool Save() override;
        bool WindowFocused() const override { return m_windowFocused; }
        void Tick(double dt) override;
        void Draw(bool& requestClose) override;

        // ---- Inspector source (inspector filters spec s6a) ----------------
        // Kind "material". ONE page, the whole document's -- title (plus
        // "(Instance)"), the Preview section (a collapsible square, s5.3) and the
        // params editor -- under ONE key, "material": opening the document
        // selects it (m_pageSel starts at epoch 1) and a click in the
        // document's content re-selects it (Draw's NoteContentClick). Tab
        // switches and focus never do (the spec's one selection rule).
        std::string_view Kind() const override { return "material"; }
        // "material" -> the material page; a resolving node key retargets the
        // ONE node page object and returns it (a returned page is valid until
        // the next Page()/PageFor(), InspectorSource.hpp:48-55, so two
        // Inspector instances -- one pinned to A, one following -- each
        // resolve right before they draw; precedent InputActionsDocument.cpp:66-77).
        InspectorPage* Page() override { return PageFor(SelectionKey()); }
        InspectorPage* PageFor(std::string_view key) override;
        // The NODE page (node page s5.1): a second key, "node:<pass>:<id>"
        // (ShaderNodeKey.hpp), mirrored from the graph canvas's selection.
        // SelectionKey answers the node key when exactly one node is selected,
        // the chain overview is not showing, and the key still resolves;
        // otherwise "material".
        std::string SelectionKey() const override;
        // True when it resolves; NO epoch bump -- a restore is not a click.
        // A node key enters its pass if needed and arms a canvas Select.
        bool RestoreSelection(std::string_view key) override;
        // PURE. "material", or a node key whose pass is in range (checked
        // BEFORE the graph lookup) and whose graph holds the id. Instances
        // and graphless bases resolve only "material".
        bool Resolves(std::string_view key) const override;
        // --select-in-document: "<id>" (active pass) or "<pass>/<id>"; a
        // scripted select IS a selection (one epoch bump).
        bool SelectByPath(std::string_view path) override;
        std::uint64_t SelectionEpoch() const override { return m_pageSel.epoch; }
        void NoteReopened() override { m_pageSel.NoteReopened(); }
        void NoteMoved(const std::filesystem::path& p) override;   // T5 s7.11
        std::vector<Arcane::Guid> LiveReferences() const override;   // T5 s7.5
        // The chain OVERVIEW is what the canvas area shows. m_inChainView is
        // seeded true for every document and Draw clears it only once a
        // non-chain surface draws, so the raw flag would report an overview
        // that is not there before the first draw.
        [[nodiscard]] bool ChainViewShowing() const noexcept { return m_surface == 0 && m_inChainView; }
        // Exposed for the headless tests (ParseErrors precedent).
        [[nodiscard]] std::size_t NavHistoryDepth() const noexcept { return m_navHistory.size(); }
        void FlushGesture() override;

        // True when the result belonged to this document's in-flight compiles.
        bool ConsumeResult(const Arcane::ShaderCompileResult& result);

        bool IsInstance() const { return m_data.IsInstance(); }
        // Graph-owned (Slice 9): the node canvas is the editing surface and the
        // snippet buffer holds GENERATED text (shown read-only). Doc-level =
        // the BASE; every pass carries its own optional graph (per-pass graphs).
        bool IsGraphOwned() const { return m_data.graph.has_value(); }

        // Undo plumbing for graph edits (same doc-identity anchor pattern as
        // ApplyParamEdit): swap in a whole graph state for ONE pass (0 = base)
        // and regenerate/recompile.
        void ApplyGraphState(std::size_t pass, std::optional<Arcane::MaterialGraph> state);
        // The graph pass `pass` edits (0 = base), or null when `pass` is out of
        // range or that pass is text-owned. Range-checked BEFORE indexing --
        // never GraphOptAt's silent base fallback. Read-only; tests and the
        // node page read graph state through it.
        [[nodiscard]] const Arcane::MaterialGraph* PassGraph(std::size_t pass) const noexcept
        {
            if (pass == 0) return m_data.graph ? &*m_data.graph : nullptr;
            if (pass > m_data.passes.size()) return nullptr;
            const std::optional<Arcane::MaterialGraph>& g = m_data.passes[pass - 1].graph;
            return g ? &*g : nullptr;
        }
        // Custom-node pin edits, ONE undo step each ("Add Pin" / "Remove Pin"),
        // called by BOTH the canvas and the node page (s5.1.4). Remove drops the
        // pin's links and literal and re-indexes later pins' links and literals
        // (both address pins by bare index). Add takes the first free "p<k>".
        // False when (pass, id[, pin]) does not resolve to a Custom node pin.
        bool AddCustomPin(std::size_t pass, std::uint32_t id);
        bool RemoveCustomPin(std::size_t pass, std::uint32_t id, std::uint32_t pin);

        // Undo plumbing for pass-canvas STRUCTURAL edits (add/remove/rewire/
        // reorder/rename): whole pass-list before/after, one step per gesture.
        // The list is small (a handful of passes, graphs of tens of nodes), so
        // the whole-state snapshot carries the same justification as
        // GraphEditCommand's. active/view ride along so undo lands the user
        // back on the pass they were editing.
        struct PassListState
        {
            std::vector<Arcane::MaterialPass> passes;
            std::vector<std::uint32_t> baseInputs;   // scene wires on the base
            int activePass = 0;
            int viewPass = -1;
        };
        [[nodiscard]] PassListState CapturePassListState() const;
        void ApplyPassListState(PassListState state);

        // Parse/chain-resolution errors. Surfaced through PublishDiagnostics
        // into the Problems panel, ahead of the compile diags. Exposed for
        // the headless tests.
        const std::vector<std::string>& ParseErrors() const { return m_parseErrors; }
        // Pending assisted renames (BeginParamRename's queue). Exposed for the
        // headless tests, like ParseErrors.
        [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& PendingParamRenames() const noexcept
        { return m_paramRenames; }

        // Undo plumbing (doc-identity commands, review M3): apply a param
        // override edit to the CURRENT instance. Undo steps hold the document
        // through m_anchor -- recompiles swap m_instance underneath them, so
        // forwarding by name hash here is what lets history survive rebinds.
        void ApplyParamEdit(std::uint32_t nameHash, bool hasValue,
                            const Arcane::MatParamValue& value);
        // s5.3: the texture param row's one write path. Set binds edit.guid,
        // Clear binds the nil guid (the never-assigned state); one undo step
        // each (SetParamWithUndo). False when `nameHash` names no bound texture param.
        bool ApplyParamRefEdit(std::uint32_t nameHash, const AssetRefEdit& edit);

        // Mesh render metadata stays on MaterialAssetData rather than joining
        // the shader-param/template state. nullopt from Capture means this
        // document's resolved surface is not Mesh, so no metadata controls are
        // exposed. Apply is the shared headless/UI write path and clamps the
        // cutoff before it can become an unauthorable invalid asset state.
        struct MeshMaterialMetadataState
        {
            std::optional<Arcane::MaterialBlendMode> blend;
            std::optional<float> alphaCutoff;
            std::optional<bool> twoSided;
            bool operator==(const MeshMaterialMetadataState&) const = default;
        };
        [[nodiscard]] std::optional<MeshMaterialMetadataState>
            CaptureMeshMaterialMetadata() const;
        void ApplyMeshMaterialMetadata(MeshMaterialMetadataState state);
        // THE UNDOABLE WRITE (F3 plan 2 final review, I2), the same shape as
        // SetParamWithUndo: capture, Apply, then ONE step whose Undo restores
        // the capture and whose Redo re-applies what actually LANDED (the
        // clamped cutoff, not the request). A write that changes nothing
        // pushes nothing. The panel's single-shot rows (blend, the override
        // boxes, two-sided) call this; the cutoff drag applies live inside the
        // EditGesture bracket and pushes the same step once, at close.
        void SetMeshMaterialMetadataWithUndo(MeshMaterialMetadataState state);

        // Assisted param rename (design 2026-07-24): the BASE document's
        // propagation rewrote this INSTANCE's file on disk -- keep this open
        // document in step (saved-params re-key + a pending rename that
        // migrates the live override when the renamed base finally rebinds).
        void PatchParamRename(const std::string& oldName, const std::string& newName);

        // External-change hooks (the app's material file watcher):
        // Reload from disk, DISCARDING the working copy -- callers gate on
        // Dirty(). Re-seeds every canvas, re-resolves chains, recompiles.
        void ReloadFromDisk();
        // True when this document's resolved parent chain contains `id`.
        bool DependsOn(const Arcane::Guid& id) const;
        // A parent's FILE changed: re-resolve the chain + recompile, keeping
        // this document's own working copy and live overrides (they migrate
        // by hash at the rebind, rename-translated).
        void RefreshParentChain();

        // Stable on-disk identity. Problems File-locator navigation
        // (EditorApp::RouteLocator, node-page phase s8.2) reaches an open
        // document through DocumentHost::OpenPath's focus-not-reopen, not here.
        const std::filesystem::path& Path() const noexcept { return m_path; }

        // Problems-panel navigation. Requests are recorded here and consumed on
        // the next Draw -- the panel must never mutate document state mid-draw
        // (the same deferral rule the editor's modals follow). Both setters
        // re-arm EXISTING dormant fields (m_jumpToLine, m_focusNode) rather
        // than adding new ones: DrawGraphPanel already consumes m_focusNode
        // (ed::SelectNode + NavigateToSelection) -- its own header comment at
        // m_focusNode's declaration already named the Problems panel as the
        // future driver -- so this task supplies the driver only; there is no
        // separate consumption step left for a later task.
        void RequestJumpToLine(int line) noexcept { m_jumpToLine = line; }
        void RequestFocusGraphNode(std::uint32_t nodeId) noexcept
        {
            m_focusNode = nodeId;
            if (nodeId != 0)
                m_focusPending.Arm();   // consumed once it LANDED (CanvasNavLatch)
            else
                m_focusPending.Disarm();
        }
        // Open the Custom-node HLSL body editor for (pass, id) (node page
        // s5.1.7). The canvas's "Edit HLSL..." passes the active pass, the
        // node page its target -- so a pinned page edits a pass that is not
        // the active one. Consumed by DrawGraphModals, which the canvas AND
        // the page both draw.
        void RequestBodyEdit(std::size_t pass, std::uint32_t id) noexcept
        { m_bodyEditPass = pass; m_bodyEditRequest = id; }

        // TEST SEAM (GraphFitTest, GraphCanvasHeadlessTest): the graph canvas's
        // node-editor context, so a headless test can read the view the
        // fit-on-open landed, and ask what is selected and where a node sits.
        // Null until the first DrawGraphPanel. Production never calls it.
        [[nodiscard]] ax::NodeEditor::EditorContext* GraphCanvasContext() const noexcept { return m_graphCtx; }
        // TEST SEAM (GraphFitTest): the pass canvas's node-editor context, the
        // chain overview's twin of GraphCanvasContext. Null until the first
        // DrawPassCanvas. Production never calls it.
        [[nodiscard]] ax::NodeEditor::EditorContext* PassCanvasContext() const noexcept { return m_passCanvasCtx; }
        // TEST SEAM ([graphcanvas], T3-D1): how the graph canvas painted pin
        // `pin` (an input when `input`) of node `id` on its LAST draw -- the
        // same call the dot and the wire end make, over the widths that draw
        // resolved. The dynamic grey with no ring when the node is not in the
        // active graph. Production never calls it.
        [[nodiscard]] GraphPinPaint CanvasPinPaint(std::uint32_t id, std::uint32_t pin, bool input) const;
        // TEST SEAM ([graphcanvas], T3-D3): the two tints the graph canvas's
        // LAST draw handed DrawGradientWire for link `linkIndex` (the active
        // graph's `links` order), before hover/selection emphasis: `from` is
        // the source pin's colour, `to` the destination pin's. nullopt for an
        // index that draw did not walk. The wire-gradient regression guard
        // (user ruling 2026-10-02: a wire is a gradient exactly where its ends
        // paint differently) reads it. Production never calls it.
        struct CanvasWireTint
        {
            ImVec4 from;
            ImVec4 to;
        };
        [[nodiscard]] std::optional<CanvasWireTint> CanvasWireTintOf(std::size_t linkIndex) const;

        // Publish this document's CURRENT diagnostic set under "material:<guid>".
        // No anti-spam gate is needed: publication groups replace, so republishing
        // an identical set is idempotent by construction. Public so the
        // [diagnostics] units can drive it headlessly (ParseErrors precedent).
        void PublishDiagnostics();
        [[nodiscard]] std::string DiagnosticKey() const;

        // ===== THE DEVICE-FREE PREVIEW DESCRIPTION ==========================
        // The compiled preview AS BYTES + merged template + instance, exactly
        // the shape PostChainCache publishes for a scene post material and
        // exactly what the graph's PostChainNode consumes. Being device-free
        // is what makes it testable with no device at all.
        //
        // Null (`passes` empty / `templ` null) until a full set of stages has
        // landed; a FAILED re-compile leaves the previous one published --
        // the LAST-GOOD rule, upheld by this publish alone.
        //
        // FULLSCREEN SURFACES ONLY. A sprite material's preview is a quad
        // through a Batcher2D, not a fullscreen chain -- SpritePreviewBlobs()
        // is its counterpart.
        [[nodiscard]] const Arcane::PostChainDesc& GraphPreviewDesc() const noexcept
        { return m_graphPost; }

        // The sprite surface's device-free half: the stitched, compiled blobs
        // the graph's Batch2DNode builds its own pipeline from
        // (Material2DDesc::vsBytes/psBytes). Both null until a full pair has
        // landed. Public for the same reason GraphPreviewDesc is.
        struct SpriteBlobs
        {
            std::shared_ptr<const std::vector<std::uint8_t>> vs, ps;
        };
        [[nodiscard]] const SpriteBlobs& SpritePreviewBlobs() const noexcept
        { return m_graphSpriteBlobs; }

        // The preview output as an ImGui texture id (the raw nri::Texture*
        // through uintptr_t -- ImGuiNri's convention). 0 when this document
        // has no preview vehicle, which is every headless test.
        [[nodiscard]] std::uint64_t GraphPreviewTextureId() const noexcept;
        // The PreviewStatus inputs (s3.2): mesh surface -> NotCompiledHere; no
        // compiler, no sources, an unavailable compiler or a refused Submit ->
        // CompilerUnavailable; HasErrors -> Errors; jobs in flight ->
        // Compiling; the seam, the two latches and PreviewReady(). What the
        // report's documents[] carries; T3 reads it for the toolbar.
        [[nodiscard]] PreviewStatus ComputeStatus() const;
        // CreateOffscreen calls this document has made (the [gpu] test's instrument).
        [[nodiscard]] std::uint32_t PreviewVehicleAttempts() const noexcept { return m_previewVehicleAttempts; }
        // T3-D6: what a MESH-surface preview frame draws from -- the bound
        // instance's CURRENT baseColor and albedo (an unsaved edit or an
        // instance override included), never the saved file. White and no
        // albedo before anything is bound. Public for the same reason
        // GraphPreviewDesc is: the frame itself is device work.
        struct MeshPreviewParams
        {
            std::array<float, 4> baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
            Arcane::Guid         albedo{};
        };
        [[nodiscard]] MeshPreviewParams MeshPreviewInputs() const;
        // A save gesture parked behind the save-with-errors confirm (the modal
        // opens at the next draw of the document window or its page).
        [[nodiscard]] bool SaveWithErrorsPending() const noexcept { return m_confirmSaveWithErrors; }
        // The "Output preview" toggle's state (s5.2). Test seam.
        [[nodiscard]] bool ShowNodePreviews() const noexcept { return m_showNodePreviews; }
        // What Ctrl+S runs -- the document's own Shortcut AND the Inspector
        // page's (RequestSaveFromInspector). Carries the error guard the
        // toolbar's Save button used to own: writing a material that does not
        // compile is allowed, but only through an explicit confirm (UE's
        // pre-apply guard shape). Save() itself stays unguarded -- the close
        // flow's save-then-close needs it, and the confirm modal's "Save
        // Anyway" is the deliberate way past.
        SaveGestureResult RequestSave() override;

    private:
        // The material page: the Preview section, Rendering (mesh), Parameters
        // -- PropertyGrid sections (s5.3), drawn by the Inspector instance
        // showing it. Carries its own EditGesture::ScopeGuard (the param rows
        // that open gestures are submitted inside it) and no Begin/End -- the
        // Inspector window is its window.
        void DrawMaterialPageBody(PropertyGrid& grid);

        // The one page this document contributes (kind "material", key
        // "material"). The base MUST be public: Page() hands &m_page out as
        // InspectorPage*, and a private base makes that conversion
        // inaccessible (MSVC C2243).
        class MaterialInspectorPage final : public InspectorPage
        {
        public:
            explicit MaterialInspectorPage(ShaderEditorDocument& doc) : m_doc(doc) {}
            std::vector<InspectorCrumb> Breadcrumb() const override
            {
                // One crumb; `select` is a no-op (the page IS the only level). An
                // instance reads "<title> (Instance)", like the window label --
                // the page body no longer carries its own title (spec s4.3).
                return { InspectorCrumb{ m_doc.IsInstance() ? m_doc.m_title + " (Instance)" : m_doc.m_title,
                                         [] {}, std::string{ "material" } } };
            }
            void Draw(PropertyGrid& g) override { m_doc.DrawMaterialPageBody(g); }

        private:
            ShaderEditorDocument& m_doc;
        };

        // The NODE page (node page s5.1.2). Holds (pass, id), never a
        // GraphNode*: DrawNodePageBody re-resolves every call.
        class NodeInspectorPage final : public InspectorPage
        {
        public:
            explicit NodeInspectorPage(ShaderEditorDocument& doc) : m_doc(doc) {}
            void SetTarget(std::size_t pass, std::uint32_t id) noexcept { m_pass = pass; m_id = id; }
            std::vector<InspectorCrumb> Breadcrumb() const override;
            void Draw(PropertyGrid& g) override { m_doc.DrawNodePageBody(g, m_pass, m_id); }

        private:
            ShaderEditorDocument& m_doc;
            std::size_t   m_pass = 0;
            std::uint32_t m_id = 0;
        };

        // The node page body: FIRST the gesture guard, then the save-with-errors
        // confirm and the graph modals, then the node re-resolved by (pass, id)
        // -- one read-only line when it is gone -- then its sections (s5.1.4).
        void DrawNodePageBody(PropertyGrid& grid, std::size_t pass, std::uint32_t id);
        // ---- The NODE page body (spec s5.1.4). Each section re-resolves (pass,
        // id) itself -- a queued edit or a live write can run between them. ----
        void DrawNodePageHeader(const Arcane::GraphNode& n);   // chip + type + description; not a Section
        void DrawNodePageInputs(PropertyGrid& grid, std::size_t pass, std::uint32_t id);
        // One input pin (s5.1.4/5.1.5): wired = "<- source", refusing = its
        // neutral read-only, else a live literal row with Reset -- each led by
        // the pin's type chip (T3-D1). `resolvedInputs` = the node's resolved
        // dynamic-input width (ResolveGraphNodeWidths, taken once per section).
        void DrawNodePageInputRow(PropertyGrid& grid, std::size_t pass, std::uint32_t id, std::uint32_t pin,
                                  int resolvedInputs);
        void DrawNodePageSettings(PropertyGrid& grid, std::size_t pass, std::uint32_t id);
        // One live numeric/vector row (s5.1.5): read into a local, draw, open the
        // gesture with GraphEditBuilder(undoLabel, pass), write through to the
        // RE-RESOLVED node when the local differs, close with EndAfterRow.
        void LiveNodeFloats(PropertyGrid& grid, const char* label, const char* undoLabel,
                            std::size_t pass, std::uint32_t id, int lanes,
                            Arcane::FunctionRef<float*(Arcane::GraphNode&)> field);
        // The colour form: the same plus ColorRow's popup pair under popupLabel.
        void LiveNodeColor(PropertyGrid& grid, const char* label, const char* undoLabel, const char* popupLabel,
                           std::size_t pass, std::uint32_t id, bool hdr,
                           Arcane::FunctionRef<float*(Arcane::GraphNode&)> field);
        // Commit-only text rows (s5.1.5): the STORED commit captures the anchor and
        // (pass, id, pin) by value, re-resolves when it fires, and drops the edit
        // when the document or the node is gone.
        enum class NodeTextField : std::uint8_t { ParamName, SwizzleMask, CommentText, CustomPinName };
        void NodeTextRow(PropertyGrid& grid, const char* label, std::string_view current,
                         std::size_t pass, std::uint32_t id, NodeTextField field, std::uint32_t pin = 0);
        void CommitNodeText(std::size_t pass, std::uint32_t id, NodeTextField field, std::uint32_t pin,
                            const std::string& text);
        void DrawNodePageOutputs(PropertyGrid& grid, std::size_t pass, std::uint32_t id);
        void DrawNodePageErrors(PropertyGrid& grid, std::size_t pass, std::uint32_t id);
        // Compile diagnostics mapped to ONE node of ANY pass: the per-pass
        // generalisation of RebuildDiagBadges' line map (that one serves only the
        // active pass; a pinned node page may show another).
        void ForEachNodeDiagnostic(std::size_t pass, std::uint32_t nodeId,
                                   const std::function<void(std::string_view)>& fn) const;
        // The ONE traversal both read: `pass`'s Error-severity compile diags
        // (its chain job's, or the single-path m_diags), stitched line ->
        // snippet line -> the line map's node id. `fn` sees every in-range
        // mapped line, nodeId 0 (graph-level statements) included.
        void ForEachPassErrorDiag(std::size_t pass,
                                  const std::function<void(std::uint32_t nodeId, std::string_view message)>& fn) const;
        // The material crumb's `select` (s5.1.3): back to the material page as
        // a SELECTION (precedent SelectMap({}), InputActionsInspectorPage.cpp:78).
        void SelectMaterialFromCrumb();

        double Now() const { return m_services.clock ? *m_services.clock : 0.0; }
        void   Rebuild();          // parse + stitch + submit both stages (structural edit)
        void   BindIfComplete();   // both stages landed -> createShader + SetMaterial
        // Pass chains (queue item 4): a fullscreen BASE material with extra
        // passes compiles/binds through the chain path -- one merged template,
        // per-pass stages, atomic SetChain (chain-level last-good).
        // Chain mode also covers the base-only POST material (scene inputs,
        // no extra passes) -- one uniform build/run path for anything that
        // reads InputTexture slots.
        bool   ChainMode() const
        {
            return m_surface == 0 && !IsInstance() &&
                   (!m_data.passes.empty() || !m_data.baseInputs.empty());
        }
        // The material whose SOURCE this document compiles: the base at the
        // end of the parent chain for an instance (an instance carries no
        // snippets, passes or scene inputs of its own), else this document's
        // own data. Null for an instance whose chain did not resolve.
        const Arcane::MaterialAssetData* CompiledSource() const
        {
            if (!IsInstance())
                return &m_data;
            return m_parentChain.empty() ? nullptr : &m_parentChain.back();
        }
        // The COMPILE predicate (T3-D6): ChainMode() is the AUTHORING one --
        // an instance has no canvas and no pass strip -- but an instance of a
        // chain base must still compile and preview its base's whole chain
        // (UE: a Material Instance previews its parent's material). Rebuild and
        // BindIfComplete read this; every authoring surface keeps ChainMode().
        bool   CompilesAsChain() const
        {
            const Arcane::MaterialAssetData* src = CompiledSource();
            return m_surface == 0 && src &&
                   (!src->passes.empty() || !src->baseInputs.empty());
        }
        // The vertex stage this document compiles (fix round 1): the BASE's
        // for an instance (an instance carries no snippets), else its own.
        // m_vsLineOffset is measured against exactly this text, so every
        // vertex-body filter (HasErrors, ForEachDiagnosticRow) reads it too.
        const std::string& CompiledVertexSnippet() const;
        void   BindChainIfComplete();

        // ---- The preview ----
        // Republish m_graphPost / m_graphSpriteBlobs from the blobs and the
        // freshly promoted template+instance. Called from the two bind sites
        // (BindIfComplete, BindChainIfComplete) immediately after
        // PromotePendingInstance -- the desc names the instance the pipelines
        // were just built against, so the two can never describe different
        // compiles.
        void   PublishGraphPreview();
        // Build (once) this document's own offscreen vehicle. No-op without
        // the DocServices graph seam, i.e. everywhere but a --nri-graph editor.
        void   EnsureGraphPreviewContext();
        // Submit + count: a non-zero id is one more job in flight; a zero
        // return is a refused submit (CompilerUnavailable, s3.2).
        std::uint64_t SubmitCompile(Arcane::ShaderCompileRequest req);
        // One preview frame into that vehicle.
        void   RenderGraphPreview(double dt);
        // Re-register the sprite preview material on the OWN device-less
        // batcher -- the only sprite-preview registration path there is.
        void   RefreshGraphSpriteBinding();
        // Invalidate-then-destroy, in the one order that is correct across two
        // contexts. Called from the destructor; idempotent.
        void   DestroyGraphPreview();

        // THE ONE ANSWER TO "what do I draw for this document's preview".
        // `id` is 0 when there is nothing to draw (no vehicle, or nothing
        // bound yet) -- every ImGui::Image site gates on that.
        // The image is square (512), so one extent describes it.
        struct PreviewImage
        {
            ImTextureID id = 0;
            float extent = 0.0f;
        };
        [[nodiscard]] PreviewImage PreviewImageOf() const;

        // Promote pending template -> bound + rebuild the instance over it
        // (parent-chain layering, override migration, dirty re-baseline).
        void   PromotePendingInstance();
        // The snippet buffer the text editor edits: pass 0 = m_snippet, else
        // the extra pass's text. Reference is only stable within the frame.
        std::string& ActiveSnippet();
        // "base" / the extra pass's display name.
        std::string PassLabel(std::size_t pass) const;
        // Descend into a pass: `chainIndex` becomes the active pass and the
        // canvas area swaps from the chain overview to that pass's editing
        // view. Clamped, because callers read ids straight off the canvas.
        void EnterPass(int chainIndex);
        // Record the view the document is CURRENTLY showing as a history entry,
        // truncating any forward branch. Called after an EXPLICIT navigation
        // (enter, crumb click) and never by back/forward itself.
        void NavRecord();
        // Walk the history by `dir` (-1 back, +1 forward). Entries whose pass
        // no longer exists are SKIPPED rather than landed on, so a deleted pass
        // cannot strand the user on a stale index. False when the walk runs off
        // the end (nothing to go back/forward to).
        bool NavStep(int dir);
        // The one-line breadcrumb above the canvas. Root crumb = the material
        // (the chain overview), second crumb = the pass being edited. Drawn in
        // BOTH views, so the overview is always one click away -- which is what
        // keeps Add Pass reachable on a material that opened straight into its
        // graph. Returns the height it consumed.
        float DrawBreadcrumbBar();
        // The pass CANVAS: every pass is a node with a live thumbnail; wires
        // are the DAG (a wire into slot pin k IS inputs[k]). DOUBLE-CLICK a
        // pass = descend into it (selection is pure selection and no longer
        // re-aims the editor); context menu adds/removes and owns the preview
        // truncation; drag wires to rewire. Structural edits recompile.
        // Fills the canvas region -- it is one of the two mutually exclusive
        // views, not a strip above another one.
        void   DrawPassCanvas();
        // Keep execution order == array order after a rewire: stable topo sort
        // (positions/active/view indices ride along). False on a cycle.
        bool   TopoSortPasses();
        // Would wiring `source` into `consumer` (chain indices) close a cycle?
        bool   PassWireWouldCycle(std::uint32_t source, std::uint32_t consumer) const;
        bool   HasErrors() const;
        bool   ParamsDirty() const;   // EffectiveSerial vs the saved baseline
        // Instance mode: walk parent -> ... -> base through the project registry
        // (cycle-guarded). Fills m_parentChain ([immediate parent, ..., base]);
        // false (with a parse error) when a hop cannot resolve or load.
        bool ResolveParentChain();
        // The snippet the compile sees: my own (base) or the chain's base's.
        const std::string& SnippetSource() const;

        void DrawToolbar();
        // The "Save With Errors?" modal: opened from m_confirmSaveWithErrors by
        // whichever of the document window (its toolbar) or the Inspector
        // page (DrawMaterialPageBody) draws first -- a background document
        // tab never runs its toolbar, so the page must be able to raise it.
        void DrawSaveWithErrorsConfirm();
        void DrawSnippetEditor();
        // ---- Diagnostics -> Problems panel (no in-document panel) ----
        // THE formatting seam. One traversal turns every diagnostic this
        // document holds -- graph codegen errors (per pass), parse/stitch
        // errors, vertex-body rows, and compile diags (per pass in chain mode,
        // m_diags otherwise) -- into presentable rows, in that order. It is the
        // ONLY place that ordering and that wording exist. Non-const only
        // because GraphOptAt (the node lookup for graph-error rows) is non-const.
        //
        // One presentable diagnostic row, with the structured source info the
        // Problems panel needs (origin, node id, snippet line) rather than a
        // flattened string -- PublishDiagnostics (public section above) is the
        // one place that flattens it, into an Arcane::Diagnostic per row.
        struct DiagnosticRow
        {
            bool          isError = false;
            std::string   message;      // already prefixed (pass label / "vertex: ")
            int           line    = 0;  // snippet-space line, 0 when not line-bound
            std::uint32_t nodeId  = 0;  // graph node, 0 when not node-bound
        };

        void ForEachDiagnosticRow(Arcane::FunctionRef<void(const DiagnosticRow&)> fn);
        // Graph mode (Slice 9, imgui-node-editor canvas). The canvas edits the
        // ACTIVE pass's graph; these resolve which optional that is.
        std::optional<Arcane::MaterialGraph>& GraphOptAt(std::size_t pass);
        // (pass, id) -> the node, or null. Range-checks the pass FIRST
        // (GraphOptAt silently falls back to the base); null for instances and
        // graphless passes. Never hold the result across frames: create and
        // paste reallocate `nodes`.
        [[nodiscard]] const Arcane::GraphNode* FindGraphNode(std::size_t pass, std::uint32_t id) const;
        [[nodiscard]] Arcane::GraphNode* FindGraphNode(std::size_t pass, std::uint32_t id);
        std::optional<Arcane::MaterialGraph>& ActiveGraphOpt();
        bool ActiveGraphOwned() { return ActiveGraphOpt().has_value(); }
        // Regenerate EVERY graph-owned pass's snippet, then Rebuild. Safe to
        // call on text-only docs (the loop no-ops); any codegen error keeps
        // last-good bound and fills that pass's badge list instead.
        void RegenerateFromGraph();
        // Is this node entirely outside the guard-banded visible canvas rect?
        // The port of SNodePanel::IsNodeCulled -- OUR side of the public API,
        // because the vendored editor has no culling of its own and must not be
        // modified. A node with no measured size yet is never culled.
        [[nodiscard]] bool NodeCulled(std::uint32_t nodeId) const;
        // One pass-canvas wire: transparent ed::Link for interaction + our own
        // curve on top, same two-layer shape the material graph's links use.
        // Raw ids rather than ed:: handles, for the same reason
        // DrawGradientWire takes them: only EditorContext is forward-declared
        // here, so the node-editor types are not nameable in this header.
        void DrawPassWire(std::uint64_t linkId, std::uint64_t fromPinId,
                          std::uint64_t toPinId);
        // Blit one canvas's shader grid backdrop. MUST be called before that
        // canvas's ed::Begin (layering + ScreenToCanvas both require it); the
        // instance is a parameter because the grid's phase is per-canvas state.
        // `phase` is this canvas's grid history. It is a parameter rather
        // than a member read because the document owns TWO canvases (graph and
        // pass) and each keeps its own.
        void DrawCanvasBackdrop(
                                GraphGridPhase& phase);
        void DrawGraphPanel();
        // The graph's two modals -- the HLSL body editor and the param-rename
        // propagation -- hoisted out of DrawGraphPanel (s5.1.7). Drawn by the
        // canvas (inside ed::Suspend) and by the node page (normal ImGui
        // space): whichever draws first consumes the request flag, the other's
        // BeginPopupModal returns false (the DrawSaveWithErrorsConfirm
        // precedent). Takes no graph: the body editor is pass-bound, and the
        // rename walks every pass itself.
        void DrawGraphModals();
        // `lod` is the canvas tier for THIS frame, computed once by
        // DrawGraphPanel before the node loop and branched on at the draw sites
        // inside. Passed rather than stored so there is exactly one read of the
        // zoom per frame and no way for two nodes to disagree.
        void DrawGraphNode(Arcane::GraphNode& node, NodeLOD lod);
        // A canvas pin's paint (T3-D1): PinPaintFor over this frame's
        // m_canvasWidths. The dots, the wire ends and CanvasPinPaint all ask it.
        [[nodiscard]] GraphPinPaint GraphPinPaintOn(const Arcane::GraphNode& node, std::uint32_t pin, bool input) const;
        // The canvas pin tooltip's text (PinTooltipText over this frame's
        // widths and `graph`'s wiring).
        [[nodiscard]] std::string CanvasPinTooltip(const Arcane::MaterialGraph& graph, const Arcane::GraphNode& node,
                                                   std::uint32_t pin, bool input) const;
        // Anchor the CURRENT pin's wire endpoint at `p` (canvas space) and
        // remember it for the frame's gradient wires. Must be called between
        // ed::BeginPin and ed::EndPin. Takes ImVec2 by value; the id is the
        // raw ed::PinId payload, so the header does not need the editor's types.
        void SetPinPivot(std::uint64_t pinId, ImVec2 p);
        // One link wire, source-pin colour at the tail blending to
        // destination-pin colour at the head. Draws into the library's own link
        // layer; the ed::Link submission that owns interaction is separate.
        void DrawGradientWire(std::uint64_t fromPinId, std::uint64_t toPinId,
                              const ImVec4& fromColor, const ImVec4& toColor,
                              bool emphasize) const;
        void HandleGraphEdits();             // link create/delete queries (inside Begin/End)
        // Copy/paste: the clip is GraphToJson of the selected subgraph on the
        // SYSTEM clipboard -- cross-document paste falls out for free, and
        // pasted Param nodes merge into same-name decls by construction.
        // Both run inside the canvas Begin/End (they use ed:: selection and
        // canvas-space coordinates).
        [[nodiscard]] std::string BuildGraphClipJson();   // "" = nothing copyable
        void DeleteCanvasSelection();   // queue selected nodes and links for the canvas delete pass
        void PasteGraphClipText(const char* text);              // ignores foreign clips
        // One undo step per completed graph gesture: `before` was captured at
        // the gesture start; `after` is read from the graph at push time. The
        // live edit already happened (ICommand contract).
        //
        // The pass-taking overload is for DEFERRED pushes -- a gesture whose
        // command builds at CLOSE rather than at the edit. Such a push can land
        // after the ACTIVE pass has moved (the pass canvas is submitted before
        // the graph panel, and an abandoned gesture closes later still, at the
        // document's ScopeGuard), so BOTH the pass index and the `after` it
        // reads must be the ones pinned when `before` was captured. Pairing
        // pass B's index with pass A's `before` would make Undo overwrite pass
        // B's graph with pass A's.
        void PushGraphUndo(const char* label, std::optional<Arcane::MaterialGraph> before);
        void PushGraphUndo(const char* label, std::optional<Arcane::MaterialGraph> before,
                           std::size_t pass);
        // One undo step per completed pass-canvas gesture (after = current).
        void PushPassUndo(const char* label, PassListState before);
        // ---- Node-edit plumbing shared by the canvas and the node page (s5.1.4/5.1.5) ----
        // Nodes resolve through FindGraphNode (one (pass, id) lookup, one set
        // of guards); never cache the result across frames.
        // The gesture close step, lifted from DrawGraphNode's buildGraphEdit:
        // `before` and `pass` pinned NOW; at close, one PushGraphUndo(label,
        // before, pass) unless the graph compares equal (no junk step).
        [[nodiscard]] std::function<void()> GraphEditBuilder(const char* label, std::size_t pass);
        // m_dirty always; RegenerateFromGraph only when m_live (was `valueEdited`).
        void NoteGraphValueEdited();
        // Esc mid-drag on a canvas INLINE value drag (T3-D3), the node page's
        // NumericRow rule (PropertyGrid.cpp): call right after the drag widget
        // with `pre`, the values (and whether a pin literal existed) captured
        // BEFORE the widget ran. On the widget's activation it latches them;
        // while it is active with the left button held, an Esc writes them back
        // into `values` (and `*existed`), clears the slot, calls ClearActiveID
        // and returns true -- the caller re-applies them and closes the gesture
        // through EditGesture::EndAfterRow(cancelled), whose builder sees the
        // graph equal to `before` and pushes NO step.
        bool CanvasDragEscape(const float (&pre)[4], bool preExisted, float* values, int lanes,
                              bool* existed = nullptr);
        // One DISCRETE edit as one step: re-resolve, capture before, mutate, then
        // NoteGraphValueEdited (or m_dirty only when !recompile -- Comment text)
        // and PushGraphUndo(label, before, pass). Nothing when the node is gone or
        // the graph compares equal.
        bool RunNodeEdit(const char* label, std::size_t pass, std::uint32_t id,
                         Arcane::FunctionRef<void(Arcane::GraphNode&, Arcane::MaterialGraph&)> mutate,
                         bool recompile = true);
        // The InputActionsInspectorPage::Defer rule: queued while the node page
        // draws (run in order after its last section, so no edit invalidates the
        // row loop), run at once otherwise (a TextRow draft flushed by CommitOrphans).
        void DeferNodeEdit(std::function<void()> fn);
        bool NodeBadged(std::uint32_t nodeId) const;
        void RebuildDiagBadges();            // compile diags -> line map -> node ids
        void DrawPreviewPanel(ImVec2 size);   // the bordered preview child: the image fitted, else PreviewBoxText
        // ---- THERE ARE NO PER-NODE PREVIEW THUMBNAILS ----
        // DrawNodePreviewImage below draws the ONE thumbnail the graph canvas
        // has: the Output node's own image, the material's real preview
        // (PreviewImageOf), never a per-node compile.
        // `width` is the node's measured content width (SG parity: the preview
        // spans the node). Zero on a node's first frame -- no width has been
        // measured yet -- which falls back to the minimum thumbnail size.
        void DrawNodePreviewImage(const Arcane::GraphNode& node, float width);
        void DrawRenderingSection(PropertyGrid& grid, Arcane::CommandStack* undo);   // mesh materials only
        void DrawParamsSection(PropertyGrid& grid, Arcane::CommandStack* undo);
        void ResetParamWithUndo(const Arcane::ParamDecl& decl);                     // clear the override, "Reset <name>"
        // R2: what a live param gesture started from, so Esc restores the
        // override FLAG as well as the value (a fresh override would otherwise
        // survive the cancel and push a junk step). Latched by buildParamEdit.
        struct LiveParamSeed { std::uint32_t nameHash = 0; bool hadBefore = false; Arcane::MatParamValue before{}; };
        LiveParamSeed m_liveParamSeed;
        std::optional<MeshMaterialMetadataState> m_cutoffGestureBefore;   // the same, for the cutoff drag
        // True when the ACTIVE surface has something bound to show.
        bool PreviewReady() const;
        void SetParamWithUndo(const Arcane::ParamDecl& decl,
                              const Arcane::MatParamValue& value);
        // One mesh-metadata step for whatever differs between `before` and the
        // document's CURRENT metadata (labelled by the field that changed);
        // nothing when nothing does. SetMeshMaterialMetadataWithUndo and the
        // cutoff drag's gesture close both end here.
        void PushMeshMaterialMetadataUndo(const MeshMaterialMetadataState& before);

        [[nodiscard]] Arcane::CommandStack* UndoStack() const { return m_services.undo ? m_services.undo() : nullptr; }
        DocServices                     m_services;
        std::filesystem::path           m_path;
        Arcane::MaterialAssetData       m_data;      // id/name/kind (+ save target)
        std::string                     m_title;     // display name
        std::string                     m_windowLabel;   // display###stable-guid-id
        std::string                     m_snippet;   // the live edit buffer
        bool                            m_dirty = false;  // TEXT dirtiness (params: ParamsDirty)
        bool                            m_live = true;    // auto-compile on edit
        bool                            m_confirmSaveWithErrors = false;
        // Latched each Draw; read by DocumentHost::FocusedDoc to route Ctrl+S.
        bool                            m_windowFocused = false;

        // Param-dirtiness baseline: the instance serial at the last Save (or
        // rebind carrying no unsaved edits). m_paramsBaseDirty carries unsaved
        // param edits ACROSS an instance swap, whose fresh serial is unrelated.
        std::uint64_t m_savedParamSerial = 0;
        bool          m_paramsBaseDirty = false;

        // Doc-identity handle for undo steps (review M3): commands hold this
        // weakly and forward through the pointee -- the anchor dies with the
        // document (steps go inert), but SURVIVES m_instance swaps.
        std::shared_ptr<ShaderEditorDocument*> m_anchor;

        // Authoring state: bound = what the pass renders (last-good); pending =
        // the latest Rebuild, promoted by BindIfComplete on dual-stage success.
        std::shared_ptr<Arcane::MaterialTemplate>  m_pendingTemplate;
        std::shared_ptr<Arcane::MaterialTemplate>  m_boundTemplate;
        std::shared_ptr<Arcane::MaterialInstance>  m_instance;   // over m_boundTemplate
        std::vector<Arcane::ParamMeta>             m_metas;      // parallel to pending->Params()
        std::vector<Arcane::ParamMeta>             m_boundMetas; // parallel to bound->Params()
        std::vector<std::string>                   m_parseErrors;
        std::vector<Arcane::ShaderDiag>            m_diags;      // last compile (active backend)

        // Chain mode in-flight compile state, parallel to [m_snippet, passes...].
        struct PassJobs
        {
            std::uint64_t vsJob = 0, psJob = 0;
            std::vector<std::uint8_t> vsBytes, psBytes;
            std::vector<Arcane::ShaderDiag> diags;   // ps stage, per pass
        };
        std::vector<PassJobs> m_passJobs;
        std::vector<int> m_passLineOffsets;   // per-pass snippet offset in its hlsl
        // Vertex stage: REPAIR MODE ONLY (graphless base) -- graph-owned
        // materials author it with the Vertex Output node and view it inside
        // the read-only HLSL view (UE's one-code-viewer shape).
        bool m_editVertex = false;
        // The combined read-only HLSL view buffer (pixel body + vertex body);
        // rebuilt each frame it is shown.
        std::string m_generatedView;
        std::vector<Arcane::ShaderDiag> m_vsDiags;
        int  m_vsLineOffset = 0;
        // Validated DAG wiring from the last chain build (what SetChain binds;
        // captured at Rebuild so async binds never race pass-list edits).
        std::vector<std::vector<std::uint32_t>> m_passInputs;
        std::uint32_t m_chainInputSlots = 1;
        // Pass-canvas state (a SECOND node-editor context; node ids are chain
        // index + 1, the Output node is kPassOutputNodeId).
        ax::NodeEditor::EditorContext* m_passCanvasCtx = nullptr;
        bool  m_passCanvasSeeded = false;   // re-seed positions after list edits
        CanvasNavLatch m_passFitPending;    // s4.5: frame-to-fit after a seed, re-issued until it lands
        bool  m_passFitOnSeed = false;      // the next seed is a fresh view (new context / reload): fit it
        std::uint32_t m_passCtxNode = 0;    // node the context menu opened on
        float m_passPopupX = 0.0f, m_passPopupY = 0.0f;
        int m_activePass = 0;   // which snippet the text editor shows (0 = base)
        int m_viewPass = -1;    // preview truncation; -1 = the full chain
        // Which of the two mutually exclusive views owns the canvas area: the
        // chain overview, or m_activePass's editing view. Seeded in the ctor
        // from whether there IS a chain worth surveying, then driven by the
        // breadcrumb (up) and double-click-to-enter (down).
        //
        // m_activePass keeps its value while the overview is up rather than
        // being cleared: the preview, the params panel and the Inspector's
        // material page all key off it, and blanking it on every trip to the overview
        // would make them flicker back to the base and lose the user's place.
        // The overview is a NAVIGATION layer over the document, not a different
        // document -- so "which pass am I editing" survives a look at the map.
        bool m_inChainView = true;

        // ---- Back/forward navigation (mouse4/mouse5) ----
        // Browser semantics over the document's two-level view space. One
        // visited view; `pass` is meaningful only when chainView is false.
        struct ViewEntry
        {
            bool chainView = true;
            int  pass = 0;
            bool operator==(const ViewEntry&) const = default;
        };
        // Visited views, oldest first, with m_navIndex pointing at the CURRENT
        // one -- so back/forward is index arithmetic and nothing else. Explicit
        // navigation truncates everything after the index before appending,
        // which is what makes a new jump abandon the forward branch.
        //
        // IN-MEMORY AND PER-DOCUMENT, deliberately: this is a record of a
        // reading session, not of the asset, so it neither persists nor rides
        // undo. Closing the document forgets it, which is what a browser tab
        // does too.
        std::vector<ViewEntry> m_navHistory;
        int m_navIndex = -1;
        // Capped at editor.shader.navHistoryMax (default 32); the oldest entry
        // drops when it is hit. An uncapped vector on a long session is a leak
        // with extra steps.

        // Instance mode (Slice 7): the resolved ancestry, immediate parent first,
        // BASE (the snippet owner) last. Empty for base materials.
        std::vector<Arcane::MaterialAssetData> m_parentChain;
        bool m_showOnlyOverridden = false;   // instance params filter

        // Preview surface (Slice 8): 0 = fullscreen, 1 = sprite (a
        // QuadMaterial on a checkerboard through the preview canvas's own
        // Batcher2D). Initialized from the material's kind; switching it on a
        // base material re-kinds the asset (structural edit).
        int m_surface = 0;

        std::uint64_t m_vsJob = 0, m_psJob = 0;      // in-flight ids (0 = none)
        std::vector<std::uint8_t> m_vsBytes, m_psBytes;

        // ===== THE PREVIEW ==================================================
        // The device-free description (see GraphPreviewDesc), and the vehicle
        // that renders it, built only when DocServices carried the preview
        // seam. There is no second preview path.
        Arcane::PostChainDesc m_graphPost;
        SpriteBlobs           m_graphSpriteBlobs;
        // Square (editor.shader.previewResolution, default 512), read ONCE when
        // the document opens and FIXED for its life -- and that is load-bearing
        // rather than cosmetic: a preview that never resizes has no
        // ResizeOffscreen seam at all, so the ONLY InvalidateUserTextureNow this
        // document owes is the one at destruction (NriGraphContext.hpp, item (2)).
        std::uint32_t m_graphPreviewSize =
            static_cast<std::uint32_t>(Arcane::Settings<ShaderEditorSettings>().previewResolution);
        std::unique_ptr<Arcane::NriGraphContext> m_graphPreview;
        // ===== The late-bound seam's state (s3.2) =====
        Arcane::RenderOverlayNode* m_previewHud = nullptr;     // the chrome node captured at vehicle creation
        bool          m_previewVehicleFailed = false;     // CreateOffscreen returned null: Tick stops retrying
        bool          m_previewFrameFailed   = false;     // a frame failed (vehicle dropped); the next Presented frame clears it
        std::uint32_t m_previewVehicleAttempts = 0;
        // T3-D6: a MESH-surface material previews as the thumbnail's lit
        // sphere (MaterialSpherePreview.hpp), rendered live in this vehicle
        // from the bound instance's CURRENT baseColor/albedo. Shared, not a
        // member value: the vehicle's mesh supply holds it, and a retired
        // vehicle can outlive this document by a frame (retireGraphPreview).
        std::shared_ptr<const Arcane::MeshData> m_previewSphere;
        bool          m_meshPreviewPresented = false;     // a sphere frame landed on this vehicle
        std::uint32_t m_jobsInFlight = 0;                 // non-zero Submits not yet answered by ConsumeResult
        bool          m_submitRefused = false;            // a Submit since the last invalidation returned 0
        // The document's OWN device-less batcher -- the sprite surface's
        // recorder, and the checkerboard backdrop's for every surface. Owned
        // rather than shared with the editor's scene batcher: this frame is
        // declared from Tick (phase 13), long after phase 10 drained that one,
        // and two owners of one batcher is how two frames' content merges.
        std::unique_ptr<Arcane::Batcher2D> m_graphBatch;
        std::uint16_t m_graphSpriteMaterial = 0xFFFF;   // id on m_graphBatch
        // Which blobs m_graphSpriteMaterial was registered from -- a re-compile
        // is a NEW shared_ptr, which is what makes "did this change" a pointer
        // compare rather than a memcmp.
        const void* m_graphSpriteStamp = nullptr;
        double m_animTime = 0.0;                     // preview Time uniform
        // 1-based; 0 = no pending jump. Re-armed by RequestJumpToLine (Task 5,
        // the Problems panel) -- its previous driver, the old errors panel,
        // was removed.
        int    m_jumpToLine = 0;
        bool   m_focusSnippet = false;               // focus the input so the jump lands
        // Armed jump consumed by the input callback. Per-document (review m2): a
        // shared static could deliver one doc's jump to another on a same-frame
        // focus race.
        int    m_callbackJumpLine = 0;

        // The Inspector page's selection (key "material", epoch 1 = selected at
        // open) and the page itself. m_page holds a reference to *this;
        // documents live behind unique_ptr in DocumentHost and never move.
        DocumentPageSelection  m_pageSel{ "material" };
        // ---- The node page's selection mirror (node page s5.1.1) ----
        // m_nodeSel: the canvas's one selected node, rebuilt after ed::End on
        // every DRAWN canvas frame and kept as-is when the canvas does not
        // draw (HLSL view, background tab). Canvas WRITES go only through
        // m_nodeSelRequest, applied inside ed::Begin/End (DrawGraphPanel);
        // m_nodeSelApplying marks that frame so the read raises no event.
        std::optional<NodeKey> m_nodeSel;
        struct NodeSelRequest
        {
            enum Op : std::uint8_t { Select, Clear };
            Op            op = Select;
            std::uint32_t id = 0;
        };
        std::optional<NodeSelRequest> m_nodeSelRequest;
        bool m_nodeSelApplying = false;
        MaterialInspectorPage  m_page{ *this };
        NodeInspectorPage      m_nodePage{ *this };

        // The document's ONE edit-gesture bracket (EditGesture). TWO draw
        // scopes open gestures against it -- Draw (the graph's value/pin
        // drags) and DrawMaterialPageBody (the param rows) -- so BOTH declare a
        // ScopeGuard as their first local. The page body draws AFTER the
        // document (the Inspector phase, EditorApp's DrawSelectionPanels, runs
        // after DrawEditorUi's m_documents.DrawAll), so Draw's guard is no
        // longer last in the frame: a gesture the page body opens is closed by
        // the body's own guard, and a gesture parked across frames is closed
        // by whichever guard runs once its widget has deactivated -- the
        // EditGesture ownership and abandonment rules, not the draw order, are
        // what guarantee every gesture closes.
        // Param-panel drags carry
        // the override value, graph value drags the WHOLE graph (small graphs --
        // the SG full-snapshot-undo pathology was per-edit reserialization plus
        // full preview regeneration, neither of which applies here). Before-
        // state on activation, one undo step at close. Per-document (review m3):
        // same-frame keyboard-nav active-ID transfer between documents could
        // cross shared statics.
        EditGesture::GestureState m_gesture;
        // CanvasDragEscape's seed: the activating item (0 = none) and its values
        // at activation. One slot: one ImGui item is active at a time.
        struct CanvasDragSeed
        {
            std::uint32_t item = 0;
            float v[4] = {};
            bool existed = true;
        };
        CanvasDragSeed m_canvasDragSeed;

        // Latched on the frame the ConstColor node's swatch opens its popup:
        // the Old half of ColorPopupBody's Old/New pair. (The material page's
        // Color param rows are PropertyGrid::ColorRow since s5.3, which keeps
        // its own draft.)
        float m_colorPopupOriginal[4] = {};

        // The node page's queued discrete edits (DeferNodeEdit), whether its body
        // is mid-draw, and whether the pin literal under the live gesture existed
        // at activation (s5.1.5: a cancelled or back-to-neutral drag leaves no NEW
        // literal). One gesture at a time per document, so one flag suffices.
        std::vector<std::function<void()>> m_nodePageEdits;
        bool m_nodePageDrawing = false;
        bool m_nodePageLiteralExisted = false;

        // ---- Graph mode (Slice 9; per-pass graphs) ----
        ax::NodeEditor::EditorContext* m_graphCtx = nullptr;   // lazy; dtor destroys
        // The two canvases' grid PHASE. There is no shader-backed lattice:
        // the ImGui-primitive one DrawGraphGridFallback draws from these IS
        // the canvas grid. Two of them, by value, because the views alternate
        // and each keeps its own pan/zoom history.
        GraphGridPhase m_gridPhase{};
        GraphGridPhase m_passGridPhase{};
        // Pass-canvas node widths, keyed by pass-canvas node id. Its own map:
        // chain-index-derived ids and graph node ids are unrelated counters.
        std::unordered_map<std::uint32_t, float> m_passNodeWidths;
        // The visible canvas rect, guard-banded, in CANVAS units. Recorded once
        // per canvas per frame by DrawCanvasBackdrop (the one point that holds
        // the screen rect before ed::Begin) and consumed by NodeCulled during
        // node submission. Invalid = cull nothing.
        ImVec2 m_cullMin{}, m_cullMax{};
        bool   m_cullRectValid = false;
        // Nodes culled on the LAST graph-canvas submission. WRITE-ONLY today
        // -- cleared and inserted into, never read -- because the per-node
        // preview path that used to read it one frame late no longer exists.
        // Left in place rather than pruned.
        std::unordered_set<std::uint32_t> m_culledGraphNodes;
        // Each node's measured width from the LAST frame it drew, keyed by node
        // id. Right-aligned output rows and full-width previews both need a
        // width that only exists after the node has been laid out, so they use
        // the previous frame's -- which is stable, because aligning to width W
        // produces rows of exactly W. Cleared with the canvas on a pass switch.
        std::unordered_map<std::uint32_t, float> m_nodeWidths;
        // THIS FRAME's wire anchor for every pin submitted, keyed by ed::PinId.
        // Written at BeginPin/EndPin time by SetPinPivot, which hands the SAME
        // point to ed::PinPivotRect -- so the gradient wires drawn afterwards
        // start from the library's own endpoints instead of re-deriving them.
        // Rebuilt every frame (positions move with the node and the view).
        std::unordered_map<std::uint64_t, ImVec2> m_pinPivots;
        // THIS FRAME's (source, destination) tint per link, in `links` order:
        // exactly the pair the link loop handed DrawGradientWire. Rebuilt with
        // m_pinPivots; read only by the CanvasWireTintOf test seam.
        std::vector<CanvasWireTint> m_wireTints;
        // THIS FRAME's dynamic-width resolution of the drawn graph
        // (Arcane::ResolveGraphNodeWidths -- codegen's own), taken ONCE before
        // the node loop. Every pin dot, wire end and pin tooltip reads it.
        std::unordered_map<std::uint32_t, Arcane::GraphNodeWidths> m_canvasWidths;
        // The pin a canvas pin row reported hovered (for a tooltip) this frame.
        struct PinTipTarget
        {
            std::uint32_t node = 0, pin = 0;
            bool isInput = false, valid = false;
        };
        PinTipTarget m_pinTip;
        bool m_pinLegendHovered = false;   // GraphPinLegendInteract's answer, for the paint after ed::End
        // Per-pass codegen state, indexed by CHAIN index (0 = base). Sized by
        // RegenerateFromGraph; empty entries = text-owned or clean.
        std::vector<std::vector<Arcane::GraphError>> m_passGraphErrors;
        std::vector<std::vector<std::uint32_t>> m_passLineNodeIds;   // GOOD line maps
        std::vector<std::uint32_t> m_diagBadgeNodes;     // ACTIVE pass's diag nodes
        int m_graphShownPass = -1;   // canvas re-seeds + reselects on pass switch
        // First snippet line's 0-based offset inside the stitched HLSL -- maps
        // compiler diag lines back into snippet space (jump + badges).
        int  m_snippetLineOffset = 0;
        bool m_graphPositionsApplied = false;   // canvas seeded from stored node positions
        CanvasNavLatch m_fitPending;            // s4.5: frame-to-fit after a pass-switch seed (fresh context), re-issued until it lands
        bool m_showGeneratedText = false;       // toolbar toggle: canvas <-> read-only HLSL
        // Select + navigate the canvas to one node. Re-armed by
        // RequestFocusGraphNode (Task 5, the Problems panel) -- the errors
        // panel's rows were its only writer before that panel was removed;
        // DrawGraphPanel still consumes it (ed::SelectNode + NavigateToSelection),
        // re-issuing until m_focusPending confirms the view landed.
        std::uint32_t m_focusNode = 0;
        CanvasNavLatch m_focusPending;
        float m_graphPopupX = 0.0f, m_graphPopupY = 0.0f;   // create-menu screen pos
        // Drag-wire searcher (SG's signature interaction): releasing a new wire
        // over empty canvas opens the create menu filtered to types with a pin
        // on the wire's far side, and the created node auto-connects. The
        // request flag carries the accept from HandleGraphEdits (canvas space)
        // into the Suspend'ed popup block; m_wireActive spans the popup.
        bool          m_wireCreateRequest = false;
        bool          m_wireActive = false;
        bool          m_wireIsInput = false;    // dragged pin is an input pin
        std::uint32_t m_wireNode = 0, m_wirePin = 0;
        char          m_createSearch[64] = {};  // create-menu search filter
        // In-progress inline text edits (pass name, comment title, param/
        // texture name, swizzle mask) all share ONE StableTextEdit buffer --
        // only one InputText is active at a time. The key is namespaced by SITE
        // KIND because a pass's CHAIN INDEX and a node's ID are unrelated
        // counters that would otherwise collide on the same small number (the
        // pre-widget-layer code kept two separate members for exactly that
        // reason); the kind tag restores the separation inside one slot.
        enum class TextEditKind : std::uint64_t { PassName = 1, NodeName, Swizzle, Comment };
        static constexpr std::uint64_t TextKey(TextEditKind k, std::uint64_t id) noexcept
        { return (static_cast<std::uint64_t>(k) << 56) | id; }
        TextCommitState m_textEdit;
        // Custom-node HLSL body editing: the node shows a plain-text preview
        // (child-window widgets drift inside the canvas); the body edits in a
        // MODAL. Request set by RequestBodyEdit (the canvas button, the node
        // page), consumed in DrawGraphModals; the buffer holds the working
        // copy until Apply commits it as one undo step.
        std::uint32_t m_bodyEditRequest = 0;
        std::size_t   m_bodyEditPass = 0;   // the pass the request AND the open modal edit
        std::uint32_t m_bodyEditNode = 0;
        char          m_bodyBuf[4096] = {};

        // ---- Assisted param rename (graph tier only; text //@param edits are
        // not reliably detectable as renames) ----
        struct RenameTarget
        {
            Arcane::Guid id;
            std::filesystem::path path;
            std::string name;
        };
        // Rename-commit hook: sole-declarer guard, local fix, registry walk
        // for dependent instance files; a nonempty hit list arms the modal.
        void BeginParamRename(const std::string& oldName, const std::string& newName);
        // Pending renames awaiting MATERIALIZATION: override-hash migration at
        // PromotePendingInstance (conditional on the target template, so an
        // undo rolling the template back translates in reverse) and saved-name
        // translation at instance Save.
        [[nodiscard]] std::uint32_t TranslateOverrideHash(
            std::uint32_t hash, const Arcane::MaterialTemplate& templ) const;
        std::vector<std::pair<std::string, std::string>> m_paramRenames;
        std::vector<RenameTarget> m_renameTargets;   // the modal's hit list
        std::string m_renameOld, m_renameNew;
        bool m_renameRequest = false;   // open the modal (Suspend space)

        // ---- The "Thumbs" toggle --------------------------------------------
        // It gates DrawNodePreviewImage's Output-node branch, which is a real,
        // live preview: the material's own image, drawn on the Output node --
        // see PreviewImageOf's 3 call sites. It gates NOTHING ELSE; there is
        // no per-node thumbnail machinery behind it.
        bool m_showNodePreviews = true; // toolbar toggle; gates the Output-node preview only

        friend struct SnippetCallbackForwarder;
    };
}
