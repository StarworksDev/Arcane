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
}
