#include <catch2/catch_test_macros.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Format.hpp>
#include <Servitor/Verbs.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Http.hpp>        // MakeToken (Task 5)
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Base/Log.hpp>         // Log::Subscribe (Task 2)
#include <Arcane/Project/Project.hpp>  // EditorLock::Self (ArcaneEngine)
#include <Json.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if !defined(ARC_BUILD_DIST)

TEST_CASE("json render adds client and keeps the spec envelope", "[servitor][client]") {
    Arcane::Remote::HostBlock host;
    host.kind = "runtime";
    host.frame = 3;
    host.state = "ready";
    auto env = Arcane::Remote::MakeOk("cvar.get", nlohmann::json{{"value", false}}, host);
    env.command = "cvar.get";
    Arcane::Remote::ClientBlock client;
    client.exitCode = 0;
    client.descriptor = "Saved/Remote/runtime-1.json";
    Servitor::RenderOptions opt;
    opt.format = Servitor::Format::Json;
    const auto printed = Servitor::Render(env, client, opt);
    const auto j = nlohmann::json::parse(printed.stdoutText);
    CHECK(j["command"] == "cvar.get");
    CHECK(j["host"]["frame"] == 3);
    CHECK(j["data"]["value"] == false);
    CHECK(j["client"]["exitCode"] == 0);
    CHECK(j["client"]["descriptor"] == "Saved/Remote/runtime-1.json");
    CHECK_FALSE(j.contains("result"));
    CHECK_FALSE(j.contains("hostFrame"));
    CHECK(printed.stderrText.empty());
}

struct RemoteVerboseLog {
    std::mutex mu;
    std::string text;
};

static void RemoteVerboseSink(const Arcane::LogRecord& rec, void* user) {
    auto* log = static_cast<RemoteVerboseLog*>(user);
    std::lock_guard<std::mutex> lock(log->mu);
    log->text += rec.text;
    log->text.push_back('\n');
}

struct LeakyHost {
    std::string token;
};

// A host whose error message echoes the bearer back (the worst case Render must scrub).
static Servitor::ExchangeResult LeakyExchange(const std::string& body, void* user) {
    auto* host = static_cast<LeakyHost*>(user);
    const auto cmd = nlohmann::json::parse(body).at("command").get<std::string>();
    Servitor::ExchangeResult result;
    result.envelope = Arcane::Remote::MakeError(cmd, "auth.denied", "saw " + host->token, "check the descriptor");
    return result;
}

TEST_CASE("verbose and human output redact the bearer token", "[servitor][client]") {
    // Drives the real flags (ruling S3-4): --verbose and --format human through
    // ParseGlobals, then Call, which logs the request line and renders human.
    const char* argv[] = {"servitor", "--verbose", "--format", "human", "--no-retry", "cvar", "get", "x"};
    Servitor::GlobalOptions opt;
    std::string err;
    REQUIRE(Servitor::ParseGlobals(8, const_cast<char**>(argv), opt, &err));
    REQUIRE(opt.verbose);
    REQUIRE(opt.format == Servitor::Format::Human);
    LeakyHost host;
    host.token = Arcane::Remote::MakeToken();
    Arcane::Remote::ClientBlock client;
    client.token = host.token;   // Run puts the descriptor token here
    RemoteVerboseLog log;
    const auto sub = Arcane::Log::Subscribe(&RemoteVerboseSink, &log);
    Servitor::CallRequest req;
    req.command = "cvar.get";
    req.args = nlohmann::json{{"name", "x"}};
    const auto printed = Servitor::Call(req, opt, &LeakyExchange, &host, &client);
    Arcane::Log::Unsubscribe(sub);
    std::string logged;
    {
        std::lock_guard<std::mutex> lock(log.mu);
        logged = log.text;
    }
    CHECK(client.exitCode == 3);
    CHECK(logged.find("servitor request: POST /remote/v1/call") != std::string::npos);   // --verbose logged
    CHECK(logged.find("Bearer <redacted>") != std::string::npos);
    CHECK(logged.find(host.token) == std::string::npos);
    CHECK(printed.stdoutText.find(host.token) == std::string::npos);              // human output
    CHECK(printed.stderrText.find(host.token) == std::string::npos);
    CHECK(printed.stderrText.find("<redacted>") != std::string::npos);
    CHECK(printed.stderrText.find("auth.denied") != std::string::npos);
    CHECK(printed.stdoutText.find("\"ok\"") == std::string::npos);              // human, not JSON
}

