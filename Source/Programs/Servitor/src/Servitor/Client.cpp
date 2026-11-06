#include <Servitor/Client.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Servitor/Discover.hpp>
#include <Servitor/FanOut.hpp>
#include <Servitor/Transport.hpp>
#include <Servitor/Verbs.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Platform/Process.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Redact.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
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
#include <io.h>     // _isatty, _fileno
#else
#include <signal.h> // sigaction (InterruptScope)
#include <unistd.h> // isatty, STDOUT_FILENO
#endif

namespace Servitor {

namespace {

ARC_CONSTANT("timing: how often an awaitsHost verb (wait) rescans for a host that has not written its descriptor yet")
constexpr int kDiscoveryPollMs = 100;

std::uint32_t ThisProcessId()
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

bool StdoutIsTty()
{
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

bool Fail(std::string* error, std::string text)
{
    if (error != nullptr)
        *error = std::move(text);
    return false;
}

// --timeout <s>: seconds, decimal allowed (spec s4.6). The whole token must
// parse; the rounded millisecond count must fit u32; anything that rounds
// below 1 ms is stored as 1.
bool ParseTimeoutSeconds(const char* text, std::uint32_t& outMs, std::string* error)
{
    const std::string bad = std::string("--timeout needs a non-negative number of seconds, got '") + text + "'";
    if (*text == '\0')
        return Fail(error, bad);
    errno = 0;
    char* end = nullptr;
    const double seconds = std::strtod(text, &end);
    if (end != text + std::strlen(text) || errno == ERANGE || !std::isfinite(seconds) || seconds < 0.0)
        return Fail(error, bad);
    const double ms = seconds * 1000.0;
    // Range first: llround of a value outside long long is undefined. A value
    // at or past max + 0.5 rounds above the u32 maximum.
    if (!(ms < static_cast<double>(std::numeric_limits<std::uint32_t>::max()) + 0.5))
        return Fail(error, "--timeout '" + std::string(text) + "' is too large (the limit is " +
                               std::to_string(std::numeric_limits<std::uint32_t>::max() / 1000u) + " seconds)");
    outMs = std::max(1u, static_cast<std::uint32_t>(std::llround(ms)));
    return true;
}

bool ParseFormat(std::string_view text, Format& out, std::string* error)
{
    if (text == "json") { out = Format::Json; return true; }
    if (text == "human") { out = Format::Human; return true; }
    if (text == "ndjson") { out = Format::Ndjson; return true; }
    return Fail(error, "unknown format " + std::string(text) + " (json, human or ndjson)");
}

bool ParsePid(const char* text, std::uint32_t& out, std::string* error)
{
    const std::string bad = std::string("--pid needs a process id, got '") + text + "'";
    if (*text < '0' || *text > '9')
        return Fail(error, bad);
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (end != text + std::strlen(text) || errno == ERANGE || v == 0 ||
        v > std::numeric_limits<std::uint32_t>::max())
        return Fail(error, bad);
    out = static_cast<std::uint32_t>(v);
    return true;
}

// The global flags may appear anywhere; every other token is the verb's, in order.
bool ParseArgs(int argc, char** argv, GlobalOptions& out, std::string* error, std::vector<std::string>* rest)
{
    out = GlobalOptions{};
    out.tty = StdoutIsTty();
    bool formatSet = false;
    Format format = Format::Json;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        // A bare "--" ends servitor's own flags: it and every token after it
        // belong to the verb, verbatim (launch's host flags, Task 16), so a
        // host's --project or --timeout is never read as servitor's.
        if (flag == "--") {
            if (rest != nullptr) {
                for (int j = i; j < argc; ++j)
                    rest->emplace_back(argv[j]);
            }
            break;
        }
        // A flag that takes a value reads the next token, which must exist and
        // must not itself be a flag (--project --json); a single '-' is a value.
        const auto value = [&](const char*& v) {
            if (i + 1 >= argc)
                return Fail(error, std::string(flag) + " needs a value");
            if (std::string_view(argv[i + 1]).starts_with("--"))
                return Fail(error, std::string(flag) + " needs a value, got the flag " + argv[i + 1]);
            v = argv[++i];
            return true;
        };
        const char* v = nullptr;
        if (flag == "--json") {
            out.json = true;
        } else if (flag == "--format") {
            if (!value(v) || !ParseFormat(v, format, error))
                return false;
            formatSet = true;
        } else if (flag == "--verbose") {
            out.verbose = true;
        } else if (flag == "--read-only") {
            out.readOnly = true;
        } else if (flag == "--yes") {
            out.yes = true;
        } else if (flag == "--no-retry") {
            out.noRetry = true;
        } else if (flag == "--retry-on-death") {
            out.retryOnDeath = true;
        } else if (flag == "--timeout") {
            if (!value(v) || !ParseTimeoutSeconds(v, out.timeoutMs, error))
                return false;
        } else if (flag == "--project") {
            if (!value(v))
                return false;
            out.project = v;
        } else if (flag == "--host") {
            if (!value(v))
                return false;
            const std::string_view kind = v;
            if (kind != "editor" && kind != "runtime" && kind != "server")
                return Fail(error, "--host is editor, runtime or server, got '" + std::string(kind) + "'");
            out.hostKind = v;
        } else if (flag == "--pid") {
            if (!value(v) || !ParsePid(v, out.pid, error))
                return false;
        } else if (flag == "--engine") {
            if (!value(v))
                return false;
            out.engine = v;
        } else if (flag == "--world") {
            if (!value(v))
                return false;
            out.world = v;
        } else if (flag == "--client") {
            // spec s4.5/s4.6: the caller's name, sent as X-Remote-Client and
            // recorded in the host's audit log. Refused here exactly as the
            // host would refuse it (final fix pass, C-I1).
            if (!value(v))
                return false;
            if (!Arcane::Remote::IsValidClientId(v))
                return Fail(error, "--client needs 1 to " + std::to_string(Arcane::Remote::kMaxClientIdBytes) +
                                       " printable ASCII characters, got '" + std::string(v) + "'");
            out.client = v;
            out.clientSet = true;
        } else if (flag == "--all") {
            // --all [editor|runtime|server]: the token after it is a kind filter
            // only when it spells one (a verb word is never a kind).
            out.all = true;
            if (i + 1 < argc) {
                const std::string_view kind = argv[i + 1];
                if (kind == "editor" || kind == "runtime" || kind == "server") {
                    out.allKind = kind;
                    ++i;
                }
            }
        } else if (rest != nullptr) {
            rest->emplace_back(flag);
        }
    }
    if (out.client.empty())
        out.client = DefaultClientId();
    // --json equals --format json and wins over TTY detection (AM4); a
    // non-TTY stdout with no format flag is JSON.
    if (out.json)
        out.format = Format::Json;
    else if (formatSet)
        out.format = format;
    else
        out.format = out.tty ? Format::Human : Format::Json;
    return true;
}

// A reply whose protocol major this client does not speak is protocol.mismatch
// (exit 4), whatever it says; the remediation names both sides (rev 8.3 V6).
void CheckProtocol(const std::string& command, Arcane::Remote::Envelope& env)
{
    if (SpeaksProtocolMajor(env.protocol))
        return;
    auto mismatch = ProtocolMismatch(command, env.protocol);
    mismatch.protocol = env.protocol;
    mismatch.protocolMinor = env.protocolMinor;
    mismatch.id = env.id;
    mismatch.host = env.host;
    env = std::move(mismatch);
}

// command.unknown keeps exit 2; its remediation names the command and the
// host's major.minor, so an older host reads as one (rev 8.3 V6).
void ExplainUnknownCommand(const std::string& command, Arcane::Remote::Envelope& env)
{
    if (env.errors.empty() || env.errors.front().code != "command.unknown")
        return;
    // catalog.describe names the command it could not find in detail.name.
    const nlohmann::json& detail = env.errors.front().detail;
    const std::string missing = detail.is_object() && detail.contains("name") && detail.at("name").is_string()
                                    ? detail.at("name").get<std::string>()
                                    : command;
    auto& remediation = env.errors.front().remediation;
    const std::string note = "the host (remote protocol " + std::to_string(env.protocol) + "." +
                             std::to_string(env.protocolMinor) + ") has no command " + missing +
                             "; update the host or use a servitor that matches it";
    remediation = remediation.empty() ? note : remediation + "; " + note;
}

// Only a transient, provably-not-run exit 5 is retried. host.unavailable is
// also exit 5 but means a host hook was never installed: it answers at once.
// So does host.quit's unsaved-changes guard (host.busy{state:"modal"}): it
// waits on a person, not on time, so retrying it only burns --timeout.
bool TransientBusy(const std::string& command, const ExchangeResult& r)
{
    if (r.connectionRefusedBeforeBody)
        return true;
    if (r.envelope.errors.empty())
        return false;
    const Arcane::Remote::Error& e = r.envelope.errors.front();
    if (command == "host.quit" && e.code == "host.busy" && e.detail.is_object() &&
        e.detail.value("state", std::string{}) == "modal")
        return false;
    return e.code == "host.busy" || e.code == "capture.minimized";
}

} // namespace

// FileTimeToSys, not clock_cast: libc++ (macOS) has no clock_cast (R8-7).
std::filesystem::path NewestReportAfter(const std::filesystem::path& dir, std::int64_t requestUnixMs)
{
    namespace fs = std::filesystem;
    using namespace std::chrono;
    fs::path best;
    system_clock::time_point bestTime{};
    std::string bestKey;
    if (dir.empty())
        return best;
    const sys_time<milliseconds> after{milliseconds(requestUnixMs)};
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".arcdiag")
            continue;
        std::error_code timeEc;
        const auto written = it->last_write_time(timeEc);
        if (timeEc)
            continue;
        const auto sys = FileTimeToSys(written);
        if (!(sys > after))
            continue;
        const std::string key = p.generic_string();
        if (best.empty() || sys > bestTime || (sys == bestTime && key > bestKey)) {
            best = p;
            bestTime = sys;
            bestKey = key;
        }
    }
    return best;
}

