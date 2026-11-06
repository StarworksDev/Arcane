#pragma once
#include <Arcane/Modules/ArcaneRemoteApi.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// AM1: whether a fixed-tick host (the server, the runtime) advances its sim on
// this loop iteration. The host asks Consume once per iteration, after
// DrainFrame and before the sim step: Run and Step call Advance (and bump
// HostGate::frame), Idle skips it while the rest of the iteration (pump,
// publish, heartbeat, render) still runs. The remote play commands (Task 29)
// write it from the main thread inside Drain; the mutex keeps a reader on
// another thread (a test, a later status hook) consistent.
enum class TickDecision : std::uint8_t { Idle, Step, Run };

class ARC_REMOTE_API TickGate {
public:
    void SetRunning(bool running);
    void SetPaused(bool paused);
    // Raises the pending single-step count to at least n; it never adds. One
    // budget per host (final fix pass, G-1): overlapping play.step calls
    // share the ticks, so two step 10s from one frame run 10, not 20, and no
    // tick runs after every caller was answered. Each job tracks its own target.
    void RequestSteps(std::uint32_t n);
    // Lowers the pending count to at most `keep` (a play.step that ended
    // before its ticks ran gives back the share no other step still needs).
    void TrimSteps(std::uint32_t keep);
    // Drops every pending single step (play.stop, Task 29).
    void ClearSteps();
    // running && !paused: Run, and a leftover step count is cleared.
    // Otherwise a pending step is Step (and the count drops by one), else Idle.
    TickDecision Consume();
    bool Running() const;
    bool Paused() const;
    // The gate holds the sim: not running, or paused (held at start, or
    // stopped / paused by servitor). Pending single steps still run one tick
    // each while held; Held stays true through them.
    bool Held() const;
    std::uint32_t StepsRemaining() const;

private:
    mutable std::mutex m_mutex;
    bool m_running = true;
    bool m_paused = false;
    std::uint32_t m_steps = 0;
};

// Task 29 (ruling B): the windowed runtime's title while the remote gate
// holds its sim -- `base` plus " [held]" when held, `base` unchanged otherwise.
ARC_REMOTE_API std::string HeldWindowTitle(std::string_view base, bool held);

} // namespace Arcane::Remote

#endif
