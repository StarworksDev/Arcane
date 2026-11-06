#include "Remote/GizmoProbeCommand.hpp"

#if !defined(ARC_BUILD_DIST)

#include "App/EditorApp.hpp"

#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <string>
#include <utility>

namespace Arcane::Editor
{

// editor.gizmo.probe takes no arguments.
struct GizmoProbeArgs {
};
ARC_REFLECT_TYPE(GizmoProbeArgs)
ARC_REFLECT_TYPE_END()

namespace
{

EditorApp* g_app = nullptr;   // main thread only (the arming site and Drain)

Remote::CommandResult GizmoProbe(const Remote::Call& call, void* /*user*/)
{
    Remote::CommandResult result;
    if (!call.args.is_null() && !call.args.is_object()) {
        result.envelope = Remote::MakeError(call.command, "arg.invalid", "editor.gizmo.probe args must be a JSON object",
                                            "editor.gizmo.probe takes no arguments");
        return result;
    }
    if (call.args.is_object() && !call.args.empty()) {
        result.envelope = Remote::MakeError(call.command, "arg.unknown",
                                            "editor.gizmo.probe has no argument '" + call.args.begin().key() + "'",
                                            "editor.gizmo.probe takes no arguments");
        return result;
    }
    if (g_app == nullptr) {
        result.envelope = Remote::MakeError(call.command, "host.unavailable", "this editor has not exposed its gizmo to remote",
                                            "re-arm the editor's remote session (editor.remote.enable)");
        return result;
    }
    if (!g_app->ArmGizmoAbandonProbe()) {
        result.envelope = Remote::MakeError(call.command, "host.unavailable",
                                            "the gizmo probe could not arm (no live scene root, or a drag is already open)",
                                            "open a scene in Edit mode and retry");
        return result;
    }
    result.envelope = Remote::MakeOk(call.command,
                                     {{"survivor", "GizmoProbeA"}, {"beforeX", 0.0}, {"sentinelX", 5.0}}, {});
    return result;
}

} // namespace

void SetGizmoProbeApp(EditorApp* app)
{
    g_app = app;
}

void RegisterGizmoProbeCommand()
{
    static bool registered = false;   // main thread only (the editor's arming site)
    if (registered)
        return;
    registered = true;

    Remote::Desc desc;
    desc.name = "editor.gizmo.probe";
    desc.summary = "Test probe: start a two-target gizmo drag, delete one target, let the frame abandon the drag";
    desc.access = Remote::Access::Drive;
    desc.mutates = Remote::Mutates::Scene;   // creates two entities and deletes one
    desc.play = Remote::PlayPolicy::EditOnly;
    desc.thread = Remote::ExecThread::Main;
    desc.needsFrame = false;
    desc.module = "";   // host: never dropped by a module unload
    desc.argsMeta = Astra::GetMeta<GizmoProbeArgs>();
    desc.fn = &GizmoProbe;
    std::string error;
    if (!Remote::CommandRegistry::Get().Register(std::move(desc), &error))
        ARC_ERROR("remote: editor.gizmo.probe refused: {}", error);
}

} // namespace Arcane::Editor

#endif
