// ResidencyAccount -- see the header for the two halves (device-less Admit,
// device-owned bookkeeping) and for the R1/R2 rulings.
//
// Include order: NRI headers first (NriCommon.hpp's rule), because the Log
// header reaches <windows.h>.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "ResidencyAccount.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderBudgetSettings.hpp>   // RenderMeshSettings, RenderTextureSettings, RenderResidencySettings

#undef ERROR

#include <algorithm>
#include <array>

namespace Arcane
{
    std::uint64_t UnusedSurplus(std::uint64_t used, std::uint64_t cap, std::uint64_t reserve) noexcept
    {
        if (used >= cap)
            return 0;
        if (used <= reserve)
            return cap > reserve ? cap - reserve : 0;
        return cap - used;
    }

    namespace
    {
        [[nodiscard]] bool SameIdentity(const ResidencyEntry& a, const ResidencyEntry& b) noexcept
        {
            if (a.id != b.id || a.slice != b.slice)
                return false;
            return a.slice == ResidencySlice::Mesh || a.space == b.space;
        }

        struct Totals
        {
            std::array<std::uint64_t, 2> used{};
            std::array<std::uint64_t, 2> cap{};
            std::array<std::uint64_t, 2> reserve{};

            [[nodiscard]] std::uint64_t Allowance(std::size_t s) const noexcept
            {
                const std::size_t o = 1u - s;
                return cap[s] + UnusedSurplus(used[o], cap[o], reserve[o]);
            }
            [[nodiscard]] bool OverSlice(std::size_t s) const noexcept { return used[s] > Allowance(s); }
            [[nodiscard]] bool OverTotal() const noexcept
            {
                return used[0] + used[1] > cap[0] + cap[1];
            }
            [[nodiscard]] bool Over() const noexcept { return OverSlice(0) || OverSlice(1) || OverTotal(); }
        };

        [[nodiscard]] Totals Measure(const std::vector<ResidencyEntry>& entries, const ResidencyCaps& caps) noexcept
        {
            Totals t;
            t.cap = { caps.meshCap, caps.textureCap };
            t.reserve = { Reservation(caps.meshCap, caps.reservationPercent),
                          Reservation(caps.textureCap, caps.reservationPercent) };
            for (const ResidencyEntry& e : entries)
                t.used[static_cast<std::size_t>(e.slice)] += e.bytes;
            return t;
        }

        // Older first; on an equal frame a texture before a mesh; then the smaller
        // Guid; then the smaller colour space (so two spaces of one Guid are ordered).
        [[nodiscard]] bool EvictsBefore(const ResidencyEntry& a, const ResidencyEntry& b) noexcept
        {
            if (a.lastDrawnFrame != b.lastDrawnFrame)
                return a.lastDrawnFrame < b.lastDrawnFrame;
            if (a.slice != b.slice)
                return a.slice == ResidencySlice::Texture;
            if (a.id != b.id)
                return a.id < b.id;
            return a.space < b.space;
        }
    }

