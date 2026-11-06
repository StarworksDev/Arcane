#include <catch2/catch_test_macros.hpp>
#include <Arcane/Crypto/Crypto.hpp>  // first after Catch2: its Logger.hpp defines SPDLOG_ACTIVE_LEVEL
#include <Arcane/Remote/Commands/PlayCommands.hpp>
#include <Arcane/Remote/Commands/StatsCommands.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/TickGate.hpp>
#include <Arcane/Remote/FrameStats.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Http.hpp>      // MakeToken
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>
#include <Astra/Registry/Registry.hpp>
#include <App/PlayMode.hpp>            // the editor program's CountsAsSimAdvance / PlayTopologyFromRemote (source-compiled)
#include <Servitor/Client.hpp>
#include <Servitor/FanOut.hpp>
#include <Servitor/Verbs.hpp>
#include "RemoteTestHttp.hpp"     // the one HttpExchange (ruling S3-5); includes RemoteTestSupport.hpp
#include <Json.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(ARC_BUILD_DIST)

// Installs one case's play hooks, play gate and JobTable* and clears all of
// them on scope exit, so no later case reads this case's stack objects
// (rulings F11, S3-9).
struct RemotePlayScope {
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::PlayHooks> hooks;
    RemoteTest::ScopedRemotePointer<Arcane::Remote::JobTable> jobs;
    RemoteTest::ScopedRemotePointer<Arcane::Remote::HostGate> gate;
    RemotePlayScope(const Arcane::Remote::PlayHooks& h, Arcane::Remote::JobTable& j, Arcane::Remote::HostGate* g)
        : hooks([](const Arcane::Remote::PlayHooks* p) {
                    Arcane::Remote::SetPlayHooks(p ? *p : Arcane::Remote::PlayHooks{});
                }, &h),
          jobs(&Arcane::Remote::RegisterPlayCommands, &j),
          gate(&Arcane::Remote::SetPlayGate, g) {}
};

TEST_CASE("step 10 advances host.frame by exactly 10 while status still answers", "[remote][play]") {
    Arcane::Remote::TickGate tick;
    tick.SetRunning(false);
    tick.SetPaused(true);
    Arcane::Remote::FrameStats stats;
    Arcane::Remote::PlayHooks hooks;
    hooks.gate = &tick;
    hooks.stats = &stats;
    hooks.undoReason = "runtime-host";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.frame = 100;
    gate.kind = "server";
    gate.state = "ready";
    RemotePlayScope play(hooks, jobs, &gate);   // SetPlayGate(&gate): the arm-time frame is 100 (ruling F11)
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    snap->frame = 100;
    atoms.status.store(snap);
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    const auto audit = std::filesystem::temp_directory_path() / "remote-play-audit" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);   // calls jobs.SetQueue(&queue)
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const std::string body = R"({"command":"play.step","args":{"n":10}})";
    std::string posted;
    std::thread worker([&] {
        posted = RemoteTest::HttpExchange(listener.Port(), listener.Token(), "", "POST /remote/v1/call HTTP/1.1", body);
    });
    const auto start = gate.frame.load();
    for (int spin = 0; spin < 200 && queue.Depth() == 0; ++spin)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    dispatcher.Drain(1000.0, 8);   // runs play.step: arms the job, binds the parked POST as its waiter
    // The step job is Running and its POST is parked while /status answers (ruling F30).
    CHECK(jobs.AnyRunning({"play.step"}));
    const std::string status = RemoteTest::HttpExchange(listener.Port(), listener.Token(), "", "GET /remote/v1/status HTTP/1.1");
    CHECK(status.find("200") != std::string::npos);
    CHECK(jobs.AnyRunning({"play.step"}));
    int advanced = 0;
    while (tick.Consume() != Arcane::Remote::TickDecision::Idle) {
        gate.frame.fetch_add(1);
        Arcane::Remote::NoteAdvance(stats, gate.frame.load(), 0.016, 1.0, 0.0);
        ++advanced;
        dispatcher.Drain(1000.0, 8);
        snap->frame = gate.frame.load();
        atoms.status.store(std::make_shared<Arcane::Remote::HostStatus>(*snap));
    }
    worker.join();
    listener.Stop();
    CHECK(advanced == 10);
    CHECK(gate.frame.load() - start == 10);
    CHECK_FALSE(jobs.AnyRunning({"play.step"}));
    CHECK(tick.Consume() == Arcane::Remote::TickDecision::Idle);
    CHECK(posted.find("\"ok\":true") != std::string::npos);
    CHECK(posted.find("\"frame\":110") != std::string::npos);   // StepAll stamped gate.Block() (ruling F2)
    CHECK(posted.find("runtime-host") != std::string::npos);      // the armed undo block rode through
    CHECK(stats.frame == 110);
    CHECK(stats.renderMs == 0.0);
    Arcane::Remote::Call later;
    later.command = "stats.frame";
    RemoteTest::ScopedRemotePointer<Arcane::Remote::FrameStats> statsGuard(&Arcane::Remote::RegisterStatsCommand, &stats);
    const auto after = Arcane::Remote::CommandRegistry::Get().Invoke(later).envelope;
    CHECK(after.data["frame"] == 110);
}

