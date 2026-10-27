#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// ResidencyAccount -- ONE residency account per device (3D-readiness arc spec
// 3.x, Plan C Task 4). Meshes and textures are two SLICES of one account:
// each has its own cap, a slice may borrow what the other leaves unused, and
// each slice keeps a reservation (a percent of its own cap) the other may not
// borrow. Every NriGraphContext on a device has its own NriMeshBufferCache and
// NriTextureCache; all of them charge THIS account, so the budget is the
// device's, not per vehicle.
//
// TWO HALVES, split the way MeshResidencyBudget.hpp splits policy from carrier:
//   * Admit(...) -- the free, DEVICE-LESS policy. A vector of entries in, the
//     evicted entries out. It touches no NRI, so the locked 700 MiB case and
//     every tie-break are provable with plain numbers.
//   * ResidencyAccount -- the device-owned bookkeeping around it: one entry per
//     (Guid, slice, ColorSpace) per device, the holders (caches) of that entry,
//     the device clock, and a per-holder queue of evictions waiting to be
//     carried out. It owns no GPU object and calls nothing back: a cache
//     drains its own queue at its own frame boundary (TakePending) and buries
//     the object through ITS OWN lane and the last-use rules. So an eviction
//     decided while context B admits is carried out by context A's cache on
//     A's timeline -- never on B's fence.
//
// R1 -- THE SAME KEY IN TWO CACHES. The viewport and a preview may each hold a
// GPU copy of one (Guid, slice, space). The account keeps ONE entry for it:
// `bytes` is the SUM of the live copies (each is real VRAM), `lastDrawnFrame` is
// the newest draw by any holder, and evicting it queues a drop for EVERY holder.
// A cache that drops a key itself (invalidate, release, teardown) uncharges its
// copy; the entry disappears at zero copies.
//
// R2 -- ONE CLOCK, AND WHY THIS ONE. lastDrawnFrame and currentFrame come from
// one device-wide counter (`Now()`), never from a context's own m_frameIndex
// (two contexts' frame counters mean nothing against each other). Each holder's
// frame is OPEN from its first draw/charge after its previous boundary until it
// calls EndFrame (which advances the clock). Admission protects every entry
// stamped at or after the START of the oldest still-open holder frame, not just
// entries stamped with the current tick: an entry drawn by ANY context in its
// current, not-yet-finished frame is never a victim, whichever context admits.
// A holder with no open frame (it drew nothing since its last boundary) pins
// nothing, so an idle preview cannot freeze eviction forever.
//
// Not thread-safe: one account per device, driven by the thread that records.

