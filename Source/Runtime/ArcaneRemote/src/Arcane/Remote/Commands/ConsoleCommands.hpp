#pragma once
// console.exec (spec s7.3; Servitor P0 Task 21): the host side of
// `servitor console <line>`, the remote surface's only free-text entry point.
// The line runs through ArcaneCore's RemoteCVarService::Handle on the stored
// user's registry, in the user's context, so a remote caller gets exactly
// what the remote cvar service grants and no more: a console command runs
// only when it carries ServerCanExecute, a Protected value is never printed,
// Hidden names are unknown, and CVarRegistry::Execute's own gates (Cheat,
// the context table, the game's policy) still apply. HostSession::Start
// installs its own user (the cvar commands' {CVarRegistry::Get(),
// cvarContext}) and registers the command (ruling F12); its teardown
// releases the user.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Commands/CvarCommands.hpp>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// Registers console.exec once per process (a second call returns):
//   console.exec {line}   Drive, Mutates::Preferences, needsFrame false, not
//                          worldScoped -> data {line, text, context};
//                          undo not-undoable-command
// The line's first token picks the path: a listed console command runs
// through the service's command path (args = the rest of the line); a cvar
// name alone is the service's get, a cvar name and a value its set.
// Refusals: command.unavailable (cvar_explain; exit 2, use cvar.explain),
// permission.denied (no ServerCanExecute, Cheat without server.cheats, the
// context table or policy; exit 3), cvar.protected (exit 3), cvar.unknown,
// cvar.type-mismatch, cvar.refused-weaker, cvar.refused (the command ran and
// reported failure, or the registry rejected the set; exit 6), arg.missing /
// arg.invalid (also a line holding a control character other than tab) /
// arg.unknown (exit 2), host.unavailable (no user; exit 5). A console
// command's permission is decided from its flags before it runs; a set of a
// Protected cvar echoes "<name> <protected>" in data.line, never the value.
ARC_REMOTE_API void RegisterConsoleCommand();
// Same user struct as Task 20. Safe to call twice; a later call replaces the
// stored pointer (ruling F11). Null answers host.unavailable (exit 5).
ARC_REMOTE_API void SetConsoleCommandUser(CvarCommandUser* user);
// Clears the stored pointer when it is `user` (HostSession's teardown); a user
// another session installed since is left alone.
ARC_REMOTE_API void ReleaseConsoleCommandUser(const CvarCommandUser* user);

} // namespace Arcane::Remote

#endif
