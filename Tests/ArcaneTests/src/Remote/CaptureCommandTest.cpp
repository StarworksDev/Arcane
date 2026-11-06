#include <catch2/catch_test_macros.hpp>
#include <Arcane/Crypto/Crypto.hpp>  // first after Catch2: its Logger.hpp defines SPDLOG_ACTIVE_LEVEL
#include <Arcane/Remote/Commands/CaptureCommands.hpp>
#include <Arcane/Remote/Commands/SessionCommands.hpp>   // kMaxWaitTimeoutMs, BoundedJobTimeoutMs (final fix pass)
#include <limits>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include "RemoteDemoArgs.hpp"
#include "RemoteTestSupport.hpp"
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Verbs.hpp>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <cstdint>
#include <filesystem>
#include <vector>

#if !defined(ARC_BUILD_DIST)

// Installs one case's hooks and JobTable* and clears both on scope exit, so a
// later case never sees this case's stack objects (rulings F11, S3-9).
struct RemoteCaptureScope {
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::CaptureHooks> hooks;
    RemoteTest::ScopedRemotePointer<Arcane::Remote::JobTable> jobs;
    RemoteCaptureScope(const Arcane::Remote::CaptureHooks& h, Arcane::Remote::JobTable& j)
        : hooks([](const Arcane::Remote::CaptureHooks* p) {
                    Arcane::Remote::SetCaptureHooks(p ? *p : Arcane::Remote::CaptureHooks{});
                }, &h),
          jobs(&Arcane::Remote::RegisterCaptureCommands, &j) {}
};

static const std::uint8_t kProbe[12] = {10, 20, 30, 255, 1, 2, 3, 40, 9, 9, 9, 9};
static const std::uint8_t kProbeB[12] = {40, 50, 60, 255, 7, 8, 9, 40, 9, 9, 9, 9};

struct CapUser {
    int copies = 0;
    int reads = 0;
    int idles = 0;
    std::uint64_t armedTicket = 0;
    std::uint64_t wanted = 0;
    bool retired = false;
    bool failMap = false;
    std::uint64_t cancelled = 0;   // the last ticket the cancel hook was given (final fix pass, C-M12)
    int cancels = 0;
};

static void CancelSlot(std::uint64_t ticket, void* user) {
    auto* u = static_cast<CapUser*>(user);
    ++u->cancels;
    u->cancelled = ticket;
}

static bool ArmSlot(std::uint64_t ticket, void* user) {
    auto* u = static_cast<CapUser*>(user);
    ++u->copies;
    u->armedTicket = ticket;
    u->wanted = ticket;
    return true;
}
static bool DrainSlot(std::uint64_t ticket, Arcane::Remote::CaptureMapped* raw, void* user) {
    auto* u = static_cast<CapUser*>(user);
    if (!u->retired || ticket != u->armedTicket)
        return false;
    raw->width = 2;
    raw->height = 1;
    raw->rowPitch = 12;
    raw->bgra = true;
    if (u->failMap) {
        raw->mapped = nullptr;
        raw->mappedBytes = 0;
        return true;
    }
    ++u->reads;
    raw->mapped = kProbe;
    raw->mappedBytes = sizeof(kProbe);
    return true;
}
static Arcane::Remote::CommandResult Ping(const Arcane::Remote::Call& call, void*) {
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeOk(call.command, nlohmann::json::object(), {});
    return r;
}
static bool ArmNo(std::uint64_t, void* user) {
    ++static_cast<CapUser*>(user)->copies;
    return false;
}

TEST_CASE("NormalizeCaptureRows drops padding and swaps BGRA", "[remote][capture]") {
    std::vector<unsigned char> rgba;
    Arcane::NormalizeCaptureRows(kProbe, 2, 1, 12, true, rgba);
    REQUIRE(rgba.size() == 8);
    CHECK(rgba[0] == 30);
    CHECK(rgba[1] == 20);
    CHECK(rgba[2] == 10);
    CHECK(rgba[3] == 255);
    CHECK(rgba[4] == 3);
    CHECK(rgba[5] == 2);
    CHECK(rgba[6] == 1);
    CHECK(rgba[7] == 40);
}