namespace {

#if defined(_WIN32)
// The exit code of {pid, processStart} once that exact process has exited.
// A second open (IsProcessLive closed its own handle): a creation-time
// mismatch is a different process, and a failed open says nothing.
std::optional<DWORD> ExitedCode(std::uint32_t pid, std::uint64_t processStart)
{
    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return std::nullopt;
    std::optional<DWORD> code;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (::GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        const std::uint64_t start =
            (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const bool hasExited = exited.dwHighDateTime != 0 || exited.dwLowDateTime != 0;
        DWORD value = 0;
        if (start == processStart && hasExited && ::GetExitCodeProcess(h, &value))
            code = value;
    }
    ::CloseHandle(h);
    return code;
}
#endif

void Write(std::FILE* stream, const std::string& text)
{
    if (!text.empty()) {
        std::fwrite(text.data(), 1, text.size(), stream);
        std::fflush(stream);
    }
}

} // namespace

int Emit(const Printed& printed, int exitCode)
{
    Write(stdout, printed.stdoutText);
    Write(stderr, printed.stderrText);
    return exitCode;
}

std::int64_t NowUnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string PathUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path PathFromUtf8(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string WideToUtf8(std::wstring_view wide)
{
    ARC_CONSTANT("Unicode: U+FFFD REPLACEMENT CHARACTER, for a lone surrogate or a value past U+10FFFF")
    constexpr char32_t kReplacement = 0xFFFD;
    std::string out;
    out.reserve(wide.size());
    const auto append = [&out](char32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const auto unit = static_cast<char32_t>(static_cast<std::make_unsigned_t<wchar_t>>(wide[i]));
        const bool high = unit >= 0xD800 && unit <= 0xDBFF;
        const bool low = unit >= 0xDC00 && unit <= 0xDFFF;
        if constexpr (sizeof(wchar_t) == 2) {
            // UTF-16 (Windows): a high surrogate followed by a low one is one
            // code point; either half alone is not a character.
            if (high && i + 1 < wide.size()) {
                const auto next = static_cast<char32_t>(static_cast<std::make_unsigned_t<wchar_t>>(wide[i + 1]));
                if (next >= 0xDC00 && next <= 0xDFFF) {
                    append(0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                    ++i;
                    continue;
                }
            }
            append(high || low ? kReplacement : unit);
        } else {
            // UTF-32 (POSIX wchar_t): every unit is a code point.
            append(high || low || unit > 0x10FFFF ? kReplacement : unit);
        }
    }
    return out;
}

Arcane::Remote::Envelope ProtocolMismatch(const std::string& command, std::int64_t hostMajor)
{
    const std::string host = std::to_string(hostMajor);
    auto env = Arcane::Remote::MakeError(
        command, "protocol.mismatch",
        "the host speaks remote protocol " + host + "; this servitor speaks " + ProtocolMajorsText(),
        "use a servitor that speaks remote protocol " + host + ", or a host that speaks remote protocol " +
            ProtocolMajorsText());
    env.protocol = static_cast<int>(hostMajor);
    return env;
}

int PrintEnvelope(const Arcane::Remote::Envelope& env, const Arcane::Remote::ClientBlock& client,
                  const GlobalOptions& opt)
{
    RenderOptions ro;
    ro.format = opt.format;
    ro.tty = opt.tty;
    return Emit(Render(env, client, ro), client.exitCode);
}

std::string DefaultClientId()
{
    return "servitor/" + std::string(kVersion) + " pid " + std::to_string(ThisProcessId());
}

std::string HelpText()
{
    // UsageText's own text after its "servitor: <problem>" line.
    const std::string text = UsageText({});
    const std::size_t nl = text.find('\n');
    return nl == std::string::npos ? text : text.substr(nl + 1);
}

bool IsHelpRequest(std::span<const std::string> tokens)
{
    return tokens.size() == 1 && (tokens.front() == "--help" || tokens.front() == "-h");
}

int PrintUsage(const std::string& problem, Format format)
{
    if (format == Format::Human) {
        Write(stderr, UsageText(problem));
        return 2;
    }
    // json / ndjson: the refusal is one envelope on stdout, stderr stays empty.
    Arcane::Remote::Envelope env = Arcane::Remote::MakeError("", "arg.invalid", problem, "servitor --help prints the usage");
    Arcane::Remote::ClientBlock client;
    client.exitCode = 2;
    RenderOptions ro;
    ro.format = format;
    Write(stdout, Render(env, client, ro).stdoutText);
    return 2;
}

Format ScanFormat(int argc, char** argv)
{
    // The format a ParseArgs failure should answer in: ParseArgs returns before
    // it assigns out.format, so read the flags again here (stopping at "--",
    // which ends servitor's own flags). --json wins over --format, as in ParseArgs.
    bool json = false;
    std::optional<Format> format;
    for (int i = 1; i < argc; ++i) {
        const std::string_view token = argv[i];
        if (token == "--")
            break;
        if (token == "--json") {
            json = true;
        } else if (token == "--format" && i + 1 < argc) {
            Format parsed{};
            if (ParseFormat(argv[i + 1], parsed, nullptr))
                format = parsed;
        }
    }
    if (json)
        return Format::Json;
    return format.value_or(StdoutIsTty() ? Format::Human : Format::Json);   // ParseGlobals' default
}

std::string UsageText(const std::string& problem)
{
    std::string text = "servitor: " + problem + "\nusage: servitor [global flags] <verb> [args]\nverbs:";
    for (const VerbRow& row : VerbTable()) {
        text += "\n  ";
        text += row.words;
        if (row.usage != nullptr) {
            text += ' ';
            text += row.usage;
        }
    }
    text += "\nglobal flags: --json, --format json|human|ndjson, --timeout <s>, --project <dir>, "
            "--host editor|runtime|server, --pid <n>, --engine <root>, --world <name>, --all [editor|runtime|server], --read-only, --yes, --no-retry, "
            "--retry-on-death, --client <name>, --verbose, --version, --help (-h)\n"
            "  --client <name> names the caller in the host's audit log (default: servitor/<version> pid <pid>); "
            "launch records it in the host's descriptor, status lists only that client's hosts, and quit quits "
            "them (with --host <kind>, also that kind's host launched without --client; --pid wins)\n"
            "  --verbose prints each request's payload, including the values you pass (a cvar set's value too); "
            "the bearer token is redacted\n"
            "  --retry-on-death resends only after the host died or hung and a new host was found (servitor status)\n"
            "  --yes confirms a Drive command sent to several hosts with --all, or quit --client matching several\n";
    return text;
}

std::string VersionText()
{
    return "Servitor " + std::string(kVersion) + " (remote protocol " + ProtocolMajorsText() + ")";
}

std::string ProtocolMajorsText()
{
    std::string text;
    for (const std::uint32_t m : kProtocolMajors) {
        if (!text.empty())
            text += ", ";
        text += std::to_string(m);
    }
    return text;
}

bool SpeaksProtocolMajor(std::int64_t major)
{
    return major >= 0 && std::find(kProtocolMajors.begin(), kProtocolMajors.end(),
                                   static_cast<std::uint64_t>(major)) != kProtocolMajors.end();
}

std::optional<std::string> LeftoverFlag(std::span<const std::string> rest)
{
    for (const std::string& token : rest) {
        if (token.starts_with("--"))
            return "unknown flag " + token;
    }
    return std::nullopt;
}

bool ParseGlobals(int argc, char** argv, GlobalOptions& out, std::string* error)
{
    return ParseArgs(argc, argv, out, error, nullptr);
}

Printed Call(const CallRequest& req, const GlobalOptions& opt, ExchangeFn exchange, void* user,
             Arcane::Remote::ClientBlock* client, Arcane::Remote::Envelope* finalEnvelope, RediscoverFn rediscover)
{
    // Built per attempt: a busy retry resends options.timeoutMs as the budget
    // REMAINING, not the original one, so a job a retried call arms on the
    // host is bounded by what the client still waits (Task 27 fix round 1).
    const auto makePayload = [&](std::uint32_t timeoutMs) {
        nlohmann::json body = {
            {"id", "c1"},
            {"command", req.command},
            {"args", req.args},
            {"options", {{"timeoutMs", timeoutMs}}},
        };
        if (opt.readOnly)
            body["options"]["readOnly"] = true;   // ruling F15
        if (!opt.world.empty())
            body["options"]["world"] = opt.world; // ruling F14; the listener copies it onto Call::world
        return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    };
    std::string payload = makePayload(opt.timeoutMs);
    Arcane::Remote::ClientBlock local;
    if (client == nullptr)
        client = &local;
    client->retried = false;
    std::string verboseLine;
    if (opt.verbose) {
        const std::string line = "servitor request: POST /remote/v1/call Authorization: Bearer " +
                                 client->token + " " + payload;
        verboseLine = Arcane::Remote::RedactSecrets(line, client->token);
        // human: the engine logger's stderr sink (R8-8). json / ndjson: the
        // line rides the envelope as a warning (below); stderr stays empty.
        if (opt.format == Format::Human)
            ARC_INFO("{}", verboseLine);
    }
    ExchangeResult last{};
    const auto started = std::chrono::steady_clock::now();
    int pauseMs = 10;
    for (;;) {
        last = exchange(payload, user);
        if (!last.connectionRefusedBeforeBody)
            CheckProtocol(req.command, last.envelope);
        const int code = last.connectionRefusedBeforeBody ? 5 : Arcane::Remote::ExitFromHost(last.envelope);
        const bool busy = code == 5 && !opt.noRetry && TransientBusy(req.command, last);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        // --retry-on-death (spec AM6): only a host that died or hung, and only
        // once rediscovery found a DIFFERENT process to send it to. A live
        // host that closed the connection (host.unreachable) or is still
        // running the command (call.timeout, exit 8) is never re-posted: the
        // first copy may have run (final fix pass, C-I2 d / C-M6).
        const bool deathCandidate = code == 7 && opt.retryOnDeath && rediscover != nullptr && !busy &&
                                    !last.envelope.errors.empty() &&
                                    (last.envelope.errors.front().code == "host.died" ||
                                     last.envelope.errors.front().code == "host.hung") &&
                                    elapsed < static_cast<long long>(opt.timeoutMs);
        const bool death = deathCandidate && rediscover(user, last.envelope);
        if ((busy || death) && elapsed + pauseMs < static_cast<long long>(opt.timeoutMs)) {
            client->retried = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(pauseMs));
            pauseMs = pauseMs < 50 ? 50 : std::min(1000, pauseMs * 2);
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            payload = makePayload(static_cast<std::uint32_t>(
                std::max<long long>(1, static_cast<long long>(opt.timeoutMs) - spent)));
            continue;
        }
        client->exitCode = code;
        break;
    }
    ExplainUnknownCommand(req.command, last.envelope);
    if (!verboseLine.empty() && opt.format != Format::Human) {
        Arcane::Remote::Error warning;
        warning.code = "servitor.verbose";
        warning.message = verboseLine;
        last.envelope.warnings.push_back(std::move(warning));
    }
    if (finalEnvelope != nullptr)
        *finalEnvelope = last.envelope;
    RenderOptions ro;
    ro.format = opt.format;
    ro.tty = opt.tty;
    return Render(last.envelope, *client, ro);
}

int Run(int argc, char** argv)
{
    // --version answers before flag parsing and verb matching (rev 8.3 V6).
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            Write(stdout, VersionText() + "\n");
            return 0;
        }
    }
    GlobalOptions opt;
    std::string error;
    std::vector<std::string> tokens;
    if (!ParseArgs(argc, argv, opt, &error, &tokens))
        return PrintUsage(error, ScanFormat(argc, argv));
    // `servitor --help` / `-h` with no verb: the usage on stdout, exit 0 (round 4).
    if (IsHelpRequest(tokens)) {
        Write(stdout, HelpText());
        return 0;
    }
    if (tokens.empty())
        return PrintUsage("no verb given", opt.format);
    std::size_t consumed = 0;
    const VerbRow* row = MatchVerb(tokens, &consumed);
    if (row == nullptr)
        return PrintUsage(tokens.front().starts_with("-") ? "unknown flag " + tokens.front()
                                                          : "unknown verb " + tokens.front(), opt.format);
    const auto rest = std::span<const std::string>(tokens).subspan(consumed);
    // Every global flag was consumed above, so a leftover "--x" is unknown
    // unless the verb's row declares it (Task 14 review carry; Task 16 rows
    // declare their flags, and launch passes everything after "--" through).
    if (const auto problem = SplitVerbTokens(*row, rest, nullptr))
        return PrintUsage(*problem, opt.format);
    if (row->args == nullptr && !rest.empty())
        return PrintUsage(std::string(row->words) + " takes no arguments, got " + rest.front(), opt.format);
    // Rule 3: --world / --read-only on a verb that posts no call is refused, never dropped.
    if (const auto problem = UnusedGlobal(*row, opt))
        return PrintUsage(*problem, opt.format);
    if (opt.all)
        return RunFanOut(*row, rest, opt);   // spec AM2: one command to every matching live host
    if (row->local != nullptr)
        return row->local(rest, opt);
    CallRequest req;
    req.command = row->command;
    if (row->args != nullptr && !row->args(rest, req.args, &error))
        return PrintUsage(error, opt.format);
    const PostedCall posted = PostVerb(*row, req, opt, false);
    return PrintEnvelope(posted.envelope, posted.client, opt);
}