TEST_CASE("step rejects n outside 1..100000 and does not arm", "[remote][play]") {
    Arcane::Remote::TickGate tick;
    Arcane::Remote::PlayHooks hooks;
    hooks.gate = &tick;
    hooks.undoReason = "runtime-host";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);   // SetPlayGate(&gate) (ruling F11)
    Arcane::Remote::Call call;
    call.command = "play.step";
    call.args = nlohmann::json{{"n", 0}};
    const auto zero = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(zero.completesJob == 0);
    CHECK(zero.envelope.errors[0].code == "arg.inapplicable");
    CHECK(Arcane::Remote::ExitFromHost(zero.envelope) == 2);
    CHECK(tick.StepsRemaining() == 0);
    call.args["n"] = 100001;
    const auto huge = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(huge.completesJob == 0);
    CHECK(tick.StepsRemaining() == 0);
}

TEST_CASE("play start does not advance, and stop returns to Idle", "[remote][play]") {
    Arcane::Remote::TickGate tick;
    tick.SetRunning(false);
    tick.SetPaused(true);
    Arcane::Remote::PlayHooks hooks;
    hooks.gate = &tick;
    hooks.undoReason = "runtime-host";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);
    Arcane::Remote::Call start;
    start.command = "play.start";
    const auto began = Arcane::Remote::CommandRegistry::Get().Invoke(start).envelope;
    CHECK(began.ok);
    CHECK(tick.Running());
    CHECK_FALSE(tick.Paused());
    CHECK(tick.Consume() == Arcane::Remote::TickDecision::Run);
    Arcane::Remote::Call stop;
    stop.command = "play.stop";
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(stop).envelope.ok);
    CHECK_FALSE(tick.Running());
    CHECK(tick.Consume() == Arcane::Remote::TickDecision::Idle);
    Arcane::Remote::Call listen;
    listen.command = "play.start";
    listen.args = nlohmann::json{{"topology", "listen"}};
    const auto refused = Arcane::Remote::CommandRegistry::Get().Invoke(listen).envelope;
    CHECK(refused.errors[0].code == "arg.inapplicable");
    CHECK(Arcane::Remote::ExitFromHost(refused) == 2);
    CHECK_FALSE(tick.Running());
}

static std::string g_topology;
static bool EditorPlayTopology(void*, std::string_view topology) {
    g_topology = std::string(topology);
    return true;
}

TEST_CASE("editor play passes the topology and refuses an unknown one", "[remote][play]") {
    g_topology.clear();
    Arcane::Remote::PlayHooks hooks;
    hooks.editor = true;
    hooks.editorPlay = &EditorPlayTopology;
    hooks.undoReason = "play-mode";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);
    Arcane::Remote::Call start;
    start.command = "play.start";
    start.args = nlohmann::json{{"topology", "client+server"}};
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(start).envelope.ok);
    CHECK(g_topology == "client+server");
    g_topology.clear();
    start.args = nlohmann::json{{"topology", "mesh"}};
    const auto bad = Arcane::Remote::CommandRegistry::Get().Invoke(start).envelope;
    CHECK(bad.errors[0].code == "arg.invalid");
    CHECK(Arcane::Remote::ExitFromHost(bad) == 2);
    CHECK(g_topology.empty());
}

TEST_CASE("server --frames counts sim advances, so a remote pause does not spend it", "[remote][play]") {
    // Documentation of the ServerApp loop shape after this task (ruling S3-3):
    // the --frames bound is rep.framesTicked, which only the Advance branch
    // increments. This models the loop; Task 30's live
    // "server --frames 1200 ticks exactly 1200 across a remote pause, step and resume"
    // case drives the real ArcaneServer and is the proof.
    constexpr std::uint64_t kFrames = 8;
    Arcane::Remote::TickGate tick;
    tick.SetRunning(true);
    tick.SetPaused(false);
    std::uint64_t framesTicked = 0;
    std::uint64_t iterations = 0;
    auto iterate = [&] {
        ++iterations;
        if (tick.Consume() != Arcane::Remote::TickDecision::Idle)
            ++framesTicked;
    };
    for (int i = 0; i < 2; ++i) iterate();
    tick.SetPaused(true);
    for (int i = 0; i < 5; ++i) iterate();      // paused: idle iterations
    CHECK(framesTicked == 2);
    tick.RequestSteps(3);
    for (int i = 0; i < 6; ++i) iterate();      // three steps, then idle again
    CHECK(framesTicked == 5);
    tick.SetPaused(false);
    while (framesTicked < kFrames) iterate();   // resume until the budget is ticked
    CHECK(framesTicked == kFrames);
    CHECK(iterations > kFrames);
}

static int g_editorSteps = 0;
static void EditorOneStep(void*) { ++g_editorSteps; }

TEST_CASE("editor step calls RequestSingleStep once per drain, not n times", "[remote][play]") {
    g_editorSteps = 0;
    Arcane::Remote::PlayHooks hooks;
    hooks.editor = true;
    hooks.editorRequestSingleStep = &EditorOneStep;
    hooks.undoReason = "play-mode";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    gate.frame = 4;
    RemotePlayScope play(hooks, jobs, &gate);
    Arcane::Remote::Call call;
    call.command = "play.step";
    call.args = nlohmann::json{{"n", 3}};
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(armed.completesJob != 0);
    CHECK(g_editorSteps == 0);
    jobs.StepAll(gate);
    CHECK(g_editorSteps == 1);
    gate.frame = 5;
    jobs.StepAll(gate);
    CHECK(g_editorSteps == 2);
    REQUIRE(armed.envelope.undo.has_value());
    CHECK(armed.envelope.undo->reason == "play-mode");
}