TEST_CASE("the copy step reads nothing and the job completes when the ticket is published", "[remote][capture]") {
    CapUser user;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks hooks;
    hooks.framesInFlight = 2;
    hooks.armWindow = &ArmSlot;
    hooks.drainWindow = &DrainSlot;
    hooks.user = &user;
    hooks.outDir = std::filesystem::temp_directory_path() / "remote-cap";
    std::filesystem::create_directories(hooks.outDir);
    RemoteCaptureScope scope(hooks, jobs);
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    Arcane::Remote::Desc ping;
    ping.name = "cap.ping";
    ping.summary = "observe while a capture is in flight";
    ping.access = Arcane::Remote::Access::Observe;
    ping.module = "cap-test";
    ping.argsMeta = Astra::GetMeta<RemoteDemoArgs>();
    ping.fn = &Ping;
    ping.needsFrame = false;
    REQUIRE(reg.Register(ping, &err));
    Arcane::Remote::Call call;
    call.command = "capture.window";
    call.args = nlohmann::json::object();
    const auto armed = reg.Invoke(call);
    REQUIRE(armed.completesJob != 0);
    CHECK(user.copies == 0);
    CHECK(user.reads == 0);
    CHECK(user.idles == 0);
    Arcane::Remote::HostGate gate;
    gate.frame = 10;
    gate.minimized = false;
    jobs.StepAll(gate);
    const auto id = Arcane::Remote::JobId{armed.completesJob};
    REQUIRE(jobs.Find(id) != nullptr);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(jobs.Find(id)->progress.label == "copied");
    CHECK(jobs.Find(id)->startedFrame == 0);
    CHECK(gate.frame == 10);
    CHECK(user.copies == 1);
    CHECK(user.armedTicket != 0);
    CHECK(user.reads == 0);
    CHECK(user.idles == 0);
    Arcane::Remote::RequestQueue queue;
    const auto audit = hooks.outDir / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call pingCall;
    pingCall.command = "cap.ping";
    REQUIRE(queue.TryPush(2, pingCall));
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult pinged;
    REQUIRE(queue.Wait(2, pinged, std::chrono::milliseconds(1000)));
    CHECK(pinged.envelope.ok);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(user.copies == 1);
    CHECK(user.reads == 0);
    CHECK(user.idles == 0);
    CHECK(gate.frame == 10);
    jobs.StepAll(gate);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(user.reads == 0);
    CHECK(user.copies == 1);
    CHECK(user.idles == 0);
    CHECK(gate.frame == 10);
    user.retired = true;
    jobs.StepAll(gate);
    REQUIRE(jobs.Find(id) != nullptr);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Done);
    CHECK(jobs.Find(id)->envelope.ok);
    CHECK(user.reads == 1);
    CHECK(user.idles == 0);
    CHECK(gate.frame == 10);
    const auto& art = jobs.Find(id)->envelope.artifacts[0];
    CHECK(art.format == "RGBA8_UNORM");
    CHECK(art.width == 2);
    CHECK(art.height == 1);
    CHECK(art.sha256.size() == 64);
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> png;
    REQUIRE(Arcane::LoadPngRgba(art.path, w, h, png));
    CHECK(w == 2);
    CHECK(h == 1);
    REQUIRE(png.size() == 8);
    CHECK(png[0] == 30);
    CHECK(png[1] == 20);
    CHECK(png[2] == 10);
    CHECK(png[3] == 255);
    CHECK(png[4] == 3);
    CHECK(png[5] == 2);
    CHECK(png[6] == 1);
    CHECK(png[7] == 40);
    reg.UnregisterModule("cap-test");
}

struct OverlapUser {
    int copies = 0;
    int reads = 0;
    int frames = 0;
    int framesInFlight = 2;
    std::uint64_t wanted = 0;
    std::uint64_t ticket[2]{};
    int retireFrame[2]{-1, -1};
};

static bool ArmOverlap(std::uint64_t ticket, void* user) {
    auto* u = static_cast<OverlapUser*>(user);
    REQUIRE(u->copies < 2);
    u->ticket[u->copies] = ticket;
    u->wanted = ticket;
    u->retireFrame[u->copies] = u->frames + u->framesInFlight;
    ++u->copies;
    return true;
}

static bool DrainOverlap(std::uint64_t ticket, Arcane::Remote::CaptureMapped* raw, void* user) {
    auto* u = static_cast<OverlapUser*>(user);
    int slot = -1;
    if (u->copies > 0 && ticket == u->ticket[0])
        slot = 0;
    else if (u->copies > 1 && ticket == u->ticket[1])
        slot = 1;
    if (slot < 0 || u->frames < u->retireFrame[slot])
        return false;
    raw->width = 2;
    raw->height = 1;
    raw->rowPitch = 12;
    raw->bgra = true;
    raw->mapped = slot == 0 ? kProbe : kProbeB;
    raw->mappedBytes = 12;
    ++u->reads;
    return true;
}