PostedCall PostVerb(const VerbRow& row, const CallRequest& req, GlobalOptions opt, bool noReplyGrace)
{
    PostedCall posted;
    // Discovery (spec s4.4): the project's descriptors, then exactly one host.
    // Only a row whose awaitsHost predicate accepts these args (`wait ready`)
    // keeps looking for up to --timeout while there is no host yet, so a wait
    // right after a launch finds it booting; the call then gets what is left
    // of the budget. Every other verb with no host is target.none at once.
    const bool awaits = row.awaitsHost != nullptr && row.awaitsHost(req.args);
    Arcane::Remote::Envelope refusal;
    const auto discoveryStart = std::chrono::steady_clock::now();
    for (;;) {
        refusal = Arcane::Remote::Envelope{};
        if (const auto dir = RemoteDirFor(opt, &refusal))
            posted.host = Select(ScanDirectory(*dir), opt, &refusal);
        const bool none = !posted.host && !refusal.errors.empty() && refusal.errors.front().code == "target.none";
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - discoveryStart).count();
        if (!awaits || !none || elapsed + kDiscoveryPollMs >= static_cast<long long>(opt.timeoutMs))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(kDiscoveryPollMs));
    }
    if (posted.host && awaits) {
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - discoveryStart).count();
        opt.timeoutMs = static_cast<std::uint32_t>(std::max<long long>(1, static_cast<long long>(opt.timeoutMs) - spent));
    }
    if (!posted.host) {
        refusal.command = req.command;
        posted.client.exitCode = Arcane::Remote::ExitFromHost(refusal);
        posted.envelope = std::move(refusal);
        return posted;
    }

    return PostToHost(*posted.host, req, opt, noReplyGrace);
}

