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
        bool any = false;
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
            if (!m_warnedFull)
            {
                m_warnedFull = true;
                ARC_WARN("[nri] BindlessTable: capacity {} exhausted -- Add refused, returning "
                         "kInvalidSlot (further occurrences are silent)",
                         m_capacity);
            }
            return kInvalidSlot;
        }

        Slot& s = m_slots[slot];
        s = Slot{};
        s.srv   = srv;
        s.owner = owner;
        s.state = State::Live;
        return slot;
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

    std::uint32_t BindlessTable::AddMapped(const Guid& guid, ColorSpace space, nri::Descriptor* srv,
                                           const void* owner)
    {
        const std::uint32_t existing = Lookup(guid, space);
        if (existing != kInvalidSlot)
            return existing;

        const std::uint32_t slot = AddAndWrite(srv, owner);
        if (slot == kInvalidSlot)
            return kInvalidSlot;

        Slot& s = m_slots[slot];
        s.mapped = true;
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
            ReleaseAll(guid, space);
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

    void BindlessTable::DropMapping(std::uint32_t slot)
    {
        Slot& s = m_slots[slot];
        if (!s.mapped)
            return;
        m_map.erase(MapKey{ s.guid, s.space });
        s.mapped = false;
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
        for (std::uint32_t i = 0; i < m_slots.size(); ++i)
            if (m_slots[i].state == State::Live)
                m_life->NoteSample(i, context, fence);
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
    }
}
