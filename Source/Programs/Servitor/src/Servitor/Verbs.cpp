#include <Servitor/Verbs.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Servitor/Commands.hpp>
#include <Servitor/Discover.hpp>
#include <Servitor/FanOut.hpp>
#include <Servitor/Launch.hpp>
#include <Servitor/Transport.hpp>

#include <Arcane/Base/DiagnosticStore.hpp> // MatchesDiagnosticFilter: the --follow --search rule
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Platform/Process.hpp>
#include <Arcane/Remote/Redact.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>

namespace Servitor {

namespace {

bool Fail(std::string* error, std::string text)
{
    if (error != nullptr)
        *error = std::move(text);
    return false;
}

ARC_CONSTANT("timing: how often quit --force samples whether the target process is gone")
constexpr int kQuitPollMs = 50;
ARC_CONSTANT("timing: how long quit --force waits for a killed host to leave the process table")
constexpr int kKillSettleMs = 5000;

// quit [--force] [--discard-changes] -> host.quit {force, discardChanges} (Task 16).
constexpr VerbFlag kQuitFlags[] = {
    {"--force", false},             // terminate the process if it is still alive after --timeout (spec s4.7)
    {"--discard-changes", false},   // the editor's guard, answered "drop them" (a drive command, audited)
    // Controller ruling (fix round 1): the report path is the host's own --report flag.
    {"--report", true, "quit takes no --report; pass --report at launch: servitor launch <kind> -- --report <path>"},
};

bool QuitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
int QuitVerb(std::span<const std::string> rest, const GlobalOptions& opt);
bool WaitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool CvarNameArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool CvarSetArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool CvarListArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool ConsoleArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool LogsArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
int LogsVerb(std::span<const std::string> rest, const GlobalOptions& opt);
bool ProblemsArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
int ProblemsVerb(std::span<const std::string> rest, const GlobalOptions& opt);
bool EntityGetArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool EcsQueryArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool ResourceGetArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool SceneTreeArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool ScreenshotViewportArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool ScreenshotWindowArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool CompareArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool PlayArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool StepArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);

// Task 29. The topology is the editor's (an editor's Play topology); a runtime
// or server was launched with its own and answers arg.inapplicable for any
// other than standalone.
constexpr VerbFlag kPlayFlags[] = {
    {"--topology", true},   // standalone (default) | listen | client+server
};

// Task 27. --out is resolved against servitor's working directory here; the
// host writes exactly that absolute path (a .png).
constexpr VerbFlag kScreenshotFlags[] = {
    {"--out", true},  // the PNG to write; omitted = the host's artifact directory (<remote dir>/out)
};

// Task 28. --bless writes the reference: a Drive call on the host (the
// read-only gate and the audit apply) and under --all (--yes, FanOut).
constexpr VerbFlag kCompareFlags[] = {
    {"--source", true},                // host (default) | viewport | window: Task 27's capture pairs
    {"--bless", false},                // write the capture as the reference instead of failing on a mismatch
    {"--max-diff-pixels", true},       // differing pixels allowed (a whole number)
    {"--max-diff-pixel-ratio", true},  // differing fraction allowed, 0..1
};

// Task 25.
constexpr VerbFlag kSceneTreeFlags[] = {
    {"--depth", true},  // levels below the roots (0 = roots only); omitted = the whole tree
};

// Task 23. Repeated list flags accumulate; each value may also be comma-separated.
constexpr VerbFlag kEntityGetFlags[] = {
    {"--component", true},  // a component name (short or qualified); repeat for more
};
constexpr VerbFlag kEcsQueryFlags[] = {
    {"--with", true},       // components every row has (A,B)
    {"--without", true},    // components no row has
    {"--fields", true},     // Component.member or Component (A.x,B)
    {"--limit", true},      // rows returned (default 100); data.count is the match count
    {"--no-strict", false}, // an unknown component is a warning and an empty match, not exit 6
};

// Task 22. --follow opens the host's SSE stream instead of posting a call.
constexpr VerbFlag kLogsFlags[] = {
    {"--since", true},    // a log seq, exclusive
    {"--level", true},    // info | warn | error: drop records below it
    {"--follow", false},  // GET /remote/v1/stream?topics=logs until --timeout or Ctrl+C
};
constexpr VerbFlag kProblemsFlags[] = {
    {"--severity", true}, // error | warning | info: exactly that severity
    {"--search", true},   // text in the message, code or detail (case-insensitive)
    {"--follow", false},  // GET /remote/v1/stream?topics=problems until --timeout or Ctrl+C
};

// Only `wait ready` waits for a host to appear (spec s4.4).
bool WaitAwaitsHost(const nlohmann::json& args)
{
    return args.is_object() && args.value("what", std::string{}) == "ready";
}

// Tasks 15-29 add their rows here (ruling F17).
const std::array<VerbRow, 34> kVerbs = {{
    {"status", nullptr, nullptr, &StatusVerb},   // client-side: lists live hosts, no connect (Task 15)
    {"stats frame", "stats.frame", nullptr, nullptr},
    // Task 16. launch is client-side: it spawns the host and waits for its descriptor.
    {"launch", nullptr, &LaunchArgs, &LaunchVerb, LaunchFlags(), true, nullptr,
     "editor|runtime|server [--headless] [--backend dx12|vulkan] [--remote-descriptor <path>] [-- <host flags>]"},
    // quit posts host.quit; --force also ends the process after --timeout, so it is client-side.
    {"quit", "host.quit", &QuitArgs, &QuitVerb, kQuitFlags, false, nullptr, "[--force] [--discard-changes]"},
    // `frames N` counts host.frame, the host's sim advances (AM1): one per fixed
    // tick on a server, a headless runtime or a step; a windowed runtime in Run
    // mode advances it once per loop pass, whatever its sub-steps. `settled` is
    // spec s4.7's --settle predicate (Task 30): two byte-equal captures with the
    // shader compiler idle, taken while the wait freezes the host's clock.
    {"wait", "host.wait", &WaitArgs, nullptr, {}, false, &WaitAwaitsHost,
     "ready|settled|frames N|reload|state <s>   (frames counts host.frame: sim advances, not rendered frames; "
     "settled is two byte-equal captures with the shaders idle, and the wait freezes the host clock while it "
     "captures)"},
    // Task 17. commands prints the static manifest with no host; list reads
    // GET /remote/v1/catalog; describe and schema post catalog.describe and
    // schema.type; call posts any command by name (client-side: the command
    // is the first token).
    {"commands", nullptr, nullptr, &CommandsVerb},
    {"list", nullptr, nullptr, &ListVerb},
    {"describe", "catalog.describe", &DescribeArgs, nullptr, {}, false, nullptr, "<command>"},
    {"schema", "schema.type", &SchemaArgs, nullptr, {}, false, nullptr, "component|resource <Name>"},
    {"call", nullptr, &CallArgs, &CallVerb, CallFlags(), false, nullptr,
     "<command> [name=value ...] [--args-json <json> | --args-file <path>]"},
    // Task 19. world list posts world.list; --world <name> (a global flag)
    // names a world for every world-taking command.
    {"world list", "world.list", nullptr, nullptr},
    // Task 20 (ruling F18). set sends the value as console text, exactly as
    // typed; the host parses it as the cvar's type (cvar.type-mismatch, exit 6).
    {"cvar get", "cvar.get", &CvarNameArgs, nullptr, {}, false, nullptr, "<name>"},
    {"cvar set", "cvar.set", &CvarSetArgs, nullptr, {}, false, nullptr, "<name> <value>"},
    {"cvar list", "cvar.list", &CvarListArgs, nullptr, {}, false, nullptr, "[prefix]"},
    {"cvar explain", "cvar.explain", &CvarNameArgs, nullptr, {}, false, nullptr, "<name>"},
    // Task 21. The tokens join into one console line; the host runs it through
    // the remote cvar service (ServerCanExecute commands, cvar reads and sets).
    {"console", "console.exec", &ConsoleArgs, nullptr, {}, false, nullptr, "<line>   (quote it, or pass its words)"},
    // Task 22. Client-side rows: with no live host, logs reads the dead host's
    // log file; --follow on either streams instead of posting (ruling P2 (f)).
    {"logs", "log.read", &LogsArgs, &LogsVerb, kLogsFlags, false, nullptr,
     "[--since N] [--level info|warn|error] [--follow]   (--follow ends at --timeout or Ctrl+C)"},
    {"problems", "problems.list", &ProblemsArgs, &ProblemsVerb, kProblemsFlags, false, nullptr,
     "[--severity error|warning|info] [--search TEXT] [--follow]"},
    // Task 23. Read-only ECS verbs; each resolves --world on the host.
    {"entity get", "entity.get", &EntityGetArgs, nullptr, kEntityGetFlags, false, nullptr,
     "guid:<id>|name:<unique name> [--component C ...]"},
    {"ecs query", "ecs.query", &EcsQueryArgs, nullptr, kEcsQueryFlags, false, nullptr,
     "--with A,B [--without C] [--fields A.x,B] [--limit N] [--no-strict]"},
    {"ecs stats", "ecs.stats", nullptr, nullptr},
    {"ecs archetypes", "ecs.archetypes", nullptr, nullptr},
    // Task 24. Read-only; each resolves --world on the host.
    {"resource list", "resource.list", nullptr, nullptr},
    {"resource get", "resource.get", &ResourceGetArgs, nullptr, {}, false, nullptr, "<Name>   (from resource list)"},
    // Task 25. scene tree resolves --world on the host (editor and runtime; a
    // server answers command.host-kind). selection is editor-only: a runtime or
    // server answers command.host-kind (ruling F-1).
    {"scene tree", "scene.tree", &SceneTreeArgs, nullptr, kSceneTreeFlags, false, nullptr,
     "[--depth N]   (0 = roots only)"},
    {"selection", "editor.selection.get", nullptr, nullptr},
    // Task 27. Observe jobs completed by a render-ticket readback; a server
    // answers capture.no-renderer (exit 2), a minimized editor host.busy (5).
    // A held runtime still renders, but its time-driven materials follow wall
    // time: a --headless host launched with --fixed-time (headless only) gives
    // byte-stable captures from one call to the next.
    {"screenshot viewport", "capture.viewport", &ScreenshotViewportArgs, nullptr, kScreenshotFlags, false, nullptr,
     "[--out FILE.png]   (time-driven materials follow the host clock; --fixed-time, on a --headless host only, "
     "pins it)"},
    {"screenshot window", "capture.window", &ScreenshotWindowArgs, nullptr, kScreenshotFlags, false, nullptr,
     "[--out FILE.png]   (time-driven materials follow the host clock; --fixed-time, on a --headless host only, "
     "pins it)"},
    // Task 28. One capture.compare job on the screenshot pairs: Passed exits 0,
    // Failed exits 8 (ok true), a missing reference exits 6 unless --bless
    // creates it. Against a golden, use a static scene or a --headless host
    // launched with --fixed-time (headless only): a held runtime's time-driven
    // materials follow wall time.
    {"compare", "capture.compare", &CompareArgs, nullptr, kCompareFlags, false, nullptr,
     "<reference> [--source host|viewport|window] [--bless] [--max-diff-pixels N] [--max-diff-pixel-ratio R]   "
     "(time-driven materials follow the host clock; --fixed-time, on a --headless host only, pins it)"},
    // Task 29. Drive commands (the host's read-only gate and audit apply; --all
    // needs --yes). An editor enters / leaves Play; a runtime or server runs or
    // holds its sim (an open-ended --remote run starts held at frame 0). step N
    // is a job: it returns once host.frame has moved N (sim ticks; loop passes
    // on a running windowed runtime, AM1), so a large N needs a --timeout to match.
    {"play", "play.start", &PlayArgs, nullptr, kPlayFlags, false, nullptr,
     "[--topology standalone|listen|client+server]   (--topology: editor only; an editor already playing "
     "answers ok with alreadyPlaying: true and its running topology)"},
    {"stop", "play.stop", nullptr, nullptr},
    {"pause", "play.pause", nullptr, nullptr},
    {"resume", "play.resume", nullptr, nullptr},
    {"step", "play.step", &StepArgs, nullptr, {}, false, nullptr,
     "[N]   (1 to 100000, default 1; returns when host.frame moved N: a fixed tick each, "
     "or one loop pass each on a running windowed runtime)"},
}};

bool QuitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("quit"), rest, &tokens))
        return Fail(error, *problem);
    if (!tokens.positional.empty())
        return Fail(error, "quit takes no arguments, got " + tokens.positional.front());
    bool discard = false;
    for (const auto& [flag, value] : tokens.flags) {
        (void)value;
        discard = discard || flag == "--discard-changes";
    }
    // --force is servitor's own (QuitForced): the host has no force path and
    // refuses a `force` arg (final fix pass, C-M7), so it is never posted.
    out = {{"discardChanges", discard}};
    return true;
}

} // namespace

