#include <Arcane/Remote/Commands/ConsoleCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/RemoteCVarService.hpp>
#include <Arcane/Reflection.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Arcane::Remote
{

// The wire args of console.exec, reflected for its schema (spec s5.3). The
// command reads its args JSON itself and refuses unknown keys.
struct ConsoleExecArgs {
    std::string line;   // one console line: "<command> [args]", "<cvar>" or "<cvar> <value>"
};
ARC_REFLECT_TYPE(ConsoleExecArgs)
    ARC_REFLECT_FIELD(ConsoleExecArgs, line)
ARC_REFLECT_TYPE_END()

namespace
{

constexpr std::string_view kUsage = "pass line: one console line, such as cvarlist or render.vsync false";
constexpr std::string_view kExplain = "cvar_explain";   // the history command the service refuses remotely
constexpr std::string_view kMasked = "<protected>";      // RemoteCVarService's mask, never a value

// The stored user (ruling F11). Main thread only: the command runs inside Drain.
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

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

// A C0 control character other than tab, or DEL. The registry splits a line on
// ' ' and the service trims '\r' / '\n', so a line carrying one would resolve
// to a different name on each side; such a line is refused before anything.
bool HasControlCharacter(std::string_view s)
{
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20 && c != '\t') || u == 0x7F)
            return true;
    }
    return false;
}

// The listed (live, not Hidden) console command named `name`, with its flags;
// nullopt when the name is not one (a cvar line, or unknown). The service
// resolves names through the same list.
std::optional<CVarFlags> ListedCommandFlags(const CVarRegistry& registry, std::string_view name)
{
    for (const CVarListEntry& c : registry.ListCommands()) {
        if (c.name == name)
            return c.flags;
    }
    return std::nullopt;
}

// A refused cvar line, mapped by Task 20's table (CvarRefusalCode).
Envelope CvarLineRefusal(const std::string& command, const std::string& name, const std::string& text)
{
    const std::string_view code = CvarRefusalCode(text);
    if (code == "cvar.unknown")
        return MakeError(command, "cvar.unknown",
                         "'" + name + "' is not a console command or cvar this host serves remotely",
                         "servitor cvar list shows the cvars; a console command runs remotely only with ServerCanExecute");
    if (code == "cvar.protected")
        return MakeError(command, "cvar.protected", text, "a Protected cvar is never readable remotely");
    if (code == "cvar.type-mismatch")
        return MakeError(command, "cvar.type-mismatch", text,
                         "pass a value of the cvar's type (servitor cvar explain " + name + " shows it)");
    if (code == "permission.denied")
        return MakeError(command, "permission.denied", text, "this host's context or policy refuses it");
    if (code == "cvar.refused-weaker")
        return MakeError(command, "cvar.refused-weaker", text,
                         "servitor cvar explain " + name + " shows the stronger source that holds it");
    if (code == "arg.missing")
        return MakeError(command, "arg.missing", text, std::string(kUsage));
    return MakeError(command, "cvar.refused", text, "servitor cvar explain " + name + " shows its state");
}

