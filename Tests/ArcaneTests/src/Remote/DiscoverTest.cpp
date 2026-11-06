#include <catch2/catch_test_macros.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Commands.hpp>   // ParseCallTokens (Task 30 fix round 1: the request cap)
#include <Servitor/Discover.hpp>
#include <Servitor/Transport.hpp>
#include <Servitor/Verbs.hpp>   // FindVerb (Task 30: --out as UTF-8)
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Http.hpp>       // MakeToken (Task 5)
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Platform/Process.hpp>  // ExecutablePath
#include <Arcane/Project/Project.hpp>   // EditorLock::Self (ArcaneEngine)
#include <Json.hpp>
#include "RemoteDemoArgs.hpp"
#include "RemoteTestHttp.hpp"
#include "RemoteTestSupport.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>        // _dup, _dup2, _open, _close, _fileno
#else
#include <unistd.h>    // dup, dup2, close
#endif

#if !defined(ARC_BUILD_DIST)

TEST_CASE("two live hosts without --pid are ambiguous and a stale file is removed", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Saved" / "Remote");
    Arcane::Remote::Descriptor a;
    a.protocol = 1;
    a.hostKind = Arcane::Remote::HostKind::Editor;
    a.pid = Arcane::EditorLock::Self().pid;
    a.processStart = Arcane::EditorLock::Self().start;
    a.port = 1;
    a.token = "a";
    a.state = "ready";
    a.heartbeatUnixMs = 1'700'000'000'000;
    Arcane::Remote::Descriptor b = a;
    b.hostKind = Arcane::Remote::HostKind::Runtime;
    b.port = 2;
    b.token = "b";
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, a.hostKind, a.pid), a));
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, b.hostKind, b.pid), b));
    Arcane::Remote::Descriptor stale = a;
    stale.pid = 1;
    stale.processStart = a.processStart + 1;
    stale.hostKind = Arcane::Remote::HostKind::Server;
    const auto stalePath = Arcane::Remote::DescriptorPath(root, stale.hostKind, stale.pid);
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(stalePath, stale));
    const auto scan = Servitor::ScanProject(root);
    CHECK(scan.live.size() == 2);
    CHECK_FALSE(std::filesystem::exists(stalePath));
    Servitor::GlobalOptions opt;
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::Select(scan, opt, &err));
    CHECK(err.errors[0].code == "target.ambiguous");
    CHECK(Arcane::Remote::ExitFromHost(err) == 2);
    opt.pid = a.pid;
    opt.hostKind = "runtime";
    auto runtime = Servitor::Select(scan, opt, &err);
    REQUIRE(runtime);
    CHECK(runtime->descriptor.port == 2);
    CHECK(runtime->descriptor.hostKind == Arcane::Remote::HostKind::Runtime);
    CHECK(runtime->hung);
    std::filesystem::remove_all(root);
}

TEST_CASE("zero hosts is target.none exit 4", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan-empty";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto scan = Servitor::ScanProject(root);
    Servitor::GlobalOptions opt;
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::Select(scan, opt, &err));
    CHECK(err.errors[0].code == "target.none");
    CHECK(Arcane::Remote::ExitFromHost(err) == 4);
    std::filesystem::remove_all(root);
}

TEST_CASE("a fresh heartbeat is not hung and status reports the flag", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan-fresh";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Saved" / "Remote");
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Arcane::Remote::Descriptor d;
    d.protocol = 1;
    d.hostKind = Arcane::Remote::HostKind::Runtime;
    d.pid = Arcane::EditorLock::Self().pid;
    d.processStart = Arcane::EditorLock::Self().start;
    d.port = 9;
    d.token = "secret-token";
    d.state = "ready";
    d.heartbeatUnixMs = nowMs;
    d.abi = 61;
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, d.hostKind, d.pid), d));
    auto scan = Servitor::ScanProject(root);
    REQUIRE(scan.live.size() == 1);
    CHECK_FALSE(scan.live[0].hung);
    auto data = Servitor::StatusData(scan);
    REQUIRE(data["hosts"].size() == 1);
    CHECK(data["hosts"][0]["hostKind"] == "runtime");
    CHECK(data["hosts"][0]["pid"] == d.pid);
    CHECK(data["hosts"][0]["abi"] == 61);   // from Descriptor::abi (ruling F27)
    CHECK(data["hosts"][0]["hung"] == false);
    CHECK(data["hosts"][0]["heartbeatAgeMs"].get<std::int64_t>() < 5000);
    CHECK(data.dump().find("secret-token") == std::string::npos);
    d.heartbeatUnixMs = nowMs - 60000;
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, d.hostKind, d.pid), d));
    scan = Servitor::ScanProject(root);
    REQUIRE(scan.live.size() == 1);
    CHECK(scan.live[0].hung);
    data = Servitor::StatusData(scan);
    CHECK(data["hosts"][0]["hung"] == true);
    CHECK(data["hosts"][0]["heartbeatAgeMs"].get<std::int64_t>() > 10000);
    Servitor::GlobalOptions opt;
    opt.hostKind = "runtime";
    Arcane::Remote::Envelope err;
    auto selected = Servitor::Select(scan, opt, &err);
    REQUIRE(selected);
    CHECK(selected->hung);
    std::filesystem::remove_all(root);
}

