#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Core/AbiVersion.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Platform/Process.hpp>
#include "ServerConfig.hpp"
#include "../Helpers/ConstantScan.hpp"   // Arcane::Test::RepoRoot (the source pins)
#include <sstream>
#include <regex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include "RemoteTestSupport.hpp"
#include "RemoteTestHttp.hpp"   // HttpExchange: the /status a client reads (final fix pass)
#include <Json.hpp>
#include <App/ModalErrorQueue.hpp>   // ReportsModal: the editor modal hook (round 2, I-2; source-compiled editor header)
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir, EnvOverride

#if !defined(ARC_BUILD_DIST)

namespace
{
Arcane::Remote::HostSession::Config ServerConfig(const std::filesystem::path& root)
{
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Server;
    cfg.cvarContext = Arcane::CVarContext::ServerAdmin;
    cfg.projectRoot = root;
    cfg.capabilities = {"console", "cvar", "ecs", "logs", "play", "problems", "stats"};
    return cfg;
}
} // namespace

TEST_CASE("a client-mode process does not arm", "[remote][host]") {
    CHECK_FALSE(Arcane::Remote::Arming::Allow(Arcane::NetMode::Client, false, true));
    CHECK(Arcane::Remote::Arming::Allow(Arcane::NetMode::DedicatedServer, false, true));
    CHECK(Arcane::Remote::Arming::Allow(Arcane::NetMode::Standalone, false, true));
    CHECK_FALSE(Arcane::Remote::Arming::Allow(Arcane::NetMode::Standalone, true, true));
    CHECK_FALSE(Arcane::Remote::Arming::Allow(Arcane::NetMode::Standalone, false, false));
    // Final fix pass (C-M16): an explicit --remote on a client-mode process is
    // refused with a reason (the host exits 2), never "ignored".
    const char* why = Arcane::Remote::Arming::RefusalReason(Arcane::NetMode::Client);
    REQUIRE(why != nullptr);
    CHECK(std::string(why).find("Client") != std::string::npos);
    CHECK(std::string(why).find("ignored") == std::string::npos);
    for (const auto mode : {Arcane::NetMode::Standalone, Arcane::NetMode::DedicatedServer, Arcane::NetMode::ListenServer})
        CHECK(Arcane::Remote::Arming::RefusalReason(mode) == nullptr);
}

TEST_CASE("HostSession writes a live descriptor and removes it on Stop", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor); // restores the suite's kind even if a REQUIRE aborts (ruling S3-9)
    auto root = std::filesystem::temp_directory_path() / "remote-session-test";
    std::filesystem::remove_all(root);
    {
        Arcane::Remote::HostSession::Config cfg;
        cfg.kind = Arcane::Remote::HostKind::Server;
        cfg.cvarContext = Arcane::CVarContext::ServerAdmin;
        cfg.projectRoot = root;
        cfg.capabilities = {"console", "cvar", "ecs", "logs", "play", "problems", "stats"};
        Arcane::Remote::HostSession session(cfg);
        const auto kindBefore = Arcane::Remote::CommandRegistry::Get().GetHostKind();
        REQUIRE(session.Start());
        CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == Arcane::Remote::HostKind::Server);
        CHECK(std::filesystem::exists(session.DescriptorFile()));
        auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        CHECK(desc->port == session.Port());
        CHECK(desc->abi == Arcane::kGamePluginABIVersion);   // ruling F27
        CHECK_FALSE(Arcane::Remote::IsStale(*desc));
        session.NoteReload(desc->abi + 7u, true, "reloaded", 12);   // any value: the host supplies it (ruling F10)
        CHECK(static_cast<const Arcane::Remote::HostSession&>(session).Gate().pluginGeneration == desc->abi + 7u);
        session.Stop("test");
        CHECK_FALSE(std::filesystem::exists(session.DescriptorFile()));
        CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == kindBefore);   // ruling S3-9

        // A session object starts at most once: a re-arm is a new object.
        CHECK_FALSE(session.Start());
        CHECK(session.Port() == 0);
        CHECK_FALSE(std::filesystem::exists(session.DescriptorFile()));
        CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == kindBefore);
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    CHECK_FALSE(std::filesystem::exists(root));
}

