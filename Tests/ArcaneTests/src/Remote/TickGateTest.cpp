#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/TickGate.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <cstdint>
#include <filesystem>
#include <limits>
#include "RemoteTestSupport.hpp"

#if !defined(ARC_BUILD_DIST)

TEST_CASE("Consume is Idle until step or run, and step decrements", "[remote][tick]") {
    Arcane::Remote::TickGate gate;
    gate.SetRunning(false);
    gate.SetPaused(true);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
    gate.RequestSteps(2);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
    CHECK(gate.StepsRemaining() == 0);
    gate.SetRunning(true);
    gate.SetPaused(false);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Run);
    gate.SetPaused(true);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
    gate.RequestSteps(1);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
}

TEST_CASE("holdSim starts Idle and a free run starts Running", "[remote][tick]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Server); // Start sets the process host kind (ruling S3-9)
    Arcane::Remote::HostSession::Config held;
    held.kind = Arcane::Remote::HostKind::Server;
    held.cvarContext = Arcane::CVarContext::ServerAdmin;
    held.projectRoot = std::filesystem::temp_directory_path() / "remote-hold-sim";
    held.holdSim = true;
    Arcane::Remote::HostSession session(held);
    REQUIRE(session.Start());
    CHECK(session.Tick().Consume() == Arcane::Remote::TickDecision::Idle);
    session.Stop("test");

    Arcane::Remote::HostSession::Config free = held;
    free.holdSim = false;
    free.projectRoot = std::filesystem::temp_directory_path() / "remote-free-sim";
    Arcane::Remote::HostSession running(free);
    REQUIRE(running.Start());
    CHECK(running.Tick().Consume() == Arcane::Remote::TickDecision::Run);
    running.Stop("test");
}

TEST_CASE("Run clears a leftover step count", "[remote][tick]") {
    Arcane::Remote::TickGate gate;
    gate.SetRunning(false);
    gate.SetPaused(true);
    gate.RequestSteps(3);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.StepsRemaining() == 2);
    gate.SetRunning(true);
    gate.SetPaused(false);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Run);
    CHECK(gate.StepsRemaining() == 0);
    gate.SetPaused(true); // pausing again does not resurrect the cleared steps
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
}

// Final fix pass (G-1, C-I2 b): one step budget per host. A request raises
// the pending count to at least n and never adds, so two overlapping
// play.step 10 calls run 10 ticks, not 20; each job tracks its own target.
TEST_CASE("RequestSteps raises the budget to the larger request, never the sum", "[remote][tick]") {
    Arcane::Remote::TickGate gate;
    gate.SetRunning(false);
    gate.RequestSteps(2);
    gate.RequestSteps(3);
    CHECK(gate.StepsRemaining() == 3);
    gate.RequestSteps(1);
    CHECK(gate.StepsRemaining() == 3);
    gate.RequestSteps(std::numeric_limits<std::uint32_t>::max());
    CHECK(gate.StepsRemaining() == std::numeric_limits<std::uint32_t>::max());
    gate.RequestSteps(7);
    CHECK(gate.StepsRemaining() == std::numeric_limits<std::uint32_t>::max());
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.StepsRemaining() == std::numeric_limits<std::uint32_t>::max() - 1);
    // TrimSteps (a timed-out step giving back its share) only ever lowers it.
    gate.TrimSteps(4);
    CHECK(gate.StepsRemaining() == 4);
    gate.TrimSteps(9);
    CHECK(gate.StepsRemaining() == 4);
}

TEST_CASE("running and paused with steps pending is Step, then Idle", "[remote][tick]") {
    Arcane::Remote::TickGate gate;
    gate.SetRunning(true);
    gate.SetPaused(true);
    gate.RequestSteps(2);
    CHECK(gate.Running());
    CHECK(gate.Paused());
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
    CHECK(gate.Running()); // stepping a paused run leaves it a paused run
    CHECK(gate.Paused());
}

TEST_CASE("ClearSteps drops pending steps and Held names the hold", "[remote][tick][play]") {
    Arcane::Remote::TickGate gate;   // the pre-remote loop: running, not paused
    CHECK_FALSE(gate.Held());
    gate.SetPaused(true);
    CHECK(gate.Held());
    gate.RequestSteps(5);
    CHECK(gate.Held());              // pending steps still tick one each while held
    gate.ClearSteps();               // play.stop (Task 29)
    CHECK(gate.StepsRemaining() == 0);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
    gate.SetPaused(false);
    gate.SetRunning(false);          // stopped, not paused: still held
    CHECK(gate.Held());
    gate.SetRunning(true);
    CHECK_FALSE(gate.Held());
}

TEST_CASE("HeldWindowTitle appends [held] only while held", "[remote][tick][play]") {
    CHECK(Arcane::Remote::HeldWindowTitle("Arcane Runtime", true) == "Arcane Runtime [held]");
    CHECK(Arcane::Remote::HeldWindowTitle("Arcane Runtime", false) == "Arcane Runtime");
    CHECK(Arcane::Remote::HeldWindowTitle("", true) == " [held]");
}

#endif
