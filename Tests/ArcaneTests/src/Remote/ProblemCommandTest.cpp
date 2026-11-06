#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/ProblemCommands.hpp>
#include <Arcane/Base/DiagnosticStore.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include "RemoteTestSupport.hpp"
#include "RemoteTestHttp.hpp"   // ConnectLoopback, SendAll, RecvUntil
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir
#include <Arcane/Remote/Dispatcher.hpp> // HostGate
#include <Arcane/Remote/Follow.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Http.hpp>       // MakeToken
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Net/TcpSocket.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#if !defined(ARC_BUILD_DIST)

TEST_CASE("problems --search uses Filtered and a dead entity handle warns", "[remote]") {
    Arcane::DiagnosticStore store;
    Arcane::Diagnostic row;
    row.severity = Arcane::DiagSeverity::Error;
    row.scope = Arcane::DiagScope::Project;
    row.code = "remote.probe";
    row.message = "needle in the row";
    row.locator = Arcane::DiagLocator::Entity(999);
    const Arcane::Diagnostic rows[] = {row};
    store.Publish("remote-probe", rows);
    RemoteTest::ScopedRemotePointer<Arcane::DiagnosticStore> storeGuard(&Arcane::Remote::SetProblemStore, &store);
    Arcane::Remote::RegisterProblemCommand();
    Arcane::Remote::Call call;
    call.command = "problems.list";
    call.args = nlohmann::json{{"severity", "error"}, {"search", "needle"}};
    const auto hit = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(hit.ok);
    REQUIRE(hit.data["problems"].size() == 1);
    CHECK(hit.data["problems"][0]["locator"]["guid"].is_null());
    CHECK(hit.data["problems"][0]["locator"]["handle"] == 999);
    CHECK(hit.warnings[0].code == "entity.no-identity");
    call.args["search"] = "absent";
    const auto miss = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(miss.data["problems"].empty());
    call.args["follow"] = true;
    const auto follow = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(follow.completesJob == 0);
    CHECK(follow.envelope.errors[0].code == "arg.inapplicable");
    CHECK(Arcane::Remote::ExitFromHost(follow.envelope) == 2);
}

// --- Task 22 fix round 1 ---

namespace {

Arcane::Diagnostic ProbeRow(Arcane::DiagSeverity severity, const char* code, const char* message) {
    Arcane::Diagnostic d;
    d.severity = severity;
    d.scope = Arcane::DiagScope::Project;
    d.code = code;
    d.message = message;
    return d;
}

std::size_t Occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1))
        ++count;
    return count;
}

} // namespace

TEST_CASE("a HostSession given a store lists the rows it held before Start", "[remote]") {
    // The runtime and server install their store before boot (RuntimeApp::Run,
    // ServerApp::Run) and hand it to the session: a boot-time row is listed.
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir root("remote-problems-session");
    Arcane::DiagnosticStore store;
    const Arcane::Diagnostic rows[] = {ProbeRow(Arcane::DiagSeverity::Warning, "boot.probe", "published at boot")};
    store.Publish("boot", rows);
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root.path;
    cfg.problemStore = &store;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK(Arcane::Remote::ProblemStore() == &store);
    Arcane::Remote::Call call;
    call.command = "problems.list";
    const auto listed = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(listed.ok);
    REQUIRE(listed.data["problems"].size() == 1);
    CHECK(listed.data["problems"][0]["code"] == "boot.probe");
    CHECK(listed.data["problems"][0]["severity"] == "warning");
    session.Stop("test");
    CHECK(Arcane::Remote::ProblemStore() == nullptr);   // released: a stopped session answers host.unavailable
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope.errors[0].code == "host.unavailable");
}

TEST_CASE("a HostSession given no store owns one and makes it the engine sink", "[remote]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Server);
    Arcane::Test::TempDir root("remote-problems-owned");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Server;
    cfg.cvarContext = Arcane::CVarContext::ServerAdmin;
    cfg.projectRoot = root.path;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    const Arcane::Diagnostic rows[] = {ProbeRow(Arcane::DiagSeverity::Error, "owned.probe", "after start")};
    Arcane::Diagnostics::Publish("remote-owned-probe", rows);
    Arcane::Remote::Call call;
    call.command = "problems.list";
    const auto listed = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(listed.ok);
    REQUIRE(listed.data["problems"].size() == 1);
    CHECK(listed.data["problems"][0]["code"] == "owned.probe");
    session.Stop("test");
    CHECK(Arcane::Remote::ProblemStore() == nullptr);
    Arcane::Diagnostics::Publish("remote-owned-probe", rows);   // the sink was uninstalled: nothing dangles
}