TEST_CASE("--read-only is posted as options.readOnly", "[servitor][client]") {
    const char* argv[] = {"servitor", "--read-only", "cvar", "set", "x", "1"};
    Servitor::GlobalOptions opt;
    std::string err;
    REQUIRE(Servitor::ParseGlobals(6, const_cast<char**>(argv), opt, &err));
    CHECK(opt.readOnly);
    static std::string posted;
    posted.clear();
    Servitor::CallRequest req;
    req.command = "cvar.set";
    Servitor::Call(req, opt,
        [](const std::string& body, void*) {
            posted = body;
            Servitor::ExchangeResult r;
            r.envelope = Arcane::Remote::MakeOk("cvar.set", nlohmann::json::object(), {});
            return r;
        }, nullptr, nullptr);
    const auto j = nlohmann::json::parse(posted);
    CHECK(j["options"]["readOnly"] == true);
    Servitor::GlobalOptions plain;
    Servitor::Call(req, plain,
        [](const std::string& body, void*) {
            posted = body;
            Servitor::ExchangeResult r;
            r.envelope = Arcane::Remote::MakeOk("cvar.set", nlohmann::json::object(), {});
            return r;
        }, nullptr, nullptr);
    CHECK_FALSE(nlohmann::json::parse(posted)["options"].contains("readOnly"));
}

TEST_CASE("the verb table maps stats frame to stats.frame", "[servitor][client]") {
    const std::vector<std::string> tokens = {"stats", "frame"};
    std::size_t consumed = 0;
    const Servitor::VerbRow* row = Servitor::MatchVerb(tokens, &consumed);
    REQUIRE(row != nullptr);
    CHECK(std::string(row->command) == "stats.frame");
    CHECK(consumed == 2);
    const std::vector<std::string> unknown = {"no", "such"};
    CHECK(Servitor::MatchVerb(unknown, &consumed) == nullptr);
}

TEST_CASE("--json wins and github is refused", "[servitor][client]") {
    const char* argv[] = {"servitor", "--json", "status"};
    Servitor::GlobalOptions opt;
    std::string err;
    REQUIRE(Servitor::ParseGlobals(3, const_cast<char**>(argv), opt, &err));
    CHECK(opt.format == Servitor::Format::Json);
    const char* bad[] = {"servitor", "--format", "github", "status"};
    CHECK_FALSE(Servitor::ParseGlobals(4, const_cast<char**>(bad), opt, &err));
    CHECK(err.find("github") != std::string::npos);
    const char* once[] = {"servitor", "--no-retry", "status"};
    REQUIRE(Servitor::ParseGlobals(3, const_cast<char**>(once), opt, &err));
    CHECK(opt.noRetry);
    const char* timed[] = {"servitor", "--timeout", "2", "status"};
    REQUIRE(Servitor::ParseGlobals(4, const_cast<char**>(timed), opt, &err));
    CHECK(opt.timeoutMs == 2000);
    const char* half[] = {"servitor", "--timeout", "0.5", "status"};
    REQUIRE(Servitor::ParseGlobals(4, const_cast<char**>(half), opt, &err));
    CHECK(opt.timeoutMs == 500);
    const char* omitted[] = {"servitor", "status"};
    Servitor::GlobalOptions omittedOpt;
    REQUIRE(Servitor::ParseGlobals(2, const_cast<char**>(omitted), omittedOpt, &err));
    CHECK(omittedOpt.timeoutMs == 30000);
    const char* zero[] = {"servitor", "--timeout", "0", "status"};
    REQUIRE(Servitor::ParseGlobals(4, const_cast<char**>(zero), opt, &err));
    CHECK(opt.timeoutMs == 1);
    const char* negative[] = {"servitor", "--timeout", "-1", "status"};
    CHECK_FALSE(Servitor::ParseGlobals(4, const_cast<char**>(negative), opt, &err));
    CHECK(err.find("timeout") != std::string::npos);
    const char* words[] = {"servitor", "--timeout", "abc", "status"};
    CHECK_FALSE(Servitor::ParseGlobals(4, const_cast<char**>(words), opt, &err));
    CHECK(err.find("timeout") != std::string::npos);
}

