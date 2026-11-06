#pragma once
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Net/TcpSocket.hpp> // sockaddr_in (ArcaneNet is a PUBLIC dependency of ArcaneRemote)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// True only for an IPv4 peer at 127.0.0.1.
ARC_REMOTE_API bool PeerIsLoopback(const sockaddr_in& peer);

// When the queue saw one call (steady clock). `popped` stays default when the
// call was Completed before a dispatcher popped it (Stop's host.died).
struct RequestTimes
{
    std::chrono::steady_clock::time_point pushed{};
    std::chrono::steady_clock::time_point popped{};
    std::chrono::steady_clock::time_point completed{};
};

// The listener -> main-thread hand-off (spec s4.3). The listener pushes each
// accepted /call under a monotonic key; the host's dispatcher pops, runs, and
// Completes it; the listener takes the published result without waiting.
// Every method is thread-safe.
class ARC_REMOTE_API RequestQueue
{
public:
    explicit RequestQueue(std::size_t depth = kQueueDepthDefault); // depth 0 holds one call
    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;

    bool TryPush(std::uint64_t id, Call call);          // false when full; does not run the command
    // queuedAt (optional) receives the steady-clock time TryPush accepted the
    // call; the dispatcher's timing.queuedMs is measured from it.
    bool TryPop(std::uint64_t& id, Call& call, std::chrono::steady_clock::time_point* queuedAt = nullptr);
    // Publishes the result of a pending id (one TryPush has not yet been
    // Completed), drops that id from the queue if it was never popped (a
    // completed call is never run), then calls the wake function. A Complete
    // for an id that is not pending (already completed, or never pushed) is
    // discarded.
    void Complete(std::uint64_t id, CommandResult result);
    // Takes the result once published; false after `timeout`. A zero timeout
    // returns at once. The listener thread never calls it.
    bool Wait(std::uint64_t id, CommandResult& out, std::chrono::milliseconds timeout);
    // Never waits; false if that id is not complete. `times` (optional) receives
    // the id's push / pop / Complete times, which the listener stamps onto a
    // parked answer whose timing is unset (a job's final envelope).
    bool TryTake(std::uint64_t id, CommandResult& out, RequestTimes* times = nullptr);
    std::uint64_t OldestPendingId() const;              // earliest TryPush id not yet Complete; 0 if none
    using WakeFn = void (*)(void* user);
    // Complete calls fn after publishing; null is legal. Once SetWake returns,
    // no call to the previous fn is still running.
    void SetWake(WakeFn fn, void* user);
    std::size_t Depth() const;                          // pushed and not yet popped

private:
    struct Entry
    {
        std::uint64_t id = 0;
        Call call;
    };

    mutable std::mutex m_mutex;
    std::condition_variable m_published;
    std::size_t m_capacity;
    std::deque<Entry> m_queue;
    std::set<std::uint64_t> m_pending;
    std::map<std::uint64_t, CommandResult> m_results;
    std::map<std::uint64_t, RequestTimes> m_times; // from TryPush until the result is taken

    std::mutex m_wakeMutex;
    WakeFn m_wake = nullptr;
    void* m_wakeUser = nullptr;
};

enum class SseLife : std::uint8_t { Opened, Closed };

struct SseEvent
{
    std::uint64_t subscriberId = 0;
    std::string topic;             // the topics= query value, e.g. "logs"
    SseLife life = SseLife::Opened;
};

// std::atomic<std::shared_ptr> is missing from Xcode 16 libc++. Same split as
// CVarRegistry.cpp:247-255 (plain shared_ptr + std::atomic_load_explicit /
// std::atomic_store_explicit under __APPLE__, std::atomic<std::shared_ptr> elsewhere).
// store/load keep std::atomic's spelling, so callers write atoms.status.store(snap).
template <class T>
class SharedSnapshot
{
public:
    void store(std::shared_ptr<const T> next) noexcept // release
    {
#if defined(__APPLE__)
        std::atomic_store_explicit(&m_ptr, std::move(next), std::memory_order_release);
#else
        m_ptr.store(std::move(next), std::memory_order_release);
#endif
    }

