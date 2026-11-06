#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// MeshMaterialCache: resolves "mesh"-kind .arcmat Guids (a MeshRenderer's
// materialOverride, or the .arcmesh asset's own default `material`) into
// Arcane::ResolvedMeshMaterial records for the scene's MeshMaterialTable --
// MeshCache's sibling, resolving the MATERIAL half of what MeshRenderer names
// where MeshCache resolves the GEOMETRY half.
//
// READS SAVED PARAM VALUES DIRECTLY, and NEVER stitches a shader source nor
// touches ShaderCompiler -- unlike SpriteMaterialCache, which does both
// (SpriteMaterialCache.cpp's Request/Bind). The one thing it takes from
// Material/MaterialSource.hpp is `MaterialSurfaceForKind`, the pure
// kind-string -> surface classification behind THE KIND GATE below;
// PostChainCache.cpp takes the identical one-function dependency for its own
// surface check. None of the stitch/template/bindings machinery in that
// header is reachable from here, and that is the part that matters: a
// "mesh"-kind .arcmat carries no snippet at all (F2a design: two params,
// `baseColor` and `albedo` -- the latter declared-but-unbound in F2a and
// given a consumer in F2b Task 11, see Services::resolveAlbedoSlot below),
// so there is no //@param declaration to parse and no MaterialTemplate to build --
// MaterialSurface::Mesh does not even have a template file yet
// (MaterialTemplateFile's ARC_ENSURE guard, Material/MaterialSource.cpp).
// Reaching for that machinery here would not just be unneeded, it would
// actively trip that guard. This is deliberate and load-bearing: it is what
// keeps SceneRenderResolver's single ShaderCompiler::Drain() site intact
// (Host/SceneRenderResolver.hpp:22-28) -- a second compiling cache would need
// a second drain.
//
// THE KIND GATE: Request refuses any material whose chain BASE is not
// "mesh"-kind. The base, not the leaf, because an instance carries no kind of
// its own. Assigning a "sprite" or "fullscreen" .arcmat to a
// MeshRenderer::materialOverride used to resolve silently -- to white (no
// baseColor param at all) or to a sprite's unrelated tint -- which is
// indistinguishable from a BROKEN reference at the desk. The refusal makes it
// one log line, and falls through the resolution chain like any other failure.
//
// INSTANCES STILL WORK, because MaterialAssetData::parent + sparse overrides
// is data, not compiled state (MaterialAsset.hpp:5-9: "no snippet, no kind --
// both come from the base at the end of the parent chain") -- and per that
// same definition, reading only `id`'s own params would be wrong: an
// instance genuinely IS "a parent Guid + sparse overrides," so its effective
// baseColor can depend on ancestors it never mentions. Request() walks the
// parent chain through Material/MaterialAsset.hpp's LoadMaterialParentChain
// -- the SAME cycle-guarded walker SpriteMaterialCache::Request calls, so
// the cycle guard and its failure strings live in exactly one place -- but
// collects each level's raw saved params from it instead of feeding them to
// a MaterialInstance, since there is no template to build one over.
//
// SAME FAILURE DISCIPLINE AS SpriteMaterialCache, NOT SpriteCache: a broken
// or unresolvable material (missing asset, malformed JSON, or a cyclic
// parent chain) stays OUT of the published table, memoized in a separate
// `failed` set. There is no placeholder material to fall back to -- Task 5's
// resolution chain (materialOverride -> mesh default -> white) simply falls
// through to its next link, the same way a nil Guid does.

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/SceneCacheRecords.hpp>   // Arcane::ResolvedMeshMaterial (full type: Table()'s value type)

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>

namespace Arcane
{
    class ARC_RENDER_API MeshMaterialCache
    {
    public:
        // Guid-shaped, matching every other cache's resolver -- see
        // MeshCache::ResolveAssetFn.
        using ResolveAssetFn =
            std::function<std::optional<std::filesystem::path>(const Guid&)>;

        // Guid (the resolved `albedo` texture) -> a bindless material-table
        // slot, or 0xFFFFFFFF ("no slot" -- BindlessTable::kInvalidSlot's own
        // value; restated as a literal for the SAME reason SceneCacheRecords.
        // hpp's ResolvedMeshMaterial::materialSlot does, see that field's own
        // comment) when there is none. F2b Task 11's device seam: this class
        // stays CONSTANTS ONLY and never reaches NriTextureCache or
        // BindlessTable itself (this header's own header comment, and
        // MeshMaterialCache.cpp's OwnAlbedo helper never sees an NRI type) --
        // the callback's IMPLEMENTATION is what does that, owned by whoever
        // supplies it (a host's live NriGraphContext -- see
        // NriGraphContext::ResolveMeshAlbedoSlot, which is what
        // SceneRenderResolver wires in here through its OWN injected
        // `resolveMeshAlbedoSlot` field, Host/SceneRenderResolver.hpp).
        using ResolveAlbedoSlotFn = std::function<std::uint32_t(const Guid&)>;

        struct Services
        {
            ResolveAssetFn resolveAsset;   // Guid -> path (project registry)

            // Null in every CPU test and on a host before its graph context
            // exists -- Request() then leaves every resolved material's
            // `materialSlot` at its default (kInvalidSlot's value), the flat
            // baseColor path, exactly the same degrade a null `resolveAsset`
            // already gets. See ResolveAlbedoSlotFn's own comment above.
            ResolveAlbedoSlotFn resolveAlbedoSlot;
        };

        explicit MeshMaterialCache(Services services);
        ~MeshMaterialCache();
        MeshMaterialCache(const MeshMaterialCache&) = delete;
        MeshMaterialCache& operator=(const MeshMaterialCache&) = delete;

        // Ensure `id` (and its whole parent chain) is resolved or known-
        // failed -- idempotent, a no-op once known. Call per frame per
        // referenced material Guid.
        void Request(const Guid& id);

        // `id`, or one of its ancestors, was re-saved: drop the table (and
        // known-failed) entry so the next Request re-resolves the whole chain
        // from disk.
        void Invalidate(const Guid& id);

        // Forget everything (project switch).
        void Clear();

        // Guid -> resolved record (the MeshMaterialTable payload). Stable
        // storage between mutating calls.
        const std::unordered_map<Guid, ResolvedMeshMaterial>& Table() const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