TEST_CASE("a newer protocol major reaches Select and is protocol.mismatch, not target.none", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan-major";
    std::filesystem::remove_all(root);
    Arcane::Remote::Descriptor d;
    d.protocol = 2;   // a newer host (rev 8.3 V6): ReadDescriptor keeps it
    d.hostKind = Arcane::Remote::HostKind::Server;
    d.pid = Arcane::EditorLock::Self().pid;
    d.processStart = Arcane::EditorLock::Self().start;
    d.port = 7;
    d.token = "t";
    d.heartbeatUnixMs = Servitor::NowUnixMs();
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, d.hostKind, d.pid), d));
    const auto scan = Servitor::ScanProject(root);
    REQUIRE(scan.live.size() == 1);
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::Select(scan, Servitor::GlobalOptions{}, &err));
    REQUIRE(err.errors.size() == 1);
    CHECK(err.errors[0].code == "protocol.mismatch");
    CHECK(err.errors[0].remediation.find('2') != std::string::npos);
    CHECK(Arcane::Remote::ExitFromHost(err) == 4);
    std::filesystem::remove_all(root);
}

TEST_CASE("--host and --pid that match no live host are target.none naming the candidates", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan-nomatch";
    std::filesystem::remove_all(root);
    Arcane::Remote::Descriptor d;
    d.hostKind = Arcane::Remote::HostKind::Editor;
    d.pid = Arcane::EditorLock::Self().pid;
    d.processStart = Arcane::EditorLock::Self().start;
    d.port = 5;
    d.token = "t";
    d.heartbeatUnixMs = Servitor::NowUnixMs();
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, d.hostKind, d.pid), d));
    const auto scan = Servitor::ScanProject(root);
    Servitor::GlobalOptions opt;
    opt.hostKind = "server";
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::Select(scan, opt, &err));
    CHECK(err.errors[0].code == "target.none");
    REQUIRE(err.errors[0].candidates.size() == 1);
    CHECK(err.errors[0].candidates[0] == "editor:" + std::to_string(d.pid));
    std::filesystem::remove_all(root);
}

TEST_CASE("HostsDir: --engine beats SERVITOR_ENGINE beats the sibling; sdk.json picks the config", "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("servitor-hostsdir-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(tmp);
    const fs::path pkg = tmp / "pkg";                       // an installed engine
    fs::create_directories(pkg / "sdk");
    { std::ofstream(pkg / "sdk" / "sdk.json") << R"({"engineConfig":"Release"})"; }
    const fs::path src = tmp / "src";                       // a source checkout (no sdk.json)
    fs::create_directories(src);
    const fs::path broken = tmp / "broken";                 // sdk.json without engineConfig
    fs::create_directories(broken / "sdk");
    { std::ofstream(broken / "sdk" / "sdk.json") << R"({})"; }

    const std::string tail = std::string(SERVITOR_OUTPUTDIR).substr(std::string(SERVITOR_OUTPUTDIR).find('-'));
    CHECK(Servitor::HostsDirForRoot(pkg) == pkg / "Binaries" / ("Release" + tail));
    CHECK(Servitor::HostsDirForRoot(src) == src / "Binaries" / SERVITOR_OUTPUTDIR);
    CHECK_FALSE(Servitor::HostsDirForRoot(broken).has_value());

    RemoteTest::ScopedEnv env("SERVITOR_ENGINE", src.string());
    Servitor::GlobalOptions flag;
    flag.engine = pkg.string();
    CHECK(Servitor::HostsDir(flag, nullptr) == Servitor::HostsDirForRoot(pkg));      // the flag wins
    Servitor::GlobalOptions none;
    CHECK(Servitor::HostsDir(none, nullptr) == Servitor::HostsDirForRoot(src));      // then the env var
    Arcane::Remote::Envelope error;
    Servitor::GlobalOptions bad;
    bad.engine = broken.string();
    CHECK_FALSE(Servitor::HostsDir(bad, &error).has_value());
    CHECK(error.errors[0].code == "engine.unresolved");
    CHECK(Arcane::Remote::ExitFromHost(error) == 4);
    fs::remove_all(tmp);
}

TEST_CASE("HostsDir without a flag or env var finds the source sibling", "[servitor][client]") {
    RemoteTest::ScopedEnv env("SERVITOR_ENGINE", std::nullopt);   // unset for this case
    const auto dir = Servitor::HostsDir(Servitor::GlobalOptions{}, nullptr);
    REQUIRE(dir.has_value());   // ArcaneTests runs from Binaries/<cfg>/ArcaneTests; its sibling folders exist
    CHECK(*dir == Arcane::Platform::ExecutablePath().parent_path().parent_path());
}

TEST_CASE("HostDiagnosticsDir resolves through HostsDir and is empty when the engine is unresolved", "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path src = fs::temp_directory_path() / ("servitor-diag-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(src);
    fs::create_directories(src);
    Servitor::GlobalOptions opt;
    opt.engine = src.string();
    CHECK(Servitor::HostDiagnosticsDir(Arcane::Remote::HostKind::Runtime, opt) ==
          src / "Binaries" / SERVITOR_OUTPUTDIR / "ArcaneRuntime" / "diagnostics");
    CHECK(Servitor::HostDiagnosticsDir(Arcane::Remote::HostKind::Editor, opt) ==
          src / "Binaries" / SERVITOR_OUTPUTDIR / "ArcaneEditor" / "diagnostics");
    fs::create_directories(src / "sdk");
    { std::ofstream(src / "sdk" / "sdk.json") << R"({"engineConfig":7})"; }   // not a string
    CHECK(Servitor::HostDiagnosticsDir(Arcane::Remote::HostKind::Server, opt).empty());
    fs::remove_all(src);
}

// --- the live transport (rev 8.3 V3) ---------------------------------------