PostedCall PostToHost(const LiveHost& host, const CallRequest& req, GlobalOptions opt, bool noReplyGrace)
{
    PostedCall posted;
    posted.host = host;
    LiveTarget target;
    target.descriptor = host.descriptor;
    target.timeoutMs = opt.timeoutMs;
    if (noReplyGrace)
        target.replyGraceMs = 0;
    target.watch = WatchFor(host, req.command, opt);
    target.clientId = opt.client.empty() ? DefaultClientId() : opt.client;   // X-Remote-Client (C-I1)
    target.opt = &opt;
    if (opt.retryOnDeath) {
        // Every host live as the call begins: rediscovery never retargets to
        // one of them (round 2, I-1).
        Arcane::Remote::Envelope ignored;
        if (const auto dir = RemoteDirFor(opt, &ignored))
            for (const LiveHost& live : ScanDirectory(*dir).live)
                target.preexisting.emplace_back(live.descriptor.pid, live.descriptor.processStart);
    }
    posted.client.token = host.descriptor.token;   // print-time redaction only; never printed
    posted.client.descriptor = PathUtf8(host.path);
    (void)Call(req, opt, &LiveExchange, &target, &posted.client, &posted.envelope, &RediscoverTarget);
    if (!SameHostProcess(target.descriptor, host.descriptor)) {
        // --retry-on-death moved the call to a new host: report that one.
        posted.host->descriptor = target.descriptor;
        posted.host->path = target.watch.descriptorPath;
        posted.client.descriptor = PathUtf8(target.watch.descriptorPath);
    }
    return posted;
}

