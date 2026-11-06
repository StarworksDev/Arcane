// RateLimiter.hpp first: its Logger.hpp defines SPDLOG_ACTIVE_LEVEL, which must
// precede the first spdlog include (Listener.cpp's rule; Listener.hpp reaches spdlog).
#include <Arcane/Net/RateLimiter.hpp>
#include <Arcane/Remote/Follow.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/LogCommands.hpp>
#include <Arcane/Remote/Commands/ProblemCommands.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Base/DiagnosticStore.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Core/Constant.hpp>

#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

namespace
{

ARC_CONSTANT("bound, not a tunable: one log.follow step publishes at most this many ring lines; the rest go next frame")
constexpr std::size_t kFollowLinesPerStep = 512;

// replace, not throw: a log line that is not UTF-8 never aborts a step.
std::string Compact(const nlohmann::json& j)
{
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// A problem row's identity for the follow diff: every Diagnostic field, raw
// (the entity locator's resolved guid is not part of it, so a row does not
// count as new when its entity dies).
std::string RowKey(const Diagnostic& d)
{
    std::string key;
    key += static_cast<char>('0' + static_cast<int>(d.severity));
    key += static_cast<char>('0' + static_cast<int>(d.scope));
    key += static_cast<char>('0' + static_cast<int>(d.locator.kind));
    for (const std::string* field : {&d.code, &d.message, &d.detail, &d.locator.file}) {
        key += '\x1f';
        key += *field;
    }
    key += '\x1f' + std::to_string(d.locator.entity) + ':' + std::to_string(d.locator.asset.hi) + ':' +
           std::to_string(d.locator.asset.lo) + ':' + std::to_string(d.locator.line) + ':' +
           std::to_string(d.locator.col) + ':' + std::to_string(d.locator.ownerAsset.hi) + ':' +
           std::to_string(d.locator.ownerAsset.lo) + ':' + std::to_string(d.locator.nodeId);
    return key;
}

// The rows each problem.follow subscriber has already seen (sorted keys, a
// multiset), so a step publishes only rows that appeared since. Keyed by the
// listener too: subscriber ids count from 1 per listener. Main thread
// (ApplyFollowEvents and the steps run inside Drain); the mutex only guards
// two hosts' sessions in one process (tests).
using SubscriberKey = std::pair<const RemoteListener*, std::uint64_t>;
struct ProblemBaselines {
    std::mutex mutex;
    std::map<SubscriberKey, std::vector<std::string>> bySubscriber;
};

ProblemBaselines& Baselines()
{
    static ProblemBaselines baselines;
    return baselines;
}

std::vector<std::string> SortedKeys(const std::vector<Diagnostic>& rows)
{
    std::vector<std::string> keys;
    keys.reserve(rows.size());
    for (const Diagnostic& d : rows)
        keys.push_back(RowKey(d));
    std::sort(keys.begin(), keys.end());
    return keys;
}

// log.follow: the ring records after progress.done, oldest first.
bool StepLogFollow(Job& job, HostGate& /*gate*/, void* user)
{
    auto* listener = static_cast<RemoteListener*>(user);
    const std::vector<LogRecord> records = Log::ReadSince(job.progress.done, kFollowLinesPerStep);
    if (records.empty())
        return false;
    if (records.front().seq > job.progress.done + 1) {
        const std::uint64_t gap = records.front().seq - job.progress.done - 1;
        listener->EnqueueSse(job.subscriberId, Compact(nlohmann::json{{"dropped", gap}}));
    }
    for (const LogRecord& record : records)
        listener->EnqueueSse(job.subscriberId, Compact(LogRecordJson(record)));
    job.progress.done = records.back().seq;
    return false;
}

// problem.follow: the store's rows that are not in this subscriber's baseline.
bool StepProblemFollow(Job& job, HostGate& /*gate*/, void* user)
{
    auto* listener = static_cast<RemoteListener*>(user);
    const DiagnosticStore* store = ProblemStore();
    if (store == nullptr)
        return false;
    const std::vector<Diagnostic> rows = store->Snapshot();
    std::vector<std::string> keys = SortedKeys(rows);
    std::vector<std::string> previous;
    {
        ProblemBaselines& b = Baselines();
        std::lock_guard lock(b.mutex);
        const auto it = b.bySubscriber.find(SubscriberKey{listener, job.subscriberId});
        if (it == b.bySubscriber.end())
            return false; // closed: its Closed event already dropped the baseline
        if (it->second == keys)
            return false;
        previous = std::exchange(it->second, keys);
    }
    // Publish each row whose key occurs more often now than in the baseline,
    // in the store's order (errors first).
    std::vector<std::string> added;
    std::set_difference(keys.begin(), keys.end(), previous.begin(), previous.end(), std::back_inserter(added));
    for (const Diagnostic& d : rows) {
        if (added.empty())
            break;
        const auto it = std::find(added.begin(), added.end(), RowKey(d));
        if (it == added.end())
            continue;
        added.erase(it);
        listener->EnqueueSse(job.subscriberId, Compact(ProblemRowJson(d, nullptr)));
    }
    job.progress.done = rows.size();
    return false;
}

} // namespace

void ApplyFollowEvents(JobTable& jobs, RemoteListener& listener)
{
    for (const SseEvent& event : TakeSseEvents(listener.Atoms())) {
        if (event.life == SseLife::Closed) {
            jobs.CancelBySubscriber(event.subscriberId);
            ProblemBaselines& b = Baselines();
            std::lock_guard lock(b.mutex);
            b.bySubscriber.erase(SubscriberKey{&listener, event.subscriberId});
            continue;
        }
        if (event.topic == "logs") {
            const JobId id = jobs.Arm("log.follow", 0, &StepLogFollow, &listener);
            jobs.SetSubscriber(id, event.subscriberId);
            jobs.SetProgressDone(id, Log::LatestSeq()); // ruling F4: never the retained ring
        } else if (event.topic == "problems") {
            const DiagnosticStore* store = ProblemStore();
            std::vector<Diagnostic> rows;
            if (store != nullptr)
                rows = store->Snapshot();
            const JobId id = jobs.Arm("problem.follow", 0, &StepProblemFollow, &listener);
            jobs.SetSubscriber(id, event.subscriberId);
            jobs.SetProgressDone(id, rows.size()); // ruling F4: only rows added after it opened
            ProblemBaselines& b = Baselines();
            std::lock_guard lock(b.mutex);
            b.bySubscriber[SubscriberKey{&listener, event.subscriberId}] = SortedKeys(rows);
        }
        // state, catalog and job:<id> streams arm nothing here.
    }
}

} // namespace Arcane::Remote

#endif