// ---- Task 29 additions beyond the brief's six cases ------------------------

namespace {

Arcane::Remote::CommandResult InvokePlay(const char* command, nlohmann::json args = nlohmann::json::object()) {
    Arcane::Remote::Call call;
    call.command = command;
    call.args = std::move(args);
    return Arcane::Remote::CommandRegistry::Get().Invoke(call);
}

void EditorLoopStep(void* user) { static_cast<Arcane::RunLoop*>(user)->RequestSingleStep(); }

bool g_editorPlaying = false;
bool g_editorPaused = false;
bool EditorIsPlaying(void*) { return g_editorPlaying; }
bool EditorPlayRefuses(void*, std::string_view) { return false; }
void EditorSetPaused(bool paused, void*) { g_editorPaused = paused; }

} // namespace

// Ruling A: the editor's AdvanceSim asks CountsAsSimAdvance before Advance and
// bumps host.frame once when it says so (EditorAppFrame.cpp). EditorApp itself
// cannot be built in ArcaneTests (a window, a device, a project), so this
// drives the same predicate against a real RunLoop in the editor's frame
// order: the Drain (StepAll) at the loop head, then the advance.
TEST_CASE("an editor-style advance moves host.frame once per sim advance, so editor step completes", "[remote][play]") {
    Astra::Registry reg;
    Arcane::SystemSchedulers sch(nullptr);
    Arcane::RunLoop loop(reg, sch);
    loop.SetPaused(true);   // Play, paused (the editor's pause hook)
    Arcane::Remote::PlayHooks hooks;
    hooks.editor = true;
    hooks.editorRequestSingleStep = &EditorLoopStep;
    hooks.editorUser = &loop;
    hooks.undoReason = "play-mode";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);

    int fixedTicks = 0;
    const auto editorAdvance = [&](bool playing) {
        const bool counts = Arcane::Editor::CountsAsSimAdvance(playing, loop);
        loop.Advance(1.0 / 60.0, [&](double) { ++fixedTicks; }, {});
        if (counts)
            gate.frame.fetch_add(1);
    };
    editorAdvance(true);   // paused, no step requested: not a sim advance
    CHECK(gate.frame.load() == 0);
    CHECK(fixedTicks == 0);

    const auto armed = InvokePlay("play.step", {{"n", 3}});
    REQUIRE(armed.completesJob != 0);
    const Arcane::Remote::JobId id{armed.completesJob};
    int frames = 0;
    while (jobs.AnyRunning({"play.step"}) && frames < 20) {
        jobs.StepAll(gate);    // the Drain at the editor's loop head
        editorAdvance(true);   // AdvanceSim later in the same frame
        ++frames;
    }
    CHECK_FALSE(jobs.AnyRunning({"play.step"}));
    REQUIRE(jobs.Find(id) != nullptr);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Done);
    CHECK(jobs.Find(id)->envelope.data.at("advanced") == 3);
    CHECK(gate.frame.load() == 3);   // one per requested single step, none for the finishing frame
    CHECK(fixedTicks == 3);
    CHECK(frames == 4);

    loop.SetPaused(false);
    editorAdvance(true);    // a running Play frame is one advance (whatever its sub-steps)
    CHECK(gate.frame.load() == 4);
    editorAdvance(false);   // an Edit frame never is
    CHECK(gate.frame.load() == 4);
}

TEST_CASE("the editor maps the three topology spellings, and the host check agrees", "[remote][play]") {
    using Topo = Arcane::Editor::PlayTopology;
    CHECK(Arcane::Editor::PlayTopologyFromRemote("standalone") == Topo::Standalone);
    CHECK(Arcane::Editor::PlayTopologyFromRemote("listen") == Topo::ListenServer);
    CHECK(Arcane::Editor::PlayTopologyFromRemote("client+server") == Topo::EmbeddedServer);
    for (const std::string_view spelling : {"standalone", "listen", "client+server", "client", "mesh", ""}) {
        INFO(std::string(spelling));
        CHECK(Arcane::Remote::IsPlayTopology(spelling) == Arcane::Editor::PlayTopologyFromRemote(spelling).has_value());
    }
}

