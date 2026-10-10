#pragma once
// EditorApp: the editor application. Constructed in main from a HostConfig
// (shared with ArcaneRuntime); Run() drives Init -> the frame loop -> Shutdown.
// Member declaration order m_gpu -> m_runtime -> m_plugin is the TEARDOWN
// CONTRACT (destruct reverse: plugin Unload while the DLL is still mapped ->
// runtime -> render stack in GpuContext -> window last). Mirrors ArcaneRuntime.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/GpuContext.hpp>
#include <Arcane/Host/FramePerf.hpp>
#include <Arcane/Host/BootSequence.hpp>
#include <Arcane/Host/BootSplashWindow.hpp>
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Host/OffscreenVehicle.hpp>   // the --headless chrome vehicle (m_offscreenChrome)
#include <Arcane/Host/SettleBound.hpp>        // --settle's bail conjunction: SettleBail (mirrors RuntimeApp.hpp)
#include <Arcane/Host/ReferenceImages.hpp>    // --compare/--bless (Task 9): ReferenceResolution
#include <Arcane/Host/VerifyReport.hpp>       // --report (Task 9): VerifyReport
#include <Arcane/Assets/ImageCompare.hpp>     // --compare (Task 9): PixelData/ImageCompareResult
#include "Panels/AssetActivityLog.hpp"
#include "Panels/AssetBrowserPanel.hpp"   // AssetBrowserPanelState (m_assetBrowserUi)
#include "Panels/AssetGraphPanel.hpp"     // AssetGraphPanelState (m_assetGraphUi) + the canvas teardown seam
#include "Panels/AssetPanelModel.hpp"
#include "Panels/AssetReferenceField.hpp"   // m_assetRefServices
#include "Panels/AssetStatusPanel.hpp"    // DrawAssetStatusPanel -- Status carries no state of its own
#include "Panels/ConsoleBuffer.hpp"
#include "Settings/EditorConsoleSettings.hpp"   // editor.console.ringLines (the ring's boot capacity)
#include <Arcane/Config/Settings.hpp>
#include "Panels/CreateAssetDialog.hpp"
#include "Panels/DiagnosticStore.hpp"
#include "App/DialogSlot.hpp"
#include "App/InputEdges.hpp"
#include "App/ModalErrorQueue.hpp"
#include "App/UndoSettings.hpp"           // m_undoLimitsApplied + ReadUndoLimits (editor.undo.*)
#include "Scene/UndoGate.hpp"             // ResolveDocumentUndo (DocumentUndo)
#include "Documents/DocumentHost.hpp"
#include "Viewport/EditorCamera.hpp"
#include "Viewport/ViewportSettings.hpp"
#include "Panels/EditorPanels.hpp"
#include "Settings/AppearanceApplier.hpp"
#include "Settings/ProjectSettingsPage.hpp"
#include "Settings/FontsPage.hpp"
#include "Settings/LayoutPage.hpp"
#include "Settings/ShortcutsPage.hpp"
#include "Settings/ThemePage.hpp"
#include "Panels/InspectorHost.hpp"          // m_inspectorHost (inspector ownership)
#include "Panels/InspectorWindows.hpp"       // m_inspectorWindows
#include "Panels/LocatorRoute.hpp"           // RouteFacts (MakeRouteFacts, s8.2)
#include "Panels/AssetInspectorSource.hpp"   // m_assetSource
#include "Panels/AssetFileOpDialogs.hpp"     // m_renameModal (T5 s7.6)
#include "Panels/SceneInspectorSource.hpp"   // m_sceneSource
#include "Project/AssetFileOps.hpp"      // AssetFileOpHost/AssetFileOpExecutor (m_assetOpHost, m_assetFileOps)
#include "Project/CookQueue.hpp"
#include "Project/IdeLaunch.hpp"         // IdeLaunch::Outcome (OpenInIde's return)
#include "Project/OsShell.hpp"           // OsShell::RecycleResult (AssetOpHost::Recycle)
#include "Project/MaterialPreviewHarvester.hpp"   // owned by value-in-unique_ptr (m_materialThumbs)
#include "Project/ModuleBuild.hpp"
#include "Project/ServerLaunch.hpp"   // ServerProcess is a BY-VALUE member (m_serverProcess)
#include "Project/StartPageModel.hpp"   // StartPageFocus (m_startPageFocus, spec 2026-09-30 s8.4)
#include "App/PlayMode.hpp"
#include "App/EditorTitle.hpp"         // TitleParts (CurrentTitleParts)
#include "Panels/ProblemsPanel.hpp"
#include "Project/EditorRecents.hpp"
#include "Scene/EditModeSchedule.hpp"
#include "Scene/SceneSession.hpp"
#include "Scene/SelectionContext.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Viewport/DeferredPick.hpp"
#include "Viewport/ViewportInput.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/ImGui/OffscreenImGuiLayer.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Render/GpuFaultInjector.hpp>   // dev-only Build -> Diagnostics -> Crash GPU (kPassName only; the injector is NriDiagnostics::FireFault)
#include <Arcane/Render/GpuSceneTypes.hpp>   // GpuSceneFrame -- the viewport's per-frame GPU-scene frame (F3 plan 1 T8)
#include <Arcane/Render/Nri/NriGraphContext.hpp>   // the graph vehicle (chrome + viewport); unconditional
#include <Arcane/Render/PickEmit.hpp>   // PickDrawable -- the frame's pickables
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <spdlog/sinks/callback_sink.h>

namespace Arcane { struct InputSnapshot; }   // by-reference phase parameters only
// F2c Task 15: MintImportMaterials' survey parameter -- opaque here, the full
// definition lives in Arcane/AssetPipeline/GltfSurvey.hpp, included only where the
// struct is actually read (EditorAppProject.cpp).
namespace Arcane::AssetPipeline { struct GltfSurvey; }
// Opaque here on purpose: the play-mode settings handler (Task 6) is the only
// thing in this header that needs these names, and ImGui's ini extension
// point (ImGuiSettingsHandler) is internal-only -- imgui_internal.h stays
// confined to EditorApp.cpp, never leaking into this header. ImGuiContext is
// forward-declared in the public imgui.h too; ImGuiTextBuffer is a complete
// type there, but only a pointer to it appears below, so this is enough.
struct ImGuiContext;
struct ImGuiSettingsHandler;
struct ImGuiTextBuffer;

namespace Arcane::Editor
{
    class EditorApp
    {
    public:
        // `splash` is a NON-OWNING pointer to main()'s stack-local
        // Arcane::BootSplashWindow (Task 8, async-boot arc): main constructs
        // and ultimately closes it (BootSplashWindow's ordering contract
        // requires both from "main" -- never under the loader lock, never from
        // two threads at once); EditorApp only reads it during Run()'s boot
        // sequence and calls Close() once, synchronously, from the
        // splash_ready stage (still on main's call stack -- see Run()). Null
        // is tolerated (no splash to hand off).
        explicit EditorApp(HostConfig cfg, Arcane::BootSplashWindow* splash = nullptr);
        int Run();   // BootSequence -> MainLoop() -> Shutdown(); process exit code

        // ---- The clean-exit hook (crash window plan 1, task 9; spec S5.7) ---
        // "Begin your ordinary exit now", for the paths the OS gives seconds
        // rather than a frame: Ctrl-C, the console close box, logoff and
        // WM_ENDSESSION all route into Diagnostics::RequestCleanExit, which
        // calls the one hook the host installed.
        //
        // STATIC, and installed from main() BEFORE this object exists, for two
        // reasons: the window it has to cover starts at Install (the whole
        // boot is in it -- a project scan is exactly where a session-end can
        // land), and the hook runs on WHATEVER thread noticed (the OS's
        // console-handler thread), which may touch nothing thread-affine. So
        // it sets one atomic that PumpFrameEvents reads at the top of the
        // frame, which is the same "request the ordinary exit" the window's
        // close box performs. Autosave-first behaviour arrives in plan 3.
        static void InstallCleanExitHook() noexcept;

        // Whether that hook has fired. Read by PumpFrameEvents; a plain
        // relaxed load per frame.
        [[nodiscard]] static bool CleanExitRequested() noexcept;

    private:
        // ---- Boot (EditorApp.cpp) -------------------------------------------
        // Run() builds Arcane::HostBoot::EditorStages(ctx) -- the SAME shared
        // function RuntimeApp calls for its own list -- then overwrites the
        // ids below with these closures, because their real work touches
        // EditorApp's own private members (m_gpu/m_runtime/m_plugin/...) or
        // editor-exe-only types (EditorTheme/EditorFonts/ShaderEditorDocument)
        // that Arcane.dll cannot see. type_context_install/project_open/
        // input_config/editor_lock are NOT in this list -- their
        // CoreStages/EditorStages body is genuinely shared and used as-is; see
        // ProjectBoot.cpp for why each stage landed where it did. Every method
        // returns false only where the equivalent Init() block used to.
        //
        // splash_ready IS in this list: it touches host-owned splash members
        // that ProjectBoot.cpp's ctx-only lambda cannot reach, exactly the
        // same structural reason render_bridge/plugin_load and the rest are
        // host-owned.
        // EditorStages() installs Make()'s Unpatched(id) sentinel for
        // splash_ready's `run` (empty by default) for the same reason it does
        // for every other host-owned id -- a host that forgets to patch it
        // fails loudly instead of quietly skipping the reveal.
        bool StageRuntimeCreate(Arcane::HostBoot::BootContext& ctx);
        bool StageGpuCore(Arcane::HostBoot::BootContext& ctx);
        bool StageEditorFonts(Arcane::HostBoot::BootContext& ctx);
        bool StageEditorShell(Arcane::HostBoot::BootContext& ctx);
        bool StageRenderBridge(Arcane::HostBoot::BootContext& ctx);
        bool StageEditCore(Arcane::HostBoot::BootContext& ctx);
        bool StageSpriteTables(Arcane::HostBoot::BootContext& ctx);
        bool StagePluginLoad(Arcane::HostBoot::BootContext& ctx);
        bool StageFinalize(Arcane::HostBoot::BootContext& ctx);
        bool StageSplashReady(Arcane::HostBoot::BootContext& ctx);

        enum class InitResult
        {
            Ok,       // boot completed; run the frame loop
            Failed,   // a Fatal stage failed, or the patch table drifted -> exit 1
            Quit,     // the user closed the splash mid-boot -> exit 0, not an error
        };

        // THE host-owned stage patch (architecture pass sec 5): Create() applies it
        // to the boot list; SwitchProject (Task 12) applies it to the same shared
        // EditorStages(ctx) list before cherry-picking its subset -- ONE patch
        // path, both directions of drift still fail loudly (see the table's own
        // comment in the .cpp). False = a patch id matched no stage -- table
        // drift; the caller must fail loud (Create() returns false; a future
        // SwitchProject call is expected to do the same).
        bool PatchHostStages(std::vector<Arcane::BootStage>& stages);

        bool       Create();
        InitResult Init();
        // THE RENDER VEHICLES' CREATE, run by Main() between boot and the
        // frame loop. It sits HERE rather than in a boot stage for an ordering
        // reason proved at three desk checkpoints: the window is Show()n FIRST
        // and the swapchain built over an ALREADY-MAPPED window, because a
        // surface created against a window the compositor has never seen is a
        // backend-specific corner a desk-only machine cannot pre-clear. False
        // = already logged; Main() turns it into exit 1.
        bool       CreateGraphVehicles();

        // THE LIVE CHROME CONTEXT, resolved in ONE place so no call site
        // branches on --headless to find it. Exactly one of the two members
        // below is ever non-null (CreateGraphVehicles builds one or the
        // other): m_graphChrome, the host-window context with a swapchain over
        // m_gpu->Win(), or m_offscreenChrome, an OffscreenVehicle that owns its
        // own device and has NO window handle and NO swapchain anywhere in it.
        // Deliberately the same shape as RuntimeApp::Graph(), and for the same
        // reason: an `if (offscreen) ... else ...` at each of the ~25 use sites
        // is how the two modes silently drift apart.
        //
        // Null only before CreateGraphVehicles runs and after
        // ShutdownGraphPath -- every guard that used to read `if
        // (m_graphChrome)` reads `if (ChromeGraph())` instead.
        [[nodiscard]] Arcane::NriGraphContext* ChromeGraph() const noexcept;
        // The VIEWPORT context and every seam it needs, with TWO callers:
        // CreateGraphVehicles at boot, and SwitchProject's
        // "render_bridge" stage on every project switch -- which rebuilds this
        // context from scratch rather than flushing it (TeardownGraphForSwitch
        // states the kept-vs-torn decision in full). Shared rather than copied
        // because three of its four statements fail SILENTLY when missed: a
        // missing AdoptImGuiContext is a blank plugin HUD, a missing resolver
        // or pixel supply is a white texel on every sprite. False = already
        // logged; both callers turn it into a loud stop.
        bool       BuildGraphViewportContext(std::uint32_t width, std::uint32_t height);
        int        Main();
        void       Shutdown();
        void       Destroy();

        void MainLoop();

        // ---- Frame loop (EditorAppFrame.cpp) --------------------------------
        // MainLoop is a straight sequence of the phase methods below, called in
        // exactly the order the statements inside them ran in when this was one
        // 1100-line function. That order is LOAD-BEARING and has NO automated
        // coverage -- EditorApp*.cpp is not compiled into ArcaneTests, so a green
        // suite proves nothing here. Each phase's definition carries the ordering
        // constraint it participates in; do not reorder the calls in MainLoop.

        // What PumpFrameEvents decided the rest of the frame should do: the
        // original block's fallthrough / `continue` / `break` respectively.
        enum class FramePump { Continue, SkipFrame, Exit };

        // Frame-loop state that outlives ONE frame -- these were MainLoop's
        // locals declared OUTSIDE the while loop, and MainLoop still owns the
        // single instance. Deliberately not EditorApp members: the lifetime is
        // exactly the MainLoop call, as before.
        struct LoopState
        {
            std::chrono::steady_clock::time_point simPrev{};
            std::chrono::steady_clock::time_point lastFrameTime{};
            bool running = true;
            // The answer the confirm modal resolved, carried from the ImGui pass of one
            // frame to the top of the next. The modal cannot perform the action where the
            // button is clicked: OpenProject tears down the plugin and closes documents
            // whose textures the frame's already-built draw lists still reference, and
            // ImGui replays those lists at Render time.
            Arcane::Editor::SceneSession::PendingRequest sceneAction;
        };

        // Values one phase computes and a LATER phase of the SAME frame reads.
        // These were locals declared at the top of the while body; MainLoop
        // declares one instance per iteration, so they are as fresh as before.
        struct FrameState
        {
            // Set inside the input phase once inViewport + the game context's
            // last-frame WantCaptureMouse are known; outlives that phase
            // so the click-pick further down this same frame can also honor it.
            bool gameUiClaims = false;
            // File-menu scene shortcuts, raised in the input phase and folded
            // into this frame's MenuRequests at the menu-request site (one launch
            // site per verb) so the keybind and the menu item cannot
            // drift apart. Same shape for the Edit-menu clipboard shortcuts
            // (Ctrl+X/C/V/D) below.
            bool scNewScene = false, scOpenScene = false, scSaveScene = false;
            bool scCut = false, scCopy = false, scPaste = false, scDuplicate = false, scCloseDocument = false;
            // This frame's Viewport panel result, read by the click-pick phase.
            Arcane::Editor::ViewportPanelResult vp{};
        };

