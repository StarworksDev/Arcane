// Crypto.hpp before any spdlog include: it includes Util/Logger.hpp, which
// defines SPDLOG_ACTIVE_LEVEL first.
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Remote/Commands/CompareCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/CaptureCommands.hpp>
#include <Arcane/Remote/Commands/CommandSupport.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

// The wire args, reflected for their schema (spec s5.3). The command reads its
// args JSON itself and refuses unknown keys. -1 is "unset" for both budgets:
// an unset budget is not applied (both unset = a zero budget, ImageCompareOptions).
struct CompareArgs {
    std::string reference;            // the reference slot: a bare name, no extension, no directory
    std::string source = "host";      // host | viewport | window (Task 27's capture pairs)
    std::int64_t maxDiffPixels = -1;  // differing pixels allowed; -1 = unset
    double maxDiffPixelRatio = -1.0;  // differing fraction allowed, [0, 1]; -1 = unset
    bool bless = false;               // write the capture as the reference (a Drive call)
};
ARC_REFLECT_TYPE(CompareArgs)
    ARC_REFLECT_FIELD(CompareArgs, reference)
    ARC_REFLECT_FIELD(CompareArgs, source)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(CompareArgs, maxDiffPixels)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(CompareArgs, maxDiffPixelRatio)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(CompareArgs, bless)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