namespace {

std::string g_discoverCaller;   // the last discover.noop call's Call::callerId (final fix pass, C-I1)

Arcane::Remote::CommandResult DiscoverNoop(const Arcane::Remote::Call& call, void*) {
    g_discoverCaller = call.callerId;
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeOk(call.command, nlohmann::json{{"noop", true}}, Arcane::Remote::HostBlock{});
    return r;
}

struct DiscoverUnregister {
    ~DiscoverUnregister() { Arcane::Remote::CommandRegistry::Get().UnregisterModule("discover-test"); }
};

// A descriptor naming this process with a fresh heartbeat, so the watch
// LiveExchange samples sees a live, unhung host.
Servitor::LiveTarget DiscoverTarget(const std::filesystem::path& dir, std::uint16_t port, std::string token) {
    Servitor::LiveTarget t;
    t.descriptor.hostKind = Arcane::Remote::HostKind::Runtime;
    t.descriptor.pid = Arcane::EditorLock::Self().pid;
    t.descriptor.processStart = Arcane::EditorLock::Self().start;
    t.descriptor.port = port;
    t.descriptor.token = std::move(token);
    t.descriptor.state = "ready";
    t.descriptor.heartbeatUnixMs = Servitor::NowUnixMs();
    const auto path = Arcane::Remote::DescriptorPath(dir, t.descriptor.hostKind, t.descriptor.pid);
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(path, t.descriptor));
    t.watch.command = "discover.noop";
    t.watch.pid = t.descriptor.pid;
    t.watch.processStart = t.descriptor.processStart;
    t.watch.descriptorPath = path;
    t.timeoutMs = 5000;
    return t;
}

