#include <Arcane/Remote/Protocol.hpp>

#if !defined(ARC_BUILD_DIST)

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

namespace Arcane::Remote
{

namespace
{

bool OneOf(std::string_view code, std::initializer_list<std::string_view> codes)
{
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

// The listed exit of `code`; nullopt when it is not listed (ruling F6's default applies).
std::optional<int> ListedExit(std::string_view code)
{
    if (code.empty()) return 2;
    if (code == "internal") return 1;
    if (OneOf(code, { "arg.invalid", "arg.unknown", "arg.missing", "arg.inapplicable", "command.unknown",
                      "command.unavailable", "command.host-kind", "target.ambiguous", "component.ambiguous",
                      "entity.ambiguous", "resource.ambiguous", "capture.no-renderer" }))
        return 2;
    if (OneOf(code, { "auth.denied", "auth.throttled", "permission.denied", "permission.read-only", "cvar.protected" }))
        return 3;
    if (OneOf(code, { "protocol.mismatch", "target.none", "engine.unresolved" })) return 4;
    if (OneOf(code, { "host.busy", "capture.minimized", "host.unavailable" })) return 5;
    if (OneOf(code, { "entity.not-found", "component.unknown", "component.absent", "compare.missing-reference",
                      "capture.no-frame", "cvar.unknown", "cvar.type-mismatch", "cvar.refused-weaker", "cvar.refused",
                      "reflect.unreadable", "ecs.artifact-failed", "resource.artifact-failed",
                      "scene.artifact-failed", "capture.artifact-failed", "compare.reference-unreadable",
                      "compare.bless-failed", "compare.artifact-failed", "play.not-playing", "play.refused",
                      "play.stopped", "module.reload-failed", "quit.force-failed", "log.artifact-failed" }))
        return 6;
    if (code.size() >= 10 && code.ends_with(".not-found")) return 6;
    if (OneOf(code, { "host.unreachable", "host.died", "host.restarting", "host.hung" })) return 7;
    if (OneOf(code, { "compare.failed", "wait.timeout", "call.timeout", "launch.timeout" })) return 8;
    return std::nullopt;
}

int ExitForCode(const std::string& code)
{
    return ListedExit(code).value_or(6); // ruling F6: the host answered and the operation failed
}

int Rank(int code)
{
    switch (code) {
        case 7: return 80;
        case 6: return 70;
        case 8: return 60;
        case 5: return 50;
        case 4: return 40;
        case 3: return 30;
        case 2: return 20;
        case 1: return 10;
        case 0: return 0;
        default: return 20;
    }
}

nlohmann::json ErrorToJson(const Error& e)
{
    nlohmann::json j = {
        { "code", e.code },
        { "message", e.message },
        { "remediation", e.remediation },
        { "detail", e.detail },
    };
    if (!e.locator.is_null()) j["locator"] = e.locator;
    if (!e.candidates.empty()) j["candidates"] = e.candidates;
    if (!e.check.empty()) j["check"] = e.check;
    return j;
}

nlohmann::json ErrorsToJson(const std::vector<Error>& errors)
{
    auto arr = nlohmann::json::array();
    for (const auto& e : errors) arr.push_back(ErrorToJson(e));
    return arr;
}

nlohmann::json ArtifactToJson(const Artifact& a)
{
    nlohmann::json j = {
        { "kind", a.kind },
        { "path", a.path },
        { "bytes", a.bytes },
        { "sha256", a.sha256 },
        { "format", a.format },
    };
    if (a.width) j["width"] = *a.width;
    if (a.height) j["height"] = *a.height;
    return j;
}

template <class T>
T Get(const nlohmann::json& j, const char* key, T fallback)
{
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

Error ErrorFromJson(const nlohmann::json& j)
{
    Error e;
    if (!j.is_object()) return e;
    e.code = Get<std::string>(j, "code", {});
    e.message = Get<std::string>(j, "message", {});
    e.remediation = Get<std::string>(j, "remediation", {});
    if (j.contains("detail") && j["detail"].is_object()) e.detail = j["detail"];
    if (j.contains("locator")) e.locator = j["locator"];
    e.candidates = Get<std::vector<std::string>>(j, "candidates", {});
    e.check = Get<std::string>(j, "check", {});
    return e;
}

std::vector<Error> ErrorsFromJson(const nlohmann::json& j, const char* key)
{
    std::vector<Error> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& e : *it) out.push_back(ErrorFromJson(e));
    return out;
}

Artifact ArtifactFromJson(const nlohmann::json& j)
{
    Artifact a;
    if (!j.is_object()) return a;
    a.kind = Get<std::string>(j, "kind", {});
    a.path = Get<std::string>(j, "path", {});
    a.bytes = Get<std::uint64_t>(j, "bytes", 0);
    a.sha256 = Get<std::string>(j, "sha256", {});
    a.format = Get<std::string>(j, "format", {});
    if (j.contains("width") && j["width"].is_number_unsigned()) a.width = j["width"].get<std::uint32_t>();
    if (j.contains("height") && j["height"].is_number_unsigned()) a.height = j["height"].get<std::uint32_t>();
    return a;
}

} // namespace

const char* ToString(HostKind kind)
{
    switch (kind) {
        case HostKind::Editor: return "editor";
        case HostKind::Runtime: return "runtime";
        case HostKind::Server: return "server";
    }
    return "editor";
}

nlohmann::json ToJson(const Envelope& env)
{
    nlohmann::json j;
    j["ok"] = env.ok;
    j["protocol"] = env.protocol;
    j["protocolMinor"] = env.protocolMinor;
    j["id"] = env.id;
    j["command"] = env.command;
    j["host"] = {
        { "kind", env.host.kind },
        { "pid", env.host.pid },
        { "world", env.host.world },
        { "frame", env.host.frame },
        { "state", env.host.state },
        { "catalog", env.host.catalog },
        { "pluginGeneration", env.host.pluginGeneration },
    };
    j["data"] = env.data;
    j["errors"] = ErrorsToJson(env.errors);
    j["warnings"] = ErrorsToJson(env.warnings);
    j["notifications"] = ErrorsToJson(env.notifications);
    auto artifacts = nlohmann::json::array();
    for (const auto& a : env.artifacts) artifacts.push_back(ArtifactToJson(a));
    j["artifacts"] = std::move(artifacts);
    if (env.undo) {
        nlohmann::json u = {
            { "label", env.undo->label },
            { "undoable", env.undo->undoable },
            { "transaction", env.undo->transaction },
        };
        if (!env.undo->reason.empty()) u["reason"] = env.undo->reason;
        j["undo"] = std::move(u);
    } else {
        j["undo"] = nullptr;
    }
    j["timing"] = { { "queuedMs", env.timing.queuedMs }, { "ranMs", env.timing.ranMs } };
    return j;
}

nlohmann::json ToJson(const HostStatus& s)
{
    return {
        { "protocol", s.protocol },
        { "protocolMinor", s.protocolMinor },
        { "kind", ToString(s.hostKind) },
        { "pid", s.pid },
        { "processStart", s.processStart },
        { "port", s.port },
        { "state", s.state },
        { "frame", s.frame },
        { "world", s.world },
        { "abi", s.abi },
        { "project", s.project },
        { "catalogueGeneration", s.catalogueGeneration },
        { "pluginGeneration", s.pluginGeneration },
        { "reload", { { "ok", s.reloadOk }, { "reason", s.reloadReason }, { "atFrame", s.reloadAtFrame } } },
        { "capabilities", s.capabilities },
        { "heartbeatUnixMs", s.heartbeatUnixMs },
        { "minimized", s.minimized },
    };
}

namespace
{
Envelope ExtractEnvelope(const nlohmann::json& j)
{
    Envelope env;
    if (!j.is_object()) return env;
    env.ok = Get<bool>(j, "ok", false);
    env.protocol = Get<int>(j, "protocol", 1);
    env.protocolMinor = Get<std::uint32_t>(j, "protocolMinor", 0); // absent is an older host: 0, never the struct default
    env.id = Get<std::string>(j, "id", {});
    env.command = Get<std::string>(j, "command", {});
    if (const auto it = j.find("host"); it != j.end() && it->is_object()) {
        env.host.kind = Get<std::string>(*it, "kind", {});
        env.host.pid = Get<std::uint32_t>(*it, "pid", 0);
        env.host.world = Get<std::string>(*it, "world", {});
        env.host.frame = Get<std::uint64_t>(*it, "frame", 0);
        env.host.state = Get<std::string>(*it, "state", {});
        env.host.catalog = Get<std::uint32_t>(*it, "catalog", 0);
        env.host.pluginGeneration = Get<std::uint32_t>(*it, "pluginGeneration", 0);
    }
    if (const auto it = j.find("data"); it != j.end()) env.data = *it;
    env.errors = ErrorsFromJson(j, "errors");
    env.warnings = ErrorsFromJson(j, "warnings");
    env.notifications = ErrorsFromJson(j, "notifications");
    if (const auto it = j.find("artifacts"); it != j.end() && it->is_array())
        for (const auto& a : *it) env.artifacts.push_back(ArtifactFromJson(a));
    if (const auto it = j.find("undo"); it != j.end() && it->is_object()) {
        UndoBlock u;
        if (it->contains("label")) u.label = (*it)["label"];
        u.undoable = Get<bool>(*it, "undoable", false);
        u.reason = Get<std::string>(*it, "reason", {});
        u.transaction = Get<std::uint64_t>(*it, "transaction", 0);
        env.undo = std::move(u);
    }
    if (const auto it = j.find("timing"); it != j.end() && it->is_object()) {
        env.timing.queuedMs = Get<double>(*it, "queuedMs", 0.0);
        env.timing.ranMs = Get<double>(*it, "ranMs", 0.0);
    }
    return env;
}

bool WellFormed(const nlohmann::json& j)
{
    if (!j.is_object()) return false;
    const auto ok = j.find("ok");
    if (ok == j.end() || !ok->is_boolean()) return false;
    const auto protocol = j.find("protocol");
    if (protocol == j.end() || !protocol->is_number_integer() || protocol->get<long long>() < 0) return false;
    // protocolMinor is optional (an older host omits it); when present it follows protocol's rule.
    if (const auto minor = j.find("protocolMinor"); minor != j.end()) {
        if (!minor->is_number_integer() || minor->get<long long>() < 0) return false;
    }
    const auto errors = j.find("errors");
    if (errors != j.end()) {
        if (!errors->is_array()) return false;
        for (const auto& e : *errors) {
            if (!e.is_object()) return false;
            const auto code = e.find("code");
            if (code == e.end() || !code->is_string()) return false;
        }
    }
    // ok:false says the operation failed, so it names the failure (s6.3); an
    // ok:false with no error is malformed (internal, exit 1), never usage.
    if (!ok->get<bool>() && (errors == j.end() || errors->empty())) return false;
    if (const auto host = j.find("host"); host != j.end() && !host->is_object()) return false;
    return true;
}
} // namespace

Envelope ParseEnvelope(const nlohmann::json& j)
{
    return ExtractEnvelope(j);
}

std::optional<Envelope> TryParseEnvelope(const nlohmann::json& j)
{
    if (!WellFormed(j)) return std::nullopt;
    return ExtractEnvelope(j);
}

Envelope MakeOk(std::string command, nlohmann::json data, HostBlock host)
{
    Envelope env;
    env.ok = true;
    env.command = std::move(command);
    env.data = std::move(data);
    env.host = std::move(host);
    env.errors.clear();
    return env;
}

Envelope MakeError(std::string command, std::string code, std::string message, std::string remediation)
{
    Envelope env;
    env.ok = false;
    env.command = std::move(command);
    env.data = nlohmann::json::object();
    Error e;
    e.code = std::move(code);
    e.message = std::move(message);
    e.remediation = std::move(remediation);
    env.errors.push_back(std::move(e));
    return env;
}

int ExitFromHost(const Envelope& env)
{
    if (!env.errors.empty()) return ExitForCode(env.errors.front().code);
    if (!env.ok) return 2;
    if (env.data.is_object()) {
        const auto verdict = env.data.find("verdict");
        if (verdict != env.data.end() && verdict->is_string() && (verdict->get<std::string>() == "Failed" || verdict->get<std::string>() == "Indeterminate")) return 8;
        const auto timedOut = env.data.find("timedOut");
        if (timedOut != env.data.end() && timedOut->is_boolean() && timedOut->get<bool>()) return 8;
    }
    return 0;
}

int WorstExit(std::span<const int> codes)
{
    int worst = 0;
    int worstRank = 0;
    for (const int c : codes) {
        const int r = Rank(c);
        if (r > worstRank) {
            worstRank = r;
            worst = c;
        }
    }
    return worst;
}

bool ExitCodeIsListed(std::string_view code)
{
    return ListedExit(code).has_value();
}

nlohmann::json ParseJsonBounded(std::string_view text, std::uint32_t maxDepth, bool* tooDeep)
{
    // The callback's depth is the number of containers already open around
    // the event (0 for the outermost value), and it keeps counting inside a
    // container it discarded, so returning false at a start event past the
    // limit stops every deeper level from being built. The flag then turns
    // the whole parse into a refusal: a partially kept document is never
    // handed on.
    bool deep = false;
    const nlohmann::json::parser_callback_t limit =
        [&deep, maxDepth](int depth, nlohmann::json::parse_event_t event, nlohmann::json& /*parsed*/) {
            const bool opens = event == nlohmann::json::parse_event_t::object_start ||
                               event == nlohmann::json::parse_event_t::array_start;
            if (opens && depth >= 0 && static_cast<std::uint32_t>(depth) >= maxDepth) {
                deep = true;
                return false;
            }
            return true;
        };
    nlohmann::json parsed = nlohmann::json::parse(text.begin(), text.end(), limit, /*allow_exceptions=*/false);
    if (tooDeep != nullptr)
        *tooDeep = deep;
    if (deep)
        return nlohmann::json(nlohmann::json::value_t::discarded);
    return parsed;
}

bool IsValidClientId(std::string_view id)
{
    if (id.empty() || id.size() > kMaxClientIdBytes)
        return false;
    for (const char c : id) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7E)
            return false;
    }
    return true;
}

} // namespace Arcane::Remote

#endif
