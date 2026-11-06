#pragma once
#include <Arcane/Modules/ArcaneClientApi.hpp>

// SceneRenderResolver: the per-frame bridge between what a scene REFERENCES
// (asset Guids on components) and what the submission path can DRAW (resolved
// tables + a bound post chain). One call per frame, from every host.
//
// WHY THIS EXISTS. RenderSubmissionSystem (Render/RenderSystems.hpp) never
// touches the Assets facade by design: it reads two pre-resolved lookup tables
// the host must publish every frame (SceneResources.hpp:4 states the contract).
// Until 2026-07-29 only the Arcane Editor did -- the resolution cache itself
// lived in Source/Programs/ArcaneEditor/src/SpriteCache.hpp -- so the standalone runtime host
// structurally could not draw a textured sprite whatever the scene contained,
// and neither could it bind a sprite material or a post chain. That is the
// second capability in one evening that the editor grew and the standalone host
// never got (the first was the camera, now Scene/SceneCamera.hpp). The fix
// under the homogenized-rendering directive is not a second cache in the
// runtime: it is ONE engine-side implementation both hosts drive.
// Brief: docs/superpowers/plans/2026-07-29-sprite-resolution-lift.md
// Design: docs/superpowers/specs/2026-07-29-sprite-resolution-lift-design.md
//
// WHAT IT OWNS. Five caches that turn a Guid into something bindable --
// SpriteCache (.arcsprite -> texture/UVs/size/pivot), SpriteMaterialCache
// (.arcmat -> a registered Batcher2D material id) and PostChainCache (.arcmat ->
// a bound post chain, published as PostChainDesc bytes for the graph
// recorder). Those three share one compile drain site, which is why they
// belong to one owner: splitting them would mean a second Drain(), and
// ShaderCompiler::Drain is the ONE place a compile result is taken up
// (ShaderCompiler.hpp:9-13).
//
// F2a (Task 6) adds their 3D siblings, MeshCache (.arcmesh -> owned CPU
// geometry + local bounds, Render/MeshCache.hpp) and MeshMaterialCache
// ("mesh"-kind .arcmat -> constants-only ResolvedMeshMaterial,
// Render/MeshMaterialCache.hpp). NEITHER touches ShaderCompiler at all -- a
// "mesh"-kind .arcmat carries no snippet to compile in the first place
// (MeshMaterialCache.hpp:9-21) -- so they do not extend the drain-sharing
// argument above; they still join this class rather than standing alone,
// for the OTHER reason every cache here is together (see WHY THIS EXISTS):
// one per-frame call, and one project-backed `resolveAsset` closure (built
// once in the constructor) that every cache -- compiling or not -- resolves
// its Guids through. A second owner for the two mesh caches would mean a
// second copy of that closure, which is exactly the kind of drift the
// sprite arc's lift was fixing in the first place.
//
// WHAT IT DOES NOT OWN. The camera (a pure sweep -- Scene/SceneCamera.hpp),
// the batcher, and the compile service: all injected. It never renders, and
// it holds no graphics device at all. F2b Task 11 adds ONE more injected
// seam under the same rule -- Services::resolveMeshAlbedoSlot, a callback
// rather than an NRI/device type, so this promise stays literally true.

#include <Arcane/Guid.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>           // MeshResolveState -- MeshSupply
#include <Arcane/Render/GraphicsBackend.hpp>   // by value in Services
#include <Arcane/Mesh/MeshBuilder.hpp>       // MeshData -- MeshSupply


#include <cstdint>
#include <functional>
#include <optional>

namespace Arcane
{
    class Batcher2D;
    class MaterialInstance;
    class ClientRuntime;
    class ShaderCompiler;
    class ShaderSourceProvider;
    struct PostChainDesc;
    struct ShaderCompileResult;