    std::vector<ResidencyEviction> Admit(std::vector<ResidencyEntry>& entries,
                                         ResidencyEntry incoming,
                                         ResidencySlice requester,
                                         const ResidencyCaps& caps,
                                         std::uint64_t currentFrame)
    {
        incoming.lastDrawnFrame = currentFrame;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const ResidencyEntry& e) { return SameIdentity(e, incoming); }),
                      entries.end());
        entries.push_back(incoming);

        std::vector<ResidencyEviction> out;
        for (;;)
        {
            const Totals t = Measure(entries, caps);
            // Stop ONLY when all three predicates are false. Fitting the requester's
            // own cap is not a stop.
            if (!t.Over())
                break;

            std::size_t victim = entries.size();
            const auto consider = [&](std::size_t i)
            {
                if (entries[i].lastDrawnFrame == currentFrame)
                    return;   // drawn this frame: never a victim, on either slice
                if (victim == entries.size() || EvictsBefore(entries[i], entries[victim]))
                    victim = i;
            };

            // Arm 1: while the requester's slice is over, it pays from its own cold
            // entries. The reservation does not shield it from its own charge.
            if (t.OverSlice(static_cast<std::size_t>(requester)))
            {
                for (std::size_t i = 0; i < entries.size(); ++i)
                    if (entries[i].slice == requester)
                        consider(i);
            }
            // Arm 2: otherwise (or when arm 1 found nothing) the oldest cold entry on a
            // slice still above its reservation. A slice at or under it gives none.
            if (victim == entries.size())
            {
                for (std::size_t i = 0; i < entries.size(); ++i)
                {
                    const std::size_t s = static_cast<std::size_t>(entries[i].slice);
                    if (t.used[s] > t.reserve[s])
                        consider(i);
                }
            }
            if (victim == entries.size())
                break;   // every eligible cold entry is gone; the frame renders

            out.push_back(ResidencyEviction{ entries[victim].id, entries[victim].slice, entries[victim].space });
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(victim));
        }
        return out;
    }

    // -------------------------------------------------------------------------

    ResidencyAccount::ResidencyAccount(const ResidencyCaps& caps) : m_caps(caps) {}

    ResidencyCaps ResidencyAccount::LatchCaps()
    {
        ResidencyCaps caps;
        caps.meshCap = Settings<RenderMeshSettings>().residencyBudgetBytes;
        caps.textureCap = Settings<RenderTextureSettings>().residencyBudgetBytes;
        caps.reservationPercent = std::min(Settings<RenderResidencySettings>().reservationPercent, 100u);
        return caps;
    }

    void ResidencyAccount::DropPending(const void* owner, const Key& key)
    {
        const auto it = m_pending.find(owner);
        if (it == m_pending.end())
            return;
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const ResidencyEviction& e)
                                  { return MakeKey(e.id, e.slice, e.space) == key; }),
                   list.end());
    }

    std::vector<ResidencyEviction> ResidencyAccount::Charge(const void* owner, const ResidencyEntry& copy)
    {
        const Key key = MakeKey(copy.id, copy.slice, copy.space);
        Meta& meta = m_entries[key];
        bool found = false;
        for (Holder& h : meta.holders)
            if (h.owner == owner)
            {
                h.bytes = copy.bytes;
                found = true;
            }
        if (!found)
            meta.holders.push_back(Holder{ owner, copy.bytes });
        meta.bytes = 0;
        for (const Holder& h : meta.holders)
            meta.bytes += h.bytes;
        meta.lastDrawnFrame = m_tick;
        DropPending(owner, key);
        m_open.try_emplace(owner, m_tick);

        // The working list: every entry, with each one an open holder frame still
        // covers stamped as drawn NOW so Admit protects it (R2). The real stamps in
        // m_entries are never touched by this.
        std::uint64_t oldestOpen = m_tick;
        bool anyOpen = false;
        for (const auto& [o, began] : m_open)
        {
            oldestOpen = anyOpen ? std::min(oldestOpen, began) : began;
            anyOpen = true;
        }
        std::vector<ResidencyEntry> work;
        work.reserve(m_entries.size());
        for (const auto& [k, m] : m_entries)
        {
            if (k == key)
                continue;
            ResidencyEntry e{ k.id, k.slice, k.space, m.bytes, m.lastDrawnFrame };
            if (anyOpen && e.lastDrawnFrame >= oldestOpen)
                e.lastDrawnFrame = m_tick;
            work.push_back(e);
        }

        ResidencyEntry incoming{ copy.id, copy.slice, key.space, meta.bytes, m_tick };
        std::vector<ResidencyEviction> evicted = Admit(work, incoming, copy.slice, m_caps, m_tick);

        for (const ResidencyEviction& ev : evicted)
        {
            const Key vk = MakeKey(ev.id, ev.slice, ev.space);
            const auto vit = m_entries.find(vk);
            if (vit == m_entries.end())
                continue;
            for (const Holder& h : vit->second.holders)
                m_pending[h.owner].push_back(ev);
            m_entries.erase(vit);
        }

        if (!m_warnedOver && Measure(work, m_caps).Over())
        {
            m_warnedOver = true;
            const Totals t = Measure(work, m_caps);
            ARC_WARN("[nri-graph] ResidencyAccount: the protected set is over the device account "
                     "(meshes {} of {} bytes, textures {} of {} bytes) -- every cold entry is already "
                     "evicted, the frame is kept intact (further occurrences are silent)",
                     t.used[0], t.cap[0], t.used[1], t.cap[1]);
        }
        return evicted;
    }

    bool ResidencyAccount::Touch(const void* owner, const Guid& id, ResidencySlice slice,
                                 NriTextureCache::ColorSpace space)
    {
        const auto it = m_entries.find(MakeKey(id, slice, space));
        if (it == m_entries.end())
            return false;
        const bool held = std::any_of(it->second.holders.begin(), it->second.holders.end(),
                                      [owner](const Holder& h) { return h.owner == owner; });
        if (!held)
            return false;
        it->second.lastDrawnFrame = m_tick;
        m_open.try_emplace(owner, m_tick);
        return true;
    }

    void ResidencyAccount::Stamp(const Guid& id, ResidencySlice slice, NriTextureCache::ColorSpace space)
    {
        const auto it = m_entries.find(MakeKey(id, slice, space));
        if (it != m_entries.end())
            it->second.lastDrawnFrame = m_tick;
    }

    void ResidencyAccount::Uncharge(const void* owner, const Guid& id, ResidencySlice slice,
                                    NriTextureCache::ColorSpace space)
    {
        const Key key = MakeKey(id, slice, space);
        DropPending(owner, key);
        const auto it = m_entries.find(key);
        if (it == m_entries.end())
            return;
        auto& holders = it->second.holders;
        holders.erase(std::remove_if(holders.begin(), holders.end(),
                                     [owner](const Holder& h) { return h.owner == owner; }),
                      holders.end());
        if (holders.empty())
        {
            m_entries.erase(it);
            return;
        }
        it->second.bytes = 0;
        for (const Holder& h : holders)
            it->second.bytes += h.bytes;
    }

    void ResidencyAccount::UnchargeOwner(const void* owner)
    {
        for (auto it = m_entries.begin(); it != m_entries.end();)
        {
            auto& holders = it->second.holders;
            holders.erase(std::remove_if(holders.begin(), holders.end(),
                                         [owner](const Holder& h) { return h.owner == owner; }),
                          holders.end());
            if (holders.empty())
            {
                it = m_entries.erase(it);
                continue;
            }
            it->second.bytes = 0;
            for (const Holder& h : holders)
                it->second.bytes += h.bytes;
            ++it;
        }
        m_pending.erase(owner);
        m_open.erase(owner);
    }

    std::vector<ResidencyEviction> ResidencyAccount::TakePending(const void* owner)
    {
        const auto it = m_pending.find(owner);
        if (it == m_pending.end())
            return {};
        std::vector<ResidencyEviction> out = std::move(it->second);
        m_pending.erase(it);
        return out;
    }

    void ResidencyAccount::EndFrame(const void* owner)
    {
        m_open.erase(owner);
        ++m_tick;
    }

    std::uint64_t ResidencyAccount::Used(ResidencySlice slice) const noexcept
    {
        std::uint64_t n = 0;
        for (const auto& [k, m] : m_entries)
            if (k.slice == slice)
                n += m.bytes;
        return n;
    }

    bool ResidencyAccount::Holds(const void* owner, const Guid& id, ResidencySlice slice,
                                 NriTextureCache::ColorSpace space) const noexcept
    {
        const auto it = m_entries.find(MakeKey(id, slice, space));
        if (it == m_entries.end())
            return false;
        return std::any_of(it->second.holders.begin(), it->second.holders.end(),
                           [owner](const Holder& h) { return h.owner == owner; });
    }

    std::uint64_t ResidencyAccount::EntryBytes(const Guid& id, ResidencySlice slice,
                                               NriTextureCache::ColorSpace space) const noexcept
    {
        const auto it = m_entries.find(MakeKey(id, slice, space));
        return it == m_entries.end() ? 0u : it->second.bytes;
    }
}
