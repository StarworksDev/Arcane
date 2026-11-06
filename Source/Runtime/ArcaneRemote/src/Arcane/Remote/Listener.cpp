// RateLimiter.hpp first: its Logger.hpp defines SPDLOG_ACTIVE_LEVEL, which must
// precede the first spdlog include, and Listener.hpp's own includes reach spdlog.
#include <Arcane/Net/RateLimiter.hpp>
#include <Arcane/Remote/Listener.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Http.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ServiceThread.hpp>
#include <Arcane/Core/Constant.hpp>

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

namespace Arcane::Remote
{

namespace
{

ARC_CONSTANT("timing: the listener's poll wait (ms); Stop and the idle clock are noticed within it without a socket event")
constexpr int kPollWaitMs = 50;
ARC_CONSTANT("security: auth failures per window before auth.throttled (Servitor P0 Task 6)")
constexpr int kAuthMaxAttempts = 20;
ARC_CONSTANT("security: the auth-failure window, seconds (Servitor P0 Task 6)")
constexpr int kAuthWindowSeconds = 10;
ARC_CONSTANT("security: the auth.throttled cooldown, seconds (Servitor P0 Task 6)")
constexpr int kAuthCooldownSeconds = 10;
ARC_CONSTANT("timing: after its response a connection waits this long (ms) for the peer's FIN, so a close never resets unread bytes")
constexpr int kLingerMs = 2000;
ARC_CONSTANT("timing: Stop keeps flushing queued answers (host.died) this long (ms) before it closes the sockets")
constexpr int kStopFlushMs = 500;
ARC_CONSTANT("timing: one poll slice (ms) of Stop's flush")
constexpr int kStopFlushSliceMs = 10;
ARC_CONSTANT("timing: back-off (ms) after a hard accept or poll error, so a persistent one (EMFILE) cannot spin the loop")
constexpr int kErrorBackoffMs = 20;
ARC_CONSTANT("capacity hint: one recv chunk")
constexpr std::size_t kRecvChunkBytes = 4096;

enum class ConnState : std::uint8_t { Reading, Parked, Streaming, Sending, Closing };

// One accepted connection. Reading: waiting for one complete request (the idle
// clock runs). Parked: a /call is queued; the answer comes from the queue.
// Streaming: an SSE subscriber. Sending: one response is being written; when it
// is out the write side shuts down (no keep-alive, no pipelining). Closing: the
// response is out and the connection waits briefly for the peer's FIN.
struct Conn
{
    SocketType sock = INVALID_SOCK;
    ConnState state = ConnState::Reading;
    std::string inbuf;
    std::string outbuf;
    std::uint64_t callId = 0;
    std::string callRequestId; // the parked /call's wire id, stamped on its answer when the envelope has none
    std::string callCommand;   // the parked /call's command, stamped on its answer when the envelope has none
    std::uint64_t subscriberId = 0;
    std::string sseTopic;
    std::chrono::steady_clock::time_point lastRead{};
    bool peerClosed = false; // the client shut its write side (half-close); never read again
    bool overflow = false;   // an SSE backlog past the bound: the service thread drops it
    bool dead = false;     // dropped at the end of this iteration
};

// Linux: no SIGPIPE from a send to a closed peer. macOS sets SO_NOSIGPIPE per
// socket instead; Windows has no SIGPIPE.
int SendFlags()
{
#if defined(__linux__)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void NoSigPipe(SocketType sock)
{
#if defined(__APPLE__)
    int one = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)sock;
#endif
}

void NoDelay(SocketType sock)
{
    int one = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

void ShutdownSend(SocketType sock)
{
#ifdef _WIN32
    ::shutdown(sock, SD_SEND);
#else
    ::shutdown(sock, SHUT_WR);
#endif
}

int SendSome(SocketType sock, const char* data, std::size_t size)
{
    return static_cast<int>(::send(sock, data, static_cast<int>(size), SendFlags()));
}

int RecvSome(SocketType sock, char* data, std::size_t size)
{
    return static_cast<int>(::recv(sock, data, static_cast<int>(size), 0));
}

std::string AddressText(const sockaddr_in& addr)
{
    char text[64] = {};
    if (::inet_ntop(AF_INET, &addr.sin_addr, text, sizeof(text)) == nullptr)
        return "<unknown>";
    return text;
}

std::string Dump(const nlohmann::json& j)
{
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool DefaultPeerPredicate(const sockaddr_in& peer, void*)
{
    return PeerIsLoopback(peer);
}

int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string PercentDecode(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = HexValue(in[i + 1]);
            const int lo = HexValue(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// The value of `name` in an a=b&c=d query, percent-decoded; empty when absent.
std::string QueryValue(std::string_view query, std::string_view name)
{
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const std::size_t eq = pair.find('=');
        if (pair.substr(0, eq) == name)
            return eq == std::string_view::npos ? std::string() : PercentDecode(pair.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return {};
}

// P0 topics (spec s6.1): logs, problems, state, catalog, job:<id>.
bool IsServedTopic(std::string_view topic)
{
    if (topic == "logs" || topic == "problems" || topic == "state" || topic == "catalog") return true;
    constexpr std::string_view kJob = "job:";
    if (!topic.starts_with(kJob) || topic.size() == kJob.size()) return false;
    for (char c : topic.substr(kJob.size()))
        if (c < '0' || c > '9') return false;
    return true;
}

// The single path every parked answer takes (the dispatcher's, a job's finish,
// Stop's host.died). A job's final envelope carries neither the request id
// nor timing, and Stop's host.died names no command, so all three are filled
// here when unset: queuedMs is push -> pop and ranMs pop -> Complete (push ->
// Complete as queuedMs when it never popped).
void StampParkedAnswer(Envelope& env, const std::string& requestId, const std::string& command,
                       const RequestTimes& times)
{
    if (env.id.empty())
        env.id = requestId;
    if (env.command.empty())
        env.command = command;
    if (env.timing.queuedMs != 0.0 || env.timing.ranMs != 0.0)
        return;
    using Ms = std::chrono::duration<double, std::milli>;
    const std::chrono::steady_clock::time_point none{};
    if (times.pushed == none || times.completed == none)
        return;
    if (times.popped == none) {
        env.timing.queuedMs = Ms(times.completed - times.pushed).count();
        return;
    }
    env.timing.queuedMs = Ms(times.popped - times.pushed).count();
    env.timing.ranMs = Ms(times.completed - times.popped).count();
}

Envelope Busy(std::string command, const char* state, const char* message)
{
    Envelope env = MakeError(std::move(command), "host.busy", message, "retry; the host refused before running anything");
    env.errors.front().detail["state"] = state;
    return env;
}

} // namespace

bool PeerIsLoopback(const sockaddr_in& peer)
{
    return peer.sin_family == AF_INET && ntohl(peer.sin_addr.s_addr) == INADDR_LOOPBACK;
}

// ---------------------------------------------------------------------------
// RequestQueue
// ---------------------------------------------------------------------------

// A depth of 0 (remote.server.queueDepth carries no minimum) would refuse every
// push; the queue holds at least one call.
RequestQueue::RequestQueue(std::size_t depth)
    : m_capacity(std::max<std::size_t>(depth, 1))
{
}

bool RequestQueue::TryPush(std::uint64_t id, Call call)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.size() >= m_capacity || m_pending.contains(id) || m_results.contains(id))
        return false;
    m_queue.push_back(Entry{ id, std::move(call) });
    m_pending.insert(id);
    m_times[id] = RequestTimes{ std::chrono::steady_clock::now(), {}, {} };
    return true;
}

bool RequestQueue::TryPop(std::uint64_t& id, Call& call, std::chrono::steady_clock::time_point* queuedAt)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
        return false;
    id = m_queue.front().id;
    call = std::move(m_queue.front().call);
    RequestTimes& times = m_times[id];
    times.popped = std::chrono::steady_clock::now();
    if (queuedAt != nullptr)
        *queuedAt = times.pushed;
    m_queue.pop_front();
    return true;
}

void RequestQueue::Complete(std::uint64_t id, CommandResult result)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending.erase(id) == 0)
            return; // already completed, or never pushed: discarded
        // Answered before it was popped (Stop's host.died): it must never run.
        const auto queued = std::find_if(m_queue.begin(), m_queue.end(),
                                         [id](const Entry& e) { return e.id == id; });
        if (queued != m_queue.end())
            m_queue.erase(queued);
        m_results[id] = std::move(result);
        m_times[id].completed = std::chrono::steady_clock::now();
    }
    m_published.notify_all();
    std::lock_guard<std::mutex> wake(m_wakeMutex);
    if (m_wake)
        m_wake(m_wakeUser);
}

