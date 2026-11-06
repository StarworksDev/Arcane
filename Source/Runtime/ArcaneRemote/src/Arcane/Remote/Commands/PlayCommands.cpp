#include <Arcane/Remote/Commands/PlayCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/CommandSupport.hpp>
#include <Arcane/Remote/Commands/SessionCommands.hpp>   // kMaxWaitTimeoutMs, BoundedJobTimeoutMs
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Reflection.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

// The wire args, reflected for their schemas (spec s5.3). Each command reads
// its args JSON itself and refuses unknown keys (Detail::CheckKeys).
struct PlayStartArgs {
    std::string topology = "standalone";   // standalone | listen | client+server (the editor's PlayTopology)
};
ARC_REFLECT_TYPE(PlayStartArgs)
    ARC_REFLECT_FIELD(PlayStartArgs, topology)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

struct PlayStepArgs {
    std::uint32_t n = 1;   // host.frame advances to wait for, 1..100000 (AM1: sim ticks; loop passes on a running windowed runtime)
};
ARC_REFLECT_TYPE(PlayStepArgs)
    ARC_REFLECT_FIELD(PlayStepArgs, n)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

// play.stop, play.pause and play.resume take no arguments.
struct PlayNoArgs {
};
ARC_REFLECT_TYPE(PlayNoArgs)
ARC_REFLECT_TYPE_END()

