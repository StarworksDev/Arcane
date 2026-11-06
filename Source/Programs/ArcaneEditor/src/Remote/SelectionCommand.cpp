#include "Remote/SelectionCommand.hpp"

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/SceneCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Remote/HostSession.hpp>   // StopEditorRemote (round 4, N3-3)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Arcane::Editor
{

// editor.selection.get takes no arguments.
struct SelectionGetArgs {
};
ARC_REFLECT_TYPE(SelectionGetArgs)
ARC_REFLECT_TYPE_END()

namespace
{

SelectionView g_view;   // main thread only (the arming site and Drain)

Remote::CommandResult SelectionGet(const Remote::Call& call, void* /*user*/)
{
    Remote::CommandResult result;
    if (!call.args.is_null() && !call.args.is_object()) {
        result.envelope = Remote::MakeError(call.command, "arg.invalid", "editor.selection.get args must be a JSON object",
                                            "servitor selection takes no arguments");
        return result;
    }
    if (call.args.is_object() && !call.args.empty()) {
        result.envelope = Remote::MakeError(call.command, "arg.unknown",
                                            "editor.selection.get has no argument '" + call.args.begin().key() + "'",
                                            "servitor selection takes no arguments");
        return result;
    }
    const SelectionView view = g_view;
    if (view.count == nullptr || view.at == nullptr || view.primary == nullptr) {
        result.envelope = Remote::MakeError(call.command, "permission.denied",
                                            "this editor has not exposed its selection to remote",
                                            "re-arm the editor's remote session (editor.remote.enable)");
        return result;
    }

    Remote::PickedWorld picked = Remote::SceneWorld(call.world);   // the world scene.tree reads (ruling F16)
    if (picked.pick != Remote::WorldPick::Ok) {
        result.envelope = std::move(picked.error);   // the dispatcher stamps call.command
        return result;
    }
    if (picked.world.registry == nullptr) {
        result.envelope = Remote::MakeError(call.command, "host.unavailable",
                                            "world '" + picked.world.name + "' has no registry yet",
                                            "retry once the editor has loaded its scene");
        return result;
    }
    Astra::Registry& reg = *picked.world.registry;

    nlohmann::json entities = nlohmann::json::array();
    std::uint64_t noIdentity = 0;
    const std::size_t count = view.count(view.user);
    for (std::size_t i = 0; i < count; ++i) {
        const Astra::Entity e = view.at(i, view.user);
        if (!reg.IsValid(e))
            continue;   // a dead entry Prune has not swept yet
        nlohmann::json ref = Remote::EntityRef(reg, e);
        if (ref["guid"].is_null())
            ++noIdentity;
        entities.push_back(std::move(ref));
    }
    nlohmann::json primary = nullptr;
    const Astra::Entity p = view.primary(view.user);
    if (count > 0 && p.IsValid() && reg.IsValid(p))
        primary = Remote::EntityRef(reg, p)["guid"];

    result.envelope = Remote::MakeOk(call.command, {{"entities", std::move(entities)}, {"primary", std::move(primary)}}, {});
    if (noIdentity > 0) {
        Remote::Error w;
        w.code = "entity.no-identity";
        w.message = std::to_string(noIdentity) + " selected entities have no Identity; their handle is diagnostic only";
        w.remediation = "give them an Identity to address them by guid:<id>";
        w.detail["count"] = noIdentity;
        result.envelope.warnings.push_back(std::move(w));
    }
    return result;
}

} // namespace

const char* StopEditorRemote(std::unique_ptr<Arcane::Remote::HostSession>& session, const char* fallbackReason)
{
    if (!session)
        return nullptr;
    const char* reason = session->ExitReason();
    session->Stop(reason != nullptr ? reason : fallbackReason);   // publishes "shutting-down" first
    session.reset();
    SetSelectionView({});   // the view names the app's m_selection
    return reason;
}

void SetSelectionView(SelectionView view)
{
    g_view = view;
}

void RegisterSelectionCommand()
{
    static bool registered = false;   // main thread only (the editor's arming site)
    if (registered)
        return;
    registered = true;

    Remote::Desc desc;
    desc.name = "editor.selection.get";
    desc.summary = "The editor's selected entities, in selection order, and the primary one";
    desc.access = Remote::Access::Observe;
    desc.mutates = Remote::Mutates::None;
    desc.play = Remote::PlayPolicy::Any;
    desc.thread = Remote::ExecThread::Main;
    desc.needsFrame = false;
    desc.module = "";          // host: never dropped by a module unload
    desc.worldScoped = true;   // resolves --world (SceneWorld)
    desc.argsMeta = Astra::GetMeta<SelectionGetArgs>();
    desc.fn = &SelectionGet;
    std::string error;
    if (!Remote::CommandRegistry::Get().Register(std::move(desc), &error))
        ARC_ERROR("remote: editor.selection.get refused: {}", error);
}

} // namespace Arcane::Editor

#endif