namespace
{

constexpr std::string_view kUsage =
    "pass reference (a bare name), and optionally source host|viewport|window, maxDiffPixels (an integer >= 0), "
    "maxDiffPixelRatio (0..1) and bless (true|false)";

// The parsed, validated args.
struct ParsedCompare {
    std::string reference;
    std::string source = "host";
    bool bless = false;
    ImageCompareOptions options;   // a budget is set only when its arg is present
};

// One armed compare job: Task 27's ticket copy, the args, the reference the
// command resolved before Arm, the hooks it was armed with, and where a
// Failed verdict's artifacts go. `new`ed by the command, passed as job.user,
// deleted by the step that finishes the job.
struct CompareJobState {
    Detail::TicketCopy copy;
    ParsedCompare args;
    ResolvedReference resolved;
    CompareHooks hooks;
    std::filesystem::path outDir;   // the capture hooks' artifact directory; empty = no artifacts
};

// The stored hooks and table (ruling F11), and every live job state keyed by
// job id, so a release frees what no step will reach. Main thread only.
struct CompareCommandState {
    CompareHooks hooks;
    JobTable* jobs = nullptr;
    std::map<std::uint64_t, CompareJobState*> live;
};

CompareCommandState& State()
{
    static CompareCommandState state;
    return state;
}

std::string Utf8(const std::filesystem::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

// Ends the job: forgets and frees its state. The caller sets job.envelope.
bool Finished(Job& job, CompareJobState* state)
{
    State().live.erase(job.id.value);
    delete state;
    return true;
}

// ReferenceImages' ReferenceNameIsSafe rule, applied at the wire boundary so a
// bad name is a usage error (exit 2) before anything is resolved or armed:
// non-empty, no separator, no ':' (a drive or an NTFS stream, G-3), no "..",
// no leading dot. An identical copy (ArcaneRemote never depends on
// ArcaneVerify, R8-2); CompareReferenceNameIsSafe exports it so a test pins
// the two copies to the same answers.
bool ReferenceNameIsSafe(const std::string& name)
{
    if (name.empty())
        return false;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        return false;
    if (name.find(':') != std::string::npos)
        return false;
    if (name.find("..") != std::string::npos)
        return false;
    return name.front() != '.';
}

std::optional<Envelope> ParseArgs(const Call& call, ParsedCompare& out)
{
    if (auto refusal = Detail::CheckKeys(call, {"reference", "source", "maxDiffPixels", "maxDiffPixelRatio", "bless"},
                                         kUsage))
        return refusal;
    if (!call.args.is_object() || !call.args.contains("reference"))
        return MakeError(call.command, "arg.missing", "capture.compare needs reference", std::string(kUsage));
    const nlohmann::json& reference = call.args.at("reference");
    if (!reference.is_string() || !ReferenceNameIsSafe(reference.get<std::string>()))
        return MakeError(call.command, "arg.invalid",
                         "reference must be a bare name (no directory, no ':', no '..', no leading '.')", std::string(kUsage));
    out.reference = reference.get<std::string>();

    if (const auto it = call.args.find("source"); it != call.args.end()) {
        const bool known = it->is_string() && (*it == "host" || *it == "viewport" || *it == "window");
        if (!known)
            return MakeError(call.command, "arg.invalid", "source must be host, viewport or window",
                             std::string(kUsage));
        out.source = it->get<std::string>();
    }
    if (const auto it = call.args.find("bless"); it != call.args.end()) {
        if (!it->is_boolean())
            return MakeError(call.command, "arg.invalid", "bless must be true or false", std::string(kUsage));
        out.bless = it->get<bool>();
    }
    if (const auto it = call.args.find("maxDiffPixels"); it != call.args.end()) {
        // An integer: unsigned (parsed JSON text) or signed (built in C++);
        // -1 is the schema's "unset"; any other negative or a fraction is refused.
        if (it->is_number_unsigned()) {
            out.options.maxDiffPixels = it->get<std::uint64_t>();
        } else if (it->is_number_integer() && it->get<std::int64_t>() >= 0) {
            out.options.maxDiffPixels = static_cast<std::uint64_t>(it->get<std::int64_t>());
        } else if (!(it->is_number_integer() && it->get<std::int64_t>() == CompareArgs{}.maxDiffPixels)) {
            return MakeError(call.command, "arg.invalid", "maxDiffPixels must be a whole number >= 0",
                             std::string(kUsage));
        }
    }
    if (const auto it = call.args.find("maxDiffPixelRatio"); it != call.args.end()) {
        const double ratio = it->is_number() ? it->get<double>() : std::nan("");
        if (ratio != CompareArgs{}.maxDiffPixelRatio) {
            if (!std::isfinite(ratio) || ratio < 0.0 || ratio > 1.0)
                return MakeError(call.command, "arg.invalid", "maxDiffPixelRatio must be a number in [0, 1]",
                                 std::string(kUsage));
            out.options.maxDiffPixelRatio = ratio;
        }
    }
    return std::nullopt;
}

// The reference's pixels, read through a wide-path std::ifstream and decoded
// in memory (no stbi file-path API, no path::string()); false with *why.
bool ReadReference(const std::filesystem::path& path, PixelData& pixels, std::string& why)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        why = "cannot open " + Utf8(path);
        return false;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        why = "cannot read " + Utf8(path);
        return false;
    }
    if (!DecodePngRgba(bytes.data(), bytes.size(), pixels) || !pixels.Valid()) {
        why = Utf8(path) + " is not a decodable image";
        return false;
    }
    return true;
}

// The host's resolve hook; a throw is nullopt with *why (never onto the main loop).
std::optional<ResolvedReference> SafeResolve(const CompareHooks& hooks, const std::string& slot, std::string& why)
{
    try {
        return hooks.resolve(hooks.projectRoot, slot, hooks.user);
    } catch (const std::exception& e) {
        why = e.what();
        return std::nullopt;
    }
}

nlohmann::json Thresholds(const ImageCompareOptions& options)
{
    nlohmann::json out = {{"maxDiffPixels", nullptr}, {"maxDiffPixelRatio", nullptr}};
    if (options.maxDiffPixels)
        out["maxDiffPixels"] = *options.maxDiffPixels;
    if (options.maxDiffPixelRatio)
        out["maxDiffPixelRatio"] = *options.maxDiffPixelRatio;
    return out;
}

