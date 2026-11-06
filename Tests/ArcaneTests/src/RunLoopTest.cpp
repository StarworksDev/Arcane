// RunLoop: a fixed-timestep accumulator drives the FixedUpdate scheduler N times
// per real frame and the Update scheduler once, exposing a render alpha in [0,1).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <vector>
#include <functional>

namespace
{
    struct Ticks { int fixed = 0; };

    // IncrementTicks is a minimal System (callable with Registry&) that increments
    // the Ticks resource. AddSystem<T> requires the System concept: callable with
    // Registry& -> void. Lambdas taking Registry& are callable but fail the
    // LambdaLike concept guard (LambdaLike excludes System-shaped callables to
    // prevent ambiguity), so we wrap in a named type instead.
    struct IncrementTicks
    {
        void operator()(Astra::Registry& r) const
        {
            r.GetResource<Ticks>()->fixed += 1;
        }
    };
}

namespace
{
    // Named engine system — records 'E' each engine fixed step.
    // Aggregate-initialized by AddSystem<EngineStep>(engineFixed, order).
    // Explicit ctor required because MSVC doesn't support parenthesized
    // aggregate init of structs with reference members under all circumstances.
    struct EngineStep
    {
        int&               n;
        std::vector<char>& order;
        EngineStep(int& n_, std::vector<char>& o_) : n(n_), order(o_) {}
        void operator()(Astra::Registry&) const { ++n; order.push_back('E'); }
    };
}

TEST_CASE("RunLoop interleaves plugin callbacks with engine fixedUpdate", "[sim][runloop]")
{
    Astra::Registry reg;
    int engineFixed = 0;
    std::vector<char> order;
    Arcane::SystemSchedulers sch(nullptr);
    // Registration success is load-bearing: engineFixed/order are the test's
    // only observable signal, so a silent AddSystem failure would read as a
    // (wrong) assertion failure below instead of a clear setup error here.
    REQUIRE(sch.fixedUpdate.AddSystem<EngineStep>(engineFixed, order).IsOk());

    Arcane::RunLoop loop(reg, sch);
    int pluginFixed = 0, pluginUpdate = 0;
    for (int i = 0; i < 60; ++i)
        loop.Advance(1.0 / 60.0,
            [&](double){ ++pluginFixed; order.push_back('P'); },
            [&](double, double){ ++pluginUpdate; });

    CHECK(pluginFixed >= 58);
    CHECK(pluginFixed == engineFixed);     // one plugin tick per engine fixed step
    CHECK(pluginUpdate == 60);             // once per frame
    // every fixed step is plugin('P') then engine('E'), so the sequence alternates P,E:
    REQUIRE(order.size() >= 2);
    CHECK(order[order.size() - 2] == 'P');
    CHECK(order[order.size() - 1] == 'E');
}

TEST_CASE("RunLoop runs a fixed-rate scheduler and clamps spikes", "[sim][runloop]")
{
    Astra::Registry reg;  // sequential fallback scheduler -- no jobs needed here
    reg.SetResource<Ticks>(Ticks{});

    Arcane::SystemSchedulers schedulers(nullptr);  // null -> sequential executor
    REQUIRE(schedulers.fixedUpdate.AddSystem<IncrementTicks>().IsOk());

    Arcane::RunLoop::Config cfg;   // 60 Hz, maxStepsPerFrame default 5
    Arcane::RunLoop loop(reg, schedulers, cfg);

    for (int i = 0; i < 60; ++i)
    {
        double alpha = loop.Advance(1.0 / 60.0);
        CHECK(alpha >= 0.0);
        CHECK(alpha < 1.0);
    }
    const int afterOneSecond = reg.GetResource<Ticks>()->fixed;
    CHECK(afterOneSecond >= 58);
    CHECK(afterOneSecond <= 62);

    const int before = reg.GetResource<Ticks>()->fixed;
    loop.Advance(10.0);  // would be 600 steps unclamped
    const int stepsTaken = reg.GetResource<Ticks>()->fixed - before;
    CHECK(stepsTaken == cfg.maxStepsPerFrame);
    CHECK(loop.Alpha() >= 0.0);
    CHECK(loop.Alpha() < 1.0);
}

// ---- sim-time control (Epic 04): pause / single-step / time-scale -----------