static void ExpectPng(const Arcane::Remote::Artifact& art, unsigned char r, unsigned char g,
                       unsigned char b) {
    CHECK(art.format == "RGBA8_UNORM");
    CHECK(art.width == 2);
    CHECK(art.height == 1);
    CHECK(art.sha256.size() == 64);
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> png;
    REQUIRE(Arcane::LoadPngRgba(art.path, w, h, png));
    CHECK(w == 2);
    CHECK(h == 1);
    REQUIRE(png.size() == 8);
    CHECK(png[0] == r);
    CHECK(png[1] == g);
    CHECK(png[2] == b);
    CHECK(png[3] == 255);
}

TEST_CASE("overlapping captures: B is capture-pending until A's ticket retires, then the retry succeeds", "[remote][capture]") {
    OverlapUser user;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks hooks;
    hooks.framesInFlight = 2;
    hooks.armViewport = &ArmOverlap;
    hooks.drainViewport = &DrainOverlap;
    hooks.armWindow = &ArmOverlap;
    hooks.drainWindow = &DrainOverlap;
    hooks.user = &user;
    hooks.outDir = std::filesystem::temp_directory_path() / "remote-cap-busy";
    std::filesystem::create_directories(hooks.outDir);
    RemoteCaptureScope scope(hooks, jobs);
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    Arcane::Remote::HostGate gate;
    gate.frame = 10;

    Arcane::Remote::Call captureA;
    captureA.command = "capture.window";
    captureA.args = nlohmann::json::object();
    const auto armed = reg.Invoke(captureA);
    REQUIRE(armed.completesJob != 0);
    jobs.StepAll(gate);
    const auto idA = Arcane::Remote::JobId{armed.completesJob};
    REQUIRE(jobs.Find(idA) != nullptr);
    CHECK(jobs.Find(idA)->state == Arcane::Remote::JobState::Running);
    CHECK(user.copies == 1);
    CHECK(user.reads == 0);
    CHECK(user.frames == 0);
    CHECK(user.wanted != 0);
    const auto ticketA = user.wanted;
    CHECK(user.retireFrame[0] == 2);
    CHECK(gate.frame == 10);

    Arcane::Remote::Call captureB;
    captureB.command = "capture.viewport";
    captureB.args = nlohmann::json::object();
    const auto busy = reg.Invoke(captureB);
    CHECK(busy.completesJob == 0);
    CHECK_FALSE(busy.envelope.ok);
    CHECK(busy.envelope.errors[0].code == "host.busy");
    CHECK(busy.envelope.errors[0].detail["state"] == "capture-pending");
    CHECK(Arcane::Remote::ExitFromHost(busy.envelope) == 5);
    CHECK(user.copies == 1);
    CHECK(user.reads == 0);
    CHECK(user.wanted == ticketA);
    CHECK(jobs.Find(idA)->state == Arcane::Remote::JobState::Running);
    CHECK(gate.frame == 10);

    for (int i = 0; i < 2; ++i) {
        ++user.frames;
        jobs.StepAll(gate);
    }
    REQUIRE(jobs.Find(idA) != nullptr);
    CHECK(jobs.Find(idA)->state == Arcane::Remote::JobState::Done);
    CHECK(jobs.Find(idA)->envelope.ok);
    CHECK(user.reads == 1);
    CHECK(user.frames == 2);
    CHECK(user.wanted == ticketA);
    CHECK(user.copies == 1);
    CHECK(gate.frame == 10);
    ExpectPng(jobs.Find(idA)->envelope.artifacts[0], 30, 20, 10);

    const auto retried = reg.Invoke(captureB);
    REQUIRE(retried.completesJob != 0);
    jobs.StepAll(gate);
    const auto idB = Arcane::Remote::JobId{retried.completesJob};
    CHECK(jobs.Find(idB)->state == Arcane::Remote::JobState::Running);
    CHECK(user.copies == 2);
    CHECK(user.wanted != ticketA);
    CHECK(user.retireFrame[1] == user.frames + 2);
    CHECK(user.reads == 1);
    CHECK(gate.frame == 10);
    for (int i = 0; i < 2; ++i) {
        ++user.frames;
        jobs.StepAll(gate);
    }
    CHECK(jobs.Find(idB)->state == Arcane::Remote::JobState::Done);
    CHECK(jobs.Find(idB)->envelope.ok);
    CHECK(user.reads == 2);
    CHECK(user.frames == 4);
    CHECK(gate.frame == 10);
    ExpectPng(jobs.Find(idB)->envelope.artifacts[0], 60, 50, 40);
    ExpectPng(jobs.Find(idA)->envelope.artifacts[0], 30, 20, 10);
}

