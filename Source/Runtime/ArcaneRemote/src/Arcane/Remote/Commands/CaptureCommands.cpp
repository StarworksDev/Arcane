// Crypto.hpp before any spdlog include: it includes Util/Logger.hpp, which
// defines SPDLOG_ACTIVE_LEVEL first.
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Remote/Commands/CaptureCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/CommandSupport.hpp>
#include <Arcane/Remote/Commands/SessionCommands.hpp>   // BoundedJobTimeoutMs (C-M12)
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Reflection.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

// The wire args of both commands, reflected for their schema (spec s5.3).
struct CaptureArgs {
    std::string out;   // absolute .png path the client resolved from --out; empty = the artifact directory
};
ARC_REFLECT_TYPE(CaptureArgs)
    ARC_REFLECT_FIELD(CaptureArgs, out)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

namespace
{

ARC_CONSTANT("a capture job's bound when the call carries no timeout: a render frame retires a ticket in a few frames, so 30 s means the host is not rendering")
constexpr std::uint32_t kDefaultCaptureTimeoutMs = 30000;

constexpr std::string_view kUsage = "pass out: an absolute .png path, or nothing for the artifact directory";

// One armed capture job (ruling P4 (a)): the ticket copy (the source's hook
// pair, the ticket the first step armed, the deadline) and where the PNG goes.
// `new`ed by the command, passed as job.user, deleted by the step that
// finishes the job.
struct CaptureJobState {
    Detail::TicketCopy copy;
    std::filesystem::path outDir;
    std::filesystem::path outFile;   // --out; empty = outDir/<command>-<unix ms>-<n>.png
    bool occludedRisk = false;
};

// The stored hooks and table (ruling F11), and every live job state keyed by
// job id, so a release frees what no step will reach. Main thread only.
struct CaptureCommandState {
    CaptureHooks hooks;
    JobTable* jobs = nullptr;
    std::map<std::uint64_t, CaptureJobState*> live;
    std::set<std::uint64_t> ringHolds;   // host.wait settled jobs with a ticket armed (Task 30)
};

CaptureCommandState& State()
{
    static CaptureCommandState state;
    return state;
}

std::uint64_t NextCaptureTicket()
{
    static std::atomic<std::uint64_t> next{1};   // 0 is never issued: "not armed"
    return next.fetch_add(1, std::memory_order_relaxed);
}

std::string Utf8(const std::filesystem::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

Envelope Failure(const std::string& command, std::string code, std::string message, std::string remediation)
{
    return MakeError(command, std::move(code), std::move(message), std::move(remediation));
}

// Ends the job: forgets and frees its state. The caller sets job.envelope.
bool Finished(Job& job, CaptureJobState* state)
{
    State().live.erase(job.id.value);
    delete state;
    return true;
}

// Normalizes the retired copy and writes it as a PNG artifact
// (Detail::WritePngArtifact: encoded in memory, a wide-path std::ofstream
// whose close is checked, a rename); the artifact, or nullopt with *why.
std::optional<Artifact> WriteCapturePng(const Job& job, const CaptureJobState& state, const CaptureMapped& raw,
                                        std::uint32_t pid, std::string& why)
{
    std::vector<unsigned char> rgba;
    if (!Detail::NormalizeRetired(raw, rgba, why))
        return std::nullopt;

    static std::atomic<std::uint64_t> serial{0};
    std::filesystem::path file = state.outFile;
    if (file.empty()) {
        // <command>-<pid>-<unix ms>-<n>: an editor and a runtime sharing one
        // project's out/ directory never pick the same name.
        const auto unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        file = state.outDir / (job.command + "-" + std::to_string(pid) + "-" + std::to_string(unixMs) + "-" +
                               std::to_string(++serial) + ".png");
    }
    return Detail::WritePngArtifact(file, raw.width, raw.height, rgba.data(), "capture", why);
}

// The job step (ruling P3 (a)): the PickNode ticket, split across render
// submissions (Detail::StepTicket). It reads no gate.frame, writes no
// startedFrame, never idles.
bool StepCapture(Job& job, HostGate& gate, void* user)
{
    CaptureCommandState& s = State();
    auto* state = static_cast<CaptureJobState*>(user);
    const auto it = s.live.find(job.id.value);
    if (it == s.live.end() || it->second != state) {
        // Released (RegisterCaptureCommands moved on): the state is already gone.
        job.envelope = Failure(job.command, "host.unavailable", "the capture's host session ended",
                               "retry once the host is armed again");
        return true;
    }

    CaptureMapped raw;
    switch (Detail::StepTicket(job, gate, state->copy, raw)) {
        case Detail::TicketStep::Pending:
            return false;
        case Detail::TicketStep::Failed:
            return Finished(job, state);
        case Detail::TicketStep::Retired:
            break;
    }

    std::string why;
    std::optional<Artifact> artifact = WriteCapturePng(job, *state, raw, gate.pid, why);
    if (!artifact) {
        job.envelope = Failure(job.command, "capture.artifact-failed", "the capture PNG was not written: " + why,
                               "check the output path and free disk space, then retry");
        return Finished(job, state);
    }
    nlohmann::json data = {{"path", artifact->path}, {"width", raw.width}, {"height", raw.height}};
    job.progress.done = 1;
    job.envelope = MakeOk(job.command, std::move(data), gate.Block());
    job.envelope.artifacts.push_back(std::move(*artifact));
    if (state->occludedRisk) {
        job.envelope.warnings.push_back(Detail::MakeIssue(
            "capture.occluded-risk", "the window capture comes from a windowed host; some drivers return black "
                                     "for an occluded window",
            "keep the host window in the foreground, or capture a --headless host"));
    }
    return Finished(job, state);
}

// Parses {out}. An --out path must be absolute (the client resolved it) and end in .png.
std::optional<Envelope> ParseArgs(const Call& call, std::filesystem::path& outFile)
{
    if (auto refusal = Detail::CheckKeys(call, {"out"}, kUsage))
        return refusal;
    if (!call.args.is_object() || !call.args.contains("out"))
        return std::nullopt;
    const nlohmann::json& out = call.args.at("out");
    if (!out.is_string() || out.get<std::string>().empty())
        return MakeError(call.command, "arg.invalid", "out must be a non-empty path", std::string(kUsage));
    const std::string text = out.get<std::string>();
    // The wire is UTF-8. Check it explicitly: on POSIX the path conversion
    // takes any bytes, so only Windows would refuse by throwing.
    if (!Detail::IsValidUtf8(text))
        return MakeError(call.command, "arg.invalid", "out is not a valid UTF-8 path", std::string(kUsage));
    // The conversion can still throw (std::system_error on Windows), which
    // must never escape onto the host main thread.
    std::filesystem::path path;
    try {
        path = std::filesystem::path(std::u8string(text.begin(), text.end()));
    } catch (const std::exception&) {
        return MakeError(call.command, "arg.invalid", "out is not a valid UTF-8 path", std::string(kUsage));
    }
    if (!path.is_absolute())
        return MakeError(call.command, "arg.invalid", "out must be an absolute path, got " + text,
                         std::string(kUsage));
    std::string ext = Utf8(path.extension());
    for (char& c : ext)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (ext != ".png")
        return MakeError(call.command, "arg.invalid", "out must name a .png file, got " + text,
                         std::string(kUsage));
    outFile = std::move(path);
    return std::nullopt;
}

CommandResult Capture(const Call& call, void* /*user*/)
{
    CommandResult result;
    CaptureCommandState& s = State();
    const bool window = call.command == "capture.window";
    const ArmCopyFn arm = window ? s.hooks.armWindow : s.hooks.armViewport;
    const DrainSlotFn drain = window ? s.hooks.drainWindow : s.hooks.drainViewport;
    // A torn-down session (no table; Teardown also cleared the hooks) is
    // host.unavailable (exit 5), not a host without a renderer.
    if (s.jobs == nullptr) {
        result.envelope = MakeError(call.command, "host.unavailable", "no armed host session serves " + call.command,
                                    "relaunch the host with --remote");
        return result;
    }
    // A server (all six hooks null) has no renderer (spec AM1): exit 2, nothing armed.
    if (arm == nullptr || drain == nullptr) {
        result.envelope = MakeError(call.command, "capture.no-renderer", "this host has no renderer to capture",
                                    "capture an editor or a runtime");
        return result;
    }
    std::filesystem::path outFile;
    if (auto refusal = ParseArgs(call, outFile)) {
        result.envelope = std::move(*refusal);
        return result;
    }
    if (outFile.empty() && s.hooks.outDir.empty()) {
        result.envelope = MakeError(call.command, "host.unavailable", "this host has no artifact directory",
                                    "pass --out <file.png>");
        return result;
    }
    // ONE capture in flight per host (ruling P4 (a)): the ring has one
    // `wanted`, so a second capture is refused before Arm and before arm.
    if (CaptureInFlight(*s.jobs))
        return CaptureBusy(call);

    auto* state = new CaptureJobState;
    state->copy.arm = arm;
    state->copy.drain = drain;
    state->copy.hookUser = s.hooks.user;
    state->copy.deadline = Detail::CaptureDeadline(call);
    state->copy.cancel = window ? s.hooks.cancelWindow : s.hooks.cancelViewport;
    state->outDir = s.hooks.outDir;
    state->outFile = std::move(outFile);
    state->occludedRisk = window && s.hooks.occludedRisk;
    const JobId id = s.jobs->Arm(call.command, 1, &StepCapture, state);
    s.live[id.value] = state;
    result.envelope = MakeOk(call.command, {{"done", false}}, {});
    result.completesJob = id.value;
    return result;
}

void RegisterOnce(Desc desc)
{
    CommandRegistry& registry = CommandRegistry::Get();
    if (registry.Find(desc.name) != nullptr)
        return;
    std::string error;
    if (!registry.Register(std::move(desc), &error))
        ARC_ERROR("remote: capture command refused: {}", error);
}

Desc CaptureDesc(const char* name, const char* summary)
{
    Desc desc;
    desc.name = name;
    desc.summary = summary;
    desc.access = Access::Observe;
    desc.mutates = Mutates::None;
    desc.play = PlayPolicy::Any;
    desc.thread = ExecThread::Job;
    desc.needsFrame = true;   // a minimized editor answers host.busy{minimized} before this runs (Task 13)
    desc.argsMeta = Astra::GetMeta<CaptureArgs>();
    desc.fn = &Capture;
    return desc;
}

} // namespace

void SetCaptureHooks(CaptureHooks hooks)
{
    State().hooks = std::move(hooks);
}

void RegisterCaptureCommands(JobTable* jobs)
{
    CaptureCommandState& s = State();
    if (s.jobs != jobs) {
        // The previous table's captures will never be stepped against these
        // hooks again: finish them (no waiter hangs) and free their state.
        for (auto& [id, state] : s.live) {
            if (s.jobs != nullptr) {
                const Job* row = s.jobs->Find(JobId{id});
                const std::string command = row != nullptr ? row->command : std::string("capture.window");
                s.jobs->Finish(JobId{id},
                               MakeError(command, "host.unavailable", "the capture's host session ended",
                                         "retry once the host is armed again"),
                               JobState::Cancelled);
            }
            delete state;
        }
        s.live.clear();
        s.ringHolds.clear();   // the ids named the previous table's jobs
    }
    s.jobs = jobs;
    RegisterOnce(CaptureDesc("capture.viewport", "Capture the viewport (the scene render target) to a PNG artifact"));
    RegisterOnce(CaptureDesc("capture.window", "Capture the window backbuffer (after tonemap and ImGui) to a PNG artifact"));
}

CommandResult CaptureBusy(const Call& call)
{
    CommandResult result;
    result.envelope = MakeError(call.command, "host.busy", "a capture or compare is already in flight on this host",
                                "retry when it completes (servitor retries host.busy until --timeout)");
    result.envelope.errors.front().detail["state"] = "capture-pending";
    return result;
}

bool CaptureInFlight(const JobTable& jobs)
{
    if (jobs.AnyRunning({"capture.viewport", "capture.window", "capture.compare"}))
        return true;
    // A settle wait holding the ring (Task 30) counts while its job runs; a
    // hold whose job already ended (cancelled under it) is ignored.
    for (const std::uint64_t id : State().ringHolds) {
        const Job* job = jobs.Find(JobId{id});
        if (job != nullptr && job->state == JobState::Running)
            return true;
    }
    return false;
}

namespace Detail
{

const CaptureHooks& StoredCaptureHooks()
{
    return State().hooks;
}

void HoldCaptureRing(std::uint64_t jobId)
{
    State().ringHolds.insert(jobId);
}

void ReleaseCaptureRing(std::uint64_t jobId)
{
    State().ringHolds.erase(jobId);
}

std::chrono::steady_clock::time_point CaptureDeadline(const Call& call)
{
    // Clamped like a wait (C-M12): an abandoned capture on a host that cannot
    // retire its ticket must not hold the one capture slot for ~49 days.
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(BoundedJobTimeoutMs(call.timeoutMs, kDefaultCaptureTimeoutMs));
}

void CancelTicket(Job& job, TicketCopy& copy)
{
    if (job.progress.label != "copied" || copy.ticket == 0)
        return;   // never armed: nothing is in the ring
    if (copy.cancel != nullptr)
        copy.cancel(copy.ticket, copy.hookUser);
    job.progress.label.clear();
    copy.ticket = 0;
}

TicketStep StepTicket(Job& job, const HostGate& gate, TicketCopy& copy, CaptureMapped& raw)
{
    if (job.progress.label != "copied") {
        copy.ticket = NextCaptureTicket();
        if (!copy.arm(copy.ticket, copy.hookUser)) {
            job.envelope = Failure(job.command, "capture.no-frame", "the host could not arm a capture copy",
                                   "retry once the host is rendering");
            return TicketStep::Failed;
        }
        job.progress.label = "copied";
        return TicketStep::Pending;
    }

    if (!copy.drain(copy.ticket, &raw, copy.hookUser)) {
        // Not retired yet. A minimized host renders nothing, so the ticket
        // cannot retire: answer at once (exit 5, retryable), on any pending
        // step. Otherwise bounded: a host that stops rendering (device lost,
        // a failing frame) ends the job at its deadline instead of parking it.
        if (gate.minimized.load(std::memory_order_acquire)) {
            job.envelope = Failure(job.command, "capture.minimized",
                                   "the host was minimized before the capture's frame rendered",
                                   "restore the window and retry");
            CancelTicket(job, copy);
            return TicketStep::Failed;
        }
        if (std::chrono::steady_clock::now() < copy.deadline)
            return TicketStep::Pending;
        job.envelope = Failure(job.command, "capture.no-frame",
                               "no rendered frame retired the capture before the timeout",
                               "check that the host is rendering (stats frame, problems) and retry");
        job.envelope.errors.front().detail["reason"] = "timeout";
        CancelTicket(job, copy);
        return TicketStep::Failed;
    }
    if (raw.mapped == nullptr) {
        job.envelope = Failure(job.command, "capture.no-frame", "the capture copy was lost (its map failed)",
                               "retry the capture");
        return TicketStep::Failed;
    }
    return TicketStep::Retired;
}

bool NormalizeRetired(const CaptureMapped& raw, std::vector<unsigned char>& rgba, std::string& why)
{
    const std::size_t needed = raw.height == 0 ? 0
                             : static_cast<std::size_t>(raw.rowPitch) * (raw.height - 1) +
                                   static_cast<std::size_t>(raw.width) * 4;
    if (raw.mapped == nullptr || raw.width == 0 || raw.height == 0 || raw.rowPitch < raw.width * 4u ||
        raw.mappedBytes < needed) {
        why = "the retired copy is " + std::to_string(raw.mappedBytes) + " bytes for " + std::to_string(raw.width) +
              "x" + std::to_string(raw.height) + " at pitch " + std::to_string(raw.rowPitch);
        return false;
    }
    NormalizeCaptureRows(raw.mapped, raw.width, raw.height, raw.rowPitch, raw.bgra, rgba);
    return true;
}

} // namespace Detail

} // namespace Arcane::Remote

#endif
