#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/SessionCommands.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Registry.hpp>
#include "RemoteTestHttp.hpp"
#include "RemoteTestSupport.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if !defined(ARC_BUILD_DIST)

TEST_CASE("wait reload completes when NoteReload bumps the generation", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    auto root = std::filesystem::temp_directory_path() / "remote-wait-reload";
    std::filesystem::remove_all(root);
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root;
    cfg.holdSim = true;
    Arcane::Remote::HostSession session(cfg);
    session.Start();
    const auto& constSession = static_cast<const Arcane::Remote::HostSession&>(session);
    const auto gen = constSession.Gate().pluginGeneration;   // const accessor (ruling F9)
    Arcane::Remote::WaitArgs args;
    args.what = "reload";
    args.timeoutMs = 5000;
    const auto started = std::chrono::steady_clock::now();
    auto pending = Arcane::Remote::WaitPoll(session, args, 0, gen, true, started);
    CHECK_FALSE(pending.data.value("done", false));
    session.NoteReload(gen + 1, true, "reloaded", 0);       // the host passes its PluginHost generation (ruling F10)
    auto done = Arcane::Remote::WaitPoll(session, args, 0, gen, true, started);
    CHECK(done.ok);
    CHECK(done.data.value("done", false));
    CHECK(Arcane::Remote::ExitFromHost(done) == 0);
    session.Stop("test");
}

TEST_CASE("wait settled on a server is capture.no-renderer exit 2", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Server);
    Arcane::Remote::WaitArgs args;
    args.what = "settled";
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Server;
    cfg.cvarContext = Arcane::CVarContext::ServerAdmin;
    cfg.projectRoot = std::filesystem::temp_directory_path() / "remote-wait-server";
    Arcane::Remote::HostSession session(cfg);
    session.Start();
    auto env = Arcane::Remote::WaitPoll(session, args, 0, 0, false, std::chrono::steady_clock::now());
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "capture.no-renderer");
    CHECK(Arcane::Remote::ExitFromHost(env) == 2);
    session.Stop("test");
}

static bool RemoteSettledNever(void*) { return false; }
static bool RemoteArmNever(std::uint64_t, void*) { return true; }
static bool RemoteDrainNever(std::uint64_t, Arcane::Remote::CaptureMapped*, void*) { return false; }

TEST_CASE("wait state times out with ok true and exit 8", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = std::filesystem::temp_directory_path() / "remote-wait-timeout";
    cfg.producersIdle = &RemoteSettledNever;
    cfg.capture.armHost = &RemoteArmNever;      // settled captures through the host pair (Task 30)
    cfg.capture.drainHost = &RemoteDrainNever;
    Arcane::Remote::HostSession session(cfg);
    session.Start();
    session.Gate().SetState("ready");
    Arcane::Remote::WaitArgs args;
    args.what = "state";
    args.state = "playing";
    args.timeoutMs = 0;                                      // in-process: already timed out
    auto env = Arcane::Remote::WaitPoll(session, args, 0, 0, true, std::chrono::steady_clock::now());
    CHECK(env.ok);
    CHECK(env.errors.empty());
    CHECK(env.data["timedOut"] == true);
    CHECK(Arcane::Remote::ExitFromHost(env) == 8);
    // A real elapsed timeout through `started` (ruling F9): 50 ms budget, armed 100 ms ago.
    Arcane::Remote::WaitArgs settled;
    settled.what = "settled";
    settled.timeoutMs = 50;
    const auto armed = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
    auto late = Arcane::Remote::WaitPoll(session, settled, 0, 0, true, armed);
    CHECK(late.ok);
    CHECK(late.data["timedOut"] == true);
    auto early = Arcane::Remote::WaitPoll(session, settled, 0, 0, true, std::chrono::steady_clock::now());
    CHECK_FALSE(early.data.value("done", false));             // pending, and 50 ms have not passed
    CHECK_FALSE(early.data.value("timedOut", false));
    session.Stop("test");
}

namespace
{
Arcane::Remote::HostSession::Config SessionConfig(Arcane::Remote::HostKind kind, const std::filesystem::path& root)
{
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = kind;
    cfg.cvarContext = kind == Arcane::Remote::HostKind::Server ? Arcane::CVarContext::ServerAdmin
                                                               : Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root;
    return cfg;
}

Arcane::Remote::Envelope Invoke(const std::string& command, nlohmann::json args)
{
    Arcane::Remote::Call call;
    call.command = command;
    call.args = std::move(args);
    return Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
}

bool RemoteDirtyAlways(void*) { return true; }

bool RemoteSettledFlag(void* user) { return *static_cast<bool*>(user); }
bool RemoteArmAlways(std::uint64_t, void*) { return true; }
bool RemoteDrainLater(std::uint64_t, Arcane::Remote::CaptureMapped*, void*) { return false; }
} // namespace