TEST_CASE("a server with null hooks returns capture.no-renderer and arms nothing", "[remote][capture]") {
    CapUser user;
    Arcane::Remote::JobTable jobs;
    const Arcane::Remote::CaptureHooks none{};   // a server: all six pointers null
    RemoteCaptureScope scope(none, jobs);
    Arcane::Remote::Call call;
    call.command = "capture.window";
    const auto result = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(result.completesJob == 0);
    CHECK_FALSE(result.envelope.ok);
    CHECK(result.envelope.errors[0].code == "capture.no-renderer");
    CHECK(Arcane::Remote::ExitFromHost(result.envelope) == 2);
    CHECK(user.copies == 0);
    CHECK(user.reads == 0);
    CHECK(jobs.FinishedCount() == 0);
}

TEST_CASE("a false arm finishes capture.no-frame without a read", "[remote][capture]") {
    CapUser user;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks hooks;
    hooks.framesInFlight = 2;
    hooks.armWindow = &ArmNo;
    hooks.drainWindow = &DrainSlot;
    hooks.user = &user;
    hooks.outDir = std::filesystem::temp_directory_path() / "remote-cap-arm";
    std::filesystem::create_directories(hooks.outDir);
    RemoteCaptureScope scope(hooks, jobs);
    Arcane::Remote::Call call;
    call.command = "capture.window";
    call.args = nlohmann::json::object();
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    REQUIRE(armed.completesJob != 0);
    Arcane::Remote::HostGate gate;
    gate.frame = 4;
    jobs.StepAll(gate);
    const auto* job = jobs.Find(Arcane::Remote::JobId{armed.completesJob});
    REQUIRE(job != nullptr);
    CHECK(job->state == Arcane::Remote::JobState::Failed);
    CHECK_FALSE(job->envelope.ok);
    CHECK(job->envelope.errors[0].code == "capture.no-frame");
    CHECK(Arcane::Remote::ExitFromHost(job->envelope) == 6);
    CHECK(user.copies == 1);
    CHECK(user.reads == 0);
    CHECK(gate.frame == 4);
}

TEST_CASE("a published ticket with a null map finishes capture.no-frame", "[remote][capture]") {
    CapUser user;
    user.failMap = true;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks hooks;
    hooks.framesInFlight = 2;
    hooks.armWindow = &ArmSlot;
    hooks.drainWindow = &DrainSlot;
    hooks.user = &user;
    hooks.outDir = std::filesystem::temp_directory_path() / "remote-cap-drain";
    std::filesystem::create_directories(hooks.outDir);
    RemoteCaptureScope scope(hooks, jobs);
    Arcane::Remote::Call call;
    call.command = "capture.window";
    call.args = nlohmann::json::object();
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    REQUIRE(armed.completesJob != 0);
    Arcane::Remote::HostGate gate;
    gate.frame = 4;
    jobs.StepAll(gate);
    const auto id = Arcane::Remote::JobId{armed.completesJob};
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(user.reads == 0);
    CHECK(gate.frame == 4);
    user.retired = true;
    jobs.StepAll(gate);
    const auto* job = jobs.Find(id);
    REQUIRE(job != nullptr);
    CHECK(job->state == Arcane::Remote::JobState::Failed);
    CHECK_FALSE(job->envelope.ok);
    CHECK(job->envelope.errors[0].code == "capture.no-frame");
    CHECK(Arcane::Remote::ExitFromHost(job->envelope) == 6);
    CHECK(user.reads == 0);
    CHECK(user.idles == 0);
    CHECK(gate.frame == 4);
}

// ---- Task 27 fix round 1 ----------------------------------------------------

namespace {

Arcane::Remote::CaptureHooks WindowHooks(CapUser& user, const std::filesystem::path& outDir) {
    Arcane::Remote::CaptureHooks hooks;
    hooks.framesInFlight = 2;
    hooks.armWindow = &ArmSlot;
    hooks.drainWindow = &DrainSlot;
    hooks.cancelWindow = &CancelSlot;
    hooks.user = &user;
    hooks.outDir = outDir;
    return hooks;
}

Arcane::Remote::CommandResult InvokeCapture(const nlohmann::json& args, std::uint32_t timeoutMs = 30000) {
    Arcane::Remote::Call call;
    call.command = "capture.window";
    call.args = args;
    call.timeoutMs = timeoutMs;
    return Arcane::Remote::CommandRegistry::Get().Invoke(call);
}

} // namespace