CommandResult ConsoleExec(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (!call.args.is_null() && !call.args.is_object()) {
        result.envelope = MakeError(call.command, "arg.invalid", call.command + " args must be a JSON object",
                                    std::string(kUsage));
        return result;
    }
    if (call.args.is_object()) {
        for (const auto& [key, value] : call.args.items()) {
            (void)value;
            if (key != "line") {
                result.envelope = MakeError(call.command, "arg.unknown",
                                            call.command + " has no argument '" + key + "'", std::string(kUsage));
                return result;
            }
        }
    }
    if (!call.args.is_object() || !call.args.contains("line")) {
        result.envelope = MakeError(call.command, "arg.missing", call.command + " needs line", std::string(kUsage));
        return result;
    }
    const nlohmann::json& lineJson = call.args.at("line");
    if (!lineJson.is_string()) {
        result.envelope = MakeError(call.command, "arg.invalid", "line must be a non-empty string", std::string(kUsage));
        return result;
    }
    const std::string& lineText = lineJson.get_ref<const std::string&>();
    if (HasControlCharacter(lineText)) {
        result.envelope = MakeError(call.command, "arg.invalid",
                                    "line must not contain control characters (only spaces and tabs separate words)",
                                    std::string(kUsage));
        return result;
    }
    const std::string_view line = Trim(lineText);
    if (line.empty()) {
        result.envelope = MakeError(call.command, "arg.invalid", "line must be a non-empty string", std::string(kUsage));
        return result;
    }
    const CvarCommandUser* user = StoredUser();
    if (user == nullptr || user->registry == nullptr) {
        result.envelope = MakeError(call.command, "host.unavailable", "no armed host session serves " + call.command,
                                    "relaunch the host with --remote");
        return result;
    }

    const std::size_t gap = line.find_first_of(" \t");
    const std::string name(line.substr(0, gap));
    const std::string rest(gap == std::string_view::npos ? std::string_view{} : Trim(line.substr(gap + 1)));
    const char* const context = ContextName(user->context);
    // The service refuses cvar_explain (its history would bypass the Protected
    // mask); refuse it here before anything resolves or runs.
    if (name == kExplain) {
        result.envelope = MakeError(call.command, "command.unavailable",
                                    "cvar_explain is not remotely executable",
                                    "use servitor cvar explain <name> (its Protected rule masks history)");
        return result;
    }

    // The permission decision for a console command is made from its flags,
    // before anything runs: ServerCanExecute (RemoteCVarService::RunCommand's
    // rule and sentence), then Execute's Cheat gate (outside the Editor a Cheat
    // command needs server.cheats; CVarRegistry::Execute's predicate). What
    // reaches the service after these is allowed, so a failed reply is the
    // command's own failure: no reply text is matched.
    const std::optional<CVarFlags> commandFlags = ListedCommandFlags(*user->registry, name);
    const nlohmann::json echoed = {{"line", std::string(line)}, {"context", context}};
    if (commandFlags && !HasFlag(*commandFlags, CVarFlags::ServerCanExecute)) {
        result.envelope = MakeError(call.command, "permission.denied",
                                    "command '" + name + "' is not remotely executable (no ServerCanExecute)",
                                    "only console commands registered with ServerCanExecute run remotely");
        result.envelope.data = echoed;
        return result;
    }
    if (commandFlags && HasFlag(*commandFlags, CVarFlags::Cheat) && user->context != CVarContext::Editor &&
        !user->registry->CheatsEnabled()) {
        result.envelope = MakeError(call.command, "permission.denied", "denied: '" + name + "' needs server.cheats",
                                    "a Cheat command runs outside the editor only while server.cheats is on");
        result.envelope.data = echoed;
        return result;
    }

    // A set of a Protected cvar echoes "<name> <protected>", never the value
    // (cvar.set masks it the same way); the service's own text is already masked.
    std::string shownLine(line);
    if (!commandFlags && !rest.empty()) {
        const auto explained = user->registry->Explain(name);
        if (explained && HasFlag(explained->flags, CVarFlags::Protected))
            shownLine = name + " " + std::string(kMasked);
    }

    // One RemoteCVarService::Handle in the user's context (ruling E-1): a
    // command is the service's command path (op set: Execute(line, context,
    // SetBy::Console, &caller)); a cvar alone is its get and a cvar with a
    // value its set, so the Protected mask and Hidden-as-unknown rules are the
    // remote cvar service's own.
    RemoteCVarService service(*user->registry);
    const RemoteCVarResponse res = service.Handle(
        {commandFlags || !rest.empty() ? "set" : "get", name, rest,
         call.callerId.empty() ? std::string("remote") : call.callerId, user->context});
    const nlohmann::json data = {{"line", shownLine}, {"text", res.text}, {"context", context}};

    if (!res.ok) {
        // A command that got this far was allowed: it ran and reported failure.
        result.envelope = commandFlags ? MakeError(call.command, "cvar.refused", res.text,
                                                   "the console command '" + name + "' ran and reported failure")
                                       : CvarLineRefusal(call.command, name, res.text);
        result.envelope.data = data;
        return result;
    }
    result.envelope = MakeOk(call.command, data, {});
    UndoBlock undo;
    undo.label = nullptr;
    undo.undoable = false;
    undo.reason = "not-undoable-command";
    result.envelope.undo = std::move(undo);
    return result;
}

} // namespace

void SetConsoleCommandUser(CvarCommandUser* user)
{
    StoredUser() = user;
}

void ReleaseConsoleCommandUser(const CvarCommandUser* user)
{
    if (user != nullptr && StoredUser() == user)
        StoredUser() = nullptr;
}

void RegisterConsoleCommand()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    Desc d;
    d.name = "console.exec";
    d.summary = "Run one console line: a ServerCanExecute command, or a cvar read or set, and return its text";
    d.access = Access::Drive;
    // Preferences, as cvar.set: a console line may set a cvar the user owns,
    // and None is refused for Drive.
    d.mutates = Mutates::Preferences;
    d.play = PlayPolicy::Any;
    d.thread = ExecThread::Main;
    d.needsFrame = false;
    d.argsMeta = Astra::GetMeta<ConsoleExecArgs>();
    d.fn = &ConsoleExec;
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(d), &error))
        ARC_ERROR("remote: console command refused: {}", error);
}

} // namespace Arcane::Remote

#endif
