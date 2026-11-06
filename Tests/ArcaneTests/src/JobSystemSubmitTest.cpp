// F2b Task 12: JobSystem::Submit -- fire-and-forget CPU work off the calling
// thread. The editor's background texture cook (Arcane::Editor::CookQueue)
// is the production consumer: a watcher-triggered cook must run without
// blocking the frame, and the JobSystem it runs on must fully drain any
// still-outstanding work at destruction (no dangling worker touching a
// destructed capture). CPU-only, [jobs] -- the same tag JobSchedulerTest.cpp/
// TaskExecutorTest.cpp already use for this class.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Jobs/JobSystem.hpp>
#include <Mosaic/Jobs/WorkScheduler.hpp>

#include "Helpers/ConstantScan.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Arcane::JobSystem;

namespace
{
    std::string ReadRepoFile(const char* relative)
    {
        std::ifstream in(Arcane::Test::RepoRoot() / relative, std::ios::binary);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Bounded spin-wait so a genuinely broken Submit() (task never runs) fails
    // the test loudly after a few seconds instead of hanging the whole suite
    // -- same "yield until a flag flips" shape ServiceThreadTest.cpp already
    // uses, with an explicit deadline added since a fire-and-forget task has
    // no join to fall back on.
    template <typename Pred>
    bool WaitUntil(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::yield();
        }
        return true;
    }
}

TEST_CASE("Submit runs the callable on a worker thread, not the caller", "[jobs]")
{
    // threads=2 -> exactly one background worker thread (thread 0 is the
    // constructing/test thread itself -- see enki::TaskScheduler::
    // Initialize(numThreadsTotal_)'s own contract) -- guarantees the
    // submitted task is picked up WITHOUT this test ever calling
    // WaitforTask/WaitforAll itself, which is exactly the fire-and-forget
    // contract under test.
    JobSystem jobs(2);

    const std::thread::id callerId = std::this_thread::get_id();
    std::atomic<bool> ran{false};
    std::mutex idMutex;
    std::thread::id ranOnId{};

    jobs.Submit([&]
    {
        {
            std::lock_guard<std::mutex> lock(idMutex);
            ranOnId = std::this_thread::get_id();
        }
        ran.store(true, std::memory_order_release);
    });

    REQUIRE(WaitUntil([&] { return ran.load(std::memory_order_acquire); }));
    std::lock_guard<std::mutex> lock(idMutex);
    CHECK(ranOnId != callerId);
}

TEST_CASE("Submit does not block the calling thread", "[jobs]")
{
    JobSystem jobs(2);

    std::atomic<bool> started{false};
    std::atomic<bool> allowedToFinish{false};
    std::atomic<bool> finished{false};

    jobs.Submit([&]
    {
        started.store(true, std::memory_order_release);
        // Held open until the test explicitly releases it -- if Submit()
        // were secretly synchronous (e.g. ran fn() inline before returning),
        // the REQUIRE below would never be reached at all, since this
        // lambda would already be blocking the test thread right here.
        while (!allowedToFinish.load(std::memory_order_acquire))
            std::this_thread::yield();
        finished.store(true, std::memory_order_release);
    });

    // Submit() itself must have already returned by construction (we are
    // past the call), and the worker must not need our help to make
    // progress -- prove the worker actually STARTED without this thread
    // doing anything but wait, which is only possible if Submit() returned
    // promptly rather than running fn() on this thread first.
    REQUIRE(WaitUntil([&] { return started.load(std::memory_order_acquire); }));
    CHECK_FALSE(finished.load(std::memory_order_acquire));

    allowedToFinish.store(true, std::memory_order_release);
    REQUIRE(WaitUntil([&] { return finished.load(std::memory_order_acquire); }));
}

TEST_CASE("Submit: every submitted task runs exactly once, disjoint full cover", "[jobs]")
{
    JobSystem jobs(0);   // hardware default -- however many workers this desk has

    constexpr std::size_t kN = 256;
    std::vector<std::atomic<int>> visited(kN);
    for (auto& v : visited)
        v.store(0, std::memory_order_relaxed);

    for (std::size_t i = 0; i < kN; ++i)
        jobs.Submit([&visited, i] { visited[i].fetch_add(1, std::memory_order_relaxed); });

    REQUIRE(WaitUntil([&]
    {
        for (auto& v : visited)
            if (v.load(std::memory_order_relaxed) == 0)
                return false;
        return true;
    }));

    for (auto& v : visited)
        CHECK(v.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("destruction drains every submitted task before returning", "[jobs]")
{
    constexpr int kN = 32;
    std::atomic<int> completed{0};

    {
        JobSystem jobs(2);
        for (int i = 0; i < kN; ++i)
        {
            jobs.Submit([&completed]
            {
                // A short sleep widens the window in which some of these are
                // still queued or in-flight when the destructor below runs --
                // the property under test is that ~JobSystem waits for ALL
                // of them regardless of how far along they are.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // ~JobSystem runs here (WaitforAllAndShutdown) -- no explicit drain
        // call is offered or needed.
    }

    CHECK(completed.load(std::memory_order_relaxed) == kN);
}

TEST_CASE("~JobSystem shuts the scheduler down before it resets the adapter", "[jobs]")
{
    const auto text = ReadRepoFile("Source/Runtime/ArcaneCore/src/Arcane/Jobs/JobSystem.cpp");
    const auto dtor = text.find("JobSystem::~JobSystem()");
    REQUIRE(dtor != std::string::npos);
    const auto end = text.find("JobSystem::WorkScheduler", dtor);
    const auto body = text.substr(dtor, end - dtor);
    const auto shut = body.find("WaitforAllAndShutdown");
    const auto drop = body.find("adapter.reset");
    REQUIRE(shut != std::string::npos);
    REQUIRE(drop != std::string::npos);
    CHECK(shut < drop);
    CHECK(body.find("SetExecutor(nullptr)") != std::string::npos);
}

TEST_CASE("a WorkScheduler kept past ~JobSystem runs ParallelFor inline instead of faulting", "[jobs]")
{
    std::shared_ptr<Mosaic::IWorkScheduler> sched;
    {
        JobSystem jobs(2);
        sched = jobs.WorkScheduler();
    }
    REQUIRE(sched);
    std::size_t counter = 0;   // inline on this thread, so no atomic is needed
    sched->ParallelFor(4, 1, [&](std::size_t b, std::size_t e, std::uint32_t)
    {
        counter += e - b;
    });
    CHECK(counter == 4);
}
