#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Http.hpp> // MakeToken
#include <Arcane/Base/Log.hpp>
#include <Arcane/Net/TcpSocket.hpp>
#include "RemoteTestHttp.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#if !defined(ARC_BUILD_DIST)

using RemoteTest::HttpExchange;
using RemoteTest::HttpExchangeHost;

TEST_CASE("the listen socket is bound to 127.0.0.1", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    REQUIRE(::getsockname(static_cast<SocketType>(listener.ListenSocket()),
                          reinterpret_cast<sockaddr*>(&bound), &len) == 0);
    char text[64] = {};
    inet_ntop(AF_INET, &bound.sin_addr, text, sizeof(text));
    CHECK(std::string(text) == "127.0.0.1");
    CHECK(listener.Port() == ntohs(bound.sin_port));
    CHECK(listener.Port() != 0);
    CHECK(listener.IdleClose() == std::chrono::seconds(Arcane::Remote::kIdleConnectionSeconds));
    CHECK(Arcane::Remote::kIdleConnectionSeconds == 30);
    listener.Stop();
}

TEST_CASE("status requires the bearer and answers from the atomic", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    snap->frame = 4;
    snap->hostKind = Arcane::Remote::HostKind::Runtime;
    atoms.status.store(snap);
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const std::string body = HttpExchange(listener.Port(), listener.Token(), "", "GET /remote/v1/status HTTP/1.1");
    CHECK(body.find("\"state\":\"ready\"") != std::string::npos);
    CHECK(body.find("\"frame\":4") != std::string::npos);
    CHECK(body.find(listener.Token()) == std::string::npos);
    const std::string denied = HttpExchange(listener.Port(), std::string(64, '0'), "", "GET /remote/v1/status HTTP/1.1");
    CHECK(denied.find("auth.denied") != std::string::npos);
    CHECK(denied.find("auth.rejected") == std::string::npos);
    auto env = Arcane::Remote::ParseEnvelope(nlohmann::json::parse(denied.substr(denied.find("\r\n\r\n") + 4)));
    CHECK(Arcane::Remote::ExitFromHost(env) == 3);
    listener.Stop();
}

TEST_CASE("a good token does not survive Origin", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    atoms.status.store(snap);
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const std::string origin = HttpExchange(listener.Port(), listener.Token(), "Origin: http://evil\r\n",
                                            "GET /remote/v1/status HTTP/1.1");
    CHECK(origin.find("403") != std::string::npos);
    CHECK(origin.find("auth.denied") != std::string::npos);
    CHECK(origin.find("\"state\":\"ready\"") == std::string::npos);
    CHECK(origin.find(listener.Token()) == std::string::npos);
    CHECK(queue.Depth() == 0);
    listener.Stop();
}

TEST_CASE("a forged Host is refused by the listener", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    atoms.status.store(snap);
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const std::string port = std::to_string(listener.Port());
    const std::string forged = HttpExchangeHost(listener.Port(), listener.Token(),
                                                "10.0.0.8:" + port, "",
                                                "GET /remote/v1/status HTTP/1.1");
    CHECK(forged.find("403") != std::string::npos);
    CHECK(forged.find("auth.denied") != std::string::npos);
    CHECK(forged.find("\"state\":\"ready\"") == std::string::npos);
    CHECK(forged.find("Origin") == std::string::npos);
    CHECK(queue.Depth() == 0);
    const std::string ok = HttpExchangeHost(listener.Port(), listener.Token(),
                                            "127.0.0.1:" + port, "",
                                            "GET /remote/v1/status HTTP/1.1");
    CHECK(ok.find("\"state\":\"ready\"") != std::string::npos);
    listener.Stop();
}

struct RemoteSeenPeer {
    sockaddr_in peer{};
    int calls = 0;
};

static bool RemoteRefusePeer(const sockaddr_in& peer, void* user) {
    auto* seen = static_cast<RemoteSeenPeer*>(user);
    seen->peer = peer;
    ++seen->calls;
    return false;
}

TEST_CASE("the accept loop closes a peer the predicate refuses", "[remote][listener]") {
    sockaddr_in forged{};
    forged.sin_family = AF_INET;
    inet_pton(AF_INET, "10.0.0.8", &forged.sin_addr);
    CHECK_FALSE(Arcane::Remote::PeerIsLoopback(forged));
    sockaddr_in loop{};
    loop.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &loop.sin_addr);
    CHECK(Arcane::Remote::PeerIsLoopback(loop));

    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    atoms.status.store(snap);
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    RemoteSeenPeer seen{};
    listener.SetPeerPredicate(&RemoteRefusePeer, &seen);
    listener.Start();
    REQUIRE(Arcane::InitSockets());
    const SocketType client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listener.Port());
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const std::string req = "GET /remote/v1/status HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(listener.Port()) + "\r\nAuthorization: Bearer " + listener.Token() + "\r\n\r\n";
    REQUIRE(static_cast<int>(::send(client, req.data(), static_cast<int>(req.size()), 0)) == static_cast<int>(req.size()));
    Arcane::SetSocketTimeout(client, 2000);
    char buf[512];
    const int n = static_cast<int>(::recv(client, buf, sizeof(buf), 0));
    std::string raw = n > 0 ? std::string(buf, buf + n) : std::string();
    CHECK(seen.calls == 1);
    char text[64] = {};
    inet_ntop(AF_INET, &seen.peer.sin_addr, text, sizeof(text));
    CHECK(std::string(text) == "127.0.0.1");
    // FIN (0) or, when the request bytes were already queued on the refused
    // socket at close, a reset (-1). Never response bytes.
    CHECK(n <= 0);
    CHECK(raw.find("200") == std::string::npos);
    CHECK(raw.find("ready") == std::string::npos);
    CHECK(queue.Depth() == 0);
    CloseSocket(client);
    listener.Stop();
}