namespace
{

ARC_CONSTANT("spec s7.3: play.step's upper bound; a larger N is arg.inapplicable (exit 2)")
constexpr std::int64_t kMaxSteps = 100000;
ARC_CONSTANT("a step call that carries no timeout is bounded like a capture: servitor's default --timeout")
constexpr std::uint32_t kDefaultStepTimeoutMs = 30000;

constexpr std::string_view kStartUsage = "pass topology: standalone (the default), listen or client+server";
constexpr std::string_view kStepUsage =
    "pass n: a whole number of sim advances (host.frame) from 1 to 100000 (default 1)";
constexpr std::string_view kNoArgsUsage = "takes no arguments";
constexpr std::string_view kDefaultTopology = "standalone";

// One armed play.step: its origin (HostGate::frame at arm time), its own
// target n, and its host-side deadline (final fix pass, C-I2: bounded like
// host.wait, at most kMaxWaitTimeoutMs).
struct StepRow {
    std::uint64_t startedFrame = 0;
    std::uint64_t n = 0;
    std::chrono::steady_clock::time_point deadline{};
    std::uint32_t requestedMs = 0;
};

// The stored hooks, gate and table (ruling F11), and the play.step jobs this
// module armed in that table (job id -> its row), so a table change finishes
// them. Main thread only: the commands and their job steps both run inside Drain.
struct PlayCommandState {
    PlayHooks hooks;
    HostGate* gate = nullptr;
    JobTable* jobs = nullptr;
    std::map<std::uint64_t, StepRow> live;
};

PlayCommandState& State()
{
    static PlayCommandState state;
    return state;
}

Envelope Unavailable(const std::string& command)
{
    return MakeError(command, "host.unavailable", "no armed host session serves " + command,
                     "relaunch the host with --remote");
}

// {label: null, undoable: false, reason}: play is never undoable (spec s8.4).
UndoBlock PlayUndo(const PlayHooks& hooks)
{
    UndoBlock undo;
    undo.label = nullptr;
    undo.undoable = false;
    undo.reason = !hooks.undoReason.empty() ? hooks.undoReason
                                            : std::string(hooks.editor ? "play-mode" : "runtime-host");
    return undo;
}

CommandResult Answer(Envelope env)
{
    CommandResult result;
    result.envelope = std::move(env);
    return result;
}

// An ok answer for a command that returns in the command, with the undo block.
CommandResult Done(const Call& call, nlohmann::json data)
{
    Envelope env = MakeOk(call.command, std::move(data), {});
    env.undo = PlayUndo(State().hooks);
    return Answer(std::move(env));
}

// The stored pointers every play command needs, and the host side it drives:
// on the editor the command's own hook (`editorHookPresent`: it is non-null),
// on a runtime or server the TickGate. nullopt when the command may run;
// otherwise its host.unavailable answer (exit 5), with nothing dereferenced.
std::optional<Envelope> CheckAvailable(const Call& call, bool editorHookPresent)
{
    const PlayCommandState& s = State();
    if (s.jobs == nullptr || s.gate == nullptr)
        return Unavailable(call.command);
    if (s.hooks.editor ? !editorHookPresent : s.hooks.gate == nullptr)
        return Unavailable(call.command);
    return std::nullopt;
}

// Editor only: pause, resume and step act inside Play (the toolbar disables
// them in Edit). Null editorPlaying: not checked.
std::optional<Envelope> CheckEditorPlaying(const Call& call)
{
    const PlayHooks& hooks = State().hooks;
    if (!hooks.editor || hooks.editorPlaying == nullptr || hooks.editorPlaying(hooks.editorUser))
        return std::nullopt;
    return MakeError(call.command, "play.not-playing", call.command + " acts inside play mode; the editor is in Edit",
                     "servitor play first");
}

// Finishes every play.step job still running in the stored table with `env`
// (the waiter, if bound, is answered with it).
void FinishSteps(const Envelope& env, JobState state)
{
    PlayCommandState& s = State();
    if (s.jobs != nullptr) {
        for (const auto& [id, row] : s.live) {
            (void)row;
            s.jobs->Finish(JobId{id}, env, state);
        }
    }
    s.live.clear();
}

Envelope StepsStopped(std::string message)
{
    return MakeError("play.step", "play.stopped", std::move(message),
                     "servitor play (or resume), then step again");
}

// ---- play.start ---------------------------------------------------------

CommandResult PlayStart(const Call& call, void* /*user*/)
{
    if (auto refusal = Detail::CheckKeys(call, {"topology"}, kStartUsage))
        return Answer(std::move(*refusal));
    std::string topology(kDefaultTopology);
    if (call.args.is_object() && call.args.contains("topology")) {
        const nlohmann::json& t = call.args.at("topology");
        if (!t.is_string())
            return Answer(MakeError(call.command, "arg.invalid", "topology must be a string", std::string(kStartUsage)));
        topology = t.get<std::string>();
    }
    // The spelling first, before any hook runs (an unknown one never reaches Play).
    if (!IsPlayTopology(topology))
        return Answer(MakeError(call.command, "arg.invalid", "'" + topology + "' is not a play topology",
                                std::string(kStartUsage)));

    PlayCommandState& s = State();
    if (auto refusal = CheckAvailable(call, s.hooks.editorPlay != nullptr))
        return Answer(std::move(*refusal));

    if (s.hooks.editor) {
        // Already playing (PlaySession::Play would be a no-op success): answer
        // the topology that is ACTUALLY running, never the requested one, and
        // refuse an explicit different one rather than ignore it (Rule 3).
        const bool explicitTopology = call.args.is_object() && call.args.contains("topology");
        if (s.hooks.editorPlaying != nullptr && s.hooks.editorTopology != nullptr &&
            s.hooks.editorPlaying(s.hooks.editorUser)) {
            const std::string running(s.hooks.editorTopology(s.hooks.editorUser));
            if (explicitTopology && topology != running) {
                Envelope refused = MakeError(call.command, "play.refused",
                                             "the editor is already playing " + running + ", not " + topology,
                                             "servitor stop, then servitor play --topology " + topology);
                refused.errors.front().detail["running"] = running;
                return Answer(std::move(refused));
            }
            return Done(call, {{"playing", true}, {"topology", running}, {"alreadyPlaying", true}});
        }
        if (!s.hooks.editorPlay(s.hooks.editorUser, topology))
            return Answer(MakeError(call.command, "play.refused",
                                    "the editor refused Play (" + topology + "): the scene snapshot or the "
                                    "server world failed, and it stayed in Edit",
                                    "read servitor problems and servitor logs, fix the cause, then retry"));
        const std::string started = s.hooks.editorTopology != nullptr
                                        ? std::string(s.hooks.editorTopology(s.hooks.editorUser))
                                        : topology;
        return Done(call, {{"playing", true}, {"topology", started}});
    }

    // Runtime and server: the topology was fixed at launch (Rule 3: a flag
    // that does not apply is refused, never ignored), and nothing starts.
    if (topology != kDefaultTopology)
        return Answer(MakeError(call.command, "arg.inapplicable",
                                "this host's topology is fixed at launch; --topology applies to the editor only",
                                "drop --topology, or play in an editor"));
    // Does not Advance: the host loop's next Consume runs the sim.
    s.hooks.gate->SetRunning(true);
    s.hooks.gate->SetPaused(false);
    return Done(call, {{"playing", true}, {"topology", topology}});
}

// ---- play.stop ----------------------------------------------------------

CommandResult PlayStop(const Call& call, void* /*user*/)
{
    if (auto refusal = Detail::CheckKeys(call, {}, kNoArgsUsage))
        return Answer(std::move(*refusal));
    PlayCommandState& s = State();
    if (auto refusal = CheckAvailable(call, s.hooks.editorStop != nullptr))
        return Answer(std::move(*refusal));

    if (s.hooks.editor) {
        // PlaySession::Stop: a no-op success when already in Edit.
        if (!s.hooks.editorStop(s.hooks.editorUser)) {
            // The refused Stop paused the loop and stays in Play, so a pending
            // step would never get its ticks: finish it now, not at client timeout.
            FinishSteps(StepsStopped("play.stop failed to restore; the editor is paused in Play"), JobState::Failed);
            return Answer(MakeError(call.command, "play.refused",
                                    "the editor could not restore the edit snapshot on Stop",
                                    "read servitor problems and servitor logs"));
        }
    } else {
        s.hooks.gate->SetRunning(false);
        s.hooks.gate->SetPaused(true);
        s.hooks.gate->ClearSteps();
    }
    // A step still waiting for ticks will never get them now.
    FinishSteps(StepsStopped("play.stop ended the simulation before the requested steps ran"), JobState::Failed);
    return Done(call, {{"playing", false}});
}

// ---- play.pause / play.resume -------------------------------------------

CommandResult PlayPauseOrResume(const Call& call, bool paused)
{
    if (auto refusal = Detail::CheckKeys(call, {}, kNoArgsUsage))
        return Answer(std::move(*refusal));
    PlayCommandState& s = State();
    if (auto refusal = CheckAvailable(call, s.hooks.editorSetPaused != nullptr))
        return Answer(std::move(*refusal));
    if (auto refusal = CheckEditorPlaying(call))
        return Answer(std::move(*refusal));

    if (s.hooks.editor) {
        s.hooks.editorSetPaused(paused, s.hooks.editorUser);
    } else {
        s.hooks.gate->SetPaused(paused);
        // Resume runs the sim: Running is true afterwards, so it also starts a
        // host held at frame 0 (an open-ended --remote run; ruling B).
        if (!paused)
            s.hooks.gate->SetRunning(true);
    }
    return Done(call, {{"paused", paused}});
}

CommandResult PlayPause(const Call& call, void* /*user*/)
{
    return PlayPauseOrResume(call, true);
}

CommandResult PlayResume(const Call& call, void* /*user*/)
{
    return PlayPauseOrResume(call, false);
}

// ---- play.step ----------------------------------------------------------

// Runtime and server: a step that ended before its ticks ran gives back the
// gate ticks no other live step still needs (the budget is shared, G-1), so
// the sim never runs on for a caller who was already answered.
void GiveBackSteps(std::uint64_t frame)
{
    PlayCommandState& s = State();
    if (s.hooks.editor || s.hooks.gate == nullptr)
        return;
    std::uint64_t need = 0;
    for (const auto& [id, row] : s.live) {
        (void)id;
        const std::uint64_t target = row.startedFrame + row.n;
        if (target > frame)
            need = std::max(need, target - frame);
    }
    s.hooks.gate->TrimSteps(static_cast<std::uint32_t>(std::min<std::uint64_t>(need, UINT32_MAX)));
}

// One job step per Drain (s7.7). The delta is HostGate::frame minus the frame
// at arm time (recorded in the live map, mirrored onto job.startedFrame); the
// job finishes when it reaches n (job.progress.total). Editor: while short and
// paused, it asks for exactly one single step and returns, so the editor's
// next sim advance runs one tick; a running editor advances by itself (G-2).
// Past its deadline it answers timedOut (C-I2). Never calls Advance.
bool StepPlay(Job& job, HostGate& gate, void* /*user*/)
{
    PlayCommandState& s = State();
    const auto row = s.live.find(job.id.value);
    if (row == s.live.end()) {
        // Released (RegisterPlayCommands moved on). Ticks may already have
        // run, so this is play.stopped (exit 6), never host.unavailable
        // (exit 5, which the client retries: a retried step would advance again).
        job.envelope = StepsStopped("the step's host session ended before the requested steps ran");
        return true;
    }
    job.startedFrame = row->second.startedFrame;   // StepAll writes this copy back to the row
    const std::uint64_t frame = gate.frame.load(std::memory_order_acquire);
    const std::uint64_t delta = frame >= job.startedFrame ? frame - job.startedFrame : 0;
    const std::uint64_t n = job.progress.total;
    job.progress.done = delta < n ? delta : n;
    if (delta >= n) {
        nlohmann::json data = {{"done", true}, {"n", n}, {"advanced", delta}, {"startedFrame", job.startedFrame}};
        if (s.hooks.stats != nullptr)
            data["dtSeconds"] = s.hooks.stats->dtSeconds;   // the last advance's dt (stats.frame)
        job.envelope = MakeOk(job.command, std::move(data), {});
        s.live.erase(job.id.value);
        return true;
    }
    if (std::chrono::steady_clock::now() >= row->second.deadline) {
        // Bounded like host.wait (spec s4.7): ok with data.timedOut, exit 8,
        // the wait timeout's answer. The ticks it still owed are dropped.
        const std::uint32_t requestedMs = row->second.requestedMs;
        job.envelope = MakeOk(job.command,
                              {{"done", false}, {"timedOut", true}, {"n", n}, {"advanced", delta},
                               {"startedFrame", job.startedFrame}},
                              {});
        if (requestedMs > kMaxWaitTimeoutMs) {
            Error clamped = Detail::MakeIssue(
                "wait.timeout-clamped",
                "the step asked for " + std::to_string(requestedMs) + " ms; a host bounds a step at " +
                    std::to_string(kMaxWaitTimeoutMs) + " ms",
                "step again if it needs longer");
            clamped.detail = {{"requestedMs", requestedMs}, {"maxMs", kMaxWaitTimeoutMs}};
            job.envelope.warnings.push_back(std::move(clamped));
        }
        s.live.erase(job.id.value);
        GiveBackSteps(frame);
        return true;
    }

    if (s.hooks.editor) {
        if (s.hooks.editorPlaying != nullptr && !s.hooks.editorPlaying(s.hooks.editorUser)) {
            job.envelope = StepsStopped("the editor left play mode before the requested steps ran");
            s.live.erase(job.id.value);
            return true;
        }
        if (s.hooks.editorRequestSingleStep == nullptr) {
            job.envelope = StepsStopped("the editor's step hook went away before the requested steps ran");
            s.live.erase(job.id.value);
            return true;
        }
        // Only while the primary loop is paused: a running loop already
        // advances host.frame each pass, and a single step there would replace
        // that frame's accumulated dt (G-2). Null editorPaused reads as paused.
        if (s.hooks.editorPaused == nullptr || s.hooks.editorPaused(s.hooks.editorUser))
            s.hooks.editorRequestSingleStep(s.hooks.editorUser);   // exactly once per Drain
        return false;
    }

    // Runtime and server: the gate's pending steps are what advance the sim.
    // Held with none left and still short (a free run cleared them, then a
    // pause held the gate) means no tick is coming for this job.
    if (s.hooks.gate == nullptr) {
        job.envelope = StepsStopped("the host's tick gate went away before the requested steps ran");
        s.live.erase(job.id.value);
        return true;
    }
    if (s.hooks.gate->Held() && s.hooks.gate->StepsRemaining() == 0) {
        job.envelope = StepsStopped("the simulation was held before the requested steps ran");
        s.live.erase(job.id.value);
        return true;
    }
    return false;
}

CommandResult PlayStep(const Call& call, void* /*user*/)
{
    if (auto refusal = Detail::CheckKeys(call, {"n"}, kStepUsage))
        return Answer(std::move(*refusal));
    std::int64_t n = 1;
    std::string given = "1";
    if (call.args.is_object() && call.args.contains("n")) {
        const nlohmann::json& v = call.args.at("n");
        if (!v.is_number_integer())
            return Answer(MakeError(call.command, "arg.invalid", "n must be a whole number", std::string(kStepUsage)));
        given = v.dump();
        // A huge unsigned value reads as out of range, never as a wrapped negative.
        n = v.is_number_unsigned() && v.get<std::uint64_t>() > static_cast<std::uint64_t>(kMaxSteps)
                ? kMaxSteps + 1
                : v.get<std::int64_t>();
    }
    if (n < 1 || n > kMaxSteps)
        return Answer(MakeError(call.command, "arg.inapplicable", "play.step steps 1 to 100000 ticks, got " + given,
                                std::string(kStepUsage)));

    PlayCommandState& s = State();
    if (auto refusal = CheckAvailable(call, s.hooks.editorRequestSingleStep != nullptr))
        return Answer(std::move(*refusal));
    if (auto refusal = CheckEditorPlaying(call))
        return Answer(std::move(*refusal));

    // Armed now; the dispatcher binds the call's waiter to the job and each
    // later Drain steps it once. The arm-time frame is the stored gate's.
    const std::uint64_t startedFrame = s.gate->frame.load(std::memory_order_acquire);
    const JobId id = s.jobs->Arm(call.command, static_cast<std::uint64_t>(n), &StepPlay, nullptr);
    StepRow row;
    row.startedFrame = startedFrame;   // the delta's origin; the first step copies it onto job.startedFrame
    row.n = static_cast<std::uint64_t>(n);
    row.requestedMs = call.timeoutMs;
    row.deadline = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(BoundedJobTimeoutMs(call.timeoutMs, kDefaultStepTimeoutMs));
    s.live[id.value] = row;
    // Runtime and server: the host loop's Consume turns these into one Advance
    // per iteration; the gate keeps one budget, raised to this step's n, so
    // overlapping steps share their ticks (G-1). The editor asks for one
    // single step per job step instead.
    if (!s.hooks.editor)
        s.hooks.gate->RequestSteps(static_cast<std::uint32_t>(n));

    // The undo block rides the arming envelope, written before the job runs,
    // so it is there while the job is still Running (and BindWaiter copies it
    // onto the final envelope).
    Envelope env = MakeOk(call.command, {{"done", false}, {"n", n}, {"startedFrame", startedFrame}}, {});
    env.undo = PlayUndo(s.hooks);
    CommandResult result = Answer(std::move(env));
    result.completesJob = id.value;
    return result;
}

// ---- registration -------------------------------------------------------

Desc PlayDesc(const char* name, const char* summary, CommandFn fn, const Astra::TypeMeta* args,
              ExecThread thread = ExecThread::Main)
{
    Desc d;
    d.name = name;
    d.summary = summary;
    d.access = Access::Drive;       // the dispatcher's read-only gate and its audit apply
    d.mutates = Mutates::PlayMode;
    d.play = PlayPolicy::Any;
    d.thread = thread;
    d.needsFrame = false;
    d.worldScoped = false;          // --world is refused by the dispatcher (Rule 3)
    d.argsMeta = args;
    d.fn = fn;
    return d;
}

void RegisterOnce(Desc desc)
{
    CommandRegistry& registry = CommandRegistry::Get();
    if (registry.Find(desc.name) != nullptr)
        return;
    std::string error;
    if (!registry.Register(std::move(desc), &error))
        ARC_ERROR("remote: play command refused: {}", error);
}

} // namespace

