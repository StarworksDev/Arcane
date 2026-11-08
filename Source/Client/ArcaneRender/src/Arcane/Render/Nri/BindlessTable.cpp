// BindlessTable -- see the header for the slot policy, the two release levels
// and the ownership rules.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>

#include "BindlessTable.hpp"

#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/ResidencyAccount.hpp>

#include <Arcane/Base/Log.hpp>

#undef ERROR

#include <algorithm>

namespace Arcane
{
    std::unique_ptr<BindlessTable> BindlessTable::Create(NriDevice& device, std::uint32_t capacity,
                                                         DeviceResourceLifetime& life)
    {
        if (capacity == 0)
        {
            ARC_ERROR("[nri] BindlessTable: refused -- capacity must be nonzero");
            return nullptr;
        }

        std::unique_ptr<BindlessTable> table(new BindlessTable());
        table->m_device   = &device;
        table->m_capacity = capacity;
        table->m_life     = &life;
        table->m_slots.reserve(capacity);
        return table;
    }

    std::unique_ptr<BindlessTable> BindlessTable::Create(NriDevice& device, std::uint32_t capacity)
    {
        auto owned = std::make_unique<DeviceResourceLifetime>();
        auto table = Create(device, capacity, *owned);
        if (table)
            table->m_ownedLife = std::move(owned);
        return table;
    }

    BindlessTable::~BindlessTable()
    {
        if (!m_device)
            return;
        bool any = !m_backings.empty();
        for (const Slot& s : m_slots)
            if (s.state != State::Free && s.srv) { any = true; break; }
        if (!any)
            return;

        ARC_WARN("[nri] BindlessTable destroyed with {} live descriptor(s) -- its owner never "
                 "called Release(). Destroying directly behind a DeviceWaitIdle.",
                 LiveCount() + PendingCount());
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (Slot& s : m_slots)
            if (s.state != State::Free && s.srv)
            {
                core.DestroyDescriptor(s.srv);
                s.srv = nullptr;
            }
        m_slots.clear();
        RunAllBackings(true);
    }

    std::uint32_t BindlessTable::LiveCount() const noexcept
    {
        std::uint32_t n = 0;
        for (const Slot& s : m_slots)
            if (s.state == State::Live)
                ++n;
        return n;
    }

    bool BindlessTable::IsLive(std::uint32_t slot) const noexcept
    {
        return slot < m_slots.size() && m_slots[slot].state == State::Live;
    }

    // ---------------------------------------------------------------- index level

    std::uint32_t BindlessTable::Add(nri::Descriptor* srv, const void* owner)
    {
        // A null descriptor is a caller-code bug, not the capacity condition
        // below -- refused silently, no slot consumed.
        if (!srv)
            return kInvalidSlot;

        // A retired index is preferred over appending, so the table only grows
        // when nothing can be recycled.
        ProcessPending();

        std::uint32_t slot = kInvalidSlot;
        if (!m_free.empty())
        {
            slot = m_free.back();
            m_free.pop_back();
        }
        else if (m_slots.size() < m_capacity)
        {
            slot = static_cast<std::uint32_t>(m_slots.size());
            m_slots.emplace_back();
        }
        else
        {
            ReclaimOldestUnreferenced();
            if (m_free.empty())
            {
                // Something pending is on its way to the free list, and the caller re-asks
                // on a later frame. With nothing pending the table really is full of
                // referenced slots.
                if (m_pending.empty() && !m_warnedFull)
                {
                    m_warnedFull = true;
                    ARC_WARN("[nri] BindlessTable: capacity {} exhausted -- Add refused, returning "
                             "kInvalidSlot (further occurrences are silent)",
                             m_capacity);
                }
                return kInvalidSlot;
            }
            slot = m_free.back();
            m_free.pop_back();
        }

        Slot& s = m_slots[slot];
        s = Slot{};
        s.srv   = srv;
        s.owner = owner;
        s.state = State::Live;
        s.mintedAt = ++m_mintCount;
        return slot;
    }

    void BindlessTable::ReclaimOldestUnreferenced()
    {
        // What is already pending covers that many asks: the same ask repeating every frame
        // until its slot retires must not cost one more mapping each time.
        ++m_refusedThisFrame;
        if (m_pending.size() >= m_refusedThisFrame)
            return;

        std::uint32_t oldest = kInvalidSlot;
        for (const auto& [key, mapping] : m_map)
        {
            if (mapping.refs != 0 || m_slots[mapping.slot].state != State::Live)
                continue;
            if (oldest == kInvalidSlot || m_slots[mapping.slot].mintedAt < m_slots[oldest].mintedAt)
                oldest = mapping.slot;
        }
        if (oldest == kInvalidSlot)
            return;
        // The mapping path: holders drop the index before it can mean anything else. It waits on
        // the last-use record, so if an earlier frame may still read it, it is free later, not now.
        DropMapping(oldest);
        PendIndex(oldest, false);
    }