// Runs LiveExchange on a worker while this thread drains the host queue, as a
// host main thread would. Gives up after 10 s.
Servitor::ExchangeResult DiscoverExchange(Servitor::LiveTarget& target, const std::string& body,
                                          Arcane::Remote::RemoteDispatcher* dispatcher) {
    auto pending = std::async(std::launch::async, [&] { return Servitor::LiveExchange(body, &target); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (pending.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        if (dispatcher != nullptr)
            dispatcher->Drain(1000.0, 8);
    }
    REQUIRE(pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    return pending.get();
}

}  // namespace

TEST_CASE("LiveExchange returns the command's envelope; a wrong token is auth.denied; a stopped listener refuses before the body",
          "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("servitor-live-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(dir);
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    DiscoverUnregister unregister;
    Arcane::Remote::Desc desc;
    desc.name = "discover.noop";
    desc.summary = "discover test no-op";
    desc.module = "discover-test";
    desc.argsMeta = Astra::GetMeta<RemoteDemoArgs>();
    desc.fn = &DiscoverNoop;
    std::string regErr;
    REQUIRE(reg.Register(desc, &regErr));

    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto status = std::make_shared<Arcane::Remote::HostStatus>();
    status->state = "ready";
    status->hostKind = Arcane::Remote::HostKind::Runtime;
    atoms.status.store(status);
    atoms.catalog.store(std::make_shared<const nlohmann::json>(nlohmann::json::object()));
    Arcane::Remote::HostGate gate;
    gate.kind = "runtime";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, dir / "audit.ndjson");
    const std::string token = Arcane::Remote::MakeToken();
    Arcane::Remote::RemoteListener listener(queue, atoms, token);
    listener.Start();
    const std::uint16_t port = listener.Port();
    const std::string body =
        R"({"id":"d1","command":"discover.noop","args":{"position":[0,0,0]},"options":{"timeoutMs":5000}})";

    auto good = DiscoverTarget(dir, port, token);
    good.clientId = "agent-7 (ci)";   // the X-Remote-Client value (C-I1): the host's Call::callerId
    g_discoverCaller.clear();
    const auto ok = DiscoverExchange(good, body, &dispatcher);
    CHECK_FALSE(ok.connectionRefusedBeforeBody);
    CHECK(g_discoverCaller == "agent-7 (ci)");
    CHECK(ok.envelope.ok);
    CHECK(ok.envelope.command == "discover.noop");
    CHECK(ok.envelope.data["noop"] == true);
    CHECK(Arcane::Remote::ExitFromHost(ok.envelope) == 0);
    CHECK(good.watch.requestUnixMs > 0);   // stamped when the exchange started

    // GET /remote/v1/status over the same codec.
    Arcane::Remote::Envelope getErr;
    const auto got = Servitor::GetJson(good.descriptor, "/remote/v1/status", 5000, &getErr);
    REQUIRE(got.has_value());
    CHECK((*got)["state"] == "ready");
    CHECK(got->dump().find(token) == std::string::npos);

    auto wrong = DiscoverTarget(dir, port, std::string(64, 'a'));
    const auto denied = DiscoverExchange(wrong, body, nullptr);
    CHECK_FALSE(denied.connectionRefusedBeforeBody);
    REQUIRE_FALSE(denied.envelope.errors.empty());
    CHECK(denied.envelope.errors[0].code == "auth.denied");   // HTTP 401; the envelope decides
    CHECK(Arcane::Remote::ExitFromHost(denied.envelope) == 3);

    listener.Stop();
    auto gone = DiscoverTarget(dir, port, token);
    const auto refused = DiscoverExchange(gone, body, nullptr);
    CHECK(refused.connectionRefusedBeforeBody);
    REQUIRE_FALSE(refused.envelope.errors.empty());       // a real envelope with a code
    CHECK(refused.envelope.errors[0].code == "host.busy");
    CHECK(refused.envelope.errors[0].detail["state"] == "connection-refused");
    CHECK(Arcane::Remote::ExitFromHost(refused.envelope) == 5);
    fs::remove_all(dir);
}

TEST_CASE("LiveExchange answers host.died before connecting when the watched process is gone", "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("servitor-live-died-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(dir);
    auto target = DiscoverTarget(dir, RemoteTest::UnusedLoopbackPort(), "t");
    target.watch.processStart += 1;   // not this process any more (a single exchange, no retry)
    const auto r = DiscoverExchange(target, R"({"id":"d","command":"discover.noop","args":{}})", nullptr);
    CHECK_FALSE(r.connectionRefusedBeforeBody);
    REQUIRE_FALSE(r.envelope.errors.empty());
    CHECK(r.envelope.errors[0].code == "host.died");
    CHECK(Arcane::Remote::ExitFromHost(r.envelope) == 7);
    fs::remove_all(dir);
}

namespace {

// The watch is live for the first sample (before the connect) and dead for
// every later one: a host that dies while the connect is being refused.
std::atomic<int> g_discoverSamples{0};
std::optional<Arcane::Remote::Envelope> DiesAfterFirstSample(const Servitor::TargetWatch& watch, std::int64_t) {
    if (g_discoverSamples.fetch_add(1) == 0)
        return std::nullopt;
    return Arcane::Remote::MakeError(watch.command, "host.died", "host process exited during the call", "relaunch");
}

std::optional<Arcane::Remote::Envelope> AlwaysLive(const Servitor::TargetWatch&, std::int64_t) {
    return std::nullopt;
}

// A target for a CannedServer: no descriptor file, a watch that never objects.
Servitor::LiveTarget CannedTarget(std::uint16_t port) {
    Servitor::LiveTarget t;
    t.descriptor.hostKind = Arcane::Remote::HostKind::Runtime;
    t.descriptor.pid = Arcane::EditorLock::Self().pid;
    t.descriptor.port = port;
    t.descriptor.token = "t";
    t.watch.command = "discover.noop";
    t.observe = &AlwaysLive;
    t.timeoutMs = 5000;
    return t;
}

const std::string kDiscoverBody = R"({"id":"d","command":"discover.noop","args":{}})";

}  // namespace

TEST_CASE("a host that dies while the connect is refused is host.died exit 7, not a retryable busy", "[servitor][client]") {
    g_discoverSamples = 0;
    auto target = CannedTarget(RemoteTest::UnusedLoopbackPort());
    target.observe = &DiesAfterFirstSample;
    const auto r = Servitor::LiveExchange(kDiscoverBody, &target);
    CHECK(g_discoverSamples.load() >= 2);   // sampled before the connect, and again after it failed
    CHECK_FALSE(r.connectionRefusedBeforeBody);
    REQUIRE_FALSE(r.envelope.errors.empty());
    CHECK(r.envelope.errors[0].code == "host.died");
    CHECK(Arcane::Remote::ExitFromHost(r.envelope) == 7);
}

TEST_CASE("a reply body that is not an envelope is internal exit 1", "[servitor][client]") {
    {
        RemoteTest::CannedServer host("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
        auto target = CannedTarget(host.Port());
        const auto r = Servitor::LiveExchange(kDiscoverBody, &target);
        CHECK_FALSE(r.connectionRefusedBeforeBody);
        REQUIRE_FALSE(r.envelope.errors.empty());
        CHECK(r.envelope.errors[0].code == "internal");
        CHECK(r.envelope.errors[0].message == "malformed host reply");
        CHECK(r.envelope.errors[0].detail["reason"] == "not-json");
        CHECK(Arcane::Remote::ExitFromHost(r.envelope) == 1);
    }
    {
        RemoteTest::CannedServer host("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 7\r\n\r\n{\"x\":1}");
        auto target = CannedTarget(host.Port());
        const auto r = Servitor::LiveExchange(kDiscoverBody, &target);
        REQUIRE_FALSE(r.envelope.errors.empty());
        CHECK(r.envelope.errors[0].code == "internal");
        CHECK(r.envelope.errors[0].detail["reason"] == "not-an-envelope");
        CHECK(r.envelope.errors[0].detail["httpStatus"] == 503);
        CHECK(Arcane::Remote::ExitFromHost(r.envelope) == 1);
    }
}

// Task 30 (C2, fix round 1 I1): a well-formed ok envelope whose data nests
// to exactly kMaxReplyDepth (the envelope object is level 1) goes through
// EnvelopeOf -> TryParseEnvelope (which copies data) -> the printer (ToJson
// plus client, dumped): every step that recurses, at the cap. One container
// more is the malformed-reply path, never parsed into a document a later copy
// or print would recurse through.
namespace {
std::string DeepReply(std::uint32_t dataLevels) {
    const std::string body = R"({"ok":true,"protocol":1,"id":"d","command":"discover.noop","errors":[],"data":)" +
                             std::string(dataLevels, '[') + std::string(dataLevels, ']') + "}";
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
           body;
}
}  // namespace

TEST_CASE("a reply nested to exactly kMaxReplyDepth is parsed, copied and printed whole", "[servitor][client]") {
    const std::uint32_t cap = Arcane::Remote::kMaxReplyDepth;
    RemoteTest::CannedServer host(DeepReply(cap - 1));
    auto target = CannedTarget(host.Port());
    const auto r = Servitor::LiveExchange(kDiscoverBody, &target);
    CHECK(r.envelope.errors.empty());
    CHECK(r.envelope.ok);
    CHECK(Arcane::Remote::ExitFromHost(r.envelope) == 0);
    Arcane::Remote::ClientBlock client;
    client.exitCode = 0;
    Servitor::RenderOptions ro;
    ro.format = Servitor::Format::Json;
    const Servitor::Printed printed = Servitor::Render(r.envelope, client, ro);
    bool deep = true;
    const auto reparsed = Arcane::Remote::ParseJsonBounded(printed.stdoutText, cap, &deep);
    REQUIRE_FALSE(reparsed.is_discarded());   // the print kept every level, and no more
    CHECK_FALSE(deep);
    CHECK(reparsed.contains("client"));
}

TEST_CASE("a reply nested past kMaxReplyDepth is internal exit 1, reason too-deep", "[servitor][client]") {
    RemoteTest::CannedServer host(DeepReply(Arcane::Remote::kMaxReplyDepth));
    auto target = CannedTarget(host.Port());
    const auto r = Servitor::LiveExchange(kDiscoverBody, &target);
    REQUIRE_FALSE(r.envelope.errors.empty());
    CHECK(r.envelope.errors[0].code == "internal");
    CHECK(r.envelope.errors[0].detail["reason"] == "too-deep");
    CHECK(Arcane::Remote::ExitFromHost(r.envelope) == 1);
}

// The client's own request JSON uses the HOST's cap: servitor refuses early
// what the host would refuse anyway.
TEST_CASE("call --args-json and name=value refuse nesting past kMaxRequestDepth before sending", "[servitor][client]") {
    const std::uint32_t cap = Arcane::Remote::kMaxRequestDepth;
    const auto nest = [](std::uint32_t levels) { return std::string(levels, '[') + std::string(levels, ']'); };
    Servitor::CallRequest req;
    std::string why;
    // The request body is level 1 and args level 2, so an --args-json member
    // may nest cap - 2 more, and a name=value value cap - 2 in all.
    CHECK(Servitor::ParseCallTokens(std::vector<std::string>{"x.y", "--args-json", R"({"v":)" + nest(cap - 2) + "}"},
                                    req, &why));
    CHECK_FALSE(Servitor::ParseCallTokens(
        std::vector<std::string>{"x.y", "--args-json", R"({"v":)" + nest(cap - 1) + "}"}, req, &why));
    CHECK(why.find("nests deeper than") != std::string::npos);
    why.clear();
    CHECK(Servitor::ParseCallTokens(std::vector<std::string>{"x.y", "v=" + nest(cap - 2)}, req, &why));
    CHECK_FALSE(Servitor::ParseCallTokens(std::vector<std::string>{"x.y", "v=" + nest(cap - 1)}, req, &why));
    CHECK(why.find("nests deeper than") != std::string::npos);
    // What it accepts, the host accepts: the body built from the deepest args
    // still parses under the host's cap.
    const nlohmann::json body = {{"id", "c"}, {"command", "x.y"},
                                 {"args", nlohmann::json{{"v", nlohmann::json::parse(nest(cap - 2))}}}};
    CHECK_FALSE(Arcane::Remote::ParseJsonBounded(body.dump(), cap).is_discarded());
}

TEST_CASE("a reply past the byte cap is refused as internal exit 1", "[servitor][client]") {
    const std::string body(4096, 'x');
    RemoteTest::CannedServer host("HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n" + body);
    auto target = CannedTarget(host.Port());
    target.maxReplyBytes = 1024;   // the production cap is kMaxReplyBytes (64 MiB)
    const auto r = Servitor::LiveExchange(kDiscoverBody, &target);
    REQUIRE_FALSE(r.envelope.errors.empty());
    CHECK(r.envelope.errors[0].code == "internal");
    CHECK(r.envelope.errors[0].detail["reason"] == "too-large");
    CHECK(Arcane::Remote::ExitFromHost(r.envelope) == 1);
}

// Final fix pass (C-I2): the body was sent and the watch still says the host
// is alive and its heartbeat fresh, so nothing died: the command may still be
// running. That is call.timeout (exit 8, a check that did not pass in time),
// never host.unreachable (exit 7, "rediscover first") and never a relaunch.
TEST_CASE("a live host that never replies is call.timeout exit 8 at the call's deadline", "[servitor][client]") {
    RemoteTest::CannedServer host("", /*holdOpen=*/true);
    auto target = CannedTarget(host.Port());
    target.timeoutMs = 200;
    target.replyGraceMs = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto r = Servitor::LiveExchange(kDiscoverBody, &target);
    const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    CHECK_FALSE(r.connectionRefusedBeforeBody);
    REQUIRE_FALSE(r.envelope.errors.empty());
    CHECK(r.envelope.errors[0].code == "call.timeout");
    CHECK(Arcane::Remote::ExitFromHost(r.envelope) == 8);
    CHECK(r.envelope.errors[0].detail.value("inFlight", false));
    CHECK(r.envelope.errors[0].remediation.find("relaunch") == std::string::npos);
    CHECK(r.envelope.errors[0].remediation.find("servitor status") != std::string::npos);
    CHECK(tookMs >= 150);
    CHECK(tookMs < 3000);
    // A second exchange of the same call shares the spent budget: it does not
    // wait another 200 ms for a reply.
    RemoteTest::CannedServer again("", /*holdOpen=*/true);
    target.descriptor.port = again.Port();
    const auto second = std::chrono::steady_clock::now();
    const auto r2 = Servitor::LiveExchange(kDiscoverBody, &target);
    REQUIRE_FALSE(r2.envelope.errors.empty());   // refused or unreachable: either way, at once
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - second).count() < 150);
}

