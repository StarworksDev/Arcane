#include "Panels/EditorPanels.hpp"
#include "Scene/ComponentCatalog.hpp"
#include "Panels/ConsoleBuffer.hpp"
#include "Input/EditorActions.hpp"
#include "Input/MenuShortcut.hpp"
#include <cstdio>
#include <Arcane/Config/ConsoleModel.hpp>
#include <Arcane/Core/Constant.hpp>   // ARC_CONSTANT (the console colours)
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/UiSettings.hpp>   // ui.copyFlashSeconds -- the Copy All flash
#include <Arcane/Config/Settings.hpp>
#include <Arcane/ImGui/ConsoleInputLine.hpp>   // the ONE command line (s8.2)
#include "Panels/CreateAssetDialog.hpp"   // CreateAssetKind (Assets -> Create, Task 12)
#include "Panels/DefaultLayout.hpp"   // the default layout's pixel geometry (BuildDefaultLayout)
#include "Panels/DiagnosticStore.hpp"   // MatchesDiagnosticFilter, reused for the console's own text search
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Scene/EntityClipboard.hpp"
#include "Panels/EntityList.hpp"
#include "Widgets/IconsLucide.h"
#include "Widgets/UiMetrics.hpp"   // Ui::Px -- the transport strip's pixel gaps follow editor.ui.scale
#include "Panels/InspectorFields.hpp"
#include "Panels/InspectorMeta.hpp"
#include "Panels/InspectorView.hpp"
#include "Panels/InspectorWindows.hpp"   // kPrimaryInspectorWindowId
#include "Panels/SeverityStyle.hpp"   // the Console toolbar's severity toggles (s8.2)
#include "Settings/EditorConsoleSettings.hpp"    // editor.console.* (the Console toolbar toggles, reply lines, category width; S6-41)
#include "Settings/EditorUiStyleSettings.hpp"    // editor.ui.toolbar.* (the brand cluster; S6-45)
#include "Settings/EditorViewportSettings.hpp"   // the view-settings popup's cvars (settings S6-29)
#include "Settings/InspectorSettings.hpp"        // editor.inspector row rhythm, editor.outliner.slowClickMaxSeconds (S6-37)
#include "App/PlayMode.hpp"
#include "Scene/SelectionContext.hpp"
#include "Scene/SelectionOps.hpp"
#include "Scene/UndoGate.hpp"   // UndoMenuState: Edit > Undo/Redo (spec s3.3b/d)

#include <Arcane/Base/Diagnostics.hpp>   // the refused-Play Problems row (final-review fix wave, minor 11)
#include <Arcane/Base/Log.hpp>   // ARC_INFO -- Paste's foreign-clipboard notice
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/Components.hpp>   // Arcane::Identity (the rename target)
#include <Arcane/Sim/RunLoop.hpp>

