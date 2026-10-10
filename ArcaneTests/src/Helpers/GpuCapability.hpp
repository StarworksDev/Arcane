#pragma once

// A DECLARED CAPABILITY REQUIREMENT, checked against the actual device.
//
// This replaces `[gpu]` as a hazard gate. That tag had come to mean two
// unrelated things -- "needs an adapter" and "excluded from the recorded
// baseline figures" -- and the confusion cost a day on 2026-08-31. The tag now
// means only the second; THIS answers the first.
//
// Deliberately NOT modelled on UE's NonNullRHI, which decides from a
// command-line switch rather than the device and DROPS a failing test from the
// enumerated list, so the run silently omits it (research doc section E.3). A
// probe plus a reported SKIP is better on both counts: Catch2 reports skipped
// cases as skipped, so a GPU-less runner says how many tests it did not run
// instead of quietly running fewer.

#include <Arcane/Platform/Platform.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>

#include <catch2/catch_test_macros.hpp>

namespace Arcane::Test
{
    // Whether a real device can be created for `backend` on this machine.
    // Probed LAZILY on first call and cached per backend -- probing both at
    // startup would pay for device creation on every run that touches neither.
    // Never throws; a failed probe is a false, not an error.
    [[nodiscard]] bool BackendAvailable(GraphicsBackend backend);

    // The human name used in the skip message.
    [[nodiscard]] const char* BackendName(GraphicsBackend backend);

    // The backend a case runs on when it is about the engine, not about one
    // graphics API: D3D12 on Windows (where those cases were written and their
    // expectations measured -- unchanged), Vulkan everywhere else, where D3D12
    // does not exist. A case that IS about one API names it, in code and in its
    // "(d3d12)"/"(vulkan)" title, and keeps skipping where that API is absent.
    // kNativeBackendCli is the same choice in the hosts' --backend spelling,
    // for the witness cases that spawn a host process.
#if ARC_PLATFORM_WINDOWS
    inline constexpr GraphicsBackend kNativeBackend    = GraphicsBackend::D3D12;
    inline constexpr const char*     kNativeBackendCli = "dx12";
#else
    inline constexpr GraphicsBackend kNativeBackend    = GraphicsBackend::Vulkan;
    inline constexpr const char*     kNativeBackendCli = "vulkan";
#endif
}

// Skip the current test case, with a stated reason, when the backend is absent.
#define ARC_REQUIRE_BACKEND(backend)                                              \
    do {                                                                          \
        if (!::Arcane::Test::BackendAvailable(backend)) {                          \
            SKIP("no " << ::Arcane::Test::BackendName(backend)                     \
                       << " adapter on this machine");                             \
        }                                                                          \
    } while (false)
