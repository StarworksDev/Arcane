#include <Arcane/Remote/Commands/EcsCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/CommandSupport.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Reflection.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>

#include <Astra/Debug/Inspector.hpp>
#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace Arcane::Remote
{

// The wire args, reflected for their schema (spec s5.3). Each command reads its
// args JSON itself and refuses unknown keys.
struct EntityGetArgs {
    std::string entity;                    // guid:<id> | name:<unique name> | a raw handle (diagnostic)
    std::vector<std::string> components;   // short or qualified names; empty = every reflected component
};
ARC_REFLECT_TYPE(EntityGetArgs)
    ARC_REFLECT_FIELD(EntityGetArgs, entity)
    ARC_REFLECT_FIELD(EntityGetArgs, components)
ARC_REFLECT_TYPE_END()

struct EcsQueryArgs {
    std::vector<std::string> with;     // every one present
    std::vector<std::string> without;  // none present
    std::vector<std::string> fields;   // Component.member or Component; empty = the `with` components
    std::uint32_t limit = 100;         // rows returned; count is the match count before it
    bool strict = true;                // false: an unknown component is a warning and an empty match
};
ARC_REFLECT_TYPE(EcsQueryArgs)
    ARC_REFLECT_FIELD(EcsQueryArgs, with)
    ARC_REFLECT_FIELD(EcsQueryArgs, without)
    ARC_REFLECT_FIELD(EcsQueryArgs, fields)
    ARC_REFLECT_FIELD(EcsQueryArgs, limit)
    ARC_REFLECT_FIELD(EcsQueryArgs, strict)
ARC_REFLECT_TYPE_END()

// ecs.stats and ecs.archetypes take no arguments.
struct EcsNoArgs {
};
ARC_REFLECT_TYPE(EcsNoArgs)
ARC_REFLECT_TYPE_END()

namespace
{

using Detail::CheckKeys;
using Detail::ErrorEnvelope;
using Detail::kInlineCapBytes;
using Detail::MakeIssue;
using Detail::PickRegistry;
ARC_CONSTANT("main-thread bound: ecs.query builds at most this many rows in one Drain; a larger --limit is clamped")
constexpr std::uint64_t kMaxQueryLimit = 10000;

constexpr std::string_view kEntityUsage =
    "pass entity: guid:<id> or name:<unique name>, and optionally components: [names]";
constexpr std::string_view kQueryUsage =
    "pass with/without/fields: arrays of component names (fields as Component.member), limit, strict";
constexpr std::string_view kNoArgsUsage = "takes no arguments; --world <name> picks one of the host's worlds";
constexpr std::string_view kComponentHint = "servitor ecs archetypes lists the components in this world";

// One component type the world knows: its qualified name (the reflected type
// name, the scene file's key) and its descriptor.
struct ComponentEntry {
    std::string qualified;
    const Astra::ComponentDescriptor* desc = nullptr;
};

std::vector<ComponentEntry> WorldComponents(const Astra::Registry& reg)
{
    std::vector<ComponentEntry> entries;
    const Astra::ComponentRegistry* components = reg.GetComponentRegistry();
    if (components == nullptr)
        return entries;
    components->ForEachComponent([&](Astra::ComponentID, const Astra::ComponentDescriptor& desc) {
        std::string name = desc.meta != nullptr ? std::string(desc.meta->typeName)
                                                : std::string(desc.name != nullptr ? desc.name : "");
        if (!name.empty())
            entries.push_back({std::move(name), &desc});
    });
    return entries;
}

// An optional array of non-empty strings.
std::optional<Envelope> ReadNames(const Call& call, const char* key, std::string_view usage,
                                  std::vector<std::string>& out)
{
    if (!call.args.is_object() || !call.args.contains(key))
        return std::nullopt;
    const nlohmann::json& v = call.args.at(key);
    bool valid = v.is_array();
    if (valid)
        for (const nlohmann::json& item : v)
            valid = valid && item.is_string() && !item.get<std::string>().empty();
    if (!valid)
        return MakeError(call.command, "arg.invalid", std::string(key) + " must be an array of non-empty strings",
                         std::string(usage));
    for (const nlohmann::json& item : v)
        out.push_back(item.get<std::string>());
    return std::nullopt;
}

// Resolves one requested component name. Ok: the entry. Otherwise the issue
// (component.unknown, or component.ambiguous with candidates) and no entry.
const ComponentEntry* ResolveComponent(const std::vector<ComponentEntry>& entries, const std::string& requested,
                                       std::optional<Error>& issue)
{
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const ComponentEntry& e : entries)
        names.push_back(e.qualified);
    NameResolution res = PickComponent(names, requested);
    if (res.pick == NamePick::One) {
        for (const ComponentEntry& e : entries)
            if (e.qualified == res.qualified)
                return &e;
    }
    if (res.pick == NamePick::Many) {
        issue = MakeIssue("component.ambiguous", "component name '" + requested + "' matches several types",
                          "pass the qualified name: one of the candidates");
        issue->candidates = std::move(res.candidates);
    } else {
        issue = MakeIssue("component.unknown", "this world has no component named '" + requested + "'",
                          std::string(kComponentHint));
    }
    issue->detail["name"] = requested;
    return nullptr;
}

nlohmann::json GuidJson(const Guid& guid)
{
    return {{"hi", guid.hi}, {"lo", guid.lo}};
}

// The component as scene-format JSON (ReflectionJsonWriter); a tag component is
// null. A field that may hold a Protected value is masked, as resource.get
// masks it (Detail::WriteMaskedFields; final fix pass, C-I5). False when the
// type is not reflected or a field is unreadable.
bool WriteComponent(const Astra::Registry& reg, Astra::Entity entity, const Astra::ComponentDescriptor& desc,
                    nlohmann::json& out, std::string& why)
{
    if (desc.meta == nullptr || desc.visitFields == nullptr) {
        why = "the component type is not reflected";
        return false;
    }
    const void* instance = reg.GetComponentByHash(entity, desc.hash);   // const: no change-detection stamp
    if (instance == nullptr) {
        if (desc.is_empty) {
            out = nullptr;
            return true;
        }
        why = "the component has no readable instance";
        return false;
    }
    return Detail::WriteMaskedFields(*desc.meta, const_cast<void*>(instance), out, why);   // the writer only reads
}

// The JSON key for each component written: its short name, or the qualified
// name when two written components share a short name.
std::map<const Astra::ComponentDescriptor*, std::string> KeysFor(const std::vector<const ComponentEntry*>& written)
{
    std::map<std::string, int> shortCount;
    for (const ComponentEntry* e : written)
        ++shortCount[ShortName(e->qualified)];
    std::map<const Astra::ComponentDescriptor*, std::string> keys;
    for (const ComponentEntry* e : written) {
        const std::string shortName = ShortName(e->qualified);
        keys[e->desc] = shortCount[shortName] > 1 ? e->qualified : shortName;
    }
    return keys;
}

// --- entity.get ------------------------------------------------------------

struct LocatedEntity {
    Astra::Entity entity;
    const Identity* identity = nullptr;
};

// guid:<id>, name:<unique name>, or a raw handle (a JSON unsigned integer or a
// string of digits).
std::optional<LocatedEntity> Locate(const Call& call, const Astra::Registry& reg, const nlohmann::json& locator,
                                    Envelope& refusal)
{
    std::optional<std::uint64_t> handle;
    std::string text;
    if (locator.is_number_unsigned() || (locator.is_number_integer() && locator.get<std::int64_t>() >= 0)) {
        handle = locator.get<std::uint64_t>();
    } else if (locator.is_string()) {
        text = locator.get<std::string>();
        if (!text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            std::uint64_t value = 0;
            bool overflow = false;
            for (const char c : text) {
                const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
                if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
                    overflow = true;
                value = value * 10u + digit;
            }
            if (!overflow)
                handle = value;
        }
    }

    using HandleType = Astra::Entity::StorageType;
    bool outOfRange = false;
    if constexpr (sizeof(HandleType) < sizeof(std::uint64_t))
        outOfRange = handle && *handle > static_cast<std::uint64_t>(std::numeric_limits<HandleType>::max());
    if (outOfRange) {
        // Out of the handle type's range: never truncated onto a live entity.
        refusal = MakeError(call.command, "entity.not-found", "no live entity has handle " + std::to_string(*handle),
                            "address entities by guid:<id> or name:<name>; handles are diagnostic only");
        refusal.errors.front().detail["handle"] = *handle;
        return std::nullopt;
    }
    if (handle) {
        const Astra::Entity entity(static_cast<HandleType>(*handle));
        if (!reg.IsValid(entity)) {
            refusal = MakeError(call.command, "entity.not-found", "no live entity has handle " + std::to_string(*handle),
                                "address entities by guid:<id> or name:<name>; handles are diagnostic only");
            refusal.errors.front().detail["handle"] = *handle;
            return std::nullopt;
        }
        return LocatedEntity{entity, reg.GetComponent<Identity>(entity)};
    }

    const bool byGuid = text.starts_with("guid:");
    const bool byName = text.starts_with("name:");
    if (!byGuid && !byName) {
        refusal = MakeError(call.command, "arg.invalid", "entity must be guid:<id>, name:<unique name> or a handle",
                            std::string(kEntityUsage));
        return std::nullopt;
    }
    const std::string key = text.substr(5);
    std::optional<Guid> guid;
    if (byGuid) {
        guid = Guid::FromString(key);
        if (!guid) {
            refusal = MakeError(call.command, "arg.invalid", "'" + key + "' is not a GUID",
                                "pass guid:<8-4-4-4-12 hex>, the ref servitor ecs query prints");
            return std::nullopt;
        }
    }

    std::vector<LocatedEntity> hits;
    for (const Astra::Entity e : reg.GetEntityManager()) {
        const Identity* id = reg.GetComponent<Identity>(e);
        if (id == nullptr)
            continue;
        if (byGuid ? id->id == *guid : id->name == key)
            hits.push_back({e, id});
    }
    if (hits.size() == 1)
        return hits.front();
    if (hits.empty()) {
        refusal = MakeError(call.command, "entity.not-found",
                            std::string("no entity has ") + (byGuid ? "guid " : "name ") + key,
                            "servitor ecs query --with Identity --fields Identity.name lists the entities");
        refusal.errors.front().detail["entity"] = text;
        return std::nullopt;
    }
    // Only a name can match twice (spec s7.2): every candidate by guid and name.
    refusal = MakeError(call.command, "entity.ambiguous", std::to_string(hits.size()) + " entities are named " + key,
                        "address one by guid:<id>: one of the candidates");
    std::vector<std::string> candidates;
    for (const LocatedEntity& hit : hits)
        candidates.push_back("guid:" + hit.identity->id.ToString() + " (" + hit.identity->name + ")");
    std::sort(candidates.begin(), candidates.end());
    refusal.errors.front().candidates = std::move(candidates);
    refusal.errors.front().detail["entity"] = text;
    return std::nullopt;
}

CommandResult EntityGet(const Call& call, void* /*user*/)
{
    CommandResult result;
    std::vector<std::string> requested;
    std::optional<Envelope> refusal = CheckKeys(call, {"entity", "components"}, kEntityUsage);
    if (!refusal && (!call.args.is_object() || !call.args.contains("entity")))
        refusal = MakeError(call.command, "arg.missing", "entity.get needs entity", std::string(kEntityUsage));
    if (!refusal)
        refusal = ReadNames(call, "components", kEntityUsage, requested);
    if (refusal) {
        result.envelope = std::move(*refusal);
        return result;
    }
    Envelope pickRefusal;
    Astra::Registry* registry = PickRegistry(call, pickRefusal);
    if (registry == nullptr) {
        result.envelope = std::move(pickRefusal);
        return result;
    }
    const Astra::Registry& reg = *registry;

    Envelope locateRefusal;
    const std::optional<LocatedEntity> located = Locate(call, reg, call.args.at("entity"), locateRefusal);
    if (!located) {
        result.envelope = std::move(locateRefusal);
        return result;
    }

    const std::vector<ComponentEntry> entries = WorldComponents(reg);
    std::vector<const ComponentEntry*> written;
    std::vector<Error> warnings;
    if (requested.empty()) {
        for (const Astra::ComponentDescriptor* desc : reg.GetEntityComponents(located->entity))
            for (const ComponentEntry& e : entries)
                if (desc != nullptr && e.desc->hash == desc->hash && desc->meta != nullptr && desc->visitFields != nullptr)
                    written.push_back(&e);
    } else {
        for (const std::string& name : requested) {
            std::optional<Error> issue;
            const ComponentEntry* entry = ResolveComponent(entries, name, issue);
            if (entry == nullptr) {
                result.envelope = ErrorEnvelope(call, std::move(*issue));
                return result;
            }
            if (!reg.HasComponentByHash(located->entity, entry->desc->hash)) {
                result.envelope = MakeError(call.command, "component.absent",
                                            "the entity has no " + ShortName(entry->qualified),
                                            "servitor entity get <ref> lists the components it has");
                result.envelope.errors.front().detail["component"] = entry->qualified;
                return result;
            }
            if (std::find(written.begin(), written.end(), entry) == written.end())
                written.push_back(entry);
        }
    }

    nlohmann::json components = nlohmann::json::object();
    const auto keys = KeysFor(written);
    for (const ComponentEntry* e : written) {
        nlohmann::json value;
        std::string why;
        if (!WriteComponent(reg, located->entity, *e->desc, value, why)) {
            if (!requested.empty()) {
                result.envelope = MakeError(call.command, "reflect.unreadable",
                                            ShortName(e->qualified) + " could not be read: " + why,
                                            "report it: every component should read as scene JSON");
                result.envelope.errors.front().detail["component"] = e->qualified;
                return result;
            }
            Error w = MakeIssue("reflect.unreadable", ShortName(e->qualified) + " was skipped: " + why, {});
            w.detail["component"] = e->qualified;
            warnings.push_back(std::move(w));
            continue;
        }
        components[keys.at(e->desc)] = std::move(value);
    }

    nlohmann::json entityJson = nlohmann::json::object();
    if (located->identity != nullptr) {
        entityJson["guid"] = GuidJson(located->identity->id);
        entityJson["ref"] = "guid:" + located->identity->id.ToString();   // the locator that addresses it
        entityJson["name"] = located->identity->name;
    } else {
        entityJson["guid"] = nullptr;
        entityJson["name"] = nullptr;
        entityJson["handle"] = static_cast<std::uint64_t>(located->entity.GetValue());
        warnings.push_back(MakeIssue("entity.no-identity",
                                     "the entity has no Identity; its handle is diagnostic only and not durable",
                                     "give it an Identity to address it by guid:<id>"));
    }
    result.envelope = MakeOk(call.command, {{"entity", std::move(entityJson)}, {"components", std::move(components)}}, {});
    result.envelope.warnings = std::move(warnings);
    return result;
}

// --- ecs.query -------------------------------------------------------------

struct QueryRow {
    Astra::Entity entity;
    const Identity* identity = nullptr;
};

bool RowLess(const QueryRow& a, const QueryRow& b)
{
    // guid.hi, guid.lo, name; rows without Identity last, by handle.
    if ((a.identity == nullptr) != (b.identity == nullptr))
        return a.identity != nullptr;
    if (a.identity == nullptr)
        return a.entity.GetValue() < b.entity.GetValue();
    if (a.identity->id.hi != b.identity->id.hi)
        return a.identity->id.hi < b.identity->id.hi;
    if (a.identity->id.lo != b.identity->id.lo)
        return a.identity->id.lo < b.identity->id.lo;
    if (a.identity->name != b.identity->name)
        return a.identity->name < b.identity->name;
    return a.entity.GetValue() < b.entity.GetValue();
}

CommandResult EcsQuery(const Call& call, void* /*user*/)
{
    CommandResult result;
    std::vector<std::string> withNames;
    std::vector<std::string> withoutNames;
    std::vector<std::string> fieldNames;
    std::uint64_t limit = EcsQueryArgs{}.limit;
    bool strict = EcsQueryArgs{}.strict;
    std::optional<Envelope> refusal = CheckKeys(call, {"with", "without", "fields", "limit", "strict"}, kQueryUsage);
    if (!refusal)
        refusal = ReadNames(call, "with", kQueryUsage, withNames);
    if (!refusal)
        refusal = ReadNames(call, "without", kQueryUsage, withoutNames);
    if (!refusal)
        refusal = ReadNames(call, "fields", kQueryUsage, fieldNames);
    if (!refusal && call.args.is_object() && call.args.contains("limit")) {
        const nlohmann::json& v = call.args.at("limit");
        if (v.is_number_unsigned() || (v.is_number_integer() && v.get<std::int64_t>() >= 0))
            limit = v.get<std::uint64_t>();
        else
            refusal = MakeError(call.command, "arg.invalid", "limit must be a non-negative integer",
                                std::string(kQueryUsage));
    }
    if (!refusal && call.args.is_object() && call.args.contains("strict")) {
        const nlohmann::json& v = call.args.at("strict");
        if (v.is_boolean())
            strict = v.get<bool>();
        else
            refusal = MakeError(call.command, "arg.invalid", "strict must be true or false", std::string(kQueryUsage));
    }
    if (refusal) {
        result.envelope = std::move(*refusal);
        return result;
    }
    Envelope pickRefusal;
    Astra::Registry* registry = PickRegistry(call, pickRefusal);
    if (registry == nullptr) {
        result.envelope = std::move(pickRefusal);
        return result;
    }
    const Astra::Registry& reg = *registry;
    const std::vector<ComponentEntry> entries = WorldComponents(reg);

    std::vector<Error> warnings;
    if (limit > kMaxQueryLimit) {
        Error w = MakeIssue("ecs.limit-clamped",
                            "limit " + std::to_string(limit) + " was clamped to " + std::to_string(kMaxQueryLimit),
                            "page with --with/--without, or narrow the query");
        w.detail["requested"] = limit;
        w.detail["limit"] = kMaxQueryLimit;
        warnings.push_back(std::move(w));
        limit = kMaxQueryLimit;
    }
    bool matchNothing = false;
    // An ambiguous name is always an error; an unknown one is an error when
    // strict, else a warning (an unknown `with` then matches nothing).
    auto resolve = [&](const std::string& name, bool isWith, const ComponentEntry*& out) -> std::optional<Envelope> {
        std::optional<Error> issue;
        out = ResolveComponent(entries, name, issue);
        if (out != nullptr)
            return std::nullopt;
        if (strict || issue->code == "component.ambiguous")
            return ErrorEnvelope(call, std::move(*issue));
        if (isWith)
            matchNothing = true;
        warnings.push_back(std::move(*issue));
        return std::nullopt;
    };

    std::vector<const ComponentEntry*> with;
    std::vector<const ComponentEntry*> without;
    for (const std::string& name : withNames) {
        const ComponentEntry* e = nullptr;
        if (auto bad = resolve(name, true, e)) {
            result.envelope = std::move(*bad);
            return result;
        }
        if (e != nullptr && std::find(with.begin(), with.end(), e) == with.end())
            with.push_back(e);
    }
    for (const std::string& name : withoutNames) {
        const ComponentEntry* e = nullptr;
        if (auto bad = resolve(name, false, e)) {
            result.envelope = std::move(*bad);
            return result;
        }
        if (e != nullptr)
            without.push_back(e);
    }

    // fields: Component (whole) or Component.member. members empty = whole component.
    std::vector<const ComponentEntry*> written;
    std::map<const ComponentEntry*, std::set<std::string>> members;
    std::set<const ComponentEntry*> whole;
    for (const std::string& field : fieldNames) {
        const std::size_t dot = field.find('.');
        const std::string componentName = field.substr(0, dot);
        const ComponentEntry* e = nullptr;
        if (auto bad = resolve(componentName, false, e)) {
            result.envelope = std::move(*bad);
            return result;
        }
        if (e == nullptr)
            continue;
        if (std::find(written.begin(), written.end(), e) == written.end())
            written.push_back(e);
        if (dot == std::string::npos) {
            whole.insert(e);
            continue;
        }
        const std::string member = field.substr(dot + 1);
        const Astra::TypeMeta* meta = e->desc->meta;
        if (meta == nullptr || meta->GetField(member) == nullptr) {
            Error issue = MakeIssue("arg.invalid", ShortName(e->qualified) + " has no member '" + member + "'",
                                    "servitor schema component " + ShortName(e->qualified) + " lists its members");
            issue.detail["field"] = field;
            if (meta != nullptr)
                for (const Astra::FieldInfo& f : meta->fields)
                    if (f.IsSerializable())
                        issue.candidates.push_back(std::string(f.name));
            if (strict) {
                result.envelope = ErrorEnvelope(call, std::move(issue));
                return result;
            }
            warnings.push_back(std::move(issue));
            continue;
        }
        members[e].insert(member);
    }
    if (fieldNames.empty())
        written = with;   // no fields: the `with` components, whole
    else
        for (const ComponentEntry* e : written)
            if (whole.contains(e))
                members.erase(e);

    std::vector<QueryRow> matches;
    if (!matchNothing) {
        for (const Astra::Entity e : reg.GetEntityManager()) {
            bool keep = true;
            for (const ComponentEntry* c : with)
                keep = keep && reg.HasComponentByHash(e, c->desc->hash);
            for (const ComponentEntry* c : without)
                keep = keep && !reg.HasComponentByHash(e, c->desc->hash);
            if (keep)
                matches.push_back({e, reg.GetComponent<Identity>(e)});
        }
    }
    std::sort(matches.begin(), matches.end(), &RowLess);

    const auto keys = KeysFor(written);
    nlohmann::json rows = nlohmann::json::array();
    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(limit, matches.size()));
    bool unreadableWarned = false;
    for (std::size_t i = 0; i < take; ++i) {
        const QueryRow& row = matches[i];
        nlohmann::json components = nlohmann::json::object();
        for (const ComponentEntry* c : written) {
            if (!reg.HasComponentByHash(row.entity, c->desc->hash))
                continue;
            nlohmann::json value;
            std::string why;
            if (!WriteComponent(reg, row.entity, *c->desc, value, why)) {
                if (!unreadableWarned) {   // one warning per query, not per row
                    Error w = MakeIssue("reflect.unreadable", ShortName(c->qualified) + " was skipped: " + why, {});
                    w.detail["component"] = c->qualified;
                    warnings.push_back(std::move(w));
                    unreadableWarned = true;
                }
                continue;
            }
            if (const auto it = members.find(c); it != members.end() && value.is_object()) {
                for (auto field = value.begin(); field != value.end();) {
                    if (it->second.contains(field.key()))
                        ++field;
                    else
                        field = value.erase(field);
                }
            }
            components[keys.at(c->desc)] = std::move(value);
        }
        nlohmann::json out = nlohmann::json::object();
        out["guid"] = row.identity != nullptr ? GuidJson(row.identity->id) : nlohmann::json(nullptr);
        out["name"] = row.identity != nullptr ? nlohmann::json(row.identity->name) : nlohmann::json(nullptr);
        if (row.identity != nullptr)
            out["ref"] = "guid:" + row.identity->id.ToString();   // the locator entity.get takes
        else
            out["handle"] = static_cast<std::uint64_t>(row.entity.GetValue());   // diagnostic only
        out["components"] = std::move(components);
        rows.push_back(std::move(out));
    }

    const std::size_t count = matches.size();
    bool truncated = rows.size() < count;
    std::vector<Artifact> artifacts;
    const std::string inlineJson = rows.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (inlineJson.size() > kInlineCapBytes) {
        // Spec s4.8: never stream megabytes into the envelope.
        const std::filesystem::path dir = Detail::ArtifactDir();
        if (dir.empty()) {
            // No artifact directory (no session armed one): the rows are not
            // returned at all; count and truncated still describe the match.
            Error w = MakeIssue("ecs.no-artifact-dir",
                                "the " + std::to_string(rows.size()) + " rows (" + std::to_string(inlineJson.size()) +
                                    " bytes) exceed the inline cap and this host has no artifact directory, so no rows "
                                    "were returned; count is the full match count",
                                "narrow the query with --with, --fields or --limit");
            w.detail["bytes"] = inlineJson.size();
            w.detail["rows"] = rows.size();
            warnings.insert(warnings.begin(), std::move(w));
        } else {
            const nlohmann::json file = {{"count", count}, {"truncated", truncated}, {"rows", rows}};
            std::string why;
            std::optional<Artifact> artifact = Detail::WriteArtifact(
                dir, "ecs.query", "ecs", file.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), why);
            if (!artifact) {
                // A real write or rename failure is an error, never ok with empty rows.
                result.envelope = MakeError(call.command, "ecs.artifact-failed",
                                            "the rows exceed the inline cap and their artifact could not be written: " + why,
                                            "free disk space or check Saved/Remote/out, or narrow the query");
                result.envelope.errors.front().detail["bytes"] = inlineJson.size();
                return result;
            }
            artifacts.push_back(std::move(*artifact));
        }
        rows = nlohmann::json::array();
        truncated = true;
    }
    result.envelope = MakeOk(call.command, {{"count", count}, {"truncated", truncated}, {"rows", std::move(rows)}}, {});
    result.envelope.warnings = std::move(warnings);
    result.envelope.artifacts = std::move(artifacts);
    return result;
}

