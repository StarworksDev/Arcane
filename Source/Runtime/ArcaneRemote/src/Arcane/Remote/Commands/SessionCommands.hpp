#pragma once
// host.quit and host.wait (spec s4.7, s7.1; Servitor P0 Task 16): the session
// commands every armed host serves. HostSession::Start registers them with its
// own session and job table (ruling F12); HostSession::Stop releases them.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Remote/Protocol.hpp>

#include <chrono>
#include <cstdint>
#include <string>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

class HostSession; // HostSession.hpp
class JobTable;    // Jobs.hpp

// The longest a host-side wait runs, whatever options.timeoutMs asks for. The
// listener and servitor's --timeout both admit up to the u32 maximum of
// milliseconds (~49 days); a wait asking for more than this is bounded here,
// and its final envelope carries a wait.timeout-clamped warning. Servitor's
// default --timeout (30 s) and every acceptance timeout are far below it.
ARC_CONSTANT("safety bound: a wait settled freezes the host clock while it captures, and nothing cancels a wait whose client went away, so a host-side wait must end; ten minutes outlasts any real settle, frame or reload wait")
inline constexpr std::uint32_t kMaxWaitTimeoutMs = 600000;

// The host-side bound of a frame-spanning job (final fix pass, C-I2 / C-M12):
// requestedMs (fallbackMs when 0), never more than kMaxWaitTimeoutMs. play.step
// and the capture and compare jobs use it, as host.wait's poll does.
ARC_REMOTE_API std::uint32_t BoundedJobTimeoutMs(std::uint32_t requestedMs, std::uint32_t fallbackMs);

struct WaitArgs {
    std::string what;          // ready | settled | frames | reload | state
    std::uint32_t frames = 0;  // Optional
    std::string state;         // Optional
    std::uint32_t timeoutMs = 30000; // milliseconds, copied from Call::timeoutMs; host.wait does not scale
};

// One poll of a host.wait condition (ruling F9): reads session.Gate() and
// session.GetConfig() through the const accessors; `started` is when the wait
// job was armed. Returns, in order of precedence:
//  - `settled` on a host with no renderer (a server), or one that installed
//    no capture host pair: capture.no-renderer (exit 2; spec AM1: settled
//    captures, and captures are refused there); on a host with no
//    producersIdle hook: host.unavailable (exit 5);
//  - `reload` when a reload noted after the arm (session.ReloadSerial() moved
//    past reloadSerialAtArm) failed and the generation did not move:
//    module.reload-failed (exit 6), detail {pluginGeneration, atFrame};
//  - the condition met: MakeOk(host.wait, {done: true, what, ...}) (exit 0);
//  - timeoutMs == 0, or more than min(timeoutMs, kMaxWaitTimeoutMs) since
//    `started`: MakeOk(host.wait, {timedOut: true, what}) -- ok, no errors,
//    exit 8;
//  - otherwise MakeOk(host.wait, {done: false, what}): still pending.
// `ready` is state ready, playing or paused; `frames` is gate.frame >=
// startedFrame + frames (host.frame counts sim advances, AM1); `reload` is
// gate.pluginGeneration != generationAtArm (HostSession::NoteReload moves it;
// PluginHost leaves the generation alone on a failed or refused reload, which
// DrainFrame notes from its attempt count); `state` is gate.State() == state.
// `settled` is never met here: the host.wait job decides it from its own
// captures (spec s4.7: two byte-equal frozen-clock captures through the
// capture host pair, with config.producersIdle true when each was armed and
// when it retired), so a poll of `settled` answers only its refusals, its
// timeout, or pending. The host freezes its own clock for the captures
// (SettleHoldsClock below, read through HostSession::SettleClockFrozen).
ARC_REMOTE_API Envelope WaitPoll(const HostSession& session, const WaitArgs& args, std::uint64_t startedFrame,
                                 std::uint32_t generationAtArm, bool hasRenderer,
                                 std::chrono::steady_clock::time_point started,
                                 std::uint64_t reloadSerialAtArm = 0);

// True while a `wait settled` armed on `session` is in its capture phase: from
// the first capture armed with the producers idle until the wait ends (done,
// timed out, failed, or released with the session), except while a busy
// producer has broken the pair and no capture is in flight. Only a wait whose
// job is still Running (or Queued) holds it, so a job ended any other way
// cannot leave the clock frozen. The host reads it
// (HostSession::SettleClockFrozen) into the same freeze its --settle hold uses
// (frame and sim dt 0), so the two captures see one frozen clock. The freeze
// lifts while a producer is busy because it also stops the compile service's
// debounce clock: a compile submitted before the freeze could otherwise never
// dispatch, and the wait could never settle. Main thread only. Module-internal.
bool SettleHoldsClock(const HostSession* session);

// Registers host.quit and host.wait. Safe to call twice; a later call replaces
// the stored pointers (ruling F11). Null pointers make both commands answer
// host.unavailable (exit 5) without dereferencing.
ARC_REMOTE_API void RegisterSessionCommands(HostSession* session, JobTable* jobs);
// Clears the stored pointers when they name `session` (HostSession's teardown),
// so a stopped session's commands answer host.unavailable instead of reaching
// a dead object. Another session's pointers are left alone.
ARC_REMOTE_API void ReleaseSessionCommands(const HostSession* session);

} // namespace Arcane::Remote

#endif
