#pragma once
// The Servitor client core (spec s4.6, s6; plan rev 8.3 V3-V7): global flags,
// the one call path with its busy retry, and the mid-call target watch.
// Program code: no export macro (V4); protocol types stay qualified (no using
// in a header). Includes no ArcaneRemote host header (V3).
#include <Servitor/Format.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Servitor {

ARC_CONSTANT("the Servitor client's own version (spec AM9); servitor --version prints it")
inline constexpr std::string_view kVersion = "0.1.0";
ARC_CONSTANT("remote protocol majors this client speaks (spec AM9)")
inline constexpr std::array<std::uint32_t, 1> kProtocolMajors = {1};

// "Servitor <kVersion> (remote protocol <kProtocolMajors, comma-joined>)".
std::string VersionText();
// kProtocolMajors comma-joined ("1"), for remediation text.
std::string ProtocolMajorsText();
// True when `major` is in kProtocolMajors.
bool SpeaksProtocolMajor(std::int64_t major);
// protocol.mismatch (exit 4) for a host speaking `hostMajor`; the message and
// remediation name both sides (rev 8.3 V6). One wording for the reply check
// and for Select's descriptor check.
Arcane::Remote::Envelope ProtocolMismatch(const std::string& command, std::int64_t hostMajor);

// system_clock milliseconds since the epoch: the one clock for heartbeats,
// IsHung and requestUnixMs.
std::int64_t NowUnixMs();
// A path as UTF-8 with forward slashes, for envelopes.
std::string PathUtf8(const std::filesystem::path& p);
// A path from UTF-8 text: argv (UTF-8 on every platform, see WideToUtf8) and
// the wire. Never the narrow path(std::string) constructor, which reads the
// ANSI code page on Windows. On Windows, text that is not UTF-8 throws.
std::filesystem::path PathFromUtf8(std::string_view utf8);
// UTF-16 (wchar_t on Windows) or UTF-32 (POSIX) to UTF-8; a lone surrogate or
// a value past U+10FFFF becomes U+FFFD, so the result is always valid UTF-8.
// servitor's Windows main converts CommandLineToArgvW's wide argv with it, so
// a character outside the ANSI code page reaches Run intact instead of as '?'.
std::string WideToUtf8(std::wstring_view wide);

struct GlobalOptions {
    Format format = Format::Human; // TTY default; non-TTY default is Json
    bool json = false;
    bool verbose = false;
    bool tty = false;
    bool readOnly = false;          // --read-only; Call posts it as options.readOnly (ruling F15)
    bool yes = false;
    bool noRetry = false;
    bool retryOnDeath = false;
    std::uint32_t timeoutMs = 30000; // milliseconds. --timeout <s> is seconds; ParseGlobals scales.
    std::string project;
    std::string hostKind;       // editor | runtime | server, optional
    std::uint32_t pid = 0;
    std::string engine;         // --engine <root>; rev 8.3 V7 (HostsDir)
    std::string world;          // --world <name>; Call posts it as options.world when non-empty (ruling F14)
    bool all = false;           // --all: fan the command out to every matching live host (spec AM2)
    std::string allKind;        // --all editor|runtime|server: the token after --all when it is a kind
    // --client <name> (spec s4.5/s4.6): the X-Remote-Client value every call
    // sends, the audit line's `client`. ParseGlobals sets DefaultClientId()
    // when the flag is absent; empty only in a hand-built GlobalOptions.
    std::string client;
    bool clientSet = false;     // --client was given (Rule 3: a verb that posts nothing refuses it)
};

// "servitor/<kVersion> pid <this process id>": the caller name a call sends
// when --client names none (spec s4.5).
std::string DefaultClientId();

// The log file a host of `kind` writes (Task 22, ruling E-3 as amended in rev 8),
// derived exactly as Diagnostics' AttachLogSink derives it: the report directory's
// parent / Logs / <appName>.log, appName ArcaneEditor | ArcaneRuntime | ArcaneServer.
// With a project: <project>/Saved/Diagnostics -> <project>/Saved/Logs/<appName>.log
// (through Paths, as the report directory). Without: the exe-relative report
// directory HostDiagnosticsDir(kind, opt) -> <exeDir>/Logs/<appName>.log; an
// unresolved engine root (rev 8.3 V7) is an EMPTY path, never a relative one.
// Not Log::FileSinkPath(): in servitor that is servitor's own sink.
std::filesystem::path HostLogFile(Arcane::Remote::HostKind kind, const std::filesystem::path& project = {},
                                  const GlobalOptions& opt = {});
ARC_CONSTANT("spec: a command never streams megabytes into an envelope; logs' file fallback keeps this tail (bytes)")
inline constexpr std::size_t kLogFileTailBytes = 1024u * 1024u;
// `servitor logs` with no live host: the dead host's log file as log.read's
// answer, ok with data {text, file} and the notification {code: "logs.from-file",
// message: <path, native separators>}. It does not invent lines: a file that
// cannot be read is target.none (exit 4) naming it. A file longer than
// tailBytes keeps only its last tailBytes, cut to whole lines (a partial first
// line is dropped), and adds the notification {code: "logs.truncated"} with
// detail {fileBytes, shownBytes}.
void ReadLogFile(const std::filesystem::path& path, Arcane::Remote::Envelope& env,
                 std::size_t tailBytes = kLogFileTailBytes);

// While one is alive, SIGINT / SIGTERM (Windows: Ctrl+C / Ctrl+Break, and a
// console close, logoff or shutdown) do not end the process: they set
// Pending() to 130 / 143 and a stream loop (FollowStream) ends cleanly with that
// exit code. The previous handlers come back when it is destroyed. One at a time.
class InterruptScope {
public:
    InterruptScope();
    ~InterruptScope();
    InterruptScope(const InterruptScope&) = delete;
    InterruptScope& operator=(const InterruptScope&) = delete;
    // The flag FollowStream polls: 0, or 130 / 143 once a signal arrived.
    static const std::atomic<int>& Pending();
};