std::optional<std::string> FollowUnusedGlobal(std::string_view words, const GlobalOptions& opt)
{
    if (!opt.world.empty())
        return "--world does not apply to " + std::string(words) +
               " --follow: the stream is the host's, not one world's";
    if (opt.readOnly)
        return "--read-only does not apply to " + std::string(words) + " --follow: a stream only reads";
    if (opt.clientSet)
        return "--client does not apply to " + std::string(words) +
               " --follow: a stream sends no X-Remote-Client (only a call is audited)";
    // Round 4 (N3-4): a stream neither retries nor rediscovers; a death ends it.
    if (opt.retryOnDeath)
        return "--retry-on-death does not apply to " + std::string(words) +
               " --follow: a stream that loses its host ends, it is never resent";
    if (opt.noRetry)
        return "--no-retry does not apply to " + std::string(words) + " --follow: a stream is never retried";
    return std::nullopt;
}

bool QuitForced(std::span<const std::string> rest)
{
    return std::find(rest.begin(), rest.end(), std::string("--force")) != rest.end();
}

namespace {

bool WaitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    const std::string usage = "wait needs ready, settled, frames N, reload or state <s>";
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("wait"), rest, &tokens))
        return Fail(error, *problem);
    const auto& p = tokens.positional;
    if (p.empty())
        return Fail(error, usage);
    const std::string& what = p.front();
    if (what == "ready" || what == "settled" || what == "reload") {
        if (p.size() != 1)
            return Fail(error, "wait " + what + " takes nothing more, got " + p[1]);
        out = {{"what", what}};
        return true;
    }
    if (what == "frames") {
        if (p.size() != 2)
            return Fail(error, "wait frames needs one count N");
        const std::string& n = p[1];
        errno = 0;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(n.c_str(), &end, 10);
        if (n.empty() || n[0] < '0' || n[0] > '9' || end != n.c_str() + n.size() || errno == ERANGE ||
            v > std::numeric_limits<std::uint32_t>::max())
            return Fail(error, "wait frames needs a non-negative whole number, got '" + n + "'");
        out = {{"what", "frames"}, {"frames", static_cast<std::uint32_t>(v)}};
        return true;
    }
    if (what == "state") {
        std::string names;
        for (const std::string_view name : Arcane::Remote::kHostStateNames)
            names += (names.empty() ? "" : ", ") + std::string(name);
        if (p.size() != 2)
            return Fail(error, "wait state needs one state: " + names);
        if (!Arcane::Remote::IsHostStateName(p[1]))
            return Fail(error, "wait state needs one of " + names + ", got '" + p[1] + "'");
        out = {{"what", "state"}, {"state", p[1]}};
        return true;
    }
    return Fail(error, usage + ", got " + what);
}

