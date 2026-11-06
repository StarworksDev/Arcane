#pragma once
// FrameStats: the last sim advance a host performed (Servitor P0 Task 26,
// stats.frame). One per HostSession; the host's advance branch writes it through
// HostSession::NoteSimAdvance, the command reads it on the main thread inside
// Drain. FramePerf is not used: it accumulates only under diagnostics.perfLog
// and resets its window.
//
// Clocks (the same ones host.frame uses, AM1): `frame` is the host.frame value
// after the advance (a sim tick on a server and a headless runtime; a render
// pass on a windowed runtime in Run mode, where only Step is exactly one tick).
// `dtSeconds` is the host frame dt of that advance (fixed under --headless,
// the wall delta otherwise); `tickMs` is the wall time of the sim advance;
// `renderMs` is the wall time of the most recent completed render pass (0 on a
// server and until a render completes). A held host (no sim advance) never
// writes, so the snapshot stays the last real advance.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>

#include <cstdint>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

struct FrameStats {
    std::uint64_t frame = 0;
    double dtSeconds = 0;
    double tickMs = 0;
    double renderMs = 0;
};

ARC_REMOTE_API void NoteAdvance(FrameStats& stats, std::uint64_t frame, double dtSeconds, double tickMs, double renderMs);

} // namespace Arcane::Remote

#endif