nlohmann::json TriedPaths(const ResolvedReference& resolved)
{
    nlohmann::json out = nlohmann::json::array();
    for (const std::string& path : resolved.triedPaths)
        out.push_back(path);
    return out;
}

// A Failed verdict's evidence (spec s7.4): the capture and the diff image,
// written into the session's artifact directory beside capture.* PNGs.
std::optional<std::vector<Artifact>> WriteEvidence(const Job& job, const CompareJobState& state, std::uint32_t pid,
                                                   const PixelData& actual, const ImageCompareResult& result,
                                                   std::string& why)
{
    static std::atomic<std::uint64_t> serial{0};
    const auto unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::string stem =
        job.command + "-" + std::to_string(pid) + "-" + std::to_string(unixMs) + "-" + std::to_string(++serial);
    std::vector<Artifact> artifacts;
    std::optional<Artifact> capture = Detail::WritePngArtifact(state.outDir / (stem + ".png"), actual.width,
                                                               actual.height, actual.rgba.data(), "capture", why);
    if (!capture)
        return std::nullopt;
    artifacts.push_back(std::move(*capture));
    const bool diffValid = result.width > 0 && result.height > 0 &&
                           result.diffRgba.size() == static_cast<std::size_t>(result.width) * result.height * 4;
    if (diffValid) {
        std::optional<Artifact> diff = Detail::WritePngArtifact(state.outDir / (stem + ".diff.png"), result.width,
                                                                result.height, result.diffRgba.data(), "diff", why);
        if (!diff)
            return std::nullopt;
        artifacts.push_back(std::move(*diff));
    }
    return artifacts;
}

