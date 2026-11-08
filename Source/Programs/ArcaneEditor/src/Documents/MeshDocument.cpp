#include "Documents/MeshDocument.hpp"
#include "Input/EditorActions.hpp"

#include "Panels/AssetPanelModel.hpp"         // AssetKind (the material row's kind filter)
#include "Panels/AssetReferenceField.hpp"     // AssetRefRow / AssetRefArgs / AssetRefEdit
#include "Documents/PreviewStatus.hpp"
#include "Widgets/EditorTheme.hpp"     // Theme::kError: the validation reason's colour
#include "Widgets/EditorWidgets.hpp"   // CenteredTextDisabled
#include "Widgets/PropertyGrid.hpp"

// The preview vehicle. Include-order note for anything moved above it: this
// reaches <NRI.h> and Extensions/NRIDeviceCreation.h, whose nri::Message
// enumerator is literally named ERROR -- the same collision every file under
// Render/Nri documents, and the same reason ShaderEditorDocument.cpp keeps
// this block where it is rather than first.
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>        // MeshSceneDesc / MeshInstance
#include <Arcane/Render/Nri/RenderOverlayNode.hpp>    // InvalidateUserTextureNow
#include <Arcane/Host/HostConfig.hpp>                  // CreateOffscreen's knobs

#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Material/MaterialSource.hpp>   // MaterialSurface (the material row's surface filter)

#include <Astra/Reflection/Attribute.hpp>   // Astra::Range

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>     // lookAtRH / perspectiveRH_ZO

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <Arcane/Core/Constant.hpp>

namespace Arcane::Editor
{
    namespace
    {
        // THE ONE SPELLING of the synthetic guid this document's preview geometry is
        // resident under on its own offscreen vehicle ('PRVW'). It appears in three
        // places -- the vehicle's mesh supply, the single MeshInstance, and
        // RebuildPreviewMesh's invalidate -- and final-review C2 is what happens when
        // one of the three is missing, so it is a constant rather than three literals
        // that can drift. NOT an asset guid: nothing in the project registry mints
        // it, and it never leaves this document's vehicle.
        ARC_CONSTANT("ID space: the fixed GUID of the mesh preview")
        inline constexpr Arcane::Guid kPreviewMeshGuid{ 0x50525657ull, 1ull };

        // One completed field gesture (or single-frame commit) as an undo
        // step -- same shape and doc-identity reasoning as SpriteDocument.cpp's
        // SpriteDataEditCommand (:31-59): the step holds the DOCUMENT weakly
        // through an anchor and forwards to whatever it currently points at,
        // because a raw MeshDocument* dangles the moment the document closes
        // with steps still on the shared stack, and the stack outlives every
        // document (EditorApp owns it; DocumentHost::Close erases the
        // document synchronously).
        class MeshDataEditCommand final : public Arcane::ICommand
        {
        public:
            MeshDataEditCommand(std::weak_ptr<MeshDocument*> anchor, std::string label,
                                Arcane::MeshAssetData before, Arcane::MeshAssetData after)
                : m_anchor(std::move(anchor)), m_label(std::move(label)),
                  m_before(std::move(before)), m_after(std::move(after))
            {
            }

            void Undo() override { Apply(m_before); }
            void Redo() override { Apply(m_after); }
            const char* Label() const override { return m_label.c_str(); }
            // Spec s3.3: a document step never dirties the scene, and it is
            // EXPIRED once its document closed (the anchor died).
            bool AffectsScene() const override { return false; }
            bool IsExpired() const override
            {
                const auto doc = m_anchor.lock();
                return !doc || !*doc;
            }

        private:
            void Apply(const Arcane::MeshAssetData& data)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyMeshData(data);
            }

