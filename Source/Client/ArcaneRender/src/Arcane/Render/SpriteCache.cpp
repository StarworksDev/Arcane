#include <Arcane/Render/SpriteCache.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <utility>

namespace Arcane
{
    // THERE IS NO KEEP-ALIVE MAP, because there is nothing to keep alive:
    // residency is NriTextureCache's job exclusively, keyed off
    // SpriteEntry::textureId, and no SpriteEntry carries a GPU texture for
    // this cache to pin. That is also why this file includes no Batcher2D.hpp.
    struct SpriteCache::Impl
    {
        Services services;
        std::unordered_map<Guid, SpriteEntry> table;
        // SpriteTable::generation's storage (see Generation()): ++ at every
        // site below that inserts into, erases from, or clears `table`.
        std::uint64_t generation = 1;
    };

    SpriteCache::SpriteCache(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);
    }

    SpriteCache::~SpriteCache() { delete m_impl; }

    const std::unordered_map<Guid, SpriteEntry>& SpriteCache::Table() const
    {
        return m_impl->table;
    }

    const std::uint64_t* SpriteCache::Generation() const noexcept
    {
        return &m_impl->generation;
    }

    void SpriteCache::Request(const Guid& id)
    {
        if (!id.IsValid() || m_impl->table.contains(id))
            return;

        // Negative-result caching: emplace a DEFAULT SpriteEntry (1x1 m,
        // untextured -- SceneCacheRecords.hpp:114-123's field defaults) directly
        // into the PUBLISHED table -- unlike SpriteMaterialCache::Request,
        // which keeps a failed material OUT of its published table via a
        // separate `failed` set (SpriteMaterialCache.cpp:47,95-101) so the
        // sprite just falls back to the plain (unmaterialed) pipeline. A
        // sprite has no such fallback -- the component's whole point is to
        // draw a specific asset -- so a broken one renders as the VISIBLE
        // placeholder instead of vanishing. Either structure gets the same
        // never-re-parsed-once-known effect via the table.contains(id) guard
        // above.
        auto cacheDefault = [&](const char* why)
        {
            ARC_WARN("SpriteCache: sprite {} unavailable -- {}", id.ToString(), why);
            m_impl->table.emplace(id, SpriteEntry{});
            ++m_impl->generation;   // a publish (the placeholder): SpriteTable::generation
        };

        if (!m_impl->services.resolveAsset)
            return cacheDefault("no asset resolver installed");
        const auto path = m_impl->services.resolveAsset(id);
        if (!path)
            return cacheDefault("not in the asset registry");
        const auto data = LoadSpriteAsset(*path);
        if (!data)
            return cacheDefault("asset failed to load");

        // Success: the entry always carries the asset's own pivot. UV/size
        // resolve only when the texture Guid is valid AND the Assets facade
        // yields pixels for it -- a memoized prior load failure returns null
        // here too, same as an unset texture Guid. Either way
        // ComputeSpriteGeom(data, 0, 0) is the documented 1x1 m / full-UV
        // fallback (SpriteAsset.hpp:64-66, SpriteAsset.cpp:104-106), so the
        // untextured case needs no special-case geometry of its own.
        SpriteEntry entry;
        entry.pivot = data->pivot;
        // The DEVICE-FREE key, and the ONLY key: it names the asset the
        // render path's NriTextureCache uploads onto its own device. Set
        // unconditionally, including when the image has no decodable pixels --
        // the record must still say WHICH image it wanted.
        entry.textureId = data->texture;

        std::uint32_t texWidth = 0, texHeight = 0;
        if (data->texture.IsValid() && m_impl->services.assets)
        {
            // Geometry's dimensions come from TextureInfoFor (ABI v21) -- device-free,
            // so this resolves with no render device bound, which is the only
            // configuration there is. NOT PixelsFor: since ABI v21, PixelsFor serves a
            // THUMBNAIL for artifact-backed content (small, preview-sized pixels),
            // while TextureInfoFor serves the TRUE source dims from the artifact's own
            // header -- geometry needs the latter, and reading dims off PixelsFor here
            // would be exactly the wrong-dims bug class this split exists to prevent
            // (F2b Task 6).
            //
            // THE DIMENSIONS ARE COPIED OUT IMMEDIATELY, and that ordering is
            // the contract, not a style choice (Assets.hpp: "valid only until
            // evicted -- callers that need it to outlive the current call must
            // copy it, not hold the pointer"). TextureInfoFor returns a BARE pointer
            // into the facade's LRU-budgeted metadata cache and drops its own pin
            // before returning, so ANY later Assets call may free it and read
            // it back as garbage. This was a REAL use-after-free once (against the
            // old PixelsFor route): a
            // second Assets call in this function, whose trailing
            // EnforceBudget() could evict the globally least-recently-used
            // entry across every cache in the facade -- included the one this
            // pointer came from -- and then its width was read after it. There is
            // no second call today, so the hazard is structurally absent
            // rather than merely avoided; the copy stays anyway, because it
            // costs nothing and it is what keeps the NEXT Assets call added
            // here from being a bug.
            const TextureInfo* info = m_impl->services.assets->TextureInfoFor(data->texture);
            if (info)
            {
                texWidth  = info->width;
                texHeight = info->height;
            }
        }

        // THERE IS NO entry.texture TO RESOLVE: a SpriteEntry carries no GPU
        // texture at all. `entry.textureId` above is what the render path
        // resolves through NriTextureCache, and TextureInfoFor is the one
        // dimension source.
        const ResolvedSpriteGeom g = ComputeSpriteGeom(*data, texWidth, texHeight);
        entry.uvMin = g.uvMin;
        entry.uvMax = g.uvMax;
        entry.sizeMeters = g.sizeMeters;

        m_impl->table.emplace(id, entry);
        ++m_impl->generation;   // a publish: BoundsSystem re-walks (SpriteTable::generation)
    }

    // NO EVICT-BEFORE-RELEASE HOOK IS OWED: this cache holds no GPU object,
    // so an invalidation is exactly the one erase below. (`Services::batcher`
    // went with that hook -- nothing else in this file ever used it.)
    void SpriteCache::Invalidate(const Guid& id)
    {
        m_impl->table.erase(id);
        ++m_impl->generation;   // the entry is gone; the re-resolve bumps again
    }

    void SpriteCache::Clear()
    {
        m_impl->table.clear();
        ++m_impl->generation;   // every entry is gone (SpriteTable::generation)
    }
}
