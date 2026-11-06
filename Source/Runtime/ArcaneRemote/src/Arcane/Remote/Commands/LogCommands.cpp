#include <Arcane/Remote/Commands/LogCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/CommandSupport.hpp>   // kInlineCapBytes, ArtifactDir, WriteArtifact (C-M10)
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

// The wire args of log.read, reflected for its schema (spec s5.3). The command
// reads its args JSON itself and refuses unknown keys.
struct LogReadArgs {
    std::uint64_t since = 0;   // exclusive: records with seq > since
    std::string level;         // trace | debug | info | warn | error | critical; empty = every level
    bool follow = false;       // refused (arg.inapplicable): the follow path is GET /remote/v1/stream?topics=logs
};
ARC_REFLECT_TYPE(LogReadArgs)
    ARC_REFLECT_FIELD(LogReadArgs, since)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(LogReadArgs, level)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(LogReadArgs, follow)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

namespace
{

constexpr std::string_view kUsage =
    "pass since (a log seq, exclusive) and level (trace, debug, info, warn, error or critical), both optional";

// The stored table (ruling F11). Main thread only: the command runs inside Drain.
JobTable*& StoredJobs()
{
    static JobTable* jobs = nullptr;
    return jobs;
}

CommandResult Refused(const Call& call, const char* code, std::string message, std::string remediation)
{
    CommandResult result;
    result.envelope = MakeError(call.command, code, std::move(message), std::move(remediation));
    return result;
}

CommandResult LogRead(const Call& call, void* /*user*/)
{
    if (!call.args.is_null() && !call.args.is_object())
        return Refused(call, "arg.invalid", call.command + " args must be a JSON object", std::string(kUsage));
    std::uint64_t since = 0;
    int minLevel = 0;
    if (call.args.is_object()) {
        for (const auto& [key, value] : call.args.items()) {
            (void)value;
            if (key != "since" && key != "level" && key != "follow")
                return Refused(call, "arg.unknown", call.command + " has no argument '" + key + "'",
                               std::string(kUsage));
        }
        if (const auto it = call.args.find("follow"); it != call.args.end()) {
            if (!it->is_boolean())
                return Refused(call, "arg.invalid", "follow must be true or false", std::string(kUsage));
            // Ruling P2 (f): one follow path. A follow call would leave a job
            // with no subscriber to end it, so it is refused and arms nothing.
            if (it->get<bool>())
                return Refused(call, "arg.inapplicable", "log.read does not follow",
                               "use servitor logs --follow, which opens GET /remote/v1/stream?topics=logs");
        }
        if (const auto it = call.args.find("since"); it != call.args.end()) {
            const bool whole = it->is_number_unsigned() || (it->is_number_integer() && it->get<std::int64_t>() >= 0);
            if (!whole)
                return Refused(call, "arg.invalid", "since must be a non-negative whole number (a log seq)",
                               std::string(kUsage));
            since = it->get<std::uint64_t>();
        }
        if (const auto it = call.args.find("level"); it != call.args.end()) {
            const std::optional<int> level = it->is_string() ? ParseLogLevel(it->get_ref<const std::string&>())
                                                             : std::nullopt;
            if (!level)
                return Refused(call, "arg.invalid",
                               "level must be one of trace, debug, info, warn, error or critical",
                               std::string(kUsage));
            minLevel = *level;
        }
    }

    // The oldest window after `since`, up to the whole ring (Log::ReadSince).
    const std::vector<LogRecord> records = Log::ReadSince(since, Log::RingCapacity());
    std::uint64_t dropped = 0;
    if (!records.empty()) {
        if (records.front().seq > since + 1)
            dropped = records.front().seq - since - 1;
    } else {
        const std::uint64_t latest = Log::LatestSeq();
        if (latest > since)
            dropped = latest - since; // everything after `since` was evicted (a zero-line ring)
    }
    nlohmann::json rows = nlohmann::json::array();
    for (const LogRecord& record : records) {
        if (record.level >= minLevel)
            rows.push_back(LogRecordJson(record));
    }
    CommandResult result;
    // The inline cap every dump applies (spec s4.8; final fix pass, C-M10):
    // records past it go to one artifact (kind "logs") under the session's
    // out/ directory, data.records is empty and data.truncated true.
    bool truncated = false;
    std::vector<Artifact> artifacts;
    std::vector<Error> warnings;
    const std::string inlineJson = rows.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (inlineJson.size() > Detail::kInlineCapBytes) {
        const std::filesystem::path dir = Detail::ArtifactDir();
        if (dir.empty()) {
            Error w = Detail::MakeIssue("log.no-artifact-dir",
                                        "the " + std::to_string(rows.size()) + " records (" +
                                            std::to_string(inlineJson.size()) +
                                            " bytes) exceed the inline cap and this host has no artifact directory, "
                                            "so none were returned",
                                        "read a later window with --since, or raise --level");
            w.detail = {{"bytes", inlineJson.size()}, {"records", rows.size()}};
            warnings.push_back(std::move(w));
        } else {
            const nlohmann::json file = {{"records", rows}, {"dropped", dropped}};
            std::string why;
            std::optional<Artifact> artifact = Detail::WriteArtifact(
                dir, "log.read", "logs", file.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), why);
            if (!artifact) {
                result.envelope = MakeError(call.command, "log.artifact-failed",
                                            "the records exceed the inline cap and their artifact could not be "
                                            "written: " + why,
                                            "free disk space or check Saved/Remote/out, or read a later window "
                                            "with --since");
                result.envelope.errors.front().detail["bytes"] = inlineJson.size();
                return result;
            }
            artifacts.push_back(std::move(*artifact));
        }
        rows = nlohmann::json::array();
        truncated = true;
    }
    result.envelope =
        MakeOk(call.command, {{"records", std::move(rows)}, {"dropped", dropped}, {"truncated", truncated}}, {});
    result.envelope.warnings = std::move(warnings);
    result.envelope.artifacts = std::move(artifacts);
    return result;
}

} // namespace

