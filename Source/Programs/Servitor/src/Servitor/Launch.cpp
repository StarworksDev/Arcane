#include <Servitor/Launch.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Servitor/Discover.hpp>

#include <Arcane/Platform/LaunchPath.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Platform/Process.hpp>
#include <Arcane/Remote/Descriptor.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Servitor {

namespace fs = std::filesystem;

namespace {

constexpr VerbFlag kLaunchFlags[] = {
    {"--headless", false},           // forwarded to the host
    {"--backend", true},             // forwarded: dx12 | vulkan
    {"--remote-descriptor", true},   // forwarded; launch waits for exactly this file
};

const char* HostName(Arcane::Remote::HostKind kind)
{
    switch (kind) {
        case Arcane::Remote::HostKind::Editor: return "ArcaneEditor";
        case Arcane::Remote::HostKind::Runtime: return "ArcaneRuntime";
        case Arcane::Remote::HostKind::Server: return "ArcaneServer";
    }
    return "ArcaneRuntime";
}

bool ParseKind(std::string_view text, Arcane::Remote::HostKind& out)
{
    if (text == "editor") { out = Arcane::Remote::HostKind::Editor; return true; }
    if (text == "runtime") { out = Arcane::Remote::HostKind::Runtime; return true; }
    if (text == "server") { out = Arcane::Remote::HostKind::Server; return true; }
    return false;
}

fs::path FromUtf8(const std::string& text)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

bool Fail(std::string* error, std::string text)
{
    if (error != nullptr)
        *error = std::move(text);
    return false;
}

// The spawned child: its pid and the creation stamp Platform::IsProcessLive
// and the descriptor's processStart use (0 when the OS would not say). On
// Windows the process handle stays open until Launch's poll ends, so an exit
// before "ready" can report its exit code (spec s4.7); it closes with the
// last copy of this struct.
struct Child {
    std::uint32_t pid = 0;
    std::uint64_t start = 0;
#if defined(_WIN32)
    std::shared_ptr<void> process;
#endif
};

std::uint32_t SelfPid()
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

// <log dir>/<Host>.servitor-launch-<pid>.log: one log per launch, so two
// launches of the same host kind never write one file. The log dir is the
// descriptor's own directory + "logs" (spec s4.7), never the engine tree.
fs::path LaunchLogFor(const fs::path& logDir, Arcane::Remote::HostKind kind, std::uint32_t pid)
{
    return logDir / (std::string(HostName(kind)) + ".servitor-launch-" + std::to_string(pid) + ".log");
}

#if defined(_WIN32)
std::wstring Widen(const std::string& utf8)
{
    return FromUtf8(utf8).wstring();
}

std::string LastErrorText(DWORD code)
{
    wchar_t* text = nullptr;
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string out = "error " + std::to_string(code);
    if (n != 0 && text != nullptr) {
        std::wstring w(text, n);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' '))
            w.pop_back();
        out += ": " + PathUtf8(fs::path(w));
    }
    if (text != nullptr)
        ::LocalFree(text);
    return out;
}