TEST_CASE("ndjson ends with a result frame", "[servitor][client]") {
    auto env = Arcane::Remote::MakeOk("logs.read", nlohmann::json::object(), {});
    Servitor::RenderOptions opt;
    opt.format = Servitor::Format::Ndjson;
    const auto printed = Servitor::Render(env, {}, opt);
    CHECK(printed.stdoutText.find("\"type\":\"result\"") != std::string::npos);
    CHECK(printed.stdoutText.find("\"count\":1") != std::string::npos);
    CHECK(printed.stdoutText.find("\"envelope\"") != std::string::npos);
}

struct BusyHost {
    int posts = 0;
    int busyAnswers = 0;
    int busyForPosts = 0;
    std::chrono::steady_clock::time_point freeAt{};
    bool useFreeAt = false;
    std::string busyState;
};

static Servitor::ExchangeResult BusyExchange(const std::string& body, void* user) {
    auto* transport = static_cast<BusyHost*>(user);
    ++transport->posts;
    Servitor::ExchangeResult result;
    const auto cmd = nlohmann::json::parse(body).at("command").get<std::string>();
    const bool stillBusy = transport->posts <= transport->busyForPosts
        || (transport->useFreeAt && std::chrono::steady_clock::now() < transport->freeAt);
    if (stillBusy) {
        ++transport->busyAnswers;
        result.envelope = Arcane::Remote::MakeError(cmd, "host.busy", "one capture is pending", "retry");
        result.envelope.errors[0].detail = nlohmann::json{{"state", "capture-pending"}};
        transport->busyState = result.envelope.errors[0].detail["state"].get<std::string>();
        return result;
    }
    Arcane::Remote::HostBlock host;
    host.kind = "runtime";
    host.state = "ready";
    result.envelope = Arcane::Remote::MakeOk(cmd, nlohmann::json{{"width", 2}, {"height", 1}}, host);
    result.envelope.command = cmd;
    return result;
}

TEST_CASE("a busy host longer than 60 ms is retried until it frees", "[servitor][client]") {
    BusyHost transport;
    transport.busyForPosts = 3;
    transport.useFreeAt = true;
    transport.freeAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    Servitor::GlobalOptions opt;
    opt.format = Servitor::Format::Json;
    opt.noRetry = false;
    opt.timeoutMs = 30000;
    Servitor::CallRequest shot;
    shot.command = "capture.window";
    shot.args = nlohmann::json::object();
    const auto started = std::chrono::steady_clock::now();
    Arcane::Remote::ClientBlock client;
    const auto printed = Servitor::Call(shot, opt, &BusyExchange, &transport, &client);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const auto json = nlohmann::json::parse(printed.stdoutText);
    CHECK(json["ok"] == true);
    CHECK(json["command"] == "capture.window");
    CHECK(json["client"]["retried"] == true);
    CHECK(json["client"]["exitCode"] == 0);
    CHECK(client.retried == true);
    CHECK(client.exitCode == 0);
    CHECK(transport.busyAnswers >= 3);
    CHECK(transport.posts == transport.busyAnswers + 1);
    CHECK(transport.busyState == "capture-pending");
    CHECK(elapsedMs >= 60);
    CHECK(elapsedMs < 5000);

    opt.noRetry = true;
    transport.posts = 0;
    transport.busyAnswers = 0;
    transport.busyForPosts = 5;
    transport.useFreeAt = false;
    Arcane::Remote::ClientBlock refusedClient;
    const auto refused = Servitor::Call(shot, opt, &BusyExchange, &transport, &refusedClient);
    const auto refusedJson = nlohmann::json::parse(refused.stdoutText);
    CHECK(refusedClient.exitCode == 5);
    CHECK(refusedClient.retried == false);
    CHECK(transport.posts == 1);
    CHECK(transport.busyAnswers == 1);
    CHECK(refusedJson["ok"] == false);
    CHECK(refusedJson["errors"][0]["code"] == "host.busy");
    CHECK(refusedJson["errors"][0]["detail"]["state"] == "capture-pending");
    CHECK(refusedJson["client"]["exitCode"] == 5);
    CHECK(refusedJson["client"]["retried"] == false);
}