TEST_CASE("wait ready, frames and state complete on their conditions; settled is never a poll's to decide", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-conditions");
    bool settledNow = false;
    auto cfg = SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path);
    cfg.producersIdle = &RemoteSettledFlag;
    cfg.hookUser = &settledNow;
    cfg.capture.armHost = &RemoteArmAlways;
    cfg.capture.drainHost = &RemoteDrainLater;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    const auto now = std::chrono::steady_clock::now();

    Arcane::Remote::WaitArgs ready;
    ready.what = "ready";
    CHECK(Arcane::Remote::WaitPoll(session, ready, 0, 0, true, now).data.value("done", false));
    session.Gate().SetState("shutting-down");
    CHECK_FALSE(Arcane::Remote::WaitPoll(session, ready, 0, 0, true, now).data.value("done", false));
    session.Gate().SetState("paused");
    CHECK(Arcane::Remote::WaitPoll(session, ready, 0, 0, true, now).data.value("done", false));

    Arcane::Remote::WaitArgs frames;
    frames.what = "frames";
    frames.frames = 3;
    session.Gate().frame = 7;
    CHECK_FALSE(Arcane::Remote::WaitPoll(session, frames, 5, 0, true, now).data.value("done", false));
    session.Gate().frame = 8;
    const auto framesDone = Arcane::Remote::WaitPoll(session, frames, 5, 0, true, now);
    CHECK(framesDone.data.value("done", false));
    CHECK(framesDone.data["frame"] == 8);

    Arcane::Remote::WaitArgs state;
    state.what = "state";
    state.state = "paused";
    CHECK(Arcane::Remote::WaitPoll(session, state, 0, 0, true, now).data.value("done", false));

    // Task 30 (spec s4.7): settled is two byte-equal captures, which only the
    // host.wait job takes. Idle producers alone no longer settle a poll.
    Arcane::Remote::WaitArgs settled;
    settled.what = "settled";
    settled.timeoutMs = 5000;
    settledNow = true;
    const auto idleOnly = Arcane::Remote::WaitPoll(session, settled, 0, 0, true, now);
    CHECK(idleOnly.ok);
    CHECK_FALSE(idleOnly.data.value("done", true));
    CHECK_FALSE(idleOnly.data.contains("timedOut"));

    // Task 30 (C4): a reload that failed after the wait armed keeps the
    // generation (PluginHost rolled back), and the wait answers the failure
    // with module.reload-failed (exit 6) instead of timing out (exit 8).
    Arcane::Remote::WaitArgs reload;
    reload.what = "reload";
    reload.timeoutMs = 5000;
    const std::uint32_t generation = session.Gate().pluginGeneration;
    const std::uint64_t armedAt = session.ReloadSerial();
    CHECK_FALSE(Arcane::Remote::WaitPoll(session, reload, 0, generation, true, now, armedAt).data.value("done", false));
    session.NoteReload(generation, false, "reload failed: the new module's ABI does not match", 9);
    auto failed = Arcane::Remote::WaitPoll(session, reload, 0, generation, true, now, armedAt);
    CHECK_FALSE(failed.ok);
    REQUIRE_FALSE(failed.errors.empty());
    CHECK(failed.errors[0].code == "module.reload-failed");
    CHECK(failed.errors[0].message == "reload failed: the new module's ABI does not match");
    CHECK(failed.errors[0].detail["pluginGeneration"] == generation);
    CHECK(failed.errors[0].detail["atFrame"] == 9);
    CHECK(Arcane::Remote::ExitFromHost(failed) == 6);
    // A wait armed after that failure is not answered by it: it times out.
    reload.timeoutMs = 50;
    auto later = Arcane::Remote::WaitPoll(session, reload, 0, generation, true,
                                                now - std::chrono::milliseconds(100), session.ReloadSerial());
    CHECK(later.ok);
    CHECK(later.data["timedOut"] == true);
    CHECK(Arcane::Remote::ExitFromHost(later) == 8);
    session.Stop("test");
}

TEST_CASE("DrainFrame notes a failed reload from the attempt count, and the wait answers it", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-reload-failed");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    // The first drain adopts the boot module's generation and attempt count.
    CHECK_FALSE(session.DrainFrame(4, 7, true));
    CHECK(session.ReloadSerial() == 0);
    Arcane::Remote::WaitArgs reload;
    reload.what = "reload";
    reload.timeoutMs = 5000;
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t armedAt = session.ReloadSerial();
    CHECK_FALSE(session.DrainFrame(4, 7, true));   // nothing tried: still pending
    CHECK_FALSE(Arcane::Remote::WaitPoll(session, reload, 0, 4, true, now, armedAt).data.value("done", false));
    // PluginHost tried, failed and rolled back: the generation stays 4.
    CHECK_FALSE(session.DrainFrame(4, 8, false));
    CHECK(session.ReloadSerial() == 1);
    CHECK_FALSE(session.LastReloadOk());
    const auto failed = Arcane::Remote::WaitPoll(session, reload, 0, 4, true, now, armedAt);
    REQUIRE_FALSE(failed.errors.empty());
    CHECK(failed.errors[0].code == "module.reload-failed");
    CHECK(Arcane::Remote::ExitFromHost(failed) == 6);
    // The one-argument form never notes a failure; a successful reload moves
    // the generation and completes a wait armed after the failure.
    const std::uint64_t armedAgain = session.ReloadSerial();
    CHECK_FALSE(session.DrainFrame(4));
    CHECK(session.ReloadSerial() == armedAgain);
    CHECK_FALSE(session.DrainFrame(5, 9, true));
    CHECK(session.LastReloadOk());
    const auto done = Arcane::Remote::WaitPoll(session, reload, 0, 4, true, now, armedAgain);
    CHECK(done.ok);
    CHECK(done.data.value("done", false));
    CHECK(Arcane::Remote::ExitFromHost(done) == 0);
    session.Stop("test");
}