TEST_CASE("RunLoop paused: fixed phase frozen, Update still runs", "[sim][runloop]")
{
    Astra::Registry reg;
    reg.SetResource<Ticks>(Ticks{});
    Arcane::SystemSchedulers sch(nullptr);
    REQUIRE(sch.fixedUpdate.AddSystem<IncrementTicks>().IsOk());
    Arcane::RunLoop loop(reg, sch);

    loop.SetPaused(true);
    CHECK(loop.IsPaused());

    int updates = 0;
    for (int i = 0; i < 60; ++i)
        loop.Advance(1.0 / 60.0, {}, [&](double, double){ ++updates; });

    CHECK(reg.GetResource<Ticks>()->fixed == 0);  // no fixed steps while paused
    CHECK(updates == 60);                          // ...but the Update phase ran every frame
}

TEST_CASE("RunLoop single-step: exactly one canonical fixed step while paused", "[sim][runloop]")
{
    Astra::Registry reg;
    reg.SetResource<Ticks>(Ticks{});
    Arcane::SystemSchedulers sch(nullptr);
    REQUIRE(sch.fixedUpdate.AddSystem<IncrementTicks>().IsOk());
    Arcane::RunLoop loop(reg, sch);

    loop.SetPaused(true);
    for (int i = 0; i < 10; ++i) loop.Advance(1.0 / 60.0);
    CHECK(reg.GetResource<Ticks>()->fixed == 0);   // frozen

    loop.RequestSingleStep();
    loop.Advance(1.0 / 60.0);
    CHECK(reg.GetResource<Ticks>()->fixed == 1);   // exactly one step

    loop.Advance(1.0 / 60.0);                       // the request was one-shot
    CHECK(reg.GetResource<Ticks>()->fixed == 1);   // still one; no lingering step
    CHECK(loop.IsPaused());                          // and still paused
}

// The runtime's remote `step` (AM1, RuntimeFrame::AdvanceSim SimPace::SingleTick)
// requests a single step on a loop that is NOT paused and relies on this.
TEST_CASE("RunLoop single-step on a running loop: one tick whatever the dt, backlog untouched", "[sim][runloop]")
{
    Astra::Registry reg;
    reg.SetResource<Ticks>(Ticks{});
    Arcane::SystemSchedulers sch(nullptr);
    REQUIRE(sch.fixedUpdate.AddSystem<IncrementTicks>().IsOk());
    Arcane::RunLoop loop(reg, sch);
    REQUIRE_FALSE(loop.IsPaused());

    loop.Advance(0.5 / 60.0);                       // half a step of backlog
    CHECK(reg.GetResource<Ticks>()->fixed == 0);

    loop.RequestSingleStep();
    loop.Advance(10.0 / 60.0);                      // a long frame: still exactly one tick
    CHECK(reg.GetResource<Ticks>()->fixed == 1);

    loop.Advance(0.5 / 60.0);                       // the half step of backlog survived the step
    CHECK(reg.GetResource<Ticks>()->fixed == 2);

    loop.SetTimeScale(0.0);                         // a stalled clock does not swallow a step
    loop.RequestSingleStep();
    loop.Advance(1.0 / 60.0);
    CHECK(reg.GetResource<Ticks>()->fixed == 3);
}

TEST_CASE("RunLoop time-scale scales the sim clock, not the step dt", "[sim][runloop]")
{
    auto stepsOverOneRealSecond = [](double scale)
    {
        Astra::Registry reg;
        reg.SetResource<Ticks>(Ticks{});
        Arcane::SystemSchedulers sch(nullptr);
        REQUIRE(sch.fixedUpdate.AddSystem<IncrementTicks>().IsOk());
        Arcane::RunLoop loop(reg, sch);
        loop.SetTimeScale(scale);
        for (int i = 0; i < 60; ++i) loop.Advance(1.0 / 60.0);  // 1s of real time
        return reg.GetResource<Ticks>()->fixed;
    };

    CHECK(stepsOverOneRealSecond(1.0) >= 58);
    CHECK(stepsOverOneRealSecond(1.0) <= 62);
    // 0.5x: the sim clock runs at half real time -> ~30 canonical steps in one real second.
    CHECK(stepsOverOneRealSecond(0.5) >= 28);
    CHECK(stepsOverOneRealSecond(0.5) <= 32);
    // 2x: ~2 fixed ticks accumulate per real frame (< the 5-step clamp), so ~120 steps.
    CHECK(stepsOverOneRealSecond(2.0) >= 116);
    CHECK(stepsOverOneRealSecond(2.0) <= 124);
    // 0x: the sim clock is stalled entirely.
    CHECK(stepsOverOneRealSecond(0.0) == 0);
}