    std::shared_ptr<const T> load() const noexcept // acquire
    {
#if defined(__APPLE__)
        return std::atomic_load_explicit(&m_ptr, std::memory_order_acquire);
#else
        return m_ptr.load(std::memory_order_acquire);
#endif
    }

private:
#if defined(__APPLE__)
    std::shared_ptr<const T> m_ptr;
#else
    std::atomic<std::shared_ptr<const T>> m_ptr;
#endif
};

// What the listener thread reads without the main thread: the host's status
// and catalogue snapshots, plus the SSE subscriber life events it publishes.
struct ListenerAtoms
{
    SharedSnapshot<HostStatus> status;
    SharedSnapshot<nlohmann::json> catalog; // GET /catalog snapshot
    std::mutex sseMu;
    std::vector<SseEvent> sseEvents; // listener pushes; TakeSseEvents swaps
    // Authenticated requests answered or queued, any route (GET /status and
    // /catalog included). HostSession counts a change as idle-exit activity.
    std::atomic<std::uint64_t> requests{0};
};

ARC_REMOTE_API std::vector<SseEvent> TakeSseEvents(ListenerAtoms& atoms);

// The loopback HTTP listener (spec s6.1). One ServiceThread runs one
// non-blocking PollSockets loop with a per-connection state machine: /status
// and /catalog answer inline from the atoms, a /call parks until its result is
// Completed on the queue, SSE streams are written as frames arrive. Binds
// 127.0.0.1 only; every response closes its connection.
class ARC_REMOTE_API LoopbackHttpTransport
{
public:
    LoopbackHttpTransport(RequestQueue& queue, ListenerAtoms& atoms, std::string tokenHex,
                          std::chrono::milliseconds idleClose = std::chrono::seconds(kIdleConnectionSeconds));
    ~LoopbackHttpTransport();
    LoopbackHttpTransport(const LoopbackHttpTransport&) = delete;
    LoopbackHttpTransport& operator=(const LoopbackHttpTransport&) = delete;

    void Start();
    // Answers every parked call host.died, closes every connection (SSE
    // subscribers publish Closed), joins the thread, closes the sockets.
    void Stop();
    std::uint16_t Port() const;
    std::uintptr_t ListenSocket() const;
    const std::string& Token() const;
    std::chrono::milliseconds IdleClose() const;
    using PeerPredicate = bool (*)(const sockaddr_in& peer, void* user);
    // Default predicate is PeerIsLoopback with a null user. The accept loop is the only caller.
    void SetPeerPredicate(PeerPredicate pred, void* user);
    bool EnqueueSse(std::uint64_t subscriberId, std::string_view data); // false if that subscriber is gone

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

class ARC_REMOTE_API RemoteListener
{
public:
    RemoteListener(RequestQueue& queue, ListenerAtoms& atoms, std::string tokenHex,
                std::chrono::milliseconds idleClose = std::chrono::seconds(kIdleConnectionSeconds));
    RemoteListener(const RemoteListener&) = delete;
    RemoteListener& operator=(const RemoteListener&) = delete;

    void Start();
    void Stop();
    std::uint16_t Port() const;
    std::uintptr_t ListenSocket() const;
    const std::string& Token() const;
    std::chrono::milliseconds IdleClose() const;
    ListenerAtoms& Atoms();
    LoopbackHttpTransport& Transport();
    void SetPeerPredicate(LoopbackHttpTransport::PeerPredicate pred, void* user);
    bool EnqueueSse(std::uint64_t subscriberId, std::string_view data);

private:
    ListenerAtoms& m_atoms;
    LoopbackHttpTransport m_transport;
};

} // namespace Arcane::Remote

#endif