TEST_CASE("wait settled needs the capture pair (capture.no-renderer) and the producers hook (host.unavailable)", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-nohook");
    Arcane::Remote::WaitArgs settled;
    settled.what = "settled";
    settled.timeoutMs = 5000;
    {
        // No capture pair: settled captures (spec s4.7), and captures are
        // refused on a host that cannot take them (spec AM1).
        Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path / "nopair"));
        REQUIRE(session.Start());
        const auto env = Arcane::Remote::WaitPoll(session, settled, 0, 0, true, std::chrono::steady_clock::now());
        CHECK_FALSE(env.ok);
        CHECK(env.errors[0].code == "capture.no-renderer");
        CHECK(Arcane::Remote::ExitFromHost(env) == 2);
        session.Stop("test");
    }
    {
        auto cfg = SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path / "nohook");
        cfg.capture.armHost = &RemoteArmAlways;
        cfg.capture.drainHost = &RemoteDrainLater;
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        const auto env = Arcane::Remote::WaitPoll(session, settled, 0, 0, true, std::chrono::steady_clock::now());
        CHECK_FALSE(env.ok);
        CHECK(env.errors[0].code == "host.unavailable");
        CHECK(Arcane::Remote::ExitFromHost(env) == 5);
        session.Stop("test");
    }
}

TEST_CASE("host.wait refuses a missing, unknown or ill-typed argument", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-args");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    const auto missing = Invoke("host.wait", nlohmann::json::object());
    CHECK(missing.errors[0].code == "arg.missing");
    CHECK(Arcane::Remote::ExitFromHost(missing) == 2);
    CHECK(Invoke("host.wait", {{"what", "frames"}}).errors[0].code == "arg.missing");
    CHECK(Invoke("host.wait", {{"what", "state"}}).errors[0].code == "arg.missing");
    CHECK(Invoke("host.wait", {{"what", "state"}, {"state", "stopping"}}).errors[0].code == "arg.invalid");   // s4.3: shutting-down
    CHECK(Invoke("host.wait", {{"what", "state"}, {"state", "shutting-down"}}).ok);
    CHECK(Invoke("host.wait", {{"what", "soon"}}).errors[0].code == "arg.invalid");
    CHECK(Invoke("host.wait", {{"what", "frames"}, {"frames", -1}}).errors[0].code == "arg.invalid");
    CHECK(Invoke("host.wait", {{"what", "frames"}, {"frames", "2"}}).errors[0].code == "arg.invalid");
    CHECK(Invoke("host.wait", {{"what", "ready"}, {"n", 2}}).errors[0].code == "arg.unknown");   // `n` is not this command
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke([] {
        Arcane::Remote::Call call;
        call.command = "host.wait";
        call.args = {{"what", "ready"}};
        return call;
    }());
    CHECK(armed.envelope.ok);
    CHECK(armed.completesJob != 0);                          // a job the dispatcher binds the waiter to
    session.Stop("test");
}

TEST_CASE("host.quit requests the quit; a dirty host answers host.busy modal unless discarding", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-quit");
    {
        auto cfg = SessionConfig(Arcane::Remote::HostKind::Editor, tmp.path / "dirty");
        cfg.dirty = &RemoteDirtyAlways;
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        const auto modal = Invoke("host.quit", nlohmann::json::object());
        REQUIRE_FALSE(modal.errors.empty());
        CHECK(modal.errors[0].code == "host.busy");
        CHECK(modal.errors[0].detail["state"] == "modal");
        CHECK(Arcane::Remote::ExitFromHost(modal) == 5);
        CHECK_FALSE(session.TakeQuit());
        CHECK(Invoke("host.quit", {{"discardChanges", "yes"}}).errors[0].code == "arg.invalid");
        CHECK(Invoke("host.quit", {{"report", "x.json"}}).errors[0].code == "arg.unknown");
        CHECK_FALSE(session.TakeQuit());
        const auto discarded = Invoke("host.quit", {{"discardChanges", true}});
        CHECK(discarded.ok);
        CHECK(discarded.data["discardedChanges"] == true);
        CHECK(session.TakeQuit());
        CHECK(session.DrainFrame(0));
        CHECK(std::string(session.ExitReason()) == "remote-quit");
        session.Stop(session.ExitReason());
    }
    {
        auto cfg = SessionConfig(Arcane::Remote::HostKind::Editor, tmp.path / "forced");
        cfg.dirty = &RemoteDirtyAlways;
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        // force is the client's (servitor quit --force ends the process after
        // --timeout). The host has no force path, so `force` on the wire is
        // refused, never accepted and ignored (final fix pass, C-M7; Rule 3).
        for (const bool value : {true, false}) {
            const auto forced = Invoke("host.quit", {{"force", value}});
            REQUIRE_FALSE(forced.errors.empty());
            CHECK(forced.errors[0].code == "arg.inapplicable");
            CHECK(Arcane::Remote::ExitFromHost(forced) == 2);
        }
        CHECK_FALSE(session.TakeQuit());
        CHECK_FALSE(Invoke("host.quit", {{"force", true}, {"discardChanges", true}}).ok);
        CHECK_FALSE(session.TakeQuit());
        CHECK(Invoke("host.quit", {{"discardChanges", true}}).ok);
        CHECK(session.TakeQuit());
        session.Stop("test");
    }
    {
        Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Server, tmp.path / "clean"));
        REQUIRE(session.Start());
        const auto clean = Invoke("host.quit", nlohmann::json::object());   // no dirty hook: never modal
        CHECK(clean.ok);
        CHECK(clean.data["discardedChanges"] == false);
        CHECK(session.TakeQuit());
        session.Stop("test");
    }
}

