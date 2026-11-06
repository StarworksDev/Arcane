#pragma once
// The one follow path (spec s6.1 GET /remote/v1/stream; ruling P2 (f); Servitor
// P0 Task 22). An SSE subscriber owns its follow job: the listener publishes
// Opened / Closed for each stream, and ApplyFollowEvents turns them into jobs
// on the host main thread. `servitor logs --follow` and `servitor problems
// --follow` open these streams; log.read and problems.list never arm a job.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

class JobTable;       // Jobs.hpp
class RemoteListener; // Listener.hpp

// Main thread (HostSession::Drain, before the dispatcher steps the jobs).
// Takes the listener's SSE life events (TakeSseEvents(listener.Atoms())), in order:
//  - Opened, topic `logs`: arms command log.follow with user = &listener,
//    SetSubscriber(event id), progress.done = Log::LatestSeq(), so its first
//    step publishes only lines logged after the stream opened (ruling F4).
//  - Opened, topic `problems`: arms command problem.follow likewise, with
//    progress.done = the problem store's Snapshot().size(); it publishes only
//    rows that appear after it opened.
//  - Opened, any other topic (state, catalog, job:<id>): arms nothing.
//  - Closed: CancelBySubscriber(event id).
// Each step publishes with EnqueueSse(job.subscriberId, line), one compact
// JSON object per SSE event: a log.read record (LogRecordJson), a
// problems.list row (ProblemRowJson), or {"dropped": N} when the log ring
// evicted N lines the stream never saw. It advances progress.done and returns
// false: a follow job ends only when its subscriber closes. A follow job is
// never waiter-bound. A host that stops its listener calls this once more
// after Stop, so every follow job is cancelled before the listener goes away.
ARC_REMOTE_API void ApplyFollowEvents(JobTable& jobs, RemoteListener& listener);

} // namespace Arcane::Remote

#endif
