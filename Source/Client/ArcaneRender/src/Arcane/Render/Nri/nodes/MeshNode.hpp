#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// MeshNode -- THE 3D MESH PASS as a render graph node (NRI Phase 4, Task 7;
// widened from opaque-only to opaque + masked + ordered transparent by F3
// plan 2 Task 3).
//
// This is the node that puts a lit, textured, DEPTH-TESTED mesh through the
// frame graph. It renders into the frame's canvas (the same RGBA16F transient
// the 2D batch draws into) with a D32_SFLOAT depth target it creates itself.
//
// ONE PASS, THREE BLEND MODES, SIX PIPELINES: opaque and masked batches draw
// INDIRECT off the counts mesh-cull filled in, then the transparent rows draw
// DIRECT in the frame's sorted order. The blend x two-sided product is six
// fixed pipelines in the cache, not a runtime shader define -- opaque, masked
// and transparent are three separate offline artifacts out of mesh.hlsl, and
// one/two-sided selects nri::CullMode::BACK/NONE on each.
//
// WHAT IT IS DELIBERATELY NOT:
//   * NOT PBR. data/shaders/mesh.hlsl is one directional light, Lambert
//     diffuse plus a constant ambient term, one albedo texture. The GGX
//     material model belongs to the Deadlock-class renderer arc; a half
//     version here would be a second material model to reconcile.
//   * NOT a commitment to forward OR deferred. One opaque pass writing a
//     colour target and a depth target is the shared prefix of both.
//   * NOT a mesh IMPORT path. Geometry comes from MeshBuilder (procedural
//     cube/sphere); cgltf/.arcmesh is a later arc, and no library is vendored
//     for it here.
//   * NOT the material system. A per-instance linear tint (MeshInstance::
//     baseColor) plus, since Task 10, an optional per-instance MATERIAL SLOT
//     (MeshInstance::materialSlot) indexing this node's own BindlessTable is
//     the whole of it. A per-instance albedo Guid resolved into per-image
//     descriptor sets was written and then REMOVED at Task 7's first fix
//     round -- nothing exercised it, and Task 8/10's BindlessTable replaced
//     the fixed white-texel t0 binding it would have competed with outright.
//     Feeding the table from a REAL cooked albedo (rather than a test's own
//     generated textures) is Task 11's -- LANDED, in NriGraphContext::
//     ResolveMeshAlbedoSlot, not in this node -- this node still only ever
//     sees an already-resolved nri::Descriptor* through AddMaterial.
//
// WHERE IT SITS IN THE FRAME: after `batch2d`, before the post chain and the
// tonemap. The canvas is MINTED AND CLEARED by AddBatch2DNode (its CLEAR
// colour load op), so the mesh pass is necessarily the canvas's second writer
// -- AddMeshNode declares a LOAD colour op and a CLEAR depth op (the depth is a
// fresh pool slot with undefined contents, cleared to
// DepthClearValue(kSelectedDepthConvention)) and never clears the colour, or
// it would wipe the 2D content that ran before it. Rendering 3D UNDER the 2D
// layer instead is a frame-order question (NriGraphContext::
// DeclareGraphFrame's THE CLEAR IS DECLARATIVE block), not this node's.
//
// WHAT THIS NODE OWNS (all persistent, all created once at Create()):
//   * the BINDLESS MATERIAL TABLE (Task 8/10) -- a BindlessTable of
//     kBindlessCapacity SRV slots, gated on both bindless tier and the
//     sampled-image descriptor limit at Create() (insufficient hardware
//     refuses the node outright rather than render wrong pixels) -- and the ONE
//     descriptor set its array lives in. This REPLACED the node's own 1x1
//     white texel + t0 binding outright: `kInvalidSlot` (MeshInstance::
//     materialSlot's default) now selects a FLAT path in mesh.hlsl that
//     skips sampling entirely, bit-for-bit F2a's old white-times-tint
//     arithmetic;
//   * the pipeline layout -- root constants b0 (the 8-byte MeshRootConstants
//     {firstOutput, flags}, F3 plan 1 T7 -- the 128-byte per-instance
//     MeshConstants block of Task 8/F2a is GONE; every per-instance value is
//     a field of the GPU scene's row now) at the implicit rootRegisterSpace,
//     ONE immutable ROOT SAMPLER at s0 (Task 10's slice-one trilinear
//     sampler -- also rootRegisterSpace; THE REGISTER-SPACE RULE below is
//     why that space can no longer be shared with either descriptor set),
//     plus TWO ordinary descriptor sets: space1 = { b1 frame CB, t0 the
//     instance rows, t1 this slot's visible indices }, space2 = { t0
//     bindless material array };
//   * one descriptor pool, and ONE descriptor set PER FRAME SLOT for space1
//     (CreateSets). Its b1 range is written ONCE, at Create, and never
//     rewritten; its t0/t1 ranges (F3) are REWRITTEN per slot, in Record,
//     whenever the GPU scene's buffers changed under it -- the instance
//     buffer grew (GpuScene::InstanceBufferGeneration) or the slot's
//     visible-index buffer was re-created (GpuScene::VisibleIndicesView).
//     That rewrite is safe for the same reason the arena write is: the
//     slot's PREVIOUS frame retired at BeginFrame (the swapchain's fence
//     wait), so nothing in flight reads that slot's set. Batch2DNode's sets
//     are never rewritten at all; this one is rewritten only for the
//     CURRENT slot, only when a buffer moved, and therefore needs no
//     update-after-set permission.
//     The bindless set is DIFFERENT again: allocated ONCE (not per frame
//     slot, since its contents do not vary by frame) and written
//     INCREMENTALLY by AddMaterial as slots are Added -- see that method's
//     own comment for the synchronization contract that discipline costs;
//   * the per-frame-slot constant-buffer arena the b1 views name.
// The GPU SCENE's buffers are NOT owned here: GpuScene (Render/Nri/
// GpuScene.hpp, owned by NriGraphContext) holds the instance / args /
// visible-index buffers, GpuSceneSyncNode writes them ahead of this pass,
// and Record receives the GpuScene* as a parameter.
// The PIPELINES are not owned here: they come from the vehicle's shared
// NriPipelineCache, keyed by (shader pair, layout, canvas format, DEPTH
// format, blend, depth-write, cull), so a format or material-state change is
// a cache miss rather than a stale PSO.
//
// THE REGISTER-SPACE RULE (Batch2DNode.hpp states it in full, verified
// against Source/Validation/DeviceVal.hpp's `rootDescriptorNum ||
// rootSamplerNum` guard): NRI refuses a pipeline layout whose
// rootRegisterSpace equals any descriptor set's registerSpace, but ONLY
// once the layout carries a root DESCRIPTOR or root SAMPLER -- root
// CONSTANTS alone are exempt (VK lowers them to push constants, outside the
// set-space numbering entirely). Batch2DNode has neither and keeps
// everything at space0; THIS layout is the first in the tree to add a root
// SAMPLER (Task 10's immutable s0), so its root space and its two ordinary
// sets must all be distinct register spaces -- see MeshNode.cpp's
// CreateBindings() for the concrete numbers and mesh.hlsl for the matching
// `register(..., spaceN)` annotations, including the SPIR-V register-shift
// entries (compile-shaders.bat / ShaderConventions.hpp::kSpirvArgs) that
// have to agree with them.
//
// WHY THE SETS ARE PER FRAME SLOT and Batch2DNode's built-in ones are not: a
// set here carries the per-frame constant buffer b1, whose (buffer, offset) is
// baked into its nri::Descriptor at creation. Double-buffering the CB means
// double-buffering the set that names it. See Batch2DNode.hpp's THE
// CONSTANT-BUFFER ARENA for the whole account of why the upload ring cannot
// back a constant buffer that a descriptor set names.
//
// WINDING AND CULLING -- read MeshBuilder.hpp's WINDING block first. That
// module emits triangles that are COUNTER-CLOCKWISE as seen from OUTSIDE the
// surface, and this node's rasterizer state is set to match:
// `frontCounterClockwise = true` + `cullMode = BACK`. See MeshNode.cpp's
// PipelineFor() for the full derivation (and the desk check that falsifies it
// in one look: get it backwards and a closed mesh renders INSIDE-OUT or, for a
// convex one, vanishes entirely).
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR and
// <windows.h> #defines ERROR via wingdi.h).
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>
#include <Extensions/NRIDeviceCreation.h>   // explicitly: GpuSceneTypes.hpp below reaches <windows.h> (GpuScene.hpp explains)

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Math/NormalMatrix.hpp>      // NormalMatrixFor (moved here from this file, F3 plan 1 T5)
#include <Arcane/Mesh/MeshBuilder.hpp>      // MeshData / MeshVertex -- the CPU geometry
#include <Arcane/Render/GpuSceneTypes.hpp>     // GpuSceneFrame -- MeshSceneDesc::scene (F3 plan 1 T6)
#include <Arcane/Render/Nri/BindlessTable.hpp> // MeshInstance::materialSlot's kInvalidSlot default
#include <Arcane/Render/Nri/NriDeviceCaps.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/FramePacing.hpp>      // kMaxFramesInFlight, FramesInFlight()
#include <Arcane/Render/RenderLookSettings.hpp>   // RenderMeshDefaultLightSettings -- MeshSceneDesc's light defaults

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Arcane
{
    class GpuScene;
    class Graveyard;
    class NriDevice;
    class NriGraphContext;

    // NormalMatrixFor lives in Arcane/Math/NormalMatrix.hpp (F3 plan 1 T5: the
    // GPU scene stages it on the CPU, NRI-free); included below and used by
    // name here exactly as before. MeshNodeTest.cpp still pins it.

    // =====================================================================
    // THE AD-HOC INSTANCE (F3 plan 1 T7): a REGISTRY-LESS caller's row --
    // MeshDocument's preview, the thumbnail harvester, a [gpu] test. Drawn
    // DIRECT (one CmdDrawIndexed each, root flags & kMeshRootDirect),
    // UNCULLED, in submission order, from the GpuScene::ScratchRows() scratch
    // rows this frame slot owns in the instance buffer
    // (render.gpuScene.scratchRowsPerFrame): MeshNode::Prepare
    // converts each one into a GpuInstance (AdHocRows()), AddMeshNode hands
    // that span to GpuSceneSyncNode, which copies it into the slot's scratch
    // region ahead of the pass. Beyond ScratchRows() per frame the rest are
    // DROPPED -- by Prepare, which is also what WARNS, once per node (the
    // sync node only ever sees the capped span, so GpuScene::Reserve's own
    // overflow guard cannot fire on this path).
    //
    // Registry-backed entities do NOT come through here any more: they are
    // rows of the GPU scene (GpuSceneSync -> BuildGpuSceneFrame ->
    // MeshSceneDesc::scene), drawn INDIRECT per batch. Fields unchanged.
    //
    // Deliberately NOT an ECS view, a Registry walk or a Scene pointer: this
    // is a RENDER vehicle's input, the same shape and the same borrowing rules
    // FrameDesc::pickables carries (Render/PickEmit.hpp is its sibling).
    // =====================================================================
    struct MeshInstance
    {
        // THE MESH'S ASSET GUID, not a borrowed CPU pointer (F2c s7.2). Geometry is
        // resolved through the vehicle's NriMeshBufferCache at DECLARATION time and is
        // already RESIDENT on the device by the time this node records -- so the
        // borrow-lifetime contract this field used to carry (a raw MeshData* that had
        // to outlive the RenderFrame call, copied into the ring every frame) is gone
        // with the ring path it existed for.
        //
        // NIL means "draw nothing", which is not an error (a scene may legitimately
        // carry a slot with no geometry yet) -- unchanged.
        Guid mesh{};

        // Model -> world, METERS (MKS). Rotation, translation AND a
        // NON-UNIFORM scale are all safe here (Task 8/F2a): MeshNode::Prepare
        // derives NormalMatrixFor(model) fresh for every instance and stages
        // it in the row alongside `model` (F3: GpuInstance::normal0..2), so
        // mesh.hlsl's vs_main transforms normals by the inverse transpose
        // rather than the upper 3x3. (Before Task 8 this
        // comment stated a UNIFORM-scale-only restriction -- F1 gave Transform
        // a glm::vec3 scale the Inspector authors freely, so the first
        // non-uniform scale-handle drag on a mesh hit the old shortcut.
        // NormalMatrixFor, above, is the fix; nothing here still depends on
        // that restriction.)
        glm::mat4 model{1.0f};

        // LINEAR, and may exceed 1.0 (the canvas is RGBA16F). When
        // `materialSlot` is `kInvalidSlot` (the default) this IS the
        // instance's colour outright -- mesh.hlsl's flat path, bit-for-bit
        // F2a's original behaviour (see `materialSlot`'s own comment).
        // Otherwise it multiplies the bindless texture's sample, same as
        // F2a's white-texel arithmetic always meant it to.
        glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};

        // ===== THE MATERIAL SLOT (Task 8/10) =====
        // Indexes THIS NODE'S OWN BindlessTable -- a slot AddMaterial handed
        // back for an SRV Added to a DIFFERENT MeshNode's table names a
        // different node's texture entirely (or nothing), a silent
        // wrong-picture bug the type system cannot catch, so callers must
        // never mix them. `kInvalidSlot`, the default, is what a mesh
        // carrying no bindless material uses -- it selects mesh.hlsl's FLAT
        // path (baseColor alone, no sample), which is F2a's original
        // behaviour: an all-white 1x1 t0 texel times baseColor, and 1.0 * x
        // is exact under IEEE-754, so this is that same result with the now-
        // redundant sample removed, not an approximation of it. A `Guid
        // albedo` resolved through the shared NriTextureCache into a
        // BindlessTable slot was written and then REMOVED at Task 7's first
        // fix round; Task 8/10's BindlessTable + this field is what finally
        // replaces it, and feeding a REAL cooked albedo in here (rather than
        // a test's own generated texture) is Task 11's -- landed: `Guid
        // albedo` now lives on `Arcane::ResolvedMeshMaterial`
        // (Scene/SceneResources.hpp), resolved into a slot by
        // NriGraphContext::ResolveMeshAlbedoSlot and copied onto this field
        // by the retired CollectMeshInstances sweep (F2a Task 5, gone in F3 plan 1 T8).
        //
        // SINCE F3 PLAN 1 T7 this travels as the row's own `materialSlot`
        // field (GpuInstance, a plain uint the shader reads by row) -- the
        // bit-packing into `normalMatrixCol0.w` that the 128-byte root block
        // forced (Task 8/10) is gone with that block. `kInvalidSlot` ==
        // kGpuInvalidMaterialSlot (GpuSceneTypes.hpp; GpuScene.cpp
        // static_asserts the two agree).
        std::uint32_t materialSlot = BindlessTable::kInvalidSlot;

        // WHICH SECTION of `mesh` this instance draws (F2c s7.4). A multi-section prop
        // becomes sections.size() instances, one per section, each carrying the
        // material its slot resolved to -- the material identity travels per row, in
        // materialSlot, so a section adds nothing to the root block.
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount  = 0;   // 0 == "the whole mesh", the F2a shape
    };

    namespace Detail
    {
        // render.mesh.defaultLight.* as the glm values MeshSceneDesc holds:
        // a light is its colour x its intensity (the colour's alpha is
        // unused). x * 1.0f is x exactly, so intensity 1 is the colour bit
        // for bit.
        [[nodiscard]] constexpr glm::vec3 ToGlm(const CVarVec3& v) noexcept { return { v.x, v.y, v.z }; }
        [[nodiscard]] constexpr glm::vec3 ToGlmRgb(const CVarColor& c, float intensity) noexcept
        {
            return { c.r * intensity, c.g * intensity, c.b * intensity };
        }
    }

    struct MeshSceneDesc
    {
        // THE AD-HOC ROWS (see MeshInstance's header): drawn direct, unculled,
        // in order, at most GpuScene::ScratchRows() of them per frame.
        // BORROWED SPAN for the duration of the RenderFrame call, exactly like
        // FrameDesc::pickables: the declaration copies the SPAN into the
        // node's exec fn, never the elements, and the exec fn runs inside the
        // same call. The elements name meshes by Guid; they no longer point at
        // CPU geometry. EMPTY IS LEGAL; see Empty() -- a registry-backed scene
        // needs none of these, and DeclareGraphFrame declares no mesh pass
        // for a scene that is Empty(), so one costs the same as no scene.
        std::span<const MeshInstance> instances;

        // The camera, already resolved by the frame driver. TWO matrices
        // rather than one product because that is what SceneCamera hands back
        // (PerspectiveCameraView, Task 5) and because a later pass that needs
        // the view alone should not have to un-multiply it.
        //
        // THE CALLER OWES VALID MATRICES. SceneCamera::PerspectiveProjection
        // is an unvalidated pure function -- a zero fov, a non-positive aspect
        // ratio or nearZ >= farZ silently produces NaN/Inf -- so build these
        // through the guarded path (ActivePerspectiveSceneCamera) or check
        // them. MeshNode::Record checks them too and drops the pass with one
        // ERROR rather than recording a draw with a NaN transform, because a
        // NaN clip position is undefined behaviour on the GPU rather than a
        // wrong picture.
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};

        // THE ONE DIRECTIONAL LIGHT. `lightDirection` points TOWARD the light.
        // It does NOT have to be unit length: MeshNode::Record normalizes it
        // once per frame on the CPU and mesh.hlsl consumes the result directly.
        //
        // A ZERO-LENGTH VECTOR IS LEGAL and means "no directional light" --
        // the pass falls back to the ambient term alone. That is a GUARD, not
        // a convention: `normalize()` on a zero vector is a division by zero
        // and yields NaN, which would propagate through N.L into every lit
        // pixel. Normalizing on the CPU is what lets the zero case be handled
        // at all (and costs one normalize per frame instead of one per pixel).
        //
        // `ambient` is a flat term added to every lit surface -- the whole of
        // the indirect lighting model here, deliberately.
        //
        // The defaults are render.mesh.defaultLight.*'s (settings arc S6-19);
        // a scene-view host overwrites them every frame from the published
        // setting with ApplyDefaultLight (Live).
        glm::vec3 lightDirection = Detail::ToGlm(RenderMeshDefaultLightSettings{}.direction);
        glm::vec3 lightColor     = Detail::ToGlmRgb(RenderMeshDefaultLightSettings{}.color,
                                                    RenderMeshDefaultLightSettings{}.intensity);
        glm::vec3 ambient        = Detail::ToGlmRgb(RenderMeshDefaultLightSettings{}.ambient,
                                                    RenderMeshDefaultLightSettings{}.ambientIntensity);

        // THE REGISTRY-BACKED SCENE (F3 plan 1 T6): what GpuSceneSync +
        // BuildGpuSceneFrame produced for this frame -- the staged rows,
        // batches, indirect args and visible indices GpuSceneSyncNode copies
        // to the device ahead of this pass, and (T7) the batches Record
        // draws INDIRECT, one CmdDrawIndexedIndirect each, in the frame's
        // order. BORROWED for the RenderFrame call like `instances`. Null is
        // "no registry scene"; `instances` above are the AD-HOC rows (a
        // preview, a thumbnail, a test) and the two are independent.
        const GpuSceneFrame* scene = nullptr;

        // "No mesh pass this frame" -- no ad-hoc instances AND a registry
        // scene (if any) that neither EMITTED a batch nor STAGED anything.
        // DeclareGraphFrame declares no node for an Empty() scene, exactly
        // as it did for an empty `instances`.
        //
        // WHY A STAGE COUNTS (ruling R-D, T7): a frame whose entities are all
        // culled emits no batch but may still carry dirty rows (a move, a
        // spawn, a full rebuild after a registry swap). The mirror's
        // lastModel history has ALREADY advanced past them (GpuSceneSync
        // stages and records in one step), so skipping the pass -- and with
        // it GpuSceneSyncNode's upload -- would leave those rows never
        // written; the moment one came into view it would draw stale (or
        // never-initialised) bytes. Such a frame declares the pass, the sync
        // node uploads, and MeshNode::Record records only its depth clear.
        // `fullRebuild` with no rows still counts: Apply stamps the synced
        // generation, which is what stops the next frame from being a full
        // rebuild too.
        [[nodiscard]] bool Empty() const noexcept
        {
            return instances.empty()
                && !(scene && (scene->HasDraws() || !scene->stage.rows.empty() || scene->stage.fullRebuild));
        }
    };

    // The scene's light from render.mesh.defaultLight.* -- the one
    // directional light and the ambient term every scene view gets until a
    // light component exists, each its colour x its intensity (the colours'
    // alpha is unused). A host passes
    // Settings<RenderMeshDefaultLightSettings>() once per frame (Live).
    inline void ApplyDefaultLight(MeshSceneDesc& scene, const RenderMeshDefaultLightSettings& light) noexcept
    {
        scene.lightDirection = Detail::ToGlm(light.direction);
        scene.lightColor     = Detail::ToGlmRgb(light.color, light.intensity);
        scene.ambient        = Detail::ToGlmRgb(light.ambient, light.ambientIntensity);
    }

    // THE 8-BYTE ROOT BLOCK (F3): `firstOutput` is the batch's start in the
    // visible-index buffer for an indirect draw, or THE ROW ITSELF when
    // `flags & kMeshRootDirect`. mesh.hlsl's MeshRoot. Public for the
    // MeshNodeTest pin; the 128-byte MeshConstants block (model + tint + the
    // packed normal matrix) this replaces lived in MeshNode.cpp's anonymous
    // namespace and is gone -- every per-instance value is a row field now.
    struct MeshRootConstants
    {
        std::uint32_t firstOutput = 0;
        std::uint32_t flags       = 0;
    };
    ARC_CONSTANT("layout: root-layout index")
    inline constexpr std::uint32_t kMeshRootDirect = 1u;
    static_assert(sizeof(MeshRootConstants) == 8, "mesh.hlsl's MeshRoot is two uints");

    struct MeshDrawSelection
    {
        bool registry = false;
        bool adHoc    = false;
    };
    [[nodiscard]] constexpr MeshDrawSelection SelectMeshDrawPaths(
        bool registryHasDraws, bool adHocHasDraws,
        const GpuSceneFrameReadiness& readiness) noexcept
    {
        return { readiness.registryReady && registryHasDraws,
                 readiness.adHocReady && adHocHasDraws };
    }

    // The three fixed offline pixel artifacts. No runtime shader define chooses
    // among them: every blend mode owns a separately compiled DXIL/SPIR-V blob.
    enum class MeshPixelShader : std::uint8_t
    {
        Opaque,
        Masked,
        Transparent,
    };

    // The complete material-derived state of one mesh PSO. Public for the
    // device-free six-combination pin; MeshNode is the sole production caller.
    struct MeshPipelineState
    {
        MeshPixelShader                          pixel      = MeshPixelShader::Opaque;
        NriPipelineCache::GraphicsKey::Blend     blend      = NriPipelineCache::GraphicsKey::Blend::Opaque;
        bool                                     depthWrite = true;
        nri::CullMode                            cullMode   = nri::CullMode::BACK;
    };

    class ARC_RENDER_API MeshNode
    {
    public:
        // Loads mesh_vs/mesh_ps through the vehicle, creates the bindless
        // material table + descriptor pool + constant arena, and registers
        // the pipeline layout. Null (already logged + latched) on any
        // failure -- a vehicle that cannot build this node must not render a
        // frame that silently draws nothing. Refuses FIRST, before any of
        // that, on a device whose bindless tier or sampled-image descriptor
        // limit cannot cover kBindlessCapacity -- the house refuse-loudly
        // posture: this node's material table cannot mean anything on such
        // hardware, so it does not get built rather than rendering wrong
        // pixels or silently changing the material model.
        static std::unique_ptr<MeshNode> Create(NriGraphContext& context);

        // Maps blend and sidedness independently to one of the six fixed mesh
        // PSOs. Opaque/masked write depth; transparent alpha-blends without a
        // depth write; one/two-sided selects BACK/NONE culling respectively.
        [[nodiscard]] static MeshPipelineState PipelineStateFor(MaterialBlendMode blend,
                                                                 bool twoSided) noexcept;

        // SAFETY NET, NOT THE PATH -- same shape as ~Batch2DNode. The
        // sanctioned release is Release() at a fence the owner knows.
        ~MeshNode();

        MeshNode(const MeshNode&)            = delete;
        MeshNode& operator=(const MeshNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent. The caller picks the fence for the same reason
        // NriPipelineCache::Clear does. Buries the bindless table's own
        // descriptors too (BindlessTable::Release), so an SRV a caller Added
        // via AddMaterial is discharged the same way everything else here
        // is -- the caller still owns the TEXTURE that view names (see
        // AddMaterial's own comment) and must release that separately.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Registers `srv` in this node's BindlessTable and writes it into
        // the bindless descriptor set at the slot BindlessTable::Add
        // returns -- `kInvalidSlot` on refusal (null `srv`, the table is at
        // capacity, or Create() never built one). The returned slot is what
        // a MeshInstance::materialSlot names to select `srv` at draw time.
        //
        // OWNERSHIP: exactly BindlessTable::Add's -- this node's table takes
        // ownership of the VIEW (`srv`) and discharges it at Release()/
        // destruction. The TEXTURE `srv` views is a SEPARATE object this
        // node never touches; the caller (Task 11's feed, or a test's own
        // generated texture) still owns it and must outlive both the view
        // and this node's use of the returned slot.
        //
        // SYNCHRONIZATION: when the device supports sampled-image update-
        // after-bind for the entire fixed table, CreateBindings opts the
        // bindless range/set/pool into it and this appends a fresh slot while
        // older frames may still be in flight. When Vulkan exposes descriptor
        // indexing without that granular feature, this method waits for
        // device idle before appending. The fallback is paid only when a new
        // material becomes resident, never per frame. BindlessTable's add-
        // only policy means neither path rewrites a descriptor an older draw
        // could already reference.
        [[nodiscard]] std::uint32_t AddMaterial(nri::Descriptor* srv);

        // Resolves the PIPELINE for the colour format the frame being declared
        // will attach. Called at DECLARATION time for the reason
        // Batch2DNode::Prepare is: a PSO compile must not land inside the
        // recording window, where a first-frame miss would stall it and a
        // FAILED miss would latch an error on a frame that is otherwise fine.
        //
        // Resolves the PIPELINE **and** this frame's mesh residency. The residency half
        // is here for the same reason the pipeline half is: NriMeshBufferCache::Resolve
        // uploads through HelperInterface::UploadData, which SUBMITS AND WAITS
        // INTERNALLY -- doing that inside an open command buffer is exactly the shape
        // the graph's no-hand-barriers rule exists to prevent (NriTextureCache.hpp's NO
        // BARRIERS section states it for images; geometry is the same hazard with a
        // bigger payload). Record() therefore only ever LOOKS UP what Prepare made
        // resident, and never resolves.
        //
        // Safe to skip entirely -- Record() then reports the missing pipeline
        // once and draws nothing. Device-less [nri] frame-shape cases pass a
        // null context to AddMeshNode, which skips this.
        //
        // `meshBuffers` is NULLABLE, and the two halves are independent: a null cache
        // skips the RESIDENCY loop ONLY, and the pipeline is still resolved. Gating
        // both on the cache (the pre-fix call-site shape) meant a hypothetically
        // cache-less context also lost its pipeline and drew nothing behind a
        // "missing pipeline" warning that named the wrong cause.
        //
        // SINCE F3 PLAN 1 T7 this ALSO builds AdHocRows(): every ad-hoc
        // instance (`scene.instances`, capped at GpuScene::ScratchRows() --
        // the overflow is dropped and WARNED once, here, naming the count) is
        // converted to a GpuInstance -- model, NormalMatrixFor(model)'s
        // columns, baseColor, materialSlot -- and its draw range remembered,
        // so AddMeshNode can hand the rows to GpuSceneSyncNode (which copies
        // them into this slot's scratch region) before the mesh node is
        // declared. The residency loop resolves each batch mesh AND each
        // ad-hoc mesh once. The ad-hoc rows are built whether or not
        // `meshBuffers` is null: they are the sync node's input either way.
        void Prepare(nri::Format canvasFormat, const MeshSceneDesc& scene,
                     NriMeshBufferCache* meshBuffers, std::uint64_t frameCounter);

        // The ad-hoc rows Prepare staged this frame, in submission order --
        // what AddMeshNode passes to AddGpuSceneSyncNode as `adHoc`. A view
        // into this node's own storage: valid until the next Prepare.
        [[nodiscard]] std::span<const GpuInstance> AdHocRows() const noexcept { return m_adHocRows; }

        // Records one scene's mesh geometry -- opaque, masked AND ordered
        // transparent -- into an ALREADY-OPEN raster pass whose colour
        // attachment is the canvas and whose depth attachment is this node's
        // depth target. In order: clear the DEPTH plane (the clear
        // seam -- graph attachments are LOAD/STORE, see
        // NriGraphContext::DeclareGraphFrame); rewrite this slot's t0/t1
        // views if the GPU scene's buffers moved (the header block); bind the
        // layout and the two sets; then (F3 plan 1 T7, plan 2 T3):
        //   1. the registry-backed BATCHES, in the frame's order (opaque
        //      before masked, nearest first within each) -- for each, bind the
        //      pipeline its (blend, twoSided) key selects, bind the mesh's
        //      already-resident vertex/index buffers, push
        //      {batch.firstOutput, 0}, one CmdDrawIndexedIndirect reading
        //      argIndex's nri::DrawIndexedDesc from GpuScene::Args(slot);
        //   2. the AD-HOC rows, DIRECT, in submission order, on the opaque
        //      one-sided pipeline -- push {scratchFirst + i, kMeshRootDirect},
        //      one CmdDrawIndexed each;
        //   3. the TRANSPARENT rows, DIRECT and one at a time, in the frame's
        //      sorted order (render order ascending, biased projected depth
        //      descending, then (entity, mesh, section)) -- push {row,
        //      kMeshRootDirect}, one CmdDrawIndexed each. They go LAST so
        //      every depth-writing surface, ad-hoc included, is in the depth
        //      plane before the first blend reads it.
        // A mesh that is not resident is SKIPPED, never a stale bind. A frame
        // with no batches, no transparent rows and no ad-hoc rows (R-D: staged
        // rows only) records the depth clear and nothing else -- no draw, no
        // error. Never uploads; never touches the frame ring for geometry.
        //
        // IT DOES NOT CLEAR THE COLOUR PLANE. batch2d already cleared and drew
        // into the canvas; clearing it here would erase that.
        //
        // Emits NO barrier: the executor derives and batches every one of them
        // from the declarations (the sync node's CopyDst writes -> this
        // node's ShaderRead / IndirectArgs reads).
        //
        // `frameSlot` is the vehicle's own per-frame slot -- the SAME number it
        // gave the upload ring, so this node's constant-buffer arena is
        // double-buffered against exactly the fence the swapchain already waits
        // on.
        //
        // `gpuScene` is the vehicle's GpuScene (NriGraphContext::Scene()),
        // passed in rather than held: this node keeps no context pointer
        // (see m_device's comment), and the scene is the one per-frame thing
        // Record reads that Prepare's parameters do not carry. Null is an
        // ERROR (logged, latched, nothing recorded) -- a vehicle without a
        // GPU scene cannot draw either path.
        //
        // NO canvasFormat PARAMETER, unlike Batch2DNode::Record: that node
        // resolves a pipeline PER SPAN at record time and needs the format
        // there. This one resolves per (blend, twoSided) key instead -- six
        // fixed pipelines since plan 2 T3, all SIX of them keyed by Prepare
        // against the canvas format it was handed, so Record only ever LOOKS
        // UP an already-built pipeline and never needs the format itself. A
        // parameter this function did not read would just be something for a
        // reader to reason about.
        void Record(RenderGraphNodeContext& context, const MeshSceneDesc& scene,
                    std::uint32_t frameSlot, GpuScene* gpuScene,
                    const GpuSceneFrameReadiness& readiness);

        // The b1 block's region size BEFORE alignment. mesh.hlsl's MeshFrameCB
        // is 112 bytes; 256 is also D3D12's constant-buffer placement
        // alignment, so on that backend this is exactly one region.
        ARC_CONSTANT("layout: constant-buffer placement size; the frame struct must fit")
        static constexpr std::uint32_t kFrameCbMaxBytes = 256;

        // THE BINDLESS MATERIAL TABLE'S CAPACITY (Task 8/10). PUBLIC (unlike
        // most of this node's internals) for the same reason kFrameCbMaxBytes
        // is: a device-less [nri] case (RenderGraphTest.cpp) recomputes
        // PoolSizes()'s expectations from first principles rather than
        // copying the implementation, and needs a real symbol to do that
        // with rather than a second hardcoded 256. A slice-one FIXED size --
        // BindlessTable never resizes (BindlessTable.hpp's SLOT POLICY) --
        // chosen well above what any scene in this arc names (the four-cube
        // proof uses four slots) and above the spec's cited eventual high-
        // water mark (T3's 144-entry box-projected cubemap array, spec
        // section 6). MUST EQUAL mesh.hlsl's own kMeshBindlessCapacity
        // literal EXACTLY: CreateBindings() sizes the t0/space2 descriptor
        // range to this number, and the shader declares its Texture2D array
        // with the matching literal -- HLSL cannot include this header, so
        // that one comparison has no compiler behind it; this comment (and
        // its mirror in mesh.hlsl) is the whole of that contract.
        ARC_CONSTANT("shader contract: must equal kMeshBindlessCapacity in data/shaders/mesh.hlsl")
        static constexpr std::uint32_t kBindlessCapacity = 256;

        // The arena's region stride on a device whose
        // deviceDesc.memoryAlignment.constantBufferOffset is
        // `constantBufferAlignment`. PURE and public for the same reason
        // Batch2DNode::CbRegionStride is: it carries an invariant whose
        // violation would be silent. The result must be BOTH a multiple of the
        // device's alignment AND at least kFrameCbMaxBytes; rounding UP
        // satisfies both for any power-of-two alignment.
        [[nodiscard]] static constexpr std::uint64_t CbRegionStride(
            std::uint64_t constantBufferAlignment) noexcept
        {
            return constantBufferAlignment <= 1
                 ? kFrameCbMaxBytes
                 : ((kFrameCbMaxBytes + constantBufferAlignment - 1) / constantBufferAlignment)
                       * constantBufferAlignment;
        }

        // Byte offset of one frame slot's constant-buffer region. PURE, static
        // and public so the [nri] cases can prove the property no device can
        // show: distinct frame slots never alias, and every region starts on a
        // multiple of `regionStride`.
        [[nodiscard]] static constexpr std::uint64_t CbRegionOffset(
            std::uint64_t regionStride, std::uint32_t frameSlot) noexcept
        {
            return (std::uint64_t)frameSlot * regionStride;
        }

        // THE DESCRIPTOR POOL'S CAPACITY. PUBLIC and separated from the
        // creation call for the reason Batch2DNode::PoolSizes is: a pool's
        // sizes are fixed at creation and NRI cannot free a single set, so a
        // capacity that does not cover what the node allocates is not a
        // compile error and not a wrong pixel -- it is an
        // AllocateDescriptorSets failure part-way through Create at the
        // desk. TWO dimensions now (Task 8/10): FramesInFlight()
        // frame sets (one CONSTANT_BUFFER descriptor each, plus -- F3 plan 1
        // T7 -- two STRUCTURED_BUFFER descriptors each: the instance rows
        // and the slot's visible indices) plus ONE bindless set
        // (kBindlessCapacity TEXTURE descriptors, MeshNode.cpp). The root
        // sampler consumes NO pool budget at all -- RootSamplerDesc is "not
        // allocated from a descriptor pool" (NRIDescs.h:1077).
        [[nodiscard]] static nri::DescriptorPoolDesc PoolSizes(bool bindlessUpdateAfterSet) noexcept;

    private:
        MeshNode() = default;

        bool Init(NriGraphContext& context);
        // Creates the BindlessTable (gated on the bindless tier and both
        // sampled-image limits by Init(), before this runs). Called before
        // CreateBindings(), which
        // sizes the bindless descriptor range off kBindlessCapacity alone
        // (a compile-time constant) rather than this table, so ordering
        // between the two is not otherwise load-bearing.
        bool CreateBindless();
        bool CreateBindings();
        bool CreateConstantArena();
        // Allocates the per-frame-slot descriptor sets (the b1 CONSTANT_
        // BUFFER range written once here; the t0/t1 STRUCTURED_BUFFER ranges
        // left for Record to write per slot, lazily, when the GPU scene's
        // buffers change -- the header block) and the ONE bindless
        // descriptor set (its range left UNWRITTEN here -- AddMaterial
        // writes it incrementally, and CreateBindings' PARTIALLY_BOUND flag
        // is what makes allocating it with an empty range legal). No
        // ResetDescriptorPool anywhere: nothing is ever freed. Frame-set
        // rewrites happen after that slot retires; bindless appends use
        // update-after-set when supported and otherwise wait for device idle.
        bool CreateSets();

        // One fixed material state for this frame's attachment formats, from
        // the shared cache. Null (already logged) if the cache refused it.
        [[nodiscard]] nri::Pipeline* PipelineFor(nri::Format canvasFormat,
                                                  MaterialBlendMode blend, bool twoSided);

        [[nodiscard]] static std::size_t PipelineIndex(MaterialBlendMode blend, bool twoSided) noexcept;

        // How many distinct mesh guids m_residents reserves room for. Prepare
        // builds that table once per frame so a scene of twenty cubes resolves
        // once and draws twenty times; a RESERVED MEMBER rather than a local, so
        // the steady state allocates nothing inside the declaration window. Past
        // this many distinct meshes the vector grows once per high-water mark.
        ARC_CONSTANT("capacity hint: resident-slot reserve; no observable preference")
        static constexpr std::size_t kInitialResidentSlots = 16;

        [[nodiscard]] std::uint64_t ArenaOffset(std::uint32_t frameSlot) const
        {
            return CbRegionOffset(m_arenaStride, frameSlot);
        }

        NriDevice*        m_device       = nullptr;
        NriPipelineCache* m_pipelines    = nullptr;
        // NO m_owner BACK-POINTER, deliberately: everything this node needs
        // from the vehicle (device, pipeline cache, shader bytecode) is taken
        // at Create, and the two per-frame values it reads -- the canvas format
        // and the frame slot -- arrive as Record/Prepare parameters.
        // Batch2DNode carries one that nothing reads; this does not copy it.
        //
        // NO NriTextureCache POINTER, still: this node's own images are its
        // BindlessTable's SRVs, Added (and therefore owned/resolved) by
        // whoever calls AddMaterial -- a test, or Task 11's scene resolver,
        // which is the consumer of the shared residency cache, not this
        // node. AddMaterial only needs the already-resolved nri::Descriptor*.

        // Bytecode is OWNED BY THE VEHICLE (NriGraphContext's bin cache) and
        // outlives this node -- which the pipeline cache's fill contract
        // (rule 2) requires, since CreateGraphicsPipeline runs after the fill
        // callback returns.
        std::span<const std::uint8_t> m_vs;
        std::span<const std::uint8_t> m_ps[3];

        // THE BINDLESS MATERIAL TABLE (Task 8/10) and the ONE descriptor set
        // its array lives in -- allocated once, at Create, and written
        // INCREMENTALLY by AddMaterial (contrast m_sets below, written once
        // and never again). Null/kInvalidLayout-shaped until CreateBindless/
        // CreateSets succeed; Create() refuses the whole node rather than
        // leave either half-built (Init()'s `&&` chain).
        std::unique_ptr<BindlessTable> m_bindless;
        nri::DescriptorSet*            m_bindlessSet = nullptr;
        // Descriptor indexing does not imply Vulkan's granular sampled-image
        // update-after-bind feature. False selects AddMaterial's idle fallback.
        bool                           m_bindlessUpdateAfterSet = false;

        nri::DescriptorPool* m_pool = nullptr;
        std::uint32_t        m_layoutId = NriPipelineCache::kInvalidLayout;

        // The per-frame-slot b1 arena: ONE HOST_UPLOAD buffer, persistently
        // mapped, carved into FramesInFlight() regions.
        nri::Buffer*     m_arena       = nullptr;
        void*            m_arenaCpu    = nullptr;
        std::uint64_t    m_arenaStride = 0;
        nri::Descriptor* m_frameCbView[kMaxFramesInFlight]{};

        // THE PER-FRAME descriptor sets, one per frame slot. Each binds that
        // slot's b1 region (written ONCE at Create -- Task 8/10 moved the
        // white texel/sampler out; see m_bindless/m_bindlessSet above and
        // the root sampler in CreateBindings) and, since F3 plan 1 T7, the
        // GPU scene's t0 instance view + t1 the slot's visible-index view,
        // rewritten by Record when either moved. One dimension only (the
        // frame slot), because the slot is the only thing a set differs by.
        nri::DescriptorSet* m_sets[kMaxFramesInFlight]{};

        // WHAT EACH SLOT'S SET CURRENTLY NAMES at t0/t1 (F3 plan 1 T7), so
        // Record rewrites the two ranges only when the GPU scene's buffers
        // actually moved: the instance buffer's generation (bumped by every
        // GpuScene growth; the view object is replaced with it) and the
        // slot's visible-index view pointer (replaced when GpuScene re-
        // creates the slot's buffer -- there is NO generation counter for
        // those, so the pointer itself is the identity). Zero / null until
        // the slot's first Record, which therefore always writes.
        std::uint64_t          m_setInstanceGen[kMaxFramesInFlight] = {};
        const nri::Descriptor* m_setVisibleView[kMaxFramesInFlight] = {};

        // MEMBERS, not locals, and that is load-bearing:
        // nri::GraphicsPipelineDesc::vertexInput is a POINTER into caller
        // memory that CreateGraphicsPipeline dereferences AFTER the fill
        // callback has returned (NriPipelineCache.hpp, fill-contract rule 2).
        nri::VertexAttributeDesc m_attributes[3]{};
        nri::VertexStreamDesc    m_stream{};
        nri::VertexInputDesc     m_vertexInput{};

        // Resolved by Prepare for every blend/sidedness combination before a
        // command buffer opens. Owned by the cache; borrowed here.
        nri::Pipeline* m_pipeline[6]{};

        // This frame's distinct-guid residency -- filled by Prepare, looked
        // up by Record. Never resolved at record time.
        //
        // BORROWED POINTERS INTO THE CACHE'S MAP, so the table is cleared at BOTH
        // ends of its life: at the top of Prepare (it is about to be rebuilt) and at
        // the bottom of Record (its last reader is done). Without the second clear,
        // an InvalidateMeshGeometry landing between Record(N) and Prepare(N+1) --
        // which erases the map node -- would leave dangling pointers sitting in a
        // live member. Nothing dereferences them today; clearing costs nothing and
        // removes the trap rather than documenting it.
        std::vector<std::pair<Guid, const NriMeshBufferCache::Resident*>> m_residents;

        // THE AD-HOC ROWS (F3 plan 1 T7), built by Prepare from
        // scene.instances (at most GpuScene::ScratchRows()) and read twice:
        // by AddMeshNode, which hands AdHocRows() to the sync node (copied
        // into this slot's scratch region), and by Record, which draws
        // m_adHocDraws[i] direct from scratch row i. Parallel vectors: row i
        // of one is draw i of the other. MEMBERS, not locals, because the
        // span the sync node's exec fn captures has to outlive the
        // declaration window -- both exec fns run inside the same RenderFrame
        // call, before the next Prepare rebuilds them.
        struct AdHocDraw
        {
            Guid          mesh{};
            std::uint32_t indexOffset = 0;
            std::uint32_t indexCount  = 0;   // 0 == the whole mesh (MeshInstance::indexCount's contract)
        };
        std::vector<GpuInstance> m_adHocRows;
        std::vector<AdHocDraw>   m_adHocDraws;

        // One WARN/ERROR each, not one per instance per frame, for the
        // degradations a reader must be able to see.
        bool m_warnedNoPipeline      = false;
        bool m_warnedBadCamera       = false;
        bool m_warnedScratchOverflow = false;   // Prepare: ad-hoc instances past m_scratchRows dropped
        // The context's GpuScene::ScratchRows(), latched by Init: the cap
        // Prepare applies, so the span it stages always fits the scratch region.
        std::uint32_t m_scratchRows = 0;
    };

    // Declares the mesh node -- opaque, masked and ordered transparent, the
    // head comment's ONE pass -- into `graph` and hands back the D32_SFLOAT
    // depth transient it created and attached.
    //
    // THE DEPTH TARGET IS MINTED HERE, in this node's own Setup -- the same
    // create-then-write-then-attach shape AddBatch2DNode uses for the canvas,
    // and the reason Task 4's placeholder "depth" node is not declared on a
    // frame that carries a mesh scene. A transient can only be minted from a
    // RenderGraphBuilder, and a builder exists only inside a node's Setup, so
    // the node that consumes it is the natural one to create it.
    //
    // `canvas` is an INPUT (unlike AddBatch2DNode's, which is an output):
    // batch2d already minted and cleared it, and this pass draws on top.
    //
    // `canvasFormat` IS A PARAMETER AND NOT AN ASSUMPTION. NRI bakes attachment
    // formats into a graphics pipeline, so binding one inside a
    // CmdBeginRendering whose colour attachment carries a different format is
    // undefined on both backends -- and RenderGraph exposes no way to read a
    // handle's format back, so this function cannot derive it. It must be the
    // format `canvas` was CREATED with; the caller that minted the handle is
    // the one that knows. (It was hardcoded to the graph canvas format until Task
    // 7's first fix round, which made a differently-formatted canvas a silent
    // mismatch with no diagnostic.)
    //
    // `context` is a POINTER because the device-less [nri] frame-shape cases drive
    // the real declarations with no device: with a null context every
    // declaration is identical and the exec fn does nothing, which is exactly
    // what makes those cases able to fail when this function's DECLARATIONS
    // change.
    //
    // `scene` is BORROWED at declaration time and its SPAN is copied into the
    // exec fn -- see MeshSceneDesc::instances for the lifetime rule.
    //
    // SINCE F3 PLAN 1 T6 this declares TWO nodes: "gpuscene-sync" first
    // (GpuSceneSyncNode.hpp -- the GPU scene's writer, importing the three
    // persistent buffers as CopyDst), then "mesh", which Reads the same
    // handles (instances + visible indices ShaderRead, args IndirectArgs) so
    // the graph derives the copy -> read barriers. The returned handle is
    // still the depth transient. Since T7 the sync node is ALSO handed the
    // ad-hoc rows Prepare built (MeshNode::AdHocRows) for its scratch
    // region, and Record draws both halves -- the batches indirect, the
    // ad-hoc rows direct -- with the context's GpuScene passed in.
    ARC_RENDER_API RgTexture AddMeshNode(RenderGraph& graph, NriGraphContext* context,
                                      RgTexture canvas, nri::Format canvasFormat,
                                      const MeshSceneDesc& scene,
                                      std::uint32_t width, std::uint32_t height);
}