std::filesystem::path HostLogFile(Arcane::Remote::HostKind kind, const std::filesystem::path& project,
                                  const GlobalOptions& opt)
{
    // The Diagnostics app names (ArcaneEditor main.cpp, RuntimeHost.cpp, ArcaneServer main.cpp).
    const char* appName = kind == Arcane::Remote::HostKind::Editor   ? "ArcaneEditor"
                          : kind == Arcane::Remote::HostKind::Server ? "ArcaneServer"
                                                                     : "ArcaneRuntime";
    // AttachLogSink: <report directory>/../Logs/<appName>.log.
    const std::filesystem::path reports =
        project.empty() ? HostDiagnosticsDir(kind, opt)
                        : Arcane::Paths::Resolve(Arcane::Paths::Location::DiagnosticsDir,
                                                 Arcane::Paths::ForProject(project));
    if (reports.empty())
        return {};   // rev 8.3 V7: an unresolved root has no log file, never a cwd-relative one
    return reports.parent_path() / "Logs" / (std::string(appName) + ".log");
}

void ReadLogFile(const std::filesystem::path& path, Arcane::Remote::Envelope& env, std::size_t tailBytes)
{
    // The notification names the file with native separators, as the host wrote it.
    const auto native = path.u8string();
    const std::string shown(reinterpret_cast<const char*>(native.data()), native.size());
    const auto unreadable = [&] {
        env = Arcane::Remote::MakeError("log.read", "target.none",
                                        "no live host, and its log file " + shown + " could not be read",
                                        "launch the host (servitor launch editor|runtime|server)");
    };
    // A directory (or any non-regular path) opens as an ifstream on some
    // standard libraries and tellg() then reports a nonsense size.
    std::error_code regularEc;
    if (!std::filesystem::is_regular_file(path, regularEc) || regularEc)
        return unreadable();
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        return unreadable();
    const std::streamoff end = in.tellg();
    const std::uint64_t fileBytes = end > 0 ? static_cast<std::uint64_t>(end) : 0;
    // A long file keeps its tail. One byte before the cut is read too, so a
    // cut that lands on a line start keeps that line whole.
    const bool cut = fileBytes > tailBytes;
    in.seekg(cut ? static_cast<std::streamoff>(fileBytes - tailBytes - 1) : 0);
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad())
        return unreadable();
    if (cut && !text.empty()) {
        const std::size_t nl = text.find('\n');
        text.erase(0, nl == std::string::npos ? 1 : nl + 1);   // drop the partial first line
    }
    const std::uint64_t shownBytes = text.size();
    env = Arcane::Remote::MakeOk("log.read", {{"text", std::move(text)}, {"file", PathUtf8(path)}}, {});
    Arcane::Remote::Error note;
    note.code = "logs.from-file";
    note.message = shown;
    note.remediation = "no host is running: this is the last host's log file, not its live ring";
    env.notifications.push_back(std::move(note));
    if (cut) {
        Arcane::Remote::Error truncated;
        truncated.code = "logs.truncated";
        truncated.message = "showing the last " + std::to_string(shownBytes) + " bytes (whole lines) of " +
                            std::to_string(fileBytes);
        truncated.remediation = "open the file for the rest";
        truncated.detail = {{"fileBytes", fileBytes}, {"shownBytes", shownBytes}};
        env.notifications.push_back(std::move(truncated));
    }
}