            std::weak_ptr<MeshDocument*> m_anchor;
            std::string                  m_label;
            Arcane::MeshAssetData        m_before;
            Arcane::MeshAssetData        m_after;
        };
    }

    MeshDocument::MeshDocument(Services services, std::filesystem::path path,
                              Arcane::MeshAssetData data)
        : m_services(std::move(services)), m_path(std::move(path)), m_data(std::move(data))
    {
        // Same name-fallback rule as SpriteDocument/ShaderEditorDocument: an
        // empty asset name (hand-authored file, or a pre-name-field asset)
        // falls back to the file stem rather than showing a blank title.
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + " (Mesh)###meshdoc_" + m_data.id.ToString();
        // The anchor every undo step routes through; it dies with the document.
        m_anchor = std::make_shared<MeshDocument*>(this);

        RebuildPreviewMesh();
        // No bind/compile event to wait for (BuildMeshData is synchronous and
        // pure), so the vehicle is built right here when the seam is already
        // up; a document built during boot gets it from Tick's retry instead
        // (s3.2). This IS the "device-less services allocate no preview
        // resources" behaviour: with chromeGraph/hostConfig unset, or
        // chromeGraph() still null during boot, this call is a null check
        // that never latches.
        EnsurePreviewContext();
    }

    MeshDocument::~MeshDocument()
    {
        // Same close-a-parked-gesture reasoning as SpriteDocument's dtor
        // (SpriteDocument.cpp:76-103): a close that lands between a gesture
        // parking and its next Draw would otherwise strand InTransaction()
        // true editor-wide.
        if (UndoStack())
            EditGesture::ClosePending(*UndoStack(), m_gesture);

        // LAST: the chrome context's ImGuiNri may hold a view over this
        // document's preview output, so that view has to be handled before
        // (or as part of) the texture's teardown -- DestroyPreviewContext
        // carries the ordering.
        DestroyPreviewContext();
    }

    void MeshDocument::NoteMoved(const std::filesystem::path& p)
    { m_path = p; m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name; m_windowLabel = m_title + " (Mesh)###meshdoc_" + m_data.id.ToString(); }

    // T5 s7.5: what Save would write -- importedSource + every slot's material (the .arcmesh branch).
    std::vector<Arcane::Guid> MeshDocument::LiveReferences() const
    {
        std::vector<Arcane::Guid> o;
        if (m_data.importedSource.IsValid())
            o.push_back(m_data.importedSource);
        for (const auto& s : m_data.slots)
            if (s.material.IsValid())
                o.push_back(s.material);
        return o;
    }

    void MeshDocument::FlushGesture()
    {
        if (Arcane::CommandStack* s = UndoStack())
            EditGesture::ClosePending(*s, m_gesture);
    }

    void MeshDocument::ApplyMeshData(const Arcane::MeshAssetData& data)
    {
        m_data = data;
        // Coarse ledger, same rationale as SpriteDocument::ApplySpriteData:
        // undoing all the way back to the saved bytes still reads dirty. That
        // errs toward a redundant save, never toward silently dropping one.
        m_dirty = true;
        RebuildPreviewMesh();
        // Same republish Save does, and for the same reason: RebuildPreviewMesh
        // above only refreshes THIS WINDOW's image. The scene's MeshCache still
        // holds the pre-undo geometry, so without this the viewport keeps
        // drawing it -- and an undo the user cannot see in the scene is
        // indistinguishable from an undo that did not happen (SpriteDocument.
        // cpp:113-121, the precedent this mirrors exactly).
        if (m_services.invalidateMesh)
            m_services.invalidateMesh(m_data.id);
    }

    void MeshDocument::PushDataEdit(std::string label, const Arcane::MeshAssetData& before)
    {
        // No stack (Play mode / an unwired document) or nothing actually
        // moved -> no step -- identical guard to SpriteDocument::PushDataEdit.
        if (!UndoStack() || before == m_data)
            return;
        UndoStack()->Push(std::make_unique<MeshDataEditCommand>(
            m_anchor, std::move(label), before, m_data));
    }

    void MeshDocument::ClearSlotMaterial(Arcane::MeshAssetData& data, std::size_t slot)
    {
        if (slot >= data.slots.size())
            return;
        if (data.source == Arcane::MeshSource::Imported)
        {
            // Keep the slot, drop the assignment -- see the declaration
            // (MeshDocument.hpp) for the positional-correspondence rule.
            data.slots[slot].material = Arcane::Guid{};
            return;
        }
        // Erase, not nil-out: an unnamed slot with a nil material is a
        // different (and here, unreachable-through-this-picker) state from
        // "no slot at all" -- the same "nil legacy material yields NO slot"
        // rule MeshAsset.cpp's loader applies.
        data.slots.erase(data.slots.begin() + static_cast<std::ptrdiff_t>(slot));
    }

    void MeshDocument::ClearPrimarySlotMaterial(Arcane::MeshAssetData& data) { ClearSlotMaterial(data, 0); }

    void MeshDocument::CommitDataEdit(const char* label, const Arcane::MeshAssetData& before)
    {
        // Single-frame commit for non-drag edits (the source combo, a material
        // Set/Clear): there is no ActiveId to bracket around a Selectable click
        // or a drop, so this pushes directly instead of through EditGesture --
        // the "no gesture bracketing needed" idiom ShaderEditorDocument::
        // SetParamWithUndo documents for its own texture-param drops.
        if (before == m_data)
            return;
        m_dirty = true;
        RebuildPreviewMesh();
        PushDataEdit(label, before);
    }

    void MeshDocument::ApplySlotMaterialEdit(std::size_t slot, const AssetRefEdit& edit)
    {
        if (edit.op == AssetRefEdit::Op::None)
            return;
        const Arcane::MeshAssetData before = m_data;
        const bool imported = m_data.source == Arcane::MeshSource::Imported;
        if (edit.op == AssetRefEdit::Op::Set)
        {
            // A generated mesh's first assignment creates its one unnamed slot
            // (the F2a single-material UX); an imported mesh never grows its
            // slot table -- it mirrors the artifact's by position.
            if (!imported && m_data.slots.empty())
                m_data.slots.push_back(Arcane::MeshSlot{ std::string(), edit.guid });
            else if (slot < m_data.slots.size())
                m_data.slots[slot].material = edit.guid;
            CommitDataEdit("Assign Material", before);
            return;
        }
        ClearSlotMaterial(m_data, imported ? slot : 0);
        CommitDataEdit("Clear Material", before);
    }

    void MeshDocument::RebuildPreviewMesh()
    {
        // Independent calls, deliberately: BuildMeshData already returns
        // nullopt exactly when ValidateMeshAsset refuses (MeshAsset.hpp's own
        // contract), but the STRING reason only comes from the validator, and
        // the panel needs both -- "no geometry" (PreviewMesh) and "here is
        // why" (ValidationReason) are two different things a caller reads.
        m_validationReason = Arcane::ValidateMeshAsset(m_data);
        // I4: an IMPORTED mesh has no procedural geometry to preview --
        // BuildMeshData's Imported arm is "wrong function, ask ResolveMeshData"
        // and ARC_WARNs to say so, which every edit gesture in this document
        // would otherwise trigger. Its geometry is the cooked artifact's,
        // resolved through the scene (Plan 2's residency) -- the panel says
        // exactly that in place of the preview. The validator still runs above
        // (a nil importedSource is a real, reportable refusal).
        m_previewMesh = (m_data.source == Arcane::MeshSource::Imported)
            ? std::nullopt
            : Arcane::BuildMeshData(m_data);
        // The ONE place the preview's inputs move, so the ONE place that owes
        // the redraw flag -- see m_previewDirty (MeshDocument.hpp) for why a
        // static image must not re-record a frame every Tick.
        m_previewDirty = true;

        // ...AND THE ONE PLACE THAT OWES THE GPU DROP (final-review C2). The redraw
        // flag alone was enough while MeshNode re-uploaded from &*m_previewMesh every
        // record; since Plan 2 Task 4 the geometry goes through the vehicle's
        // NriMeshBufferCache, which caches by guid and only consults the supply on a
        // MISS. Without this the vehicle keeps serving the FIRST shape this document
        // ever built -- through all four rebuild paths (ctor, ApplyMeshData, Save,
        // and the page's live field edits), i.e. every topology drag, every source
        // switch, every undo. It lives here, beside the flag, rather than at the four
        // call sites, for exactly the reason the flag does.
        //
        // The counter is bumped whether or not a vehicle exists, so a device-less
        // test can pin which paths issue the drop -- see its accessor's comment.
        ++m_previewGeometryInvalidations;
        if (m_preview)
            m_preview->InvalidateMeshGeometry(kPreviewMeshGuid);
    }

    bool MeshDocument::Save()
    {
        if (!Arcane::SaveMeshAsset(m_path, m_data))
            return false;   // failed save: m_dirty stays set, edits are not silently lost
        m_dirty = false;
        // The SCENE's MeshCache resolve this replaces is otherwise permanent
        // (once-per-Guid, Render/MeshCache.cpp) -- without this call every
        // MeshRenderer referencing this asset keeps drawing the PRE-edit
        // geometry until the project is switched. This document's own preview
        // needs no such hook; see the file-top block on MeshDocument.hpp for
        // the two-cache split, and Services::invalidateMesh for the
        // borrow-lifetime rule this call site satisfies.
        if (m_services.invalidateMesh)
            m_services.invalidateMesh(m_data.id);
        return true;
    }

    void MeshDocument::Tick(double)
    {
        // The whole render phase, and only when the image can actually have
        // changed: DocumentHost::TickAll ticks EVERY open document every
        // editor frame with no visibility gate (DocumentHost.cpp:152-156), and
        // nothing in this preview animates -- see m_previewDirty. A missing
        // vehicle simply means no preview this Tick -- the same
        // degraded-not-fatal outcome EnsurePreviewContext and RenderPreview
        // both already carry.
        //
        // THE LATE-BOUND SEAM'S RETRY (s3.2): a document built during boot has
        // no vehicle yet. Not after a creation refusal (the latch) and not
        // after a frame failure (today's drop stays permanent for a mesh).
        if (!m_preview && !m_previewVehicleFailed && !m_previewFrameFailed)
            EnsurePreviewContext();
        if (m_preview && m_previewDirty)
            RenderPreview();
    }

    // =========================================================================
    // THE PREVIEW
    // =========================================================================
    void MeshDocument::EnsurePreviewContext()
    {
        // The seam is unset in every headless test (no EditorApp at all),
        // and its resolver returns null during boot: either way this is a
        // null check that never latches (s3.2).
        if (m_preview || !m_services.hostConfig || !m_services.chromeGraph)
            return;
        Arcane::NriGraphContext* chrome = m_services.chromeGraph();
        if (!chrome)
            return;   // the seam is not up yet (boot): a null check, never a latch

        // Default NodeSet{} -- this preview draws no chrome, no game HUD and
        // has nothing to pick, exactly like ShaderEditorDocument's own
        // graph-preview vehicle.
        ++m_previewVehicleAttempts;
        m_preview = Arcane::NriGraphContext::CreateOffscreen(
            *m_services.hostConfig, chrome->Device(), m_previewSize, m_previewSize);
        if (!m_preview)
        {
            // Degraded, not fatal: no preview image, already logged inside
            // CreateOffscreen. The latch stops Tick retrying (s3.2).
            m_previewVehicleFailed = true;
            ARC_WARN("MeshDocument '{}': the preview context could not be created -- "
                     "this document shows no preview", m_title);
            return;
        }
        m_previewVehicleFailed = false;
        m_previewHud       = chrome->ImGuiHud();
        m_previewPresented = false;
        m_previewDirty     = true;   // a fresh texture holds nothing yet
        m_preview->SetMeshSupply(
            [this](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
            {
                if (id == kPreviewMeshGuid && m_previewMesh)
                    return { &*m_previewMesh, Arcane::MeshResolveState::Ready };
                return { nullptr, Arcane::MeshResolveState::Failed };
            });
    }

    void MeshDocument::RenderPreview()
    {
        if (!m_preview)
            return;

        // ALWAYS records a frame below, even when m_previewMesh is nullopt
        // (an invalid param set) -- fix-round Finding 2. Returning early here
        // (the pre-fix shape) left the offscreen texture holding whatever
        // undefined bytes CreateOffscreen's creation left in it, because
        // OffscreenTextureId() reports non-zero -- "there is a texture" -- the
        // INSTANT CreateOffscreen succeeds, independent of whether any frame
        // has ever rendered into it (NriGraphContext.cpp:582-588). `scene`
        // stays default-constructed (empty instances, identity view/
        // projection) on the invalid-data path: NriGraphContext's
        // `wantsMesh` gate (`shape.mesh != nullptr && !shape.mesh->
        // instances.empty()`, NriGraphContext.cpp:1186) reads an EMPTY span
        // as "no mesh pass this frame", so MeshNode is never even declared
        // and only Batch2DNode's plain clear colour (kCanvasClear,
        // Batch2DNode.cpp:45) lands in the texture -- clean, not garbage.
        Arcane::MeshInstance instanceStorage[1]{};
        Arcane::MeshSceneDesc scene;
        if (m_previewMesh)
        {
            // Frame the WHOLE mesh regardless of source/topology: a
            // bounding-sphere radius from ComputeMeshBounds' AABB (half the
            // diagonal, a safe over-estimate for any view direction) placed
            // at a distance that keeps it inside the vertical FOV, plus a
            // margin so the mesh never touches the frame edge.
            const Arcane::MeshBounds bounds = Arcane::ComputeMeshBounds(*m_previewMesh);
            const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
            float radius = glm::length(bounds.max - bounds.min) * 0.5f;
            if (!(radius > 1e-4f))
                radius = 0.5f;   // degenerate-bounds guard; unreached by any of the five generators today

            const MeshDocSettings& ms = Arcane::Settings<MeshDocSettings>();
            const float fovYDegrees = ms.previewFov;
            const float distance = radius / std::sin(glm::radians(fovYDegrees * 0.5f)) * ms.previewMargin;
            // A fixed three-quarter viewing direction -- there is no camera
            // authoring here (F4's job), so one angle that reads every one of
            // the five sources reasonably is enough; +Y up matches the
            // engine-wide convention (SceneCamera.hpp).
            const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.75f, 1.0f));
            const glm::vec3 eye = center + dir * distance;

            instanceStorage[0].mesh = kPreviewMeshGuid;
            instanceStorage[0].model = glm::mat4(1.0f);   // unit geometry -- see MeshAsset.hpp's UNIT RULE
            // Neutral white: resolving `material`'s actual baseColor/texture
            // into this preview is F2b/F2c's bindless-table territory (the
            // material Guid field here is authored, not rendered-through) --
            // deliberately out of this task's scope. The preview's job is to
            // show the SHAPE.
            instanceStorage[0].baseColor = glm::vec4(1.0f);
            scene.instances = std::span<const Arcane::MeshInstance>(instanceStorage, 1);

            // RH, depth [0,1] -- the same convention SceneCamera.hpp's
            // PerspectiveProjection/ActivePerspectiveSceneCamera document at
            // length; restated by direct call here rather than by including
            // that ECS-facing header, which this device-less, registry-free
            // preview has no other reason to depend on.
            scene.view = glm::lookAtRH(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
            scene.projection = glm::perspectiveRH_ZO(glm::radians(fovYDegrees), 1.0f,
                                                      0.05f, distance * 4.0f + 1.0f);
            // The ONE preview light (editor.preview.light.*, R1), shared with
            // the material sphere and the thumbnails; normalized here as this
            // preview always has. Its ambient is brighter than MeshSceneDesc's
            // own default (0.05): a lone preview mesh has no fill light of any
            // kind, and the default reads as near-black on its unlit side. It
            // has no bearing on how a scene's own mesh instances light (those
            // keep MeshSceneDesc's real default).
            const EditorPreviewLightSettings& l = Arcane::Settings<EditorPreviewLightSettings>();
            scene.lightDirection = glm::normalize(glm::vec3(l.direction.x, l.direction.y, l.direction.z));
            scene.lightColor = glm::vec3(l.color.r, l.color.g, l.color.b);
            scene.ambient = glm::vec3(l.ambient);
        }

        Arcane::NriGraphContext::FrameDesc frame;
        frame.mesh = &scene;   // batch/post/globals all left null: no 2D content, no chain

        const auto outcome = m_preview->RenderFrameOffscreen(frame);
        if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)
        {
            ARC_ERROR("MeshDocument '{}': the preview frame failed -- dropping this "
                      "document's preview vehicle", m_title);
            m_previewFrameFailed = true;   // s3.2's FrameFailed
            DestroyPreviewContext();
            return;
        }
        // Cleared on `Presented` ONLY -- for an offscreen context that reads
        // as "rendered and submitted" (NriGraphContext.hpp's FrameOutcome
        // comment). `Skipped` is the routine zero-extent/uncreatable-target
        // case and left NOTHING in the texture, so the flag has to survive it
        // or the preview would stay blank for the document's lifetime once a
        // single Tick was skipped.
        if (outcome == Arcane::NriGraphContext::FrameOutcome::Presented)
        {
            // Dirty STAYS set while this context still has an upload in flight (Plan C
            // Task 5): the mesh this frame resolved for the first time was only queued, so
            // the picture just rendered does not have it yet and the preview must render
            // again (the dirty-driven tick would otherwise never redraw it).
            m_previewDirty       = m_preview->HasPendingUploads();
            m_previewPresented   = true;
            m_previewFrameFailed = false;   // the next good frame clears the latch
        }
    }

    bool MeshDocument::PreviewUploadsPending() const noexcept
    {
        return m_preview && m_preview->HasPendingUploads();
    }

    std::uint64_t MeshDocument::PreviewTextureId() const noexcept
    {
        return m_preview ? m_preview->OffscreenTextureId() : 0;
    }

    PreviewStatus MeshDocument::ComputeStatus() const
    {
        PreviewStatusInputs in;
        in.notCompiledHere = m_data.source == Arcane::MeshSource::Imported;
        in.hasErrors       = m_validationReason.has_value();
        in.deviceSeam      = m_services.chromeGraph && m_services.chromeGraph() != nullptr;
        in.vehicleFailed   = m_previewVehicleFailed;
        in.frameFailed     = m_previewFrameFailed;
        in.imageBound      = m_preview && m_previewPresented;
        return ComputePreviewStatus(in);
    }

    void MeshDocument::DestroyPreviewContext()
    {
        if (!m_preview)
            return;
        m_previewPresented = false;
        Arcane::RenderOverlayNode* hud = std::exchange(m_previewHud, nullptr);

        // ===== THE VEHICLE IS RETIRED, NOT DESTROYED (see Services::
        // retireGraphPreview / DocServices' identical field for the full
        // reasoning) =====
        if (m_services.retireGraphPreview)
        {
            m_services.retireGraphPreview(std::move(m_preview));
            return;
        }

        // ===== NO SINK: THE CROSS-CONTEXT INVALIDATE, OWED *BEFORE* =====
        // Reached only where no further frame will be recorded (shutdown's
        // drain, or a [gpu] test that wires no retire sink) -- against the
        // node recorded at creation (s3.2): ChromeGraph() is already null
        // after ShutdownGraphPath.
        if (hud)
            (void)hud->InvalidateUserTextureNow(m_preview->OffscreenOutput());
        m_preview.reset();
    }

    // =========================================================================
    // THE WINDOW (toolbar + preview) AND THE FORM (the Inspector's mesh page)
    // =========================================================================
    void MeshDocument::Draw(bool& requestClose)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard
        // (EditGesture.hpp:280-285) and SpriteDocument::Draw's identical
        // placement for why: it covers the early return below (a collapsed
        // window or a background tab, where no widget inside can report its
        // own deactivation).
        const EditGesture::ScopeGuard gestureGuard{ UndoStack(), m_gesture };

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(420.0f, 640.0f), ImGuiCond_FirstUseEver);
        const ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        if (!ImGui::Begin(m_windowLabel.c_str(), &open, flags))
        {
            m_windowFocused = false;
            ImGui::End();
            requestClose = !open;
            return;
        }
        m_windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        if (m_windowFocused) EditorActions::Get().MarkContextActive(ActionContext::Document);
        if (m_windowFocused && EditorActions::Get().Pressed("document.save"))
            Save();
        if (ImGui::Button("Save"))
            Save();
        ImGui::SameLine();
        ImGui::TextDisabled(Dirty() ? "(unsaved)" : "(saved)");
        ImGui::Separator();

        // ---- preview -------------------------------------------------------
        // ORDER IS LOAD-BEARING (fix-round Finding 1): the reason before the
        // image. A vehicle's offscreen texture is non-null (PreviewTextureId()
        // != 0) the INSTANT CreateOffscreen succeeds -- long before any frame
        // has rendered into it -- so in ANY session that actually has a GPU
        // device, an image check first would make the reason branch dead code
        // in exactly the real-editor case it exists for: a hand-edited invalid
        // file is the ONE situation ValidateMeshAsset's refusal is reachable
        // at all (every WIDGET below is bounded at its floor), and that is
        // precisely when the user needs to read why. RenderPreview() (see its
        // own comment) still records a frame for the invalid case, so the
        // texture holds a clean cleared image rather than garbage -- but this
        // document shows the REASON over that image regardless, because a
        // clean blank picture answers "what" and not "why".
        ImGui::BeginChild("##meshpreview", ImVec2(0.0f, 220.0f), ImGuiChildFlags_Borders);
        const PreviewStatus status = ComputeStatus();   // T1's mesh inputs (s3.2)
        if (m_validationReason)
        {
            // The Errors arm: the reason IS this box's text -- ValidateMeshAsset names the field.
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Theme::kError, "%s", m_validationReason->c_str());
            ImGui::PopTextWrapPos();
        }
        else if (status.image && status.compile != CompileStatus::NotCompiledHere)
        {
            // I4: an imported mesh never gets a procedural preview
            // (RebuildPreviewMesh never builds one) -- whatever the vehicle
            // presented for it is a cleared frame, so it reads as its reason
            // below instead (s3.2's image bit does not consult the compile).
            // The one placement rule every preview box shares (T3-D6): fitted and centred.
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const PreviewFit fit = FitPreviewImage(avail.x, avail.y, static_cast<float>(m_previewSize));
            const ImVec2 at = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(at.x + fit.x, at.y + fit.y));
            ImGui::Image(static_cast<ImTextureID>(PreviewTextureId()), ImVec2(fit.side, fit.side));
        }
        else
        {
            // Every other state names itself (s5.2): an imported mesh, no GPU
            // device, a failed vehicle or frame -- never "compiling...".
            CenteredTextDisabled(PreviewBoxText(status));
        }
        ImGui::EndChild();

        // The form (source, topology, material) is the Inspector's mesh page
        // now (DrawFormBody, drawn by whichever Inspector instance shows this
        // document -- inspector filters s6a). What stays is the toolbar above
        // and the preview, which reads only document state (m_data,
        // m_validationReason, the vehicle), never a form local -- so a live
        // topology edit in the page shows here on the next frame.
        //
        // Opened = selected; a click anywhere in the content (the toolbar, the
        // preview) re-selects the mesh page (spec s3's one selection rule).
        // AFTER the content, and only on this non-collapsed path: a
        // collapsed/background tab has no content to click.
        m_pageSel.NoteContentClick();

        ImGui::End();
        requestClose = !open;
    }

    void MeshDocument::DrawFormBody(PropertyGrid& grid)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard.
        // The topology rows below open gestures against m_gesture. The body
        // draws inside an Inspector instance window, AFTER the documents, and
        // on collapsed/background-tab frames too (InspectorWindows calls
        // page->Draw even when Begin returns false) -- where no widget inside
        // can report its own deactivation, which is exactly what this guard
        // covers. Draw keeps its own guard for the document window's refused-
        // Begin path (ShaderEditorDocument's Draw + DrawMaterialPageBody are
        // the precedent for two guards on one gesture).
        Arcane::CommandStack* const undo = UndoStack();
        const EditGesture::ScopeGuard gestureGuard{ undo, m_gesture };

        const bool imported = (m_data.source == Arcane::MeshSource::Imported);

        // Continuous-drag bracket (widget-layer Task 7 idiom), called right
        // after each ranged row: the row's VALUE widget is g.LastItemData, so
        // the gesture opens on its activation frame. The close is EndAfterRow
        // (s4.1): an Escape mid-drag closes the gesture AT the row, where the
        // builder's before == after guard pushes nothing.
        const auto bracket = [&](const char* label)
        {
            EditGesture::BeginOnActivate(undo, m_gesture,
                [&] { return std::string(label); },
                [&]
                {
                    return std::function<void()>(
                        [this, label = std::string(label), before = m_data]
                        { PushDataEdit(label, before); });
                });
            EditGesture::EndAfterRow(undo, m_gesture, grid.LastRowEvents().cancelled);
        };

        // Null in the headless tests: the cell's null services (s4.2).
        static const AssetRefServices kNoRefs{};
        const AssetRefServices& refs = m_services.assetRefs ? *m_services.assetRefs : kNoRefs;

        // I4: an IMPORTED mesh's source is not a choice -- it IS its model
        // (importedSource), and the five generated sources would be a silent
        // way to detach a companion from the artifact it mirrors. The generated
        // table deliberately does NOT carry Imported, so no arm flips a
        // generated mesh to Imported by hand either.
        static constexpr Arcane::MeshSource kSources[] = {
            Arcane::MeshSource::Plane, Arcane::MeshSource::Cube, Arcane::MeshSource::UvSphere,
            Arcane::MeshSource::Cylinder, Arcane::MeshSource::Capsule,
        };
        static constexpr const char* kSourceLabels[] = { "Plane", "Cube", "UV Sphere", "Cylinder", "Capsule" };
        static constexpr int kSourceCount = static_cast<int>(std::size(kSources));
        static_assert(std::size(kSourceLabels) == std::size(kSources));

        if (grid.Section("Mesh"))
        {
            PropertyGrid::Rows rows(grid, "##mesh");
            if (rows)
            {
                if (imported)
                {
                    // The source IS the model: browse-to only (no picker, no
                    // clear, drops refused).
                    AssetRefArgs source;
                    source.guid = m_data.importedSource;
                    source.readOnly = true;
                    (void)AssetRefRow(grid, "Source model", source, refs);
                }
                else
                {
                    int current = 0;
                    for (int i = 0; i < kSourceCount; ++i)
                        if (kSources[i] == m_data.source)
                            current = i;
                    if (const int picked = grid.ComboRow("Source", kSourceLabels, kSourceCount, current); picked >= 0)
                    {
                        const Arcane::MeshAssetData before = m_data;
                        m_data.source = kSources[picked];
                        CommitDataEdit("Change Source", before);
                    }
                }

                // ---- per-source topology ------------------------------------
                // ONLY the fields THIS source's generator reads (MeshAsset.hpp's
                // table) -- a Plane shows no rings/segments, a Cube and an
                // imported mesh no topology at all. Every floor is exactly
                // ValidateMeshAsset's per-source rule, so normal interaction can
                // never drive the document into a refused state; the ceilings
                // are generous UI-only bounds against a mis-drag producing a
                // million-triangle mesh. Live: the row writes its draft every
                // frame, so the preview rebuilds as the value moves.
                bool changed = false;
                // editor.mesh.primitiveRanges.* (S6-45): the cvar ranges keep each
                // minimum at or above the validator's floor; a maximum below its
                // minimum collapses to the minimum.
                const auto intRow = [&](const char* label, std::uint32_t& field, std::int32_t lo, std::int32_t hi)
                {
                    int v = static_cast<int>(field);
                    (void)grid.IntRow(label, v, Astra::Range(static_cast<double>(lo),
                                                             static_cast<double>(std::max(lo, hi)), 1.0));
                    if (static_cast<std::uint32_t>(v) != field)
                    {
                        field = static_cast<std::uint32_t>(v);
                        changed = true;
                    }
                    bracket(label);
                };
                const EditorMeshPrimitiveRangesSettings& caps = Arcane::Settings<EditorMeshPrimitiveRangesSettings>();
                switch (m_data.source)
                {
                    case Arcane::MeshSource::Plane:
                        intRow("Subdivisions", m_data.subdivisions, caps.planeSubdivisionsMin, caps.planeSubdivisionsMax);
                        break;
                    case Arcane::MeshSource::UvSphere:
                        intRow("Rings", m_data.rings, caps.sphereRingsMin, caps.sphereRingsMax);
                        intRow("Segments", m_data.segments, caps.segmentsMin, caps.segmentsMax);
                        break;
                    case Arcane::MeshSource::Cylinder:
                        // Deliberately NOT `rings` -- BuildCylinder never reads it.
                        intRow("Segments", m_data.segments, caps.segmentsMin, caps.segmentsMax);
                        break;
                    case Arcane::MeshSource::Capsule:
                    {
                        // The cap-ring floor is 2, not 3 -- a two-step arc still
                        // closes a hemisphere; UvSphere's rings span pole to pole.
                        intRow("Rings", m_data.rings, caps.capsuleRingsMin, caps.capsuleRingsMax);
                        intRow("Segments", m_data.segments, caps.segmentsMin, caps.segmentsMax);
                        float ratio = m_data.capsuleLengthRatio;
                        const double ratioLo = static_cast<double>(caps.capsuleLengthRatioMin);
                        (void)grid.FloatRow("Length Ratio", ratio, Arcane::Settings<MeshDocSettings>().capsuleRatioDragSpeed,
                                            Astra::Range(ratioLo, std::max(ratioLo, static_cast<double>(caps.capsuleLengthRatioMax))),
                                            "%.2f");
                        if (ratio != m_data.capsuleLengthRatio)
                        {
                            m_data.capsuleLengthRatio = ratio;
                            changed = true;
                        }
                        bracket("Length Ratio");
                        break;
                    }
                    case Arcane::MeshSource::Cube:
                    case Arcane::MeshSource::Imported:
                        break;   // no topology rows
                }
                if (changed)
                {
                    m_dirty = true;
                    RebuildPreviewMesh();
                }
            }
        }

        // ---- material (F2c's slots[]) --------------------------------------------
        // A generated mesh keeps the F2a single-material UX over slot 0 (Set
        // creates the one unnamed slot, Clear erases it); an imported mesh gets
        // one cell per slot of its artifact, Clear nilling and KEEPING the slot
        // (ClearSlotMaterial, I4). Both route through ApplySlotMaterialEdit.
        if (grid.Section("Material"))
        {
            PropertyGrid::Rows rows(grid, "##material");
            if (rows)
            {
                AssetRefArgs args;
                args.kindFilter = static_cast<int>(AssetKind::Material);
                args.surfaceFilter = static_cast<int>(Arcane::MaterialSurface::Mesh);
                if (!imported)
                {
                    args.guid = m_data.slots.empty() ? Arcane::Guid{} : m_data.slots[0].material;
                    ApplySlotMaterialEdit(0, AssetRefRow(grid, "Material", args, refs));
                }
                else if (m_data.slots.empty())
                {
                    grid.ReadOnlyRow("Material", "(the model defines no material slots)");
                }
                else
                {
                    for (std::size_t k = 0; k < m_data.slots.size(); ++k)
                    {
                        const std::string label = m_data.slots[k].name.empty() ? "Slot " + std::to_string(k)
                                                                               : m_data.slots[k].name;
                        args.guid = m_data.slots[k].material;
                        ImGui::PushID(static_cast<int>(k));   // two slots may share a name
                        const AssetRefEdit edit = AssetRefRow(grid, label.c_str(), args, refs);
                        ImGui::PopID();
                        if (edit.op != AssetRefEdit::Op::None)
                        {
                            ApplySlotMaterialEdit(k, edit);
                            break;   // m_data changed under the loop: the rest draws next frame
                        }
                    }
                }
            }
        }

        // ---- info (generated meshes only, 9.28 #27) -----------------------------
        // An imported mesh's geometry lives in the cooked artifact, and
        // m_previewMesh is null for it (RebuildPreviewMesh); an invalid param
        // set has no geometry either -- no section rather than zeros.
        if (!imported && m_previewMesh && grid.Section("Info"))
        {
            PropertyGrid::Rows rows(grid, "##info");
            if (rows)
            {
                grid.ReadOnlyRow("Vertices", std::to_string(m_previewMesh->vertices.size()));
                grid.ReadOnlyRow("Triangles", std::to_string(m_previewMesh->indices.size() / 3));
                const Arcane::MeshBounds b = Arcane::ComputeMeshBounds(*m_previewMesh);
                char text[96];
                std::snprintf(text, sizeof(text), "%.2f \xC3\x97 %.2f \xC3\x97 %.2f m",
                              b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z);
                grid.ReadOnlyRow("Bounds", text);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("min (%.3f, %.3f, %.3f)\nmax (%.3f, %.3f, %.3f)",
                                      b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
            }
        }
    }
}