// CreateProcessW, no shell (ruling E-2): the argv quoted token by token, the
// child's stdout and stderr on the truncated log, its stdin the null device,
// and ONLY those two handles inherited (a pipe servitor's own caller reads
// must not stay open in a host that outlives servitor). A headless host or
// the server gets CREATE_NO_WINDOW; a windowed host is DETACHED_PROCESS, so
// it never shares servitor's console (closing the terminal must not end it).
// Its working directory is its own folder (the RuntimeLaunch pattern). The
// thread handle closes at once; the process handle rides in Child until the
// poll ends (its exit code, s4.7).
std::optional<Child> Spawn(const LaunchPlan& plan, std::string* error)
{
    std::wstring cmd = Arcane::QuoteWindowsArg(plan.exe.wstring());
    for (const std::string& arg : plan.argv) {
        cmd.push_back(L' ');
        cmd += Arcane::QuoteWindowsArg(Widen(arg));
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    // Shared for writing too: a second launch of the same host kind truncates
    // and reuses the log while the first host still holds it open.
    const HANDLE log = ::CreateFileW(plan.log.c_str(), GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        Fail(error, "cannot open the launch log " + PathUtf8(plan.log) + " (" + LastErrorText(::GetLastError()) + ")");
        return std::nullopt;
    }
    const HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        Fail(error, "cannot open the null device (" + LastErrorText(::GetLastError()) + ")");
        ::CloseHandle(log);
        return std::nullopt;
    }

    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuffer(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    HANDLE inherit[2] = {nul, log};
    const bool listOk = ::InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                        ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                    sizeof(inherit), nullptr, nullptr);
    if (!listOk) {
        Fail(error, "cannot limit the inherited handles (" + LastErrorText(::GetLastError()) + ")");
        ::CloseHandle(nul);
        ::CloseHandle(log);
        return std::nullopt;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    si.lpAttributeList = attrs;
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP |
                        (plan.hideConsole ? CREATE_NO_WINDOW : DETACHED_PROCESS);
    const std::wstring workDir = plan.exe.parent_path().wstring();
    PROCESS_INFORMATION pi{};
    // lpCommandLine must be mutable: cmd is a local std::wstring.
    const BOOL ok = ::CreateProcessW(plan.exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                     workDir.c_str(), &si.StartupInfo, &pi);
    const DWORD spawnError = ::GetLastError();
    ::DeleteProcThreadAttributeList(attrs);
    ::CloseHandle(nul);   // the child holds its own duplicates
    ::CloseHandle(log);
    if (!ok) {
        Fail(error, "CreateProcessW failed (" + LastErrorText(spawnError) + ")");
        return std::nullopt;
    }

    Child child;
    child.pid = pi.dwProcessId;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (::GetProcessTimes(pi.hProcess, &created, &exited, &kernel, &user))
        child.start = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    ::CloseHandle(pi.hThread);
    child.process = std::shared_ptr<void>(pi.hProcess, [](void* h) { ::CloseHandle(static_cast<HANDLE>(h)); });
    return child;
}

// The open process handle answers both: signalled = exited, and its code.
bool StillRunning(const Child& child, std::optional<int>* exitCode)
{
    if (!child.process)
        return Arcane::Platform::IsProcessLive(child.pid, child.start);
    if (::WaitForSingleObject(static_cast<HANDLE>(child.process.get()), 0) != WAIT_OBJECT_0)
        return true;
    DWORD code = 0;
    if (exitCode != nullptr && ::GetExitCodeProcess(static_cast<HANDLE>(child.process.get()), &code))
        *exitCode = static_cast<int>(code);
    return false;
}
#else
// posix_spawn, no shell (ruling E-2): the child's fd 1 and 2 on the truncated
// log, fd 0 on /dev/null, in its own process group (a Ctrl-C at servitor's
// terminal does not reach it). It inherits servitor's working directory:
// posix_spawn has no portable chdir, and the hosts resolve their own data
// beside the executable.
std::optional<Child> Spawn(const LaunchPlan& plan, std::string* error)
{
    std::vector<std::string> storage;
    storage.reserve(plan.argv.size() + 1);
    storage.push_back(plan.exe.string());
    for (const std::string& arg : plan.argv)
        storage.push_back(arg);
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& s : storage)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    const std::string logPath = plan.log.string();
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, static_cast<short>(POSIX_SPAWN_SETPGROUP));
    posix_spawnattr_setpgroup(&attr, 0);

    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, storage.front().c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        Fail(error, std::string("posix_spawn failed (") + std::strerror(rc) + ")");
        return std::nullopt;
    }
    Child child;
    child.pid = static_cast<std::uint32_t>(pid);
    child.start = Arcane::Platform::QueryProcess(child.pid).start;
    return child;
}