TEST_CASE("a pending capture past its deadline is capture.no-frame timeout; minimized is capture.minimized", "[remote][capture]") {
    CapUser user;   // never retired: every drain answers false
    Arcane::Remote::JobTable jobs;
    const auto hooks = WindowHooks(user, std::filesystem::temp_directory_path() / "remote-cap-deadline");
    RemoteCaptureScope scope(hooks, jobs);
    Arcane::Remote::HostGate gate;
    gate.frame = 7;

    const auto timedOut = InvokeCapture(nlohmann::json::object(), 1);
    REQUIRE(timedOut.completesJob != 0);
    jobs.StepAll(gate);   // arms
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    jobs.StepAll(gate);   // pending, past the 1 ms deadline, not minimized
    const auto* job = jobs.Find(Arcane::Remote::JobId{timedOut.completesJob});
    REQUIRE(job != nullptr);
    CHECK(job->state == Arcane::Remote::JobState::Failed);
    CHECK(job->envelope.errors[0].code == "capture.no-frame");
    CHECK(job->envelope.errors[0].detail["reason"] == "timeout");
    CHECK(Arcane::Remote::ExitFromHost(job->envelope) == 6);
    // Final fix pass (C-M12, G-M5): a failed ticket is cleared from the ring,
    // so it cannot hold a slot or land in the next capture's bytes.
    CHECK(user.cancels == 1);
    CHECK(user.cancelled == user.armedTicket);

    // Minimized: answered on the first pending step, long before a 30 s deadline.
    const auto minimized = InvokeCapture(nlohmann::json::object());
    REQUIRE(minimized.completesJob != 0);
    jobs.StepAll(gate);   // arms
    gate.minimized.store(true);
    jobs.StepAll(gate);
    const auto* job2 = jobs.Find(Arcane::Remote::JobId{minimized.completesJob});
    REQUIRE(job2 != nullptr);
    CHECK(job2->state == Arcane::Remote::JobState::Failed);
    CHECK(job2->envelope.errors[0].code == "capture.minimized");
    CHECK(Arcane::Remote::ExitFromHost(job2->envelope) == 5);
    CHECK(user.cancels == 2);
    CHECK(user.cancelled == user.armedTicket);   // the minimized capture's own ticket
    CHECK(user.reads == 0);
    CHECK(gate.frame == 7);
}

TEST_CASE("releasing the capture table cancels an armed capture and completes its waiter", "[remote][capture]") {
    CapUser user;
    Arcane::Remote::JobTable jobs;
    const auto hooks = WindowHooks(user, std::filesystem::temp_directory_path() / "remote-cap-cancel");
    RemoteCaptureScope scope(hooks, jobs);
    Arcane::Remote::HostGate gate;
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::RemoteDispatcher dispatcher(queue, Arcane::Remote::CommandRegistry::Get(), gate, jobs,
                                                hooks.outDir / "audit.ndjson");
    Arcane::Remote::Call call;
    call.command = "capture.window";
    REQUIRE(queue.TryPush(41, call));
    dispatcher.Drain(1000.0, 8);   // runs the command: the job is armed and the waiter bound
    dispatcher.Drain(1000.0, 8);   // steps it once: the copy is armed
    CHECK(user.copies == 1);
    REQUIRE(jobs.AnyRunning({"capture.window"}));

    Arcane::Remote::RegisterCaptureCommands(nullptr);   // what HostSession::Teardown does
    CHECK_FALSE(jobs.AnyRunning({"capture.window"}));
    Arcane::Remote::CommandResult answered;
    REQUIRE(queue.Wait(41, answered, std::chrono::milliseconds(1000)));
    CHECK_FALSE(answered.envelope.ok);
    CHECK(answered.envelope.errors[0].code == "host.unavailable");
    user.retired = true;
    jobs.StepAll(gate);   // the cancelled row is not stepped again; nothing dangles
    CHECK(user.reads == 0);
    // A torn-down session answers host.unavailable, not capture.no-renderer.
    const auto after = InvokeCapture(nlohmann::json::object());
    CHECK(after.completesJob == 0);
    CHECK(after.envelope.errors[0].code == "host.unavailable");
    CHECK(Arcane::Remote::ExitFromHost(after.envelope) == 5);
}