TEST_CASE("an idle connection is closed and the production limit is 30s", "[remote][listener]") {
    CHECK(Arcane::Remote::kIdleConnectionSeconds == 30);
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken(), std::chrono::milliseconds(200));
    CHECK(listener.IdleClose() == std::chrono::milliseconds(200));
    listener.Start();
    const SocketType client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listener.Port());
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    Arcane::SetSocketTimeout(client, 2000);
    char buf[8];
    const int n = static_cast<int>(::recv(client, buf, sizeof(buf), 0));
    CHECK(n == 0);
    CHECK(queue.Depth() == 0);
    CloseSocket(client);
    listener.Stop();
}

TEST_CASE("POST /remote/v1/call reads command and the 65th is queue-full", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue(64);
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    atoms.catalog.store(std::make_shared<const nlohmann::json>(nlohmann::json::object()));
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    for (std::uint64_t i = 1; i <= 64; ++i) {
        Arcane::Remote::Call call;
        call.id = std::to_string(i);
        call.command = "demo.look";
        REQUIRE(queue.TryPush(i, call));
    }
    const std::string payload = R"({"id":"r-65","command":"demo.look","args":{}})";
    const std::string body = HttpExchange(listener.Port(), listener.Token(), "",
                                          "POST /remote/v1/call HTTP/1.1", payload);
    CHECK(body.find("\"code\":\"host.busy\"") != std::string::npos);
    CHECK(body.find("queue-full") != std::string::npos);
    CHECK(body.find("\"command\"") != std::string::npos);
    CHECK(queue.Depth() == 64);
    const auto env = Arcane::Remote::ParseEnvelope(nlohmann::json::parse(body.substr(body.find("\r\n\r\n") + 4)));
    CHECK(Arcane::Remote::ExitFromHost(env) == 5);
    const std::string catalog = HttpExchange(listener.Port(), listener.Token(), "", "GET /remote/v1/catalog HTTP/1.1");
    CHECK(catalog.find("200") != std::string::npos);
    listener.Stop();
}

// The subscriber runs on the listener's logging thread; the test thread reads
// the text only after listener.Stop() and Unsubscribe, under the mutex (ruling F29).
struct RemoteSpill {
    std::mutex mu;
    std::string text;
};

static void RemoteSpillSink(const Arcane::LogRecord& rec, void* user) {
    auto* spill = static_cast<RemoteSpill*>(user);
    std::lock_guard<std::mutex> lock(spill->mu);
    spill->text += rec.text;
    spill->text.push_back('\n');
}

TEST_CASE("a refused request does not log the token", "[remote][listener]") {
    RemoteSpill spill;
    const auto sub = Arcane::Log::Subscribe(&RemoteSpillSink, &spill);
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const std::string forged(64, 'b'); // the bearer this client actually sends
    const std::string denied = HttpExchange(listener.Port(), forged, "", "GET /remote/v1/status HTTP/1.1");
    CHECK(denied.find("auth.denied") != std::string::npos);
    const std::string realToken = listener.Token();
    listener.Stop();
    Arcane::Log::Unsubscribe(sub);
    std::string seen;
    {
        std::lock_guard<std::mutex> lock(spill.mu);
        seen = spill.text;
    }
    CHECK(seen.find(forged) == std::string::npos);    // the refused bearer is never logged
    CHECK(seen.find(realToken) == std::string::npos); // nor is the listener's own token
    // Not vacuous: the subscriber saw the listener's refusal line.
    CHECK(seen.find("remote: refused (auth): auth.denied") != std::string::npos);
}

