#pragma once
#include <Arcane/Modules/ArcaneRemoteProtocolApi.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Json.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Remote
{

// The two remote.server.* cvar defaults sit outside the Dist guard: Dispatcher.cpp
// declares those Dev cvars in every configuration, so Dist's registry refuses
// them by name (IsCompiledOut) instead of never seeing them.
ARC_CONSTANT("a change would be a bug: the one spelling of remote.server.queueDepth's default")
inline constexpr std::uint32_t kQueueDepthDefault = 64;
ARC_CONSTANT("a change would be a bug: the one spelling of remote.server.frameBudgetMs' default")
inline constexpr double kFrameBudgetMs = 4.0;

} // namespace Arcane::Remote

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

ARC_CONSTANT("wire protocol: remote protocol major (spec s6)")
inline constexpr std::uint32_t kProtocol = 1;
ARC_CONSTANT("remote protocol minor; raised on every additive wire change (spec AM9); remote-protocol.json mirrors it")
inline constexpr std::uint32_t kProtocolMinor = 0;
ARC_CONSTANT("wire protocol: s10.1 request body bound")
inline constexpr std::uint32_t kMaxBodyBytes = 4u * 1024u * 1024u;
ARC_CONSTANT("wire protocol: s10.1 header block bound (16 KiB)")
inline constexpr std::uint32_t kMaxHeaderBytes = 16u * 1024u;
ARC_CONSTANT("capacity: s10.1 connection cap, under Winsock FD_SETSIZE (64); POSIX polls with poll(2)")
inline constexpr std::uint32_t kMaxConnections = 16;
ARC_CONSTANT("capacity: s10.1 SSE subscriber cap")
inline constexpr std::uint32_t kMaxSseSubscribers = 8;
ARC_CONSTANT("capacity hint: s4.3 default commands drained per tick")
inline constexpr std::uint32_t kFrameBudgetCommands = 8;
ARC_CONSTANT("wire protocol: s10.1 idle connection close")
inline constexpr std::uint32_t kIdleConnectionSeconds = 30;
// The two nesting caps (Task 30 fix round 1). nlohmann parses iteratively,
// but a copy or a dump recurses once per level, and the only Debug figure is a
// FLOOR, not a measurement: a bare 256-level chain overflowed a 1 MiB stack
// (SceneCommands.cpp), so a level costs at least ~4 KiB there. The at-limit
// tests ([remote][host], [remote][dispatch], [servitor][client]) push exactly
// each cap through the paths that recurse.
ARC_CONSTANT("stack bound: what a HOST accepts (request bodies, args, any host-side parse); a request needs a few levels, and the listener's protocol dump and the audit's argsDigest dump recurse through all of them on the listener and main threads, so 64 keeps them far under the ~256-level Debug overflow floor")
inline constexpr std::uint32_t kMaxRequestDepth = 64;
ARC_CONSTANT("stack bound: what SERVITOR accepts from a host (replies, stream events); scene.tree's deepest legal reply nests ~134 containers (64 levels below the roots, two per level), so 160 admits every reply a host builds and stays under the ~256-level Debug overflow floor in servitor's copy and print")
inline constexpr std::uint32_t kMaxReplyDepth = 160;
ARC_CONSTANT("wire protocol: spec s4.5's caller name is a short label for the audit log, never a payload")
inline constexpr std::size_t kMaxClientIdBytes = 128;

// spec s4.5 (AM7 spelling): the request header naming the caller, recorded as
// the audit line's `client` (s10.4). Servitor sends "servitor/<version> pid
// <pid>" unless --client <name> names it.
inline constexpr std::string_view kClientHeader = "X-Remote-Client";
// A caller name both sides accept: 1 to kMaxClientIdBytes bytes of printable
// ASCII (0x20-0x7E). A host refuses any other X-Remote-Client value with
// arg.invalid (exit 2); servitor refuses such a --client before it sends.
ARC_REMOTE_PROTOCOL_API bool IsValidClientId(std::string_view id);

enum class HostKind : std::uint8_t { Editor, Runtime, Server };

// spec s4.3: the host-state vocabulary HostStatus.state, the descriptor and
// host.wait `state` speak. A state outside it is a usage error (exit 2).
inline constexpr std::array<std::string_view, 10> kHostStateNames = {
    "booting", "ready", "reloading", "compiling-shaders", "building",
    "modal", "playing", "paused", "minimized", "shutting-down",
};
inline bool IsHostStateName(std::string_view state)
{
    for (const std::string_view name : kHostStateNames)
        if (name == state)
            return true;
    return false;
}
// The one "serves commands" predicate (servitor launch, wait ready).
inline bool IsReadyState(std::string_view state)
{
    return state == "ready" || state == "playing" || state == "paused";
}
enum class HostState : std::uint8_t { Starting, Ready, Playing, Paused, Reloading, Stopping, Hung };
enum class Access : std::uint8_t { Observe, Drive };
enum class Mutates : std::uint8_t { None, Scene, Project, Preferences, PlayMode };
enum class PlayPolicy : std::uint8_t { Any, EditOnly, PlayingOnly };
enum class ExecThread : std::uint8_t { Main, Service, Job };