// cvar get <name> / cvar explain <name> -> {name}. No cvar verb declares a
// flag, so any "--x" token is the SplitVerbTokens refusal (usage exit 2).
bool CvarNameArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("cvar get"), rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.size() != 1)
        return Fail(error, "cvar get and cvar explain need one cvar name (servitor cvar list shows them)");
    out = {{"name", tokens.positional.front()}};
    return true;
}

// cvar set <name> <value> -> {name, value}: value is console text (a vector is
// one quoted token, "1 2 3", or "[1,2,3]").
bool CvarSetArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("cvar set"), rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.size() != 2)
        return Fail(error, "cvar set needs a cvar name and one value (quote a value with spaces)");
    out = {{"name", tokens.positional[0]}, {"value", tokens.positional[1]}};
    return true;
}

// cvar list [prefix] -> {} or {prefix}.
bool CvarListArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("cvar list"), rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.size() > 1)
        return Fail(error, "cvar list takes at most one dotted prefix, got " + tokens.positional[1]);
    out = tokens.positional.empty() ? nlohmann::json::object()
                                    : nlohmann::json{{"prefix", tokens.positional.front()}};
    return true;
}

// console <line...> -> {line}: the positional tokens joined by one space, so
// `console cvar_explain x` and `console "cvar_explain x"` post the same line.
// The verb declares no flag: a "--x" token is the SplitVerbTokens refusal.
bool ConsoleArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("console"), rest, &tokens))
        return Fail(error, *problem);
    std::string line;
    for (const std::string& token : tokens.positional)
        line += (line.empty() ? "" : " ") + token;
    if (line.find_first_not_of(" \t") == std::string::npos)
        return Fail(error, "console needs a console line, such as cvarlist or render.vsync false");
    out = {{"line", line}};
    return true;
}

// Appends the comma-separated, non-empty parts of `value` to `out`.
void AppendList(const std::string& value, nlohmann::json& out)
{
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = std::min(value.find(',', start), value.size());
        if (comma > start)
            out.push_back(value.substr(start, comma - start));
        start = comma + 1;
    }
}

// entity get <ref> [--component C ...] -> {entity[, components]}.
bool EntityGetArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("entity get"), rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.size() != 1)
        return Fail(error, "entity get needs one entity: guid:<id> or name:<unique name>");
    out = {{"entity", tokens.positional.front()}};
    nlohmann::json components = nlohmann::json::array();
    for (const auto& [flag, value] : tokens.flags)
        AppendList(value, components);
    if (!components.empty())
        out["components"] = std::move(components);
    return true;
}

// resource get <Name> -> {name}. The verb declares no flag: a "--x" token is
// the SplitVerbTokens refusal (usage exit 2).
bool ResourceGetArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("resource get"), rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.size() != 1)
        return Fail(error, "resource get needs one resource name (servitor resource list names them)");
    out = {{"name", tokens.positional.front()}};
    return true;
}

// scene tree [--depth N] -> {} or {depth}.
bool ScreenshotArgs(const char* words, std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb(words), rest, &tokens))
        return Fail(error, *problem);
    if (!tokens.positional.empty())
        return Fail(error, std::string(words) + " takes flags only (--out FILE.png), got " + tokens.positional.front());
    out = nlohmann::json::object();
    for (const auto& [flag, value] : tokens.flags) {
        // --out is the only flag the row declares.
        if (value.empty())
            return Fail(error, "--out needs a file path");
        // argv is UTF-8 on every platform (main.cpp converts Windows' wide
        // command line, Task 30), so the path is built from UTF-8 and sent as
        // UTF-8. A conversion that fails is a usage error, never an escaped
        // exception.
        try {
            const std::filesystem::path given = PathFromUtf8(value);
            std::string ext;
            for (const char8_t c : given.extension().u8string())
                ext += static_cast<char>(c >= u8'A' && c <= u8'Z' ? c - u8'A' + u8'a' : c);
            if (ext != ".png")
                return Fail(error, "--out must name a .png file, got " + value);
            std::error_code ec;
            const std::filesystem::path absolute = std::filesystem::absolute(given, ec);
            if (ec)
                return Fail(error, "--out cannot be resolved: " + value);
            const std::u8string utf8 = absolute.lexically_normal().u8string();
            out["out"] = std::string(utf8.begin(), utf8.end());
        } catch (const std::exception&) {
            return Fail(error, "--out is not a path this system can represent: " + value);
        }
    }
    return true;
}

