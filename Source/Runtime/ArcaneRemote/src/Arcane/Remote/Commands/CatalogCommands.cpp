#include <Arcane/Remote/Commands/CatalogCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/EcsCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Schema.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <Astra/Reflection/MetaRegistry.hpp>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

// The wire args of the two commands, reflected for their schema (spec s5.3).
// Each command reads its args JSON itself and refuses unknown keys.
struct CatalogDescribeArgs {
    std::string name;   // a registered command's dotted name
};
ARC_REFLECT_TYPE(CatalogDescribeArgs)
    ARC_REFLECT_FIELD(CatalogDescribeArgs, name)
ARC_REFLECT_TYPE_END()

struct SchemaTypeArgs {
    std::string kind;   // component | resource
    std::string name;   // the reflected type's name
};
ARC_REFLECT_TYPE(SchemaTypeArgs)
    ARC_REFLECT_FIELD(SchemaTypeArgs, kind)
    ARC_REFLECT_FIELD(SchemaTypeArgs, name)
ARC_REFLECT_TYPE_END()

namespace
{

constexpr std::string_view kDescribeUsage = "pass name: a command from servitor list";
constexpr std::string_view kSchemaUsage = "pass kind: component or resource, and name: the reflected type name";

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

// A required, non-empty string argument.
std::optional<Envelope> ReadString(const Call& call, const char* key, std::string_view usage, std::string& out)
{
    if (!call.args.is_object() || !call.args.contains(key))
        return MakeError(call.command, "arg.missing", call.command + " needs " + key, std::string(usage));
    const nlohmann::json& v = call.args.at(key);
    if (!v.is_string() || v.get<std::string>().empty())
        return MakeError(call.command, "arg.invalid", std::string(key) + " must be a non-empty string",
                         std::string(usage));
    out = v.get<std::string>();
    return std::nullopt;
}

CommandResult CatalogDescribe(const Call& call, void* /*user*/)
{
    CommandResult result;
    std::string name;
    std::optional<Envelope> refusal = CheckKeys(call, {"name"}, kDescribeUsage);
    if (!refusal)
        refusal = ReadString(call, "name", kDescribeUsage, name);
    if (refusal) {
        result.envelope = std::move(*refusal);
        return result;
    }

    // The catalogue row is the one spelling of the declaration (the
    // lower-case enumerator names GET /remote/v1/catalog serves).
    const CommandRegistry& registry = CommandRegistry::Get();
    const nlohmann::json catalogue = registry.Catalogue();
    for (const nlohmann::json& row : catalogue.at("commands")) {
        if (row.value("name", std::string{}) != name)
            continue;
        nlohmann::json data = row;
        data["schema"] = registry.SchemaOf(name);
        result.envelope = MakeOk(call.command, std::move(data), {});
        return result;
    }
    result.envelope = MakeError(call.command, "command.unknown", "no command named '" + name + "' on this host",
                                "servitor list");
    // The client's command.unknown remediation names this command, not catalog.describe.
    result.envelope.errors.front().detail["name"] = name;
    return result;
}

CommandResult SchemaType(const Call& call, void* /*user*/)
{
    CommandResult result;
    std::string kind;
    std::string name;
    std::optional<Envelope> refusal = CheckKeys(call, {"kind", "name"}, kSchemaUsage);
    if (!refusal)
        refusal = ReadString(call, "kind", kSchemaUsage, kind);
    if (!refusal && kind != "component" && kind != "resource")
        refusal = MakeError(call.command, "arg.invalid", "kind must be component or resource, got '" + kind + "'",
                            std::string(kSchemaUsage));
    if (!refusal)
        refusal = ReadString(call, "name", kSchemaUsage, name);
    if (refusal) {
        result.envelope = std::move(*refusal);
        return result;
    }

    const Astra::TypeMeta* meta = Astra::GetMetaByName(name);
    if (meta == nullptr && kind == "component") {
        // A short component name (spec s7.2, Task 23): the one resolver
        // entity.get and ecs.query use, over the reflected types linked to a
        // ComponentID (a registry in this process registered them as
        // components). A same-named plain struct (an args type, a settings
        // block) is not a candidate, so it cannot make a component ambiguous.
        std::vector<std::string> names;
        Astra::MetaRegistry& metas = Astra::MetaRegistry::Instance();
        metas.ForEachType([&](const Astra::TypeMeta& t) {
            if (metas.GetComponentId(t.typeHash) != Astra::INVALID_COMPONENT)
                names.emplace_back(t.typeName);
        });
        NameResolution res = PickComponent(names, name);
        if (res.pick == NamePick::One) {
            meta = Astra::GetMetaByName(res.qualified);
        } else if (res.pick == NamePick::Many) {
            result.envelope = MakeError(call.command, "component.ambiguous",
                                        "component name '" + name + "' matches several types",
                                        "pass the qualified name: one of the candidates");
            result.envelope.errors.front().candidates = std::move(res.candidates);
            result.envelope.errors.front().detail["name"] = name;
            return result;
        }
    }
    if (meta == nullptr) {
        result.envelope = kind == "component"
            ? MakeError(call.command, "component.unknown", "no reflected component named '" + name + "'",
                        "check the name against servitor ecs stats or the scene file")
            : MakeError(call.command, "resource.not-found", "no reflected resource named '" + name + "'",
                        "check the name against servitor resource list");
        result.envelope.errors.front().detail["name"] = name;
        return result;
    }
    result.envelope = MakeOk(call.command, SchemaFor(*meta), {});
    return result;
}

void RegisterOne(Desc desc)
{
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(desc), &error))
        ARC_ERROR("remote: catalogue command refused: {}", error);
}

} // namespace

void RegisterCatalogCommands()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    Desc describe;
    describe.name = "catalog.describe";
    describe.summary = "Describe one registered command: its declaration and its args schema";
    describe.access = Access::Observe;
    describe.mutates = Mutates::None;
    describe.play = PlayPolicy::Any;
    describe.thread = ExecThread::Main;
    describe.needsFrame = false;
    describe.argsMeta = Astra::GetMeta<CatalogDescribeArgs>();
    describe.fn = &CatalogDescribe;
    RegisterOne(std::move(describe));

    Desc schema;
    schema.name = "schema.type";
    schema.summary = "The JSON Schema of a reflected component or resource type, by name";
    schema.access = Access::Observe;
    schema.mutates = Mutates::None;
    schema.play = PlayPolicy::Any;
    schema.thread = ExecThread::Main;
    schema.needsFrame = false;
    schema.argsMeta = Astra::GetMeta<SchemaTypeArgs>();
    schema.fn = &SchemaType;
    RegisterOne(std::move(schema));
}

} // namespace Arcane::Remote

#endif
