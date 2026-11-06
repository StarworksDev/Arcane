#include <Arcane/Remote/Dispatcher.hpp>

#include <Arcane/Config/CVarDecl.hpp>

namespace Arcane::Remote
{

namespace
{

// Registered outside the Dist guard: Dist's registry refuses Dev cvars by name
// (Dist [sweep][coverage] sees them), and the handles read these defaults. The
// accessors below are NOT: a Dist ArcaneRemote exports nothing but its module
// anchor (the Task 10 export gate), and every caller is Dist-guarded.
ARC_CVAR(cvar_remoteServerFrameBudgetMs, "remote.server.frameBudgetMs", double, kFrameBudgetMs,
    .flags = CVarFlags::Dev, .audience = Audience::Server, .scope = SettingScope::Project,
    .help = "Milliseconds of remote commands drained before the next tick.");
ARC_CVAR(cvar_remoteServerQueueDepth, "remote.server.queueDepth", std::uint32_t, kQueueDepthDefault,
    .flags = CVarFlags::Dev, .audience = Audience::Server, .scope = SettingScope::Project,
    .help = "Remote requests a host queues before it answers host.busy queue-full.");

} // namespace

#if !defined(ARC_BUILD_DIST)
double FrameBudgetMsSetting()
{
    return cvar_remoteServerFrameBudgetMs.Get();
}

std::uint32_t QueueDepthSetting()
{
    return cvar_remoteServerQueueDepth.Get();
}
#endif

} // namespace Arcane::Remote

#if !defined(ARC_BUILD_DIST)

// Crypto.hpp before Log.hpp: it includes Util/Logger.hpp, which defines
// SPDLOG_ACTIVE_LEVEL and must run before spdlog is first included.
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Core/Constant.hpp>

#include <algorithm>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

std::string HostGate::State() const
{
    std::lock_guard lock(stateMutex);
    return state;
}

void HostGate::SetState(std::string s)
{
    std::lock_guard lock(stateMutex);
    state = std::move(s);
}

HostBlock HostGate::Block() const
{
    HostBlock block;
    block.kind = kind;
    block.pid = pid;
    block.world = world;
    block.frame = frame.load(std::memory_order_acquire);
    block.state = State();
    block.catalog = catalog;
    block.pluginGeneration = pluginGeneration;
    return block;
}

namespace
{

ARC_CONSTANT("s10.4: Saved/Remote/out keeps the newest 200 artifacts; older ones are pruned when a dispatcher starts")
constexpr std::size_t kOutKeepFiles = 200;
ARC_CONSTANT("s4.3: one Invoke longer than this many frame budgets is warned about (once per command)")
constexpr double kSlowInvokeBudgets = 10.0;

using Clock = std::chrono::steady_clock;

double MsBetween(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

std::int64_t NowUnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Envelope Busy(const std::string& command, const std::string& state, std::string message, std::string remediation)
{
    Envelope env = MakeError(command, "host.busy", std::move(message), std::move(remediation));
    env.errors.front().detail["state"] = state;
    return env;
}

// The state gate (s4.3, ruling F19): a refusal means `fn` is never called.
std::optional<Envelope> Gate(const Desc& desc, Access access, const Call& call, const HostGate& gate)
{
    // Spec s4.6 Rule 3: a flag is never ignored. --world means something only
    // to a command that resolves a world; everywhere else it is a usage error.
    if (!call.world.empty() && !desc.worldScoped)
        return MakeError(call.command, "arg.inapplicable", call.command + " does not take --world",
                         call.command + " is not world-scoped; drop --world");
    const bool hostReadOnly = gate.readOnly.load(std::memory_order_acquire);
    if (access == Access::Drive && (hostReadOnly || call.readOnly))
        return MakeError(call.command, "permission.read-only",
                         call.command + " drives the host, and " +
                             (hostReadOnly ? "this host is read-only" : "this request is read-only"),
                         hostReadOnly ? "relaunch the host without ARC_REMOTE_READ_ONLY"
                                      : "drop --read-only to run a Drive command");
    const std::string state = gate.State();
    // Play is HostGate::inPlay (final fix pass, C-I3): the state shows only the
    // highest-ranked state, and a modal or a build in Play is still Play. A
    // gate set to "playing" or "paused" by hand (no session) reads as Play too.
    const bool inPlay = gate.inPlay.load(std::memory_order_acquire) || state == "playing" || state == "paused";
    if (desc.play == PlayPolicy::EditOnly && inPlay)
        return Busy(call.command, state, call.command + " runs in edit mode only; the host is in play mode (" + state + ")",
                    "stop play mode, then retry");
    if (desc.play == PlayPolicy::PlayingOnly && !inPlay)
        return Busy(call.command, state, call.command + " runs only while playing or paused; the host is " + state,
                    "enter play mode, then retry");
    if (desc.needsFrame && gate.minimized.load(std::memory_order_acquire))
        return Busy(call.command, "minimized", call.command + " needs a rendered frame; the host is minimized",
                    "restore the host window, then retry");
    return std::nullopt;
}

// Keeps the newest kOutKeepFiles regular files of `outDir` by last_write_time.
void PruneOut(const std::filesystem::path& outDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(outDir, ec))
        return;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    fs::directory_iterator it(outDir, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec))
    {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc))
            continue;
        const fs::file_time_type written = it->last_write_time(entryEc);
        if (entryEc)
            continue;
        files.emplace_back(written, it->path());
    }
    if (files.size() <= kOutKeepFiles)
        return;
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; }); // newest first
    for (std::size_t i = kOutKeepFiles; i < files.size(); ++i)
    {
        std::error_code removeEc;
        fs::remove(files[i].second, removeEc);
    }
}

} // namespace

