#include <catch2/catch_test_macros.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Discover.hpp>
#include <Servitor/Launch.hpp>
#include <Servitor/Verbs.hpp>
#include <Arcane/Platform/Process.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
#include "RemoteTestHttp.hpp"   // UnusedLoopbackPort
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#if !defined(ARC_BUILD_DIST)

namespace
{
// <root>/Binaries/<SERVITOR_OUTPUTDIR>/<Host>/<Host>[.exe]: where HostExePath
// looks for a source-checkout engine root (no sdk/sdk.json; rev 8.3 V7).
std::filesystem::path FakeHostExe(const std::filesystem::path& root, const char* host)
{
    std::filesystem::path exe = root / "Binaries" / SERVITOR_OUTPUTDIR / host / host;
#if defined(_WIN32)
    exe += ".exe";
#endif
    return exe;
}

std::filesystem::path MakeProject(const std::filesystem::path& dir)
{
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "Fake.arcproj") << "{}";
    return dir;
}

bool Contains(const std::vector<std::string>& argv, const std::string& token)
{
    for (const auto& a : argv)
        if (a == token)
            return true;
    return false;
}
} // namespace

TEST_CASE("launch with an unresolvable --engine spawns nothing", "[servitor][client]") {
    Arcane::Test::TempDir root("servitor-launch-empty");
    Servitor::GlobalOptions opt;
    opt.engine = root.path.string();
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::HostExePath(Arcane::Remote::HostKind::Editor, opt, &err).has_value());
    REQUIRE_FALSE(err.errors.empty());
    CHECK(err.errors[0].code == "engine.unresolved");
    CHECK(err.errors[0].message.find(Servitor::PathUtf8(root.path)) != std::string::npos);
    Arcane::Remote::Envelope out;
    CHECK(Servitor::Launch(Arcane::Remote::HostKind::Editor, opt, {}, &out) == 4);
    REQUIRE_FALSE(out.errors.empty());
    CHECK(out.errors[0].code == "engine.unresolved");
    CHECK(Arcane::Remote::ExitFromHost(out) == 4);
}

