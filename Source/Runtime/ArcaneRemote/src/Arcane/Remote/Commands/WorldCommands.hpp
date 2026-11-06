#pragma once
// world.list (spec s7.1; Servitor P0 Task 19, ruling F16): the host side of
// `servitor world list`. It reads the one command world table (Worlds.hpp,
// CommandWorlds()), so it holds no pointer; HostSession::Start registers it on
// every host and installs the table, Stop clears the table.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// Registers, once per process (a second call is a no-op), one Observe host
// command that runs on the main thread with needsFrame false:
//   world.list {}  -> data {worlds: [{name, role}]} sorted by name (WorldList).
// No table installed (CommandWorlds().fn null) is host.unavailable (exit 5).
// A non-empty options.world (--world) must name one of the worlds, else the
// ResolveWorld refusal (arg.unknown, exit 2): the flag is never ignored.
ARC_REMOTE_API void RegisterWorldCommands();

} // namespace Arcane::Remote

#endif