RemoteDispatcher::RemoteDispatcher(RequestQueue& queue, CommandRegistry& registry, HostGate& gate,
                                   JobTable& jobs, std::filesystem::path auditPath)
    : m_queue(queue)
    , m_registry(registry)
    , m_gate(gate)
    , m_jobs(jobs)
    , m_auditPath(std::move(auditPath))
{
    m_jobs.SetQueue(&m_queue); // ruling F2: every job finish path can Complete its waiter
    const std::filesystem::path dir = m_auditPath.parent_path();
    if (!dir.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        PruneOut(dir / "out");
    }
}

void RemoteDispatcher::Drain(double budgetMs, std::uint32_t budgetCommands)
{
    m_lastDrainRan = 0;
    // 1. Each running job, at most one step; not charged against the command budget.
    m_jobs.StepAll(m_gate);

    // 2. Queued calls until the queue is empty, the command budget is spent, or
    // the time budget has elapsed (checked before each command starts).
    // The cvar declaration carries no minimum, so it is sanitized here: a
    // non-positive (or NaN) published value reads as the default, never as "drain nothing".
    const double setting = FrameBudgetMsSetting();
    const double budget = budgetMs > 0.0 ? budgetMs : (setting > 0.0 ? setting : kFrameBudgetMs);
    const std::uint32_t maxCommands = budgetCommands != 0 ? budgetCommands : kFrameBudgetCommands;
    const Clock::time_point start = Clock::now();
    while (m_lastDrainRan < maxCommands && MsBetween(start, Clock::now()) < budget)
    {
        std::uint64_t key = 0;
        Call call;
        Clock::time_point queuedAt{};
        if (!m_queue.TryPop(key, call, &queuedAt))
            break;
        RunOne(key, call, queuedAt, budget);
    }
}

std::uint32_t RemoteDispatcher::LastDrainRan() const
{
    return m_lastDrainRan;
}