TEST_CASE("PollSockets reports one readable socket and times out the idle one", "[remote][listener]") {
    REQUIRE(Arcane::InitSockets());
    const SocketType listen = Arcane::CreateListenSocket(0, "127.0.0.1");
    REQUIRE(listen != INVALID_SOCK);
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    REQUIRE(::getsockname(listen, reinterpret_cast<sockaddr*>(&bound), &len) == 0);

    Arcane::PollInterest before[1];
    before[0].socket = listen;
    before[0].wantRead = true;
    CHECK(Arcane::PollSockets(before, 0) == 0);
    CHECK_FALSE(before[0].readable);

    const SocketType client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(client != INVALID_SOCK);
    REQUIRE(::connect(client, reinterpret_cast<sockaddr*>(&bound), sizeof(bound)) == 0);
    Arcane::PollInterest listening[1];
    listening[0].socket = listen;
    listening[0].wantRead = true;
    CHECK(Arcane::PollSockets(listening, 200) == 1);
    CHECK(listening[0].readable);

    sockaddr_in peerAddr{};
    socklen_t peerLen = sizeof(peerAddr);
    const SocketType accepted = ::accept(listen, reinterpret_cast<sockaddr*>(&peerAddr), &peerLen);
    REQUIRE(accepted != INVALID_SOCK);
    REQUIRE(Arcane::SetNonBlocking(accepted, true));
    REQUIRE(Arcane::SetNonBlocking(client, true));
    const char byte = 'Q';
    REQUIRE(static_cast<int>(::send(client, &byte, 1, 0)) == 1);

    const SocketType idleListen = Arcane::CreateListenSocket(0, "127.0.0.1");
    REQUIRE(idleListen != INVALID_SOCK);
    sockaddr_in idleBound{};
    socklen_t idleLen = sizeof(idleBound);
    REQUIRE(::getsockname(idleListen, reinterpret_cast<sockaddr*>(&idleBound), &idleLen) == 0);
    const SocketType idleClient = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(::connect(idleClient, reinterpret_cast<sockaddr*>(&idleBound), sizeof(idleBound)) == 0);
    sockaddr_in idlePeer{};
    socklen_t idlePeerLen = sizeof(idlePeer);
    const SocketType idleAccepted = ::accept(idleListen, reinterpret_cast<sockaddr*>(&idlePeer), &idlePeerLen);
    REQUIRE(idleAccepted != INVALID_SOCK);
    REQUIRE(Arcane::SetNonBlocking(idleAccepted, true));

    Arcane::PollInterest both[2];
    both[0].socket = accepted;
    both[0].wantRead = true;
    both[1].socket = idleAccepted;
    both[1].wantRead = true;
    const int ready = Arcane::PollSockets(both, 200);
    CHECK(ready >= 1);
    CHECK(both[0].readable);
    CHECK_FALSE(both[1].readable);
    char got = 0;
    REQUIRE(static_cast<int>(::recv(accepted, &got, 1, 0)) == 1);
    CHECK(got == 'Q');
    char extra = 0;
    const int again = static_cast<int>(::recv(accepted, &extra, 1, 0));
    CHECK(again < 0);
#ifdef _WIN32
    CHECK(WSAGetLastError() == WSAEWOULDBLOCK);
#else
    const int err = errno;
    CHECK((err == EAGAIN || err == EWOULDBLOCK));
#endif

    Arcane::PollInterest idleOnly[1];
    idleOnly[0].socket = idleAccepted;
    idleOnly[0].wantRead = true;
    const auto t0 = std::chrono::steady_clock::now();
    const int idleRc = Arcane::PollSockets(idleOnly, 50);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(idleRc == 0);
    CHECK_FALSE(idleOnly[0].readable);
    CHECK(ms >= 40);
    CHECK(ms < 500);

    CloseSocket(client);
    CloseSocket(accepted);
    CloseSocket(listen);
    CloseSocket(idleClient);
    CloseSocket(idleAccepted);
    CloseSocket(idleListen);
}

