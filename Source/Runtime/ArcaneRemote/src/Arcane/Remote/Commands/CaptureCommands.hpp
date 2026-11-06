#pragma once
// capture.viewport and capture.window (spec s4.8 / s9; Servitor P0 Task 27):
// the host side of `servitor screenshot viewport|window`. A render-ticket
// readback job (rulings P2 (a), P3 (a)): the first step arms one copy through
// the host's hook, later steps drain that same ticket, and the job completes
// when the ticket retires on the render side -- never on host.frame, never by
// idling the device. ArcaneRemote includes no render header (R8-2): the host
// program installs the hooks below, which forward to its NriGraphContext's
// capture ring (ArmTicketCapture / DrainTicketCapture).
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Registry.hpp>

#include <cstdint>
#include <filesystem>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

class JobTable; // Jobs.hpp

// One retired copy as the host's drain hook hands it over: padded rows in the
// render target's format. The job normalizes them (NormalizeCaptureRows).
struct CaptureMapped {
    std::uint32_t width = 0, height = 0, rowPitch = 0;
    bool bgra = false;
    const std::uint8_t* mapped = nullptr; // null and drain returns true: the map failed
    std::uint32_t mappedBytes = 0;
};
// Main thread. Arm: store `ticket` as the next recorded frame's copy; false =
// nothing will record it (capture.no-frame). Drain: false until that ticket's
// copy has retired; then true with *raw filled (mapped null = lost).
using ArmCopyFn = bool (*)(std::uint64_t ticket, void* user);
using DrainSlotFn = bool (*)(std::uint64_t ticket, CaptureMapped* raw, void* user);
// Main thread (final fix pass, C-M12): the job gave up on `ticket` (it failed,
// or the settle wait that armed it ended). The host forgets it in its ring:
// NriGraphContext::CancelTicketCapture.
using CancelCopyFn = void (*)(std::uint64_t ticket, void* user);
struct CaptureHooks {
    std::uint32_t framesInFlight = 2; // host ring only; the job does not wait on host.frame
    ArmCopyFn armViewport = nullptr, armWindow = nullptr, armHost = nullptr;
    DrainSlotFn drainViewport = nullptr, drainWindow = nullptr, drainHost = nullptr;
    // A windowed host: some drivers return black for an occluded window, so a
    // capture.window envelope carries the capture.occluded-risk warning (spec s9).
    bool occludedRisk = false;
    void* user = nullptr;
    // Where a capture without --out is written. Empty: HostSession::Start
    // fills it with the session's artifact directory (<remote dir>/out).
    std::filesystem::path outDir;
    // Final fix pass (C-M12): each pair's cancel; null = the ring keeps it.
    CancelCopyFn cancelViewport = nullptr, cancelWindow = nullptr, cancelHost = nullptr;
};
// Both are safe to call twice; a later call replaces the stored hooks / pointer
// (ruling F11). A null JobTable* makes capture.* answer host.unavailable (exit 5).
// RegisterCaptureCommands(nullptr) also finishes every capture job still
// running in the previous table (Cancelled, host.unavailable) and frees its
// state, so no job outlives the hooks it would call.
ARC_REMOTE_API void SetCaptureHooks(CaptureHooks hooks);
ARC_REMOTE_API void RegisterCaptureCommands(JobTable* jobs);
// host.busy {state: "capture-pending"} (exit 5): a capture or compare is
// already in flight on this host. Provably not run; the client retries it.
ARC_REMOTE_API CommandResult CaptureBusy(const Call& call);
// A capture.viewport, capture.window or capture.compare job is Running.
ARC_REMOTE_API bool CaptureInFlight(const JobTable& jobs);

} // namespace Arcane::Remote

#endif
