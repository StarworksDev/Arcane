#pragma once
// scene.tree (spec s7.2; Servitor P0 Task 25): the read-only host side of
// `servitor scene tree`. It resolves its registry through SceneWorld, which is
// ResolveWorld(CommandWorlds(), call.world) (ruling F16), so it holds no world
// pointer; HostSession::Start registers it. The editor program's
// editor.selection.get (Source/Programs/ArcaneEditor/src/Remote/) reads the
// same world through SceneWorld and writes each entity through EntityRef, so
// the two commands cannot disagree on the world or on the entity shape.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Worlds.hpp>

#include <Astra/Entity/Entity.hpp>
#include <Json.hpp>

#include <string>

#if !defined(ARC_BUILD_DIST)

namespace Astra
{
class Registry;
}

namespace Arcane::Remote
{

// Registers, once per process (a second call returns), one Observe host
// command that runs on the main thread with needsFrame false and resolves --world:
//   scene.tree {depth} -> data {roots:[{guid, ref, name, handle, children:[...]}]}
//     Roots are the entities without a parent (Registry::HasParent false);
//     children come from Registry::GetChildren. Siblings sort by Identity.id
//     (Guid::operator<: hi, then lo); entities without an Identity sort last
//     (by handle), carry guid null, and the tree adds one warning
//     entity.no-identity (detail.count). depth (optional; absent or -1 = the
//     whole tree, below -1 or above INT32_MAX is arg.invalid) stops the walk:
//     depth 0 is roots only. A node the walk cut keeps children [] and
//     childrenOmitted = how many children it has. The walk never goes below
//     64 levels whatever depth says (warning scene.depth-limit). A tree whose
//     JSON exceeds the 256 KiB inline cap goes to one `kind:"scene"` artifact
//     in the session's artifact directory (SetEcsArtifactDir) with data null and
//     warning scene.in-artifact; scene.artifact-failed (exit 6) when it cannot
//     be written; with no directory, data null and warning scene.no-artifact-dir.
//   On a server host (CommandRegistry::GetHostKind() == HostKind::Server) the
//   command answers command.host-kind (exit 2) before it resolves a world
//   (ruling F8): a server has no authored scene to show.
ARC_REMOTE_API void RegisterSceneCommands();

// The resolved world (ruling F16): ResolveWorld(CommandWorlds(), requested).
// There is no SetSceneWorld; tests install a one-row table through SetCommandWorlds.
ARC_REMOTE_API PickedWorld SceneWorld(const std::string& requested = {});

// The {guid, name, handle} object scene.tree writes per node (without children):
// guid {hi, lo} plus ref "guid:<canonical>" (the locator entity.get takes) and
// name from Identity, or guid null and name null without one; handle always
// (diagnostic only, not durable). Reads are const: no change-detection stamp.
ARC_REMOTE_API nlohmann::json EntityRef(Astra::Registry& reg, Astra::Entity e);

} // namespace Arcane::Remote

#endif