TEST_CASE("a parked call does not stop status", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    atoms.status.store(snap);
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    REQUIRE(Arcane::InitSockets());
    const SocketType caller = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listener.Port());
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(::connect(caller, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const std::string payload = R"({"id":"r-park","command":"demo.look","args":{}})";
    const std::string req = "POST /remote/v1/call HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(listener.Port()) + "\r\nAuthorization: Bearer " + listener.Token() +
        "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(payload.size()) +
        "\r\n\r\n" + payload;
    REQUIRE(static_cast<int>(::send(caller, req.data(), static_cast<int>(req.size()), 0)) == static_cast<int>(req.size()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (queue.Depth() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(queue.Depth() == 1);
    const std::string status = HttpExchange(listener.Port(), listener.Token(), "",
                                            "GET /remote/v1/status HTTP/1.1");
    CHECK(status.find("200") != std::string::npos);
    CHECK(status.find("\"state\":\"ready\"") != std::string::npos);
    CHECK(queue.Depth() == 1);
    Arcane::Remote::CommandResult done;
    done.envelope = Arcane::Remote::MakeOk("demo.look", nlohmann::json::object(), {});
    const std::uint64_t parked = queue.OldestPendingId();
    REQUIRE(parked != 0);
    queue.Complete(parked, done);
    Arcane::SetSocketTimeout(caller, 2000);
    std::string raw;
    char buf[4096];
    const auto readDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (raw.find("demo.look") == std::string::npos && std::chrono::steady_clock::now() < readDeadline) {
        const int n = static_cast<int>(::recv(caller, buf, sizeof(buf), 0));
        if (n > 0) raw.append(buf, buf + n);
        else break;
    }
    CHECK(raw.find("200") != std::string::npos);
    CHECK(raw.find("demo.look") != std::string::npos);
    CloseSocket(caller);
    listener.Stop();
}

// Sends a raw POST /remote/v1/call on its own connection and returns the socket
// (the call parks, so no response is read here).
static SocketType RemotePostCall(std::uint16_t port, const std::string& token, const std::string& payload) {
    const SocketType sock = RemoteTest::ConnectLoopback(port);
    RemoteTest::SendAll(sock, "POST /remote/v1/call HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
        "\r\nAuthorization: Bearer " + token + "\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(payload.size()) + "\r\n\r\n" + payload);
    return sock;
}

static bool RemoteWaitDepth(const Arcane::Remote::RequestQueue& queue, std::size_t depth) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (queue.Depth() != depth && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return queue.Depth() == depth;
}

static int RemoteExitOf(const std::string& raw) {
    const auto at = raw.find("\r\n\r\n");
    REQUIRE(at != std::string::npos);
    return Arcane::Remote::ExitFromHost(Arcane::Remote::ParseEnvelope(nlohmann::json::parse(raw.substr(at + 4))));
}

TEST_CASE("POST /remote/v1/call copies options onto the Call and refuses bad bodies unqueued", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const std::uint16_t port = listener.Port();
    const std::string& token = listener.Token();
    const std::string call = "POST /remote/v1/call HTTP/1.1";

    const std::string unknown = HttpExchange(port, token, "", call, R"({"command":"demo.look","bogus":1})");
    CHECK(unknown.find("\"code\":\"arg.unknown\"") != std::string::npos);
    CHECK(RemoteExitOf(unknown) == 2);
    const std::string missing = HttpExchange(port, token, "", call, R"({"id":"r-1","args":{}})");
    CHECK(missing.find("\"code\":\"arg.missing\"") != std::string::npos);
    CHECK(RemoteExitOf(missing) == 2);
    const std::string mismatch = HttpExchange(port, token, "", call, R"({"protocol":2,"command":"demo.look"})");
    CHECK(mismatch.find("\"code\":\"protocol.mismatch\"") != std::string::npos);
    CHECK(RemoteExitOf(mismatch) == 4);
    const std::string batch = HttpExchange(port, token, "", "POST /remote/v1/batch HTTP/1.1", R"({"calls":[]})");
    CHECK(batch.find("\"code\":\"command.unknown\"") != std::string::npos);
    CHECK(RemoteExitOf(batch) == 2);
    CHECK(queue.Depth() == 0);

    const SocketType sock = RemotePostCall(port, token,
        R"({"id":"r-7","command":"demo.look","args":{"n":3},"protocol":1,)"
        R"("options":{"world":"server","readOnly":true,"timeoutMs":1500}})");
    REQUIRE(RemoteWaitDepth(queue, 1));
    std::uint64_t key = 0;
    Arcane::Remote::Call popped;
    REQUIRE(queue.TryPop(key, popped));
    CHECK(key != 0);
    CHECK(popped.id == "r-7");
    CHECK(popped.command == "demo.look");
    CHECK(popped.args == nlohmann::json{{"n", 3}});
    CHECK(popped.world == "server");
    CHECK(popped.readOnly);
    CHECK(popped.timeoutMs == 1500u);
    Arcane::Remote::CommandResult done;
    done.envelope = Arcane::Remote::MakeOk("demo.look", nlohmann::json::object(), {});
    done.envelope.id = popped.id;
    queue.Complete(key, done);
    const std::string answered = RemoteTest::RecvUntil(sock, "\"r-7\"", std::chrono::seconds(2));
    CHECK(answered.find("HTTP/1.1 200") != std::string::npos);
    CHECK(answered.find("X-Remote-Protocol: 1") != std::string::npos);
    CloseSocket(sock);

    const SocketType plain = RemotePostCall(port, token, R"({"command":"demo.look"})");
    REQUIRE(RemoteWaitDepth(queue, 1));
    REQUIRE(queue.TryPop(key, popped));
    CHECK(popped.timeoutMs == 30000u);
    CHECK(popped.world.empty());
    CHECK_FALSE(popped.readOnly);
    queue.Complete(key, done);
    CloseSocket(plain);
    listener.Stop();
}

// Rev 8.3 V6: new request fields arrive inside `options`, so an older host
// ignores an options key it does not know; the call is still queued and runs.
TEST_CASE("listener ignores unknown options keys", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const SocketType sock = RemotePostCall(listener.Port(), listener.Token(),
        R"({"id":"r-future","command":"demo.look","options":{"futureFlag":true,"timeoutMs":2500}})");
    REQUIRE(RemoteWaitDepth(queue, 1));
    std::uint64_t key = 0;
    Arcane::Remote::Call popped;
    REQUIRE(queue.TryPop(key, popped));
    CHECK(popped.id == "r-future");
    CHECK(popped.command == "demo.look");
    CHECK(popped.timeoutMs == 2500u); // the known key beside it is still copied
    Arcane::Remote::CommandResult done;
    done.envelope = Arcane::Remote::MakeOk("demo.look", nlohmann::json{{"ran", true}}, {});
    done.envelope.id = popped.id;
    queue.Complete(key, done);
    const std::string answered = RemoteTest::RecvUntil(sock, "\"ran\":true", std::chrono::seconds(2));
    CHECK(answered.find("HTTP/1.1 200") != std::string::npos);
    CHECK(answered.find("\"r-future\"") != std::string::npos);
    CHECK(answered.find("arg.unknown") == std::string::npos);
    CHECK(RemoteExitOf(answered) == 0);
    CloseSocket(sock);
    // Unknown TOP-LEVEL keys stay refusals.
    const std::string topLevel = HttpExchange(listener.Port(), listener.Token(), "", "POST /remote/v1/call HTTP/1.1",
                                              R"({"command":"demo.look","futureFlag":true})");
    CHECK(topLevel.find("\"code\":\"arg.unknown\"") != std::string::npos);
    CHECK(RemoteExitOf(topLevel) == 2);
    CHECK(queue.Depth() == 0);
    listener.Stop();
}

TEST_CASE("the listener closes the connection after each response", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    atoms.status.store(snap);
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    // Two requests in one write: the second is never answered (no pipelining,
    // no keep-alive), and the listener closes after the first response.
    const std::string one = "GET /remote/v1/status HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(listener.Port()) +
        "\r\nAuthorization: Bearer " + listener.Token() + "\r\n\r\n";
    const SocketType sock = RemoteTest::ConnectLoopback(listener.Port());
    RemoteTest::SendAll(sock, one + one);
    Arcane::SetSocketTimeout(sock, 2000);
    std::string raw;
    char buf[4096];
    int last = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        last = static_cast<int>(::recv(sock, buf, sizeof(buf), 0));
        if (last <= 0) break;
        raw.append(buf, buf + last);
    }
    CHECK(last == 0); // an orderly close, not a timeout or a reset
    CHECK(raw.find("HTTP/1.1 200") != std::string::npos);
    CHECK(raw.find("Connection: close") != std::string::npos);
    CHECK(raw.find("HTTP/1.1", raw.find("HTTP/1.1") + 1) == std::string::npos);
    CloseSocket(sock);
    listener.Stop();
}

TEST_CASE("auth failures past the budget answer auth.throttled; Host refusals do not spend it", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    atoms.status.store(snap);
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const std::uint16_t port = listener.Port();
    const std::string status = "GET /remote/v1/status HTTP/1.1";
    const std::string bad(64, 'c');
    // 25 forged-Host refusals (a valid bearer) and 25 Origin refusals spend nothing.
    for (int i = 0; i < 25; ++i) {
        const std::string forged = HttpExchangeHost(port, listener.Token(), "10.0.0.8:" + std::to_string(port), "", status);
        REQUIRE(forged.find("403") != std::string::npos);
        const std::string origin = HttpExchange(port, listener.Token(), "Origin: http://evil\r\n", status);
        REQUIRE(origin.find("403") != std::string::npos);
    }
    // The budget is 20 attempts per 10 s: the first 20 bad bearers are 401.
    for (int i = 0; i < 20; ++i) {
        const std::string denied = HttpExchange(port, bad, "", status);
        REQUIRE(denied.find("HTTP/1.1 401") != std::string::npos);
        REQUIRE(denied.find("\"code\":\"auth.denied\"") != std::string::npos);
    }
    std::string throttled;
    for (int i = 0; i < 5 && throttled.find("HTTP/1.1 429") == std::string::npos; ++i)
        throttled = HttpExchange(port, bad, "", status);
    CHECK(throttled.find("HTTP/1.1 429") != std::string::npos);
    CHECK(throttled.find("\"code\":\"auth.throttled\"") != std::string::npos);
    CHECK(throttled.find(bad) == std::string::npos);
    CHECK(throttled.find(listener.Token()) == std::string::npos);
    CHECK(RemoteExitOf(throttled) == 3);
    // The right bearer never consults the limiter.
    const std::string ok = HttpExchange(port, listener.Token(), "", status);
    CHECK(ok.find("\"state\":\"ready\"") != std::string::npos);
    listener.Stop();
}

TEST_CASE("the 17th connection is answered host.busy connections and closed", "[remote][listener]") {
    CHECK(Arcane::Remote::kMaxConnections == 16);
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    std::vector<SocketType> held;
    for (std::uint32_t i = 0; i < Arcane::Remote::kMaxConnections; ++i)
        held.push_back(RemoteTest::ConnectLoopback(listener.Port()));
    // The accept loop takes connections in arrival order, so the 17th is this one.
    const SocketType extra = RemoteTest::ConnectLoopback(listener.Port());
    // No marker: read until the listener closes the connection.
    const std::string busy = RemoteTest::RecvUntil(extra, "\x01", std::chrono::seconds(2));
    CHECK(busy.find("\"code\":\"host.busy\"") != std::string::npos);
    CHECK(busy.find("\"state\":\"connections\"") != std::string::npos);
    CHECK(RemoteExitOf(busy) == 5);
    char byte = 0;
    Arcane::SetSocketTimeout(extra, 2000);
    CHECK(static_cast<int>(::recv(extra, &byte, 1, 0)) == 0); // then closed
    CloseSocket(extra);
    // Closing one held connection frees a slot, and a NEW connection gets it.
    CloseSocket(held.back());
    held.pop_back();
    std::string ok;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ok.find("HTTP/1.1 200") == std::string::npos && std::chrono::steady_clock::now() < deadline)
        ok = HttpExchange(listener.Port(), listener.Token(), "", "GET /remote/v1/status HTTP/1.1");
    CHECK(ok.find("HTTP/1.1 200") != std::string::npos);
    for (const SocketType s : held)
        CloseSocket(s);
    listener.Stop();
}

TEST_CASE("Stop answers a parked call host.died", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const SocketType sock = RemotePostCall(listener.Port(), listener.Token(), R"({"command":"demo.look"})");
    REQUIRE(RemoteWaitDepth(queue, 1));
    listener.Stop();
    const std::string raw = RemoteTest::RecvUntil(sock, "host.died", std::chrono::seconds(2));
    CHECK(raw.find("\"code\":\"host.died\"") != std::string::npos);
    CHECK(raw.find("\"command\":\"demo.look\"") != std::string::npos);   // stamped from the parked call
    CHECK(RemoteExitOf(raw) == 7);
    CHECK(queue.Depth() == 0); // a completed call is never run
    CHECK(queue.OldestPendingId() == 0);
    CloseSocket(sock);
}

static std::vector<Arcane::Remote::SseEvent> RemoteWaitSse(Arcane::Remote::ListenerAtoms& atoms, Arcane::Remote::SseLife life) {
    std::vector<Arcane::Remote::SseEvent> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& e : Arcane::Remote::TakeSseEvents(atoms))
            seen.push_back(std::move(e));
        for (const auto& e : seen)
            if (e.life == life) return seen;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return seen;
}

TEST_CASE("an SSE stream opens, carries EnqueueSse frames, and reports its close", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const std::uint16_t port = listener.Port();

    const std::string watch = HttpExchange(port, listener.Token(), "", "GET /remote/v1/stream?topics=watch:1 HTTP/1.1");
    CHECK(watch.find("\"code\":\"arg.inapplicable\"") != std::string::npos);
    CHECK(RemoteExitOf(watch) == 2);
    const std::string unknownTopic = HttpExchange(port, listener.Token(), "", "GET /remote/v1/stream?topics=bogus HTTP/1.1");
    CHECK(unknownTopic.find("\"code\":\"arg.invalid\"") != std::string::npos);
    CHECK(RemoteExitOf(unknownTopic) == 2);
    CHECK(Arcane::Remote::TakeSseEvents(atoms).empty()); // a refused stream never opened

    const SocketType client = RemoteTest::ConnectLoopback(port);
    RemoteTest::SendAll(client, "GET /remote/v1/stream?topics=logs HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(port) + "\r\nAuthorization: Bearer " + listener.Token() + "\r\n\r\n");
    const std::string headers = RemoteTest::RecvUntil(client, "\r\n\r\n", std::chrono::seconds(2));
    CHECK(headers.find("HTTP/1.1 200") != std::string::npos);
    CHECK(headers.find("text/event-stream") != std::string::npos);
    CHECK(headers.find("Content-Length") == std::string::npos);
    const auto opened = RemoteWaitSse(atoms, Arcane::Remote::SseLife::Opened);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].topic == "logs");
    const std::uint64_t sub = opened[0].subscriberId;
    CHECK(sub != 0);

    REQUIRE(listener.EnqueueSse(sub, "remote-sse-frame"));
    const std::string frame = RemoteTest::RecvUntil(client, "remote-sse-frame\n\n", std::chrono::seconds(2));
    CHECK(frame.find("data: remote-sse-frame\n\n") != std::string::npos);

    CloseSocket(client);
    const auto closed = RemoteWaitSse(atoms, Arcane::Remote::SseLife::Closed);
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].subscriberId == sub);
    CHECK(closed[0].topic == "logs");
    CHECK_FALSE(listener.EnqueueSse(sub, "after-close"));
    listener.Stop();
}

