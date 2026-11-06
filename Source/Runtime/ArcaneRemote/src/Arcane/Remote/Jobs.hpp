#pragma once
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// The internal job table (spec s7.7). A command that must wait for a frame, a
// tick, a settle or a reload arms a job and returns; the dispatcher steps each
// Running job once per Drain. Public `servitor job` / `--detach` are P3.

struct JobId { std::uint64_t value = 0; };

enum class JobState : std::uint8_t { Queued, Running, Done, Failed, Cancelled };

struct JobProgress {
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::string label;
};

struct HostGate;      // defined in Dispatcher.hpp, which includes this header (ruling F1)
class RequestQueue;   // Listener.hpp (Task 6)
struct Job;
using JobStepFn = bool (*)(Job& job, HostGate& gate, void* user); // true = finished

struct Job {
    JobId id;
    std::string command;
    JobState state = JobState::Queued;
    JobProgress progress;
    std::uint64_t startedFrame = 0;
    Envelope envelope;
    std::int64_t finishedUnixMs = 0;
    JobStepFn step = nullptr;
    void* user = nullptr;
    std::uint64_t waiterId = 0; // RequestQueue id to Complete when the job finishes; 0 = none
    std::optional<UndoBlock> undo;  // the arming envelope's undo block (BindWaiter); copied onto the final envelope
    std::uint64_t subscriberId = 0; // SSE follow jobs only; 0 = not a follow job
};

// Main-thread table; every method locks one mutex. A step runs outside the
// lock on a copy of its row, so a step may arm or finish other jobs.
// RequestQueue::Complete is called after the lock is released.
class ARC_REMOTE_API JobTable {
public:
    // Ids start at 1. The row starts Running with progress {0, total} and an
    // ok envelope for `command` (MakeOk, empty data). A null step is legal.
    JobId Arm(std::string command, std::uint64_t total, JobStepFn step, void* user);
    // Task 8's Drain calls this with the queue key and the arming envelope's undo block (ruling F2).
    void BindWaiter(JobId id, std::uint64_t waiterId, std::optional<UndoBlock> undo = std::nullopt);
    void SetSubscriber(JobId id, std::uint64_t subscriberId);
    // Sets a Running row's progress.done (a follow job starts at the newest
    // record, Task 22 ruling F4); a row that is not Running is left alone.
    void SetProgressDone(JobId id, std::uint64_t done);
    void CancelBySubscriber(std::uint64_t subscriberId); // Running rows with that id become Cancelled
    // Each Running job, at most one step; a finished step stamps gate.Block()
    // as the envelope's host, ends Done (envelope.ok) or Failed, and Completes its waiter.
    void StepAll(HostGate& gate);
    // Finishes a Running row with `result` (state Done, Failed or Cancelled);
    // a row that is not Running is left alone.
    void Finish(JobId id, Envelope result, JobState state);
    const Job* Find(JobId id) const;
    bool AnyRunning(std::initializer_list<std::string_view> commands) const;
    void Prune(std::int64_t nowUnixMs);    // finished older than 10 min, then cap 64
    std::size_t FinishedCount() const;
    void SetQueue(RequestQueue* queue);    // StepAll and Finish Complete waiterId when non-zero; Task 8's RemoteDispatcher constructor calls it

private:
    mutable std::mutex m_mutex;
    std::vector<Job> m_jobs;
    std::uint64_t m_nextId = 1;
    RequestQueue* m_queue = nullptr;
};

} // namespace Arcane::Remote

#endif