bool RequestQueue::Wait(std::uint64_t id, CommandResult& out, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_published.wait_for(lock, timeout, [&] { return m_results.contains(id); }))
        return false;
    auto it = m_results.find(id);
    out = std::move(it->second);
    m_results.erase(it);
    m_times.erase(id);
    return true;
}

bool RequestQueue::TryTake(std::uint64_t id, CommandResult& out, RequestTimes* times)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_results.find(id);
    if (it == m_results.end())
        return false;
    out = std::move(it->second);
    m_results.erase(it);
    const auto stamp = m_times.find(id);
    if (stamp != m_times.end())
    {
        if (times != nullptr)
            *times = stamp->second;
        m_times.erase(stamp);
    }
    return true;
}

std::uint64_t RequestQueue::OldestPendingId() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.empty() ? 0 : *m_pending.begin();
}

void RequestQueue::SetWake(WakeFn fn, void* user)
{
    std::lock_guard<std::mutex> lock(m_wakeMutex);
    m_wake = fn;
    m_wakeUser = user;
}

std::size_t RequestQueue::Depth() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

std::vector<SseEvent> TakeSseEvents(ListenerAtoms& atoms)
{
    std::vector<SseEvent> out;
    std::lock_guard<std::mutex> lock(atoms.sseMu);
    out.swap(atoms.sseEvents);
    return out;
}

// ---------------------------------------------------------------------------
// LoopbackHttpTransport
// ---------------------------------------------------------------------------

struct LoopbackHttpTransport::Impl
{
    Impl(RequestQueue& q, ListenerAtoms& a, std::string token, std::chrono::milliseconds idle)
        : queue(q), atoms(a), tokenHex(std::move(token)), idleClose(idle)
    {
        authConfig.maxAttempts = kAuthMaxAttempts;
        authConfig.windowSeconds = kAuthWindowSeconds;
        authConfig.cooldownSeconds = kAuthCooldownSeconds;
    }

    RequestQueue& queue;
    ListenerAtoms& atoms;
    const std::string tokenHex;
    const std::chrono::milliseconds idleClose;

    SocketType listenSock = INVALID_SOCK;
    SocketType wakeRead = INVALID_SOCK;
    SocketType wakeWrite = INVALID_SOCK; // guarded by wakeMu once Start returns
    std::mutex wakeMu;                   // Poke vs Stop: the write end is never used after it closes
    std::uint16_t port = 0;
    std::string portDecimal;

