#pragma once

// The ONLY unmangled C surface in the engine. The game DLL exports the seven
// GamePlugin_* functions; the host resolves them by name into a PluginVTable.
// EngineContext is the C++ facade handed to the plugin (Arcane::Runtime).

#include <Arcane/Core/Api.hpp>
#include <Arcane/EcsFwd.hpp>   // Arcane::{TypeContext,BinaryWriter,BinaryReader}, Arcane::IWorkScheduler
#include <Arcane/Plugin/SystemFactory.hpp>   // NetMode (an EngineContext field, ABI 30)

#include <cstdint>
#include <Arcane/Core/Constant.hpp>

namespace Arcane
{
    class Runtime;  // defined in ArcaneCore.dll; the plugin holds it opaquely via EngineContext
    class ProcessContext;  // <Arcane/Base/ProcessContext.hpp>; the process's one (ABI 30)
    class ClientRuntime;   // ArcaneClient.dll's presentation extension; null on a headless host (ABI 30)
    struct ITaskExecutor;  // <Arcane/Jobs/TaskExecutor.hpp>; same enki pool, worker-index face

    // Bump on ANY change to EngineContext layout or the entry-point set/signatures.
    // v2 (2026-06-20): added the ImGui cross-DLL handoff fields below + GamePlugin_DrawUI.
    // v3 (2026-06-26): added taskExecutor (the ITaskExecutor face of the engine scheduler).
    // v4 (2026-07-10): foundations branch changed the cross-DLL C++ surface plugins
    //     consume through EngineContext: Runtime::SnapshotRegistry now returns
    //     Astra::Result (E02-4), Batcher2D gained the pure virtual RemoveTexture
    //     mid-vtable (E01-2), and Assets gained SetDevice before GetTexture. Any of
    //     these mismatched across the boundary is a vtable/stack smash; the version
    //     gate must reject a cross-build pairing.
    // v5 (2026-07-19): the re-vendored Astra migrated its threading seam to
    //     Mosaic, so workScheduler is now Mosaic::IWorkScheduler (per-lane worker id
    //     + FunctionRef callback) -- a DIFFERENT vtable than the prior Astra-native
    //     IWorkScheduler. A stale plugin would vtable-mismatch; reject the pairing.
    // v6 (2026-07-24): shader-editor Slice 8 changed the cross-DLL surface twice:
    //     Batcher2D gained RegisterMaterial/UpdateMaterial/SetGlobals/QuadMaterial
    //     mid-vtable, and SpriteRenderer grew a Guid `material` field (+16 bytes;
    //     plugins compile the header-only RenderSubmissionSystem + components).
    //     A stale plugin's SetLayer call lands on UpdateMaterial and its Quad on
    //     SetGlobals -- observed as an AV on project open; reject the pairing.
    // v7 (2026-07-24): LocalTransform renamed to Transform. The reflected type
    //     name IS the cross-module component identity (TypeID name hash +
    //     name-keyed scene JSON), so a stale plugin would register/query the
    //     old name and silently diverge from engine systems; reject the pairing.
    //     NOTE (2026-07-25, post arc slices 2+3, deliberately NO bump):
    //     OffscreenCanvas gained SetPostChain/SetPostGlobals APPENDED at the
    //     END of its vtable (every pre-existing slot index unchanged; only
    //     hosts -- in-tree, rebuilt with the engine -- call the new slots),
    //     and PostProcess is an ADDED component type (name-keyed identity; a
    //     stale plugin's roster simply lacks it -- typed views come back
    //     empty, nothing smashes). Neither changes EngineContext, the entry
    //     points, or any existing layout, so a v7<->v7 cross-build pairing
    //     stays memory-safe in both directions.
    //     Outliner slice 1 rides the same argument: Identity + Hidden are
    //     two more appended name-keyed component types, and the Not<Hidden>
    //     filter in the plugin-compiled RenderSubmissionSystem is behavioral
    //     (a stale plugin just doesn't honor hiding). Still NO bump.
    // v8 (2026-07-28): the sprite-asset arc re-shaped SpriteRenderer's LAYOUT --
    //     textureId (uint32) + size (vec2) removed, a Guid `sprite` added first --
    //     so tint/sortingLayer/orderInLayer/shape/material all sit at new
    //     offsets, and TextureTable was deleted from SceneResources.hpp. Plugins
    //     compile both headers themselves (components + the header-only
    //     RenderSubmissionSystem), which is the same change class the v6 entry
    //     above (:34-35) bumped for. Unlike v6 the failure is not a vtable slide
    //     but the plugin's own copy of submission reading the component at stale
    //     offsets: tint moved 12 -> 16 and sortingLayer 28 -> 32, so a stale tint
    //     read at 12 comes out of the tail of the new `sprite` Guid plus the
    //     first three tint floats, and a stale sortingLayer read at 28 comes out
    //     of tint.w. It also resolves a TextureTable resource the engine no
    //     longer registers. Corruption rather than a guaranteed AV, and silent
    //     either way; reject the pairing.
    // v9 (2026-07-29): Astra re-vendored to dev f8c9998 -- 40 of 60 shared
    //     headers changed, 8 added, 1 removed (Archetype/ArchetypeGraph.hpp).
    //     Same reasoning as the v5 entry above (:28-31), which bumped for a
    //     previous Astra re-vendor: plugins compile Astra's headers THEMSELVES
    //     (Registry, ComponentRegistry, Archetype storage, EntityTable,
    //     Serialization's Binary{Reader,Writer} -- the very types crossing the
    //     boundary through EngineContext::typeContext and the SaveState/
    //     LoadState entry points), so a plugin built against the old headers
    //     and a host built against the new ones disagree on layout and on
    //     TypeContext identity rules. Concretely observable: the new
    //     TypeContext REFUSES a type whose unqualified name-hash collides with
    //     an already-registered one (it used to alias them silently), handing
    //     back an INVALID ComponentID that then overflows the component bitmap
    //     -- an assert in a checked build, a wrong-component read in a
    //     shipping one. Reject the pairing.
    // v10 (2026-08-09): Astra re-vendored to dev f8a75a9 -- the ComponentModule
    //     program. ReRegisterComponent NO LONGER EXISTS (plugins own their types
    //     via RAII ComponentModule handles, heap-held + reset in Shutdown), and
    //     ComponentRegistry grew owner/shadow/meta-thunk state, so a v9 plugin's
    //     inlined template code manipulates a registry whose layout changed.
    //     Layout mismatch plus a removed API: reject the pairing.
    // v11 (2026-08-14): Batcher2D grew a virtual -- Batch2DDrained Drain(), the
    //     read interface Batch2DNode consumes instead of
    //     End(). Plugins compile their own copy of Batcher2D.hpp (through
    //     SceneResources.hpp's RenderContext2D, which the header-only
    //     RenderSubmissionSystem calls) and dispatch through this vtable, so a
    //     module built against a Batcher2D with a different virtual set is the
    //     same failure class as the v6 entry above (:34-35). Drain() is declared
    //     LAST specifically so no EXISTING slot moves -- a v10 module would in
    //     fact still dispatch correctly -- but "correct by luck" is not what the
    //     gate is for: the header a v10 module baked in has no Drain() at all,
    //     so anything that calls it through a plugin-provided Batcher2D
    //     implementation (a test double, a future editor batcher) hits a slot
    //     that module never emitted. Reject the pairing; the failure mode of
    //     NOT rejecting it is silent.
    // v12 (2026-08-14): Batcher2D grew a second appended virtual --
    //     `const Material2DDesc* MaterialDesc(uint16_t)`, the id -> registration
    //     lookup the render path needs to build a REGISTERED material's own
    //     pipeline and bindings. Two data layouts moved with
    //     it and each is independently sufficient to require this bump:
    //     Material2DDesc gained the retained shader BLOBS (vsBytes/psBytes) and
    //     Batch2DDrained gained `globals`. Both are passed BY VALUE across the
    //     Batcher2D vtable (RegisterMaterial takes a Material2DDesc; Drain
    //     returns a Batch2DDrained), so a module compiled against the v11
    //     layouts and a host compiled against these disagree about object size
    //     -- which corrupts the stack rather than merely misdispatching. Same
    //     verdict as v11, with a harder reason.
    // v13 (2026-08-14): THE SEVERANCE -- the frame's data-supply side stops
    //     depending on an NVRHI device, and three plugin-compiled surfaces
    //     move with it. ONE bump covers all three, deliberately:
    //       * Batcher2D grew a THIRD appended virtual, `QuadTextured(uint16_t,
    //         const Guid&, ...)` -- QuadMaterial plus the image ASSET's Guid.
    //         Appended, so no existing slot moves; the v11 entry above (:96-103)
    //         states why "correct by luck" is still refused.
    //       * Batch2DDrawSpan gained `Guid textureId` (+16 bytes) and it is
    //         returned BY VALUE across the vtable inside Batch2DDrained (a span
    //         over an array of them, whose ELEMENT STRIDE just changed), so a
    //         v12 module iterating a v13 host's drained spans reads every field
    //         of every span after the first at the wrong offset. Same failure
    //         class as v12's Material2DDesc growth, one indirection deeper.
    //       * SpriteEntry (Scene/SceneResources.hpp) gained `Guid textureId`,
    //         and the header-only RenderSubmissionSystem the plugin compiles
    //         ITSELF now reads it and calls QuadTextured. A v12 module's copy
    //         of that system reads a SpriteEntry one size smaller out of a map
    //         the host filled -- every entry past the first at a stale offset.
    //     Why any of this: no NVRHI device exists, so
    //     Assets::GetTexture yields null for every sprite and the
    //     `nvrhi::ITexture*` in a span can no longer tell "untextured" from
    //     "textured on a device that is not there". The Guid can, and the NRI
    //     recorder resolves it through the shared NriTextureCache. Reject the
    //     pairing.
    // v14 (2026-08-19): TWO independent reasons, and the SECOND is the
    //     stronger one -- it gates a layout change that had been sitting in
    //     the tree UNGATED.
    //       * THE RENDER SURFACE NO LONGER EXPOSES NVRHI TYPES. Runtime lost
    //         SetRenderResources(nvrhi::IDevice*, ShaderLibrary*), Device() and
    //         Shaders() outright (Base/Runtime.hpp). Both hosts had passed
    //         (nullptr, nullptr) by then and both accessors could only
    //         return null, so nothing observable changes -- but Runtime is a
    //         concrete class a plugin links against, and removing three exported
    //         members changes what a module built against v13 resolves at load.
    //       * MATERIAL2DDESC SHRANK AT ABI 13 AND WAS NEVER GATED: `vs`,
    //         `ps` and `paramTextures` were deleted from Material2DDesc.
    //         That struct crosses the plugin boundary BY VALUE:
    //         Runtime::SetRenderContext(Batcher2D*) hands the plugin a batcher,
    //         plugins compile their OWN copy of Batcher2D.hpp (:92 above), and
    //         RegisterMaterial(Material2DDesc)/UpdateMaterial are by-value
    //         vtable slots. This is EXACTLY the class of change v12 was bumped
    //         for ("Material2DDesc gained the retained shader BLOBS", :105-111)
    //         and v13 ("same class as v12's growth, one indirection deeper",
    //         :125-129) -- a host and a module that disagree about the size of a
    //         by-value argument corrupt the stack rather than merely
    //         misdispatching. Until this bump a module built before that
    //         shrink loaded into the current engine with NO gate at all: the
    //         v12/v13 failure class with its safety catch disarmed. Closed
    //         here.
    //     WHAT v14 DOES NOT COVER -- read this before assuming Batcher2D is
    //     clean: Batcher2D.hpp still carries NVRHI in nine places, including
    //     `Batch2DDrawSpan::texture` (an nvrhi::ITexture* returned BY VALUE
    //     inside Batch2DDrained) and the Begin/End/Quad/Glyph/RemoveTexture
    //     recorder virtuals. That whole half is dead (every production
    //     Batcher2D::Create passes (nullptr, nullptr); End() has no caller
    //     outside SeveranceTest), but it cannot be removed here: the internal
    //     run list IS Batch2DDrawSpan (`using BatchRun = Batch2DDrawSpan`), so
    //     the field and the recorder go together, and the recorder's signatures
    //     reach into Text/TextSystem and ShaderLibrary. Those go in a later
    //     bump, and they will need one. Reject the pairing.
    //     (Both named files are gone as of v15; the entry was accurate when
    //     it was written.)
    // v15 (2026-08-19): exactly the bump v14's closing paragraph above owed.
    //     NVRHI LEAVES THE PLUGIN SURFACE
    //     ENTIRELY: no header a game module compiles includes <nvrhi/...> any
    //     more, which is what lets Task 10 delete ThirdParty/nvrhi. Three
    //     independent reasons, each on its own sufficient:
    //       * BATCH2DDRAWSPAN SHRANK. `nvrhi::ITexture* texture` is deleted
    //         from the MIDDLE of the struct, so `firstIndex`/`indexCount`/
    //         `textureId` all move. It is returned BY VALUE across the vtable
    //         inside Batch2DDrained (a span over an array of them, whose
    //         ELEMENT STRIDE changed again), so a v14 module reads every field
    //         of every span after the first at the wrong offset. Precisely the
    //         v13 failure class (:124-129), running the other direction.
    //       * SPRITEENTRY SHRANK, AND AT ITS FIRST FIELD. `nvrhi::ITexture*
    //         texture` was SpriteEntry's leading member (Scene/
    //         SceneResources.hpp); removing it moves every remaining field,
    //         and the header-only RenderSubmissionSystem the plugin compiles
    //         ITSELF reads them out of a map the host filled. Same reasoning
    //         as the v13 entry's third bullet (:130-134), inverted.
    //       * THE BATCHER2D VTABLE CHANGED SHAPE, NOT JUST ITS SIGNATURES.
    //         Five virtuals lost NVRHI parameters (Begin, Quad, QuadMaterial,
    //         Glyph, QuadTextured), Create's static factory beside them
    //         changed signature too (never a vtable slot), and
    //         `RemoveTexture(nvrhi::ITexture*)` was REMOVED FROM THE
    //         MIDDLE of the class -- which slides Stats/Drain/MaterialDesc/
    //         QuadTextured up one slot each. That is the exact hazard the v11
    //         entry (:96-103) explains and Drain()'s "declared last" comment
    //         guards against; it is done here deliberately, and it is safe
    //         ONLY because this gate refuses every pre-v15 module outright.
    //     Assets also lost SetDevice, both GetTexture overloads and the three
    //     NVRHI free functions (LoadDisplayTexture/ReadTexturePixels/
    //     SaveTexturePng), and ShaderLibrary was deleted outright -- all
    //     callerless, none of them by-value plugin surface, so they add
    //     nothing to the verdict above. NOTHING SHIPPED between v14 and v15,
    //     so no compatibility shim is owed. Reject the pairing.
    // v16 (2026-08-21): Transform went 3D. position/scale widened from
    //     glm::vec2 to glm::vec3, rotation from a float (radians) to a
    //     glm::quat, and ToMatrix from mat3 to mat4; WorldTransform::matrix and
    //     PreviousTransform moved with them.
    //     This is the class of change the v7 entry above (:38-42) established --
    //     these are name-keyed reflected components that a plugin COMPILES
    //     ITSELF from this header set -- except that where v7 changed the
    //     component's identity, v16 changes its LAYOUT: Transform grows
    //     20 -> 40 bytes with every field at a new offset -- vec2 + float + vec2
    //     = 8 + 4 + 8, all align-4 and unpadded, becomes vec3 + quat + vec3 =
    //     12 + 16 + 12 (glm's quat is packed_highp here, align 4, not the
    //     16-byte-aligned SIMD flavour) -- and WorldTransform 36 -> 64
    //     (mat3 -> mat4). That is the same hazard as the v8 SpriteRenderer
    //     re-shape (:55-62), on the type EVERY spatial entity carries.
    //     The header-only systems a plugin instantiates moved with the types:
    //     a v15 copy of RenderSubmissionSystem reads a sprite's world
    //     translation from matrix[2], which in a mat4 is the Z BASIS AXIS, so
    //     every sprite it submits would draw at the origin; a v15
    //     TransformPropagationSystem would compose mat3s over mat4 storage.
    //     Reject the pairing.
    //     Edit::WorldMatrix/ParentWorldMatrix and DecomposeTRS/ComposeTRS
    //     (ARC_API, mat3 -> mat4) also changed signature, but a mangled-name
    //     mismatch fails loudly at load; it adds nothing to the verdict above.
    // v16, amended (2026-08-22, F1 Task 4): the transform PROPAGATION was
    //     replaced -- TransformSystems.hpp gained a public TransformOrder
    //     (a Registry resource holding a flat topological order + dirty state)
    //     and TransformPropagationSystem stopped walking
    //     Relations::ForEachDescendant. The version STAYS AT 16 deliberately:
    //     no existing type's layout moved, no ARC_API signature changed, and
    //     TransformOrder is new, so a module built against the earlier v16
    //     header cannot reference it. Both copies of the header-only system
    //     compute the same `parentWorld * local` product and this system is the
    //     sole writer of WorldTransform::matrix, so a mixed pairing is
    //     redundant, not wrong -- which is the whole reason it does not need a
    //     rejection.
    //     Say what that costs, though: a module carrying the earlier v16 header
    //     REGISTERS ITS OWN copy of the system (ReferenceGame.cpp does exactly
    //     this into fixedUpdate), so it reintroduces the two per-frame
    //     ForEachDescendant traversals inside its own scheduler. Task 4's
    //     headline property -- a steady frame walks no descendants -- is
    //     therefore HOST-SIDE ONLY under such a pairing, and is only a
    //     whole-process property once the module is rebuilt against this
    //     header. Accept the pairing; rebuild the module to get the win.
    // v16, amended (2026-08-22, F1 final review): two additive changes, and the
    //     version STAYS AT 16 for both.
    //       * `ARC_API bool IsPlanarBasis(const glm::mat4&)` joins
    //         DecomposeTRS/ComposeTRS in Edit/Gizmo.hpp. A NEW export: no
    //         existing signature moved, and a module built against the earlier
    //         v16 header cannot reference a symbol it has no declaration for.
    //       * `Arcane::Collider2D::fixtures` gained Serializable(false). That is
    //         a reflection ATTRIBUTE, not layout -- the struct is byte-identical
    //         and the binary Serialize(Archive&) path does not consult it. A
    //         mixed pairing disagrees only about whether the name-keyed JSON
    //         walk visits a field that neither side could read or write anyway
    //         (it has no container branch), which is why this could brick a
    //         scene rather than corrupt one. Accept the pairing.
    //     THE SCENE FILE FORMAT DID MOVE, though, and it has its own number:
    //     Scene::kSceneJsonVersion went 2 -> 3 for the 3D Transform's on-disk
    //     shape, which the v16 entry above describes and which nothing had
    //     stamped until now. That gate is enforced per FILE (SceneAsset.hpp)
    //     and per CLIPBOARD PAYLOAD (Edit::InstantiateSubtrees) -- both HOST
    //     paths, so it is not a plugin-pairing question today (no module in the
    //     tree loads a scene itself). It is recorded here because every
    //     .arcscene beside a v16 module has to carry 3, the same way the
    //     .arcproj beside it has to carry 16: ReferenceProject and Gacha's Game
    //     were both restamped with this change.
    // v17 (2026-08-22, F2a): the mesh vocabulary lands -- MeshRenderer, the
    //     .arcmesh asset, and the caches/resources/exports that resolve one
    //     into the other. Checked against the SAME "does this actually
    //     corrupt a mixed pairing" bar the v7 and v16-amended entries above
    //     apply, and on that bar every individual piece is additive:
    //       * `MeshRenderer` (Scene/Components.hpp) is a NEW name-keyed
    //         reflected component -- the v7 PostProcess/Identity/Hidden case
    //         (:51-54 above): a stale plugin's roster simply lacks it, typed
    //         views come back empty, nothing smashes.
    //       * `MeshTable`/`MeshMaterialTable` (Scene/SceneResources.hpp) are
    //         NEW registry resources, published through TWO NEW methods
    //         APPENDED to Runtime -- `SetMeshTable`/`SetMeshMaterials`,
    //         siblings of `SetSpriteTable`/`SetSpriteMaterials`
    //         (Base/Runtime.hpp:162,169). `MeshTable`'s value type,
    //         `MeshEntry` (geometry + bounds + a `material` Guid copied off
    //         the loaded .arcmesh), crosses too, not just the resource
    //         wrapping it: the mesh sweep (F2a's MeshSubmissionSystem.hpp,
    //         since F3 plan 1 Render/GpuSceneSync.hpp) is HEADER-ONLY and
    //         plugin-compiled, and reads `MeshEntry` straight off the
    //         resolved table, including `MeshEntry::slots` for the override
    //         -> mesh-default fallback -- the v8/v15 class of plugin-crossing surface,
    //         additive here only because the type is brand new. Runtime
    //         crosses the boundary by POINTER (EngineContext::engine) and,
    //         unlike Batcher2D (v6/v11/v12/v13 above), declares no virtual
    //         anywhere -- no vtable, so there is no slot for an appended
    //         member to slide (contrast the v14 entry's "concrete class a
    //         plugin links against" warning, :143-149, which was about
    //         REMOVING members, not adding one).
    //       * `MaterialSurface` (Material/MaterialSource.hpp) gained a THIRD
    //         value, `Mesh`, APPENDED after `Sprite` -- Fullscreen and Sprite
    //         keep their existing integer values, so nothing that already
    //         switches on this enum reads a different case.
    //       * Everything else named below -- `MeshCache`, `MeshMaterialCache`,
    //         `ComputeMeshBounds`, `BuildPlane`/`BuildCylinder`/
    //         `BuildCapsule`, `LoadMaterialParentChain`, `SaveMeshAsset`/
    //         `LoadMeshAsset`/`ValidateMeshAsset`/`BuildMeshData` -- is a
    //         brand-NEW exported symbol; a stale plugin never links against a
    //         symbol it has no declaration for, and MeshCache/
    //         MeshMaterialCache fill the same ENGINE-SIDE per-frame-
    //         resolution role SpriteCache/SpriteMaterialCache already have
    //         (MeshCache.hpp's own header comment), not a plugin-driven one.
    //     So this bump is NOT a corruption finding, and is recorded as such
    //     rather than dressed up as one. It is the same desk-legibility call
    //     the v16 entry's closing paragraph (:274-277) made for scene schema
    //     3 beside ABI 16: a game module now has an entire vocabulary --
    //     MeshRenderer, the .arcmesh round trip (MeshSource/MeshAssetData/
    //     SaveMeshAsset/LoadMeshAsset/ValidateMeshAsset/BuildMeshData), the
    //     "mesh" .arcmat kind, MeshCache/MeshMaterialCache resolving both
    //     into MeshTable/MeshMaterialTable each frame, ComputeMeshBounds, and
    //     the three new unit generators (BuildPlane/BuildCylinder/
    //     BuildCapsule) -- it could not reference before this header, and 17
    //     is the stamp that says a module was built knowing it exists.
    //     ReferenceProject and Gacha's Game are restamped with this change,
    //     the same precedent.
    //     THE SCENE FILE FORMAT DID NOT MOVE: MeshRenderer is additive the
    //     same way PostProcess/Identity/Hidden were (v7 above), so
    //     Scene::kSceneJsonVersion (Serialization/SceneSerializer.hpp:62)
    //     stays at 3.
    // v18 (2026-08-30, arc 2): the automation-tooling close-out moved three
    //     ARC_API surfaces. Checked against the same "does this actually
    //     corrupt a mixed pairing" bar the v7, v16-amended and v17 entries
    //     apply -- and unlike v17, TWO of these are genuine LAYOUT changes, so
    //     this one clears that bar rather than resting on desk legibility:
    //       * `HostConfig` (Host/HostConfig.hpp:13, an ARC_API STRUCT)
    //         gained `settleTimeoutMs` (uint64). Layout. A v17 module holding
    //         one by value disagrees with the host about its SIZE.
    //       * `VerifyReport` (Host/VerifyReport.hpp:134, an ARC_API CLASS)
    //         gained SetSettle plus five members, and its JSON went
    //         schemaVersion 2 -> 3 with `mode` changing value "offscreen" ->
    //         "headless". Layout, and a WIRE contract change besides -- the
    //         version field is what announces it, which is the whole reason a
    //         wire value may move on a bump and not otherwise.
    //       * `Project::Open` and `Runtime::OpenProject` each gained a
    //         DEFAULTED third parameter, `ProjectOpenOptions`. Defaulted is a
    //         source-compatibility convenience only: it changes the MANGLED
    //         NAME either way, so a module built against the v17 header that
    //         referenced either symbol fails IMPORT RESOLUTION at load. That
    //         is loud and immediate, not a silent misread -- but it is exactly
    //         what this gate exists to catch before LoadLibrary rather than
    //         after.
    //     Additive and harmless on their own, recorded so the list is complete:
    //     `ProjectOpenOptions` (Project/ProjectOpenOptions.hpp) and
    //     `SettleBound.hpp` are new headers -- the latter is constexpr-only and
    //     pulls in nothing but <cstdint>; `BootContext` gained a field, but
    //     Host/ProjectBoot.hpp is consumed by the two HOSTS and ArcaneTests and
    //     by no game module.
    //     MEASURED, not assumed: nothing in ReferenceProject's game module
    //     references HostConfig, VerifyReport, ProjectBoot, Project::Open or
    //     OpenProject, so no module in the tree breaks on this bump.
    //     THE SCENE FILE FORMAT DID NOT MOVE: Scene::kSceneJsonVersion stays
    //     at 3. ReferenceProject and Gacha's Game are restamped with this
    //     change, the same precedent v16 and v17 set.
    // v19 (2026-09-03, Arc A -- the automation verdict vocabulary): the two
    //     ARC_API surfaces v18 moved MOVED AGAIN, and a third joined them.
    //     Checked against the same "does this actually corrupt a mixed
    //     pairing" bar the v18 entry applies -- all three clear it on layout
    //     or on mangled name, so this one does not rest on desk legibility
    //     either:
    //       * `HostConfig` (Host/HostConfig.hpp:13, an ARC_API STRUCT)
    //         gained `std::optional<double> fixedTimeSeconds` -- --fixed-time,
    //         which pins the scene clock independently of the frame count.
    //         LAYOUT: `optional<double>` is 16 bytes, so a v18 module holding
    //         a HostConfig by value disagrees with the host about its SIZE.
    //         Identical failure to the one the v18 entry recorded for
    //         `settleTimeoutMs`.
    //       * `VerifyReport` (Host/VerifyReport.hpp, an ARC_API CLASS)
    //         gained `m_compareMaxLocalDifference`, and `SetCompare` gained a
    //         NON-DEFAULTED eleventh parameter (`double maxLocalDifference`).
    //         Both at once: the member is LAYOUT, and the new parameter moves
    //         SetCompare's MANGLED NAME, so a v18 module that called it fails
    //         import resolution at load. The report's JSON went schemaVersion
    //         3 -> 4 with an added `compare.maxLocalDifference`. Unlike the
    //         2 -> 3 move, NO existing field changed meaning -- which is why
    //         `kOldestSupportedSchemaVersion` stays 3 and a 3-era consumer
    //         still reads a v4 report correctly.
    //       * `Arcane::Compare` (Assets/ImageCompare.hpp:186-191, ARC_CORE_API)
    //         gained a DEFAULTED trailing parameter,
    //         `std::array<std::uint64_t, 100>* localBlocks = nullptr`.
    //         Defaulted is a source-compatibility convenience only -- the same
    //         reasoning the v18 entry gives for `ProjectOpenOptions`: it moves
    //         the mangled name either way, so a v18 module that referenced
    //         this symbol fails import resolution at load. Loud and immediate,
    //         not a silent misread, but still exactly what this gate exists to
    //         catch before LoadLibrary rather than after.
    //     Additive and harmless on their own, recorded so the list is complete:
    //     `Host/Verdict.hpp` and `Host/ExclusionList.hpp` are new ARC_API
    //     headers, and `ImageCompareOptions`/`ImageCompareResult` each gained
    //     a member -- all of them consumed by the two HOSTS and ArcaneTests and
    //     by no game module.
    //     MEASURED, not assumed: `grep -rn` for HostConfig, VerifyReport,
    //     ImageCompare, CompareImages, Verdict and ExclusionList over BOTH
    //     game modules in the two trees -- ReferenceProject/Source/
    //     (GameApi.hpp, ReferenceGame.cpp) and Gacha's Game/Source/
    //     (GameApi.hpp, Aphelyon.cpp) -- returns nothing, so no module in
    //     either tree breaks on this bump. The gate catches a stale stamp; the
    //     compiler would not have.
    //     THE SCENE FILE FORMAT DID NOT MOVE: Scene::kSceneJsonVersion stays
    //     at 3. ReferenceProject and Gacha's Game are restamped with this
    //     change, the same precedent v16, v17 and v18 set.
    // v20 (2026-09-03, Arc B -- the host witness harness): the verify-report
    //     surface v19 moved MOVED AGAIN, plus the resolver struct it reads
    //     from. Both clear the v18 entry's "layout or mangled name" bar:
    //       * `ReferenceResolution` (Host/ReferenceImages.hpp, returned BY
    //         VALUE from the exported `ResolveReference`) gained `triedPaths`
    //         (std::vector<std::filesystem::path>) -- the ordered candidate
    //         search space the resolver probed. LAYOUT: a v19 module calling
    //         `ResolveReference` disagrees with the host about the return
    //         struct's SIZE.
    //       * `VerifyReport` (Host/VerifyReport.hpp:134, an ARC_API CLASS)
    //         gained `m_compareTriedPaths`, and `SetCompare` gained a
    //         NON-DEFAULTED twelfth parameter
    //         (`std::vector<std::string> triedPaths`). The same pairing the
    //         v19 entry records: the member is LAYOUT, and the new parameter
    //         moves SetCompare's MANGLED NAME, so a v19 module that called it
    //         fails import resolution at load. The report's JSON went
    //         schemaVersion 4 -> 5 with an added `compare.triedPaths`
    //         (deliberately in the REPORT, not the log -- UE logs its tried
    //         list and discards it). NO existing field changed meaning, so
    //         `kOldestSupportedSchemaVersion` stays 3 and a 3-era consumer
    //         still reads a v5 report correctly.
    //     MEASURED, not assumed: `grep -rn` for SetCompare and
    //     ReferenceResolution over BOTH game modules in the two trees --
    //     ReferenceProject/Source/ (GameApi.hpp, ReferenceGame.cpp) and
    //     Gacha's Game/Source/ (GameApi.hpp, Aphelyon.cpp) -- returns
    //     nothing, so no module in either tree breaks on this bump. The gate
    //     catches a stale stamp; the compiler would not have.
    //     THE SCENE FILE FORMAT DID NOT MOVE: Scene::kSceneJsonVersion stays
    //     at 3. ReferenceProject and Gacha's Game are restamped with this
    //     change, the same precedent v16 through v19 set.
    // v21 (2026-09-04, F2b Task 6 -- the runtime artifact route): `Assets`
    //     (Assets/Assets.hpp, an ARC_CORE_API PURE-VIRTUAL facade) gained
    //     `TextureInfoFor(const Guid&)`, inserted between `SetAssetResolver`
    //     and `PixelsFor` in declaration order -- every virtual AFTER it in
    //     the class MOVES down one vtable slot. This is a LAYOUT change in
    //     the sharpest sense a pure-virtual interface can have: a v20 module
    //     that calls `PixelsFor` (or any of `GetBytes`/`GetJson`/`Stats`, all
    //     declared after the insertion point) through a v21 `Assets*` reads
    //     the WRONG slot and calls the wrong function -- not a link error,
    //     not a crash, a silent wrong-function call through a valid-looking
    //     vtable. The two `PluginABI`-checked entry points (module Init/
    //     Shutdown) never touch `Assets` directly, so a stale module still
    //     LOADS; it is any in-module call through the facade that would
    //     misbehave, which is exactly the class of defect this gate exists to
    //     preempt before it can happen rather than after.
    //     `PixelsFor` ALSO NARROWS ITS CONTRACT on this same bump (documented
    //     fully at its own declaration): for artifact-backed content it now
    //     serves a THUMBNAIL instead of the source's full decode. Not itself
    //     a layout change, but a behavior change riding the same bump -- a
    //     module reading `PixelData::width/height` off `PixelsFor` for
    //     geometry (rather than the new `TextureInfoFor`) would silently get
    //     preview-sized dims instead of the source's true dims. No module in
    //     the tree does this (see the MEASURED line below), but a future one
    //     must reach for `TextureInfoFor`, not `PixelsFor`, for true dims.
    //     MEASURED, not assumed: `grep -rn` for `Assets::`, `->PixelsFor`,
    //     `->TextureInfoFor`, `assets->` and `Assets.hpp` over BOTH game
    //     modules in the two trees -- ReferenceProject/Source/ (GameApi.hpp,
    //     ReferenceGame.cpp) and Gacha's Game/Source/ (GameApi.hpp,
    //     Aphelyon.cpp) -- returns nothing: neither module calls the Assets
    //     facade at all today, so no module in either tree breaks on this
    //     bump. The gate catches a stale stamp; the compiler would not have.
    //     THE SCENE FILE FORMAT DID NOT MOVE: Scene::kSceneJsonVersion stays
    //     at 3. ReferenceProject and Gacha's Game are restamped with this
    //     change, the same precedent v16 through v20 set.
    //
    //     v21 LEDGER EXTENSION (final-review wave, 2026-09-04): F2b landed three
    //     MORE `Assets` changes after Task 6 above, all under this SAME v21
    //     number (no further bump) -- recorded here retroactively so this entry
    //     is the COMPLETE v21 delta, not just what triggered it:
    //       * Task 7: `ArtifactFor(const Guid&)` APPENDED after `Stats()` --
    //         the tail of the class, per the established "new virtuals go at
    //         the end" rule (Batcher2D.hpp's own precedent, cited at
    //         `ArtifactFor`'s own declaration). An append does not reshuffle
    //         any EXISTING slot the way Task 6's mid-class insert did, so it
    //         does not independently demand a bump -- it rides this one.
    //       * Task 12: `InvalidateArtifact(const Guid&)` APPENDED immediately
    //         after `ArtifactFor`, same tail-append shape. Additive and
    //         harmless on its own for the same reason.
    //       * Task 12: `SetArtifactRefusalObserver`/`ContentArtifactRefusalObserved`/
    //         `ContentArtifactRefusalDetail`/`ResetContentArtifactRefusal`
    //         (Assets.hpp) are FREE `ARC_CORE_API` functions, not virtuals on
    //         `Assets` at all -- new exported symbols, never a vtable-layout
    //         concern for any module (stale or otherwise).
    //       * Also Task 12: `JobSystem::Submit(std::function<void()>)`
    //         (JobSystem.hpp) and `Runtime::Jobs()` (Base/Runtime.hpp) are
    //         NON-VIRTUAL additions to PIMPL'd concrete classes -- no vtable
    //         at all on that path, and no change to either class's own
    //         `sizeof` as seen across the DLL boundary. Not part of this
    //         bump's REASON; recorded here only so "what shipped under v21"
    //         is the complete, honest answer.
    //       * Desk-fix 2 (2026-09-05, the cook-pending quiet seam):
    //         `SetCookPendingProbe(std::function<bool(const Guid&)>)`
    //         APPENDED immediately after `InvalidateArtifact` -- the same
    //         tail-append shape as Task 7/Task 12's three appends above, so it
    //         does not independently demand a bump either. Consulted ONLY on
    //         an `ArtifactRefusal::Missing` outcome (never HashMismatch/
    //         VersionNewerThanEngine); absent -- the default for every
    //         non-editor host, and the editor itself before this same arc's
    //         wiring installs one -- preserves the existing loud/memoized/
    //         latched refusal verbatim. See Assets.hpp's own doc comment on
    //         this virtual for the full contract.
    //     MEASURED, not assumed: `grep -rn` for `ArtifactFor`,
    //     `InvalidateArtifact`, `SetArtifactRefusalObserver`,
    //     `SetCookPendingProbe`, `->Jobs()` and `JobSystem` over BOTH game
    //     modules in the two trees -- ReferenceProject/Source/ and Gacha's
    //     Game/Source/ -- returns nothing, so none of these additions break
    //     either module either.
    // v22 (2026-09-06, asset-manager arc, Task 1 -- the Assets panel rebuild):
    //     `Assets` (Assets/Assets.hpp, an ARC_CORE_API PURE-VIRTUAL facade) gained
    //     `MaterialSurfaceFor(const Guid&)`, APPENDED immediately after
    //     `SetCookPendingProbe` -- the tail of the class, the exact "new
    //     virtuals go at the end" shape the v21 ledger extension above
    //     (:482-517) already used for `ArtifactFor`/`InvalidateArtifact`/
    //     `SetCookPendingProbe` themselves.
    //     Checked against the same "does this actually corrupt a mixed
    //     pairing" bar every entry above applies: an append reshuffles no
    //     EXISTING vtable slot, and -- unlike Batcher2D, which a plugin's own
    //     header-only RenderSubmissionSystem dispatches through -- nothing in
    //     a game module ever SUBCLASSES `Assets`. The one concrete class,
    //     `AssetsImpl`, is private to Assets.cpp behind the single
    //     `Assets::Create()` factory; a module only ever CALLS through the
    //     pointer the host hands it, never implements the vtable itself. On
    //     that bar this bump is NOT a corruption finding -- the same honest
    //     call the v17 entry above made for its own all-additive mesh
    //     vocabulary (:319-321) -- and is recorded here as the arc's own
    //     documentation stamp ("a module was built knowing this query
    //     exists"), not dressed up as a safety fix it does not need.
    //     MEASURED, not assumed: `grep -rn` for `MaterialSurfaceFor`,
    //     `Assets::`, `->PixelsFor`, `->TextureInfoFor`, `assets->` and
    //     `Assets.hpp` over BOTH game modules in the two trees --
    //     ReferenceProject/Source/ (GameApi.hpp, ReferenceGame.cpp) and
    //     Gacha's Game/Source/ (GameApi.hpp, Aphelyon.cpp) -- returns
    //     nothing, so no module in either tree breaks on this bump.
    //     THIS ENTRY STAYS OPEN, not closed: Task 2 of the same arc appends a
    //     second virtual, `ListAssetReferences`, immediately after this one,
    //     UNDER THIS SAME v22 NUMBER -- no further bump -- and will extend
    //     this entry the way the v21 entry above was itself extended three
    //     times (Task 7, Task 12 x2, desk-fix 2) without moving its number.
    //     THE SCENE FILE FORMAT DID NOT MOVE: Scene::kSceneJsonVersion stays
    //     at 3. ReferenceProject is restamped with this change, the same
    //     precedent v16 through v21 set (Gacha's Game restamp is this arc's
    //     own follow-up in that repo, tracked there rather than here).
    // Task 2 (same date, same arc): the promise directly above is now
    //     fulfilled. `ListAssetReferences(const Guid&)` is APPENDED
    //     immediately after `MaterialSurfaceFor` -- the new tail of the
    //     class -- returning `std::optional<std::vector<AssetRef>>`.
    //     `AssetRef` and `AssetRefKind` (Assets.hpp) are two NEW BY-VALUE
    //     types this bump introduces: a `std::uint8_t`-backed two-value enum
    //     and a Guid+kind struct. Neither crosses the plugin vtable except as
    //     this one method's return value, so the SAME bar Task 1's paragraph
    //     above applies still holds verbatim: an append reshuffles no
    //     EXISTING vtable slot, and nothing in a game module ever
    //     SUBCLASSES `Assets` (the one concrete class, `AssetsImpl`, stays
    //     private to Assets.cpp behind `Assets::Create()`) -- so this second
    //     append is not an independent hazard, just the same one, twice.
    //     MEASURED, not assumed: `grep -rn` for `ListAssetReferences`,
    //     `AssetRef` and `AssetRefKind` over BOTH game modules in the two
    //     trees -- ReferenceProject/Source/ and Gacha's Game/Source/ --
    //     returns nothing, so this second append breaks neither module.
    //     THIS ENTRY IS NOW CLOSED for v22: Task 3 of the same arc only fills
    //     in `ScanSceneReferences`'s BODY (a free function `ListAssetReferences`
    //     already calls, never itself a virtual), and no remaining task in
    //     this arc's plan (4 through 16) touches the `Assets` vtable, so
    //     nothing further is scheduled to extend this entry.
    // v23 (2026-09-07, asset-manager arc Plan 2, Task 1 -- the scene reference
    //     manifest): THE SCENE FILE FORMAT MOVED. `Scene::kSceneJsonVersion`
    //     (Serialization/SceneSerializer.hpp) goes 3 -> 4, the first format
    //     move since v16 -- and every ledger entry from v17 through v22 above
    //     said in so many words "the scene file format did NOT move", so this
    //     one is the entry that stops saying it.
    //     What v4 adds: a top-level `"assets": [<guid strings>]` reference
    //     manifest, emitted at save time as a byproduct of the reflected-field
    //     walk SaveJson already performs. Nothing a v3 file said changed shape.
    //     WHY THIS IS A BUMP AT ALL, honestly stated: there is NO vtable, NO
    //     layout and NO API change here -- `ReflectionJsonWriter` gained a
    //     DEFAULTED trailing constructor parameter, and nothing else that
    //     crosses the boundary changed at all. Both classes are header-only and
    //     compiled into each module from source, so a mixed pairing cannot
    //     disagree about them the way it could about a real vtable. The bump is
    //     DOCUMENTARY: it is the v16 precedent (a scene format move IS an
    //     engine-version fact a project's `engine.abi` should record) plus the
    //     standing "ABI bumps are cheap during engine dev" rule, and it is
    //     recorded as exactly that, not dressed up as a corruption fix.
    //     BACKWARD COMPATIBLE, deliberately, and this is the part that makes
    //     the bump safe rather than merely cheap: both scene load gates --
    //     `LoadJson` (SceneSerializer.hpp) and `ReadSceneFile`
    //     (SceneAsset.hpp) -- widened from an equality test to the range
    //     [`kSceneJsonVersionMin` = 3, `kSceneJsonVersion` = 4], so every v3
    //     scene already on disk still loads under an ABI-23 engine. v1/v2 stay
    //     refused (their Transform shape really is incompatible). The entity
    //     clipboard's gate (Edit::InstantiateSubtrees) stays an EQUALITY test
    //     on purpose -- a clipboard payload is written and read by one running
    //     build and has no old corpus to be compatible with.
    //     MEASURED, not assumed: `grep -rn` for `SaveJson`, `SceneSerializer`,
    //     `SaveSceneFile`, `kSceneJsonVersion`, `SerializeSubtrees`,
    //     `LoadJson`, `ReadSceneFile`, `IsIdentityGuidFieldName` and
    //     `ReflectionJsonWriter` over BOTH game modules in the two trees --
    //     ReferenceProject/Source/ (GameApi.hpp, ReferenceGame.cpp) and Gacha's
    //     Game/Source/ (GameApi.hpp, Aphelyon.cpp) -- returns nothing, so no
    //     module in either tree touches the serializer this bump is about.
    //     ReferenceProject is restamped with this change, the precedent v16
    //     through v22 set (Gacha's Game restamp is this arc's follow-up in that
    //     repo, tracked there rather than here).
    // v24 (2026-09-10): Astra re-vendored to dev fa0900a -- the meta binder
    //     scoping program (Astra spec 2026-09-09). Same failure class as v10:
    //     ComponentRegistry's LAYOUT changed (the per-slot MetaBuildFn thunk
    //     array became a per-slot ModuleToken array; ShadowEntry carries a
    //     token instead of a thunk) and MetaRegistry's entries grew a binder
    //     stack, so a v23 plugin's inlined template code (RegisterComponent /
    //     ComponentModule::Register are header templates instantiated IN the
    //     plugin) would manipulate a registry whose layout it does not know.
    //     Reject the pairing. Public surface is source-compatible: Open /
    //     Register / RegisterMeta / Reset keep their signatures; SetTypeContext
    //     gained a DEFAULTED ModuleResidency parameter. Arcane itself calls none
    //     of the changed internals (verified 2026-09-09), so this is a header
    //     sync + bump with no Arcane logic change. What the sync buys: several
    //     ComponentRegistry instances per TypeContext are now a supported shape
    //     (meta erased only when no registry, handle, or resident module holds
    //     it), which lifts the precondition that kept the engine roster on
    //     anonymous registration (Runtime.cpp) -- whether to move it to a
    //     Runtime-owned ComponentModule under SetTypeContext(ctx, Resident) is
    //     a separate decision that re-opens the 2026-08-10 ratification.
    //     ReferenceProject restamped with this change, per the v16+ precedent
    //     (Gacha's Game restamp is tracked in that repo).
    // v25 (2026-09-10, F2c Task 11 -- CPU resolution of an imported mesh): `Assets`
    //     (Assets/Assets.hpp, an ARC_CORE_API PURE-VIRTUAL facade) gained THREE virtuals,
    //     `MeshArtifactFor(const Guid&)`, `InvalidateMeshArtifact(const Guid&)` and
    //     `CookPending(const Guid&) const`, all APPENDED immediately after
    //     `ListAssetReferences` -- the tail of the class, the exact "new virtuals go at
    //     the end" shape the v21/v22 entries above already used for `ArtifactFor`/
    //     `InvalidateArtifact`/`SetCookPendingProbe` and `MaterialSurfaceFor`/
    //     `ListAssetReferences` themselves.
    //     Checked against the same bar every append-only entry above applies: an append
    //     reshuffles no EXISTING vtable slot, and `AssetsImpl` -- the one concrete class
    //     implementing this interface -- stays private to Assets.cpp behind the single
    //     `Assets::Create()` factory, so a module only ever CALLS through the pointer the
    //     host hands it, never subclasses the vtable itself (the v22 entry's own framing
    //     for why this class's appends are not a corruption finding).
    //     TWO NEW BY-VALUE TYPES ride along (Assets/ArtifactReader.hpp): `LoadedClientMesh`
    //     (vertices/indices/sections/aabb -- the mesh half of `LoadedClientArtifact`) and
    //     `MeshSectionView` (name/indexOffset/indexCount/slotIndex). Neither crosses the
    //     plugin vtable except as `MeshArtifactFor`'s own return pointer -- the SAME "by
    //     value, never a layout hazard on its own" bar the v22 entry applied to `AssetRef`/
    //     `AssetRefKind`.
    //     THE SCHEMA FACT RIDING ALONG (already shipped, one commit earlier, at 01ad0da7 --
    //     recorded here for completeness, not itself the reason for this bump): `.arcmesh`
    //     grew `slots[]`/`importedSource` and `MeshSource::Imported = 5` (Task 10).
    //     BACKWARD COMPATIBLE: the legacy scalar `"material"` key is still read (tolerantly
    //     mapped to one unnamed slot) whenever `slots` is absent, so every v3-era `.arcmesh`
    //     on disk still loads under a v25 engine with no migration.
    //     MEASURED, not assumed: `grep -rn` for `MeshArtifactFor`, `InvalidateMeshArtifact`,
    //     `CookPending`, `LoadedClientMesh`, `MeshSlot`, `MeshSection` and `ResolveMeshData`
    //     over BOTH game modules in the two trees -- ReferenceProject/Source/ (GameApi.hpp,
    //     ReferenceGame.cpp) and Gacha's Game/Source/ (GameApi.hpp, Aphelyon.cpp) -- returns
    //     NOTHING in either tree:
    //
    //       $ grep -rn -E "MeshArtifactFor|InvalidateMeshArtifact|CookPending|LoadedClientMesh|MeshSlot|MeshSection|ResolveMeshData" ReferenceProject/Source/
    //       (no output)
    //       $ grep -rn -E "MeshArtifactFor|InvalidateMeshArtifact|CookPending|LoadedClientMesh|MeshSlot|MeshSection|ResolveMeshData" <Gacha checkout>/Game/Source/
    //       (no output)
    //
    //     so no module in either tree breaks on this bump. The gate catches a stale stamp;
    //     the compiler would not have.
    //     THIS BUMP IS 24 -> 25, NOT 23 -> 24: the v24 entry directly above (an Astra
    //     vendor sync, unrelated to this arc) already consumed 24 by the time this task
    //     landed -- ABI bumps are cheap during engine dev, so the fix is simply to take
    //     the next number, not to contest which change "deserved" 24.
    //     ReferenceProject.arcproj is restamped with this change, the same precedent v16
    //     through v24 set. Gacha's Game restamp is that repo's own follow-up, tracked
    //     there rather than here -- the grep evidence above is what proves it is safe to
    //     defer, not evidence it was done.
    // v26 (2026-09-11): Astra re-vendored to dev at the change-detection merge
    //     (feat/change-detection, 18 commits over f3e311d incl. the S1
    //     load-robustness fix + the adoption's two primitives). SAME FAILURE
    //     CLASS AS v10/v24: plugins compile Astra's headers THEMSELVES, and
    //     this diff moves in-memory layouts their
    //     inlined templates index -- ArchetypeChunkPool's `Column` gained
    //     `EntityTicks* ticks` beside `disabledWords`; `ArchetypeColumnMeta`
    //     gained `trackedColumns[MAX_COMPONENTS]` + `trackedColumnCount` (128
    //     slots here: Arcane defines no ASTRA_MAX_COMPONENTS); the chunk arena
    //     now carves a per-column `Tick` version region and per-entity tick
    //     columns, shifting component-array offsets; `EntityLocation` became an
    //     aggregate; and the whole View/Query/ViewIterator ForEach path (which
    //     TransformSystems.hpp / RenderSystems.hpp instantiate INSIDE
    //     ReferenceGame.dll and Aphelyon.dll) now stamps chunk versions on
    //     entry. A v25 plugin under a v26 host would read component arrays at
    //     stale offsets and never stamp. Reject the pairing.
    //     TWO ARCANE FACTS RIDE ALONG. (1) Residency (Plan 1 Task 3): Runtime
    //     now calls SetTypeContext(ctx, ModuleResidency::Resident) and owns the
    //     engine roster through a Runtime-held Astra::ComponentModule "Arcane"
    //     -- the decision the v24 entry deferred, taken 2026-09-11 (spec
    //     docs/specs/2026-09-11-astra-adoption-design.md s5). Reverses the
    //     2026-08-10 ratification: its blocking caveat ("one registry per
    //     context") is gone from the vendored headers, and a Resident binder is
    //     pinned so the last Runtime's Reset reports Retained, never Erased.
    //     Plugins, hosts and the test exe keep the one-arg (Transient) call.
    //     (2) A TRACKED TYPE (Plan 1 Task 5): Transform declares
    //     `static constexpr bool AstraChangeTracked = true` -- a static member,
    //     so sizeof/reflect block/serialized bytes are unchanged and it needs
    //     no bump of its own; it is recorded here because it is a header change
    //     compiled into every plugin, and a non-const view over Transform now
    //     yields Astra::Mut<Transform> (source-compatible via the implicit T&).
    //     MEASURED, not assumed: `grep -rn -E` for every compile-affecting
    //     change in the diff -- EntityLocation, AddEntity, BatchAddEntities,
    //     ViewIterable, ViewIterator, Deserialize(, ComponentDescriptor,
    //     ArchetypeColumnMeta, SystemContext(, ISystemExecutor,
    //     ComponentUpdated, CreateView<, GetComponent<, GetRelations,
    //     ModuleResidency, AstraChangeTracked, Changed<, Modified(, IsChanged
    //     -- over BOTH game modules in the two trees returns NOTHING:
    //
    //       $ grep -rn -E "EntityLocation|AddEntity|BatchAddEntities|ViewIterable|ViewIterator|Deserialize\(|ComponentDescriptor|ArchetypeColumnMeta|SystemContext\(|ISystemExecutor|ComponentUpdated|CreateView<|GetComponent<|GetRelations|ModuleResidency|AstraChangeTracked|Changed<|Modified\(|IsChanged" ReferenceProject/Source/
    //       (no output)
    //       $ grep -rn -E "<same pattern>" D:/dev/starworks/Gacha/Game/Source/
    //       (no output)
    //
    //     Both modules name only Astra::{BinaryReader, BinaryWriter, Entity,
    //     Registry, SetTypeContext} (+ ComponentModule in a ReferenceGame
    //     comment) and AddSystem<TransformPropagationSystem/RenderSubmission
    //     System> (the pair the engine registers itself since v29) -- header-only
    //     systems whose bodies change under them, which
    //     is exactly why the gate, not the compiler, is what refuses a stale DLL.
    //     ReferenceProject.arcproj restamped with this change, per the v16+
    //     precedent. Gacha's Game restamp (21 -> 26) is Plan 1's LAST task, in
    //     that repo, together with the Aphelyon.dll rebuild -- not deferred.
    // v27 (2026-09-11, Astra adoption Plan 2): PreviousTransform DELETED --
    //     struct, reflect block, its RegisterSceneComponents slot and its slot
    //     in Runtime's Resident ComponentModule roster -- together with LerpPose
    //     (Components.hpp). Sprite render interpolation now reads the physics
    //     side's PhysicsInterpBuffer through its new entity->slot map
    //     (RenderSubmissionSystem, spec docs/specs/2026-09-11-astra-adoption-
    //     design.md s8 -- the map rather than the spec's Optional<PhysicsBodyRef>
    //     term, because PhysicsComponents.hpp drags Manifold2D headers a game
    //     module's include surface does not carry; controller ruling 2026-09-11),
    //     and PhysicsSystem's PASS 4 no longer stashes a previous local pose.
    //     WHY A BUMP: every plugin compiles Components.hpp and SceneResources.hpp
    //     (via RenderSystems.hpp) and so (a) bakes the reflect roster into its
    //     own drained baselines, (b) inlines PhysicsInterpBuffer's LAYOUT (it
    //     grew an Astra::FlatMap member), and (c) instantiates
    //     RenderSubmissionSystem's body -- which reads that resource -- INSIDE
    //     the DLL; and the roster's first-touch ids after the deleted slot shift
    //     by one. A v26 DLL under a v27 host would register a roster the host no
    //     longer knows and read a resource the host lays out differently. Reject
    //     the pairing. The v16 entry above still names PreviousTransform: that is
    //     history, and stays. The game-module include surface (build/arcane.lua)
    //     is UNCHANGED.
    //     MEASURED, not assumed: `grep -rn -E "PreviousTransform|LerpPose|
    //     PhysicsBodyRef|PhysicsInterpBuffer|InterpPose|InterpSlot"` over BOTH game modules
    //     -- ReferenceProject/Source/ and Gacha's Game/Source/ -- returns NOTHING
    //     in either tree, so neither breaks at compile time; the gate, not the
    //     compiler, refuses the stale DLL.
    //
    //       $ grep -rn -E "PreviousTransform|LerpPose|PhysicsBodyRef|PhysicsInterpBuffer|InterpPose|InterpSlot" ReferenceProject/Source/
    //       (no output)
    //       $ grep -rn -E "<same pattern>" D:/dev/starworks/Gacha/Game/Source/
    //       (no output)
    //
    //     ReferenceProject.arcproj restamped with this change. Gacha's Game
    //     restamp (26 -> 27) is that repo's own follow-up, tracked there -- the
    //     grep is what proves it safe to defer, not evidence it was done.
    // v28 (2026-09-11, 2D physics wiring Plan 1): Astra re-vendored at 2e691c2
    //     (dev, one commit over a08bb04): FieldInfo grew std::vector element
    //     access -- elementTypeHash, elementSize and five std::function
    //     accessors (vectorSize / vectorResize / vectorElement / vectorErase /
    //     vectorInsert) -- so sizeof(FieldInfo) and every reflect block's
    //     static-init shape moved. SAME FAILURE CLASS AS v10/v24/v26: plugins
    //     compile Astra's reflect macros THEMSELVES (Components.hpp's blocks
    //     are instantiated inside ReferenceGame.dll and Aphelyon.dll), so a v27
    //     plugin would hand the host FieldInfo records laid out for the old
    //     struct. Reject the pairing.
    //     FOUR ARCANE FACTS RIDE ALONG, all from spec docs/specs/2026-09-11-
    //     physics-2d-wiring-design.md and landing across this plan's tasks
    //     (this entry is written at the vendor task, as v26's was):
    //     (1) a new engine roster component, Arcane::PhysicsSettings
    //     {glm::vec2 gravity} (Components.hpp), APPENDED after MeshRenderer in
    //     RegisterSceneComponents and in Runtime's Resident roster, so no id
    //     before it shifts; (2) Collider2D and RigidBody2D declare
    //     AstraChangeTracked = true (static members -- no byte change; recorded
    //     per the v26 precedent for Transform); (3) PhysicsSystem is
    //     schedulable -- RequiresExclusive, Astra::Before<TransformPropagation
    //     System>, PASS 4 gated on stepWorld, paused-pass re-mint criteria in
    //     PASS 1 -- and Runtime installs it into fixedUpdate ITSELF: no game
    //     module names it, and PhysicsSystem.hpp / PhysicsComponents.hpp are
    //     NOT on the game-module include surface (build/arcane.lua carries no
    //     Manifold2D row); (4) the reflection->JSON bridge gained a container
    //     branch and Collider2D::fixtures lost Serializable(false) (scene
    //     schema v5). The game-module include surface (build/arcane.lua) is
    //     UNCHANGED.
    //     MEASURED, not assumed: `grep -rn -E "FieldInfo|isVector|
    //     PhysicsSettings|PhysicsSystem|PhysicsResource|Collider2D|RigidBody2D|
    //     PhysicsBodyRef|EnsurePhysics|InstallEngineSystems|PhysicsEditPass"`
    //     over BOTH game modules -- ReferenceProject/Source/ and Gacha's
    //     Game/Source/ -- returns NOTHING in either tree:
    //
    //       $ grep -rn -E "FieldInfo|isVector|PhysicsSettings|PhysicsSystem|PhysicsResource|Collider2D|RigidBody2D|PhysicsBodyRef|EnsurePhysics|InstallEngineSystems|PhysicsEditPass" ReferenceProject/Source/
    //       (no output)
    //       $ grep -rn -E "<same pattern>" D:/dev/starworks/Gacha/Game/Source/
    //       (no output)
    //
    //     so neither breaks at compile time; the gate, not the compiler,
    //     refuses the stale DLL. ReferenceProject.arcproj restamped with this
    //     change. Gacha's Game restamp (27 -> 28) is this plan's Task 10, in
    //     that repo, together with the Aphelyon.dll rebuild -- not deferred.
    // v29 (2026-09-13, game-module boilerplate): the ENGINE now registers
    //     TransformPropagationSystem (fixedUpdate) and RenderSubmissionSystem
    //     (render) in Runtime::InstallEngineSystems beside PhysicsSystem; a
    //     game module registers ONLY its own systems and places them with
    //     Astra::Before/After. Export names, signatures and EngineContext are
    //     UNCHANGED -- a v28 module's own AddSystem<> of the pair returns
    //     AlreadyRegistered (std::ignore'd, harmless), so nothing breaks at
    //     load; the bump makes the contract change visible through the gate
    //     instead of letting it pass by accident. Arcane/Plugin/GameModule.hpp
    //     (ARC_GAME_MODULE) is the SDK face of the same contract.
    //     ReferenceProject.arcproj restamped with this change; Gacha's Game
    //     restamp (28 -> 29) is this plan's Task 4, in that repo, with the
    //     Aphelyon.cpp conversion -- not deferred.
    // v30 (2026-09-15, Core-DLL split): headers a module compiles moved DLLs
    //     (Base/Scene/Plugin/Project/Serialization/Sim/... now export from
    //     ArcaneCore.dll, ARC_CORE_API; a module links BOTH import libs, build/
    //     arcane.lua); EngineContext gained process, client and netMode -- exactly
    //     three, and NOT tail-appended: they sit BETWEEN `engine` and the four
    //     ImGui void*s (see the struct below), so the ImGui block's offsets move
    //     as well as the struct's size; and the module contract gained system-factory
    //     registration with role masks (GameModule::RegisterSystem, spec s4) --
    //     each Runtime instantiates what its NetMode matches. A v29 module under a
    //     v30 host would read EngineContext at the old size AND read its ImGui
    //     handoff at the wrong offsets, and would register no factories. Reject
    //     the pairing. ReferenceProject.arcproj restamped with
    //     this change; Gacha's Game restamp (29 -> 30) is this plan's Task 5 Gacha
    //     commit, with the Aphelyon.dll rebuild -- not deferred.
    // v31 (2026-09-16, Astra type-identity discriminator): the vendored Astra
    //     moved to `b8291b9`, whose load-bearing commit `056063c` REPLACED the last
    //     field of Astra::TypeIdentity -- `const std::type_info* rtti` became
    //     `uint64_t rttiName`, a hash of the RTTI mangled name. Same offset, same
    //     struct size, so nothing about the layout announces the change; but
    //     MakeTypeIdentity<T>, GetOrAssignComponentID and IsTypeIdentityCollision
    //     are header-inline and therefore instantiated in EVERY module, so a module
    //     built against the OLD header hands the SHARED TypeContext a pointer where
    //     the host stores a hash, and its own inline collision check dereferences
    //     the host's hash as a type_info*. OBSERVED: a v30 ReferenceGame.dll built
    //     before the vendoring, under a host built after it, failed the E1 editor
    //     witness at EditorWitnessTest.cpp:32 (GradeProcessFacts); rebuilding the
    //     module against the new header passed. Reject the pairing -- the mismatch
    //     is a wild dereference, not a degraded feature. (The fix itself removes the
    //     mirror-image hazard: the old pointer field dangled once the image that
    //     FIRST resolved a type unloaded, which is exactly the plugin case --
    //     pinned by PluginHostTest's [hotreload][typecontext] scenario.)
    //     ReferenceProject.arcproj restamped with this change; Gacha's Game restamp
    //     (30 -> 31) is the paired commit in that repo, with the Aphelyon.dll
    //     rebuild -- not deferred.
    // v32 (2026-09-17, F4 plan 1): ClientRuntime::SetCamera(vec2,float)/CameraOffset/
    //     CameraZoom REMOVED in favour of SetView(const ViewTransform&)/View();
    //     RenderContext2D carries a ViewTransform; PickView/GizmoView carry Affine2D.
    //     A module built against v31 calls a removed export -- refuse. Also
    //     recorded under this bump: the scene file format moved v5 -> v6 (+Y up;
    //     a <=v5 file migrates on load) in Task 2, which landed without a bump of
    //     its own (a data-format change, not a shape change). ReferenceProject.arcproj
    //     restamped with this change.
    //     Task 4, same bump: Batcher2D gained QuadWorld/CircleWorld/SetViewProjection APPENDED at the end of its vtable, and Batch2DVertex::pos is vec3 (36-byte wire vertex; Batch2DDrawSpan::worldSpace, Batch2DDrained::viewProjection).
    // v33 (2026-09-17, F4 plan 2): Edit/Gizmo.hpp is 3D. GizmoTransform is
    //     { vec3, quat, vec3 }; GizmoView is REMOVED (HitTest/Draw/ApplyDrag take
    //     a ViewTransform, a GizmoHandleMask and the size scale); GizmoAxis gained
    //     Z/XY/YZ/XZ/Screen; DecomposeTRS/ComposeTRS/MakeGroupDelta/ApplyGroupDelta
    //     changed signature; IsPlanarBasis is REMOVED (its whole reason -- a planar
    //     decomposition demoting through a tilted parent -- no longer exists);
    //     WithMirrorOn, WorldUnitsPerPixel, ClosestLineParam, RayPlane are NEW.
    //     PickView (Render/PickEmit.hpp) is REMOVED and PickDrawable is world-space
    //     (same bump, plan 2 Task 1). A module built against v32 references removed
    //     exports -- refuse. ReferenceProject.arcproj restamped.
    //     Also under this bump (plan 2 Task 1): CollectPickables(Registry&,
    //     std::vector<PickDrawable>&) lost its view parameter; PickIdVertex grew
    //     32 -> 36 bytes (vec3 pos); NriGraphContext::FrameDesc gained
    //     ViewTransform pickView; RgPickHandles gained depth; PickNode::
    //     PrepareDrawables/Record changed signature; GizmoHandleMask is NEW
    //     (Task 2).
    // v34 (2026-09-18, F4 plan 2 desk pass): Edit/Gizmo.hpp's Draw takes a
    //     GizmoDrawSink (a NEW exported pixel-sink interface) instead of a
    //     Batcher2D -- the gizmo is painted as host FOREGROUND chrome over the
    //     finished frame (Unreal's SDPG_Foreground) rather than into the scene
    //     batch the mesh pass overpaints. A module built against v33 references
    //     the removed overload -- refuse. ReferenceProject.arcproj restamped.
    // v35 (2026-09-18, F4 plan 2 desk pass, UE parity): GizmoDrawSink gained
    //     Circle; Draw gained a trailing `const GizmoRotateSweep*`; RotateSweep
    //     and GizmoRotateSweep are NEW. A module built against v34 calls the old
    //     Draw signature -- refuse. ReferenceProject.arcproj restamped.
    // v36 (2026-09-18, F3 plan 1): Scene/Components.hpp gains WorldBounds (a
    //     new engine component; RegisterSceneComponents and the
    //     EngineComponentRoster append it after PhysicsSettings), WorldTransform
    //     and MeshRenderer become AstraChangeTracked (their reflected layout is
    //     unchanged, their column ticks are not); Scene/SpriteGeometry.hpp is
    //     new in Core; Scene/BoundsSystem.hpp is NEW (the engine's third
    //     fixedUpdate system). A module built against v35 registers one
    //     component fewer -- refuse. ReferenceProject.arcproj restamped.
    // v37 (2026-09-19, F3 plan 1 review fix): Scene/SceneResources.hpp's
    //     MeshTable and SpriteTable -- registry resources read by
    //     plugin-compiled header-only code -- each gain a trailing
    //     `const std::uint64_t* generation` (the owning cache's publish
    //     counter; BoundsSystem re-walks every drawable when it moves), and
    //     ClientRuntime::SetMeshTable/SetSpriteTable gain the matching
    //     defaulted parameter. A module built against v36 reads an 8-byte
    //     resource the host publishes as 16 -- refuse. ReferenceProject.arcproj
    //     restamped.
    // v38 (2026-09-19, F3 plan 2): ResolvedMeshMaterial (a Core resource
    //     visible to game modules) gains blend/alphaCutoff/twoSided, and
    //     MeshRenderer gains translucencyRenderOrder/translucencyDepthSortBias.
    //     A v37 module reads either header-defined layout at stale offsets;
    //     reject the pairing. ReferenceProject.arcproj restamped.
    // v39 (2026-09-22, crash window plan 1): ArcaneCore.dll gains the whole
    //     crash-window export surface, and Diagnostics::Config gains five
    //     fields (productName/unattended/commandLine/logDir/exitSeconds) that
    //     change the size and layout of a struct a module's own headers can
    //     construct. New exports across tasks 1-9: the crash thread and
    //     SubmitReport/ReportRequest, the fail-fast family (assert, ensure,
    //     terminate, abort, invalid parameter, pure call, stack overflow), the
    //     exit sentinel's RequestCleanExit/SetCleanExitHook/SimulateConsoleCtrl,
    //     the log file sink, and GuaranteeStackForThisThread. A v38 module was
    //     compiled against a Core with neither; reject the pairing.
    //     ReferenceProject.arcproj restamped.
    // v40 (2026-09-23, crash window plan 2): layout and export changes a
    //     module's own headers can see. Diag::Envelope gains `std::string
    //     reason` (D2) -- the type crosses ArcaneCore.dll, so its size and
    //     member offsets moved. Diagnostics::Config gains `launchMonitor`
    //     (D4, monitor mode), changing the layout of a struct a module can
    //     construct. HostConfig gains `hangMainFrame` (D10, the dev-only hang
    //     trigger) -- another `ARC_API` struct layout move, same house
    //     convention as the v18/v19 entries. ArcaneCore.dll gains the
    //     NativeWindow surface (NativeWindow, INativeWindowPresenter,
    //     NativeWindowDesc incl. `dialogNavigation`).
    //     ProgressStallRule::WasReported() is inline and adds no export. A
    //     v39 module was compiled against the old layouts; reject the
    //     pairing. ReferenceProject.arcproj restamped.
    // v41-v43 (2026-09-28, reference platformer): three stamps that landed in
    //     one unpushed checkpoint. ArcaneCore.dll exports GetBodyMotion2D and
    //     SetBodyVelocity2D (Scene/PhysicsCommands.hpp, the game-module-safe
    //     seam over the PhysicsWorld linked inside Core) and the reflected
    //     sample component PlayerController2D gains movement tuning and
    //     transient jump state. The two exports v41/v42 had named
    //     (SetBodyHorizontalVelocity, TryJumpBody) never gained a caller and
    //     were removed at v44, before the push.
    // v44 (2026-09-28, project input actions; the hygiene pass): the
    //     module-visible input surface moved. `class ARC_API InputActions`
    //     gained 21 pure virtuals MID-VTABLE (LoadAsset, SetControlScheme, the
    //     ID-keyed FindAction/Value/Phase/Pressed/... family, SetBindingPath,
    //     the Maps/Actions/Bindings enumeration, BeginFixedStep and the
    //     *ThisFixedStep queries); RuntimePresentation, a by-value member of
    //     `ARC_API ClientRuntime`, gained `LocalInputUser gameInput`, moving
    //     every later member; ArcaneClient.dll exports LocalInputUser,
    //     InputActionAsset, InputBindingProfile, InputRebindOperation,
    //     ClientRuntime::GameInput and HostBoot::LoadGameplayInput;
    //     ArcaneCore.dll exports Runtime::SetProjectInputActionsAsset, and the
    //     .arcproj manifest gained `inputActions`. A v43 module was compiled
    //     against the old vtable and layout; reject the pairing.
    //     ReferenceProject.arcproj restamped.
    // v45 (2026-09-28, inspector-ownership/input-editor arc): `InputActions`
    //     gained the pure virtual `BindingValue`; `InputRebindOperation`
    //     gained `heldModifiers_`; `LocalInputUser` (held by value inside
    //     `ARC_API ClientRuntime`) gained `mapStack_`/`scheme_`. A v44
    //     module was compiled against the old vtable and layouts; reject the
    //     pairing. ReferenceProject.arcproj restamped. Also in 45 (plan T12):
    //     `HostConfig` gained `selectInDocument` (--select-in-document), and
    //     `VerifyReport` gained `m_inspectorSet`/`m_inspectorSource`/
    //     `m_inspectorBreadcrumb` behind the newly exported `SetInspector`
    //     (report schemaVersion 11) -- both layouts moved.
    // v46 (2026-09-29, inspector filters): `HostConfig` gained `selectAsset`
    //     (--select-asset), and `VerifyReport` gained `InspectorInstance` /
    //     `m_inspectorInstances` behind a new `SetInspector(source, breadcrumb,
    //     instances)` signature (report schemaVersion 12) -- both layouts moved.
    //     ReferenceProject.arcproj restamped; the Aphelyon restamp is owed.
    // v47 (2026-10-01, node page + editor upgrades T1 correctness): `ICommand`
    //     gained the virtuals `AffectsScene`, `IsExpired` and `PayloadBytes`
    //     (vtable); `CommandStack` gained `UndoLimits`/`SetLimits` (the depth
    //     ctor parameter is gone), `Clear(reason)`/`ClearedReason`,
    //     `SceneStateId` and spill state (API and layout moved), beside the
    //     newly exported `UndoPayload`/`Detail::UndoSpillFile`;
    //     `RegistryStateCommand` holds `UndoPayload`s (layout); vendored Astra
    //     975cdb7: `Registry` gained `m_instanceId` (layout), `SetParent`
    //     returns `bool` and takes an index, exhausted entity slots retire
    //     instead of wrapping and `Clear()` recycles (allocator rules both
    //     sides must agree on); `VerifyReport` schemaVersion 13
    //     (`documents[]`). A v46 module was compiled against the old vtables
    //     and layouts; reject the pairing. ReferenceProject.arcproj and
    //     Aphelyon.arcproj restamped.
    // v48 (2026-09-30, node page T3): `GraphNodeTypeInfo` gained the appended
    //     `description` column (an exported Core struct returned by reference
    //     from GraphNodeInfo/AllGraphNodeInfos -- its layout moved), and Core
    //     exports `GraphPinNeutralDefault` (same tranche, same bump; s5.1.8).
    //     ReferenceProject.arcproj and Aphelyon.arcproj restamped; both game
    //     modules rebuilt.
    // v49 (2026-09-30, node page phase T5 -- asset file operations, s7.2):
    //     ArcaneCore.dll exports AssetRegistry::PeekId/Remove/Rebind (+ the
    //     RebindResult enum), Project::UnregisterAsset/RebindAsset and
    //     Runtime::UnregisterAsset/RebindMovedAsset; the `Assets` facade APPENDED
    //     the pure virtuals EvictPath and ForgetUnresolved (no existing slot moved,
    //     but the vtable grew). The same stamp also covers two T3-D6 host
    //     changes that landed after v48 (T4-GATE ruling, option a): `HostConfig`
    //     gained `windowWidth`/`windowHeight` (--window-size, 6d6c1008), a layout
    //     move like the v40/v45/v46 HostConfig fields, and `Window::Show()`
    //     became `Show(bool activate = true)` (f35fd5b4), which moves the mangled
    //     name (the v18 reasoning). A v48 module was compiled against the old
    //     surface; reject the pairing. ReferenceProject.arcproj and
    //     Aphelyon.arcproj restamped.
    // v50 (2026-09-30, node-page phase T6): `ConsoleModel` gained history
    //     (`m_history`/`m_draft`/`m_historyPos`) and `CompleteInput`/`HistoryPrev`/
    //     `HistoryNext`; `CVarRegistry::ListCommands` and `Log::SetLevel` are new
    //     ArcaneCore exports; ArcaneClient exports `DrawConsoleInputLine`. Every
    //     registry now registers `console.historySize`, and Log::Init registers
    //     `log.level`. ReferenceProject.arcproj and Aphelyon.arcproj restamped.
    // v51 (2026-10-03, input and time as resources -- input-seam spec s9): the
    //     one bump for the input seam. `LocalInputUser` (held by value inside
    //     `ARC_API ClientRuntime`) gained `generation_` (layout). Vendored
    //     Astra 0743077 + c46a90e: `ComponentDescriptor` gained
    //     `isTransientResource` -- a layout change in a header every module
    //     compiles and both sides of the boundary build descriptors from;
    //     param systems take ordering traits and are keyed by their own functor
    //     type (scheduler keying and ordering both sides must agree on); an
    //     `AstraTransientResource` is never serialized, so a snapshot written by
    //     one side no longer carries the resources the other side expects to
    //     republish. RunLoop publishes `Arcane::Time` and ClientRuntime
    //     publishes `Arcane::GameInput` (+ `ActionRef`) as registry resources;
    //     `Arcane::Physics2D` (= the published PhysicsResource) exports
    //     `Motion`/`SetVelocity`; `Scene/PhysicsCommands.hpp` is deleted with its
    //     exports `GetBodyMotion2D`/`SetBodyVelocity2D` (v41-v43), and the
    //     reflected sample component PlayerController2D lost its copied input
    //     and time fields. A v50 module was compiled against the old layouts and
    //     exports; reject the pairing. ReferenceProject.arcproj and
    //     Aphelyon.arcproj restamped.
    // v52 (2026-10-04, settings arc S1 -- the cvar core):
    //   - Layouts:
    //     - CVarValue's variant grew (Color, Vec2/3/4, Enum);
    //     - CVarDesc gained displayName, keywords, widget, audience, scope,
    //       apply, order, categoryPath and enumNames;
    //     - Plugin owns its cvar module (a new member).
    //   - Signatures:
    //     - Permission is gone: CVarRegistry::Set/Execute, ConsoleModel::Submit,
    //       ApplyCVarCommandLine, ApplyCVarDirectory and ArcaneClient's
    //       DrawConsoleInputLine take a CVarContext (+ an optional CVarCaller);
    //     - HostBoot::BootContext::cvarPermission became cvarContext;
    //     - CommandFn returns CommandResult.
    //   - New ArcaneCore exports:
    //     - CVarRegistry::Snapshot, PublishImmediate, ApplyLayersFor,
    //       SetPolicy, SetAuditSink, RegisterAlias, ModuleOf, IsCompiledOut,
    //       ScopedModule and CurrentModule;
    //     - CVarModuleScope, ValidateCVarLayers, PublishCVarConfigDiagnostics;
    //     - Runtime::CVarLayerSources and SetCVarCommandLine;
    //     - Arcane::Paths;
    //     - Module::IsMapped (Plugin::Load no longer drops a live module's
    //       registrations when it refuses an already-mapped image).
    //   - Declarations: the positional ARC_CVAR and ARC_CVAR_RANGED forms are
    //     removed, and every module registers under its own ARC_MODULE_NAME.
    // A v51 module was compiled against the old layouts and signatures; reject
    // the pairing. ReferenceProject.arcproj and Aphelyon.arcproj restamped.
    // v53 (2026-10-03, settings arc S2): settings structs. CVarSnapshot gained
    //     `settings` (typed blocks, read INLINE by Settings<T>/SettingsShared<T>),
    //     and CVarExplain gained audience/scope/apply (returned by value). New
    //     ArcaneCore exports reached from inline headers: CVarRegistry::
    //     RegisterSettings/SettingsBlock/ClearRung (ARC_SETTINGS), the
    //     Log::*MosaicLevel* family (InstallMosaicSink and GameModule::Shutdown
    //     now call them), WriteCVarArchive(dir, SetBy), the PreferenceScope API,
    //     ResolveWorkerThreads, ApplyLogSettings and Runtime::SetEditorUserConfigDir.
    //     ARC_REFLECT_FIELD routes through Arcane::Detail::ReflectField. Also
    //     covers S3: CVarDesc gained `group` (passed through CVarRegistry::
    //     Register); S3-GATE deferred its bump here. A v52 module was compiled
    //     against the old layouts and lacks the new imports; reject the pairing.
    //     ReferenceProject.arcproj and Aphelyon.arcproj restamped.
    // v54 (2026-10-07, settings arc S7 -- players and server, spec s8, s9; one
    //     bump for every exported change since v53, S4..S7 -- carry ruling 05:03):
    //   - S7: `ProjectManifest` gained `company` (an exported Core struct a
    //     module reads through Project::Manifest() -- its layout moved).
    //     ArcaneCore exports PlayerSettings (List/Set/SetSessionMode/
    //     SessionMode/SessionContext), CVarContextFor, CVarRegistry::ListEx (+
    //     the CVarListEntryEx struct), RemoteCVarService, Paths::
    //     ResolveGameUserDir/SanitizePathSegment/CurrentPlatformDirs, and
    //     PathsConfigFor/PathsConfigWithoutProject.
    //   - S6 layouts: ProjectManifest dropped its splash/physics stores for
    //     `legacySettings`; HostConfig gained `backendSupplied`;
    //     DiagnosticsSettings gained `logTailLines`; settings blocks grew
    //     (Physics2DWorldSettings `gravity`, JobsSettings, SimSettings,
    //     NetSettings, RenderSettings, RenderMeshDefaultLightSettings);
    //     PhysicsDebugDrawOptions was rebuilt; GridSceneDesc and
    //     GizmoAxisColors grew; GizmoSnap lost its member defaults; the
    //     frames-in-flight node arrays (GpuScene, VisibilityRing, Batch2DNode,
    //     the fullscreen/grid/mesh/pick/outline nodes) grew to
    //     kMaxFramesInFlight; NriGraphContext, Batch2DNode, PostChainNode,
    //     GpuScene, MeshNode, NriMeshBufferCache and ImGuiNri gained members.
    //   - S6 signatures: HostBoot::ApplyEarlyConfigRungs; Gizmo HitTest/Draw/
    //     ApplyDrag take a GizmoTuning; Batch2DNode::CapsFrom/PoolSizes take
    //     framesInFlight and Record a clear colour; ShaderCompiler::Initialize
    //     split (InitializeWithDebounce); ProtocolLoader::GetSettings returns
    //     by value.
    //   - S6 new exports: Diagnostics::ReporterSettingsArgs/
    //     CurrentReporterSettingsArgs/AttachMissingCVarCallbacks,
    //     ApplyLegacyManifestSettings and the Project migration methods,
    //     ColorFromSrgb8/ToSrgb8, CheckFramesInFlightLatch, GraphCanvasFormat/
    //     GraphDepthFormat/LatchedCanvasFormat/LatchedDepthFormat/
    //     CheckGraphFormatLatch, OutlineMaxThicknessPx, PickSupersample,
    //     HostBoot::ShouldReportScanProgress, Window::IsFocused,
    //     ConsoleModel::AppendLine, Runtime::SetSnapshotSaveConfig/
    //     SnapshotSaveConfig, imgui's SetColorMarkerColors/
    //     GetColorMarkerColors; S2-H's ApplyEngineConfigRung and
    //     WarnUnlistedSettingsEnumValue.
    //   - S7-SEC, folded into 54 (unreleased: the settings arc is not on main
    //     yet): CVarFlags::LaunchesProgram; CVarApplyReport (refused),
    //     CVarConfigIssue (Kind::Refused, refusal) and LayerSources
    //     (projectRoots) grew; ApplyCVarDirectory took projectRoots; new
    //     exports CVarDirInsideAny, CheckLaunchPath/LaunchPathStatusText/
    //     HasCommandLineBreaker/QuoteWindowsArg and Diagnostics::ReporterExeFor.
    //     Its fix round 1: ApplyLegacyManifestSettings takes the manifest
    //     file; new exports Diagnostics::ReportDirFor/ReporterStemSafe/
    //     MonitorCommandFor (and the MonitorCommand struct), plus the inline
    //     Project::ManifestFile().
    //   A v53 module was compiled against the old layouts; reject the pairing.
    //   ReferenceProject.arcproj and Aphelyon.arcproj restamped.
    // v55 (2026-10-08, 2D physics events, spec docs/specs/2026-10-08-physics-2d-
    //     events-design.md): Fixture gained contactEvents/sensorEvents/hitEvents
    //     (an exported Core struct a module embeds through Collider2D -- its layout
    //     moved). PhysicsResource gained bodyRecords/stepEvents/frameEvents and the
    //     exported RecordBody/RetireBody/CaptureStep/StepEvents/FrameEvents/
    //     BeginFrame/ContactsOf; new SDK header Scene/PhysicsEvents2D.hpp; new
    //     settings block PhysicsEventSettings (physics.events); RunLoop gained the
    //     frame-begin hook member (Runtime.hpp includes RunLoop.hpp). The vendored
    //     Manifold2D moved to <M2D_EVENTS> (PhysicsResource holds its world).
    //     A v54 module was compiled against the old layouts; reject the pairing.
    //     ReferenceProject.arcproj and Aphelyon.arcproj restamped.
    // v56 (2026-10-10, namespace facades): the ECS prelude moved from Arcane::
    //     into Arcane::ECS, and the 2D physics types moved into
    //     Arcane::Physics2D under the short names (World, System, RigidBody,
    //     Collider, Fixture, SceneSettings, BodyRef, the event types, BodyType,
    //     ShapeKind). World's solver members are private. A v55 module was
    //     compiled against the old names and the old World layout; reject the
    //     pairing. ReferenceProject.arcproj and Aphelyon.arcproj restamped.
    // v57 (2026-10-10, flat gameplay API): the ECS prelude moved back into
    //     namespace Arcane, and the 2D physics types moved out of
    //     Arcane::Physics2D onto flat Arcane names (PhysicsWorld2D,
    //     PhysicsSystem2D, RigidBody2D, Collider2D, Fixture2D,
    //     PhysicsSettings2D, PhysicsBodyRef2D, and the event and settings
    //     types). Solver records live in Arcane::Detail::Physics2D. A v56
    //     module was compiled against Arcane::ECS and Arcane::Physics2D;
    //     reject the pairing. ReferenceProject.arcproj and Aphelyon.arcproj
    //     restamped.
    // v58 (2026-10-10, the Linux/macOS port -- adapter-keyed golden references):
    //     `NriDeviceCaps` (held by value inside the exported `NriDevice`) gained
    //     `adapterName` (std::string) and `softwareAdapter` -- layout.
    //     `ReferenceLevel` gained `Adapter`; `ReferenceAdapterSet`, a four-
    //     argument `ResolveReference` and `ResolveReferenceIn` are new
    //     ARC_API exports (additive; `ReferenceResolution`'s layout did not
    //     move). Consumed by the two hosts and ArcaneTests only -- no game
    //     module names NriDevice or ReferenceImages -- but a v57 module was
    //     compiled against the old NriDevice layout; reject the pairing.
    //     ReferenceProject.arcproj restamped (Aphelyon.arcproj restamps with
    //     its next engine sync). Main already consumed v52 through v57
    //     (settings, physics events, namespace facades, the flat API), so
    //     this layout change is v58.
    ARC_CONSTANT("ABI: the game-module ABI version; the host refuses a mismatched module")
    inline constexpr uint32_t kGamePluginABIVersion = 58;

