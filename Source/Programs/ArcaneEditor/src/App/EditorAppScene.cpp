// EditorApp, scene lifecycle: the .arcscene effects (new/open/save), the scene
// dialog launch helper, the editor-state teardown they share, and the
// viewport-framing helpers. Split out of EditorApp.cpp as a pure move.
//
// The effects here are called ONLY from the frame loop's top-of-frame phases or
// its deferred sceneAction (EditorAppFrame.cpp) -- never mid-render. The scene
// dialogs launch through the shared PathPickedThunk trampoline (EditorAppFrame.cpp),
// which Stash()es into m_dialogs.sceneOpen/sceneSave -- the background-thread
// half of that contract.

#include "App/EditorApp.hpp"
#include "Settings/SettingsHost.hpp"
#include "Viewport/EditorCamera.hpp"
#include "Project/RuntimeLaunch.hpp"
#include "Project/ServerLaunch.hpp"   // DoLaunchServer's candidate list + argv

#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/EntityOps.hpp>   // AddPrimitiveEntity / WorldMatrix (ConsumeAddPrimitive, F4 plan 1 T11); LiveSceneRoot (DoSaveScene)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // Arcane::SceneRoot (EnsureScene; ConsumeAddPrimitive's spawn parent)
#include <Arcane/Serialization/SceneAsset.hpp>   // .arcscene read/apply/save (New/Open/Save Scene)

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <chrono>   // Asset-manager Plan 2 Task 5: m_assetActivity's now() stamp
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Arcane::Editor
{
    namespace
    {
        // Same FILE, not the same spelling. The Open-Scene dialog and the Save-Scene
        // dialog can hand back different-but-equivalent paths for one file
        // (separator style, casing, a non-canonical prefix), and DoSaveScene keys
        // the scene's asset Guid off this compare -- a false "different file"
        // re-mints the Guid and dangles the id the asset registry already holds for
        // that file (which boot-scene references store BY Guid). weakly_canonical
        // does not require the file to exist; the error_code overload keeps this
        // exception-free, and a filesystem error falls back to the lexical compare
        // rather than reporting a bogus match.
        bool SameSceneFile(const std::filesystem::path& a, const std::filesystem::path& b)
        {
            std::error_code ea, eb;
            const std::filesystem::path ca = std::filesystem::weakly_canonical(a, ea);
            const std::filesystem::path cb = std::filesystem::weakly_canonical(b, eb);
            if (ea || eb) return a == b;
            return ca == cb;
        }

        // The editor exe's OWN directory -- RuntimeLaunch::ExeCandidates wants
        // this to find ArcaneRuntime.exe beside or as a sibling of it. Same
        // GetModuleFileNameW pattern as EditorFonts.cpp's private ExeDir()
        // (not shared: each TU that needs it keeps its own small copy, same
        // as that file's own comment on the precedent).
        std::filesystem::path CurrentExeDir()
        {
#ifdef _WIN32
            wchar_t buf[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) != 0)
                return std::filesystem::path(buf).parent_path();
#endif
            return std::filesystem::current_path();
        }
    }

    // There is ALWAYS a scene open, the way Unity and UE always have a level open.
    //
    // Without this, a project with no bootScene whose game module does not spawn
    // anything (the correct shape for a data-driven project) opens with no SceneRoot
    // at all -- and since both the render walk and the save walk are subtree walks
    // rooted there, the editor would silently refuse the first thing anyone does:
    // Outliner > New Entity returns invalid and the menu item appears to do nothing.
    //
    // Deliberately does NOT reset the registry. A plugin that spawned its own entities
    // and published its own SceneRoot keeps them -- "no scene loaded means nothing
    // clears" is what makes data-driven scenes safe to adopt one project at a time.
    void EditorApp::EnsureScene()
    {
        Astra::Registry& reg = m_runtime->Registry();
        if (reg.GetResource<Arcane::SceneRoot>())
            return;

        Arcane::Scene::CreateEmpty(reg);
        m_scene.Reset(*m_undo);
        m_editSchedule->RequestFrame(Arcane::Editor::FrameRequest::SceneOpen);
        ARC_INFO("No scene loaded -- started an empty one");
    }

    // Scene dialogs start in the project's Content/scenes, created on demand:
    // a project scaffolded before scenes existed has no such folder, and the
    // dialog would silently fall back to the OS default.
    std::string EditorApp::SceneDialogDir()
    {
        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj) return {};
        const std::filesystem::path dir = proj->Root() / "Content" / "scenes";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.string();
    }

    void EditorApp::ShowSceneSaveDialog()
    {
        const std::string dir = SceneDialogDir();
        m_gpu->Win().ShowSaveFileDialog(&PathPickedThunk,
            new PathDialogRequest{ &m_dialogs.sceneSave, m_dialogs.sceneSave.Arm() },
            "Arcane Scene", "arcscene",
            dir.empty() ? nullptr : dir.c_str());
    }

    bool EditorApp::ClearSceneReferences(std::string reason)
    {
        // Every entity handle the editor is holding names an entity of the OUTGOING
        // scene, and none of them survive the registry swap that follows (Runtime::
        // ResetRegistry, or PlaySession::Stop's RestoreRegistry). Play is stopped
        // FIRST because Stop restores the pre-Play snapshot: left running, it would
        // later overwrite whatever scene is loaded after this.
        const bool wasPlaying = InPlayMode();
        if (wasPlaying && !m_play.Stop(m_runtime->Core(), m_plugin ? &*m_plugin : nullptr))
        {
            // The restore failed and the session is STILL in Play (PlaySession::Stop):
            // the registry holds play-time state. Replacing or clearing anything now
            // would act on the wrong scene, so touch nothing below -- no flip, no
            // server-process stop, no selection or undo clear -- and tell the caller.
            PushPlayStopFailedModal();
            return false;
        }
        AfterPlayFlip(wasPlaying);   // the one flip rule (EditorApp.hpp)
        // The separate-server topology's other half dies with the session it
        // belongs to. Unconditional: a silent no-op on every other topology.
        m_serverProcess.Stop();
        m_selection.Clear();
        // Click-pick can have a readback IN FLIGHT that names entities of the
        // outgoing scene. Bumping the
        // epoch is what makes it refuse to apply to the incoming one, and the
        // Reset inside drops it immediately rather than waiting for the answer
        // to arrive and be rejected. Both belong HERE, with the other handles
        // this function exists to release, for the same reason they do.
        NoteSceneReplaced();
        m_outliner = {};
        // A gizmo drag holds pre-drag poses for entities that are about to stop
        // existing, and both it and the Inspector park a CommandStack ownership
        // token that Clear() below retires.
        m_gizmoDrag = {};
        m_inspector = {};
        // Inspector ownership: the pinned keys and history entries naming
        // entities of the OUTGOING registry die here too. A fresh registry
        // (Runtime::ResetRegistry -> a new Astra::Registry, EntityIDStack from
        // id 0 / version 1) re-mints the SAME (id,version) values, so
        // SceneInspectorSource::Alive cannot tell an old key from a new entity
        // -- the owner of the swap says so explicitly (UE pushes
        // RemoveDeletedObjects into every details view for the same reason).
        // Play > Stop is NOT such a swap: it restores the pre-Play snapshot, so
        // the same keys name the same entities and pins/history survive it.
        m_inspectorHost.InvalidateSource(m_sceneSource);
        m_sceneSelectionEdge.lastEpoch = m_selection.Epoch();   // re-arm on the Clear() above: the first GESTURE in the new scene is the first event
        if (m_undo) m_undo->Clear(std::move(reason));
        return true;
    }

    bool EditorApp::DoNewScene()
    {
        if (!ClearSceneReferences("New scene"))
            return false;   // Play could not be stopped; the modal is queued
        m_runtime->ResetRegistry();
        Arcane::Scene::CreateEmpty(m_runtime->Registry());
        m_scene.Reset(*m_undo);
        // Unlike Open Scene, this path never calls LoadJson to republish (and
        // thereby retract) the "scene" key -- do it explicitly so the outgoing
        // scene's rows don't survive into the new, empty one.
        Arcane::Diagnostics::Clear("scene");
        m_editSchedule->RequestFrame(Arcane::Editor::FrameRequest::SceneOpen);
        ARC_INFO("New scene");
        Arcane::Editor::SettingsWorldCreated();   // settings arc S3: NextWorld settings applied with this world
        return true;
    }

    bool EditorApp::DoOpenScene(const std::filesystem::path& file)
    {
        // READ FIRST. A rejected file must leave the current scene exactly as it is,
        // not empty the editor and then report a failure -- which is what a
        // ResetRegistry-then-load would do. This is the whole reason ReadSceneFile
        // and ApplySceneDocument are separate calls (SceneAsset.hpp).
        std::string err;
        const auto doc = Arcane::Scene::ReadSceneFile(file, &err);
        if (!doc)
        {
            ARC_ERROR("Open Scene: {}", err);
            m_modalErrors.Push("Scene Error", err);
            return false;
        }

        if (!ClearSceneReferences("Opened scene " + file.stem().string()))
            return false;   // Play could not be stopped; the file read above is not applied
        m_runtime->ResetRegistry();
        if (!Arcane::Scene::ApplySceneDocument(*doc, m_runtime->Registry()))
        {
            // Validated but unloadable -- the failure mode ReadSceneFile's structural
            // gate cannot see: the reflection reader latched while walking a
            // component's data. Since Task 3 (F1) that is usually MALFORMED DATA (a
            // stale or hand-edited field value in a shape the field cannot take),
            // with an unsupported reflected field type (E02-3) the rarer,
            // code-defect case -- so the answer is normally IN THE FILE, not in the
            // engine. LoadJson names the component and the field in the Console and
            // publishes a "scene.component.malformed" row to Problems, which is what
            // makes the "(see Console)" in the modal below a real instruction rather
            // than a dead end (final-review finding 1: it used to be a dead end).
            // The registry is already empty by contract, so nothing of the previous
            // scene was overwritten; give the user a well-formed empty scene rather
            // than the half-populated one this left behind.
            Arcane::Scene::CreateEmpty(m_runtime->Registry());
            m_scene.Reset(*m_undo);
            m_modalErrors.Push("Scene Error", "'" + file.generic_string() +
                           "' parsed but could not be loaded -- see the Console or the "
                           "Problems panel for the component and field that refused.");
            ARC_ERROR("Open Scene: ApplySceneDocument failed for {}", file.generic_string());
            return false;
        }

        m_scene.Adopt(file, doc->id, *m_undo);
        m_recents.NoteSceneOpened(m_runtime->CurrentProject(), file);
        m_editSchedule->RequestFrame(Arcane::Editor::FrameRequest::SceneOpen);
        ARC_INFO("Opened scene {}", file.generic_string());
        Arcane::Editor::SettingsWorldCreated();   // settings arc S3: NextWorld settings applied with this world
        return true;
    }

    bool EditorApp::DoSaveScene(const std::filesystem::path& file)
    {
        // Play BACKSTOP, enforced here -- where the bytes are written -- rather than
        // only at the UI call sites. During Play the registry holds play-time
        // mutation that PlaySession::Stop exists to DISCARD; the authored scene is
        // the pre-Play snapshot. Serializing the live registry would therefore
        // overwrite the user's authored file with throwaway state AND report
        // success. The File-menu items and the Ctrl+S edge gate themselves, but
        // gating only there is what let this through: New Scene and Open Scene are
        // deliberately NOT disabled during Play, so the unsaved-changes confirm
        // modal is reachable mid-Play and its Save button lands straight here (it
        // now stops Play first, and satisfies this rather than tripping it), and
        // Play can still begin between a Save As dialog's launch and the frame its
        // result arrives on. From here, no call site can bypass the invariant.
        if (InPlayMode())
        {
            ARC_ERROR("Save Scene: refused -- play mode is running");
            m_modalErrors.Push("Scene Error", "Cannot save while play mode is running.\n"
                           "Stop play mode -- which restores the authored scene -- and save again.");
            return false;
        }

        // SaveJson walks the SceneRoot subtree and returns an EMPTY document when
        // that resource is absent (SceneSerializer.hpp), so without this guard a
        // rootless registry writes {version, entities: []} over the target file,
        // registers it, marks the session clean and logs success -- silent data
        // loss dressed as a save. A DEAD root is the same loss in a different
        // shape: the resource survives a root delete, SaveJson seeds its walk
        // with the dead handle and writes ONE EMPTY ENTITY. LiveSceneRoot is the
        // one check for both (node page + editor upgrades s3.1).
        if (!Arcane::Edit::LiveSceneRoot(m_runtime->Registry()))
        {
            ARC_ERROR("Save Scene: refused -- the registry has no live scene root");
            m_modalErrors.Push("Scene Error", "There is no scene to save.\n"
                           "Create one with File -> New Scene, or open an existing scene.");
            return false;
        }

        // Reuse the scene's existing id when saving over itself; mint one for a new
        // file so the asset registry has something stable to register it under.
        // Identity is by canonical path, not by spelling -- see SameSceneFile.
        const bool sameFile = !m_scene.Path().empty() && SameSceneFile(m_scene.Path(), file);
        const Arcane::Guid id = (sameFile && m_scene.Id().IsValid())
                              ? m_scene.Id() : Arcane::Guid::Generate();

        std::string err;
        if (!Arcane::Scene::SaveSceneFile(file, m_runtime->Registry(), id, &err))
        {
            ARC_ERROR("Save Scene: {}", err);
            m_modalErrors.Push("Scene Error", err);
            return false;
        }

        // Register the written file so it resolves by Guid and lists in the browser.
        // AssetRegistry reads the id back out of the file, so the registered Guid is
        // the one just stamped. A scene saved outside the project's content root
        // cannot be registered -- Runtime/Project already log exactly why, and it is
        // not a save failure: the bytes are on disk either way.
        // Asset-manager Plan 2 Task 5: the return was discarded above (only
        // its truthiness gated MarkAllDirty) -- captured here so the
        // activity feed's Created row can carry the guid AssetRegistry just
        // read back out of the file, same "new-file branch" scope the
        // brief's table names.
        if (const auto registered = m_runtime->RegisterCreatedAsset(file))
        {
            m_assetModel.MarkAllDirty();
            m_assetActivity.Push({ std::chrono::steady_clock::now(), *registered,
                                    file.filename().string(),
                                    Arcane::Editor::AssetActivityKind::Created, {} });
        }

        m_scene.Adopt(file, id, *m_undo);
        m_recents.NoteSceneOpened(m_runtime->CurrentProject(), file);
        ARC_INFO("Saved scene {}", file.generic_string());

        // A save is the moment the project's cover thumbnail is most honest
        // -- what was just saved is on the viewport. Safe here: saves run
        // from top-of-frame phases, never mid-render (this file's header),
        // which is exactly what the capture's waitForIdle needs.
        WriteAutoScreenshot();
        return true;
    }

    // Play button's SeparateWindow branch (Task 6, runtime-host-fold arc):
    // fire-and-forget spawn of ArcaneRuntime.exe on the ACTIVE scene, as
    // SAVED (spec option b) -- never a snapshot of unsaved edits. The editor
    // takes no lock on the child and does not track it; m_play/the toolbar's
    // Play toggle are untouched either way (see DrawSimTimeToolbar).
    //
    // MANDATORY guard, not optional (Task 5 review finding): a never-saved
    // scene has a NIL guid (SceneSession.hpp -- "nil until saved"; the
    // id-reuse branch a few lines above DoSaveScene's SaveSceneFile call is
    // the other place this matters), and RuntimeLaunch::BuildArgs silently
    // OMITS --scene for a nil guid -- the spawned runtime would then boot the
    // project manifest's bootScene instead of what is on screen, with no
    // signal anything was skipped. So SceneSession::Request treats "no valid
    // id yet" exactly like "dirty": both park behind the shared "Unsaved
    // Scene" modal (DrawModals, EditorAppFrame.cpp), and only a successful
    // save (which assigns the id, same as any other Save Scene) lets the
    // LaunchStandalone intent proceed to this function.
    //
    // Called from RunSceneAction's LaunchStandalone case -- both the
    // immediate accept (Request returned true on an already clean, saved
    // scene) and the parked-resume path (the modal's Save button, or
    // ConsumeSceneDialogResults once a never-saved scene's Save-As lands)
    // route through there, per SceneSession::Request's completion-convention
    // comment.
    void EditorApp::DoLaunchStandalone()
    {
        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj)
        {
            m_modalErrors.Push("Play in Separate Window Failed",
                                "Open a project before playing in a separate window.");
            return;
        }

        // Backstop, same rationale as DoSaveScene's Play refusal: the Request
        // machine is the gate; if a caller reaches here unready anyway, refuse
        // loudly rather than spawn against a stale file.
        if (!m_scene.Id().IsValid() || m_scene.IsDirty(*m_undo))
        {
            ARC_ERROR("Play Standalone: refused -- scene is unsaved (the intent machine should have parked this)");
            return;
        }

        // Packaged layout first (ArcaneRuntime.exe beside ArcaneEditor.exe),
        // dev bin layout second (premake's per-project sibling directories) --
        // see RuntimeLaunch::ExeCandidates. Existence is this caller's job by
        // that function's own contract; SpawnDetached would also refuse a
        // missing exe, but resolving here is what lets the failure message
        // name BOTH candidate paths instead of just the one SpawnDetached tried.
        const std::vector<std::filesystem::path> candidates =
            Arcane::Editor::RuntimeLaunch::ExeCandidates(CurrentExeDir());

        std::filesystem::path resolved;
        std::error_code ec;
        for (const std::filesystem::path& candidate : candidates)
        {
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                resolved = candidate;
                break;
            }
        }

        if (resolved.empty())
        {
            std::string looked;
            for (const std::filesystem::path& candidate : candidates)
            {
                if (!looked.empty()) looked += "\nand\n";
                looked += "'" + candidate.string() + "'";
            }
            ARC_ERROR("LaunchStandalone: ArcaneRuntime.exe not found ({})", looked);
            m_modalErrors.Push("Play in Separate Window Failed",
                                "ArcaneRuntime.exe was not found. Looked in:\n" + looked);
            return;
        }

        const std::vector<std::wstring> args = Arcane::Editor::RuntimeLaunch::BuildArgs(
            proj->Root(), m_scene.Id(), m_config.backend);

        if (!Arcane::Editor::RuntimeLaunch::SpawnDetached(resolved, args))
            m_modalErrors.Push("Play in Separate Window Failed",
                                "Failed to launch '" + resolved.string() +
                                "'. See the Console for details.");
    }

    // The "Client + separate server process" play mode's spawn (Core-DLL split,
    // plan 1 Task 7). Same shape as DoLaunchStandalone directly above --
    // resolve, refuse loudly, spawn -- with the two differences its declaration
    // states: the child is TRACKED (m_serverProcess, so Stop can end it) and the
    // scene-readiness gate does not apply (ArcaneServer boots the manifest's
    // bootScene, not the live document).
    void EditorApp::DoLaunchServer()
    {
        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj)
        {
            m_modalErrors.Push("Play with Separate Server Failed",
                                "Open a project before playing with a separate server process.");
            return;
        }

        const std::vector<std::filesystem::path> candidates =
            Arcane::Editor::ServerLaunch::ExeCandidates(CurrentExeDir());

        std::filesystem::path resolved;
        std::error_code ec;
        for (const std::filesystem::path& candidate : candidates)
        {
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                resolved = candidate;
                break;
            }
        }

        if (resolved.empty())
        {
            std::string looked;
            for (const std::filesystem::path& candidate : candidates)
            {
                if (!looked.empty()) looked += "\nand\n";
                looked += "'" + candidate.string() + "'";
            }
            ARC_ERROR("LaunchServer: ArcaneServer.exe not found ({})", looked);
            m_modalErrors.Push("Play with Separate Server Failed",
                                "ArcaneServer.exe was not found. Looked in:\n" + looked);
            return;
        }

        if (!m_serverProcess.Spawn(resolved, Arcane::Editor::ServerLaunch::BuildArgs(proj->Root())))
            m_modalErrors.Push("Play with Separate Server Failed",
                                "Failed to launch '" + resolved.string() +
                                "'. See the Console for details.");
    }

    // F4 plan 1 Task 11 (spec s8): see the declaration's comment (EditorApp.hpp)
    // for the contract. Mirrors the Outliner's own "New Child Entity" arm
    // (EditorPanels.cpp) -- the same ApplyStructural, the same `made` list for
    // the unsaved asterisks, the same collapsed-parent expand and Select --
    // with the mint and the focus point, which only the app has, in front.
    void EditorApp::ConsumeAddPrimitive()
    {
        if (m_outliner.addPrimitivePending < 0)
            return;
        const auto source = static_cast<Arcane::MeshSource>(m_outliner.addPrimitivePending);
        const Astra::Entity parent = m_outliner.addPrimitiveParent;
        m_outliner.addPrimitivePending = -1;
        m_outliner.addPrimitiveParent  = Astra::Entity::Invalid();

        const char* name = Arcane::Editor::PrimitiveMeshName(source);
        if (!name)
            return;   // not a primitive -- nothing the menu can raise

        // The asset first, OUTSIDE the undo step: minting a file is not a
        // registry edit and must not be undone with the entity (a second Add
        // after an undo reuses it, which is the whole point of reuse-by-path).
        const Arcane::Guid mesh = MintOrReusePrimitiveMesh(source);
        if (!mesh.IsValid())
        {
            m_modalErrors.Push("Add 3D Object Failed",
                               std::string("Could not find or create 'Content/")
                                   + Arcane::Editor::PrimitiveMeshRelativePath(source)
                                   + "' (see Console).");
            return;
        }

        Astra::Registry& registry = m_runtime->Registry();
        // The view's focus point is WORLD space; Transform::position is the
        // parent's LOCAL space, so convert through the world matrix of the
        // entity the spawn actually lands under -- `parent`, or SceneRoot for
        // a root spawn (CreateEntityInScene's rule; SceneRoot carries a
        // Transform the user can move, so it is not assumed identity). A dead
        // `parent` falls back to root creation inside CreateEntity, and
        // WorldMatrix(dead) is identity, which matches.
        glm::vec3 position = m_camera.FocusPoint();
        Astra::Entity under = parent;
        if (!under.IsValid())
            if (const Arcane::SceneRoot* root = registry.GetResource<Arcane::SceneRoot>())
                under = root->entity;
        if (under.IsValid() && registry.IsValid(under))
            position = glm::vec3(glm::inverse(Arcane::Edit::WorldMatrix(registry, under))
                                 * glm::vec4(position, 1.0f));

        Astra::Entity created = Astra::Entity::Invalid();
        std::vector<Astra::Entity> made;
        if (Arcane::Editor::ApplyStructural(*m_undo, m_editBinding, "Add 3D Object",
                [&] { created = Arcane::Edit::AddPrimitiveEntity(registry, parent, position,
                                                                 mesh, name);
                      if (created.IsValid()) made.push_back(created);
                      return created.IsValid(); },
                &made))
        {
            if (parent.IsValid())
                m_outliner.collapsed.erase(static_cast<std::uint64_t>(parent.GetValue()));
            m_selection.Select(created);
            FrameCamera(/*selectionOnly=*/true);
        }
    }

    void EditorApp::FrameCamera(bool selectionOnly)
    {
        // Recorded, not executed: WorldTransform is DERIVED and Edit mode's
        // propagation runs once per frame in phase 9 (EditModeSchedule). The
        // request is serviced right after that pass, so framing an entity created
        // or moved THIS frame reads its real world pose -- same frame, before the
        // scene renders. No propagation runs at input time any more.
        m_editSchedule->RequestFrame(selectionOnly ? Arcane::Editor::FrameRequest::Selection
                                                   : Arcane::Editor::FrameRequest::Scene);
    }
}
