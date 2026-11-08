#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// Per-frame-slot upload arenas. There is no volatile-CB or
// writeBuffer-in-an-open-command-list idiom on the render
// path -- per-frame data (constant buffers, sprite VB/IB) is a ring
// allocation, bound to a draw via a root-descriptor OFFSET into one
// persistent, mapped UPLOAD-heap buffer per queued frame slot. Ring buffers
// are NOT render-graph resources: they bypass RenderGraph's barrier
// machinery entirely -- HOST_UPLOAD memory is host-visible/coherent, and the
// access patterns this phase uses (CPU writes before the GPU reads, guarded
// by the SAME frame-pacing fence the swapchain already waits on -- see
// NriSwapChain.hpp) need no barrier to be correct.
//
// Two halves, split for testability (the plan's Step 1 TDD requirement):
//
//   - RingLayout: pure bump-allocator math over ONE frame slot's byte
//     range. No NRI, no device -- fully unit-testable device-less, in
//     RenderGraphTest.cpp's "[nri]" cases. This is what every test drives
//     directly.
//   - NriUploadRing: the thin NRI-facing wrapper. Owns
//     FramesInFlight() RingLayouts plus one persistent-mapped
//     UPLOAD-heap nri::Buffer per slot.
//
// NONE-backend footgun (plan's constraints): ImplNONE's MapBuffer returns
// null UNCONDITIONALLY, so NriUploadRing::Init() FAILS on the NONE backend
// by construction -- every method here that touches a real nri::Buffer/
// nri::Device is therefore a [gpu] desk-verify item, never exercised by a
// device-less [nri]~[gpu] test. Do not add a device-backed unit test for this
// half; there is nothing for it to run against in CI or at the desk's own
// dev-loop gate. (BeginFrame()/Allocate()/HighWater() touch no NRI call
// directly, but they only ever operate on slots Init() populated -- calling
// any of them before a successful Init() is a caller bug, ARC_ASSERT'd in
// debug -- so they inherit the same "needs Init() first" gate and stay
// untested here too.)
//
// Include order: same rule as every other file in this directory (see
// NriCommon.hpp) -- NRI headers first, because Extensions/NRIDeviceCreation.h
// (pulled in transitively via NriDevice.hpp) declares nri::Message::ERROR,
// which <windows.h> (via Arcane/Base/Log.hpp -> spdlog, reachable from the
// .cpp) #defines away.
#include <NRI.h>

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/FramePacing.hpp>   // FramesInFlight()

#include <cstdint>
#include <vector>

namespace Arcane
{
    // -----------------------------------------------------------------
    // RingLayout -- pure bump-allocator math for ONE frame slot's byte
    // range. Every method here is deterministic and touches nothing but its
    // own four counters. NriUploadRing owns FramesInFlight() of
    // these; nothing else needs to, but nothing stops a caller from using
    // one standalone (which is exactly what the device-less tests do).
    // -----------------------------------------------------------------
    class ARC_RENDER_API RingLayout
    {
    public:
        struct AllocResult
        {
            std::uint64_t offset = 0;
            bool          ok     = false;   // false: overflow -- offset is meaningless, caller must not use it
        };

        RingLayout() = default;

        // (Re)configures capacity and resets EVERYTHING, including
        // highWater/overflowCount -- called once from
        // NriUploadRing::Init(), never mid-session. A default-constructed
        // (never-Init()'d) RingLayout has capacity 0, so every Allocate()
        // overflows rather than reading/writing out of bounds.
        void Init(std::uint64_t capacityBytes);

        // BeginFrame's half: cursor -> 0. highWater and overflowCount are
        // NOT touched -- both are lifetime peaks meant to survive every
        // reset, for NriUploadRing::HighWater()'s shutdown-log use.
        void Reset();

        // Rounds the current cursor up to `align` (documented, and
        // ARC_ASSERT-enforced in debug, as a power of two -- every NRI
        // memoryAlignment field is one, by both the D3D12 and VK specs),
        // then claims `size` bytes there. `align` <= 1 means "no
        // alignment" (offset == cursor). A zero-size allocation is legal
        // and succeeds exactly at the (aligned) cursor, claiming nothing.
        //
        // On overflow -- the aligned start plus size would exceed capacity
        // -- returns {0, false} and leaves the cursor UNCHANGED: never
        // wraps, never partially commits, never claims a byte outside
        // [0, capacity). overflowCount is bumped regardless of whether the
        // caller goes on to handle the failure gracefully, so a read at
        // shutdown still shows the ring ran dry even when every individual
        // frame recovered from it.
        [[nodiscard]] AllocResult Allocate(std::uint64_t size, std::uint64_t align);

        // Would `size` bytes at `align` fit what is left of this slot? EXACTLY the
        // negation of the overflow condition Allocate uses (Allocate calls this).
        // Does NOT bump the overflow count: the upload planner asks first, so a
        // payload it routes elsewhere (retry, dedicated staging) never counts as
        // a ring overflow. Claims nothing.
        [[nodiscard]] bool Fits(std::uint64_t size, std::uint64_t align) const noexcept
        {
            return FitsAt(m_capacity, m_cursor, size, align);
        }