#include <Astra/Registry/Registry.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder* (:3765) + ImGuiDockNode::LocalFlags
                              // (:2037) -- the docking layout, none of it public.
                              // The grid's table internals left with the widget
                              // layer; the Outliner's own table below is all
                              // public imgui.h API.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    using Arcane::Editor::EditorActions;
    using Arcane::Editor::ActionContext;
    namespace
    {
        // The root guard's visible half (s3.1, 9.27.1): a root-only selection
        // greys the verb, and this says why. ForTooltip's default mouse flags
        // include AllowWhenDisabled (imgui.cpp:1587), which is what lets it
        // reach a greyed item.
        void RootRefusalTooltip(bool rootOnly, SceneRootVerb verb)
        {
            if (rootOnly && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", SceneRootRefusal(verb));
        }

        // The strip status (node page phase s6.4): project (TextDisabled), the
        // Inspector breadcrumb's chevron (TextDisabled), scene (Text), " *" when
        // dirty; "No project" alone otherwise. Read-only, not clickable.
        struct StripStatusMetrics { float naturalW; float minW; };

        StripStatusMetrics MeasureStripStatus(const TitleParts& t)
        {
            if (t.project.empty())
            {
                const float w = ImGui::CalcTextSize("No project").x;
                return { w, w };   // never elided: it is drawn whole or not at all
            }
            const float sp    = ImGui::GetStyle().ItemInnerSpacing.x;
            const float chev  = ImGui::CalcTextSize(ICON_LC_CHEVRON_RIGHT).x;
            const float star  = t.sceneDirty ? ImGui::CalcTextSize(" *").x : 0.0f;
            const float dots  = ImGui::CalcTextSize("...").x;
            const float fixed = 2.0f * sp + chev + star;
            return { fixed + ImGui::CalcTextSize(t.project.c_str()).x + ImGui::CalcTextSize(t.scene.c_str()).x,
                     fixed + 2.0f * dots };
        }

        // Draws at window-local `at` within `budget` px: elides the project
        // first, then the scene (EllipsisToWidth), keeping the chevron and " *".
        // The three-line tooltip always carries the full text.
        void DrawStripStatus(const ToolbarStatus& status, ImVec2 at, float budget)
        {
            const TitleParts& t = status.title;
            ImGui::SetCursorPos(at);
            ImGui::BeginGroup();
            if (t.project.empty())
                ImGui::TextDisabled("No project");
            else
            {
                const float sp    = ImGui::GetStyle().ItemInnerSpacing.x;
                const float fixed = 2.0f * sp + ImGui::CalcTextSize(ICON_LC_CHEVRON_RIGHT).x
                                  + (t.sceneDirty ? ImGui::CalcTextSize(" *").x : 0.0f);
                const float dots  = ImGui::CalcTextSize("...").x;
                float projW  = ImGui::CalcTextSize(t.project.c_str()).x;
                float sceneW = ImGui::CalcTextSize(t.scene.c_str()).x;
                if (fixed + projW + sceneW > budget) projW  = std::max(dots, budget - fixed - sceneW);
                if (fixed + projW + sceneW > budget) sceneW = std::max(dots, budget - fixed - projW);
                const std::string project = EllipsisToWidth(t.project, projW);
                const std::string scene   = EllipsisToWidth(t.scene, sceneW);
                ImGui::TextDisabled("%s", project.c_str());
                ImGui::SameLine(0.0f, sp);
                ImGui::TextDisabled(ICON_LC_CHEVRON_RIGHT);
                ImGui::SameLine(0.0f, sp);
                ImGui::TextUnformatted(scene.c_str());
                if (t.sceneDirty) { ImGui::SameLine(0.0f, 0.0f); ImGui::TextUnformatted(" *"); }
            }
            ImGui::EndGroup();
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(FormatStripStatus(t).c_str());
                if (status.scenePath.empty()) ImGui::TextUnformatted("Scene not saved yet");
                else                          ImGui::Text("Scene file: %s", status.scenePath.c_str());
                if (t.sceneDirty) ImGui::TextUnformatted("Unsaved changes");
                ImGui::EndTooltip();
            }
        }
    }

    void BeginDockSpace(Arcane::CommandStack& undo, MenuRequests& requests,
                        bool sceneDirty, bool playing,
                        bool buildingModule, bool hasGameModule,
                        IdeMenuState ideState,
                        PanelVisibility& panels,
                        bool hasSelection,
                        bool selectionRootOnly,
                        bool hasAssetSelection,
                        bool physicsOverlayOn,
                        const RecentSelection* recents,
                        const SceneRecents::List* sceneRecents,
                        bool closeableDocument)
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_MenuBar;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("EditorDockHost", nullptr, flags);
        ImGui::PopStyleVar(3);

        // Editor menu bar (layout ratified 2026-08-10; a few items are still
        // VISUAL-only -- the wiring pass is landing them task by task).
        // File's scene/project items (including both Open Recent submenus),
        // Save All, Exit, and Assets' Show in Explorer / Copy Path all land
        // in `requests` now, as do Edit's clipboard and selection items.
        // Edit > Preferences... and Project Settings... open the settings windows (settings arc S3).
        // Edit's Undo/Redo drive the CommandStack.
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                requests.fileMenuOpen = true;
                if (ImGui::MenuItem("New Scene", MenuKey("file.newScene").c_str())) requests.newScene = true;
                if (ImGui::MenuItem("Open Scene", MenuKey("file.openScene").c_str())) requests.openScene = true;
                // Open Recent Scene: PER-PROJECT history (SceneRecents.hpp),
                // unlike Open Recent Project below (the Hub's shared,
                // machine-wide list) -- UE's "Recent Levels" shape. Greyed
                // when there is nothing to show (no project, or a project
                // that has never opened/saved a scene).
                const bool anySceneRecents = sceneRecents && !sceneRecents->paths.empty();
                if (ImGui::BeginMenu("Open Recent Scene", anySceneRecents))
                {
                    for (const std::string& p : sceneRecents->paths)
                    {
                        // Stem as the label (its project-relative identity),
                        // full path in the tooltip -- the recents-menu shape
                        // Open Recent Project uses below.
                        const std::string label =
                            std::filesystem::path(p).stem().string() + "##" + p;
                        if (ImGui::MenuItem(label.c_str()))
                            requests.openRecentScenePath = p;
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", p.c_str());
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                // Disabled during Play: the authored scene is the pre-Play snapshot,
                // and the live registry is play-time mutation that PlaySession::Stop
                // exists to discard, so saving it would persist garbage. Both items
                // carry the same tooltip because IsItemHovered names the LAST
                // submitted item -- one call after the pair would explain the greying
                // of "Save As..." only, and "Save" (the item carrying the Ctrl+S
                // hint) is the one users reach for.
                if (ImGui::MenuItem(sceneDirty ? "Save *" : "Save",
                                    MenuKey("file.saveScene").c_str(), false, !playing))
                    requests.saveScene = true;
                if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Stop play mode to save the scene");
                if (ImGui::MenuItem("Save As...", nullptr, false, !playing))
                    requests.saveSceneAs = true;
                if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Stop play mode to save the scene");
                if (ImGui::MenuItem("Close Document",
                                    MenuKey("document.close").c_str(),
                                    false, closeableDocument))
                    requests.closeDocument = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Open Project")) requests.openProject = true;
                if (ImGui::MenuItem("Open Folder...")) requests.openProjectFolder = true;
                // Open Recent Project: the Hub's shared list, already filtered
                // to what THIS editor's ABI can open (RecentProjects.hpp).
                // Greyed when there is nothing at all to show. The picked path
                // lands in the SAME slot the Open Project dialog fills, so it
                // inherits every guard that path has (unsaved-scene confirm,
                // rival-editor lock, ABI gate, failure modal).
                const bool anyRecents =
                    recents && (!recents->visible.empty() || recents->hiddenForAbi > 0);
                if (ImGui::BeginMenu("Open Recent Project", anyRecents))
                {
                    for (const RecentProject& r : recents->visible)
                    {
                        if (ImGui::MenuItem(r.name.c_str()))
                            requests.openRecentPath = r.path;
                        // Full path on hover: the name alone cannot separate two
                        // projects sharing a folder name -- UE's tooltip choice
                        // (FRecentProjectsMenu::MakeMenu).
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", r.path.c_str());
                    }
                    // Never leave the list silently short: an ABI-filtered
                    // absence must be distinguishable from a broken list.
                    if (recents->hiddenForAbi > 0)
                    {
                        if (!recents->visible.empty())
                            ImGui::Separator();
                        const std::string hidden = Recents::HiddenForAbiLine(recents->hiddenForAbi);
                        ImGui::BeginDisabled();
                        ImGui::MenuItem(hidden.c_str());
                        ImGui::EndDisabled();
                    }
                    ImGui::EndMenu();
                }
                // UE's File-menu item is "Save All", and that is what this
                // saves: the scene + every dirty open document. There is no
                // project-level state to save (the manifest is immutable
                // outside narrow seams) -- the old "Save Project" label lied.
                if (ImGui::MenuItem("Save All", nullptr, false, !playing))
                    requests.saveAll = true;
                if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Stop play mode to save");
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))
                    requests.exitEditor = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit"))
            {
                // Undo/Redo share the CommandStack with the Ctrl+Z / Ctrl+Y shortcuts
                // (handled in the app input loop); the shortcut text here is display-only.
                const bool inTxn = undo.InTransaction();
                const UndoMenuItem undoItem = UndoMenuState(undo.CanUndo(), playing, inTxn,
                                                            undo.ClearedReason(), undo.UndoLabel());
                const UndoMenuItem redoItem = UndoMenuState(undo.CanRedo(), playing, inTxn,
                                                            {}, undo.RedoLabel(), /*redo*/ true);
                if (ImGui::MenuItem(undoItem.label.c_str(), MenuKey("edit.undo").c_str(), false, undoItem.enabled)) undo.Undo();
                if (!undoItem.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", undoItem.tooltip.c_str());
                if (ImGui::MenuItem(redoItem.label.c_str(), MenuKey("edit.redo").c_str(), false, redoItem.enabled)) undo.Redo();
                if (!redoItem.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", redoItem.tooltip.c_str());
                ImGui::Separator();
                // Play greys the whole group: structural edits refuse in Play
                // (ApplyStructural's editMode gate), and the affordance rule is
                // that a refusal is visible before the click -- 2026-08-10
                // final review, user-ratified over the spec's old "no extra
                // Play gating" line.
                // The root guard (s3.1): a ROOT-ONLY selection greys the four
                // structural verbs with the reason; a mixed one stays enabled
                // and the verb drops the root itself (SelectionWithoutSceneRoot).
                // Rename stays enabled: the root is renameable.
                const bool structural     = hasSelection && !selectionRootOnly && !playing;
                const bool showRootReason = selectionRootOnly && !playing;
                if (ImGui::MenuItem("Cut", MenuKey("edit.cut").c_str(), false, structural))
                    requests.cutSelection = true;
                RootRefusalTooltip(showRootReason, SceneRootVerb::Cut);
                if (ImGui::MenuItem("Copy", MenuKey("edit.copy").c_str(), false, structural))
                    requests.copySelection = true;
                RootRefusalTooltip(showRootReason, SceneRootVerb::Copy);
                if (ImGui::MenuItem("Paste", MenuKey("edit.paste").c_str(), false, !playing))
                    requests.paste = true;
                if (ImGui::MenuItem("Duplicate", MenuKey("edit.duplicate").c_str(), false, structural))
                    requests.duplicateSelection = true;
                RootRefusalTooltip(showRootReason, SceneRootVerb::Duplicate);
                // Same code paths as the Outliner's F2/Del bindings.
                if (ImGui::MenuItem("Rename", MenuKey("outliner.rename").c_str(), false, hasSelection && !playing))
                    requests.renameSelected = true;
                if (ImGui::MenuItem("Delete", MenuKey("outliner.delete").c_str(), false, structural))
                    requests.deleteSelected = true;
                RootRefusalTooltip(showRootReason, SceneRootVerb::Delete);
                ImGui::Separator();
                if (ImGui::MenuItem("Select All"))       requests.selectAll = true;
                if (ImGui::MenuItem("Deselect All"))     requests.deselectAll = true;
                if (ImGui::MenuItem("Invert Selection")) requests.invertSelection = true;
                ImGui::Separator();
                // UE's placement and order: the Edit menu's closing section is
                // Editor Preferences... then Project Settings... (vendored
                // MainMenu.cpp:261-276). Both open the generated settings
                // windows (settings arc S3, spec s6.1).
                if (ImGui::MenuItem("Preferences..."))
                    requests.showPreferences = true;
                if (ImGui::MenuItem("Project Settings..."))
                    requests.showProjectSettings = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Assets"))
            {
                if (ImGui::BeginMenu("Create"))
                {
                    // Spec s7's FINAL menu: Material... / Material Instance...
                    // / -- / Mesh... / Sprite... / Scene...  Today's
                    // Material.../Mesh Material... pair collapsed into one
                    // Material... whose dialog carries the surface field, so
                    // "post" and "sprite" materials get a real creation route
                    // for the first time (sprite used to be reachable only by
                    // re-kinding an already-created fullscreen document).
                    //
                    // The SAME list the Asset Browser's `+ Create` popup and
                    // a row's Create submenu draw (AssetPanelCommon.cpp's
                    // DrawCreateMenuEntries) -- spelled twice only because
                    // this menu bar lives in a different TU with a different
                    // request struct; both raise the identical
                    // CreateAssetKind value into the identical
                    // BeginCreateAsset entry, which is what the invariant
                    // actually requires.
                    const auto entry = [&](const char* label, Arcane::Editor::CreateAssetKind kind)
                    {
                        if (ImGui::MenuItem(label))
                            requests.requestCreateKind = static_cast<int>(kind);
                    };
                    entry("Material...",          Arcane::Editor::CreateAssetKind::Material);
                    entry("Material Instance...", Arcane::Editor::CreateAssetKind::MaterialInstance);
                    ImGui::Separator();
                    // Raise the request now; the dialog grows their fields in
                    // Task 13 (DrawCreateAssetDialog's own scope comment).
                    // F4 plan 1 Task 11 (spec s8): Mesh is a submenu of the
                    // five primitives -- the same shape DrawCreateMenuEntries
                    // draws, each entry the one request with its MeshSource
                    // preset.
                    if (ImGui::BeginMenu("Mesh"))
                    {
                        for (const Arcane::MeshSource source : Arcane::Editor::kPrimitiveMeshSources)
                        {
                            if (ImGui::MenuItem(Arcane::Editor::PrimitiveMeshName(source)))
                            {
                                requests.requestCreateKind =
                                    static_cast<int>(Arcane::Editor::CreateAssetKind::Mesh);
                                requests.requestMeshSource = static_cast<int>(source);
                            }
                        }
                        ImGui::EndMenu();
                    }
                    entry("Sprite...", Arcane::Editor::CreateAssetKind::Sprite);
                    entry("Scene...",  Arcane::Editor::CreateAssetKind::Scene);
                    entry("Input Actions...", Arcane::Editor::CreateAssetKind::InputActions);
                    ImGui::Separator();
                    // The editor<->IDE surface, step 3: code under Source/.
                    entry("C++ Class...", Arcane::Editor::CreateAssetKind::CppClass);
                    ImGui::EndMenu();
                }
                // Act on the Assets panel's last-clicked row; greyed until one
                // exists. Resolution (Guid -> mount -> file) happens app-side
                // at the click -- a stale selection warns instead of acting.
                if (ImGui::MenuItem("Show in Explorer", nullptr, false, hasAssetSelection))
                    requests.showInExplorer = true;
                if (ImGui::MenuItem("Copy Path", nullptr, false, hasAssetSelection))
                    requests.copyAssetPath = true;
                if (ImGui::MenuItem("Open as text", nullptr, false, hasAssetSelection))
                    requests.openAssetAsText = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                // The whole-world physics overlay (outlines + contacts), Edit
                // and Play alike -- spec 2026-09-11-physics-2d-wiring s6.3. The
                // SELECTED body's outline needs no toggle: it is always drawn
                // in Edit mode. Session state, deliberately not persisted.
                if (ImGui::MenuItem("Physics Overlay", nullptr, physicsOverlayOn))
                    requests.togglePhysicsOverlay = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Window"))
            {
                // Grouped sections over the registry (panel-split spec s4.2):
                // dim section label, that section's panels, separator. The
                // Viewport (permanent) is deliberately NOT listed -- its tab is
                // always physically present in the central tab bar, so a menu
                // row would duplicate it (spec s4.3; do not re-add "for
                // completeness").
                for (PanelSection section : kSectionMenuOrder)
                {
                    ImGui::TextDisabled("%s", kSectionLabels[static_cast<std::size_t>(section)]);
                    for (const PanelInfo& p : kPanels)
                    {
                        if (p.section != section || p.permanent)
                            continue;
                        ImGui::MenuItem(p.name, nullptr,
                                        &panels.visible[static_cast<std::size_t>(p.id)]);
                    }
                    ImGui::Separator();
                }
                // Another Inspector instance (inspector-ownership spec s3.3):
                // pin one, let the other follow.
                // Stays enabled: InspectorHost::AddInstance() returns -1 when the
                // pool of 8 is full and the app treats that as a no-op; the menu
                // has no host access.
                if (ImGui::MenuItem("New Inspector"))
                    requests.newInspector = true;
                ImGui::Separator();
                // Rebuild the stock dock layout (the first-run path, on
                // demand) and re-show everything -- also the standing cure for
                // an old imgui.ini hiding newly shipped panels.
                if (ImGui::MenuItem("Reset Layout"))
                    requests.resetLayout = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Tools"))
            {
                // Disabled during Play -- the UE model: the level editor
                // refuses a recompile while PIE runs (vendored source,
                // LevelEditorActions.cpp:1360-1374) and hot reload defers a
                // finished compile until play ends (HotReload.cpp:1272).
                // Mid-Play reload is a later opt-in, desk-verified first.
                // Also disabled while a build is already running (one at a
                // time -- the Runner refuses too, this just says so up front)
                // and when the open project has no gameModule to rebuild.
                const bool canRebuild = !playing && !buildingModule && hasGameModule;
                if (ImGui::MenuItem(buildingModule ? "Rebuilding..." : "Rebuild Game Module",
                                    nullptr, false, canRebuild))
                    requests.rebuildModule = true;
                if (!canRebuild && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(playing        ? "Stop to rebuild"
                                      : buildingModule ? "A rebuild is already running (see Console)"
                                                       : "This project has no game module");
                // Source/ in the Asset Browser, step 2: the solution-level
                // entry point (Unreal keeps its "Open Visual Studio" under
                // Tools; Build is our developer-actions menu and this arc,
                // like the Rebuild item's, adds no top-level menu). Greyed on
                // the two facts the app hands in -- no project, or no Visual
                // Studio install (UE's CanAccessSourceCode greying) -- and
                // NOT on Play/build state: opening the IDE disturbs neither.
                // A never-generated project has no .slnx yet; the app runs
                // premake first (see EditorApp::OpenInIde), not the menu.
                const bool canOpenIde = ideState == IdeMenuState::Available;
                if (ImGui::MenuItem("Open Visual Studio Solution...", nullptr, false, canOpenIde))
                    requests.openIde = true;
                if (!canOpenIde && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(ideState == IdeMenuState::NoProject
                                          ? "Open a project first"
                                          : "No Visual Studio install found (vswhere found no devenv.exe)");
#if !defined(ARC_BUILD_DIST)
                // GPU crash diagnostics arc, Task 11: the desk battery's
                // trigger. Build is the developer-actions menu (it already owns
                // Rebuild Game Module) and this arc ratified no new top-level
                // menu, so it lands here rather than inventing a Debug menu for
                // one item.
                //
                // Behind a SUBMENU, deliberately. Every other item in this menu
                // bar is recoverable; this one deliberately loses the device and
                // ends the session, so it should not sit one stray click away
                // from Rebuild Game Module. Two intentional steps is the whole
                // guard -- no confirm modal, because a command whose label says
                // "Crash GPU (diagnostics test)" and whose tooltip spells out the
                // consequence has already asked.
                //
                // NOT gated on Play: faulting during Play is a legitimate case to
                // capture. NOT gated on the injector existing either -- creation
                // is lazy at the click, and a failure logs ARC_ERROR rather than
                // silently greying an item nobody could explain.
                ImGui::Separator();
                if (ImGui::BeginMenu("Diagnostics"))
                {
                    if (ImGui::MenuItem("Crash GPU (diagnostics test)"))
                        requests.crashGpu = true;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "Dispatches a deliberately faulting compute shader.\n"
                            "The OS watchdog (TDR) resets the GPU, the device is lost,\n"
                            "and THIS EDITOR SESSION ENDS -- on purpose.\n"
                            "A crash report lands in Saved/Diagnostics/.");
                    ImGui::EndMenu();
                }
#endif
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        // Host window is left OPEN: the caller draws the fixed toolbar strip
        // (DrawSimTimeToolbar) into it, then closes it via EndDockSpace().
    }

    // ---- The default layout's geometry: Panels/DefaultLayout.hpp (USER
    // DECISION 2026-09-30 -- the user's ReferenceProject layout is the default;
    // the pixel targets, their clamps and the band's proportion are computed
    // there from editor.layout.factory.*, LayoutFactorySettings). ----

    namespace
    {
        // A node's size along `axis` for a split: its live Size, else (a node
        // just loaded from the ini, never updated by a DockSpace() yet) its SizeRef.
        float DockNodeExtent(ImGuiID nodeId, ImGuiAxis axis)
        {
            const ImGuiDockNode* n = ImGui::DockBuilderGetNode(nodeId);
            if (!n) return 0.0f;
            return n->Size[axis] > 0.0f ? n->Size[axis] : n->SizeRef[axis];
        }

        // DockBuilderSplitNode's ratio for giving the node at the split
        // direction `px` pixels of `nodeId`: DockNodeTreeSplit divides the
        // node's size minus one DockingSeparatorSize (imgui.cpp DockNodeTreeSplit).
        float PixelSplitRatio(ImGuiID nodeId, ImGuiAxis axis, float px)
        {
            const float avail = DockNodeExtent(nodeId, axis) - ImGui::GetStyle().DockingSeparatorSize;
            return avail > 1.0f ? std::clamp(px / avail, 0.05f, 0.95f) : 0.5f;
        }
    }

    // Build the standard editor layout once (when there is no saved .ini node
    // yet) or on Window -> Reset Layout -- the geometry above. Names must match
    // each panel's ImGui::Begin() title. `size` is the dockspace's size THIS
    // frame (what the DockSpace() call right after will occupy).
    static void BuildDefaultLayout(ImGuiID dockspaceId, ImVec2 size)
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, size);
        // editor.layout.factory.* (settings S6-32): read once, at build time.
        const LayoutFactorySettings& factory = Arcane::Settings<LayoutFactorySettings>();
        const DefaultLayoutPixels px = ComputeDefaultLayoutPixels(size.x, size.y, factory);

        // The central node follows the INHERITOR side of every split (the side
        // opposite the direction), so it ends up top-right of the left block.
        ImGuiID left = 0;
        const ImGuiID inspectorId = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Right,
            PixelSplitRatio(dockspaceId, ImGuiAxis_X, px.inspector), nullptr, &left);
        ImGuiID top = 0;
        const ImGuiID bandId = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down,
            PixelSplitRatio(left, ImGuiAxis_Y, px.bottomBand), nullptr, &top);
        ImGuiID central = 0;
        const ImGuiID outlinerId = ImGui::DockBuilderSplitNode(top, ImGuiDir_Left,
            PixelSplitRatio(top, ImGuiAxis_X, px.outliner), nullptr, &central);
        // Inspector filters (spec s6): the "Assets only" Inspector sits right
        // of the browser's tab node, so browsing assets never replaces the
        // entity page in the main Inspector. NOT a pixel target: neither
        // side of this split holds the central node, so ImGui re-divides it
        // by SizeRef ratio on every resize -- the user's 1920-scale
        // proportion (DefaultAssetsInspectorBandFraction) is right at every
        // size, a pixel target only at the build size.
        ImGuiID browserNodeId = 0;
        const ImGuiID assetsInspectorId = ImGui::DockBuilderSplitNode(bandId, ImGuiDir_Right,
            DefaultAssetsInspectorBandFraction(factory), nullptr, &browserNodeId);

        ImGui::DockBuilderDockWindow("Outliner", outlinerId);
        // The main Inspector owns the right column alone, full height: a
        // material's page (preview + params) is an Inspector page now
        // (inspector filters s6a), so there is no "Material" window beside it.
        ImGui::DockBuilderDockWindow(kPrimaryInspectorWindowId, inspectorId);   // the legacy "Inspector" id (### skipped)
        // ONE tab node, Asset Browser first (the first docked is the selected tab).
        ImGui::DockBuilderDockWindow("Asset Browser", browserNodeId);
        ImGui::DockBuilderDockWindow("Asset Graph",   browserNodeId);
        ImGui::DockBuilderDockWindow("Asset Status",  browserNodeId);
        ImGui::DockBuilderDockWindow("Console",       browserNodeId);
        ImGui::DockBuilderDockWindow("Problems",      browserNodeId);
        ImGui::DockBuilderDockWindow(kAssetsInspectorWindowId, assetsInspectorId);
        ImGui::DockBuilderDockWindow("Viewport",      central);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void SelectDockTab(const char* windowName)
    {
        // FindWindowByName rather than a stored handle: the panel windows are
        // created by their own Begin, and this may run before the first one.
        ImGuiWindow* w = ImGui::FindWindowByName(windowName);
        if (!w || !w->DockNode || !w->DockNode->TabBar)
            return;
        // A docked window's tab id IS window->TabId -- imgui.cpp:19614 and
        // :19620 both select a tab by exactly that value.
        //
        // TabBarQueueFocus's ImGuiTabItem* overload (imgui_widgets.cpp:
        // 10379-10382) is the one that works here: the const char* overload
        // ASSERTS `(tab_bar->Flags & ImGuiTabBarFlags_DockNode) == 0`
        // (imgui_widgets.cpp:10386, "Only supported for manual/explicit tab
        // bars"), so naming the tab would fire an assert on every call.
        //
        // It parks NextSelectedTabId, which TabBarLayout applies on the next
        // frame -- deliberately a REQUEST, not a write: it loses to the node's
        // own newly-added-tab rule (imgui.cpp:19617-19620) rather than fighting
        // it, and it never touches g.NavWindow, so the center document keeps
        // both focus and its tab.
        if (ImGuiTabItem* tab = ImGui::TabBarFindTabByID(w->DockNode->TabBar, w->TabId))
            ImGui::TabBarQueueFocus(w->DockNode->TabBar, tab);
    }

    void FocusDockTab(const char* windowName)
    {
        SelectDockTab(windowName);
        // After a context-menu click ImGui restores NavWindow to the menu's
        // owner (the Browser). QueueFocus then loses to 19611-19613 unless
        // NavWindow is the window whose tab we want. SetWindowFocus is the
        // Inspector/Material-forbidden call; it is correct HERE because the
        // user asked to go to this panel, and ConsumeAssetPanelActions runs
        // BEFORE DocumentHost::DrawAll, so a center SetNextWindowFocus later
        // in the same frame can still win the center node.
        ImGui::SetWindowFocus(windowName);
    }

    static_assert(InspectorHost::kAssetsInstanceId == 1, "kAssetsInspectorWindowId names ###inspector_1");

    DockSpaceResult EndDockSpace(bool resetLayout, int upgradeLegacyInspectorId)
    {
        const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
        DockSpaceResult result;

        // First run (no saved .ini layout) -- or an explicit Window -> Reset
        // Layout: arrange the default editor layout. Same call, same safe
        // point (DockBuilder mutations before the DockSpace() submission).
        if (resetLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
        {
            // The size the DockSpace() below takes (ImVec2(0, 0) = the
            // remaining content region of the host window).
            ImVec2 size = ImGui::GetContentRegionAvail();
            if (size.x <= 1.0f || size.y <= 1.0f) size = ImGui::GetMainViewport()->WorkSize;
            BuildDefaultLayout(dockspaceId, size);
            result.builtDefault = true;
        }
        else if (upgradeLegacyInspectorId >= 1)
        {
            // The one-time pre-feature upgrade (spec s6): split the Asset
            // Browser's node and dock the Assets instance on its right. The
            // browser's DockId comes from its window (or its ini settings when
            // the window has not been submitted yet this session).
            ImGuiID browserDock = 0;
            if (ImGuiWindow* w = ImGui::FindWindowByName("Asset Browser")) browserDock = w->DockId;
            else if (ImGuiWindowSettings* s = ImGui::FindWindowSettingsByID(ImHashStr("Asset Browser"))) browserDock = s->DockId;
            const std::string id = "###inspector_" + std::to_string(upgradeLegacyInspectorId);
            // The main Inspector needs no re-dock: "###Inspector" hashes to the
            // legacy bare "Inspector" id (ImHashStr skips "###", imgui.cpp:2539),
            // so its [Window][Inspector] entry and DockId carry over unchanged.
            //
            // A window only ever docks into a LEAF, so its DockId names one;
            // the IsLeafNode() guard is belt and braces: DockBuilderSplitNode
            // on a split parent would re-split a node that already has
            // children. DockBuilderSplitNode(node, Right, ratio, at_dir,
            // opposite): the node's current windows move to the child
            // OPPOSITE the split direction (imgui.cpp:18399, child 0 for
            // Right), so the browser's tab set stays LEFT and the new right
            // child is the Inspector's. Like the default's band, neither child
            // holds the central node (a pre-feature browser tab set sits in
            // its own bottom node), so ImGui divides this split by SizeRef
            // ratio on resize: it takes the default's SAME proportion
            // (DefaultAssetsInspectorBandFraction), not pixels. (Were the
            // browser docked INTO the central node, the central flag stays
            // with the browser's child and Inspector 2 keeps the pixels that
            // proportion gives it here -- still a sane width.)
            ImGuiDockNode* node = browserDock != 0 ? ImGui::DockBuilderGetNode(browserDock) : nullptr;
            if (node != nullptr && node->IsLeafNode())
            {
                // A node just loaded from the ini has not been laid out yet:
                // Size is 0 and only SizeRef is known. DockNodeTreeSplit sizes
                // the children from node->Size (clamped up to 2x WindowMinSize,
                // 64 px), and ImTrunc at that scale skews the stored SizeRef
                // ratio -- which is all a ratio-shared split keeps (measured:
                // 47 : 17, 26.6% instead of 25.5%). Split at the loaded size.
                if (node->Size.x <= 0.0f && node->SizeRef.x > 0.0f)
                    node->Size = node->SizeRef;
                ImGuiID left = browserDock;
                const ImGuiID right = ImGui::DockBuilderSplitNode(left, ImGuiDir_Right,
                    DefaultAssetsInspectorBandFraction(Arcane::Settings<LayoutFactorySettings>()), nullptr, &left);
                ImGui::DockBuilderDockWindow(id.c_str(), right);
            }
            // An undocked (or non-leaf) browser splits nothing: the new
            // instance then takes DrawInspectorWindows' New Inspector
            // placement (the primary Inspector's dock node).
            ImGui::DockBuilderFinish(dockspaceId);
        }

        // Emit the dockspace into the still-open host window. Anything drawn between
        // BeginDockSpace and here is a fixed strip above it.
        //
        // The central node is NOT locked anymore: the Viewport is a normal tab
        // (movable, dockable-over) and editor documents open as tabs beside it
        // -- the UE/Unity shape where asset editors share the main area with
        // the scene.
        // NoWindowMenuButton kills the tab-bar arrow menu (the one offering
        // "Hide tab bar") on EVERY node under this dockspace: DockSpace() flags
        // become the root's SharedFlags each frame, and SharedFlagsInheritMask_
        // is ~0 so children inherit them (imgui.cpp:18914/20202) -- no per-node
        // flags, no layout reset needed.
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_NoWindowMenuButton);

        // SCRUB the old lock: NoTabBar/NoCloseButton are SAVED dock flags, so
        // every imgui.ini written while the lock existed still carries them --
        // simply not re-applying changes nothing on machines with a layout.
        if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
            central->LocalFlags &= ~(ImGuiDockNodeFlags_NoTabBar
                                   | ImGuiDockNodeFlags_NoCloseButton
                                   | ImGuiDockNodeFlags_NoDockingOverMe
                                   | ImGuiDockNodeFlags_NoUndocking);

        // SCRUB HiddenTabBar the same way, on every node under the dockspace: a
        // tab bar hidden through the now-removed arrow menu in an earlier session
        // is a SAVED flag too, and with the menu gone there would be no way back.
        // Floating windows are separate root nodes, not under this dockspace, so
        // their single-window look is untouched.
        if (ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId))
        {
            std::vector<ImGuiDockNode*> stack{ root };
            while (!stack.empty())
            {
                ImGuiDockNode* n = stack.back();
                stack.pop_back();
                n->LocalFlags &= ~ImGuiDockNodeFlags_HiddenTabBar;
                if (n->ChildNodes[0]) stack.push_back(n->ChildNodes[0]);
                if (n->ChildNodes[1]) stack.push_back(n->ChildNodes[1]);
            }
        }

        ImGui::End();
        return result;
    }

    ToolbarResult DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                                     Arcane::PluginHost* host, PlayLaunchMode& mode,
                                     uint64_t logoTex, const ToolbarStatus& status,
                                     const std::function<void()>& beforePlay)
    {
        ToolbarResult result;

        // Icon button with a hover tooltip (icons need discoverable labels).
        // `id` is an ImGui ID-only suffix (e.g. "##sim_playstop") appended to the
        // glyph so that two buttons showing the SAME icon (e.g. Play and Resume
        // both render ICON_LC_PLAY) never hash to the same widget ID -- without
        // a unique id, ImGui::Button(icon) collides on the label hash and the
        // button drawn first steals the shared ActiveId out from under the one
        // drawn second, making the second button unclickable.
        auto iconBtn = [](const char* icon, const char* id, const char* tip) -> bool
        {
            const std::string label = std::string(icon) + id;
            const bool clicked = ImGui::Button(label.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return clicked;
        };

        // Fixed transport strip drawn into the CURRENT window (the dockspace host -- call
        // between BeginDockSpace and EndDockSpace). Not its own window: no tab, cannot be
        // docked or moved. The logo mark is a touch TALLER than the transport buttons (a
        // small brand mark reads better with a few more pixels), vertically centered on the
        // button row -- so the top/bottom padding also absorbs its overhang above/below.
        const ImGuiStyle& st = ImGui::GetStyle();
        const float btnH     = ImGui::GetFrameHeight();
        const EditorUiToolbarSettings& toolbar = Arcane::Settings<EditorUiToolbarSettings>();   // editor.ui.toolbar.*
        const float logoH    = (logoTex != 0) ? std::floor(btnH * toolbar.logoScale) : btnH;
        const float overhang = (logoH - btnH) * 0.5f;   // logo extends this far above/below the row
        ImGui::Dummy(ImVec2(0.0f, Ui::Px(3.0f) + overhang));

        // Measure the full strip at the button-row start so the transport centers in the
        // whole width (not the width left of the logo).
        const float fullContentW = ImGui::GetContentRegionAvail().x;
        const float lineStartX   = ImGui::GetCursorPosX();
        const float rowY         = ImGui::GetCursorPosY();

        // -- LEFT cluster (Unity-style branding): [pad] [logo] [wordmark], inset from the
        // window edge and vertically centered on the button row. Absolute SetCursorPos so it
        // never perturbs the transport's own placement. leftX tracks the running right edge.
        const float leftPad = Ui::Px(8.0f);
        float leftX = lineStartX + leftPad;
        if (logoTex != 0)
        {
            ImGui::SetCursorPos(ImVec2(leftX, rowY - overhang));
            ImGui::Image((ImTextureID)logoTex, ImVec2(logoH, logoH));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Arcane");
            leftX += logoH + Ui::Px(8.0f);   // gap between logo and wordmark
        }
        if (ImFont* brand = GetEditorFonts().brand)
        {
            // "Arcane" wordmark in the display face, a touch shorter than the logo, centered
            // on the same row. PushFont(font, size) renders it at a display size (ImGui 1.92).
            const float brandSize = std::floor(logoH * toolbar.brandScale);
            ImGui::PushFont(brand, brandSize);
            const ImVec2 sz = ImGui::CalcTextSize("Arcane");
            ImGui::SetCursorPos(ImVec2(leftX, rowY + (btnH - sz.y) * 0.5f));
            ImGui::TextUnformatted("Arcane");
            ImGui::PopFont();
            leftX += sz.x;
        }

        // -- CENTER: transport, Unity-style toggles: [Play|mode] Pause Step. Center the
        // group within the full toolbar width, placed on the button row -- but never
        // behind the left cluster (clamp on narrow windows). Undo/Redo moved to the Edit
        // menu; the transform-gizmo tools moved to the Viewport overlay.
        //
        // The play-mode chevron is FUSED onto Play as one split control rather than
        // standing alone: it configures Play, but sitting after Step it read as Step's.
        // Same shape Unreal uses for its play-mode dropdown.
        //
        // kTransportGap overrides ImGui's default ItemSpacing.x (8) for this group only.
        // At 8 the buttons read as four separate objects instead of one instrument; the
        // group is an instrument. Everything else on the row keeps the global spacing,
        // which is why this is a scoped PushStyleVar and not a style change.
        const float kTransportGap = Ui::Px(2.0f);
        const float kCaretPadX    = Ui::Px(2.0f);   // the caret half is slimmer than a full icon button
        auto btnW = [&](const char* icon)
        { return ImGui::CalcTextSize(icon).x + st.FramePadding.x * 2.0f; };
        // The split's two halves overlap by one border so the shared edge is a single
        // 1px line, not two stacked -- hence the -FrameBorderSize, mirrored in the draw
        // below. Miss it here and the group centers a pixel off.
        const float caretW  = ImGui::CalcTextSize(ICON_LC_CHEVRON_DOWN).x + kCaretPadX * 2.0f;
        const float splitW  = btnW(ICON_LC_PLAY) + caretW - st.FrameBorderSize;
        const float transportW = splitW + btnW(ICON_LC_PAUSE) + btnW(ICON_LC_STEP_FORWARD)
                               + kTransportGap * 2.0f;
        const float centerStart = lineStartX + (fullContentW - transportW) * 0.5f;
        const float transportX = std::max(centerStart, leftX + Ui::Px(12.0f));
        ImGui::SetCursorPos(ImVec2(transportX, rowY));

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kTransportGap, st.ItemSpacing.y));

        // Play/Stop toggle: the glyph SWAPS to a stop square while playing (user
        // ruling 2026-08-10, reversing the icon-font arc's tint-only Unity model)
        // and stays tinted, so the lit square reads unambiguously as "this stops".
        // The swap/tint is PIE-ONLY: a SeparateWindow launch is fire-and-forget
        // (the editor takes no lock on the child and does not track it), so it
        // never lights this button -- Stop would have nothing of its own to
        // restore for a launch it never tracked.
        const bool playing = play.IsPlaying();
        // IconToggle (s4.9/s6.2): lit = the accent trio, so a playing Stop square
        // reads as ON under the cursor too. Its tooltip stays this site's own.
        const bool playClicked =
            IconToggle(playing ? ICON_LC_SQUARE "##sim_play" : ICON_LC_PLAY "##sim_play", playing);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", playing ? "Stop" : "Play");
        if (playClicked)
        {
            if (playing)
            {
                // ONE Stop for every topology -- PlaySession tears down whatever
                // it stood up (the embedded server world detaches here). The
                // SEPARATE server PROCESS is not this object's to kill; the caller
                // stops it right after, keyed on the Play->Edit flip.
                // A refused Stop stays in Play: the caller pushes the same error
                // modal the menu / command Stop use (EditorApp::PushPlayStopFailedModal).
                if (!play.Stop(runtime, host))
                    result.stopFailed = true;
            }
            else
            {
                // A REFUSED Play IS REPORTED, not just logged (final-review fix
                // wave, minor 11). PlaySession returns false for a failed
                // snapshot, a failed restore into the embedded server world, or
                // a PluginHost that refused to attach it -- all of which leave
                // the session in Edit, so the ONLY thing the user sees is a Play
                // button that did not light. Publish one Problems row, keyed
                // "editor:play" (this is its sole publisher; the next
                // successful Play's Clear retracts it, and that Clear is
                // EditorApp::AfterPlayFlip's, the one flip rule servitor play
                // shares, so a Play from either side retracts the row).
                const auto reportRefusal = [](const char* what)
                {
                    Arcane::Diagnostic d;
                    d.severity = Arcane::DiagSeverity::Warning;
                    d.scope    = Arcane::DiagScope::Scene;
                    d.code     = "play.refused";
                    d.message  = std::string("Play was refused (") + what + ")";
                    d.detail   = "The scene snapshot failed, or the plugin host refused the "
                                 "server world. The editor stayed in Edit mode; see the Console.";
                    const std::vector<Arcane::Diagnostic> rows{std::move(d)};
                    Arcane::Diagnostics::Publish("editor:play", rows);
                };
                const auto tryPlay = [&](PlayTopology topology, const char* what)
                {
                    if (beforePlay) beforePlay();   // flush document gestures while still in Edit (s3.3b)
                    if (!play.Play(runtime, host, topology))
                        reportRefusal(what);   // success: the app's AfterPlayFlip clears "editor:play"
                };
                switch (mode)
                {
                    case PlayLaunchMode::Viewport:
                        tryPlay(PlayTopology::Standalone, "in viewport");
                        break;
                    case PlayLaunchMode::SeparateWindow:
                        result.launchStandalone = true;   // caller resolves + spawns; play/plugin untouched
                        break;
                    case PlayLaunchMode::ListenServer:
                        tryPlay(PlayTopology::ListenServer, "listen server");
                        break;
                    case PlayLaunchMode::EmbeddedServer:
                        tryPlay(PlayTopology::EmbeddedServer, "embedded server");
                        break;
                    case PlayLaunchMode::SeparateServerProcess:
                        // BOTH halves, and in this order: the viewport world enters
                        // Play as a CLIENT here, and the caller spawns the authority
                        // (ArcaneServer.exe) for it. A Play that refuses leaves the
                        // request standing anyway -- the caller's own spawn refusal
                        // path is the one that reports, and a spawned server with no
                        // client is still stoppable, whereas a silently skipped spawn
                        // would leave the picker looking like it did nothing. That
                        // combination is UNREACHABLE today: ClientOnly only re-roles
                        // the one world, and the only `return false` before that point
                        // is a failed snapshot of a world the editor is already
                        // holding -- the row below exists for a future refusal, not an
                        // observed one.
                        tryPlay(PlayTopology::ClientOnly, "client + separate server");
                        result.launchServer = true;
                        break;
                }
            }
        }

        // The split's second half. Zero spacing, then back up one border width so the
        // two frames share an edge instead of showing a 2px seam. It carries Play's lit
        // state so the pair reads as ONE control while playing -- an untinted caret
        // welded to a tinted Play would look like a separate button again, which is the
        // whole thing this layout is fixing.
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - st.FrameBorderSize);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kCaretPadX, st.FramePadding.y));
        // Lit with Play (an unlit caret welded to a lit Play reads as a separate button).
        const bool caretClicked = IconToggle(ICON_LC_CHEVRON_DOWN "##sim_playmode", playing);
        const PopupAnchor caretAnchor = LastItemAnchor();   // T2-C2's BeginPopupBelow("##play_mode", caretAnchor) reads it
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play mode");
        if (caretClicked)
            ImGui::OpenPopup("##play_mode");
        ImGui::PopStyleVar();

        // Play-mode dropdown (Task 6, runtime-host-fold arc): choose whether the Play
        // button this hangs off enters PIE ("In viewport", today's behavior) or spawns a
        // standalone ArcaneRuntime window on the active scene ("Separate window",
        // fire-and-forget). Those two rows WERE the future SERVER-SET seam, and the
        // Core-DLL split's Task 7 is that future arriving: the three rows below are
        // the network TOPOLOGIES, landing here as rows rather than as a separate
        // UI-only concept bolted on elsewhere, exactly as promised.
        if (BeginPopupBelow("##play_mode", caretAnchor))
        {
            // MarkIniSettingsDirty on change, as the shader editor's layout
            // handler does (ShaderEditorDocument.cpp): ImGui::Shutdown saves the
            // ini on a clean exit anyway, so without this the setting survives a
            // normal quit but is LOST if the editor is killed or crashes.
            if (ImGui::MenuItem("In viewport", nullptr, mode == PlayLaunchMode::Viewport))
            {
                mode = PlayLaunchMode::Viewport;
                ImGui::MarkIniSettingsDirty();
            }
            if (ImGui::MenuItem("Separate window", nullptr, mode == PlayLaunchMode::SeparateWindow))
            {
                mode = PlayLaunchMode::SeparateWindow;
                ImGui::MarkIniSettingsDirty();
            }
            ImGui::Separator();
            // The topology rows (spec s7). The first two stay IN the viewport --
            // they are PlaySession topologies, one world and two worlds
            // respectively -- while the third is the viewport world as a pure
            // client with the authority in a spawned ArcaneServer.exe.
            if (ImGui::MenuItem("Listen server (in viewport)", nullptr,
                                mode == PlayLaunchMode::ListenServer))
            {
                mode = PlayLaunchMode::ListenServer;
                ImGui::MarkIniSettingsDirty();
            }
            if (ImGui::MenuItem("Client + embedded server (in viewport)", nullptr,
                                mode == PlayLaunchMode::EmbeddedServer))
            {
                mode = PlayLaunchMode::EmbeddedServer;
                ImGui::MarkIniSettingsDirty();
            }
            if (ImGui::MenuItem("Client + separate server process", nullptr,
                                mode == PlayLaunchMode::SeparateServerProcess))
            {
                mode = PlayLaunchMode::SeparateServerProcess;
                ImGui::MarkIniSettingsDirty();
            }
            ImGui::EndPopup();
        }

        // Re-fetch AFTER a possible Stop -- Runtime::RestoreRegistry destroys and
        // replaces m_impl->loop on Stop, so a reference taken before this dangles.
        // (Nothing between that Stop and here touches `loop`, so fetching here is safe.)
        Arcane::RunLoop& loop = runtime.Loop();
        ImGui::SameLine();
        // Pause/Resume toggle (Unity-style): ALWAYS the pause glyph, tinted while paused.
        // Disabled OUTSIDE Play: the loop's pause IS the Edit-mode sim freeze
        // (PlaySession::Stop pauses it, Play unpauses it), so toggling it in Edit would
        // run the sim with no active Play session and no way to Stop. Pause only within
        // Play; tint only while actually playing so it never looks "armed" in Edit.
        ImGui::BeginDisabled(!play.IsPlaying());
        const bool pauseClicked = IconToggle(ICON_LC_PAUSE "##sim_pause", play.IsPlaying() && loop.IsPaused());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", loop.IsPaused() ? "Resume" : "Pause");
        if (pauseClicked)
            loop.SetPaused(!loop.IsPaused());
        ImGui::EndDisabled();
        ImGui::SameLine();
        // Step: momentary; disabled outside Play (single-step is meaningless in Edit),
        // matching Unity's greyed-out Step.
        ImGui::BeginDisabled(!play.IsPlaying());
        if (iconBtn(ICON_LC_STEP_FORWARD, "##sim_step", "Step")) loop.RequestSingleStep();
        ImGui::EndDisabled();

        ImGui::PopStyleVar();   // kTransportGap: the rest of the row keeps the global spacing

        // -- RIGHT cluster (node page phase s6.4/s6.5): [chip] [status], laid out
        // right to left by LayoutStripCluster, absolute SetCursorPos like the left
        // cluster. The cursor is restored afterwards so the closing Dummy -- and
        // with it the strip height -- is exactly where the transport left it.
        {
            const ImVec2 afterTransport = ImGui::GetCursorPos();
            const float  rightEdge = lineStartX + fullContentW - leftPad;          // mirrors leftPad
            const float  minX      = transportX + transportW + Ui::Px(12.0f);             // mirrors the left clamp
            const float  gap       = st.ItemSpacing.x * 2.0f;
            const float  lineH     = ImGui::GetTextLineHeight();
            const StripStatusMetrics m = MeasureStripStatus(status.title);
            const float  chipW = status.problems
                ? ImGui::CalcTextSize(status.problems->label.c_str()).x + st.FramePadding.x * 2.0f : 0.0f;
            const StripClusterLayout lay = LayoutStripCluster(minX, rightEdge, chipW, m.naturalW, m.minW, gap);
            if (status.problems)
            {
                const StripChip& chip = *status.problems;
                ImGui::SetCursorPos(ImVec2(lay.chipX, rowY));
                result.problemsChipClicked = ImGui::InvisibleButton("##strip_problems", ImVec2(chipW, btnH));
                const ImVec2 cmin = ImGui::GetItemRectMin(), cmax = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(cmin, cmax, ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
                                                                                          : ImGuiCol_Button));
                dl->AddRect(cmin, cmax, ImGui::GetColorU32(ImGuiCol_Border));
                dl->AddText(ImVec2(cmin.x + st.FramePadding.x, cmin.y + (btnH - lineH) * 0.5f),
                            ImGui::GetColorU32(chip.color), chip.label.c_str());
                if (!chip.tooltip.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", chip.tooltip.c_str());
            }
            if (lay.drawStatus)
                DrawStripStatus(status, ImVec2(lay.statusX, rowY + (btnH - lineH) * 0.5f), lay.statusBudget);
            ImGui::SetCursorPos(afterTransport);
        }

        ImGui::Dummy(ImVec2(0.0f, Ui::Px(3.0f) + overhang));   // clear the logo's lower overhang too
        ImGui::Separator();
        return result;
    }

    void DrawConsolePanel(ConsoleBuffer& console, ConsoleUiState& ui, bool suppressBadges, bool* open)
    {
        // Snapshot once: CollapseConsole holds pointers into its input, and a
        // worker thread can push (and therefore evict) mid-frame. Snapshot
        // BEFORE Begin: the tab title carries the unseen count (s8.2).
        const std::vector<ConsoleEntry> entries = console.Snapshot();
        // editor.console.* (settings sweep S6-41): the toolbar toggles are cvars;
        // a click writes the User rung (visible from the next publish), and this
        // frame draws with the clicked value.
        const EditorConsoleSettings consoleSettings = Settings<EditorConsoleSettings>();   // a copy: held across the draw
        bool collapse   = consoleSettings.collapse;
        bool autoScroll = consoleSettings.autoScroll;
        bool wrap       = consoleSettings.wrap;
        const std::size_t unseen = suppressBadges ? 0 : UnseenAlerts(entries, ui.lastSeenSeq);
        const bool tint = unseen > 0;
        if (tint)
            ImGui::PushStyleColor(ImGuiCol_Text, UnseenErrors(entries, ui.lastSeenSeq) > 0 ? Theme::kError : Theme::kWarning);
        ImGui::Begin(ConsoleTabTitle(unseen).c_str(), open);
        if (tint) ImGui::PopStyleColor();   // only the tab label is tinted
        // "Shown" is the window's own Hidden flag, not BeginChild's return:
        // a new window's first frame is Hidden yet does NOT skip items
        // (HiddenFramesCannotSkipItems), so the rows child still opens and
        // would mark every startup warning seen before anyone could look.
        const bool shown = !ImGui::GetCurrentWindowRead()->Hidden;

        std::size_t nInfo = 0, nWarn = 0, nErr = 0;
        for (const ConsoleEntry& e : entries)
        {
            if (e.level == Arcane::DiagSeverity::Error)        ++nErr;
            else if (e.level == Arcane::DiagSeverity::Warning) ++nWarn;
            else                                               ++nInfo;
        }

        // Row selection (UE model: the output log body is a READ-ONLY TEXT BOX
        // -- SMultiLineEditableTextBox with IsReadOnly, SOutputLog.cpp:1221-1228
        // -- so its text is selectable and Ctrl+C copies the selection; the
        // context menu is the text box's Copy/Select All extended with Clear
        // Log via ExtendTextBoxMenu). ImGui has no colored-text widget with
        // character selection, so the mapping here is LINE-granular: rows are
        // multi-selectable (drag box-select, Shift/Ctrl click, Ctrl+A) and
        // copy reproduces the drawn rows via FormatConsoleRow.
        //
        // A panel-local static rather than a ConsoleUiState member: exactly
        // one Console panel exists per process (EditorApp is one-per-process),
        // and this keeps ImGuiSelectionBasicStorage -- an imgui.h type -- out
        // of EditorPanels.hpp, which is deliberately ImGui-free.
        static ImGuiSelectionBasicStorage s_selection;

        bool wantCopyAll = false;   // resolved below, once the visible rows exist

        if (ImGui::Button("Clear"))
        {
            console.Clear();
            s_selection.Clear();
        }
        ImGui::SameLine();
        // Copies the rows AS DRAWN (filters + collapse + the same text the
        // selection copy produces) -- it used to dump every raw message, which
        // made the button and a select-all copy disagree about the same panel.
        // The label flashes "Copied" until the deadline the copy handler below
        // set: "###" keeps the widget id identical across the swap (id from
        // the suffix alone, so hover/active state survives), and the width is
        // fixed at the LONGER label's so the toolbar never shifts mid-flash.
        const bool copyFlash = ImGui::GetTime() < ui.copyFlashUntil;
        const float copyW = ImGui::CalcTextSize("Copied").x +
                            ImGui::GetStyle().FramePadding.x * 2.0f;
        wantCopyAll = ImGui::Button(copyFlash ? "Copied###consolecopy"
                                              : "Copy###consolecopy",
                                    ImVec2(copyW, 0.0f));
        ImGui::SameLine();
        if (ImGui::Checkbox("Collapse", &collapse)) SetConsoleToggle(kConsoleCollapseCVar, collapse);
        ImGui::SameLine();
        if (ImGui::Checkbox("Scroll", &autoScroll)) SetConsoleToggle(kConsoleAutoScrollCVar, autoScroll);
        ImGui::SameLine();
        if (ImGui::Checkbox("Wrap", &wrap)) SetConsoleToggle(kConsoleWrapCVar, wrap);

        ImGui::SameLine(); (void)SeverityToggleFor("console_err",  Arcane::DiagSeverity::Error,   nErr,  ui.showError);
        ImGui::SameLine(); (void)SeverityToggleFor("console_warn", Arcane::DiagSeverity::Warning, nWarn, ui.showWarning);
        ImGui::SameLine(); (void)SeverityToggleFor("console_info", Arcane::DiagSeverity::Info,    nInfo, ui.showInfo);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("##consolecat", ui.categoryFilter.empty() ? "All categories" : ui.categoryFilter.c_str()))
        {
            if (ImGui::Selectable("All categories", ui.categoryFilter.empty())) ui.categoryFilter.clear();
            std::vector<std::string> cats;
            for (const ConsoleEntry& e : entries)
                if (std::find(cats.begin(), cats.end(), e.category) == cats.end()) cats.push_back(e.category);
            std::sort(cats.begin(), cats.end());
            for (const std::string& c : cats)
                if (ImGui::Selectable(c.c_str(), ui.categoryFilter == c)) ui.categoryFilter = c;
            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##consolesearch", "Search", ui.search, sizeof(ui.search));

        ImGui::Separator();
        // The gate matters, not just politeness: when the Console is a HIDDEN
        // dock tab this child is SkipItems, where Selectable() early-returns
        // BEFORE ItemAdd (imgui_widgets.cpp:7370-ish) -- but
        // SetNextItemSelectionUserData() arms g.NextItemData UNCONDITIONALLY
        // (imgui_widgets.cpp:8227) and only ItemAdd clears it (imgui.cpp:
        // 12015). Run the multi-select body anyway and the armed
        // IsMultiSelect flag leaks to the first REAL item drawn later in the
        // frame; if that item is a Selectable/TreeNode (an Outliner row), it
        // calls MultiSelectItemHeader with g.CurrentMultiSelect == null and
        // crashes (imgui_widgets.cpp:7453 -> 8249). Frame ONE of any launch
        // where another tab covers the Console hits this. So: no body in a
        // skipped child, period.
        // The cvar console's own history sits between the log rows and the
        // input line: reserve the input line plus up to editor.console.replyLines
        // reply lines. (Hygiene pass 2026-09-28: the replies were never drawn here.)
        const std::size_t replyCap = static_cast<std::size_t>(consoleSettings.replyLines > 0 ? consoleSettings.replyLines : 0);
        const std::size_t cvarLines = ui.cvars.Lines().size() < replyCap ? ui.cvars.Lines().size() : replyCap;
        const float reserved = ImGui::GetFrameHeightWithSpacing() * (1.0f + static_cast<float>(cvarLines));
        if (!ImGui::BeginChild("##consolerows", ImVec2(0.0f, -reserved)))
        {
            ImGui::EndChild();   // always called -- BeginChild's contract, unlike Begin's
            ImGui::End();
            return;
        }
        if (shown && !entries.empty()) ui.lastSeenSeq = entries.back().seq;   // the badge clears next frame

        const auto visible = [&](const ConsoleEntry& e)
        {
            if (e.level == Arcane::DiagSeverity::Error   && !ui.showError)   return false;
            if (e.level == Arcane::DiagSeverity::Warning && !ui.showWarning) return false;
            if (e.level == Arcane::DiagSeverity::Info    && !ui.showInfo)    return false;
            if (!ui.categoryFilter.empty() && e.category != ui.categoryFilter) return false;
            Arcane::Diagnostic probe;               // reuse the one filter definition
            probe.severity = e.level;
            probe.message  = e.message;
            return MatchesDiagnosticFilter(probe, SeverityMask::All, ui.search);
        };

        // The visible rows, resolved BEFORE the multi-select scope opens:
        // BeginMultiSelect wants the item count up front, and the draw loop,
        // Ctrl+A, and both copy paths must all agree on one display-ordered
        // list. Pointers reach into `entries` (the frame's snapshot), which
        // outlives every use below.
        struct Row { const ConsoleEntry* e; std::size_t count; };
        std::vector<Row> rows;
        if (collapse)
        {
            for (const CollapsedRow& r : CollapseConsole(entries))
                if (r.first && visible(*r.first)) rows.push_back({ r.first, r.count });
        }
        else
        {
            rows.reserve(entries.size());
            for (const ConsoleEntry& e : entries)
                if (visible(e)) rows.push_back({ &e, 1 });
        }

        // Rows in display order -> one clipboard string (FormatConsoleRow is
        // the same text the rows draw, so copy is WYSIWYG). Returns whether
        // anything actually reached the clipboard.
        const auto copyRows = [&](bool selectedOnly) -> bool
        {
            std::string text;
            for (const Row& r : rows)
                if (!selectedOnly || s_selection.Contains(static_cast<ImGuiID>(r.e->seq)))
                {
                    text += FormatConsoleRow(*r.e, r.count);
                    text += '\n';
                }
            if (text.empty())
                return false;
            ImGui::SetClipboardText(text.c_str());
            return true;
        };
        // The "Copied" flash arms only when the copy actually LANDED -- an
        // empty console (or a filter that hides everything) writes nothing to
        // the clipboard and must not claim otherwise.
        if (wantCopyAll && copyRows(false))
            ui.copyFlashUntil = ImGui::GetTime() + Arcane::Settings<Arcane::UiSettings>().copyFlashSeconds;

        // Selection ids are entry seqs, not row indices: the ring evicts from
        // the front and the filter hides rows, so an index would name a
        // different line every frame; a seq follows its line for life
        // (ConsoleEntry::seq).
        ImGuiMultiSelectFlags msFlags = ImGuiMultiSelectFlags_ClearOnEscape |
                                        ImGuiMultiSelectFlags_ClearOnClickVoid |
                                        ImGuiMultiSelectFlags_BoxSelect1d;
        ImGuiMultiSelectIO* ms =
            ImGui::BeginMultiSelect(msFlags, s_selection.Size, static_cast<int>(rows.size()));
        s_selection.UserData = &rows;
        s_selection.AdapterIndexToStorageId =
            [](ImGuiSelectionBasicStorage* self, int idx) -> ImGuiID
        {
            const auto& r = *static_cast<const std::vector<Row>*>(self->UserData);
            return static_cast<ImGuiID>(r[static_cast<std::size_t>(idx)].e->seq);
        };
        s_selection.ApplyRequests(ms);

        const float lineH = ImGui::GetTextLineHeight();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            const Row& row = rows[static_cast<std::size_t>(i)];
            const ConsoleEntry& e = *row.e;

            ARC_CONSTANT("console info grey; unify with editor.theme.text in the axis re-bless task")
            ImVec4 col(0.80f, 0.80f, 0.80f, 1.0f);
            if (e.level == Arcane::DiagSeverity::Error)        col = Theme::kError;
            else if (e.level == Arcane::DiagSeverity::Warning) col = Theme::kWarning;

            const std::string clock = ClockText(e.timestampMs);
            char cat[64];
            std::snprintf(cat, sizeof(cat), "%-*s", static_cast<int>(consoleSettings.categoryWidth), e.category.c_str());
            std::string msg = e.message;
            if (row.count > 1)
            {
                msg += "  (x";
                msg += std::to_string(row.count);
                msg += ')';
            }

            // The whole row is ONE full-width Selectable, sized up front so a
            // wrapped message stays clickable on every visual line -- the text
            // is then drawn OVER it (the standard rewind trick; Text items
            // carry no ID, so the Selectable keeps every click).
            float rowH = lineH;
            const float spacingX = ImGui::GetStyle().ItemSpacing.x;
            const float prefixW  = ImGui::CalcTextSize(clock.c_str()).x + spacingX +
                                   ImGui::CalcTextSize(cat).x + spacingX;
            if (wrap)
            {
                const float wrapW = ImGui::GetContentRegionAvail().x - prefixW;
                if (wrapW > 1.0f)
                    rowH = std::max(rowH,
                                    ImGui::CalcTextSize(msg.c_str(), nullptr, false, wrapW).y);
            }

            ImGui::PushID(i);
            ImGui::SetNextItemSelectionUserData(i);
            const bool selected = s_selection.Contains(static_cast<ImGuiID>(e.seq));
            const ImVec2 rowPos = ImGui::GetCursorPos();
            ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(0.0f, rowH));
            const ImVec2 afterPos = ImGui::GetCursorPos();

            // Timestamp and category are dimmed prefixes so the message stays
            // the thing the eye lands on.
            ImGui::SetCursorPos(rowPos);
            ImGui::TextDisabled("%s", clock.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", cat);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (wrap) ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(msg.c_str());
            if (wrap) ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::SetCursorPos(afterPos);
            ImGui::PopID();
        }

        // The LAST row's trailing SetCursorPos(afterPos) has no item after it
        // -- every other row's is consumed by the next row's Selectable -- and
        // ImGui ASSERTS at EndChild when a raw SetCursorPos is left extending
        // the window (ErrorCheckUsingSetCursorPosToExtendParentBoundaries,
        // imgui.cpp:11544, whose message says to do exactly this). This was
        // the launch-crash on 2026-08-10, presenting as a zero-trace abort:
        // CRT assert box -> Abort -> exit(3), no dump, no report.
        if (!rows.empty())
            ImGui::Dummy(ImVec2(0.0f, 0.0f));

        ms = ImGui::EndMultiSelect();
        s_selection.ApplyRequests(ms);

        // Ctrl+C copies the selection while the log child has focus (settings
        // S4: console.copy). Ctrl+A stays BeginMultiSelect's own.
        if (ImGui::IsWindowFocused()) EditorActions::Get().MarkContextActive(ActionContext::Console);
        if (s_selection.Size > 0 && ImGui::IsWindowFocused() && EditorActions::Get().Pressed("console.copy"))
            copyRows(true);

        // UE parity: the read-only text box's Copy/Select All, extended with
        // Clear Log (SOutputLog::ExtendTextBoxMenu).
        if (ImGui::BeginPopupContextWindow("##consolectx"))
        {
            if (ImGui::MenuItem("Copy", MenuKey("console.copy").c_str(), false, s_selection.Size > 0))
                copyRows(true);
            if (ImGui::MenuItem("Copy All"))
                copyRows(false);
            if (ImGui::MenuItem("Select All", "Ctrl+A"))
            {
                s_selection.Clear();
                for (const Row& r : rows)
                    s_selection.SetItemSelected(static_cast<ImGuiID>(r.e->seq), true);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Log"))
            {
                console.Clear();
                s_selection.Clear();
            }
            ImGui::EndPopup();
        }

        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        if (cvarLines > 0)
        {
            const auto& lines = ui.cvars.Lines();
            for (std::size_t i = lines.size() - cvarLines; i < lines.size(); ++i)
            {
                if (lines[i].ok) ImGui::TextUnformatted(lines[i].text.c_str());
                else
                {
                    ARC_CONSTANT("console error-detail red; unify with editor.theme.error in the axis re-bless task")
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", lines[i].text.c_str());
                }
            }
        }
        (void)Arcane::DrawConsoleInputLine("##cvarline", ui.cvars, Arcane::CVarRegistry::Get(), Arcane::CVarContext::Editor);
        ImGui::End();
    }

    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH,
                                          ViewportToolState& tools, const ViewportChrome& chrome,
                                          const ViewportImageOverlayFn& imageOverlay)
    {
        // The style alpha OUTSIDE any BeginDisabled scope, captured up front:
        // BeginDisabled multiplies g.Style.Alpha (imgui.cpp:8899-8900) and a
        // tooltip Begin()s under whatever alpha is current, so a greyed
        // button's tooltip would itself come out at the disabled alpha. The
        // helpers push this value back around SetTooltip so the explanation
        // of WHY a tool is greyed is drawn at full strength.
        const float tooltipAlpha = ImGui::GetStyle().Alpha;
        auto tooltip = [tooltipAlpha](const char* tip)
        {
            // AllowWhenDisabled: the disabled Move/Rotate/Scale buttons still
            // explain themselves on hover (imgui.cpp:4971 masks plain hover).
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, tooltipAlpha);
            ImGui::SetTooltip("%s", tip);
            ImGui::PopStyleVar();
        };
        // Stateless icon-button helper (mirrors the toolbar's).
        auto iconBtn = [&tooltip](const char* icon, const char* id, const char* tip) -> bool
        {
            const bool clicked = ImGui::Button((std::string(icon) + id).c_str());
            tooltip(tip);
            return clicked;
        };

        ViewportPanelResult r;
        // The scene's unsaved dot on the Viewport tab (s6.4), beside the
        // documents' own (every document raises UnsavedDocument when dirty).
        // ImGui widens a marked tab by one glyph (imgui_widgets.cpp:10052-10053),
        // so tabs to its right shift while the scene is dirty.
        ImGui::Begin("Viewport", nullptr,
                     chrome.sceneDirty ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None);
        r.dockId = static_cast<unsigned int>(ImGui::GetWindowDockID());
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        r.desiredW = avail.x > 0 ? static_cast<uint32_t>(avail.x) : 1;
        r.desiredH = avail.y > 0 ? static_cast<uint32_t>(avail.y) : 1;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (textureId != 0 && texW > 0 && texH > 0)
            ImGui::Image((ImTextureID)textureId, ImVec2((float)texW, (float)texH));
        r.imageRect = ViewportRect{ origin.x, origin.y, (float)texW, (float)texH };
        // The FOREGROUND overlay (the gizmo): over the image, under the tool
        // buttons drawn below, clipped to the image so nothing leaks into the
        // window chrome.
        if (imageOverlay && textureId != 0 && texW > 0 && texH > 0)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(origin, ImVec2(origin.x + (float)texW, origin.y + (float)texH), true);
            imageOverlay(*dl, origin);
            dl->PopClipRect();
        }
        // Play presence (s6.3): four 2 px kAccent bands INSIDE the image rect --
        // inside, so there is no half-pixel stroke arithmetic and no bleed into the
        // window padding. After the gizmo overlay, before the tool overlay (hidden
        // in Play anyway). Draw only: r.hovered, the click capture below and game
        // input are untouched. `playing` is PlaySession::IsPlaying(), so every
        // in-editor topology frames and a Separate-window launch never does.
        if (chrome.playing && textureId != 0 && texW > 0 && texH > 0)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32  col = ImGui::GetColorU32(Theme::kAccent);
            ARC_CONSTANT("base px; drawn as Ui::Px(base) (s16.11)")
            constexpr float kPlayBandPx = 2.0f;
            const float t = Ui::Px(kPlayBandPx);
            const ImVec2 a = origin;
            const ImVec2 b(origin.x + (float)texW, origin.y + (float)texH);
            dl->AddRectFilled(a, ImVec2(b.x, a.y + t), col);                          // top
            dl->AddRectFilled(ImVec2(a.x, b.y - t), b, col);                          // bottom
            dl->AddRectFilled(ImVec2(a.x, a.y + t), ImVec2(a.x + t, b.y - t), col);   // left
            dl->AddRectFilled(ImVec2(b.x - t, a.y + t), ImVec2(b.x, b.y - t), col);   // right
        }
        r.hovered = ImGui::IsWindowHovered();
        r.focused = ImGui::IsWindowFocused();

        // UE5-style tool overlay at the top-right of the viewport image, two
        // groups on one row (F4 plan 1 T8, spec s6):
        //   [2D | Persp] [gear]   [Select] [Move] [Rotate] [Scale] [Local/World]
        // The view control on the LEFT: a two-segment toggle for the editor
        // camera's ViewMode (the same assignment Alt+J / Alt+G make) and a
        // gear opening the view-settings popup (grid, grid plane, fov, camera
        // speed, gizmo size). The transform tools on the right as before.
        // Drawn over the image; a click on it changes the tool/view and must
        // NOT also pick an entity. Hidden in Play mode (chrome.showToolOverlay=false):
        // the game owns the viewport there, and with the overlay gone
        // overlayHovered stays false, so clicks in that corner fall through to
        // the game like anywhere else.
        bool overlayHovered = false;
        if (chrome.showToolOverlay)
        {
            using Arcane::Editor::ViewMode;
            using Arcane::Editor::GridPlane;
            bool&               gizmoEnabled = tools.gizmoEnabled;
            Arcane::GizmoMode&  mode         = tools.mode;
            Arcane::GizmoSpace& space        = tools.space;

            const ImGuiStyle& st = ImGui::GetStyle();
            auto bw = [&](const char* ic){ return ImGui::CalcTextSize(ic).x + st.FramePadding.x * 2.0f; };
            // Eight buttons; the gap between the two groups is three item
            // spacings (the other six joints are one each), so the row's
            // right edge lands `pad` from the image's regardless of the font.
            const float groupGap = st.ItemSpacing.x * 3.0f;
            const float totalW = bw(ICON_LC_SQUARE) + bw(ICON_LC_BOX) + bw(ICON_LC_SETTINGS_2)
                               + bw(ICON_LC_MOUSE_POINTER_2) + bw(ICON_LC_MOVE) + bw(ICON_LC_ROTATE_3D)
                               + bw(ICON_LC_SCALE_3D) + bw(ICON_LC_BOX)
                               + st.ItemSpacing.x * 6.0f + groupGap;
            const float pad = 8.0f;
            ImGui::SetCursorScreenPos(ImVec2(origin.x + (float)texW - totalW - pad, origin.y + pad));
            ImGui::BeginGroup();

            // --- View control: 2D | Persp + the settings gear ----------------
            // MarkIniSettingsDirty on every view-mode edit here (the popup's
            // preferences are cvars), as the shader editor's do (:716): the [EditorViewport]
            // handler only WRITES when ImGui next saves, and a camera or
            // settings change on its own dirties nothing.
            const bool view2d = IconToggle(ICON_LC_SQUARE "##view_2d", tools.viewMode == ViewMode::TwoD);
            tooltip(WithChord("2D view", "editor.view.ortho2D").c_str());
            if (view2d) { tools.viewMode = ViewMode::TwoD; ImGui::MarkIniSettingsDirty(); }
            ImGui::SameLine();
            const bool viewPersp = IconToggle(ICON_LC_BOX "##view_persp", tools.viewMode == ViewMode::Perspective);
            tooltip(WithChord("Perspective view", "editor.view.perspective").c_str());
            if (viewPersp) { tools.viewMode = ViewMode::Perspective; ImGui::MarkIniSettingsDirty(); }
            ImGui::SameLine();
            if (iconBtn(ICON_LC_SETTINGS_2, "##view_settings", "View settings"))
                ImGui::OpenPopup("##viewsettings");
            // Drop the popup from the gear's bottom edge, right-aligned to it:
            // the row hugs the image's right edge, so a left-anchored popup of
            // this width would run off it. Appearing only -- ImGui keeps the
            // position while it stays open.
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x,
                                           ImGui::GetItemRectMax().y + st.ItemSpacing.y),
                                    ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
            if (ImGui::BeginPopup("##viewsettings"))
            {
                // The preferences are cvars (settings S6-29): each widget edits
                // a copy of the published value and writes the cvar at the User
                // rung on change (SetViewportPref also queues the archive), so
                // nothing here touches imgui.ini any more.
                EditorViewportSettings view = Arcane::Settings<EditorViewportSettings>();
                EditorCameraSettings   cam  = Arcane::Settings<EditorCameraSettings>();
                float gizmoSize             = Arcane::Settings<EditorGizmoSettings>().size;
                ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
                if (ImGui::Checkbox("Show grid", &view.showGrid))
                    SetViewportPref("editor.viewport.showGrid", Arcane::CVarValue::Bool(view.showGrid));
                {
                    int plane = static_cast<int>(view.gridPlane);
                    if (ImGui::Combo("Grid plane", &plane, "XZ (ground)\0XY (2D plane)\0"))
                        SetViewportPref("editor.viewport.gridPlane",
                                        Arcane::CVarValue::Enum(static_cast<std::int32_t>(plane == 1 ? GridPlane::XY : GridPlane::XZ)));
                }
                // AlwaysClamp on every slider: a Ctrl+click typed value past the
                // range is held to the slider's ends, not just the cvar's range.
                if (ImGui::SliderFloat("Field of view", &cam.fovYDeg, 20.0f, 120.0f, "%.0f deg",
                                       ImGuiSliderFlags_AlwaysClamp))
                    SetViewportPref("editor.camera.fovYDeg", Arcane::CVarValue::Float32(cam.fovYDeg));
                const auto [speedLo, speedHi] = CameraSpeedScalarRange();
                if (ImGui::SliderFloat("Camera speed", &cam.speedScalar, speedLo, speedHi, "%.2fx",
                                       ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
                    SetViewportPref("editor.camera.speedScalar", Arcane::CVarValue::Float32(cam.speedScalar));
                if (ImGui::SliderFloat("Gizmo size", &gizmoSize, 0.5f, 3.0f, "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp))
                    SetViewportPref("editor.gizmo.size", Arcane::CVarValue::Float32(gizmoSize));
                ImGui::PopItemWidth();
                ImGui::EndPopup();
            }
            // The popup is a window of its own, so while the cursor is over it
            // r.hovered is already false; this covers the click OUTSIDE it that
            // closes it (processed at EndFrame, imgui.cpp's
            // UpdateMouseMovingWindowEndFrame, so IsPopupOpen is still true
            // here on that frame): dismissing the dropdown must not also pick.
            const bool viewSettingsOpen = ImGui::IsPopupOpen("##viewsettings");

            ImGui::SameLine(0.0f, groupGap);

            // --- Transform tools ---------------------------------------------
            const bool toolSel = IconToggle(ICON_LC_MOUSE_POINTER_2 "##tool_sel", !gizmoEnabled);
            tooltip(WithChord("Select", "editor.viewport.toolSelect").c_str());
            if (toolSel) gizmoEnabled = false;
            ImGui::SameLine();
            const bool toolT = IconToggle(ICON_LC_MOVE_3D "##tool_t", gizmoEnabled && mode == Arcane::GizmoMode::Translate);
            tooltip(WithChord("Move", "editor.viewport.toolTranslate").c_str());
            if (toolT) { gizmoEnabled = true; mode = Arcane::GizmoMode::Translate; }
            ImGui::SameLine();
            const bool toolR = IconToggle(ICON_LC_ROTATE_3D "##tool_r", gizmoEnabled && mode == Arcane::GizmoMode::Rotate);
            tooltip(WithChord("Rotate", "editor.viewport.toolRotate").c_str());
            if (toolR) { gizmoEnabled = true; mode = Arcane::GizmoMode::Rotate; }
            ImGui::SameLine();
            const bool toolS = IconToggle(ICON_LC_SCALE_3D "##tool_s", gizmoEnabled && mode == Arcane::GizmoMode::Scale);
            tooltip(WithChord("Scale", "editor.viewport.toolScale").c_str());
            if (toolS) { gizmoEnabled = true; mode = Arcane::GizmoMode::Scale; }
            ImGui::SameLine();
            {
                const bool local = (space == Arcane::GizmoSpace::Local);
                if (iconBtn(local ? ICON_LC_BOX : ICON_LC_GLOBE, "##tool_space",
                            local ? "Local space" : "World space"))
                    space = local ? Arcane::GizmoSpace::World : Arcane::GizmoSpace::Local;
            }
            ImGui::EndGroup();
            overlayHovered = ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())
                          || viewSettingsOpen;
        }

        // Capture a left-click inside the image, in viewport-local px (origin = image
        // top-left), UNLESS it landed on the tool overlay above. EditorApp unprojects
        // it through the plugin camera and drives the entity pick.
        if (r.hovered && !overlayHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const ImVec2 m = ImGui::GetMousePos();
            const float lx = m.x - origin.x, ly = m.y - origin.y;
            if (lx >= 0 && ly >= 0 && lx < (float)texW && ly < (float)texH)
            {
                r.clicked = true;
                r.altHeld = ImGui::GetIO().KeyAlt;
                r.ctrlHeld = ImGui::GetIO().KeyCtrl;
                r.clickLocalX = lx; r.clickLocalY = ly;
            }
        }
        ImGui::End();
        return r;
    }

    namespace
    {
        // Payload type tag for outliner entity drag/reparent; shared by the
        // source/target sites below so the string only ever appears once.
        constexpr const char* kOutlinerDragType = "ARC_OUTLINER_ENTITY";

        // Whether a structural (whole-registry memento) edit can run RIGHT NOW.
        // Two independent refusals, both of which used to be invisible in the UI:
        //   editMode      -- false during Play (a play-time structural edit would
        //                    be undone against the restored registry).
        //   InTransaction -- ApplyRegistryMutation refuses inside an open gesture
        //                    (Cancel discards pending commands WITHOUT reverting,
        //                    so a memento pushed mid-gesture could strand an
        //                    already-applied edit with no undo coverage).
        // The second one only ARC_WARN'd, which presents at the desk as "the
        // right-click menu stopped working". Every structural affordance disables
        // on this predicate so the refusal is visible before the click.
        [[nodiscard]] bool CanEditStructure(const Arcane::CommandStack& undo,
                                            const SceneEditBinding& b) noexcept
        {
            return b.editMode && !undo.InTransaction();
        }

        // F4 plan 1 Task 11 (spec s8): the `Add 3D Object >` submenu -- the
        // five primitives in CreateAssetDialog.hpp's roster and spelling (the
        // SAME names `Create > Mesh >` offers and the .arcmesh files carry).
        // Latches the pick on OutlinerState for the app (see the field's own
        // comment); nothing is created inside the draw.
        void DrawAddPrimitiveSubmenu(OutlinerState& state, Astra::Entity parent)
        {
            if (!ImGui::BeginMenu("Add 3D Object"))
                return;
            for (const Arcane::MeshSource source : kPrimitiveMeshSources)
            {
                if (ImGui::MenuItem(PrimitiveMeshName(source)))
                {
                    state.addPrimitivePending = static_cast<int>(source);
                    state.addPrimitiveParent  = parent;
                }
            }
            ImGui::EndMenu();
        }
    }

    // `touched` names the entities the edit affects, for the Outliner's
    // unsaved asterisks (CommandStack::TouchedSinceState) -- read AFTER
    // mutate() runs, so creates can append their new ids from inside the
    // lambda (ApplyRegistryMutation's contract).
    bool ApplyStructural(Arcane::CommandStack& undo, const SceneEditBinding& b,
                         std::string label, Arcane::FunctionRef<bool()> mutate,
                         const std::vector<Astra::Entity>* touched)
    {
        if (!b.editMode)
            return false;
        return Arcane::ApplyRegistryMutation(undo, std::move(label),
                                             b.snapshot, b.restore, mutate, touched);
    }

    // `current` is Identity::name RAW -- never Edit::DisplayName, which
    // substitutes "Entity <id>" for an empty name (EntityOps.cpp:49-55).
    // Seeding that fallback made a no-edit commit on an empty-named entity
    // write "Entity 7" into the component; seeding the raw (possibly empty)
    // name keeps Escape and no-edit commits true no-ops.
    void BeginRename(OutlinerState& st, Astra::Entity e, const std::string& current)
    {
        st.renameTarget = e;
        st.renameBuf = current;
        st.renameFocusPending = true;
        // Starting a NEW rename abandons any deferred one. The user has
        // moved on; landing a parked rename afterwards would apply a name
        // they already replaced (or applied to an entity they have since
        // stopped editing) out of nowhere, one frame late.
        st.pendingRename = Astra::Entity::Invalid();
        st.pendingRenameName.clear();
    }

    void DeleteSelection(Astra::Registry& registry, SelectionContext& sel,
                         Arcane::CommandStack& undo, const SceneEditBinding& binding)
    {
        // The root guard (s3.1): a mixed selection drops the root, a
        // root-only one deletes nothing and pushes no step.
        const std::vector<Astra::Entity> doomed = SelectionWithoutSceneRoot(registry, sel.Entities());
        if (doomed.empty())
            return;
        if (ApplyStructural(undo, binding, "Delete",
                [&] { return Arcane::Edit::DeleteEntities(registry, doomed) > 0; },
                &doomed))
            sel.Clear();
    }

    // Edit -> clipboard (spec II.B). Copy serializes the selection's
    // subtree roots to a JSON envelope on the OS clipboard; every
    // structural half wraps in ApplyStructural like the Outliner's ops.
    bool CopySelectionToClipboard(Astra::Registry& registry, const SelectionContext& sel)
    {
        const std::vector<Astra::Entity> copied = SelectionWithoutSceneRoot(registry, sel.Entities());
        if (copied.empty())
            return false;   // nothing, or the scene root alone (s3.1)
        nlohmann::json payload = Arcane::Edit::SerializeSubtrees(registry, copied);
        if (payload["entities"].empty())
            return false;
        ImGui::SetClipboardText(
            Arcane::Editor::WrapEntityClipboard(std::move(payload)).c_str());
        return true;
    }

    void CutSelection(Astra::Registry& registry, SelectionContext& sel,
                      Arcane::CommandStack& undo, const SceneEditBinding& binding)
    {
        // No-clipboard-clobber rule: a Play-mode (or otherwise structure-
        // locked) Cut must not overwrite the clipboard with a cut that never
        // happens. Checked BEFORE the copy, not after.
        if (!binding.editMode)
            return;
        // Copy only when the delete half can apply -- see above.
        if (!CopySelectionToClipboard(registry, sel))
            return;
        // Delete the FULL captured subtrees -- DeleteEntities splices
        // children up, so passing only the roots would orphan what the
        // clipboard just took (EntityOps.hpp, SubtreeEntities).
        const std::vector<Astra::Entity> roots =
            Arcane::Edit::SelectionRoots(registry, SelectionWithoutSceneRoot(registry, sel.Entities()));
        const std::vector<Astra::Entity> doomed =
            Arcane::Edit::SubtreeEntities(registry, roots);
        if (Arcane::Editor::ApplyStructural(undo, binding, "Cut",
                [&] { return Arcane::Edit::DeleteEntities(registry, doomed) > 0; },
                &doomed))
            sel.Clear();
    }

    // Shared instantiate+select half of Paste/Duplicate: apply the
    // structural instantiate, then move the selection onto the fresh
    // roots. One implementation -- Paste and Duplicate differ only in
    // where the payload comes from.
    static void InstantiateAndSelect(Astra::Registry& registry, SelectionContext& sel,
                                     Arcane::CommandStack& undo, const SceneEditBinding& binding,
                                     const nlohmann::json& payload, std::string label)
    {
        std::vector<Astra::Entity> roots;
        std::vector<Astra::Entity> made;
        if (Arcane::Editor::ApplyStructural(undo, binding, std::move(label),
                [&]
                {
                    roots = Arcane::Edit::InstantiateSubtrees(registry, payload);
                    if (!roots.empty())
                        made = Arcane::Edit::SubtreeEntities(registry, roots);
                    return !roots.empty();
                },
                &made))
        {
            sel.Clear();
            sel.AddRange(roots, roots.back());
        }
    }

    void PasteFromClipboard(Astra::Registry& registry, SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding)
    {
        if (const auto payload =
                Arcane::Editor::ParseEntityClipboard(ImGui::GetClipboardText()))
            InstantiateAndSelect(registry, sel, undo, binding, *payload, "Paste");
        else
            ARC_INFO("Paste: the clipboard holds no Arcane entities");
    }

    void DuplicateSelection(Astra::Registry& registry, SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding)
    {
        const std::vector<Astra::Entity> duplicated = SelectionWithoutSceneRoot(registry, sel.Entities());
        if (duplicated.empty())
            return;   // the root alone never duplicates -- it nested a second scene + Camera (s3.1)
        const nlohmann::json payload = Arcane::Edit::SerializeSubtrees(registry, duplicated);
        if (!payload["entities"].empty())
            InstantiateAndSelect(registry, sel, undo, binding, payload, "Duplicate");
    }

    namespace
    {
        // Popup id shared by the Inspector's "+ Add Component" button and the
        // Outliner row menu's "Add Component...".
        //
        // NOTE the ids at those two sites are NOT equal -- ImGui seeds a popup id
        // with the CURRENT WINDOW, so this literal resolves to a different id in
        // the Inspector than in the Outliner. (The previous comment here claimed
        // the opposite. Corrected 2026-07-26.) That is harmless: each panel opens
        // and draws its own popup at its own scope, and each is self-consistent.
        constexpr const char* kAddComponentPopup = "##addcomponent";

        // The searchable Add Component popup: draws the catalog, applies the
        // pick as ONE undo step over the whole selection. The caller opens it
        // with ImGui::OpenPopup(kAddComponentPopup) and then calls this every
        // frame at the same id-stack level.
        //
        // The function-local search buffer is justified by ONE POPUP BEING OPEN AT
        // A TIME (not by the ids being equal -- see above), same as the asset-ref
        // pick popup below.
        void DrawAddComponentPopup(Astra::Registry& registry,
                                   const std::vector<Astra::Entity>& selection,
                                   Arcane::CommandStack& undo,
                                   const SceneEditBinding& binding)
        {
            static char s_search[64] = {};
            const Astra::ComponentDescriptor* chosen = nullptr;
            // Evaluated ONCE for the whole popup: a popup left open when Play
            // starts used to keep its rows fully interactive and then silently
            // no-op in ApplyStructural.
            const bool canEdit = CanEditStructure(undo, binding);

            if (ImGui::BeginPopup(kAddComponentPopup))
            {
                if (ImGui::IsWindowAppearing())
                {
                    s_search[0] = '\0';
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::SetNextItemWidth(260.0f);
                ImGui::InputTextWithHint("##compsearch", "Search...",
                                         s_search, sizeof(s_search));
                ImGui::Separator();

                const std::vector<ComponentCatalogEntry> entries =
                    BuildComponentCatalog(registry, selection, s_search);
                if (entries.empty())
                {
                    ImGui::TextDisabled("no matching components");
                }
                else
                {
                    ImGui::BeginChild("##complist", ImVec2(260.0f, 260.0f));
                    for (const ComponentCatalogEntry& e : entries)
                    {
                        // missingCount == 0 means every selected entity already
                        // carries it, so the add would be a no-op. Shown
                        // disabled rather than hidden: "you already have this"
                        // reads better than a row that silently vanishes.
                        const bool addable = e.missingCount > 0 && canEdit;
                        if (!addable)
                            ImGui::BeginDisabled();
                        if (ImGui::Selectable(e.typeName.c_str()) && addable)
                            chosen = e.desc;
                        if (!addable)
                            ImGui::EndDisabled();
                    }
                    ImGui::EndChild();
                }

                if (chosen)
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            // Applied OUTSIDE the popup scope: the mutation invalidates the
            // catalog vector the loop above is still holding.
            if (chosen)
            {
                const Astra::ComponentDescriptor& desc = *chosen;
                ApplyStructural(undo, binding, "Add Component",
                    [&] { return Arcane::Edit::AddComponent(registry, selection, desc) > 0; },
                    &selection);
            }
        }
    }

    void DrawOutlinerPanel(Astra::Registry& registry, SelectionContext& sel,
                           Arcane::CommandStack& undo, const SceneEditBinding& binding,
                           OutlinerState& state, std::uint64_t savedStateId,
                           bool* open)
    {
        ImGui::Begin("Outliner", open);

        // A rename the commit site below could not land because another
        // transaction was open. Retried BEFORE any row is built, so this
        // frame's rows already show the new name. EVERY result but Deferred
        // consumes the slot: Renamed landed it, NoChange means someone else got
        // there first, and Invalid means the entity is gone or lost its
        // Identity -- retrying any of those forever would be a slow leak that
        // could also fire long after the user moved on.
        if (state.pendingRename.IsValid())
        {
            bool consumed = true;
            if (binding.editMode)
            {
                consumed = Arcane::Edit::RenameWithUndo(undo, registry, state.pendingRename,
                                                        state.pendingRenameName)
                           != Arcane::Edit::RenameResult::Deferred;
            }
            else
            {
                // Play started between the commit frame and this one. Applied
                // WITHOUT undo bracketing and consumed either way, exactly like
                // the commit site's own play-mode branch below: an entry
                // recorded now would let a later Ctrl+Z write play-time bytes
                // over the registry Stop restored. Holding the slot until Stop
                // instead would land a rename the user typed a whole play
                // session ago.
                Arcane::Edit::RenameEntity(registry, state.pendingRename,
                                           state.pendingRenameName);
            }
            if (consumed)
            {
                state.pendingRename = Astra::Entity::Invalid();
                state.pendingRenameName.clear();
            }
        }

        // Hoisted once per frame: every structural affordance in this panel keys
        // off it, and BeginDisabled/EndDisabled pairs must agree.
        const bool canEditStructure = CanEditStructure(undo, binding);

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##outliner_search", ICON_LC_SEARCH " Filter",
                                 state.search, sizeof(state.search));

        // Per-entity unsaved markers (the asterisk column): the CommandStack
        // diff between the scene's save baseline and now. An unreachable
        // baseline (undone past the save point then diverged, or evicted)
        // honestly marks EVERYTHING possibly-modified via `all`.
        const Arcane::CommandStack::TouchedSince touchedSince =
            undo.TouchedSinceState(savedStateId);
        std::unordered_set<std::uint64_t> touchedIds;
        touchedIds.reserve(touchedSince.entities.size());
        for (Astra::Entity e : touchedSince.entities)
            touchedIds.insert(static_cast<std::uint64_t>(e.GetValue()));
        const OutlinerModified modified{ &touchedIds, !touchedSince.baselineFound };

        const std::vector<OutlinerRow> rows =
            BuildOutlinerRows(registry, state.search, state.sort, state.collapsed, modified);

        // The rename target can stop being drawable two ways: a structural
        // undo/redo destroys it, or it simply leaves the visible set (its
        // parent collapsed, or the filter excludes it). Either way no row
        // draws the InputText, so IsItemDeactivated never fires -- drop the
        // target or `renaming` wedges shut, taking F2 and Delete with it.
        if (state.renameTarget.IsValid())
        {
            bool hasRow = false;
            for (const OutlinerRow& r : rows)
                if (r.entity == state.renameTarget)
                {
                    hasRow = true;
                    break;
                }
            if (!hasRow)
                state.renameTarget = Astra::Entity::Invalid();
        }

        const bool renaming = state.renameTarget.IsValid();
        // NoPopupHierarchy (the input document's ColumnKeysLive rule): a popup
        // opened from the Outliner (a row's context menu advertising
        // "Delete  Del" / "Rename  F2") must NOT count as the Outliner's focus,
        // or Del/F2 would act on the selection behind the open menu. Keys stay
        // inert while any popup owns focus; the menu's own items act instead.
        const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_NoPopupHierarchy);
        if (windowFocused) EditorActions::Get().MarkContextActive(ActionContext::Outliner);
        // Shortcuts must not fire while any text field owns the keyboard
        // (e.g. the search box above) -- else Delete/F2 hijack typing.
        if (binding.editMode && windowFocused && !renaming && !ImGui::GetIO().WantTextInput)
        {
            if (EditorActions::Get().Pressed("outliner.rename") && sel.Count() == 1)
            {
                // Rename edits an EXISTING Identity -- Edit::RenameEntity
                // refuses when there is none and never mints one
                // (EntityOps.cpp:180-186). An entity without one is a runtime
                // spawn with no durable identity, so F2 does nothing rather
                // than opening a box whose commit could not land.
                if (const Arcane::Identity* info =
                        std::as_const(registry).GetComponent<Arcane::Identity>(sel.Primary()))
                    BeginRename(state, sel.Primary(), info->name);
            }
            if (EditorActions::Get().Pressed("outliner.delete") && sel.HasSelection())
                DeleteSelection(registry, sel, undo, binding);
        }

        const float footerH = ImGui::GetFrameHeightWithSpacing();
        // PadOuterX is NOT a default for borderless tables (only BordersOuterV
        // implies it), so without it the outermost columns run flush to the
        // table edges -- the header's eye sat on the far left edge. One flag
        // pads header and rows alike, so the row icons stay column-aligned
        // under their header glyphs. NOT ImGuiTableFlags_Sortable: sort state
        // is OURS (state.sort, cycled by the custom header below), because
        // ImGui's TableHeader right-justifies its sort arrow in the cell and
        // offers no per-column way to hide it -- and this outliner wants the
        // arrow ONLY on Label, sitting directly after the text.
        const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##outliner_rows", 3, tflags, ImVec2(0.0f, -footerH)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            // SQUARE icon slots: sized to each GLYPH, not GetFrameHeight --
            // frame height carries FramePadding.y the unframed rows don't
            // have, which left the eye column ~10px wider than the row is
            // tall. Glyph advance + the cell padding both sides lands the
            // header cell and the row button at the row's own height.
            ImGui::TableSetupColumn(ICON_LC_EYE, ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize(ICON_LC_EYE).x);
            ImGui::TableSetupColumn(ICON_LC_ASTERISK, ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize(ICON_LC_ASTERISK).x);
            ImGui::TableSetupColumn("Label");

            // CUSTOM header row (see the flags comment): each cell is a
            // Selectable (HeaderHovered fill on hover, like a real header)
            // that cycles state.sort ascending -> descending -> none -- the
            // tri-state the old SortTristate flag provided. All three columns
            // sort; only Label ever shows the arrow, an inline chevron padded
            // directly after the text ("###" keeps the widget id stable while
            // the label swaps). Legend text dims, as before: chrome fill +
            // muted text is what separates "column header" from "entity row".
            // Rows were built with LAST frame's sort (one-frame lag, rebuilt
            // every frame anyway).
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const auto headerCell = [&state](int col, OutlinerSort::Column id,
                                             const char* label, bool showArrow)
            {
                ImGui::TableSetColumnIndex(col);
                std::string text = label;
                if (showArrow && state.sort.column == id)
                    text += state.sort.ascending ? "  " ICON_LC_CHEVRON_UP
                                                 : "  " ICON_LC_CHEVRON_DOWN;
                text += "###hdr";
                text += static_cast<char>('0' + col);
                if (ImGui::Selectable(text.c_str(), false))
                {
                    if (state.sort.column != id)
                        state.sort = OutlinerSort{ id, true };
                    else if (state.sort.ascending)
                        state.sort.ascending = false;
                    else
                        state.sort = OutlinerSort{};   // third click: tree order
                }
            };
            headerCell(0, OutlinerSort::Column::Visibility, ICON_LC_EYE,      false);
            headerCell(1, OutlinerSort::Column::Modified,   ICON_LC_ASTERISK, false);
            headerCell(2, OutlinerSort::Column::Label,      "Label",          true);
            ImGui::PopStyleColor();

            // Full-width row highlight via the TABLE's row background, not
            // the tree item's own frame: TreeNodeEx sits AFTER the per-depth
            // Indent, so its Header fill starts at the indent (childed rows
            // read half-highlighted) and can never cover the eye column. The
            // row bg spans every column edge to edge. Priority is SELECTION
            // FIRST: a selected row stays selection-blue under the cursor --
            // the item path let HeaderHovered grey paint over it -- and hover
            // grey only ever shows on unselected rows.
            // TableGetHoveredRow is imgui_internal (this file already
            // includes it for DockBuilder) and reports LAST frame's hovered
            // row -- one frame of hover lag, invisible in practice. Row 0 is
            // the TableHeadersRow, so data rows count from 1.
            const int hoveredRow = ImGui::TableGetHoveredRow();
            int tableRow = 0;
            for (const OutlinerRow& row : rows)
            {
                ImGui::TableNextRow();
                ++tableRow;
                const bool rowSelected = sel.Contains(row.entity);
                const bool rowHovered  = (hoveredRow == tableRow);
                if (rowSelected)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                        ImGui::GetColorU32(ImGuiCol_Header));
                else if (rowHovered)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                        ImGui::GetColorU32(ImGuiCol_HeaderHovered));
                ImGui::PushID(static_cast<int>(row.entity.GetValue()));

                // -- column 0: the eye --------------------------------------
                // ON DEMAND (UE's gutter: SVisibilityWidget::GetBrush swaps
                // brushes on visible x hovered, SceneOutlinerGutter.cpp:
                // 108-121): a HIDDEN entity always wears its closed eye --
                // that state must stay readable at a glance -- while visible
                // entities only show one when the row is hovered or selected.
                // To click the eye you must hover the row, so the button is
                // always there when reachable; rowHovered carries
                // TableGetHoveredRow's one-frame lag, invisible here too.
                ImGui::TableSetColumnIndex(0);
                if (row.hidden || rowSelected || rowHovered)
                {
                    const char* icon = row.hidden ? ICON_LC_EYE_OFF : ICON_LC_EYE;
                    if (row.hidden)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (binding.editMode)
                    {
                        // Chrome-free toggle, drawn to be pixel-identical to
                        // the Play branch's plain text below: transparent
                        // fill, no frame border (the theme's global 1px
                        // FrameBorderSize is what boxed the eye), and zero
                        // FramePadding so the glyph sits at the same x as the
                        // header's icon. Only the hover/active fill remains,
                        // hugging the glyph -- so the column stays aligned
                        // across modes and the button reads as an icon, not
                        // a button.
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
                        if (ImGui::SmallButton(icon))
                        {
                            const Astra::Entity e = row.entity;
                            const bool hide = !row.hidden;
                            // Recursive toggle -> the whole subtree is the
                            // touched set for the unsaved markers.
                            std::vector<Astra::Entity> targets{ e };
                            registry.GetRelations(e).ForEachDescendant(
                                [&](Astra::Entity d, std::size_t) { targets.push_back(d); });
                            ApplyStructural(undo, binding, hide ? "Hide" : "Show",
                                [&] { return Arcane::Edit::SetHiddenRecursive(registry, e, hide) > 0; },
                                &targets);
                        }
                        ImGui::PopStyleVar(2);
                        ImGui::PopStyleColor();
                    }
                    else
                        ImGui::TextUnformatted(icon);
                    if (row.hidden)
                        ImGui::PopStyleColor();
                }

                // -- column 1: the unsaved-changes asterisk -----------------
                // Only a MODIFIED entity wears one (mirrors the hidden eye:
                // the state itself must stay readable, so no hover gating).
                // row.modified is a seam today (EntityList.hpp) -- nothing
                // sets it, so this column stays quiet until per-entity dirty
                // tracking lands; the button's save-ish action arrives with
                // that wiring, same chrome-free dressing as the eye.
                ImGui::TableSetColumnIndex(1);
                if (row.modified)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
                    ImGui::SmallButton(ICON_LC_ASTERISK);
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Unsaved changes");
                }

                // -- column 2: tree arrow + label (or inline rename) --------
                ImGui::TableSetColumnIndex(2);
                if (state.renameTarget == row.entity)
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (state.renameFocusPending)
                    {
                        ImGui::SetKeyboardFocusHere();
                        state.renameFocusPending = false;
                    }
                    InputTextString("##rename", &state.renameBuf,
                                    ImGuiInputTextFlags_AutoSelectAll);
                    // Commit on deactivate; the equality guard below IS the
                    // cancel path. Escape reverts the buffer to its
                    // activation-time text BEFORE deactivating
                    // (imgui_widgets.cpp:5212 raises revert_edit, :5300-5308
                    // writes TextToRevertTo back), so a cancelled rename
                    // arrives here equal to its seed and the guard drops it.
                    // Enter (:5180) and click-away deactivate the same way, so
                    // telling the three apart needs neither EnterReturnsTrue
                    // nor the global IsKeyPressed(Escape) query this replaced
                    // -- that query answered "was Escape pressed anywhere",
                    // not "did THIS box cancel".
                    if (ImGui::IsItemDeactivated())
                    {
                        const Astra::Entity e = row.entity;
                        // Re-read rather than trust the frame that opened the
                        // box: an undo/redo between the two can change or
                        // remove the component.
                        const Arcane::Identity* info =
                            std::as_const(registry).GetComponent<Arcane::Identity>(e);
                        if (info && state.renameBuf != info->name)
                        {
                            if (binding.editMode)
                            {
                                // ONE ComponentEditCommand in its OWN
                                // transaction -- the same shape as an Inspector
                                // field edit, not the whole-registry memento the
                                // structural edits in this panel use.
                                //
                                // This can fire in the same frame as a gizmo
                                // press or an Inspector field activation, so the
                                // stack may already be busy. RenameWithUndo then
                                // returns Deferred and mutates NOTHING: it
                                // refuses to join, because a joined rename rides
                                // the owner's Commit/Cancel and Cancel discards
                                // pending snapshots without reverting
                                // (CommandStack.cpp:75-82) -- which would leave
                                // the rename applied and permanently
                                // un-undoable. Park it and retry at the top of
                                // the next frame instead.
                                if (Arcane::Edit::RenameWithUndo(undo, registry, e, state.renameBuf)
                                    == Arcane::Edit::RenameResult::Deferred)
                                {
                                    state.pendingRename     = e;
                                    state.pendingRenameName = state.renameBuf;
                                }
                            }
                            else
                            {
                                // Play mode: applied WITHOUT undo bracketing,
                                // matching the Inspector's field visitor, which
                                // leaves its stack pointer null while Play runs
                                // (an entry recorded now would let a later
                                // Ctrl+Z write play-time bytes over the registry
                                // Stop restored). Deliberate change: this used
                                // to be refused outright and silently.
                                //
                                // Reached only when Play STARTS with a rename
                                // box already open -- all three entry points
                                // still require Edit mode to open one -- so it
                                // is the finish-what-you-typed path, not a
                                // play-time rename affordance.
                                Arcane::Edit::RenameEntity(registry, e, state.renameBuf);
                            }
                        }
                        state.renameTarget = Astra::Entity::Invalid();
                    }
                }
                else
                {
                    const float indent = row.depth * ImGui::GetStyle().IndentSpacing;
                    if (indent > 0.0f)
                        ImGui::Indent(indent);

                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                                             | ImGuiTreeNodeFlags_OpenOnArrow
                                             | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    if (!row.hasChildren)
                        flags |= ImGuiTreeNodeFlags_Leaf;
                    if (rowSelected)
                        flags |= ImGuiTreeNodeFlags_Selected;

                    const std::uint64_t value = static_cast<std::uint64_t>(row.entity.GetValue());
                    const bool open = !state.collapsed.contains(value);
                    ImGui::SetNextItemOpen(open, ImGuiCond_Always);
                    if (row.dimmed)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    // The row bg set at TableNextRow owns ALL row
                    // highlighting; the tree item's own fills are silenced so
                    // the indent-clipped frame never paints a second, shorter
                    // highlight over the full-width one. Behavior (click,
                    // arrow toggle, nav) is untouched.
                    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
                    const bool nowOpen = ImGui::TreeNodeEx(row.label.c_str(), flags);
                    ImGui::PopStyleColor(3);
                    if (row.dimmed)
                        ImGui::PopStyleColor();
                    if (row.hasChildren && nowOpen != open)
                    {
                        if (nowOpen) state.collapsed.erase(value);
                        else         state.collapsed.insert(value);
                    }

                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)
                        && !ImGui::IsItemToggledOpen())
                    {
                        const double now = ImGui::GetTime();
                        const bool ctrl = ImGui::GetIO().KeyCtrl;
                        const bool shift = ImGui::GetIO().KeyShift;
                        if (ctrl)
                            sel.Toggle(row.entity);
                        else if (shift && sel.HasSelection())
                        {
                            // An anchor with no visible row (filtered out, or
                            // under a collapsed parent) yields an empty range;
                            // degrade to a plain select rather than moving the
                            // primary outside the selection.
                            const std::vector<Astra::Entity> range =
                                RowRange(rows, sel.Primary(), row.entity);
                            if (range.empty())
                                sel.Select(row.entity);
                            else
                                sel.AddRange(range, row.entity);
                        }
                        else
                        {
                            // Slow second click on the sole-selected row = rename.
                            // Gated on Identity like the other two entry
                            // points (see the F2 site); without one the click
                            // stays a plain select.
                            const Arcane::Identity* info =
                                std::as_const(registry).GetComponent<Arcane::Identity>(row.entity);
                            const bool slowSecond = binding.editMode && info != nullptr
                                && sel.Count() == 1 && sel.Primary() == row.entity
                                && state.lastClicked == row.entity
                                && (now - state.lastClickTime) > ImGui::GetIO().MouseDoubleClickTime
                                && (now - state.lastClickTime) < Arcane::Settings<OutlinerSettings>().slowClickMaxSeconds;
                            if (slowSecond)
                                BeginRename(state, row.entity, info->name);
                            else
                                sel.Select(row.entity);
                        }
                        state.lastClicked = row.entity;
                        state.lastClickTime = now;
                    }

                    // Right-click selects (if outside the selection) then menus.
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)
                        && !sel.Contains(row.entity))
                        sel.Select(row.entity);
                    if (ImGui::BeginPopupContextItem("##row_ctx"))
                    {
                        // CanEditStructure, not just editMode: a memento refuses
                        // inside an open gesture too, and that refusal used to be an
                        // ARC_WARN only -- i.e. invisible, reading as "the menu
                        // stopped working".
                        if (!canEditStructure)
                            ImGui::BeginDisabled();
                        if (ImGui::MenuItem("New Child Entity"))
                        {
                            Astra::Entity created = Astra::Entity::Invalid();
                            const Astra::Entity parent = row.entity;
                            std::vector<Astra::Entity> made;
                            if (ApplyStructural(undo, binding, "Create Entity",
                                    [&] { created = Arcane::Edit::CreateEntityInScene(registry, parent);
                                          if (created.IsValid()) made.push_back(created);
                                          return created.IsValid(); },
                                    &made))
                            {
                                state.collapsed.erase(
                                    static_cast<std::uint64_t>(parent.GetValue()));
                                sel.Select(created);
                            }
                        }
                        // Spec s8: a primitive under THIS row (the app spawns
                        // it at the view's focus point, selected and framed).
                        DrawAddPrimitiveSubmenu(state, row.entity);
                        ImGui::Separator();
                        // The root guard (s3.1). The right-click above already
                        // selected this row when it was outside the selection.
                        const bool rootOnly = sel.Contains(row.entity)
                            ? IsSceneRootOnly(registry, sel.Entities())
                            : Arcane::Edit::IsSceneRoot(registry, row.entity);
                        // Edit-menu parity via the shared functions above.
                        // Acts on the SELECTION -- the right-click already
                        // selected this row when it was outside it.
                        if (ImGui::MenuItem("Cut", MenuKey("edit.cut").c_str(), false, !rootOnly))
                            CutSelection(registry, sel, undo, binding);
                        RootRefusalTooltip(rootOnly, SceneRootVerb::Cut);
                        if (ImGui::MenuItem("Copy", MenuKey("edit.copy").c_str(), false, !rootOnly))
                            CopySelectionToClipboard(registry, sel);
                        RootRefusalTooltip(rootOnly, SceneRootVerb::Copy);
                        if (ImGui::MenuItem("Paste", MenuKey("edit.paste").c_str()))
                            PasteFromClipboard(registry, sel, undo, binding);
                        if (ImGui::MenuItem("Duplicate", MenuKey("edit.duplicate").c_str(), false, !rootOnly))
                            DuplicateSelection(registry, sel, undo, binding);
                        RootRefusalTooltip(rootOnly, SceneRootVerb::Duplicate);
                        ImGui::Separator();
                        // Disabled rather than hidden without an Identity, so
                        // the refusal is visible before the click -- the same
                        // treatment the structural items get above. ForTooltip's
                        // default mouse flags include AllowWhenDisabled
                        // (imgui.cpp:1587, not overridden by this editor), which
                        // is what lets the explanation reach a greyed item.
                        const Arcane::Identity* rowInfo =
                            std::as_const(registry).GetComponent<Arcane::Identity>(row.entity);
                        if (ImGui::MenuItem("Rename", MenuKey("outliner.rename").c_str(), false, rowInfo != nullptr))
                            BeginRename(state, row.entity, rowInfo->name);
                        if (rowInfo == nullptr
                            && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                            ImGui::SetTooltip("Runtime entity: no Identity component to rename");
                        // ImGui cannot open a popup from inside another popup's
                        // scope, so the request is latched and consumed at panel
                        // scope below (the standard deferred-OpenPopup pattern).
                        if (ImGui::MenuItem("Add Component..."))
                            state.addComponentPending = true;
                        if (ImGui::MenuItem("Delete", MenuKey("outliner.delete").c_str(), false, !rootOnly))
                        {
                            if (!sel.Contains(row.entity))
                                sel.Select(row.entity);
                            DeleteSelection(registry, sel, undo, binding);
                        }
                        RootRefusalTooltip(rootOnly, SceneRootVerb::Delete);
                        if (!canEditStructure)
                            ImGui::EndDisabled();
                        ImGui::EndPopup();
                    }

                    // Reparent-by-drag is structural too, so it honours the same
                    // predicate rather than starting a drag that will refuse.
                    // The scene root is never a drag source (s3.1): dropping it
                    // onto an orphan moved SceneRoot out of the subtree SaveJson
                    // walks. Dropping ONTO the root row is unchanged.
                    if (canEditStructure && !Arcane::Edit::IsSceneRoot(registry, row.entity)
                        && ImGui::BeginDragDropSource())
                    {
                        ImGui::SetDragDropPayload(kOutlinerDragType,
                                                  &row.entity, sizeof(Astra::Entity));
                        ImGui::TextUnformatted(row.label.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (canEditStructure && ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* p =
                                ImGui::AcceptDragDropPayload(kOutlinerDragType))
                        {
                            Astra::Entity dragged;
                            std::memcpy(&dragged, p->Data, sizeof(dragged));
                            const std::vector<Astra::Entity> moving = SelectionWithoutSceneRoot(registry,
                                sel.Contains(dragged) ? sel.Entities()
                                                      : std::vector<Astra::Entity>{ dragged });
                            const Astra::Entity target = row.entity;
                            ApplyStructural(undo, binding, "Reparent",
                                [&] { return Arcane::Edit::Reparent(registry, moving, target) > 0; },
                                &moving);
                        }
                        ImGui::EndDragDropTarget();
                    }

                    if (indent > 0.0f)
                        ImGui::Unindent(indent);
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Drop below the table = move to the TOP OF THE SCENE (s3.1: under
        // SceneRoot, never a registry root beside it). Only visible mid-drag, and
        // only for our own entity payload -- GetDragDropPayload() returns
        // non-null for ANY active drag (e.g. an asset-browser drag), which
        // used to show this strip for foreign payloads too. The bool is kept:
        // the status bar below yields the footer area to this strip while it
        // is up.
        const ImGuiPayload* activeDrag = ImGui::GetDragDropPayload();
        const bool droppingBelowTable = canEditStructure && activeDrag != nullptr
            && activeDrag->IsDataType(kOutlinerDragType);
        if (droppingBelowTable)
        {
            ImGui::Selectable("(drop here to move to scene root)", false,
                              ImGuiSelectableFlags_Disabled);
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload(kOutlinerDragType))
                {
                    Astra::Entity dragged;
                    std::memcpy(&dragged, p->Data, sizeof(dragged));
                    const std::vector<Astra::Entity> moving = SelectionWithoutSceneRoot(registry,
                        sel.Contains(dragged) ? sel.Entities()
                                              : std::vector<Astra::Entity>{ dragged });
                    // ONE undo step for a move; a refusal (no live root, or
                    // nothing to move) returns 0 and pushes none.
                    ApplyStructural(undo, binding, "Unparent",
                        [&] { return Arcane::Edit::ReparentInScene(registry, moving,
                                                                   Astra::Entity::Invalid()) > 0; },
                        &moving);
                }
                ImGui::EndDragDropTarget();
            }
        }

        // BeginPopupContextWindow cannot serve this: the ScrollY rows table
        // opens a child window that covers the panel, and the window-hover
        // test behind that helper demands an EXACT window match against the
        // outer window. Detect the hover across the child hierarchy and open
        // the popup by hand.
        if (canEditStructure
            && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
            && !ImGui::IsAnyItemHovered()
            && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##outliner_ctx");
        if (ImGui::BeginPopup("##outliner_ctx"))
        {
            if (ImGui::MenuItem("New Entity"))
            {
                Astra::Entity created = Astra::Entity::Invalid();
                std::vector<Astra::Entity> made;
                if (ApplyStructural(undo, binding, "Create Entity",
                        [&] { created = Arcane::Edit::CreateEntityInScene(registry,
                                            Astra::Entity::Invalid());
                              if (created.IsValid()) made.push_back(created);
                              return created.IsValid(); },
                        &made))
                    sel.Select(created);
            }
            // Spec s8: the scene-level `Add > 3D Object` -- a primitive under
            // SceneRoot (the same Invalid-parent rule "New Entity" takes).
            DrawAddPrimitiveSubmenu(state, Astra::Entity::Invalid());
            ImGui::Separator();
            if (ImGui::MenuItem("Paste", MenuKey("edit.paste").c_str()))
                PasteFromClipboard(registry, sel, undo, binding);
            ImGui::EndPopup();
        }

        // Latched by the row menu one step earlier -- see the comment there.
        // The right-clicked row is already in the selection (the row's
        // right-click handler selects it when it was outside), so the popup
        // operates on exactly what the user aimed at.
        if (state.addComponentPending)
        {
            state.addComponentPending = false;
            ImGui::OpenPopup(kAddComponentPopup);
        }
        DrawAddComponentPopup(registry, sel.Entities(), undo, binding);

        // ---- status bar -----------------------------------------------------
        // UE's outliner closes with a full-width count bar, not floating text.
        // Drawn ENTIRELY through the draw list -- fill, seam, and text -- so
        // it is pure chrome: no items, no cursor moves, no content-size
        // growth (a trailing SetCursorPos-then-text here would re-create the
        // Console's imgui.cpp:11544 abort, and an item this low would make
        // the outer window want a scrollbar). Full-bleed on purpose: the
        // fill runs edge to edge UNDER the window padding, which is what
        // makes it read as panel chrome rather than another row. The rows
        // table's footerH reserve (above) is what keeps rows from sliding
        // beneath it. Suppressed while the unparent drop strip occupies the
        // footer -- mid-drag that strip is the load-bearing UI.
        if (!droppingBelowTable)
        {
            std::size_t total = 0;
            for (Astra::Entity e : registry.GetEntityManager())
            {
                (void)e;
                ++total;
            }

            const ImVec2 winPos  = ImGui::GetWindowPos();
            const ImVec2 winSize = ImGui::GetWindowSize();
            const float  statusH = ImGui::GetFrameHeight();
            const ImVec2 barMin(winPos.x, winPos.y + winSize.y - statusH);
            const ImVec2 barMax(winPos.x + winSize.x, winPos.y + winSize.y);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(barMin, barMax, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
            dl->AddLine(barMin, ImVec2(barMax.x, barMin.y),
                        ImGui::GetColorU32(ImGuiCol_Border));

            char status[64];
            std::snprintf(status, sizeof(status), "%zu entities (%zu selected)",
                          total, sel.Count());
            const ImVec2 textSize = ImGui::CalcTextSize(status);
            dl->AddText(ImVec2(barMin.x + ImGui::GetStyle().FramePadding.x * 2.0f,
                               barMin.y + (statusH - textSize.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), status);
        }

        ImGui::End();
    }

    void DrawInspectorBody(Astra::Registry& registry, const SelectionContext& sel,
                           Arcane::CommandStack& undo, const SceneEditBinding& binding,
                           const Arcane::Project* project, InspectorState& state,
                           const InspectorServices* services)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard.
        const EditGesture::ScopeGuard gestureGuard{ &undo, state.gesture };
        // The section headers and row grids below draw through the shared
        // page primitives (Widgets/PropertyGrid.hpp); state.grid carries the
        // panel-wide label split every grid seeds from and adopts back.
        PropertyGrid pg(state.grid);

        if (!sel.HasSelection())
        {
            ImGui::TextDisabled("No selection");
            return;
        }

        const Astra::Entity primary = sel.Primary();
        // No name line (spec 2026-09-30 s4.3): the header's leaf crumb already
        // names the entity and its " (+N)" (SceneInspectorSource::Breadcrumb);
        // the body starts at its first control.

        // Search, UE's Details-panel shape (SDetailsViewBase.cpp:1016 --
        // OnFilterTextChanged -> FilterView). Filters components AND fields live.
        // UE's SSearchBox ships a built-in clear "X" (SSearchBox.cpp:149-161,
        // OnClearSearch) rather than requiring select-all-delete, and shows no
        // "no matches" text anywhere in PropertyEditor -- both mirrored below.
        // The button only claims layout space when there is something to
        // clear, so an empty query keeps the box at the full -FLT_MIN width it
        // would have if the button did not exist.
        const bool hasQuery = state.searchBuffer[0] != '\0';
        if (hasQuery)
        {
            const float clearButtonWidth = ImGui::CalcTextSize(ICON_LC_X).x
                                          + ImGui::GetStyle().FramePadding.x * 2.0f
                                          + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(-clearButtonWidth);
        }
        else
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
        }
        ImGui::InputTextWithHint("##inspector_search", ICON_LC_SEARCH " Filter",
                                 state.searchBuffer, sizeof(state.searchBuffer));
        if (hasQuery)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_LC_X))
                state.searchBuffer[0] = '\0';
        }
        // INVARIANT: `searchBuffer` is written ONLY by a widget that takes
        // ActiveId this same frame -- typing (already owned it from the prior
        // frame) or a click on the clear button above (takes it fresh, same
        // as clicking into the box). Either way, whatever field widget was
        // previously active deactivates THIS frame, so EndGesture() closes
        // state.gesture's open transaction before the next frame can redraw
        // under the new query and make that field vanish. A clear path that does
        // NOT move ActiveId (Escape, clear-on-selection-change) would skip that
        // close; the ScopeGuard closes it on the way out of this panel instead, so
        // the leak is contained rather than permanent -- see the Outliner's
        // hasRow sweep in DrawOutlinerPanel, which drops a renameTarget that
        // stopped being drawn, for the same hazard class. (Cited by NAME on
        // purpose: the line number this used to carry had already rotted.)
        const std::string_view query(state.searchBuffer);

        // Hoisted once per frame; see the Outliner's copy. Add/Remove Component
        // are structural (whole-registry memento), so they refuse during Play AND
        // inside an open field/gizmo gesture -- both now visible as disabled.
        const bool canEditStructure = CanEditStructure(undo, binding);

        // Removal is DEFERRED past the loop: Edit::RemoveComponent moves the
        // entity to a different archetype, which dangles every ci.data pointer
        // in the vector being iterated. Descriptor pointers themselves are
        // stable (they live in ComponentRegistry's fixed array).
        const Astra::ComponentDescriptor* pendingRemove = nullptr;

        // Section order: Identity first, Transform second, everything else
        // in registry order (matches UE's Details layout). ComponentInfo is
        // three raw pointers -- trivially copyable, and the pointers it holds
        // (descriptor into ComponentRegistry's fixed array, per the comment
        // above; data into the current archetype chunk, untouched before this
        // sort runs) stay valid across the reorder. stable_sort, not sort:
        // ties (rank 2, the overwhelming majority of components) must keep
        // their registry order, exactly like the loop did before this sort
        // existed. A null ci.meta ranks with the catch-all (2) rather than
        // being filtered here -- the loop body below already `continue`s on
        // null descriptor/meta, so leaving it in the vector is harmless and
        // this comparator never dereferences meta itself.
        std::vector<Astra::Registry::ComponentInfo> components = registry.InspectEntity(primary);
        std::stable_sort(components.begin(), components.end(),
            [](const Astra::Registry::ComponentInfo& a, const Astra::Registry::ComponentInfo& b)
            {
                const std::string_view an = a.meta ? a.meta->typeName : std::string_view{};
                const std::string_view bn = b.meta ? b.meta->typeName : std::string_view{};
                return InspectorSectionRank(an) < InspectorSectionRank(bn);
            });

        // One rhythm region for the whole per-component sweep, popped
        // unconditionally right after the loop closes. Both header types
        // fold FramePadding.y into their own frame height (TreeNodeBehavior,
        // imgui_widgets.cpp:6886-6888,6898): frame_height = label_size.y +
        // padding.y * 2, where padding.y IS style.FramePadding.y for the
        // framed CollapsingHeader, and is ImMin(CurrLineTextBaseOffset,
        // style.FramePadding.y) for the unframed category TreeNodeEx -- a
        // lower FramePadding.y can only shrink or hold that clamp, never grow
        // it, so the push still reaches both.
        //
        // ItemSpacing.y does NOT reach field-grid rows, though, despite
        // ItemSize adding it to the cursor advance (imgui.cpp:12130): that
        // same call subtracts it back out of CursorMaxPos.y one line later
        // (imgui.cpp:12132), and a table row's height is read from
        // CursorMaxPos.y + RowCellPaddingY at cell-close (TableEndCell,
        // imgui_tables.cpp:2268) -- CellPadding.y, untouched by this arc, is
        // what actually governs inner-row rhythm. What the ItemSpacing.y
        // push DOES reach: the header rows themselves (drawn outside any
        // table, where ItemSize's effect stands uncancelled) and the gaps
        // between sections. If a future desk pass wants the grid rows
        // themselves tighter too, CellPadding.y is the knob to add here, not
        // this one.
        // .x is carried over from the live style; only .y is overridden.
        //
        // Balanced on every path: this is OUTSIDE the loop, so none of the
        // loop's own `continue`s (component filter above, the category-field
        // sweep inside it) can skip the pop below -- they only skip to the
        // next `ci`, and the pop runs once the loop itself is done.
        //
        // Category indent is untouched: the open category's children are
        // indented by TreePushOverrideID's call to Indent() (imgui_widgets.cpp:
        // 7233-7239), which advances window->DC.Indent.x by g.Style.IndentSpacing
        // (imgui.cpp:12246) -- a third style var, distinct from the two pushed
        // here.
        // Row rhythm (UE's Details rows read visibly tighter than ImGui's own
        // theme defaults): editor.inspector.framePaddingY / itemSpacingY
        // (settings S6-37), base px at the editor.ui.scale. The defaults (3, 4)
        // are the pre-sweep constants, which equal ImGui's stock FramePadding.y
        // / ItemSpacing.y. Only .y is a tuning target: the live style's .x is
        // kept so horizontal spacing elsewhere in the panel (search box,
        // buttons) is untouched.
        const InspectorSettings& rhythm = Arcane::Settings<InspectorSettings>();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            ImVec2(ImGui::GetStyle().FramePadding.x, Ui::Px(rhythm.framePaddingY)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
            ImVec2(ImGui::GetStyle().ItemSpacing.x, Ui::Px(rhythm.itemSpacingY)));
        for (const Astra::Registry::ComponentInfo& ci : components)
        {
            // An unreflected component has no name to show and no fields to
            // visit: visitFields is populated FROM TypeMeta at registration, so
            // meta != null implies visitFields != null.
            if (!ci.descriptor || !ci.meta)
                continue;
            // TypeMeta::typeName is a std::string_view into a substring of a larger
            // compile-time literal (__FUNCSIG__/__PRETTY_FUNCTION__) -- NOT
            // guaranteed NUL-terminated, so it is copied into a std::string before
            // handing a `const char*` to ImGui.
            const std::string typeName(ci.meta->typeName);
            // Derived/runtime-owned state is never authored -- but as of task 5
            // this DISPLAY gate no longer matches the Add Component catalog or
            // Remove Component below: those two consult IsStructureLocked
            // (ComponentCatalog.hpp), which also covers Arcane::Identity, so
            // that identity renders its own section here (name editable, id
            // view-only) while staying un-addable and un-removable. The three
            // sites used to share one predicate by construction; splitting it
            // was the whole point of task 5, so read each site's predicate
            // rather than assuming parity.
            if (IsHiddenInInspector(typeName))
                continue;

            // Component-type INTERSECTION: editing a component only some of the
            // selection carries would silently edit a subset, so hide it entirely.
            // HasComponentByHash, NOT GetComponentByHash: an empty (tag) component
            // has no storage array, so the getter returns null even when present,
            // which used to make every tag component look unshared.
            bool sharedByAll = true;
            for (Astra::Entity e : sel.Entities())
            {
                if (e != primary && !registry.HasComponentByHash(e, ci.descriptor->hash))
                {
                    sharedByAll = false;
                    break;
                }
            }
            if (!sharedByAll)
                continue;

            // Friendly header, raw type in the tooltip: the label should read as
            // words, but the C++ type name is what you search the source for. The
            // hide-list check above and the PushID below both stay keyed on the
            // real type name -- only the drawn string changes. Built here rather
            // than after the PushID because the filter decision needs it.
            const std::string headerLabel = Arcane::Editor::DisplayNameForComponent(typeName);

            // A component survives the filter when its own name matches, or when
            // at least one of its fields does. Deciding BEFORE the header is drawn
            // is what makes a component with no matches disappear rather than
            // render as an empty section; the `continue` lands before the PushID
            // below so the ID stack stays balanced.
            //
            // An empty query matches every component here, so this whole field
            // sweep is skipped outright in the unfiltered case.
            bool componentVisible = Arcane::Editor::ComponentMatchesFilter(headerLabel, query);
            if (!componentVisible)
            {
                for (const Astra::FieldInfo& f : ci.meta->fields)
                {
                    // FieldIsDrawable (InspectorMeta.hpp) is BOTH skips the
                    // visitor applies, so a field this sweep counts is a field
                    // the visitor will actually draw. Missing either one lets a
                    // component whose only matching field fails that skip vote
                    // itself visible and draw an empty body.
                    if (!Arcane::Editor::FieldIsDrawable(f))
                        continue;
                    if (Arcane::Editor::MatchesInspectorFilter(
                            headerLabel, Arcane::Editor::DisplayNameForField(f), f.name, query))
                    {
                        componentVisible = true;
                        break;
                    }
                }
            }
            if (!componentVisible)
                continue;

            // PER-COMPONENT ID SCOPE. Without it every id inside this section is
            // seeded only by the BARE field name (FieldInfo::nameHash), so two
            // components on one entity that share a field name collide --
            // SpriteRenderer::material and PostProcess::material resolved to ONE
            // "##assetpick" popup id, both BeginPopup calls returned true in the
            // same frame, and a pick landed on the wrong field. Scoped for the
            // header too so the context menu (which inherits the header's item id)
            // is distinct even for identically-named component types.
            //
            // Nothing between this push and its pop may `continue`: the branches
            // below are deliberately nested rather than early-outs so the ID stack
            // stays balanced on every path.
            ImGui::PushID(static_cast<int>(ci.descriptor->hash));
            // PropertyGrid::Section scopes the header band tight around the
            // header call only -- the band colors must not leak into the
            // tooltip/popup below, which read the theme's own ImGuiCol_* set
            // like every other popup in the editor (the band pops as Section
            // returns).
            const bool open = pg.Section(headerLabel.c_str());
            // Safe to sit between the header and BeginPopupContextItem below,
            // which resolves its id from g.LastItemData: SetTooltip opens and
            // closes a window, and ImGui restores LastItemData in End()
            // (imgui.cpp:8849), so the popup still inherits the HEADER's id.
            // ForTooltip gates on stationary+delay (style.HoverFlagsForTooltipMouse,
            // imgui.h:1515) so scrolling past headers does not flicker a tooltip per
            // header.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", typeName.c_str());
            // Structure-locked components (identity + derived caches) have no context
            // actions at all, so the popup must not OPEN -- an empty popup frame reads
            // as a glitch. BeginPopupContextItem is self-balancing (imgui.cpp:13333-13344:
            // the window is pushed only when it returns true, and EndPopup runs only
            // inside the branch), so gating the whole block is safe.
            if (!Arcane::Editor::IsStructureLocked(typeName) && ImGui::BeginPopupContextItem())
            {
                if (!canEditStructure)
                    ImGui::BeginDisabled();
                if (ImGui::MenuItem("Remove Component"))
                    pendingRemove = ci.descriptor;
                if (!canEditStructure)
                    ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            if (open)
            {
                // Tag components (Astra's is_empty optimization) have no storage
                // array, so ci.data is null even though the entity carries them.
                // They still get a header: that is what makes them visible at all,
                // and what makes them removable through the menu above.
                if (!ci.data)
                {
                    ImGui::TextDisabled("(tag component -- no fields)");
                }
                else if (!Arcane::Editor::AnyFieldDrawable(ci.meta->fields))
                {
                    // Every reflected field is non-serializable or Hidden, so the
                    // grid below would open, visit nothing and close -- a header
                    // over a void. Arcane::Collider2D WAS the case when this row
                    // was added: its only field, `fixtures`, was Serializable(false)
                    // because the reflection->JSON bridge had no container branch,
                    // and adding a Arcane::Collider2D from the catalog gave NO confirmation
                    // it had done anything. Both halves are gone (2D physics wiring
                    // Plans 1-2: the bridge grew the branch, the Inspector grew
                    // FieldKind::Vector), so Arcane::Collider2D now draws a real list; the
                    // row stays for the next component that reflects only what it
                    // cannot show.
                    //
                    // Says "not editable here" rather than "no fields": the
                    // component genuinely carries state (Arcane::PhysicsSystem2D builds a
                    // body from those fixtures), it just has no authoring surface
                    // in this panel yet. The tag-component line above is the
                    // other statement and they must not be confused.
                    //
                    // Filter-independent by construction (AnyFieldDrawable does
                    // not consult the query), so a component visible only through
                    // a header-name match with no matching fields still falls to
                    // the empty grid below rather than claiming a permanent
                    // property it does not have.
                    ImGui::TextDisabled("(no fields editable here)");
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                        ImGui::SetTooltip("%s carries data, but none of its reflected fields "
                                          "have an Inspector widget yet.", headerLabel.c_str());
                }
                else
                {
                    // Built ONCE per component and re-driven per category:
                    // `activeCategory` is the only member that moves between
                    // the calls below, exactly as it was the only visitor
                    // member the category loop used to re-point. Positional
                    // init in declaration order -- see InspectorView.hpp.
                    ReflectedComponentArgs fieldArgs{
                        registry,
                        ci,
                        primary,
                        sel.Entities(),                     // fan-out targets
                        // Null while Play is running: BeginGestureIfActivated/EndGesture
                        // both early-return on a null stack, so gesture bracketing is
                        // fully inert (no Begin, no Commit/Cancel) against the live
                        // simulating registry.
                        binding.editMode ? &undo : nullptr,
                        project,
                        services,
                        state,
                        // Both outlive the drives below: headerLabel is this
                        // iteration's local and the query views InspectorState's
                        // buffer.
                        headerLabel,                        // componentDisplayName
                        std::string_view{},                 // activeCategory
                        query,                              // filterQuery
                    };

                    // Categories, UE's Details shape: the uncategorised fields
                    // ungrouped FIRST (UE's NoCategory fallback,
                    // DetailCategoryBuilderImpl.cpp:230), then each named
                    // category under its own collapsible sub-header. Named
                    // categories run in first-appearance order over declaration
                    // order, so the ordering is stable and a component still
                    // reads in the Inspector the way it reads in source -- the
                    // fields themselves are never reordered.
                    //
                    // A category is collected only when at least one of its
                    // fields will actually DRAW, which means reproducing every
                    // skip on the way to the widget: FieldIsDrawable (the
                    // visitor's own two, shared with the component sweep above),
                    // plus the live filter. Counting a field the visitor then
                    // drops is what renders a header over an empty body -- the
                    // same defect one level up that the component sweep exists
                    // to avoid.
                    //
                    // The cheap `cat.empty()` rejection leads deliberately: it is
                    // the answer for EVERY field until components carry Category
                    // attributes, and it keeps the filter's display-name build
                    // off that path entirely.
                    std::vector<std::string_view> categories;
                    for (const Astra::FieldInfo& f : ci.meta->fields)
                    {
                        const std::string_view cat = Arcane::Editor::CategoryOfField(f);
                        if (cat.empty())
                            continue;
                        if (!Arcane::Editor::FieldIsDrawable(f))
                            continue;
                        if (!Arcane::Editor::MatchesInspectorFilter(
                                headerLabel, Arcane::Editor::DisplayNameForField(f), f.name, query))
                            continue;
                        if (std::find(categories.begin(), categories.end(), cat) == categories.end())
                            categories.push_back(cat);
                    }

                    // Uncategorised pass -- `activeCategory` is empty by
                    // default. While no field carries a Category attribute this
                    // is the same drive over the same fields as before grouping
                    // existed.
                    //
                    // The grid is opened HERE rather than inside the visitor so
                    // the component header and the category sub-headers below
                    // stay full-width, exactly as UE lays them out; the visitor
                    // only ever emits rows. A FieldGrid that converts to false
                    // is ImGui culling the table, and then no row may be
                    // submitted -- the same "field stopped being drawn" shape
                    // as a collapsed header, which the ScopeGuard above already
                    // covers. The block scopes the grid closed before the
                    // category headers below, which must draw full-width.
                    {
                        PropertyGrid::Rows rows(pg, "##fields");
                        if (rows)
                            DrawReflectedComponent(fieldArgs);
                    }

                    // The view is into the Category attribute's own literal and
                    // is NOT guaranteed NUL-terminated: PropertyGrid::SubSection
                    // pushes it through the begin/end PushID overload and draws
                    // a "%.*s" label rather than a c_str(), so the sub-header's
                    // id comes from that push, not from the drawn text. Its
                    // header band wraps only the call that reads Header/
                    // HeaderHovered/HeaderActive, not the rows below.
                    //
                    // Each category draws its own grid, sharing the panel-wide
                    // label WIDTH (state.grid.labelColWidth) with every
                    // uncategorized grid -- but the visual split does NOT line
                    // up across sections. This grid draws under
                    // TreePushOverrideID's own Indent() call
                    // (imgui_widgets.cpp:7233-7239 -- TreeNodeBehavior pushes it
                    // when the category is open), which advances the cursor by
                    // g.Style.IndentSpacing (imgui.cpp:12246; stock default
                    // 21.0f, imgui.cpp:1538) before this table opens, so a
                    // category grid's border sits one IndentSpacing right of the
                    // uncategorized grids' border. Whether that offset reads
                    // fine is a desk call -- the spec locks category indent to
                    // the tree's own indent rather than fighting it back to 0.
                    for (const std::string_view cat : categories)
                    {
                        if (!pg.SubSection(cat))
                            continue;
                        fieldArgs.activeCategory = cat;
                        {
                            PropertyGrid::Rows rows(pg, "##fields");
                            if (rows)
                                DrawReflectedComponent(fieldArgs);
                        }
                        pg.EndSubSection();
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar(2);

        if (pendingRemove)
        {
            // Copy the selection: ApplyStructural's mutate runs immediately and
            // the span must outlive it.
            const std::vector<Astra::Entity> targets = sel.Entities();
            const Astra::ComponentDescriptor& desc = *pendingRemove;
            ApplyStructural(undo, binding, "Remove Component",
                [&] { return Arcane::Edit::RemoveComponent(registry, targets, desc) > 0; },
                &targets);
        }

        // Bottom of the panel, full width -- the UE/Unity placement.
        ImGui::Separator();
        if (!canEditStructure)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_LC_PLUS " Add Component", ImVec2(-FLT_MIN, 0.0f)))
            ImGui::OpenPopup(kAddComponentPopup);
        if (!canEditStructure)
            ImGui::EndDisabled();
        // Drawn unconditionally at window scope: BeginPopup is a no-op until
        // the button above (or a previous frame's click) opened it.
        DrawAddComponentPopup(registry, sel.Entities(), undo, binding);
    }
}
