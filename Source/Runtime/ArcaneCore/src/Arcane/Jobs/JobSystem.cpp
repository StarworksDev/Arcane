#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Jobs/ArcaneWorkScheduler.hpp>   // presents the enki ITaskExecutor as a Mosaic::IWorkScheduler
#include <Arcane/Config/Bindings/JobsBinding.hpp>   // jobs.externalThreads (settings arc S6-8)
#include <Arcane/Base/Diagnostics.hpp>   // GuaranteeStackForThisThread -- every worker's first statement (crash window plan 1, R23)

#include <TaskScheduler.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <mutex>
#include <vector>

namespace Arcane
{
    namespace
    {
        // Adapts enkiTS to Arcane::ITaskExecutor -- the worker-index-aware face.
        // Internal to this TU: consumers only see ITaskExecutor via JobSystem::TaskExecutor().
        class EnkiTaskExecutor final : public ITaskExecutor
        {
        public:
            explicit EnkiTaskExecutor(enki::TaskScheduler& ts) : m_ts(ts) {}

            void ParallelFor(std::size_t count, std::size_t minBatch,
                             FunctionRef<void(std::size_t, std::size_t, std::uint32_t)> fn) override
            {
                if (count == 0)
                    return;
                assert(count <= static_cast<std::size_t>((std::numeric_limits<uint32_t>::max)()));
                assert(minBatch <= static_cast<std::size_t>((std::numeric_limits<uint32_t>::max)()));

                // fn outlives the task: WaitforTask blocks until all partitions complete.
                // The enki `threadnum` is the worker index (the value the old
                // IWorkScheduler adapter discarded).
                enki::TaskSet task(
                    static_cast<uint32_t>(count),
                    [&fn](enki::TaskSetPartition range, uint32_t threadnum)
                    {
                        fn(range.start, range.end, threadnum);
                    });
                task.m_MinRange = static_cast<uint32_t>(minBatch == 0 ? 1 : minBatch);

                m_ts.AddTaskSetToPipe(&task);
                m_ts.WaitforTask(&task);   // calling thread participates -> nested-safe
            }

            std::uint32_t WorkerCount() const noexcept override
            {
                return static_cast<std::uint32_t>(m_ts.GetNumTaskThreads());
            }

        private:
            enki::TaskScheduler& m_ts;
        };
    }

    struct JobSystem::Impl
    {
        enki::TaskScheduler                     ts;
        std::unique_ptr<EnkiTaskExecutor>       taskExec;  // the sole enki adapter (worker-index face)
        std::shared_ptr<ArcaneWorkScheduler>    adapter;   // ArcaneWorkScheduler over taskExec; concrete so ~JobSystem can null its executor

        // F2b Task 12: Submit()'s fire-and-forget task objects. enki::TaskSet
        // must outlive its own execution (AddTaskSetToPipe only stores a
        // pointer), so each submitted task is heap-allocated here and kept
        // alive until it completes. Reaped opportunistically on the NEXT
        // Submit() call (GetIsComplete() is O(1), the sweep is O(n) over
        // whatever is still outstanding) rather than immediately after
        // AddTaskSetToPipe, since a task is essentially never complete by the
        // time the call that submitted it returns -- an unconditional
        // check-right-after would almost always find nothing to reap and just
        // cost an extra pass every call. WaitforAllAndShutdown (~JobSystem)
        // guarantees every entry still here at destruction has finished
        // before this vector is torn down.
        std::mutex                                    submittedMutex;
        std::vector<std::unique_ptr<enki::TaskSet>>   submitted;
    };

