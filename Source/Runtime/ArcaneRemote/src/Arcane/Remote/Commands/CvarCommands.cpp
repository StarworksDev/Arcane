#include <Arcane/Remote/Commands/CvarCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/RemoteCVarService.hpp>
#include <Arcane/Reflection.hpp>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Arcane::Remote
{

// The wire args of the four commands, reflected for their schemas (spec s5.3).
// Each command reads its args JSON itself and refuses unknown keys.
struct CvarGetArgs {
    std::string name;
};
ARC_REFLECT_TYPE(CvarGetArgs)
    ARC_REFLECT_FIELD(CvarGetArgs, name)
ARC_REFLECT_TYPE_END()

struct CvarSetArgs {
    std::string name;
    std::string value;   // console text; a JSON bool, number or number array is accepted too
};
ARC_REFLECT_TYPE(CvarSetArgs)
    ARC_REFLECT_FIELD(CvarSetArgs, name)
    ARC_REFLECT_FIELD(CvarSetArgs, value)
ARC_REFLECT_TYPE_END()

struct CvarListArgs {
    std::string prefix;   // a dotted prefix ("render" matches render.*); empty lists every visible cvar
};
ARC_REFLECT_TYPE(CvarListArgs)
    ARC_REFLECT_FIELD(CvarListArgs, prefix)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

struct CvarExplainArgs {
    std::string name;
};
ARC_REFLECT_TYPE(CvarExplainArgs)
    ARC_REFLECT_FIELD(CvarExplainArgs, name)
ARC_REFLECT_TYPE_END()

namespace
{

constexpr const char* kMasked = "<protected>";        // RemoteCVarService's mask, never a value
constexpr std::string_view kLineSeparator = " = ";   // RemoteCVarService list lines: "name = value"
constexpr std::string_view kNameUsage = "pass name: the cvar (servitor cvar list shows them)";
constexpr std::string_view kSetUsage = "pass name and value: the cvar and its new value as console text";
constexpr std::string_view kListUsage = "pass prefix (optional): a dotted cvar prefix such as render";

// The stored user (ruling F11). Main thread only: every command runs inside Drain.
CvarCommandUser*& StoredUser()
{
    static CvarCommandUser* user = nullptr;
    return user;
}

const char* ContextName(CVarContext context)
{
    switch (context) {
        case CVarContext::Editor: return "Editor";
        case CVarContext::LocalHost: return "LocalHost";
        case CVarContext::ServerAdmin: return "ServerAdmin";
        case CVarContext::Client: return "Client";
    }
    return "?";
}

// The service's JSON text fields (valueJson, publishedJson, ...) as JSON; a
// field that is empty or does not parse is JSON null.
nlohmann::json ParseField(const std::string& text)
{
    if (text.empty())
        return nullptr;
    nlohmann::json parsed = ParseJsonBounded(text, kMaxRequestDepth);   // a String cvar's text is caller-supplied (Task 30)
    return parsed.is_discarded() ? nlohmann::json(nullptr) : parsed;
}

bool IsProtectedRefusal(const std::string& text)
{
    return std::string_view(CvarRefusalCode(text)) == "cvar.protected";
}

// A refused Handle response mapped to its error envelope (CvarRefusalCode).
Envelope Refusal(const std::string& command, const std::string& name, const std::string& text)
{
    const std::string_view code = CvarRefusalCode(text);
    if (code == "cvar.protected") {
        Envelope env = MakeError(command, "cvar.protected", text, "a Protected cvar is never readable remotely");
        env.data = {{"name", name}, {"value", nullptr}, {"masked", kMasked}};
        return env;
    }
    if (code == "cvar.unknown" && (text.starts_with("command '") || text.find("is a command") != std::string::npos))
        return MakeError(command, "cvar.unknown", "'" + name + "' is a console command, not a cvar",
                         "run console commands with servitor console");
    if (code == "cvar.unknown")
        return MakeError(command, "cvar.unknown", text, "servitor cvar list shows the cvars this host knows");
    if (code == "cvar.type-mismatch")
        return MakeError(command, "cvar.type-mismatch", text,
                         "pass a value of the cvar's type (servitor cvar explain " + name + " shows it)");
    if (code == "permission.denied")
        return MakeError(command, "permission.denied", text, "this host's context or policy refuses it");
    if (code == "arg.missing")
        return MakeError(command, "arg.missing", text,
                         std::string(command == "cvar.set" ? kSetUsage : kNameUsage));
    if (code == "cvar.refused-weaker")
        return MakeError(command, "cvar.refused-weaker", text,
                         "servitor cvar explain " + name + " shows the stronger source that holds it");
    return MakeError(command, "cvar.refused", text, "servitor cvar explain " + name + " shows its state");
}

Envelope Unavailable(const std::string& command)
{
    return MakeError(command, "host.unavailable", "no armed host session serves " + command,
                     "relaunch the host with --remote");
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

// args.name as a non-empty string, or the usage refusal.
std::optional<Envelope> ReadName(const Call& call, std::string& out, std::string_view usage)
{
    if (!call.args.is_object() || !call.args.contains("name"))
        return MakeError(call.command, "arg.missing", call.command + " needs name", std::string(usage));
    const nlohmann::json& v = call.args.at("name");
    if (!v.is_string() || v.get<std::string>().empty())
        return MakeError(call.command, "arg.invalid", "name must be a non-empty string", std::string(usage));
    out = v.get<std::string>();
    return std::nullopt;
}

// args.value as console text: a string verbatim; a bool, number or number
// array in the form ParseCVarText reads ("true", "0.5", "[1,2,3]").
std::optional<Envelope> ReadValue(const Call& call, std::string& out)
{
    if (!call.args.is_object() || !call.args.contains("value"))
        return MakeError(call.command, "arg.missing", call.command + " needs value", std::string(kSetUsage));
    const nlohmann::json& v = call.args.at("value");
    if (v.is_string()) {
        out = v.get<std::string>();
        return std::nullopt;
    }
    bool numbers = v.is_array() && !v.empty();
    if (numbers) {
        for (const nlohmann::json& e : v)
            numbers = numbers && e.is_number();
    }
    if (v.is_boolean() || v.is_number() || numbers) {
        out = v.dump();
        return std::nullopt;
    }
    return MakeError(call.command, "arg.invalid", "value must be console text, a bool, a number or a number array",
                     std::string(kSetUsage));
}

std::string CallerOf(const Call& call)
{
    return call.callerId.empty() ? std::string("remote") : call.callerId;
}

// One RemoteCVarService::Handle on the user's registry in the user's context.
RemoteCVarResponse Ask(const CvarCommandUser& user, std::string op, std::string name, std::string value,
                       const Call& call)
{
    RemoteCVarService service(*user.registry);
    return service.Handle({std::move(op), std::move(name), std::move(value), CallerOf(call), user.context});
}

// The stored user when it can serve, else null (host.unavailable).
const CvarCommandUser* Serving()
{
    const CvarCommandUser* user = StoredUser();
    return user != nullptr && user->registry != nullptr ? user : nullptr;
}

CommandResult CvarGet(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (auto bad = CheckKeys(call, {"name"}, kNameUsage)) {
        result.envelope = std::move(*bad);
        return result;
    }
    std::string name;
    if (auto bad = ReadName(call, name, kNameUsage)) {
        result.envelope = std::move(*bad);
        return result;
    }
    const CvarCommandUser* user = Serving();
    if (user == nullptr) {
        result.envelope = Unavailable(call.command);
        return result;
    }
    const RemoteCVarResponse res = Ask(*user, "get", name, {}, call);
    if (!res.ok) {
        result.envelope = Refusal(call.command, name, res.text);
        return result;
    }
    result.envelope = MakeOk(call.command,
                             {{"name", name},
                              {"type", res.type},
                              {"value", ParseField(res.valueJson)},
                              {"context", ContextName(user->context)}},
                             {});
    return result;
}

CommandResult CvarExplain(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (auto bad = CheckKeys(call, {"name"}, kNameUsage)) {
        result.envelope = std::move(*bad);
        return result;
    }
    std::string name;
    if (auto bad = ReadName(call, name, kNameUsage)) {
        result.envelope = std::move(*bad);
        return result;
    }
    const CvarCommandUser* user = Serving();
    if (user == nullptr) {
        result.envelope = Unavailable(call.command);
        return result;
    }
    const RemoteCVarResponse res = Ask(*user, "explain", name, {}, call);
    if (!res.ok) {
        result.envelope = Refusal(call.command, name, res.text);
        return result;
    }
    result.envelope = MakeOk(call.command,
                             {{"name", name},
                              {"type", res.type},
                              {"value", ParseField(res.valueJson)},
                              {"setBy", res.setBy},
                              {"audience", res.audience},
                              {"scope", res.scope},
                              {"apply", res.apply},
                              {"flags", res.flags},
                              {"published", ParseField(res.publishedJson)},
                              {"pending", ParseField(res.pendingJson)},
                              {"history", ParseField(res.historyJson)},
                              {"context", ContextName(user->context)}},
                             {});
    return result;
}

CommandResult CvarList(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (auto bad = CheckKeys(call, {"prefix"}, kListUsage)) {
        result.envelope = std::move(*bad);
        return result;
    }
    std::string prefix;
    if (call.args.is_object() && call.args.contains("prefix")) {
        const nlohmann::json& v = call.args.at("prefix");
        if (!v.is_string()) {
            result.envelope = MakeError(call.command, "arg.invalid", "prefix must be a string",
                                        std::string(kListUsage));
            return result;
        }
        prefix = v.get<std::string>();
    }
    const CvarCommandUser* user = Serving();
    if (user == nullptr) {
        result.envelope = Unavailable(call.command);
        return result;
    }
    const RemoteCVarResponse res = Ask(*user, "list", prefix, {}, call);
    if (!res.ok) {
        result.envelope = Refusal(call.command, prefix, res.text);
        return result;
    }
    // The service's text is one "name = value" line per visible cvar, sorted by
    // name, or a "(no cvars ...)" line when none matches. A Protected cvar's
    // line carries the mask, never its value.
    nlohmann::json rows = nlohmann::json::array();
    std::string_view text = res.text;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        const std::size_t eq = line.find(kLineSeparator);
        if (eq == std::string_view::npos)
            continue;   // "(no cvars)" / "(no cvars match 'x')"
        const std::string name(line.substr(0, eq));
        const std::string_view value = line.substr(eq + kLineSeparator.size());
        const auto explained = user->registry->Explain(name);
        if (value == std::string_view(kMasked) && explained && HasFlag(explained->flags, CVarFlags::Protected))
            rows.push_back({{"name", name}, {"text", nullptr}, {"masked", kMasked}});
        else
            rows.push_back({{"name", name}, {"text", std::string(value)}});
    }
    result.envelope = MakeOk(call.command,
                             {{"prefix", prefix}, {"context", ContextName(user->context)}, {"cvars", std::move(rows)}},
                             {});
    return result;
}

// Ruling F18: three Handle calls in the same context -- the leading get is
// data.previous, the set writes, the trailing explain fills the rest.
CommandResult CvarSet(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (auto bad = CheckKeys(call, {"name", "value"}, kSetUsage)) {
        result.envelope = std::move(*bad);
        return result;
    }
    std::string name;
    if (auto bad = ReadName(call, name, kSetUsage)) {
        result.envelope = std::move(*bad);
        return result;
    }
    std::string value;
    if (auto bad = ReadValue(call, value)) {
        result.envelope = std::move(*bad);
        return result;
    }
    const CvarCommandUser* user = Serving();
    if (user == nullptr) {
        result.envelope = Unavailable(call.command);
        return result;
    }

    // (1) previous. A Protected cvar's get is refused: previous stays null and
    // the set proceeds (its audit is masked by the service). A name that is not
    // a cvar here stops before anything is written.
    nlohmann::json previous = nullptr;
    const RemoteCVarResponse before = Ask(*user, "get", name, {}, call);
    if (before.ok) {
        previous = ParseField(before.valueJson);
    } else {
        Envelope refused = Refusal(call.command, name, before.text);
        if (refused.errors.front().code == "cvar.unknown") {
            result.envelope = std::move(refused);
            return result;
        }
    }

    // (2) the write.
    const RemoteCVarResponse wrote = Ask(*user, "set", name, value, call);
    if (!wrote.ok) {
        result.envelope = Refusal(call.command, name, wrote.text);
        return result;
    }

    // (3) what is in effect now.
    nlohmann::json data = {{"name", name}, {"previous", std::move(previous)}};
    const RemoteCVarResponse after = Ask(*user, "explain", name, {}, call);
    if (after.ok) {
        data["type"] = after.type;
        data["value"] = ParseField(after.valueJson);
        data["setBy"] = after.setBy;
        data["audience"] = after.audience;
        data["apply"] = after.apply;
    } else {
        data["value"] = nullptr;
        if (IsProtectedRefusal(after.text))
            data["masked"] = kMasked;
    }
    data["context"] = ContextName(user->context);
    result.envelope = MakeOk(call.command, std::move(data), {});
    UndoBlock undo;
    undo.label = nullptr;
    undo.undoable = false;
    undo.reason = "not-undoable-command";
    result.envelope.undo = std::move(undo);
    return result;
}

Desc MakeDesc(const char* name, const char* summary, Access access, Mutates mutates, const Astra::TypeMeta* meta,
              CommandFn fn)
{
    Desc d;
    d.name = name;
    d.summary = summary;
    d.access = access;
    d.mutates = mutates;
    d.play = PlayPolicy::Any;
    d.thread = ExecThread::Main;
    d.needsFrame = false;
    d.argsMeta = meta;
    d.fn = fn;
    return d;
}

} // namespace

const char* CvarRefusalCode(std::string_view text)
{
    // "<name> is protected: never readable remotely" (the service) and
    // "denied: '<name>' is protected" (the registry). Names have no spaces.
    if (text.find(" is protected") != std::string_view::npos)
        return "cvar.protected";
    if (text.starts_with("unknown"))   // "unknown cvar 'x'", "unknown 'x'" (absent in this context)
        return "cvar.unknown";
    if (text.starts_with("command '") || text.find("is a command") != std::string_view::npos)
        return "cvar.unknown";         // a console command is not a cvar
    if (text.starts_with("expected ") || text == "type has no accessor")   // ParseCVarText's reasons
        return "cvar.type-mismatch";
    if (text.starts_with("denied"))   // the access table, a rung refusal, or the game's policy
        return "permission.denied";
    if (text.ends_with("missing value") || text.ends_with("missing name"))
        return "arg.missing";
    if (text.starts_with("refused: a stronger source holds"))   // SetResult::RefusedWeaker (spec s6.4)
        return "cvar.refused-weaker";
    return "cvar.refused";             // "rejected" (SetResult::Stale / TypeMismatch) and anything unrecognised
}

void SetCvarCommandUser(CvarCommandUser* user)
{
    StoredUser() = user;
}

void ReleaseCvarCommandUser(const CvarCommandUser* user)
{
    if (user != nullptr && StoredUser() == user)
        StoredUser() = nullptr;
}

void RegisterCvarCommands()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    Desc descs[] = {
        MakeDesc("cvar.get", "Read one cvar's published value", Access::Observe, Mutates::None,
                 Astra::GetMeta<CvarGetArgs>(), &CvarGet),
        // Preferences, though a remote set is the Console rung, which is never
        // archived (WriteCVarArchive writes User/EditorUser only): no Mutates
        // value means "runtime state", None is refused for Drive, and
        // Preferences is the nearest -- it changes a setting the user owns.
        MakeDesc("cvar.set", "Set one cvar from console text and publish it", Access::Drive, Mutates::Preferences,
                 Astra::GetMeta<CvarSetArgs>(), &CvarSet),
        MakeDesc("cvar.list", "List the cvars visible to this host's context, optionally under a dotted prefix",
                 Access::Observe, Mutates::None, Astra::GetMeta<CvarListArgs>(), &CvarList),
        MakeDesc("cvar.explain", "Explain one cvar: type, audience, scope, apply mode and who set it", Access::Observe,
                 Mutates::None, Astra::GetMeta<CvarExplainArgs>(), &CvarExplain),
    };
    for (Desc& d : descs) {
        std::string error;
        if (!CommandRegistry::Get().Register(std::move(d), &error))
            ARC_ERROR("remote: cvar command refused: {}", error);
    }
}

} // namespace Arcane::Remote

#endif