        FramePump PumpFrameEvents();
        // editor.perf.backgroundFps: when the previous frame's pump ran, so an
        // unfocused frame can be held to the configured rate.
        std::chrono::steady_clock::time_point m_lastFramePump{};
        void RunSceneAction(const Arcane::Editor::SceneSession::PendingRequest& req,
                            LoopState& ls);
        void ConsumeDeferredSceneAction(LoopState& ls);
        void ConsumeSceneDialogResults(LoopState& ls);
        void ConsumeProjectDialogResult();
        void ConsumeMaterialDialogResults();
        void ConsumeSettingsDialogResults();
        void ApplyAppearanceSettings();   // S4: theme/UI cvars -> the editor style, outside the ImGui frame
        void ApplyPendingLayoutRequest();
        void RepublishGameInput(const Arcane::Guid& asset, const Arcane::InputActionAsset& parsed);
        [[nodiscard]] bool IsDesignatedInputAsset(const Arcane::Guid& asset);   // the open project's manifest inputActions names `asset`
        void FrameInput(LoopState& ls, FrameState& fs);
        void HandleUndoRedoAndSceneShortcuts(FrameState& fs);
        void HandleGizmoModeKeys(const Arcane::InputSnapshot& snap);
        void UpdateEditorCamera(const Arcane::InputSnapshot& snap, bool inViewport,
                                float lx, float ly, float dt);
        void UpdateGizmoInteraction(const Arcane::InputSnapshot& snap, bool inViewport,
                                    float lx, float ly, bool gameUiClaims);
        void AdvanceSim(LoopState& ls);
        void ApplyPendingViewportResize();
        void RefreshSceneResolution();
        void RenderSceneToViewport();
        // The scene's 2D submission plus the two Edit-mode overlays drawn on
        // top of it (the scene-camera rect and the transform gizmo), against
        // whichever batcher the caller hands in -- in practice the
        // device-less GpuContext batcher RenderSceneToViewport Begin()s
        // itself. Contains no target, no command list and no device, which is
        // what keeps the ORDER of its statements the whole of its contract.
        void SubmitSceneToBatcher(Arcane::Batcher2D& b);
        // Phase 11's CPU HALF: the game context's
        // per-frame input, and -- in Play, with a plugin that draws one --
        // its BeginFrame + DrawUIAll. Returns true when a game-UI frame was
        // BEGUN and therefore owes exactly one Render / RenderToDrawData /
        // EndFrameDiscard; false when nothing was begun.
        //
        // Its own function so the statements and their ORDER are stated in
        // one place: RenderToDrawData()'s output goes to a graph node in phase
        // 10. Same discipline, and the same reason, as SubmitSceneToBatcher.
        [[nodiscard]] bool BeginGameUiFrame();
        // The graph arm's FrameDesc arming for the pick + outline chain and the
        // game HUD -- i.e. everything phases 11, 12 and 17 contribute to a frame
        // that is DECLARED in phase 10. Fills the members the FrameDesc's spans
        // point at (m_pickDrawables / m_pickSelectedIds) and returns nothing by
        // value, because a span into a temporary is the one mistake this shape
        // exists to make impossible.
        void ArmGraphViewportFrame(Arcane::NriGraphContext::FrameDesc& vp);
        // NO phase 11 / phase 12 members. CompositeGameUi and
        // RenderSelectionOutline were declared here and called from MainLoop
        // between phases 10 and 13; ArmGraphViewportFrame above satisfies both
        // one phase earlier, inside the frame phase 10 declares. See its
        // definition, and the phase-11/12 note at that call site in MainLoop.
        void PumpEditorDocuments();
        void DrawEditorUi(LoopState& ls, const FrameState& fs);
        void ConsumeMenuRequests(Arcane::Editor::MenuRequests& menuReq,
                                 const FrameState& fs, LoopState& ls);
        // Start page (spec 2026-09-30 s8.4): drawn every frame no project is
        // open, tabbed into the Viewport's node. Its buttons and rows fill `req`.
        void DrawStartPage(Arcane::Editor::MenuRequests& req);
        // THE launch site for Open Project / Open Folder / Open Recent, shared by
        // the File menu (ConsumeMenuRequests) and the start page (drawn after it).
        void LaunchProjectOpenRequests(const Arcane::Editor::MenuRequests& req);
        // Asset-manager redesign, Plan 1 Task 9: takes AssetPanelActions
        // (the panels' own action-report contract); the old AssetBrowserActions
        // overload (superseded when AssetBrowser.* was retired, Task 15) is
        // gone with the call site that produced it. Panel-split Task 7
        // renamed it from ConsumeBrowserActions: consumption is identical per
        // value and the host now runs it over THREE returned values (Browser,
        // Graph, Status, in draw order), so naming it after one panel had
        // stopped being true.
        void ConsumeAssetPanelActions(const Arcane::Editor::AssetPanelActions& panelActions,
                                      LoopState& ls);
        // ---- Unified create (asset-manager redesign, Plan 1 Task 12) -------
        // THE INVARIANT (spec s7): no creation path may bypass
        // CreateAssetRequest. These two functions are the whole of it --
        // BeginCreateAsset is the ONE place a create dialog opens, and
        // ConsumeCreateResult the ONE place a create dispatches to a mint.
        // Every producer (the Assets menu, the panel's + Create popup, the
        // rail's per-kind +, a row's Create submenu, the Asset page's New
        // Instance..., and Plan 3's graph pin-drag) raises a request and stops
        // there; none of them opens a dialog or writes a file.
        void BeginCreateAsset(const Arcane::Editor::CreateAssetRequest& request);
        void ConsumeCreateResult(const Arcane::Editor::CreateAssetResult& result);
        void DrawModals(LoopState& ls);
        void DrawViewportPanelPhase(FrameState& fs);
        void HandleViewportPick(const FrameState& fs);
        void DrawSelectionPanels();
        bool PresentFrame();
        // Phase 19: the editor's main-window frame as a graph frame --
        // clear + one ImGui node over the EDITOR
        // context's draw data -> the chrome context's backbuffer -> present.
        // Same return contract as PresentFrame (false = nothing was presented,
        // MainLoop skips the rest of the frame). Its own definition carries the
        // frame's declared shape and the four fields deliberately left default.
        bool PresentChromeFrame();
        // NO SEPARATE CAPTURE MEMBER: the viewport capture is armed as a NODE
        // inside RenderSceneToViewport's own FrameDesc and read back there.
        // The recorder has no standalone "read this texture" entry point, so
        // piggybacking on the already-declared frame is the only shape that
        // exists.
        //
        // --settle N (Task 9): also where the WHOLE LOOP decides it is done,
        // once the base --frames budget is spent -- NOT `m_frameCount >=
        // maxFrames` alone any more (that stays the whole story only when
        // --settle was never requested). PresentChromeFrame's settle branch
        // owns readback attempts and counts them off m_settleAttemptsUsed,
        // never off this function's frame counter (a Skipped chrome
        // present -- routine under --headless too -- does not reach here
        // at all, so tying attempts to frame advancement can spin forever;
        // see PresentChromeFrame's own comment). This is what makes "arm
        // every frame past the base budget" and "arm the one last frame"
        // the same expression there, exactly like RuntimeFrame::RenderGraph's
        // willBeLastFrame.
        void EndFrame(LoopState& ls);

        // ---- THE EXIT-CODE FOLD ---------------------------------------------
        // The editor's counterpart of RuntimeApp::m_graphExit + its
        // ShutdownGraphPath, and deliberately the same codes and the same
        // precedence (1 > 2 > 3 > 4): 1 = a graph frame FAILED (it says WHERE
        // the run died), 2 = RenderErrorCount GREW across the run, teardown
        // included, 3 = --settle N never converged (Task 9), 4 = --compare
        // named a missing/undecodable reference (fail-fast, before the loop
        // even starts) or a converged --bless run failed to WRITE its
        // reference -- see main.cpp's exit-code table for the full accounting
        // and the deliberate reuse of 3 against the pre-boot double-open
        // guard's own code.
        //
        // Both CONTEXTS fold into the one code -- a viewport frame that could
        // not be recorded (RenderSceneToViewport, phase 10) and a chrome frame
        // that could not be presented (PresentChromeFrame, phase 19) are the
        // same class of failure and neither was visible in an exit code before.
        void NoteGraphFrameFailure(const char* what);
        // ---- The project switch's render teardown ---------------------------
        // SwitchProject's "switch_teardown" stage: an ordered idle ->
        // invalidate -> release (whose own drain closes the sequence), with
        // the chrome context and m_gameImgui deliberately KEPT and the
        // viewport context deliberately DESTROYED. The full kept-vs-torn
        // decision, the reason a content flush is not an option, and the
        // adjacency rule that makes the ordering safe are all stated at the
        // definition -- read it before changing either end of the switch.
        // Hands back the outgoing viewport extent so the rebuild in
        // "render_bridge" can recreate at the size the panel is actually
        // showing; 0/0 means "use the boot default".
        void TeardownGraphForSwitch(std::uint32_t& keepWidth, std::uint32_t& keepHeight);
        // Destroys BOTH graph contexts, in the one order that is correct, and
        // then reads the latch back -- which is the whole reason it exists as a
        // function rather than as member destruction: a teardown-only
        // validation error must still fail the run, and member destructors run
        // after Run() has already returned its code. Mirrors
        // RuntimeApp::ShutdownGraphPath.
        void ShutdownGraphPath();

        HostConfig                        m_config;

        // DECLARED HERE FOR ITS LIFETIME, NOT ITS SUBJECT. The semantics live
        // with RetargetLayoutIni() far below; this is the only reason the
        // declaration is not down there beside them.
        //
        // ImGui BORROWS this string -- `io.IniFilename = m_layoutIniPath.c_str()`
        // and ImGui never copies it. ImGui::Shutdown(), reached from
        // ~ImGuiLayer which m_gpu OWNS, then calls SaveIniSettingsToDisk() on
        // that borrowed pointer. Members destruct in REVERSE declaration order,
        // so this string MUST be declared ABOVE m_gpu to outlive it. Declared
        // below m_gpu it dies first and the save reads freed heap: a real
        // use-after-free that only faults once the block has been reused, which
        // is why it surfaced as an INTERMITTENT 0xC0000005 in MultiByteToWideChar
        // at editor teardown -- about one run in twelve -- rather than as a
        // reliable crash.
        //
        // Same discipline as m_config above, which m_bootCtx borrows
        // projectPath/pluginPath from for exactly the same reason.
        std::string                       m_layoutIniPath;

        std::unique_ptr<GpuContext>       m_gpu;                    // destructs LAST, except
                                                                    // m_config and m_layoutIniPath
                                                                    // which outlive it BY DESIGN
                                                                    // (see the note above)

        // ---- The render vehicles --------------------------------------------
        // THE CHROME CONTEXT: the host-window NriGraphContext. It owns the
        // process's ONLY graphics device (GpuContext::Create builds none),
        // arms the crash chain, and its swapchain binds m_gpu's window --
        // exactly what RuntimeApp::m_graphContext is. LIVE ON EVERY RUN, in
        // every configuration including Dist -- this is null only on a failed
        // boot (CreateGraphVehicles' `if (!ChromeGraph())` returns false
        // before its caller proceeds).
        //
        // IT DRAWS THE EDITOR'S CHROME FRAME (clear + one ImGui node over the
        // editor context's draw data -> backbuffer -> present), and it carries
        // two further loads:
        //   * it is what CREATES the shared NriDevice the viewport context
        //     below borrows, and
        //   * ImGuiHud() is the backend that caches the viewport's output
        //     texture, so it is the node the resize contract's
        //     InvalidateUserTextureNow must be called on -- see
        //     ViewportTargets::ApplyPendingResize.
        //
        // NOT #if-guarded, for exactly the reason RuntimeApp.hpp
        // states for its own member: the type is compiled into the engine DLL
        // in every configuration, and a preprocessor-guarded MEMBER would
        // force every use site in the frame body to grow a guard of its own --
        // which is how a Dist-only compile break gets introduced. The
        // CREATION is unconditional too; there is no configuration where this
        // carries a null pointer on an ordinary run.
        //
        // Declared AFTER m_gpu so it destructs BEFORE it, and that ordering is
        // LOAD-BEARING: this object's swapchain is bound to the window inside
        // m_gpu (NriGraphContext.hpp, THE BORROWED WINDOW). It is also declared
        // BEFORE m_viewportTargets, whose offscreen context BORROWS the device
        // this one owns -- so the borrower (declared later) destructs first.
        //
        // NOT on ResetPerProjectState's list, and neither is the viewport
        // context below: these hold DEVICE resources, not project ones, and
        // the device outlives a project switch.
        //
        // ===== AND THIS ONE SURVIVES A SWITCH OUTRIGHT =====
        // The decision, made and recorded rather than left implicit: THIS
        // context is window/device/session-scoped and a project switch does
        // not touch it, while the VIEWPORT context below is project-scoped in
        // its caches and is destroyed and rebuilt by every switch. Nothing
        // this object holds can name an asset of the outgoing project -- its
        // NriTextureCache's only possible tenant is the toolbar logo, whose
        // pixel supply answers exactly one synthetic per-run Guid and null for
        // everything else (CreateGraphVehicles), and it is handed no asset
        // resolver at all. What a switch DOES owe it is the ordered idle ->
        // invalidate -> release, with the invalidate made ON THIS OBJECT'S
        // ImGuiHud() for every texture the switch is about to destroy: it is
        // the backend that caches the viewport output and the document
        // previews by raw pointer. EditorApp::TeardownGraphForSwitch carries
        // the whole sequence and the argument for the split.
        std::unique_ptr<Arcane::NriGraphContext> m_graphChrome;

        // THE --headless TWIN of m_graphChrome, and the ONLY one of the pair
        // that is non-null under that flag. It owns a native device + NRI wrap
        // + an offscreen NriGraphContext (OffscreenVehicle), so it is a
        // strictly LARGER object than m_graphChrome -- which merely borrows the
        // device GpuContext made. Everything downstream reaches whichever one
        // is live through ChromeGraph().
        //
        // DECLARED IMMEDIATELY AFTER m_graphChrome so the two are adjacent in
        // reverse-destruction order: every member that BORROWS the chrome
        // device (m_retiredDocPreviews, m_documents, m_viewportTargets.graph)
        // is declared below and is therefore destroyed first, which is the
        // same guarantee m_graphChrome's own comment relies on.
        std::unique_ptr<Arcane::OffscreenVehicle> m_offscreenChrome;

        // ---- Closed documents' preview vehicles ------------------------------
        // A shader document owns an offscreen NriGraphContext, and it is
        // destroyed INSIDE the editor's ImGui pass
        // (DocumentHost::DrawAll erases the unique_ptr right after its draw
        // loop) -- while this frame's draw lists still name its output texture
        // by raw pointer and the chrome frame that replays them has not been
        // recorded. So the dying document hands the vehicle here instead, and
        // DrainRetiredDocPreviews destroys it at the TOP of the next frame,
        // invalidate first. Full reasoning: DocServices::retireGraphPreview.
        //
        // DECLARED HERE, AND THE POSITION IS THE POINT (fix round 1). It has to
        // sit BETWEEN m_graphChrome and m_documents, because reverse-order
        // destruction then runs:
        //     ~m_documents  ->  ~m_retiredDocPreviews  ->  ~m_graphChrome
        // i.e. a document destroyed by ~EditorApp can still push its vehicle in
        // here, and every vehicle in here still dies before the context that
        // owns the device it borrows. Declared after m_documents (where it
        // first was) the vector would be GONE by the time ~ShaderEditorDocument
        // pushed into it.
        //
        // WHAT THAT ORDER DOES NOT PROVIDE, stated rather than glossed: ~vector
        // destroys a vehicle WITHOUT the chrome-side InvalidateUserTextureNow,
        // which only DrainRetiredDocPreviews does. That is not a live hole --
        // a vehicle exists only if MakeDocServices saw m_graphChrome, nothing
        // nulls it before Shutdown, and ShutdownGraphPath therefore always runs
        // its CloseAll + drain -- so the destructor path is unreachable with a
        // non-empty vector. The declaration order is what keeps it SAFE rather
        // than merely unreached.
        //
        // THE PROJECT SWITCH IS THE SECOND CloseAll + drain PAIRING:
        // ResetPerProjectState's CloseAll fills this list mid-switch, and
        // TeardownGraphForSwitch drains it in the same stage -- not at the next
        // frame's phase 13, which is across a plugin unload and a project open.
        std::vector<std::unique_ptr<Arcane::NriGraphContext>> m_retiredDocPreviews;
        void RetireDocPreview(std::unique_ptr<Arcane::NriGraphContext> vehicle);
        void DrainRetiredDocPreviews();

        // The graph arm's own exit code, 0 on every ordinary run -- see
        // NoteGraphFrameFailure / ShutdownGraphPath above and Run()'s tail,
        // which is where it reaches the process. Not #if-guarded, for the same
        // reason m_graphChrome is not: a preprocessor-guarded member forces a
        // guard at every use site.
        int           m_graphExit = 0;
        // RenderErrorCount at the moment the chrome context was created --
        // BOOT-time errors belong to the boot, and everything from there until
        // the last NRI object is gone belongs to the graph. Read back in
        // ShutdownGraphPath, which prints the pair as one line so the desk
        // watch item ("the count must not grow across a clean exit") is legible
        // rather than inferred.
        std::uint64_t m_graphErrorBaseline = 0;


        // The pre-device splash: non-owning, see the ctor's doc comment. It is
        // BootSequence::Run's presenter for the WHOLE boot, via
        // m_splashPresenter below -- a MEMBER constructed from this pointer,
        // see that member's own note for why it cannot be a local. There is no
        // swapchain-backed presenter at all: the splash is the only one.
        Arcane::BootSplashWindow*             m_splash = nullptr;
        // A class member, not a Run()-local (2026-07-30 review round 2,
        // finding 2): StageSplashReady needs to call Disarm() on THIS exact
        // instance right before it closes the splash itself, and Disarm()
        // has to reach it through `this`, not through BootSequence's opaque
        // IBootPresenter*. Declared right after m_splash (constructed from
        // it, in declaration order, in the ctor's init list below) -- has no
        // device dependency, so unlike m_presenter it can exist for the
        // App's whole lifetime rather than being emplaced late.
        Arcane::BootSplashPresenter           m_splashPresenter;

        // Captured at the end of StageGpuCore (right after GpuContext::Create
        // has created the editor's ImGuiLayer, which is the only ImGui
        // context in existence at that point) and checked against
        // ImGui::GetCurrentContext() at the top of StageEditorShell
        // (2026-07-30 review, Fix 1's verification requirement): a
        // permanent regression tripwire for the exact bug this fix closes --
        // if some future boot-stage reordering lets plugin_load (or anything
        // else that can call ImGui::SetCurrentContext) run before
        // editor_shell again, this fires immediately with a clear cause
        // instead of degrading into wrong fonts/theme and a much later,
        // much harder to diagnose DockBuilderAddNode crash.
        ImGuiContext*                          m_editorImguiContext = nullptr;