    class ARC_CLIENT_API SceneRenderResolver
    {
    public:
        struct Services
        {
            // The client runtime whose Registry is swept and whose SpriteTable /
            // SpriteMaterialTable resources are published. Required. ClientRuntime,
            // not Runtime: the four Set*Table publishers are presentation and live
            // above the Core seam since the Core-DLL split (plan 1 Task 4).
            ClientRuntime* runtime = nullptr;

            // The frame batcher registered materials bind into. Null (a
            // device-less host or a test) disables material binding; sprite
            // resolution still works. Material binding is the whole of what
            // this field does: there is no texture-eviction hook, because
            // nothing here holds a device object to evict.
            Batcher2D* batcher = nullptr;

            // The shader-compile target profile, forwarded to the sub-caches
            // that need one. No device travels beside it: none of the caches
            // builds device objects.
            GraphicsBackend backend{};

            // The app-shared compile service. Null, or an unavailable one (no
            // dxcompiler.dll), disables materials AND the post chain -- but NOT
            // sprite resolution, which is a synchronous JSON+texture load with
            // no compile step. That distinction is load-bearing: gating the
            // sprite sweep on the compiler is exactly the bug the sprite arc's
            // F1 fix removed (a DXC-less machine drew every sprite as an
            // indistinguishable untextured 1x1 quad, with no diagnostic).
            ShaderCompiler*       compiler = nullptr;
            ShaderSourceProvider* sources  = nullptr;

            // Offered every drained compile result BEFORE the caches; true means
            // consumed. This is how the process keeps exactly ONE drain site
            // while the editor still routes results to its open shader
            // documents. Hosts with no documents leave it unset.
            std::function<bool(const ShaderCompileResult&)> consumeFirst;

            // F2b Task 11: resolves a mesh material's albedo Guid into a
            // bindless material-table slot on the frame's live NRI device --
            // forwarded verbatim into MeshMaterialCache::Services
            // (Render/MeshMaterialCache.hpp's ResolveAlbedoSlotFn carries the
            // full contract). Injected for the SAME reason `consumeFirst` is:
            // this class holds no device of its own (WHAT IT DOES NOT OWN,
            // above), so the one thing here that ever touches NRI is a
            // callback a host with a live NriGraphContext supplies -- see
            // NriGraphContext::ResolveMeshAlbedoSlot, the implementation
            // both hosts wire in. Null (every test, and a host before its
            // graph context exists) leaves every mesh's albedo unresolved,
            // the flat baseColor path.
            std::function<std::uint32_t(const Guid&)> resolveMeshAlbedoSlot;

            // F2c Plan 2 Task 6: drop resident GPU buffers for this .arcmesh
            // guid. Injected for the same reason resolveMeshAlbedoSlot is --
            // this class holds no device. Null (tests that do not care, a host
            // before its graph exists) skips the GPU half; CPU Invalidate +
            // Request still run.
            std::function<void(const Guid&)> invalidateMeshGeometry;
        };

        // This frame's facts. `now` is the compile service's clock (monotonic
        // seconds): material and post Requests debounce against it and the
        // compile service polls on it, so it must keep running. `sceneTime`,
        // when set (--fixed-time), is the Time the material globals report
        // instead of `now`; pinning it never pins the compile clock.
        // Viewport extent sizes the material globals.
        struct FrameInfo
        {
            double now = 0.0;
            std::optional<double> sceneTime;
            double dt  = 0.0;
            float  viewportWidth  = 0.0f;
            float  viewportHeight = 0.0f;
        };

        explicit SceneRenderResolver(Services services);

        // Publishes nulls through the Runtime, because the registry holds
        // NON-OWNING pointers to this object's maps (SceneResources.hpp:96-99).
        // CONTRACT for every host: declare the resolver so it destructs BEFORE
        // the Runtime (whose registry would otherwise be left pointing at freed
        // maps). There is no "before the render device" half to this
        // contract: none of the five caches this class owns holds a
        // device-bound handle (see SpriteCache.cpp's own note), so the
        // Runtime-registry ordering above is the whole of it.
        ~SceneRenderResolver();