TEST_CASE("problem.follow arms at the snapshot size and streams only rows that are new", "[remote][listener]") {
    Arcane::DiagnosticStore store;
    const Arcane::Diagnostic first[] = {ProbeRow(Arcane::DiagSeverity::Error, "old.a", "old-row-a"),
                                        ProbeRow(Arcane::DiagSeverity::Warning, "old.b", "old-row-b")};
    store.Publish("k1", first);
    RemoteTest::ScopedRemotePointer<Arcane::DiagnosticStore> storeGuard(&Arcane::Remote::SetProblemStore, &store);
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const SocketType client = RemoteTest::ConnectLoopback(listener.Port());
    RemoteTest::SendAll(client, "GET /remote/v1/stream?topics=problems HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(listener.Port()) + "\r\nAuthorization: Bearer " + listener.Token() + "\r\n\r\n");
    const std::string headers = RemoteTest::RecvUntil(client, "\r\n\r\n", std::chrono::seconds(1));
    CHECK(headers.find("200") != std::string::npos);

    Arcane::Remote::JobTable jobs;
    Arcane::Remote::JobId followId{};
    const Arcane::Remote::Job* opened = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (opened == nullptr && std::chrono::steady_clock::now() < deadline) {
        Arcane::Remote::ApplyFollowEvents(jobs, listener);
        for (std::uint64_t id = 1; id < 8 && opened == nullptr; ++id) {
            const Arcane::Remote::Job* candidate = jobs.Find(Arcane::Remote::JobId{id});
            if (candidate && candidate->subscriberId != 0) {
                opened = candidate;
                followId = candidate->id;
            }
        }
        if (opened == nullptr)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(opened != nullptr);
    CHECK(opened->command == "problem.follow");
    CHECK(opened->state == Arcane::Remote::JobState::Running);
    CHECK(opened->progress.done == 2);   // the store's snapshot size when the stream opened (ruling F4)

    Arcane::Remote::HostGate gate;
    jobs.StepAll(gate);                  // nothing new yet
    const Arcane::Diagnostic added[] = {ProbeRow(Arcane::DiagSeverity::Info, "new.c", "brand-new-row")};
    store.Publish("k2", added);
    jobs.StepAll(gate);
    std::string streamed = RemoteTest::RecvUntil(client, "brand-new-row", std::chrono::seconds(1));
    CHECK(jobs.Find(followId)->progress.done == 3);
    // Re-publishing identical sets (a producer's periodic refresh) streams nothing.
    store.Publish("k1", first);
    store.Publish("k2", added);
    jobs.StepAll(gate);
    const Arcane::Diagnostic second[] = {ProbeRow(Arcane::DiagSeverity::Error, "new.d", "second-new-row")};
    store.Publish("k3", second);
    jobs.StepAll(gate);
    streamed += RemoteTest::RecvUntil(client, "second-new-row", std::chrono::seconds(1));
    CHECK(Occurrences(streamed, "brand-new-row") == 1);
    CHECK(Occurrences(streamed, "second-new-row") == 1);
    CHECK(Occurrences(streamed, "old-row") == 0);   // the rows present at open are never streamed
    CHECK(jobs.Find(followId)->progress.done == 4);

    CloseSocket(client);
    Arcane::Remote::JobState state = Arcane::Remote::JobState::Running;
    for (int i = 0; i < 50 && state != Arcane::Remote::JobState::Cancelled; ++i) {
        Arcane::Remote::ApplyFollowEvents(jobs, listener);
        state = jobs.Find(followId)->state;
        if (state != Arcane::Remote::JobState::Cancelled)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(state == Arcane::Remote::JobState::Cancelled);
    listener.Stop();
}

#endif