TEST_CASE("session commands answer host.unavailable with no session, and after it stops", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Remote::RegisterSessionCommands(nullptr, nullptr);
    Arcane::Remote::RegisterSessionCommands(nullptr, nullptr);   // twice is safe (ruling F11)
    const auto quit = Invoke("host.quit", nlohmann::json::object());
    CHECK(quit.errors[0].code == "host.unavailable");
    CHECK(Arcane::Remote::ExitFromHost(quit) == 5);
    CHECK(Invoke("host.wait", {{"what", "ready"}}).errors[0].code == "host.unavailable");

    Arcane::Test::TempDir tmp("remote-session-release");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    CHECK(Invoke("host.wait", {{"what", "ready"}}).ok);
    session.Stop("test");                                         // releases the stored pointers
    CHECK(Invoke("host.quit", nlohmann::json::object()).errors[0].code == "host.unavailable");
}

TEST_CASE("host.wait over the listener: the job completes after N frames and answers the parked call", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-e2e");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);
    const std::uint16_t port = session.Port();
    const std::string token = desc->token;
    const std::string body =
        R"({"id":"w1","command":"host.wait","args":{"what":"frames","frames":3},"options":{"timeoutMs":5000}})";
    auto reply = std::async(std::launch::async, [port, token, body] {
        return RemoteTest::HttpExchange(port, token, "", "POST /remote/v1/call HTTP/1.1", body);
    });
    // The host loop: one Drain per iteration, one sim frame per iteration.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::uint64_t iterations = 0;
    while (reply.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        session.DrainFrame(0);
        session.Gate().frame.fetch_add(1);
        ++iterations;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(reply.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const std::string raw = reply.get();
    const auto split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    const auto env = Arcane::Remote::ParseEnvelope(nlohmann::json::parse(raw.substr(split + 4)));
    CHECK(env.ok);
    CHECK(env.id == "w1");
    CHECK(env.command == "host.wait");
    CHECK(env.data["done"] == true);
    CHECK(env.data["what"] == "frames");
    CHECK(env.host.frame >= 3);
    CHECK(Arcane::Remote::ExitFromHost(env) == 0);
    CHECK(iterations >= 3);
    session.Stop("test");
}

// ---- Task 30 (C1): wait settled is spec s4.7's --settle predicate -----------

namespace
{
// A fake render ring on the capture host pair: one "rendered frame" per host
// loop iteration (the test bumps `frame`), a ticket retires one frame after it
// was armed, and the n-th retired capture carries pixels[n] (the last one
// repeats). 1x1 RGBA, so the bytes compared are exactly these four.
struct SettleRing {
    std::uint64_t frame = 0;
    std::uint64_t ticket = 0;
    std::uint64_t armedAt = 0;
    int arms = 0;
    int refusedArms = 0;
    std::size_t drained = 0;
    bool retire = true;
    bool idle = true;
    bool refuseArm = false;              // the ring cannot record a copy yet (a booting host)
    std::vector<std::array<std::uint8_t, 4>> pixels;
    // Non-null: the captured pixel is this host clock (a wall-clock-driven
    // material) instead of `pixels`, so two captures match only if the clock
    // did not move between them.
    const double* clock = nullptr;
    std::array<std::uint8_t, 4> clockPixel{};
    std::uint64_t cancelled = 0;   // the last ticket the cancel hook got (final fix pass, C-M12)
    int cancels = 0;
};

void SettleCancel(std::uint64_t ticket, void* user)
{
    auto* ring = static_cast<SettleRing*>(user);
    ++ring->cancels;
    ring->cancelled = ticket;
}

bool SettleArm(std::uint64_t ticket, void* user)
{
    auto* ring = static_cast<SettleRing*>(user);
    if (ring->refuseArm) {
        ++ring->refusedArms;
        return false;
    }
    ring->ticket = ticket;
    ring->armedAt = ring->frame;
    ++ring->arms;
    return true;
}

bool SettleDrain(std::uint64_t ticket, Arcane::Remote::CaptureMapped* raw, void* user)
{
    auto* ring = static_cast<SettleRing*>(user);
    if (!ring->retire || ticket != ring->ticket || ring->frame < ring->armedAt + 1)
        return false;
    raw->width = 1;
    raw->height = 1;
    raw->rowPitch = 4;
    raw->bgra = false;
    raw->mappedBytes = 4;
    if (ring->clock != nullptr) {
        const auto ms = static_cast<std::uint32_t>(*ring->clock * 1000.0);
        ring->clockPixel = {static_cast<std::uint8_t>(ms & 0xFFu), static_cast<std::uint8_t>((ms >> 8) & 0xFFu),
                            static_cast<std::uint8_t>((ms >> 16) & 0xFFu), 255};
        ++ring->drained;
        raw->mapped = ring->clockPixel.data();
        return true;
    }
    const std::size_t index = std::min(ring->drained, ring->pixels.size() - 1);
    ++ring->drained;
    raw->mapped = ring->pixels[index].data();
    return true;
}

bool SettleIdle(void* user)
{
    return static_cast<SettleRing*>(user)->idle;
}

Arcane::Remote::HostSession::Config SettleConfig(const std::filesystem::path& root, SettleRing& ring)
{
    auto cfg = SessionConfig(Arcane::Remote::HostKind::Runtime, root);
    cfg.hookUser = &ring;
    cfg.producersIdle = &SettleIdle;
    cfg.capture.user = &ring;
    cfg.capture.armHost = cfg.capture.armWindow = cfg.capture.armViewport = &SettleArm;
    cfg.capture.drainHost = cfg.capture.drainWindow = cfg.capture.drainViewport = &SettleDrain;
    cfg.capture.cancelHost = cfg.capture.cancelWindow = cfg.capture.cancelViewport = &SettleCancel;
    cfg.capture.outDir = root / "out";
    return cfg;
}

// POSTs host.wait settled with `timeoutMs` and runs the host loop (one Drain
// and one rendered frame per iteration) until the reply arrives. `during`
// runs once per iteration, after the Drain.
template <class During>
Arcane::Remote::Envelope WaitSettledOverTheWire(Arcane::Remote::HostSession& session, SettleRing& ring,
                                                std::uint32_t timeoutMs, During during)
{
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);
    const std::uint16_t port = session.Port();
    const std::string token = desc->token;
    const std::string body = R"({"id":"s1","command":"host.wait","args":{"what":"settled"},"options":{"timeoutMs":)" +
                             std::to_string(timeoutMs) + "}}";
    auto reply = std::async(std::launch::async, [port, token, body] {
        return RemoteTest::HttpExchange(port, token, "", "POST /remote/v1/call HTTP/1.1", body);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs + 4000);
    while (reply.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        session.DrainFrame(0);
        during();
        ++ring.frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(reply.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const std::string raw = reply.get();
    const auto split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    return Arcane::Remote::ParseEnvelope(nlohmann::json::parse(raw.substr(split + 4)));
}
} // namespace

TEST_CASE("wait settled completes on two byte-equal captures through the host pair with the producers idle", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-settled");
    SettleRing ring;
    ring.pixels = {{10, 20, 30, 255}, {40, 50, 60, 255}, {40, 50, 60, 255}};   // A, B, B
    Arcane::Remote::HostSession session(SettleConfig(tmp.path, ring));
    REQUIRE(session.Start());
    const std::uint64_t frameBefore = session.Gate().frame.load();
    auto env = WaitSettledOverTheWire(session, ring, 5000, [] {});
    CHECK(env.ok);
    CHECK(env.command == "host.wait");
    CHECK(env.data["done"] == true);
    CHECK(env.data["what"] == "settled");
    CHECK(env.data["captures"] == 3);   // A differs from B; the second B equals the first
    CHECK(env.data["width"] == 1);
    CHECK(env.data["height"] == 1);
    CHECK(Arcane::Remote::ExitFromHost(env) == 0);
    CHECK(ring.arms == 3);              // one ticket per capture, armed one at a time
    CHECK(session.Gate().frame.load() == frameBefore);   // render tickets, never host.frame
    session.Stop("test");
}

TEST_CASE("wait settled never completes on captures that differ, or while a producer is busy", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-unsettled");
    {
        SettleRing ring;
        for (int i = 0; i < 400; ++i)   // A, B, A, B, ...: an animated frame under an unfrozen clock
            ring.pixels.push_back(i % 2 == 0 ? std::array<std::uint8_t, 4>{1, 2, 3, 255}
                                             : std::array<std::uint8_t, 4>{4, 5, 6, 255});
        Arcane::Remote::HostSession session(SettleConfig(tmp.path / "differ", ring));
        REQUIRE(session.Start());
        auto env = WaitSettledOverTheWire(session, ring, 300, [] {});
        CHECK(env.ok);
        CHECK(env.errors.empty());
        CHECK(env.data["timedOut"] == true);
        CHECK(Arcane::Remote::ExitFromHost(env) == 8);
        CHECK(ring.arms >= 2);
        session.Stop("test");
    }
    {
        // Identical frames, but the shader compiler (the producers hook) is
        // never idle: nothing is armed, so nothing can settle.
        SettleRing ring;
        ring.idle = false;
        ring.pixels = {{7, 7, 7, 255}};
        Arcane::Remote::HostSession session(SettleConfig(tmp.path / "busy", ring));
        REQUIRE(session.Start());
        auto env = WaitSettledOverTheWire(session, ring, 200, [] {});
        CHECK(env.data["timedOut"] == true);
        CHECK(Arcane::Remote::ExitFromHost(env) == 8);
        CHECK(ring.arms == 0);
        session.Stop("test");
    }
    {
        // Identical frames, but a producer turns busy while the second capture
        // is in flight: that capture retires with it busy, so it pairs with
        // neither neighbour, and the wait needs two more (4, not 2).
        SettleRing ring;
        ring.pixels = {{9, 9, 9, 255}};
        Arcane::Remote::HostSession session(SettleConfig(tmp.path / "flicker", ring));
        REQUIRE(session.Start());
        auto env = WaitSettledOverTheWire(session, ring, 5000, [&] {
            ring.idle = !(ring.arms == 2 && ring.drained == 1);
        });
        CHECK(env.ok);
        CHECK(env.data["done"] == true);
        CHECK(env.data["captures"] == 4);
        session.Stop("test");
    }
}

TEST_CASE("a settle capture holds the one capture slot: a screenshot meanwhile is host.busy capture-pending", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-settle-slot");
    SettleRing ring;
    ring.retire = false;                 // the settle ticket stays in flight
    ring.pixels = {{1, 1, 1, 255}};
    Arcane::Remote::HostSession session(SettleConfig(tmp.path, ring));
    REQUIRE(session.Start());
    Arcane::Remote::CommandResult busy;
    Arcane::Remote::CommandResult freed;
    bool askedWhileHeld = false;
    auto env = WaitSettledOverTheWire(session, ring, 300, [&] {
        if (ring.arms == 1 && !askedWhileHeld) {
            Arcane::Remote::Call shot;
            shot.command = "capture.window";
            shot.args = nlohmann::json::object();
            busy = Arcane::Remote::CommandRegistry::Get().Invoke(shot);
            askedWhileHeld = true;
        }
    });
    REQUIRE(askedWhileHeld);
    REQUIRE_FALSE(busy.envelope.errors.empty());
    CHECK(busy.envelope.errors[0].code == "host.busy");
    CHECK(busy.envelope.errors[0].detail["state"] == "capture-pending");
    CHECK(Arcane::Remote::ExitFromHost(busy.envelope) == 5);
    CHECK(busy.completesJob == 0);
    CHECK(env.data["timedOut"] == true);   // the ticket never retired
    // The wait ended, and its hold went with it: the screenshot now arms.
    Arcane::Remote::Call shot;
    shot.command = "capture.window";
    shot.args = nlohmann::json::object();
    freed = Arcane::Remote::CommandRegistry::Get().Invoke(shot);
    CHECK(freed.envelope.ok);
    CHECK(freed.completesJob != 0);
    session.Stop("test");
}

// ---- Task 30 (C2, fix round 1 I1): the listener bounds a request's depth ----

namespace
{
// POSTs `body` to a started session's listener with no Drain at all: a body
// the listener refuses is answered at once, on its own thread.
Arcane::Remote::Envelope PostWithoutDrain(Arcane::Remote::HostSession& session, const std::string& body,
                                          std::string* statusLine)
{
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);
    const std::string raw =
        RemoteTest::HttpExchange(session.Port(), desc->token, "", "POST /remote/v1/call HTTP/1.1", body);
    const auto split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    *statusLine = raw.substr(0, raw.find("\r\n"));
    return Arcane::Remote::ParseEnvelope(nlohmann::json::parse(raw.substr(split + 4)));
}

std::string Nest(std::uint32_t levels)
{
    return std::string(levels, '[') + std::string(levels, ']');
}
} // namespace

TEST_CASE("the listener accepts a body at exactly kMaxRequestDepth through its protocol dump, and refuses one more", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-deep-body");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    const std::uint32_t cap = Arcane::Remote::kMaxRequestDepth;
    std::string status;
    {
        // At the cap: the body object (level 1) holds a non-integer protocol
        // nested cap - 1 deep. It parses, and the listener's refusal Dumps it
        // into the message on the listener thread: the recursion at the cap.
        const std::string body = R"({"protocol":)" + Nest(cap - 1) + R"(,"command":"host.wait"})";
        auto env = PostWithoutDrain(session, body, &status);
        CHECK(status.rfind("HTTP/1.1 400", 0) == 0);
        REQUIRE_FALSE(env.errors.empty());
        CHECK(env.errors[0].code == "protocol.mismatch");
        CHECK(env.errors[0].message.find(Nest(cap - 1)) != std::string::npos);   // the whole value was dumped
        CHECK(Arcane::Remote::ExitFromHost(env) == 4);
    }
    {
        // One level more: refused as too deep before anything copies or dumps it.
        const std::string body = R"({"protocol":)" + Nest(cap) + R"(,"command":"host.wait"})";
        auto env = PostWithoutDrain(session, body, &status);
        CHECK(status.rfind("HTTP/1.1 400", 0) == 0);
        REQUIRE_FALSE(env.errors.empty());
        CHECK(env.errors[0].code == "arg.invalid");
        CHECK(env.errors[0].message == "the call body nests deeper than " + std::to_string(cap) + " levels");
        CHECK(env.errors[0].detail["reason"] == "too-deep");
        CHECK(env.errors[0].detail["maxDepth"] == cap);
        CHECK(Arcane::Remote::ExitFromHost(env) == 2);
    }
    {
        // Not JSON at all keeps the old malformed-body answer, without the depth detail.
        auto env = PostWithoutDrain(session, "{not json", &status);
        REQUIRE_FALSE(env.errors.empty());
        CHECK(env.errors[0].code == "arg.invalid");
        CHECK(env.errors[0].message == "the call body is not a JSON object");
        CHECK_FALSE(env.errors[0].detail.contains("reason"));
    }
    session.Stop("test");
}

// ---- Task 30 fix round 1 (I2): the settle wait freezes the host clock --------

TEST_CASE("wait settled freezes the host clock for its captures, so a wall-clock value settles", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-settle-freeze");
    double clock = 0.0;                  // a held or windowed host's wall clock: +16 ms per frame unless frozen
    SettleRing ring;
    ring.clock = &clock;
    Arcane::Remote::HostSession session(SettleConfig(tmp.path, ring));
    REQUIRE(session.Start());
    CHECK_FALSE(session.SettleClockFrozen());
    bool sawFrozen = false;
    // The host loop: what AdvanceSim's settleHold does with the freeze (dt 0).
    auto env = WaitSettledOverTheWire(session, ring, 5000, [&] {
        if (session.SettleClockFrozen())
            sawFrozen = true;
        else
            clock += 0.016;
    });
    CHECK(env.ok);
    CHECK(env.data["done"] == true);
    CHECK(env.data["captures"] == 2);    // both captures under the one frozen clock
    CHECK(sawFrozen);
    // Restored: the wait is over, so the clock runs again.
    CHECK_FALSE(session.SettleClockFrozen());
    const double before = clock;
    session.DrainFrame(0);
    if (!session.SettleClockFrozen())
        clock += 0.016;
    CHECK(clock > before);
    session.Stop("test");
}

TEST_CASE("the settle freeze lifts when the wait times out, while a producer is busy, and on teardown", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-settle-restore");
    {
        // Timeout: captures that never match. Frozen during, restored after.
        SettleRing ring;
        for (int i = 0; i < 400; ++i)
            ring.pixels.push_back(i % 2 == 0 ? std::array<std::uint8_t, 4>{1, 2, 3, 255}
                                             : std::array<std::uint8_t, 4>{4, 5, 6, 255});
        Arcane::Remote::HostSession session(SettleConfig(tmp.path / "timeout", ring));
        REQUIRE(session.Start());
        bool sawFrozen = false;
        auto env = WaitSettledOverTheWire(session, ring, 300, [&] { sawFrozen = sawFrozen || session.SettleClockFrozen(); });
        CHECK(env.data["timedOut"] == true);
        CHECK(env.data["captures"].get<std::uint64_t>() >= 2);
        CHECK_FALSE(env.data.contains("lastRefusal"));
        CHECK(sawFrozen);
        CHECK_FALSE(session.SettleClockFrozen());
        session.Stop("test");
    }
    {
        // A busy producer: no capture is armed, so the clock is never frozen
        // (the freeze would also stop the compile service's debounce clock).
        SettleRing ring;
        ring.idle = false;
        ring.pixels = {{7, 7, 7, 255}};
        Arcane::Remote::HostSession session(SettleConfig(tmp.path / "busy", ring));
        REQUIRE(session.Start());
        bool sawFrozen = false;
        auto env = WaitSettledOverTheWire(session, ring, 200, [&] { sawFrozen = sawFrozen || session.SettleClockFrozen(); });
        CHECK(env.data["timedOut"] == true);
        CHECK_FALSE(sawFrozen);
        CHECK_FALSE(session.SettleClockFrozen());
        session.Stop("test");
    }
    {
        // Teardown while a capture is in flight: the stop answers the parked
        // call and the freeze goes with the session.
        SettleRing ring;
        ring.retire = false;
        ring.pixels = {{1, 1, 1, 255}};
        Arcane::Remote::HostSession session(SettleConfig(tmp.path / "teardown", ring));
        REQUIRE(session.Start());
        const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        const std::uint16_t port = session.Port();
        const std::string token = desc->token;
        const std::string body =
            R"({"id":"t1","command":"host.wait","args":{"what":"settled"},"options":{"timeoutMs":5000}})";
        auto reply = std::async(std::launch::async, [port, token, body] {
            return RemoteTest::HttpExchange(port, token, "", "POST /remote/v1/call HTTP/1.1", body);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!session.SettleClockFrozen() && std::chrono::steady_clock::now() < deadline) {
            session.DrainFrame(0);
            ++ring.frame;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(session.SettleClockFrozen());
        CHECK(ring.arms == 1);
        session.Stop("test");
        CHECK_FALSE(session.SettleClockFrozen());
        REQUIRE(reply.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const std::string raw = reply.get();
        const auto split = raw.find("\r\n\r\n");
        REQUIRE(split != std::string::npos);
        const auto env = Arcane::Remote::ParseEnvelope(nlohmann::json::parse(raw.substr(split + 4)));
        REQUIRE_FALSE(env.errors.empty());
        CHECK(env.errors[0].code == "host.died");
    }
}

// Fix round 1 (M3): a host whose ring keeps refusing the arm is not a silent
// timeout: the timeout envelope names the last refusal.
TEST_CASE("a settle wait whose arm is always refused times out naming the last refusal", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-settle-refused");
    SettleRing ring;
    ring.refuseArm = true;
    ring.pixels = {{3, 3, 3, 255}};
    Arcane::Remote::HostSession session(SettleConfig(tmp.path, ring));
    REQUIRE(session.Start());
    auto env = WaitSettledOverTheWire(session, ring, 200, [] {});
    CHECK(env.ok);
    CHECK(env.errors.empty());
    CHECK(env.data["timedOut"] == true);
    CHECK(env.data["captures"] == 0);
    REQUIRE(env.data.contains("lastRefusal"));
    CHECK(env.data["lastRefusal"]["code"] == "capture.no-frame");
    CHECK(Arcane::Remote::ExitFromHost(env) == 8);
    CHECK(ring.refusedArms >= 2);           // retried within the timeout
    CHECK_FALSE(session.SettleClockFrozen());
    session.Stop("test");
}

// ---- Verification N2: the freeze follows the wait JOB, not only its row ------

// The freeze is read from the session's wait rows. A row ends with its job
// today (StepWait), but a later cancel path (cancel on client disconnect)
// could end the job without the wait's own step running. A test-owned table
// stands in for that path: it ends the job directly, and the clock must run.
TEST_CASE("the settle freeze ends when its wait job stops running, even if its row outlives it", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-settle-cancelled");
    SettleRing ring;
    ring.retire = false;                 // the capture stays in flight, so the freeze holds
    ring.pixels = {{1, 1, 1, 255}};
    Arcane::Remote::HostSession session(SettleConfig(tmp.path, ring));
    REQUIRE(session.Start());
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RegisterSessionCommands(&session, &jobs);
    Arcane::Remote::Call call;
    call.command = "host.wait";
    call.args = {{"what", "settled"}};
    call.timeoutMs = 5000;
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    REQUIRE(armed.envelope.ok);
    REQUIRE(armed.completesJob != 0);
    const Arcane::Remote::JobId id{armed.completesJob};
    for (int i = 0; i < 50 && !session.SettleClockFrozen(); ++i) {
        jobs.StepAll(session.Gate());
        ++ring.frame;
    }
    REQUIRE(session.SettleClockFrozen());
    CHECK(ring.arms == 1);
    jobs.Finish(id, Arcane::Remote::MakeError("host.wait", "job.cancelled", "cancelled by the test", "arm the wait again"),
                Arcane::Remote::JobState::Cancelled);
    REQUIRE(jobs.Find(id) != nullptr);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Cancelled);
    CHECK_FALSE(session.SettleClockFrozen());
    session.Stop("test");               // releases the row and its ring hold
}

// ---- Verification N3: a host-side wait's timeout is bounded ----------------

TEST_CASE("a host-side wait is bounded at kMaxWaitTimeoutMs, and a clamped wait says so", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-timeout-cap");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    session.Gate().SetState("ready");
    const std::uint32_t cap = Arcane::Remote::kMaxWaitTimeoutMs;
    CHECK(cap >= 30000u);               // servitor's default --timeout (30 s) is never clamped
    const auto now = std::chrono::steady_clock::now();
    {
        // WaitPoll: a wait asking for the u32 maximum (what the listener and
        // servitor's --timeout both admit) has timed out once the cap passed.
        Arcane::Remote::WaitArgs args;
        args.what = "state";
        args.state = "playing";         // never reached here
        args.timeoutMs = std::numeric_limits<std::uint32_t>::max();
        const auto past = now - std::chrono::milliseconds(cap) - std::chrono::milliseconds(50);
        auto late = Arcane::Remote::WaitPoll(session, args, 0, 0, true, past);
        CHECK(late.ok);
        CHECK(late.data["timedOut"] == true);
        CHECK(Arcane::Remote::ExitFromHost(late) == 8);
        // Under the cap it is still pending.
        const auto recent = now - std::chrono::milliseconds(cap) + std::chrono::milliseconds(60000);
        auto pending = Arcane::Remote::WaitPoll(session, args, 0, 0, true, recent);
        CHECK_FALSE(pending.data.value("done", true));
        CHECK_FALSE(pending.data.contains("timedOut"));
    }
    // The job's final envelope warns when it clamped, and only then.
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RegisterSessionCommands(&session, &jobs);
    const auto runWait = [&](std::uint32_t timeoutMs) {
        Arcane::Remote::Call call;
        call.command = "host.wait";
        call.args = {{"what", "state"}, {"state", "ready"}};
        call.timeoutMs = timeoutMs;
        const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(call);
        REQUIRE(armed.completesJob != 0);
        jobs.StepAll(session.Gate());
        const Arcane::Remote::Job* job = jobs.Find(Arcane::Remote::JobId{armed.completesJob});
        REQUIRE(job != nullptr);
        REQUIRE(job->state == Arcane::Remote::JobState::Done);
        return job->envelope;
    };
    {
        const auto env = runWait(std::numeric_limits<std::uint32_t>::max());
        CHECK(env.ok);
        CHECK(env.data["done"] == true);
        REQUIRE(env.warnings.size() == 1);
        CHECK(env.warnings[0].code == "wait.timeout-clamped");
        CHECK(env.warnings[0].detail["requestedMs"] == std::numeric_limits<std::uint32_t>::max());
        CHECK(env.warnings[0].detail["maxMs"] == cap);
        CHECK(Arcane::Remote::ExitFromHost(env) == 0);
    }
    {
        const auto env = runWait(cap);  // exactly the cap: not clamped
        CHECK(env.ok);
        CHECK(env.warnings.empty());
    }
    session.Stop("test");
}

// Final fix pass (C-M12, G-M5): a settle wait that ends while its capture is
// armed (here: the ring never retires it, and the wait times out) clears that
// ticket from the ring, so a later screenshot never waits behind it.
TEST_CASE("a settle wait that ends with a capture armed cancels that ticket", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-settle-cancel");
    SettleRing ring;
    ring.retire = false;
    ring.pixels = {{1, 1, 1, 255}};
    Arcane::Remote::HostSession session(SettleConfig(tmp.path, ring));
    REQUIRE(session.Start());
    const auto env = WaitSettledOverTheWire(session, ring, 200, [] {});
    CHECK(env.data["timedOut"] == true);
    REQUIRE(ring.arms == 1);
    CHECK(ring.cancels == 1);
    CHECK(ring.cancelled == ring.ticket);
    session.Stop("test");
}

#endif