TEST_CASE("the 9th SSE subscriber is answered host.busy and not held open", "[remote][listener]") {
    CHECK(Arcane::Remote::kMaxSseSubscribers == 8);
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const std::string streamReq = "GET /remote/v1/stream?topics=state HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(listener.Port()) + "\r\nAuthorization: Bearer " + listener.Token() + "\r\n\r\n";
    std::vector<SocketType> streams;
    for (std::uint32_t i = 0; i < Arcane::Remote::kMaxSseSubscribers; ++i) {
        const SocketType s = RemoteTest::ConnectLoopback(listener.Port());
        RemoteTest::SendAll(s, streamReq);
        const std::string headers = RemoteTest::RecvUntil(s, "\r\n\r\n", std::chrono::seconds(2));
        REQUIRE(headers.find("text/event-stream") != std::string::npos);
        streams.push_back(s);
    }
    const std::string ninth = HttpExchange(listener.Port(), listener.Token(), "",
                                           "GET /remote/v1/stream?topics=state HTTP/1.1");
    CHECK(ninth.find("text/event-stream") == std::string::npos);
    CHECK(ninth.find("\"code\":\"host.busy\"") != std::string::npos);
    CHECK(ninth.find("\"state\":\"connections\"") != std::string::npos);
    CHECK(RemoteExitOf(ninth) == 5);
    std::size_t opened = 0;
    for (const auto& e : Arcane::Remote::TakeSseEvents(atoms))
        if (e.life == Arcane::Remote::SseLife::Opened) ++opened;
    CHECK(opened == Arcane::Remote::kMaxSseSubscribers);
    for (const SocketType s : streams)
        CloseSocket(s);
    listener.Stop();
}