        SceneRenderResolver(const SceneRenderResolver&) = delete;
        SceneRenderResolver& operator=(const SceneRenderResolver&) = delete;

        // THE per-frame call. Sweep -> request -> pump -> latch -> publish, in
        // that order (see the .cpp for why each step sits where it does).
        //
        // Call it BEFORE the frame's Batcher2D::Begin and before its
        // ExtractFrame: the drain REGISTERS materials with the batcher
        // (pipeline + binding-set table mutation), which does not belong inside
        // a Begin..Drain recording, and the tables it publishes are what that
        // frame's submission reads.
        void Refresh(const FrameInfo& frame);

        // Engine-global material constants for THIS frame (Time/Delta/Viewport).
        // Feed Batcher2D::SetGlobals with it after Begin -- registered sprite
        // materials read zeros otherwise (built-ins ignore it).
        const GlobalParams& Globals() const;

        // The scene's bound post material instance, or null for "no chain".
        // Re-read every frame AFTER Refresh: a drain may swap the bound
        // instance under an asset re-save.
        const MaterialInstance*  PostInstance() const;

        // The bound chain as bytecode + layout + values, for the graph's
        // post nodes (see PostChainDesc in
        // Render/PostChainCache.hpp). Null for "no chain", and latched beside
        // PostInstance() in the same Refresh step, so both describe one
        // compile. Re-read every frame AFTER Refresh, for the same reason
        // PostInstance() is.
        //
        // THE ONLY BOUND-CHAIN ACCESSOR: the graph recorder is the only
        // render path, and it binds from exactly these bytes.
        const PostChainDesc* PostDesc() const;

        // What the scene REFERENCES versus what is bound right now.
        //
        // Materials bind asynchronously -- Refresh submits the compiles and a
        // LATER Refresh drains whatever the compile worker finished in the
        // meantime -- so "the scene declares a material" and "this frame can
        // draw it" are different facts, separated by WALL-CLOCK time. For an
        // interactive host that gap is invisible (a material appears a few
        // frames in and nobody minds). For a CAPTURE it is the whole
        // ballgame: a captured frame missing its materials is not a slower
        // frame, it is a picture of DIFFERENT CONTENT.
        //
        // So this is the probe a host uses to answer "is what I am about to
        // capture actually the scene?" -- read it AFTER draining the compile
        // service to idle. Deliberately computed on demand (it re-walks the
        // SpriteRenderer and PostProcess views) rather than accumulated every
        // Refresh: only a capture or probe path calls it, and no ordinary
        // frame should pay for it. Both halves read the LIVE registry against the LIVE
        // caches, so the answer never depends on whether a Refresh has run --
        // a resolver that has never swept simply reports everything unbound.
        //
        // `spriteReferenced` counts sprite COMPONENTS carrying a valid material
        // Guid, not distinct Guids -- "every referencing sprite can draw its
        // material" is the property a host actually wants, and it stays correct
        // when two sprites share one material.
        struct MaterialCensus
        {
            int  spriteReferenced = 0;       // SpriteRenderers with a valid material Guid
            int  spriteBound      = 0;       // ...whose material is registered with the batcher
            bool postReferenced   = false;   // a PostProcess assignment was found
            // ...and its chain is bound and ready -- computed from Desc()
            // != nullptr (see Materials()'s own comment), which is NOT
            // device-gated: a chain counts as bound once its bytes exist.
            bool postBound        = false;
            // The 3D half (F2a), the same referenced/bound pair one dimension
            // up: MeshRenderers carrying a valid `mesh` Guid, and how many of
            // those the MeshCache has actually resolved into a MeshTable entry.
            //
            // WHY THE GEOMETRY GUID AND NOT THE MATERIAL ONE, in a struct
            // called MaterialCensus: `meshReferenced` has to be PURE SCENE
            // DATA for this struct's stated cold-equals-warm property to hold
            // (see the block above -- both halves read the live registry, so a
            // resolver that has never swept reports everything unbound and
            // nothing un-REFERENCED). A MeshRenderer's effective material is
            // the materialOverride -> MeshEntry::slots -> white chain
            // (GpuSceneSync's ResolveRowMaterial, Render/GpuSceneSync.hpp), and its middle link is only
            // knowable AFTER MeshCache has resolved the mesh -- so a
            // material-shaped `meshReferenced` would answer 0 cold and 1 warm,
            // breaking exactly that property. The mesh half of the material
            // chain is still reachable to a caller that wants it, through the
            // published MeshTable/MeshMaterialTable resources.
            //
            // Unlike the sprite pair, this one CAN report bound in a device-less
            // census: neither mesh cache compiles anything, so Refresh's mesh
            // sweep is ungated on the compiler and the batcher
            // (SceneRenderResolver.cpp's (1b)).
            int  meshReferenced   = 0;       // MeshRenderers with a valid mesh Guid
            int  meshBound        = 0;       // ...whose .arcmesh resolved into the MeshTable
        };
        MaterialCensus Materials() const;