        // The hosted plugin's OWN ImGui context, rendered INTO the viewport's
        // output texture (Unity/Unreal "game view") so the plugin's debug HUD
        // lives inside the Viewport panel, never over the editor chrome. Created
        // in Init after the editor ImGui layer is up; the plugin is pointed at it
        // via ClientRuntime::SetImGui (in place of the editor context). Declared after
        // m_gpu (destructs BEFORE it -- see below for the real reason this
        // ordering matters) and before
        // m_runtime/m_plugin (destructs AFTER the
        // plugin -- the plugin holds this ctx via SetImGui and may touch ImGui
        // during Unload/Shutdown, so this must outlive it). Its destructor
        // restores the editor context, so ~GpuContext's ImGuiLayer teardown stays
        // valid. See BeginGameUiFrame, phase 11's surviving CPU half.
        //
        // NON-NULL UNCONDITIONALLY: the single OffscreenImGuiLayer::Create
        // call at StageRenderBridge is the only path here.
        //
        // WHY IT IS ITS OWN CONTEXT AT ALL: the EDITOR's context has
        // io.IniFilename pointed at the PER-PROJECT LAYOUT FILE, where this
        // one's is deliberately null. A plugin window submitted through the
        // editor's context would be persisted into the user's layout,
        // permanently, and per-id ini state silently overrides authored UI on
        // every later boot. The isolation is structural -- own context, own
        // io, own pinning discipline -- not held by control flow.
        //
        // ===== AND ITS POSITION IN THIS LIST IS LOAD-BEARING =====
        // The viewport context's game ImGuiNriNode ADOPTS
        // this context, and ImGuiNri::Release PINS the adopted context to walk
        // its platform texture list -- which is a DEREFERENCE. So THIS OBJECT
        // MUST OUTLIVE THAT NODE'S Release. It does, and by construction rather
        // than luck: it is declared here, far ABOVE m_viewportTargets (which
        // owns the offscreen NriGraphContext), so reverse-order destruction
        // runs ~NriGraphContext -- and the Release inside it -- while this is
        // still alive. Do not move either declaration past the other.
        //
        // THE PROJECT SWITCH DISCHARGES THE SAME ORDERING EXPLICITLY, in both
        // halves: switch_teardown (EditorApp::TeardownGraphForSwitch) destroys the
        // VIEWPORT context -- and therefore that node's Release, which PINS
        // this context -- WITHOUT touching this member, which is why nothing
        // in that function resets it; and the rebuild re-issues
        // ImGuiNriNode::AdoptImGuiContext on the new node
        // (BuildGraphViewportContext), without which the HUD goes silently
        // blank after the first switch. The full obligation is stated on
        // ImGuiNri's m_imguiContext member.
        std::unique_ptr<Arcane::OffscreenImGuiLayer> m_gameImgui;

        std::unique_ptr<Arcane::ProcessContext> m_process;          // the process's ONE (spec s3); declared before m_runtime so it outlives it
        std::optional<Arcane::ClientRuntime> m_runtime;             // destructs before m_gpu

        // ===== THE PLAY SESSION SITS BETWEEN m_runtime AND m_plugin ==============
        // Declaration order IS the teardown contract here (final-review fix wave,
        // I2). Since plan 1 Task 7 m_play OWNS a Runtime -- the embedded
        // DedicatedServer world -- and PluginHost keeps a RAW pointer to every world
        // it serves for the whole attach. So:
        //   * AFTER m_process/m_runtime  => destroyed BEFORE them: the server world
        //     was built on that ProcessContext and on the primary's ComponentRegistry.
        //   * BEFORE m_plugin            => destroyed AFTER it: ~PluginHost's
        //     Unload -> TeardownImage iterates its `runtimes` calling ClientHooks()/
        //     ClearSystems()/ResetRegistry(), so every world it points at must still
        //     be alive. With m_play declared after m_plugin (as it was) that loop ran
        //     over a freed Runtime whenever the editor exited mid-EmbeddedServer Play.
        // EditorApp::Shutdown ALSO ends the session explicitly, before any of this --
        // belt and braces, and it is what keeps the VerifyReport's `worlds` array
        // honest. Do not drop either half.

        // Play-in-editor (Task 8): Edit|Play state machine. Play() snapshots the
        // registry + unpauses the RunLoop; Stop() restores the snapshot + re-pauses.
        // Arcane Editor boots in Edit (see Init: the RunLoop is paused right after the
        // plugin loads).
        Arcane::Editor::PlaySession m_play;

        // The "Client + separate server process" play mode's child (Core-DLL split,
        // plan 1 Task 7): ONE tracked ArcaneServer.exe, spawned by DoLaunchServer and
        // stopped on every Play->Edit flip. Beside m_play deliberately -- it is the
        // other half of that topology's Play session, and the two are started and
        // stopped together. Stop() is a silent no-op when nothing was spawned, which
        // is what lets every Stop site call it unconditionally; the destructor is the
        // backstop for an editor that exits mid-session.
        Arcane::Editor::ServerLaunch::ServerProcess m_serverProcess;

        std::optional<Arcane::PluginHost> m_plugin;                 // destructs before m_play, m_runtime
        FramePerf                         m_perf;
        std::uint64_t                     m_frameCount = 0;

        // The settle HOLD (F4 plan 1 T12, Ruling N -- RuntimeFrame.cpp's
        // `settleHold`, ported): true once a headless --settle run has spent
        // its base --frames budget. From then on every extra frame exists only
        // to give the async producers more REAL wall-clock time before the
        // next capture attempt, not to advance content -- so both of this
        // host's dt sources (FrameInput's frameDt, AdvanceSim's simDt) read
        // 0 while this is true. ONE predicate, read by both, so the two
        // clocks can never disagree about whether the hold is on.
        [[nodiscard]] bool SettleHoldActive() const noexcept
        {
            return m_config.headless && m_config.settleAttempts != 0 &&
                   m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames;
        }

        // ---- --settle/--report/--compare (Task 9): the editor's
        // verification surface. Mirrors RuntimeApp.hpp's own block
        // field-for-field -- see that header's comments for the full
        // reasoning on each; only the ones worth restating for the editor's
        // own shape are repeated here. PresentChromeFrame (EditorAppFrame.
        // cpp) owns the whole settle loop, ported from RuntimeFrame.cpp's
        // CaptureTail; EditorApp::MainLoop's pre-loop block resolves
        // --compare's reference, mirroring RuntimeApp::MainLoop's own; and
        // ShutdownGraphPath (EditorApp.cpp) folds settle/bless into
        // m_graphExit and writes the VerifyReport, mirroring RuntimeApp::
        // ShutdownGraphPath.

        // THE LAST-FRAME CAPTURE. Filled once a capture is either the
        // ordinary single-shot (--settle not requested) or the settle
        // loop's converged attempt -- never a mid-settle working capture
        // (see m_previousCapture* below for that). False on a run that
        // never asked for --screenshot/--report, that broke out early, or
        // whose settle loop never converged.
        bool                                  m_captureRead   = false;
        std::uint32_t                         m_captureWidth  = 0, m_captureHeight = 0;
        std::vector<unsigned char>            m_captureRgba;

        // --settle N. The WORKING comparison baseline (churns every
        // attempt, compared against the NEXT one) plus the running state of
        // BOTH halves of the bail conjunction (the attempt counter here, the
        // clock in the trio just below) -- see PresentChromeFrame, a direct
        // port of RuntimeFrame::CaptureTail's settle branch. m_settleConverged
        // is what ShutdownGraphPath reads to fold exitReason
        // "settle-not-converged" into m_graphExit (3, matching RuntimeApp's
        // own code for the identical fact). Both stay at their defaults on
        // a run that never asked for --settle, which must never read as a
        // failure.
        std::vector<unsigned char>            m_previousCaptureRgba;
        std::uint32_t                         m_previousCaptureWidth = 0, m_previousCaptureHeight = 0;
        bool                                  m_previousCaptureValid = false;
        // uint64_t, matching HostConfig::settleAttempts' own width, for the
        // same overflow reason RuntimeApp.hpp's twin field documents.
        std::uint64_t                         m_settleAttemptsUsed   = 0;
        bool                                  m_settleConverged      = false;
        // The TIME half of the bail conjunction, mirroring RuntimeApp's own
        // trio field-for-field (Arcane/Host/SettleBound.hpp). Elapsed is
        // measured from the FIRST settle attempt, never from process start.
        std::chrono::steady_clock::time_point m_settleStartedAt{};
        std::uint64_t                         m_settleElapsedMs      = 0;
        Arcane::SettleBail                    m_settleBail           = Arcane::SettleBail::Keep;
        // THE DEFENSIVE STOP, as its own flag rather than as a forged attempt
        // count. PresentChromeFrame's fail-closed branch used to stop the loop
        // by assigning m_settleAttemptsUsed = m_config.settleAttempts, which
        // worked only while the bail was attempts-ONLY: under the conjunction
        // that assignment stops nothing, because the TIME bound is still
        // unspent -- the unstoppable run it exists to prevent would come
        // straight back. A separate flag also keeps m_settleAttemptsUsed
        // HONEST (it stays the REAL count), which matters to anything
        // reporting the attempts actually taken.
        bool                                  m_settleAborted        = false;
        // Whether NO readback EVER landed across the whole settle phase, so the
        // loop never had two frames to compare and NEITHER bound got a fair
        // test. A DIFFERENT FACT from "captured fine, never agreed": one is a
        // broken capture path, the other a genuinely unstable scene, and they
        // have different fixes. Written once, at the bail, and carried into
        // VerifyReport::SetSettle, whose emitted `settleBailReason` ranks it
        // ABOVE the governing bound for exactly that reason. Stays false on a
        // run that converged (convergence requires a readback) and on one that
        // never asked for --settle at all.
        bool                                  m_settleCaptureFailed  = false;

        // --compare / --bless. Resolved ONCE, before the settle loop starts
        // -- EditorApp::MainLoop's pre-loop block, mirroring RuntimeApp::
        // MainLoop's own resolve-before-the-loop site (Finding 3 of the
        // Task 8 dispatch audit: a missing reference fails FAST, at zero
        // frames rendered, rather than spending the whole --settle budget
        // reporting "could not compare" on every attempt).
        Arcane::ReferenceResolution           m_compareResolution;
        // The software adapter's own reference set (Arcane::ReferenceAdapterSet),
        // "" on hardware; fixed at the first resolve for the post-bless one.
        std::string                           m_compareAdapterSet;
        // The reference pixels the settle loop's compare conjunct reads.
        // Left default/invalid whenever --compare was not given, or --bless
        // disables the conjunct entirely (see m_compareRequested below).
        Arcane::PixelData                     m_referencePixels;
        // Whether the settle loop's convergence predicate grows its THIRD
        // conjunct this run -- true only when --compare was given AND
        // --bless was NOT (computed once, in MainLoop's pre-loop block,
        // from HostConfig::compareReference/::bless). See RuntimeApp.hpp's
        // FrameIo::compareRequested comment for why a --bless run must
        // disable this rather than gate on it: blessing exists to REPLACE
        // the reference, so the stale one on disk must never also be the
        // convergence goal.
        bool                                  m_compareRequested     = false;
        // The MOST RECENT CompareImages() verdict -- overwritten every
        // settle attempt that actually evaluates the conjunct.
        Arcane::ImageCompareResult            m_compareResult;
        // Whether m_compareResult was ever actually written by a real
        // CompareImages() call this run.
        bool                                  m_compareEvaluated     = false;
        // Set true ONLY by MainLoop's pre-loop resolution, when --compare
        // named a reference that does not exist on disk (or exists but
        // failed to decode) and --bless was not given -- the fail-fast
        // path. ShutdownGraphPath reads this to fold exitReason
        // "compare-missing-reference" into m_graphExit (4) rather than
        // falling through the ordinary settle/frames-complete
        // classification.
        bool                                  m_compareMissingFatal  = false;
        // F2b Task 12: set true ONLY by MainLoop's pre-loop cook gate, when
        // a synchronous CookSession::CookProject pass (run BEFORE the
        // reference/settle machinery below, so a fresh --bless never blesses
        // a placeholder either) reports at least one content source that
        // FAILED to cook -- the refuse half of "refuses-or-waits while
        // CookSession-reported cooks are pending". ShutdownGraphPath reads
        // this the same way it reads m_compareMissingFatal, folding
        // exitReason "compare-cook-refused" into m_graphExit (5) ahead of
        // the ordinary settle/frames-complete classification. Mutually
        // exclusive with m_compareMissingFatal by construction: both are
        // ahead-of-the-loop `return`s, so only one can ever fire per run.
        bool                                  m_cookRefusedFatal     = false;

        // ---- Boot state (EditorApp::Create/Init) ----------------------------
        // Deliberately declared HERE, outside the m_gpu/m_runtime/m_plugin
        // block above: those three participate in the load-bearing reverse-
        // declaration teardown contract documented in Shutdown(), and these two
        // take no part in it (Destroy() releases m_bootSeq explicitly, before
        // any member destructor runs).
        //
        // Both must outlive Create(): every patched stage's `run` captures
        // `this` and reads m_bootCtx, and those closures live inside m_bootSeq,
        // which Init() drives after Create() has returned.
        //
        // CONSTRAINT: m_bootCtx holds c_str() views into m_config
        // (projectPath/pluginPath) for this object's whole lifetime, so
        // m_config must not be mutated after construction. It is not today --
        // the only writes are in the ctor's initializer list -- and that is now
        // load-bearing rather than incidental.
        Arcane::HostBoot::BootContext       m_bootCtx{};
        // emplace-only: BootSequence deletes its copy constructor, which
        // suppresses the implicit move too, so it is neither copyable nor
        // movable -- optional can still hold it, but only via emplace().
        std::optional<Arcane::BootSequence> m_bootSeq;
        // Set by Init() on a successful BootSequence::Run. Read by Shutdown()
        // to decide whether this process owns the editor lock it is about to
        // clear -- see that call site.
        bool                                m_bootCompleted = false;

        // Console buffer + engine sink, and the Problems-panel diagnostic
        // STATE (distinct from the console's append-only log stream), grouped
        // together because Install()/Uninstall() are called from the same two
        // sites (Create()/Shutdown()) for the same "nothing may dispatch into
        // a half-torn-down editor" reason. See those methods (EditorApp.cpp)
        // for the create/shutdown timing rationale.
        struct ConsoleDiagnostics
        {
            // Sized at construction, after HostBoot::ApplyEarlyConfigRungs (main.cpp):
            // a Restart setting; the frame then follows editor.console.displayLineCap.
            ConsoleBuffer                                    console{ static_cast<std::size_t>(
                Arcane::Settings<Arcane::Editor::EditorConsoleSettings>().ringLines) };
            std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;      // erased in Uninstall
            Arcane::Editor::ConsoleUiState                   ui;
            Arcane::Editor::DiagnosticStore                  store;
            Arcane::Editor::ProblemsUiState                  problemsUi;
            void Install();     // console sink + store.InstallAsEngineSink (was
                                // InstallConsoleSink + the Create() call beside it)
            void Uninstall();   // the Shutdown-top erase/uninstall pair
        };
        ConsoleDiagnostics m_consoleDiag;

        // (m_play / m_serverProcess are declared UP with m_runtime/m_plugin -- the
        // teardown contract, see that block.)

        // THE Play/edit predicate (architecture pass sec 1). Editor code asks this,
        // never m_play.IsPlaying() raw, so the predicate has one greppable name.
        [[nodiscard]] bool InPlayMode() const noexcept { return m_play.IsPlaying(); }

        // Every document's undo resolver target (spec s3.3b): null while Play runs.
        [[nodiscard]] Arcane::CommandStack* DocumentUndo() noexcept
        {
            return Arcane::Editor::ResolveDocumentUndo(InPlayMode(), m_undo ? &*m_undo : nullptr);
        }

        // ---- THE HOVER predicate --------------------------------------------
        // "The pointer is over the Viewport panel AND that fact is allowed to
        // affect what is rendered." The second half is what this exists for:
        // m_gameUi.inViewport is re-derived from the LIVE cursor every frame
        // (FrameInput, EditorAppFrame.cpp), so a hover-driven outline makes
        // the rendered frame a function of where the mouse happens to be
        // sitting -- which is exactly what a SCRIPTED run must not be. The
        // same command line would otherwise produce a cyan hover outline over
        // whatever entity the pointer rests on, or none at all when it rests
        // elsewhere.
        //
        // "No hover" is already the chain's documented sentinel --
        // FrameDesc::hoverPixel spells it (-1, -1) -- so suppressing hover
        // needs no coordinate invented for it, which matters because there is
        // no honest one to invent: the panel's position is imgui.ini layout
        // state, saved per project GUID.
        //
        // READ BY ArmGraphViewportFrame, which carries phase 12's gate.
        [[nodiscard]] bool HoverLive() const noexcept
        { return m_gameUi.inViewport; }

        // ---- THE GIZMO predicate --------------------------------------------
        // "The transform gizmo may interact and draw." HoverLive()'s sibling
        // (see that predicate for why a NAMED predicate rather than "m_gizmoEnabled
        // defaults to false"): the gizmo draws INTO THE SCENE BATCH and its hover
        // comes from the live cursor, so both consequences are gated here. Since F4
        // (spec s7.2) the gizmo works in every view mode -- the 2D view only masks its
        // Z handles (GizmoHandles() below) -- so this is the tool state alone.
        [[nodiscard]] bool GizmoLive() const noexcept { return m_gizmoEnabled; }

        // The handles the current view offers: everything in Perspective; in the
        // 2D view the planar set for the current mode (spec s7.2 -- the Z arrow,
        // the YZ/XZ squares, the X/Y rings, the Z box and the screen ring hide,
        // and Z components pass through the drags untouched). A HOST decision,
        // not a gizmo mode: Arcane::Gizmo has one code path.
        [[nodiscard]] Arcane::GizmoHandleMask GizmoHandles() const noexcept
        {
            return m_camera.mode == Arcane::Editor::ViewMode::TwoD
                 ? Arcane::GizmoHandleMask::Planar(m_gizmoMode)
                 : Arcane::GizmoHandleMask::All();
        }

