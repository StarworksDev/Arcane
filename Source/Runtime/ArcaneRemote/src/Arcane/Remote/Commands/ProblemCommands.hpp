#pragma once
// problems.list (spec s7.1; Servitor P0 Task 22): the host side of `servitor
// problems`. It reads the host's DiagnosticStore (ArcaneCore): the editor's
// Problems-panel store, or the store a runtime or server session owns and
// installs as the engine's diagnostics sink. The live follow is not this
// command: `servitor problems --follow` opens GET /remote/v1/stream?topics=problems,
// and ApplyFollowEvents (Follow.hpp) arms the subscriber's problem.follow job
// (ruling P2 (f)). HostSession::Start installs the store and registers the
// command (ruling F12); its teardown releases the store.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Json.hpp>

#if !defined(ARC_BUILD_DIST)

namespace Arcane
{
class DiagnosticStore; // Arcane/Base/DiagnosticStore.hpp
struct Diagnostic;     // Arcane/Base/Diagnostics.hpp
}

namespace Arcane::Remote
{

// Registers, once per process (a second call returns), one Observe host
// command on the main thread with needsFrame false, not worldScoped (the store
// is process-wide, so the dispatcher refuses --world):
//   problems.list {severity?, search?, follow?} -> data {problems: [row]}
// The rows are store.Filtered(mask, search): `severity` error | warning | info
// selects exactly that SeverityMask (omitted: all three); `search` is passed
// unchanged, the text MatchesDiagnosticFilter sees (case-insensitive, in the
// message, code or detail). Row shape: ProblemRowJson. Each row whose entity
// locator cannot be resolved to a live entity with Identity adds its handle to
// one `entity.no-identity` warning. follow: true is arg.inapplicable (exit 2)
// and arms nothing (ruling P2 (f)). No store: host.unavailable (exit 5). Other
// refusals: arg.unknown / arg.invalid (exit 2).
ARC_REMOTE_API void RegisterProblemCommand();
// Safe to call twice; a later call replaces the stored pointer (ruling F11).
// Null: problems.list answers host.unavailable (exit 5) and a problem.follow
// stream publishes nothing.
ARC_REMOTE_API void SetProblemStore(DiagnosticStore* store);
// Clears the stored pointer when it is `store` (HostSession's teardown); a
// store another session installed since is left alone.
ARC_REMOTE_API void ReleaseProblemStore(const DiagnosticStore* store);
// The stored store (main thread; problem.follow's step reads it). Null when none.
ARC_REMOTE_API DiagnosticStore* ProblemStore();

// One problem row: {severity, scope, code, message, detail, locator}.
// severity is error | warning | info, scope project | assets | scene | plugin
// | material | shader, locator null (kind None) or {kind, ...}:
//   entity      {kind, handle, guid}  guid {hi, lo} when the handle is alive in
//               the host's one world (CommandWorlds) and has Identity, else null
//               and *unresolvedEntity (when non-null) is set
//   asset       {kind, guid}
//   file        {kind, file, line, col}
//   graph-node  {kind, owner, node}
// Main thread: an entity locator is resolved against the live registry.
ARC_REMOTE_API nlohmann::json ProblemRowJson(const Diagnostic& diagnostic, bool* unresolvedEntity);

} // namespace Arcane::Remote

#endif