// --- ecs.stats / ecs.archetypes -------------------------------------------

std::optional<Envelope> NoArgs(const Call& call)
{
    if (!call.args.is_null() && !call.args.is_object())
        return MakeError(call.command, "arg.invalid", call.command + " args must be a JSON object",
                         call.command + " " + std::string(kNoArgsUsage));
    if (call.args.is_object() && !call.args.empty())
        return MakeError(call.command, "arg.unknown", call.command + " has no argument '" + call.args.begin().key() + "'",
                         call.command + " " + std::string(kNoArgsUsage));
    return std::nullopt;
}

CommandResult EcsStats(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (std::optional<Envelope> refusal = NoArgs(call)) {
        result.envelope = std::move(*refusal);
        return result;
    }
    Astra::Registry* registry = PickRegistry(call, result.envelope);
    if (registry == nullptr)
        return result;
    Astra::Debug::InspectorSnapshot snapshot;
    Astra::Debug::Capture(*registry, snapshot);
    result.envelope = MakeOk(call.command,
                             {{"entityCount", snapshot.registry.entityCount},
                              {"archetypeCount", snapshot.registry.archetypeCount},
                              {"chunkCount", snapshot.registry.chunkCount},
                              {"bytesUsed", snapshot.registry.bytesUsed}},
                             {});
    return result;
}

