// 3D-readiness arc completion check (Plan C, Task 6): two Runtimes in one
// process share ProcessContext's one scheduler. Pins behaviour fix #11 landed.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Jobs/JobSystem.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

TEST_CASE("two Runtimes in one process share one scheduler", "[runtime][jobs][arc-complete]")
{
    Arcane::Runtime b(Arcane::Test::Process(), Arcane::NetMode::Standalone);
    Arcane::IWorkScheduler* shared = b.WorkScheduler();
    REQUIRE(shared != nullptr);
    {
        Arcane::Runtime a(Arcane::Test::Process(), Arcane::NetMode::Standalone);
        CHECK(a.WorkScheduler() == shared);
        CHECK(&a.Jobs() == &b.Jobs());
        // While a pool is alive the accessor returns it; the count is ignored.
        const std::shared_ptr<Arcane::JobSystem> process = Arcane::Test::Process().AcquireJobSystem(1);
        CHECK(process.get() == &a.Jobs());
        CHECK(process->WorkScheduler().get() == shared);
    }   // a is gone; b still holds the pool
    CHECK(b.WorkScheduler() == shared);
    std::atomic<std::size_t> covered{0};
    shared->ParallelFor(1024, 16, [&](std::size_t begin, std::size_t end, std::uint32_t) { covered.fetch_add(end - begin); });
    CHECK(covered.load() == 1024u);
}