// The child is servitor's own until servitor exits, so waitpid reaps it and
// reads its status (a zombie would otherwise still have a process entry).
bool StillRunning(const Child& child, std::optional<int>* exitCode)
{
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(child.pid), &status, WNOHANG);
    if (r == static_cast<pid_t>(child.pid)) {
        if (exitCode != nullptr && WIFEXITED(status))
            *exitCode = WEXITSTATUS(status);
        else if (exitCode != nullptr && WIFSIGNALED(status))
            *exitCode = 128 + WTERMSIG(status);
        return false;
    }
    if (r == 0)
        return true;
    // -1 (ECHILD: already reaped, or not ours after all): the process table answers.
    return Arcane::Platform::IsProcessLive(child.pid, child.start);
}
#endif

// The newest *.arcdiag written after the spawn, from the two places a host
// writes one: the open project's Saved/Diagnostics (Paths::DiagnosticsDir) and
// the exe-relative <host>/diagnostics (HostDiagnosticsDir; a death before the
// project opened). Empty when neither has one.
fs::path NewestLaunchReport(Arcane::Remote::HostKind kind, const GlobalOptions& opt, const fs::path& projectRoot,
                            std::int64_t spawnUnixMs)
{
    std::vector<fs::path> found;
    if (!projectRoot.empty()) {
        found.push_back(NewestReportAfter(
            Arcane::Paths::Resolve(Arcane::Paths::Location::DiagnosticsDir, Arcane::Paths::ForProject(projectRoot)),
            spawnUnixMs));
    }
    found.push_back(NewestReportAfter(HostDiagnosticsDir(kind, opt), spawnUnixMs));
    fs::path best;
    fs::file_time_type bestTime{};
    for (const fs::path& p : found) {
        std::error_code ec;
        if (p.empty())
            continue;
        const auto t = fs::last_write_time(p, ec);
        if (!ec && (best.empty() || t > bestTime)) {
            best = p;
            bestTime = t;
        }
    }
    return best;
}

} // namespace

std::span<const VerbFlag> LaunchFlags()
{
    return kLaunchFlags;
}

std::optional<fs::path> HostExePath(Arcane::Remote::HostKind kind, const GlobalOptions& opt,
                                    Arcane::Remote::Envelope* error)
{
    const auto dir = HostsDir(opt, error);   // engine.unresolved when no root resolves (rev 8.3 V7)
    if (!dir)
        return std::nullopt;
    fs::path exe = *dir / HostName(kind) / HostName(kind);
#if defined(_WIN32)
    exe += ".exe";
#endif
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) {
        if (error != nullptr)
            *error = Arcane::Remote::MakeError("", "engine.unresolved",
                                               std::string("no ") + HostName(kind) + " at " + PathUtf8(exe),
                                               "build the engine's hosts, or pass --engine <root> or set SERVITOR_ENGINE");
        return std::nullopt;
    }
    return exe;
}