        // A .arcsprite was re-saved: evict, then re-resolve SYNCHRONOUSLY, so no
        // frame can render the placeholder in the gap. (SpriteCache::Request has
        // no async step, so this costs one JSON load. The material caches get
        // the same no-gap property from their last-good scheme instead.)
        void InvalidateSprite(const Guid& id);

        // An .arcmesh was re-saved -- or undone/redone in its open document:
        // evict, then re-resolve SYNCHRONOUSLY, the same no-gap rule
        // InvalidateSprite states above and for the same reason (MeshCache::
        // Request has no async step either, so the entry is back before this
        // returns). The gap would be WORSE here than for a sprite: an evicted
        // mesh has no placeholder at all (MeshTable's own comment,
        // Scene/SceneResources.hpp), so a frame landing between the erase and
        // the re-resolve draws NOTHING rather than a wrong-looking quad.
        //
        // WHEN THIS MAY BE CALLED IS A LIFETIME CONTRACT, not a preference.
        // GpuSceneSync still looks the Guid up in MeshTable to decide
        // whether the entity is drawable; MeshCache::Invalidate ERASES that
        // entry. So this must never land between a host's ExtractFrame
        // (which publishes the GPU scene) and the RenderFrame call that consumes its output. Both call
        // sites today satisfy that by construction: MeshDocument::Save and
        // MeshDocument::ApplyMeshData run from the editor's document phases
        // (PumpEditorDocuments / DrawEditorUi), which are strictly AFTER phase
        // 10's ArmGraphViewportFrame -> RenderFrameOffscreen pair has returned
        // and strictly BEFORE the next frame's. A future caller inside the
        // render phase would have to defer instead.
        //
        // Only the GEOMETRY cache is touched. Re-saving an .arcmesh can change
        // which material Guid a slot names, and the Request below re-reads that
        // into the fresh MeshEntry::slots -- but the resolved VALUES of any
        // .arcmat are untouched by a mesh edit. A .arcmat re-save is
        // InvalidateMaterial's business, below.
        //
        // Device side (F2c s7.3 / Plan 2 Task 7): capture the entry's GEOMETRY
        // IDENTITY (MeshEntry::GeometryIdentity -- source, importedSource and every
        // generator parameter) before the erase, re-resolve, compare the whole
        // tuple. Unchanged -> leave the resident GPU buffers alone, so re-pointing a
        // material never re-uploads a two-million-triangle prop. Changed, or the
        // entry did not exist -> invalidateMeshGeometry. A SLOT REASSIGNMENT IS THE
        // ONLY THING THAT KEEPS RESIDENCY; a topology edit drops it (final-review C3
        // -- the identity was source-only, so it did not). Distinct from
        // InvalidateMeshArtifact, which always drops residency (the cook landed new
        // vertices).
        void InvalidateMesh(const Guid& id);