TEST_CASE("launch plans --project and --remote and passes the host flags through verbatim", "[servitor][client]") {
    Arcane::Test::TempDir tmp("servitor-launch-plan");
    const auto exe = FakeHostExe(tmp.path / "engine", "ArcaneRuntime");
    std::filesystem::create_directories(exe.parent_path());
    std::ofstream(exe) << "not a program";   // PlanLaunch checks the path; it spawns nothing
    const auto project = MakeProject(tmp.path / "Game");

    Servitor::GlobalOptions opt;
    opt.engine = (tmp.path / "engine").string();
    opt.project = project.string();
    const std::vector<std::string> hostArgs = {"--headless", "--frames", "0", "--note", "a b \"c\""};
    Arcane::Remote::Envelope err;
    const auto plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, hostArgs, &err);
    REQUIRE(plan.has_value());
    CHECK(plan->exe == exe);
    // One log per launch: opened under servitor's pid, renamed to the child's after the spawn. It lives
    // in the project's Saved/Remote/logs, never beside the host exe in the engine tree (spec s4.7).
    CHECK(plan->logDir == Arcane::Remote::DescriptorDir(std::filesystem::absolute(project), false) / "logs");
    CHECK(plan->log.parent_path() == plan->logDir);
    CHECK(plan->log.parent_path() != exe.parent_path());
    CHECK(plan->log.filename().string().starts_with("ArcaneRuntime.servitor-launch-"));
    CHECK(plan->projectRoot == std::filesystem::absolute(project));
    CHECK(plan->descriptorDir == Arcane::Remote::DescriptorDir(std::filesystem::absolute(project), false));
    CHECK(plan->descriptor.empty());   // <descriptorDir>/runtime-<pid>.json once the pid is known
    CHECK(plan->hideConsole);          // --headless
    const std::vector<std::string> expected = {"--project", Servitor::PathUtf8(std::filesystem::absolute(project)),
                                               "--headless", "--frames", "0", "--note", "a b \"c\"", "--remote"};
    CHECK(plan->argv == expected);

    // --remote-descriptor among the host flags names the exact descriptor to
    // wait for (made absolute: the host runs in its own folder); --remote is
    // not added twice.
    const std::vector<std::string> withDescriptor = {"--remote", "--remote-descriptor", "d/runtime.json"};
    const auto named = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, withDescriptor, &err);
    REQUIRE(named.has_value());
    CHECK(named->descriptor == std::filesystem::absolute("d/runtime.json"));
    CHECK(named->logDir == std::filesystem::absolute("d") / "logs");   // beside its descriptor
    CHECK(named->log.parent_path() == named->logDir);
    CHECK_FALSE(named->hideConsole);   // a windowed runtime keeps its console flags
    const std::vector<std::string> expectedNamed = {"--project", Servitor::PathUtf8(std::filesystem::absolute(project)),
                                                    "--remote", "--remote-descriptor",
                                                    Servitor::PathUtf8(std::filesystem::absolute("d/runtime.json"))};
    CHECK(named->argv == expectedNamed);

    // Projectless: the projectless editor dir's Remote/logs (the descriptor family's own directory).
    {
        // No --project and a working directory outside any project (restored on exit).
        const auto previous = std::filesystem::current_path();
        std::filesystem::create_directories(tmp.path / "elsewhere");
        std::filesystem::current_path(tmp.path / "elsewhere");
        Servitor::GlobalOptions bare;
        bare.engine = opt.engine;
        const auto projectless = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, bare, {}, &err);
        std::filesystem::current_path(previous);
        REQUIRE(projectless.has_value());
        CHECK(projectless->projectRoot.empty());
        CHECK(projectless->logDir == Arcane::Remote::DescriptorDir({}, true) / "logs");
        CHECK(projectless->log.parent_path() == projectless->logDir);
    }

    // --project both before and after `--` is ambiguous: refused, nothing spawned.
    const std::vector<std::string> twice = {"--project", project.string()};
    Arcane::Remote::Envelope refused;
    CHECK_FALSE(Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, twice, &refused).has_value());
    CHECK(refused.errors[0].code == "arg.invalid");
    CHECK(Arcane::Remote::ExitFromHost(refused) == 2);

    // A missing exe under a resolved root is engine.unresolved naming the path tried.
    Arcane::Remote::Envelope missing;
    CHECK_FALSE(Servitor::HostExePath(Arcane::Remote::HostKind::Server, opt, &missing).has_value());
    CHECK(missing.errors[0].code == "engine.unresolved");
    CHECK(missing.errors[0].message.find("ArcaneServer") != std::string::npos);
}

TEST_CASE("a launched host that exits before it is ready is host.died exit 7, and its log stays", "[servitor][client]") {
    Arcane::Test::TempDir tmp("servitor-launch-dies");
    const auto exe = FakeHostExe(tmp.path / "engine", "ArcaneServer");
    std::filesystem::create_directories(exe.parent_path());
    // A real program that exits at once whatever its arguments: cmd.exe with
    // the null device as stdin (Windows), false (POSIX).
#if defined(_WIN32)
    const char* systemRoot = std::getenv("SystemRoot");
    REQUIRE(systemRoot != nullptr);
    std::filesystem::copy_file(std::filesystem::path(systemRoot) / "System32" / "cmd.exe", exe);
#else
    const std::filesystem::path falseExe =
        std::filesystem::exists("/bin/false") ? std::filesystem::path("/bin/false") : std::filesystem::path("/usr/bin/false");
    std::filesystem::copy_file(falseExe, exe);
    std::filesystem::permissions(exe, std::filesystem::perms::owner_all);
#endif
    Servitor::GlobalOptions opt;
    opt.engine = (tmp.path / "engine").string();
    opt.project = MakeProject(tmp.path / "Game").string();
    opt.timeoutMs = 20000;
    Arcane::Remote::Envelope out;
    const auto started = std::chrono::steady_clock::now();
    CHECK(Servitor::Launch(Arcane::Remote::HostKind::Server, opt, {}, &out) == 7);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(15));   // noticed, not timed out
    REQUIRE_FALSE(out.errors.empty());
    CHECK(out.errors[0].code == "host.died");
    CHECK(out.command == "launch");
    const auto pid = out.errors[0].detail["pid"].get<std::uint32_t>();
    CHECK(pid != 0);
    const auto log = Arcane::Remote::DescriptorDir(std::filesystem::absolute(tmp.path / "Game"), false) / "logs" /
                     ("ArcaneServer.servitor-launch-" + std::to_string(pid) + ".log");
    CHECK(std::filesystem::exists(log));   // per launch, named for the child's pid, in the project
    CHECK_FALSE(std::filesystem::exists(exe.parent_path() / log.filename()));   // nothing in the engine tree
    CHECK(out.errors[0].detail["log"] == Servitor::PathUtf8(log));
    // spec s4.7: the exit code (observed on both platforms) and the last state seen.
    CHECK(out.errors[0].detail.contains("exitCode"));
    CHECK(out.errors[0].detail["lastState"] == "");   // it never wrote a descriptor
    CHECK_FALSE(out.errors[0].detail.contains("arcdiag"));   // no crash report was written
}

