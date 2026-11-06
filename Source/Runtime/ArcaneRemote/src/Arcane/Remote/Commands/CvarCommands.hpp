#pragma once
// cvar.get / cvar.set / cvar.list / cvar.explain (spec s7.1; Servitor P0 Task
// 20, ruling F18): the host side of `servitor cvar ...`. Every command goes
// through ArcaneCore's RemoteCVarService::Handle on the stored user's
// registry, in the user's context, so the remote surface and the server
// console share one policy: a Protected cvar is never readable remotely, in
// any context. HostSession::Start installs its own user and registers the
// commands (ruling F12); its teardown releases the user.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Config/CVarTypes.hpp>

#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Arcane
{
class CVarRegistry; // Arcane/Config/CVarRegistry.hpp
}

namespace Arcane::Remote
{

// The registry the commands read and write, and the CVarContext they ask in:
// Editor on ArcaneEditor, LocalHost on ArcaneRuntime, ServerAdmin on ArcaneServer.
struct CvarCommandUser {
    CVarRegistry* registry = nullptr;
    CVarContext context = CVarContext::Editor;
};

// Safe to call twice; a later call replaces the stored pointer (ruling F11).
// While it is null (or its registry is), every cvar.* command answers
// host.unavailable (exit 5) and dereferences nothing (ruling S3-9).
ARC_REMOTE_API void SetCvarCommandUser(CvarCommandUser* user);
// Clears the stored pointer when it is `user` (HostSession's teardown), so a
// stopped session's cvar commands answer host.unavailable instead of reaching
// a dead object. A user another session installed since is left alone.
ARC_REMOTE_API void ReleaseCvarCommandUser(const CvarCommandUser* user);
// Registers, once per process (a second call returns), four host commands
// that run on the main thread with needsFrame false:
//   cvar.get {name}            Observe -> data {name, type, value, context}
//   cvar.list {prefix?}        Observe -> data {prefix, context, cvars: [{name, text} | {name, text: null, masked}]}
//   cvar.explain {name}        Observe -> data {name, type, value, setBy, audience, scope, apply, flags,
//                                               published, pending, history, context}
//   cvar.set {name, value}     Drive, Mutates::Preferences -> data {name, type, value, previous, setBy,
//                                               audience, apply, context}; undo not-undoable-command
// `value` is the console text (a JSON string, as servitor sends it) or a JSON
// bool, number or number array, which is written as console text.
// Refusals: cvar.protected (exit 3; data.value null, data.masked "<protected>"),
// cvar.unknown, cvar.type-mismatch, cvar.refused-weaker and cvar.refused
// (exit 6), permission.denied (exit 3), arg.missing / arg.unknown /
// arg.invalid (exit 2). None of them is worldScoped: --world is refused by the
// dispatcher (arg.inapplicable), since cvars are per process, not per world.
ARC_REMOTE_API void RegisterCvarCommands();

// The error code for a refused RemoteCVarService response's text:
// cvar.protected, cvar.unknown (also a console command's name), cvar.type-mismatch,
// permission.denied, arg.missing, cvar.refused-weaker ("refused: a stronger
// source holds x", spec s6.4) or cvar.refused ("rejected" and anything else).
ARC_REMOTE_API const char* CvarRefusalCode(std::string_view text);

} // namespace Arcane::Remote

#endif