TEST_CASE("an unwritable capture path is capture.artifact-failed and leaves no .tmp", "[remote][capture]") {
    const auto blocker = std::filesystem::temp_directory_path() / "remote-cap-blocker";
    std::filesystem::remove_all(blocker);
    { std::ofstream(blocker) << "a plain file where the out dir should be"; }
    CapUser user;
    user.retired = true;
    Arcane::Remote::JobTable jobs;
    const auto hooks = WindowHooks(user, blocker);
    RemoteCaptureScope scope(hooks, jobs);
    Arcane::Remote::HostGate gate;
    const auto armed = InvokeCapture(nlohmann::json::object());
    REQUIRE(armed.completesJob != 0);
    jobs.StepAll(gate);
    jobs.StepAll(gate);
    const auto* job = jobs.Find(Arcane::Remote::JobId{armed.completesJob});
    REQUIRE(job != nullptr);
    CHECK(job->state == Arcane::Remote::JobState::Failed);
    CHECK(job->envelope.errors[0].code == "capture.artifact-failed");
    CHECK(Arcane::Remote::ExitFromHost(job->envelope) == 6);
    CHECK(job->envelope.artifacts.empty());
    CHECK(std::filesystem::is_regular_file(blocker));
    std::filesystem::remove(blocker);
}

TEST_CASE("capture args: relative, non-png and unknown keys are refused before anything is armed", "[remote][capture]") {
    CapUser user;
    Arcane::Remote::JobTable jobs;
    const auto hooks = WindowHooks(user, std::filesystem::temp_directory_path() / "remote-cap-args");
    RemoteCaptureScope scope(hooks, jobs);
    const auto abs = (std::filesystem::temp_directory_path() / "shot.jpg").generic_u8string();
    const std::vector<std::pair<nlohmann::json, std::string>> refused = {
        {{{"out", "relative/shot.png"}}, "arg.invalid"},
        {{{"out", std::string(abs.begin(), abs.end())}}, "arg.invalid"},
        {{{"out", ""}}, "arg.invalid"},
        {{{"out", 3}}, "arg.invalid"},
        {{{"bogus", true}}, "arg.unknown"},
        // Not UTF-8: refused by an explicit check on every platform (POSIX paths take any
        // bytes, so the Windows conversion throw is not the check), never an escaped exception.
        {{{"out", std::string(abs.begin(), abs.end() - 4) + "\xff.png"}}, "arg.invalid"},
        {{{"out", std::string(abs.begin(), abs.end() - 4) + "\xed\xa0\x80.png"}}, "arg.invalid"},   // a UTF-16 surrogate
        {{{"out", std::string(abs.begin(), abs.end() - 4) + "\xe2\x82.png"}}, "arg.invalid"},       // a truncated sequence
    };
    for (const auto& [args, code] : refused) {
        const auto result = InvokeCapture(args);
        CHECK(result.completesJob == 0);
        REQUIRE_FALSE(result.envelope.errors.empty());
        CHECK(result.envelope.errors[0].code == code);
        CHECK(Arcane::Remote::ExitFromHost(result.envelope) == 2);
    }
    CHECK(jobs.FinishedCount() == 0);
    CHECK_FALSE(jobs.AnyRunning({"capture.window"}));
    CHECK(user.copies == 0);
}