TEST_CASE("HostSession: an unwritable descriptor is a failed start that leaves nothing behind", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-fail");
    // A regular file where the descriptor's parent directory must be: the
    // directory cannot be created, so WriteDescriptorAtomic fails on every platform.
    const auto blocker = tmp.path / "blocker";
    {
        std::ofstream out(blocker);
        out << "not a directory";
    }
    auto cfg = ServerConfig(tmp.path / "project");
    cfg.descriptorOverride = blocker / "server.json";
    Arcane::Remote::HostSession session(cfg);
    const auto kindBefore = Arcane::Remote::CommandRegistry::Get().GetHostKind();
    CHECK_FALSE(session.Start());
    CHECK(session.Port() == 0);
    CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == kindBefore);
    CHECK_FALSE(std::filesystem::exists(session.DescriptorFile()));
    CHECK(std::filesystem::is_regular_file(blocker));      // the blocker was not replaced
    CHECK_FALSE(session.DrainFrame(1));                     // a failed session never tells the host to stop
    CHECK(session.ExitReason() == nullptr);
    CHECK_FALSE(session.Start());                           // nor does it start on a retry
    CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == kindBefore);
}

TEST_CASE("HostSession: ARC_REMOTE_READ_ONLY=1 makes the host read-only", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-ro");
    {
        Arcane::Test::EnvOverride env("ARC_REMOTE_READ_ONLY", "1");   // restored on scope exit
        Arcane::Remote::HostSession session(ServerConfig(tmp.path / "ro"));
        REQUIRE(session.Start());
        CHECK(session.Gate().readOnly.load());
        session.Stop("test");
    }
    {
        Arcane::Test::EnvOverride env("ARC_REMOTE_READ_ONLY", "0");   // only "1" counts
        Arcane::Remote::HostSession session(ServerConfig(tmp.path / "rw"));
        REQUIRE(session.Start());
        CHECK_FALSE(session.Gate().readOnly.load());
        session.Stop("test");
    }
}

TEST_CASE("HostSession: DrainFrame adopts the boot generation, notes a reload, and latches a quit", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-quit");
    Arcane::Remote::HostSession session(ServerConfig(tmp.path));
    REQUIRE(session.Start());
    CHECK_FALSE(session.DrainFrame(3));                 // the module the host booted with, not a reload
    CHECK(session.Gate().pluginGeneration == 3u);
    CHECK_FALSE(session.DrainFrame(4));                 // a hot reload
    CHECK(session.Gate().pluginGeneration == 4u);
    CHECK(session.ExitReason() == nullptr);
    session.RequestQuit();
    CHECK(session.DrainFrame(4));
    REQUIRE(session.ExitReason() != nullptr);
    CHECK(std::string(session.ExitReason()) == "remote-quit");

    session.MarkStopping();                             // the host leaves its loop
    CHECK(session.Gate().State() == "shutting-down");
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);
    CHECK(desc->state == "shutting-down");
    session.Stop(session.ExitReason());
    CHECK_FALSE(std::filesystem::exists(session.DescriptorFile()));
}