        // The same predicate with the state passed in -- what Fits evaluates, and
        // what the pure staging classifier (NriUploadStaging.hpp) shares so there
        // is ONE definition of "fits". Subtraction form: never wraps.
        [[nodiscard]] static constexpr bool FitsAt(std::uint64_t capacity, std::uint64_t cursor,
                                                    std::uint64_t size, std::uint64_t align) noexcept
        {
            const std::uint64_t alignedOffset = align <= 1 ? cursor : ((cursor + align - 1) / align) * align;
            if (alignedOffset > capacity) return false;
            if (size > capacity - alignedOffset) return false;
            return true;
        }

        [[nodiscard]] std::uint64_t Capacity() const noexcept { return m_capacity; }
        [[nodiscard]] std::uint64_t Cursor() const noexcept { return m_cursor; }
        // Peak cursor value ever reached -- survives Reset(); see Reset()'s comment.
        [[nodiscard]] std::uint64_t HighWater() const noexcept { return m_highWater; }
        // Count of Allocate() calls that returned {., false} -- survives Reset().
        [[nodiscard]] std::uint64_t OverflowCount() const noexcept { return m_overflowCount; }

    private:
        std::uint64_t m_capacity      = 0;
        std::uint64_t m_cursor        = 0;
        std::uint64_t m_highWater     = 0;
        std::uint64_t m_overflowCount = 0;
    };

    // -----------------------------------------------------------------
    // NriUploadRing -- the NRI-facing wrapper. See the file header for the
    // NONE-backend / [gpu] caveat: nothing below has ever executed in this
    // tree -- it is a desk-verify item for Task 8+'s integration.
    // -----------------------------------------------------------------
    class ARC_RENDER_API NriUploadRing
    {
    public:
        struct Alloc
        {
            nri::Buffer*  buffer = nullptr;
            std::uint64_t offset = 0;
            void*         cpu    = nullptr;   // slot's mapped base + offset; write here directly
        };

        NriUploadRing() = default;
        ~NriUploadRing();

        // Owns live NRI objects tied to one device -- copying would either
        // double-free or alias a mapped pointer across two owners.
        NriUploadRing(const NriUploadRing&)            = delete;
        NriUploadRing& operator=(const NriUploadRing&) = delete;

        // Creates FramesInFlight() persistent-mapped UPLOAD-heap
        // buffers, each `slotBytes` long, and their matching RingLayouts.
        // [gpu]-only (see the file header): fails outright on the NONE
        // backend, because MapBuffer does. Returns false (already logged)
        // on any NRI failure or a zero `slotBytes`; anything partially
        // created is torn down before returning. `device` must outlive this
        // object (same contract as every other Nri/ wrapper).
        bool Init(NriDevice& device, std::uint64_t slotBytes);

        // Resets `frameSlot`'s bump offset to 0 (RingLayout::Reset()) and
        // makes it the CURRENT slot: every Allocate() until the next
        // BeginFrame() call lands here. Call once per frame, before that
        // frame's first Allocate(). `frameSlot` must be <
        // FramesInFlight() (ARC_ASSERT in debug; a release-mode
        // out-of-range call is a no-op, matching Allocate()'s never-UB
        // guarantee).
        void BeginFrame(std::uint32_t frameSlot);

        // Bump-allocates `size` bytes aligned to `align` out of the CURRENT
        // slot. Returns {nullptr, 0, nullptr} on overflow -- or if
        // BeginFrame()/Init() was never called -- and NEVER wraps or reads/
        // writes outside the slot's mapped range. The CALLER (Task 8+) is
        // responsible for reporting via NoteError("nri-graph", ...) and
        // dropping the draw; this class only counts the failure
        // (RingLayout::OverflowCount(), folded into the shutdown log
        // alongside HighWater()).
        [[nodiscard]] Alloc Allocate(std::uint64_t size, std::uint64_t align);

        // Forwards to the CURRENT slot's RingLayout::Fits; allocates nothing and
        // never bumps the overflow count. False before Init().
        [[nodiscard]] bool Fits(std::uint64_t size, std::uint64_t align) const noexcept;

        // The latched per-slot capacity (render.upload ring bytes per frame); 0
        // before Init().
        [[nodiscard]] std::uint64_t SlotCapacity() const noexcept;

        // Allocate calls that overflowed, summed over every slot (read-only, lifetime).
        [[nodiscard]] std::uint64_t OverflowCount() const noexcept;

        // Peak bytes ever claimed from `slot` across every BeginFrame()
        // reset since Init() -- meant to be read once, at shutdown, and
        // logged (not reset by BeginFrame()). 0 for an out-of-range slot.
        [[nodiscard]] std::uint64_t HighWater(std::uint32_t slot) const;

    private:
        struct Slot
        {
            RingLayout    layout;
            nri::Buffer*  buffer = nullptr;
            void*         cpu    = nullptr;   // persistent MapBuffer() result; unmapped once in Destroy()
        };

        void Destroy();   // shared by ~NriUploadRing and a failed Init()'s cleanup

        NriDevice*        m_device      = nullptr;
        std::vector<Slot> m_slots;
        std::uint32_t     m_currentSlot = 0;
    };
}