#include <Arcane/Guid.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    enum class ResidencySlice : std::uint8_t { Mesh, Texture };

    struct ResidencyEntry
    {
        Guid id{};
        ResidencySlice slice = ResidencySlice::Mesh;
        // Texture identity includes ColorSpace. Mesh entries leave this at Srgb
        // and do not use it. NriTextureCache::Key is (Guid, ColorSpace)
        // (NriTextureCache.hpp). One Guid resident in both spaces is two entries.
        NriTextureCache::ColorSpace space = NriTextureCache::ColorSpace::Srgb;
        std::uint64_t bytes = 0;
        std::uint64_t lastDrawnFrame = 0;
    };

    struct ResidencyEviction
    {
        Guid id{};
        ResidencySlice slice = ResidencySlice::Mesh;
        NriTextureCache::ColorSpace space = NriTextureCache::ColorSpace::Srgb;
    };

    struct ResidencyCaps
    {
        std::uint64_t meshCap = 0;
        std::uint64_t textureCap = 0;
        std::uint32_t reservationPercent = 50;
    };

    [[nodiscard]] inline std::uint64_t Reservation(std::uint64_t cap, std::uint32_t percent) noexcept
    {
        return cap * static_cast<std::uint64_t>(percent) / 100ull; // multiply, then divide
    }

    // 0 when used >= cap; cap - reserve when used <= reserve; cap - used otherwise.
    [[nodiscard]] ARC_RENDER_API std::uint64_t UnusedSurplus(std::uint64_t used, std::uint64_t cap,
                                                             std::uint64_t reserve) noexcept;

    // Charges `incoming` onto `requester` first, with lastDrawnFrame = currentFrame
    // (an entry already in `entries` with the same identity is replaced: `incoming`
    // carries the identity's total bytes). Evicts whole entries until
    // overSlice(mesh), overSlice(tex), and overTotal are all false.
    // Returns the evicted entries in order, each with Guid, slice, and ColorSpace.
    // The incoming entry is never a victim (its lastDrawnFrame is currentFrame),
    // and neither is any entry whose lastDrawnFrame == currentFrame.
    [[nodiscard]] ARC_RENDER_API std::vector<ResidencyEviction> Admit(
        std::vector<ResidencyEntry>& entries,
        ResidencyEntry incoming,
        ResidencySlice requester,
        const ResidencyCaps& caps,
        std::uint64_t currentFrame);

    class ARC_RENDER_API ResidencyAccount
    {
    public:
        explicit ResidencyAccount(const ResidencyCaps& caps);

        // The caps from render.mesh.residencyBudgetBytes,
        // render.texture.residencyBudgetBytes and render.residency.reservationPercent.
        [[nodiscard]] static ResidencyCaps LatchCaps();

        [[nodiscard]] const ResidencyCaps& Caps() const noexcept { return m_caps; }
        // The device clock.
        [[nodiscard]] std::uint64_t Now() const noexcept { return m_tick; }

        // `owner` (a cache) now holds a copy of `copy`'s identity of `copy.bytes`
        // bytes, stamped drawn at Now(). Returns the entries Admit evicted (each is
        // also queued for every holder; see TakePending). Re-charging a key the
        // owner already holds replaces that holder's byte count; re-charging a key
        // with a queued drop cancels the drop.
        std::vector<ResidencyEviction> Charge(const void* owner, const ResidencyEntry& copy);

        // A draw by `owner`: stamps the entry at Now() and opens the owner's frame.
        // False when the owner does not hold the key (it was evicted or never
        // charged) -- the caller re-charges.
        bool Touch(const void* owner, const Guid& id, ResidencySlice slice,
                   NriTextureCache::ColorSpace space);

        // A draw seen from below the cache (a bindless slot sampled by a mesh
        // pass): stamps the entry at Now() without opening any holder's frame.
        void Stamp(const Guid& id, ResidencySlice slice, NriTextureCache::ColorSpace space);

        // `owner` dropped its copy itself.
        void Uncharge(const void* owner, const Guid& id, ResidencySlice slice,
                      NriTextureCache::ColorSpace space);
        // `owner` is going away: every copy, its queue, its open frame.
        void UnchargeOwner(const void* owner);

        // The evictions queued for `owner`, which it now carries out. Empties the queue.
        [[nodiscard]] std::vector<ResidencyEviction> TakePending(const void* owner);

        // `owner`'s frame ended (its boundary): closes its open frame, advances the clock.
        void EndFrame(const void* owner);

        // Introspection (tests, diagnostics).
        [[nodiscard]] std::uint64_t Used(ResidencySlice slice) const noexcept;
        [[nodiscard]] std::size_t EntryCount() const noexcept { return m_entries.size(); }
        [[nodiscard]] bool Holds(const void* owner, const Guid& id, ResidencySlice slice,
                                 NriTextureCache::ColorSpace space) const noexcept;
        [[nodiscard]] std::uint64_t EntryBytes(const Guid& id, ResidencySlice slice,
                                               NriTextureCache::ColorSpace space) const noexcept;

    private:
        struct Key
        {
            Guid id{};
            ResidencySlice slice = ResidencySlice::Mesh;
            NriTextureCache::ColorSpace space = NriTextureCache::ColorSpace::Srgb;
            bool operator==(const Key&) const noexcept = default;
        };
        struct KeyHash
        {
            std::size_t operator()(const Key& k) const noexcept
            {
                const std::size_t h = std::hash<Guid>{}(k.id);
                return h ^ ((static_cast<std::size_t>(k.slice) * 2u + static_cast<std::size_t>(k.space))
                            + 0x9E3779B9u + (h << 6) + (h >> 2));
            }
        };
        struct Holder
        {
            const void*   owner = nullptr;
            std::uint64_t bytes = 0;
        };
        struct Meta
        {
            std::vector<Holder> holders;
            std::uint64_t bytes = 0;            // sum of the holders' copies
            std::uint64_t lastDrawnFrame = 0;   // newest draw by any holder
        };

        [[nodiscard]] static Key MakeKey(const Guid& id, ResidencySlice slice,
                                         NriTextureCache::ColorSpace space) noexcept
        {
            // Mesh entries do not use the colour space: normalise it away.
            return Key{ id, slice, slice == ResidencySlice::Mesh ? NriTextureCache::ColorSpace::Srgb : space };
        }
        void DropPending(const void* owner, const Key& key);

        ResidencyCaps m_caps;
        std::uint64_t m_tick = 1;
        std::unordered_map<Key, Meta, KeyHash> m_entries;
        std::unordered_map<const void*, std::uint64_t> m_open;   // owner -> tick its open frame began
        std::unordered_map<const void*, std::vector<ResidencyEviction>> m_pending;
        bool m_warnedOver = false;
    };
}