// The job step: Task 27's ticket step (Detail::StepTicket) on the source's
// pair, then -- only on the drain that returns the retired ticket's bytes --
// CompareImages and, with bless, the host's BlessFn. Reads no gate.frame,
// writes no startedFrame, never idles.
bool StepCompare(Job& job, HostGate& gate, void* user)
{
    CompareCommandState& s = State();
    auto* state = static_cast<CompareJobState*>(user);
    const auto it = s.live.find(job.id.value);
    if (it == s.live.end() || it->second != state) {
        // Released (RegisterCompareCommand moved on): the state is already gone.
        job.envelope = MakeError(job.command, "host.unavailable", "the compare's host session ended",
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
    PixelData actual;
    if (!Detail::NormalizeRetired(raw, actual.rgba, why)) {
        job.envelope = MakeError(job.command, "capture.no-frame", "the compare's capture is unusable: " + why,
                                 "retry the compare");
        return Finished(job, state);
    }
    actual.width = raw.width;
    actual.height = raw.height;

    // Compare against the reference the command resolved before Arm. A bless
    // replaces an unreadable reference without comparing against it.
    std::optional<ImageCompareResult> compared;
    if (state->resolved.found) {
        PixelData expected;
        if (ReadReference(state->resolved.path, expected, why)) {
            compared = CompareImages(expected, actual, state->args.options);
        } else if (!state->args.bless) {
            job.envelope = MakeError(job.command, "compare.reference-unreadable",
                                     "the reference could not be read: " + why,
                                     "replace it: servitor compare " + state->args.reference + " --bless");
            return Finished(job, state);
        }
    }

    bool passed = compared && compared->passed;
    if (state->args.bless) {
        bool wrote = false;
        try {
            wrote = state->hooks.bless(state->hooks.projectRoot, state->args.reference, actual.width, actual.height,
                                       actual.rgba.data(), state->hooks.user);
        } catch (const std::exception& e) {
            why = e.what();
        }
        if (!wrote) {
            job.envelope = MakeError(job.command, "compare.bless-failed",
                                     "the host did not write the reference " + state->args.reference +
                                         (why.empty() ? std::string() : ": " + why),
                                     "check that the project's Verify/References directory is writable, then retry");
            return Finished(job, state);
        }
        // The report describes the resolution that applies now (the runtime's
        // --bless report re-resolves the same way).
        std::string ignored;
        if (std::optional<ResolvedReference> after = SafeResolve(state->hooks, state->args.reference, ignored);
            after && after->found)
            state->resolved = std::move(*after);
        passed = true;   // the reference now is this capture
    }

    nlohmann::json data = {
        {"verdict", passed ? "Passed" : "Failed"},
        {"reference", state->args.reference},
        {"resolvedLevel", state->resolved.level},
        {"triedPaths", TriedPaths(state->resolved)},
        {"diffPixels", nullptr},
        {"diffRatio", nullptr},
        {"maxLocalDifference", nullptr},
        {"thresholds", Thresholds(state->args.options)},
        {"blessed", state->args.bless},
    };
    if (compared) {
        data["diffPixels"] = compared->diffCount;
        data["diffRatio"] = compared->diffRatio;
        data["maxLocalDifference"] = compared->maxLocalDifference;
    }

    std::vector<Artifact> artifacts;
    std::optional<Error> noDir;
    if (!passed && compared) {
        if (state->outDir.empty()) {
            noDir = Detail::MakeIssue("compare.no-artifact-dir",
                                      "this host has no artifact directory; the capture and diff were not written",
                                      "relaunch the host with --remote so its session has an out/ directory");
        } else {
            std::optional<std::vector<Artifact>> written = WriteEvidence(job, *state, gate.pid, actual, *compared, why);
            if (!written) {
                job.envelope = MakeError(job.command, "compare.artifact-failed",
                                         "the compare's capture or diff PNG was not written: " + why,
                                         "check the artifact directory and free disk space, then retry");
                job.envelope.errors.front().detail = {{"verdict", "Failed"}, {"diffPixels", compared->diffCount}};
                return Finished(job, state);
            }
            artifacts = std::move(*written);
        }
    }

    job.progress.done = 1;
    job.envelope = MakeOk(job.command, std::move(data), gate.Block());
    job.envelope.artifacts = std::move(artifacts);
    if (noDir)
        job.envelope.warnings.push_back(std::move(*noDir));
    return Finished(job, state);
}

CommandResult Compare(const Call& call, void* /*user*/)
{
    CommandResult result;
    CompareCommandState& s = State();
    // A torn-down session (no table; Teardown also cleared the hooks) is
    // host.unavailable (exit 5), not a host without a renderer (Task 27's order).
    if (s.jobs == nullptr) {
        result.envelope = MakeError(call.command, "host.unavailable", "no armed host session serves " + call.command,
                                    "relaunch the host with --remote");
        return result;
    }
    ParsedCompare args;
    if (auto refusal = ParseArgs(call, args)) {
        result.envelope = std::move(*refusal);
        return result;
    }
    // The source picks one of Task 27's pairs; a null pair is a host without
    // that renderer (a server has none): exit 2, nothing armed.
    const CaptureHooks& capture = Detail::StoredCaptureHooks();
    const ArmCopyFn arm = args.source == "viewport" ? capture.armViewport
                        : args.source == "window"   ? capture.armWindow
                                                    : capture.armHost;
    const DrainSlotFn drain = args.source == "viewport" ? capture.drainViewport
                            : args.source == "window"   ? capture.drainWindow
                                                        : capture.drainHost;
    if (arm == nullptr || drain == nullptr) {
        result.envelope = MakeError(call.command, "capture.no-renderer",
                                    "this host has no renderer to capture for source " + args.source,
                                    "compare on an editor or a runtime");
        return result;
    }
    if (s.hooks.resolve == nullptr || s.hooks.bless == nullptr) {
        result.envelope = MakeError(call.command, "host.unavailable", "this host installed no reference resolver",
                                    "compare on an editor or a runtime launched with --remote");
        return result;
    }
    // ONE capture in flight per host (ruling P4 (a)): a screenshot or compare
    // already Running is the same slot. Refused before resolve and before Arm.
    if (CaptureInFlight(*s.jobs))
        return CaptureBusy(call);

    std::string why;
    std::optional<ResolvedReference> resolved = SafeResolve(s.hooks, args.reference, why);
    if (!resolved) {
        result.envelope = MakeError(call.command, "compare.reference-unreadable",
                                    "the reference " + args.reference + " could not be resolved: " + why,
                                    "check the project's Verify/References directory");
        return result;
    }
    if (!resolved->found && !args.bless) {
        result.envelope = MakeError(call.command, "compare.missing-reference",
                                    "no reference image named " + args.reference + " exists for this host",
                                    "create it: servitor compare " + args.reference + " --bless");
        result.envelope.errors.front().detail["triedPaths"] = TriedPaths(*resolved);
        return result;
    }

    auto* state = new CompareJobState;
    state->copy.arm = arm;
    state->copy.drain = drain;
    state->copy.hookUser = capture.user;
    state->copy.deadline = Detail::CaptureDeadline(call);
    state->copy.cancel = args.source == "viewport" ? capture.cancelViewport
                       : args.source == "window"   ? capture.cancelWindow
                                                   : capture.cancelHost;
    state->args = std::move(args);
    state->resolved = std::move(*resolved);
    state->hooks = s.hooks;
    state->outDir = capture.outDir;
    const JobId id = s.jobs->Arm(call.command, 1, &StepCompare, state);
    s.live[id.value] = state;
    result.envelope = MakeOk(call.command, {{"done", false}}, {});
    result.completesJob = id.value;
    return result;
}

// Ruling F19: --bless writes a reference, so it is a Drive call (the gate's
// read-only refusal and the audit apply); everything else observes. A bless
// that is present but not a boolean is gated as Drive, then refused by the
// command (arg.invalid) -- never let through as Observe.
Access CompareAccess(const Call& call)
{
    if (!call.args.is_object())
        return Access::Observe;
    const auto it = call.args.find("bless");
    if (it == call.args.end())
        return Access::Observe;
    if (it->is_boolean())
        return it->get<bool>() ? Access::Drive : Access::Observe;
    return Access::Drive;
}

void RegisterOnce()
{
    CommandRegistry& registry = CommandRegistry::Get();
    if (registry.Find("capture.compare") != nullptr)
        return;
    Desc desc;
    desc.name = "capture.compare";
    desc.summary = "Compare a capture against a reference image; bless writes the reference";
    desc.access = Access::Observe;
    desc.mutates = Mutates::Project;
    desc.play = PlayPolicy::Any;
    desc.thread = ExecThread::Job;
    desc.needsFrame = true;   // a minimized editor answers host.busy{minimized} before this runs (Task 13)
    desc.argsMeta = Astra::GetMeta<CompareArgs>();
    desc.fn = &Compare;
    desc.accessFor = &CompareAccess;
    std::string error;
    if (!registry.Register(std::move(desc), &error))
        ARC_ERROR("remote: capture.compare refused: {}", error);
}

} // namespace

bool CompareReferenceNameIsSafe(const std::string& name)
{
    return ReferenceNameIsSafe(name);
}

void SetCompareHooks(CompareHooks hooks)
{
    State().hooks = std::move(hooks);
}

void RegisterCompareCommand(JobTable* jobs)
{
    CompareCommandState& s = State();
    if (s.jobs != jobs) {
        // The previous table's compares will never be stepped against these
        // hooks again: finish them (no waiter hangs) and free their state.
        for (auto& [id, state] : s.live) {
            if (s.jobs != nullptr)
                s.jobs->Finish(JobId{id},
                               MakeError("capture.compare", "host.unavailable", "the compare's host session ended",
                                         "retry once the host is armed again"),
                               JobState::Cancelled);
            delete state;
        }
        s.live.clear();
    }
    s.jobs = jobs;
    RegisterOnce();
}

} // namespace Arcane::Remote

#endif
