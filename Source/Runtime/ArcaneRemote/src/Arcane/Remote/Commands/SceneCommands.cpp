#include <Arcane/Remote/Commands/SceneCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/CommandSupport.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Reflection.hpp>
#include <Arcane/Scene/Components.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Remote
{

// The wire args, reflected for their schema (spec s5.3). The command reads its
// args JSON itself and refuses unknown keys.
struct SceneTreeArgs {
    std::int32_t depth = -1;   // levels below the roots to walk; -1 = the whole tree, 0 = roots only
};
ARC_REFLECT_TYPE(SceneTreeArgs)
    ARC_REFLECT_FIELD(SceneTreeArgs, depth)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

namespace
{

using Detail::CheckKeys;
using Detail::kInlineCapBytes;
using Detail::MakeIssue;

constexpr std::string_view kTreeUsage = "pass depth: -1 (the whole tree, the default), 0 (roots only) or N levels";

ARC_CONSTANT("stack bound: the walk is iterative, but the envelope's JSON copy and dump recurse per level (measured in Debug: ~4 KiB of main-thread stack per level, a bare 256-level chain overflows 1 MiB, a 128-level tree overflowed inside a test); 64 keeps headroom; deeper is cut with scene.depth-limit")
constexpr std::int32_t kMaxWalkDepth = 64;

// Identity.id order (Guid::operator<: hi, then lo); no Identity last, by handle.
struct Sibling {
    Astra::Entity entity;
    const Identity* identity = nullptr;
};

bool SiblingLess(const Sibling& a, const Sibling& b)
{
    if ((a.identity == nullptr) != (b.identity == nullptr))
        return a.identity != nullptr;
    if (a.identity != nullptr && a.identity->id != b.identity->id)
        return a.identity->id < b.identity->id;
    return a.entity.GetValue() < b.entity.GetValue();
}

std::vector<Sibling> Sorted(const Astra::Registry& reg, const std::vector<Astra::Entity>& entities)
{
    std::vector<Sibling> out;
    out.reserve(entities.size());
    for (const Astra::Entity e : entities)
        if (reg.IsValid(e))
            out.push_back({e, reg.GetComponent<Identity>(e)});
    std::sort(out.begin(), out.end(), &SiblingLess);
    return out;
}

struct Walk {
    Astra::Registry& reg;
    std::int32_t depthLimit = -1;   // -1: none
    std::uint64_t noIdentity = 0;
    bool hitStackBound = false;
    std::set<std::uint64_t> visited; // a malformed hierarchy (a cycle) never loops
};

// Writes one node's own fields into `out` and decides whether the walk goes
// below it: the sorted children to visit, or none when --depth or the hard
// bound cuts it there (childrenOmitted then says how many it has).
std::vector<Sibling> OpenNode(Walk& walk, nlohmann::json& out, const Sibling& node, std::int32_t level)
{
    out = EntityRef(walk.reg, node.entity);
    if (node.identity == nullptr)
        ++walk.noIdentity;
    const Astra::Registry& reg = walk.reg;
    const std::vector<Astra::Entity> children = reg.GetChildren(node.entity);
    out["children"] = nlohmann::json::array();
    const bool cutByArg = walk.depthLimit >= 0 && level >= walk.depthLimit;
    const bool cutByBound = level >= kMaxWalkDepth;
    if (cutByArg || cutByBound) {
        if (!children.empty()) {
            out["childrenOmitted"] = children.size();
            walk.hitStackBound = walk.hitStackBound || (cutByBound && !cutByArg);
        }
        return {};
    }
    return Sorted(reg, children);
}

// The subtree under `root`, depth-first with an explicit stack, so a deep
// hierarchy costs heap, not main-thread stack. A frame's `node` lives in its
// parent's children array, which grows only after that frame is popped, so the
// pointer stays valid while the frame is on the stack.
void WalkSubtree(Walk& walk, nlohmann::json& out, const Sibling& root)
{
    struct Frame {
        nlohmann::json* node = nullptr;
        std::vector<Sibling> children;
        std::size_t next = 0;
        std::int32_t level = 0;
    };
    std::vector<Frame> stack;
    stack.push_back({&out, OpenNode(walk, out, root, 0), 0, 0});
    while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next == top.children.size()) {
            stack.pop_back();
            continue;
        }
        const Sibling child = top.children[top.next++];
        if (!walk.visited.insert(child.entity.GetValue()).second)
            continue;
        nlohmann::json& siblings = (*top.node)["children"];
        siblings.push_back(nlohmann::json());
        nlohmann::json* slot = &siblings.back();
        const std::int32_t level = top.level + 1;
        std::vector<Sibling> grandchildren = OpenNode(walk, *slot, child, level);
        stack.push_back({slot, std::move(grandchildren), 0, level});   // `top` is not used past here
    }
}

