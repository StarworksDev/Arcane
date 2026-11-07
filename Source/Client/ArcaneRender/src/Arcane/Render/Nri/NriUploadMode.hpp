#pragma once

// NriUploadMode -- how a residency cache gets its bytes onto the device.
//
// Deferred (the production mode, Plan C Task 5): Resolve of a fresh payload
// creates the device object, queues the upload and answers NOT-READY; the
// graph's `upload` node records the copy into a frame's own command buffer.
//
// Immediate: the pre-Task-5 path -- NRI's HelperInterface::UploadData, which
// submits and waits INSIDE Resolve. It survives for TESTS ONLY: the device-less
// cache cases that assert on a resident entry straight after Resolve, and the
// [gpu] byte-compare, which needs a reference picture from the old path. No
// production code selects it.

#include <cstdint>

namespace Arcane
{
    enum class NriUploadMode : std::uint8_t { Deferred, Immediate };

    // THE PER-FRAME RING BUDGET (Plan C Task 5 fix round 1, render.upload.frameBudgetPercent).
    // Upload runs first in the frame, so without a cap a burst of first-use payloads would
    // take the whole ring slot and starve Batch2D / overlay / GpuScene / pick of theirs. One
    // budget per context per frame, shared by both caches, applied at the SNAPSHOT:
    //   * a payload bigger than the cap goes to dedicated staging (it costs no ring bytes);
    //   * otherwise it is admitted FIFO while its ring bytes fit what is left, and the first
    //     one that does not stops the snapshot (later entries never overtake it). A fresh
    //     budget (remaining == cap) therefore always admits the first entry that is <= cap.
    struct UploadBudget
    {
        std::uint64_t cap       = ~0ull;   // unlimited by default
        std::uint64_t remaining = ~0ull;

        static constexpr UploadBudget WithCap(std::uint64_t capBytes) noexcept { return UploadBudget{ capBytes, capBytes }; }

        // True when the payload (ring bytes `ringBytes`) is admitted to this frame's snapshot.
        [[nodiscard]] constexpr bool Admit(std::uint64_t ringBytes) noexcept
        {
            if (ringBytes > cap)
                return true;           // dedicated staging: no ring bytes
            if (ringBytes > remaining)
                return false;          // waits for the next frame, FIFO
            remaining -= ringBytes;
            return true;
        }
    };
}