std::optional<LaunchPlan> PlanLaunch(Arcane::Remote::HostKind kind, const GlobalOptions& opt,
                                     std::span<const std::string> hostArgs, Arcane::Remote::Envelope* error)
{
    const auto exe = HostExePath(kind, opt, error);
    if (!exe)
        return std::nullopt;
    const Arcane::LaunchPathStatus status = Arcane::CheckLaunchPath(*exe);
    if (status != Arcane::LaunchPathStatus::Ok) {
        if (error != nullptr)
            *error = Arcane::Remote::MakeError("", "engine.unresolved",
                                               "cannot launch " + PathUtf8(*exe) + ": the path " +
                                                   std::string(Arcane::LaunchPathStatusText(status)),
                                               "pass --engine <root> naming a built engine");
        return std::nullopt;
    }

    LaunchPlan plan;
    plan.exe = *exe;
    std::vector<std::string> argv(hostArgs.begin(), hostArgs.end());
    bool hasRemote = false;
    bool hasClient = false;
    bool headless = false;
    std::optional<std::size_t> projectAt;
    std::optional<std::size_t> descriptorAt;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (argv[i] == "--remote") {
            hasRemote = true;
        } else if (argv[i] == "--client" || argv[i].starts_with("--client=")) {
            hasClient = true;
            // M1 (B7 fix round 1): a host --client that differs from servitor's
            // own would hide the host from a later `quit --client <servitor's>`.
            // Cli takes --client=NAME too (final fix pass, b7 m4).
            const bool joined = argv[i] != "--client";
            const bool same = joined ? argv[i].substr(std::string_view("--client=").size()) == opt.client
                                     : i + 1 < argv.size() && argv[i + 1] == opt.client;
            if (opt.clientSet && !same) {
                if (error != nullptr)
                    *error = Arcane::Remote::MakeError("", "arg.invalid",
                                                       "--client is given both to servitor (" + opt.client +
                                                           ") and after -- with another value",
                                                       "pass --client once, before the verb");
                return std::nullopt;
            }
        } else if (argv[i] == "--headless") {
            headless = true;
        } else if (argv[i] == "--project" && i + 1 < argv.size()) {
            projectAt = ++i;
        } else if (argv[i] == "--remote-descriptor" && i + 1 < argv.size()) {
            descriptorAt = ++i;
        }
    }

    // The host's project decides where its descriptor lands, so launch resolves it the same way.
    std::optional<fs::path> root;
    if (projectAt) {
        if (!opt.project.empty()) {
            if (error != nullptr)
                *error = Arcane::Remote::MakeError("", "arg.invalid",
                                                   "--project is given both to servitor and after --",
                                                   "pass --project once, before the verb");
            return std::nullopt;
        }
        GlobalOptions hostProject = opt;
        hostProject.project = argv[*projectAt];
        root = ProjectRootFor(hostProject, error);
        if (!root)
            return std::nullopt;
        argv[*projectAt] = PathUtf8(fs::absolute(FromUtf8(argv[*projectAt])));
    } else {
        root = ProjectRootFor(opt, error);
        if (!root)
            return std::nullopt;
    }
    plan.projectRoot = *root;
    plan.descriptorDir = root->empty() ? Arcane::Remote::DescriptorDir({}, true)
                                       : Arcane::Remote::DescriptorDir(*root, false);
    if (descriptorAt) {
        std::error_code ec;
        plan.descriptor = fs::absolute(FromUtf8(argv[*descriptorAt]), ec);
        argv[*descriptorAt] = PathUtf8(plan.descriptor);
    }
    // The log stays beside its descriptor, in a logs/ folder (spec s4.7): the
    // engine tree is the Hub's to verify and holds no Servitor files. Opened
    // under this servitor's pid; Launch renames it to the child's pid once the
    // child exists (LaunchLogFor), so concurrent launches never share one.
    plan.logDir = (descriptorAt ? plan.descriptor.parent_path() : plan.descriptorDir) / "logs";
    plan.log = plan.logDir / (std::string(HostName(kind)) + ".servitor-launch-starting-" +
                              std::to_string(SelfPid()) + ".log");
    if (!projectAt && !root->empty())
        argv.insert(argv.begin(), {"--project", PathUtf8(*root)});
    if (!hasRemote)
        argv.emplace_back("--remote");
    // The operator's --client NAME reaches the host so its descriptor records it.
    // Only an explicit one: opt.client holds DefaultClientId() otherwise, which
    // would stamp every host with "servitor/<version> pid N". A --client after
    // "--" is the operator's own and is left alone.
    if (opt.clientSet && !hasClient) {
        argv.emplace_back("--client");
        argv.emplace_back(opt.client);
    }
    plan.argv = std::move(argv);
    plan.hideConsole = headless || kind == Arcane::Remote::HostKind::Server;
    return plan;
}