void RemoteDispatcher::RunOne(std::uint64_t key, const Call& call, Clock::time_point queuedAt, double budgetMs)
{
    const Clock::time_point popped = Clock::now();
    const double queuedMs = queuedAt == Clock::time_point{} ? 0.0 : MsBetween(queuedAt, popped);

    // The effective access is computed once; the gate and the audit share it.
    // An unregistered command has no gate; Invoke answers command.unknown.
    std::optional<Envelope> refusal;
    bool drive = false;
    if (const Desc* desc = m_registry.Find(call.command))
    {
        const Access access = desc->accessFor != nullptr ? desc->accessFor(call) : desc->access;
        drive = access == Access::Drive;
        refusal = Gate(*desc, access, call, m_gate);
    }

    CommandResult result;
    double ranMs = 0.0;
    if (refusal)
    {
        result.envelope = std::move(*refusal);
        if (drive)
            Audit(call, result.envelope, true); // s10.4: a Drive call is audited allowed or refused
    }
    else
    {
        const Clock::time_point invoked = Clock::now();
        result = m_registry.Invoke(call);
        ranMs = MsBetween(invoked, Clock::now());
        ++m_lastDrainRan;
        if (ranMs > kSlowInvokeBudgets * budgetMs && m_slowWarned.insert(call.command).second)
            ARC_WARN("remote: {} took {:.1f} ms, over {}x the {:.1f} ms frame budget", call.command, ranMs,
                     kSlowInvokeBudgets, budgetMs);
        if (drive)
            Audit(call, result.envelope, false);
    }

    result.envelope.id = call.id;
    // A command may answer with a helper's envelope that names no command
    // (ResolveWorld's refusals, Task 19): the call's command is stamped here, once.
    if (result.envelope.command.empty())
        result.envelope.command = call.command;
    result.envelope.host = m_gate.Block();
    result.envelope.timing.queuedMs = queuedMs;
    result.envelope.timing.ranMs = ranMs;

    if (result.completesJob == 0)
    {
        Publish(key, std::move(result));
        return;
    }

    // A Job-thread command armed a job: the job's finish path (StepAll or
    // Finish) Completes this key with the job's final envelope (ruling F2).
    const JobId job{result.completesJob};
    const Job* row = m_jobs.Find(job); // main thread only (Task 9 carry 3)
    if (row != nullptr && row->state == JobState::Running && row->subscriberId == 0)
    {
        m_jobs.BindWaiter(job, key, result.envelope.undo);
        return;
    }
    // Never bind a waiter to a subscriber-cancellable job (Task 9 carry 2): a
    // cancel would leave the call parked forever, so it is answered with the
    // arming envelope. A job that already finished is answered now with its
    // own envelope; a missing job, with the arming one.
    if (row != nullptr && row->state != JobState::Running && row->state != JobState::Queued)
    {
        Envelope finished = row->envelope;
        if (!finished.undo)
            finished.undo = result.envelope.undo;
        finished.id = call.id;
        finished.host = m_gate.Block();
        finished.timing = result.envelope.timing;
        result.envelope = std::move(finished);
    }
    result.completesJob = 0;
    Publish(key, std::move(result));
}

void RemoteDispatcher::Publish(std::uint64_t key, CommandResult result)
{
    m_queue.Complete(key, std::move(result));
}

void RemoteDispatcher::Audit(const Call& call, const Envelope& result, bool refused)
{
    // s10.4: one ndjson line per Drive call, allowed or refused: invoked
    // ("ok" / "error") or refused by the gate's policy before it ran
    // ("refused", with the gate's code: permission.*, arg.inapplicable, ...).
    // A host.busy refusal is not audited (coordinator ruling, Task 21 fix
    // round 1): it is transient, provably not run, and the client retries it,
    // so it would write one line per retry. Observe calls are not audited. The
    // args are never written; argsDigest is SHA-256 of their canonical
    // (sorted-key) dump.
    if (refused && !result.errors.empty() && result.errors.front().code == "host.busy")
        return;
    nlohmann::ordered_json line;
    line["time"] = NowUnixMs();
    line["client"] = call.callerId;
    line["pid"] = m_gate.pid;
    line["command"] = call.command;
    line["argsDigest"] =
        Crypto::Sha256Hex(call.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    line["outcome"] = refused ? "refused" : (result.ok ? "ok" : "error");
    line["errorCode"] = !result.ok && !result.errors.empty() ? result.errors.front().code : std::string();
    line["frame"] = m_gate.frame.load(std::memory_order_acquire);

    std::ofstream out(m_auditPath, std::ios::binary | std::ios::app);
    if (out)
        out << line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    if (!out && !m_auditWarned)
    {
        m_auditWarned = true;
        ARC_WARN("remote: cannot append to the audit log {}", m_auditPath.string());
    }
}

} // namespace Arcane::Remote

#endif
