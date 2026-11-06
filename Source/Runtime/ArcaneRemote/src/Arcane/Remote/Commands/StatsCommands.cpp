#include <Arcane/Remote/Commands/StatsCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace Arcane::Remote
{

// stats.frame takes no arguments; reflected (empty) for its schema (spec s5.3).
struct StatsFrameArgs {
};
ARC_REFLECT_TYPE(StatsFrameArgs)
ARC_REFLECT_TYPE_END()

void NoteAdvance(FrameStats& stats, std::uint64_t frame, double dtSeconds, double tickMs, double renderMs)
{
    stats.frame = frame;
    stats.dtSeconds = dtSeconds;
    stats.tickMs = tickMs;
    stats.renderMs = renderMs;
}

namespace
{

constexpr std::string_view kUsage = "stats.frame takes no arguments";

// The stored snapshot (ruling F11). Main thread only: the command runs inside Drain.
FrameStats*& StoredStats()
{
    static FrameStats* stats = nullptr;
    return stats;
}

CommandResult StatsFrameCommand(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (!call.args.is_null() && !call.args.is_object()) {
        result.envelope = MakeError(call.command, "arg.invalid", call.command + " args must be a JSON object",
                                    std::string(kUsage));
        return result;
    }
    if (call.args.is_object() && !call.args.empty()) {
        result.envelope = MakeError(call.command, "arg.unknown",
                                    call.command + " has no argument '" + call.args.begin().key() + "'",
                                    std::string(kUsage));
        return result;
    }
    const FrameStats* stats = StoredStats();
    if (stats == nullptr) {
        result.envelope = MakeError(call.command, "host.unavailable", "no armed host session serves stats.frame",
                                    "relaunch the host with --remote");
        return result;
    }
    result.envelope = MakeOk(call.command,
                             {{"frame", stats->frame},
                              {"dtSeconds", stats->dtSeconds},
                              {"tickMs", stats->tickMs},
                              {"renderMs", stats->renderMs}},
                             {});
    return result;
}

} // namespace

void RegisterStatsCommand(FrameStats* stats)
{
    StoredStats() = stats;
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    Desc d;
    d.name = "stats.frame";
    d.summary = "The last sim advance: host frame, dt, tick and render milliseconds";
    d.access = Access::Observe;
    d.mutates = Mutates::None;
    d.play = PlayPolicy::Any;
    d.thread = ExecThread::Main;
    d.needsFrame = false;
    d.worldScoped = false;   // process-wide
    d.argsMeta = Astra::GetMeta<StatsFrameArgs>();
    d.fn = &StatsFrameCommand;
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(d), &error))
        ARC_ERROR("remote: stats command refused: {}", error);
}

void ReleaseStatsCommand(const FrameStats* stats)
{
    if (stats != nullptr && StoredStats() == stats)
        StoredStats() = nullptr;
}

} // namespace Arcane::Remote

#endif