bool ScreenshotViewportArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    return ScreenshotArgs("screenshot viewport", rest, out, error);
}

bool ScreenshotWindowArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    return ScreenshotArgs("screenshot window", rest, out, error);
}

// compare <reference> [--source S] [--bless] [--max-diff-pixels N] [--max-diff-pixel-ratio R]
// -> {reference[, source][, bless][, maxDiffPixels][, maxDiffPixelRatio]}.
// An omitted budget is left out (unset: both unset is a zero budget). The
// reference name itself is checked by the host (arg.invalid), which owns the rule.
bool CompareArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("compare"), rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.size() != 1)
        return Fail(error, "compare needs one reference name, such as editor-ui");
    out = {{"reference", tokens.positional.front()}};
    for (const auto& [flag, value] : tokens.flags) {
        // Every flag once: a repeated one is ambiguous, never last-wins (Rule 3).
        const std::string key = flag == "--source"            ? "source"
                              : flag == "--bless"             ? "bless"
                              : flag == "--max-diff-pixels"   ? "maxDiffPixels"
                                                              : "maxDiffPixelRatio";
        if (out.contains(key))
            return Fail(error, flag + " is given more than once");
        if (flag == "--source") {
            if (value != "host" && value != "viewport" && value != "window")
                return Fail(error, "--source needs host, viewport or window, got '" + value + "'");
            out[key] = value;
        } else if (flag == "--bless") {
            out[key] = true;
        } else if (flag == "--max-diff-pixels") {
            errno = 0;
            char* end = nullptr;
            const unsigned long long v = std::strtoull(value.c_str(), &end, 10);
            if (value.empty() || value[0] < '0' || value[0] > '9' || end != value.c_str() + value.size() ||
                errno == ERANGE || v > static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max()))
                return Fail(error, "--max-diff-pixels needs a non-negative whole number, got '" + value + "'");
            out[key] = static_cast<std::uint64_t>(v);
        } else {
            errno = 0;
            char* end = nullptr;
            const double r = std::strtod(value.c_str(), &end);
            const bool numeric = !value.empty() && ((value[0] >= '0' && value[0] <= '9') || value[0] == '.');
            if (!numeric || end != value.c_str() + value.size() || errno == ERANGE || !(r >= 0.0 && r <= 1.0))
                return Fail(error, "--max-diff-pixel-ratio needs a number from 0 to 1, got '" + value + "'");
            out[key] = r;
        }
    }
    return true;
}

// play [--topology T] -> {} or {topology}. The spelling is checked here and
// again by the host (arg.invalid), which owns the rule.
bool PlayArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("play"), rest, &tokens))
        return Fail(error, *problem);
    if (!tokens.positional.empty())
        return Fail(error, "play takes flags only (--topology T), got " + tokens.positional.front());
    out = nlohmann::json::object();
    for (const auto& [flag, value] : tokens.flags) {
        // --topology is the only flag the row declares; once (Rule 3: never last-wins).
        if (out.contains("topology"))
            return Fail(error, flag + " is given more than once");
        if (value != "standalone" && value != "listen" && value != "client+server")
            return Fail(error, "--topology needs standalone, listen or client+server, got '" + value + "'");
        out["topology"] = value;
    }
    return true;
}

// step [N] -> {} or {n}. N is a whole number of host.frame advances; the
// host owns the 1..100000 range (arg.inapplicable, exit 2).
bool StepArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("step"), rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.size() > 1)
        return Fail(error, "step takes one count N, got " + tokens.positional[1]);
    out = nlohmann::json::object();
    if (tokens.positional.empty())
        return true;
    const std::string& n = tokens.positional.front();
    const bool digits = !n.empty() && n.size() <= 9 &&
                        std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!digits)
        return Fail(error, "step needs a whole number N (1 to 100000), got '" + n + "'");
    out["n"] = std::stoul(n);
    return true;
}

bool SceneTreeArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("scene tree"), rest, &tokens))
        return Fail(error, *problem);
    if (!tokens.positional.empty())
        return Fail(error, "scene tree takes flags only (--depth N), got " + tokens.positional.front());
    out = nlohmann::json::object();
    for (const auto& [flag, value] : tokens.flags) {
        // --depth is the only flag the row declares.
        const bool digits = !value.empty() && value.size() <= 9 &&
                            std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (!digits)
            return Fail(error, "--depth needs a non-negative whole number, got " + value);
        out["depth"] = std::stoul(value);
    }
    return true;
}

// ecs query --with A,B [--without C] [--fields A.x,B] [--limit N] [--no-strict]
// -> {with, without?, fields?, limit?, strict?}.
bool EcsQueryArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("ecs query"), rest, &tokens))
        return Fail(error, *problem);
    if (!tokens.positional.empty())
        return Fail(error, "ecs query takes flags only (--with A,B), got " + tokens.positional.front());
    nlohmann::json with = nlohmann::json::array();
    nlohmann::json without = nlohmann::json::array();
    nlohmann::json fields = nlohmann::json::array();
    out = nlohmann::json::object();
    for (const auto& [flag, value] : tokens.flags) {
        if (flag == "--with") {
            AppendList(value, with);
        } else if (flag == "--without") {
            AppendList(value, without);
        } else if (flag == "--fields") {
            AppendList(value, fields);
        } else if (flag == "--limit") {
            const bool digits = !value.empty() && value.size() <= 9 &&
                                std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (!digits)
                return Fail(error, "--limit needs a non-negative whole number, got " + value);
            out["limit"] = std::stoul(value);
        } else if (flag == "--no-strict") {
            out["strict"] = false;
        }
    }
    out["with"] = std::move(with);
    if (!without.empty())
        out["without"] = std::move(without);
    if (!fields.empty())
        out["fields"] = std::move(fields);
    return true;
}

// quit: posts host.quit. With --client NAME, QuitByClient picks the host (the
// ones that client launched, --host's untagged host, or --pid) and runs QuitOne
// on it. With --force (spec s4.7) QuitOne then watches the target
// process (pid + creation stamp from its descriptor) until --timeout, measured
// from the start of the verb, and terminates it if it is still alive -- also
// when the host answered host.busy{modal} (--force does not discard changes;
// --discard-changes does) or did not answer at all. A host that is gone is
// exit 0 with data.forced (true when servitor killed it); what the host
// answered before the kill is kept in `warnings`.
int QuitVerb(std::span<const std::string> rest, const GlobalOptions& opt)
{
    CallRequest req;
    req.command = "host.quit";
    std::string error;
    if (!QuitArgs(rest, req.args, &error))
        return PrintUsage(error, opt.format);
    if (opt.clientSet) {
        // --client NAME on quit selects the hosts that client launched (their
        // descriptors record it), and is still the X-Remote-Client audit
        // header of the host.quit it sends. QuitByClient reads those
        // descriptors before any host scan.
        Arcane::Remote::Envelope refusal;
        const auto dir = RemoteDirFor(opt, &refusal);
        if (!dir) {
            refusal.command = "host.quit";
            Arcane::Remote::ClientBlock client;
            client.exitCode = Arcane::Remote::ExitFromHost(refusal);
            return PrintEnvelope(refusal, client, opt);
        }
        return QuitByClient(rest, opt, *dir, nullptr, nullptr);
    }
    return QuitOne(rest, opt);
}

} // namespace