CommandResult EcsArchetypes(const Call& call, void* /*user*/)
{
    CommandResult result;
    if (std::optional<Envelope> refusal = NoArgs(call)) {
        result.envelope = std::move(*refusal);
        return result;
    }
    Astra::Registry* registry = PickRegistry(call, result.envelope);
    if (registry == nullptr)
        return result;
    Astra::Debug::InspectorSnapshot snapshot;
    Astra::Debug::Capture(*registry, snapshot);
    nlohmann::json rows = nlohmann::json::array();
    for (const Astra::Debug::ArchetypeInfo& info : snapshot.archetypes)
        rows.push_back({{"signature", info.signature}, {"entityCount", info.entityCount}, {"chunkCount", info.chunkCount}});
    result.envelope = MakeOk(call.command, {{"archetypes", std::move(rows)}}, {});
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
    desc.worldScoped = true;   // every ECS command resolves --world (ResolveWorld)
    desc.argsMeta = argsMeta;
    desc.fn = fn;
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(desc), &error))
        ARC_ERROR("remote: ECS command refused: {}", error);
}

} // namespace

std::string ShortName(std::string_view qualified)
{
    const std::size_t at = qualified.rfind("::");
    return std::string(at == std::string_view::npos ? qualified : qualified.substr(at + 2));
}