CommandResult SceneTree(const Call& call, void* /*user*/)
{
    CommandResult result;
    // Ruling F8: a server has no authored scene; refused before any world is resolved.
    if (CommandRegistry::Get().GetHostKind() == HostKind::Server) {
        result.envelope = MakeError(call.command, "command.host-kind", "scene.tree is not served by a server host",
                                    "run it against an editor or a runtime; servitor ecs query reads a server's world");
        return result;
    }
    std::optional<Envelope> refusal = CheckKeys(call, {"depth"}, kTreeUsage);
    std::int32_t depth = SceneTreeArgs{}.depth;
    if (!refusal && call.args.is_object() && call.args.contains("depth")) {
        const nlohmann::json& v = call.args.at("depth");
        constexpr auto kMaxDepthArg = std::numeric_limits<std::int32_t>::max();
        // Both branches are bounded: a value above INT32_MAX never truncates.
        const bool inRange = v.is_number_integer() &&
                             (v.is_number_unsigned()
                                  ? v.get<std::uint64_t>() <= static_cast<std::uint64_t>(kMaxDepthArg)
                                  : (v.get<std::int64_t>() >= -1 && v.get<std::int64_t>() <= kMaxDepthArg));
        if (inRange)
            depth = static_cast<std::int32_t>(v.get<std::int64_t>());
        else
            refusal = MakeError(call.command, "arg.invalid", "depth must be a whole number of at least -1",
                                std::string(kTreeUsage));
    }
    if (refusal) {
        result.envelope = std::move(*refusal);
        return result;
    }

    PickedWorld picked = SceneWorld(call.world);
    if (picked.pick != WorldPick::Ok) {
        result.envelope = std::move(picked.error);   // the dispatcher stamps call.command
        return result;
    }
    if (picked.world.registry == nullptr) {
        result.envelope = MakeError(call.command, "host.unavailable",
                                    "world '" + picked.world.name + "' has no registry yet",
                                    "retry once the host has loaded its scene");
        return result;
    }

    Walk walk{*picked.world.registry, depth};
    const Astra::Registry& reg = walk.reg;
    std::vector<Astra::Entity> rootEntities;
    for (const Astra::Entity e : reg.GetEntityManager())
        if (!reg.HasParent(e))
            rootEntities.push_back(e);
    nlohmann::json roots = nlohmann::json::array();
    for (const Sibling& root : Sorted(reg, rootEntities)) {
        if (!walk.visited.insert(root.entity.GetValue()).second)
            continue;
        roots.push_back(nlohmann::json());
        WalkSubtree(walk, roots.back(), root);
    }

    std::vector<Error> warnings;
    if (walk.noIdentity > 0) {
        Error w = MakeIssue("entity.no-identity",
                            std::to_string(walk.noIdentity) +
                                " entities have no Identity: guid null, sorted last, handle diagnostic only",
                            "give them an Identity to address them by guid:<id>");
        w.detail["count"] = walk.noIdentity;
        warnings.push_back(std::move(w));
    }
    if (walk.hitStackBound) {
        Error w = MakeIssue("scene.depth-limit",
                            "the hierarchy is deeper than " + std::to_string(kMaxWalkDepth) +
                                " levels; nodes below that keep childrenOmitted",
                            "read a deep branch with servitor entity get");
        w.detail["limit"] = kMaxWalkDepth;
        warnings.push_back(std::move(w));
    }

    nlohmann::json data = {{"roots", std::move(roots)}};
    const std::string inlineJson = data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (inlineJson.size() <= kInlineCapBytes) {
        result.envelope = MakeOk(call.command, std::move(data), {});
        result.envelope.warnings = std::move(warnings);
        return result;
    }
    // Spec s4.8: never stream megabytes into the envelope.
    const std::filesystem::path dir = Detail::ArtifactDir();
    if (dir.empty()) {
        Error w = MakeIssue("scene.no-artifact-dir",
                            "the tree (" + std::to_string(inlineJson.size()) +
                                " bytes of JSON) exceeds the inline cap and this host has no artifact directory, "
                                "so it was not returned",
                            "pass --depth to read fewer levels");
        w.detail["bytes"] = inlineJson.size();
        warnings.insert(warnings.begin(), std::move(w));
        result.envelope = MakeOk(call.command, nullptr, {});
        result.envelope.warnings = std::move(warnings);
        return result;
    }
    std::string why;
    std::optional<Artifact> artifact = Detail::WriteArtifact(dir, "scene.tree", "scene", inlineJson, why);
    if (!artifact) {
        // A real write or rename failure is an error, never ok with no tree.
        result.envelope = MakeError(call.command, "scene.artifact-failed",
                                    "the tree exceeds the inline cap and its artifact could not be written: " + why,
                                    "free disk space or check Saved/Remote/out, or pass --depth");
        result.envelope.errors.front().detail["bytes"] = inlineJson.size();
        return result;
    }
    Error w = MakeIssue("scene.in-artifact",
                        "the tree (" + std::to_string(inlineJson.size()) +
                            " bytes of JSON) exceeds the inline cap; it is in the artifact",
                        "read the artifact's path");
    w.detail["bytes"] = inlineJson.size();
    warnings.insert(warnings.begin(), std::move(w));
    result.envelope = MakeOk(call.command, nullptr, {});
    result.envelope.warnings = std::move(warnings);
    result.envelope.artifacts.push_back(std::move(*artifact));
    return result;
}

} // namespace