TEST_CASE("verb rows declare their flags; unknown ones are refused and launch passes -- through", "[servitor][client]") {
    const Servitor::VerbRow* launch = Servitor::FindVerb("launch");
    const Servitor::VerbRow* quit = Servitor::FindVerb("quit");
    const Servitor::VerbRow* wait = Servitor::FindVerb("wait");
    const Servitor::VerbRow* status = Servitor::FindVerb("status");
    REQUIRE(launch != nullptr);
    REQUIRE(quit != nullptr);
    REQUIRE(wait != nullptr);
    REQUIRE(status != nullptr);

    const std::vector<std::string> launchRest = {"runtime", "--headless", "--backend", "vulkan", "--",
                                                 "--frames", "3", "--json", "--bogus"};
    Servitor::VerbTokens tokens;
    CHECK_FALSE(Servitor::SplitVerbTokens(*launch, launchRest, &tokens).has_value());
    CHECK(tokens.positional == std::vector<std::string>{"runtime"});
    CHECK(tokens.passThrough == std::vector<std::string>{"--frames", "3", "--json", "--bogus"});
    nlohmann::json args;
    std::string error;
    REQUIRE(Servitor::LaunchArgs(launchRest, args, &error));
    CHECK(args["kind"] == "runtime");
    CHECK(args["hostArgs"] == nlohmann::json::array({"--headless", "--backend", "vulkan", "--frames", "3", "--json", "--bogus"}));
    CHECK_FALSE(Servitor::LaunchArgs(std::vector<std::string>{"--headless"}, args, &error));      // no kind
    CHECK_FALSE(Servitor::LaunchArgs(std::vector<std::string>{"client"}, args, &error));          // not a host kind
    CHECK_FALSE(Servitor::LaunchArgs(std::vector<std::string>{"runtime", "--backend"}, args, &error));   // a value flag needs its value

    const std::vector<std::string> unknownBeforeSeparator = {"runtime", "--bogus"};
    const auto problem = Servitor::SplitVerbTokens(*launch, unknownBeforeSeparator, nullptr);
    REQUIRE(problem.has_value());
    CHECK(problem->find("--bogus") != std::string::npos);
    // A bare `--` is only a separator for a row that passes tokens through.
    CHECK(Servitor::SplitVerbTokens(*status, std::vector<std::string>{"--"}, nullptr).has_value());
    CHECK(Servitor::SplitVerbTokens(*status, std::vector<std::string>{"--bogus"}, nullptr)->find("unknown flag --bogus") !=
          std::string::npos);

    // Final fix pass (C-M7): --force is the client's own (it ends the process
    // after --timeout); the host has no force path and refuses `force`, so the
    // posted args never carry it.
    REQUIRE(quit->args(std::vector<std::string>{"--force"}, args, &error));
    CHECK(args == nlohmann::json{{"discardChanges", false}});
    CHECK(Servitor::QuitForced(std::vector<std::string>{"--force"}));
    CHECK_FALSE(Servitor::QuitForced(std::vector<std::string>{"--discard-changes"}));
    REQUIRE(quit->args(std::vector<std::string>{"--discard-changes"}, args, &error));
    CHECK(args == nlohmann::json{{"discardChanges", true}});
    // quit --report stays refused (controller ruling); the problem says where --report goes.
    const auto report = Servitor::SplitVerbTokens(*quit, std::vector<std::string>{"--report", "r.json"}, nullptr);
    REQUIRE(report.has_value());
    CHECK(report->find("pass --report at launch: servitor launch <kind> -- --report <path>") != std::string::npos);
    CHECK_FALSE(quit->args(std::vector<std::string>{"now"}, args, &error));

    REQUIRE(wait->args(std::vector<std::string>{"frames", "10"}, args, &error));
    CHECK(args == nlohmann::json{{"what", "frames"}, {"frames", 10}});
    REQUIRE(wait->args(std::vector<std::string>{"state", "playing"}, args, &error));
    CHECK(args == nlohmann::json{{"what", "state"}, {"state", "playing"}});
    for (const char* what : {"ready", "settled", "reload"}) {
        REQUIRE(wait->args(std::vector<std::string>{what}, args, &error));
        CHECK(args == nlohmann::json{{"what", what}});
    }
    CHECK_FALSE(wait->args(std::vector<std::string>{}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"frames"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"frames", "x"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"frames", "-1"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"state"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"state", "stopping"}, args, &error));   // spec s4.3: shutting-down
    CHECK(error.find("shutting-down") != std::string::npos);
    REQUIRE(wait->args(std::vector<std::string>{"state", "compiling-shaders"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"soon"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"ready", "now"}, args, &error));
    // Only `wait ready` waits for a host to appear (spec s4.4); quit and the other waits do not.
    REQUIRE(wait->awaitsHost != nullptr);
    CHECK(wait->awaitsHost(nlohmann::json{{"what", "ready"}}));
    CHECK_FALSE(wait->awaitsHost(nlohmann::json{{"what", "settled"}}));
    CHECK(quit->awaitsHost == nullptr);
}