    void BindlessTable::Release(std::uint32_t slot)
    {
        if (!IsLive(slot))
            return;
        DropMapping(slot);
        PendIndex(slot, false);
    }

    void BindlessTable::PendIndex(std::uint32_t slot, bool force)
    {
        m_slots[slot].state = State::Pending;
        m_pending.push_back(slot);
        if (TryRetire(slot, force))
            m_pending.pop_back();
    }

    bool BindlessTable::TryRetire(std::uint32_t slot, bool force)
    {
        Slot& s = m_slots[slot];
        if (!force && !m_life->AllRetired(slot, m_completed, m_completedUser))
            return false;

        // Every context that sampled the slot has finished with it: the
        // descriptor can go now (never buried in NriDevice::Graves(), never at
        // another context's fence), and the index is reusable.
        if (s.srv)
            m_device->Core().DestroyDescriptor(s.srv);
        m_life->Clear(slot);
        s = Slot{};
        m_free.push_back(slot);
        // A texture whose destruction joined this slot's retire goes after the
        // descriptor (descriptors before the resources they view).
        OnSlotRetired(slot);
        return true;
    }

    void BindlessTable::OnSlotRetired(std::uint32_t slot)
    {
        for (std::size_t i = 0; i < m_backings.size();)
        {
            Backing& b = m_backings[i];
            b.waiting.erase(std::remove(b.waiting.begin(), b.waiting.end(), slot), b.waiting.end());
            if (b.waiting.empty())
            {
                std::function<void()> destroy = std::move(b.destroy);
                m_backings.erase(m_backings.begin() + static_cast<std::ptrdiff_t>(i));
                if (destroy)
                    destroy();
            }
            else
            {
                ++i;
            }
        }
    }

    void BindlessTable::RunAllBackings(bool run)
    {
        std::vector<Backing> backings = std::move(m_backings);
        m_backings.clear();
        if (!run)
            return;
        for (Backing& b : backings)
            if (b.destroy)
                b.destroy();
    }

    bool BindlessTable::AdoptTextureBacking(const nri::Texture* texture, std::uint64_t fence,
                                            std::function<void()> destroy)
    {
        if (!texture)
            return false;
        Backing b;
        b.texture = texture;
        for (std::uint32_t i = 0; i < m_slots.size(); ++i)
        {
            const Slot& s = m_slots[i];
            if (s.state == State::Free || s.texture != texture)
                continue;
            b.waiting.push_back(i);
            // The owning context may still be reading its own view in a submit
            // that has not completed.
            if (s.owner)
                m_life->NoteSample(i, s.owner, fence);
        }
        if (b.waiting.empty())
            return false;
        b.destroy = std::move(destroy);
        m_backings.push_back(std::move(b));
        return true;
    }

    void BindlessTable::ProcessPending()
    {
        for (std::size_t i = 0; i < m_pending.size();)
        {
            if (TryRetire(m_pending[i], false))
            {
                m_pending[i] = m_pending.back();
                m_pending.pop_back();
            }
            else
            {
                ++i;
            }
        }
    }

    // ------------------------------------------------------------- the write layer

    void BindlessTable::WriteOne(nri::DescriptorSet* set, std::uint32_t slot, nri::Descriptor* srv)
    {
        if (m_writeHook)
        {
            m_writeHook(m_hookUser, set, slot, srv);
            return;
        }
        // `view`'s ADDRESS must outlive UpdateDescriptorRanges.
        const nri::Descriptor* view = srv;
        nri::UpdateDescriptorRangeDesc update = {};
        update.descriptorSet  = set;
        update.rangeIndex     = 0;      // the bindless array is the set's only range
        update.baseDescriptor = slot;   // WHERE in the array -- not an append
        update.descriptors    = &view;
        update.descriptorNum  = 1;
        m_device->Core().UpdateDescriptorRanges(&update, 1);
    }

    void BindlessTable::WriteSlot(std::uint32_t slot, nri::Descriptor* srv)
    {
        for (const SetEntry& e : m_sets)
            WriteOne(e.set, slot, srv);
    }

    bool BindlessTable::IdleOnce()
    {
        bool ok = true;
        if (m_idleHook)
            ok = m_idleHook(m_hookUser);
        else
            ok = ARC_NRI_CHECK(m_device->Core().DeviceWaitIdle(&m_device->Device()));
        if (ok)
            m_idledThisFrame = true;
        return ok;
    }

