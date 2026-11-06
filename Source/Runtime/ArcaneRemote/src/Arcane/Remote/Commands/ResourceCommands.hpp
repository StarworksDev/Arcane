#pragma once
// resource.list and resource.get (spec s7.2; Servitor P0 Task 24): the
// read-only host side of `servitor resource list` and `servitor resource get`.
// Both resolve their registry through ResolveWorld(CommandWorlds(), call.world)
// (ruling F16), so they hold no world pointer; HostSession::Start registers them.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// Registers, once per process (a second call returns), two Observe host
// commands that run on the main thread with needsFrame false and resolve --world:
//   resource.list {}      -> data {resources:[{name}]} sorted by name (the
//                            resource type's registered name, e.g. "Arcane::Time")
//   resource.get {name}   -> data: the resource as scene-format JSON
//                            (ReflectionJsonWriter). `name` is the registered name
//                            or a unique short name (text after the last "::");
//                            two short-name hits are resource.ambiguous (exit 2,
//                            candidates), none is resource.not-found (exit 6), an
//                            unreflected resource is reflect.unreadable (exit 6).
// A field reflected with Attr::Flags Protected (on the field or its type), a
// nested struct or vector element holding one, and any field the walk cannot see
// into (std::array, C array, other containers, unresolved types) read
// "<protected>", never their value. JSON over the
// 256 KiB inline cap goes to one `kind:"resource"` artifact in the session's
// artifact directory (SetEcsArtifactDir) with data null and warning
// resource.in-artifact; resource.artifact-failed (exit 6) when it cannot be
// written; with no directory, data null and warning resource.no-artifact-dir.
// Reads are const: no resource is marked changed.
ARC_REMOTE_API void RegisterResourceCommands();

} // namespace Arcane::Remote

#endif