TEST_CASE("runtime resume starts a held gate, pause holds it, and stop answers a pending step", "[remote][play]") {
    Arcane::Remote::TickGate tick;
    tick.SetRunning(false);
    tick.SetPaused(true);   // held at frame 0 (an open-ended --remote run)
    Arcane::Remote::PlayHooks hooks;
    hooks.gate = &tick;
    hooks.undoReason = "runtime-host";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RequestQueue queue;
    jobs.SetQueue(&queue);
    Arcane::Remote::HostGate gate;
    gate.frame = 7;
    RemotePlayScope play(hooks, jobs, &gate);

    const auto resumed = InvokePlay("play.resume").envelope;
    CHECK(resumed.ok);
    CHECK(tick.Running());
    CHECK_FALSE(tick.Held());
    CHECK(tick.Consume() == Arcane::Remote::TickDecision::Run);
    REQUIRE(resumed.undo.has_value());
    CHECK(resumed.undo->reason == "runtime-host");
    CHECK_FALSE(resumed.undo->undoable);
    CHECK(resumed.undo->label.is_null());

    CHECK(InvokePlay("play.pause").envelope.ok);
    CHECK(tick.Held());
    CHECK(tick.Running());
    CHECK(tick.Consume() == Arcane::Remote::TickDecision::Idle);

    Arcane::Remote::Call stepCall;
    stepCall.command = "play.step";
    stepCall.args = nlohmann::json{{"n", 5}};
    REQUIRE(queue.TryPush(9, stepCall));
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(stepCall);
    REQUIRE(armed.completesJob != 0);
    jobs.BindWaiter(Arcane::Remote::JobId{armed.completesJob}, 9, armed.envelope.undo);   // as Drain does
    CHECK(armed.envelope.data.at("startedFrame") == 7);
    CHECK(tick.StepsRemaining() == 5);
    CHECK(tick.Consume() == Arcane::Remote::TickDecision::Step);
    gate.frame.fetch_add(1);
    jobs.StepAll(gate);   // 1 of 5
    CHECK(jobs.AnyRunning({"play.step"}));
    CHECK(jobs.Find(Arcane::Remote::JobId{armed.completesJob})->startedFrame == 7);   // the arm-time frame, on the row
    Arcane::Remote::CommandResult out;
    CHECK_FALSE(queue.TryTake(9, out));

    CHECK(InvokePlay("play.stop").envelope.ok);
    CHECK_FALSE(tick.Running());
    CHECK(tick.Paused());
    CHECK(tick.StepsRemaining() == 0);
    CHECK(tick.Consume() == Arcane::Remote::TickDecision::Idle);
    // No tick will come for the step now: stop answered its parked call.
    CHECK_FALSE(jobs.AnyRunning({"play.step"}));
    REQUIRE(queue.TryTake(9, out));
    REQUIRE_FALSE(out.envelope.errors.empty());
    CHECK_FALSE(out.envelope.ok);
    CHECK(out.envelope.errors[0].code == "play.stopped");
    CHECK(Arcane::Remote::ExitFromHost(out.envelope) == 6);
    CHECK(jobs.Find(Arcane::Remote::JobId{armed.completesJob})->state == Arcane::Remote::JobState::Failed);
    jobs.SetQueue(nullptr);
}

TEST_CASE("a runtime step whose pending steps a free run cleared ends play.stopped once held", "[remote][play]") {
    Arcane::Remote::TickGate tick;   // running: a step on a running host waits for N frames
    Arcane::Remote::PlayHooks hooks;
    hooks.gate = &tick;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);
    const auto armed = InvokePlay("play.step", {{"n", 3}});
    REQUIRE(armed.completesJob != 0);
    REQUIRE(armed.envelope.undo.has_value());
    CHECK(armed.envelope.undo->reason == "runtime-host");   // the default when the hooks name none
    CHECK(tick.Consume() == Arcane::Remote::TickDecision::Run);   // a free run drops the pending steps
    gate.frame.fetch_add(1);
    jobs.StepAll(gate);
    CHECK(jobs.AnyRunning({"play.step"}));
    tick.SetPaused(true);   // held, nothing pending, 1 of 3: no tick is coming
    jobs.StepAll(gate);
    const auto* row = jobs.Find(Arcane::Remote::JobId{armed.completesJob});
    REQUIRE(row != nullptr);
    CHECK(row->state == Arcane::Remote::JobState::Failed);
    CHECK(row->envelope.errors[0].code == "play.stopped");
}

TEST_CASE("an editor in Edit refuses pause, resume and step, and a refused Play is play.refused", "[remote][play]") {
    g_editorPlaying = false;
    g_editorPaused = false;
    g_editorSteps = 0;
    Arcane::Remote::PlayHooks hooks;
    hooks.editor = true;
    hooks.editorPlay = &EditorPlayRefuses;
    hooks.editorSetPaused = &EditorSetPaused;
    hooks.editorRequestSingleStep = &EditorOneStep;
    hooks.editorPlaying = &EditorIsPlaying;
    hooks.undoReason = "play-mode";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);

    for (const char* command : {"play.pause", "play.resume"}) {
        INFO(command);
        const auto env = InvokePlay(command).envelope;
        REQUIRE_FALSE(env.errors.empty());
        CHECK(env.errors[0].code == "play.not-playing");
        CHECK(Arcane::Remote::ExitFromHost(env) == 6);
    }
    CHECK_FALSE(g_editorPaused);
    const auto step = InvokePlay("play.step", {{"n", 2}});
    CHECK(step.completesJob == 0);
    CHECK(step.envelope.errors[0].code == "play.not-playing");
    CHECK(g_editorSteps == 0);
    const auto start = InvokePlay("play.start").envelope;
    REQUIRE_FALSE(start.errors.empty());
    CHECK(start.errors[0].code == "play.refused");
    CHECK(Arcane::Remote::ExitFromHost(start) == 6);

    // Inside Play they act, with the play-mode undo reason.
    g_editorPlaying = true;
    const auto paused = InvokePlay("play.pause").envelope;
    CHECK(paused.ok);
    CHECK(g_editorPaused);
    REQUIRE(paused.undo.has_value());
    CHECK(paused.undo->reason == "play-mode");
    CHECK(InvokePlay("play.resume").envelope.ok);
    CHECK_FALSE(g_editorPaused);
    // A step whose Play ends under it is play.stopped, not a hang.
    const auto armed = InvokePlay("play.step", {{"n", 2}});
    REQUIRE(armed.completesJob != 0);
    jobs.StepAll(gate);
    CHECK(g_editorSteps == 1);
    g_editorPlaying = false;
    jobs.StepAll(gate);
    CHECK(g_editorSteps == 1);
    const auto* row = jobs.Find(Arcane::Remote::JobId{armed.completesJob});
    REQUIRE(row != nullptr);
    CHECK(row->state == Arcane::Remote::JobState::Failed);
    CHECK(row->envelope.errors[0].code == "play.stopped");
}