TEST_CASE("--project resolves a directory up to its .arcproj, like the working directory", "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path proj = fs::temp_directory_path() / ("servitor-project-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(proj);
    fs::create_directories(proj / "Content" / "scenes");
    { std::ofstream(proj / "Demo.arcproj") << "{}"; }
    const auto expected = Arcane::Remote::DescriptorDir(proj, false);
    Servitor::GlobalOptions opt;
    Arcane::Remote::Envelope err;
    opt.project = (proj / "Content" / "scenes").string();
    CHECK(Servitor::RemoteDirFor(opt, &err) == expected);   // walks up
    opt.project = (proj / "Demo.arcproj").string();
    CHECK(Servitor::RemoteDirFor(opt, &err) == expected);   // the file's folder
    opt.project = proj.string();
    CHECK(Servitor::RemoteDirFor(opt, &err) == expected);
    opt.project = (proj / "missing").string();
    CHECK_FALSE(Servitor::RemoteDirFor(opt, &err).has_value());
    CHECK(err.errors[0].code == "arg.invalid");
    CHECK(Arcane::Remote::ExitFromHost(err) == 2);
    fs::remove_all(proj);
}

// Task 30 (C3): servitor's argv is UTF-8 on every platform. The Windows main
// converts CommandLineToArgvW's wide argv with WideToUtf8, so a character
// outside the ANSI code page is not '?', and the argv consumers build paths
// from UTF-8 (PathFromUtf8), never through the ANSI page.
TEST_CASE("argv is UTF-8: WideToUtf8 keeps non-ANSI text, and --project and --out read it as UTF-8", "[servitor][client]") {
    namespace fs = std::filesystem;
    CHECK(Servitor::WideToUtf8(L"plain --flag") == "plain --flag");
    CHECK(Servitor::WideToUtf8(L"café-中") == "caf\xC3\xA9-\xE4\xB8\xAD");
    CHECK(Servitor::WideToUtf8(L"\U0001F600") == "\xF0\x9F\x98\x80");   // a surrogate pair in UTF-16
    const std::wstring lone(1, static_cast<wchar_t>(0xD800));
    CHECK(Servitor::WideToUtf8(lone) == "\xEF\xBF\xBD");                // never invalid UTF-8
    CHECK(Servitor::WideToUtf8(L"").empty());

    fs::path proj = fs::temp_directory_path() / std::u8string(u8"servitor-utf8-é中-");
    proj += std::to_string(Arcane::EditorLock::Self().pid);
    fs::remove_all(proj);
    fs::create_directories(proj);
    { std::ofstream(proj / "Demo.arcproj") << "{}"; }

    Servitor::GlobalOptions opt;
    Arcane::Remote::Envelope err;
    opt.project = Servitor::PathUtf8(proj);   // what main hands Run for this argument
    const auto root = Servitor::ProjectRootFor(opt, &err);
    REQUIRE(root.has_value());
    CHECK(fs::equivalent(*root, proj));

    const Servitor::VerbRow* row = Servitor::FindVerb("screenshot window");
    REQUIRE(row != nullptr);
    REQUIRE(row->args != nullptr);
    const fs::path shot = proj / std::u8string(u8"shot-é.png");
    const std::vector<std::string> rest = {"--out", Servitor::PathUtf8(shot)};
    nlohmann::json args;
    std::string why;
    REQUIRE(row->args(rest, args, &why));
    REQUIRE(args.contains("out"));
    CHECK(Servitor::PathFromUtf8(args["out"].get<std::string>()).lexically_normal() == shot.lexically_normal());
    fs::remove_all(proj);
}

