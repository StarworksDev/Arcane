#pragma once
// log.read (spec s7.1; Servitor P0 Task 22): the host side of `servitor logs`.
// It reads ArcaneCore's sequenced log ring (Log::ReadSince, Task 2) on the main
// thread. The live follow is not this command: `servitor logs --follow` opens
// GET /remote/v1/stream?topics=logs, and ApplyFollowEvents (Follow.hpp) arms
// the subscriber's log.follow job (ruling P2 (f)). HostSession::Start installs
// its job table and registers the command (ruling F12); its teardown releases
// the table.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Json.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Arcane
{
struct LogRecord; // Arcane/Base/Log.hpp
}

namespace Arcane::Remote
{

class JobTable; // Jobs.hpp

// Registers, once per process (a second call returns), one Observe host
// command on the main thread with needsFrame false, not worldScoped (the log
// ring is process-wide, so the dispatcher refuses --world):
//   log.read {since?, level?, follow?} -> data {records: [{seq, time, level, logger, text}], dropped}
// `since` is exclusive (records with seq > since; default 0, the whole ring).
// `dropped` is the gap between `since` and the oldest retained seq: lines the
// ring had already evicted. `level` (trace, debug, info, warn, warning, error,
// critical) drops records below it; it does not change `dropped`. `time` is
// unix milliseconds; `level` in a record is spdlog's name ("warning").
// follow: true is arg.inapplicable (exit 2) and arms nothing: the follow path
// is the stream (ruling P2 (f)). Other refusals: arg.unknown / arg.invalid (exit 2).
ARC_REMOTE_API void RegisterLogCommand();
// The table the session's jobs live in. Safe to call twice; a later call
// replaces the stored pointer (ruling F11). log.read never arms a job (follow
// is refused), so a null table changes nothing it answers.
ARC_REMOTE_API void SetLogJobs(JobTable* jobs);
// Clears the stored pointer when it is `jobs` (HostSession's teardown); a table
// another session installed since is left alone.
ARC_REMOTE_API void ReleaseLogJobs(const JobTable* jobs);

// The wire shape of one ring record, {seq, time, level, logger, text}: the
// log.read row and the log.follow stream event share it.
ARC_REMOTE_API nlohmann::json LogRecordJson(const LogRecord& record);
// The spdlog level a log.read `level` names (trace, debug, info, warn,
// warning, error, critical); nullopt for anything else.
ARC_REMOTE_API std::optional<int> ParseLogLevel(std::string_view name);

} // namespace Arcane::Remote

#endif
