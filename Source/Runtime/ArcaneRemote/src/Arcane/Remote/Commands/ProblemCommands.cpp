#include <Arcane/Remote/Commands/ProblemCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Base/DiagnosticStore.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>
#include <Arcane/Scene/Components.hpp> // Identity
#include <Astra/Registry/Registry.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

// The wire args of problems.list, reflected for its schema (spec s5.3). The
// command reads its args JSON itself and refuses unknown keys.
struct ProblemsListArgs {
    std::string severity;   // error | warning | info; empty = all three
    std::string search;     // DiagnosticStore::Filtered's search text, passed unchanged
    bool follow = false;    // refused (arg.inapplicable): the follow path is GET /remote/v1/stream?topics=problems
};
ARC_REFLECT_TYPE(ProblemsListArgs)
    ARC_REFLECT_FIELD(ProblemsListArgs, severity)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(ProblemsListArgs, search)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(ProblemsListArgs, follow)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

namespace
{

constexpr std::string_view kUsage =
    "pass severity (error, warning or info) and search (text in the message, code or detail), both optional";

// The stored store (ruling F11). Main thread only: the command runs inside Drain.
DiagnosticStore*& StoredStore()
{
    static DiagnosticStore* store = nullptr;
    return store;
}

const char* SeverityName(DiagSeverity severity)
{
    switch (severity) {
        case DiagSeverity::Error: return "error";
        case DiagSeverity::Warning: return "warning";
        case DiagSeverity::Info: return "info";
    }
    return "info";
}

const char* ScopeName(DiagScope scope)
{
    switch (scope) {
        case DiagScope::Project: return "project";
        case DiagScope::Assets: return "assets";
        case DiagScope::Scene: return "scene";
        case DiagScope::Plugin: return "plugin";
        case DiagScope::Material: return "material";
        case DiagScope::Shader: return "shader";
    }
    return "project";
}

nlohmann::json GuidJson(const Guid& guid)
{
    return {{"hi", guid.hi}, {"lo", guid.lo}};
}

// The Identity guid of `handle` in the host's one world; nullopt when there is
// no single world, the handle is not alive there, or it carries no Identity.
std::optional<Guid> EntityGuid(std::uint64_t handle)
{
    const PickedWorld picked = ResolveWorld(CommandWorlds(), {});
    if (picked.pick != WorldPick::Ok || picked.world.registry == nullptr)
        return std::nullopt;
    const Astra::Registry& registry = *picked.world.registry;
    const Astra::Entity entity(static_cast<Astra::Entity::StorageType>(handle));
    if (!entity.IsValid() || !registry.IsValid(entity))
        return std::nullopt;
    const Identity* identity = registry.GetComponent<Identity>(entity);
    if (identity == nullptr)
        return std::nullopt;
    return identity->id;
}

nlohmann::json LocatorJson(const DiagLocator& locator, bool* unresolvedEntity)
{
    switch (locator.kind) {
        case DiagLocator::Kind::None:
            return nullptr;
        case DiagLocator::Kind::Entity: {
            const std::optional<Guid> guid = EntityGuid(locator.entity);
            if (!guid && unresolvedEntity != nullptr)
                *unresolvedEntity = true;
            return {{"kind", "entity"}, {"handle", locator.entity},
                    {"guid", guid ? GuidJson(*guid) : nlohmann::json(nullptr)}};
        }
        case DiagLocator::Kind::Asset:
            return {{"kind", "asset"}, {"guid", GuidJson(locator.asset)}};
        case DiagLocator::Kind::File:
            return {{"kind", "file"}, {"file", locator.file}, {"line", locator.line}, {"col", locator.col}};
        case DiagLocator::Kind::GraphNode:
            return {{"kind", "graph-node"}, {"owner", GuidJson(locator.ownerAsset)}, {"node", locator.nodeId}};
    }
    return nullptr;
}

CommandResult Refused(const Call& call, const char* code, std::string message, std::string remediation)
{
    CommandResult result;
    result.envelope = MakeError(call.command, code, std::move(message), std::move(remediation));
    return result;
}

CommandResult ProblemsList(const Call& call, void* /*user*/)
{
    if (!call.args.is_null() && !call.args.is_object())
        return Refused(call, "arg.invalid", call.command + " args must be a JSON object", std::string(kUsage));
    SeverityMask mask = SeverityMask::All;
    std::string search;
    if (call.args.is_object()) {
        for (const auto& [key, value] : call.args.items()) {
            (void)value;
            if (key != "severity" && key != "search" && key != "follow")
                return Refused(call, "arg.unknown", call.command + " has no argument '" + key + "'",
                               std::string(kUsage));
        }
        if (const auto it = call.args.find("follow"); it != call.args.end()) {
            if (!it->is_boolean())
                return Refused(call, "arg.invalid", "follow must be true or false", std::string(kUsage));
            // Ruling P2 (f): one follow path, the stream; a follow call arms nothing.
            if (it->get<bool>())
                return Refused(call, "arg.inapplicable", "problems.list does not follow",
                               "use servitor problems --follow, which opens GET /remote/v1/stream?topics=problems");
        }
        if (const auto it = call.args.find("severity"); it != call.args.end()) {
            const std::string name = it->is_string() ? it->get<std::string>() : std::string{};
            if (name == "error")
                mask = SeverityMask::Error;
            else if (name == "warning")
                mask = SeverityMask::Warning;
            else if (name == "info")
                mask = SeverityMask::Info;
            else
                return Refused(call, "arg.invalid", "severity must be error, warning or info", std::string(kUsage));
        }
        if (const auto it = call.args.find("search"); it != call.args.end()) {
            if (!it->is_string())
                return Refused(call, "arg.invalid", "search must be a string", std::string(kUsage));
            search = it->get<std::string>();
        }
    }
    const DiagnosticStore* store = StoredStore();
    if (store == nullptr)
        return Refused(call, "host.unavailable", "no armed host session serves " + call.command,
                       "relaunch the host with --remote");

    nlohmann::json rows = nlohmann::json::array();
    nlohmann::json unresolved = nlohmann::json::array();
    for (const Diagnostic& d : store->Filtered(mask, search)) {
        bool missing = false;
        rows.push_back(ProblemRowJson(d, &missing));
        if (missing)
            unresolved.push_back(d.locator.entity);
    }
    CommandResult result;
    result.envelope = MakeOk(call.command, {{"problems", std::move(rows)}}, {});
    if (!unresolved.empty()) {
        Error warning;
        warning.code = "entity.no-identity";
        warning.message = std::to_string(unresolved.size()) +
                          " problem row(s) name an entity that is gone or has no Identity; "
                          "their locator carries the packed handle and guid null";
        warning.remediation = "the handle is only meaningful in this host process, for this session";
        warning.detail = {{"handles", std::move(unresolved)}};
        result.envelope.warnings.push_back(std::move(warning));
    }
    return result;
}

} // namespace