// --- Run's flag rules (Task 14 review carries) ------------------------------

namespace {

// Points fd 2 at a file for its lifetime, so Run's usage text is captured
// rather than printed into the suite's output.
class DiscoverStderrCapture {
public:
    explicit DiscoverStderrCapture(const std::filesystem::path& file) : m_file(file) {
        std::fflush(stderr);
#if defined(_WIN32)
        m_saved = ::_dup(2);
        const int fd = ::_wopen(file.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd >= 0) {
            ::_dup2(fd, 2);
            ::_close(fd);
        }
#else
        m_saved = ::dup(2);
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            ::dup2(fd, 2);
            ::close(fd);
        }
#endif
    }
    ~DiscoverStderrCapture() { Restore(); }
    // Restores fd 2 and returns what was written to it.
    std::string Take() {
        Restore();
        std::ifstream in(m_file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    DiscoverStderrCapture(const DiscoverStderrCapture&) = delete;
    DiscoverStderrCapture& operator=(const DiscoverStderrCapture&) = delete;

private:
    void Restore() {
        if (m_saved < 0)
            return;
        std::fflush(stderr);
#if defined(_WIN32)
        ::_dup2(m_saved, 2);
        ::_close(m_saved);
#else
        ::dup2(m_saved, 2);
        ::close(m_saved);
#endif
        m_saved = -1;
    }
    std::filesystem::path m_file;
    int m_saved = -1;
};

}  // namespace

TEST_CASE("a value flag refuses a value that is itself a flag", "[servitor][client]") {
    std::vector<std::string> words = {"servitor", "--project", "--json", "status"};
    std::vector<char*> argv;
    for (auto& w : words)
        argv.push_back(w.data());
    Servitor::GlobalOptions opt;
    std::string error;
    CHECK_FALSE(Servitor::ParseGlobals(static_cast<int>(argv.size()), argv.data(), opt, &error));
    CHECK(error.find("--project") != std::string::npos);

    std::vector<std::string> negative = {"servitor", "--engine", "-x", "status"};   // a single '-' is a value
    argv.clear();
    for (auto& w : negative)
        argv.push_back(w.data());
    CHECK(Servitor::ParseGlobals(static_cast<int>(argv.size()), argv.data(), opt, &error));
    CHECK(opt.engine == "-x");
}

TEST_CASE("a leftover --flag after the verb is refused with usage exit 2", "[servitor][client]") {
    const std::vector<std::string> flagged = {"--bogus"};
    const auto problem = Servitor::LeftoverFlag(flagged);
    REQUIRE(problem.has_value());
    CHECK(problem->find("--bogus") != std::string::npos);
    const std::vector<std::string> negative = {"-1", "name=value"};
    CHECK_FALSE(Servitor::LeftoverFlag(negative).has_value());

    // --format human: the human usage stays on stderr (json/ndjson answer on stdout, Task 20).
    std::vector<std::string> words = {"servitor", "--format", "human", "status", "--bogus"};
    std::vector<char*> argv;
    for (auto& w : words)
        argv.push_back(w.data());
    const auto file = std::filesystem::temp_directory_path() /
                      ("servitor-usage-" + std::to_string(Arcane::EditorLock::Self().pid) + ".txt");
    int code = 0;
    std::string usage;
    {
        DiscoverStderrCapture capture(file);
        code = Servitor::Run(static_cast<int>(argv.size()), argv.data());
        usage = capture.Take();
    }
    std::filesystem::remove(file);
    CHECK(code == 2);
    CHECK(usage.find("servitor: unknown flag --bogus") != std::string::npos);
}

// Task 19 fix round 1 (Rule 3): Run refuses a host-scoped global on a verb
// that posts no call, before discovery, with usage exit 2.
TEST_CASE("Run refuses --world and --read-only on a verb that posts no call", "[servitor][client]") {
    const std::vector<std::pair<std::vector<std::string>, std::string>> cases = {
        {{"servitor", "--format", "human", "--world", "main", "status"}, "servitor: --world does not apply to status"},
        {{"servitor", "--format", "human", "list", "--read-only"}, "servitor: --read-only does not apply to list"},
    };
    for (const auto& [words, expected] : cases) {
        INFO(expected);
        std::vector<std::string> copy = words;
        std::vector<char*> argv;
        for (auto& w : copy)
            argv.push_back(w.data());
        const auto file = std::filesystem::temp_directory_path() /
                          ("servitor-unused-global-" + std::to_string(Arcane::EditorLock::Self().pid) + ".txt");
        int code = 0;
        std::string usage;
        {
            DiscoverStderrCapture capture(file);
            code = Servitor::Run(static_cast<int>(argv.size()), argv.data());
            usage = capture.Take();
        }
        std::filesystem::remove(file);
        CHECK(code == 2);
        CHECK(usage.find(expected) != std::string::npos);
    }
}
// ---- Final fix round 2 (I-1): --retry-on-death's rediscovery ---------------
//
// A resend may go only to the dead target's REPLACEMENT: a host of the same
// kind and project whose process did not exist when the call began (not in
// the call's opening scan; identity is {pid, start}, never the pid alone).
// With none, rediscovery answers at once, so the call reports host.died
// promptly instead of waiting out --timeout.
TEST_CASE("--retry-on-death retargets only to a new host of the dead target's kind and project",
          "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path proj = fs::temp_directory_path() / ("servitor-rediscover-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(proj);
    fs::create_directories(proj);
    { std::ofstream(proj / "Game.arcproj") << "{}"; }
    const auto self = Arcane::EditorLock::Self();
    const std::string project = Servitor::PathUtf8(proj);
    const auto write = [&](Arcane::Remote::HostKind kind, std::uint64_t start, const std::string& projectText) {
        Arcane::Remote::Descriptor d;
        d.hostKind = kind;
        d.pid = self.pid;
        d.processStart = start;
        d.port = 1;
        d.token = std::string(64, 'a');
        d.project = projectText;
        d.state = "ready";
        d.heartbeatUnixMs = Servitor::NowUnixMs();
        REQUIRE(Arcane::Remote::WriteDescriptorAtomic(Arcane::Remote::DescriptorPath(proj, kind, d.pid), d));
        return d;
    };
    // The target that died: a runtime whose {pid, start} is no longer live.
    Arcane::Remote::Descriptor dead;
    dead.hostKind = Arcane::Remote::HostKind::Runtime;
    dead.pid = self.pid;
    dead.processStart = self.start + 1;
    dead.project = project;
    Servitor::GlobalOptions opt;
    opt.project = project;
    opt.retryOnDeath = true;
    const auto targetFor = [&] {
        Servitor::LiveTarget t;
        t.descriptor = dead;
        t.watch.command = "cvar.set";
        t.timeoutMs = 30000;
        t.started = std::chrono::steady_clock::now();
        t.opt = &opt;
        return t;
    };
    const auto death = Arcane::Remote::MakeError("cvar.set", "host.died", "gone", "relaunch");

    {
        // Only another KIND is live (an editor): never a replacement. With no
        // candidate, rediscovery gives up at the call's remaining budget
        // (round 3, N-1), not later.
        write(Arcane::Remote::HostKind::Editor, self.start, project);
        auto t = targetFor();
        t.timeoutMs = 600;
        const auto began = std::chrono::steady_clock::now();
        CHECK_FALSE(Servitor::RediscoverTarget(&t, death));
        CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(3));
        CHECK(Servitor::SameHostProcess(t.descriptor, dead));   // still the dead target
    }
    {
        // A runtime of the same project whose pid equals the dead one's but
        // whose start differs: a different process (pid reuse), and new.
        const auto fresh = write(Arcane::Remote::HostKind::Runtime, self.start, project);
        CHECK(fresh.pid == dead.pid);
        CHECK_FALSE(Servitor::SameHostProcess(fresh, dead));
        auto t = targetFor();
        REQUIRE(Servitor::RediscoverTarget(&t, death));
        CHECK(t.descriptor.hostKind == Arcane::Remote::HostKind::Runtime);
        CHECK(t.descriptor.processStart == self.start);
        CHECK(Servitor::SameHostProcess(t.descriptor, fresh));
        // The same runtime, but live when the call began: never the target's
        // replacement (a resend would run the command on a host it already reached).
        auto pre = targetFor();
        pre.timeoutMs = 300;   // no eligible host: the poll ends at this budget (round 4, N3-2)
        pre.preexisting.emplace_back(fresh.pid, fresh.processStart);
        CHECK_FALSE(Servitor::RediscoverTarget(&pre, death));
    }
    {
        // Same kind, another project: not a replacement.
        write(Arcane::Remote::HostKind::Runtime, self.start, project + "-other");
        auto t = targetFor();
        t.timeoutMs = 400;
        CHECK_FALSE(Servitor::RediscoverTarget(&t, death));
    }
    fs::remove_all(proj);
}

