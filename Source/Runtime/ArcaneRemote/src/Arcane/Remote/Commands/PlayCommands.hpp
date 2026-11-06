#pragma once
// play.start, play.stop, play.pause, play.resume and play.step (spec s7.3,
// AM1; Servitor P0 Task 29): the host side of `servitor play|stop|pause|resume|step`.
// All five are Drive commands (the dispatcher's read-only gate and audit
// apply), Mutates::PlayMode, needsFrame false, not world scoped, and answer
// undo {label: null, undoable: false, reason: PlayHooks::undoReason}.
//
// Runtime and server drive their AM1 TickGate: start and resume run it, stop
// holds it and drops pending steps, pause holds it. The editor drives its
// PlaySession and RunLoop through the function pointers below, which the
// editor program installs (R8-2: this module includes no editor header).
//
// play.step N is a frame-spanning job (s7.7, 0.12): the command arms one job
// and returns. N counts host.frame, the host's sim advances (AM1): one per
// fixed tick on a server, a headless runtime or a step, and one per loop pass
// on a windowed runtime that is running. It never calls Advance, AdvanceSim or RequestSingleStep itself.
// Runtime and server: the command raises the gate's one step budget to at
// least N (overlapping steps share it, G-1) and the host loop's Consume
// performs one Advance per iteration. Editor: while the primary loop is paused,
// each job step (one per Drain) requests one single step until the frame delta
// reaches N; a running loop advances by itself (G-2). The job completes when
// HostGate::frame minus the frame at arm time reaches N, or answers ok with
// data.timedOut (exit 8) at its host-side bound (BoundedJobTimeoutMs, C-I2),
// giving back the gate ticks no other running step still needs.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/FrameStats.hpp>
#include <Arcane/Remote/TickGate.hpp>

#include <string>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

class JobTable;  // Jobs.hpp
struct HostGate; // Dispatcher.hpp

struct PlayHooks {
    TickGate* gate = nullptr;                 // runtime and server
    bool (*editorPlay)(void*, std::string_view topology) = nullptr; // PlaySession::Play; topology is the CLI spelling
    bool (*editorStop)(void*) = nullptr;      // PlaySession::Stop
    void (*editorSetPaused)(bool paused, void*) = nullptr;
    void (*editorRequestSingleStep)(void*) = nullptr;
    void* editorUser = nullptr;
    bool editor = false;
    FrameStats* stats = nullptr;
    std::string undoReason; // "play-mode" on the editor, "runtime-host" on runtime and server
    // Task 29 addition (last, so the fields above keep their brace order):
    // the editor's PlaySession::IsPlaying. Pause, resume and step act only
    // inside Play on the editor (the toolbar disables them in Edit, where the
    // RunLoop's pause IS the edit-mode sim freeze), so they answer
    // play.not-playing when it returns false, and a running step ends
    // play.stopped when Play ends under it. Null: not checked.
    bool (*editorPlaying)(void*) = nullptr;
    // Task 29 fix round 1: the running Play session's topology in the CLI
    // spelling (PlaySession::Topology; "client" for ClientOnly). Read only
    // while editorPlaying is true: play.start on a playing editor answers the
    // ACTUAL topology, and an explicit different one is play.refused with
    // detail.running. Null: play.start always calls editorPlay.
    std::string_view (*editorTopology)(void*) = nullptr;
    // Final fix pass (G-2): whether the editor's PRIMARY RunLoop is paused.
    // play.step asks for a single step only while it is (a running loop
    // already advances host.frame each pass, as a running runtime does; a
    // single step there would replace the frame's accumulated dt). Null: read
    // as paused (one single step per Drain, the Task 29 behaviour).
    bool (*editorPaused)(void*) = nullptr;
};

// The CLI spellings of play.start's topology (spec s7.3): standalone (the
// default), listen, client+server. Anything else answers arg.invalid.
ARC_REMOTE_API bool IsPlayTopology(std::string_view topology);

// All three are safe to call twice; a later call replaces the stored hooks /
// pointer (ruling F11). A null JobTable* or HostGate* makes play.* answer
// host.unavailable (exit 5) without dereferencing it (ruling S3-9).
// RegisterPlayCommands with a different table (nullptr included) finishes
// every play.step job still running in the previous one (Cancelled,
// play.stopped, exit 6: ticks may already have run, so never the retryable
// host.unavailable), so no waiter is left parked on a table nobody steps.
ARC_REMOTE_API void SetPlayHooks(PlayHooks hooks);
ARC_REMOTE_API void SetPlayGate(HostGate* gate);       // the gate play.step reads its arm-time frame from
ARC_REMOTE_API void RegisterPlayCommands(JobTable* jobs);

} // namespace Arcane::Remote

#endif