int QuitOne(std::span<const std::string> rest, const GlobalOptions& opt)
{
    CallRequest req;
    req.command = "host.quit";
    std::string error;
    if (!QuitArgs(rest, req.args, &error))
        return PrintUsage(error, opt.format);
    const bool force = QuitForced(rest);
    const auto started = std::chrono::steady_clock::now();
    PostedCall posted = PostVerb(*FindVerb("quit"), req, opt, force);
    if (!force || !posted.host)
        return PrintEnvelope(posted.envelope, posted.client, opt);

    const Arcane::Remote::Descriptor& d = posted.host->descriptor;
    const auto alive = [&d] { return Arcane::Platform::IsProcessLive(d.pid, d.processStart); };
    const auto deadline = started + std::chrono::milliseconds(opt.timeoutMs);
    while (alive() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(kQuitPollMs));
    bool forced = false;
    if (alive()) {
        forced = Arcane::Platform::TerminateProcess(d.pid, d.processStart);
        const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(kKillSettleMs);
        while (alive() && std::chrono::steady_clock::now() < settle)
            std::this_thread::sleep_for(std::chrono::milliseconds(kQuitPollMs));
    }

    Arcane::Remote::Envelope env;
    if (!alive()) {
        nlohmann::json data = posted.envelope.ok && posted.envelope.data.is_object() ? posted.envelope.data
                                                                                   : nlohmann::json::object();
        data["forced"] = forced;
        data["exited"] = true;
        if (forced) {
            // A killed host cannot delete its own descriptor; it is stale now
            // (IsStale), so remove it the way a scan would.
            std::error_code ec;
            data["descriptorRemoved"] = std::filesystem::remove(posted.host->path, ec) && !ec;
        }
        env = Arcane::Remote::MakeOk("host.quit", std::move(data), posted.envelope.host);
        env.id = posted.envelope.id;
        env.timing = posted.envelope.timing;
        env.warnings = posted.envelope.warnings;
        if (!posted.envelope.ok) {
            for (const Arcane::Remote::Error& e : posted.envelope.errors)
                env.warnings.push_back(e);   // e.g. host.busy{modal}, or host.unreachable from a stuck host
        }
    } else {
        env = Arcane::Remote::MakeError("host.quit", "quit.force-failed",
                                        "host process " + std::to_string(d.pid) +
                                            " is still running after --force: the kill was refused",
                                        "end process " + std::to_string(d.pid) + " by hand");
        env.errors.front().detail["pid"] = d.pid;
        env.host = posted.envelope.host;
    }
    if (env.host.pid == 0) {
        env.host.pid = d.pid;
        env.host.kind = Arcane::Remote::ToString(d.hostKind);
    }
    posted.client.exitCode = Arcane::Remote::ExitFromHost(env);
    return PrintEnvelope(env, posted.client, opt);
}

namespace {

// ---------------------------------------------------------------------------
// logs and problems (Task 22). Both rows are client-side: logs falls back to
// the dead host's log file, and --follow on either opens the SSE stream
// (GET /remote/v1/stream?topics=logs|problems) instead of posting a call. Under
// --all the fan-out posts log.read / problems.list through LogsArgs /
// ProblemsArgs, which refuse --follow.

std::string Dump(const nlohmann::json& j)
{
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// --since N: a log seq, a non-negative whole number.
bool ParseSeq(const std::string& text, std::uint64_t& out)
{
    if (text.empty() || text[0] < '0' || text[0] > '9')
        return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
    if (end != text.c_str() + text.size() || errno == ERANGE)
        return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

// logs [--since N] [--level info|warn|error] [--follow] -> log.read {since?, level?}.
bool ParseLogsTokens(std::span<const std::string> rest, nlohmann::json& out, bool& follow, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("logs"), rest, &tokens))
        return Fail(error, *problem);
    if (!tokens.positional.empty())
        return Fail(error, "logs takes no arguments, got " + tokens.positional.front());
    out = nlohmann::json::object();
    follow = false;
    for (const auto& [flag, value] : tokens.flags) {
        if (flag == "--follow") {
            follow = true;
        } else if (flag == "--since") {
            std::uint64_t seq = 0;
            if (!ParseSeq(value, seq))
                return Fail(error, "--since needs a log seq (a non-negative whole number), got '" + value + "'");
            out["since"] = seq;
        } else if (flag == "--level") {
            if (value != "info" && value != "warn" && value != "error")
                return Fail(error, "--level is info, warn or error, got '" + value + "'");
            out["level"] = value;
        }
    }
    return true;
}

// problems [--severity error|warning|info] [--search TEXT] [--follow] -> problems.list {severity?, search?}.
bool ParseProblemsTokens(std::span<const std::string> rest, nlohmann::json& out, bool& follow, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("problems"), rest, &tokens))
        return Fail(error, *problem);
    if (!tokens.positional.empty())
        return Fail(error, "problems takes no arguments, got " + tokens.positional.front() +
                               " (quote a --search text with spaces)");
    out = nlohmann::json::object();
    follow = false;
    for (const auto& [flag, value] : tokens.flags) {
        if (flag == "--follow") {
            follow = true;
        } else if (flag == "--severity") {
            if (value != "error" && value != "warning" && value != "info")
                return Fail(error, "--severity is error, warning or info, got '" + value + "'");
            out["severity"] = value;
        } else if (flag == "--search") {
            out["search"] = value;
        }
    }
    return true;
}

// A member of a JSON row as text (a string as is, anything else dumped); "" when absent or null.
std::string Str(const nlohmann::json& row, const char* key)
{
    const auto it = row.is_object() ? row.find(key) : row.end();
    if (!row.is_object() || it == row.end() || it->is_null())
        return {};
    return it->is_string() ? it->get<std::string>() : Dump(*it);
}

// A log level's rank (spdlog order); -1 for a name that is not one.
int LevelRank(std::string_view name)
{
    if (name == "trace") return 0;
    if (name == "debug") return 1;
    if (name == "info") return 2;
    if (name == "warn" || name == "warning") return 3;
    if (name == "error" || name == "err") return 4;
    if (name == "critical") return 5;
    return -1;
}