    RateLimiter authLimiter; // dedicated: never the process-wide limiter's keys
    RateLimiter::Config authConfig;

    std::atomic<bool> stopping{ false };
    std::unique_ptr<ServiceThread> thread;

    // Guards conns (and every Conn's outbuf), the peer predicate, and the
    // orphan set. The service thread holds it while it works, never while it polls.
    std::mutex connMu;
    std::vector<std::unique_ptr<Conn>> conns;
    std::vector<std::uint64_t> orphans; // parked call ids whose client left
    PeerPredicate peerPred = &DefaultPeerPredicate;
    void* peerUser = nullptr;
    std::uint64_t nextCallKey = 1;
    std::uint64_t nextSubscriber = 1;

    static void WakeThunk(void* user) { static_cast<Impl*>(user)->Poke(); }

    // One byte on the wake pair. Would-block means a wake is already pending.
    // Callable from any thread (queue Complete, EnqueueSse, Stop); wakeMu keeps
    // it off a write end that Stop has closed (the handle could be reused).
    void Poke()
    {
        std::lock_guard<std::mutex> lock(wakeMu);
        if (wakeWrite == INVALID_SOCK) return;
        const char byte = 'w';
        (void)SendSome(wakeWrite, &byte, 1);
    }

    // Once-per-burst error logging and back-off for hard accept / poll errors.
    std::chrono::steady_clock::time_point acceptPausedUntil{};
    bool acceptErrorLogged = false;
    bool pollErrorLogged = false;

    bool OpenWakePair();
    void Run();
    void Iterate();
    void Shutdown();

    void Accept();
    void ReadConn(Conn& c);
    void FlushConn(Conn& c);
    void Drop(Conn& c);
    void HandleRequest(Conn& c, const HttpRequest& req);
    void HandleStream(Conn& c, std::string_view query);
    void HandleCall(Conn& c, const HttpRequest& req);
    void Respond(Conn& c, int status, const char* reason, const Envelope& env);
    void RespondJson(Conn& c, int status, const char* reason, const nlohmann::json& body);
    void Refuse(Conn& c, int status, const char* reason, const Envelope& env, std::string_view what);
    void LogInfo(std::string_view text) const;
    std::size_t SseCount() const;
    std::size_t LiveCount() const;
};

void LoopbackHttpTransport::Impl::LogInfo(std::string_view text) const
{
    ARC_INFO("{}", RedactSecrets(text, tokenHex));
}