namespace {

std::atomic<int> g_interrupt{0};
static_assert(std::atomic<int>::is_always_lock_free, "a signal handler may only touch a lock-free atomic");

#if defined(_WIN32)
BOOL WINAPI OnConsoleControl(DWORD type)
{
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            g_interrupt.store(130, std::memory_order_release);
            return TRUE;
        // Best-effort: Windows ends the process once the handler returns from
        // these three (CTRL_CLOSE_EVENT after a short grace), so the stream loop
        // may not get to print its 143 result frame.
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_interrupt.store(143, std::memory_order_release);
            return TRUE;
        default:
            return FALSE;
    }
}
#else
struct sigaction g_previousInt;
struct sigaction g_previousTerm;

// Async-signal-safe: one store to a lock-free atomic.
void OnStopSignal(int sig)
{
    g_interrupt.store(sig == SIGINT ? 130 : 143, std::memory_order_release);
}
#endif

} // namespace

InterruptScope::InterruptScope()
{
    g_interrupt.store(0, std::memory_order_release);
#if defined(_WIN32)
    ::SetConsoleCtrlHandler(&OnConsoleControl, TRUE);
#else
    // No SA_RESTART: a blocked poll returns EINTR, so the loop sees the flag at once.
    struct sigaction action {};
    action.sa_handler = &OnStopSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    ::sigaction(SIGINT, &action, &g_previousInt);
    ::sigaction(SIGTERM, &action, &g_previousTerm);
#endif
}