namespace {
bool EditorStopRefuses(void*) { return false; }
}

TEST_CASE("a refused editor play.stop fails the pending step instead of leaving it to time out", "[remote][play]") {
    g_editorPlaying = true;
    g_editorPaused = false;
    g_editorSteps = 0;
    Arcane::Remote::PlayHooks hooks;
    hooks.editor = true;
    hooks.editorSetPaused = &EditorSetPaused;
    hooks.editorRequestSingleStep = &EditorOneStep;
    hooks.editorPlaying = &EditorIsPlaying;
    hooks.editorStop = &EditorStopRefuses;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);

    const auto armed = InvokePlay("play.step", {{"n", 5}});
    REQUIRE(armed.completesJob != 0);
    const auto stop = InvokePlay("play.stop").envelope;
    REQUIRE_FALSE(stop.errors.empty());
    CHECK(stop.errors[0].code == "play.refused");
    // The pending step is finished now, not left waiting for a client timeout.
    const auto* row = jobs.Find(Arcane::Remote::JobId{armed.completesJob});
    REQUIRE(row != nullptr);
    CHECK(row->state == Arcane::Remote::JobState::Failed);
    REQUIRE_FALSE(row->envelope.errors.empty());
    CHECK(row->envelope.errors[0].code == "play.stopped");
}

TEST_CASE("play commands answer host.unavailable unarmed, and a new table ends a running step play.stopped", "[remote][play]") {
    Arcane::Remote::TickGate tick;
    tick.SetRunning(false);
    tick.SetPaused(true);
    Arcane::Remote::PlayHooks hooks;
    hooks.gate = &tick;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RequestQueue queue;
    jobs.SetQueue(&queue);
    Arcane::Remote::HostGate gate;
    {
        RemotePlayScope play(hooks, jobs, &gate);
        Arcane::Remote::Call stepCall;
        stepCall.command = "play.step";
        stepCall.args = nlohmann::json{{"n", 4}};
        REQUIRE(queue.TryPush(3, stepCall));
        const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(stepCall);
        REQUIRE(armed.completesJob != 0);
        jobs.BindWaiter(Arcane::Remote::JobId{armed.completesJob}, 3, armed.envelope.undo);
        Arcane::Remote::JobTable other;
        Arcane::Remote::RegisterPlayCommands(&other);   // a later call replaces the table (ruling F11)
        Arcane::Remote::CommandResult out;
        REQUIRE(queue.TryTake(3, out));   // the step was finished, its call answered
        // Fix round 1 (M5): ticks may already have run, so the answer is
        // play.stopped (exit 6), never host.unavailable (exit 5, which the
        // client retries and would step again).
        REQUIRE_FALSE(out.envelope.errors.empty());
        CHECK(out.envelope.errors[0].code == "play.stopped");
        CHECK(Arcane::Remote::ExitFromHost(out.envelope) == 6);
        CHECK(jobs.Find(Arcane::Remote::JobId{armed.completesJob})->state == Arcane::Remote::JobState::Cancelled);
        Arcane::Remote::RegisterPlayCommands(&jobs);    // back before `other` goes out of scope
        tick.ClearSteps();
    }
    // The scope nulled the table, the gate and the hooks (ruling S3-9).
    for (const char* command : {"play.start", "play.stop", "play.pause", "play.resume", "play.step"}) {
        INFO(command);
        const auto result = InvokePlay(command);
        CHECK(result.completesJob == 0);
        REQUIRE_FALSE(result.envelope.errors.empty());
        CHECK(result.envelope.errors[0].code == "host.unavailable");
        CHECK(Arcane::Remote::ExitFromHost(result.envelope) == 5);
    }
    CHECK_FALSE(tick.Running());
    CHECK(tick.StepsRemaining() == 0);
    jobs.SetQueue(nullptr);
}

