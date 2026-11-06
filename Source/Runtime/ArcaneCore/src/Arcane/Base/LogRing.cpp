// Sequenced log ring (Servitor P0 task 2). Separate from the crash backlog in
// Log.cpp: this ring is mutexed, holds structured records, and has
// subscribers. Log.cpp attaches MakeRingSink() inside Log::Init's dist sink.

#include <Arcane/Base/Log.hpp>

#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Core/Constant.hpp>

#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>

namespace Arcane::Log
{
    namespace
    {
        ARC_CONSTANT("the log.remote.ringLines default: one spelling, shared with the ARC_CVAR below (ruling B-1)")
        constexpr std::uint32_t kDefaultRingLines = 4096u;

        ARC_CVAR(cvar_logRemoteRingLines, "log.remote.ringLines", std::uint32_t, kDefaultRingLines,
                 .min = 1u, .flags = CVarFlags::Dev, .audience = Audience::Editor,
                 .scope = SettingScope::PreferencesMachine, .help = "How many log lines the remote log ring retains.");

        struct Subscriber
        {
            std::uint64_t id = 0;
            LogSinkFn fn = nullptr;
            void* user = nullptr;
        };

        struct Ring
        {
            std::mutex mutex;
            std::deque<LogRecord> records;
            std::vector<Subscriber> subscribers;
            std::uint64_t lastSeq = 0;
            std::uint64_t nextSubId = 1;
            std::size_t capacity = kDefaultRingLines;
        };

        // Leaked on purpose: the logger can emit during static destruction.
        Ring& GetRing()
        {
            static Ring* ring = new Ring();
            return *ring;
        }

        // Caller holds the ring mutex. Reads the cvar; a handle that is not
        // constructed yet (a line logged during static init) reads 0, which
        // keeps the current capacity.
        void RefreshCapacity(Ring& ring)
        {
            const std::uint32_t want = cvar_logRemoteRingLines.Get();
            if (want == 0u)
                return;
            ring.capacity = want;
            while (ring.records.size() > ring.capacity)
                ring.records.pop_front();
        }

        class RingSink final : public spdlog::sinks::base_sink<std::mutex>
        {
        protected:
            void sink_it_(const spdlog::details::log_msg& msg) override
            {
                LogRecord rec;
                rec.unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 msg.time.time_since_epoch()).count();
                rec.level = static_cast<int>(msg.level);
                rec.logger.assign(msg.logger_name.data(), msg.logger_name.size());
                rec.text.assign(msg.payload.data(), msg.payload.size());

                Ring& ring = GetRing();
                std::lock_guard<std::mutex> lock(ring.mutex);
                RefreshCapacity(ring);
                rec.seq = ++ring.lastSeq;
                ring.records.push_back(std::move(rec));
                while (ring.records.size() > ring.capacity)
                    ring.records.pop_front();
                const LogRecord& pushed = ring.records.back();
                for (const Subscriber& sub : ring.subscribers)
                    sub.fn(pushed, sub.user);
            }

            void flush_() override {}
        };
    }

    std::shared_ptr<spdlog::sinks::sink> MakeRingSink()
    {
        return std::make_shared<RingSink>();
    }

    LogSubscription Subscribe(LogSinkFn fn, void* user)
    {
        if (!fn)
            return {};
        Ring& ring = GetRing();
        std::lock_guard<std::mutex> lock(ring.mutex);
        const std::uint64_t id = ring.nextSubId++;
        ring.subscribers.push_back({ id, fn, user });
        return { id };
    }

    void Unsubscribe(LogSubscription sub)
    {
        if (sub.id == 0)
            return;
        Ring& ring = GetRing();
        std::lock_guard<std::mutex> lock(ring.mutex);
        std::erase_if(ring.subscribers, [&](const Subscriber& s) { return s.id == sub.id; });
    }

    std::vector<LogRecord> ReadSince(std::uint64_t seqExclusive, std::size_t maxLines)
    {
        std::vector<LogRecord> out;
        Ring& ring = GetRing();
        std::lock_guard<std::mutex> lock(ring.mutex);
        for (const LogRecord& rec : ring.records)
        {
            if (out.size() >= maxLines)
                break;
            if (rec.seq > seqExclusive)
                out.push_back(rec);
        }
        return out;
    }

    std::uint64_t LatestSeq()
    {
        Ring& ring = GetRing();
        std::lock_guard<std::mutex> lock(ring.mutex);
        return ring.lastSeq;
    }

    std::size_t RingCapacity()
    {
        Ring& ring = GetRing();
        std::lock_guard<std::mutex> lock(ring.mutex);
        RefreshCapacity(ring);
        return ring.capacity;
    }
}
