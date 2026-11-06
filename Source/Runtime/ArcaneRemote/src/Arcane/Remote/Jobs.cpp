#include <Arcane/Remote/Jobs.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Remote/Registry.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace Arcane::Remote
{

namespace
{

ARC_CONSTANT("s7.7: a finished job row is kept 10 minutes")
constexpr std::int64_t kFinishedKeepMs = 600000;
ARC_CONSTANT("s7.7: at most 64 finished job rows are kept")
constexpr std::size_t kFinishedKeepCount = 64;

std::int64_t NowUnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool IsFinished(JobState state)
{
    return state == JobState::Done || state == JobState::Failed || state == JobState::Cancelled;
}

// Applies the s7.7 finish rule to a row under the table lock. Returns the
// waiter to Complete (0 = none) and clears it, so each waiter completes once.
std::uint64_t FinishRow(Job& job, JobState state, CommandResult& out)
{
    if (!job.envelope.undo && job.undo)
        job.envelope.undo = job.undo;
    job.state = state;
    job.finishedUnixMs = NowUnixMs();
    const std::uint64_t waiter = job.waiterId;
    job.waiterId = 0;
    if (waiter != 0)
        out = CommandResult{job.envelope, 0};
    return waiter;
}

} // namespace

JobId JobTable::Arm(std::string command, std::uint64_t total, JobStepFn step, void* user)
{
    std::lock_guard lock(m_mutex);
    Job job;
    job.id = JobId{m_nextId++};
    // The row's envelope starts as an ok answer for its command: a step that
    // only advances progress finishes Done; a failing step replaces it.
    job.envelope = MakeOk(command, nlohmann::json::object(), {});
    job.command = std::move(command);
    job.state = JobState::Running;
    job.progress.done = 0;
    job.progress.total = total;
    job.step = step;
    job.user = user;
    m_jobs.push_back(std::move(job));
    return m_jobs.back().id;
}

void JobTable::BindWaiter(JobId id, std::uint64_t waiterId, std::optional<UndoBlock> undo)
{
    std::lock_guard lock(m_mutex);
    for (Job& job : m_jobs)
    {
        if (job.id.value != id.value)
            continue;
        job.waiterId = waiterId;
        job.undo = std::move(undo);
        return;
    }
}

void JobTable::SetSubscriber(JobId id, std::uint64_t subscriberId)
{
    std::lock_guard lock(m_mutex);
    for (Job& job : m_jobs)
    {
        if (job.id.value == id.value)
        {
            job.subscriberId = subscriberId;
            return;
        }
    }
}

void JobTable::SetProgressDone(JobId id, std::uint64_t done)
{
    std::lock_guard lock(m_mutex);
    for (Job& job : m_jobs)
    {
        if (job.id.value == id.value)
        {
            if (job.state == JobState::Running)
                job.progress.done = done;
            return;
        }
    }
}

void JobTable::CancelBySubscriber(std::uint64_t subscriberId)
{
    if (subscriberId == 0)
        return;
    std::lock_guard lock(m_mutex);
    for (Job& job : m_jobs)
    {
        if (job.state != JobState::Running || job.subscriberId != subscriberId)
            continue;
        job.state = JobState::Cancelled;
        job.finishedUnixMs = NowUnixMs();
    }
}

void JobTable::StepAll(HostGate& gate)
{
    // Only the rows Running when the pass starts are stepped; a job armed by a
    // step waits for the next pass.
    std::vector<std::uint64_t> ids;
    {
        std::lock_guard lock(m_mutex);
        ids.reserve(m_jobs.size());
        for (const Job& job : m_jobs)
            if (job.state == JobState::Running && job.step != nullptr)
                ids.push_back(job.id.value);
    }

    for (const std::uint64_t id : ids)
    {
        Job work;
        {
            std::lock_guard lock(m_mutex);
            const auto it = std::find_if(m_jobs.begin(), m_jobs.end(),
                                         [id](const Job& job) { return job.id.value == id; });
            if (it == m_jobs.end() || it->state != JobState::Running)
                continue; // finished or cancelled by an earlier step of this pass
            work = *it;
        }

        const bool finished = work.step(work, gate, work.user);

        std::uint64_t waiter = 0;
        CommandResult result;
        RequestQueue* queue = nullptr;
        {
            std::lock_guard lock(m_mutex);
            const auto it = std::find_if(m_jobs.begin(), m_jobs.end(),
                                         [id](const Job& job) { return job.id.value == id; });
            if (it == m_jobs.end() || it->state != JobState::Running)
                continue; // finished or cancelled while the step ran
            // The step's copy comes back whole, except the fields the table
            // owns (a call made while the step ran may have changed them).
            work.id = it->id;
            work.state = it->state;
            work.finishedUnixMs = it->finishedUnixMs;
            work.waiterId = it->waiterId;
            work.undo = std::move(it->undo);
            work.subscriberId = it->subscriberId;
            *it = std::move(work);
            if (finished)
            {
                it->envelope.host = gate.Block();
                waiter = FinishRow(*it, it->envelope.ok ? JobState::Done : JobState::Failed, result);
            }
            queue = m_queue;
        }
        if (waiter != 0 && queue != nullptr)
            queue->Complete(waiter, std::move(result));
    }
}

void JobTable::Finish(JobId id, Envelope result, JobState state)
{
    std::uint64_t waiter = 0;
    CommandResult completed;
    RequestQueue* queue = nullptr;
    {
        std::lock_guard lock(m_mutex);
        const auto it = std::find_if(m_jobs.begin(), m_jobs.end(),
                                     [&id](const Job& job) { return job.id.value == id.value; });
        if (it == m_jobs.end() || it->state != JobState::Running)
            return;
        it->envelope = std::move(result);
        waiter = FinishRow(*it, state, completed);
        queue = m_queue;
    }
    if (waiter != 0 && queue != nullptr)
        queue->Complete(waiter, std::move(completed));
}

const Job* JobTable::Find(JobId id) const
{
    std::lock_guard lock(m_mutex);
    for (const Job& job : m_jobs)
        if (job.id.value == id.value)
            return &job;
    return nullptr;
}

bool JobTable::AnyRunning(std::initializer_list<std::string_view> commands) const
{
    std::lock_guard lock(m_mutex);
    for (const Job& job : m_jobs)
    {
        if (job.state != JobState::Running)
            continue;
        for (const std::string_view name : commands)
            if (job.command == name)
                return true;
    }
    return false;
}

void JobTable::Prune(std::int64_t nowUnixMs)
{
    std::lock_guard lock(m_mutex);
    std::erase_if(m_jobs, [nowUnixMs](const Job& job) {
        return IsFinished(job.state) && nowUnixMs - job.finishedUnixMs > kFinishedKeepMs;
    });

    std::vector<const Job*> finished;
    for (const Job& job : m_jobs)
        if (IsFinished(job.state))
            finished.push_back(&job);
    if (finished.size() <= kFinishedKeepCount)
        return;

    // Oldest first: earliest finish, then earliest id.
    std::sort(finished.begin(), finished.end(), [](const Job* a, const Job* b) {
        if (a->finishedUnixMs != b->finishedUnixMs)
            return a->finishedUnixMs < b->finishedUnixMs;
        return a->id.value < b->id.value;
    });
    std::vector<std::uint64_t> drop;
    drop.reserve(finished.size() - kFinishedKeepCount);
    for (std::size_t i = 0; i < finished.size() - kFinishedKeepCount; ++i)
        drop.push_back(finished[i]->id.value);
    std::erase_if(m_jobs, [&drop](const Job& job) {
        return std::find(drop.begin(), drop.end(), job.id.value) != drop.end();
    });
}

std::size_t JobTable::FinishedCount() const
{
    std::lock_guard lock(m_mutex);
    return static_cast<std::size_t>(
        std::count_if(m_jobs.begin(), m_jobs.end(), [](const Job& job) { return IsFinished(job.state); }));
}

void JobTable::SetQueue(RequestQueue* queue)
{
    std::lock_guard lock(m_mutex);
    m_queue = queue;
}

} // namespace Arcane::Remote

#endif