TEST_CASE("a non-ASCII --out path is written as given, hashed from the written bytes, named with the pid", "[remote][capture]") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::u8string(u8"remote-cap-\u00e9\u4e2d");
    std::filesystem::remove_all(dir);
    CapUser user;
    user.retired = true;
    Arcane::Remote::JobTable jobs;
    const auto hooks = WindowHooks(user, dir);
    RemoteCaptureScope scope(hooks, jobs);
    Arcane::Remote::HostGate gate;
    gate.pid = 4242;

    const std::u8string outUtf8 = (dir / std::u8string(u8"shot-\u00e9.png")).generic_u8string();
    const auto named = InvokeCapture({{"out", std::string(outUtf8.begin(), outUtf8.end())}});
    REQUIRE(named.completesJob != 0);
    jobs.StepAll(gate);
    jobs.StepAll(gate);
    const auto* job = jobs.Find(Arcane::Remote::JobId{named.completesJob});
    REQUIRE(job != nullptr);
    REQUIRE(job->state == Arcane::Remote::JobState::Done);
    const auto& art = job->envelope.artifacts[0];
    CHECK(art.path == std::string(outUtf8.begin(), outUtf8.end()));
    const std::filesystem::path written(std::u8string(art.path.begin(), art.path.end()));
    REQUIRE(std::filesystem::exists(written));
    CHECK(std::filesystem::file_size(written) == art.bytes);
    std::string bytes;
    {
        std::ifstream in(written, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(Arcane::Crypto::Sha256Hex(bytes) == art.sha256);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(written).concat(".tmp")));

    // No --out: <command>-<pid>-<unix ms>-<n>.png in the artifact directory.
    const auto unnamed = InvokeCapture(nlohmann::json::object());
    REQUIRE(unnamed.completesJob != 0);
    jobs.StepAll(gate);
    jobs.StepAll(gate);
    const auto* job2 = jobs.Find(Arcane::Remote::JobId{unnamed.completesJob});
    REQUIRE(job2 != nullptr);
    REQUIRE(job2->state == Arcane::Remote::JobState::Done);
    const std::filesystem::path auto_(std::u8string(job2->envelope.artifacts[0].path.begin(),
                                                    job2->envelope.artifacts[0].path.end()));
    const std::u8string leaf = auto_.filename().u8string();
    CHECK(std::string(leaf.begin(), leaf.end()).starts_with("capture.window-4242-"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("the capture ring re-copies a ticket whose frame never submitted, and drains only after framesInFlight", "[remote][capture]") {
    Arcane::CaptureRing ring;
    ring.framesInFlight = 2;
    ring.wanted = 9;
    // Frame 10, slot 0: the copy is recorded, then Execute fails (no confirm,
    // m_frameIndex stays 10).
    auto step = Arcane::PlanCaptureRingSlot(ring, 0, 10, true);
    CHECK_FALSE(step.drain);
    REQUIRE(step.record);
    Arcane::RecordCaptureRingSlot(ring, 0, 10);
    CHECK(ring.wanted == 0);
    // Frame 10 again, slot 0: the unsubmitted copy is NOT drained as stale bytes; it is copied again.
    step = Arcane::PlanCaptureRingSlot(ring, 0, 10, true);
    CHECK_FALSE(step.drain);
    REQUIRE(step.record);
    CHECK(ring.wanted == 9);
    Arcane::RecordCaptureRingSlot(ring, 0, 10);
    Arcane::ConfirmCaptureRingSubmission(ring, 10);   // this time Execute succeeded
    CHECK(ring.submitted[0]);
    // Frame 11, slot 1: nothing to do.
    step = Arcane::PlanCaptureRingSlot(ring, 1, 11, true);
    CHECK_FALSE(step.drain);
    CHECK_FALSE(step.record);
    // Frame 12, slot 0: framesInFlight frames after the submitted copy, so it drains.
    step = Arcane::PlanCaptureRingSlot(ring, 0, 12, true);
    REQUIRE(step.drain);
    CHECK_FALSE(step.record);
    Arcane::RetireCaptureRingSlot(ring, 0);
    CHECK(ring.haveRetired);
    CHECK(ring.retiredTicket == 9);
    CHECK_FALSE(ring.pending[0]);
}

TEST_CASE("screenshot verbs resolve --out to an absolute .png and refuse the rest", "[servitor][client]") {
    const Servitor::VerbRow* row = Servitor::FindVerb("screenshot window");
    REQUIRE(row != nullptr);
    REQUIRE(row->args != nullptr);
    REQUIRE(Servitor::FindVerb("screenshot viewport") != nullptr);
    nlohmann::json out;
    std::string error;
    const std::vector<std::string> none;
    REQUIRE(row->args(none, out, &error));
    CHECK(out == nlohmann::json::object());
    const std::vector<std::string> rel = {"--out", "shots/a.PNG"};
    REQUIRE(row->args(rel, out, &error));
    const std::string resolved = out.at("out").get<std::string>();
    const std::filesystem::path resolvedPath(std::u8string(resolved.begin(), resolved.end()));
    CHECK(resolvedPath.is_absolute());
    CHECK(resolvedPath.filename() == "a.PNG");
    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {"--out", "a.jpg"}, {"--out"}, {"extra"}, {"--bogus"}}) {
        error.clear();
        CHECK_FALSE(row->args(bad, out, &error));
        CHECK_FALSE(error.empty());
    }
}

namespace {

struct TimeoutHost {
    int posts = 0;
    std::vector<std::uint32_t> timeouts;
};

Servitor::ExchangeResult BusyTwice(const std::string& body, void* user) {
    auto* host = static_cast<TimeoutHost*>(user);
    ++host->posts;
    const auto json = nlohmann::json::parse(body);
    host->timeouts.push_back(json.at("options").at("timeoutMs").get<std::uint32_t>());
    Servitor::ExchangeResult result;
    const std::string cmd = json.at("command").get<std::string>();
    if (host->posts <= 2) {
        result.envelope = Arcane::Remote::MakeError(cmd, "host.busy", "a capture is pending", "retry");
        result.envelope.errors[0].detail = nlohmann::json{{"state", "capture-pending"}};
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return result;
    }
    result.envelope = Arcane::Remote::MakeOk(cmd, nlohmann::json::object(), {});
    return result;
}

} // namespace