nlohmann::json LogRecordJson(const LogRecord& record)
{
    const auto level = static_cast<spdlog::level::level_enum>(record.level);
    const spdlog::string_view_t name = spdlog::level::to_string_view(level);
    return {
        {"seq", record.seq},
        {"time", record.unixMs},
        {"level", std::string(name.data(), name.size())},
        {"logger", record.logger},
        {"text", record.text},
    };
}

std::optional<int> ParseLogLevel(std::string_view name)
{
    if (name == "trace") return static_cast<int>(spdlog::level::trace);
    if (name == "debug") return static_cast<int>(spdlog::level::debug);
    if (name == "info") return static_cast<int>(spdlog::level::info);
    if (name == "warn" || name == "warning") return static_cast<int>(spdlog::level::warn);
    if (name == "error") return static_cast<int>(spdlog::level::err);
    if (name == "critical") return static_cast<int>(spdlog::level::critical);
    return std::nullopt;
}

void SetLogJobs(JobTable* jobs)
{
    StoredJobs() = jobs;
}

void ReleaseLogJobs(const JobTable* jobs)
{
    if (jobs != nullptr && StoredJobs() == jobs)
        StoredJobs() = nullptr;
}

void RegisterLogCommand()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    Desc d;
    d.name = "log.read";
    d.summary = "Read the host's log ring: the records after a seq, optionally from a level up";
    d.access = Access::Observe;
    d.mutates = Mutates::None;
    d.play = PlayPolicy::Any;
    d.thread = ExecThread::Main;
    d.needsFrame = false;
    d.argsMeta = Astra::GetMeta<LogReadArgs>();
    d.fn = &LogRead;
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(d), &error))
        ARC_ERROR("remote: log command refused: {}", error);
}

} // namespace Arcane::Remote

#endif