        // THE VIEWPORT'S EXTENT: the offscreen graph output's surface size
        // (m_viewportTargets.graph).
        // Every phase that needs a viewport size asks HERE, the same way
        // RuntimeFrame's FrameExtent is the runtime's single source of truth,
        // so the resolver's material globals, the scene camera's fit, the
        // camera-rect overlay and the panel's Image can never end up fitted
        // to two different rectangles. 0 before the vehicle exists (or after
        // a failed resize), which every caller already tolerates -- the
        // panel skips its Image at 0 and EditModeSchedule::ServicePendingFrame
        // defers a SceneOpen request until the panel has a real size.
        [[nodiscard]] std::uint32_t ViewportWidth()  const noexcept;
        [[nodiscard]] std::uint32_t ViewportHeight() const noexcept;
        // Both, as the uvec2 EditorCamera::Resolve / Frame / Pan2D / ZoomAt2D
        // take (F4 plan 1 T6).
        [[nodiscard]] glm::uvec2 ViewportSize() const noexcept { return { ViewportWidth(), ViewportHeight() }; }
        // Play-mode dropdown (Task 6, runtime-host-fold arc): which action the
        // transport's Play button performs (see DrawSimTimeToolbar). Viewport =
        // m_play above, unchanged. SeparateWindow = LaunchStandalone (below) --
        // m_play/its toggle are never touched by that path. The mode is the
        // editor.play.launchMode cvar (settings S6-32, Pref-P): the toolbar
        // reads the published value and its menu writes the cvar. An old
        // "[EditorPlayMode][State]" ini section is still READ (the handler
        // below) into m_legacyPlayMode, imported once at the top of the next
        // frame (ImportLegacyPlayMode, User rung, only where the user has not
        // chosen), and never written again.
        std::optional<Arcane::Editor::PlayLaunchMode> m_legacyPlayMode;

