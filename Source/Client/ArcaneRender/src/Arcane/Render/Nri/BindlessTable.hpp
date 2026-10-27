#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// BindlessTable -- the descriptor array + slot allocator behind bindless SRV
// indexing, ONE PER NriDevice (3D-readiness arc spec 3.5, approach A).
//
// WHAT THIS IS ON THE NRI API: a flat table of shader-resource descriptors
// that back one descriptor set per sampling context (each MeshNode owns a set
// whose array range is the table's capacity; SupportsBindless()-tier hardware
// indexes it dynamically in the shader). The device owns the INDEX space; the
// sets are views of it. A slot index is one device-wide value, and a write to
// it updates EVERY registered set (RegisterSet), so a context never sees an
// index whose descriptor its own set lacks.
//
// SLOT POLICY: Add pops a retired free index, else appends at the next dense
// slot -- so a scene that only adds still receives 0..n-1 in add order, and a
// live slot never moves (GPU rows keep a stable index). Two release levels:
//
//   * INDEX level, Release(slot): the slot leaves service but is NOT free. It
//     waits (pending) until DeviceResourceLifetime::AllRetired says every
//     context that sampled it has completed the fence of the submit that did,
//     on that context's OWN timeline. Only then is the descriptor destroyed
//     and the index pushed on the free list. Pending slots are re-checked on
//     every Add and by ProcessPending(). Nothing is buried in
//     NriDevice::Graves() and no fence is ever compared across contexts.
//   * MAPPING level (the device memo, key = (Guid, ColorSpace) -- the texture
//     cache's own key): Release(guid, space) drops one material reference;
//     at zero ReleaseAll semantics apply. ReleaseAll(guid, space) is the
//     unconditional form (texture invalidation, eviction, project switch).
//     Either erases the memo, tells every observer the slot is gone (so
//     resolved materials and GPU rows holding it fall back to kInvalidSlot and
//     are re-staged by GpuSceneSync) and then takes the index-level path.
//
// OWNERSHIP: Add() takes ownership of the nri::Descriptor* it is handed -- the
// index-level destroy is what discharges it. The whole-table
// Release(Graveyard&, fence) is the TEARDOWN path (buries every occupied
// descriptor); if the owner never calls any of them, the destructor is the
// SAFETY NET behind a DeviceWaitIdle and says so at WARN.
//
// Not thread-safe: driven by the thread that records and submits.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR, and
// <windows.h>, dragged in transitively by Arcane/Base/Log.hpp -> spdlog,
// #defines ERROR via wingdi.h).
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/Nri/DeviceResourceLifetime.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;

    class ARC_RENDER_API BindlessTable
    {
    public:
        ARC_CONSTANT("sentinel: invalid bindless slot")
        static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

        using ColorSpace = NriTextureCache::ColorSpace;

        // Borrows `device` (which must outlive this object) for DestroyDescriptor
        // and the descriptor writes. `life` is the device-wide last-use record the
        // table recycles against; it must outlive the table. Refused (null,
        // logged) if capacity == 0.
        [[nodiscard]] static std::unique_ptr<BindlessTable> Create(NriDevice& device,
                                                                     std::uint32_t capacity,
                                                                     DeviceResourceLifetime& life);
        // Convenience: a table that owns its own record (device-less tests).
        [[nodiscard]] static std::unique_ptr<BindlessTable> Create(NriDevice& device,
                                                                     std::uint32_t capacity);

        // SAFETY NET, NOT THE PATH: destroys still-occupied descriptors directly
        // behind a DeviceWaitIdle and says so at WARN.
        ~BindlessTable();

        BindlessTable(const BindlessTable&)            = delete;
        BindlessTable& operator=(const BindlessTable&) = delete;

        [[nodiscard]] DeviceResourceLifetime& Lifetime() noexcept { return *m_life; }

        // The completion query AllRetired is asked with: "has `fence` completed on
        // `context`'s timeline". Unset, a slot with any note is never retired.
        void SetCompletionQuery(DeviceResourceLifetime::CompletedFn completed, void* user) noexcept
        {
            m_completed = completed;
            m_completedUser = user;
        }

        // ---------------- index level ----------------------------------------

        // Pops a retired free index, else appends. Refuses -- returns
        // kInvalidSlot, no slot consumed, no warning -- when `srv` is null; at
        // capacity with nothing recyclable it returns kInvalidSlot with a ONE-SHOT
        // warning. `owner` (a context identity, may be null) is whose objects the
        // descriptor views: ReleaseOwnedBy discharges them at that owner's teardown.
        // Does NOT write any descriptor set (see AddAndWrite).
        [[nodiscard]] std::uint32_t Add(nri::Descriptor* srv, const void* owner = nullptr);

        // Defers `slot` until AllRetired, then destroys its descriptor and pushes
        // the index on the free list. A slot that is not live is ignored.
        void Release(std::uint32_t slot);

        // Re-checks every pending slot against the last-use record.
        void ProcessPending();

        // ---------------- the write layer -------------------------------------

        // A context's bindless descriptor set. Every currently live slot is written
        // into it (a context created after the first materials still sees them).
        void RegisterSet(const void* owner, nri::DescriptorSet* set);
        void UnregisterSet(const void* owner);

        // Whether the device updates a bound set after the fact. When it does not,
        // the first descriptor write after NoteFrame() waits for device idle.
        void SetUpdateAfterSet(bool updateAfterSet) noexcept { m_updateAfterSet = updateAfterSet; }
        // One frame boundary: the next descriptor-rewriting Add may idle again.
        void NoteFrame() noexcept { m_idledThisFrame = false; }

        // Add + write the descriptor into every registered set. kInvalidSlot on
        // refusal (or an idle failure).
        [[nodiscard]] std::uint32_t AddAndWrite(nri::Descriptor* srv, const void* owner = nullptr);

        // Test seam: replaces the descriptor write and the device idle. Null
        // restores the NRI calls.
        using WriteFn = void (*)(void* user, nri::DescriptorSet* set, std::uint32_t slot, nri::Descriptor* srv);
        using IdleFn  = bool (*)(void* user);
        void SetHooks(WriteFn write, IdleFn idle, void* user) noexcept
        {
            m_writeHook = write;
            m_idleHook = idle;
            m_hookUser = user;
        }

        // ---------------- mapping level ---------------------------------------

        // The memoized slot for (guid, space), or kInvalidSlot.
        [[nodiscard]] std::uint32_t Lookup(const Guid& guid, ColorSpace space) const;

        // The texture object the mapped slot's descriptor views (as recorded by
        // AddMapped), or null when the key is unmapped or recorded no texture.
        [[nodiscard]] const nri::Texture* MappedTexture(const Guid& guid, ColorSpace space) const;

        // AddAndWrite + memoize under (guid, space). If the key is already mapped
        // the existing slot is returned and `srv` is NOT consumed (the caller keeps
        // ownership of it).
        // `texture` (optional) is the texture object the descriptor views; it lets
        // AdoptTextureBacking find the slots a texture's destruction must wait on.
        [[nodiscard]] std::uint32_t AddMapped(const Guid& guid, ColorSpace space,
                                              nri::Descriptor* srv, const void* owner = nullptr,
                                              const nri::Texture* texture = nullptr);

        // One more live material holds (guid, space). No-op for an unmapped key.
        void AcquireRef(const Guid& guid, ColorSpace space);
        // One material dropped its hold. At zero the key is NOT released at once:
        // it is queued, and FlushZeroRefs() (a frame boundary) releases it if it is
        // still at zero -- an AcquireRef before then cancels the release, so a
        // whole-cache Clear + re-resolve in one frame keeps every index (acquire
        // before release). A key with no references is untouched.
        void Release(const Guid& guid, ColorSpace space);
        // The frame boundary: ReleaseAll every queued key still at zero references.
        void FlushZeroRefs();
        // Unconditional: erase the memo, notify observers, take the index path.
        void ReleaseAll(const Guid& guid, ColorSpace space);
        // Project switch: ReleaseAll for every mapping.
        void ReleaseEveryMapping();
        // A context is tearing down (its textures die with it). Every slot whose
        // descriptor views that owner's objects is released; `deviceIdle` true
        // means the caller already drained the device, so the slots retire at once.
        void ReleaseOwnedBy(const void* owner, bool deviceIdle);

        // A context's texture cache is dropping `texture` (whose destruction
        // `destroy` performs: its own view, then the texture). If a live or pending
        // slot's descriptor views it, the destruction JOINS those slots' retire --
        // it runs once every one of them has retired (descriptor first, then
        // `destroy`) -- and the owning context's own pending submit (`fence` on the
        // slot owner's timeline) is noted so it is waited on too. Returns true when
        // adopted (the caller must NOT destroy or bury the texture); false when no
        // slot views it (the caller keeps its own path).
        [[nodiscard]] bool AdoptTextureBacking(const nri::Texture* texture, std::uint64_t fence,
                                               std::function<void()> destroy);
        [[nodiscard]] std::size_t BackingCount() const noexcept { return m_backings.size(); }

        // Notes, for every live slot, that `context` samples it in the submit that
        // signals `fence` on its timeline. A PENDING (released, not yet retired)
        // slot is noted by a context only on that context's FIRST record after the
        // release: the release observers fire synchronously, so holders re-staged
        // by then and later records do not use the index -- noting it on every
        // record would push its wait to a fresh fence each frame and it would
        // never retire. Also runs the retire sweep (ProcessPending), so a slot
        // retires as soon as its fences complete even when no Add happens.
        void NoteSampledBy(const void* context, std::uint64_t fence);
        // The context's timeline is gone; drop its notes.
        void ForgetContext(const void* context) { m_life->ForgetContext(context); }

        // Told, with the slot index, each time a MAPPED slot is released (the
        // mapping is already erased). One observer per `key`: SetObserver with a
        // key that is already registered leaves the existing one in place (so a
        // per-frame "ensure" is a scan, not a copy). The observer must not call
        // back into the table.
        using Observer = std::function<void(std::uint32_t slot)>;
        void SetObserver(const void* key, const Observer& observer);
        void RemoveObserver(const void* key);

        // ---------------- teardown / diagnostics -------------------------------

        // Buries every occupied slot's descriptor at `fence` via Graveyard::Bury --
        // never destroys directly -- then empties the table. Idempotent.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Device teardown: the caller has already drained the device. Destroys
        // every occupied descriptor directly (`destroy` false forgets them: a lost
        // D3D12 device leaks on purpose) and empties the table, quietly.
        void DestroyAllNow(bool destroy);

        [[nodiscard]] std::uint32_t Capacity() const noexcept { return m_capacity; }
        [[nodiscard]] std::uint32_t LiveCount() const noexcept;
        [[nodiscard]] std::uint32_t PendingCount() const noexcept { return static_cast<std::uint32_t>(m_pending.size()); }
        [[nodiscard]] std::uint32_t FreeCount() const noexcept { return static_cast<std::uint32_t>(m_free.size()); }
        [[nodiscard]] bool IsLive(std::uint32_t slot) const noexcept;

    private:
        BindlessTable() = default;

        enum class State : std::uint8_t { Free, Live, Pending };
        struct Slot
        {
            nri::Descriptor* srv   = nullptr;
            const void*      owner = nullptr;
            State            state = State::Free;
            bool             mapped = false;
            const nri::Texture* texture = nullptr;   // what the descriptor views (optional)
            // Pending only: the contexts that already noted this slot since it was
            // released (a context notes a pending slot ONCE, at its first record).
            std::vector<const void*> pendingNotedBy;
            Guid             guid{};
            ColorSpace       space = ColorSpace::Srgb;
        };
        struct MapKey
        {
            Guid       guid;
            ColorSpace space = ColorSpace::Srgb;
            bool operator==(const MapKey& o) const noexcept { return guid == o.guid && space == o.space; }
        };
        struct MapKeyHash
        {
            std::size_t operator()(const MapKey& k) const noexcept
            {
                return std::hash<Guid>{}(k.guid) * 31u + static_cast<std::size_t>(k.space);
            }
        };
        struct Mapping
        {
            std::uint32_t slot = kInvalidSlot;
            std::uint32_t refs = 0;
        };
        struct Backing
        {
            const nri::Texture*        texture = nullptr;
            std::vector<std::uint32_t> waiting;     // slots that must retire first
            std::function<void()>      destroy;
        };
        struct SetEntry
        {
            const void*         owner = nullptr;
            nri::DescriptorSet* set   = nullptr;
        };

        void PurgeZeroRef(const MapKey& key);                 // forget a queued zero-ref release
        void DropMapping(std::uint32_t slot);                 // erase memo + notify observers
        void PendIndex(std::uint32_t slot, bool force);       // Live -> Pending, retire now if possible
        bool TryRetire(std::uint32_t slot, bool force);       // Pending -> Free when AllRetired (or forced)
        void OnSlotRetired(std::uint32_t slot);               // backings waiting on it
        void RunAllBackings(bool run);                        // teardown
        void WriteSlot(std::uint32_t slot, nri::Descriptor* srv);
        void WriteOne(nri::DescriptorSet* set, std::uint32_t slot, nri::Descriptor* srv);
        [[nodiscard]] bool IdleOnce();

        NriDevice*                     m_device   = nullptr;
        std::uint32_t                  m_capacity = 0;
        DeviceResourceLifetime*        m_life     = nullptr;
        std::unique_ptr<DeviceResourceLifetime> m_ownedLife;
        DeviceResourceLifetime::CompletedFn m_completed = nullptr;
        void*                          m_completedUser  = nullptr;

        std::vector<Slot>              m_slots;     // size() is the high-water mark, <= m_capacity
        std::vector<std::uint32_t>     m_free;      // retired indices, LIFO
        std::vector<std::uint32_t>     m_pending;   // released, waiting on AllRetired
        std::unordered_map<MapKey, Mapping, MapKeyHash> m_map;
        std::vector<SetEntry>          m_sets;
        std::vector<Backing>           m_backings;
        std::vector<MapKey>            m_zeroRefs;  // keys whose last reference dropped this frame

        std::vector<std::pair<const void*, Observer>> m_observers;

        WriteFn m_writeHook = nullptr;
        IdleFn  m_idleHook  = nullptr;
        void*   m_hookUser  = nullptr;
        bool    m_updateAfterSet  = true;
        bool    m_idledThisFrame  = false;

        // THE ONE-SHOT CAPACITY WARN: one line per table, not one per refused Add().
        bool    m_warnedFull = false;
    };
}
