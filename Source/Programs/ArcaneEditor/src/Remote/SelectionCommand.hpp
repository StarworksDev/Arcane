#pragma once
// editor.selection.get (spec s7.2; Servitor P0 Task 25, R8-2): the editor's
// selection, read over remote. The command lives in the editor program because
// SelectionContext does; ArcaneRemote never includes an editor header. The
// editor installs a SelectionView over EditorApp::m_selection at its remote
// arming site (EditorAppFrame.cpp, right after HostSession::Start) and clears
// it when the session stops. Runtime and server never register the command;
// there the registry answers command.host-kind (ruling F-1).
#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <memory>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Editor
{

// Main thread only. count/at/primary read the selection through `user`.
struct SelectionView {
    std::size_t (*count)(void*) = nullptr;
    Astra::Entity (*at)(std::size_t index, void*) = nullptr;
    Astra::Entity (*primary)(void*) = nullptr;
    void* user = nullptr;
};

// Copies the view; a later call replaces it (ruling F11). A view with a null
// count, at or primary answers permission.denied (exit 3).
void SetSelectionView(SelectionView view);

// editor.selection.get; module "" (host), safe to call twice. Observe, main
// thread, needsFrame false, resolves --world through Arcane::Remote::SceneWorld:
//   data {entities:[EntityRef...], primary}: entities in Entities() order (not
//   sorted), primary the primary entity's guid object ({hi, lo}), or null when
//   the selection is empty or the primary has no Identity. A dead entity is
//   skipped; one without an Identity adds warning entity.no-identity.
void RegisterSelectionCommand();

} // namespace Arcane::Editor

namespace Arcane::Remote
{
class HostSession;   // Arcane/Remote/HostSession.hpp
}

namespace Arcane::Editor
{

// The editor's one remote teardown (Servitor final fix round 4, N3-3), used by
// MainLoop's end and by Shutdown(), so a boot that exits before MainLoop with
// a session already armed (--play-as arms in StageFinalize) stops it too.
// When `session` is set: latches its ExitReason, Stop(that reason, else
// `fallbackReason`) -- parked calls answer host.died and the descriptor goes
// -- resets it, and clears the selection view (it names the app's
// m_selection). Returns the latched reason (a static literal), or null when
// there was no session or no remote reason.
const char* StopEditorRemote(std::unique_ptr<Arcane::Remote::HostSession>& session, const char* fallbackReason);

} // namespace Arcane::Editor

#endif