NameResolution PickComponent(const std::vector<std::string>& qualifiedNames, std::string_view requested)
{
    NameResolution res;
    for (const std::string& name : qualifiedNames) {
        if (name == requested) {
            res.pick = NamePick::One;
            res.qualified = name;
            return res;
        }
    }
    std::vector<std::string> hits;
    for (const std::string& name : qualifiedNames)
        if (ShortName(name) == requested)
            hits.push_back(name);
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    if (hits.size() == 1) {
        res.pick = NamePick::One;
        res.qualified = hits.front();
    } else if (hits.size() > 1) {
        res.pick = NamePick::Many;
        res.candidates = std::move(hits);
    }
    return res;
}

void RegisterEcsCommands()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    RegisterOne("entity.get", "Read one entity's reflected components as scene-format JSON",
                Astra::GetMeta<EntityGetArgs>(), &EntityGet);
    RegisterOne("ecs.query", "Entities with and without components; rows of guid, name and the chosen fields",
                Astra::GetMeta<EcsQueryArgs>(), &EcsQuery);
    RegisterOne("ecs.stats", "ECS totals: entities, archetypes, chunks and bytes used",
                Astra::GetMeta<EcsNoArgs>(), &EcsStats);
    RegisterOne("ecs.archetypes", "Every archetype: its component signature, entity count and chunk count",
                Astra::GetMeta<EcsNoArgs>(), &EcsArchetypes);
}

// The directory is shared with resource.get (CommandSupport.hpp).
void SetEcsArtifactDir(const std::filesystem::path& dir)
{
    Detail::SetArtifactDir(dir);
}

void ReleaseEcsArtifactDir(const std::filesystem::path& dir)
{
    Detail::ReleaseArtifactDir(dir);
}

} // namespace Arcane::Remote

#endif