TEST_CASE("global flags after a bare -- belong to the host, not to servitor", "[servitor][client]") {
    std::vector<std::string> words = {"servitor", "--json", "launch", "runtime", "--", "--project", "X", "--timeout", "9"};
    std::vector<char*> argv;
    for (auto& w : words)
        argv.push_back(w.data());
    Servitor::GlobalOptions opt;
    std::string error;
    REQUIRE(Servitor::ParseGlobals(static_cast<int>(argv.size()), argv.data(), opt, &error));
    CHECK(opt.json);
    CHECK(opt.project.empty());
    CHECK(opt.timeoutMs == 30000);
}

TEST_CASE("quit --force terminates a host that does not answer, once --timeout is spent", "[servitor][client]") {
    // The "host": a live sleeper process behind a descriptor whose port no one
    // listens on, so host.quit can never be delivered (every connect is refused).
    Arcane::Test::TempDir tmp("servitor-quit-force");
    const auto project = MakeProject(tmp.path / "Game");
    Arcane::Remote::Descriptor desc;
#if defined(_WIN32)
    wchar_t sys[MAX_PATH];
    REQUIRE(::GetSystemDirectoryW(sys, MAX_PATH) > 0);
    std::wstring cmd = std::wstring(L"\"") + sys + L"\\ping.exe\" 127.0.0.1 -n 30";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    REQUIRE(::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi));
    ::CloseHandle(pi.hThread);
    FILETIME created{}, exited{}, kernel{}, user{};
    REQUIRE(::GetProcessTimes(pi.hProcess, &created, &exited, &kernel, &user));
    desc.pid = pi.dwProcessId;
    desc.processStart = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
