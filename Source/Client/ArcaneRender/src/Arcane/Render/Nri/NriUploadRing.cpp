// See NriUploadRing.hpp for the RingLayout/NriUploadRing split and the
// NONE-backend [gpu] caveat. Include-order rule per NriCommon.hpp: NRI
// headers first -- Extensions/NRIDeviceCreation.h (pulled in transitively
// via NriDevice.hpp) declares nri::Message::ERROR, and <windows.h> (via
// Arcane/Base/Log.hpp -> spdlog) #defines ERROR via wingdi.h.
#include <NRI.h>

#include "NriUploadRing.hpp"

#include "NriCommon.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // GpuDeviceLostObserved -- the device-lost teardown gate

#include <algorithm>
#include <cstdint>

namespace Arcane
{
    namespace
    {
        // Ceiling-division alignment, NOT a bitmask: correct for ANY
        // positive alignment, not only a power of two. RingLayout::Allocate
        // documents (and ARC_ASSERTs, in debug) power-of-two as the
        // alignment it expects -- every NRI memoryAlignment field is one --
        // but a release build that gets a non-power-of-two `align` past the
        // compiled-out assert still rounds correctly rather than doing
        // something undefined.
        std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align)
        {
            if (align <= 1)
                return value;
            return ((value + align - 1) / align) * align;
        }
    }

    // ----------------------------------------------------------------------
    // RingLayout
    // ----------------------------------------------------------------------

    void RingLayout::Init(std::uint64_t capacityBytes)
    {
        m_capacity      = capacityBytes;
        m_cursor        = 0;
        m_highWater     = 0;
        m_overflowCount = 0;
    }

    void RingLayout::Reset()
    {
        m_cursor = 0;
    }

    RingLayout::AllocResult RingLayout::Allocate(std::uint64_t size, std::uint64_t align)
    {
        ARC_ASSERT(align != 0 && (align & (align - 1)) == 0,
                    "RingLayout::Allocate: align must be a power of two -- every NRI "
                    "memoryAlignment field is one; a release build still rounds correctly "
                    "via ceiling division, but callers must not rely on that");

        const std::uint64_t alignedOffset = AlignUp(m_cursor, align);

        // Overflow-safe: Fits compares via subtraction, not
        // `alignedOffset + size > capacity`, which could itself wrap for a
        // pathological `size` before the comparison ever ran.
        if (!Fits(size, align))
        {
            ++m_overflowCount;
            return AllocResult{ 0, false };
        }

        m_cursor    = alignedOffset + size;
        m_highWater = std::max(m_highWater, m_cursor);
        return AllocResult{ alignedOffset, true };
    }

    // ----------------------------------------------------------------------
    // NriUploadRing
    // ----------------------------------------------------------------------

    NriUploadRing::~NriUploadRing()
    {
        Destroy();
    }

    bool NriUploadRing::Init(NriDevice& device, std::uint64_t slotBytes)
    {
        // Re-Init on a live ring is very likely a caller bug (double
        // setup) -- tear down first rather than leaking the old buffers.
        if (m_device)
            Destroy();

        if (slotBytes == 0)
        {
            ARC_ERROR("[nri] NriUploadRing::Init: slotBytes must be nonzero");
            return false;
        }

        m_device      = &device;
        m_currentSlot = 0;
        m_slots.resize(FramesInFlight());

        const nri::CoreInterface& core = device.Core();

        // Covers every per-frame allocation kind the plan names for this
        // ring: constant buffers, and sprite VB/IB (plan: "per-frame data
        // (constant buffers, sprite VB/IB) will be ring allocations").
        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = slotBytes;
        bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER
                          | nri::BufferUsageBits::VERTEX_BUFFER
                          | nri::BufferUsageBits::INDEX_BUFFER;

        for (Slot& slot : m_slots)
        {
            slot.layout.Init(slotBytes);

            if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(device.Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                           0.0f, bufferDesc, slot.buffer))
                || !slot.buffer)
            {
                ARC_ERROR("[nri] NriUploadRing::Init: CreateCommittedBuffer (HOST_UPLOAD, {} bytes) failed",
                           slotBytes);
                Destroy();
                return false;
            }

            // Persistent map: once here, unmapped once in Destroy().
            // NONE-backend footgun (brief/plan): ImplNONE's MapBuffer
            // returns null unconditionally, so THIS is the line that makes
            // Init() fail on the NONE backend -- by construction, not a
            // special case. See the file header: nothing past this point is
            // reachable from a device-less [nri]~[gpu] test.
            slot.cpu = core.MapBuffer(*slot.buffer, 0, nri::WHOLE_SIZE);
            if (!slot.cpu)
            {
                ARC_ERROR("[nri] NriUploadRing::Init: MapBuffer failed -- the NONE backend "
                          "cannot persistently map (ImplNONE.cpp); this method must only be "
                          "reached from a [gpu] desk run, never a device-less [nri]~[gpu] test");
                Destroy();
                return false;
            }
        }

        return true;
    }

    void NriUploadRing::BeginFrame(std::uint32_t frameSlot)
    {
        ARC_ASSERT(frameSlot < m_slots.size(),
                    "NriUploadRing::BeginFrame: frameSlot out of range -- Init() was not "
                    "called, or frameSlot >= FramesInFlight()");
        if (frameSlot >= m_slots.size())
            return;   // release: no-op rather than an OOB slot access

        m_currentSlot = frameSlot;
        m_slots[frameSlot].layout.Reset();
    }

    NriUploadRing::Alloc NriUploadRing::Allocate(std::uint64_t size, std::uint64_t align)
    {
        if (m_currentSlot >= m_slots.size())
            return {};   // Init() was never called (or left no slots) -- sentinel, not UB

        Slot& slot = m_slots[m_currentSlot];
        const RingLayout::AllocResult result = slot.layout.Allocate(size, align);
        if (!result.ok)
            return {};   // overflow -- {nullptr, 0, nullptr}; caller reports + drops the draw

        Alloc alloc;
        alloc.buffer = slot.buffer;
        alloc.offset = result.offset;
        alloc.cpu    = static_cast<std::uint8_t*>(slot.cpu) + result.offset;
        return alloc;
    }

    bool NriUploadRing::Fits(std::uint64_t size, std::uint64_t align) const noexcept
    {
        if (m_currentSlot >= m_slots.size())
            return false;
        return m_slots[m_currentSlot].layout.Fits(size, align);
    }

    std::uint64_t NriUploadRing::SlotCapacity() const noexcept
    {
        return m_slots.empty() ? 0u : m_slots.front().layout.Capacity();
    }

    std::uint64_t NriUploadRing::HighWater(std::uint32_t slot) const
    {
        if (slot >= m_slots.size())
            return 0;
        return m_slots[slot].layout.HighWater();
    }

    void NriUploadRing::Destroy()
    {
        if (!m_device)
            return;

        const nri::CoreInterface& core = m_device->Core();

        // Unlike most Nri/ teardown, this does NOT route through the
        // device's Graveyard. DeviceWaitIdle() below already guarantees every
        // previously-submitted GPU operation has completed -- nothing is
        // left "in flight" that could still be reading one of these buffers,
        // so an immediate Unmap+Destroy is correct without any fence-based
        // deferral. Burying at a fixed sentinel value here would NOT be safe
        // for this class: Graveyard::Bury()
        // asserts nondecreasing fenceValue order across EVERY caller sharing
        // one device's graveyard (Graveyard.hpp), and NriUploadRing has no
        // way to know it is burying after the highest fenceValue some other
        // subsystem (the frame graph's own transient/imported-resource
        // teardown, Task 6+) has already buried there -- a fixed low
        // sentinel could violate that invariant and fatally assert in debug.
        // Idle-then-destroy sidesteps the question entirely.
        //
        // SKIPPED ON A LOST DEVICE. Same rule as
        // NriGraphContext's and ~NriDevice's: once the loss is observed this
        // call can only fail, and the destroys below happen anyway -- on VK
        // they are REQUIRED (vkDestroyDevice's valid usage, and VMA asserts on
        // survivors), and on D3D12 these staging buffers are upload-heap
        // memory the hung GPU is not reading. The healthy path is unchanged:
        // GpuDeviceLostObserved() is false for every ordinary shutdown.
        if (core.DeviceWaitIdle && !GpuDeviceLostObserved())
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        for (Slot& slot : m_slots)
        {
            if (slot.cpu)
                core.UnmapBuffer(*slot.buffer);
            if (slot.buffer)
                core.DestroyBuffer(slot.buffer);
        }
        m_slots.clear();
        m_device = nullptr;
    }
}