bool LoopbackHttpTransport::Impl::OpenWakePair()
{
    const SocketType tmp = CreateListenSocket(0, "127.0.0.1");
    if (tmp == INVALID_SOCK)
        return false;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(tmp, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        CloseSocket(tmp);
        return false;
    }
    const SocketType w = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (w == INVALID_SOCK || ::connect(w, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (w != INVALID_SOCK) CloseSocket(w);
        CloseSocket(tmp);
        return false;
    }
    sockaddr_in mine{};
    socklen_t mineLen = sizeof(mine);
    ::getsockname(w, reinterpret_cast<sockaddr*>(&mine), &mineLen);
    // Accept until the connection is ours: another local process could race
    // a connect onto the temporary port.
    SocketType r = INVALID_SOCK;
    for (;;) {
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        r = ::accept(tmp, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (r == INVALID_SOCK) break;
        if (peer.sin_port == mine.sin_port && PeerIsLoopback(peer)) break;
        CloseSocket(r);
    }
    CloseSocket(tmp);
    if (r == INVALID_SOCK) {
        CloseSocket(w);
        return false;
    }
    SetNonBlocking(r, true);
    SetNonBlocking(w, true);
    NoSigPipe(r);
    NoSigPipe(w);
    NoDelay(w);
    wakeRead = r;
    wakeWrite = w;
    return true;
}

std::size_t LoopbackHttpTransport::Impl::SseCount() const
{
    return static_cast<std::size_t>(std::count_if(conns.begin(), conns.end(), [](const std::unique_ptr<Conn>& c) {
        return !c->dead && c->state == ConnState::Streaming;
    }));
}

std::size_t LoopbackHttpTransport::Impl::LiveCount() const
{
    return static_cast<std::size_t>(std::count_if(conns.begin(), conns.end(),
                                                  [](const std::unique_ptr<Conn>& c) { return !c->dead; }));
}

void LoopbackHttpTransport::Impl::Run()
{
    while (!stopping.load(std::memory_order_acquire))
        Iterate();
    Shutdown();
}

void LoopbackHttpTransport::Impl::Iterate()
{
    std::vector<PollInterest> fds;
    std::vector<Conn*> polled;
    {
        std::lock_guard<std::mutex> lock(connMu);
        fds.reserve(conns.size() + 2);
        PollInterest listenIn;
        // Up to kMaxConnections live connections plus the one being answered
        // host.busy; past that the backlog waits (no busy-spin on a readable listen socket).
        // A hard accept error pauses accepting briefly (a persistent EMFILE
        // leaves the listen socket readable forever).
        const bool acceptPaused = std::chrono::steady_clock::now() < acceptPausedUntil;
        listenIn.socket = conns.size() <= kMaxConnections && !acceptPaused ? listenSock : INVALID_SOCK;
        listenIn.wantRead = true;
        fds.push_back(listenIn);
        PollInterest wakeIn;
        wakeIn.socket = wakeRead;
        wakeIn.wantRead = true;
        fds.push_back(wakeIn);
        for (const auto& c : conns) {
            PollInterest in;
            in.socket = c->sock;
            in.wantRead = c->state != ConnState::Sending && !c->peerClosed;
            in.wantWrite = !c->outbuf.empty();
            fds.push_back(in);
            polled.push_back(c.get());
        }
    }

    const int ready = PollSockets(fds, kPollWaitMs);
    if (ready < 0) {
        // EINTR or a hard error: back off so a persistent one cannot spin, and
        // log once per burst.
        if (!pollErrorLogged)
            LogInfo("remote: socket poll failed; the listener backs off and retries");
        pollErrorLogged = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(kErrorBackoffMs));
        return;
    }
    pollErrorLogged = false;

    std::lock_guard<std::mutex> lock(connMu);
    if (ready > 0) {
        if (fds[1].readable || fds[1].hungup) {
            char sink[64];
            while (RecvSome(wakeRead, sink, sizeof(sink)) > 0) {}
        }
        for (std::size_t i = 0; i < polled.size(); ++i) {
            Conn& c = *polled[i];
            const PollInterest& in = fds[i + 2];
            if (!c.dead && c.peerClosed && in.hungup)
                Drop(c); // a reset after the half-close: nothing more will arrive
            if (!c.dead && (in.readable || in.hungup))
                ReadConn(c);
            if (!c.dead && in.writable)
                FlushConn(c);
        }
        if (fds[0].readable)
            Accept();
    }

    // Answers published by the main thread (or a job's finish path).
    for (const auto& c : conns) {
        if (c->dead || c->state != ConnState::Parked)
            continue;
        CommandResult result;
        RequestTimes times;
        if (queue.TryTake(c->callId, result, &times)) {
            StampParkedAnswer(result.envelope, c->callRequestId, c->callCommand, times);
            Respond(*c, 200, "OK", result.envelope);
            FlushConn(*c);
        }
    }
    // A Complete for a call whose client left finds no Conn: discard it.
    std::erase_if(orphans, [this](std::uint64_t id) {
        CommandResult discarded;
        return queue.TryTake(id, discarded);
    });

    const auto now = std::chrono::steady_clock::now();
    for (const auto& c : conns) {
        if (c->dead)
            continue;
        // Parked and Streaming are never idle-closed: a long /call and a quiet
        // stream both stay open.
        if (c->overflow)
            Drop(*c); // a subscriber that stopped reading
        else if (c->state == ConnState::Reading && now - c->lastRead >= idleClose)
            Drop(*c);
        else if (c->state == ConnState::Closing && now - c->lastRead >= std::chrono::milliseconds(kLingerMs))
            Drop(*c);
    }
    std::erase_if(conns, [](const std::unique_ptr<Conn>& c) { return c->dead; });
}

void LoopbackHttpTransport::Impl::Accept()
{
    for (;;) {
        if (LiveCount() > kMaxConnections)
            return; // the backlog waits for a slot
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        const SocketType sock = ::accept(listenSock, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (sock == INVALID_SOCK) {
            if (IsSocketTimeoutError())
                return; // would-block: the poll raced and there is no socket
            // A hard error (EMFILE, ENFILE, ...): pause accepting, log once per burst.
            if (!acceptErrorLogged)
                LogInfo("remote: accept failed; the listener pauses accepting and retries");
            acceptErrorLogged = true;
            acceptPausedUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(kErrorBackoffMs);
            return;
        }
        acceptErrorLogged = false;
        // The peer check runs on the address of THIS accepted socket.
        sockaddr_in addr{};
        socklen_t addrLen = sizeof(addr);
        const bool named = ::getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &addrLen) == 0;
        if (!named || !peerPred(addr, peerUser)) {
            CloseSocket(sock);
            LogInfo("remote: peer refused " + (named ? AddressText(addr) : std::string("<unnamed>")));
            continue;
        }
        // Linux accept does not inherit O_NONBLOCK; set it everywhere.
        SetNonBlocking(sock, true);
        NoSigPipe(sock);
        NoDelay(sock);
        auto conn = std::make_unique<Conn>();
        conn->sock = sock;
        conn->lastRead = std::chrono::steady_clock::now();
        Conn& c = *conn;
        const bool overCap = LiveCount() >= kMaxConnections;
        conns.push_back(std::move(conn));
        if (overCap) {
            LogInfo("remote: connection refused host.busy (connections)");
            Respond(c, 503, "Service Unavailable",
                    Busy("", "connections", "the host already holds its maximum remote connections"));
            FlushConn(c);
        }
    }
}

void LoopbackHttpTransport::Impl::ReadConn(Conn& c)
{
    if (c.peerClosed)
        return; // half-closed: everything the client sent is already read
    char buf[kRecvChunkBytes];
    bool got = false;
    std::size_t taken = 0;
    // Bounded per call, so a client that never stops sending cannot hold the
    // loop; past the bound the parser answers "bounds".
    while (taken < kMaxHeaderBytes + kMaxBodyBytes) {
        const int n = RecvSome(c.sock, buf, sizeof(buf));
        if (n > 0) {
            taken += static_cast<std::size_t>(n);
            if (c.state == ConnState::Reading) {
                c.inbuf.append(buf, buf + n);
                got = true;
            }
            continue; // bytes on a Parked / Streaming / Closing connection are discarded
        }
        if (n < 0 && IsSocketTimeoutError())
            break; // would-block: everything available is read
        if (n == 0 && c.state == ConnState::Reading) {
            // Half-close: the request may already be complete in inbuf.
            c.peerClosed = true;
            break;
        }
        Drop(c);   // the peer closed (0) or a hard error
        return;
    }
    if (c.state != ConnState::Reading || (!got && !c.peerClosed))
        return;
    if (got)
        c.lastRead = std::chrono::steady_clock::now();

    std::string error;
    const std::optional<HttpRequest> req = ParseHttpRequest(c.inbuf, &error);
    if (!req) {
        if (error == "partial") {
            if (c.peerClosed)
                Drop(c); // closed before a complete request
            return;
        }
        if (error == "version")
            Refuse(c, 505, "HTTP Version Not Supported",
                   MakeError("", "arg.invalid", "only HTTP/1.1 is accepted", "send HTTP/1.1"), "version");
        else if (error == "chunked")
            Refuse(c, 400, "Bad Request",
                   MakeError("", "arg.invalid", "Transfer-Encoding is refused", "send the body with Content-Length"), "chunked");
        else if (error == "bounds")
            Refuse(c, 413, "Payload Too Large",
                   MakeError("", "arg.invalid", "the request exceeds the header or body bound", "send a smaller request"), "bounds");
        else if (error == "content-type")
            Refuse(c, 415, "Unsupported Media Type",
                   MakeError("", "arg.invalid", "a request body must be application/json", "send Content-Type: application/json"),
                   "content-type");
        else
            Refuse(c, 400, "Bad Request", MakeError("", "arg.invalid", "malformed HTTP request", "send one HTTP/1.1 request"),
                   "malformed");
        FlushConn(c);
        return;
    }
    c.inbuf.clear(); // bytes past Content-Length are ignored: one request per connection
    HandleRequest(c, *req);
    // A half-close is answered when the answer is inline (status, catalog, a
    // refusal). A parked call or a stream on a closed read side is the brief's
    // "client drop" whichever read saw the EOF first: the call stays queued
    // (an orphan, its result discarded), the stream reports Closed.
    if (!c.dead && c.peerClosed && (c.state == ConnState::Parked || c.state == ConnState::Streaming)) {
        Drop(c);
        return;
    }
    if (!c.dead && !c.outbuf.empty())
        FlushConn(c);
}

void LoopbackHttpTransport::Impl::FlushConn(Conn& c)
{
    while (!c.outbuf.empty()) {
        const int n = SendSome(c.sock, c.outbuf.data(), c.outbuf.size());
        if (n > 0) {
            c.outbuf.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && IsSocketTimeoutError())
            return; // keep the rest for the next writable poll
        Drop(c);   // EPIPE / reset: the peer is gone
        return;
    }
    if (c.state == ConnState::Sending) {
        if (c.peerClosed) {
            Drop(c); // the client's FIN is already in: nothing to linger for
            return;
        }
        // One response per connection: shut the write side so the client reads
        // EOF, then linger for its FIN so unread request bytes never reset it.
        ShutdownSend(c.sock);
        c.state = ConnState::Closing;
        c.lastRead = std::chrono::steady_clock::now();
    }
}

void LoopbackHttpTransport::Impl::Drop(Conn& c)
{
    if (c.dead)
        return;
    if (c.state == ConnState::Streaming) {
        std::lock_guard<std::mutex> lock(atoms.sseMu);
        atoms.sseEvents.push_back(SseEvent{ c.subscriberId, c.sseTopic, SseLife::Closed });
    }
    if (c.state == ConnState::Parked)
        orphans.push_back(c.callId);
    CloseSocket(c.sock);
    c.sock = INVALID_SOCK;
    c.dead = true;
}

void LoopbackHttpTransport::Impl::Respond(Conn& c, int status, const char* reason, const Envelope& env)
{
    RespondJson(c, status, reason, ToJson(env));
}

void LoopbackHttpTransport::Impl::RespondJson(Conn& c, int status, const char* reason, const nlohmann::json& body)
{
    HttpResponse response;
    response.status = status;
    response.reason = reason;
    response.headers.emplace_back("Connection", "close");
    response.body = Dump(body);
    c.outbuf = response.Serialize();
    c.state = ConnState::Sending;
}

void LoopbackHttpTransport::Impl::Refuse(Conn& c, int status, const char* reason, const Envelope& env,
                                         std::string_view what)
{
    const std::string code = env.errors.empty() ? std::string() : env.errors.front().code;
    LogInfo("remote: refused (" + std::string(what) + "): " + code);
    Respond(c, status, reason, env);
}

void LoopbackHttpTransport::Impl::HandleRequest(Conn& c, const HttpRequest& req)
{
    const TransportCheck check = CheckTransport(req, portDecimal, tokenHex);
    if (check.refuse == TransportRefuse::Origin || check.refuse == TransportRefuse::Host) {
        // Not an auth attempt: does not spend the auth budget.
        const bool origin = check.refuse == TransportRefuse::Origin;
        Refuse(c, 403, "Forbidden",
               MakeError("", check.code, check.message,
                         origin ? "browsers may not call the host; send the request without that header"
                                : "send Host: 127.0.0.1:<port> or localhost:<port>"),
               origin ? "origin" : "host");
        return;
    }
    if (check.refuse != TransportRefuse::None) {
        if (authLimiter.Allow("remote-auth", authConfig))
            Refuse(c, 401, "Unauthorized",
                   MakeError("", "auth.denied", check.message, "read the bearer token from the host's descriptor"), "auth");
        else
            Refuse(c, 429, "Too Many Requests",
                   MakeError("", "auth.throttled", "too many failed bearer attempts", "wait, then retry with the descriptor's token"),
                   "auth throttled");
        return;
    }
    // An authenticated request of any route is a client at work (spec s4.7:
    // the idle exit counts from the last request). HostSession::Drain reads
    // the count on the main thread (final fix pass, C-M4).
    atoms.requests.fetch_add(1, std::memory_order_relaxed);

    const std::string_view target = req.target;
    const std::size_t q = target.find('?');
    const std::string_view path = target.substr(0, q);
    const std::string_view query = q == std::string_view::npos ? std::string_view() : target.substr(q + 1);

    if (req.method == "GET" && path == "/remote/v1/status") {
        // Answered here, on the listener thread, in every host state.
        const std::shared_ptr<const HostStatus> status = atoms.status.load();
        if (!status) {
            Respond(c, 503, "Service Unavailable",
                    MakeError("", "host.unavailable", "the host has not published its status yet", "retry"));
            return;
        }
        RespondJson(c, 200, "OK", ToJson(*status));
        return;
    }
    if (req.method == "GET" && path == "/remote/v1/catalog") {
        const std::shared_ptr<const nlohmann::json> catalog = atoms.catalog.load();
        RespondJson(c, 200, "OK", catalog ? *catalog : nlohmann::json::object());
        return;
    }
    if (req.method == "GET" && path == "/remote/v1/stream") {
        HandleStream(c, query);
        return;
    }
    if (req.method == "POST" && path == "/remote/v1/call") {
        HandleCall(c, req);
        return;
    }
    // POST /remote/v1/batch is P1 (s8.3); every other route is unknown.
    Refuse(c, 404, "Not Found",
           MakeError("", "command.unknown", "no remote route " + req.method + " " + std::string(path),
                     "use /remote/v1/status, /remote/v1/call, /remote/v1/stream or /remote/v1/catalog"),
           "route");
}

void LoopbackHttpTransport::Impl::HandleStream(Conn& c, std::string_view query)
{
    const std::string topic = QueryValue(query, "topics");
    if (topic.empty()) {
        Refuse(c, 400, "Bad Request", MakeError("", "arg.missing", "a stream needs topics=", "pass topics=logs"), "stream");
        return;
    }
    if (topic.starts_with("watch:")) {
        Refuse(c, 400, "Bad Request",
               MakeError("", "arg.inapplicable", "watch streams are not served by this host", "use servitor status or wait"),
               "stream watch");
        return;
    }
    if (!IsServedTopic(topic)) {
        Refuse(c, 400, "Bad Request",
               MakeError("", "arg.invalid", "unknown stream topic '" + topic + "'",
                         "use one of logs, problems, state, catalog, job:<id>"),
               "stream topic");
        return;
    }
    if (SseCount() >= kMaxSseSubscribers) {
        Refuse(c, 503, "Service Unavailable",
               Busy("", "connections", "the host already holds its maximum stream subscribers"), "stream busy");
        return;
    }
    c.outbuf = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
               "X-Remote-Protocol: " + std::to_string(kProtocol) + "\r\n\r\n";
    c.state = ConnState::Streaming;
    c.subscriberId = nextSubscriber++;
    c.sseTopic = topic;
    std::lock_guard<std::mutex> lock(atoms.sseMu);
    atoms.sseEvents.push_back(SseEvent{ c.subscriberId, c.sseTopic, SseLife::Opened });
}

void LoopbackHttpTransport::Impl::HandleCall(Conn& c, const HttpRequest& req)
{
    // kMaxBodyBytes bounds the size, not the depth: a body nested past
    // kMaxRequestDepth is refused here, on the listener thread, before any
    // copy or dump could recurse through it (Task 30).
    bool tooDeep = false;
    nlohmann::json body = ParseJsonBounded(req.body, kMaxRequestDepth, &tooDeep);
    if (tooDeep) {
        Envelope env = MakeError("", "arg.invalid",
                                 "the call body nests deeper than " + std::to_string(kMaxRequestDepth) + " levels",
                                 "send args nested at most " + std::to_string(kMaxRequestDepth) + " levels deep");
        env.errors.front().detail = {{"reason", "too-deep"}, {"maxDepth", kMaxRequestDepth}};
        Refuse(c, 400, "Bad Request", env, "call body");
        return;
    }
    if (body.is_discarded() || !body.is_object()) {
        Refuse(c, 400, "Bad Request",
               MakeError("", "arg.invalid", "the call body is not a JSON object", "send {id, command, args, options}"), "call body");
        return;
    }
    const std::string command = body.contains("command") && body["command"].is_string()
        ? body["command"].get<std::string>() : std::string();
    const std::string id = body.contains("id") && body["id"].is_string() ? body["id"].get<std::string>() : std::string();
    auto refuse = [&](int status, const char* reason, const char* code, const std::string& message, const char* remediation) {
        Envelope env = MakeError(command, code, message, remediation);
        env.id = id;
        Refuse(c, status, reason, env, "call");
    };

    // Protocol first: a newer major may carry keys this host does not know.
    if (body.contains("protocol")) {
        const nlohmann::json& p = body["protocol"];
        if (!p.is_number_integer() || p.get<std::int64_t>() != static_cast<std::int64_t>(kProtocol)) {
            refuse(400, "Bad Request", "protocol.mismatch", "this host speaks remote protocol " + std::to_string(kProtocol) +
                   "; the request asked for " + Dump(p), "use a Servitor that speaks this host's protocol major");
            return;
        }
    }
    for (const auto& [key, value] : body.items()) {
        (void)value;
        if (key != "id" && key != "command" && key != "args" && key != "options" && key != "protocol") {
            refuse(400, "Bad Request", "arg.unknown", "unknown request key '" + key + "'",
                   "send only id, command, args, options");
            return;
        }
    }
    if (!body.contains("command") || (body["command"].is_string() && command.empty())) {
        refuse(400, "Bad Request", "arg.missing", "the call names no command", "set command");
        return;
    }
    if (!body["command"].is_string() || (body.contains("id") && !body["id"].is_string())) {
        refuse(400, "Bad Request", "arg.invalid", "id and command must be strings", "send id and command as strings");
        return;
    }

    Call call;
    call.id = id;
    call.command = command;
    // spec s4.5: X-Remote-Client names the caller; the audit line records it
    // (s10.4). Absent is legal (an empty client); a value outside 1-128
    // printable ASCII bytes is refused before it is queued (final fix pass, C-I1).
    for (const auto& [name, value] : req.headers) {
        if (name != "x-remote-client")
            continue;
        if (!IsValidClientId(value)) {
            refuse(400, "Bad Request", "arg.invalid",
                   std::string(kClientHeader) + " must be 1 to " + std::to_string(kMaxClientIdBytes) +
                       " printable ASCII characters",
                   "name the caller with printable ASCII, e.g. servitor --client ci-nightly");
            return;
        }
        call.callerId = value;
        break;
    }
    if (body.contains("args")) {
        if (!body["args"].is_object()) {
            refuse(400, "Bad Request", "arg.invalid", "args must be an object", "send args as a JSON object");
            return;
        }
        call.args = std::move(body["args"]);
    }
    if (body.contains("options")) {
        const nlohmann::json& options = body["options"];
        if (!options.is_object()) {
            refuse(400, "Bad Request", "arg.invalid", "options must be an object", "send options as a JSON object");
            return;
        }
        // Unknown keys inside options are ignored (rev 8.3 V6): new request
        // fields arrive here, so an older host tolerates a newer client.
        if (options.contains("world")) {
            if (!options["world"].is_string()) {
                refuse(400, "Bad Request", "arg.invalid", "options.world must be a string", "send a world name");
                return;
            }
            call.world = options["world"].get<std::string>();
        }
        if (options.contains("readOnly")) {
            if (!options["readOnly"].is_boolean()) {
                refuse(400, "Bad Request", "arg.invalid", "options.readOnly must be a boolean", "send true or false");
                return;
            }
            call.readOnly = options["readOnly"].get<bool>();
        }
        if (options.contains("timeoutMs")) {
            const nlohmann::json& t = options["timeoutMs"];
            // Already milliseconds: copied as is, never scaled.
            if (!t.is_number_unsigned() || t.get<std::uint64_t>() == 0 ||
                t.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
                refuse(400, "Bad Request", "arg.invalid", "options.timeoutMs must be a positive integer of milliseconds",
                       "send a positive integer");
                return;
            }
            call.timeoutMs = static_cast<std::uint32_t>(t.get<std::uint64_t>());
        }
    }

    const std::uint64_t key = nextCallKey++;
    if (!queue.TryPush(key, std::move(call))) {
        Envelope env = Busy(command, "queue-full", "the host's request queue is full");
        env.id = id;
        LogInfo("remote: call refused host.busy (queue-full)");
        Respond(c, 200, "OK", env);
        return;
    }
    // Parked before this thread polls again: Depth() == 1 means this
    // connection is already waiting for its answer.
    c.callId = key;
    c.callRequestId = id;
    c.callCommand = command;
    c.state = ConnState::Parked;
}

void LoopbackHttpTransport::Impl::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(connMu);
        for (const auto& c : conns) {
            if (c->dead || c->state != ConnState::Parked)
                continue;
            queue.Complete(c->callId, CommandResult{ MakeError("", "host.died", "listener stopped", "relaunch") });
            CommandResult result;
            RequestTimes times;
            if (queue.TryTake(c->callId, result, &times)) {
                StampParkedAnswer(result.envelope, c->callRequestId, c->callCommand, times);
                Respond(*c, 200, "OK", result.envelope);
            }
        }
    }
    // Flush what is queued (the host.died answers) for a bounded time.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStopFlushMs);
    for (;;) {
        std::vector<PollInterest> fds;
        std::vector<Conn*> polled;
        {
            std::lock_guard<std::mutex> lock(connMu);
            for (const auto& c : conns) {
                if (c->dead || c->outbuf.empty())
                    continue;
                PollInterest in;
                in.socket = c->sock;
                in.wantWrite = true;
                fds.push_back(in);
                polled.push_back(c.get());
            }
        }
        if (fds.empty() || std::chrono::steady_clock::now() >= deadline)
            break;
        PollSockets(fds, kStopFlushSliceMs);
        std::lock_guard<std::mutex> lock(connMu);
        for (std::size_t i = 0; i < polled.size(); ++i)
            if (fds[i].writable || fds[i].hungup)
                FlushConn(*polled[i]);
    }
    std::lock_guard<std::mutex> lock(connMu);
    for (const auto& c : conns)
        Drop(*c); // Streaming subscribers publish Closed
    conns.clear();
    // Calls whose client left: answer them too, so one still queued is removed
    // and never runs after the listener is gone, then discard the answer.
    for (std::uint64_t id : orphans) {
        queue.Complete(id, CommandResult{ MakeError("", "host.died", "listener stopped", "relaunch") });
        CommandResult discarded;
        queue.TryTake(id, discarded);
    }
    orphans.clear();
}

