#include <Arcane/Remote/Commands/SessionCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/CaptureCommands.hpp>
#include <Arcane/Remote/Commands/CommandSupport.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

// The wire args of the two commands, reflected for their schema (spec s5.3).
// Each command reads its args JSON itself and refuses unknown keys.
// `force` is not a host argument: servitor quit --force ends the process
// itself after --timeout, and the host has no force path, so a `force` arg on
// the wire is refused (arg.inapplicable), never accepted and ignored (final
// fix pass, C-M7; Rule 3).
struct HostQuitArgs {
    bool discardChanges = false;   // quit even with unsaved changes
};
ARC_REFLECT_TYPE(HostQuitArgs)
    ARC_REFLECT_FIELD(HostQuitArgs, discardChanges)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

struct HostWaitArgs {
    std::string what;              // ready | settled | frames | reload | state
    std::uint32_t frames = 0;      // what = frames
    std::string state;             // what = state
};
ARC_REFLECT_TYPE(HostWaitArgs)
    ARC_REFLECT_FIELD(HostWaitArgs, what)
    ARC_REFLECT_FIELD(HostWaitArgs, frames)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(HostWaitArgs, state)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

namespace
{

// A settle wait's captures (spec s4.7, Task 30): the ticket in flight on the
// capture host pair, and the last retired capture to compare the next one to.
struct SettleCaptures {
    Detail::TicketCopy copy;
    bool armed = false;               // a ticket is in flight (the ring is held)
    bool idleAtArm = false;           // producersIdle when that ticket was armed
    bool havePrevious = false;        // `previous` was captured with the producers idle at both ends
    std::vector<unsigned char> previous;
    std::uint32_t width = 0, height = 0;
    std::uint64_t captures = 0;       // retired captures so far
    std::optional<Error> lastRefusal; // the last arm the ring refused; a timeout reports it (data.lastRefusal)
};

// One armed host.wait job's inputs, recorded when the command armed it.
struct WaitRow {
    WaitArgs args;
    std::uint64_t startedFrame = 0;
    std::uint32_t generationAtArm = 0;
    std::uint64_t reloadSerialAtArm = 0;   // session.ReloadSerial() at the arm (Task 30)
    bool hasRenderer = true;
    std::chrono::steady_clock::time_point started{};
    SettleCaptures settle;                 // what = settled only
};

// The stored pointers (ruling F11) and the armed waits, keyed by job id.
// Main thread only: commands and job steps both run inside Drain.
struct SessionCommandState {
    HostSession* session = nullptr;
    JobTable* jobs = nullptr;
    std::map<std::uint64_t, WaitRow> waits;
};

SessionCommandState& State()
{
    static SessionCommandState state;
    return state;
}

Envelope Unavailable(const std::string& command)
{
    return MakeError(command, "host.unavailable", "no armed host session serves " + command,
                     "relaunch the host with --remote");
}

bool IsPending(const Envelope& env)
{
    return env.ok && env.data.is_object() && !env.data.value("done", true);
}

// args must be an object (null reads as empty) whose keys are all in `known`.
std::optional<Envelope> CheckKeys(const Call& call, std::initializer_list<std::string_view> known,
                                  std::string_view usage)
{
    if (call.args.is_null())
        return std::nullopt;
    if (!call.args.is_object())
        return MakeError(call.command, "arg.invalid", call.command + " args must be a JSON object",
                         std::string(usage));
    for (const auto& [key, value] : call.args.items()) {
        (void)value;
        if (std::find(known.begin(), known.end(), std::string_view(key)) == known.end())
            return MakeError(call.command, "arg.unknown", call.command + " has no argument '" + key + "'",
                             std::string(usage));
    }
    return std::nullopt;
}

std::optional<Envelope> ReadBool(const Call& call, const char* key, bool& out)
{
    if (!call.args.is_object() || !call.args.contains(key))
        return std::nullopt;
    const nlohmann::json& v = call.args.at(key);
    if (!v.is_boolean())
        return MakeError(call.command, "arg.invalid", std::string(key) + " must be true or false",
                         std::string("pass ") + key + ": true");
    out = v.get<bool>();
    return std::nullopt;
}

std::string StateUsage()
{
    std::string text = "pass state: one of";
    for (const std::string_view name : kHostStateNames)
        text += " " + std::string(name);
    return text;
}

constexpr std::string_view kQuitUsage = "pass discardChanges (an optional boolean)";
constexpr std::string_view kWaitUsage = "pass what: ready, settled, frames (with frames: N), reload or state (with state: <s>)";

std::optional<Envelope> ParseWait(const Call& call, WaitArgs& out)
{
    if (auto refusal = CheckKeys(call, {"what", "frames", "state"}, kWaitUsage))
        return refusal;
    const nlohmann::json args = call.args.is_object() ? call.args : nlohmann::json::object();
    const auto what = args.find("what");
    if (what == args.end())
        return MakeError(call.command, "arg.missing", "host.wait needs what", std::string(kWaitUsage));
    if (!what->is_string())
        return MakeError(call.command, "arg.invalid", "what must be a string", std::string(kWaitUsage));
    out.what = what->get<std::string>();
    const std::string_view w = out.what;
    if (w != "ready" && w != "settled" && w != "frames" && w != "reload" && w != "state")
        return MakeError(call.command, "arg.invalid", "host.wait cannot wait for '" + out.what + "'",
                         std::string(kWaitUsage));

    if (const auto frames = args.find("frames"); frames != args.end()) {
        if (w != "frames")
            return MakeError(call.command, "arg.inapplicable", "frames applies to what: frames only",
                             std::string(kWaitUsage));
        if (!frames->is_number_integer() || frames->get<std::int64_t>() < 0 ||
            frames->get<std::int64_t>() > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
            return MakeError(call.command, "arg.invalid", "frames must be a non-negative integer",
                             "pass frames: N");
        out.frames = static_cast<std::uint32_t>(frames->get<std::int64_t>());
    } else if (w == "frames") {
        return MakeError(call.command, "arg.missing", "what: frames needs frames: N", "pass frames: N");
    }

    if (const auto state = args.find("state"); state != args.end()) {
        if (w != "state")
            return MakeError(call.command, "arg.inapplicable", "state applies to what: state only",
                             std::string(kWaitUsage));
        if (!state->is_string() || !IsHostStateName(state->get<std::string>()))
            return MakeError(call.command, "arg.invalid", "state must be a host state (spec s4.3)",
                             StateUsage());
        out.state = state->get<std::string>();
    } else if (w == "state") {
        return MakeError(call.command, "arg.missing", "what: state needs state: <s>", StateUsage());
    }
    return std::nullopt;
}

CommandResult HostQuit(const Call& call, void* /*user*/)
{
    CommandResult result;
    SessionCommandState& s = State();
    if (s.session == nullptr) {
        result.envelope = Unavailable(call.command);
        return result;
    }
    if (call.args.is_object() && call.args.contains("force")) {
        result.envelope = MakeError(call.command, "arg.inapplicable",
                                    "force is servitor quit's own flag: the host has no force path",
                                    "use servitor quit --force, which ends the process itself after --timeout");
        return result;
    }
    bool discardChanges = false;
    std::optional<Envelope> refusal = CheckKeys(call, {"discardChanges"}, kQuitUsage);
    if (!refusal)
        refusal = ReadBool(call, "discardChanges", discardChanges);
    if (refusal) {
        result.envelope = std::move(*refusal);
        return result;
    }

    // The editor's unsaved-changes guard (spec s4.7): a dirty host does not
    // quit unless the caller says to drop the changes. Nothing has run.
    const HostSession::Config& config = s.session->GetConfig();
    const bool dirty = config.dirty != nullptr && config.dirty(config.hookUser);
    if (dirty && !discardChanges) {
        result.envelope = MakeError(call.command, "host.busy",
                                    "the host has unsaved changes; quitting now would drop them",
                                    "save them first, or pass --discard-changes");
        result.envelope.errors.front().detail["state"] = "modal";
        return result;
    }

    s.session->RequestQuit(); // the host loop's DrainFrame exits remote-quit after this Drain
    ARC_INFO("remote: host.quit accepted{}", dirty ? "; unsaved changes are discarded" : "");
    result.envelope = MakeOk(call.command, {{"quitting", true}, {"discardedChanges", dirty}}, {});
    return result;
}

// Ends a wait row: a settle capture still armed is cancelled in the ring
// (C-M12, G-M5: a later screenshot never waits behind it), then its ring hold
// and its row go.
void ForgetWait(SessionCommandState& s, Job& job)
{
    if (const auto it = s.waits.find(job.id.value); it != s.waits.end() && it->second.settle.armed) {
        Detail::CancelTicket(job, it->second.settle.copy);
        it->second.settle.armed = false;
    }
    Detail::ReleaseCaptureRing(job.id.value);
    s.waits.erase(job.id.value);
}

// One step of `wait settled` (spec s4.7; Task 30): the --settle predicate, two
// byte-equal captures with the producers idle, through the capture host pair
// and Task 27's ticket step. Each capture is one render ticket: armed on one
// step (only while no screenshot or compare holds the ring, and only while
// producersIdle answers true), drained on later ones; it never waits on
// host.frame and never idles the device. A capture whose producers were busy
// at either end breaks the pair, so the next two start over. WaitPoll has
// already answered the refusals and the timeout. True when the job finished.
bool StepSettle(Job& job, HostGate& gate, SessionCommandState& s, WaitRow& row)
{
    const HostSession::Config& config = s.session->GetConfig();
    SettleCaptures& st = row.settle;
    if (!st.armed) {
        // One capture in flight per host (ruling P4 (a)): a screenshot or a
        // compare holds the ring; a wait waits for it rather than refusing.
        if (CaptureInFlight(*s.jobs))
            return false;
        st.idleAtArm = config.producersIdle(config.hookUser);
        if (!st.idleAtArm) {
            st.havePrevious = false;   // a busy producer breaks the pair
            return false;
        }
        st.copy.arm = config.capture.armHost;
        st.copy.drain = config.capture.drainHost;
        st.copy.hookUser = config.capture.user;
        st.copy.cancel = config.capture.cancelHost;
        st.copy.deadline = std::chrono::steady_clock::time_point::max();   // the wait's own timeout governs
        job.progress.label.clear();   // StepTicket arms while the label is not "copied"
    }

    CaptureMapped raw;
    const Detail::TicketStep step = Detail::StepTicket(job, gate, st.copy, raw);
    if (!st.armed) {
        if (step == Detail::TicketStep::Failed) {
            // Nothing will record a copy yet (a host still booting): not the
            // wait's failure. StepTicket wrote its MakeError; keep its error
            // for the timeout (a host that never arms is not silently a plain
            // timeout), restore the arm seed and try again on a later Drain,
            // bounded by the timeout.
            if (!job.envelope.errors.empty())
                st.lastRefusal = job.envelope.errors.front();
            job.envelope = MakeOk(job.command, nlohmann::json::object(), {});
            return false;
        }
        st.armed = true;   // StepTicket's arming step is always Pending
        Detail::HoldCaptureRing(job.id.value);
        return false;
    }
    if (step == Detail::TicketStep::Pending)
        return false;
    st.armed = false;
    if (step == Detail::TicketStep::Failed) {
        // capture.minimized (exit 5: a minimized host renders nothing) or a
        // lost copy (capture.no-frame); StepTicket wrote the envelope.
        ForgetWait(s, job);
        return true;
    }
    Detail::ReleaseCaptureRing(job.id.value);

    std::vector<unsigned char> rgba;
    std::string why;
    if (!Detail::NormalizeRetired(raw, rgba, why)) {
        job.envelope = MakeError(job.command, "capture.no-frame", "a settle capture is unusable: " + why,
                                 "retry the wait");
        ForgetWait(s, job);
        return true;
    }
    ++st.captures;
    const bool idleThroughout = st.idleAtArm && config.producersIdle(config.hookUser);
    const bool sameBytes = st.havePrevious && raw.width == st.width && raw.height == st.height && rgba == st.previous;
    if (idleThroughout && sameBytes) {
        job.envelope = MakeOk(job.command,
                              {{"done", true}, {"what", "settled"}, {"captures", st.captures}, {"width", raw.width},
                               {"height", raw.height}},
                              gate.Block());
        ForgetWait(s, job);
        return true;
    }
    st.previous = std::move(rgba);
    st.width = raw.width;
    st.height = raw.height;
    st.havePrevious = idleThroughout;
    return false;
}

// One step of an armed wait's row; true when the job finished (the row is
// gone then).
bool StepWaitRow(Job& job, HostGate& gate, SessionCommandState& s, WaitRow& row)
{
    Envelope env = WaitPoll(*s.session, row.args, row.startedFrame, row.generationAtArm, row.hasRenderer,
                            row.started, row.reloadSerialAtArm);
    if (IsPending(env)) {
        if (row.args.what == "settled")
            return StepSettle(job, gate, s, row);
        if (row.args.what == "frames") {
            const std::uint64_t now = gate.frame.load(std::memory_order_acquire);
            job.progress.done = std::min<std::uint64_t>(now >= row.startedFrame ? now - row.startedFrame : 0,
                                                        row.args.frames);
        }
        return false;
    }
    // Done, timed out, or a failure (MakeError): the row's ok envelope is
    // replaced. A settle capture still in flight is abandoned (the ring drops
    // an unclaimed ticket); its hold, and its clock freeze, go with the row.
    if (row.args.what == "settled" && env.ok && env.data.value("timedOut", false)) {
        env.data["captures"] = row.settle.captures;
        if (row.settle.lastRefusal) {
            const Error& refusal = *row.settle.lastRefusal;
            env.data["lastRefusal"] = {{"code", refusal.code}, {"message", refusal.message}};
        }
    }
    job.envelope = std::move(env);
    ForgetWait(s, job);
    return true;
}

bool StepWait(Job& job, HostGate& gate, void* user)
{
    SessionCommandState& s = *static_cast<SessionCommandState*>(user);
    const auto it = s.waits.find(job.id.value);
    if (it == s.waits.end() || s.session == nullptr) {
        // The session that armed this wait is gone (stopped, or replaced).
        job.envelope = Unavailable(job.command);
        ForgetWait(s, job);
        return true;
    }
    // WaitPoll bounds the wait at kMaxWaitTimeoutMs; the final envelope says
    // so when the caller asked for more.
    const std::uint32_t requestedMs = it->second.args.timeoutMs;
    const bool finished = StepWaitRow(job, gate, s, it->second);   // erases the row when finished
    if (finished && requestedMs > kMaxWaitTimeoutMs) {
        Error clamped = Detail::MakeIssue(
            "wait.timeout-clamped",
            "the wait asked for " + std::to_string(requestedMs) + " ms; a host bounds a wait at " +
                std::to_string(kMaxWaitTimeoutMs) + " ms",
            "wait again if the condition can take longer");
        clamped.detail = {{"requestedMs", requestedMs}, {"maxMs", kMaxWaitTimeoutMs}};
        job.envelope.warnings.push_back(std::move(clamped));
    }
    return finished;
}

CommandResult HostWait(const Call& call, void* /*user*/)
{
    CommandResult result;
    SessionCommandState& s = State();
    if (s.session == nullptr || s.jobs == nullptr) {
        result.envelope = Unavailable(call.command);
        return result;
    }
    WaitArgs args;
    if (auto refusal = ParseWait(call, args)) {
        result.envelope = std::move(*refusal);
        return result;
    }
    args.timeoutMs = call.timeoutMs; // milliseconds from the wire; not scaled again (ruling AX (1))

    // Armed now; the dispatcher binds the call's waiter to the job, and each
    // later Drain's StepAll polls it once (s7.7: nothing blocks the main thread).
    const HostSession& session = *s.session;
    WaitRow row;
    row.args = args;
    row.startedFrame = session.Gate().frame.load(std::memory_order_acquire);
    row.generationAtArm = session.Gate().pluginGeneration;
    row.reloadSerialAtArm = session.ReloadSerial();   // a failure noted after this answers the wait (Task 30)
    row.hasRenderer = session.GetConfig().kind != HostKind::Server;
    row.started = std::chrono::steady_clock::now();
    const JobId id = s.jobs->Arm(call.command, args.what == "frames" ? args.frames : 0u, &StepWait, &s);
    s.waits[id.value] = std::move(row);
    result.envelope = MakeOk(call.command, {{"done", false}, {"what", args.what}}, {});
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
        ARC_ERROR("remote: session command refused: {}", error);
}

} // namespace

Envelope WaitPoll(const HostSession& session, const WaitArgs& args, std::uint64_t startedFrame,
                  std::uint32_t generationAtArm, bool hasRenderer, std::chrono::steady_clock::time_point started,
                  std::uint64_t reloadSerialAtArm)
{
    const HostGate& gate = session.Gate();
    const std::string command = "host.wait";
    nlohmann::json data = {{"done", true}, {"what", args.what}};
    bool done = false;

    if (args.what == "ready") {
        const std::string state = gate.State();
        done = IsReadyState(state);
        data["state"] = state;
    } else if (args.what == "settled") {
        // Settled is two captures (spec s4.7), and captures are refused on a
        // host with no renderer (spec AM1): the same answer screenshot gives.
        if (!hasRenderer)
            return MakeError(command, "capture.no-renderer", "this host has no renderer",
                             "wait on an editor or a runtime");
        const HostSession::Config& config = session.GetConfig();
        if (config.capture.armHost == nullptr || config.capture.drainHost == nullptr)
            return MakeError(command, "capture.no-renderer",
                             "this host installed no capture pair, and settled compares two captures",
                             "wait on an editor or a runtime");
        if (config.producersIdle == nullptr)
            return MakeError(command, "host.unavailable", "this host installed no settle hook",
                             "wait on an editor or a runtime host");
        done = false;   // the host.wait job decides it from its captures (StepSettle)
    } else if (args.what == "frames") {
        const std::uint64_t frame = gate.frame.load(std::memory_order_acquire);
        done = frame >= startedFrame + args.frames;
        data["frame"] = frame;
        data["startedFrame"] = startedFrame;
    } else if (args.what == "reload") {
        done = gate.pluginGeneration != generationAtArm;
        // A reload that failed after the arm keeps the generation, so without
        // this the wait would run out its timeout (exit 8) on a known failure.
        if (!done && session.ReloadSerial() != reloadSerialAtArm && !session.LastReloadOk()) {
            Envelope failed = MakeError(command, "module.reload-failed", session.LastReloadReason(),
                                        "fix the cause the host log names (servitor logs), rebuild, then wait "
                                        "reload again");
            failed.errors.front().detail = {{"pluginGeneration", gate.pluginGeneration},
                                            {"atFrame", session.LastReloadFrame()}};
            return failed;
        }
        data["pluginGeneration"] = gate.pluginGeneration;
    } else if (args.what == "state") {
        if (!IsHostStateName(args.state))
            return MakeError(command, "arg.invalid", "'" + args.state + "' is not a host state (spec s4.3)",
                             StateUsage());
        const std::string state = gate.State();
        done = state == args.state;
        data["state"] = state;
    } else {
        return MakeError(command, "arg.invalid", "host.wait cannot wait for '" + args.what + "'",
                         std::string(kWaitUsage));
    }

    if (done)
        return MakeOk(command, std::move(data), gate.Block());
    const std::uint32_t timeoutMs = std::min(args.timeoutMs, kMaxWaitTimeoutMs);
    if (timeoutMs == 0 || std::chrono::steady_clock::now() - started > std::chrono::milliseconds(timeoutMs))
        return MakeOk(command, {{"timedOut", true}, {"what", args.what}}, gate.Block());
    return MakeOk(command, {{"done", false}, {"what", args.what}}, gate.Block());
}

std::uint32_t BoundedJobTimeoutMs(std::uint32_t requestedMs, std::uint32_t fallbackMs)
{
    return std::min(requestedMs != 0 ? requestedMs : fallbackMs, kMaxWaitTimeoutMs);
}

void RegisterSessionCommands(HostSession* session, JobTable* jobs)
{
    SessionCommandState& s = State();
    s.session = session;
    s.jobs = jobs;
    for (const auto& entry : s.waits)
        Detail::ReleaseCaptureRing(entry.first);   // a settle capture's hold (Task 30)
    s.waits.clear(); // a wait armed under the previous pointers answers host.unavailable

    Desc quit;
    quit.name = "host.quit";
    quit.summary = "Exit the host cleanly at the next frame boundary (exitReason remote-quit)";
    quit.access = Access::Drive;
    quit.mutates = Mutates::Project; // ends the host's session with its project
    quit.play = PlayPolicy::Any;
    quit.thread = ExecThread::Main;
    quit.argsMeta = Astra::GetMeta<HostQuitArgs>();
    quit.fn = &HostQuit;
    RegisterOnce(std::move(quit));

    Desc wait;
    wait.name = "host.wait";
    wait.summary = "Wait until the host is ready or settled, N frames pass, a module reloads, or it reaches a state";
    wait.access = Access::Observe;
    wait.mutates = Mutates::None;
    wait.play = PlayPolicy::Any;
    wait.thread = ExecThread::Job;
    wait.argsMeta = Astra::GetMeta<HostWaitArgs>();
    wait.fn = &HostWait;
    RegisterOnce(std::move(wait));
}

bool SettleHoldsClock(const HostSession* session)
{
    const SessionCommandState& s = State();
    if (session == nullptr || s.session != session)
        return false;
    for (const auto& entry : s.waits) {
        const WaitRow& row = entry.second;
        if (row.args.what != "settled" || !(row.settle.armed || row.settle.havePrevious))
            continue;
        // A row ends with its job today (StepWait's ForgetWait), but only a
        // job that is still live may hold the clock: a later path that ends a
        // host.wait job without its step (a cancel) must not leave it frozen.
        const Job* job = s.jobs != nullptr ? s.jobs->Find(JobId{entry.first}) : nullptr;
        if (job != nullptr && (job->state == JobState::Running || job->state == JobState::Queued))
            return true;
    }
    return false;
}

void ReleaseSessionCommands(const HostSession* session)
{
    SessionCommandState& s = State();
    if (session == nullptr || s.session != session)
        return;
    s.session = nullptr;
    s.jobs = nullptr;
    for (const auto& entry : s.waits)
        Detail::ReleaseCaptureRing(entry.first);
    s.waits.clear();
}

} // namespace Arcane::Remote

#endif