struct Error {
    std::string code;
    std::string message;
    std::string remediation;
    nlohmann::json detail = nlohmann::json::object();
    nlohmann::json locator = nullptr;
    std::vector<std::string> candidates;
    std::string check;
};

struct HostBlock {
    std::string kind;                 // "editor" | "runtime" | "server"
    std::uint32_t pid = 0;
    std::string world;
    std::uint64_t frame = 0;
    std::string state;
    std::uint32_t catalog = 0;
    std::uint32_t pluginGeneration = 0;
};

struct Artifact {
    std::string kind;
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::string format;
};

struct UndoBlock {
    nlohmann::json label = nullptr;   // string or JSON null
    bool undoable = false;
    std::string reason;               // play-mode | runtime-host | not-undoable-command
    std::uint64_t transaction = 0;
};

struct Timing {
    double queuedMs = 0;
    double ranMs = 0;
};

struct Envelope {
    bool ok = false;
    int protocol = 1;
    std::uint32_t protocolMinor = kProtocolMinor;
    std::string id;
    std::string command;
    HostBlock host;
    nlohmann::json data = nlohmann::json::object();
    std::vector<Error> errors;
    std::vector<Error> warnings;
    std::vector<Error> notifications;
    std::vector<Artifact> artifacts;
    std::optional<UndoBlock> undo;    // nullopt serializes as null
    Timing timing;
};

struct ClientBlock {
    int exitCode = 0;
    bool retried = false;
    std::string descriptor;
    std::string token; // print-time only; ToJson(Envelope) never emits this
};

struct HostStatus {
    std::uint32_t protocol = kProtocol;
    std::uint32_t protocolMinor = kProtocolMinor;
    HostKind hostKind = HostKind::Editor;
    std::uint32_t pid = 0;
    std::uint64_t processStart = 0;
    std::uint16_t port = 0;
    std::string state;
    std::uint64_t frame = 0;
    std::string world;
    std::uint32_t abi = 0;
    std::string project;
    std::uint32_t catalogueGeneration = 0;
    std::uint32_t pluginGeneration = 0;
    bool reloadOk = true;
    std::string reloadReason;
    std::uint64_t reloadAtFrame = 0;
    std::vector<std::string> capabilities;
    std::int64_t heartbeatUnixMs = 0;
    bool minimized = false;
};

ARC_REMOTE_PROTOCOL_API const char* ToString(HostKind kind);
ARC_REMOTE_PROTOCOL_API nlohmann::json ToJson(const Envelope& env);
ARC_REMOTE_PROTOCOL_API nlohmann::json ToJson(const HostStatus& status); // no token field exists on HostStatus
ARC_REMOTE_PROTOCOL_API Envelope ParseEnvelope(const nlohmann::json& j);
ARC_REMOTE_PROTOCOL_API std::optional<Envelope> TryParseEnvelope(const nlohmann::json& j); // nullopt when malformed
ARC_REMOTE_PROTOCOL_API Envelope MakeOk(std::string command, nlohmann::json data, HostBlock host);
ARC_REMOTE_PROTOCOL_API Envelope MakeError(std::string command, std::string code, std::string message, std::string remediation);
ARC_REMOTE_PROTOCOL_API int ExitFromHost(const Envelope& env);
ARC_REMOTE_PROTOCOL_API int WorstExit(std::span<const int> codes);
// The one parse of untrusted or unbounded JSON on both sides of the wire:
// nlohmann's iterative parser with a callback that refuses any container
// nested deeper than maxDepth (the outermost container is level 1). The cap is
// always named at the call: kMaxRequestDepth for what a host accepts and for
// servitor's own request JSON (--args-json, name=value: refused early, as the
// host would), kMaxReplyDepth for what servitor accepts from a host. Returns a
// discarded value (is_discarded()) when the text is not JSON or nests too
// deep, and never throws. *tooDeep (when non-null) is true only for the depth
// refusal. A container past the limit is never built, so a body of a million
// '[' costs no stack here or in a later copy or dump.
ARC_REMOTE_PROTOCOL_API nlohmann::json ParseJsonBounded(std::string_view text, std::uint32_t maxDepth,
                                                        bool* tooDeep = nullptr);
// True when ExitForCode lists `code` explicitly (a named entry or the
// *.not-found rule), false when it falls to ruling F6's default (exit 6). The
// exit-table test pins every code the spec's s6.4 table names with it, so
// deleting an entry whose exit equals the default still fails a CHECK.
ARC_REMOTE_PROTOCOL_API bool ExitCodeIsListed(std::string_view code);

} // namespace Arcane::Remote

#else
// Dist builds compile the remote surface out; this empty branch keeps the include legal.
#endif