TEST_CASE("busy retries stop at --timeout", "[servitor][client]") {
    BusyHost transport;
    transport.busyForPosts = 100;
    Servitor::GlobalOptions opt;
    opt.format = Servitor::Format::Json;
    opt.noRetry = false;
    // The backoff pauses 10, 50, 100, 200, 400 ms, so 1000 ms fits posts at about
    // 0/10/60/160/360/760 ms. A 200 ms budget fit exactly 4 and failed on hosted
    // macOS, whose sleeps overshoot (timer coalescing); this leaves slack for that.
    opt.timeoutMs = 1000;
    Servitor::CallRequest shot;
    shot.command = "capture.window";
    const auto started = std::chrono::steady_clock::now();
    Arcane::Remote::ClientBlock client;
    const auto printed = Servitor::Call(shot, opt, &BusyExchange, &transport, &client);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const auto json = nlohmann::json::parse(printed.stdoutText);
    CHECK(json["ok"] == false);
    CHECK(client.exitCode == 5);
    CHECK(client.retried == true);
    CHECK(transport.posts >= 4);
    CHECK(transport.posts < 8);
    CHECK(elapsedMs >= 160);    // 4 posts slept 10 + 50 + 100 ms at least
    CHECK(elapsedMs < 1500);    // stops at the 1000 ms budget plus one fast exchange
    CHECK(json["errors"][0]["detail"]["state"] == "capture-pending");
}

struct UnavailableHost {
    int posts = 0;
};

static Servitor::ExchangeResult UnavailableExchange(const std::string& body, void* user) {
    auto* host = static_cast<UnavailableHost*>(user);
    ++host->posts;
    const auto cmd = nlohmann::json::parse(body).at("command").get<std::string>();
    Servitor::ExchangeResult result;
    result.envelope = Arcane::Remote::MakeError(cmd, "host.unavailable", "no capture hook installed", "");
    return result;
}

TEST_CASE("host.unavailable is exit 5 and is not retried", "[servitor][client]") {
    // Controller ruling (rev 8.1 follow-up): host.unavailable means a host-side hook
    // was never installed; it will not appear later, so the busy loop leaves it alone.
    UnavailableHost host;
    Servitor::GlobalOptions opt;
    opt.format = Servitor::Format::Json;
    opt.noRetry = false;
    opt.timeoutMs = 30000;
    Servitor::CallRequest shot;
    shot.command = "capture.window";
    Arcane::Remote::ClientBlock client;
    const auto printed = Servitor::Call(shot, opt, &UnavailableExchange, &host, &client);
    CHECK(client.exitCode == 5);
    CHECK(client.retried == false);
    CHECK(host.posts == 1);
    CHECK(nlohmann::json::parse(printed.stdoutText)["errors"][0]["code"] == "host.unavailable");
}

static Servitor::ExchangeResult ModalQuitExchange(const std::string& body, void* user) {
    auto* host = static_cast<UnavailableHost*>(user);
    ++host->posts;
    const auto cmd = nlohmann::json::parse(body).at("command").get<std::string>();
    Servitor::ExchangeResult result;
    result.envelope = Arcane::Remote::MakeError(cmd, "host.busy", "the host has unsaved changes",
                                                "save them first, or pass --discard-changes");
    result.envelope.errors.front().detail["state"] = "modal";
    return result;
}

TEST_CASE("host.quit's modal answer is exit 5 at once, not retried for --timeout", "[servitor][client]") {
    // Fix round 1: the unsaved-changes guard waits on a person, so the busy
    // loop answers it once, like host.unavailable.
    UnavailableHost host;
    Servitor::GlobalOptions opt;
    opt.format = Servitor::Format::Json;
    opt.timeoutMs = 30000;
    Servitor::CallRequest quit;
    quit.command = "host.quit";
    Arcane::Remote::ClientBlock client;
    const auto started = std::chrono::steady_clock::now();
    const auto printed = Servitor::Call(quit, opt, &ModalQuitExchange, &host, &client);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(client.exitCode == 5);
    CHECK(client.retried == false);
    CHECK(host.posts == 1);
    const auto json = nlohmann::json::parse(printed.stdoutText);
    CHECK(json["errors"][0]["detail"]["state"] == "modal");
    CHECK(json["errors"][0]["remediation"].get<std::string>().find("--discard-changes") != std::string::npos);

    // Any other command's modal busy keeps the retry (provably not run).
    UnavailableHost other;
    Servitor::CallRequest cvar;
    cvar.command = "cvar.set";
    opt.timeoutMs = 200;
    Arcane::Remote::ClientBlock otherClient;
    (void)Servitor::Call(cvar, opt, &ModalQuitExchange, &other, &otherClient);
    CHECK(otherClient.retried);
    CHECK(other.posts > 1);
}