TEST_CASE("play commands are Drive: a read-only host refuses them and --world is refused", "[remote][play]") {
    Arcane::Remote::TickGate tick;
    tick.SetRunning(false);
    tick.SetPaused(true);
    Arcane::Remote::PlayHooks hooks;
    hooks.gate = &tick;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.readOnly = true;
    RemotePlayScope play(hooks, jobs, &gate);
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    for (const char* name : {"play.start", "play.stop", "play.pause", "play.resume", "play.step"}) {
        INFO(name);
        const Arcane::Remote::Desc* desc = reg.Find(name);
        REQUIRE(desc != nullptr);
        CHECK(desc->access == Arcane::Remote::Access::Drive);
        CHECK(desc->mutates == Arcane::Remote::Mutates::PlayMode);
        CHECK_FALSE(desc->needsFrame);
        CHECK_FALSE(desc->worldScoped);
        CHECK(desc->thread == (std::string_view(name) == "play.step" ? Arcane::Remote::ExecThread::Job
                                                                      : Arcane::Remote::ExecThread::Main));
    }
    const auto audit = std::filesystem::temp_directory_path() / "remote-play-drive" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call start;
    start.command = "play.start";
    REQUIRE(queue.TryPush(1, start));
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult out;
    REQUIRE(queue.TryTake(1, out));
    CHECK(out.envelope.errors[0].code == "permission.read-only");
    CHECK(Arcane::Remote::ExitFromHost(out.envelope) == 3);
    CHECK_FALSE(tick.Running());

    gate.readOnly = false;
    Arcane::Remote::Call world;
    world.command = "play.step";
    world.world = "main";
    REQUIRE(queue.TryPush(2, world));
    dispatcher.Drain(1000.0, 8);
    REQUIRE(queue.TryTake(2, out));
    CHECK(out.envelope.errors[0].code == "arg.inapplicable");
    CHECK(Arcane::Remote::ExitFromHost(out.envelope) == 2);
    CHECK(tick.StepsRemaining() == 0);
    CHECK_FALSE(jobs.AnyRunning({"play.step"}));
    jobs.SetQueue(nullptr);
}

// Ruling F12 / B: HostSession::Start wires the play commands to its own tick
// gate, starts an open-ended --remote runtime held, and Stop unwires them.
TEST_CASE("a held runtime session starts held and play.start through its wiring runs it", "[remote][play]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    const auto root = std::filesystem::temp_directory_path() / "remote-play-session";
    std::filesystem::remove_all(root);
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root;
    cfg.holdSim = true;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK(session.Tick().Held());
    const auto started = InvokePlay("play.start").envelope;
    CHECK(started.ok);
    CHECK_FALSE(session.Tick().Held());
    CHECK(session.Tick().Consume() == Arcane::Remote::TickDecision::Run);
    REQUIRE(started.undo.has_value());
    CHECK(started.undo->reason == "runtime-host");
    const auto armed = InvokePlay("play.step", {{"n", 2}});
    REQUIRE(armed.completesJob != 0);
    CHECK(armed.envelope.data.at("startedFrame") == session.Gate().frame.load());
    session.Stop("test");
    const auto after = InvokePlay("play.start").envelope;
    REQUIRE_FALSE(after.errors.empty());
    CHECK(after.errors[0].code == "host.unavailable");
    std::filesystem::remove_all(root);
}

namespace {

std::string g_runningTopology = "standalone";
int g_editorPlayCalls = 0;
bool EditorPlayCounts(void*, std::string_view) { ++g_editorPlayCalls; return true; }
std::string_view EditorRunningTopology(void*) { return g_runningTopology; }

} // namespace

// Fix round 1 (I2): an editor already in Play answers its ACTUAL topology and
// refuses an explicit different one; PlaySession::Play is never re-called.
TEST_CASE("play.start on a playing editor reports the running topology and refuses a different one", "[remote][play]") {
    g_editorPlaying = true;
    g_runningTopology = "standalone";
    g_editorPlayCalls = 0;
    Arcane::Remote::PlayHooks hooks;
    hooks.editor = true;
    hooks.editorPlay = &EditorPlayCounts;
    hooks.editorPlaying = &EditorIsPlaying;
    hooks.editorTopology = &EditorRunningTopology;
    hooks.undoReason = "play-mode";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);

    const auto differs = InvokePlay("play.start", {{"topology", "listen"}}).envelope;
    REQUIRE_FALSE(differs.errors.empty());
    CHECK(differs.errors[0].code == "play.refused");
    CHECK(Arcane::Remote::ExitFromHost(differs) == 6);
    CHECK(differs.errors[0].detail.at("running") == "standalone");
    CHECK(g_editorPlayCalls == 0);

    for (const nlohmann::json& args : {nlohmann::json::object(), nlohmann::json{{"topology", "standalone"}}}) {
        INFO(args.dump());
        const auto same = InvokePlay("play.start", args).envelope;
        CHECK(same.ok);
        CHECK(same.data.at("topology") == "standalone");   // the running one, not a default or a request
        CHECK(same.data.at("alreadyPlaying") == true);
    }
    g_runningTopology = "client+server";
    const auto implicit = InvokePlay("play.start").envelope;   // no --topology: "standalone" is only the default
    CHECK(implicit.ok);
    CHECK(implicit.data.at("topology") == "client+server");
    CHECK(g_editorPlayCalls == 0);

    // In Edit, Play runs and the answer names what the session started.
    g_editorPlaying = false;
    g_runningTopology = "listen";
    const auto started = InvokePlay("play.start", {{"topology", "listen"}}).envelope;
    CHECK(started.ok);
    CHECK(g_editorPlayCalls == 1);
    CHECK(started.data.at("topology") == "listen");
    CHECK_FALSE(started.data.contains("alreadyPlaying"));
}