    // The ABI version compiled into the LOADED Arcane.dll -- i.e. the one the
    // plugin gate actually enforces at runtime.
    //
    // kGamePluginABIVersion above is a header constant, so every module bakes in
    // its OWN copy at compile time. A host exe reporting that constant reports
    // what IT was built against, which is not necessarily what the DLL beside it
    // enforces: a partially-updated install (fresh Arcane.dll, stale
    // ArcaneEditor.exe, or the reverse) stamps a number the runtime will reject.
    // Anything PUBLISHING the ABI -- the `--print-engine-info` probe the Arcane
    // Hub stamps into new .arcproj files -- must ask the DLL, not itself.
    ARC_CORE_API uint32_t PluginABIVersion();

    struct EngineContext
    {
        uint32_t                abiVersion;    // == kGamePluginABIVersion at the host
        Arcane::TypeContext*    typeContext;   // plugin calls Astra::SetTypeContext(this) FIRST
        Arcane::IWorkScheduler* workScheduler; // the one engine enkiTS adapter (shared instance)
        Arcane::ITaskExecutor*  taskExecutor;  // SAME enki pool, worker-index ParallelFor (physics/general)
        Arcane::Runtime*        engine;        // registry, schedulers, snapshot/restore, render ctx

        // ABI 30 (Core-DLL split, spec 2026-09-15 s2/s3/s4) -- the ONLY three additions:
        Arcane::ProcessContext*  process;      // the process's one (TypeContext, system factories)
        Arcane::ClientRuntime*   client;       // presentation extension; NULL on a headless host (ArcaneServer, an embedded server world)
        Arcane::NetMode          netMode;      // the PRIMARY Runtime's mode -- systems branch on THIS, never on process->IsDedicatedServerProcess()