TEST_CASE("Stop closes open SSE streams and reports them Closed", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const SocketType client = RemoteTest::ConnectLoopback(listener.Port());
    RemoteTest::SendAll(client, "GET /remote/v1/stream?topics=job:7 HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(listener.Port()) + "\r\nAuthorization: Bearer " + listener.Token() + "\r\n\r\n");
    REQUIRE(RemoteTest::RecvUntil(client, "\r\n\r\n", std::chrono::seconds(2)).find("200") != std::string::npos);
    const auto opened = RemoteWaitSse(atoms, Arcane::Remote::SseLife::Opened);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].topic == "job:7");
    listener.Stop();
    const auto closed = Arcane::Remote::TakeSseEvents(atoms);
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].life == Arcane::Remote::SseLife::Closed);
    CHECK(closed[0].subscriberId == opened[0].subscriberId);
    CHECK_FALSE(listener.EnqueueSse(opened[0].subscriberId, "after-stop"));
    char byte = 0;
    Arcane::SetSocketTimeout(client, 2000);
    CHECK(static_cast<int>(::recv(client, &byte, 1, 0)) == 0);
    CloseSocket(client);
}

static void RemoteShutdownSend(SocketType sock) {
#ifdef _WIN32
    ::shutdown(sock, SD_SEND);
#else
    ::shutdown(sock, SHUT_WR);
#endif
}

