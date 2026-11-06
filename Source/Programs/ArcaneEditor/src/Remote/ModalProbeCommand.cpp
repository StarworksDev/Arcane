#include "Remote/ModalProbeCommand.hpp"

#if !defined(ARC_BUILD_DIST)

#include "App/EditorApp.hpp"
#include "App/ModalGate.hpp"

#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <string>
#include <utility>

namespace Arcane::Editor
{

// The wire args of editor.modal.probe, reflected for its schema.
struct ModalProbeArgs {
    std::string op;   // arm | doc | docclear | dismiss | drawn
};
ARC_REFLECT_TYPE(ModalProbeArgs)
    ARC_REFLECT_FIELD(ModalProbeArgs, op)
ARC_REFLECT_TYPE_END()

namespace
{

EditorApp* g_app = nullptr;   // main thread only (the arming site and Drain)

constexpr const char* kUsage = "pass op=arm, op=doc, op=docclear, op=dismiss or op=drawn";

const char* KindName(ModalKind kind)
{
    switch (kind) {
        case ModalKind::None: return "None";
        case ModalKind::Error: return "Error";
        case ModalKind::UnsavedScene: return "UnsavedScene";
        case ModalKind::UnsavedDocument: return "UnsavedDocument";
        case ModalKind::CreateAsset: return "CreateAsset";
    }
    return "None";
}

Remote::CommandResult ModalProbe(const Remote::Call& call, void* /*user*/)
{
    Remote::CommandResult result;
    if (!call.args.is_object()) {
        result.envelope = Remote::MakeError(call.command, "arg.invalid", call.command + " args must be a JSON object", kUsage);
        return result;
    }
    for (const auto& [key, value] : call.args.items()) {
        (void)value;
        if (key != "op") {
            result.envelope = Remote::MakeError(call.command, "arg.unknown", call.command + " has no argument '" + key + "'", kUsage);
            return result;
        }
    }
    if (!call.args.contains("op") || !call.args.at("op").is_string()) {
        result.envelope = Remote::MakeError(call.command, "arg.missing", call.command + " needs op", kUsage);
        return result;
    }
    if (g_app == nullptr) {
        result.envelope = Remote::MakeError(call.command, "host.unavailable", "this editor has not exposed its modals to remote",
                                            "re-arm the editor's remote session (editor.remote.enable)");
        return result;
    }
    const std::string& op = call.args.at("op").get_ref<const std::string&>();
    if (op == "arm") {
        const bool createOpen = g_app->ArmModalProbe();
        if (!createOpen) {
            result.envelope = Remote::MakeError(call.command, "host.unavailable",
                                                "the create dialog did not open (no project is open)",
                                                "launch the editor with --project");
            return result;
        }
        result.envelope = Remote::MakeOk(call.command, {{"createOpen", true}}, {});
    } else if (op == "doc") {
        // A dirty document's close confirm, beside whatever else is queued (ModalGate).
        const bool pending = g_app->ArmDocumentModalProbe();
        result.envelope = pending ? Remote::MakeOk(call.command, {{"documentPending", true}}, {})
                                  : Remote::MakeError(call.command, "host.unavailable",
                                                      "a document confirm is already pending", kUsage);
    } else if (op == "docclear") {
        g_app->ClearDocumentModalProbe();
        result.envelope = Remote::MakeOk(call.command, {{"cleared", true}}, {});
    } else if (op == "dismiss") {
        g_app->DismissModalProbe();
        result.envelope = Remote::MakeOk(call.command, {{"dismissed", true}}, {});
    } else if (op == "drawn") {
        result.envelope = Remote::MakeOk(call.command, {{"kind", KindName(g_app->ModalProbeKind())}}, {});
    } else {
        result.envelope = Remote::MakeError(call.command, "arg.invalid", "op must be arm, doc, docclear, dismiss or drawn", kUsage);
    }
    return result;
}

} // namespace

void SetModalProbeApp(EditorApp* app)
{
    g_app = app;
}

void RegisterModalProbeCommand()
{
    static bool registered = false;   // main thread only (the editor's arming site)
    if (registered)
        return;
    registered = true;

    Remote::Desc desc;
    desc.name = "editor.modal.probe";
    desc.summary = "Test probe: queue an error beside an open create dialog, dismiss it, or read which dockspace modal drew";
    desc.access = Remote::Access::Drive;
    desc.mutates = Remote::Mutates::Preferences;   // editor UI state only; nothing on disk or in the scene
    desc.play = Remote::PlayPolicy::Any;
    desc.thread = Remote::ExecThread::Main;
    desc.needsFrame = false;
    desc.module = "";   // host: never dropped by a module unload
    desc.argsMeta = Astra::GetMeta<ModalProbeArgs>();
    desc.fn = &ModalProbe;
    std::string error;
    if (!Remote::CommandRegistry::Get().Register(std::move(desc), &error))
        ARC_ERROR("remote: editor.modal.probe refused: {}", error);
}

} // namespace Arcane::Editor

#endif