    void BindlessTable::RegisterSet(const void* owner, nri::DescriptorSet* set)
    {
        if (!set)
            return;
        bool known = false;
        for (SetEntry& e : m_sets)
            if (e.owner == owner) { e.set = set; known = true; break; }
        if (!known)
            m_sets.push_back(SetEntry{ owner, set });
        // A set created after the first materials must still hold them.
        for (std::uint32_t i = 0; i < m_slots.size(); ++i)
            if (m_slots[i].state == State::Live)
                WriteOne(set, i, m_slots[i].srv);
    }

    void BindlessTable::UnregisterSet(const void* owner)
    {
        m_sets.erase(std::remove_if(m_sets.begin(), m_sets.end(),
                                    [owner](const SetEntry& e) { return e.owner == owner; }),
                     m_sets.end());
    }

    std::uint32_t BindlessTable::AddAndWrite(nri::Descriptor* srv, const void* owner)
    {
        if (!srv)
            return kInvalidSlot;

        // The fallback path (no update-after-set): a rewrite may land on a set a
        // frame in flight reads, so the device idles -- at most once per frame
        // that rewrites a descriptor, not once per slot.
        if (!m_sets.empty() && !m_updateAfterSet && !m_idledThisFrame)
        {
            if (!IdleOnce())
            {
                ARC_ERROR("[nri] BindlessTable: could not retire in-flight work before "
                          "updating a bindless material descriptor");
                return kInvalidSlot;
            }
        }

        const std::uint32_t slot = Add(srv, owner);
        if (slot == kInvalidSlot)
            return kInvalidSlot;
        WriteSlot(slot, srv);
        return slot;
    }

    // ----------------------------------------------------------- mapping level

    std::uint32_t BindlessTable::Lookup(const Guid& guid, ColorSpace space) const
    {
        const auto it = m_map.find(MapKey{ guid, space });
        return it == m_map.end() ? kInvalidSlot : it->second.slot;
    }

    const nri::Texture* BindlessTable::MappedTexture(const Guid& guid, ColorSpace space) const
    {
        const std::uint32_t slot = Lookup(guid, space);
        return slot == kInvalidSlot || slot >= m_slots.size() ? nullptr : m_slots[slot].texture;
    }

    std::uint32_t BindlessTable::AddMapped(const Guid& guid, ColorSpace space, nri::Descriptor* srv,
                                           const void* owner, const nri::Texture* texture)
    {
        const std::uint32_t existing = Lookup(guid, space);
        if (existing != kInvalidSlot)
            return existing;

        const std::uint32_t slot = AddAndWrite(srv, owner);
        if (slot == kInvalidSlot)
            return kInvalidSlot;

        Slot& s = m_slots[slot];
        s.mapped = true;
        s.texture = texture;
        s.guid   = guid;
        s.space  = space;
        m_map[MapKey{ guid, space }] = Mapping{ slot, 0 };
        return slot;
    }

    void BindlessTable::AcquireRef(const Guid& guid, ColorSpace space)
    {
        const auto it = m_map.find(MapKey{ guid, space });
        if (it != m_map.end())
            ++it->second.refs;
    }

    void BindlessTable::Release(const Guid& guid, ColorSpace space)
    {
        const auto it = m_map.find(MapKey{ guid, space });
        if (it == m_map.end() || it->second.refs == 0)
            return;
        if (--it->second.refs == 0)
            m_zeroRefs.push_back(MapKey{ guid, space });   // released at the frame boundary unless re-acquired
    }

    void BindlessTable::FlushZeroRefs()
    {
        std::vector<MapKey> keys = std::move(m_zeroRefs);
        m_zeroRefs.clear();
        for (const MapKey& k : keys)
        {
            const auto it = m_map.find(k);
            if (it != m_map.end() && it->second.refs == 0)
                ReleaseAll(k.guid, k.space);
        }
        m_refusedThisFrame = 0;
        ProcessPending();
    }

    void BindlessTable::StampReferenced()
    {
        ResidencyAccount* account = m_device ? m_device->ResidencyIfCreated() : nullptr;
        if (!account)
            return;
        for (const auto& [key, mapping] : m_map)
            if (mapping.refs > 0)
                account->Stamp(key.guid, ResidencySlice::Texture,
                               key.space == ColorSpace::Display ? NriTextureCache::ColorSpace::Display
                                                                : NriTextureCache::ColorSpace::Srgb);
    }

    void BindlessTable::ReleaseAll(const Guid& guid, ColorSpace space)
    {
        const auto it = m_map.find(MapKey{ guid, space });
        if (it == m_map.end())
            return;
        const std::uint32_t slot = it->second.slot;
        DropMapping(slot);
        PendIndex(slot, false);
    }