// Fix round 1 (I1): the one flip rule every editor Play/Stop site runs
// (EditorApp::AfterPlayFlip applies it; the toolbar, servitor play/stop, the
// save modal and ClearSceneReferences all call that). Without the shared rule
// a remote Play left the toolbar's "editor:play" row standing.
TEST_CASE("the editor's flip rule names the same follow-ups for every Play and Stop site", "[remote][play][editor]") {
    const auto play = Arcane::Editor::PlanPlayFlip(false, true);
    CHECK(play.settingsWorldCreated);
    CHECK(play.clearPlayRefusal);   // a successful Play retracts "editor:play", from the toolbar or servitor
    CHECK(play.sceneReplaced);
    CHECK_FALSE(play.stopServerProcess);
    const auto stop = Arcane::Editor::PlanPlayFlip(true, false);
    CHECK(stop.stopServerProcess);
    CHECK(stop.sceneReplaced);
    CHECK_FALSE(stop.settingsWorldCreated);
    CHECK_FALSE(stop.clearPlayRefusal);
    for (const bool mode : {false, true}) {   // a refused Play, or a Play/Stop no-op: nothing follows
        const auto none = Arcane::Editor::PlanPlayFlip(mode, mode);
        CHECK_FALSE(none.stopServerProcess);
        CHECK_FALSE(none.settingsWorldCreated);
        CHECK_FALSE(none.clearPlayRefusal);
        CHECK_FALSE(none.sceneReplaced);
    }
    using Topo = Arcane::Editor::PlayTopology;
    for (const Topo t : {Topo::Standalone, Topo::ListenServer, Topo::EmbeddedServer}) {
        const std::string_view name = Arcane::Editor::PlayTopologyRemoteName(t);
        CHECK(Arcane::Editor::PlayTopologyFromRemote(name) == t);   // the reported name round-trips
    }
    CHECK(Arcane::Editor::PlayTopologyRemoteName(Topo::ClientOnly) == "client");
    CHECK_FALSE(Arcane::Remote::IsPlayTopology("client"));   // reported, never accepted in P0
}

TEST_CASE("servitor play, stop, pause, resume and step build their play.* calls", "[servitor][client]") {
    struct Row { const char* words; const char* command; };
    for (const Row r : {Row{"play", "play.start"}, Row{"stop", "play.stop"}, Row{"pause", "play.pause"},
                        Row{"resume", "play.resume"}, Row{"step", "play.step"}}) {
        INFO(r.words);
        const Servitor::VerbRow* row = Servitor::FindVerb(r.words);
        REQUIRE(row != nullptr);
        REQUIRE(row->command != nullptr);
        CHECK(std::string(row->command) == r.command);
        CHECK(Servitor::PostsCall(*row));   // --world rides the call; the host refuses it (Rule 3)
        CHECK(Servitor::IsDriveCommand(nlohmann::json{{"command", r.command}}));   // --all needs --yes
    }

    nlohmann::json out;
    std::string error;
    const Servitor::VerbRow* play = Servitor::FindVerb("play");
    REQUIRE(play->args(std::vector<std::string>{}, out, &error));
    CHECK(out == nlohmann::json::object());
    REQUIRE(play->args(std::vector<std::string>{"--topology", "client+server"}, out, &error));
    CHECK(out == nlohmann::json{{"topology", "client+server"}});
    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {"--topology", "mesh"},
             {"--topology"},
             {"standalone"},
             {"--topology", "listen", "--topology", "listen"},
             {"--bogus"}}) {
        error.clear();
        CHECK_FALSE(play->args(bad, out, &error));
        CHECK_FALSE(error.empty());
    }

    const Servitor::VerbRow* step = Servitor::FindVerb("step");
    REQUIRE(step->args(std::vector<std::string>{}, out, &error));
    CHECK(out == nlohmann::json::object());   // the host's default n is 1
    REQUIRE(step->args(std::vector<std::string>{"10"}, out, &error));
    CHECK(out == nlohmann::json{{"n", 10}});
    REQUIRE(step->args(std::vector<std::string>{"0"}, out, &error));   // the host owns the range (arg.inapplicable)
    CHECK(out == nlohmann::json{{"n", 0}});
    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {"-1"}, {"1.5"}, {"ten"}, {"1", "2"}, {"--n", "3"}, {"9999999999"}}) {
        error.clear();
        CHECK_FALSE(step->args(bad, out, &error));
        CHECK_FALSE(error.empty());
    }

    // stop, pause and resume take nothing: Run refuses any token before discovery (usage exit 2).
    const char* stopNow[] = {"servitor", "stop", "now"};
    CHECK(Servitor::Run(3, const_cast<char**>(stopNow)) == 2);
    const char* pauseFlag[] = {"servitor", "pause", "--bogus"};
    CHECK(Servitor::Run(3, const_cast<char**>(pauseFlag)) == 2);
}
// ---- Final fix pass ----------------------------------------------------------

namespace {

Arcane::Remote::CommandResult InvokeStep(std::uint32_t n, std::uint32_t timeoutMs) {
    Arcane::Remote::Call call;
    call.command = "play.step";
    call.args = nlohmann::json{{"n", n}};
    call.timeoutMs = timeoutMs;
    return Arcane::Remote::CommandRegistry::Get().Invoke(call);
}

bool g_primaryPaused = true;
bool EditorPrimaryPaused(void*) { return g_primaryPaused; }

} // namespace