bool IsPlayTopology(std::string_view topology)
{
    return topology == "standalone" || topology == "listen" || topology == "client+server";
}

void SetPlayHooks(PlayHooks hooks)
{
    State().hooks = std::move(hooks);
}

void SetPlayGate(HostGate* gate)
{
    State().gate = gate;
}

void RegisterPlayCommands(JobTable* jobs)
{
    PlayCommandState& s = State();
    if (s.jobs != jobs) {
        // The previous table's steps will never be stepped against these
        // hooks again: finish them (a bound waiter is answered, none hangs).
        // play.stopped (exit 6), not host.unavailable (exit 5): ticks may
        // already have run, and the client retries exit 5.
        FinishSteps(StepsStopped("the step's host session ended before the requested steps ran"),
                    JobState::Cancelled);
    }
    s.jobs = jobs;
    RegisterOnce(PlayDesc("play.start", "Enter play mode (editor) or run the held simulation (runtime, server)",
                          &PlayStart, Astra::GetMeta<PlayStartArgs>()));
    RegisterOnce(PlayDesc("play.stop", "Leave play mode (editor) or hold the simulation and drop pending steps",
                          &PlayStop, Astra::GetMeta<PlayNoArgs>()));
    RegisterOnce(PlayDesc("play.pause", "Pause the simulation (play mode keeps its state)", &PlayPause,
                          Astra::GetMeta<PlayNoArgs>()));
    RegisterOnce(PlayDesc("play.resume", "Resume the simulation (also starts a host held at frame 0)", &PlayResume,
                          Astra::GetMeta<PlayNoArgs>()));
    RegisterOnce(PlayDesc("play.step", "Advance the simulation N sim advances; completes when host.frame moved N "
                          "(a fixed tick, or one loop pass on a running windowed runtime)",
                          &PlayStep, Astra::GetMeta<PlayStepArgs>(), ExecThread::Job));
}

} // namespace Arcane::Remote

#endif