InterruptScope::~InterruptScope()
{
#if defined(_WIN32)
    ::SetConsoleCtrlHandler(&OnConsoleControl, FALSE);
#else
    ::sigaction(SIGINT, &g_previousInt, nullptr);
    ::sigaction(SIGTERM, &g_previousTerm, nullptr);
#endif
}

const std::atomic<int>& InterruptScope::Pending()
{
    return g_interrupt;
}

bool RediscoverTarget(void* user, const Arcane::Remote::Envelope& death)
{
    (void)death;
    auto* target = static_cast<LiveTarget*>(user);
    if (target == nullptr || target->opt == nullptr)
        return false;
    const GlobalOptions& opt = *target->opt;
    // Eligible (spec AM6, "retry against the new pid"): the dead target's
    // REPLACEMENT only -- the same kind and project root (SameProjectRoot,
    // normalized: N-2), not hung, not the process that died ({pid, start}),
    // not any host already live when the call began (target->preexisting: a
    // resend there could run the command on a host the call already reached).
    // --pid is refused with --retry-on-death (round 4, N3-1). A restarting host writes its
    // descriptor only once it serves, so the scan is repeated every
    // kWatchPollMs within what is left of --timeout (round 3, N-1). Two
    // eligible hosts are refused at once (never a guess); none by the
    // deadline is false, and the call reports host.died / host.hung.
    const Arcane::Remote::Descriptor dead = target->descriptor;
    const auto budgetEnd = (target->started == std::chrono::steady_clock::time_point{}
                                ? std::chrono::steady_clock::now()
                                : target->started) +
                           std::chrono::milliseconds(target->timeoutMs);
    for (;;) {
        Arcane::Remote::Envelope refusal;
        const auto dir = RemoteDirFor(opt, &refusal);
        if (!dir)
            return false;
        const Scan scan = ScanDirectory(*dir);
        const LiveHost* found = nullptr;
        for (const LiveHost& h : scan.live) {
            const Arcane::Remote::Descriptor& d = h.descriptor;
            if (h.hung || d.hostKind != dead.hostKind || SameHostProcess(d, dead) ||
                !SameProjectRoot(d.project, dead.project))
                continue;
            const bool wasLive = std::find(target->preexisting.begin(), target->preexisting.end(),
                                           std::make_pair(d.pid, d.processStart)) != target->preexisting.end();
            if (wasLive)
                continue;
            if (found != nullptr)
                return false;   // two candidates: never guess
            found = &h;
        }
        if (found != nullptr) {
            target->descriptor = found->descriptor;
            const std::int64_t requestUnixMs = target->watch.requestUnixMs;
            target->watch = WatchFor(*found, target->watch.command, opt);
            target->watch.requestUnixMs = requestUnixMs;
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= budgetEnd)
            return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(budgetEnd - now);
        std::this_thread::sleep_for(std::min(left, std::chrono::milliseconds(kWatchPollMs)));
    }
}

