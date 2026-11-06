#include <Arcane/Remote/Commands/ResourceCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/CommandSupport.hpp>
#include <Arcane/Remote/Commands/EcsCommands.hpp> // ShortName, PickComponent: the shared name resolution
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Reflection.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

// The wire args, reflected for their schema (spec s5.3). Each command reads its
// args JSON itself and refuses unknown keys.
struct ResourceGetArgs {
    std::string name;   // the registered name ("Arcane::Time") or a unique short name ("Time")
};
ARC_REFLECT_TYPE(ResourceGetArgs)
    ARC_REFLECT_FIELD(ResourceGetArgs, name)
ARC_REFLECT_TYPE_END()

// resource.list takes no arguments.
struct ResourceListArgs {
};
ARC_REFLECT_TYPE(ResourceListArgs)
ARC_REFLECT_TYPE_END()

namespace
{

using Detail::CheckKeys;
using Detail::ErrorEnvelope;
using Detail::kInlineCapBytes;
using Detail::MakeIssue;
using Detail::PickRegistry;

constexpr std::string_view kListUsage = "takes no arguments; --world <name> picks one of the host's worlds";
constexpr std::string_view kGetUsage = "pass name: a resource name from servitor resource list";
constexpr std::string_view kResourceHint = "servitor resource list names this world's resources";

// The name a resource answers to: its registered type name, else its reflected one.
std::string ResourceName(const Astra::Registry::ResourceInfo& info)
{
    if (info.descriptor != nullptr && info.descriptor->name != nullptr && info.descriptor->name[0] != '\0')
        return info.descriptor->name;
    if (info.meta != nullptr)
        return std::string(info.meta->typeName);
    return {};
}

// The resource as scene-format JSON: Detail::WriteMaskedFields over the
// reflected fields when the type is reflected (a field that may hold a
// Protected value is masked whole), else the descriptor's visitFields. False
// with *why when the resource cannot be read.
bool WriteResource(const Astra::Registry::ResourceInfo& info, nlohmann::json& out, std::string& why)
{
    out = nlohmann::json::object();
    void* data = info.data;   // read only: the writer never writes through it
    if (data == nullptr) {
        why = "the resource has no readable instance";
        return false;
    }
    if (info.meta != nullptr)
        return Detail::WriteMaskedFields(*info.meta, data, out, why);
    ReflectionJsonWriter writer(out);
    if (info.descriptor != nullptr && info.descriptor->visitFields != nullptr) {
        info.descriptor->visitFields(data, writer);
    } else {
        why = "the resource type is not reflected";
        return false;
    }
    if (writer.HasError()) {
        why = writer.Error();
        return false;
    }
    return true;
}

CommandResult ResourceList(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (std::optional<Envelope> refusal = CheckKeys(call, {}, kListUsage)) {
        result.envelope = std::move(*refusal);
        return result;
    }
    const Astra::Registry* registry = PickRegistry(call, result.envelope);
    if (registry == nullptr)
        return result;
    std::vector<std::string> names;
    for (const Astra::Registry::ResourceInfo& info : registry->InspectResources())
        if (std::string name = ResourceName(info); !name.empty())
            names.push_back(std::move(name));
    std::sort(names.begin(), names.end());
    nlohmann::json rows = nlohmann::json::array();
    for (std::string& name : names)
        rows.push_back({{"name", std::move(name)}});
    result.envelope = MakeOk(call.command, {{"resources", std::move(rows)}}, {});
    return result;
}

CommandResult ResourceGet(const Call& call, void* /*user*/)
{
    CommandResult result;
    std::optional<Envelope> refusal = CheckKeys(call, {"name"}, kGetUsage);
    if (!refusal && (!call.args.is_object() || !call.args.contains("name")))
        refusal = MakeError(call.command, "arg.missing", "resource.get needs name", std::string(kGetUsage));
    if (!refusal && (!call.args.at("name").is_string() || call.args.at("name").get<std::string>().empty()))
        refusal = MakeError(call.command, "arg.invalid", "name must be a non-empty string", std::string(kGetUsage));
    if (refusal) {
        result.envelope = std::move(*refusal);
        return result;
    }
    const std::string requested = call.args.at("name").get<std::string>();
    const Astra::Registry* registry = PickRegistry(call, result.envelope);
    if (registry == nullptr)
        return result;

    // const: InspectResources stamps nothing, so no resource is marked changed.
    const std::vector<Astra::Registry::ResourceInfo> resources = registry->InspectResources();
    std::vector<std::string> names;
    names.reserve(resources.size());
    for (const Astra::Registry::ResourceInfo& info : resources)
        names.push_back(ResourceName(info));
    NameResolution res = PickComponent(names, requested);
    if (res.pick == NamePick::Many) {
        Error issue = MakeIssue("resource.ambiguous", "resource name '" + requested + "' matches several types",
                                "pass the qualified name: one of the candidates");
        issue.candidates = std::move(res.candidates);
        issue.detail["name"] = requested;
        result.envelope = ErrorEnvelope(call, std::move(issue));
        return result;
    }
    const Astra::Registry::ResourceInfo* found = nullptr;
    if (res.pick == NamePick::One)
        for (std::size_t i = 0; i < resources.size(); ++i)
            if (names[i] == res.qualified)
                found = &resources[i];
    if (found == nullptr) {
        result.envelope = MakeError(call.command, "resource.not-found",
                                    "this world has no resource named '" + requested + "'", std::string(kResourceHint));
        result.envelope.errors.front().detail["name"] = requested;
        return result;
    }

    nlohmann::json value;
    std::string why;
    if (!WriteResource(*found, value, why)) {
        result.envelope = MakeError(call.command, "reflect.unreadable", res.qualified + " could not be read: " + why,
                                    "reflect the resource type (ARC_REFLECT_TYPE) to read it remotely");
        result.envelope.errors.front().detail["name"] = res.qualified;
        return result;
    }

    const std::string inlineJson = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (inlineJson.size() <= kInlineCapBytes) {
        result.envelope = MakeOk(call.command, std::move(value), {});
        return result;
    }
    // Spec s4.8: never stream megabytes into the envelope.
    const std::filesystem::path dir = Detail::ArtifactDir();
    if (dir.empty()) {
        Error w = MakeIssue("resource.no-artifact-dir",
                            res.qualified + " (" + std::to_string(inlineJson.size()) +
                                " bytes of JSON) exceeds the inline cap and this host has no artifact directory, "
                                "so its value was not returned",
                            "read it from a host with a project (Saved/Remote/out)");
        w.detail["bytes"] = inlineJson.size();
        result.envelope = MakeOk(call.command, nullptr, {});
        result.envelope.warnings.push_back(std::move(w));
        return result;
    }
    std::optional<Artifact> artifact = Detail::WriteArtifact(dir, "resource.get", "resource", inlineJson, why);
    if (!artifact) {
        // A real write or rename failure is an error, never ok with no value.
        result.envelope = MakeError(call.command, "resource.artifact-failed",
                                    res.qualified + " exceeds the inline cap and its artifact could not be written: " + why,
                                    "free disk space or check Saved/Remote/out");
        result.envelope.errors.front().detail["bytes"] = inlineJson.size();
        return result;
    }
    Error w = MakeIssue("resource.in-artifact",
                        res.qualified + " (" + std::to_string(inlineJson.size()) +
                            " bytes of JSON) exceeds the inline cap; its value is in the artifact",
                        "read the artifact's path");
    w.detail["bytes"] = inlineJson.size();
    result.envelope = MakeOk(call.command, nullptr, {});
    result.envelope.warnings.push_back(std::move(w));
    result.envelope.artifacts.push_back(std::move(*artifact));
    return result;
}

void RegisterOne(const char* name, const char* summary, const Astra::TypeMeta* argsMeta, CommandFn fn)
{
    Desc desc;
    desc.name = name;
    desc.summary = summary;
    desc.access = Access::Observe;
    desc.mutates = Mutates::None;
    desc.play = PlayPolicy::Any;
    desc.thread = ExecThread::Main;
    desc.needsFrame = false;
    desc.worldScoped = true;   // both resolve --world (ResolveWorld)
    desc.argsMeta = argsMeta;
    desc.fn = fn;
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(desc), &error))
        ARC_ERROR("remote: resource command refused: {}", error);
}

} // namespace

void RegisterResourceCommands()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    RegisterOne("resource.list", "Name every resource in the world", Astra::GetMeta<ResourceListArgs>(), &ResourceList);
    RegisterOne("resource.get", "Read one resource as scene-format JSON (Protected fields masked)",
                Astra::GetMeta<ResourceGetArgs>(), &ResourceGet);
}

} // namespace Arcane::Remote

#endif