// ---- Final fix round 3 (N-1, N-2) -----------------------------------------

// N-1: a restarting host writes its descriptor only once it serves, seconds
// after the old one died. Rediscovery polls (about every 250 ms) within the
// call's remaining --timeout, with round 2's strict filter, so the
// replacement that appears mid-poll gets the resend. Two eligible candidates
// are refused at once (never a guess).
TEST_CASE("--retry-on-death waits within --timeout for the replacement, and refuses two at once", "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path proj = fs::temp_directory_path() / ("servitor-repoll-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(proj);
    fs::create_directories(proj);
    { std::ofstream(proj / "Game.arcproj") << "{}"; }
    const auto self = Arcane::EditorLock::Self();
    const std::string project = Servitor::PathUtf8(proj);
    const auto descriptorFor = [&](Arcane::Remote::HostKind kind, std::uint32_t pid, std::uint64_t start) {
        Arcane::Remote::Descriptor d;
        d.hostKind = kind;
        d.pid = pid;
        d.processStart = start;
        d.port = 1;
        d.token = std::string(64, 'a');
        d.project = project;
        d.state = "ready";
        d.heartbeatUnixMs = Servitor::NowUnixMs();
        return d;
    };
    Arcane::Remote::Descriptor dead = descriptorFor(Arcane::Remote::HostKind::Runtime, self.pid, self.start + 1);
    Servitor::GlobalOptions opt;
    opt.project = project;
    opt.retryOnDeath = true;
    const auto death = Arcane::Remote::MakeError("wait.frames", "host.died", "gone", "relaunch");
    {
        Servitor::LiveTarget t;
        t.descriptor = dead;
        t.watch.command = "host.wait";
        t.timeoutMs = 10000;
        t.started = std::chrono::steady_clock::now();
        t.opt = &opt;
        // The replacement comes up 600 ms after rediscovery starts polling.
        std::thread relaunch([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            const auto fresh = descriptorFor(Arcane::Remote::HostKind::Runtime, self.pid, self.start);
            (void)Arcane::Remote::WriteDescriptorAtomic(
                Arcane::Remote::DescriptorPath(proj, Arcane::Remote::HostKind::Runtime, self.pid), fresh);
        });
        const auto began = std::chrono::steady_clock::now();
        const bool found = Servitor::RediscoverTarget(&t, death);
        const auto took = std::chrono::steady_clock::now() - began;
        relaunch.join();
        CHECK(found);
        CHECK(t.descriptor.processStart == self.start);
        CHECK(took >= std::chrono::milliseconds(500));
        CHECK(took < std::chrono::seconds(5));
    }
    {
        // Two eligible runtime candidates (the one above and a second file
        // naming a live process): refused at once, never a guess.
        const auto second = descriptorFor(Arcane::Remote::HostKind::Runtime, self.pid, self.start);
        REQUIRE(Arcane::Remote::WriteDescriptorAtomic(proj / "Saved" / "Remote" / "runtime-twin.json", second));
        Servitor::LiveTarget t;
        t.descriptor = dead;
        t.watch.command = "host.wait";
        t.timeoutMs = 30000;
        t.started = std::chrono::steady_clock::now();
        t.opt = &opt;
        const auto began = std::chrono::steady_clock::now();
        CHECK_FALSE(Servitor::RediscoverTarget(&t, death));
        CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
    }
    fs::remove_all(proj);
}

// N-2: the descriptor's `project` is the host's own spelling of its root. Two
// spellings of one root (separators, a trailing separator, Windows case, a
// "." segment) are the same project; another root is not.
TEST_CASE("project roots compare normalized, not as text", "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("servitor-roots-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::create_directories(root / "Game");
    const std::string plain = Servitor::PathUtf8(root / "Game");
    CHECK(Servitor::SameProjectRoot(plain, plain));
    CHECK(Servitor::SameProjectRoot(plain, plain + "/"));
    CHECK(Servitor::SameProjectRoot(plain, Servitor::PathUtf8(root / "." / "Game")));
    CHECK(Servitor::SameProjectRoot(plain, (root / "Game").make_preferred().string()));
#if defined(_WIN32)
    std::string upper = plain;
    for (char& c : upper)
        c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    CHECK(Servitor::SameProjectRoot(plain, upper));
#endif
    CHECK_FALSE(Servitor::SameProjectRoot(plain, Servitor::PathUtf8(root / "Other")));
    CHECK(Servitor::SameProjectRoot("", ""));           // the projectless editor's descriptors
    CHECK_FALSE(Servitor::SameProjectRoot("", plain));
    fs::remove_all(root);
}

#endif