// --since (seq <= N dropped) and --level (records ranked below it dropped) on
// one log.follow event. The {"dropped": N} event carries no seq and always passes.
bool PassesLogFilters(const nlohmann::json& record, const nlohmann::json& args)
{
    if (!record.is_object() || !record.contains("seq"))
        return true;
    const nlohmann::json& seq = record.at("seq");
    if (const auto since = args.find("since");
        since != args.end() && seq.is_number_unsigned() && seq.get<std::uint64_t>() <= since->get<std::uint64_t>())
        return false;
    if (const auto level = args.find("level"); level != args.end() && level->is_string()) {
        const auto name = record.find("level");
        const int rank = name != record.end() && name->is_string() ? LevelRank(name->get<std::string>()) : -1;
        if (rank < LevelRank(level->get<std::string>()))
            return false;
    }
    return true;
}

// --severity (exactly that severity) and --search on one problem.follow row:
// the host's own rule, Arcane::MatchesDiagnosticFilter (case-insensitive, in the
// message, code or detail), applied to the row's fields.
bool PassesProblemFilters(const nlohmann::json& row, const nlohmann::json& args)
{
    if (!row.is_object())
        return true;
    Arcane::Diagnostic d;
    const std::string severity = Str(row, "severity");
    d.severity = severity == "error"     ? Arcane::DiagSeverity::Error
                 : severity == "warning" ? Arcane::DiagSeverity::Warning
                                         : Arcane::DiagSeverity::Info;
    d.code = Str(row, "code");
    d.message = Str(row, "message");
    d.detail = Str(row, "detail");
    Arcane::SeverityMask mask = Arcane::SeverityMask::All;
    if (const auto wanted = args.find("severity"); wanted != args.end() && wanted->is_string()) {
        const std::string& name = wanted->get_ref<const std::string&>();
        mask = name == "error"     ? Arcane::SeverityMask::Error
               : name == "warning" ? Arcane::SeverityMask::Warning
                                   : Arcane::SeverityMask::Info;
    }
    const std::string search = args.value("search", std::string{});
    return Arcane::MatchesDiagnosticFilter(d, mask, search);
}

// The --all path (FanOutVerb): one call per host, so a stream is refused.
bool LogsArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    bool follow = false;
    if (!ParseLogsTokens(rest, out, follow, error))
        return false;
    if (follow)
        return Fail(error, "logs --follow streams from one host; it does not combine with --all");
    return true;
}

bool ProblemsArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    bool follow = false;
    if (!ParseProblemsTokens(rest, out, follow, error))
        return false;
    if (follow)
        return Fail(error, "problems --follow streams from one host; it does not combine with --all");
    return true;
}

// The selected host's identity for a client-made envelope's `host` block.
Arcane::Remote::HostBlock BlockOf(const Arcane::Remote::Descriptor& d)
{
    Arcane::Remote::HostBlock block;
    block.kind = Arcane::Remote::ToString(d.hostKind);
    block.pid = d.pid;
    block.state = d.state;
    block.frame = d.frame;
    return block;
}

// One human line for a log.read record (also a log.follow event), or for the
// {"dropped": N} event a follow publishes when the ring evicted lines first.
std::string HumanLogLine(const nlohmann::json& record)
{
    if (record.is_object() && record.contains("dropped") && !record.contains("seq"))
        return "(" + Str(record, "dropped") + " lines dropped: the host's ring evicted them first)\n";
    return Str(record, "seq") + " [" + Str(record, "level") + "] " + Str(record, "logger") + ": " +
           Str(record, "text") + "\n";
}

// One human line (and its indented detail and locator) for a problems.list row.
std::string HumanProblemLines(const nlohmann::json& row)
{
    std::string text = Str(row, "severity") + " " + Str(row, "code") + ": " + Str(row, "message") + "\n";
    if (const std::string detail = Str(row, "detail"); !detail.empty())
        text += "  " + detail + "\n";
    if (row.is_object() && row.contains("locator") && !row.at("locator").is_null())
        text += "  at " + Dump(row.at("locator")) + "\n";
    return text;
}

// Human output for log.read / problems.list: the summary line and diagnostics
// as Render prints them, then one line per record or row (a file fallback's
// text as it is); notifications as "note" lines on stderr. json and ndjson,
// and any refusal, print through PrintEnvelope unchanged.
int PrintRows(const Arcane::Remote::Envelope& env, const Arcane::Remote::ClientBlock& client,
              const GlobalOptions& opt)
{
    if (opt.format != Format::Human || !env.ok || !env.data.is_object())
        return PrintEnvelope(env, client, opt);
    Arcane::Remote::Envelope summary = env;
    summary.data = nlohmann::json::object();
    RenderOptions ro;
    ro.format = Format::Human;
    ro.tty = opt.tty;
    Printed printed = Render(summary, client, ro);
    std::string rows;
    if (const auto text = env.data.find("text"); text != env.data.end() && text->is_string()) {
        rows = text->get<std::string>();
        if (!rows.empty() && rows.back() != '\n')
            rows += '\n';
    }
    if (const auto records = env.data.find("records"); records != env.data.end() && records->is_array()) {
        for (const nlohmann::json& record : *records)
            rows += HumanLogLine(record);
    }
    if (const auto problems = env.data.find("problems"); problems != env.data.end() && problems->is_array()) {
        for (const nlohmann::json& row : *problems)
            rows += HumanProblemLines(row);
    }
    std::string notes;
    if (const auto dropped = env.data.find("dropped");
        dropped != env.data.end() && dropped->is_number_unsigned() && dropped->get<std::uint64_t>() > 0)
        notes += "note: " + Dump(*dropped) + " older lines were already evicted from the host's ring\n";
    for (const Arcane::Remote::Error& n : env.notifications)
        notes += "note " + n.code + ": " + n.message + "\n";
    printed.stdoutText += Arcane::Remote::RedactSecrets(rows, client.token);
    printed.stderrText += Arcane::Remote::RedactSecrets(notes, client.token);
    return Emit(printed, client.exitCode);
}

// What one follow prints per event, on the calling thread.
struct FollowPrint {
    Format format = Format::Json;
    std::string topic;
    nlohmann::json args;      // the verb's filters (log.read / problems.list args), applied per event
    std::string token;        // print-time redaction only; never printed
    std::size_t printed = 0;  // events that passed the filters
};