Arcane::Remote::Envelope LaunchTimedOut(Arcane::Remote::HostKind kind, std::uint32_t pid, std::uint32_t timeoutMs,
                                        const fs::path& log, const fs::path& descriptor, const std::string& lastState)
{
    // Not host.busy (exit 5: provably not run, retry is safe): a process IS
    // running, and a second launch would fight it for the project (final fix
    // pass, C-I6). launch.timeout is exit 8, a check that did not pass in time.
    const std::string host = std::string(HostName(kind)) + " (pid " + std::to_string(pid) + ")";
    const std::string pidText = std::to_string(pid);
    auto env = Arcane::Remote::MakeError("launch", "launch.timeout",
                                         host + " did not report ready within " + std::to_string(timeoutMs) +
                                             " ms; it is still running",
                                         "do not relaunch: wait for it with servitor wait ready --pid " + pidText +
                                             ", or stop it with servitor quit --pid " + pidText +
                                             " (add --force if it does not answer); its output is in " +
                                             PathUtf8(log));
    auto& detail = env.errors.front().detail;
    detail["pid"] = pid;
    detail["log"] = PathUtf8(log);
    detail["descriptor"] = PathUtf8(descriptor);
    detail["state"] = lastState.empty() ? std::string("starting") : lastState;
    env.host.pid = pid;
    env.host.kind = Arcane::Remote::ToString(kind);
    return env;
}

int Launch(Arcane::Remote::HostKind kind, const GlobalOptions& opt, std::span<const std::string> hostArgs,
           Arcane::Remote::Envelope* out)
{
    Arcane::Remote::Envelope local;
    if (out == nullptr)
        out = &local;

    Arcane::Remote::Envelope error;
    const auto plan = PlanLaunch(kind, opt, hostArgs, &error);   // spawns nothing when it fails
    if (!plan) {
        error.command = "launch";
        *out = std::move(error);
        return Arcane::Remote::ExitFromHost(*out);
    }

    {
        std::error_code ec;
        fs::create_directories(plan->logDir, ec);   // Spawn reports a log it still cannot open
    }
    std::string spawnError;
    const std::int64_t spawnUnixMs = NowUnixMs();
    const auto child = Spawn(*plan, &spawnError);
    if (!child) {
        std::error_code ec;
        fs::remove(plan->log, ec);   // the never-used launch log
        *out = Arcane::Remote::MakeError("launch", "internal", "could not start " + PathUtf8(plan->exe) + ": " + spawnError,
                                         "check that the host binary runs on this machine");
        return Arcane::Remote::ExitFromHost(*out);
    }

    const fs::path descriptor = !plan->descriptor.empty()
                                    ? plan->descriptor
                                    : plan->descriptorDir / (std::string(Arcane::Remote::ToString(kind)) + "-" +
                                                             std::to_string(child->pid) + ".json");
    const std::string host = std::string(HostName(kind)) + " (pid " + std::to_string(child->pid) + ")";
    // One log per launch: the child already writes the file, and it was
    // opened shareable for delete, so it can take its pid name now. A failed
    // rename leaves the starting name, which the envelope then reports.
    fs::path log = plan->log;
    {
        const fs::path named = LaunchLogFor(plan->logDir, kind, child->pid);
        std::error_code ec;
        fs::remove(named, ec);   // an older launch's log under a recycled pid
        fs::rename(plan->log, named, ec);
        if (!ec)
            log = named;
    }
    const auto fillDetail = [&](Arcane::Remote::Envelope& env) {
        auto& detail = env.errors.front().detail;
        detail["pid"] = child->pid;
        detail["log"] = PathUtf8(log);
        detail["descriptor"] = PathUtf8(descriptor);
    };

    // Poll that exact descriptor (never a scan: another host of the same kind
    // may be booting beside this one) until it names this process, the process
    // is live (IsStale), and it reads "ready".
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt.timeoutMs);
    std::string lastState;
    for (;;) {
        if (const auto desc = Arcane::Remote::ReadDescriptor(descriptor);
            desc && desc->pid == child->pid &&
            (child->start == 0 || desc->processStart == 0 || desc->processStart == child->start)) {
            lastState = desc->state;
            if (!Arcane::Remote::IsStale(*desc) && Arcane::Remote::IsReadyState(desc->state)) {
                Arcane::Remote::HostBlock block;
                block.kind = Arcane::Remote::ToString(kind);
                block.pid = desc->pid;
                block.state = desc->state;
                block.frame = desc->frame;
                *out = Arcane::Remote::MakeOk("launch",
                                              {{"pid", desc->pid},
                                               {"port", desc->port},
                                               {"descriptor", PathUtf8(descriptor)},
                                               {"hostKind", Arcane::Remote::ToString(kind)},
                                               {"log", PathUtf8(log)},
                                               {"exe", PathUtf8(plan->exe)}},
                                              std::move(block));
                return 0;
            }
        }
        std::optional<int> exitCode;
        if (!StillRunning(*child, &exitCode)) {
            *out = Arcane::Remote::MakeError("launch", "host.died",
                                             host + " exited before it was ready; its output is in " +
                                                 PathUtf8(log),
                                             "read the log, then fix the host flags or the project");
            fillDetail(*out);
            // What s4.7 asks a death to name: the exit code if observed, the
            // last state seen, and a crash report written after the spawn.
            auto& detail = out->errors.front().detail;
            if (exitCode)
                detail["exitCode"] = *exitCode;
            detail["lastState"] = lastState;
            if (const fs::path report = NewestLaunchReport(kind, opt, plan->projectRoot, spawnUnixMs); !report.empty())
                detail["arcdiag"] = PathUtf8(report);
            out->host.pid = child->pid;
            out->host.kind = Arcane::Remote::ToString(kind);
            return Arcane::Remote::ExitFromHost(*out);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *out = LaunchTimedOut(kind, child->pid, opt.timeoutMs, log, descriptor, lastState);
            return Arcane::Remote::ExitFromHost(*out);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kLaunchPollMs));
    }
}