PickedWorld SceneWorld(const std::string& requested)
{
    return ResolveWorld(CommandWorlds(), requested);
}

nlohmann::json EntityRef(Astra::Registry& reg, Astra::Entity e)
{
    const Identity* identity = std::as_const(reg).GetComponent<Identity>(e);   // const: no change stamp
    nlohmann::json out = nlohmann::json::object();
    if (identity != nullptr) {
        out["guid"] = {{"hi", identity->id.hi}, {"lo", identity->id.lo}};
        out["ref"] = "guid:" + identity->id.ToString();   // the locator entity.get takes
        out["name"] = identity->name;
    } else {
        out["guid"] = nullptr;
        out["name"] = nullptr;
    }
    out["handle"] = static_cast<std::uint64_t>(e.GetValue());   // diagnostic only
    return out;
}

void RegisterSceneCommands()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    Desc desc;
    desc.name = "scene.tree";
    desc.summary = "The scene's entity hierarchy: parentless roots, children sorted by guid";
    desc.access = Access::Observe;
    desc.mutates = Mutates::None;
    desc.play = PlayPolicy::Any;
    desc.thread = ExecThread::Main;
    desc.needsFrame = false;
    desc.worldScoped = true;   // resolves --world (SceneWorld)
    desc.argsMeta = Astra::GetMeta<SceneTreeArgs>();
    desc.fn = &SceneTree;
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(desc), &error))
        ARC_ERROR("remote: scene command refused: {}", error);
}

} // namespace Arcane::Remote

#endif