TEST_CASE("RunLoop unpause does not burst catch-up steps", "[sim][runloop]")
{
    Astra::Registry reg;
    reg.SetResource<Ticks>(Ticks{});
    Arcane::SystemSchedulers sch(nullptr);
    REQUIRE(sch.fixedUpdate.AddSystem<IncrementTicks>().IsOk());
    Arcane::RunLoop loop(reg, sch);

    loop.SetPaused(true);
    for (int i = 0; i < 600; ++i) loop.Advance(1.0 / 60.0);  // 10 real seconds, paused
    CHECK(reg.GetResource<Ticks>()->fixed == 0);

    loop.SetPaused(false);
    loop.Advance(1.0 / 60.0);                                 // one real frame after unpause
    // Paused frames accumulated NOTHING, so there is no 600-step debt to burn down.
    CHECK(reg.GetResource<Ticks>()->fixed <= 1);
}

// ---- Arcane::Time (input-seam spec s3) --------------------------------------
#include <Arcane/Sim/Time.hpp>

namespace
{
    // Records the Time each fixed step saw.
    struct RecordFixedTime
    {
        std::vector<Arcane::Time>* seen;
        explicit RecordFixedTime(std::vector<Arcane::Time>* s) : seen(s) {}
        void operator()(Astra::Registry& r) const { seen->push_back(*r.GetResource<Arcane::Time>()); }
    };
    struct RecordUpdateTime
    {
        std::vector<Arcane::Time>* seen;
        explicit RecordUpdateTime(std::vector<Arcane::Time>* s) : seen(s) {}
        void operator()(Astra::Registry& r) const { seen->push_back(*r.GetResource<Arcane::Time>()); }
    };
}

TEST_CASE("RunLoop publishes Time before every fixed step and before Update", "[sim][runloop][time]")
{
    Astra::Registry reg;
    std::vector<Arcane::Time> fixedSeen, updateSeen;
    Arcane::SystemSchedulers sch(nullptr);
    REQUIRE(sch.fixedUpdate.AddSystem<RecordFixedTime>(&fixedSeen).IsOk());
    REQUIRE(sch.update.AddSystem<RecordUpdateTime>(&updateSeen).IsOk());
    Arcane::RunLoop loop(reg, sch);

    for (int i = 0; i < 30; ++i) loop.Advance(1.0 / 60.0);

    REQUIRE_FALSE(fixedSeen.empty());
    REQUIRE(updateSeen.size() == 30);
    for (std::size_t i = 0; i < fixedSeen.size(); ++i)
    {
        CHECK(fixedSeen[i].fixedStep == i + 1);                       // one per step, from 1
        CHECK(fixedSeen[i].inFixedStep);
        CHECK(fixedSeen[i].fixedDt == 1.0 / 60.0);
        CHECK(fixedSeen[i].elapsed == static_cast<double>(i + 1) * (1.0 / 60.0));
    }
    const Arcane::Time& last = updateSeen.back();
    CHECK_FALSE(last.inFixedStep);
    CHECK(last.fixedStep == fixedSeen.size());
    CHECK(last.realDt == 1.0 / 60.0);
    CHECK(last.dt == 1.0 / 60.0);
    CHECK(last.alpha == loop.Alpha());
    CHECK_FALSE(last.paused);
    CHECK(last.timeScale == 1.0);
}

TEST_CASE("Time: the plugin-callback Advance publishes too, before the plugin's fixed hook", "[sim][runloop][time]")
{
    Astra::Registry reg;
    Arcane::SystemSchedulers sch(nullptr);
    Arcane::RunLoop loop(reg, sch);
    std::vector<std::uint64_t> pluginSaw;
    for (int i = 0; i < 10; ++i)
        loop.Advance(1.0 / 60.0,
            [&](double){ pluginSaw.push_back(reg.GetResource<Arcane::Time>()->fixedStep); },
            [&](double, double){ CHECK_FALSE(reg.GetResource<Arcane::Time>()->inFixedStep); });
    REQUIRE_FALSE(pluginSaw.empty());
    for (std::size_t i = 0; i < pluginSaw.size(); ++i) CHECK(pluginSaw[i] == i + 1);
}