bool LaunchArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    const VerbRow* row = FindVerb("launch");
    if (row == nullptr)
        return Fail(error, "launch is not in the verb table");
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*row, rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.size() != 1)
        return Fail(error, tokens.positional.empty()
                               ? "launch needs a host kind: editor, runtime or server"
                               : "launch takes one host kind; pass host flags after --, got " + tokens.positional[1]);
    Arcane::Remote::HostKind kind{};
    if (!ParseKind(tokens.positional.front(), kind))
        return Fail(error, "launch needs editor, runtime or server, got '" + tokens.positional.front() + "'");
    nlohmann::json hostArgs = nlohmann::json::array();
    for (const auto& [flag, value] : tokens.flags) {
        hostArgs.push_back(flag);
        if (flag != "--headless")   // the only switch; --backend and --remote-descriptor carry a value
            hostArgs.push_back(value);
    }
    for (const std::string& token : tokens.passThrough)
        hostArgs.push_back(token);
    out = {{"kind", tokens.positional.front()}, {"hostArgs", std::move(hostArgs)}};
    return true;
}

int LaunchVerb(std::span<const std::string> rest, const GlobalOptions& opt)
{
    Arcane::Remote::ClientBlock client;
    nlohmann::json args;
    std::string error;
    if (!LaunchArgs(rest, args, &error))
        return PrintUsage(error, opt.format);
    Arcane::Remote::HostKind kind{};
    ParseKind(args["kind"].get<std::string>(), kind);
    std::vector<std::string> hostArgs;
    for (const auto& token : args["hostArgs"])
        hostArgs.push_back(token.get<std::string>());
    Arcane::Remote::Envelope env;
    client.exitCode = Launch(kind, opt, hostArgs, &env);
    if (env.ok && env.data.contains("descriptor"))
        client.descriptor = env.data["descriptor"].get<std::string>();
    return PrintEnvelope(env, client, opt);
}

} // namespace Servitor

#endif