#else
    std::string exe = "/bin/sleep";
    std::string arg = "30";
    char* argv[] = {exe.data(), arg.data(), nullptr};
    pid_t child = 0;
    REQUIRE(::posix_spawn(&child, exe.c_str(), nullptr, nullptr, argv, environ) == 0);
    desc.pid = static_cast<std::uint32_t>(child);
    for (int i = 0; i < 100 && desc.processStart == 0; ++i) {
        desc.processStart = Arcane::Platform::QueryProcess(desc.pid).start;
        if (desc.processStart == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(desc.processStart != 0);
#endif
    desc.hostKind = Arcane::Remote::HostKind::Runtime;
    desc.port = RemoteTest::UnusedLoopbackPort();
    desc.token = std::string(64, 'a');
    desc.project = Servitor::PathUtf8(project);
    desc.state = "ready";
    desc.heartbeatUnixMs = Servitor::NowUnixMs();
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(Arcane::Remote::DescriptorPath(project, desc.hostKind, desc.pid), desc));

    Servitor::GlobalOptions opt;
    opt.project = project.string();
    opt.format = Servitor::Format::Json;
    opt.timeoutMs = 1000;
    const Servitor::VerbRow* quit = Servitor::FindVerb("quit");
    REQUIRE(quit != nullptr);
    REQUIRE(quit->local != nullptr);
    const auto started = std::chrono::steady_clock::now();
    const int code = quit->local(std::vector<std::string>{"--force"}, opt);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(code == 0);                                             // the host is gone: exit 0, data.forced
    CHECK(elapsed >= std::chrono::milliseconds(900));             // it waited out --timeout first
    CHECK(elapsed < std::chrono::seconds(10));
    CHECK_FALSE(Arcane::Platform::IsProcessLive(desc.pid, desc.processStart));
    CHECK_FALSE(std::filesystem::exists(Arcane::Remote::DescriptorPath(project, desc.hostKind, desc.pid)));   // stale: removed
#if defined(_WIN32)
    if (::WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0)
        ::TerminateProcess(pi.hProcess, 1);   // never leave the sleeper behind
    ::CloseHandle(pi.hProcess);
#else
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
#endif
}

// Final fix pass (C-I6): a spawned host that is not ready by --timeout is still
// running, so the answer is never host.busy (exit 5: provably not run, retry
// is safe), which would invite a second launch beside it.
TEST_CASE("a launched host not ready by --timeout is launch.timeout exit 8 naming the running pid", "[servitor][client]") {
    auto env = Servitor::LaunchTimedOut(Arcane::Remote::HostKind::Runtime, 4242, 30000, "C:/p/logs/a.log",
                                              "C:/p/runtime-4242.json", "");
    REQUIRE_FALSE(env.errors.empty());
    CHECK(env.command == "launch");
    CHECK(env.errors[0].code == "launch.timeout");
    CHECK(Arcane::Remote::ExitFromHost(env) == 8);
    CHECK(env.errors[0].detail["pid"] == 4242);
    CHECK(env.errors[0].detail["state"] == "starting");
    CHECK(env.errors[0].detail["log"] == "C:/p/logs/a.log");
    CHECK(env.errors[0].detail["descriptor"] == "C:/p/runtime-4242.json");
    CHECK(env.errors[0].message.find("still running") != std::string::npos);
    CHECK(env.errors[0].remediation.find("servitor wait ready --pid 4242") != std::string::npos);
    CHECK(env.errors[0].remediation.find("servitor quit --pid 4242") != std::string::npos);
    CHECK(env.host.pid == 4242);
    CHECK(env.host.kind == "runtime");
    auto booting = Servitor::LaunchTimedOut(Arcane::Remote::HostKind::Editor, 7, 100, "l", "d", "booting");
    CHECK(booting.errors[0].detail["state"] == "booting");
}