    JobSystem::JobSystem(uint32_t threads) : m_impl(std::make_unique<Impl>())
    {
        // Initialize through the CONFIG overload, not the count one, for a
        // single reason: profilerCallbacks.threadStart is enki's worker-entry
        // hook (TaskScheduler.cpp:258, the first statement of
        // TaskingThreadFunction after the thread registers itself), and that
        // is where spec S5.1 item 4's stack guarantee has to go -- a worker
        // that overflows its stack must still be able to run the exception
        // filter. A captureless lambda is exactly the ProfilerCallbackFunc
        // function pointer the struct wants.
        //
        // `threads` IS A TOTAL, not a worker count -- JobSystem.hpp's own
        // contract, and enki's: Initialize(numThreadsTotal_) sets
        // numTaskThreadsToCreate = numThreadsTotal_ - 1
        // (TaskScheduler.cpp:1131), because the CALLING thread participates in
        // every ParallelFor (WaitforTask runs tasks on it). Hence `threads - 1`
        // here: passing `threads` straight through made JobSystem(1) -- the
        // serial, no-worker scheduler the thread-count-invariance test uses --
        // quietly spawn a real worker. numTaskThreadsToCreate keeps enki's own
        // default (hardware threads minus the calling thread) when `threads` is
        // 0, which is what Initialize() with no argument did.
        enki::TaskSchedulerConfig config;
        if (threads != 0)
            config.numTaskThreadsToCreate = threads - 1;
        // jobs.externalThreads (settings arc S6-8, Restart): read when the pool
        // is built. 0, enkiTS' own default, is the pre-sweep value.
        config.numExternalTaskThreads = Settings<JobsSettings>().externalThreads;
        config.profilerCallbacks.threadStart =
            [](uint32_t) { Arcane::Diagnostics::GuaranteeStackForThisThread(); };
        m_impl->ts.Initialize(config);
        m_impl->taskExec = std::make_unique<EnkiTaskExecutor>(m_impl->ts);
        // WorkScheduler() presents the SAME enki pool as a Mosaic::IWorkScheduler by
        // wrapping the worker-index-aware ITaskExecutor -- one adapter, no duplicate,
        // and it now forwards the per-lane worker id the reconciled seam requires.
        m_impl->adapter  = std::make_shared<ArcaneWorkScheduler>(m_impl->taskExec.get());
    }

    JobSystem::~JobSystem()
    {
        // Drain and join the pool FIRST, while taskExec and adapter are alive: a
        // queued task may still call back through either. Then null the
        // executor on the adapter, because a caller can keep the
        // shared_ptr<IWorkScheduler> from WorkScheduler() past this object, and
        // a null executor runs ParallelFor inline as worker 0 instead of
        // dereferencing the dead taskExec. Only then drop the adapter and the
        // executor. A kept WorkScheduler must not be inside a ParallelFor on
        // another thread while ~JobSystem runs (the write is atomic, the call in
        // flight is not drained).
        m_impl->ts.WaitforAllAndShutdown();
        m_impl->adapter->SetExecutor(nullptr);
        m_impl->adapter.reset();
        m_impl->taskExec.reset();
    }

    std::shared_ptr<Mosaic::IWorkScheduler> JobSystem::WorkScheduler() const
    {
        return m_impl->adapter;
    }

    ITaskExecutor* JobSystem::TaskExecutor() const noexcept
    {
        return m_impl->taskExec.get();
    }

    uint32_t JobSystem::WorkerCount() const noexcept
    {
        return m_impl->ts.GetNumTaskThreads();
    }

    void JobSystem::Submit(std::function<void()> fn)
    {
        // enki::TaskSet's own std::function-based constructor (TaskSetFunction
        // is void(TaskSetPartition, uint32_t)) -- setSize/minRange both default
        // to 1, so this runs ExecuteRange exactly once, on exactly one worker,
        // for the whole fn: a single unit of fire-and-forget work, not a
        // parallel-for.
        auto task = std::make_unique<enki::TaskSet>(
            [fn = std::move(fn)](enki::TaskSetPartition, uint32_t) { fn(); });

        std::lock_guard<std::mutex> lock(m_impl->submittedMutex);

        // Reap whatever earlier submissions have already finished before
        // adding this one -- amortized cleanup, see Impl::submitted's own
        // comment for why this is done here rather than right after
        // AddTaskSetToPipe below.
        m_impl->submitted.erase(
            std::remove_if(m_impl->submitted.begin(), m_impl->submitted.end(),
                            [](const std::unique_ptr<enki::TaskSet>& t) { return t->GetIsComplete(); }),
            m_impl->submitted.end());

        // The task object must already be reachable from m_impl->submitted
        // BEFORE AddTaskSetToPipe, in case a worker picks it up and completes
        // it before this function returns -- there is no window where a
        // worker could be executing a task this vector does not yet own.
        enki::TaskSet* raw = task.get();
        m_impl->submitted.push_back(std::move(task));
        m_impl->ts.AddTaskSetToPipe(raw);
    }
}