// One stream event through FollowEventText (filters + format), to stdout.
void PrintFollowEvent(std::string_view data, void* user)
{
    auto& ctx = *static_cast<FollowPrint*>(user);
    const std::string line = FollowEventText(ctx.topic, data, ctx.args, ctx.format);
    if (line.empty())
        return;
    ++ctx.printed;
    Printed printed;
    printed.stdoutText = Arcane::Remote::RedactSecrets(line, ctx.token);
    (void)Emit(printed, 0);
}

// `logs --follow` / `problems --follow`: one SSE subscription to the selected
// host until --timeout, the host closing it, or Ctrl+C / SIGTERM (exit 130 /
// 143). It never posts log.read or problems.list with follow (ruling P2 (f)).
// The stream carries every record, so the verb's own filters (--since, --level,
// --severity, --search) are applied here, per event (FollowEventText).
// json / ndjson end with {"type":"result","count":<printed events>,"envelope":...},
// so a truncated stream is detectable; human prints only a failure.
int FollowVerb(std::string_view words, const char* topic, const char* command, const nlohmann::json& args,
               const GlobalOptions& opt)
{
    // Rule 3: a stream posts no call, so these globals would be dropped silently.
    if (const auto problem = FollowUnusedGlobal(words, opt))
        return PrintUsage(*problem, opt.format);

    Arcane::Remote::Envelope env;
    Arcane::Remote::ClientBlock client;
    std::optional<LiveHost> host;
    if (const auto dir = RemoteDirFor(opt, &env))
        host = Select(ScanDirectory(*dir), opt, &env);
    if (!host) {
        env.command = command;
        client.exitCode = Arcane::Remote::ExitFromHost(env);
        return PrintEnvelope(env, client, opt);
    }

    LiveTarget target;
    target.descriptor = host->descriptor;
    target.watch = WatchFor(*host, command, opt);
    target.timeoutMs = opt.timeoutMs;
    client.token = host->descriptor.token;
    client.descriptor = PathUtf8(host->path);
    FollowPrint print;
    print.format = opt.format;
    print.topic = topic;
    print.args = args;
    print.token = host->descriptor.token;

    StreamOutcome outcome;
    {
        InterruptScope interrupts;
        outcome = FollowStream(target, topic, &InterruptScope::Pending(), &PrintFollowEvent, &print);
    }
    const auto ended = [&](const char* reason) {
        return Arcane::Remote::MakeOk(command, {{"events", outcome.events}, {"printed", print.printed}, {"ended", reason}},
                                      BlockOf(host->descriptor));
    };
    switch (outcome.end) {
        case StreamOutcome::End::Timeout:
            env = ended("timeout");
            client.exitCode = 0;
            break;
        case StreamOutcome::End::Closed:
            env = ended("closed");
            client.exitCode = 0;
            break;
        case StreamOutcome::End::Interrupted:
            env = ended("interrupted");
            client.exitCode = outcome.interrupt;
            break;
        case StreamOutcome::End::Failed:
            env = std::move(outcome.failure);
            if (env.command.empty())
                env.command = command;
            client.exitCode = Arcane::Remote::ExitFromHost(env);
            break;
    }
    if (opt.format == Format::Human) {
        if (env.ok)
            return client.exitCode;
        return PrintEnvelope(env, client, opt);
    }
    nlohmann::json envelope = Arcane::Remote::ToJson(env);
    envelope["client"] = {{"exitCode", client.exitCode}, {"retried", false}, {"descriptor", client.descriptor}};
    Printed printed;
    printed.stdoutText = Arcane::Remote::RedactSecrets(
        Dump(nlohmann::json{{"type", "result"}, {"count", print.printed}, {"envelope", envelope}}) + "\n",
        client.token);
    return Emit(printed, client.exitCode);
}

// The newest existing log file of a dead host: the kinds --host names, else the
// kinds of the stale descriptors this scan found (their names are <kind>-<pid>.json),
// else all three; the project the scan was for, else the exe-relative logs.
std::filesystem::path DeadHostLogFile(const Scan& scan, const GlobalOptions& opt)
{
    using Arcane::Remote::HostKind;
    std::vector<HostKind> kinds;
    const auto kindOf = [](std::string_view name) -> std::optional<HostKind> {
        if (name == "editor") return HostKind::Editor;
        if (name == "runtime") return HostKind::Runtime;
        if (name == "server") return HostKind::Server;
        return std::nullopt;
    };
    if (!opt.hostKind.empty()) {
        if (const auto kind = kindOf(opt.hostKind))
            kinds.push_back(*kind);
    } else {
        for (const auto* list : {&scan.removedStale, &scan.keptStale}) {
            for (const std::filesystem::path& stale : *list) {
                const std::string stem = stale.stem().string();
                const auto kind = kindOf(std::string_view(stem).substr(0, stem.find('-')));
                if (kind && std::find(kinds.begin(), kinds.end(), *kind) == kinds.end())
                    kinds.push_back(*kind);
            }
        }
        if (kinds.empty())
            kinds = {HostKind::Editor, HostKind::Runtime, HostKind::Server};
    }
    const std::filesystem::path project = ProjectRootFor(opt, nullptr).value_or(std::filesystem::path{});
    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    for (const HostKind kind : kinds) {
        const std::filesystem::path file = HostLogFile(kind, project, opt);
        if (file.empty())
            continue;
        std::error_code ec;
        const auto written = std::filesystem::last_write_time(file, ec);
        if (ec)
            continue;   // no such file
        if (best.empty() || written > bestTime) {
            best = file;
            bestTime = written;
        }
    }
    return best;
}

int LogsVerb(std::span<const std::string> rest, const GlobalOptions& opt)
{
    CallRequest req;
    req.command = "log.read";
    bool follow = false;
    std::string error;
    if (!ParseLogsTokens(rest, req.args, follow, &error))
        return PrintUsage(error, opt.format);
    if (follow)
        return FollowVerb("logs", "logs", "log.follow", req.args, opt);

    Arcane::Remote::Envelope refusal;
    Arcane::Remote::ClientBlock client;
    Scan scan;
    std::optional<LiveHost> host;
    if (const auto dir = RemoteDirFor(opt, &refusal)) {
        scan = ScanDirectory(*dir);
        host = Select(scan, opt, &refusal);
    }
    if (host) {
        const PostedCall posted = PostToHost(*host, req, opt, false);
        return PrintRows(posted.envelope, posted.client, opt);
    }
    // No live host: the dead host's log file, when there is one (never invented lines).
    const bool none = !refusal.errors.empty() && refusal.errors.front().code == "target.none";
    if (none) {
        if (const std::filesystem::path file = DeadHostLogFile(scan, opt); !file.empty()) {
            Arcane::Remote::Envelope env;
            ReadLogFile(file, env);
            if (env.ok && !req.args.empty()) {
                Arcane::Remote::Error warning;
                warning.code = "logs.unfiltered";
                warning.message = "--since and --level filter the live ring; the log file is printed whole";
                env.warnings.push_back(std::move(warning));
            }
            client.exitCode = Arcane::Remote::ExitFromHost(env);
            return PrintRows(env, client, opt);
        }
    }
    refusal.command = req.command;
    client.exitCode = Arcane::Remote::ExitFromHost(refusal);
    return PrintEnvelope(refusal, client, opt);
}

