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

#include <Arcane/Core/Constant.hpp>

#include <cstddef>
#include <cstdint>

namespace Arcane
{
    enum class NriUploadMode : std::uint8_t { Deferred, Immediate };

    // THE PER-FRAME UPLOAD BUDGET (Plan C Task 5 fix round 1, render.upload.frameBudgetPercent;
    // spec Revision 6 for the over-cap half). Upload runs first in the frame, so without a cap
    // a burst of first-use payloads would take the whole ring slot and starve Batch2D / overlay
    // / GpuScene / pick of theirs. One budget per context per frame, shared by both caches,
    // applied at the SNAPSHOT:
    //   * a payload bigger than the cap goes to dedicated staging (it costs no ring bytes), and
    //     AT MOST ONE such payload is admitted per frame, because each one is a main-thread
    //     memcpy plus a committed HOST_UPLOAD allocation: a hundred 2K textures landing in one
    //     frame would be the first-use hitch this node exists to remove;
    //   * otherwise it is admitted FIFO while its ring bytes fit what is left;
    //   * the first payload refused stops the snapshot (later entries never overtake it). A
    //     fresh budget therefore always admits the first entry, whatever its size: progress holds.
    struct UploadBudget
    {
        std::uint64_t cap          = ~0ull;   // unlimited by default
        std::uint64_t remaining    = ~0ull;
        bool          overCapTaken = false;   // this frame's one over-cap payload is spoken for

        static constexpr UploadBudget WithCap(std::uint64_t capBytes) noexcept { return UploadBudget{ capBytes, capBytes, false }; }

        // True when the payload (ring bytes `ringBytes`) is admitted to this frame's snapshot.
        [[nodiscard]] constexpr bool Admit(std::uint64_t ringBytes) noexcept
        {
            if (ringBytes > cap)
            {
                if (overCapTaken)
                    return false;      // waits for the next frame, FIFO
                overCapTaken = true;
                return true;           // dedicated staging: no ring bytes
            }
            if (ringBytes > remaining)
                return false;          // waits for the next frame, FIFO
            remaining -= ringBytes;
            return true;
        }
    };

    // How many times a dedicated staging buffer that could not be created or mapped is tried
    // before the upload is memoized as refused (spec Revision 6): the failure may belong to a
    // burst and not to this payload, so one failure proves nothing about the next frame.
    ARC_CONSTANT("retry bound: dedicated staging tries before an upload is refused (spec Revision 6)")
    inline constexpr std::uint32_t kMaxStagingTries = 3;

    // THE ONE-SHOT RENDERERS' BOUND (a thumbnail harvest, a cover capture). A picture is not
    // final while uploads are on their way, so the renderer renders again. How many frames that
    // takes grows with the content, because the budget above paces it: a mesh with five over-cap
    // sections needs five upload frames. So the bound is on PROGRESS, not on frames: `maxStalled`
    // extra frames may pass without the queue reaching a new low (editor.thumbnail.maxRetries),
    // and every new low starts that count again. It terminates: the low only ever falls, and it
    // cannot fall below zero.
    struct UploadSettleBound
    {
        std::int32_t maxStalled   = 0;
        std::size_t  lowestQueued = 0;   // start it at the queue length the first frame left
        std::int32_t stalled      = 0;

        // Called before each extra frame with the queue length the last frame left.
        [[nodiscard]] constexpr bool AllowAnotherFrame(std::size_t queued) noexcept
        {
            if (queued < lowestQueued)
            {
                lowestQueued = queued;
                stalled = 0;
                return true;
            }
            return stalled++ < maxStalled;
        }
    };
}
