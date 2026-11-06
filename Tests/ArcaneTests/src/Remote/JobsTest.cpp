#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Dispatcher.hpp>   // HostGate (this task)
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Listener.hpp>     // RequestQueue (Task 6)
#include <chrono>
#include <vector>

#if !defined(ARC_BUILD_DIST)

TEST_CASE("finished jobs prune at 10 minutes and at 64", "[remote][job]") {
    Arcane::Remote::JobTable table;
    std::vector<Arcane::Remote::JobId> ids;
    for (int i = 0; i < 70; ++i) {
        auto id = table.Arm("capture.viewport", 1, nullptr, nullptr);
        table.Finish(id, Arcane::Remote::MakeOk("capture.viewport", nlohmann::json::object(), {}),
                     Arcane::Remote::JobState::Done);
        ids.push_back(id);
    }
    table.Prune(0);
    CHECK(table.FinishedCount() == 64);
    CHECK(table.Find(ids.front()) == nullptr);
    CHECK(table.Find(ids.back()) != nullptr);
    auto live = table.Arm("host.wait", 1, nullptr, nullptr);
    table.Prune(9'000'000'000);
    CHECK(table.Find(live) != nullptr);
    CHECK(table.Find(live)->state == Arcane::Remote::JobState::Running);
}

TEST_CASE("StepAll advances one running job by one and leaves the next for later", "[remote][job]") {
    Arcane::Remote::JobTable table;
    int hits = 0;
    auto id = table.Arm("play.step", 3,
        [](Arcane::Remote::Job& job, Arcane::Remote::HostGate&, void* user) {
            *static_cast<int*>(user) += 1;
            job.progress.done += 1;
            return job.progress.done >= job.progress.total;
        }, &hits);
    Arcane::Remote::HostGate gate;
    table.StepAll(gate);
    CHECK(hits == 1);
    CHECK(table.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(table.Find(id)->progress.done == 1);
    table.StepAll(gate);
    table.StepAll(gate);
    CHECK(hits == 3);
    CHECK(table.Find(id)->state == Arcane::Remote::JobState::Done);
    table.StepAll(gate);
    CHECK(hits == 3);
}

TEST_CASE("AnyRunning is true only for a Running row of the named command", "[remote][job]") {
    Arcane::Remote::JobTable table;
    CHECK_FALSE(table.AnyRunning({"capture.window", "capture.compare"}));
    auto id = table.Arm("capture.window", 1, nullptr, nullptr);
    CHECK(table.AnyRunning({"capture.window"}));
    CHECK(table.AnyRunning({"capture.compare", "capture.window"}));
    CHECK_FALSE(table.AnyRunning({"capture.compare"}));
    table.Finish(id, Arcane::Remote::MakeOk("capture.window", nlohmann::json::object(), {}),
                 Arcane::Remote::JobState::Done);
    CHECK_FALSE(table.AnyRunning({"capture.window"}));
}

TEST_CASE("CancelBySubscriber cancels only the keyed job", "[remote][job]") {
    Arcane::Remote::JobTable table;
    int hits = 0;
    auto follow = table.Arm("log.follow", 0,
        [](Arcane::Remote::Job&, Arcane::Remote::HostGate&, void* user) {
            *static_cast<int*>(user) += 1;
            return false;
        }, &hits);
    table.SetSubscriber(follow, 7);
    auto other = table.Arm("play.step", 3, nullptr, nullptr);
    table.CancelBySubscriber(7);
    CHECK(table.Find(follow)->state == Arcane::Remote::JobState::Cancelled);
    CHECK(table.Find(other)->state == Arcane::Remote::JobState::Running);
    Arcane::Remote::HostGate gate;
    table.StepAll(gate);
    CHECK(hits == 0);
    table.CancelBySubscriber(7);
    CHECK(table.Find(other)->state == Arcane::Remote::JobState::Running);
}

TEST_CASE("a finished job is kept for 10 minutes and dropped after", "[remote][job]") {
    Arcane::Remote::JobTable table;
    auto id = table.Arm("capture.window", 1, nullptr, nullptr);
    table.Finish(id, Arcane::Remote::MakeOk("capture.window", nlohmann::json::object(), {}),
                 Arcane::Remote::JobState::Done);
    REQUIRE(table.Find(id) != nullptr);
    const std::int64_t finished = table.Find(id)->finishedUnixMs;
    table.Prune(finished + 599999);
    CHECK(table.Find(id) != nullptr);
    table.Prune(finished + 600001);
    CHECK(table.Find(id) == nullptr);
}

TEST_CASE("StepAll completes the bound waiter with the gate's host block and the bound undo", "[remote][job]") {
    Arcane::Remote::JobTable table;
    Arcane::Remote::RequestQueue queue;
    table.SetQueue(&queue);
    Arcane::Remote::Call call;
    call.command = "play.step";
    REQUIRE(queue.TryPush(5, call));
    auto id = table.Arm("play.step", 2,
        [](Arcane::Remote::Job& job, Arcane::Remote::HostGate&, void*) {
            job.progress.done += 1;
            job.envelope = Arcane::Remote::MakeOk(job.command, nlohmann::json{{"done", job.progress.done}}, {});
            return job.progress.done >= job.progress.total;
        }, nullptr);
    Arcane::Remote::UndoBlock undo;
    undo.reason = "runtime-host";
    table.BindWaiter(id, 5, undo);
    Arcane::Remote::HostGate gate;
    gate.kind = "server";
    Arcane::Remote::CommandResult out;
    table.StepAll(gate);
    CHECK_FALSE(queue.TryTake(5, out));
    gate.frame.store(110);
    table.StepAll(gate);
    REQUIRE(queue.TryTake(5, out));
    CHECK(out.envelope.ok);
    CHECK(out.envelope.data["done"] == 2);
    CHECK(out.envelope.host.frame == 110);
    CHECK(out.envelope.host.kind == "server");
    REQUIRE(out.envelope.undo.has_value());
    CHECK(out.envelope.undo->reason == "runtime-host");
    CHECK(table.Find(id)->state == Arcane::Remote::JobState::Done);
    table.SetQueue(nullptr);
}

#endif