// G-1 / C-I2 (b): two overlapping steps from one frame share the gate's ticks.
// The sim advances to the larger target and no further; every caller is
// answered when its own target is reached, and nothing runs afterwards.
TEST_CASE("overlapping steps finish at the larger request, not the sum", "[remote][play]") {
    Arcane::Remote::TickGate tick;
    tick.SetRunning(false);
    tick.SetPaused(true);
    Arcane::Remote::PlayHooks hooks;
    hooks.gate = &tick;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);
    const auto a = InvokeStep(10, 30000);
    const auto b = InvokeStep(10, 30000);
    REQUIRE(a.completesJob != 0);
    REQUIRE(b.completesJob != 0);
    CHECK(tick.StepsRemaining() == 10);
    const auto c = InvokeStep(4, 30000);   // a shorter one inside the same window adds nothing
    REQUIRE(c.completesJob != 0);
    CHECK(tick.StepsRemaining() == 10);
    int advanced = 0;
    while (tick.Consume() == Arcane::Remote::TickDecision::Step && advanced < 100) {
        gate.frame.fetch_add(1);
        ++advanced;
        jobs.StepAll(gate);
    }
    CHECK(advanced == 10);
    CHECK(gate.frame.load() == 10);
    for (const auto& armed : {a, b, c}) {
        const auto* row = jobs.Find(Arcane::Remote::JobId{armed.completesJob});
        REQUIRE(row != nullptr);
        CHECK(row->state == Arcane::Remote::JobState::Done);
    }
    CHECK(jobs.Find(Arcane::Remote::JobId{c.completesJob})->envelope.data.at("advanced") == 4);   // its own target
    CHECK(tick.Consume() == Arcane::Remote::TickDecision::Idle);   // nothing runs after every caller was answered
}

// C-I2 (a): play.step is bounded on the host like host.wait. At its deadline
// it answers ok with data.timedOut (exit 8, the spec's wait timeout) and gives
// back the ticks no other step still needs.
TEST_CASE("a step that outlives its timeout stops and answers timedOut", "[remote][play]") {
    Arcane::Remote::TickGate tick;
    tick.SetRunning(false);
    tick.SetPaused(true);
    Arcane::Remote::PlayHooks hooks;
    hooks.gate = &tick;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);
    const auto longStep = InvokeStep(20, 30);     // gives up after 30 ms
    const auto shortStep = InvokeStep(5, 30000);
    REQUIRE(longStep.completesJob != 0);
    REQUIRE(shortStep.completesJob != 0);
    CHECK(tick.StepsRemaining() == 20);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(tick.Consume() == Arcane::Remote::TickDecision::Step);
        gate.frame.fetch_add(1);
        jobs.StepAll(gate);
    }
    CHECK(jobs.AnyRunning({"play.step"}));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    jobs.StepAll(gate);
    const auto* timed = jobs.Find(Arcane::Remote::JobId{longStep.completesJob});
    REQUIRE(timed != nullptr);
    CHECK(timed->state == Arcane::Remote::JobState::Done);
    CHECK(timed->envelope.ok);
    CHECK(timed->envelope.errors.empty());
    CHECK(timed->envelope.data.at("timedOut") == true);
    CHECK(timed->envelope.data.at("advanced") == 2);
    CHECK(Arcane::Remote::ExitFromHost(timed->envelope) == 8);
    CHECK(tick.StepsRemaining() == 3);   // the short step still needs 3 of its 5; the rest is dropped
    while (tick.Consume() == Arcane::Remote::TickDecision::Step) {
        gate.frame.fetch_add(1);
        jobs.StepAll(gate);
    }
    CHECK(gate.frame.load() == 5);
    CHECK(jobs.Find(Arcane::Remote::JobId{shortStep.completesJob})->state == Arcane::Remote::JobState::Done);
    // Alone, a timed-out step clears every pending tick it asked for.
    const auto alone = InvokeStep(50, 1);
    REQUIRE(alone.completesJob != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    jobs.StepAll(gate);
    CHECK(jobs.Find(Arcane::Remote::JobId{alone.completesJob})->envelope.data.at("timedOut") == true);
    CHECK(tick.StepsRemaining() == 0);
}

// G-2: on the editor a single step replaces the frame's accumulated dt, so it
// is asked for only while the primary loop is paused. A running editor
// advances host.frame every pass by itself: the step waits on it, as a running
// runtime's does.
TEST_CASE("editor step requests single steps only while the primary loop is paused", "[remote][play]") {
    g_editorSteps = 0;
    g_primaryPaused = false;
    Arcane::Remote::PlayHooks hooks;
    hooks.editor = true;
    hooks.editorRequestSingleStep = &EditorOneStep;
    hooks.editorPaused = &EditorPrimaryPaused;
    hooks.undoReason = "play-mode";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::HostGate gate;
    RemotePlayScope play(hooks, jobs, &gate);
    const auto armed = InvokeStep(3, 30000);
    REQUIRE(armed.completesJob != 0);
    jobs.StepAll(gate);
    gate.frame.fetch_add(1);   // the running loop's own pass
    jobs.StepAll(gate);
    CHECK(g_editorSteps == 0);
    g_primaryPaused = true;    // paused under the step: from now on one single step per Drain
    jobs.StepAll(gate);
    CHECK(g_editorSteps == 1);
    gate.frame.fetch_add(1);
    jobs.StepAll(gate);
    CHECK(g_editorSteps == 2);
    gate.frame.fetch_add(1);
    jobs.StepAll(gate);
    CHECK(g_editorSteps == 2);   // the third advance finished it; no extra request
    CHECK(jobs.Find(Arcane::Remote::JobId{armed.completesJob})->state == Arcane::Remote::JobState::Done);
    g_primaryPaused = true;
}

#endif