    void BindlessTable::ReleaseEveryMapping()
    {
        std::vector<std::uint32_t> slots;
        slots.reserve(m_map.size());
        for (const auto& kv : m_map)
            slots.push_back(kv.second.slot);
        for (std::uint32_t slot : slots)
        {
            DropMapping(slot);
            PendIndex(slot, false);
        }
    }

    void BindlessTable::PurgeZeroRef(const MapKey& key)
    {
        m_zeroRefs.erase(std::remove(m_zeroRefs.begin(), m_zeroRefs.end(), key), m_zeroRefs.end());
    }

    void BindlessTable::DropMapping(std::uint32_t slot)
    {
        Slot& s = m_slots[slot];
        if (!s.mapped)
            return;
        m_map.erase(MapKey{ s.guid, s.space });
        s.mapped = false;
        PurgeZeroRef(MapKey{ s.guid, s.space });
        // Resolved materials and GPU rows holding this index fall back to
        // kInvalidSlot (and are re-staged) BEFORE the index can mean anything else.
        const auto observers = m_observers;   // copy: an observer may unregister itself
        for (const auto& o : observers)
            if (o.second)
                o.second(slot);
    }

    void BindlessTable::ReleaseOwnedBy(const void* owner, bool deviceIdle)
    {
        for (std::uint32_t i = 0; i < m_slots.size(); ++i)
        {
            Slot& s = m_slots[i];
            if (s.owner != owner)
                continue;
            if (s.state == State::Live)
            {
                DropMapping(i);
                PendIndex(i, deviceIdle);
            }
            else if (s.state == State::Pending && deviceIdle)
            {
                if (TryRetire(i, true))
                    m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), i), m_pending.end());
            }
        }
        ProcessPending();
    }

    void BindlessTable::NoteSampledBy(const void* context, std::uint64_t fence)
    {
        // A mapped texture sampled through a bindless slot is DRAWN, though no cache
        // Resolve ran for it this frame: stamp it on the device residency account so
        // a texture a live mesh material samples is never the account's oldest. An
        // orphaned mapping (no material reference) is not, so it ages.
        StampReferenced();
        for (std::uint32_t i = 0; i < m_slots.size(); ++i)
        {
            Slot& s = m_slots[i];
            if (s.state == State::Live)
            {
                m_life->NoteSample(i, context, fence);
            }
            else if (s.state == State::Pending
                     && std::find(s.pendingNotedBy.begin(), s.pendingNotedBy.end(), context)
                            == s.pendingNotedBy.end())
            {
                s.pendingNotedBy.push_back(context);
                m_life->NoteSample(i, context, fence);
            }
        }
        // The per-frame, per-context retire sweep.
        ProcessPending();
    }

    void BindlessTable::SetObserver(const void* key, const Observer& observer)
    {
        for (const auto& o : m_observers)
            if (o.first == key)
                return;
        m_observers.emplace_back(key, observer);
    }

    void BindlessTable::RemoveObserver(const void* key)
    {
        m_observers.erase(std::remove_if(m_observers.begin(), m_observers.end(),
                                         [key](const auto& o) { return o.first == key; }),
                          m_observers.end());
    }

    // ------------------------------------------------------------------ teardown

    void BindlessTable::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // One burial per occupied slot (Live or Pending): Add() transferred
        // ownership of each descriptor here, so this is what discharges it --
        // buried rather than destroyed directly.
        for (std::uint32_t i = 0; i < m_slots.size(); ++i)
        {
            Slot& s = m_slots[i];
            if (s.state == State::Free || !s.srv)
                continue;
            graveyard.Bury(fence, [core, p = s.srv] { core->DestroyDescriptor(p); });
            m_life->Clear(i);
        }
        m_slots.clear();
        m_free.clear();
        m_pending.clear();
        m_map.clear();
        m_zeroRefs.clear();
        // Textures that joined a slot's retire go after every descriptor.
        for (Backing& b : m_backings)
            if (b.destroy)
                graveyard.Bury(fence, std::move(b.destroy));
        m_backings.clear();
    }

    void BindlessTable::DestroyAllNow(bool destroy)
    {
        if (!m_device)
            return;
        const nri::CoreInterface& core = m_device->Core();
        for (Slot& s : m_slots)
            if (destroy && s.state != State::Free && s.srv)
                core.DestroyDescriptor(s.srv);
        m_slots.clear();
        m_free.clear();
        m_pending.clear();
        m_map.clear();
        m_zeroRefs.clear();
        RunAllBackings(destroy);
    }
}