// Starts from GlobalOptions{} and applies the global flags found anywhere in
// argv[1..]. Any other token is left for the verb. False (exit 2) sets *error,
// including a value flag whose value starts with "--" (--project --json).
bool ParseGlobals(int argc, char** argv, GlobalOptions& out, std::string* error);

// The usage problem for the first token after the verb that starts with "--"
// (an unknown flag for a verb that declares none). Run checks the tokens
// through SplitVerbTokens (Verbs.hpp), which applies the same rule to a row's
// undeclared flags. A single '-' stays a value (negative numbers). nullopt: none.
std::optional<std::string> LeftoverFlag(std::span<const std::string> rest);
// "servitor: <problem>", the verbs and the global flags: the usage text.
std::string UsageText(const std::string& problem);
// The usage text with no problem line: what `servitor --help` / `-h` print
// on stdout (round 4 add-on).
std::string HelpText();
// True when the verb tokens (globals removed) are exactly one "--help" or
// "-h": a help request with no verb. Run prints HelpText and exits 0.
bool IsHelpRequest(std::span<const std::string> tokens);
// Human: prints UsageText(problem) to stderr. json / ndjson: one arg.invalid
// envelope on stdout (client.exitCode 2) and nothing on stderr. Returns 2.
int PrintUsage(const std::string& problem, Format format);
// The format a failed ParseArgs answers in, read from argv again (ParseArgs
// returns before it assigns out.format): --json, else --format json|ndjson|human,
// else Human. Stops at "--".
Format ScanFormat(int argc, char** argv);
// Writes printed.stdoutText to stdout and printed.stderrText to stderr (main
// thread only); returns exitCode.
int Emit(const Printed& printed, int exitCode);
// Renders `env` in opt's format (human: diagnostics on stderr; json and ndjson:
// everything in the envelope on stdout, stderr empty); returns client.exitCode.
int PrintEnvelope(const Arcane::Remote::Envelope& env, const Arcane::Remote::ClientBlock& client,
                  const GlobalOptions& opt);

struct CallRequest {
    std::string command;
    nlohmann::json args = nlohmann::json::object();
};

struct ExchangeResult {
    Arcane::Remote::Envelope envelope;
    bool connectionRefusedBeforeBody = false;
};

using ExchangeFn = ExchangeResult (*)(const std::string& requestBody, void* user);
// --retry-on-death's rediscovery (spec AM6: rediscover first, then retry
// against the new pid). Called with the exchange's user and the host.died /
// host.hung envelope; true when `user` now targets a DIFFERENT live process
// (a new pid or creation stamp), so a resend cannot reach the one that died.
using RediscoverFn = bool (*)(void* user, const Arcane::Remote::Envelope& death);

// Posts {id, command, args, options:{timeoutMs[, readOnly][, world]}} through `exchange`,
// retries a provably-not-run busy answer with backoff until opt.timeoutMs, checks
// the reply's protocol major, and renders it. Opens no socket itself.
// *finalEnvelope (when non-null) receives the envelope it rendered.
// With opt.retryOnDeath, an exit 7 is resent only when it is host.died or
// host.hung AND `rediscover` (non-null) retargets `user` to a new process;
// host.unreachable (a live host that closed the connection) and call.timeout
// (a live host still running the command, exit 8) are never resent.
Printed Call(const CallRequest& req, const GlobalOptions& opt, ExchangeFn exchange, void* user,
             Arcane::Remote::ClientBlock* client, Arcane::Remote::Envelope* finalEnvelope = nullptr,
             RediscoverFn rediscover = nullptr);
// servitor.exe's main calls this.
int Run(int argc, char** argv);

// The newest *.arcdiag directly under `dir` written strictly after
// requestUnixMs; equal mtimes break on the greater generic_string(). Empty
// when none qualifies (or `dir` is empty).
std::filesystem::path NewestReportAfter(const std::filesystem::path& dir, std::int64_t requestUnixMs);

struct TargetWatch {
    std::string command;
    std::uint32_t pid = 0;
    std::uint64_t processStart = 0;
    std::filesystem::path descriptorPath;
    std::filesystem::path diagnosticsDir;
    std::int64_t requestUnixMs = 0;
};

// file_clock <-> system_clock without clock_cast (R8-7: libc++ on macOS has
// none). libstdc++ and libc++ give file_clock to_sys/from_sys; MSVC's STL
// gives it to_utc/from_utc only, so that branch goes through utc_clock.
inline std::chrono::system_clock::time_point FileTimeToSys(std::filesystem::file_time_type t)
{
#if defined(_MSVC_STL_VERSION)
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::utc_clock::to_sys(std::chrono::file_clock::to_utc(t)));
#else
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(std::chrono::file_clock::to_sys(t));
#endif
}

inline std::filesystem::file_time_type SysToFileTime(std::chrono::system_clock::time_point t)
{
#if defined(_MSVC_STL_VERSION)
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
        std::chrono::file_clock::from_utc(std::chrono::utc_clock::from_sys(t)));
#else
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
        std::chrono::file_clock::from_sys(t));
#endif
}

// nullopt: the pid is live and the descriptor heartbeat is fresh.
// An engaged optional is a finished error envelope. The caller does not post it.
std::optional<Arcane::Remote::Envelope> ObserveTarget(const TargetWatch& watch, std::int64_t nowUnixMs);

} // namespace Servitor

#endif