nlohmann::json ProblemRowJson(const Diagnostic& diagnostic, bool* unresolvedEntity)
{
    return {
        {"severity", SeverityName(diagnostic.severity)},
        {"scope", ScopeName(diagnostic.scope)},
        {"code", diagnostic.code},
        {"message", diagnostic.message},
        {"detail", diagnostic.detail},
        {"locator", LocatorJson(diagnostic.locator, unresolvedEntity)},
    };
}

void SetProblemStore(DiagnosticStore* store)
{
    StoredStore() = store;
}

void ReleaseProblemStore(const DiagnosticStore* store)
{
    if (store != nullptr && StoredStore() == store)
        StoredStore() = nullptr;
}

DiagnosticStore* ProblemStore()
{
    return StoredStore();
}

void RegisterProblemCommand()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    Desc d;
    d.name = "problems.list";
    d.summary = "List the host's diagnostics (the Problems store), filtered by severity and search text";
    d.access = Access::Observe;
    d.mutates = Mutates::None;
    d.play = PlayPolicy::Any;
    d.thread = ExecThread::Main;
    d.needsFrame = false;
    d.argsMeta = Astra::GetMeta<ProblemsListArgs>();
    d.fn = &ProblemsList;
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(d), &error))
        ARC_ERROR("remote: problems command refused: {}", error);
}

} // namespace Arcane::Remote

#endif