static std::int64_t UnixMs(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

TEST_CASE("ObserveTarget reports host.hung with the newest later .arcdiag and host.died when the pid is gone", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-observe";
    std::filesystem::remove_all(root);
    const auto diag = root / "diagnostics";
    std::filesystem::create_directories(diag);
    Arcane::Remote::Descriptor d;
    d.protocol = 1;
    d.hostKind = Arcane::Remote::HostKind::Runtime;
    d.pid = Arcane::EditorLock::Self().pid;
    d.processStart = Arcane::EditorLock::Self().start;
    d.port = 9;
    d.state = "ready";
    d.heartbeatUnixMs = 5000;
    const auto descPath = root / "runtime.json";
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(descPath, d));
    Servitor::TargetWatch watch;
    watch.command = "cvar.get";
    watch.pid = d.pid;
    watch.processStart = d.processStart;
    watch.descriptorPath = descPath;
    watch.diagnosticsDir = diag;
    CHECK_FALSE(Servitor::ObserveTarget(watch, d.heartbeatUnixMs));
    CHECK_FALSE(Servitor::ObserveTarget(watch, d.heartbeatUnixMs + 10000));
    const auto now = std::chrono::system_clock::now();
    watch.requestUnixMs = UnixMs(now - std::chrono::minutes(30));
    auto hung = Servitor::ObserveTarget(watch, d.heartbeatUnixMs + 10001);
    REQUIRE(hung);
    CHECK(hung->command == "cvar.get");
    CHECK(hung->errors[0].code == "host.hung");
    CHECK(hung->errors[0].remediation == "relaunch");
    CHECK(Arcane::Remote::ExitFromHost(*hung) == 7);
    CHECK(hung->host.pid == d.pid);
    CHECK(hung->host.state == "ready");
    CHECK(hung->errors[0].detail["lastState"] == "ready");
    CHECK_FALSE(hung->errors[0].detail.contains("arcdiag"));

    const auto older = diag / "old.arcdiag";
    const auto newer = diag / "new.arcdiag";
    { std::ofstream(older) << "old"; }
    std::filesystem::last_write_time(older,
        Servitor::SysToFileTime(now - std::chrono::hours(1)));
    auto stillNone = Servitor::ObserveTarget(watch, d.heartbeatUnixMs + 10001);
    REQUIRE(stillNone);
    CHECK_FALSE(stillNone->errors[0].detail.contains("arcdiag"));
    { std::ofstream(newer) << "new"; }
    std::filesystem::last_write_time(newer, Servitor::SysToFileTime(now));
    auto withFile = Servitor::ObserveTarget(watch, d.heartbeatUnixMs + 10001);
    REQUIRE(withFile);
    CHECK(std::filesystem::equivalent(
        std::filesystem::path(withFile->errors[0].detail["arcdiag"].get<std::string>()), newer));

    const auto a = diag / "a.arcdiag";
    const auto m = diag / "m.arcdiag";
    { std::ofstream(a) << "a"; }
    { std::ofstream(m) << "m"; }
    const auto stamp = Servitor::SysToFileTime(now + std::chrono::seconds(2));
    std::filesystem::last_write_time(a, stamp);
    std::filesystem::last_write_time(m, stamp);
    auto tie = Servitor::ObserveTarget(watch, d.heartbeatUnixMs + 10001);
    REQUIRE(tie);
    CHECK(std::filesystem::equivalent(
        std::filesystem::path(tie->errors[0].detail["arcdiag"].get<std::string>()), m));

    watch.descriptorPath = root / "absent.json";
    auto unread = Servitor::ObserveTarget(watch, UnixMs(now));
    REQUIRE(unread);
    CHECK(unread->errors[0].code == "host.hung");
    CHECK(unread->errors[0].detail["lastState"] == "");

    Servitor::TargetWatch gone;
    gone.command = "cvar.get";
    gone.pid = 1;
    gone.processStart = Arcane::EditorLock::Self().start + 1;
    gone.descriptorPath = descPath;
    gone.diagnosticsDir = diag;
    gone.requestUnixMs = watch.requestUnixMs;
    auto died = Servitor::ObserveTarget(gone, d.heartbeatUnixMs + 10001);
    REQUIRE(died);
    CHECK(died->errors[0].code == "host.died");
    CHECK(Arcane::Remote::ExitFromHost(*died) == 7);
    CHECK_FALSE(died->errors[0].detail.contains("exitCode"));

#if defined(_WIN32)
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"C:\\Windows\\System32\\cmd.exe /c exit 3";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    REQUIRE(CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", mutableCmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    REQUIRE(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0);
    FILETIME created{}, exited{}, kernel{}, userTime{};
    REQUIRE(GetProcessTimes(pi.hProcess, &created, &exited, &kernel, &userTime));
    Servitor::TargetWatch exitedWatch;
    exitedWatch.command = "cvar.get";
    exitedWatch.pid = pi.dwProcessId;
    exitedWatch.processStart = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    exitedWatch.descriptorPath = root / "absent.json";
    exitedWatch.diagnosticsDir = diag;
    exitedWatch.requestUnixMs = UnixMs(now);
    auto coded = Servitor::ObserveTarget(exitedWatch, UnixMs(now));
    REQUIRE(coded);
    CHECK(coded->errors[0].code == "host.died");
    CHECK(coded->host.pid == pi.dwProcessId);
    CHECK(coded->errors[0].detail["exitCode"] == 3);
    CHECK(Arcane::Remote::ExitFromHost(*coded) == 7);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#endif
    std::filesystem::remove_all(root);
}

static nlohmann::json g_compatReply;
static Servitor::ExchangeResult CompatExchange(const std::string&, void*) {
    Servitor::ExchangeResult result;
    result.envelope = *Arcane::Remote::TryParseEnvelope(g_compatReply);
    return result;
}

TEST_CASE("servitor talks to newer minors and refuses unknown majors", "[servitor][client]") {
    Servitor::GlobalOptions opt;
    opt.noRetry = true;
    opt.format = Servitor::Format::Json;   // errors print on stdout in json; human sends them to stderr
    Servitor::CallRequest req;
    req.command = "stats.frame";
    // A newer minor with fields this client has never seen: still a success.
    g_compatReply = Arcane::Remote::ToJson(Arcane::Remote::MakeOk("stats.frame", nlohmann::json{{"frame", 3}}, {}));
    g_compatReply["protocolMinor"] = 9;
    g_compatReply["futureTopLevel"] = 1;
    Arcane::Remote::ClientBlock ok;
    Servitor::Call(req, opt, &CompatExchange, nullptr, &ok);
    CHECK(ok.exitCode == 0);
    // A major outside kProtocolMajors: protocol.mismatch, exit 4.
    g_compatReply["protocol"] = 2;
    Arcane::Remote::ClientBlock mismatch;
    const auto printed = Servitor::Call(req, opt, &CompatExchange, nullptr, &mismatch);
    CHECK(mismatch.exitCode == 4);
    CHECK(printed.stdoutText.find("protocol.mismatch") != std::string::npos);
    // A host without the command: command.unknown keeps exit 2 and names the host's major.minor.
    g_compatReply = Arcane::Remote::ToJson(Arcane::Remote::MakeError("stats.frame", "command.unknown", "no such command", ""));
    g_compatReply["protocolMinor"] = 4;
    Arcane::Remote::ClientBlock unknown;
    const auto missing = Servitor::Call(req, opt, &CompatExchange, nullptr, &unknown);
    CHECK(unknown.exitCode == 2);
    CHECK(missing.stdoutText.find("1.4") != std::string::npos);
    // Rev 8.3 V6: the remediation names both the command and the host's major.minor.
    const auto missingJson = nlohmann::json::parse(missing.stdoutText);
    const auto remediation = missingJson["errors"][0]["remediation"].get<std::string>();
    CHECK(remediation.find("stats.frame") != std::string::npos);
    CHECK(remediation.find("1.4") != std::string::npos);
}

TEST_CASE("servitor --version names its version and protocol", "[servitor][client]") {
    CHECK(std::string(Servitor::kVersion) == "0.1.0");
    CHECK(Servitor::kProtocolMajors.size() == 1);
    CHECK(Servitor::kProtocolMajors[0] == 1);
    CHECK(Servitor::VersionText() == "Servitor 0.1.0 (remote protocol 1)");
    const char* version[] = {"servitor", "--version"};
    CHECK(Servitor::Run(2, const_cast<char**>(version)) == 0);   // handled before ParseGlobals
    const char* argv[] = {"servitor", "--engine", "D:/some/root", "status"};
    Servitor::GlobalOptions opt;
    std::string err;
    REQUIRE(Servitor::ParseGlobals(4, const_cast<char**>(argv), opt, &err));
    CHECK(opt.engine == "D:/some/root");
    const char* bare[] = {"servitor", "--engine"};
    Servitor::GlobalOptions missingValue;
    CHECK_FALSE(Servitor::ParseGlobals(2, const_cast<char**>(bare), missingValue, &err));
}

#endif // !defined(ARC_BUILD_DIST)
