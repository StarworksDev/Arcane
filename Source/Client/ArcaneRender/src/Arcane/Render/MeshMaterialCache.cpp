#include <Arcane/Render/MeshMaterialCache.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
// MaterialSurfaceForKind ONLY -- the pure kind-string -> surface
// classification. NOT the stitch/compile half of MaterialSource this cache's
// header rules out; PostChainCache.cpp takes exactly the same one-function
// dependency for exactly the same surface check.
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Material/MaterialTypes.hpp>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Arcane
{
    // No pending/keep-alive state of any kind: unlike SpriteMaterialCache,
    // this cache submits no compile job, so Request() resolves synchronously
    // in one call and there is nothing for a later drain to finish. `failed`
    // is the ONLY extra bookkeeping beyond `table` -- see the header's
    // FAILURE DISCIPLINE block.
    struct MeshMaterialCache::Impl
    {
        Services services;
        std::unordered_map<Guid, ResolvedMeshMaterial> table;
        std::unordered_set<Guid> failed;
        // Materials whose slot the device released: Request re-resolves their
        // albedo (until it lands) without re-reading the asset.
        std::unordered_set<Guid> reresolve;

        // A resolved material holding a slot gives its reference back.
        void ReleaseRef(const ResolvedMeshMaterial& r) const
        {
            if (r.materialSlot != 0xFFFFFFFFu && r.albedo.IsValid() && services.releaseAlbedoSlot)
                services.releaseAlbedoSlot(r.albedo);
        }
    };

    namespace
    {
        // This ONE level's own saved "baseColor", if it declares one -- Color
        // and Float4 both pack as a linear float4 (MatParamType's own
        // comment), and a "mesh"-kind asset carries no //@param decl to check
        // the author's spelling against (it has no snippet at all), so both
        // spellings are accepted the same way a real decl eventually would.
        // Any OTHER type saved under the name "baseColor" (a hand-edited
        // file, most likely) is ignored rather than reinterpreted -- reading
        // .f[0..3] off a Texture value would read `tex`'s bytes as floats.
        //
        // Scans the whole vector rather than stopping at the first hit, and
        // returns the LAST match. NOT because a loaded file can contain two
        // "baseColor" entries -- it cannot: LoadMaterialAsset builds `params`
        // from a JSON OBJECT (MaterialAsset.cpp's `doc["params"].items()`),
        // whose keys are unique by construction, so a duplicate in the raw
        // text is already collapsed by the parser. The reason is simply that
        // `params` is an ordered vector with no uniqueness invariant of its
        // own: an IN-MEMORY MaterialAssetData assembled by some future caller
        // could carry two, and last-wins is the only rule consistent with how
        // the layering loop below already treats later levels.
        std::optional<glm::vec4> OwnBaseColor(const MaterialAssetData& data)
        {
            std::optional<glm::vec4> found;
            for (const auto& [name, value] : data.params)
            {
                if (name != "baseColor")
                    continue;
                if (value.type != MatParamType::Color && value.type != MatParamType::Float4)
                    continue;
                found = glm::vec4(value.f[0], value.f[1], value.f[2], value.f[3]);
            }
            return found;
        }

        // OwnBaseColor's mirror for the OTHER F2a-declared param (F2b Task
        // 11 gives it a consumer): a "mesh"-kind asset carries no //@param
        // snippet to check the author's spelling/type against either, so a
        // saved "albedo" of any OTHER MatParamType is ignored the same way
        // an OTHER-typed "baseColor" is above -- reading `.tex` off a Color/
        // Float4 value would read those floats' bytes as a Guid. Same
        // last-write-wins, whole-vector-scan reasoning as OwnBaseColor (see
        // its own comment); nothing here duplicates that account.
        std::optional<Guid> OwnAlbedo(const MaterialAssetData& data)
        {
            std::optional<Guid> found;
            for (const auto& [name, value] : data.params)
            {
                if (name != "albedo")
                    continue;
                if (value.type != MatParamType::Texture)
                    continue;
                found = value.tex;
            }
            return found;
        }
    }

    MeshMaterialCache::MeshMaterialCache(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);
    }

    MeshMaterialCache::~MeshMaterialCache() { delete m_impl; }

    const std::unordered_map<Guid, ResolvedMeshMaterial>& MeshMaterialCache::Table() const
    {
        return m_impl->table;
    }

    void MeshMaterialCache::Request(const Guid& id)
    {
        Impl& im = *m_impl;
        if (!id.IsValid() || im.failed.contains(id))
            return;
        if (const auto known = im.table.find(id); known != im.table.end())
        {
            // Known. The only work left: a slot the device released is re-resolved
            // (the texture may be resident by now), once it lands the flag clears.
            if (!im.reresolve.empty() && im.reresolve.contains(id))
            {
                ResolvedMeshMaterial& r = known->second;
                if (r.albedo.IsValid() && im.services.resolveAlbedoSlot)
                {
                    r.materialSlot = im.services.resolveAlbedoSlot(r.albedo);
                    if (r.materialSlot != 0xFFFFFFFFu)
                        im.reresolve.erase(id);
                }
                else
                {
                    im.reresolve.erase(id);
                }
            }
            return;
        }

        // Same shape as SpriteMaterialCache::Request's fail() -- WARN once,
        // memoize into `failed` so the guard above turns back every later
        // Request for this Guid before this lambda runs again.
        auto fail = [&](const std::string& why)
        {
            ARC_WARN("MeshMaterialCache: material {} unavailable -- {}", id.ToString(), why);
            im.failed.insert(id);
        };

        if (!im.services.resolveAsset)
            return fail("no asset resolver installed");
        const auto path = im.services.resolveAsset(id);
        if (!path)
            return fail("not in the asset registry");
        auto data = LoadMaterialAsset(*path);
        if (!data)
            return fail("asset failed to load");

        // Walk to the base through the SHARED walker (Material/MaterialAsset.
        // hpp's LoadMaterialParentChain), the same one SpriteMaterialCache::
        // Request now calls -- this cache collects raw MaterialAssetData from
        // it instead of feeding a MaterialInstance, since there is no
        // template to build one over.
        std::vector<MaterialAssetData> chain;   // immediate parent .. base
        if (const auto why = LoadMaterialParentChain(im.services.resolveAsset, id, data->parent, chain))
            return fail(*why);

        // THE KIND GATE, read off the chain's BASE and not the leaf -- an
        // instance carries no kind of its own (MaterialAsset.hpp:7-9: "no
        // snippet, no kind -- both come from the base at the end of the parent
        // chain"), so the base is the only level whose `kind` means anything.
        // Exactly the shape PostChainCache.cpp uses for its own surface
        // (":not a fullscreen material -- the post slot runs fullscreen chains
        // only").
        //
        // Without this, assigning a "sprite" or "fullscreen" .arcmat to a
        // MeshRenderer::materialOverride resolved SILENTLY: a fullscreen
        // material has no `baseColor` param at all, so the mesh drew plain
        // white; a sprite material that happens to declare one drew its
        // unrelated tint. Both are plausible authoring mistakes with an
        // indistinguishable symptom, and a white mesh is exactly what a
        // BROKEN reference looks like too. Refusing here turns all of that
        // into one diagnosable log line, and the refusal falls through the
        // same way any other failure does -- GpuSceneSync's material chain
        // simply moves on to its next link (MeshMaterialCache.hpp's FAILURE
        // DISCIPLINE block).
        const MaterialAssetData& base = chain.empty() ? *data : chain.back();
        if (MaterialSurfaceForKind(base.kind) != MaterialSurface::Mesh)
            return fail("not a mesh material -- MeshRenderer resolves \"mesh\"-kind "
                        "materials only (this one's base kind is '" + base.kind + "')");

        // Layer base -> ... -> immediate parent -> this asset, last write
        // wins -- the same order SpriteMaterialCache::Impl::Bind applies
        // saved values in (base's params <- ... <- this asset's params,
        // "overrides on top"), just without a MaterialInstance to apply them
        // onto. A level that declares no "baseColor" of its own leaves the
        // running value untouched, so an instance that overrides nothing
        // inherits its base's colour exactly as UMaterialInstance would.
        ResolvedMeshMaterial resolved;   // baseColor (1,1,1,1), albedo nil, materialSlot kInvalidSlot
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            if (auto c = OwnBaseColor(*it))
                resolved.baseColor = *c;
            if (auto a = OwnAlbedo(*it))
                resolved.albedo = *a;
            if (it->blend)
                resolved.blend = *it->blend;
            if (it->alphaCutoff)
                resolved.alphaCutoff = *it->alphaCutoff;
            if (it->twoSided)
                resolved.twoSided = *it->twoSided;
        }
        if (auto c = OwnBaseColor(*data))
            resolved.baseColor = *c;
        if (auto a = OwnAlbedo(*data))
            resolved.albedo = *a;
        if (data->blend)
            resolved.blend = *data->blend;
        if (data->alphaCutoff)
            resolved.alphaCutoff = *data->alphaCutoff;
        if (data->twoSided)
            resolved.twoSided = *data->twoSided;

        // F2b Task 11: resolve the declared albedo into a bindless slot
        // through the injected device seam -- see Services::
        // resolveAlbedoSlot's own comment for why this cache never reaches
        // NriTextureCache/BindlessTable itself. A nil albedo, or no callback
        // installed (every CPU test, and a host before its graph context
        // exists), leaves the default materialSlot -- kInvalidSlot's value,
        // mesh.hlsl's flat baseColor path.
        bool needsReresolve = false;
        if (resolved.albedo.IsValid() && im.services.resolveAlbedoSlot)
        {
            resolved.materialSlot = im.services.resolveAlbedoSlot(resolved.albedo);
            // A table at capacity (or a texture not resident yet) answers invalid;
            // that must NOT be stored flat for the life of the entry -- queue the
            // material so a later Request re-resolves it once a slot frees.
            needsReresolve = resolved.materialSlot == 0xFFFFFFFFu;
        }

        im.table.emplace(id, resolved);
        if (needsReresolve)
            im.reresolve.insert(id);
    }

    void MeshMaterialCache::Invalidate(const Guid& id)
    {
        if (const auto it = m_impl->table.find(id); it != m_impl->table.end())
            m_impl->ReleaseRef(it->second);
        m_impl->table.erase(id);
        m_impl->failed.erase(id);
        m_impl->reresolve.erase(id);
    }

    void MeshMaterialCache::Clear()
    {
        for (const auto& kv : m_impl->table)
            m_impl->ReleaseRef(kv.second);
        m_impl->table.clear();
        m_impl->failed.clear();
        m_impl->reresolve.clear();
    }

    void MeshMaterialCache::ResetAlbedoSlot(std::uint32_t slot)
    {
        for (auto& kv : m_impl->table)
        {
            if (kv.second.materialSlot != slot)
                continue;
            kv.second.materialSlot = 0xFFFFFFFFu;
            m_impl->reresolve.insert(kv.first);
        }
    }
}