static std::string RemoteStreamRequest(std::uint16_t port, const std::string& token, const std::string& topic) {
    return "GET /remote/v1/stream?topics=" + topic + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
        "\r\nAuthorization: Bearer " + token + "\r\n\r\n";
}

TEST_CASE("EnqueueSse racing Stop is clean", "[remote][listener]") {
    // Several rounds: a thread spams EnqueueSse while this thread stops the
    // listener (the wake write end is closed under the same lock Poke takes).
    for (int round = 0; round < 8; ++round) {
        Arcane::Remote::RequestQueue queue;
        Arcane::Remote::ListenerAtoms atoms;
        atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
        Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
        listener.Start();
        const SocketType client = RemoteTest::ConnectLoopback(listener.Port());
        RemoteTest::SendAll(client, RemoteStreamRequest(listener.Port(), listener.Token(), "logs"));
        REQUIRE(RemoteTest::RecvUntil(client, "\r\n\r\n", std::chrono::seconds(2)).find("200") != std::string::npos);
        const auto opened = RemoteWaitSse(atoms, Arcane::Remote::SseLife::Opened);
        REQUIRE(opened.size() == 1);
        const std::uint64_t sub = opened[0].subscriberId;
        std::atomic<bool> done{ false };
        std::atomic<int> accepted{ 0 };
        std::thread spam([&] {
            while (!done.load(std::memory_order_acquire))
                if (listener.EnqueueSse(sub, "spam"))
                    accepted.fetch_add(1, std::memory_order_relaxed);
        });
        while (accepted.load(std::memory_order_relaxed) == 0)
            std::this_thread::yield();
        listener.Stop();
        done.store(true, std::memory_order_release);
        spam.join();
        CHECK(accepted.load() > 0);
        CHECK_FALSE(listener.EnqueueSse(sub, "after-stop"));
        CloseSocket(client);
    }
}

TEST_CASE("Stop answers an orphaned queued call so it never runs", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const SocketType sock = RemotePostCall(listener.Port(), listener.Token(), R"({"command":"demo.look"})");
    REQUIRE(RemoteWaitDepth(queue, 1));
    const std::uint64_t key = queue.OldestPendingId();
    REQUIRE(key != 0);
    CloseSocket(sock);
    // Give the poll loop several iterations to see the drop (the call becomes an orphan).
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(queue.Depth() == 1); // the drop alone does not unqueue it
    listener.Stop();
    std::uint64_t popped = 0;
    Arcane::Remote::Call call;
    CHECK_FALSE(queue.TryPop(popped, call)); // never runs
    CHECK(queue.Depth() == 0);
    CHECK(queue.OldestPendingId() == 0);
    Arcane::Remote::CommandResult left;
    CHECK_FALSE(queue.TryTake(key, left)); // no result left behind
}