TEST_CASE("omitted --client is not forwarded to the host, and an explicit one is", "[servitor][client]") {
    Arcane::Test::TempDir tmp("servitor-launch-client");
    const auto exe = FakeHostExe(tmp.path / "engine", "ArcaneRuntime");
    std::filesystem::create_directories(exe.parent_path());
    std::ofstream(exe) << "not a program";
    const auto project = MakeProject(tmp.path / "Game");
    const auto has = [](const std::vector<std::string>& argv, const std::string& token) {
        return std::find(argv.begin(), argv.end(), token) != argv.end();
    };

    const std::vector<std::string> headlessArgs = {"--headless"};
    const std::vector<std::string> ownClientArgs = {"--client", "mine"};
    Servitor::GlobalOptions opt;
    opt.engine = (tmp.path / "engine").string();
    opt.project = project.string();
    opt.client = Servitor::DefaultClientId();   // what ParseArgs stores when --client is omitted
    opt.clientSet = false;
    Arcane::Remote::Envelope err;
    auto plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, headlessArgs, &err);
    REQUIRE(plan.has_value());
    CHECK_FALSE(has(plan->argv, "--client"));
    CHECK_FALSE(has(plan->argv, Servitor::DefaultClientId()));

    opt.client = "grok-review-r1";
    opt.clientSet = true;
    plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, headlessArgs, &err);
    REQUIRE(plan.has_value());
    const auto at = std::find(plan->argv.begin(), plan->argv.end(), "--client");
    REQUIRE(at != plan->argv.end());
    REQUIRE(at + 1 != plan->argv.end());
    CHECK(*(at + 1) == "grok-review-r1");
    CHECK_FALSE(has(plan->argv, Servitor::DefaultClientId()));

    // B7 fix round 1 (M1): a --client after "--" that differs from servitor's
    // own is refused (a later quit --client grok-review-r1 could not find the host).
    plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, ownClientArgs, &err);
    CHECK_FALSE(plan.has_value());
    REQUIRE_FALSE(err.errors.empty());
    CHECK(err.errors.front().code == "arg.invalid");
    // Final fix pass (b7 m4): the --client=NAME spelling goes through the same check.
    const std::vector<std::string> ownClientEq = {"--client=mine"};
    err = {};
    plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, ownClientEq, &err);
    CHECK_FALSE(plan.has_value());
    REQUIRE_FALSE(err.errors.empty());
    CHECK(err.errors.front().code == "arg.invalid");
    const std::vector<std::string> sameClientEq = {"--client=grok-review-r1"};
    plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, sameClientEq, &err);
    REQUIRE(plan.has_value());
    CHECK(std::count_if(plan->argv.begin(), plan->argv.end(),
                        [](const std::string& a) { return a == "--client" || a.starts_with("--client="); }) == 1);
    // The same name typed twice is not doubled; without servitor's --client, the host's own is kept.
    const std::vector<std::string> sameClientArgs = {"--client", "grok-review-r1"};
    plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, sameClientArgs, &err);
    REQUIRE(plan.has_value());
    CHECK(std::count(plan->argv.begin(), plan->argv.end(), "--client") == 1);
    opt.client = Servitor::DefaultClientId();
    opt.clientSet = false;
    plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, ownClientArgs, &err);
    REQUIRE(plan.has_value());
    CHECK(std::count(plan->argv.begin(), plan->argv.end(), "--client") == 1);
    CHECK(has(plan->argv, "mine"));
}

TEST_CASE("launch passes --owner-pid through only when the operator typed it", "[servitor][client]") {
    Arcane::Test::TempDir tmp("servitor-launch-owner");
    const auto exe = FakeHostExe(tmp.path / "engine", "ArcaneRuntime");
    std::filesystem::create_directories(exe.parent_path());
    std::ofstream(exe) << "not a program";
    const auto project = MakeProject(tmp.path / "Game");
    Servitor::GlobalOptions opt;
    opt.engine = (tmp.path / "engine").string();
    opt.project = project.string();
    Arcane::Remote::Envelope err;
    const std::vector<std::string> none = {"--headless"};
    auto plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, none, &err);
    REQUIRE(plan.has_value());
    CHECK(std::find(plan->argv.begin(), plan->argv.end(), "--owner-pid") == plan->argv.end());
    const std::vector<std::string> owned = {"--headless", "--owner-pid", "4242"};
    plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, owned, &err);
    REQUIRE(plan.has_value());
    CHECK(std::count(plan->argv.begin(), plan->argv.end(), "--owner-pid") == 1);
    const auto at = std::find(plan->argv.begin(), plan->argv.end(), "--owner-pid");
    REQUIRE(at + 1 != plan->argv.end());
    CHECK(*(at + 1) == "4242");
}
#endif