        // A mesh ARTIFACT changed (a cook landed for the .gltf/.glb this mesh
        // imports from). Distinct from InvalidateMesh, which is a .arcmesh SAVE:
        //   * InvalidateMeshArtifact  -> the GEOMETRY changed. Drop the resolved
        //                                MeshEntry AND the resident GPU buffers, and
        //                                re-request.
        //   * InvalidateMesh          -> the .arcmesh changed. Drop the resolved
        //                                MeshEntry and re-request, and KEEP THE
        //                                RESIDENT BUFFERS only when the geometry
        //                                identity is unchanged -- i.e. a SLOT
        //                                REASSIGNMENT. A TOPOLOGY EDIT (source,
        //                                importedSource, rings, segments,
        //                                subdivisions, capsuleLengthRatio) lands in
        //                                THIS arm and drops them -- see Task 7 and
        //                                final-review C3.
        // `id` here is the .arcmesh's guid, not the model's: the caller maps the cooked
        // MODEL guid to its companion through the mint (or the reference index), because
        // the render side keys everything on the mesh asset the scene actually names.
        void InvalidateMeshArtifact(const Guid& id);

        // A .arcmat was re-saved: re-resolve on the next Refresh. Hits BOTH the
        // sprite-material and post caches -- one Guid cannot be known to be
        // only one of the two, and a wrong guess silently strands the other.
        //
        // ...AND, one level deeper (F2a, Task 6), the mesh-material cache --
        // but NOT by the same per-Guid rule the two caches above use. Neither
        // material cache tracks reverse parent -> child dependencies
        // (MeshMaterialCache::Invalidate(id) drops only `id`), so from a base
        // material's Guid alone you cannot tell which INSTANCES inherit from
        // it -- the identical "one Guid cannot be known to be only one of the
        // two" problem stated above, applied to a chain instead of a pair: a
        // wrong guess here silently strands every descendant instance at its
        // pre-edit colour. That is precisely the desk checkpoint this arc
        // ships against (create a material, create an instance of it, assign
        // the instance to a second entity, edit the base, expect BOTH
        // entities to follow). So the mesh-material cache does not get a
        // targeted Invalidate(id) call at all -- it gets Clear()'d whole.
        // That is cheap and provably correct here, unlike for the two caches
        // above: MeshMaterialCache is params-only, holds no compiled
        // artifact and straddles no async step (its own header, "NEVER
        // touches MaterialSource or ShaderCompiler"), so a full drop costs
        // exactly one re-read per referenced material on the next Refresh --
        // there is no in-flight compile to strand and no last-good bind to
        // lose.
        //
        // The sprite path (SpriteMaterialCache) has the IDENTICAL gap -- an
        // instance of a re-saved base SPRITE material would go stale the
        // same way -- and it is deliberately left alone here: it is
        // pre-existing, not a regression this task introduces, and widening
        // into it is a different task's business.
        void InvalidateMaterial(const Guid& id);

        // Project switch: forget everything. A Guid resolves through the CURRENT
        // project's registry, so a cached entry may resolve to something else
        // entirely (or nothing) once the project changes.
        void Clear();

        // F2c Plan 2 Task 3: Guid -> CPU geometry the GPU cache uploads.
        // Ready + pointer on a Table() hit; otherwise Query()'s PendingCook /
        // Failed with a null mesh. Device-free -- the host wraps this into
        // NriGraphContext::SetMeshSupply.
        struct MeshGeometrySupply
        {
            const MeshData*  mesh  = nullptr;
            MeshResolveState state = MeshResolveState::Failed;
        };
        [[nodiscard]] MeshGeometrySupply MeshSupply(const Guid& id) const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