int ProblemsVerb(std::span<const std::string> rest, const GlobalOptions& opt)
{
    CallRequest req;
    req.command = "problems.list";
    bool follow = false;
    std::string error;
    if (!ParseProblemsTokens(rest, req.args, follow, &error))
        return PrintUsage(error, opt.format);
    if (follow)
        return FollowVerb("problems", "problems", "problem.follow", req.args, opt);
    const PostedCall posted = PostVerb(*FindVerb("problems"), req, opt, false);
    return PrintRows(posted.envelope, posted.client, opt);
}

// How many leading tokens spell `words` (space-separated); 0 when they do not.
std::size_t WordsMatched(std::string_view words, std::span<const std::string> tokens)
{
    std::size_t used = 0;
    while (!words.empty()) {
        const std::size_t space = words.find(' ');
        const std::string_view word = words.substr(0, space);
        if (used >= tokens.size() || tokens[used] != word)
            return 0;
        ++used;
        words = space == std::string_view::npos ? std::string_view{} : words.substr(space + 1);
    }
    return used;
}

} // namespace

std::string FollowEventText(std::string_view topic, std::string_view data, const nlohmann::json& args, Format format)
{
    nlohmann::json parsed = Arcane::Remote::ParseJsonBounded(data, Arcane::Remote::kMaxReplyDepth);   // a host stream event
    if (parsed.is_discarded())
        parsed = std::string(data);
    const bool logs = topic == "logs";
    const nlohmann::json filters = args.is_object() ? args : nlohmann::json::object();
    if (logs ? !PassesLogFilters(parsed, filters) : !PassesProblemFilters(parsed, filters))
        return {};
    if (format != Format::Human)
        return Dump(nlohmann::json{{"type", "event"}, {"topic", std::string(topic)}, {"data", parsed}}) + "\n";
    return logs ? HumanLogLine(parsed) : HumanProblemLines(parsed);
}

std::span<const VerbRow> VerbTable()
{
    return kVerbs;
}

const VerbRow* MatchVerb(std::span<const std::string> tokens, std::size_t* consumed)
{
    const VerbRow* best = nullptr;
    std::size_t bestUsed = 0;
    for (const VerbRow& row : kVerbs) {
        const std::size_t used = WordsMatched(row.words, tokens);
        if (used > bestUsed) {
            best = &row;
            bestUsed = used;
        }
    }
    if (consumed != nullptr)
        *consumed = bestUsed;
    return best;
}

const VerbRow* FindVerb(std::string_view words)
{
    for (const VerbRow& row : kVerbs) {
        if (words == row.words)
            return &row;
    }
    return nullptr;
}

bool PostsCall(const VerbRow& row)
{
    return row.command != nullptr || std::string_view(row.words) == "call";
}

std::optional<std::string> UnusedGlobal(const VerbRow& row, const GlobalOptions& opt)
{
    // --yes confirms a Drive command fanned out to several hosts (spec AM2);
    // without --all it confirms nothing (final fix pass, C-I4). One exception:
    // `quit --client NAME` fans out to the hosts that client launched, and its
    // --yes confirms that several-host selection (it still does not mean --all).
    const std::string_view verbWords = row.words;
    const bool quitByClient = verbWords == "quit" && opt.clientSet;
    if (opt.yes && !opt.all && !quitByClient)
        return std::string("--yes applies only with --all (it confirms a Drive command sent to several hosts)");
    // Round 2 (I-1): under --all a rediscovered "replacement" could be a
    // sibling the fan-out already reached, so a resend could run twice there.
    if (opt.all && opt.retryOnDeath)
        return std::string("--retry-on-death does not apply with --all (a resend could reach a host the fan-out "
                           "already reached); retry one host kind at a time with --host <kind>");
    // Round 4 (N3-1): a restarted host always has a new pid, so a pinned pid
    // can never be retargeted; a kind selects the replacement.
    if (opt.pid != 0 && opt.retryOnDeath)
        return std::string("--retry-on-death does not apply with --pid (a restarted host has a new pid, so a pid "
                           "is never retargeted); select the host with --host <kind>");
    const std::string_view words = row.words;
    // launch spawns a new host and commands prints the static manifest: neither
    // selects a host, so --host and --pid would be dropped (status filters by
    // them, list and every posting verb select by them).
    if (words == "launch" || words == "commands") {
        if (!opt.hostKind.empty())
            return "--host does not apply to " + std::string(words);
        if (opt.pid != 0)
            return "--pid does not apply to " + std::string(words);
    }
    if (PostsCall(row))
        return std::nullopt;
    // Round 2 (M-1): X-Remote-Client rides a /call only. launch and status take
    // --client too: launch forwards it to the host (the descriptor records it)
    // and status lists only the hosts that client launched.
    if (opt.clientSet && words != "launch" && words != "status")
        return "--client does not apply to " + std::string(row.words);
    if (!opt.world.empty())
        return "--world does not apply to " + std::string(row.words);
    if (opt.readOnly)
        return "--read-only does not apply to " + std::string(row.words);
    return std::nullopt;
}

std::optional<std::string> SplitVerbTokens(const VerbRow& row, std::span<const std::string> rest, VerbTokens* out)
{
    VerbTokens local;
    VerbTokens& tokens = out != nullptr ? *out : local;
    tokens = VerbTokens{};
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& token = rest[i];
        if (token == "--" && row.passThrough) {
            tokens.passThrough.assign(rest.begin() + static_cast<std::ptrdiff_t>(i) + 1, rest.end());
            return std::nullopt;
        }
        if (!token.starts_with("--")) {   // a single '-' stays a value (negative numbers)
            tokens.positional.push_back(token);
            continue;
        }
        const VerbFlag* flag = nullptr;
        for (const VerbFlag& f : row.flags) {
            if (token == f.name)
                flag = &f;
        }
        if (flag == nullptr)
            return "unknown flag " + token;
        if (flag->refused != nullptr)
            return std::string(flag->refused);
        if (!flag->takesValue) {
            tokens.flags.emplace_back(token, std::string{});
            continue;
        }
        if (i + 1 >= rest.size() || rest[i + 1].starts_with("--"))
            return token + " needs a value";
        tokens.flags.emplace_back(token, rest[++i]);
    }
    return std::nullopt;
}

} // namespace Servitor

#endif
