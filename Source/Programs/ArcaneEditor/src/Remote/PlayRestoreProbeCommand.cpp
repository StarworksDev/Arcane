#include "Remote/PlayRestoreProbeCommand.hpp"

#if !defined(ARC_BUILD_DIST)

#include "App/EditorApp.hpp"

#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <string>
#include <utility>

namespace Arcane::Editor
{

// editor.play.failnextrestore takes no arguments.
struct PlayRestoreProbeArgs {
};
ARC_REFLECT_TYPE(PlayRestoreProbeArgs)
ARC_REFLECT_TYPE_END()

namespace
{

EditorApp* g_app = nullptr;   // main thread only (the arming site and Drain)

Remote::CommandResult PlayRestoreProbe(const Remote::Call& call, void* /*user*/)
{
    Remote::CommandResult result;
    if (!call.args.is_null() && !call.args.is_object()) {
        result.envelope = Remote::MakeError(call.command, "arg.invalid", "editor.play.failnextrestore args must be a JSON object",
                                            "editor.play.failnextrestore takes no arguments");
        return result;
    }
    if (call.args.is_object() && !call.args.empty()) {
        result.envelope = Remote::MakeError(call.command, "arg.unknown",
                                            "editor.play.failnextrestore has no argument '" + call.args.begin().key() + "'",
                                            "editor.play.failnextrestore takes no arguments");
        return result;
    }
    if (g_app == nullptr) {
        result.envelope = Remote::MakeError(call.command, "host.unavailable", "this editor has not exposed its play session to remote",
                                            "re-arm the editor's remote session (editor.remote.enable)");
        return result;
    }
    g_app->ArmFailNextRestore();
    result.envelope = Remote::MakeOk(call.command, {{"armed", true}}, {});
    return result;
}

} // namespace

void SetPlayRestoreProbeApp(EditorApp* app)
{
    g_app = app;
}

void RegisterPlayRestoreProbeCommand()
{
    static bool registered = false;   // main thread only (the editor's arming site)
    if (registered)
        return;
    registered = true;

    Remote::Desc desc;
    desc.name = "editor.play.failnextrestore";
    desc.summary = "Test probe: make the next play.stop's restore fail once, leaving the editor in Play";
    desc.access = Remote::Access::Drive;
    desc.mutates = Remote::Mutates::PlayMode;
    desc.play = Remote::PlayPolicy::Any;
    desc.thread = Remote::ExecThread::Main;
    desc.needsFrame = false;
    desc.module = "";   // host: never dropped by a module unload
    desc.argsMeta = Astra::GetMeta<PlayRestoreProbeArgs>();
    desc.fn = &PlayRestoreProbe;
    std::string error;
    if (!Remote::CommandRegistry::Get().Register(std::move(desc), &error))
        ARC_ERROR("remote: editor.play.failnextrestore refused: {}", error);
}

} // namespace Arcane::Editor

#endif