TEST_CASE("Time: time scale shows in dt, the fixed step stays canonical", "[sim][runloop][time]")
{
    Astra::Registry reg;
    Arcane::SystemSchedulers sch(nullptr);
    Arcane::RunLoop loop(reg, sch);
    loop.SetTimeScale(0.5);
    loop.Advance(1.0 / 60.0);
    const Arcane::Time* t = reg.GetResource<Arcane::Time>();
    REQUIRE(t);
    CHECK(t->dt == 0.5 / 60.0);
    CHECK(t->realDt == 1.0 / 60.0);
    CHECK(t->fixedDt == 1.0 / 60.0);
    CHECK(t->timeScale == 0.5);
}

// Review Focus #2: Play started while paused, then single-stepped; Stop (Rebind).
TEST_CASE("Time while paused: no steps, dt 0; each single step adds exactly 1; Rebind resets the clock but not the pause",
          "[sim][runloop][time]")
{
    Astra::Registry reg;
    Arcane::SystemSchedulers sch(nullptr);
    Arcane::RunLoop loop(reg, sch);
    loop.SetPaused(true);

    for (int i = 0; i < 5; ++i) loop.Advance(1.0 / 60.0);
    const Arcane::Time* t = reg.GetResource<Arcane::Time>();
    REQUIRE(t);
    CHECK(t->fixedStep == 0);
    CHECK(t->paused);
    CHECK(t->dt == 0.0);
    CHECK(t->realDt == 1.0 / 60.0);

    for (int s = 1; s <= 3; ++s)
    {
        loop.RequestSingleStep();
        loop.Advance(1.0 / 60.0);
        t = reg.GetResource<Arcane::Time>();
        CHECK(t->fixedStep == static_cast<std::uint64_t>(s));
        CHECK(t->paused);
        CHECK(t->dt == 0.0);
    }

    Astra::Registry swapped;
    loop.Rebind(swapped);
    CHECK(loop.IsPaused());                                  // host mode survives (RunLoop.hpp:138)
    loop.RequestSingleStep();
    loop.Advance(1.0 / 60.0);
    const Arcane::Time* t2 = swapped.GetResource<Arcane::Time>();
    REQUIRE(t2);
    CHECK(t2->fixedStep == 1);                               // counter restarted for the new registry
    CHECK(t2->elapsed == 1.0 / 60.0);
}

// elapsed is fixedStep * fixedDt at a constant rate (pinned above); a SetFixedHz
// change rebases it, so the steps before the change keep the time they took and
// the clock never runs backwards.
TEST_CASE("Time: elapsed stays monotonic across a SetFixedHz change", "[sim][runloop][time]")
{
    Astra::Registry reg;
    std::vector<Arcane::Time> fixedSeen;
    Arcane::SystemSchedulers sch(nullptr);
    REQUIRE(sch.fixedUpdate.AddSystem<RecordFixedTime>(&fixedSeen).IsOk());
    Arcane::RunLoop loop(reg, sch);

    for (int i = 0; i < 10; ++i) loop.Advance(1.0 / 60.0);
    REQUIRE(fixedSeen.size() == 10);
    const double before = fixedSeen.back().elapsed;
    CHECK(before == 10.0 * (1.0 / 60.0));

    loop.SetFixedHz(30.0);
    for (int i = 0; i < 10; ++i) loop.Advance(1.0 / 30.0);
    REQUIRE(fixedSeen.size() == 20);
    for (std::size_t i = 1; i < fixedSeen.size(); ++i)
        CHECK(fixedSeen[i].elapsed > fixedSeen[i - 1].elapsed);
    CHECK(fixedSeen.back().fixedStep == 20);
    CHECK(fixedSeen.back().fixedDt == 1.0 / 30.0);
    CHECK(fixedSeen.back().elapsed == before + 10.0 * (1.0 / 30.0));
}