TargetWatch WatchFor(const LiveHost& host, const std::string& command, const GlobalOptions& opt)
{
    TargetWatch watch;
    watch.command = command;
    watch.pid = host.descriptor.pid;
    watch.processStart = host.descriptor.processStart;
    watch.descriptorPath = host.path;
    // The host's report directory (ruling AX (2)): <project>/Saved/Diagnostics
    // when the descriptor names a project, else the exe-relative one.
    if (!host.descriptor.project.empty()) {
        const std::string& p = host.descriptor.project;
        const std::filesystem::path project(std::u8string(reinterpret_cast<const char8_t*>(p.data()), p.size()));
        watch.diagnosticsDir =
            Arcane::Paths::Resolve(Arcane::Paths::Location::DiagnosticsDir, Arcane::Paths::ForProject(project));
    } else {
        watch.diagnosticsDir = HostDiagnosticsDir(host.descriptor.hostKind, opt);
    }
    return watch;
}

std::optional<Arcane::Remote::Envelope> ObserveTarget(const TargetWatch& watch, std::int64_t nowUnixMs)
{
    const bool live = Arcane::Platform::IsProcessLive(watch.pid, watch.processStart);
    const std::optional<Arcane::Remote::Descriptor> desc = Arcane::Remote::ReadDescriptor(watch.descriptorPath);
    // Died wins over a stale heartbeat; a live process with no readable
    // heartbeat is hung, because a fresh one cannot be proved.
    if (live && desc && !Arcane::Remote::IsHung(*desc, nowUnixMs))
        return std::nullopt;

    std::string message;
    if (!live)
        message = "host process " + std::to_string(watch.pid) + " exited during the call";
    else if (!desc)
        message = "host process " + std::to_string(watch.pid) +
                  " is running but its descriptor is unreadable, so a fresh heartbeat cannot be proved";
    else
        message = "host process " + std::to_string(watch.pid) + " has not refreshed its heartbeat for " +
                  std::to_string(nowUnixMs - desc->heartbeatUnixMs) + " ms";
    auto env = Arcane::Remote::MakeError(watch.command, live ? "host.hung" : "host.died", std::move(message),
                                         "relaunch");
    env.host.pid = watch.pid;
    if (desc) {
        env.host.state = desc->state;
        env.host.kind = Arcane::Remote::ToString(desc->hostKind);
    }
    auto& detail = env.errors.front().detail;
    detail["lastState"] = desc ? desc->state : std::string{};
#if defined(_WIN32)
    // POSIX gives an exit status only to the parent, and the target is not
    // servitor's child, so detail.exitCode exists on Windows alone.
    if (!live) {
        if (const auto code = ExitedCode(watch.pid, watch.processStart))
            detail["exitCode"] = *code;
    }
#endif
    if (const auto report = NewestReportAfter(watch.diagnosticsDir, watch.requestUnixMs); !report.empty())
        detail["arcdiag"] = PathUtf8(report);
    return env;
}

} // namespace Servitor

#endif