TEST_CASE("HostSession: a short idle exit latches remote-idle", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-idle");
    auto cfg = ServerConfig(tmp.path);
    cfg.idleExitSeconds = 1;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK_FALSE(session.IdleExpired());
    CHECK_FALSE(session.DrainFrame(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(session.IdleExpired());
    CHECK(session.DrainFrame(1));
    REQUIRE(session.ExitReason() != nullptr);
    CHECK(std::string(session.ExitReason()) == "remote-idle");
    session.RequestQuit();                              // the first reason stays: it is what ended the loop
    CHECK(session.DrainFrame(1));
    CHECK(std::string(session.ExitReason()) == "remote-idle");
    session.Stop(session.ExitReason());
}
// ---- Final fix pass ----------------------------------------------------------

namespace
{
// The /status snapshot the listener serves, read over the wire.
nlohmann::json StatusOver(const Arcane::Remote::HostSession& session)
{
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);
    const std::string raw = RemoteTest::HttpExchange(session.Port(), desc->token, "", "GET /remote/v1/status HTTP/1.1");
    const auto body = raw.find("\r\n\r\n");
    REQUIRE(body != std::string::npos);
    return nlohmann::json::parse(raw.substr(body + 4));
}

struct EditorFacts {
    bool playing = false;
    bool paused = false;
    bool modal = false;
    bool building = false;
};
bool FactPlaying(void* user) { return static_cast<EditorFacts*>(user)->playing; }
bool FactPaused(void* user) { return static_cast<EditorFacts*>(user)->paused; }
bool FactModal(void* user) { return static_cast<EditorFacts*>(user)->modal; }
bool FactBuilding(void* user) { return static_cast<EditorFacts*>(user)->building; }
} // namespace

// C-I3: a runtime or server publishes its AM1 tick gate as the host state:
// held (at start, or stopped) is ready, running is playing, a paused run is
// paused. /status and every envelope's host block read it, so `wait state`
// completes and the PlayPolicy gate sees Play.
TEST_CASE("HostSession: a fixed-tick host publishes ready, playing and paused from its tick gate", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-state-tick");
    auto cfg = ServerConfig(tmp.path);
    cfg.holdSim = true;   // an open-ended --remote run: held at frame 0
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "ready");
    CHECK_FALSE(session.Gate().inPlay.load());
    session.Tick().SetRunning(true);   // servitor play
    session.Tick().SetPaused(false);
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "playing");
    CHECK(session.Gate().inPlay.load());
    CHECK(StatusOver(session)["state"] == "playing");
    session.Tick().SetPaused(true);    // servitor pause
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "paused");
    CHECK(session.Gate().Block().state == "paused");
    session.NoteMinimized(true);       // the play axis outranks minimized (the gate flag still refuses captures)
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "paused");
    session.Tick().SetRunning(false);  // servitor stop
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "minimized");
    CHECK_FALSE(session.Gate().inPlay.load());
    session.NoteMinimized(false);
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "ready");
    session.MarkStopping();
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "shutting-down");   // latched: a later Drain never takes it back
    session.Stop("test");
}

// C-I3: the editor's facts. Precedence: shutting-down, reloading, building,
// modal, paused, playing, minimized, ready. A reload is synchronous on the
// main thread, so the host publishes it the moment it begins and ends
// (NoteReloading, from PluginHost's reload observer): /status and the
// descriptor read "reloading" while the main thread is inside the reload.
TEST_CASE("HostSession: an editor publishes playing, paused, modal, building and reloading", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-state-editor");
    EditorFacts facts;
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Editor;
    cfg.cvarContext = Arcane::CVarContext::Editor;
    cfg.projectRoot = tmp.path;
    cfg.play.editor = true;
    cfg.play.editorUser = &facts;
    cfg.play.editorPlaying = &FactPlaying;
    cfg.play.editorPaused = &FactPaused;
    cfg.hookUser = &facts;
    cfg.modal = &FactModal;
    cfg.building = &FactBuilding;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "ready");
    facts.playing = true;
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "playing");
    facts.paused = true;
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "paused");
    CHECK(session.Gate().inPlay.load());
    facts.modal = true;
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "modal");
    CHECK(session.Gate().inPlay.load());   // a modal in Play is still Play for the PlayPolicy gate
    facts.building = true;
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "building");
    session.NoteReloading(true);           // no Drain: published at once
    CHECK(session.Gate().State() == "reloading");
    CHECK(StatusOver(session)["state"] == "reloading");
    const auto during = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(during);
    CHECK(during->state == "reloading");
    session.NoteReloading(false);
    CHECK(session.Gate().State() == "building");
    facts = EditorFacts{};
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "ready");
    CHECK_FALSE(session.Gate().inPlay.load());
    session.Stop("test");
}