TEST_CASE("a busy retry resends options.timeoutMs as the remaining budget", "[servitor][client]") {
    TimeoutHost host;
    Servitor::GlobalOptions opt;
    opt.format = Servitor::Format::Json;
    opt.timeoutMs = 5000;
    Servitor::CallRequest shot;
    shot.command = "capture.window";
    shot.args = nlohmann::json::object();
    Arcane::Remote::ClientBlock client;
    (void)Servitor::Call(shot, opt, &BusyTwice, &host, &client);
    CHECK(client.exitCode == 0);
    CHECK(client.retried);
    REQUIRE(host.timeouts.size() == 3);
    CHECK(host.timeouts[0] == 5000);
    CHECK(host.timeouts[1] < host.timeouts[0]);
    CHECK(host.timeouts[2] < host.timeouts[1]);
    CHECK(host.timeouts[2] >= 1);
    CHECK(host.timeouts[0] - host.timeouts[2] >= 60);   // two 30 ms busy answers plus the backoff
}
// Final fix pass (C-M12, G-M5): cancelling a ticket the job gave up on. A
// still-wanted copy is never recorded; an unsubmitted one frees its slot; a
// submitted one is in flight, so its slot is abandoned: it drains nothing into
// retiredBytes once its frames have passed, and the next copy records there.
TEST_CASE("the capture ring cancels a wanted, an unsubmitted and a submitted ticket", "[remote][capture]") {
    Arcane::CaptureRing ring;
    ring.framesInFlight = 2;
    ring.wanted = 5;
    Arcane::CancelCaptureRingTicket(ring, 4);   // not this ring's: nothing changes
    CHECK(ring.wanted == 5);
    Arcane::CancelCaptureRingTicket(ring, 5);
    CHECK(ring.wanted == 0);

    ring.wanted = 6;
    REQUIRE(Arcane::PlanCaptureRingSlot(ring, 0, 10, true).record);
    Arcane::RecordCaptureRingSlot(ring, 0, 10);   // recorded, its frame not yet submitted
    Arcane::CancelCaptureRingTicket(ring, 6);
    CHECK_FALSE(ring.pending[0]);
    CHECK(ring.wanted == 0);                      // never re-wanted

    ring.wanted = 7;
    REQUIRE(Arcane::PlanCaptureRingSlot(ring, 0, 20, true).record);
    Arcane::RecordCaptureRingSlot(ring, 0, 20);
    Arcane::ConfirmCaptureRingSubmission(ring, 20);   // in flight on the GPU
    Arcane::CancelCaptureRingTicket(ring, 7);
    CHECK(ring.pending[0]);                       // the copy cannot be recalled
    ring.wanted = 8;                              // the retried capture's ticket
    const auto early = Arcane::PlanCaptureRingSlot(ring, 0, 21, true);
    CHECK_FALSE(early.drain);
    CHECK_FALSE(early.record);                    // still in flight: the slot is busy
    const auto later = Arcane::PlanCaptureRingSlot(ring, 0, 22, true);
    CHECK_FALSE(later.drain);                     // abandoned: its bytes never reach retiredBytes
    CHECK(later.record);                          // and the new copy records in its slot
    CHECK_FALSE(ring.haveRetired);
    Arcane::RecordCaptureRingSlot(ring, 0, 22);
    CHECK(ring.ticket[0] == 8);
}

// C-M12: a capture's host-side bound is the wait bound (kMaxWaitTimeoutMs), so
// an abandoned capture cannot hold the one capture slot for up to ~49 days.
TEST_CASE("a job's host-side timeout is clamped to kMaxWaitTimeoutMs", "[remote][capture]") {
    CHECK(Arcane::Remote::BoundedJobTimeoutMs(0, 30000) == 30000);
    CHECK(Arcane::Remote::BoundedJobTimeoutMs(500, 30000) == 500);
    CHECK(Arcane::Remote::BoundedJobTimeoutMs(Arcane::Remote::kMaxWaitTimeoutMs, 30000) ==
          Arcane::Remote::kMaxWaitTimeoutMs);
    CHECK(Arcane::Remote::BoundedJobTimeoutMs(std::numeric_limits<std::uint32_t>::max(), 30000) ==
          Arcane::Remote::kMaxWaitTimeoutMs);
}

#endif
