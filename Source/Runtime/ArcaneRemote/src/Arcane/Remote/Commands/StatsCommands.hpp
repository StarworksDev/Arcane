#pragma once
// stats.frame (Servitor P0 Task 26): the host side of `servitor stats frame`.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/FrameStats.hpp>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// Registers stats.frame on the first call and stores the pointer. Safe to call
// twice; a later call replaces the stored pointer (ruling F11). Null: stats.frame
// answers host.unavailable (exit 5) and dereferences nothing (ruling S3-9).
// stats.frame {} -> data {frame, dtSeconds, tickMs, renderMs}: a read of the
// last snapshot, needsFrame false, process-wide (not world scoped). Errors:
// arg.invalid / arg.unknown (exit 2), host.unavailable (exit 5).
ARC_REMOTE_API void RegisterStatsCommand(FrameStats* stats);
// Clears the stored pointer when it is `stats` (HostSession's teardown); a
// pointer another session installed since is left alone.
ARC_REMOTE_API void ReleaseStatsCommand(const FrameStats* stats);

} // namespace Arcane::Remote

#endif