// C-I3: `wait state playing` completes once the host reaches it (it could
// never complete while every host published ready).
TEST_CASE("HostSession: wait state playing completes after play", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-state-wait");
    auto cfg = ServerConfig(tmp.path);
    cfg.holdSim = true;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK_FALSE(session.DrainFrame(1));
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);
    std::string answered;
    std::atomic<bool> replied{false};
    std::thread worker([&] {
        answered = RemoteTest::HttpExchange(session.Port(), desc->token, "", "POST /remote/v1/call HTTP/1.1",
                                            R"({"command":"host.wait","args":{"what":"state","state":"playing"},)"
                                            R"("options":{"timeoutMs":20000}})");
        replied.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool played = false;
    bool stopped = false;   // asserted after the join: Catch2 assertions stay on one thread at a time
    int drains = 0;
    while (!replied.load() && std::chrono::steady_clock::now() < deadline) {
        stopped = session.DrainFrame(1) || stopped;
        if (++drains == 20 && !played) {
            session.Tick().SetRunning(true);   // servitor play, a little later
            session.Tick().SetPaused(false);
            played = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    worker.join();
    CHECK_FALSE(stopped);
    CHECK(played);
    CHECK(answered.find("\"done\":true") != std::string::npos);
    CHECK(answered.find("\"state\":\"playing\"") != std::string::npos);
    session.Stop("test");
}

// C-M3: a deleted descriptor (Saved/Remote removed under a live host) is
// written again on the next heartbeat, through the same owner-only path.
TEST_CASE("HostSession: the heartbeat rewrites a deleted descriptor", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-rewrite");
    Arcane::Remote::HostSession session(ServerConfig(tmp.path));
    REQUIRE(session.Start());
    const auto before = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(before);
    std::error_code ec;
    std::filesystem::remove_all(session.DescriptorFile().parent_path(), ec);
    REQUIRE_FALSE(std::filesystem::exists(session.DescriptorFile()));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // past the 1 s heartbeat interval
    CHECK_FALSE(session.DrainFrame(1));
    const auto after = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(after);
    CHECK(after->token == before->token);
    CHECK(after->port == before->port);
    CHECK(after->heartbeatUnixMs >= before->heartbeatUnixMs);
    session.Stop("test");
    CHECK_FALSE(std::filesystem::exists(session.DescriptorFile()));
}

// C-M4: GET /status and GET /catalog are requests (spec s4.7: the idle exit
// counts the time since a request arrived), so a client that only polls them
// keeps a headless host alive.
TEST_CASE("HostSession: a GET /status counts as activity for the idle exit", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-idle-status");
    auto cfg = ServerConfig(tmp.path);
    cfg.idleExitSeconds = 1;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    (void)StatusOver(session);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK_FALSE(session.DrainFrame(1));   // 1.2 s since Start, 0.5 s since the status request
    CHECK(session.ExitReason() == nullptr);
    session.Stop("test");
}
// ---- Final fix round 2 ----------------------------------------------------

namespace
{
struct HeadlessEditor {
    Arcane::Editor::ModalErrorQueue dialogs;
    EditorFacts facts;
};
bool HeadlessModal(void* user)
{
    // The editor's modal hook on a headless host: no interactive window.
    return Arcane::Editor::ReportsModal(static_cast<HeadlessEditor*>(user)->dialogs, false, false);
}
bool HeadlessPlaying(void* user) { return static_cast<HeadlessEditor*>(user)->facts.playing; }
bool HeadlessPaused(void* user) { return static_cast<HeadlessEditor*>(user)->facts.paused; }
} // namespace

// I-2: a headless editor's queued error dialog (a module-load failure, a
// --project that did not open) has no one to answer it. It never makes the
// host read modal, so launch and wait ready still see a serving host.
TEST_CASE("a headless editor with a queued error dialog reads its real state, never modal", "[remote][host][editor]") {
    Arcane::Editor::ModalErrorQueue empty;
    Arcane::Editor::ModalErrorQueue queued;
    queued.Push("Game Module Failed", "ABI mismatch");
    CHECK(Arcane::Editor::ReportsModal(queued, true, false));    // a desk editor: someone can answer it
    CHECK(Arcane::Editor::ReportsModal(empty, true, true));
    CHECK_FALSE(Arcane::Editor::ReportsModal(empty, true, false));
    CHECK_FALSE(Arcane::Editor::ReportsModal(queued, false, false));   // headless: unattended
    CHECK_FALSE(Arcane::Editor::ReportsModal(queued, false, true));

    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-headless-modal");
    HeadlessEditor editor;
    editor.dialogs.Push("Open Project Failed", "--project 'x' could not be opened");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Editor;
    cfg.cvarContext = Arcane::CVarContext::Editor;
    cfg.projectRoot = tmp.path;
    cfg.play.editor = true;
    cfg.play.editorUser = &editor;
    cfg.play.editorPlaying = &HeadlessPlaying;
    cfg.play.editorPaused = &HeadlessPaused;
    cfg.hookUser = &editor;
    cfg.modal = &HeadlessModal;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "ready");
    editor.facts.playing = true;
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "playing");
    editor.facts.paused = true;
    CHECK_FALSE(session.DrainFrame(1));
    CHECK(session.Gate().State() == "paused");
    session.Stop("test");
}

// M-2: the editor's --remote refusal ends the host only on its startup arm;
// a later re-arm that cannot apply (an editor.remote.enable toggle during
// ClientOnly Play, a project switch) logs and stays disarmed.
TEST_CASE("an explicit --remote refusal exits only at startup", "[remote][host]") {
    CHECK(Arcane::Remote::Arming::RefusalExits(true, true));
    CHECK_FALSE(Arcane::Remote::Arming::RefusalExits(true, false));   // later: log, stay disarmed
    CHECK_FALSE(Arcane::Remote::Arming::RefusalExits(false, true));   // the cvar alone never exits
    CHECK_FALSE(Arcane::Remote::Arming::RefusalExits(false, false));
}

// ---- Final fix round 3 ----------------------------------------------------

// M-2: the editor's arm-attempt sequencing (EditorApp::UpdateRemoteSession
// calls Arming::Attempt past its early-return guard). The first attempt is the
// startup arm whether it arms or is refused; every later one -- a cvar toggle
// during ClientOnly or client+server Play, a project switch -- only logs.
TEST_CASE("the editor's arm attempts: only the first is the startup arm, armed or refused", "[remote][host]") {
    using Arcane::Remote::Arming;
    using Verdict = Arcane::Remote::Arming::Verdict;
    bool tried = false;
    CHECK(Arming::Attempt(tried, Arcane::NetMode::Standalone, true) == Verdict::Arm);   // `servitor launch editor`
    CHECK(tried);
    // The reviewer's scenario: the user enters client+server Play (a Client
    // primary) and cycles editor.remote.enable: never an exit.
    CHECK(Arming::Attempt(tried, Arcane::NetMode::Client, true) == Verdict::RefuseLog);
    CHECK(Arming::Attempt(tried, Arcane::NetMode::Standalone, true) == Verdict::Arm);

    bool refusedAtBoot = false;
    CHECK(Arming::Attempt(refusedAtBoot, Arcane::NetMode::Client, true) == Verdict::RefuseExit);
    CHECK(Arming::Attempt(refusedAtBoot, Arcane::NetMode::Client, true) == Verdict::RefuseLog);

    bool cvarOnly = false;   // armed by the cvar alone: no explicit --remote to refuse
    CHECK(Arming::Attempt(cvarOnly, Arcane::NetMode::Client, false) == Verdict::RefuseLog);
}

TEST_CASE("HostSession records its client in the descriptor at Start and on every rewrite", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    for (const std::string client : {std::string{}, std::string{"grok-review-r1"}}) {
        INFO("client '" << client << "'");
        Arcane::Test::TempDir tmp("remote-session-client");
        auto cfg = ServerConfig(tmp.path);
        cfg.client = client;
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        CHECK(desc->client == client);
        CHECK_FALSE(session.DrainFrame(1));
        // The heartbeat rewrites the whole descriptor: remove it and force a rewrite.
        std::error_code ec;
        std::filesystem::remove(session.DescriptorFile(), ec);
        session.MarkStopping();   // WriteHeartbeat, at once
        desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        CHECK(desc->client == client);
        session.Stop("test");
    }
}

// Final fix pass (I1): the descriptor says whether an explicit --remote armed
// the host, at Start and on every rewrite, and every host copies its own flag
// (the editor's m_config.remote, never editor.remote.enable). A descriptor
// without the key (an older host) reads false.
TEST_CASE("HostSession records the explicit --remote mark in the descriptor", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    for (const bool marked : {false, true}) {
        INFO("explicitRemote " << marked);
        Arcane::Test::TempDir tmp("remote-session-explicit");
        auto cfg = ServerConfig(tmp.path);
        cfg.explicitRemote = marked;
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        CHECK(desc->explicitRemote == marked);
        std::error_code ec;
        std::filesystem::remove(session.DescriptorFile(), ec);
        session.MarkStopping();   // WriteHeartbeat, at once
        desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        CHECK(desc->explicitRemote == marked);
        session.Stop("test");
    }
    CHECK_FALSE(Arcane::Remote::HostSession::Config{}.explicitRemote);

    Arcane::Test::TempDir old("remote-descriptor-explicit");
    const auto path = old.path / "editor-77.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"protocol":1,"hostKind":"editor","pid":77,"port":5555,"token":"t"})";
    }
    const auto back = Arcane::Remote::ReadDescriptor(path);
    REQUIRE(back);
    CHECK_FALSE(back->explicitRemote);

    const auto read = [](const char* relative) {
        std::ifstream in(Arcane::Test::RepoRoot() / relative);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    const auto lineWith = [](const std::string& text, const std::string& needle) {
        const auto at = text.find(needle);
        REQUIRE(at != std::string::npos);
        return text.substr(at, text.find('\n', at) - at);
    };
    const std::regex editorFlag(R"(rc\.explicitRemote\s*=\s*m_config\.remote;)");
    const std::regex serverFlag(R"(rc\.explicitRemote\s*=\s*m_cfg\.remote;)");
    CHECK(std::regex_search(lineWith(read("Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp"), "rc.explicitRemote"), editorFlag));
    CHECK(std::regex_search(lineWith(read("Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.cpp"), "rc.explicitRemote"), editorFlag));
    CHECK(std::regex_search(lineWith(read("Source/Programs/ArcaneServer/src/ServerApp.cpp"), "rc.explicitRemote"), serverFlag));
}

// B7 fix round 1 (M2): Cli takes the next token as --client's value even when it
// is another flag, so --client --headless would swallow --headless. Both hosts
// refuse that and an empty name, as --remote-descriptor does (exit 2).
TEST_CASE("host --client refuses an empty name or another flag as its value", "[host][remote]") {
    char* flagValue[] = {const_cast<char*>("ArcaneEditor"), const_cast<char*>("--remote"),
                         const_cast<char*>("--client"), const_cast<char*>("--headless")};
    char* emptyValue[] = {const_cast<char*>("ArcaneEditor"), const_cast<char*>("--remote"),
                          const_cast<char*>("--client=")};
    char* named[] = {const_cast<char*>("ArcaneEditor"), const_cast<char*>("--remote"),
                     const_cast<char*>("--client"), const_cast<char*>("b7-x")};
    auto h0 = Arcane::HostConfig::Parse(4, flagValue);
    CHECK_FALSE(h0.config);
    CHECK(h0.exitCode == 2);
    auto h1 = Arcane::HostConfig::Parse(3, emptyValue);
    CHECK_FALSE(h1.config);
    CHECK(h1.exitCode == 2);
    auto h2 = Arcane::HostConfig::Parse(4, named);
    REQUIRE(h2.config);
    CHECK(h2.config->remoteClient == "b7-x");
    char* serverFlag[] = {const_cast<char*>("ArcaneServer"), const_cast<char*>("--project"),
                          const_cast<char*>("ReferenceProject"), const_cast<char*>("--remote"),
                          const_cast<char*>("--client"), const_cast<char*>("--frames")};
    auto s0 = Arcane::Server::ServerConfig::Parse(6, serverFlag);
    CHECK_FALSE(s0.config);
    CHECK(s0.exitCode == 2);
}

TEST_CASE("omitted --owner-pid is zero, and a dead owner pid exits", "[host][remote]") {
    // The flag: omitted is 0, typed is the number (HostConfig and ServerConfig).
    {
        char* bare[] = {const_cast<char*>("ArcaneEditor"), const_cast<char*>("--remote")};
        char* owned[] = {const_cast<char*>("ArcaneEditor"), const_cast<char*>("--remote"),
                         const_cast<char*>("--owner-pid"), const_cast<char*>("4242")};
        auto h0 = Arcane::HostConfig::Parse(2, bare);
        auto h1 = Arcane::HostConfig::Parse(4, owned);
        REQUIRE(h0.config);
        REQUIRE(h1.config);
        CHECK(h0.config->ownerPid == 0);
        CHECK(h1.config->ownerPid == 4242);
        char* serverBare[] = {const_cast<char*>("ArcaneServer"), const_cast<char*>("--project"),
                              const_cast<char*>("ReferenceProject"), const_cast<char*>("--remote")};
        char* serverOwned[] = {const_cast<char*>("ArcaneServer"), const_cast<char*>("--project"),
                               const_cast<char*>("ReferenceProject"), const_cast<char*>("--remote"),
                               const_cast<char*>("--owner-pid"), const_cast<char*>("4242")};
        auto s0 = Arcane::Server::ServerConfig::Parse(4, serverBare);
        auto s1 = Arcane::Server::ServerConfig::Parse(6, serverOwned);
        REQUIRE(s0.config);
        REQUIRE(s1.config);
        CHECK(s0.config->ownerPid == 0);
        CHECK(s1.config->ownerPid == 4242);
    }

    // HostSession, ownerPid 0: nothing is watched, nothing exits, the key reads 0 across Start and the rewrite.
    {
        RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
        Arcane::Test::TempDir tmp("remote-session-owner0");
        auto cfg = ServerConfig(tmp.path);
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        CHECK(desc->ownerPid == 0);
        CHECK_FALSE(session.DrainFrame(1));
        CHECK(session.ExitReason() == nullptr);
        std::error_code ec;
        std::filesystem::remove(session.DescriptorFile(), ec);
        session.MarkStopping();   // the heartbeat rewrite
        desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        CHECK(desc->ownerPid == 0);
        session.Stop("test");
    }

    // HostSession, a dead owner: the first pid below 0x0FFFFFF0 that IsProcessLive refuses
    // (never this process, never the shell). DrainFrame takes the idle exit.
    {
        std::uint32_t dead = 0x0FFFFFF0u;
        while (dead > 1u && Arcane::Platform::IsProcessLive(dead, 0))
            --dead;
        REQUIRE(dead > 1u);
        REQUIRE_FALSE(Arcane::Platform::IsProcessLive(dead, 0));
        RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
        Arcane::Test::TempDir tmp("remote-session-ownerdead");
        auto cfg = ServerConfig(tmp.path);
        cfg.ownerPid = dead;
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        CHECK(desc->ownerPid == dead);
        CHECK(session.DrainFrame(1));
        REQUIRE(session.ExitReason() != nullptr);
        CHECK(std::string(session.ExitReason()) == "remote-idle");
        session.Stop("test");
    }

    // Source pins: the idle assignments key off the explicit --remote and maxFrames/frames (never headless),
    // through Arming's helpers, and every rc block copies ownerPid through Arming::OwnerPid.
    const auto read = [](const char* relative) {
        std::ifstream in(Arcane::Test::RepoRoot() / relative);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    const auto lineWith = [](const std::string& text, const std::string& needle) {
        const auto at = text.find(needle);
        REQUIRE(at != std::string::npos);
        const auto end = text.find('\n', at);
        return text.substr(at, end - at);
    };
    const std::string runtime = read("Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.cpp");
    const std::string editor = read("Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    const std::string server = read("Source/Programs/ArcaneServer/src/ServerApp.cpp");
    const std::string runtimeIdle = lineWith(runtime, "rc.idleExitSeconds");
    const std::string editorIdle = lineWith(editor, "rc.idleExitSeconds");
    CHECK(runtimeIdle.find("headless") == std::string::npos);
    CHECK(runtimeIdle.find("Arming::IdleExitSeconds(m_config.remote, m_config.maxFrames,") != std::string::npos);
    CHECK(editorIdle.find("headless") == std::string::npos);
    CHECK(editorIdle.find("Arming::IdleExitSeconds(m_config.remote, m_config.maxFrames,") != std::string::npos);
    CHECK(lineWith(server, "rc.idleExitSeconds").find("Arming::IdleExitSeconds(m_cfg.remote, m_cfg.frames,") != std::string::npos);
    CHECK(lineWith(runtime, "rc.ownerPid").find("Arming::OwnerPid(m_config.remote,") != std::string::npos);
    CHECK(lineWith(editor, "rc.ownerPid").find("Arming::OwnerPid(m_config.remote,") != std::string::npos);
    CHECK(lineWith(server, "rc.ownerPid").find("Arming::OwnerPid(m_cfg.remote,") != std::string::npos);
}

// B7 fix round 1 (C2): editor.remote.enable defaults ON, so the editor a user
// opens by hand arms a HostSession with no --remote. That editor must never
// end itself on the idle timer (outside host.quit's unsaved-changes guard) and
// never watch an owner pid: only an explicit --remote (servitor launch always
// passes it) arms either, and only on an open-ended run.
TEST_CASE("an editor armed only by editor.remote.enable never idle-exits or watches an owner", "[host][remote]") {
    const Arcane::HostConfig byHand{};   // editor.remote.enable at its default, no --remote, no --frames
    REQUIRE_FALSE(byHand.remote);
    REQUIRE(byHand.maxFrames == 0);
    REQUIRE(byHand.remoteIdleExitSeconds == 600);

    // The editor's session config comes from these lines (EditorAppFrame.cpp is
    // not compiled into ArcaneTests): both must key off the explicit --remote.
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string editor = ss.str();
    const auto lineWith = [&editor](const std::string& needle) {
        const auto at = editor.find(needle);
        REQUIRE(at != std::string::npos);
        return editor.substr(at, editor.find('\n', at) - at);
    };
    // m_config.remote as a whole word: m_config.remoteIdleExitSeconds is not the flag.
    const std::regex explicitRemote(R"(m_config\.remote\b)");
    CHECK(std::regex_search(lineWith("rc.idleExitSeconds"), explicitRemote));
    CHECK(std::regex_search(lineWith("rc.ownerPid"), explicitRemote));

    // The rule those lines call: the hand-opened editor gets neither; a Servitor
    // launch (--remote, open-ended) keeps both; --frames N never idles out.
    using Arming = Arcane::Remote::Arming;
    CHECK(Arming::IdleExitSeconds(byHand.remote, byHand.maxFrames, byHand.remoteIdleExitSeconds) == 0);
    CHECK(Arming::OwnerPid(byHand.remote, 4242) == 0);
    CHECK(Arming::IdleExitSeconds(true, 0, 600) == 600);
    CHECK(Arming::IdleExitSeconds(true, 5, 600) == 0);
    CHECK(Arming::OwnerPid(true, 4242) == 4242);
}

// Final fix pass (b7 m5): a Config built without Arming never idles out. All
// three host sites set idleExitSeconds through Arming::IdleExitSeconds; a
// future site that forgets must not reopen C2 with a 600 s default.
TEST_CASE("HostSession::Config's idle exit defaults to never", "[host][remote]") {
    const Arcane::Remote::HostSession::Config cfg{};
    CHECK(cfg.idleExitSeconds == 0u);
}

TEST_CASE("a descriptor without ownerPid reads zero, and one with it round-trips", "[host][remote][descriptor]") {
    Arcane::Test::TempDir tmp("remote-descriptor-owner");
    const auto path = tmp.path / "runtime-77.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"protocol":1,"hostKind":"runtime","pid":77,"port":5555,"token":"t"})";
    }
    auto back = Arcane::Remote::ReadDescriptor(path);
    REQUIRE(back);
    CHECK(back->ownerPid == 0);
    Arcane::Remote::Descriptor d = *back;
    d.ownerPid = 4242;
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(path, d));
    back = Arcane::Remote::ReadDescriptor(path);
    REQUIRE(back);
    CHECK(back->ownerPid == 4242);
}
#endif