        // ImGui cross-DLL handoff (v2). ImGui's globals (GImGui) and heap do not
        // cross the DLL boundary; a plugin that wants to draw ImGui must adopt the
        // host's context + allocators via ImGui::SetCurrentContext / SetAllocatorFunctions
        // in Init. Kept as void* so this ABI header stays imgui-include-free; the
        // plugin reinterpret_casts them to ImGuiContext* / ImGuiMemAllocFunc /
        // ImGuiMemFreeFunc. Null in headless hosts (no ImGuiLayer) -> plugin skips.
        void* imguiContext  = nullptr;   // ImGuiContext*
        void* imguiAlloc    = nullptr;   // ImGuiMemAllocFunc
        void* imguiFree     = nullptr;   // ImGuiMemFreeFunc
        void* imguiUserData = nullptr;
    };

    namespace PluginEntry
    {
        inline constexpr const char* kABIVersion  = "GamePlugin_ABIVersion";
        inline constexpr const char* kInit        = "GamePlugin_Init";
        inline constexpr const char* kShutdown    = "GamePlugin_Shutdown";
        inline constexpr const char* kFixedUpdate = "GamePlugin_FixedUpdate";
        inline constexpr const char* kUpdate      = "GamePlugin_Update";
        inline constexpr const char* kSaveState   = "GamePlugin_SaveState";
        inline constexpr const char* kLoadState   = "GamePlugin_LoadState";
        // v2: the host calls DrawUI between ImGuiLayer::BeginFrame and ::Render --
        // the only window where ImGui draw calls are valid. The plugin's Update runs
        // in the sim phase (before BeginFrame), so it is too early for ImGui.
        inline constexpr const char* kDrawUI      = "GamePlugin_DrawUI";
    }

    // Host-side resolved function-pointer table for one loaded plugin image.
    struct PluginVTable
    {
        uint32_t (*ABIVersion)()                         = nullptr;
        bool     (*Init)(EngineContext*)                 = nullptr;
        void     (*Shutdown)()                           = nullptr;
        void     (*FixedUpdate)(double dt)               = nullptr;
        void     (*Update)(double dt, double alpha)      = nullptr;
        // SaveState is void because BinaryWriter is error-latching; callers check writer.HasError()
        // after the call, mirroring how LoadState signals failure via its bool return.
        void     (*SaveState)(Arcane::BinaryWriter&)     = nullptr;
        bool     (*LoadState)(Arcane::BinaryReader&)     = nullptr;
        // v2: host calls this between ImGuiLayer BeginFrame and Render (the only valid
        // ImGui draw window). Update is sim-phase -- too early. May be null if a plugin
        // does not export it (resolution is lenient); the host null-checks before calling.
        void     (*DrawUI)()                             = nullptr;
    };
}