LoopbackHttpTransport::LoopbackHttpTransport(RequestQueue& queue, ListenerAtoms& atoms, std::string tokenHex,
                                             std::chrono::milliseconds idleClose)
    : m(std::make_unique<Impl>(queue, atoms, std::move(tokenHex), idleClose))
{
}

LoopbackHttpTransport::~LoopbackHttpTransport()
{
    Stop();
}

void LoopbackHttpTransport::Start()
{
    if (m->thread)
        return;
    if (!InitSockets()) {
        ARC_ERROR("remote: socket initialisation failed; the listener is not running");
        return;
    }
    m->listenSock = CreateListenSocket(0, "127.0.0.1"); // loopback only, never INADDR_ANY
    if (m->listenSock == INVALID_SOCK) {
        ARC_ERROR("remote: could not open the loopback listener");
        return;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (!SetNonBlocking(m->listenSock, true) ||
        ::getsockname(m->listenSock, reinterpret_cast<sockaddr*>(&bound), &len) != 0 || !m->OpenWakePair()) {
        ARC_ERROR("remote: could not prepare the loopback listener");
        CloseSocket(m->listenSock);
        m->listenSock = INVALID_SOCK;
        return;
    }
    m->port = ntohs(bound.sin_port);
    m->portDecimal = std::to_string(m->port);
    m->queue.SetWake(&Impl::WakeThunk, m.get());
    m->stopping.store(false, std::memory_order_release);
    Impl* impl = m.get();
    m->thread = std::make_unique<ServiceThread>(
        "RemoteListener", [impl] { impl->Run(); },
        [impl] {
            impl->stopping.store(true, std::memory_order_release);
            impl->Poke();
        });
}

void LoopbackHttpTransport::Stop()
{
    if (!m || !m->thread)
        return;
    m->thread->RequestStop(); // the body answers parked calls host.died, then closes every connection
    m->thread.reset();        // joins
    m->queue.SetWake(nullptr, nullptr);
    CloseSocket(m->listenSock);
    m->listenSock = INVALID_SOCK;
    CloseSocket(m->wakeRead);
    m->wakeRead = INVALID_SOCK;
    std::lock_guard<std::mutex> lock(m->wakeMu); // no Poke is mid-send on this handle
    CloseSocket(m->wakeWrite);
    m->wakeWrite = INVALID_SOCK;
}

std::uint16_t LoopbackHttpTransport::Port() const
{
    return m->port;
}

std::uintptr_t LoopbackHttpTransport::ListenSocket() const
{
    return static_cast<std::uintptr_t>(m->listenSock);
}

const std::string& LoopbackHttpTransport::Token() const
{
    return m->tokenHex;
}

std::chrono::milliseconds LoopbackHttpTransport::IdleClose() const
{
    return m->idleClose;
}

void LoopbackHttpTransport::SetPeerPredicate(PeerPredicate pred, void* user)
{
    std::lock_guard<std::mutex> lock(m->connMu);
    m->peerPred = pred ? pred : &DefaultPeerPredicate;
    m->peerUser = pred ? user : nullptr;
}

bool LoopbackHttpTransport::EnqueueSse(std::uint64_t subscriberId, std::string_view data)
{
    {
        std::lock_guard<std::mutex> lock(m->connMu);
        const auto it = std::find_if(m->conns.begin(), m->conns.end(), [&](const std::unique_ptr<Conn>& c) {
            return !c->dead && c->state == ConnState::Streaming && c->subscriberId == subscriberId;
        });
        if (it == m->conns.end() || (*it)->overflow)
            return false;
        std::string& out = (*it)->outbuf;
        if (out.size() >= kMaxBodyBytes) {
            // The client stopped reading: drop it (on the service thread) rather
            // than grow its backlog without bound.
            (*it)->overflow = true;
            m->Poke();
            return false;
        }
        // One SSE event; each payload line is its own data: field.
        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = data.find('\n', start);
            out += "data: ";
            out.append(data.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
            out += '\n';
            if (nl == std::string_view::npos)
                break;
            start = nl + 1;
        }
        out += '\n';
    }
    m->Poke();
    return true;
}

// ---------------------------------------------------------------------------
// RemoteListener
// ---------------------------------------------------------------------------

RemoteListener::RemoteListener(RequestQueue& queue, ListenerAtoms& atoms, std::string tokenHex,
                               std::chrono::milliseconds idleClose)
    : m_atoms(atoms)
    , m_transport(queue, atoms, std::move(tokenHex), idleClose)
{
}

void RemoteListener::Start() { m_transport.Start(); }
void RemoteListener::Stop() { m_transport.Stop(); }
std::uint16_t RemoteListener::Port() const { return m_transport.Port(); }
std::uintptr_t RemoteListener::ListenSocket() const { return m_transport.ListenSocket(); }
const std::string& RemoteListener::Token() const { return m_transport.Token(); }
std::chrono::milliseconds RemoteListener::IdleClose() const { return m_transport.IdleClose(); }
ListenerAtoms& RemoteListener::Atoms() { return m_atoms; }
LoopbackHttpTransport& RemoteListener::Transport() { return m_transport; }

void RemoteListener::SetPeerPredicate(LoopbackHttpTransport::PeerPredicate pred, void* user)
{
    m_transport.SetPeerPredicate(pred, user);
}

bool RemoteListener::EnqueueSse(std::uint64_t subscriberId, std::string_view data)
{
    return m_transport.EnqueueSse(subscriberId, data);
}

} // namespace Arcane::Remote

#endif
