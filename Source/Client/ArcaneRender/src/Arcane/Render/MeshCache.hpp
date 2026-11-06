#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// MeshCache: resolves .arcmesh Guids referenced by MeshRenderer::mesh into
// Arcane::MeshEntry records for the scene's MeshTable. Mirrors SpriteCache's
// host integration (SpriteCache.hpp) and its structure almost exactly -- like
// that cache, there is no async compile step of its OWN here: load the
// .arcmesh JSON (LoadMeshAsset), then resolve its geometry through
// Mesh/MeshAsset.hpp's ResolveMeshData (F2c Task 11) -- a generated source
// resolves synchronously in that one call (BuildMeshData + ComputeMeshBounds
// internally, unchanged since before Task 11), but an Imported source can
// come back PendingCook: the mesh COOK is async (arccook, or the editor's
// background queue), even though this cache's OWN resolve step is not. See
// Request's own comment for what PendingCook does to the per-frame contract
// below.
//
// FAILURE DISCIPLINE, and this is the one place this cache DIFFERS from
// SpriteCache rather than mirroring it: a failed resolve (unresolvable Guid,
// missing file, malformed JSON, an invalid mesh) stays OUT of the published
// table, in a separate `failed` set -- SpriteMaterialCache's scheme
// (SpriteMaterialCache.cpp:49,97-101), not SpriteCache's own "cache a visible
// placeholder" one. A sprite has a natural placeholder (a 1x1 m untextured
// quad); a mesh does not -- there is no meaningful "wrong shape" to draw
// instead of the right one, and drawing one would be worse than drawing
// nothing. So a nullptr Resolve() is the correct outcome, and
// GpuSceneSync (Render/GpuSceneSync.hpp) is expected to skip the entity entirely, the
// same way it would for a nil MeshRenderer::mesh.
//
// PendingCook (F2c Task 11) is a THIRD outcome, neither success nor failure:
// an Imported mesh whose cook has not landed YET stays out of BOTH `table`
// and `failed` -- see Request's own comment for why entering `failed` here
// would be the exact regression F2b's desk-fix 2 fixed for the texture path.
//
// WHAT IT DOES NOT KEEP: the loaded MeshAssetData. An `AssetFor(Guid) ->
// const MeshAssetData*` accessor lived here through Tasks 4-11, backed by a
// second map beside `table`, on the stated premise that Task 5's material
// chain would read the mesh's own default `material` Guid through it. Task 5
// resolved that differently and better -- MeshEntry::slots (F2c Task 10 grew
// this from a scalar `material`; SceneCacheRecords.hpp) carries the Guid(s) on
// the published table, so the submission sweep needs no cache pointer at all
// -- and no other consumer ever appeared: MeshDocument, its one named
// candidate, edits the SOURCE .arcmesh and never touches the resolved cache.
// Deleted at F2a close with zero production callers, which also removes the
// second map and the write-ordering hazard between the two.
//
// ENGINE-SIDE, not editor-side, from the moment it is written -- the same
// placement rule the sprite-resolution lift (2026-07-29) established for
// SpriteCache: this lives in Arcane.dll so both the editor and the
// standalone runtime host draw through the one implementation.

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>          // MeshArtifactSupplyFn / CookPendingFn -- Services' Task 11 fields
#include <Arcane/Scene/SceneCacheRecords.hpp>   // Arcane::MeshEntry (full type: Table()'s value type)

#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>

namespace Arcane
{
    class ARC_RENDER_API MeshCache
    {
    public:
        // Guid-shaped, matching SpriteCache/SpriteMaterialCache/PostChainCache
        // exactly, so one host service can hand every cache the same resolver
        // (the AssetId round-trip happens inside).
        using ResolveAssetFn =
            std::function<std::optional<std::filesystem::path>(const Guid&)>;

        struct Services
        {
            ResolveAssetFn resolveAsset;   // Guid -> path (project registry)
            // F2c Task 11: forwarded VERBATIM from the Assets facade
            // (Assets::MeshArtifactFor / Assets::CookPending) -- the same "no wrapper
            // needed, the shapes already match" reasoning MeshMaterialCache::Services::
            // resolveAlbedoSlot already uses for its own forward (F2b Task 11). Only an
            // Imported .arcmesh's resolve (Request -> ResolveMeshData) ever consults
            // these; a generated source needs neither.
            MeshArtifactSupplyFn meshArtifactFor;
            CookPendingFn        cookPending;
        };

        explicit MeshCache(Services services);
        ~MeshCache();
        MeshCache(const MeshCache&) = delete;
        MeshCache& operator=(const MeshCache&) = delete;

        // Ensure `id` is resolved (or known-failed) -- idempotent, a no-op
        // once known (same per-frame sweep contract as SpriteCache::Request).
        // Call per frame per referenced mesh Guid.
        //
        // F2c Task 11: an Imported mesh whose ResolveMeshData call answers
        // MeshResolveState::PendingCook is a NO-OP, not a failure -- the guid stays out
        // of both `table` and `failed`, so the very next Request (next frame) asks
        // again. This is deliberately NOT idempotent for that one state (every OTHER
        // outcome -- Ready or Failed -- IS idempotent, per the paragraph above): a
        // PendingCook mesh has nothing memoized to skip, by design, the same "nothing
        // was ever latched, so there is nothing to un-latch" posture Assets.hpp's
        // SetCookPendingProbe states for the texture path (F2b desk-fix 2).
        void Request(const Guid& id);

        // Asset re-saved / removed: drop the table (and known-failed) entry
        // so the next Request re-resolves from disk.
        void Invalidate(const Guid& id);

        // Forget everything (project switch): a Guid resolves through the
        // CURRENT project's registry, so a cached entry from the outgoing
        // project may resolve to something else entirely (or nothing) once
        // the project changes.
        void Clear();

        // Guid -> resolved record (the MeshTable payload). Stable storage
        // between mutating calls -- see MeshEntry's own comment
        // (SceneCacheRecords.hpp) for exactly what "stable" guarantees here and
        // why (element addresses survive a rehash; only an erase invalidates
        // them).
        const std::unordered_map<Guid, MeshEntry>& Table() const;

        // MeshTable::generation's storage (SceneResources.hpp): a counter this
        // cache bumps on EVERY mutation of Table() -- the Ready emplace in
        // Request, Invalidate's erase, Clear -- so BoundsSystem can tell that a
        // resolved record changed under an entity that itself did not (a
        // primitive parameter edit, a reimport). Published beside the map by
        // SceneRenderResolver through ClientRuntime::SetMeshTable; the pointer
        // is as stable as this object. Starts at 1 so a consumer whose stored
        // value is 0 ("never read") sees the first publish as a change.
        [[nodiscard]] const std::uint64_t* Generation() const noexcept;

        // F2c Plan 2 Task 3: the GPU cache's supply needs Pending vs Failed
        // without re-resolving. Ready = in Table(); Failed = known-failed set;
        // PendingCook = neither (never requested, or Imported still cooking).
        //
        // PRECONDITION: `id` has been Request()ed. PendingCook is a promise that
        // asking again will eventually resolve, and only a requested guid can keep
        // it -- see the definition for the full account and for the one-shot WARN a
        // never-requested guid gets instead of a fourth state.
        [[nodiscard]] MeshResolveState Query(const Guid& id) const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