        // ImGuiSettingsHandler callbacks for m_legacyPlayMode, in the standard
        // ImGuiSettingsHandler read/write shape -- static member functions rather than free
        // functions (like the scene/project dialog Thunks below) so they can
        // reach the private m_legacyPlayMode of the instance handed through
        // handler->UserData; there is exactly one EditorApp per process.
        // WriteAll writes nothing: the section leaves the ini on its next save.
        static void* PlayModeSettingsReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                              const char* name);
        static void  PlayModeSettingsReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                              void* entry, const char* line);
        static void  PlayModeSettingsWriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                              ImGuiTextBuffer* buf);
        // Each handler's ClearAllFn (ImGui::ClearIniSettings -- a windowed
        // project switch, RetargetLayoutIni) resets its members to a fresh
        // EditorApp's values before the incoming file is read.
        static void  PlayModeSettingsClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler);
        void RegisterPlayModeSettings();   // called from Init

        // ImGuiSettingsHandler callbacks for m_camera + m_legacyViewport
        // ("[EditorViewport][Camera]", F4 plan 1 T7), mirroring the PlayMode
        // handler above line for line; the line format and its refusal table
        // are ViewportSettings::WriteIni / ReadIniLine (pure, unit-tested),
        // and these callbacks only wrap them. Registered at the same Init
        // site. ReadLine re-applies the --view-mode seed after each line so
        // the ini's Mode can never overrule the flag (the ini is read at the
        // FIRST NewFrame, which is after StageFinalize on a windowed run, and
        // again on a windowed project switch: RetargetLayoutIni's
        // ClearIniSettings + LoadIniSettingsFromDisk at the loop top).
        static void* ViewportSettingsReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                              const char* name);
        static void  ViewportSettingsReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                              void* entry, const char* line);
        static void  ViewportSettingsWriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                              ImGuiTextBuffer* buf);
        static void  ViewportSettingsClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler);
        void RegisterViewportSettings();

        // The gizmo session's start (settings S6-31): ApplyGizmoSessionDefaults
        // writes editor.gizmo.defaultMode / defaultSpace / defaultTool
        // (NextWorld) to m_gizmoMode / m_gizmoSpace / m_gizmoEnabled;
        // ApplyGizmoToolSeed then lays the --tool flag on top (the flag beats
        // the defaults). Boot: the defaults in StageEditorShell, the seed in
        // StageFinalize after the boot scene. A windowed project switch: both,
        // in ViewportSettingsClearAll beside ApplyFreshPose, so the incoming
        // project's Pref-P defaults apply without an editor restart.
        void ApplyGizmoSessionDefaults();
        void ApplyGizmoToolSeed();

        // ImGuiSettingsHandler callbacks for m_panelVis ("[EditorPanels]
        // [Visibility]", one name-keyed line per hideable panel), mirroring
        // the PlayMode handler above. Registered at the same Init site.
        void RegisterPanelVisibilitySettings();
        static void* PanelVisibilitySettingsReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                                     const char* name);
        static void  PanelVisibilitySettingsReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                                     void* entry, const char* line);
        static void  PanelVisibilitySettingsWriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                                     ImGuiTextBuffer* buf);
        static void  PanelVisibilitySettingsClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler);

        // The Inspector instance list's handler ("[EditorInspector][Instances]")
        // lives beside the Inspector windows: RegisterInspectorInstancesSettings
        // (Panels/InspectorWindows.hpp), registered at the same Init site.
        // The document an Inspector source IS, for the Ctrl+S routes: null for
        // the scene and asset sources and for null (EditorDocument derives from
        // InspectorSource, Task 2).
        [[nodiscard]] Arcane::Editor::EditorDocument* InspectorSaveTarget(Arcane::Editor::InspectorSource* src) const
        {
            return dynamic_cast<Arcane::Editor::EditorDocument*>(src);
        }

        // ---- Editor layout ini (imgui.ini), per project ---------------------
        // <Paths::EditorUserDir>\Layouts\Session\<project-guid>.ini, i.e.
        // %LOCALAPPDATA%\Arcane\Editor\Layouts\Session (settings spec s11.0,
        // s7.4: named layouts own Layouts\); "default" project-less. io.IniFilename BORROWS this string (ImGui never
        // copies it), so it lives here, never in a local. Retargeted at boot
        // (StageFinalize, before the first NewFrame auto-loads it) and on
        // project switch. A windowed switch loads the INCOMING project's
        // layout: ClearIniSettings (every editor handler's ClearAllFn resets
        // to defaults) then LoadIniSettingsFromDisk of the new file, at the
        // loop-top safe point before the editor's ImGui frame (Task 13).
        //
        // THE STRING ITSELF IS DECLARED UP WITH m_config, ABOVE m_gpu -- ImGui
        // borrows it and saves through that borrow from ~ImGuiLayer, so it has
        // to outlive m_gpu. See the lifetime note at its declaration.
        void RetargetLayoutIni();

        // ---- Diagnostics dump dir, per project (GPU crash diagnostics arc,
        // Task 8; F-6 in the seam-facts survey) -----------------------------
        // Same call-site family as RetargetLayoutIni immediately above --
        // every site that retargets the layout ini also retargets where a
        // crash/hang report lands, right after, so a report from THIS
        // project always lands under THIS project's Saved/Diagnostics
        // instead of a stale one another project left behind. See
        // RetargetDumpDir's own definition (EditorApp.cpp) for the
        // <project>/Saved/Diagnostics vs exe-relative-default split.
        void RetargetDumpDir();

        // SeparateWindow's launch flow (DoLaunchStandalone, EditorAppScene.cpp)
        // is the LaunchStandalone SceneSession intent (Scene/SceneSession.hpp):
        // the toolbar's Play click issues a Request, which parks behind the
        // shared "Unsaved Scene" modal when the scene is dirty or never saved
        // and otherwise proceeds immediately, per Request's completion-convention
        // comment. The spawn effect itself lives only in DoLaunchStandalone,
        // called from RunSceneAction's LaunchStandalone case.
        //
        // Effect for the LaunchStandalone intent (RunSceneAction's case). Not
        // called directly outside that -- the SceneSession machine is the gate.
        void DoLaunchStandalone();

        // The "Client + separate server process" spawn (Core-DLL split, plan 1
        // Task 7), mirroring DoLaunchStandalone above: resolve ArcaneServer.exe
        // from ServerLaunch::ExeCandidates(CurrentExeDir()), take the first
        // candidate that is a real file, and spawn it on the open project.
        // Both refusals (no project, no exe) surface as a modal naming what was
        // looked for, exactly as the standalone launch's do.
        //
        // NOT routed through the SceneSession intent machine, and that is the one
        // real difference: the server boots the project MANIFEST's bootScene, not
        // whatever is on screen, so there is no "save this scene first" question
        // to park behind -- the unsaved-scene guard DoLaunchStandalone needs
        // exists because THAT spawn passes --scene <guid> of the live document.
        void DoLaunchServer();

        // Ordered multi-select source of truth (set + primary); slice-2 consumers
        // operate on Primary(), shared by the Hierarchy panel (and, later, the
        // Inspector + viewport pick -- see SelectionContext.hpp).
        Arcane::Editor::SelectionContext m_selection;

        // Which .arcscene is being edited, whether it has unsaved changes, and the
        // one-at-a-time unsaved-changes confirm machine (SceneSession.hpp). Pure
        // state: every effect below it (dialogs, file IO, ResetRegistry) is
        // performed here.
        Arcane::Editor::SceneSession m_scene;

        // Window-menu panel visibility (PanelRegistry.hpp). The menu's
        // checkmarks and the tabs' X buttons write these same bools;
        // persisted by the [EditorPanels][Visibility] ini handler.
        Arcane::Editor::PanelVisibility m_panelVis;

        // Outliner panel state + structural-edit binding (slice 2)
        Arcane::Editor::OutlinerState   m_outliner;
        Arcane::Editor::SceneEditBinding m_editBinding;
        // Inspector panel state: holds the field-edit gesture's CommandStack
        // ownership token across the frames the gesture spans (see InspectorState).
        Arcane::Editor::InspectorState  m_inspector;
        // Built ONCE in Init: the reflected rows' borrowed app seams (assetRefs).
        Arcane::Editor::InspectorServices m_inspectorServices;
        // Spec 2026-09-30 s4.2: the ONE asset-reference services value. The
        // Inspector and every document's Services point here (app lifetime;
        // m_documents, declared below, destructs first).
        Arcane::Editor::AssetRefServices  m_assetRefServices;
        // Inspector ownership (spec 2026-09-28): the scene source, the host
        // that routes the last-selecting source to every Inspector instance,
        // the per-instance draw state, and the two epoch watermarks (the
        // scene's SelectionEdge over SelectionContext::Epoch(), the per-
        // document map over SelectionEpoch()) that turn a selection GESTURE
        // into a host event (never focus, never a prune). Declared in
        // dependency order: the host holds a reference to the scene source.
        // m_documents (declared far below) destructs FIRST: the host stores
        // raw EditorDocument* sources, and ~InspectorHost is implicit and
        // never touches them; Shutdown() snapshots the Inspector BEFORE
        // ShutdownGraphPath's CloseAll and ReleaseAll()s the host AFTER it
        // (see there).
        Arcane::Editor::SceneInspectorSource  m_sceneSource;
        // Inspector filters (spec 2026-09-29 s6): the Asset Browser's shared
        // selection as its own PERMANENT source. Declared before the host for
        // the same reason m_sceneSource is: the host holds a raw pointer to
        // it, so it must outlive the host (declaration order = construction
        // order; destruction runs in reverse).
        Arcane::Editor::AssetPanelServices   m_assetPanelServices;   // the asset page's thumbnails + peeks; a MEMBER: the page draws after DrawEditorUi returns
        Arcane::Editor::AssetInspectorSource m_assetSource;          // permanent Inspector source over m_assetModel.selected
        Arcane::Editor::SelectionEdge        m_assetSelectionEdge;   // Observe(m_assetModel.selectionGesture, key)
        Arcane::Editor::AssetPanelActions    m_assetPageActions;     // the asset page's clicks, drained next frame
        Arcane::Editor::InspectorHost         m_inspectorHost{ m_sceneSource };
        Arcane::Editor::InspectorWindowsState m_inspectorWindows;
        Arcane::Editor::SelectionEdge         m_sceneSelectionEdge;   // epoch-based (Task 2): Observe(m_selection.Epoch(), key)
        std::unordered_map<const Arcane::Editor::EditorDocument*, std::uint64_t> m_docSelectionEpochs;
        Arcane::Editor::InspectorSource* m_inspectorFocusedSource = nullptr;   // latched each Inspector draw; read by the Ctrl+S gate
        // Pushed by a saved input document's onSaved, applied after DrawAll.
        // ONLY the project's designated input asset is enqueued (a save of any
        // other .arcinput republishes nothing, so Save All can never displace
        // it); one slot, because every enqueued save is of that ONE asset and
        // the last one in a frame is its disk state.
        std::optional<std::pair<Arcane::Guid, Arcane::InputActionAsset>> m_pendingInputRepublish;
        // Asset-manager redesign, Plan 1 Task 7: the Assets panel's thumbnail
        // resolver (resolveAssetThumb), built once in StageSpriteTables --
        // a [this]-capture over the chrome context's texture cache. Nothing calls it yet (Task 9 hands it
        // to the new Browse panel); wiring only, no behavior change.
        Arcane::Editor::AssetServices    m_assetServices;

        // Editor undo/redo history. Deliberately NOT cleared on Play: Stop restores
        // the pre-Play registry, so the edits behind these entries are still on
        // screen and must stay undoable -- and because SceneSession reads this
        // stack's SceneStateId as the SOLE input to the scene's dirty flag
        // (SceneSession.hpp), clearing it would silently report an unsaved scene as
        // clean. It is cleared only where no entity handle in it could survive:
        // ClearSceneReferences, ahead of a registry swap. Constructed in
        // Init once the runtime's registry exists; optional so it can be built
        // after m_runtime. Declared AFTER m_runtime/m_plugin so it destructs
        // BEFORE them -- its resolver lambda captures `&*m_runtime` (a raw
        // Runtime*, dereferenced fresh each call), so it must not outlive it.
        std::optional<Arcane::CommandStack> m_undo;
        Arcane::UndoLimits m_undoLimitsApplied;   // last limits pushed (per-frame change check)
        // Settings arc S4: editor.theme.* / editor.ui.* -> the editor's ImGui
        // style, compared per frame against what was last applied.
        Arcane::Editor::AppearanceApplier m_appearance;
        Arcane::Editor::ThemePageState m_themePage;
        Arcane::Editor::ShortcutsPageState m_shortcutsPage;   // Preferences > Keyboard
        Arcane::Editor::FontsPageState m_fontsPage;           // Preferences > Appearance > Fonts and Scale
        Arcane::Editor::LayoutPageState m_layoutPage;         // Preferences > Layout

        // Mouse edge tracking (architecture pass sec 6). Keyboard chords now
        // read EditorActions (settings S4); only the camera/gizmo buttons remain.
        struct InputEdges
        {
            Edge lmb, rmb, mmb;   // gizmo press/release; camera gestures (keys: EditorActions, settings S4)
        };
        InputEdges m_edges;

        // Transform-gizmo state (Edit-mode; drives the Viewport handles). Mode/
        // space persist across frames and selection changes; m_gizmoHovered is
        // recomputed every frame the gizmo is interactable (cleared otherwise) so
        // Draw's highlight always matches this frame's cursor. m_gizmoDrag spans
        // the mouse-down..mouse-up gesture; `start` is the pre-drag GizmoTransform
        // so ApplyDrag recomputes from origin each frame (no accumulation drift).
        // Transform is parent-local, but every GizmoTransform stored here is WORLD
        // space (Unreal parity) -- EditorApp converts through Edit::WorldMatrix on
        // read and Edit::ParentWorldMatrix's inverse on write-back. Mode, space
        // and tool start from editor.gizmo.defaultMode / defaultSpace /
        // defaultTool (NextWorld), applied at boot and on every windowed
        // project switch (ApplyGizmoSessionDefaults, settings S6-31).
        Arcane::GizmoMode  m_gizmoMode    = Arcane::GizmoMode::Translate;
        Arcane::GizmoSpace m_gizmoSpace   = Arcane::GizmoSpace::World;
        bool               m_gizmoEnabled = false;  // false = Select tool (click-to-pick, no gizmo)
        Arcane::GizmoAxis  m_gizmoHovered = Arcane::GizmoAxis::None;
        struct GizmoDrag
        {
            bool                   active = false;
            Arcane::GizmoAxis      axis   = Arcane::GizmoAxis::None;
            Arcane::GizmoTransform start;                    // the PRIMARY's pre-drag WORLD pose (gizmo anchor)
            glm::vec2              mouseStartScreen{0.0f, 0.0f};
            // Every selection ROOT carrying a Transform, with its pre-drag WORLD
            // pose and the axis (1 = Y, 2 = Z, else 0) its AUTHORED scale carries a
            // mirror on -- DecomposeTRS reads any mirror as a negative X, and the
            // write-back re-homes it (WithMirrorOn) so a Y-mirrored sprite's
            // Inspector numbers survive a drag. Roots only: a selected child rides
            // its selected parent through WorldTransform propagation.
            struct Target { Astra::Entity entity; Arcane::GizmoTransform startWorld; int mirrorAxis; };
            std::vector<Target> targets;
            // Ownership token for the drag's undo transaction, minted on press
            // and spent on release (CommandStack::Begin). Parked here because the
            // gesture spans frames and the Inspector shares the same stack: only
            // the token holder may Commit/Cancel, so an Inspector edit landing in
            // the same frame can no longer close this drag out from under it.
            Arcane::TransactionId txn = Arcane::TransactionId::None;
            // The active ROTATE drag's sweep (start angle + turn in the ring's
            // frame), recomputed each drag frame from the same inputs ApplyDrag
            // reads; Draw paints it as UE's swept sector. Empty otherwise.
            std::optional<Arcane::GizmoRotateSweep> sweep;
        } m_gizmoDrag;

        // The EDITOR's viewport camera (Edit mode). ClientRuntime::SetView is the
        // PLUGIN's seam, so a project whose game module never calls it would be
        // stuck at the identity view. EditorApp drives this from viewport input
        // and pushes m_camera.Resolve(ViewportSize()) into the Runtime in Edit
        // mode only; in Play the scene camera wins. Two persisted transforms
        // (Ortho2D / Orbit3D) by ViewMode -- see EditorCamera.hpp (F4 spec s4).
        Arcane::Editor::EditorCamera m_camera;
        // Edit-mode propagation + the pending frame request (spec 2026-09-11 s7).
        //
        // LAZY ON PURPOSE, and the declaration slot is deliberate (2026-09-16).
        // EditModeSchedule's ctor runs AddSystem<TransformPropagationSystem>, which
        // resolves WorldTransform/Transform/... through Astra::TypeID<T>::Value() --
        // and that CACHES PER MODULE, permanently. As a plain member it ran at
        // EditorApp construction, i.e. BEFORE StageRuntimeCreate installs the shared
        // TypeContext, so ArcaneEditor.exe kept the ids from its own private
        // DefaultTypeContext forever. They coincided with the shared ones only by
        // first-touch accident; the moment anything resolved an extra type in the
        // shared context first (ProcessContext::Create's engine-resource pre-warm),
        // Materialise read WorldTransform under SceneRoot's id and the viewport
        // rendered EMPTY with RenderErrorCount 0. Emplaced in StageRuntimeCreate
        // immediately after Astra::SetTypeContext. Same slot as the old member, so
        // destruction order is unchanged: it still destructs before m_runtime.
        std::optional<Arcane::Editor::EditModeSchedule> m_editSchedule;
        bool m_physicsOverlay = false;   // View -> Physics Overlay (spec s6.3, session-only)
        // The editor.viewport.physicsOverlay value last applied to m_physicsOverlay
        // (settings S6-32): the frame's sync moves the toggle only when the cvar changes.
        bool m_physicsOverlayPref = false;
        // An old [EditorViewport][Camera] block's preference lines (fov,
        // speed, grid, gizmo size), captured by ViewportSettingsReadLine and
        // imported ONCE at the top of the next frame (settings S6-29:
        // ImportLegacyViewportPrefs, User rung, only where the user has not
        // chosen). The preferences themselves are editor.viewport.* /
        // editor.camera.* / editor.gizmo.* cvars, read via Settings<T>().
        Arcane::Editor::LegacyViewportPrefs m_legacyViewport;
        // Set by ViewportSettingsReadLine when the persisted block restores a
        // transform (an Ortho= or Orbit= line parsed). F4 plan 1 final
        // review, F3: a restored camera cancels the boot-time SceneOpen
        // framing (EditModeSchedule::CancelFrame) so centre/halfHeight/
        // pivot/distance survive a restart as spec s4 says; a later File >
        // Open still frames (the ini is read at the first NewFrame -- or in
        // RetargetLayoutIni under --headless -- and again by RetargetLayoutIni
        // on a windowed PROJECT switch, whose clear resets this to false;
        // never on a scene open).
        bool m_cameraRestoredFromIni = false;
        // The ONE live mouse-drag camera gesture (F4 plan 1 T7): 2D RMB pan, or
        // in Perspective RMB look / Alt+LMB orbit / MMB pan. Rules: a gesture
        // may only START over the viewport, keeps tracking anywhere once
        // started, and ends when its button lifts -- see UpdateEditorCamera.
        struct CameraGesture
        {
            enum Kind : std::uint8_t { None, Pan2D, Look, Orbit, Pan3D };
            Kind      kind = None;
            glm::vec2 lastMouse{0.0f, 0.0f};   // WINDOW px -- only the delta is used
            // TRUE only on the frame the gesture STARTED (Ruling C): lastMouse
            // is then whatever the cursor was last frame, possibly far away,
            // so no delta is applied until the next frame, when it is a real
            // previous cursor. False on every later frame of the gesture.
            bool      fresh = false;
        };
        CameraGesture m_camGesture;
        // Records a frame request serviced after this frame's propagation
        // (selectionOnly) or at the whole scene. A no-op when there is nothing
        // framable, so the user's view is never thrown away by an F press that
        // had no target.
        void FrameCamera(bool selectionOnly);
        // F4 plan 1 Task 11 (spec s8): the scene's `Add > 3D Object >
        // <primitive>`, consumed from OutlinerState's latch right after
        // DrawOutlinerPanel returns. MintOrReusePrimitiveMesh(source) for the
        // asset, then ONE ApplyStructural("Add 3D Object") around
        // Edit::AddPrimitiveEntity at the view's focus point (m_camera.
        // FocusPoint(): 2D = the centre at z = 0, perspective = the pivot;
        // converted into the parent's local space when there is one), then
        // select the new entity and request a frame of the selection
        // (FrameCamera(true) -- serviced after this frame's propagation, so it
        // reads the spawned entity's real world pose). One undo step; a failed
        // mint spawns nothing and reports through the modal error queue.
        void ConsumeAddPrimitive();
        // Set for the remainder of THIS frame when a gizmo drag starts or ends,
        // so the click-pick phase (later in the frame) does not also treat the
        // same click as a selection change. Reset at the top of FrameInput
        // each frame.
        bool m_gizmoCapturedClick = false;

        // Scene-in-a-panel viewport: the graph's batch->[post]->tonemap-> pick
        // /outline path, rendered into a panel texture instead of the window's
        // own backbuffer. Resized to the Viewport panel's content region each
        // frame; input into the plugin is gated on m_viewportActive and
        // remapped through m_viewportRect (see ViewportInput.hpp + FrameInput).
        struct ViewportTargets
        {
            // THE WHOLE VIEWPORT, IN ONE OBJECT -- non-null once the editor
            // has booted.
            //
            // WHY ONE MEMBER RATHER THAN THREE: the picker and the selection
            // outline are NODES INSIDE this same context's frame
            // (PickNode/OutlineNode, armed per frame through
            // FrameDesc::pickOutline/pickables/selectedIds), not sibling
            // render targets with their own devices and command lists.
            //
            // Offscreen mode: no window, no swapchain, one persistent
            // BGRA8_UNORM output the chrome pass samples as an ImGui image.
            // It BORROWS the NriDevice m_graphChrome owns, so it must not
            // outlive it -- guaranteed by declaration order (m_graphChrome is
            // declared near the top of this class, m_viewportTargets far
            // below, and reverse-order destruction runs this one first).
            //
            // PROJECT-SCOPED, AND THEREFORE REBUILT BY EVERY PROJECT SWITCH
            // -- unlike the chrome context, which survives one. Not because
            // the OBJECT belongs to a project (its
            // two injected seams re-read CurrentProject per call and would
            // survive fine) but because its CACHES do: NriTextureCache holds
            // uploads keyed by the outgoing project's asset Guids, and
            // Batch2DNode's write-once sprite/material descriptor sets name
            // that cache's views. See EditorApp::TeardownGraphForSwitch.
            std::unique_ptr<Arcane::NriGraphContext> graph;

            std::uint32_t pendingW = 0, pendingH = 0;   // measured last frame, applied at frame top
            // body: the current EditorApp::ApplyPendingViewportResize.
            // `chrome` is the HOST-WINDOW context whose ImGuiNri caches the
            // offscreen output -- the resize contract
            // (NriGraphContext::ResizeOffscreen) requires invalidating that
            // cache BEFORE the resize, and this class owns no ImGuiNri for it.
            void ApplyPendingResize(Arcane::NriGraphContext* chrome);
        };
        ViewportTargets m_viewportTargets;
        Arcane::Editor::ViewportRect                   m_viewportRect{};
        bool                                     m_viewportActive = false;
        bool                                     m_rebindCaptureLive = false;   // an Input Actions rebind capture owns the keyboard this frame; computed at the top of FrameInput, fed to EditorActions::BeginFrame

        // Viewport-local input snapshot for the game ImGui pass, captured inside
        // FrameInput (whose locals are out of scope at the render site) and read
        // where the game UI is composited. Only what the game context needs is
        // hoisted -- the input phase's scope stays narrow.
        struct GameUiInputHandoff
        {
            glm::vec2    viewportMouse{0.0f, 0.0f};  // viewport-local cursor px
            std::uint8_t mouseButtons = 0;           // raw snap.mouseButtons (LMB=bit0)
            float        wheel        = 0.0f;        // raw snap.wheelY
            bool         inViewport   = false;
            double       frameDt      = 0.0;         // per-frame dt (seconds)
        };
        GameUiInputHandoff m_gameUi;

        // ---- The viewport's pick + outline arming ---------------------------
        // THIS FRAME's pickable silhouettes and the hit-proxy ids the outline
        // traces, held as MEMBERS because FrameDesc borrows both as spans and a
        // span into a per-frame temporary is a dangling read the moment the
        // declaration outlives the statement. Reused rather than rebuilt, so a
        // steady-state Edit frame allocates nothing.
        //
        // Produced by CollectPickables, whose k-th entry IS hit-proxy id k+1
        // -- the ONE id assignment in the process.
        std::vector<Arcane::PickDrawable> m_pickDrawables;
        std::vector<std::uint32_t>        m_pickSelectedIds;

        // ---- The viewport's opaque 3D pass (F2a Task 10; F3 plan 1 T8) -----
        // THIS FRAME's GPU-scene frame -- the staged rows, the batch table,
        // the indirect args and the CPU-written visible-index list -- held as
        // a MEMBER for the identical reason m_pickDrawables above is one:
        // FrameDesc::mesh -> MeshSceneDesc::scene BORROWS it for the duration
        // of the RenderFrame call, so a per-frame temporary would dangle the
        // moment the declaration outlives the statement that filled it.
        //
        // Rebuilt every frame by PrepareSceneForRender (Host/GpuSceneHost.hpp)
        // from RenderSceneToViewport, between the schedulers and the sprite
        // sweep -- the visible set(s) it builds are what that sweep and the
        // pick pass cull against, so it cannot run later, inside
        // ArmGraphViewportFrame where the pick spans are filled. Its
        // containers are reused, so a steady-state frame with a static scene
        // allocates nothing after the first.
        Arcane::GpuSceneFrame m_gpuSceneFrame;

        // THE VIEW THE MESH PASS DRAWS WITH this frame -- the editor view in
        // Edit, the perspective scene camera in Play (RuntimeFrame.cpp's
        // rule), nullopt when Play has no such camera (no mesh batches then;
        // the pass still declares itself for any staged rows, ruling R-D).
        // Resolved beside the PrepareSceneForRender call (it culls against
        // this view) and read again by ArmGraphViewportFrame for the
        // MeshSceneDesc's matrices, so the two cannot disagree.
        std::optional<Arcane::ViewTransform> m_meshView;

        // THIS FRAME'S MESH SCENE. Unlike m_pickDrawables/m_pickSelectedIds
        // (borrowed by FrameDesc as spans, stored inline in `vp` the moment
        // ArmGraphViewportFrame assigns them), FrameDesc::mesh is a POINTER
        // to a whole MeshSceneDesc -- and RenderSceneToViewport's
        // RenderFrameOffscreen(vp) call happens in THAT function, after
        // ArmGraphViewportFrame (which fills this struct) has already
        // returned. A MeshSceneDesc local to ArmGraphViewportFrame would be
        // destroyed at that return, leaving vp.mesh dangling for the call
        // that reads it -- so, like m_pickDrawables, this has to be a member
        // that outlives the function that fills it.
        Arcane::MeshSceneDesc m_meshScene;

        // THIS FRAME'S 3D REFERENCE GRID (F4 plan 1 T10, spec s5.2) -- a
        // member for exactly m_meshScene's reason: FrameDesc::grid is a
        // POINTER read by RenderFrameOffscreen after ArmGraphViewportFrame
        // (which fills it) has returned.
        Arcane::GridSceneDesc m_gridScene;

        // The click-pick, which cannot answer in the frame it is asked -- see
        // DeferredPick.hpp for the three hazards it closes.
        Arcane::Editor::DeferredPick m_deferredPick;

        // WHICH SCENE the entities in flight belong to. Bumped by every path
        // that REPLACES the registry's contents wholesale -- ClearSceneReferences
        // (New/Open Scene, which also stops Play) and ResetPerProjectState (a
        // project switch) -- so a readback armed against the outgoing scene
        // cannot be applied to the incoming one. Play START/STOP is covered
        // separately, by DeferredPick recording the mode alongside this: those
        // two swap the registry object without going through either site.
        //
        // A COUNTER rather than a pointer comparison: Runtime swaps the registry
        // OBJECT on Play/Stop and on plugin hot-reload, and a fresh one may land
        // on the address the previous one just vacated -- the same ABA the
        // ImGuiNri texture cache has to defend against, for the same reason.
        std::uint64_t m_sceneEpoch = 0;
        // Bumped from the two sites above; named so both read as one decision.
        void NoteSceneReplaced() noexcept { ++m_sceneEpoch; m_deferredPick.Reset(); }

        // Shader-editor services + open documents (Slice 5). The compiler is the
        // app-shared compile service: documents Submit through it, and the
        // resolver's Refresh (phase 9) Polls/Drains it once per frame, offering
        // each result to the open documents first. That drain hands documents
        // raw DXIL/SPIR-V bytecode and nothing device-shaped (see
        // ShaderEditorDocument::BindIfComplete: the bytes are retained, never
        // bound to a device, and the preview builds its own NRI pipeline from
        // exactly those bytes). Documents hold no device-bound resources;
        // declared after m_runtime because they still borrow its Assets.
        std::unique_ptr<Arcane::ShaderCompiler> m_shaderCompiler;
        Arcane::ShaderSourceProvider            m_shaderSources;
        // Scene asset resolution (sprite-resolution lift, 2026-07-29): ONE
        // engine-side service owning the sprite / sprite-material / post-chain
        // caches, swept and published once per frame by RefreshSceneResolution.
        // It replaced three editor-held caches -- and the editor holding them was
        // exactly why the standalone runtime host could draw none of the three.
        // ArcaneRuntime now constructs the same object. It publishes the
        // registry's SpriteTable/SpriteMaterialTable, whose pointers are
        // non-owning, so it MUST destruct before m_runtime -- which it does,
        // being declared after it (reverse-order destruction), and before
        // m_viewportTargets, whose batcher it holds.
        // ONE trap if this ever changes: m_documents below destructs FIRST, so
        // the consumeFirst hook installed on the resolver (which reaches into
        // m_documents) is dead by the time ~SceneRenderResolver runs. That is
        // fine only because the dtor never drains -- it just un-publishes.
        std::unique_ptr<Arcane::SceneRenderResolver> m_resolver;
        Arcane::Editor::DocumentHost            m_documents;
        // --open-asset: frames left on which the scripted open re-requests
        // focus for its document. A brand-new window docked on its first
        // frame loses the tab selection to the seeded layout's persisted
        // choice; OpenPath on an already-open document only re-arms the focus
        // request (focus-not-reopen), so repeating it for a few frames is the
        // contract-preserving way to make the tab stick. 0 = nothing pending.
        int m_scriptedOpenFocusFrames = 0;
        bool m_projectSettingsOpen = false;
        bool m_preferencesOpen = false;            // Edit > Preferences... (settings arc S3-13)
        bool m_restartRequested = false;
        std::filesystem::path m_relaunchRoot;
        bool m_settingsBootOpenConsumed = false;   // editor.settings.openAtBoot read once, on the first UI frame
        std::string m_settingsPathCvar;            // the path row a Browse dialog answers for
        void ConfigureSettings();                  // the settings host's per-project config (OnProjectOpened)
        void BrowseSettingsPath(const std::string& cvar, bool folder);
        // Settings arc S3-12: Project Settings' "Project" page (the host draws it; EditorApp applies its requests).
        static void ProjectPageThunk(void* user);
        void DrawProjectPage();
        void ApplyProjectSettingsRequests(const Arcane::Editor::ProjectSettingsRequests& settings);
        // Asset-manager redesign, Plan 1 Task 9 -> panel-split Task 7: the
        // asset panels' session UI state, one struct per WINDOW now that the
        // three lenses are three panels (spec s6). The Asset Status window
        // has no third member here on purpose -- its dashboard is entirely
        // derived from the model and services, and by Task 3 its only state
        // writes had already become actions.
        Arcane::Editor::AssetBrowserPanelState  m_assetBrowserUi;   // search / rail / folds / preview width
        bool m_browserOwnsEditKeys = false;   // T5 s7.10: last frame's Browser answer
        Arcane::Editor::AssetGraphPanelState    m_assetGraphUi;     // canvas + projection + focus + gesture stash
        // Asset-manager redesign, Plan 1 Task 12: the unified create dialog's
        // cross-frame state (a modal outlives the draw that opened it). Set up
        // by the ONE entry (BeginCreateAsset) and drained by the ONE
        // dispatcher (ConsumeCreateResult) -- spec s7's invariant, "no
        // creation path may bypass CreateAssetRequest", is exactly the claim
        // that nothing else in this class opens a create flow.
        Arcane::Editor::CreateDialogState       m_createDialog;
        // Asset-manager redesign, Plan 1 Task 5: the pure, cached model behind
        // the Assets panel's Browse lens -- kept current every frame ahead of
        // ANY panel draw (DrawEditorUi's RebuildIfDirty call, immediately
        // before the Assets panel).
        Arcane::Editor::AssetPanelModel         m_assetModel;
        // Asset-manager redesign, Plan 2 Task 5: the session-only activity
        // ring behind the Assets panel's activity feed (spec s9.2, Task 8
        // draws it). MAIN-THREAD ONLY, no mutex -- same precedent
        // m_cookDiagnostics/CookDiagRow documents at their own declaration
        // below (:1368-1379 as of this task): every push site is one of
        // this class's own main-thread-only poll/pump/dispatch paths
        // (PollAssetWatch, OnCookCompleted, OnArtifactRefused,
        // ConsumeCreateResult, DoSaveScene, PollDiagnosticReports), so no
        // cross-thread access is ever reachable here either. Pushed BESIDE
        // the existing m_assetModel.MarkDirty/MarkAllDirty calls at those
        // same seams, never in place of them. Cleared on every project
        // switch (SwitchProject, EditorApp.cpp) -- session-only, like the
        // model itself is rebuilt fresh.
        Arcane::Editor::AssetActivityLog        m_assetActivity;
        // Asset-manager redesign, Plan 2 Task 5: resolves a guid to the
        // activity log's `name` snapshot when only the guid is in hand
        // (the cook-completion/refusal seams) -- project->Registry().
        // Resolve(guid)'s mount path, filename only. Empty string (never a
        // fallback string) when there is no current project or the guid is
        // unresolvable; the feed row falls back to the guid string itself
        // (Task 8). A NAMED helper rather than an inline lambda at each of
        // the three call sites, specifically so Task 7's recook-diagnostics
        // consumer can reuse it too.
        [[nodiscard]] std::string NameOfAsset(const Arcane::Guid& guid) const;
        // Asset-manager redesign, Plan 1 Task 8: LIVE 64px material
        // thumbnails, harvested from a lazily-created offscreen vehicle and
        // persisted to <project>/Saved/Thumbnails (see the class's own header
        // for the whole shape). OWNED here -- Task 7 left a non-owning raw
        // pointer for its null-guard, and this is the ownership shape it
        // deferred to this task.
        //
        // DECLARED AFTER m_graphChrome, which is the borrower-dies-first half
        // of the rule EditorApp::ShutdownGraphPath states: the harvester's
        // preview vehicle BORROWS the chrome context's device. Declaration
        // order is the backstop; ShutdownGraphPath's explicit
        // m_materialThumbs->Shutdown() is what actually closes it, for the
        // same reason it closes the documents' previews there rather than
        // trusting member order (both contexts are reset by that function,
        // long before ~EditorApp runs).
        //
        // Null ONLY between construction of this object and Init -- every use
        // is guarded, which also keeps every headless/failed-boot path safe.
        std::unique_ptr<Arcane::Editor::MaterialPreviewHarvester> m_materialThumbs;
        // Task 8's two fan-outs, both of which need the model/providers this
        // class owns and therefore live here rather than inside the harvester
        // (which knows nothing about an asset registry):
        //   * a material changed (saved, edited on disk, or an ancestor of it
        //     changed) -- invalidate it AND every registry material whose
        //     parent chain passes through it, because an instance's picture is
        //     its base's picture plus overrides;
        //   * a batch of textures finished cooking -- invalidate every
        //     material that References any of them, because their declared
        //     params just changed pixels.
        //
        // The cook variant takes the WHOLE batch rather than one guid: each
        // call costs one registry walk with a refsFor per material, and a
        // first-cook pass over a big project reports hundreds of guids at
        // once -- per-guid calls would be that walk hundreds of times over.
        void InvalidateMaterialThumb(const Arcane::Guid& material);
        void InvalidateMaterialThumbsForTextures(const std::vector<Arcane::Guid>& textures);
        // The model's facade seam, built once per project open
        // (MakeAssetPanelProviders, EditorAppProject.cpp) and cached here --
        // rebuilding it every frame would be pointless closure churn for
        // something that only ever reads through m_runtime/m_cookDiagnostics,
        // both already stable for the lifetime of one open project.
        Arcane::Editor::AssetPanelProviders     m_assetPanelProviders;
        // Builds m_assetPanelProviders' three callables: surfaceFor/refsFor
        // forward straight to the Assets facade (Tasks 1-3); cookStateFor
        // composes CookStateOf(kind, HasPermanentCookDiag(g), IsCookPending(g, kind))
        // -- see CookStateOf's own header comment for why pending is gated on
        // kind (only Texture/Sprite have a cook pipeline; IsCookPending's
        // answer is meaningless for a kind the cook never enumerates, and
        // would otherwise show every JSON asset as permanently Queued).
        Arcane::Editor::AssetPanelProviders MakeAssetPanelProviders();
        // True when m_cookDiagnostics carries a PERMANENT row for `id` (a
        // refusal) -- as opposed to IsCookPending's own reading of the SAME
        // map, which answers an ABSENT row by asking the artifact store. See
        // CookDiagRow::permanent's own declaration for the permanent/
        // transient split.
        [[nodiscard]] bool HasPermanentCookDiag(const Arcane::Guid& id) const;
        // Asset-manager redesign, Plan 1 Task 7: resolveAssetThumb's helper
        // for the Sprite branch. A sprite entry's refs (m_assetPanelProviders.
        // refsFor) always carry EXACTLY ONE texture ref -- DerivesFrom for the
        // 1:1 auto-mint fold, References for a sliced sub-rect sprite (see
        // AssetPanelModel.cpp's own `sliced` derivation) -- so this returns
        // the first ref's target, kind-agnostic, either way. A nil Guid means
        // no answer (no provider, no refs, or an empty list); the caller's
        // `!tex.IsValid()` guard handles that.
        [[nodiscard]] Arcane::Guid FirstTextureRefOf(const Arcane::Guid& guid) const;
        double m_editorClock = 0.0;   // the compile service's Poll/Submit clock

        // Asset file watcher: a ~1 Hz mtime sweep over the registry's .arcmat
        // files (ShaderLibrary's hot-reload pattern) AND, since F2b Task 12,
        // every AssetKind::Texture source PLUS its .meta sidecar -- a
        // hand-edited or inspector-written .meta is a cook trigger exactly
        // like a source edit (spec s7 as amended). An EXTERNAL .arcmat change
        // (git pull, sibling repo, hand edit) invalidates the sprite cache
        // and reloads/refreshes open documents; a texture/.meta change calls
        // m_cookQueue->NoteChanged() (watcher-triggered, hash-decided, never
        // blocks -- see CookQueue.hpp). Our own saves re-baseline through
        // onAssetSaved (the material half) so they never bounce back; the
        // editor never hand-writes a texture's PIXELS in place, only its
        // .meta sidecar (the inspector's four cook-setting knobs), so THAT
        // write re-baselines itself the same way immediately below.
        //
        // F2b desk-checkpoint fix: PollAssetWatch's sweep above can only
        // watch what the registry ALREADY knows -- a .png dropped into
        // Content/ mid-session has no registry entry and is invisible to it.
        // A content-discovery step (Project/ContentDiscovery.hpp) runs on
        // its own, slower cadence (m_contentDiscoveryNext below) ahead of
        // the sweep: it enumerates Content/ for .png candidates the registry
        // doesn't know yet and registers each one (Runtime::
        // RegisterCreatedAsset), so the freshly-minted texture entry hits
        // the SAME first-sighting-counts-as-a-change branch in the sweep
        // below, in the SAME tick.
        void PollAssetWatch();
        std::unordered_map<std::string, std::filesystem::file_time_type> m_materialMtimes;
        double m_materialWatchNext = 0.0;
        // Gates the content-discovery step above -- deliberately a SEPARATE,
        // slower-cadence gate from m_materialWatchNext (~2s vs. ~1s): that
        // step pays for a full recursive directory walk of Content/ (cost
        // proportional to total file COUNT under Content/, never file
        // size -- no reads, no hashing), a materially different cost shape
        // than the sweep's own per-ALREADY-KNOWN-file stat() calls, so it
        // does not need to run on every single watcher tick to still land
        // well inside the spec's "cooks in background" ergonomic. Reset
        // (with m_materialWatchNext) in ResetPerProjectState so a project
        // switch's first tick discovers immediately, same as the sweep's own
        // open-heal convention.
        double m_contentDiscoveryNext = 0.0;

        // ---- Background texture cook (F2b Task 12) --------------------------
        // CookQueue itself is per-project (its CookSession's staleness memo
        // and its projectDir are meaningless across a switch) -- constructed
        // in OnProjectOpened, destroyed in ResetPerProjectState, same
        // lifetime shape as m_resolver's per-project caches.
        std::optional<Arcane::Editor::CookQueue> m_cookQueue;
        // Desk-fix 2 (the cook-pending quiet seam): true from the moment
        // m_cookQueue is (re)constructed in OnProjectOpened until the FIRST
        // CookProject pass for THIS project finishes (OnCookCompleted's first
        // invocation after that construction) -- the "still settling" window
        // that covers the boot race: frame 1's scene resolution runs strictly
        // before the watcher's own first tick (PollAssetWatch, ~1 Hz), so
        // EVERY texture guid the boot scene references is genuinely Missing at
        // that instant, not broken. PollAssetWatch's own first tick forces
        // that first pass to happen (an explicit NoteChanged() call, gated on
        // THIS flag, not on any texture entry actually changing) specifically
        // so this window closes even for a project with ZERO texture sources
        // -- CookSession::CookProject's own source enumeration is empty in
        // that case, so the forced pass completes trivially (no cooks, no
        // failures) and still flips this flag, rather than leaving it stuck
        // true for a session that never happens to trigger a cook.
        // DELIBERATELY NOT forced any earlier (e.g. here in OnProjectOpened,
        // during boot stages) -- see PollAssetWatch's own comment for the
        // DEVICE_LOST a first cut of this hit by starting real background
        // cook work before Main() -> CreateGraphVehicles() had a device up.
        // Read (never written) by the SetCookPendingProbe closure installed on
        // the Assets facade in the SAME OnProjectOpened block -- MAIN-THREAD
        // ONLY, no mutex, same invariant m_cookDiagnostics documents below (the
        // probe fires from the same main-thread-only scene-resolution /
        // NriTextureCache::Resolve path). Reset to false in
        // ResetPerProjectState alongside m_cookQueue itself.
        bool m_cookQueueSettling = false;
        // Drains m_cookQueue's finished passes once per frame -- see
        // PumpEditorDocuments (EditorAppFrame.cpp), the same safe window
        // (strictly after this frame's render, strictly before the next
        // one's) SceneRenderResolver::InvalidateMesh's own doc comment
        // already establishes for GPU-adjacent invalidation.
        void PollCookQueue();
        // The completion handler CookQueue's SetOnCookComplete callback
        // (installed once, in OnProjectOpened) forwards into -- see
        // EditorAppProject.cpp for what it actually does (Assets::
        // InvalidateArtifact, NriGraphContext::InvalidateContentTexture/
        // InvalidateMeshAlbedoSlot, m_cookDiagnostics bookkeeping, and --
        // desk-fix 2 -- closing m_cookQueueSettling's window on its first
        // call after a project open).
        void OnCookCompleted(const Arcane::AssetPipeline::CookResult& result);

        // One row per guid this session has seen refused (via the per-
        // refusal observer, Assets.hpp) or failed to cook (via CookQueue's
        // completion callback) -- accumulated across the WHOLE session,
        // republished in full under "diagnostics:cook" every time it
        // changes (the Diagnostics publication-group contract: a producer
        // owns a key and republishes its ENTIRE set). `permanent` is what
        // IsCookPending below reads: true for HashMismatch/
        // VersionNewerThanEngine/a CookSession import failure (none of
        // these will ever resolve without a user fixing the source), false
        // for ArtifactMissing (presumed still-cooking until proven
        // otherwise). A row is ERASED (not merely left stale) the moment
        // the SAME guid appears in a LATER CookResult::cookedGuids -- "refuse,
        // never limp... until fixed" extended to the Problems pane.
        struct CookDiagRow
        {
            Arcane::Diagnostic diagnostic;
            bool permanent = false;
        };
        // MAIN-THREAD ONLY, no mutex -- unlike CookQueue's own m_results
        // (worker -> main handoff), every touch of this map happens on the
        // main thread by construction: OnCookCompleted writes it from
        // inside Pump() (this class's own threading contract -- Pump() is
        // where a worker's result crosses back to the main thread), and
        // OnArtifactRefused writes it from wherever an Assets accessor
        // refuses, which is always scene resolution / NriTextureCache::
        // Resolve, both main-thread-only by THEIR OWN contracts (see
        // NriTextureCache.hpp's "CALL AT DECLARATION TIME ONLY"). IsCookPending
        // reads it from the SetCookPendingOracle callback, invoked from
        // inside that same main-thread-only Resolve() path, and from
        // cookStateFor during an AssetPanelModel rebuild, which is likewise
        // main-thread-only. No cross-thread access is ever reachable.
        std::unordered_map<Arcane::Guid, CookDiagRow> m_cookDiagnostics;
        // Republishes m_cookDiagnostics's CURRENT contents under
        // "diagnostics:cook" -- called after every mutation of the map
        // (never left for the next frame to notice, since a refusal can
        // land off the per-frame cook-queue rhythm entirely, from ordinary
        // scene resolution).
        void PublishCookDiagnostics();
        // The per-refusal observer (Assets.hpp's SetArtifactRefusalObserver)
        // installed once, in Create() -- see Assets.hpp's own doc comment
        // for why this fires EVERY refusal rather than just the process's
        // first. `user` is always `this`.
        static void OnArtifactRefused(const Arcane::Guid& id, const char* kind, void* user);
        // "Is a cook still outstanding for this guid" -- TWO consumers: the
        // viewport graph's texture cache (NriTextureCache::
        // SetCookPendingOracle, via NriGraphContext) chooses checkerboard-vs-
        // refused with it, and MakeAssetPanelProviders' cookStateFor turns it
        // into the Assets panel's Queued/Cooked badge.
        //
        // A row in m_cookDiagnostics answers directly (see that map's
        // `permanent` field). An ABSENT row is answered by the ARTIFACT
        // STORE -- CookSession::ResolveCurrentArtifactPath on the current
        // cook key -- NOT by a presumption: 2026-09-08 desk fix, since a
        // HEALTHY asset never has a row at all (a cook success ERASES one),
        // so the old "no row means pending" default left every healthy
        // texture/sprite permanently Queued. See the definition
        // (EditorAppProject.cpp) for the full account, the sprite-through-its-
        // texture rule, and the cost note.
        //
        // NOT reused by the Assets facade's own SetCookPendingProbe closure
        // (desk-fix 2, revised after a review finding): row-absence must
        // never be what decides whether RefuseArtifact fires AT ALL -- that
        // would be a closed loop for any guid CookSession will never attempt
        // (see OnProjectOpened's own comment on the probe for the full
        // account). The probe instead asks a POSITIVE question of its own: is
        // the cook queue doing active work, and does this guid's registered
        // source still exist on disk. Keeping the two separate ALSO keeps
        // this function's artifact-store ask off the facade's own refusal
        // path, where it would be asking the cook-pending seam to answer
        // itself.
        //
        // `kind` IS REQUIRED, and gates the store ask (review round 1): only
        // Texture/Sprite cook, the store ask walks Content/, and every caller
        // already knows the kind -- so a non-cooking kind is answered false
        // for free rather than after an O(content-tree) walk whose answer
        // CookStateOf would then discard anyway. Callers pass the guid's REAL
        // kind: cookStateFor resolves it through the registry, and the render
        // oracle passes Texture, the only kind NriTextureCache resolves.
        [[nodiscard]] bool IsCookPending(const Arcane::Guid& id,
                                          Arcane::Editor::AssetKind kind) const;
        // Removes Artifacts/** files this project's registry no longer names
        // any live guid for (ArtifactStore::SweepOrphans) -- called once, at
        // project open (OnProjectOpened), NOT on every cook pass: this is
        // garbage collection for guids that were REMOVED from the project
        // entirely, unrelated to CookQueue's own per-source staleness
        // decisions. RebuildIndexFromScan is called first (SweepOrphans'
        // own header comment: it sweeps the IN-MEMORY index, which starts
        // empty every call).
        void SweepArtifactOrphans();

        // ---- Async file-dialog inbox (architecture pass sec 2) --------------
        // One DialogSlot per dialog kind. The old six pending strings + three
        // mutexes + the unguarded m_pendingInstanceParent sidecar live here now.
        //
        // TASK 12 RETIRED THREE OF THESE. `materialNew`, `meshMaterialNew` and
        // `instanceNew` each existed to carry an OS save-dialog's picked path
        // back to a mint. All three creation flows now go through the in-editor
        // CreateAssetDialog instead (spec s7's invariant), which needs no
        // async slot at all: it returns its result synchronously, in the same
        // frame, from ImGui. `instanceNew`'s payload struct, its
        // InstanceDialogRequest, and the InstancePickedThunk trampoline that
        // filled it went with it -- PathPickedThunk (DialogSlot.hpp) is the
        // only trampoline left, serving the OS file pickers (scene open/save,
        // project open, the material OPEN path, and settings path:file /
        // path:dir Browse). DialogInbox / PathDialogRequest / PathPickedThunk
        // live in DialogSlot.hpp so ArcaneTests can fire the same trampoline
        // the OS picker does.
        DialogInbox m_dialogs;

        // Mint a GRAPH-owned .arcmat (UE-model: nodes are the authoring tier)
        // + open its doc. Legacy text-owned files still open via OpenPath.
        // `surface` selects the .arcmat kind: Fullscreen (default) mints
        // kind="fullscreen" with the graph-owned starter (Color -> Output);
        // Sprite (asset-manager Plan 1 Task 12) mints kind="sprite" with the
        // SAME starter graph generated against the sprite surface; Mesh (F2b
        // Task 13) mints kind="mesh" with NO snippet and NO graph -- mesh
        // materials stitch no shader source at all (MaterialSurface's own
        // comment, Material/MaterialSource.hpp) -- and instead saves the
        // F2a-declared `baseColor`/`albedo` params MeshMaterialCache reads
        // directly. ALL THREE SURFACES ARE NOW MINTABLE: sprite used to be
        // reachable only by re-kinding an already-created fullscreen document
        // through the shader editor's surface picker.
        //
        // Both return the REGISTERED guid (the one Project::RegisterAsset
        // assigned, which is what the panel model and every Guid reference
        // key off), or Nil on any failure -- the same "Nil on failure" shape
        // MintOrReuseSpriteForTexture/MintMeshAsset below already use, so the
        // one dispatcher (ConsumeCreateResult) can report a failed create
        // through ModalErrorQueue and select a successful one, without a
        // second lookup.
        //
        // CreateMaterialFileAt is CreateMaterialAt's file-production half, WITHOUT
        // the OpenPath below it -- factored out (2026-09 review, Task 15) so
        // import-time automation (EnsureMeshImportBaseMaterial) can mint the shared
        // mesh-import base material without popping open a Material Editor tab as a
        // side effect of a background import; the same reasoning MintImportMaterials'
        // own instance-mint arm already applies to itself. CreateMaterialAt is now a
        // thin wrapper: call this, then OpenPath the result for the user-dialog path.
        Arcane::Guid CreateMaterialFileAt(std::filesystem::path path, Arcane::MaterialSurface surface);
        Arcane::Guid CreateMaterialAt(std::filesystem::path path,
                                      Arcane::MaterialSurface surface = Arcane::MaterialSurface::Fullscreen);
        Arcane::Guid CreateInstanceAt(std::filesystem::path path, Arcane::Guid parent);
        // Reuse-or-mint policy (sprite-asset spec, Section 3): exactly one
        // registered .arcsprite referencing `textureGuid` -> reuse its id;
        // zero or several -> mint a fresh sibling .arcsprite next to the
        // texture (never guess among duplicates). Nil on failure (no project,
        // invalid input, or an unresolvable/unwritable path). Never opens a
        // dialog; the caller (browser action consumer / Inspector drop
        // branch / Task 13's ConsumeCreateResult) decides whether to also
        // open a document.
        //
        // `target`, Task 13: the dialog's validated Name+Location choice.
        // Null (the pre-Task-13 shape, still what the quick "Create Sprite"
        // row/context action passes) keeps the auto-placed-sibling branch
        // exactly as it was, "-N" loop included; non-null replaces ONLY that
        // placement on the fresh-mint branch -- the reuse check above still
        // runs first and still wins on a single match, `target` or not, so a
        // dialog Create against an already-wrapped texture reuses rather than
        // minting a same-texture duplicate under a different name. No "-N"
        // loop needed on this branch: ValidateCreateName already proved the
        // path unique at Create-click time (same shortcut CreateMaterialAt/
        // CreateInstanceAt take on their own dialog-supplied path).
        Arcane::Guid MintOrReuseSpriteForTexture(const Arcane::Guid& textureGuid,
                                                 const std::filesystem::path* target = nullptr);
        // F2a, Task 9: mints a fresh .arcmesh at `target` -- MeshAssetData's
        // own defaults (a unit Cube) are already a complete, valid asset, so
        // identity is the only thing this sets. A mesh needs no reuse-or-mint
        // policy the way a sprite does (there is no source asset to key reuse
        // off). Nil on failure (no project); never opens a dialog or a
        // document -- same "the caller decides" split as
        // MintOrReuseSpriteForTexture.
        //
        // Task 13: `target` used to be computed internally (a hardcoded
        // ".../Content/New Mesh[-N].arcmesh", "-N" uniquify loop included) --
        // now it is the caller's (ConsumeCreateResult's) dialog-validated
        // Name+Location, unique already, so no loop survives here either.
        //
        // F4 plan 1 Task 11 (spec s8): `source` is the generator the file is
        // saved with -- the `Create > Mesh > <primitive>` preset the request
        // carried, or Cube (MeshAssetData's own default) when it carried none.
        Arcane::Guid MintMeshAsset(const std::filesystem::path& target,
                                   Arcane::MeshSource source = Arcane::MeshSource::Cube);

        // F4 plan 1 Task 11 (spec s8): the scene's `Add > 3D Object > <primitive>`
        // binds its MeshRenderer to `Content/meshes/<Name>.arcmesh`
        // (CreateAssetDialog.hpp's PrimitiveMeshRelativePath -- lowercase
        // `meshes/`, the folder ReferenceProject already carries; the spec's
        // `Content/Meshes/` names the same directory on Windows and the registry
        // must never key it two ways), MINTED ON FIRST USE AND REUSED BY PATH
        // AFTER -- the EnsureMeshImportBaseMaterial shape (the path IS the
        // identity: an asset already registered there is returned; else it is
        // minted through MintMeshAsset(target, source) with the folder created
        // on demand), in the MintOrReuseSpriteForTexture lineage. Nil on
        // failure (no project, a non-primitive source, or the mint itself
        // failing -- already ARC_WARN'd). Never opens a document: the caller
        // is spawning an entity, not editing the asset. Registry/scene-time
        // automation, not the user dialog path -- the "no creation path may
        // bypass CreateAssetRequest" invariant governs the dialog only (spec
        // s6's own note, the same reading MintImportMaterials takes).
        Arcane::Guid MintOrReusePrimitiveMesh(Arcane::MeshSource source);

        // F2c Task 14 (spec s4.2, R3): the companion .arcmesh mint after the FIRST
        // successful cook of an imported model, and name-keyed slot reconciliation
        // (MeshImportWave.hpp's ReconcileSlots) on every re-cook after that. Same
        // reuse-or-mint shape as MintOrReuseSpriteForTexture above -- a registry scan
        // for the (at most one) existing companion, mint fresh on zero/several, update
        // in place on exactly one, never guessing among duplicates.
        //
        // Called ONLY from OnCookCompleted, for a guid that just finished cooking AND
        // classifies as AssetKind::Model -- never a general-purpose entry point, and
        // never before OnCookCompleted's own Assets/mesh-artifact invalidation for
        // `modelGuid` has already run (this function's own definition explains why: it
        // reads the FRESH artifact, not the memo the cook just replaced). A no-op when
        // no project is open, `modelGuid` is invalid, or the fresh mesh artifact is not
        // (yet, or ever) available -- a refused/failed cook can still reach this call,
        // and that is not an error here.
        // Returns the companion .arcmesh guid when one was found or minted
        // (including a no-op slot-reconcile recook -- geometry may still have
        // changed). nullopt when nothing to invalidate.
        std::optional<Arcane::Guid> MintOrUpdateCompanionMesh(const Arcane::Guid& modelGuid);

        // F2c Task 15 (spec s6, R4): the shared import base -- `Content/
        // mesh_import_base.arcmat` (MaterialSurface::Mesh, baseColor white, albedo
        // nil) at that FIXED path. The path IS the identity: an already-registered
        // asset there is REUSED (returns its guid); otherwise this is the first
        // import and it is minted via CreateMaterialFileAt (the FILE half only,
        // never CreateMaterialAt -- 2026-09 review: this is background import
        // automation, so it must not pop open a Material Editor tab as a side
        // effect), which already produces exactly this file (kind="mesh", the two
        // F2a params, no snippet/graph -- CreateMaterialFileAt's own comment).
        // Never uniquifies, never overwrites. Nil on failure (no project open, or
        // the mint itself fails).
        Arcane::Guid EnsureMeshImportBaseMaterial();

        // F2c Task 15 (spec s6, R4 steps 1-3): per glTF material in `survey`, in
        // order -- reuse-by-name first (FindReusableMeshMaterial over the registry's
        // MESH-surface materials; a hit means NOTHING is created), else mint a fresh
        // INSTANCE of EnsureMeshImportBaseMaterial()'s base at
        // `UniqueSiblingPath(source.parent_path(), ...)` with sparse overrides
        // (`baseColor` from `baseColorFactor`, `albedo` from the extracted/
        // registered base-color texture when one exists). One `ARC_WARN` per file,
        // per material, naming every `droppedInputs` entry (spec s6's list).
        //
        // THE INVARIANT: import never overwrites an existing `.arcmat`. The reuse
        // arm creates nothing; the mint arm can only ever land on a path
        // UniqueSiblingPath has already proven free. Companion mints in the
        // MintOrReuseSpriteForTexture lineage (registry/import-time automation) --
        // the "no creation path may bypass CreateAssetRequest" invariant governs the
        // user dialog path only, and is untouched here (spec s6's own note).
        //
        // Returns glTF material name -> the guid its slot should point at (reused or
        // freshly minted); a name absent from the map means the mint itself failed
        // (already ARC_WARN'd) -- the caller leaves that slot exactly as it was.
        // `modelGuid` resolves the import's own source path (for placement); empty
        // map when there is no project, `modelGuid` does not resolve, or the base
        // material could not be ensured.
        std::unordered_map<std::string, Arcane::Guid> MintImportMaterials(
            const Arcane::Guid& modelGuid, const Arcane::AssetPipeline::GltfSurvey& survey);

        Arcane::Editor::DocServices MakeDocServices();

        // Problems-panel row click -> editor navigation. One switch over
        // ClassifyLocator's RouteAction (node-page phase s8.2): select the
        // entity, open + focus the document (a shader at its line), reveal an
        // editor-less asset, raise a graph node, Explorer for directories and
        // binaries, open-as-text otherwise. Performed from the frame loop,
        // never mid-draw.
        void RouteLocator(const Arcane::DiagLocator& locator);
        // The Problems router's facts over live app state (node-page phase s8.2):
        // entity alive, guid -> path, has an editor, directory, exists.
        [[nodiscard]] Arcane::Editor::RouteFacts MakeRouteFacts();

        // A thin wrapper RouteLocator needs, added here rather than on
        // DocumentHost: DocumentHost only indexes documents by asset Guid
        // (its own header comment -- "open/dirty/save lifecycle over one GUID
        // asset"), so Guid->path resolution is an
        // EditorApp-level concern, the same way MintOrReuseSpriteForTexture
        // above resolves through m_runtime->CurrentProject() rather than
        // living on DocumentHost.
        //
        // Resolve `guid` through the open project's registry and open/focus
        // its document (DocumentHost::OpenPath's focus-not-reopen). Null with
        // no project, an invalid guid, or an asset that does not resolve to a
        // file (mirrors MintOrReuseSpriteForTexture's failure shape).
        Arcane::Editor::EditorDocument* OpenAssetDocument(const Arcane::Guid& guid);

        // THE ARCANE LOGO shown at the left of the transport toolbar
        // (Unity-style), decoded device-free (Arcane::LoadDisplayPixels) and
        // uploaded by the CHROME context's NriTextureCache under a synthetic
        // per-run Guid -- with ColorSpace::Display, because ImGui draws AFTER
        // the tonemap and an sRGB view under it decodes a second time and
        // reads dark.
        //
        // The pixels are RETAINED (rather than decoded on demand) because the
        // cache's supply is a callback it may invoke at any Resolve, and this
        // one answers exactly one Guid.
        //
        // NO INVALIDATE OBLIGATION, unlike the viewport output, and the reason
        // is worth stating: this texture lives in the SAME context as the
        // ImGuiNri that caches it, so ~NriGraphContext's own ordering covers
        // it (nodes Release() before m_textures->Release(), i.e. the backend's
        // view is buried before the cache's texture). Cross-context is what
        // needs a caller-side hook; same-context does not.
        Arcane::Guid      m_graphLogoId;
        Arcane::PixelData m_graphLogoPixels;
        std::uint64_t     m_graphLogoTexture = 0;   // ImTextureID (raw nri::Texture*)
        // The toolbar's mark -- 0 = no mark, which DrawSimTimeToolbar already
        // treats as "skip it".
        [[nodiscard]] std::uint64_t ToolbarLogoTextureId() const noexcept
        {
            return m_graphLogoTexture;
        }

        // File -> Open Project (soft-restart). The menu sets menuReq.openProject;
        // DrawEditorUi launches the async .arcproj FILE dialog via the shared
        // PathPickedThunk trampoline, targeting m_dialogs.projectOpen; the next
        // frame's ConsumeProjectDialogResult Take()s the slot and (outside any
        // lock -- DialogSlot owns its own) runs SwitchProject at a safe point.
        // (Project::Open accepts the .arcproj file directly.)
        void SwitchProject(const std::filesystem::path& path);  // validate-then-soft-restart
        // Reset every mutable member whose value refers to the current
        // project. Called by SwitchProject's switch_teardown stage only --
        // boot has no prior project to reset. Full per-member rationale is
        // the comment block above this function's definition in
        // EditorAppProject.cpp -- that is the single owned reset list new
        // per-project members must join (architecture pass sec 3, audit
        // defect A3).
        void ResetPerProjectState();

        // The project-open SUCCESS tail (architecture pass sec 5) -- previously
        // duplicated verbatim across StageFinalize and a hand-rolled
        // switch_plugin_load stage, with a partial third copy in the switch
        // failure fallback. Now a single function called from all three sites:
        // StageFinalize, SwitchProject's plugin_load stage (which since Task 12
        // reuses the same StagePluginLoad body -- switch_plugin_load no longer
        // exists as a separate stage), and the switch failure fallback.
        // recordRecents=false is the fallback's case: it re-establishes the
        // PROJECT-LESS baseline (or the old project), and a refused open must
        // never reorder the recents lists. Adopts the current project's boot
        // scene (if m_undo and a project exist), then
        // EnsureScene/UpdateWindowTitle/NoteProjectOpened.
        // Does NOT call RetargetLayoutIni() -- every call site still owns that
        // separately, immediately after, since it is not part of the "a project
        // (or none) is now open" fact this tail represents (see each site).
        void OnProjectOpened(bool recordRecents = true);

        // ---- Build -> Rebuild Game Module (ModuleBuild.hpp) -----------------
        // StartModuleRebuild resolves arcbuild.exe beside the editor (else
        // ../arcbuild/), composes `arcbuild build --project <root> --config
        // <editor's flavor> --sdk <the RUNNING editor's root>` and starts the
        // worker; the driver runs premake then msbuild, forcing /t:Rebuild
        // only when Binaries/ holds the other configuration's DLL (spec
        // 2026-09-13 s4.3). The menu item is greyed while playing/building/
        // module-less, and this re-checks the same gates for any future
        // caller. PollModuleBuild is the per-frame drain (EditorAppProject.cpp,
        // called from MainLoop's top-of-frame consume block): worker lines ->
        // "Build: "-prefixed Console entries; on finish, failure publishes ONE
        // Problems row under the "build:<root>" key and success clears it,
        // restamps a stale manifest abi (the module now matches this engine),
        // and -- when the module had been REFUSED at open (stale ABI), which
        // left no PluginHost watching -- re-engages the host the way
        // StagePluginLoad does. A LIVE host needs nothing here: the existing
        // debounced watcher (PluginHost::Poll, EndFrame) sees the fresh DLL
        // mtime and hot-reloads with state on its own; forcing it would race
        // the debounce.
        void StartModuleRebuild();
        void PollModuleBuild();
        Arcane::Editor::ModuleBuild::Runner m_moduleBuild;   // joined in Shutdown
        // The project root the RUNNING build was started for. Publish/Clear
        // key identity ("build:<root>") + the guard that skips restamp/
        // re-engage when the project switched mid-build.
        // Survives a project switch ON PURPOSE: it guards an async worker --
        // PollModuleBuild's root-mismatch check is the staleness guard. See
        // ResetPerProjectState's rule.
        std::filesystem::path m_moduleBuildRoot;

        // ---- Build -> Open Visual Studio / open source in VS (IdeLaunch.hpp) --
        // OpenInIde(file) opens `file` in the Visual Studio that has the open
        // project's solution loaded -- or, with `file` empty, just brings that
        // solution up (the menu item). Both go: Toolchain::DiscoverSolution
        // -> if none, RegenerateSolution (arcbuild generate, SYNCHRONOUS, its
        // lines to the Console as "Build: ") -> rediscover ->
        // IdeLaunch::OpenSolution/OpenFileAtLine, whose Outcome is logged in one
        // Console line. Reached from Build -> Open Visual Studio
        // (MenuRequests::openIde) and from a Source row's Open
        // (AssetPanelActions::openInIde), EditorAppFrame.cpp.
        //
        // devenv.exe is resolved through IdeLaunch::DevenvCache: on the first
        // project open, then again only when build.ideExecutable changes
        // (Live; vswhere spawns a process, ~100 ms, so never per frame).
        // RefreshDevenv runs at project open, before every menu draw and on
        // every OpenInIde; m_devenv.Path() holds the answer (empty = no
        // install found -> the menu greys with a tooltip).
        // Returns the outcome (no project = NoSolution) so OpenSourceAtLine can
        // fall back to the shell. `line` > 0 puts the caret there (IdeLaunch::OpenFileAtLine).
        IdeLaunch::Outcome OpenInIde(const std::filesystem::path& file, int line = 0);
        // The crash viewer's [file:line] link (node-page phase s8.1): OpenInIde,
        // and on NoDevenv / NoSolution / DetectionFailed, OsShell::ShellOpen(file).
        void OpenSourceAtLine(const std::filesystem::path& file, int line);
        [[nodiscard]] Arcane::Editor::IdeMenuState IdeMenuStateNow() const;
        void RefreshDevenv();
        IdeLaunch::DevenvCache m_devenv;

        // Run `arcbuild generate` alone, SYNCHRONOUSLY, for the open project
        // against the running editor's SDK (ModuleBuild::ComposeDriverCommand
        // / RunCapture), its lines to the Console as "Build: ". Two callers:
        // OpenInIde when no .slnx exists yet, and MintCppClass ALWAYS (the
        // .vcxproj must list the new files before Visual Studio opens them).
        // Returns false when the driver failed (exit != 0 or the shell could
        // not start); the caller decides what that means for its own step.
        bool RegenerateSolution();

        // Assets -> Create -> C++ Class (ConsumeCreateResult's CppClass arm):
        // render `templateKind` (a ClassTemplates::Kind) for `className` in
        // the project's namespace, write the header (at `headerTarget`) and
        // the source (beside it, when the kind has one -- refused if either
        // already exists), register both under source://, regenerate the
        // solution, and return the guid of the file to open (the .cpp when
        // there is one, else the .hpp); nil on failure with the reason in the
        // Console. Opening it in Visual Studio is the dispatcher's post-mint
        // step, same "the caller decides" split as MintMeshAsset.
        Arcane::Guid MintCppClass(const std::filesystem::path& headerTarget,
                                  const std::string& className,
                                  int templateKind,
                                  int systemPhaseIndex,
                                  int systemRoleIndex);

        // ---- Report-written notify (GPU crash diagnostics arc, Task 9) -----
        // Diagnostics::ReportWrittenHook (Diagnostics.hpp) fires on WHATEVER
        // thread wrote the report -- the hang/gpu-stall watchdog thread for
        // the two survivable kinds, or the faulting thread for a crash
        // (about to terminate; registering an asset there is moot -- there
        // is no next frame left to drain the queue into, so it simply never
        // happens, which is the honest boundary rather than a special case).
        // AssetRegistry has no lock of its own (see AssetRegistry.hpp), and
        // the Problems row's locator needs the Guid RegisterCreatedAsset
        // mints, so BOTH the registration and the Publish happen on the
        // main thread, never inside the hook itself. Same worker-writes/
        // main-drains shape as ModuleBuild::Runner (DrainLines() /
        // PollModuleBuild, above) -- OnReportWritten only pushes a path
        // into this mutex-guarded queue; PollDiagnosticReports (per-frame,
        // called beside PollModuleBuild) drains it and does the real work.
        static void OnReportWritten(const std::filesystem::path& diagPath, void* user);
        void PollDiagnosticReports();
        // Both members below are on ResetPerProjectState's reset list
        // (EditorAppProject.cpp) -- see that function's member-rationale
        // comment block for the entry. m_pendingReports because a path
        // queued against the outgoing project and drained after the switch
        // would try to RegisterCreatedAsset it into the WRONG (incoming)
        // project's registry; m_reportDiagnostics because its rows carry
        // DiagLocator::Asset(guid) values that exist only in the outgoing
        // project's registry -- surviving a switch would resurrect stale
        // rows whose click silently no-ops and whose detail line names a
        // file from a project that is no longer open (the switch-staleness
        // class ResetPerProjectState exists to institutionalize against).
        std::mutex                         m_pendingReportsMutex;
        std::vector<std::filesystem::path> m_pendingReports;      // guarded by m_pendingReportsMutex
        // KEY OWNERSHIP: "diagnostics:reports" -- accumulated across the
        // CURRENT project's session (one row per report successfully
        // registered against it), so an earlier report's row survives a
        // later report's Publish (publication-group replace semantics --
        // Diagnostics.hpp) UNTIL a project switch resets it. Main-thread
        // only, like the queue above -- PollDiagnosticReports is the only
        // writer.
        std::vector<Arcane::Diagnostic>    m_reportDiagnostics;

        // KEY OWNERSHIP: "assets:create" -- the failed-create trail, spec s7's
        // "failures route through the existing ModalErrorQueue + Problems" and
        // s13's same pairing. Accumulated across the CURRENT project's session
        // exactly like m_reportDiagnostics above (one row per failed create,
        // republished whole so an earlier failure survives a later one under
        // publication-group replace semantics), and reset by
        // ResetPerProjectState for the same reason: every row's detail line
        // names a path under the OUTGOING project's Content tree. Main-thread
        // only -- ConsumeCreateResult, the one create dispatcher, is the only
        // writer. The modal is the immediate notice; this is the trace that
        // outlives the modal's OK button.
        std::vector<Arcane::Diagnostic>    m_createDiagnostics;