TEST_CASE("a client that drops while parked leaves no result behind", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const SocketType sock = RemotePostCall(listener.Port(), listener.Token(), R"({"command":"demo.look"})");
    REQUIRE(RemoteWaitDepth(queue, 1));
    std::uint64_t key = 0;
    Arcane::Remote::Call call;
    REQUIRE(queue.TryPop(key, call));
    CloseSocket(sock);
    // Let the loop see the drop first, so the Complete below finds no Conn.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Arcane::Remote::CommandResult done;
    done.envelope = Arcane::Remote::MakeOk("demo.look", nlohmann::json::object(), {});
    queue.Complete(key, done);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Arcane::Remote::CommandResult left;
    CHECK_FALSE(queue.TryTake(key, left)); // the listener discarded it
    CHECK(queue.OldestPendingId() == 0);
    listener.Stop();
}

TEST_CASE("a half-closed client still gets its response", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    atoms.status.store(snap);
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    const SocketType status = RemoteTest::ConnectLoopback(listener.Port());
    RemoteTest::SendAll(status, "GET /remote/v1/status HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(listener.Port()) +
        "\r\nAuthorization: Bearer " + listener.Token() + "\r\n\r\n");
    RemoteShutdownSend(status);
    const std::string answered = RemoteTest::RecvUntil(status, "\"state\":\"ready\"", std::chrono::seconds(2));
    CHECK(answered.find("HTTP/1.1 200") != std::string::npos);
    CHECK(answered.find("\"state\":\"ready\"") != std::string::npos);
    CloseSocket(status);

    // A half-closed /call is still read and queued (its bytes were complete),
    // but EOF on a parked call is the client drop of the brief whichever read
    // sees it: the connection closes with no answer, and the later result is
    // discarded. Deterministic whether the FIN arrives with the request or after.
    const SocketType sock = RemotePostCall(listener.Port(), listener.Token(), R"({"id":"r-half","command":"demo.look"})");
    RemoteShutdownSend(sock);
    REQUIRE(RemoteWaitDepth(queue, 1));
    std::uint64_t key = 0;
    Arcane::Remote::Call call;
    REQUIRE(queue.TryPop(key, call));
    CHECK(call.id == "r-half");
    const std::string closed = RemoteTest::RecvUntil(sock, "\x01", std::chrono::seconds(2));
    CHECK(closed.empty()); // closed, no bytes
    Arcane::Remote::CommandResult done;
    done.envelope = Arcane::Remote::MakeOk("demo.look", nlohmann::json::object(), {});
    queue.Complete(key, done);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Arcane::Remote::CommandResult left;
    CHECK_FALSE(queue.TryTake(key, left));
    CloseSocket(sock);
    listener.Stop();
}

TEST_CASE("parked and streaming connections are not idle-closed", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken(), std::chrono::milliseconds(200));
    listener.Start();
    const SocketType stream = RemoteTest::ConnectLoopback(listener.Port());
    RemoteTest::SendAll(stream, RemoteStreamRequest(listener.Port(), listener.Token(), "state"));
    REQUIRE(RemoteTest::RecvUntil(stream, "\r\n\r\n", std::chrono::seconds(2)).find("200") != std::string::npos);
    const auto opened = RemoteWaitSse(atoms, Arcane::Remote::SseLife::Opened);
    REQUIRE(opened.size() == 1);
    const SocketType parked = RemotePostCall(listener.Port(), listener.Token(), R"({"id":"r-slow","command":"demo.look"})");
    REQUIRE(RemoteWaitDepth(queue, 1));
    std::uint64_t key = 0;
    Arcane::Remote::Call call;
    REQUIRE(queue.TryPop(key, call));
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 2.5x the idle limit

    CHECK(Arcane::Remote::TakeSseEvents(atoms).empty()); // the stream did not close
    REQUIRE(listener.EnqueueSse(opened[0].subscriberId, "still-open"));
    CHECK(RemoteTest::RecvUntil(stream, "still-open\n\n", std::chrono::seconds(2)).find("data: still-open") !=
          std::string::npos);
    Arcane::Remote::CommandResult done;
    done.envelope = Arcane::Remote::MakeOk("demo.look", nlohmann::json::object(), {});
    done.envelope.id = call.id;
    queue.Complete(key, done);
    const std::string reply = RemoteTest::RecvUntil(parked, "\"r-slow\"", std::chrono::seconds(2));
    CHECK(reply.find("HTTP/1.1 200") != std::string::npos);
    CHECK(reply.find("\"r-slow\"") != std::string::npos);
    CloseSocket(parked);
    CloseSocket(stream);
    listener.Stop();
}

#endif