#if !defined(ARC_BUILD_DIST)
        // ---- Deliberate GPU fault (GPU crash diagnostics arc, Task 11) ------
        // The desk battery's trigger: Build -> Diagnostics -> Crash GPU
        // (diagnostics test) raises MenuRequests::crashGpu, ConsumeMenuRequests
        // calls this, and everything downstream of the dispatch is the arc's own
        // capture path with nothing special about having been asked for.
        //
        // NO INJECTOR OBJECT: NriDiagnostics::FireFault is stateless -- it
        // creates and drops its own one-off objects per dispatch -- so there
        // is nothing to own, nothing to keep off ResetPerProjectState's list,
        // and no handle whose release order against the device a declaration
        // position here would have to guarantee.
        void FireDeliberateGpuFault();
        // --crash-gpu N fired already. The menu item is deliberately NOT
        // latched by this -- a user who clicks twice meant it twice; the latch
        // exists only so a per-frame schedule fires once.
        bool                                      m_gpuFaultFired = false;

        // --hang-main N (crash window plan 2, D10): the witness hang lane's
        // trigger, honoured by both hosts. Same fired-once shape as
        // m_gpuFaultFired directly above.
        bool                                      m_hangMainFired = false;
#endif

        // Editor state naming entities of the OUTGOING scene, torn down before any
        // registry swap. Shared by SwitchProject and the scene effects below.
        void ClearSceneReferences(std::string reason);
        // Saved/UndoCache follows the project (spec s3.3, R12): wipe the
        // outgoing dir, wipe the incoming one (crash leftovers), retarget the
        // stack. Null = no project = memory-only.
        void RetargetUndoCache(const Arcane::Project* project);
        // Establish an empty scene when nothing published a SceneRoot, so the editor
        // always has one open. Never clears a registry a plugin already populated.
        void EnsureScene();

        // Scene dialog launches, shared by the menu-request site and the
        // unsaved-scene confirm modal (they were one lambda pair inside
        // MainLoop, called from both).
        std::string SceneDialogDir();
        void        ShowSceneSaveDialog();

        // The scene effects. Each returns false when the effect itself failed, and
        // pushes m_modalErrors with the reason under a "Scene Error" title
        // (DrawModals draws the queue's front as a modal).
        bool DoNewScene();
        bool DoOpenScene(const std::filesystem::path& file);
        bool DoSaveScene(const std::filesystem::path& file);

        // Write <projectRoot>/Saved/AutoScreenshot.png from the viewport
        // output -- the editor half of the Arcane Hub's cover-thumbnail
        // chain (Unreal's model). Called on scene save and on clean
        // shutdown; a no-op without a project, a viewport, or a device.
        void WriteAutoScreenshot();
        // One extra VIEWPORT frame with FrameDesc::capture armed, then
        // ReadCapture into the shared thumbnail writer. NEVER the chrome
        // context -- an editor cover is a picture of the SCENE, and see
        // PresentChromeFrame's absence table for why a chrome capture would
        // redefine what a capture means.
        //
        // NOT #if-guarded, for the same reason m_graphChrome is not (see its
        // declaration): a preprocessor-guarded member forces a guard at every
        // use site. Reachable in EVERY configuration, Dist included --
        // CreateGraphVehicles runs unconditionally, so m_viewportTargets.graph
        // is populated everywhere.
        bool CaptureGraphViewportPng(const std::filesystem::path& file);

        // Window title, recomposed from project + scene + dirty state each frame
        // and pushed to the window only when it changes (SetTitle is an SDL call,
        // and the string is identical on the overwhelming majority of frames).
        std::string m_windowTitle;
        void        UpdateWindowTitle();
        // The ONLY assembly of the title parts (s6.4): the project name, the
        // scene's display name, and SceneSession::IsDirty (scene-affecting steps
        // only, after T1 s3.3). The OS title, the toolbar strip and the
        // Viewport tab's dot (ViewportChrome) all read it.
        [[nodiscard]] Arcane::Editor::TitleParts CurrentTitleParts() const;

        // ---- File -> Open Recent / Open Recent Scene ------------------------
        // Both recents lists live behind one facade (EditorRecents.hpp,
        // architecture pass sec 10, B8): the Hub's shared machine-wide project
        // list (m_recents.projects, RecentProjects.hpp) and the PER-PROJECT
        // scene list (m_recents.scenes, <root>/Saved/recent_scenes.json,
        // SceneRecents.hpp) were two fully parallel ~10-site call ladders,
        // always invoked in adjacent lines -- these used to be separate
        // members (m_recents/m_fileMenuWasOpen/m_sceneRecents) with four
        // methods between them.
        //
        // Cached rather than rebuilt per frame: building the project list
        // reads a file and stats every candidate, and the menu bar draws
        // every frame whether or not File is ever opened. Both lists are
        // refreshed at exactly two moments -- a successful project open
        // (NoteProjectOpened) and the frame the File menu is first opened
        // (RefreshAll, m_recents.fileMenuWasOpen tracks the rising edge). The
        // scene list is pushed ONLY from the two scene success paths --
        // DoOpenScene and DoSaveScene, after m_scene.Adopt -- so a refused
        // open or a failed save never reorders or pollutes either list.
        Arcane::Editor::EditorRecents m_recents;

        // The dock node the Viewport occupied LAST frame (0 = floating):
        // where new document windows dock as sibling tabs (DrawAll).
        unsigned int m_viewportDockId = 0;

        // Failure surfacing for project-open, scene, and standalone-launch
        // refusals (architecture pass sec 7): SwitchProject's refusals used to
        // be console-only and were repeatedly missed at the desk, and the
        // scene/launch effects had their own parallel string + popup each.
        // Any refusal/failure Push()es a titled error here; DrawModals draws
        // the queue's front as a blocking modal (main thread only -- pushed
        // inside the effects/SwitchProject/Init, read in the ImGui pass; no
        // lock needed). Errors display one at a time, FIFO, instead of racing
        // for the popup stack.
        Arcane::Editor::ModalErrorQueue m_modalErrors;

        // ---- Asset file operations (T5 s7.3, s7.12) -------------------------
        // Declared AFTER m_undo so the executor (which holds *m_undo) destructs
        // BEFORE the stack it pushes to; Shutdown's RetargetUndoCache(nullptr)
        // resets it earlier still.
        class AssetOpHost final : public Arcane::Editor::AssetFileOpHost   // T5 s7.3: the executor's only door into the app
        {
        public:
            explicit AssetOpHost(EditorApp& a) : m_app(a) {}
            Arcane::Editor::AssetOpGates Gates() const override;   // T5-A9: AssetFileOpHost's first pure virtual
            Arcane::RebindResult Rebind(const Arcane::Guid&, const std::filesystem::path&) override;
            bool Unregister(const Arcane::Guid&) override;
            std::optional<Arcane::Guid> Register(const std::filesystem::path&) override;
            Arcane::Editor::OsShell::RecycleResult Recycle(std::span<const std::filesystem::path>) override;
            bool CloseDocumentFor(const Arcane::Guid&, bool discardDirty) override;
            void NoteMoved(const Arcane::Guid&, const std::filesystem::path&, const std::filesystem::path&) override;
            void AssetsChanged(std::span<const Arcane::Guid>, std::span<const Arcane::Guid>) override;
            void Invalidate(const Arcane::Guid&, Arcane::Editor::AssetKind) override;
            void EvictPaths(std::span<const std::filesystem::path>) override;
            void Activity(Arcane::Editor::AssetActivityEntry) override;
            void ReportError(std::string title, std::string message) override;
        private:
            EditorApp& m_app;
        };
        struct AssetOpFactsStore   // AssetOpFacts' spans point in here; the delete modal keeps its own
        { std::vector<std::pair<Arcane::Guid, std::string>> registry; std::vector<Arcane::Guid> sceneAssets; std::vector<Arcane::Editor::AssetOpFacts::Doc> docs; };
        AssetOpHost m_assetOpHost{ *this };
        // Per project: T5-A9's constructor takes (host, stack, contentDir), m_undo is emplaced in the ctor body and the
        // content dir is the project's, so no member initializer can build it. RetargetUndoCache(project) (T1-B10,
        // EditorAppProject.cpp), after SetSpillDirectory:
        //   m_assetFileOps = project ? std::make_unique<Arcane::Editor::AssetFileOpExecutor>(m_assetOpHost, *m_undo, project->Root() / "Content") : nullptr;
        std::unique_ptr<Arcane::Editor::AssetFileOpExecutor> m_assetFileOps;
        AssetOpFactsStore m_assetOpFacts;
        [[nodiscard]] std::string AssetOpGateReason();
        [[nodiscard]] Arcane::Editor::AssetOpFacts GatherAssetOpFacts(AssetOpFactsStore&, bool withLiveScene);
        std::optional<Arcane::Editor::AssetOpPlan> RunAssetOp(const Arcane::Editor::AssetOpRequest&);
        // T5 s7.7: the post-op selection step every RunAssetOp caller runs on a returned plan (B17 and B20 extend it).
        void AfterAssetOp(const Arcane::Editor::AssetOpPlan&);
        void InvalidateAssetCaches(const Arcane::Guid&, Arcane::Editor::AssetKind);
        // T5 s7.6: the asset page's Rename modal (requestRename opens it).
        Arcane::Editor::RenameModalState m_renameModal;
        // T5 s7.5: the ONE Delete confirm modal (requestDelete opens it). Its
        // facts are gathered ONCE on open (live scene included) into their own
        // store, so the cascade box re-plans against the same facts; reset on
        // Confirm/Cancel.
        Arcane::Editor::DeleteConfirmState m_deleteConfirm;
        AssetOpFactsStore m_deleteFactsStore;
        std::optional<Arcane::Editor::AssetOpFacts> m_deleteFacts;
        void BeginAssetDelete(std::vector<Arcane::Guid>);
        void ConsumeDeleteConfirm();
        // T5 s7.8: the Move to... modal (requestMoveTo opens it) and the New
        // Folder modal (requestNewFolder, or nested from Move to...'s button).
        Arcane::Editor::MoveToState m_moveTo;
        Arcane::Editor::NewFolderState m_newFolder;
        // RefreshLabels runs once per model rebuild (an entriesStamp edge), so
        // a renamed asset's Inspector history labels and pin names follow.
        std::uint32_t m_labelsAtEntriesStamp = 0, m_fileOpRefusalMemoStamp = 0;
        // AssetPanelServices::fileOpRefusal's dry-run memo: request key ->
        // first refusal ("" = runs). Cleared when the model rebuilds, the
        // gate reason changes or the open/boot scene changes (s7.5), so a
        // per-frame caller (the rename box, the page pencil) plans once per
        // distinct request, not once per frame.
        std::unordered_map<std::string, std::string> m_fileOpRefusalMemo; std::string m_fileOpRefusalMemoGate;   // dry-run memo

        // DrawStartPage's rising edge (visible = no project, derived every frame,
        // never latched): refresh recents once per appearance, and focus the
        // page once per appearance on its first docked frame (StepStartPageFocus).
        Arcane::Editor::StartPageFocus m_startPageFocus;

        // Set by SwitchProject when its BootSequence reports the window closed
        // mid-switch (BootResult::quitRequested), instead of treating that
        // as a failed switch (Important 1, 2026-07-31 review). The rationale
        // was that the switch's overlay presenter consumed the OS quit event
        // during its own event pump, so it would not reach PumpFrameEvents'
        // SDL_EVENT_QUIT check on a later frame; that presenter is gone and the
        // switch runs with none, so the flag now only buys exiting on the FIRST
        // click rather than a frame later -- see SwitchProject's own comment.
        // PumpFrameEvents reads this flag first and requests the same
        // normal exit a live quit would, instead of surfacing through m_modalErrors.
        // Safe to check unconditionally: SwitchProject's switch_teardown stage
        // has already reset m_scene to a clean Untitled state by the time this
        // can be set, so there is nothing for the unsaved-changes confirm modal
        // to protect -- an immediate exit is correct, not just convenient.
        bool m_requestExit = false;
    };
}
