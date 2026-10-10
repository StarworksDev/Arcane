// EditorApp, frame loop: MainLoop plus the per-frame phase methods it calls.
// Split out of EditorApp.cpp as a pure move -- every statement kept its
// position relative to every other.
//
// MainLoop is a straight sequence of the phase calls, IN THE ORDER THE ORIGINAL
// STATEMENTS RAN. That order is load-bearing: the render pass has to precede
// the ImGui pass that samples its texture, the camera has to be pushed twice,
// the deferred dialog results have to be consumed before any new request is
// issued, and so on. None of it is covered by a test -- this TU is not compiled
// into ArcaneTests -- so each phase below carries the constraint it
// participates in, and the phase DEFINITIONS are kept in frame order so the
// "above"/"below" locators in their comments stay true.

#include "App/EditorApp.hpp"
#include "App/HarnessRules.hpp"   // UnderVerifyHarness: badges + chip stay out of goldens (s8.2)
#include "Settings/SettingsHost.hpp"
#include "Settings/SettingsWindow.hpp"
#include "Panels/ConsoleModel.hpp"   // ProblemsChip (s8.2)
#include "Panels/EditorPanels.hpp"
#include "Project/OsShell.hpp"   // AssetPathAction's Show in Explorer / Open as text (s4.6)
#include "Project/StartPageModel.hpp"   // DialogStartDir / BuildStartPage (spec 2026-09-30 s8.4)
#include "Scene/PhysicsOverlay.hpp"
#include "Scene/SelectionOps.hpp"
#include "Scene/UndoGate.hpp"   // UndoBarred: Ctrl+Z/Y share the Play barrier (spec s3.3b)
#include "Settings/AxisColors.hpp"
#include "Settings/EditorConsoleSettings.hpp" // editor.console.displayLineCap (the ring follows it; settings S6-41)
#include "Settings/EditorGridSettings.hpp" // editor.viewport.grid.* / grid3D.* (MakeGridScene, settings S6-21)
#include "Settings/EditorPlaySettings.hpp"     // editor.play.launchMode (settings S6-32)
#include "Settings/EditorViewportSettings.hpp" // editor.viewport.* / camera.* / gizmo.* (settings S6-29..32)
#include "Settings/EditorThemeSettings.hpp" // editor.theme.viewport.cameraFrame (settings S6-26)
#include "Settings/EditorUiSettings.hpp"   // editor.ui.* (ApplyAppearanceSettings, settings S4-15)
#include "Settings/EditorUiStyleSettings.hpp"   // editor.ui.* style metrics (settings S6-28)
#include "Settings/LayoutSettings.hpp"     // editor.layout.openPanelsAtStart (Reset Layout, S4-18)
#include "Settings/LayoutPage.hpp"
#include "Panels/LayoutLibrary.hpp"       // ParseOpenPanels
#include "Project/ModuleBuild.hpp"          // ModuleBuild::ExeDir: the bundled font families
#include "Widgets/EditorFonts.hpp"         // the deferred font-atlas rebuild
#include "Viewport/ViewportGrid.hpp"   // the 2D reference grid (F4 plan 1 T9, spec s5.1)
#include "Viewport/ViewportImGuiInput.hpp"
#include "Input/EditorActions.hpp"
#include "Viewport/ViewportActions.hpp"

#include <Arcane/AssetPipeline/CookSession.hpp>   // F2b Task 12: MainLoop's pre-loop cook gate
#include <Arcane/Assets/Assets.hpp>      // Arcane::WritePngRgba (RenderSceneToViewport's capture block)
#include <Arcane/Assets/ImageCompare.hpp>   // --compare (Task 9): PixelData/ImageCompareOptions/CompareImages; also pulls in ImageIo.hpp's LoadPngRgba
#include <Arcane/Audio/AudioDevice.hpp>  // complete type for AudioSystem().Update (per-frame voice reap)
#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::Heartbeat -- the hang watchdog's liveness signal
#include <fstream>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Platform/Paths.hpp>   // EditorUserDir/Fonts: the user font families
#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include "Viewport/GizmoOverlay.hpp"   // ImGuiGizmoSink (the gizmo as foreground chrome)
#include <Arcane/Host/GpuSceneHost.hpp>   // PrepareSceneForRender (F3 plan 1 T8): visible set(s) + GPU-scene sync + the mesh pass's frame, once per viewport frame
#include <Arcane/Host/ReferenceImages.hpp>   // --compare/--bless (Task 9): ResolveReference (MainLoop's pre-loop fail-fast)
#include <Arcane/Input/InputSnapshot.hpp>
#include "Documents/InputActionsDocument.hpp"
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // Arcane::GpuDeviceLostObserved -- the device-loss latch
#include <Arcane/Render/Nri/nodes/PickOutlineNodes.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>   // Physics overlay (spec 2026-09-11-physics-2d-wiring s6.3)
#include <Arcane/Render/RenderLookSettings.hpp>   // render.mesh.defaultLight.* (the scene light, Live)
#include <Arcane/Render/ShaderCompiler.hpp>   // --settle N's IsIdle() quiescence check (Task 9, mirrors RuntimeFrame.cpp)
#include <Arcane/Scene/Components.hpp>   // Arcane::Transform (gizmo drag target)
#include <Arcane/Scene/PhysicsSystem.hpp>   // Arcane::PhysicsWorld2D (physics overlay)
#include <Arcane/Sim/SimSettings.hpp>   // ClampFrameDelta / ApplySimStepCap
#include <Arcane/Host/HostSettings.hpp>   // app.window.minimizedSleepMs
#include "Settings/EditorPerfSettings.hpp"   // editor.perf.backgroundFps
#include <Arcane/Scene/SceneCamera.hpp>  // Arcane::ActiveSceneCamera (Play view + camera rect); Arcane::ActivePerspectiveSceneCamera (the mesh pass's camera)
#include <Arcane/Serialization/SceneAsset.hpp>   // Arcane::Scene::kSceneExt (Save-dialog suffix)

#include <Astra/Registry/Registry.hpp>   // Registry::InspectEntity/GetComponent (gizmo descriptor resolve)

#include <glm/glm.hpp>

#include "Widgets/EditorTheme.hpp"   // Theme::kTextDim (the start page's dim path / time)
#include "Widgets/UiMetrics.hpp"   // Ui::Px -- the modal button widths follow editor.ui.scale
#include "Widgets/IconsLucide.h"   // the start page's Open Project... / Open Folder... icons
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>       // std::isfinite (the camera-rect overlay's projected corners)
#include <cstdint>
#include <cstdio>      // std::snprintf (the Rename modal's prefill)
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <Arcane/Core/Constant.hpp>

namespace Arcane::Editor
{
    namespace
    {
        // --compare / --bless (Task 9). Own private copy, matching
        // RuntimeApp.cpp's (which explains the reasoning in full): this is a
        // FILE PATH COMPONENT ("dx12"/"vulkan", matching HostConfig.cpp's
        // --backend Choices() and the shipped ReferenceProject fixtures
        // byte-for-byte), never Arcane::ToString(backend) ("D3D12"/"Vulkan",
        // the human-facing log/report label used elsewhere in this file).
        // Not shared with RuntimeApp.cpp's copy because the two live in
        // different DLLs/exes with no common host-tier TU to hold it -- see
        // this task's own dispatch for why a third copy (EditorApp.cpp's
        // ShutdownGraphPath also needs one) was the pragmatic choice over
        // inventing shared plumbing for four lines.
        const char* CompareBackendName(Arcane::GraphicsBackend backend)
        {
            return backend == Arcane::GraphicsBackend::Vulkan ? "vulkan" : "dx12";
        }

        // Modifier scancodes still read here for camera orbit (Alt) and the
        // gizmo snap / fly-boost (Ctrl / Shift). Letter keys go through
        // EditorActions (settings S4, spec s7.2).
        ARC_CONSTANT("vendored-library constant: SDL's scancode table; the bindings are editor.keys.* cvars, the codes are not")
        constexpr uint32_t kScLCtrl  = 224;  // SDL_SCANCODE_LCTRL
        ARC_CONSTANT("vendored-library constant: SDL's scancode table; the bindings are editor.keys.* cvars, the codes are not")
        constexpr uint32_t kScRCtrl  = 228;  // SDL_SCANCODE_RCTRL
        ARC_CONSTANT("vendored-library constant: SDL's scancode table; the bindings are editor.keys.* cvars, the codes are not")
        constexpr uint32_t kScLShift = 225;  // SDL_SCANCODE_LSHIFT
        ARC_CONSTANT("vendored-library constant: SDL's scancode table; the bindings are editor.keys.* cvars, the codes are not")
        constexpr uint32_t kScRShift = 229;  // SDL_SCANCODE_RSHIFT
        ARC_CONSTANT("vendored-library constant: SDL's scancode table; the bindings are editor.keys.* cvars, the codes are not")
        constexpr uint32_t kScLAlt   = 226;  // SDL_SCANCODE_LALT
        ARC_CONSTANT("vendored-library constant: SDL's scancode table; the bindings are editor.keys.* cvars, the codes are not")
        constexpr uint32_t kScRAlt   = 230;  // SDL_SCANCODE_RALT

        // ASCII-lowercased extension, for a case-insensitive suffix check: a
        // hand-typed "MyScene.ARCSCENE" already names an .arcscene, and stapling a
        // second suffix onto it produces a file the user did not ask for.
        std::string LowerExtension(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            for (char& c : ext)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return ext;
        }

        // Resolve the ComponentDescriptor for Transform on `e`, for bracketing
        // a gizmo drag into the undo stack via CommandStack::SnapshotComponent.
        // Mirrors EditorPanels.cpp's Inspector loop (InspectEntity + meta->typeName
        // match) -- namespace-qualified, matching how ASTRA_REFLECT_TYPE registers it.
        const Astra::ComponentDescriptor* FindTransformDescriptor(Astra::Registry& reg, Astra::Entity e)
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            {
                if (ci.meta && ci.meta->typeName == "Arcane::Transform")
                {
                    return ci.descriptor;
                }
            }
            return nullptr;
        }

        // Assets -> Show in Explorer / Copy Path / Open as text, on a resolved asset Guid.
        // Extracted so the menu-bar route (the panel's tracked row) and the
        // Asset Browser's own row context menu (AssetBrowserPanel.cpp) resolve the
        // SAME way -- one implementation, two entry points.
        void AssetPathAction(const Arcane::Project* proj, const Arcane::Guid& guid,
                             bool showInExplorer, bool copyPath, bool openAsText = false)
        {
            const auto assetPath = proj
                ? proj->ResolveAsset(Arcane::AssetId::FromGuid(guid))
                : std::nullopt;
            if (!assetPath)
            {
                ARC_WARN("Assets: the selected asset no longer resolves to a file");
                return;
            }
            if (showInExplorer)
            {
                const OsShell::ShellResult r = OsShell::ShowInExplorer(*assetPath);
                if (r != OsShell::ShellResult::Ok)
                    ARC_WARN("Assets: Show in Explorer failed for '{}' ({})",
                             assetPath->generic_string(), OsShell::Describe(r));
            }
            if (copyPath)
                ImGui::SetClipboardText(assetPath->string().c_str());
            if (openAsText)
            {
                // The OS default handler (a .arcinput is JSON: the user's text
                // editor); no association falls back to Open With inside
                // OsShell::OpenAsText. With the JSON tab retired this is the
                // repair banner's only route.
                const OsShell::ShellResult r = OsShell::OpenAsText(*assetPath);
                if (r != OsShell::ShellResult::Ok)
                    ARC_WARN("Assets: Open as text failed for '{}' ({})",
                             assetPath->generic_string(), OsShell::Describe(r));
            }
        }
    }

    // PathPickedThunk lives in DialogSlot.hpp -- the OS picker trampoline
    // ShowOpen*Dialog registers. Tests fire the same function.

    // The frame. Every line here is a phase call, and the ORDER IS THE
    // BEHAVIOUR -- see this file's header comment and each phase's own note.
    void EditorApp::MainLoop()
    {
        LoopState ls;
        ls.simPrev       = std::chrono::steady_clock::now();
        ls.lastFrameTime = ls.simPrev;

        // Boot is over; anything the watchdog reports from here on belongs to
        // the frame loop, not to a stale boot stage.
        Arcane::Diagnostics::SetPhase("editor frame loop");

        // ===== F2b Task 12: the cook gate =====================================
        // verify/capture/bless REFUSES-OR-WAITS while CookSession-reported
        // cooks are pending -- gated BEFORE m_compareRequested is set below
        // (Task 9's own reference-resolution block), and deliberately BEFORE
        // that block too: a --bless run with no reference yet must not bless
        // a checkerboard/refused placeholder as the new "golden" capture any
        // more than an ordinary --compare run may compare against one.
        //
        // WAITS: this is a one-shot batch verify run, not the live editing
        // session's fire-and-forget JobSystem::Submit queue (CookQueue) --
        // blocking here, synchronously, on a plain CookSession::CookProject
        // pass IS the wait. REFUSES: if that pass reports any source that
        // FAILED to cook (corrupt source / disk write error -- CookSession's
        // own vocabulary), this exits fast, at zero frames rendered, exactly
        // like the missing-reference fail-fast path below -- neither a
        // placeholder nor a refusal is something to compare against or
        // bless. A run with no open project (projectRoot empty) has no
        // Content/ tree to cook and is left to the reference-resolution
        // block's own handling.
        if (!m_config.compareReference.empty())
        {
            const std::filesystem::path cookProjectRoot =
                (m_runtime && m_runtime->CurrentProject()) ? m_runtime->CurrentProject()->Root()
                                                            : std::filesystem::path{};
            if (!cookProjectRoot.empty())
            {
                Arcane::AssetPipeline::CookSession cookGate;
                cookGate.SetTextureDefaults(Arcane::Settings<Arcane::AssetPipeline::TextureMetaSettings>());   // S6-6
                const Arcane::AssetPipeline::CookResult cookResult =
                    cookGate.CookProject(cookProjectRoot);
                if (cookResult.failed > 0)
                {
                    ARC_ERROR("--compare '{}': {} content source(s) failed to cook -- refusing "
                              "rather than comparing/blessing against a placeholder or refused "
                              "render",
                              m_config.compareReference, cookResult.failed);
                    m_cookRefusedFatal = true;
                    m_graphExit = 5;
                    ShutdownGraphPath();
                    return;
                }
                if (cookResult.cooked > 0)
                    ARC_INFO("--compare '{}': cooked {} content source(s) before comparing",
                             m_config.compareReference, cookResult.cooked);
            }
        }

        // ===== --compare / --bless (Task 9) =================================
        // Resolve the reference BEFORE the settle loop starts, mirroring
        // RuntimeApp::MainLoop's own pre-loop block (Finding 3 of the Task 8
        // dispatch audit) EXACTLY: a reference that does not exist will not
        // start existing after N wasted settle attempts, so a missing/
        // undecodable reference fails FAST here, at ZERO frames rendered,
        // rather than entering the loop with an invalid m_referencePixels
        // and spending the whole --settle budget reporting "could not
        // compare" on every attempt. CreateGraphVehicles has already
        // succeeded by the time Main() calls this function, so
        // ShutdownGraphPath's own `if (!ChromeGraph() && ...) return;` guard
        // does not swallow the report this fail-fast path still owes -- see
        // that function for the bless/settle/report folding this run's exit
        // (4, "compare-missing-reference") funnels through.
        if (!m_config.compareReference.empty())
        {
            const std::filesystem::path projectRoot =
                (m_runtime && m_runtime->CurrentProject()) ? m_runtime->CurrentProject()->Root()
                                                            : std::filesystem::path{};
            const char* const backendName = CompareBackendName(m_config.backend);
            if (Arcane::NriGraphContext* chrome = ChromeGraph())
            {
                const Arcane::NriDeviceCaps& caps = chrome->Device().Caps();
                m_compareAdapterSet = Arcane::ReferenceAdapterSet(backendName, caps.adapterName,
                                                                  caps.softwareAdapter);
                if (!m_compareAdapterSet.empty())
                    ARC_INFO("--compare: software adapter '{}' -> reference set '{}'",
                             caps.adapterName, m_compareAdapterSet);
            }
            m_compareResolution = Arcane::ResolveReference(projectRoot, m_config.compareReference,
                                                            backendName, m_compareAdapterSet);

            if (m_compareResolution.level == Arcane::ReferenceLevel::None)
            {
                if (!m_config.bless)
                {
                    ARC_ERROR("--compare '{}': no reference image on disk for backend '{}' -- "
                              "re-run with --bless to create one",
                              m_config.compareReference, backendName);
                    m_compareMissingFatal = true;
                    m_graphExit = 4;
                    ShutdownGraphPath();
                    return;
                }
                // else: --bless is present -- the normal FIRST bless. Fall
                // through with m_compareRequested false below (Finding 4 of
                // the Task 8 audit): there is nothing yet to compare
                // against, and the conjunct would be disabled for the whole
                // run regardless.
            }
            else if (!m_config.bless)
            {
                // A real reference exists and this run is not overwriting
                // it -- load it for the settle loop's compare conjunct.
                if (!Arcane::LoadPngRgba(m_compareResolution.path, m_referencePixels.width,
                                          m_referencePixels.height, m_referencePixels.rgba) ||
                    !m_referencePixels.Valid())
                {
                    ARC_ERROR("--compare '{}': reference '{}' exists but could not be decoded -- "
                              "treating it as missing (re-run with --bless to replace it)",
                              m_config.compareReference, m_compareResolution.path.string());
                    m_compareMissingFatal = true;
                    m_graphExit = 4;
                    ShutdownGraphPath();
                    return;
                }
            }
            // else: a reference exists but --bless will overwrite it -- also
            // nothing to load; m_compareRequested stays false either way.
        }

        // Finding 4 (Task 8 audit): disabled outright whenever --bless is
        // set, regardless of whether m_compareResolution actually found
        // anything -- see the member's own comment (EditorApp.hpp) for why
        // blessing must not also gate convergence.
        m_compareRequested = !m_config.compareReference.empty() && !m_config.bless;

        while (ls.running)
        {
            // FIRST statement in the frame, before any phase can block. The
            // watchdog's whole definition of "alive" is this call, so it has to
            // sit on the loop itself rather than inside any one phase.
            Arcane::Diagnostics::Heartbeat();

            // Device-lost exit: the latch is set only AFTER the gpu-crash
            // report was written, so breaking here is "report captured, stop
            // cleanly". Checked at the frame top so no phase downstream ever
            // records against the dead device -- the next resource-creating
            // path faults inside the driver rather than returning an error
            // code. Mirrors RuntimeApp::MainLoop.
            if (Arcane::GpuDeviceLostObserved())
            {
                ARC_ERROR("GPU device lost -- crash report written; shutting down");
                break;
            }

            const FramePump pump = PumpFrameEvents();
            if (pump == FramePump::Exit)
            {
                break;
            }
            if (pump == FramePump::SkipFrame)
            {
                continue;
            }

            ConsumeDeferredSceneAction(ls);
            ConsumeSceneDialogResults(ls);
            ConsumeProjectDialogResult();
            ConsumeMaterialDialogResults();
            ConsumeSettingsDialogResults();
            // Worker -> main-thread drain of the module rebuild, at the same
            // safe point the dialog results land at: its finish path can run
            // effects (plugin re-engage, scene reload) that must never land
            // mid-render. See EditorApp.hpp's Build section.
            PollModuleBuild();
            // Watchdog/faulting-thread -> main-thread drain of the report-
            // written queue, same safe point: registers each new .arcdiag
            // as an asset and publishes one Problems row per report. See
            // EditorApp.hpp's Report-written notify section.
            PollDiagnosticReports();

#if !defined(ARC_BUILD_DIST)
            // --crash-gpu N: the same deliberate fault Build -> Diagnostics ->
            // Crash GPU fires, on a schedule, so the desk battery's editor items
            // can be SCRIPTED rather than clicked -- and so a flag both hosts
            // parse is not silently inert in one of them. Here rather than at
            // the menu-request site because this is the frame's provably safe
            // point: FireDeliberateGpuFault dispatches through
            // NriDiagnostics::FireFault on its own command buffer, and this
            // top-of-frame point is safe because no graph frame has been
            // declared yet. Fires exactly once.
            if (m_config.crashGpuFrame != 0 && !m_gpuFaultFired &&
                m_frameCount >= m_config.crashGpuFrame)
            {
                m_gpuFaultFired = true;   // set FIRST: a failed build must not retry every frame
                FireDeliberateGpuFault();
            }

            // --hang-main N (crash window plan 2, D10): stop beating on
            // purpose. The sleep sits here, after the beat published just
            // above, so the watchdog sees a beat that then goes stale for
            // kHangMainSeconds.
            if (m_config.hangMainFrame != 0 && !m_hangMainFired &&
                m_frameCount >= m_config.hangMainFrame)
            {
                m_hangMainFired = true;   // set FIRST: exactly once
                ARC_WARN("--hang-main: blocking the main thread for {} s without a heartbeat", Arcane::kHangMainSeconds);
                std::this_thread::sleep_for(std::chrono::seconds(Arcane::kHangMainSeconds));
            }
#endif

            FrameState fs;
            // An old [EditorViewport][Camera] block's preference lines, read by
            // the ini handler since the last frame (boot, RetargetLayoutIni, a
            // windowed project switch's reload): imported ONCE into the User
            // rung where the user has not chosen (settings S6-29), BEFORE the
            // barrier below so this frame already reads them, and queued for
            // the debounced archive write.
            if (m_legacyViewport.Any())
            {
                Arcane::Editor::ImportLegacyViewportPrefs(Arcane::CVarRegistry::Get(), m_legacyViewport);
                Arcane::Editor::ForEachLegacyViewportPref(m_legacyViewport, [](std::string_view name)
                    { Arcane::Editor::NoteSettingEdited(Arcane::SetBy::User, std::string(name)); });
                m_legacyViewport = {};
            }
            // Likewise an old [EditorPlayMode][State] section's mode (settings
            // S6-32): imported once, never written back to the ini.
            if (m_legacyPlayMode)
            {
                if (Arcane::Editor::ImportLegacyPlayMode(Arcane::CVarRegistry::Get(), *m_legacyPlayMode))
                    Arcane::Editor::NoteSettingEdited(Arcane::SetBy::User, "editor.play.launchMode");
                m_legacyPlayMode.reset();
            }
            // THE CVAR PUBLISH BARRIER, once per frame: every Set a console
            // line, a callback or a plugin made since the last one becomes
            // visible here, to all of this frame's readers at once. The
            // runtime does the same at the top of AdvanceSim.
            Arcane::CVarRegistry::Get().Publish();
            // editor.camera.* -> the camera's runtime copies, before any input
            // reads them (the wheel and the view-settings popup write the cvar;
            // the camera picks it up here, next frame).
            {
                const auto& camPrefs = Arcane::Settings<Arcane::Editor::EditorCameraSettings>();
                m_camera.orbit.fovYDeg = camPrefs.fovYDeg;
                m_camera.speedScalar   = camPrefs.speedScalar;
            }
            // editor.viewport.physicsOverlay (settings S6-32): the View-menu
            // toggle stays session state (spec s6.3); the cvar is where a
            // session starts, and a change to it (the settings window, a
            // project switch's Pref-P value) moves the toggle to it.
            if (const bool overlayPref = Arcane::Settings<Arcane::Editor::EditorViewportSettings>().physicsOverlay;
                overlayPref != m_physicsOverlayPref)
            {
                m_physicsOverlay     = overlayPref;
                m_physicsOverlayPref = overlayPref;
            }
            Arcane::Editor::TickSettingsHost();   // the settings windows' debounced writes (editor.settings.saveDebounceMs)
            // editor.undo.* -> the stack (s2.4): pushed on change, never read by the stack.
            if (m_undo)
                if (const Arcane::UndoLimits limits = Arcane::Editor::ReadUndoLimits();
                    limits != m_undoLimitsApplied)
                {
                    m_undo->SetLimits(limits);
                    m_undoLimitsApplied = limits;
                }
            // editor.theme.* / editor.ui.* -> the editor style (settings S4).
            // After the barrier, before the ImGui frame opens.
            ApplyAppearanceSettings();
            ApplyPendingLayoutRequest();
            FrameInput(ls, fs);
            AdvanceSim(ls);
            ApplyPendingViewportResize();
            RefreshSceneResolution();
            RenderSceneToViewport();
            // PHASES 11, 12 AND THE CAPTURE ARE DISCHARGED BY PHASE 10.
            // RenderSceneToViewport declares ONE graph frame whose nodes cover
            // all three: ArmGraphViewportFrame fills FrameDesc::pickOutline/
            // pickables/selectedIds for 12 and BeginGameUiFrame's draw data
            // for 11, FrameDesc::capture carries the screenshot readback, and
            // phase 10 compiles and executes the lot.
            //
            // THE ORDERING CONTRACT is what the engine-side node comments
            // cite: the HUD composites after the scene render and before the
            // outline. Node order inside ONE frame enforces it, which is
            // strictly stronger than three separate statements here would be
            // -- see ArmGraphViewportFrame.
            //
            // The numbering keeps its gaps deliberately: phases 11 and 12 are
            // real slots in this frame's contract, they are simply satisfied
            // upstream, and renumbering would invalidate every engine-side
            // comment that reasons about "phase 11 then phase 12" order.
            PumpEditorDocuments();
            DrawEditorUi(ls, fs);
            DrawModals(ls);
            DrawViewportPanelPhase(fs);
            HandleViewportPick(fs);
            DrawSelectionPanels();
            if (!PresentFrame())
            {
                continue;
            }
            EndFrame(ls);
        }

    }

    // Phase 1: pump the window/OS event queue. Must stay first -- everything
    // below reads the window size and the quit/minimize state it settles.
    EditorApp::FramePump EditorApp::PumpFrameEvents()
    {
        // A project-switch overlay presenter already saw and consumed the OS
        // quit event on its own pump (SwitchProject, EditorAppProject.cpp) --
        // it will never reach the SDL_EVENT_QUIT check below. m_requestExit is
        // how that failure branch hands the exit back to the normal frame loop
        // instead of reporting it as a switch failure (Important 1, 2026-07-31
        // review). The scene is already clean at that point (switch_teardown
        // reset it), so an immediate exit is correct -- no confirm modal.
        if (m_requestExit)
            return FramePump::Exit;

        // The OS asked for this session to end -- Ctrl-C, the console close
        // box, logoff or shutdown, all routed through
        // Diagnostics::RequestCleanExit into the hook main() installed (crash
        // window plan 1, task 9; spec S5.7). Deliberately the same immediate
        // exit m_requestExit takes and NOT the confirm modal below: the OS
        // gives a session-end handler about five seconds, and nobody can
        // answer a dialog inside it. (Autosave-before-exit is plan 3's job;
        // this only starts the ordinary exit as early as possible.)
        if (CleanExitRequested())
        {
            ARC_INFO("Editor: exiting on a clean-exit request (Ctrl-C, console close, or session end)");
            return FramePump::Exit;
        }

        auto events = m_gpu->Win().PumpEvents();
        if (events.quitRequested)
        {
            // Unsaved scene: park the quit behind the confirm modal rather than
            // dropping the user's work on a window close. Request returns false
            // when it parks, and this frame then runs normally so the modal draws.
            if (m_scene.Request(Arcane::Editor::SceneIntent::Exit, {}, *m_undo))
            {
                // Exiting here returns 0 from Run(), which is indistinguishable
                // from a normal shutdown in the process exit code -- so an
                // unwanted quit looks like the editor simply vanished. Say it.
                ARC_INFO("Editor: exiting on a quit request (see the Window line above for its source)");
                return FramePump::Exit;
            }
        }
        // ONE resize, to this run's one presentation surface, and strictly
        // BETWEEN frames -- which the top of the loop structurally is
        // (RuntimeFrame::PumpAndResize carries the same pair for the same
        // reason). The CHROME context's swapchain is the surface bound to
        // this window. The VIEWPORT's offscreen output is NOT resized here at
        // all -- it tracks the panel, not the window, and phase 8 owns it.
        //
        // NOT UNDER --headless: there is no swapchain to resize, the window is
        // never mapped so a real resize event cannot arrive, and
        // NriGraphContext::Resize is HOST-WINDOW MODE ONLY (ResizeOffscreen is
        // the offscreen twin, and it is NOT wanted here either -- the offscreen
        // chrome target is deliberately fixed at the boot extent for the whole
        // run, which is what makes two runs' captures the same size).
        if (events.resized)
        {
            if (ChromeGraph() && !ChromeGraph()->IsOffscreen())
                ChromeGraph()->Resize(events.width, events.height);
        }
        if (m_gpu->Win().IsMinimized())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(Arcane::Settings<Arcane::AppWindowSettings>().minimizedSleepMs));
            return FramePump::SkipFrame;
        }
        // editor.perf.backgroundFps (0 = off, the default): an unfocused
        // editor sleeps out the rest of its 1000 / fps ms frame. Never under
        // --headless, whose never-shown window has no input focus.
        const std::uint32_t backgroundFps = Arcane::Settings<Arcane::Editor::EditorPerfSettings>().backgroundFps;
        if (backgroundFps != 0 && !m_config.headless && !m_gpu->Win().IsFocused())
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_lastFramePump);
            std::this_thread::sleep_for(Arcane::Editor::BackgroundFrameWait(backgroundFps, elapsed));
        }
        m_lastFramePump = std::chrono::steady_clock::now();
        return FramePump::Continue;
    }

    // Perform a scene intent SceneSession parked (or one that needed no
    // confirmation). Only ever called from the top-of-frame phases below -- see
    // sceneAction.
    void EditorApp::RunSceneAction(const Arcane::Editor::SceneSession::PendingRequest& req, LoopState& ls)
    {
        switch (req.intent)
        {
            case Arcane::Editor::SceneIntent::NewScene:
                DoNewScene();
                break;
            case Arcane::Editor::SceneIntent::OpenScene:
                DoOpenScene(req.path);
                break;
            case Arcane::Editor::SceneIntent::OpenProject:
                SwitchProject(req.path);
                break;
            case Arcane::Editor::SceneIntent::Exit:
                ls.running = false;
                break;
            case Arcane::Editor::SceneIntent::LaunchStandalone:
                DoLaunchStandalone();
                break;
            case Arcane::Editor::SceneIntent::None:
                break;
        }
    }

    // Phase 2: last frame's confirm-modal answer. MUST run before any phase that
    // can issue a new SceneSession::Request -- the session parks only one intent
    // at a time.
    void EditorApp::ConsumeDeferredSceneAction(LoopState& ls)
    {
        // The confirm modal's answer from LAST frame, run here at the same safe
        // point the project switch uses. Taken before any new Request below: the
        // session allows only one parked intent at a time.
        if (ls.sceneAction.intent != Arcane::Editor::SceneIntent::None)
        {
            const Arcane::Editor::SceneSession::PendingRequest req = ls.sceneAction;
            ls.sceneAction = {};
            RunSceneAction(req, ls);
        }
    }

    // Phase 3: scene file-dialog results. The thunks Stash() into the slots on
    // an SDL BACKGROUND thread; this is the main-thread Take() half and must
    // stay at the top of the frame, never mid-render.
    void EditorApp::ConsumeSceneDialogResults(LoopState& ls)
    {
        if (const auto sceneOpen = m_dialogs.sceneOpen.Take())
        {
            // Guarded: opening over unsaved work parks the intent for the confirm
            // modal (drawn later this frame) instead of discarding it.
            if (m_scene.Request(Arcane::Editor::SceneIntent::OpenScene, *sceneOpen, *m_undo))
            {
                DoOpenScene(*sceneOpen);
            }
        }
        if (const auto sceneSave = m_dialogs.sceneSave.Take())
        {
            std::filesystem::path p = *sceneSave;
            // The save dialog does not force the extension (Window.hpp), and a
            // scene without it does not scan as an asset. Case-insensitive:
            // a hand-typed "MyScene.ARCSCENE" must not become
            // "MyScene.ARCSCENE.arcscene".
            if (LowerExtension(p) != Arcane::Scene::kSceneExt)
            {
                p += Arcane::Scene::kSceneExt;
            }
            const bool saved = DoSaveScene(p);

            // This save may be the confirm modal's "Save" answer on a never-saved
            // scene: that branch launches this dialog and leaves the intent parked
            // until the path lands, which is now. Proceed only if the bytes
            // actually went to disk -- a failed save must not go on to discard the
            // work it was meant to preserve.
            if (m_scene.Pending() != Arcane::Editor::SceneIntent::None)
            {
                if (saved)
                {
                    RunSceneAction(m_scene.TakePending(), ls);
                }
                else
                {
                    m_scene.ClearPending();
                }
            }
        }
    }

    // Phase 4: File->Open Project result. Same epoch-guarded slot pattern;
    // the soft restart must land at the top of the frame, never mid-render.
    void EditorApp::ConsumeProjectDialogResult()
    {
        // File->Open Project: run a pending soft-restart at a safe point (top of
        // frame, never mid-render). Stashed by PathPickedThunk, which runs on an
        // SDL-owned background thread -- Take() the path out of the slot, then
        // switch on the local copy (SwitchProject itself never touches m_dialogs).
        if (const auto pending = m_dialogs.projectOpen.Take())
        {
            // Same guard as Open Scene: the outgoing project's scene may have
            // unsaved changes, and switching would drop them.
            if (m_scene.Request(Arcane::Editor::SceneIntent::OpenProject, *pending, *m_undo))
            {
                SwitchProject(*pending);
            }
        }
    }

    // Phase 5: material file-dialog results. Same epoch-guarded slot pattern;
    // consumed here so document creation never lands mid-render.
    //
    // TASK 12: the three CREATION slots this used to drain (materialNew,
    // meshMaterialNew, instanceNew) are gone -- every material/instance
    // creation now routes through BeginCreateAsset -> DrawCreateAssetDialog ->
    // ConsumeCreateResult, which is synchronous ImGui and needs no async slot.
    // What is left is the material OPEN path, which is not a creation path and
    // is still a real OS file picker.
    void EditorApp::ConsumeMaterialDialogResults()
    {
        if (const auto materialOpen = m_dialogs.materialOpen.Take())
        {
            m_documents.OpenPath(*materialOpen);
        }
    }

    // Settings arc S4: the OS pickers the settings pages opened.
    void EditorApp::ConsumeSettingsDialogResults()
    {
        if (const auto path = m_dialogs.themeImport.Take()) (void)Arcane::Editor::ImportThemeFrom(m_themePage, *path);
        if (const auto path = m_dialogs.themeExport.Take()) (void)Arcane::Editor::ExportThemeTo(m_themePage, *path);
    }

    // Load a named layout at the frame boundary, after the settings publish.
    void EditorApp::ApplyPendingLayoutRequest()
    {
        if (!m_layoutPage.loadRequest) return;
        const std::string name = *std::exchange(m_layoutPage.loadRequest, std::nullopt);
        if (m_config.headless || !m_editorImguiContext)
        {
            ARC_WARN("layout: Load '{}' refused -- the layout is pinned under --headless", name);
            return;
        }
        const Arcane::Editor::LayoutLibrary lib(m_layoutPage.dir.empty() ? Arcane::Editor::NamedLayoutDir() : m_layoutPage.dir);
        const std::optional<std::string> text = lib.Load(name);
        if (!text)
        {
            m_layoutPage.status = "Could not read layout '" + name + "'";
            return;
        }
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_editorImguiContext);
        ImGui::ClearIniSettings();
        ImGui::LoadIniSettingsFromMemory(text->data(), text->size());
        ImGui::MarkIniSettingsDirty();
        ImGui::SetCurrentContext(prev);
        m_layoutPage.status = "Loaded '" + name + "'";
        ARC_INFO("layout: loaded named layout '{}'", name);
    }

    void EditorApp::ApplyAppearanceSettings()
    {
        if (!m_editorImguiContext)
            return;
        // The game context may be current here (the offscreen layer); the
        // editor's style and font atlas are the ones this writes, and the
        // atlas calls below act on the CURRENT context.
        ImGuiContext* const prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_editorImguiContext);
        ImGuiStyle& style = ImGui::GetStyle();
        // A font change queued LAST frame lands now, between frames (spec s7.3: deferred one frame).
        if (const std::optional<Arcane::Editor::EditorUiSettings> fonts = m_appearance.TakeFontRebuild())
        {
            const std::filesystem::path exe = Arcane::Editor::ModuleBuild::ExeDir();
            const auto families = Arcane::Editor::ListEditorFontFamilies(
                exe, Arcane::Paths::Get(Arcane::Paths::Location::EditorUserDir) / "Fonts");
            Arcane::Editor::EditorFontRequest req;
            req.uiFace   = Arcane::Editor::ResolveEditorFontFamily(families, fonts->fontFamily, "Inter");
            req.monoFace = Arcane::Editor::ResolveEditorFontFamily(families, fonts->monoFontFamily, "JetBrains Mono");
            req.altFace  = Arcane::Editor::ResolveEditorFontFamily(families, fonts->altFontFamily, "Roboto");
            req.sizePx   = fonts->fontSize;
            Arcane::Editor::ReinstallEditorFonts(req);
            style.FontSizeBase = fonts->fontSize;
        }
        m_appearance.UpdateTheme(Arcane::Settings<Arcane::Editor::EditorThemeSettings>(), style);
        m_appearance.UpdateStyle(Arcane::Settings<Arcane::Editor::EditorUiStyleSettings>(), style);   // before UpdateUi: recomposes at the applied scale
        m_appearance.UpdateUi(Arcane::Settings<Arcane::Editor::EditorUiSettings>(),
                              m_gpu ? m_gpu->Win().DisplayScale() : 1.0f, style);
        ImGui::SetCurrentContext(prev);
    }

    // Phase 6: input sample + the editor's own keybinds + gizmo interaction.
    // Input sample (before ImGui BeginFrame so capture flags are set).
    // Runs before the sim advance, and the camera push at its tail is the FIRST
    // of this frame's two -- the gizmo hit-test below it reads
    // ClientRuntime::View().
    void EditorApp::FrameInput(LoopState& ls, FrameState& fs)
    {
        // Cleared here, set below if a gizmo drag starts or ends THIS frame --
        // the click-pick block (later, after ImGui builds the Viewport panel)
        // checks it to avoid treating a gizmo-handle click as a selection change.
        m_gizmoCapturedClick = false;

        // Play/edit authority (architecture pass sec 1): re-derived HERE -- before
        // any phase that reads it -- and again immediately after the toolbar draw
        // in DrawEditorUi, the one site that can START Play mid-frame. RULE: every
        // site that can call PlayMode Start/Stop within the frame must be followed
        // by this same re-derivation. (A mid-frame STOP elsewhere leaves edit
        // affordances off for the rest of that frame -- the safe direction; a
        // mid-frame START without re-derivation is the A1 memento-vs-play-registry
        // bug this write exists to kill.)
        m_editBinding.editMode = !InPlayMode();

        // THE FRAME CLOCK (F4 plan 1 T12, Ruling N) -- RuntimeFrame.cpp's
        // AdvanceSim input block, ported. FIXED under --headless, FROZEN during
        // the settle hold, wall-clock otherwise -- and steady_clock::now() is
        // not even CALLED in the fixed branches, deliberately: this frameDt
        // does not stay local to input. It becomes m_gameUi.frameDt, which
        // advances m_editorClock, which PrepareFrame hands the scene resolver as
        // FrameInfo::dt/now -- the Time/DeltaTime the material globals report
        // to every bound shader (SceneRenderResolver.cpp, Batcher2D::
        // SetGlobals). A wall-clock frameDt here leaks straight into anything
        // an .arcmat animates by time, so two --headless runs of the same
        // `--frames N` render different pixels, and no two settle attempts can
        // ever compare byte-equal.
        //
        // THE EDITOR NEVER HAD THIS, and the golden lane never noticed: the
        // pre-F4 two-camera editor framed ReferenceProject's PulseBox (its
        // `sin(Time * PulseSpeed)` material, Content/materials/pulse_sprite.
        // arcmat) OFF-SCREEN, so no time-animated pixel was ever in the
        // editor-ui capture and the wall clock was harmless. F4's SceneOpen
        // framing (T6/T7) frames the whole scene, PulseBox included, and the
        // very next gate run sat at settle-not-converged for its whole budget
        // (T12's Finding A). The runtime host has carried this policy since
        // its own --settle task for exactly this material; the comment there
        // calls it load-bearing, not an optimisation.
        //
        // GATED to the capture mode only: `m_config.headless` is what selects
        // the fixed branch, so a windowed editing session keeps wall-clock dt
        // and nothing about normal editing changes. SettleHoldActive() is the
        // hold (EditorApp.hpp), read here AND by AdvanceSim's simDt so the two
        // clocks agree.
        //
        // ACCEPTED RISK (Ruling N): a frozen clock also freezes the compile
        // service's clock -- m_editorClock is what ShaderCompiler::Poll's
        // readyAt check and MaterialPreviewHarvester::Pump read -- so a compile
        // SUBMITTED during the hold (readyAt = now + debounce) would never
        // dispatch. Every scene material is requested at scene open, dozens of
        // frames before the hold begins, so in practice nothing is pending by
        // then; and if it ever is, the settle loop's idle conjunct keeps
        // failing and the run exits settle-not-converged, LOUDLY -- never a
        // wrong bless. That is the same contract RuntimeFrame.cpp accepts.
        double frameDt;
        if (SettleHoldActive())
        {
            frameDt = 0.0;   // see SettleHoldActive's own comment (EditorApp.hpp)
        }
        else if (m_config.headless)
        {
            frameDt = m_config.fixedDtSeconds;
        }
        else
        {
            const auto now = std::chrono::steady_clock::now();
            frameDt = std::chrono::duration<double>(now - ls.lastFrameTime).count();
            ls.lastFrameTime = now;
        }
        const Arcane::InputSnapshot snap = m_gpu->InDevices().Sample(m_gpu->Imgui().WantCaptureKeyboard(), m_gpu->Imgui().WantCaptureMouse());
        m_rebindCaptureLive = false;
        m_documents.ForEach([&](EditorDocument& document)
        {
            if (auto* input = dynamic_cast<InputActionsDocument*>(&document))
            {
                input->SetPreviewSnapshot(snap);
                m_rebindCaptureLive = m_rebindCaptureLive || input->InputSwallowed();
            }
        });

        // Settings arc S4: the action map's frame. The Global/Viewport actions
        // below read this snapshot; the panels' actions read ImGui's events.
        {
            Arcane::Editor::ActionFrameInput keys;
            keys.snap = &snap;
            keys.playMode = InPlayMode();
            keys.wantCaptureKeyboard = snap.wantCaptureKeyboard;
            keys.wantTextInput = m_editorImguiContext && m_editorImguiContext->IO.WantTextInput;
            keys.captureLive = m_rebindCaptureLive;
            keys.viewportActive = m_viewportActive;
            keys.dt = frameDt;
            if (m_editorImguiContext)
            {
                keys.repeatDelay = m_editorImguiContext->IO.KeyRepeatDelay;
                keys.repeatRate = m_editorImguiContext->IO.KeyRepeatRate;
            }
            Arcane::Editor::EditorActions::Get().BeginFrame(keys);
        }

        // The plugin only sees scene-relevant input when the Viewport panel
        // is active (hovered/focused), with the cursor remapped into
        // viewport-local pixels (m_viewportRect/m_viewportActive are set at
        // the end of the PREVIOUS frame's ImGui pass -- see below). Otherwise
        // zero the mouse buttons/scroll so the plugin's camera does not pan
        // and spawn/drag does not fire while editing panels.
        Arcane::InputSnapshot pluginSnap = snap;
        float lx = 0, ly = 0;
        const bool inViewport = m_viewportActive && Arcane::Editor::ToViewportLocal(m_viewportRect, snap.mouseX, snap.mouseY, lx, ly);
        if (inViewport)
        {
            pluginSnap.mouseX = lx;      // plugin camera works in viewport-local px
            pluginSnap.mouseY = ly;      // (panel size == offscreen size => scale 1)
            // The Viewport panel IS the world surface: from the scene's point of
            // view the cursor is in the world, not over a HUD widget. The sampled
            // wantCaptureMouse is true (the cursor is over an ImGui window -- the
            // Viewport), which the plugin uses to SUPPRESS camera pan/zoom + world
            // interaction. Clear it so RMB-pan and wheel-zoom work in the viewport.
            // (When the plugin's own HUD floats over the viewport and is hovered,
            // ImGui's hover z-order makes the Viewport window not-hovered ->
            // inViewport is false -> the suppression below still applies.)
            pluginSnap.wantCaptureMouse = false;
        }
        else
        {
            pluginSnap.mouseButtons = 0;
            pluginSnap.wheelY       = 0.0f;
        }

        // The game's viewport debug UI (its own ImGui context, composited into
        // the viewport texture by the graph's GameUi ImGui node -- phase 11)
        // sits over the
        // scene, so a click on one of its widgets must claim the pointer ahead
        // of both the plugin's gameplay input and the editor gizmo/click-pick.
        // Uses the game context's LAST-frame WantCaptureMouse: this frame's
        // game ImGui pass runs later (after this input phase closes -- see
        // BeginGameUiFrame), so its capture state is not known yet this frame. That
        // 1-frame lag matches the one already inherent to inViewport/
        // m_viewportActive (both computed from the previous frame's panel
        // hover/focus).
        //
        // UNCONDITIONAL: the viewport builds a real OffscreenImGuiLayer and
        // composites its draw data through a graph node, so its
        // WantCaptureMouse is a real answer rather than a stand-in.
        fs.gameUiClaims = Arcane::Editor::GameUiClaimsPointer(
            InPlayMode(), inViewport, m_gameImgui->WantCaptureMouse());

        // Snapshot the viewport-local cursor + RAW buttons/wheel + dt for the
        // game ImGui pass, which composites into the viewport AFTER this input
        // phase returns (see BeginGameUiFrame). Only the few values the game
        // context needs are hoisted -- the input phase stays narrow. Off the
        // viewport, hasInput is false so the game context reads the cursor as
        // off-target (BeginFrame injects -FLT_MAX).
        m_gameUi.viewportMouse = glm::vec2(lx, ly);
        m_gameUi.inViewport    = inViewport;
        m_gameUi.mouseButtons  = snap.mouseButtons;
        m_gameUi.wheel         = snap.wheelY;
        m_gameUi.frameDt       = frameDt;

        // Edit mode: the editor owns the left mouse button in the viewport
        // (click-pick + gizmo), so the hosted plugin must not also see it --
        // otherwise its LMB interactions (e.g. Sandbox spawn/drag/throw) fire
        // while editing. RMB + wheel stay live so plugin camera pan/zoom still
        // navigates the scene. (LMB=bit0; InputSnapshot.hpp.) ALSO clear it
        // when the game's viewport debug UI claims the pointer this frame
        // (Play + cursor over a game HUD widget) -- otherwise a HUD click
        // would fall through and spawn/drag gameplay underneath it.
        if (!InPlayMode() || fs.gameUiClaims)
        {
            pluginSnap.mouseButtons &= ~static_cast<uint8_t>(0x1u);
        }
        // Edit mode: the EDITOR camera owns RMB-drag pan and wheel zoom
        // (see UpdateEditorCamera below), so the plugin must not see those
        // either. Both cameras write the SAME Runtime slot, and the
        // editor's push (before SubmitRender) wins in Edit mode -- so a
        // plugin still panning underneath would just be invisible work
        // whose result reappears the moment Play starts, from a viewpoint
        // the user never chose. RMB=bit1; InputSnapshot.hpp. Play is
        // untouched: the plugin keeps RMB + wheel and its camera wins.
        // MMB=bit2 joined the mask with the Perspective pan (F4 plan 1 T7);
        // Alt+LMB (orbit) needs no extra clause -- LMB is already masked in
        // Edit mode above, unconditionally.
        if (!InPlayMode())
        {
            pluginSnap.mouseButtons &= ~static_cast<uint8_t>(0x2u | 0x4u);
            pluginSnap.wheelY = 0.0f;
        }
        m_runtime->SetInputSnapshot(pluginSnap);
        Arcane::InputSnapshot gameplaySnap = pluginSnap;
        gameplaySnap.wantCaptureKeyboard = !InPlayMode() || !inViewport ||
            snap.wantCaptureKeyboard || m_gameImgui->WantCaptureKeyboard();
        gameplaySnap.wantCaptureMouse = !InPlayMode() || !inViewport ||
            pluginSnap.wantCaptureMouse || fs.gameUiClaims;
        if (!InPlayMode() || !inViewport)
            gameplaySnap.gamepadConnected = false;
        m_runtime->UpdateGameInput(frameDt, gameplaySnap);
        m_gpu->Input().Update(frameDt, snap);

        HandleUndoRedoAndSceneShortcuts(fs);
        HandleGizmoModeKeys(snap);
        UpdateEditorCamera(snap, inViewport, lx, ly, static_cast<float>(frameDt));
        UpdateGizmoInteraction(snap, inViewport, lx, ly, fs.gameUiClaims);
    }

    // Phase 6a: undo/redo + the Ctrl+N/O/S scene shortcuts. Runs EARLIER in the
    // frame than the gizmo phase below, which is why the open-transaction guard
    // is here rather than there.
    void EditorApp::HandleUndoRedoAndSceneShortcuts(FrameState& fs)
    {
        const Arcane::Editor::GlobalShortcutPresses keys =
            Arcane::Editor::ReadGlobalShortcuts(Arcane::Editor::EditorActions::Get());
        // Also refuse while a transaction is open (e.g. a live gizmo drag):
        // CommandStack::Undo()/Redo() have no open-transaction guard, and this
        // keybind block runs earlier in the frame than the gizmo block below,
        // so an Undo here would apply the previous entry while the drag's own
        // "Gizmo" transaction is still holding pre-undo `before` bytes -- its
        // later Commit then pushes a transaction whose `before` predates the
        // undo and clobbers the redo entry. Ctrl is also the gizmo SNAP
        // modifier, so Ctrl-held drags are the normal case, not an edge case.
        // Settings arc S3-13: a focused settings window owns Ctrl+Z / Ctrl+Y for
        // its window-local undo (spec s6.3); the scene stack stands down.
        // SceneConsumesUndoKeys folds Play (UndoBarred), the open-txn guard,
        // and the settings-window yield into one predicate. keys.undo/redo
        // already apply EditorShortcutsLive (Play, capture, rebind).
        const bool noOpenTxn = !m_undo->InTransaction();
        const bool barred    = Arcane::Editor::UndoBarred(InPlayMode());   // the ONE Play barrier (s3.3b)
        Arcane::Editor::DispatchSceneUndoKeys(*m_undo, !barred && noOpenTxn, InPlayMode(),
            m_undo->InTransaction(), Arcane::Editor::SettingsWindowFocused(), keys.undo, keys.redo);
        fs.scNewScene      = keys.newScene;
        fs.scOpenScene     = keys.openScene;
        fs.scSaveScene     = keys.saveScene;
        fs.scCut           = keys.cut;
        fs.scCopy          = keys.copy;
        fs.scPaste         = keys.paste;
        fs.scDuplicate     = keys.duplicate;
        fs.scCloseDocument = keys.closeDocument;
    }

    // Phase 6b: gizmo mode keys. Pure state, but it runs before the gizmo
    // interaction phase so a mode change takes effect on the same frame.
    void EditorApp::HandleGizmoModeKeys(const Arcane::InputSnapshot& snap)
    {
        // RMB held: W/E/Q are the Look gesture's fly keys (UE's WASD_RMBOnly);
        // the raw button, because the camera phase starts the gesture later.
        const bool rmbHeld = (snap.mouseButtons & 0x2u) != 0;
        switch (Arcane::Editor::ReadViewportTool(Arcane::Editor::EditorActions::Get(), rmbHeld))
        {
        case Arcane::Editor::ViewportTool::Select:    m_gizmoEnabled = false; break;
        case Arcane::Editor::ViewportTool::Translate: m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Translate; break;
        case Arcane::Editor::ViewportTool::Rotate:    m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Rotate;    break;
        case Arcane::Editor::ViewportTool::Scale:     m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Scale;     break;
        case Arcane::Editor::ViewportTool::None:      break;
        }
    }

    // Phase 6c: editor viewport camera. Its tail holds the FIRST of the frame's
    // two SetView pushes -- see the comment there; the gizmo phase below reads
    // what it writes.
    void EditorApp::UpdateEditorCamera(const Arcane::InputSnapshot& snap, bool inViewport,
                                       float lx, float ly, float dt)
    {
        // Editor viewport camera (Edit mode), Unreal's input model by view
        // mode (EditorCamera.hpp, F4 spec s4):
        //   2D          -- RMB-drag pans, wheel zooms at the cursor.
        //   Perspective -- RMB-drag looks about the eye (WASD/QE fly while it
        //                  is held; Shift boosts; the wheel then steps the
        //                  speed), Alt+LMB-drag orbits the pivot, MMB-drag
        //                  pans in the view plane, the wheel dollies.
        //   both        -- F frames the selection (everything when nothing is
        //                  selected), Home frames everything, Alt+G / Alt+J
        //                  switch to Perspective / 2D.
        // The plugin no longer sees RMB/MMB/wheel (nor LMB) in Edit mode
        // (see the pluginSnap mask above), so the two cameras cannot fight
        // over the same gesture.
        //
        // All three button edges are tracked HERE, unconditionally, every
        // frame (this phase runs before the gizmo phase, which reads
        // m_edges.lmb rather than updating it again -- a second Update with
        // the same value would erase the press edge). A button already held
        // before the cursor enters the viewport is therefore never misread as
        // a fresh press.
        const bool lmb = (snap.mouseButtons & 0x1u) != 0;
        const bool rmb = (snap.mouseButtons & 0x2u) != 0;
        const bool mmb = (snap.mouseButtons & 0x4u) != 0;
        m_edges.lmb.Update(lmb);
        m_edges.rmb.Update(rmb);
        m_edges.mmb.Update(mmb);
        const bool alt   = snap.ScancodeDown(kScLAlt)   || snap.ScancodeDown(kScRAlt);
        const bool shift = snap.ScancodeDown(kScLShift) || snap.ScancodeDown(kScRShift);
        const glm::vec2 mouse(snap.mouseX, snap.mouseY);
        const glm::vec2 delta = mouse - m_camGesture.lastMouse;

        using Gesture = CameraGesture;
        using Arcane::Editor::ViewMode;
        if (!InPlayMode())
        {
            // A gesture may only START over the viewport, but once started it
            // keeps tracking anywhere -- same rule as the gizmo drag, so
            // crossing the panel edge mid-drag does not strand the view. In
            // Perspective the Alt+LMB test comes FIRST so an orbit press is
            // never read as a look (RMB) or pan (MMB) start on the same frame.
            const bool wasNone = m_camGesture.kind == Gesture::None;
            if (wasNone && inViewport)
            {
                if (m_camera.mode == ViewMode::TwoD)
                {
                    if (m_edges.rmb.pressed)             m_camGesture.kind = Gesture::Pan2D;
                }
                else if (alt && m_edges.lmb.pressed)     m_camGesture.kind = Gesture::Orbit;
                else if (m_edges.rmb.pressed)            m_camGesture.kind = Gesture::Look;
                else if (m_edges.mmb.pressed)            m_camGesture.kind = Gesture::Pan3D;
            }
            // Ruling C: `fresh` is TRUE only on the frame the gesture started
            // and FALSE on every later frame of it (and while there is none).
            // Assigned BEFORE the apply below reads it, so the press frame
            // itself is the one that applies nothing.
            m_camGesture.fresh = wasNone && m_camGesture.kind != Gesture::None;

            // A gesture lives exactly as long as ITS button is held -- an
            // orbit survives Alt lifting mid-drag (UE does the same), and a
            // mode switch mid-gesture ends it on the next frame through the
            // button test, never through the mode.
            const bool held = (m_camGesture.kind == Gesture::Pan2D && rmb)
                           || (m_camGesture.kind == Gesture::Look  && rmb)
                           || (m_camGesture.kind == Gesture::Orbit && lmb)
                           || (m_camGesture.kind == Gesture::Pan3D && mmb);
            if (!held)
            {
                m_camGesture.kind  = Gesture::None;
                m_camGesture.fresh = false;
            }
            else if (!m_camGesture.fresh)
            {
                // Not the press frame (Ruling C): lastMouse is a real previous
                // cursor, so the delta is the cursor's motion this frame and
                // not the whole distance from wherever it last was (same
                // guard as the old RMB-pan's "held across BOTH frames").
                switch (m_camGesture.kind)
                {
                case Gesture::Pan2D: m_camera.Pan2D(delta, ViewportSize()); break;
                case Gesture::Look:  m_camera.Look(delta);                  break;
                case Gesture::Orbit: m_camera.Orbit(delta);                 break;
                case Gesture::Pan3D: m_camera.Pan3D(delta, ViewportSize()); break;
                default: break;
                }
            }
            // WASD/QE fly only while LOOKING (right button). Narrower than
            // UE's WASD_RMBOnly default, which admits any held mouse button;
            // and the Shift boost is OURS (Unity's) -- UE has no boost, its
            // flight keys are DISABLED while Shift is held. The gizmo mode
            // keys (HandleGizmoModeKeys) already stand down while RMB is held
            // so W/E/Q cannot also switch the tool. dt is the frame's wall dt
            // so flight speed is in m/s regardless of frame rate.
            if (m_camGesture.kind == Gesture::Look)
            {
                const glm::vec3 axis = Arcane::Editor::ReadFlyAxis(Arcane::Editor::EditorActions::Get());
                if (axis != glm::vec3(0.0f))
                    m_camera.Fly(glm::normalize(axis), dt, shift);
                // The wheel WHILE looking steps the speed scalar (UE's
                // CameraSpeedScalar via the wheel during RMB flight), never
                // the zoom/dolly.
                if (snap.wheelY != 0.0f)
                {
                    m_camera.AdjustSpeed(snap.wheelY);   // live this frame; the cvar carries it on
                    Arcane::Editor::SetViewportPref("editor.camera.speedScalar",
                                                    Arcane::CVarValue::Float32(m_camera.speedScalar));
                }
            }
            // The wheel otherwise: 2D zoom anchored on the viewport-local
            // cursor (the pixel space the resolved view maps to), or the
            // Perspective dolly. Deliberately NOT gated on
            // snap.wantCaptureMouse: it is true over the viewport image by
            // design (see the pluginSnap comment above); inViewport already
            // folds in m_viewportActive, which is false whenever another
            // panel owns the cursor.
            else if (inViewport && snap.wheelY != 0.0f)
            {
                if (m_camera.mode == ViewMode::TwoD)
                    m_camera.ZoomAt2D(glm::vec2(lx, ly), snap.wheelY, ViewportSize());
                else
                    m_camera.Dolly(snap.wheelY);
            }
        }
        else
        {
            m_camGesture.kind  = Gesture::None;   // Play owns the pointer; drop any live gesture
            m_camGesture.fresh = false;
        }
        m_camGesture.lastMouse = mouse;

        // Alt+G -> Perspective, Alt+J -> 2D. The two persisted transforms mean
        // a switch RESTORES the other mode's framing rather than reprojecting
        // (EditorCamera.hpp); the mode is persisted with them. Same gate as
        // framing below (no viewport-focus requirement: the chord is
        // unambiguous, and it acts on the viewport wherever focus sits). A
        // live gesture is not cut short here: the next frame's `held` test
        // ends it when its button lifts, and its deltas keep going to the
        // op of the kind that started it, which is still meaningful.
        const Arcane::Editor::GlobalShortcutPresses keys =
            Arcane::Editor::ReadGlobalShortcuts(Arcane::Editor::EditorActions::Get());
        if (keys.perspective) m_camera.mode = ViewMode::Perspective;
        if (keys.ortho2D)     m_camera.mode = ViewMode::TwoD;
        if (keys.frameSelected) FrameCamera(m_selection.HasSelection());
        if (keys.frameAll)      FrameCamera(false);

        // Push the editor camera BEFORE the gizmo block below reads
        // ClientRuntime::View(). Those reads happen earlier
        // in the frame than the pre-SubmitRender push further down, so
        // without this the gizmo would hit-test against whatever camera
        // was stored LAST frame -- and on the very first frame against
        // the identity (zoom 1), placing the handles 100x off the
        // sprite. The click-pick and gizmo DRAW sites run after the
        // later push and are already consistent with it.
        if (!InPlayMode())
            m_runtime->SetView(m_camera.Resolve(ViewportSize()));
    }

    // Phase 6d: transform-gizmo hit-test + drag. Reads the camera the phase
    // above just pushed; must stay after it.
    void EditorApp::UpdateGizmoInteraction(const Arcane::InputSnapshot& snap, bool inViewport,
                                           float lx, float ly, bool gameUiClaims)
    {
        // Transform-gizmo interaction: hit-test + drag against the selected
        // entity's Transform, bracketed into the undo stack (one drag =
        // one undo step -- Begin/SnapshotComponent on press, Commit on release;
        // a no-move drag self-drops since Commit only pushes if bytes changed).
        // mouseScreen is viewport-local px (lx/ly computed above), the space
        // ViewTransform::WorldToScreen projects into, so the gizmo aligns
        // pixel-for-pixel with the scene in EVERY view mode (the id pass
        // projects through the same view). The LMB edge is tracked by the
        // camera phase just above
        // (UpdateEditorCamera Updates all three button edges once per frame,
        // unconditionally, so a button already held before the cursor enters
        // the viewport is never misread as a fresh press); this phase only
        // READS it -- a second Update with the same value would erase the
        // press edge. Deliberately does NOT gate on snap.wantCaptureMouse --
        // it is true over the viewport image by design (see the pluginSnap
        // comment above); `inViewport` already folds in m_viewportActive.
        const bool mousePressedLeft  = m_edges.lmb.pressed;
        const bool mouseReleasedLeft = m_edges.lmb.released;
        const glm::vec2 mouseScreen(lx, ly);
        const bool ctrlHeld = snap.ScancodeDown(kScLCtrl) || snap.ScancodeDown(kScRCtrl);

        // An in-progress drag must keep tracking (and commit on release)
        // even after the cursor leaves the viewport rect -- only a FRESH
        // drag requires the cursor in-viewport. Aborting a live drag on
        // viewport-exit would strand the Transform at a mid-drag value
        // with no undo record (CommandStack::Cancel does not revert), so
        // viewport-exit is deliberately NOT an abort. !gameUiClaims is
        // defensive: gameUiClaims is Play-only and this is already
        // !IsPlaying()-gated, so it is a no-op today, but it keeps this
        // gate correct if the gizmo is ever allowed to run in Play.
        //
        const bool gizmoActive = !InPlayMode() && !gameUiClaims &&
                                 GizmoLive() && m_selection.HasSelection() &&
                                 (m_gizmoDrag.active || inViewport);
        Astra::Registry*        regPtr = nullptr;
        const Arcane::Transform* lt     = nullptr;
        if (gizmoActive)
        {
            regPtr = &m_runtime->Registry();
            lt = std::as_const(*regPtr).GetComponent<Arcane::Transform>(m_selection.Primary());
        }

        if (lt)
        {
            const Astra::Entity sel = m_selection.Primary();
            // WORLD pose (Transform is parent-local; the gizmo anchors at the primary's world location).
            const Arcane::GizmoTransform gt = Arcane::DecomposeTRS(Arcane::Edit::WorldMatrix(*regPtr, sel));
            const Arcane::ViewTransform& view = m_runtime->View();
            const Arcane::GizmoHandleMask handles = GizmoHandles();
            const float gizmoSize = Arcane::Settings<Arcane::Editor::EditorGizmoSettings>().size;
            const Arcane::GizmoTuning gizmoTuning = Arcane::Editor::MakeGizmoTuning();   // editor.gizmo.* (settings S6-31)

            if (!m_gizmoDrag.active)
            {
                // Hover + drag-start only when the cursor is over the viewport.
                if (inViewport)
                {
                    m_gizmoHovered = Arcane::HitTest(m_gizmoMode, m_gizmoSpace, gt, view, handles, gizmoSize, mouseScreen, gizmoTuning);
                    if (m_gizmoHovered != Arcane::GizmoAxis::None && mousePressedLeft)
                    {
                        // A press on a handle owns the click regardless of
                        // whether the descriptor resolves, so it never falls
                        // through to the click-pick below.
                        m_gizmoCapturedClick = true;
                        const Astra::ComponentDescriptor* desc =
                            FindTransformDescriptor(*regPtr, sel);
                        if (desc)
                        {
                            // Roots only -- see GizmoDrag::targets. One
                            // Begin + N SnapshotComponent = ONE undo step:
                            // CommandStack dedupes per (entity, descriptor)
                            // and Commit packs them all into one transaction.
                            const std::vector<Astra::Entity> roots =
                                Arcane::Edit::SelectionRoots(*regPtr, m_selection.Entities());
                            m_gizmoDrag.targets.clear();
                            m_gizmoDrag.targets.reserve(roots.size());
                            // Keep the token: only its holder may close
                            // this transaction (see GizmoDrag::txn).
                            m_gizmoDrag.txn = m_undo->Begin("Gizmo");
                            for (Astra::Entity e : roots)
                            {
                                const Arcane::Transform* et = std::as_const(*regPtr).GetComponent<Arcane::Transform>(e);
                                if (!et)
                                    continue;   // non-spatial node in the selection
                                const Astra::ComponentDescriptor* ed =
                                    FindTransformDescriptor(*regPtr, e);
                                if (!ed)
                                    continue;
                                const glm::mat4 worldMat = Arcane::Edit::WorldMatrix(*regPtr, e);
                                m_undo->SnapshotComponent(e, ed);
                                // The mirror axis the AUTHOR chose, if any (first negative component).
                                int mirrorAxis = 0;
                                if      (et->scale.y < 0.0f && et->scale.x >= 0.0f) mirrorAxis = 1;
                                else if (et->scale.z < 0.0f && et->scale.x >= 0.0f && et->scale.y >= 0.0f) mirrorAxis = 2;
                                m_gizmoDrag.targets.push_back({ e, Arcane::DecomposeTRS(worldMat), mirrorAxis });
                            }
                            m_gizmoDrag.active           = true;
                            m_gizmoDrag.axis             = m_gizmoHovered;
                            m_gizmoDrag.start            = gt;
                            m_gizmoDrag.mouseStartScreen = mouseScreen;
                        }
                    }
                }
                else
                {
                    m_gizmoHovered = Arcane::GizmoAxis::None;
                }
            }
            else
            {
                // Current cursor in viewport-local px, EXTRAPOLATED past the
                // rect edges (ToViewportLocal writes lx/ly even when it returns
                // false) so the drag keeps tracking while the cursor is outside
                // the viewport. Recomputed here because the shared lx/ly are
                // only written when m_viewportActive is set.
                float dragLx = 0.0f, dragLy = 0.0f;
                Arcane::Editor::ToViewportLocal(m_viewportRect, snap.mouseX, snap.mouseY, dragLx, dragLy);
                const glm::vec2 dragMouse(dragLx, dragLy);

                const Arcane::GizmoSnap gsnap = Arcane::Editor::MakeGizmoSnap(ctrlHeld);   // editor.gizmo.snap.*
                const Arcane::GizmoTransform nt = Arcane::ApplyDrag(
                    m_gizmoMode, m_gizmoSpace, m_gizmoDrag.axis, m_gizmoDrag.start, view,
                    m_gizmoDrag.mouseStartScreen, dragMouse, gsnap, gizmoTuning);
                // The sector Draw paints for a rotate drag -- the same inputs.
                m_gizmoDrag.sweep = m_gizmoMode == Arcane::GizmoMode::Rotate
                    ? Arcane::RotateSweep(m_gizmoSpace, m_gizmoDrag.axis, m_gizmoDrag.start, view,
                                          m_gizmoDrag.mouseStartScreen, dragMouse, gsnap)
                    : std::nullopt;
                // One delta from the primary's drag, replayed onto every
                // target's PRE-drag pose -- recomputed from `start` each
                // frame, so nothing accumulates drift. The primary is in
                // `targets` when it is itself a root and round-trips
                // exactly (see ApplyGroupDelta).
                const Arcane::GizmoGroupDelta gd =
                    Arcane::MakeGroupDelta(m_gizmoDrag.start, nt);
                for (const auto& [e, startPose, mirrorAxis] : m_gizmoDrag.targets)
                {
                    Arcane::Transform* et = regPtr->GetComponent<Arcane::Transform>(e);
                    if (!et) continue;   // destroyed mid-drag
                    // startPose/gd are WORLD; demote through the parent's inverse
                    // before writing the LOCAL Transform (Unreal's SetWorldTransform).
                    const Arcane::GizmoTransform w = Arcane::ApplyGroupDelta(startPose, gd);
                    const glm::mat4 localMat = glm::inverse(Arcane::Edit::ParentWorldMatrix(*regPtr, e)) * Arcane::ComposeTRS(w);
                    // FULLY 3D since F4 (spec s7.2): every component is the gizmo's to
                    // write. The only massaging is the mirror's home axis.
                    const Arcane::GizmoTransform r = Arcane::WithMirrorOn(Arcane::DecomposeTRS(localMat), mirrorAxis);
                    et->position = r.position;
                    et->rotation = r.rotation;
                    et->scale    = r.scale;
                }
                m_gizmoHovered = m_gizmoDrag.axis;   // keep the active handle highlighted

                if (mouseReleasedLeft)
                {
                    m_undo->Commit(m_gizmoDrag.txn);   // no-move drag self-drops (after == before)
                    m_gizmoDrag          = {};         // clears txn back to None
                    m_gizmoCapturedClick = true;
                }
            }
        }
        else
        {
            m_gizmoHovered = Arcane::GizmoAxis::None;
            // Only reached with an active drag when the entity/component
            // genuinely vanished (deleted/deselected mid-drag) -- discard the
            // now-meaningless transaction.
            if (m_gizmoDrag.active)
            {
                m_undo->Cancel(m_gizmoDrag.txn);
                m_gizmoDrag = {};
            }
        }
    }

    // Phase 7: sim advance. The plugin's UpdateAll runs HERE, before the
    // pre-submit camera push below -- that ordering is what lets the editor
    // camera override a plugin that pushes its own every frame (Sandbox does).
    void EditorApp::AdvanceSim(LoopState& ls)
    {
        // Sim advance through the RunLoop with the plugin callbacks interleaved.
        // THE SIM CLOCK (F4 plan 1 T12, Ruling N) -- RuntimeFrame.cpp's simDt
        // branch, ported alongside FrameInput's frameDt: FIXED under
        // --headless, FROZEN during the settle hold, wall-clock otherwise. simDt
        // reaches RunLoop::Advance -- FixedUpdateAll/UpdateAll, gameplay -- so a
        // wall-clock simDt means `--frames N` advances a --play-as run's sim by
        // however long N frames happened to take on THIS machine, and no two
        // runs agree. The sim.maxFrameDeltaSeconds spiral-of-death clamp stays wall-clock-only:
        // fixedDtSeconds is refused at parse time unless positive (HostConfig.
        // cpp) and is a deliberate per-run choice, not a stall to guard against.
        // Edit mode steps no physics either way (EditModeSchedule owns the
        // edit pass), so the golden lane is unaffected by this half; it is
        // here so the editor's two clocks tell one story.
        double simDt;
        if (SettleHoldActive())
        {
            simDt = 0.0;   // see SettleHoldActive's own comment (EditorApp.hpp)
        }
        else if (m_config.headless)
        {
            simDt = m_config.fixedDtSeconds;
        }
        else
        {
            const auto now = std::chrono::steady_clock::now();
            simDt = std::chrono::duration<double>(now - ls.simPrev).count();
            ls.simPrev = now;
            simDt = Arcane::ClampFrameDelta(simDt);   // sim.maxFrameDeltaSeconds: the one clamp ArcaneRuntime shares
        }
        m_runtime->EnsurePhysics();   // engine-owned physics (spec s4.3); Edit mode's pass is EditModeSchedule's (Task 7)
        Arcane::ApplySimStepCap(m_runtime->Loop());   // sim.maxStepsPerFrame (Live)
        m_runtime->Loop().Advance(simDt,
            [&](double dt)          { m_runtime->BeginGameInputFixedStep(); if (m_plugin) m_plugin->FixedUpdateAll(dt); },
            [&](double dt, double a){ if (m_plugin) m_plugin->UpdateAll(dt, a); });
        // The embedded server world (EmbeddedServer topology) advances on the same
        // real dt, right after -- its OWN loop and its OWN Server-masked systems.
        // The module's FixedUpdate hook above is bound to the PRIMARY world and is
        // deliberately NOT re-run for it (MultiRuntimeReloadTest's StepAll documents
        // the same rule). A no-op on every other topology.
        m_play.TickServer(simDt);
        m_runtime->AudioSystem().Update(simDt);
    }

    // Phase 8: deferred viewport resize. Must stay before the scene render.
    void EditorApp::ApplyPendingViewportResize()
    {
        m_viewportTargets.ApplyPendingResize(ChromeGraph());
    }

    // Deferred resize: the Viewport panel's content-region size measured LAST
    // frame, applied at the START of THIS frame (before this frame records).
    // This mirrors the m_viewportRect/m_viewportActive one-frame lag on
    // EditorApp -- it avoids a same-frame use-after-free where a resize
    // (called right after ImGui::Image bakes the current texture pointer into
    // this frame's draw list) synchronously frees that very texture before
    // ImGui replays the draw list at Render time: ResizeOffscreen destroys the
    // output texture synchronously.
    void EditorApp::ViewportTargets::ApplyPendingResize(Arcane::NriGraphContext* chrome)
    {
        // ================= THE RESIZE CONTRACT ============================
        // Its shape is prescribed, clause for
        // clause, on NriGraphContext::ResizeOffscreen's declaration (and item
        // (2) of that header's TWO CONTEXTS, TWO LANES block). Read it before
        // touching these five lines; what follows is why each one is where it
        // is, not a restatement.
        //
        //   (i) THE SIZE GUARD IS OURS. ResizeOffscreen no-ops on an unchanged
        //       size, but that guard is INSIDE the call -- after
        //       InvalidateUserTextureNow has already idled the whole device
        //       unconditionally. Without the test below, a panel that is
        //       simply sitting there would stall the device EVERY FRAME,
        //       forever, on a size that never changed. SurfaceWidth/Height are
        //       exactly what ResizeOffscreen compares against, so the two tests
        //       cannot disagree.
        //
        //  (ii) INVALIDATE **BEFORE**, WITH THE `Now` VARIANT. The chrome
        //       context's ImGuiNri caches by RAW nri::Texture*, and NRI does
        //       not ref-count -- the replacement may land on the freed address,
        //       so a pointer compare cannot detect the swap and re-reading
        //       OffscreenTextureId() is not a substitute. Invalidating AFTER
        //       (or with the deferred variant) would bury the CHROME backend's
        //       view over an ALREADY-DESTROYED texture into the CHROME lane --
        //       DestroyDescriptor after DestroyTexture, every resize, for the
        //       one descriptor that inherently spans both contexts, which no
        //       lane ordering can repair because nothing orders two graveyards.
        //       `Now` destroys the view inside the call behind its own idle, so
        //       run first it provably dies while the texture is still alive.
        //       Unconditional, never gated on the pointer having changed: an
        //       unchanged pointer is precisely the case a recycled address
        //       fakes.
        //
        // (iii) THE PAIR IS ONE OPERATION. Nothing may render between them --
        //       after the invalidate there is NO cache entry for `old`, so a
        //       chrome frame recorded in between would take EnsureEntry's
        //       CREATE path and build a fresh view + descriptor set over a
        //       texture about to be destroyed. Hence: same statement block, no
        //       frame boundary, no early-out, nothing added between these two
        //       lines. `chrome` is guarded like every in-tree ImGuiHud() caller
        //       (it is null only if the chrome context failed to create, which
        //       CreateGraphVehicles refuses to continue past).
        //
        // COST, stated because it is visible: two full device idles per
        // resizing frame during a viewport drag-storm. That is the accepted
        // price of the ordering; a stall that OUTLIVES the drag would mean (i)
        // is broken. See the DESK WATCH block on ResizeOffscreen.
        //
        // A NULL `graph` means one thing only: a project switch whose rebuild
        // failed (SwitchProject's "render_bridge"), already exiting. This
        // frame still has to finish, and it finishes by doing nothing here.
        if (!graph)
            return;

        if (pendingW != 0 && pendingH != 0 &&
            (pendingW != graph->SurfaceWidth() || pendingH != graph->SurfaceHeight()))
        {
            nri::Texture* const old = graph->OffscreenOutput();
            if (chrome && chrome->ImGuiHud())
                chrome->ImGuiHud()->InvalidateUserTextureNow(old);   // BEFORE
            graph->ResizeOffscreen(pendingW, pendingH);
        }
    }

    // Phase 9: scene asset resolution + the Edit-mode derived-transform refresh
    // + the SECOND camera push. All three sit immediately before the scene
    // submit in the next phase, and each says below why it has to.
    void EditorApp::RefreshSceneResolution()
    {
        // Sprite / sprite-material / post-chain resolution, in ONE engine-side
        // call the standalone runtime host makes too (sprite-resolution lift).
        // It sweeps the scene for referenced Guids, pumps the compile service
        // (routing results to open documents first, via the consumeFirst hook
        // installed in EditorApp::Init), and publishes the registry's
        // SpriteTable + SpriteMaterialTable.
        //
        // WHY HERE, before RenderSceneToViewport rather than after it: the
        // tables this publishes are what THIS frame's submission reads. The
        // pre-lift editor swept and published in phase 13, AFTER the scene
        // render, so every table update was one frame late -- a newly referenced
        // sprite drew the 1x1 placeholder for a frame first. It also must stay
        // outside the frame batcher's Begin..Drain, since the drain REGISTERS
        // materials with it.
        //
        // The compile clock advances here (phase 13 advanced it pre-lift) because
        // everything downstream -- Request debounce, material globals, document
        // Submits -- reads it, and it must advance exactly ONCE per frame.
        m_editorClock += m_gameUi.frameDt;

        if (m_resolver)
        {
            Arcane::SceneRenderResolver::FrameInfo frame;
            // See RuntimeFrame.cpp's identical override: m_editorClock also drives
            // the compile service's Poll/Submit and the material file-watch
            // debounce (EditorAppProject.cpp), so only the scene's view of time is
            // pinned here.
            frame.now            = m_config.fixedTimeSeconds.value_or(m_editorClock);
            frame.dt             = m_gameUi.frameDt;
            frame.viewportWidth  = (float)ViewportWidth();
            frame.viewportHeight = (float)ViewportHeight();
            m_resolver->Refresh(frame);
        }
        if (!InPlayMode())
        {
            // Once per Edit-mode frame, through the editor's own scheduler (spec
            // 2026-09-11 s7) -- Play mode's fixedUpdate owns propagation, so
            // nothing runs twice. Then the ONE pending camera-frame request, on
            // the WorldTransforms this pass just refreshed and before the camera
            // push below, which every render path reads.
            m_editSchedule->RunFrame(m_runtime->Registry(), /*inPlayMode*/ false);
            m_editSchedule->ServicePendingFrame(m_runtime->Registry(), m_selection.Entities(), m_camera,
                                                glm::vec2(ViewportSize()));
        }

        // Editor camera -> the Runtime slot SetRenderContext reads, for
        // the SECOND time this frame (the first is in the input block, so
        // the gizmo hit-test sees it). It has to be re-pushed HERE, after
        // the plugin's UpdateAll ran above: a plugin that pushes its own
        // camera every frame -- Sandbox does -- would otherwise own the
        // Edit-mode view. Must stay after that Advance and before
        // SubmitRender below; moving it earlier hands the view back to
        // the plugin.
        //
        // Play mode pushes the SCENE's camera instead of the editor's, so
        // play-in-editor shows what a standalone ArcaneRuntime would show --
        // both now resolve the view from the same place (ActiveSceneCamera).
        // Falls through to the plugin's own camera when the scene has no
        // active Camera entity, which is what keeps the Sandbox showcase
        // (a plugin that drives its own camera) working unchanged.
        if (!InPlayMode())
        {
            m_runtime->SetView(m_camera.Resolve(ViewportSize()));
        }
        else if (const auto sceneCam = Arcane::ActiveSceneCamera(
                     m_runtime->Registry(),
                     glm::uvec2{ ViewportWidth(), ViewportHeight() }))
        {
            m_runtime->SetView(sceneCam->view);
        }

        // THE POST CHAIN'S HANDOFF IS NOT MADE HERE, and needs no statement:
        // the resolver runs device-less (StageSpriteTables) and PostChainCache
        // publishes BYTES, which travel to the recorder as FrameDesc::post +
        // FrameDesc::globals and are read fresh at the render site in the next
        // phase (RenderSceneToViewport).
    }

    // Phase 10: the scene render. Must run BEFORE the ImGui pass builds the
    // Viewport panel's Image, and the gizmo inside it is drawn AFTER the scene
    // submit so it renders on top.
    // ===== "IS A CAPTURE WANTED AT ALL", IN ONE PLACE =======================
    // Named rather than spelled out at each of the two capture sites below,
    // because Task 8 was bitten on the RUNTIME by exactly the divergence a
    // second copy invites: that host's gate read `screenshotPath` ALONE, so
    // `--report` with pixel probes and no `--screenshot` ran, exited 0, wrote a
    // well-formed JSON, and reported "no capture set" for every probe -- a
    // silent success an agent reads as a pass. RuntimeFrame.cpp arms on
    // capture-WANTED (`!screenshotPath.empty() || !reportPath.empty()`).
    //
    // TASK 9: THIS HOST'S GATE IS NOW THE SAME TWO-CLAUSE ONE, no longer the
    // narrower `--screenshot` alone this comment used to defend. --report is
    // wired (PresentChromeFrame's settle branch, ShutdownGraphPath's
    // VerifyReport), so a bare `--report r.json` with no `--screenshot` must
    // still arm the chrome readback -- without the OR, that combination would
    // arm nothing, and the report's capture section would silently read "no
    // capture set" even though the run converged cleanly. `--probe` is
    // deliberately NOT a third clause here: it stays refused at main.cpp, and
    // this host evaluates no probes, so widening this predicate for it would
    // arm a readback nothing downstream reads -- see main.cpp's own refusal
    // comment. Wire --probe here and both places must grow together the
    // same day.
    static bool CaptureWanted(const Arcane::HostConfig& cfg) noexcept
    {
        return !cfg.screenshotPath.empty() || !cfg.reportPath.empty();
    }

    // T3-D6: the WINDOWED composited-frame capture, opt in. A windowed
    // --screenshot captures the VIEWPORT texture (main.cpp's flag table);
    // `--set editor.automation.windowedFrameCapture=true` makes it capture the
    // swapchain backbuffer the editor just presented instead -- chrome,
    // panels and viewport, the picture --headless captures offscreen -- so an
    // automated desk pass can judge the WINDOWED editor (works-headless/
    // broken-windowed is a bug class) without driving the desktop. Dev (never
    // in Dist), NOT Archive: an automation switch must never persist into a
    // user's settings. The editor's --set runs in the Editor context, which
    // writes any setting (UserSettable is derived from the audience now, so a
    // declaration no longer passes it -- settings spec s3.2).
    namespace
    {
        ARC_CVAR(cvar_windowedFrameCapture, "editor.automation.windowedFrameCapture", bool, false,
                 .flags = ::Arcane::CVarFlags::Dev,
                 .audience = ::Arcane::Audience::Editor, .scope = ::Arcane::SettingScope::PreferencesProject,
                 .help = "Windowed --screenshot captures the composited editor frame (the presented "
                         "backbuffer: chrome, panels, viewport) instead of the viewport texture");
    }

    static bool WindowedFrameCapture(const Arcane::HostConfig& cfg)
    {
        if (cfg.headless)
            return false;   // --headless already captures the composited frame, offscreen
        return cvar_windowedFrameCapture.Get();
    }

    void EditorApp::RenderSceneToViewport()
    {
        // ================= THE VIEWPORT FRAME =============================
        // THIS PHASE DECLARES THE WHOLE VIEWPORT FRAME -- the scene, the game
        // HUD (phase 11's content) and the pick + outline chain (phases 12 and
        // 17's), because all three are NODES in ONE declaration rather than
        // three passes submitted at three points in the frame.
        // ArmGraphViewportFrame below is the other phases' half; see its
        // comment for why it has to run HERE and not where those phases sit.
        //
        // WHAT THIS FRAME DOES NOT CARRY, so the gap stays named rather than
        // discovered:
        //   * imgui        -- HOST CHROME's draw data. Not the game HUD (that
        //                     is `gameUi`): this viewport has no chrome on it,
        //                     and the editor's own chrome belongs to phase 10
        //                     on the CHROME context. Absent here.
        //   * capture      -- the auto-screenshot re-renders through
        //                     CaptureGraphViewportPng's OWN fresh frame rather
        //                     than reading this one. The --screenshot capture
        //                     piggybacks on THIS declaration instead, on the
        //                     run's last frame only -- see below.
        if (m_viewportTargets.graph)
        {
            const std::uint32_t w = ViewportWidth();
            const std::uint32_t h = ViewportHeight();

            // F3 (plan 1 T8): the CPU visible set(s), the GPU-scene sync and
            // the frame's batches -- between the schedulers (phase 9's
            // Advance left WorldTransform + WorldBounds current) and the
            // sprite sweep inside SubmitSceneToBatcher (which reads views[0]
            // through MainVisibleSet). The mesh view is the editor view in
            // Edit and the perspective scene camera in Play (RuntimeFrame.cpp's
            // rule); nullopt when Play has none, and then no batch is built --
            // the pass still declares itself for any staged rows (R-D).
            //
            // HERE, not inside SubmitSceneToBatcher next to its SubmitRender:
            // that function is also CaptureGraphViewportPng's (a scene save's
            // cover, shutdown), and a second GpuSceneSync there would stage
            // rows into m_gpuSceneFrame that no mesh pass uploads -- the
            // mirror's lastModel history having already advanced past them,
            // the next viewport frame would never re-stage them. One sync per
            // viewport frame, from the one function that declares the frame.
            m_meshView.reset();
            if (!InPlayMode())
                m_meshView = m_runtime->View();
            else
                m_meshView = Arcane::ActivePerspectiveSceneCamera(m_runtime->Registry(), glm::uvec2{ w, h });
            Arcane::PrepareSceneForRender(m_runtime->Registry(), m_runtime->View(), m_meshView,
                                          m_viewportTargets.graph->Scene(), m_gpuSceneFrame);

            Arcane::Batcher2D& b = m_gpu->Batch();
            b.Begin(w, h);
            SubmitSceneToBatcher(b);

            // Copied, not borrowed, by FrameDesc -- but the local has to
            // outlive the call all the same, and it is the SAME GlobalParams
            // the batcher just got via SubmitSceneToBatcher's SetGlobals, so
            // the two recorders cannot bind different b1 contents.
            const Arcane::GlobalParams globals =
                m_resolver ? m_resolver->Globals() : Arcane::GlobalParams{};

            Arcane::NriGraphContext::FrameDesc vp;
            vp.batch   = &b;
            // Re-read every frame, like the runtime's: a drain may swap the
            // bound instance under an asset re-save. Null (no PostProcess
            // assignment, or the compile has not landed) simply renders
            // canvas -> tonemap, which is also how a caller asks for the frame
            // WITHOUT a post chain at all.
            vp.post    = m_resolver ? m_resolver->PostDesc() : nullptr;
            vp.globals = &globals;
            // Phases 11, 12 and 17's contribution to THIS declaration. Strictly
            // AFTER SubmitSceneToBatcher above, which is not incidental: it
            // runs the plugin's DrawUI, and phase 11 follows phase 10. A
            // DrawUI that mutates the scene must land on THIS frame, not the
            // next.
            ArmGraphViewportFrame(vp);

            // THE --screenshot CAPTURE, armed on the LAST frame only -- the
            // same "+1" the runtime's RenderGraph uses (there io.frameCount is
            // bumped in CaptureTail, i.e. after that arm returns; here
            // EndFrame's own m_frameCount increment happens later still, at
            // phase 20). Piggybacking on the frame ALREADY being declared
            // here, rather than re-rendering the way CaptureGraphViewportPng
            // does for the auto-screenshot, is what keeps the captured picture
            // governed by exactly the frame the run was showing.
            //
            // `maxFrames != 0` matches the runtime: both capture on the LAST
            // frame, and without --frames there is no last frame.
            //
            // AND NOT UNDER --headless, which is the whole point of Task 11:
            // there the screenshot is the COMPOSITED EDITOR FRAME, captured off
            // the chrome context in PresentChromeFrame (phase 19) -- panels,
            // docking, asset browser and this viewport's texture inside its
            // panel. Leaving this arm live as well would have the viewport
            // capture land first and the chrome capture overwrite it with a
            // different picture at the same path: same run, same flag, two
            // meanings. The WINDOWED behaviour is unchanged -- there the chrome
            // context is a swapchain nothing reads back, so the viewport
            // texture remains the only editor screenshot there is (main.cpp's
            // flag table says so).
            // ...nor when the windowed composited capture is opted in (T3-D6,
            // WindowedFrameCapture): then the chrome frame owns the PNG, for
            // the same one-flag-one-meaning reason.
            const bool isCaptureLastFrame = m_config.maxFrames != 0 &&
                                            (m_frameCount + 1) >= m_config.maxFrames &&
                                            CaptureWanted(m_config) &&
                                            !m_config.headless &&
                                            !WindowedFrameCapture(m_config);
            vp.capture = isCaptureLastFrame;

            if (Arcane::OutlineNode* outline = m_viewportTargets.graph->Outline())
            {
                const Arcane::Editor::AxisRoleColors roles = Arcane::Editor::DeriveAxisRoles(Arcane::Editor::Theme::Live());
                outline->SetColors(roles.outlineSelect, roles.outlineHover);
            }
            const Arcane::NriGraphContext::FrameOutcome outcome =
                m_viewportTargets.graph->RenderFrameOffscreen(vp);
            // Any outcome but Presented means GpuSceneSyncNode did not run, so
            // the rows PrepareSceneForRender staged above never reached the
            // GPU and would otherwise never be re-staged (ruling R-F): give
            // the mirror a new generation so the next sync is a full rebuild.
            if (outcome != Arcane::NriGraphContext::FrameOutcome::Presented)
                Arcane::GpuSceneInvalidate(m_runtime->Registry());
            // Skipped stays UNACTED-ON (beyond the invalidate just above), and
            // that is the routine case: a
            // collapsed panel, or a resize whose replacement could not be
            // created -- OffscreenTextureId() is 0 then and phase 16 draws no
            // Image, which IS the signal. A run whose last frame lands Skipped
            // simply does not capture this iteration: m_frameCount is driven
            // by phase 19/20 (the CHROME frame's own success), not by this
            // outcome.
            //
            // Failed is acted on. It has already bumped RenderErrorCount, so
            // the run's exit code
            // would be 2 on its own -- but 2 says "a validation error fired"
            // where 1 says WHERE the run died, and a viewport frame that could
            // not be recorded or submitted is the second thing. NoteGraphFrame-
            // Failure records both halves: the exit code and the request to
            // stop.
            //
            // THE REST OF THIS FRAME STILL RUNS, and the reason is NOT an ImGui
            // pairing: nothing is owed at this point in the frame. Phase 10
            // runs BEFORE DrawEditorUi (phase 13) begins the editor's frame, so
            // there is nothing begun to pair; and the GAME context's frame is
            // begun and paired entirely inside ArmGraphViewportFrame above,
            // before the call that just failed. Bailing here would strand
            // neither.
            //
            // The reason is that this phase has no stop channel and should not
            // grow one: RenderSceneToViewport returns void, and threading an
            // immediate bail through the nine phases between here and the loop
            // bottom would be real control flow added for a case that costs one
            // more chrome frame. That frame is safe to run -- if the device is
            // genuinely gone it fails too and folds to the same exit code (1,
            // first-failure-wins). So the exit lands at the TOP of the next
            // frame instead, where PumpFrameEvents reads m_requestExit before
            // any phase runs: nothing renders after this frame.
            if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)
            {
                NoteGraphFrameFailure("the viewport frame could not be recorded or submitted");
                return;
            }

            // The readback, RIGHT HERE rather than a later phase:
            // ReadCapture is synchronous (it idles the device internally --
            // the same stall CaptureGraphViewportPng pays, and this call is
            // rare -- ordinarily exactly once, on the run's last frame), and
            // the captured pixels belong to the frame that was JUST
            // presented -- reading them any later risks the next frame's
            // Begin/Draw overwriting the vehicle's transient state first.
            //
            // NOT ALWAYS EXACTLY ONCE, though: if the CHROME frame (phase 19,
            // PresentFrame/PresentChromeFrame) fails to present this same
            // iteration, MainLoop `continue`s WITHOUT running EndFrame,
            // m_frameCount does not advance, and `isCaptureLastFrame` is
            // unchanged on the retry -- so a second viewport frame that DOES
            // present here re-attempts the capture and simply overwrites the
            // same file with equivalent content. Harmless, and orthogonal to
            // whatever made the chrome frame fail.
            if (isCaptureLastFrame && outcome == Arcane::NriGraphContext::FrameOutcome::Presented)
            {
                std::uint32_t cw = 0, ch = 0;
                std::vector<unsigned char> actual;
                if (!m_viewportTargets.graph->ReadCapture(cw, ch, actual))
                {
                    // A WARN, never an exit code -- the same contract
                    // RuntimeFrame::CaptureTail keeps.
                    // "(no capture landed)" -- the SAME wording
                    // RuntimeFrame::CaptureTail uses. It was
                    // "(viewport capture readback)" here, a third variant
                    // across the two hosts, which makes a log line unsearchable
                    // for the one failure it names.
                    ARC_WARN("screenshot FAILED: {} (no capture landed)",
                             m_config.screenshotPath);
                }
                else
                {
                    if (Arcane::WritePngRgba(m_config.screenshotPath, cw, ch, actual.data()))
                        ARC_INFO("screenshot written: {}", m_config.screenshotPath);
                    else
                        ARC_WARN("screenshot FAILED: {}", m_config.screenshotPath);
                }
            }
            return;
        }

        // A session that lost its viewport context to a failed post-switch
        // rebuild has nothing to fall through to: the exit is already
        // requested, and this frame just ends here without a fault.
    }

    // The scene submission plus the two Edit-mode overlays, against whichever
    // batcher the caller hands in. Kept as its own function so the ORDER of
    // these statements is stated in one place -- every overlay's position
    // relative to the scene submission is part of the contract.
    void EditorApp::SubmitSceneToBatcher(Arcane::Batcher2D& b)
    {
        // Globals for registered sprite materials (Time/Delta/
        // Viewport); built-in pipelines ignore them. Taken from the
        // resolver, which built them from the same frame in phase 9 --
        // this used to be a second hand-rolled copy of the same four
        // fields, and two copies of one fact is how the editor and the
        // runtime drifted apart in the first place.
        b.SetGlobals(m_resolver ? m_resolver->Globals() : Arcane::GlobalParams{});

        // The frame's view-projection for the WORLD-space spans (F4 plan 1
        // T5): sprites submit world METRES through QuadWorld/CircleWorld and
        // the vertex shader projects them, so the batcher needs the same view
        // the overlay and the gizmo below read. Sticky within this Begin()
        // bracket only -- both callers (the viewport frame and the capture
        // re-Begin) reach this line right after their Begin.
        b.SetViewProjection(m_runtime->View().ViewProjection());

        // The 2D reference grid (F4 plan 1 T9, spec s5.1), FIRST and at the
        // BOTTOM batcher layer (0, 0) so every sprite paints over it: decade
        // levels on the metre, chosen from this frame's pixels-per-metre and
        // clipped to the view's own world rect (ViewportGrid.hpp). Edit mode,
        // the 2D view mode and the Show grid setting gate it; the Persp view's
        // grid is Task 10's depth-tested GridNode, not these lines. In Play
        // the view is the scene camera's and the grid is an editor affordance,
        // so it stays off.
        if (!InPlayMode() && m_camera.mode == Arcane::Editor::ViewMode::TwoD
            && Arcane::Settings<Arcane::Editor::EditorViewportSettings>().showGrid)
        {
            const Arcane::ViewTransform& view = m_runtime->View();
            Arcane::Editor::DrawGrid2D(b, view, Arcane::Editor::PlanGrid2D(Arcane::Editor::PixelsPerMetre(view)));
        }

        m_runtime->SetRenderContext(&b);
        m_runtime->Loop().SubmitRender();

        // Physics overlay (spec 2026-09-11-physics-2d-wiring s6.3), drawn AFTER
        // the sprites and BEFORE the gizmo, through the SAME camera and the
        // SAME interp buffer + alpha the sprites used, so overlay and sprite
        // agree to the bit. The decision is PlanPhysicsOverlay (pure, tested).
        {
            Astra::Registry& reg = m_runtime->Registry();
            const Arcane::PhysicsWorld2D* phys = reg.GetResource<Arcane::PhysicsWorld2D>();
            std::optional<Manifold2D::Physics::BodyHandle> selectedBody;
            // entityToBody (a plain Arcane-level map), not Arcane::PhysicsBodyRef2D +
            // PhysicsWorld::IsValid: ArcaneEditor.exe does not link Manifold2D
            // (physics logic lives inside Arcane.dll only, same boundary the
            // NRI include-only comment above documents) -- a direct call into
            // an out-of-line PhysicsWorld method here is an unresolved symbol.
            if (m_selection.HasSelection() && phys)
                if (auto it = Arcane::Detail::Physics2D::Access::Entities(*phys).find(m_selection.Primary()); it != Arcane::Detail::Physics2D::Access::Entities(*phys).end())
                    selectedBody = it->second;
            const Arcane::Editor::PhysicsOverlayPlan plan =
                Arcane::Editor::PlanPhysicsOverlay(InPlayMode(), m_physicsOverlay, selectedBody.has_value());
            // The overlay draws in pixels through the view's Affine2D (F4 plan 1
            // T3): no context, or a view with no per-axis affine (perspective,
            // Task 7), skips the overlay this frame.
            const Arcane::RenderContext2D* ctx = reg.GetResource<Arcane::RenderContext2D>();
            const std::optional<Arcane::Affine2D> overlayAffine =
                ctx ? ctx->view.AsAffine2D() : std::nullopt;
            if (plan.draw && phys && Arcane::Detail::Physics2D::Access::Solver(*phys) && overlayAffine)
            {
                Arcane::PhysicsDebugDrawOptions2D opts = Arcane::MakePhysicsDebugDrawOptions();   // debug.physics.*
                opts.view   = *overlayAffine;
                opts.alpha  = ctx->alpha;
                opts.interp = reg.GetResource<Arcane::PhysicsInterpBuffer2D>();
                opts.velocities = opts.comMarkers = opts.orientations = false;   // outlines + contacts (spec)
                opts.contacts   = plan.wholeWorld;
                if (!plan.wholeWorld) opts.onlyBody = selectedBody;
                Arcane::DrawPhysicsDebug(*Arcane::Detail::Physics2D::Access::Solver(*phys), b, opts);
            }
        }

        // Transform gizmo, drawn AFTER the scene submit so it renders on
        // top. Frame ordering guarantees the input block above already ran
        // this frame, so m_gizmoHovered/m_gizmoDrag and the live
        // Transform read here are current. Edit-mode + has-selection
        // only (mirrors the interaction gate; no viewport-hover requirement
        // here -- the gizmo should stay visible while e.g. the mouse is over
        // the Inspector, just not be interactable there).
        // Camera rect: what the scene camera will actually capture, drawn
        // in the EDIT view so framing is authorable without guessing (the
        // affordance Unity's camera gizmo provides). Edit mode only -- in
        // Play the view IS the camera, so a rect would just trace the
        // viewport border.
        //
        // Asked for at the camera's OWN aspect (the viewport's), then drawn
        // through the EDITOR's view: ActiveSceneCamera reports the authored
        // worldCenter/halfHeight precisely so a caller can draw the camera
        // instead of looking through it. The four WORLD corners go through
        // ViewTransform::WorldToScreen (F4 plan 1 T3), which works in both
        // view modes; a corner that projects to a non-finite pixel (behind a
        // perspective eye) skips the rect for the frame.
        if (!InPlayMode())
        {
            const glm::uvec2 vp{ ViewportWidth(), ViewportHeight() };
            if (const auto cam = Arcane::ActiveSceneCamera(m_runtime->Registry(), vp))
            {
                const float aspect = (vp.y > 0u) ? (float(vp.x) / float(vp.y)) : 1.0f;
                const float hw = cam->halfHeight * aspect;
                const float hh = cam->halfHeight;
                const glm::vec2 c = cam->worldCenter;
                const Arcane::ViewTransform& ev = m_runtime->View();
                const glm::vec3 p[4] = {
                    ev.WorldToScreen(glm::vec3(c.x - hw, c.y - hh, 0.0f)),
                    ev.WorldToScreen(glm::vec3(c.x + hw, c.y - hh, 0.0f)),
                    ev.WorldToScreen(glm::vec3(c.x + hw, c.y + hh, 0.0f)),
                    ev.WorldToScreen(glm::vec3(c.x - hw, c.y + hh, 0.0f)),
                };
                bool finite = true;
                for (const glm::vec3& q : p)
                    finite = finite && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
                if (finite)
                {
                    // A thin desaturated line stays legible over both bright and
                    // dark scene content without competing with the selection
                    // outline or the gizmo axes. (Dashed would read better still,
                    // but the batcher has no dash primitive.)
                    // editor.theme.viewport.cameraFrame (settings S6-26).
                    static const EditorThemeViewportSettings kVpDefaults{};
                    const ImVec4 frame = ResolveDomainColor(Arcane::Settings<EditorThemeViewportSettings>().cameraFrame,
                                                            kVpDefaults.cameraFrame, kCameraFrameColor);
                    const glm::vec4 col(frame.x, frame.y, frame.z, frame.w);
                    for (int i = 0; i < 4; ++i)
                        b.Line(glm::vec2(p[i]), glm::vec2(p[(i + 1) % 4]), 1.0f, col);
                }
            }
        }

        // The gizmo is NOT drawn here any more (F4 plan 2, desk finding
        // 2026-09-18): the scene batch is painted before the mesh pass, so a
        // gizmo in it vanished inside any mesh covering its handles. It paints
        // as viewport FOREGROUND chrome instead -- see the gizmoOverlay lambda
        // handed to DrawViewportPanel.
    }

    // Phase 11's CPU HALF -- see the declaration. The GPU composite is a NODE
    // inside the viewport frame rather than a call from here, so this is all
    // of phase 11 there is as code, and ArmGraphViewportFrame is its only
    // caller.
    bool EditorApp::BeginGameUiFrame()
    {
        // Edit mode: not run (clean viewport); the input is reset so a stray
        // draw sees no cursor/buttons. THIS RESET IS NOT CONDITIONAL ON THE
        // ARM and must not become so -- it is what guarantees a game context
        // that is never begun in Edit still reads the cursor as off-target if
        // anything ever does begin it.
        if (!InPlayMode())
            m_gameImgui->SetInput({});

        const Arcane::PluginVTable* vtGame = m_plugin ? m_plugin->Vtable() : nullptr;
        if (!InPlayMode() || !vtGame || !vtGame->DrawUI)
            return false;

        Arcane::OffscreenImGuiLayer::Input gi;
        gi.displaySize  = glm::vec2((float)ViewportWidth(), (float)ViewportHeight());
        gi.deltaTime    = (float)m_gameUi.frameDt;
        gi.hasInput     = m_gameUi.inViewport;
        gi.mousePos     = m_gameUi.viewportMouse;
        gi.mouseDown[0] = (m_gameUi.mouseButtons & 0x1u) != 0;   // LMB
        gi.mouseDown[1] = (m_gameUi.mouseButtons & 0x2u) != 0;   // RMB
        gi.mouseDown[2] = (m_gameUi.mouseButtons & 0x4u) != 0;   // MMB
        gi.wheel        = m_gameUi.wheel;
        m_gameImgui->SetInput(gi);
        m_gameImgui->BeginFrame();
        m_plugin->DrawUIAll();   // game module + any secondary plugins, into the game context
        // The caller now owes EXACTLY ONE of Render / RenderToDrawData /
        // EndFrameDiscard. Returning true rather than doing it here is what
        // keeps that obligation visible at the two call sites instead of split
        // across a helper.
        return true;
    }

    // Phases 11, 12 and 17's contribution to the ONE declaration phase 10
    // makes.
    //
    // WHY IT IS CALLED FROM PHASE 10 RATHER THAN FROM THOSE PHASES. All three
    // are NODES in one graph that is declared, compiled and executed by a
    // single RenderFrameOffscreen call -- so everything the frame will contain
    // has to be known BEFORE that call, and phase 10 is where it is made.
    // Moving the call EARLIER is not an option either: the game HUD's draw
    // data must be produced AFTER the scene submission, so that a plugin whose
    // DrawUI mutates the scene lands on this frame rather than the next.
    // Phases 11, 12 and 17 keep their gates and each points at this
    // function.
    void EditorApp::ArmGraphViewportFrame(Arcane::NriGraphContext::FrameDesc& vp)
    {
        // ---- phase 11's content: the game HUD ---------------------------
        // Play-mode only, exactly as that phase is. The draw data lives in the
        // game context until its next NewFrame -- a whole frame away -- which
        // is what lets the node copy the geometry at record time
        // (FrameDesc::gameUi).
        if (BeginGameUiFrame())
        {
            // The draw data goes to the graph, which declares the GameUi
            // node for it. A caller that wants the frame WITHOUT the game HUD
            // leaves vp.gameUi null instead -- that is the whole mechanism
            // now, and it lives in FrameDesc rather than in a stage ordinal.
            vp.gameUi = m_gameImgui->RenderToDrawData();
        }

        // ---- F2a Task 10's content: the opaque 3D pass (F3 plan 1 T8) -----
        // UNCONDITIONAL, unlike the pick + outline chain below: a scene's
        // meshes are ordinary scene content, and SubmitSceneToBatcher (the 2D
        // equivalent, called from RenderSceneToViewport just before this
        // function) draws in Edit AND Play alike -- neither is gated on
        // InPlayMode(). This section has to sit ABOVE the pick chain's early
        // return just below, or a frame with no selection and no hover would
        // silently stop submitting meshes too.
        //
        // m_gpuSceneFrame was built by PrepareSceneForRender in
        // RenderSceneToViewport (see the comment there for why it runs
        // BEFORE the sprite sweep rather than here); this site only hands
        // it to the graph. THE GATE IS Empty(), NOT HasDraws() (ruling R-D):
        // a frame whose meshes are all culled -- or that has no mesh view at
        // all (Play with no perspective scene camera) -- can still carry
        // staged rows or a full rebuild, and the mirror's lastModel history
        // has already advanced past them, so the pass must declare itself
        // for GpuSceneSyncNode's upload; MeshNode then records only its
        // depth clear. Without a mesh view the matrices are left as they
        // were (no batch reads them).
        m_meshScene.scene = &m_gpuSceneFrame;
        if (m_meshView)
        {
            m_meshScene.view       = m_meshView->view;
            m_meshScene.projection = m_meshView->projection;
        }
        // NO LIGHT COMPONENT YET: this engine has none anywhere --
        // Scene/Components.hpp declares none and SceneModule.hpp registers
        // none -- so the scene's one light is render.mesh.defaultLight.*
        // (RenderLookSettings.hpp; Live, read once per frame). A light
        // component is future work and a reviewed scene-schema change.
        Arcane::ApplyDefaultLight(m_meshScene, Arcane::Settings<Arcane::RenderMeshDefaultLightSettings>());

        // vp.mesh is left at FrameDesc's own default (null) for an Empty()
        // scene -- the same "ask for the frame WITHOUT this stage by leaving
        // the field null" mechanism vp.gameUi just used above.
        if (!m_meshScene.Empty())
            vp.mesh = &m_meshScene;

        // ---- F4 plan 1 T10's content: the 3D reference grid ----------------
        // EDITOR CHROME, gated three ways: Edit mode (an editor affordance,
        // never the game's picture), the PERSPECTIVE view mode (the 2D view's
        // grid is the batched ViewportGrid lines drawn in SubmitSceneToBatcher
        // -- one grid per view mode, never both), and the Show grid setting.
        // The plane is the view setting; SetPlane also picks the V-axis
        // colour spec s5.2 pairs with it (Z blue on XZ, Y green on XY). The
        // camera is the SAME ViewTransform the sprites, the mesh pass and the
        // gizmo just used, so the grid sits under the meshes to the pixel.
        // Nothing here reaches the pick chain below: the grid is canvas
        // content only (GridNode.hpp).
        if (!InPlayMode() && m_camera.mode == Arcane::Editor::ViewMode::Perspective
            && Arcane::Settings<Arcane::Editor::EditorViewportSettings>().showGrid)
        {
            // The tunables are editor.viewport.grid.* / grid3D.* (settings
            // S6-21), rebuilt every frame so a Live edit lands next frame.
            m_gridScene = Arcane::Editor::MakeGridScene(Arcane::Settings<Arcane::Editor::EditorGridSettings>(),
                                                        Arcane::Settings<Arcane::Editor::EditorGrid3DSettings>());
            m_gridScene.view = m_runtime->View();
            m_gridScene.SetPlane(Arcane::Settings<Arcane::Editor::EditorViewportSettings>().gridPlane == Arcane::Editor::GridPlane::XY
                                     ? Arcane::GridSceneDesc::Plane::XY
                                     : Arcane::GridSceneDesc::Plane::XZ);
            vp.grid = &m_gridScene;
        }

        // ---- phases 12 + 17's content: the pick + outline chain ----------
        // THE HOVER GOES THROUGH HoverLive(), not m_gameUi.inViewport
        // directly: that field is derived from the LIVE pointer, so reading it
        // here would make what this frame outlines a function of where the
        // mouse happens to be. See the predicate's own comment.
        const bool hoverLive = HoverLive();
        // PHASE 12'S GATE, VERBATIM: Edit mode, and something to outline
        // (a selection, or a cursor in the viewport that might hover one).
        // In EVERY view mode (F4 spec s7.1): the id pass projects world-space
        // drawables through FrameDesc::pickView, so a perspective view
        // outlines and picks exactly as the 2D one does -- plan 1's
        // AsAffine2D gate on this predicate is lifted.
        const bool wantOutline = !InPlayMode() && (m_selection.HasSelection() || hoverLive);
        // ...and phase 17's: a click whose readback has not landed keeps the
        // chain declared even when nothing wants an outline, because the
        // readback node is the only thing that DRAINS the slot its copy went
        // into. Dropping the chain mid-flight would strand that slot's pending
        // flag until the chain happened to come back. DeferredPick::Busy() is
        // true from the click until the answer lands or is abandoned, which is
        // exactly the window this needs.
        const bool wantPick = m_deferredPick.Busy();
        // NOTE THE ASYMMETRY WITH THE ENGINE: RgFrameShape::pickOutline is
        // deliberately independent of how the scene is sliced, because the
        // runtime arms it from `--pick-probe` and nothing else. The EDITOR's
        // own decision about when to arm the chain therefore lives HERE, at
        // the call site, rather than in DeclareGraphFrame -- gating it in the
        // engine would change what that flag means on the other host.
        if (!wantOutline && !wantPick)
        {
            // Cleared rather than left stale: these are the spans the NEXT
            // armed frame will publish, and a leftover from three frames ago is
            // an id assignment that no longer matches any id pass.
            m_pickDrawables.clear();
            m_pickSelectedIds.clear();
            return;
        }

        // THE ONE EMITTER (PickEmit.hpp) -- a pure registry walk that emits
        // WORLD-space silhouettes (F4 spec s7.1); the id pass projects them
        // through the ViewTransform handed over below, so they register
        // pixel-for-pixel with what was drawn in EVERY view mode. The k-th
        // entry IS hit-proxy id k+1.
        m_pickDrawables.clear();
        Arcane::CollectPickables(m_runtime->Registry(), m_pickDrawables);

        // Every selected entity that made it into THIS frame's id pass, over
        // the same drawables that were just handed to the pick node rather
        // than over a table some earlier pass left behind. Empty in Play,
        // which is how
        // "phase 12 does not run in Play" is expressed here: the seed finds
        // no selected silhouette and the composite discards every pixel.
        m_pickSelectedIds.clear();
        if (wantOutline)
        {
            m_pickSelectedIds.reserve(m_selection.Count());
            for (Astra::Entity e : m_selection.Entities())
            {
                const std::uint32_t id = Arcane::PickPassIdOf(m_pickDrawables, e);
                if (id != 0u)
                    m_pickSelectedIds.push_back(id);
            }
        }

        vp.pickOutline = true;
        vp.pickables   = m_pickDrawables;
        // The same ViewTransform the sprites, the mesh pass and the gizmo just
        // used -- Edit: the editor camera; Play: the scene camera SetView pushed.
        vp.pickView    = m_runtime->View();
        vp.selectedIds = m_pickSelectedIds;
        // The HOVER cursor: the viewport-local pointer when it is over the
        // viewport, and the (-1,-1) "no hover" sentinel otherwise -- including
        // in Play, where the outline must not light anything.
        vp.hoverPixel  = (wantOutline && hoverLive)
                       ? glm::ivec2((int)m_gameUi.viewportMouse.x, (int)m_gameUi.viewportMouse.y)
                       : glm::ivec2(-1, -1);

        // THE CLICK, if one has been waiting for a frame to rasterise it. The
        // id<->entity table handed over is THIS frame's -- the one the id pass
        // about to be declared will be built from, and therefore the only table
        // the id coming back can honestly be inverted through.
        vp.pickPixel  = glm::ivec2(0, 0);   // nothing to resolve: a texel nobody reads
        vp.pickTicket = 0;                  // ...and the label that says so (DeferredPick::Land refuses 0)
        if (m_deferredPick.State() == Arcane::Editor::DeferredPick::Phase::Armed)
        {
            std::vector<Astra::Entity> ordered;
            ordered.reserve(m_pickDrawables.size());
            for (const Arcane::PickDrawable& d : m_pickDrawables)
                ordered.push_back(d.entity);
            if (const auto request = m_deferredPick.TakeRequest(ordered))
            {
                vp.pickPixel  = request->pixel;
                vp.pickTicket = request->ticket;
            }
        }
    }

    // Phase 11: the game's own ImGui HUD, composited into the viewport texture.
    // Runs AFTER the scene render above and BEFORE the editor's BeginFrame below.
    // Game debug UI -> the plugin's OWN ImGui context, composited into the
    // viewport's output texture (Play only). Edit mode: not run (clean
    // viewport).
    //
    // ===== THIS PHASE'S WORK HAPPENS IN PHASE 10 =====
    // The HUD is a NODE inside the viewport frame, and that frame is
    // declared, compiled and executed by phase 10's single
    // RenderFrameOffscreen call -- which has already happened by the time
    // control reaches here. ArmGraphViewportFrame (above) is where this
    // phase's content is produced, through the SAME BeginGameUiFrame helper,
    // at the same point relative to the scene submission.
    //
    // THE GAME HUD HAS ITS OWN ImGui CONTEXT, and that is load-bearing rather
    // than tidy: the editor's context has io.IniFilename pointed at the
    // PER-PROJECT LAYOUT FILE, so a plugin window drawn through it would
    // persist into the user's layout permanently -- the imgui.ini veto class.
    // The game context's io.IniFilename is null for exactly that reason.
    //
    // PHASE 12 (Edit-mode selection/hover outline) HAS NO FUNCTION OF ITS OWN.
    // It occupied the same slot as the Play-only game-HUD composite of phase 11
    // -- the two are mutually exclusive by mode -- and both had to run after the
    // scene render and before the editor ImGui pass. Both are satisfied inside
    // phase 10: the picker and the selection outline are NODES in the
    // viewport context's own frame
    // (PickNode/OutlineNode) armed through FrameDesc::pickOutline/pickables/
    // selectedIds/hoverPixel, and that frame is declared, compiled and executed
    // by phase 10's single RenderFrameOffscreen call. ArmGraphViewportFrame
    // (above) carries phase 12's gate VERBATIM -- Edit mode, and a selection
    // or a cursor in the viewport -- and maps m_selection through the k+1 id
    // assignment CollectPickables defines.

    // Phase 13: the editor's own document upkeep. The compile pump that used to
    // live here -- Poll/Drain, the scene sweep, the table publish and the clock
    // advance -- moved into SceneRenderResolver::Refresh (phase 9) when
    // resolution was lifted into the engine so both hosts share it. Open
    // documents still get first refusal on every drained result through the
    // consumeFirst hook installed in EditorApp::Init, so the process still has
    // exactly ONE drain site. What stays is what only an editor has: the
    // external-edit watcher and the per-document tick (each document renders
    // its own preview into its own offscreen NriGraphContext).
    void EditorApp::PumpEditorDocuments()
    {
        // FIRST, and before anything can close another document: destroy the
        // preview vehicles that LAST frame's closes handed over. By now the
        // chrome frame whose draw lists named them has
        // been recorded AND submitted, which is the whole condition the
        // deferral exists to satisfy.
        DrainRetiredDocPreviews();
        PollAssetWatch();   // external .arcmat/texture/.meta edits (~1 Hz mtime sweep)
        // F2b Task 12: drains the background cook queue's finished passes --
        // same safe window (strictly after this frame's render, strictly
        // before the next one's) InvalidateMesh's own doc comment already
        // establishes, since a completed cook's invalidation touches the
        // Assets facade and the viewport graph's GPU-adjacent caches.
        PollCookQueue();
        // Asset-manager Task 8: at most ONE 64px material thumbnail per frame.
        // It sits HERE, in the same safe window PollAssetWatch and
        // PollCookQueue already occupy -- strictly after this frame's render,
        // strictly before the next one's -- because a harvest renders a whole
        // offscreen frame and then ReadCapture idles the device, neither of
        // which may land inside the render phases above. Pump() returns
        // without touching a single device object when nothing is queued, so
        // an idle editor with every thumbnail already harvested pays nothing
        // for this line (see the early-out at the top of Pump).
        if (m_materialThumbs)
            m_materialThumbs->Pump(m_editorClock);
        m_documents.TickAll(m_gameUi.frameDt);
    }

    // Phase 14: the editor ImGui pass -- dockspace, menus, dialogs, panels.
    // Everything it draws samples the viewport texture the render phases above
    // already produced. The Ctrl+N/O/S edges raised in the input phase are
    // folded into this frame's MenuRequests here, so keybind and menu item
    // cannot drift apart.
    void EditorApp::DrawEditorUi(LoopState& ls, const FrameState& fs)
    {
        // ImGui: editor shell -- full-viewport dockspace + Sim toolbar + Console panel
        // + the Viewport panel showing the scene texture just rendered above.
        UpdateWindowTitle();   // project + scene name + unsaved marker
        // Sampled ONCE before the toolbar (s6.4): a change consumed later this
        // frame shows next frame, as the OS title already does.
        const Arcane::Editor::TitleParts titleParts = CurrentTitleParts();
        m_gpu->Imgui().BeginFrame();
        Arcane::Editor::MenuRequests menuReq;
        // Build -> Rebuild Game Module gating inputs: the menu greys the item
        // while playing (UE model), while a build runs, and for a project
        // with no gameModule (content-only, or none open).
        const Arcane::Project* menuProj = m_runtime->CurrentProject();
        const bool hasGameModule = menuProj && !menuProj->Manifest().gameModule.empty();
        if (menuProj) RefreshDevenv();   // build.ideExecutable is Live: one string compare unless it changed
        Arcane::Editor::BeginDockSpace(*m_undo, menuReq, m_scene.IsDirty(*m_undo),
                                       InPlayMode(),
                                       m_moduleBuild.Running(), hasGameModule,
                                       IdeMenuStateNow(),
                                       m_panelVis,
                                       m_selection.HasSelection(),
                                       Arcane::Editor::IsSceneRootOnly(m_runtime->Registry(), m_selection.Entities()),
                                       m_assetModel.selected.IsValid(),
                                       m_physicsOverlay,
                                       &m_recents.projects,
                                       &m_recents.scenes,
                                       m_documents.CloseTarget() != nullptr);
        if (menuReq.togglePhysicsOverlay) m_physicsOverlay = !m_physicsOverlay;
        // Play button's SeparateWindow branch: the toolbar only REPORTS the
        // click (same "panel reports, app performs" split as ViewportPanelResult);
        // the readiness checks live in SceneSession::Request's park condition
        // (dirty OR never-saved) and DoLaunchStandalone keeps only a loud
        // backstop before the spawn.
        // The Play->Edit flip this toolbar can perform is also what ends a
        // "Client + separate server process" session, and the panel has no
        // business owning a process handle -- so the flip is OBSERVED here
        // (panel reports, app performs, same split as the two requests below).
        const bool wasPlaying = InPlayMode();
        Arcane::Editor::ToolbarStatus stripStatus;
        stripStatus.title     = titleParts;
        stripStatus.scenePath = m_scene.Path().string();   // empty = never saved
        // s8.2: the Problems chip. Never under the harness (goldens stay machine-free).
        if (!Arcane::Editor::UnderVerifyHarness(m_config))
        {
            const std::size_t nErr  = m_consoleDiag.store.Count(Arcane::DiagSeverity::Error);
            const std::size_t nWarn = m_consoleDiag.store.Count(Arcane::DiagSeverity::Warning);
            if (const auto chip = Arcane::Editor::ProblemsChip(nErr, nWarn))
                stripStatus.problems = Arcane::Editor::StripChip{ chip->label,
                    nErr > 0 ? Arcane::Editor::Theme::kError : Arcane::Editor::Theme::kWarning, chip->tooltip };
        }
        // editor.play.launchMode (settings S6-32; NextWorld: the next Play):
        // the toolbar reads the published value, and its mode menu's choice
        // writes the cvar (User rung, archived).
        const Arcane::Editor::PlayLaunchMode publishedPlayMode =
            Arcane::Settings<Arcane::Editor::EditorPlaySettings>().launchMode;
        Arcane::Editor::PlayLaunchMode playMode = publishedPlayMode;
        const Arcane::Editor::ToolbarResult toolbar =
            Arcane::Editor::DrawSimTimeToolbar(m_play, m_runtime->Core(),
                                               m_plugin ? &*m_plugin : nullptr, playMode,
                                               ToolbarLogoTextureId(), stripStatus,
                                               [this]() { m_documents.FlushGestures(); });   // T1-B14's beforePlay (s3.3b)
        if (playMode != publishedPlayMode)
            Arcane::Editor::SetPlayLaunchMode(playMode);
        if (toolbar.launchStandalone)
        {
            // Mid-ImGui-pass site -> the deferral convention (SceneSession::Request's
            // comment): clean+saved acts next frame top; dirty/never-saved parks
            // behind the shared confirm modal.
            if (m_scene.Request(Arcane::Editor::SceneIntent::LaunchStandalone, {}, *m_undo))
                ls.sceneAction = { Arcane::Editor::SceneIntent::LaunchStandalone, {} };
        }
        // Unlike the standalone launch above, this one needs no SceneSession
        // gate: ArcaneServer boots the project MANIFEST's bootScene, so there
        // is no unsaved live document for it to get wrong (DoLaunchServer's
        // own declaration states the split).
        if (toolbar.launchServer)
            DoLaunchServer();
        if (toolbar.problemsChipClicked)
        {
            m_panelVis.visible[static_cast<std::size_t>(Arcane::Editor::PanelId::Problems)] = true;
            Arcane::Editor::SelectDockTab("Problems");
        }
        // Stop is the end of the whole session, including the child process.
        // A no-op on every topology that never spawned one.
        if (wasPlaying && !InPlayMode())
            m_serverProcess.Stop();
        if (!wasPlaying && InPlayMode())
            Arcane::Editor::SettingsWorldCreated();   // NextWorld settings applied with this world (spec s6.5)
        // The toolbar's Play/Stop click is the only mid-frame PlayMode flip point
        // (sec 1's rule): re-derive so this frame's consume blocks and panels see
        // the true state, not last frame's.
        //
        // A FLIP HERE ALSO REPLACES THE SCENE: both PlaySession::Play and
        // ::Stop go through Runtime::Snapshot/RestoreRegistry, which destroys
        // and replaces the registry OBJECT -- so a click-pick in flight names
        // entities of a registry that no longer exists. Noted here rather than
        // inside PlaySession because
        // this is the ONE site that can flip the mode, and because the same
        // line already exists to say exactly that.
        if (m_editBinding.editMode == InPlayMode())
            NoteSceneReplaced();
        m_editBinding.editMode = !InPlayMode();
        // ALWAYS consume the flag: on a Reset Layout frame a pending flag
        // would otherwise survive into the next frame and split the freshly
        // built default layout's browser node a second time.
        //
        // Ordering (inspector filters spec s6): on a FIRST run there is no
        // ini, so TakeLegacyLayoutUpgrade() is false and builtDefault applies
        // the defaults. If a loaded ini had no dock node (the build fires AND
        // the legacy flag was set), UpgradeLegacyInspectorLayout runs first
        // and ApplyDefaultInspectorLayout then overrides it to exactly {0, 1}
        // -- the build wins, which is correct: the default layout just docked
        // instance 1, not the upgrade's id.
        const bool pendingLegacy = m_inspectorHost.TakeLegacyLayoutUpgrade();
        menuReq.resetLayout |= std::exchange(m_layoutPage.resetRequested, false);
        const bool legacy = pendingLegacy && !menuReq.resetLayout;
        const int legacyAssetsId = legacy ? m_inspectorHost.UpgradeLegacyInspectorLayout() : -1;
        const Arcane::Editor::DockSpaceResult dock = Arcane::Editor::EndDockSpace(menuReq.resetLayout, legacyAssetsId);
        if (dock.builtDefault)
        {
            m_inspectorHost.ApplyDefaultInspectorLayout();   // first run and Window > Reset Layout: exactly {0, 1}
            // The instances it removed take their grids with them, as the
            // closed-X loop after DrawInspectorWindows does.
            std::erase_if(m_inspectorWindows.grids, [this](const auto& kv)
                          { return m_inspectorHost.Find(kv.first) == nullptr; });
        }
        if (menuReq.resetLayout)
            m_panelVis = Arcane::Editor::ParseOpenPanels(Arcane::Editor::cvar_layoutOpenPanelsAtStart.Get());   // reset shows the configured panels ("*" = all)

        ConsumeMenuRequests(menuReq, fs, ls);

        // Asset-manager redesign, Plan 1 Task 5: the invariant later tasks
        // rely on -- the model is current before ANY panel draw, whether or
        // not any of the three panels below is even visible this frame (a
        // consumer must never observe a stale rebuild just because the asset
        // windows happened to be docked shut). Cheap when clean
        // (RebuildIfDirty's own doc comment): no registry walk, no provider
        // calls, unless something actually marked it dirty since the last
        // frame. Panel-split Task 7: ONE rebuild, still, ahead of all THREE
        // panels -- the invariant is now load-bearing for three windows
        // instead of three lenses of one.
        const Arcane::Project* proj = m_runtime->CurrentProject();
        m_assetModel.RebuildIfDirty(proj ? &proj->Registry() : nullptr, m_assetPanelProviders);
        // T5 s7.6: a rebuild (a rename/move landed, among others) refreshes the
        // Inspector's history labels and pin names -- once per rebuild.
        if (m_assetModel.entriesStamp != m_labelsAtEntriesStamp) { m_labelsAtEntriesStamp = m_assetModel.entriesStamp; m_inspectorHost.RefreshLabels(); }

        // Plan 1 Task 7's AssetServices seam, re-shaped into Task 9's
        // AssetPanelServices -- two distinct struct types (different
        // consumers, per AssetServices's own header comment) that happen to
        // share the thumbnail callable. Built ONCE and handed to all three
        // panels (they read the identical seams), outside the visibility
        // gates because the gate bools it carries are about the OTHER panels,
        // not about the one being drawn.
        //
        // Plan 2 Task 7 adds the Status panel's two seams here rather than
        // to AssetServices: neither has any consumer outside these panels.
        // `cookDetailFor` is what keeps the panels free of any knowledge of
        // Arcane::Diagnostic or of this class's cook bookkeeping -- it
        // answers ONLY for a PERMANENT row (a real refusal), so a
        // transient ArtifactMissing row can never masquerade as a refusal
        // reason on a card; nullopt sends the card to its own bare
        // "cook refused" fallback.
        Arcane::Editor::AssetPanelServices assetPanelServices;
        assetPanelServices.resolveAssetThumb = m_assetServices.resolveAssetThumb;
        assetPanelServices.cookDetailFor =
            [this](const Arcane::Guid& g) -> std::optional<std::string>
            {
                const auto it = m_cookDiagnostics.find(g);
                if (it == m_cookDiagnostics.end() || !it->second.permanent)
                    return std::nullopt;
                return it->second.diagnostic.detail.empty() ? it->second.diagnostic.message
                                                            : it->second.diagnostic.detail;
            };
        // Borrowed non-owning for the draw only (Task 8's feed reads it);
        // this log outlives every frame and is Clear()ed, never destroyed,
        // on a project switch.
        assetPanelServices.activity = &m_assetActivity;
        // Panel-split spec s7.3: the four focus-if-open gates (R1) -- filled
        // at the same site every other services seam above is. As of Task 7
        // all four are independent reads of four real panel ids; the first
        // three used to alias PanelId::Assets, when one window housed all
        // three lenses and they could only ever read identically.
        assetPanelServices.browserOpen  = m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetBrowser);
        assetPanelServices.graphOpen    = m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetGraph);
        assetPanelServices.statusOpen   = m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetStatus);
        assetPanelServices.problemsOpen = m_panelVis.IsVisible(Arcane::Editor::PanelId::Problems);
        // T5 s7.1/s7.6: the file-op verbs' disabled reasons -- the gates, then
        // a dry-run PlanAssetOp, memoized per request until the model rebuilds
        // or the gate reason changes. RunAssetOp re-plans from fresh facts.
        // T5 s7.5: Delete's open-scene and boot-scene refusals change with no
        // model rebuild, so those two guids join the memo's invalidation key.
        assetPanelServices.fileOpRefusal = [this](const Arcane::Editor::AssetOpRequest& r) -> std::string
        {
            const std::string gate = AssetOpGateReason();
            const std::string memoGate = gate + "|" + m_scene.Id().ToString() + "|"
                + Arcane::Editor::BootSceneGuid(m_runtime ? m_runtime->CurrentProject() : nullptr).ToString();
            if (m_fileOpRefusalMemoStamp != m_assetModel.entriesStamp || m_fileOpRefusalMemoGate != memoGate)
            { m_fileOpRefusalMemo.clear(); m_fileOpRefusalMemoStamp = m_assetModel.entriesStamp; m_fileOpRefusalMemoGate = memoGate; }
            if (!gate.empty()) return gate;
            std::string key = std::to_string(static_cast<int>(r.kind)) + "|" + r.newStem + "|" + r.destFolder + (r.cascadeDerived ? "|1" : "|0");
            for (const Arcane::Guid& g : r.guids) key += "|" + g.ToString();
            if (const auto it = m_fileOpRefusalMemo.find(key); it != m_fileOpRefusalMemo.end()) return it->second;
            const auto plan = Arcane::Editor::PlanAssetOp(r, GatherAssetOpFacts(m_assetOpFacts, false));
            return m_fileOpRefusalMemo[key] = plan.refusals.empty() ? std::string{} : plan.refusals.front().reason;
        };
        // Inspector filters s6: the asset Inspector page draws in
        // DrawSelectionPanels, AFTER this function has returned, so it binds
        // a MEMBER copy -- a pointer to the local above would dangle. The
        // three panels below keep the local.
        m_assetPanelServices = assetPanelServices;

        // The three asset windows, each gated on its own PanelId exactly like
        // Console/Problems below. Each returns its own AssetPanelActions;
        // consumption is identical per value (spec s5), so the one handler
        // runs over each in draw order -- Browser, Graph, Status. That order
        // is not semantically load-bearing (spec s7.4): consumption happens
        // after all three draws, so a deep link raised this frame lands in
        // the next frame's draw whichever panel raised it.
        Arcane::Editor::AssetPanelActions browserActions, graphActions, statusActions;
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetBrowser))
            browserActions = Arcane::Editor::DrawAssetBrowserPanel(
                m_assetBrowserUi, m_assetModel, proj, m_documents, assetPanelServices,
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::AssetBrowser));
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetGraph))
            graphActions = Arcane::Editor::DrawAssetGraphPanel(
                m_assetGraphUi, m_assetModel, proj, m_documents, assetPanelServices,
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::AssetGraph));
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetStatus))
            statusActions = Arcane::Editor::DrawAssetStatusPanel(
                m_assetModel, proj, m_documents, assetPanelServices,
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::AssetStatus));
        // T5 s7.10: read by NEXT frame's clipboard fold in ConsumeMenuRequests,
        // which runs before this draw (the DocumentHost::FocusedDoc one-frame
        // tolerance). A hidden Browser leaves the default false.
        m_browserOwnsEditKeys = browserActions.ownsEditKeys;
        ConsumeAssetPanelActions(browserActions, ls);
        ConsumeAssetPanelActions(graphActions, ls);
        ConsumeAssetPanelActions(statusActions, ls);
        // The asset page's clicks (Inspector filters s6): raised during LAST
        // frame's Inspector draw, performed here with the panels' own -- one
        // frame late, invisible, and the same handler as the three panels.
        ConsumeAssetPanelActions(m_assetPageActions, ls);
        m_assetPageActions = {};
        m_assetSource.Bind({ &m_assetModel, proj, &m_assetPanelServices, &m_assetPageActions });
        // T5 s7.6: the asset page's Rename modal (opened by requestRename in
        // ConsumeAssetPanelActions just above); a commit runs at once.
        if (const auto req = Arcane::Editor::DrawRenameAssetModal(m_renameModal, m_assetPanelServices))
            if (const auto plan = RunAssetOp(*req)) AfterAssetOp(*plan);
        // T5 s7.5: the Delete confirm modal (opened by requestDelete above).
        ConsumeDeleteConfirm();
        // T5 s7.8: Move to... and New Folder. A folder made from INSIDE Move
        // to... (its nested New Folder) is picked in Move's combo once the
        // MarkAllDirty rebuild AssetsChanged arms lists it. The top-level New
        // Folder (folder row, background) draws only while Move is shut, read
        // BEFORE Move draws, so the two instances never share a frame -- an
        // ungated top-level BeginPopupModal would close the nested one.
        if (proj)
        {
            const bool moveWasOpen = m_moveTo.open;
            const Arcane::Editor::MoveToResult moved = Arcane::Editor::DrawMoveToModal(m_moveTo, m_newFolder, m_assetModel, m_assetPanelServices, *proj);
            if (moved.move) if (const auto plan = RunAssetOp(*moved.move)) AfterAssetOp(*plan);
            if (moved.newFolder && RunAssetOp(*moved.newFolder) && m_moveTo.open)
                m_moveTo.selectAfterCreate = moved.newFolder->destFolder.empty() ? moved.newFolder->newStem
                                                                                  : moved.newFolder->destFolder + "/" + moved.newFolder->newStem;
            if (!moveWasOpen)
                if (const auto req = Arcane::Editor::DrawNewFolderModal(m_newFolder, *proj)) (void)RunAssetOp(*req);
        }

        if (const auto lineCap = static_cast<std::size_t>(Arcane::Settings<Arcane::Editor::EditorConsoleSettings>().displayLineCap);
            lineCap != m_consoleDiag.console.Capacity())
            m_consoleDiag.console.SetCapacity(lineCap);
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Console))
            Arcane::Editor::DrawConsolePanel(m_consoleDiag.console, m_consoleDiag.ui,
                Arcane::Editor::UnderVerifyHarness(m_config),
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::Console));

        // Problems panel: current diagnostic STATE (Console above is the
        // append-only log stream). A clicked row's locator is routed here,
        // mid-DrawEditorUi -- the same place browserActions.createSpriteFrom
        // above already opens documents synchronously; only project/scene
        // teardown needs the frame-boundary deferral this function's other
        // effects use.
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Problems))
            if (const std::optional<Arcane::DiagLocator> hit =
                    Arcane::Editor::DrawProblemsPanel(m_consoleDiag.store, m_consoleDiag.problemsUi,
                        MakeRouteFacts(), Arcane::Editor::UnderVerifyHarness(m_config),
                        m_panelVis.OpenFlag(Arcane::Editor::PanelId::Problems)))
                RouteLocator(*hit);

        // Settings arc S3: editor.settings.openAtBoot (automation) is read once,
        // on the first UI frame -- --set has applied by then.
        if (!m_settingsBootOpenConsumed)
        {
            m_settingsBootOpenConsumed = true;
            const Arcane::Editor::SettingsBootOpen boot = Arcane::Editor::ConsumeSettingsOpenAtBoot();
            m_preferencesOpen = m_preferencesOpen || boot.preferences;
            m_projectSettingsOpen = m_projectSettingsOpen || boot.project;
        }
        Arcane::Editor::ConsumeSettingsPathPick(m_settingsPathCvar, m_dialogs.settingsPath);
        // Always called: a closed window is a no-op that flushes its archive on its close frame.
        Arcane::Editor::DrawEditorPreferences(&m_preferencesOpen);
        Arcane::Editor::EndListenIfPageHidden(Arcane::Editor::EditorActions::Get(), m_shortcutsPage, ImGui::GetFrameCount());
        Arcane::Editor::DrawProjectSettings(&m_projectSettingsOpen);

        // --open-asset: keep re-requesting focus for the scripted document
        // over its first frames (the member's comment says why); OpenPath on
        // an open document is focus-not-reopen, so this only re-arms focus.
        if (m_scriptedOpenFocusFrames > 0)
        {
            --m_scriptedOpenFocusFrames;
            if (const auto guid = Arcane::Guid::FromString(m_config.openAsset))
                (void)OpenAssetDocument(*guid);
        }
        // Start page (spec 2026-09-30 s8.4): drawn after this frame's menu
        // requests were consumed, so its own requests take the one launch site.
        {
            Arcane::Editor::MenuRequests startReq;
            DrawStartPage(startReq);
            LaunchProjectOpenRequests(startReq);
        }
        // New documents tab into the Viewport's node (captured last frame).
        m_documents.DrawAll(m_viewportDockId);
        // Input-editor spec s2.6: a saved input document pushed a republish
        // request from Save(); apply it here, at the frame boundary, whether or
        // not the document still exists (save-and-close destroys it inside
        // DrawAll -- DocumentHost.cpp:208 -> :117-121). Only the designated
        // asset is ever enqueued (EditorApp.cpp's .arcinput factory), so the
        // one slot holds that asset's last save this frame: its disk state.
        if (m_pendingInputRepublish)
        {
            auto req = std::move(*m_pendingInputRepublish);
            m_pendingInputRepublish.reset();
            RepublishGameInput(req.first, req.second);
        }
    }

    bool EditorApp::IsDesignatedInputAsset(const Arcane::Guid& asset)
    {
        const auto* project = m_runtime->CurrentProject();
        if (!project) return false;
        const auto designated = Arcane::Guid::FromString(project->Manifest().inputActions);
        return designated && *designated == asset;
    }

    void EditorApp::RepublishGameInput(const Arcane::Guid& asset, const Arcane::InputActionAsset& parsed)
    {
        if (!IsDesignatedInputAsset(asset)) return;   // not the project's gameplay input: nothing to republish
        const auto* project = m_runtime->CurrentProject();
        const auto projectId = Arcane::Guid::FromString(project->Manifest().guid);
        if (!projectId) return;
        // Same project, same LocalInputUser: the re-Configure branch (Task 5,
        // LocalInputUser::Configure) replays the pushed map stack, the control
        // scheme and the dirty overrides, and primes the new evaluator with the
        // last snapshot so a control held across the save does not re-fire.
        if (!m_runtime->ConfigureGameInput(parsed, *projectId))
            ARC_WARN("input: saved asset {} could not be compiled; the running session keeps the previous definition", asset.ToString());
    }

    void EditorApp::ConsumeMenuRequests(Arcane::Editor::MenuRequests& menuReq,
                                        const FrameState& fs, LoopState& ls)
    {
        if (menuReq.showProjectSettings) m_projectSettingsOpen = true;
        if (menuReq.showPreferences) m_preferencesOpen = true;
        // The Restart bar is enabled only when Play is stopped and edits are
        // saved. Exit after capturing the project; Shutdown relaunches it.
        if (m_restartRequested)
        {
            m_restartRequested = false;
            if (const Arcane::Project* proj = m_runtime->CurrentProject())
            {
                m_relaunchRoot = proj->Root();
                ls.sceneAction = { Arcane::Editor::SceneIntent::Exit, {} };
            }
        }
        // Window > New Inspector -- reopen before create (UE's summon-details
        // rule: reuse an open view, else the first CLOSED slot; never a copy
        // beside a closed one). Instance 0 is the standing follower (spec
        // s3.3) and its visibility is the Window > Inspector checkbox.
        // Selection events never open an Inspector; only this path reopens.
        if (menuReq.newInspector)
        {
            bool* primary = m_panelVis.OpenFlag(Arcane::Editor::PanelId::Inspector);
            if (primary && !*primary) *primary = true;       // hidden follower: bring it back, no new instance
            else (void)m_inspectorHost.AddInstance();        // visible: another instance with its own pin (-1 = pool full, no-op)
        }
        LaunchProjectOpenRequests(menuReq);
        // A picked recent scene lands in the SAME slot the Open Scene dialog's
        // callback fills, so it flows through ConsumeSceneDialogResults and
        // inherits the unsaved-scene guard -- a second open path would have to
        // re-earn it.
        if (!menuReq.openRecentScenePath.empty())
            m_dialogs.sceneOpen.Stash(m_dialogs.sceneOpen.Arm(), menuReq.openRecentScenePath);
        // Refresh on the RISING EDGE of the File menu only. That is what lets a
        // project opened in the Hub while this editor runs appear without a
        // restart, without paying a file read plus a stat per project on every
        // frame of a session where the menu is never touched.
        if (menuReq.fileMenuOpen && !m_recents.fileMenuWasOpen)
            m_recents.RefreshAll(m_runtime->CurrentProject());
        m_recents.fileMenuWasOpen = menuReq.fileMenuOpen;
        // Build -> Rebuild Game Module: spawn the worker right here (no
        // dialog, no teardown -- nothing about starting a build needs the
        // frame-boundary deferral the scene/project requests above use). The
        // finish-side effects all live in PollModuleBuild.
        if (menuReq.rebuildModule)
            StartModuleRebuild();
        // Build -> Open Visual Studio: same immediacy as the rebuild above --
        // nothing about finding/launching an IDE needs the frame-boundary
        // deferral the scene/project requests use. Synchronous for the
        // premake head (sub-second) and the COM probe; the launch itself is
        // fire-and-forget.
        if (menuReq.openIde)
            OpenInIde({});
#if !defined(ARC_BUILD_DIST)
        // Build -> Diagnostics -> Crash GPU: fired RIGHT HERE, mid-ImGui-pass,
        // rather than deferred to a frame boundary the way the scene/project
        // requests above are. It needs no teardown and no dialog, and this
        // point in the frame is safe for it: no graph frame has been declared
        // yet, so FireDeliberateGpuFault's own one-off command buffer cannot
        // collide with anything already recording. Deferring would also make
        // the capture LESS representative rather than more: a device lost
        // while the host is mid-frame is exactly the shape this arc exists
        // to survive.
        if (menuReq.crashGpu)
            FireDeliberateGpuFault();
#endif
        // Edit -> selection ops. Pure selection state -- no undo step (UE
        // does not undo selection either).
        if (menuReq.deselectAll)
            m_selection.Clear();
        if (menuReq.selectAll || menuReq.invertSelection)
        {
            const std::vector<Astra::Entity> all =
                Arcane::Editor::CollectSceneEntities(m_runtime->Registry());
            const std::vector<Astra::Entity> next = menuReq.selectAll
                ? all
                : Arcane::Editor::InvertSelectionSet(all, m_selection);
            m_selection.Clear();
            m_selection.AddRange(next, next.empty() ? Astra::Entity::Invalid()
                                                    : next.back());
        }
        // Edit -> Rename / Delete: the Outliner's own F2/Del code paths
        // (promoted in EditorPanels.hpp) -- menu and keybind cannot drift.
        if (menuReq.renameSelected && m_selection.HasSelection())
        {
            if (const Arcane::Identity* info =
                    std::as_const(m_runtime->Registry()).GetComponent<Arcane::Identity>(m_selection.Primary()))
            {
                // The rename box lives in the Outliner: un-hide it and pull its
                // tab forward so the edit box appears where the user can see it,
                // instead of latching a rename that ambushes them whenever the
                // panel next shows (final-review Important 2).
                m_panelVis.visible[static_cast<std::size_t>(Arcane::Editor::PanelId::Outliner)] = true;
                Arcane::Editor::SelectDockTab("Outliner");
                Arcane::Editor::BeginRename(m_outliner, m_selection.Primary(), info->name);
            }
        }
        if (menuReq.deleteSelected)
            Arcane::Editor::DeleteSelection(m_runtime->Registry(), m_selection,
                                            *m_undo, m_editBinding);

        // Edit -> clipboard shortcuts (Ctrl+X/C/V/D, raised in the input phase
        // above). Folded in HERE, before the clipboard consume block below reads
        // menuReq -- same fold-in shape as the Ctrl+N/O/S scene shortcuts fold
        // further down, just earlier in the frame so the request this edge
        // raises cannot lag a frame behind its own keypress. T5 s7.10: folded
        // only while the Asset Browser does NOT own the keys (last frame's
        // answer -- the Browser draws after this), so Ctrl+D in the Browser
        // no longer duplicates the selected entity.
        Arcane::Editor::FoldEntityClipboardShortcuts(menuReq, { fs.scCut, fs.scCopy, fs.scPaste, fs.scDuplicate }, m_browserOwnsEditKeys);
        // Edit -> clipboard (spec II.B), delegated to the shared functions
        // promoted in EditorPanels.cpp/.hpp -- the menu-bar consume, the
        // keybinds (folded in above), and the Outliner's context menus all
        // route through the same four, so they cannot drift.
        if (menuReq.copySelection)
            Arcane::Editor::CopySelectionToClipboard(m_runtime->Registry(), m_selection);
        if (menuReq.cutSelection)
            Arcane::Editor::CutSelection(m_runtime->Registry(), m_selection, *m_undo, m_editBinding);
        if (menuReq.paste)
            Arcane::Editor::PasteFromClipboard(m_runtime->Registry(), m_selection, *m_undo, m_editBinding);
        if (menuReq.duplicateSelection)
            Arcane::Editor::DuplicateSelection(m_runtime->Registry(), m_selection, *m_undo, m_editBinding);

        // File -> Save All: scene first (never-saved routes through the Save
        // As dialog, exactly like Save), then every dirty document in place.
        if (menuReq.saveAll)
        {
            if (m_scene.IsDirty(*m_undo))
            {
                if (!m_scene.Path().empty()) DoSaveScene(m_scene.Path());
                else                         ShowSceneSaveDialog();
            }
            if (const std::size_t failed = m_documents.SaveAllDirty())
                ARC_ERROR("Save All: {} document save(s) failed (see Console)", failed);
        }
        // File -> Exit: the SAME SceneIntent::Exit flow the OS quit uses --
        // dirty scene parks behind the confirm modal; a clean exit defers to
        // the top of the next frame like every other scene action.
        if (menuReq.exitEditor &&
            m_scene.Request(Arcane::Editor::SceneIntent::Exit, {}, *m_undo))
        {
            ls.sceneAction = { Arcane::Editor::SceneIntent::Exit, {} };
        }

        // Assets -> Show in Explorer / Copy Path / Open as text, on the panel's tracked
        // selection (Task 10: repointed from the frozen m_assetBrowser.selected
        // to m_assetModel.selected -- see Task 9's carry-over note).
        if ((menuReq.showInExplorer || menuReq.copyAssetPath || menuReq.openAssetAsText) &&
            m_assetModel.selected.IsValid())
        {
            AssetPathAction(m_runtime->CurrentProject(), m_assetModel.selected,
                            menuReq.showInExplorer, menuReq.copyAssetPath, menuReq.openAssetAsText);
        }

        // Assets -> Create -> <kind>...  TASK 12: the two ShowSaveFileDialog
        // launches that used to live here (newMaterial / newMeshMaterial) are
        // RETIRED -- the menu is now a thin producer of a CreateAssetRequest
        // like every other entry point, and BeginCreateAsset is the one place
        // a create dialog opens (spec s7's invariant).
        if (menuReq.requestCreateKind >= 0 &&
            menuReq.requestCreateKind < Arcane::Editor::kCreateAssetKindCount)
        {
            Arcane::Editor::CreateAssetRequest request;
            request.kind = static_cast<Arcane::Editor::CreateAssetKind>(menuReq.requestCreateKind);
            // Create -> Mesh -> <primitive> (F4 plan 1 Task 11): the preset
            // rides the SAME request; -1 when the entry carried none.
            request.prefillMeshSource = menuReq.requestMeshSource;
            // The Assets menu acts on the asset selection (Show in Explorer
            // and Copy Path above do too), so its "Material Instance..." and
            // "Sprite..." prefill from a selected material / texture exactly
            // as the Asset Browser's `+ Create` does (T3-D4, T3-D5).
            if (m_assetModel.selected.IsValid())
                request.prefillParent = Arcane::Editor::CreatePrefillFor(
                    request.kind, m_assetModel.Find(m_assetModel.selected));
            BeginCreateAsset(request);
        }
        if (menuReq.openMaterial)
        {
            // The material OPEN path is NOT a creation path -- it stays a real
            // OS file picker, starting in the project's Content/ (no project =
            // OS default).
            const Arcane::Project* proj = m_runtime->CurrentProject();
            const std::string contentDir =
                proj ? (proj->Root() / "Content").string() : std::string();
            m_gpu->Win().ShowOpenFileDialog(&PathPickedThunk,
                new PathDialogRequest{ &m_dialogs.materialOpen, m_dialogs.materialOpen.Arm() },
                "Arcane Material", "arcmat",
                contentDir.empty() ? nullptr : contentDir.c_str());
        }

        // Scene menu items + their Ctrl+N/O/S shortcuts (raised in the input
        // phase above). Folded together here so both routes hit the same guard.
        menuReq.newScene  |= fs.scNewScene;
        menuReq.openScene |= fs.scOpenScene;
        // Ctrl+S means "save what I am looking at". An open asset document binds
        // its own Ctrl+S with ImGui::Shortcut, which routes to the focused
        // window by itself -- but THIS keybind is raw scancodes off the input
        // snapshot and knows nothing about that routing, so without the check
        // one keypress inside a material or sprite document would save the
        // document AND the scene. Asking the host who is focused is what keeps
        // exactly one of them firing.
        //
        // The same stand-down applies to an Inspector instance showing a
        // DOCUMENT's page (the document's Ctrl+S then routes through that
        // instance, InspectorWindows.cpp). An instance on the scene page
        // leaves the scene keybind alone.
        //
        // Only the SHORTCUT routes. The File menu's Save Scene item folds in
        // below unconditionally: an item that names the scene saves the scene,
        // whatever happens to hold focus.
        const bool docOwnsSave =
            fs.scSaveScene && (m_documents.FocusedDoc() != nullptr ||
                               InspectorSaveTarget(m_inspectorFocusedSource) != nullptr);
        menuReq.saveScene |= (fs.scSaveScene && !docOwnsSave);

        // Both the shortcut and menu use the normal document close flow.
        menuReq.closeDocument |= fs.scCloseDocument;
        if (menuReq.closeDocument)
            if (Arcane::Editor::EditorDocument* doc = m_documents.CloseTarget())
                m_documents.RequestClose(doc);

        if (menuReq.newScene &&
            m_scene.Request(Arcane::Editor::SceneIntent::NewScene, {}, *m_undo))
        {
            // Deferred to the top of the next frame for the same reason the
            // confirm modal's answer is -- see sceneAction.
            ls.sceneAction = { Arcane::Editor::SceneIntent::NewScene, {} };
        }
        if (menuReq.openScene)
        {
            const std::string dir = SceneDialogDir();
            m_gpu->Win().ShowOpenFileDialog(&PathPickedThunk,
                new PathDialogRequest{ &m_dialogs.sceneOpen, m_dialogs.sceneOpen.Arm() },
                "Arcane Scene", "arcscene",
                dir.empty() ? nullptr : dir.c_str());
        }
        // Save on a never-saved scene IS Save As -- otherwise the item would look
        // enabled and do nothing.
        if (menuReq.saveScene && !m_scene.Path().empty())
            DoSaveScene(m_scene.Path());
        if (menuReq.saveSceneAs || (menuReq.saveScene && m_scene.Path().empty()))
            ShowSceneSaveDialog();
    }

    void EditorApp::LaunchProjectOpenRequests(const Arcane::Editor::MenuRequests& req)
    {
        if (req.openProject)
        {
            // Start beside the most recent project; null = the OS default.
            const std::string start = Arcane::Editor::DialogStartDir(m_recents.projects);
            m_gpu->Win().ShowOpenFileDialog(&PathPickedThunk,
                new PathDialogRequest{ &m_dialogs.projectOpen, m_dialogs.projectOpen.Arm() },
                "Arcane Project", "arcproj", start.empty() ? nullptr : start.c_str());
        }
        // Same slot, same thunk: FolderPickedCallback and FilePickedCallback share
        // a signature (Window.hpp:93, 103).
        if (req.openProjectFolder)
            m_gpu->Win().ShowOpenFolderDialog(&PathPickedThunk,
                new PathDialogRequest{ &m_dialogs.projectOpen, m_dialogs.projectOpen.Arm() });
        // Open Recent lands in the SAME slot the dialogs' callback fills, so it
        // flows through ConsumeProjectDialogResult and inherits every guard (the
        // unsaved-scene confirm, the rival-editor lock, the ABI gate, the failure modal).
        if (!req.openRecentPath.empty())
            m_dialogs.projectOpen.Stash(m_dialogs.projectOpen.Arm(), req.openRecentPath);
    }

    void EditorApp::ProjectPageThunk(void* user)
    {
        static_cast<EditorApp*>(user)->DrawProjectPage();
    }

    void EditorApp::DrawProjectPage()
    {
        Arcane::Editor::ProjectSettingsRequests requests;
        Arcane::Editor::DrawProjectIdentityPage(m_runtime->CurrentProject(), Arcane::Editor::CurrentSettingsGrid(),
                                                &m_assetRefServices, requests);
        ApplyProjectSettingsRequests(requests);
    }

    void EditorApp::ApplyProjectSettingsRequests(const Arcane::Editor::ProjectSettingsRequests& settings)
    {
        if (settings.create)
        {
            Arcane::Editor::CreateAssetRequest request;
            request.kind = Arcane::Editor::CreateAssetKind::InputActions;
            BeginCreateAsset(request);
        }
        if (settings.open)
        {
            if (const auto* project = m_runtime->CurrentProject())
            {
                const auto id = Arcane::Guid::FromString(project->Manifest().inputActions);
                if (id && id->IsValid())
                    if (const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(*id)))
                        m_documents.OpenPath(*path);
            }
        }
        if (settings.clear || settings.select)
        {
            const auto id = settings.clear ? Arcane::Guid::Nil() : settings.selection;
            if (m_runtime->SetProjectInputActionsAsset(id))
            {
                m_runtime->GameInput().Clear();
                if (id.IsValid())
                {
                    if (const auto* project = m_runtime->CurrentProject())
                    {
                        if (const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(id)))
                        {
                            std::ifstream file(*path, std::ios::binary);
                            const std::string raw(std::istreambuf_iterator<char>{file}, {});
                            const auto json = nlohmann::json::parse(raw, nullptr, false);
                            const auto asset = Arcane::InputActionAsset::FromJson(json);
                            const auto projectId = Arcane::Guid::FromString(project->Manifest().guid);
                            if (asset && projectId)
                                (void)m_runtime->ConfigureGameInput(*asset, *projectId);
                        }
                    }
                }
            }
        }
        if (settings.bootScene && !m_runtime->SetProjectBootScene(*settings.bootScene))
            ARC_WARN("Project Settings: the boot scene could not be written to the .arcproj (see the line above)");
    }

    void EditorApp::DrawStartPage(Arcane::Editor::MenuRequests& req)
    {
        // Derived every frame, never latched: a switch that fails after
        // teardown brings the page back; a successful one hides it.
        const bool visible = m_runtime->CurrentProject() == nullptr;
        // A focus lands only once the page is docked and no popup (a boot
        // error modal) holds focus; until then it waits (StepStartPageFocus).
        const bool canFocus = m_viewportDockId != 0 &&
                              !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const Arcane::Editor::StartPageStep step =
            Arcane::Editor::StepStartPageFocus(m_startPageFocus, visible, canFocus);
        if (!visible) return;
        if (step.appearing)
            m_recents.RefreshAll(nullptr);   // read-only: the page never writes the shared recents
        if (step.focusNow)
            ImGui::SetNextWindowFocus();     // the first frame it can land, so the Start tab is in front
        if (m_viewportDockId != 0)
            ImGui::SetNextWindowDockID(static_cast<ImGuiID>(m_viewportDockId), ImGuiCond_Always);
        if (ImGui::Begin("Start###startpage", nullptr, ImGuiWindowFlags_NoSavedSettings))   // no Close X (9.27 #14)
        {
            const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const Arcane::Editor::StartPageModel page = Arcane::Editor::BuildStartPage(m_recents.projects, now);
            const float avail = ImGui::GetContentRegionAvail().x;
            const float width = std::min(Ui::Px(640.0f), avail);   // start page column (settings S6-28: px follow editor.ui.scale)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - width) * 0.5f));
            ImGui::BeginChild("##startcol", ImVec2(width, 0.0f));
            // ---- section 1: heading + actions ---------------------------------
            ImGui::Dummy(ImVec2(0.0f, Ui::Px(24.0f)));
            ImGui::TextUnformatted("No project open");
            ImGui::Spacing();
            if (ImGui::Button(ICON_LC_FOLDER_OPEN " Open Project...")) req.openProject = true;
            ImGui::SameLine();
            if (ImGui::Button(ICON_LC_FOLDER " Open Folder...")) req.openProjectFolder = true;
            ImGui::Dummy(ImVec2(0.0f, Ui::Px(16.0f)));
            // ---- (crash-window plan 3's "Recover" section slots in HERE) ------
            // ---- section 2: recent projects -------------------------------------
            ImGui::TextDisabled("Recent projects");
            ImGui::Separator();
            const ImGuiStyle& style = ImGui::GetStyle();
            const float lineH = ImGui::GetTextLineHeight();
            const float rowH = lineH * 2.0f + style.FramePadding.y * 3.0f;
            for (std::size_t i = 0; i < page.rows.size(); ++i)
            {
                const Arcane::Editor::StartPageRow& row = page.rows[i];
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable("##recent", false, ImGuiSelectableFlags_None, ImVec2(0.0f, rowH)))
                    req.openRecentPath = row.path;   // the Open Recent slot
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", row.path.c_str());
                const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImU32 dim = ImGui::ColorConvertFloat4ToU32(Theme::kTextDim);
                dl->AddText(ImVec2(lo.x + style.FramePadding.x, lo.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), row.name.c_str());
                dl->AddText(ImVec2(hi.x - style.FramePadding.x - ImGui::CalcTextSize(row.opened.c_str()).x, lo.y + style.FramePadding.y), dim, row.opened.c_str());
                dl->PushClipRect(lo, hi, true);
                dl->AddText(ImVec2(lo.x + style.FramePadding.x, lo.y + style.FramePadding.y * 2.0f + lineH), dim, row.path.c_str());
                dl->PopClipRect();
                ImGui::PopID();
            }
            if (!page.hiddenLine.empty()) ImGui::TextDisabled("%s", page.hiddenLine.c_str());
            else if (page.rows.empty())   ImGui::TextDisabled("No recent projects");
            ImGui::EndChild();
        }
        ImGui::End();
    }

    void EditorApp::ConsumeAssetPanelActions(const Arcane::Editor::AssetPanelActions& panelActions,
                                             LoopState& ls)
    {
        // Unified create (Task 12). Both of these used to be their own flows;
        // both are now the SAME request into the SAME entry, which is what
        // spec s7's invariant ("no creation path may bypass
        // CreateAssetRequest") actually asserts.
        if (panelActions.requestCreateKind >= 0 &&
            panelActions.requestCreateKind < Arcane::Editor::kCreateAssetKindCount)
        {
            Arcane::Editor::CreateAssetRequest request;
            request.kind = static_cast<Arcane::Editor::CreateAssetKind>(panelActions.requestCreateKind);
            request.prefillParent = panelActions.createPrefillParent;
            // Create -> Mesh -> <primitive> (F4 plan 1 Task 11): the preset
            // rides the SAME request; -1 when the entry carried none.
            request.prefillMeshSource = panelActions.requestMeshSource;
            BeginCreateAsset(request);
        }
        // "New Instance..." (a material row's context menu, and the preview
        // pane's kind-specific button): the same request with the clicked
        // material PRE-FILLED as the parent. This replaces its own
        // ShowSaveFileDialog launch -- an instance mint that bypassed
        // CreateAssetRequest was exactly the second half of what the invariant
        // forbids.
        if (panelActions.createInstanceOf.IsValid())
        {
            BeginCreateAsset({ Arcane::Editor::CreateAssetKind::MaterialInstance,
                               panelActions.createInstanceOf });
        }
        if (panelActions.createSpriteFrom.IsValid())
        {
            // Browser-initiated mint OPENS the new sprite document (the user
            // right-clicked a specific texture asking for exactly this); the
            // Inspector's texture-drop auto-mint below does NOT -- that one is
            // a means to filling a field, not a request to edit the sprite.
            if (const Arcane::Guid minted =
                    MintOrReuseSpriteForTexture(panelActions.createSpriteFrom);
                minted.IsValid())
            {
                if (const Arcane::Project* proj = m_runtime->CurrentProject())
                    if (const auto p = proj->ResolveAsset(Arcane::AssetId::FromGuid(minted)))
                        m_documents.OpenPath(*p);
            }
        }
        // Asset-manager redesign spec s7: "the silent + Mesh toolbar button
        // dies; Mesh gets the [unified Create] dialog." Retired for good in
        // Task 13: AssetBrowserActions::createMesh (the dead old panel's own
        // flag) is deleted, and requestCreateKind==Mesh above already routes
        // through BeginCreateAsset -> ConsumeCreateResult's Mesh arm
        // (MintMeshAsset) -- there is no separate createMesh consumer here,
        // by design.
        if (!panelActions.openScene.empty())
        {
            // A scene double-clicked in the browser is not a document -- it
            // replaces the editing session, so it goes through the same
            // SceneSession guard as File -> Open Scene. Request() is pure
            // state and safe to call from here; the load itself is deferred
            // to sceneAction for the same reason menuReq's scene items above
            // are (this call site is mid-ImGui-pass, after BeginDockSpace).
            // A parked (dirty-scene) result is picked up by the "Unsaved
            // Scene" modal below, whose Save/Discard branches already set
            // sceneAction via TakePending().
            if (m_scene.Request(Arcane::Editor::SceneIntent::OpenScene,
                                panelActions.openScene, *m_undo))
                ls.sceneAction = { Arcane::Editor::SceneIntent::OpenScene,
                                   panelActions.openScene };
        }
        // Source/ in the Asset Browser, step 2: a Source row's Open goes to
        // the IDE, not a document (OpenAssetRow sets this instead of calling
        // DocumentHost). No session guard: opening a file in Visual Studio
        // touches nothing the editor owns.
        if (!panelActions.openInIde.empty())
            OpenInIde(panelActions.openInIde);
        if (panelActions.setBootScene.IsValid())
        {
            // No unsaved-changes guard: this only rewrites the project
            // manifest and does not touch the live registry or session.
            if (m_runtime->SetProjectBootScene(panelActions.setBootScene))
                ARC_INFO("Boot scene set to {}", panelActions.setBootScene.ToString());
            else
                m_modalErrors.Push("Scene Error", "Could not write the project's boot scene (see Console).");
        }
        // Row context menu parity for Show in Explorer / Copy Path (Part 3):
        // the SAME helper the menu-bar route above uses, on whichever row
        // the browser's own popup was just opened against.
        if (panelActions.showInExplorer.IsValid())
            AssetPathAction(m_runtime->CurrentProject(), panelActions.showInExplorer, true, false);
        if (panelActions.copyPath.IsValid())
            AssetPathAction(m_runtime->CurrentProject(), panelActions.copyPath, false, true);
        if (panelActions.openAsText.IsValid())
            AssetPathAction(m_runtime->CurrentProject(), panelActions.openAsText, false, false, true);
        // Spec s6's new context-menu entry: Copy Guid, straight to the
        // clipboard (no file resolution needed -- unlike copyPath/
        // showInExplorer, a guid needs no project lookup to be copyable).
        if (panelActions.copyGuid.IsValid())
            ImGui::SetClipboardText(panelActions.copyGuid.ToString().c_str());

        // ---- Cross-panel deep links (panel-split spec s7.1) ----------------
        // Three former direct `state.lens` writers: host-routed since Task 3,
        // and genuinely cross-WINDOW commands since Task 7. Each raise site
        // already gated itself on the matching AssetPanelServices bool before
        // writing its action field (R1/s7.3: focus if open, else disabled +
        // tooltipped); the IsVisible checks below are defense in depth, not a
        // second gate. Under R1 the one thing this consumer must never do is
        // OPEN a panel -- a closed target is a no-op, never an un-hide.
        // SelectDockTab alone still does real work: a
        // visible-but-buried-behind-a-sibling-tab window is not surfaced.
        //
        // `showStatus` (the digest chip, Browser/Graph -> Status) -- the tab
        // focus and nothing else. No selection or filter change; that is
        // today's semantics, carried over unchanged from when it flipped a
        // lens.
        if (panelActions.showStatus &&
            m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetStatus))
            Arcane::Editor::FocusDockTab("Asset Status");
        // `focusInGraph` (Status's scene card -> Graph) -- write the focus,
        // select, surface the tab. The old focus-BEFORE-select-BEFORE-lens
        // ordering dance dissolves with the lens (spec s7.1): both writes
        // land here, before any next-frame draw, and the Graph panel's own
        // rebuild trigger (graphBuiltStamp/graphBuiltFocus against the model)
        // does the rest on its next frame. Select still earns its line -- it
        // is what makes the graph CENTER on the scene rather than merely
        // contain it.
        if (panelActions.focusInGraph.IsValid() &&
            m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetGraph))
        {
            m_assetGraphUi.graphFocus = panelActions.focusInGraph;
            m_assetGraphUi.graphKindFilter.reset();
            m_assetModel.Select(panelActions.focusInGraph);
            Arcane::Editor::FocusDockTab("Asset Graph");
        }
        // `revealInBrowse` (Status's Unreferenced card -> Browser) -- the
        // shared helper (AssetPanelCommon.*) carries the exact Ruling-10
        // reveal sequence (clear both filters, force the folder ancestry and
        // any derived fold open, then select); this consumer's own job is
        // only the trailing tab focus the extracted helper deliberately does
        // not make.
        if (panelActions.revealInBrowse.IsValid() &&
            m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetBrowser))
        {
            Arcane::Editor::RevealAssetInBrowser(m_assetBrowserUi, m_assetModel,
                                                 panelActions.revealInBrowse);
            Arcane::Editor::FocusDockTab("Asset Browser");
        }
        // `openAsset` (an asset-reference cell's name double-click, spec
        // 2026-09-30 s4.2): opened HERE, a frame after the cell queued it,
        // exactly like a Browser double-click (OpenAssetRow: a scene through
        // openScene, a source file to the IDE). Its follow-ups are consumed at
        // once; OpenAssetRow never sets openAsset, so this recurses one level.
        if (panelActions.openAsset.IsValid())
            if (const Arcane::Editor::AssetPanelEntry* e = m_assetModel.Find(panelActions.openAsset))
            {
                Arcane::Editor::AssetPanelActions follow;
                Arcane::Editor::OpenAssetRow(*e, m_runtime->CurrentProject(), m_documents, follow);
                ConsumeAssetPanelActions(follow, ls);
            }
        // T5 s7.6/s7.7: a committed inline rename or a Duplicate (Browser key,
        // row menu, asset page) runs through the ONE front door, then the
        // post-op selection step; the page's pencil opens the Rename modal.
        if (panelActions.fileOp)
            if (const auto plan = RunAssetOp(*panelActions.fileOp)) AfterAssetOp(*plan);
        if (const auto* e = m_assetModel.Find(panelActions.requestRename))
        { m_renameModal = { true, e->guid, {}, true }; std::snprintf(m_renameModal.buf, sizeof(m_renameModal.buf), "%s", e->name.c_str()); }
        if (!panelActions.requestDelete.empty()) BeginAssetDelete(panelActions.requestDelete);   // T5 s7.5: Del, row menu, page trash
        if (!panelActions.requestMoveTo.empty()) m_moveTo = { true, true, panelActions.requestMoveTo, 0, {} };   // T5 s7.8: row menu
        if (panelActions.requestNewFolder) { m_newFolder = {}; m_newFolder.open = m_newFolder.justOpened = true; m_newFolder.parent = *panelActions.requestNewFolder; }   // folder row, background

        // ---- Status lens attention cards (asset-manager Plan 2 Task 7) -----
        // Recook, per the plan's Ruling 8: invalidate the artifact, ERASE this
        // guid's cook-diagnostic row, republish, poke the cook queue, and mark
        // the model dirty.
        //
        // Erasing the row is what makes the card flip Refused -> Queued
        // HONESTLY rather than cosmetically: with no row, IsCookPending
        // (EditorAppProject.cpp) re-derives the answer from the ARTIFACT
        // STORE on the source's CURRENT cook key -- so a source that has
        // never produced an artifact for today's key reads Queued, and one
        // that still cannot cook re-fails, OnCookCompleted/OnArtifactRefused
        // writes the row back, and the card returns. Nothing here fakes a
        // success: the only way this guid reaches Cooked is a real artifact
        // on disk for its current key.
        //
        // There is no per-guid cook API: CookQueue::NoteChanged() is
        // whole-project, coalescing and hash-decided, which is exactly right
        // here (the same call PollAssetWatch makes for a changed source).
        if (panelActions.recook.IsValid())
        {
            if (m_runtime)
                m_runtime->AssetsFacade().InvalidateArtifact(panelActions.recook);
            m_cookDiagnostics.erase(panelActions.recook);
            PublishCookDiagnostics();   // the Problems row clears with it
            if (m_cookQueue)
                m_cookQueue->NoteChanged();
            m_assetModel.MarkDirty(panelActions.recook);
            // Beside the dirty mark, never instead of it -- Task 5's rule for
            // every activity push. SourceChanged is the honest kind: the user
            // asked for the same thing a source edit asks for.
            m_assetActivity.Push({ std::chrono::steady_clock::now(), panelActions.recook,
                                   NameOfAsset(panelActions.recook),
                                   Arcane::Editor::AssetActivityKind::SourceChanged,
                                   "recook requested" });
        }
        // "Problems" SURFACES THE PANE and nothing more (Ruling 9 / spec s9.2
        // verbatim: "jumps to the pane"). No pre-filtering -- none is
        // specified, so none is invented. Panel-split spec s7.3/R1
        // (2026-09-09): the un-hide line is GONE -- nothing opens a panel
        // except the Window menu, and the raise site (DrawAttentionCard's
        // "Problems" button, AssetStatusPanel.cpp) now disables itself with a
        // tooltip when Problems is closed, so this consumer only ever runs
        // while the panel is already visible. SelectDockTab alone still does
        // real work: a visible-but-buried-behind-a-sibling-tab panel is not
        // surfaced, the same reason Edit -> Rename's own two-step
        // (ConsumeMenuRequests above) brings the Outliner forward -- Problems
        // just no longer gets the first of those two steps.
        if (panelActions.showProblems)
            Arcane::Editor::SelectDockTab("Problems");
    }

    // ---- Unified create (asset-manager redesign, Plan 1 Task 12) ------------
    // THE ONE ENTRY. Every producer's request lands here and nowhere else;
    // this function opens the dialog and does not create anything.
    void EditorApp::BeginCreateAsset(const Arcane::Editor::CreateAssetRequest& request)
    {
        const Arcane::Project* project = m_runtime->CurrentProject();
        if (!project)
        {
            // A created asset can only register + resolve by GUID inside a
            // project (Project.cpp:334-338). Refused LOUDLY rather than
            // opening a dialog whose Create could only ever fail.
            m_modalErrors.Push("Create Failed",
                               "Open a project first -- a new asset can only be created inside one.");
            return;
        }

        // Fresh state per request (a cancelled dialog must not leave a
        // half-typed name or a stale parent for the next one to inherit),
        // seeded from the request by the dialog's own unit, which is where
        // the prefill rules live and are tested (T3-D4).
        m_createDialog = Arcane::Editor::MakeCreateDialogState(request, m_assetModel, project->Root());
        // A C++ Class's Location combo defaults to the game module's OWN
        // directory (CppClassDefaultFolder(manifest.sourceDir)), not Source/
        // itself -- the source:// mount stays Source/ (plan ruling S2); only
        // the DEFAULT folder for a new class moves with the module layout.
        if (request.kind == Arcane::Editor::CreateAssetKind::CppClass)
            m_createDialog.request.cppDefaultFolder =
                Arcane::Editor::CppClassDefaultFolder(project->Manifest().sourceDir);
    }

    // THE ONE DISPATCHER. A completed dialog result -> the matching mint.
    void EditorApp::ConsumeCreateResult(const Arcane::Editor::CreateAssetResult& r)
    {
        // Spec s7 ("failures route through the existing ModalErrorQueue +
        // Problems") and s13's identical pairing. The modal is the immediate
        // notice and dies with its OK button; the Problems row is the trace
        // that outlives it. One helper so the three failure exits below cannot
        // drift apart on wording, code, or scope -- and so the row's message is
        // the SAME string the modal showed. See m_createDiagnostics'
        // member-rationale comment (EditorApp.hpp) for the "assets:create" key
        // ownership and why the accumulator is per-project.
        auto failCreate = [this](std::string message, std::string detail, std::string file)
        {
            m_modalErrors.Push("Create Failed", message);

            Arcane::Diagnostic d;
            d.severity = Arcane::DiagSeverity::Error;
            d.scope    = Arcane::DiagScope::Assets;
            d.code     = "assets.create.failed";
            d.message  = std::move(message);
            d.detail   = std::move(detail);
            // A File locator, routed by ClassifyLocator (s8.2): a row whose
            // path does not exist is plain text, an existing one opens or shows
            // in Explorer -- the build.module.failed row's precedent. There is
            // deliberately no Asset locator: the create FAILED, so no guid
            // exists to point at.
            if (!file.empty())
                d.locator = Arcane::DiagLocator::File(std::move(file));
            m_createDiagnostics.push_back(std::move(d));
            Arcane::Diagnostics::Publish("assets:create", m_createDiagnostics);
        };

        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj)
        {
            failCreate("The project closed before the asset was created.",
                       "The create dialog outlived the project it was opened for.",
                       /*file=*/"");
            return;
        }

        // Sprite's mint-or-reuse notice, "Open existing" (Task 13): every
        // OTHER field on `r` is meaningless here -- select + open the already-
        // registered sprite and stop, same "no bypass, just no mint" shape as
        // every other branch below going through this one dispatcher.
        if (r.openExisting.IsValid())
        {
            m_assetModel.Select(r.openExisting);
            OpenAssetDocument(r.openExisting);
            return;
        }

        // The Location combo always offers the kind's DEFAULT folder, whether
        // or not the project has one yet (BuildFolderChoices' own comment), so
        // the directory is made on demand here rather than the create failing
        // on a project that simply has no "materials/" yet. The ROOT is the
        // kind's (CreateKindRoot): Content/ for assets, Source/ for a C++
        // class -- the same rule the dialog's Location combo keyed off.
        std::filesystem::path dir = proj->Root() / Arcane::Editor::CreateKindRoot(r.kind);
        if (!r.folder.empty())
            dir /= r.folder;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            ARC_WARN("Arcane Editor: could not create '{}': {}", dir.generic_string(), ec.message());
            failCreate("Could not create the folder '" + dir.generic_string() + "' (see Console).",
                       ec.message(), dir.generic_string());
            return;
        }

        const std::filesystem::path target =
            dir / (r.name + Arcane::Editor::CreateKindExtension(r.kind));

        Arcane::Guid created;
        switch (r.kind)
        {
            case Arcane::Editor::CreateAssetKind::Material:
                created = CreateMaterialAt(target, static_cast<Arcane::MaterialSurface>(r.surface));
                break;
            case Arcane::Editor::CreateAssetKind::MaterialInstance:
                created = CreateInstanceAt(target, r.parent);
                break;
            case Arcane::Editor::CreateAssetKind::Mesh:
                // The submenu's preset when the request carried one (Create >
                // Mesh > <primitive>, F4 plan 1 Task 11); MeshAssetData's own
                // Cube default otherwise.
                created = MintMeshAsset(target,
                                        r.meshSource >= 0
                                            ? static_cast<Arcane::MeshSource>(r.meshSource)
                                            : Arcane::MeshSource::Cube);
                break;
            case Arcane::Editor::CreateAssetKind::Sprite:
                created = MintOrReuseSpriteForTexture(r.texture, &target);
                break;
            case Arcane::Editor::CreateAssetKind::Scene:
            {
                // Task 13: a scratch registry, never the live editing session
                // -- a scene is not a DocumentHost document (spec s6), so
                // "create" must not replace what the user is currently
                // editing. CreateEmpty's own shape (a root + a Main Camera)
                // is what New Scene ships, so this mints the SAME starting
                // point New Scene does, just serialized straight to `target`
                // without ever touching m_runtime->Registry().
                Astra::Registry scratch;
                Arcane::Scene::CreateEmpty(scratch);
                std::string err;
                if (Arcane::Scene::SaveSceneFile(target, scratch, Arcane::Guid::Generate(), &err))
                {
                    if (const auto registered = m_runtime->RegisterCreatedAsset(target))
                    {
                        m_assetModel.MarkAllDirty();
                        created = *registered;
                    }
                }
                else
                {
                    ARC_WARN("Arcane Editor: could not create a scene at '{}': {}",
                             target.generic_string(), err);
                }
                break;
            }
            case Arcane::Editor::CreateAssetKind::CppClass:
                // `target` is the HEADER (CreateKindExtension's ".hpp"); the
                // mint derives the .cpp beside it, registers both under
                // source:// and regenerates the solution. `created` is the
                // file to open in Visual Studio (the .cpp when there is one).
                created = MintCppClass(target, r.name, r.classTemplate,
                                       r.systemPhaseIndex, r.systemRoleIndex);
                break;
            case Arcane::Editor::CreateAssetKind::InputActions:
            {
                if (std::filesystem::exists(target)) break;
                const Arcane::InputActionAsset asset = Arcane::InputActionAsset::CreateDefault();
                std::ofstream output(target, std::ios::binary);
                if (!output) break;
                output << asset.ToJson().dump(2) << '\n';
                output.close();
                if (!output) break;
                if (const auto registered = m_runtime->RegisterCreatedAsset(target))
                {
                    created = *registered;
                    m_assetModel.MarkAllDirty();
                }
                break;
            }
        }

        if (!created.IsValid())
        {
            failCreate("Could not create '" + target.generic_string() + "' (see Console).",
                       "The mint for this kind refused or the file could not be written.",
                       target.generic_string());
            return;
        }

        // Kind-specific post-mint effects the dispatcher owns: Material and
        // MaterialInstance already open their own document inside
        // CreateMaterialAt/CreateInstanceAt, but MintMeshAsset/
        // MintOrReuseSpriteForTexture deliberately don't (their own doc
        // comments: "the caller decides") -- so the caller, here, does.
        // Scene never opens as a document at all (it replaces the editing
        // session instead, via the openScene action's unsaved-changes guard
        // -- ConsumeAssetPanelActions above) -- `setAsBoot` is its only other
        // effect, and reuses the SAME message text the row/preview-pane
        // "Set as Boot Scene" quick action already logs (this file, ~:2366).
        switch (r.kind)
        {
            case Arcane::Editor::CreateAssetKind::Mesh:
            case Arcane::Editor::CreateAssetKind::Sprite:
            case Arcane::Editor::CreateAssetKind::InputActions:
                OpenAssetDocument(created);
                break;
            case Arcane::Editor::CreateAssetKind::Scene:
                if (r.setAsBoot)
                {
                    if (m_runtime->SetProjectBootScene(created))
                        ARC_INFO("Boot scene set to {}", created.ToString());
                    else
                        m_modalErrors.Push("Scene Error",
                                           "Could not write the project's boot scene (see Console).");
                }
                break;
            case Arcane::Editor::CreateAssetKind::CppClass:
                // The IDE is a source file's editor (never a DocumentHost
                // document): open the new file in Visual Studio through the
                // same path a Source row's double-click takes -- UE opens the
                // IDE after New C++ Class too. The solution was regenerated
                // by the mint, so the file is already in the project VS sees.
                if (const auto path = proj->ResolveAsset(Arcane::AssetId::FromGuid(created)))
                    OpenInIde(*path);
                break;
            default:
                break;
        }

        // Asset-manager Plan 2 Task 5: every CreateAssetKind arm above funnels
        // through this one tail (spec s7's "no creation path may bypass
        // CreateAssetRequest" -- the same invariant this dispatcher's own
        // header comment states), so ONE push here covers every kind rather
        // than one at each mint. `target` is the file this whole dispatcher
        // just wrote (set once, above, before the switch on r.kind).
        m_assetActivity.Push({ std::chrono::steady_clock::now(), created,
                                target.filename().string(),
                                Arcane::Editor::AssetActivityKind::Created, {} });

        // Land SELECTED in the Browse lens. Every mint above already
        // registered the asset and marked the model dirty, so the next
        // frame's RebuildIfDirty (DrawEditorUi's, ahead of every panel draw)
        // has an entry for this guid -- and the panel's scroll-to-selection
        // stamp brings it into view on that same frame. Select() only
        // records the guid + bumps the stamp, so calling it before the
        // rebuild is correct, not early.
        m_assetModel.Select(created);
    }

    // Phase 15: the modals. Opened and drawn at DOCKSPACE level, outside any
    // panel window -- that is why they live here and not inside a panel draw.
    void EditorApp::DrawModals(LoopState& ls)
    {
        // ONE error modal, FIFO off the queue (architecture pass sec 7). The title
        // is per-error so it stays honest about which action failed; Pop advances
        // to the next queued error on the following frame.
        if (const Arcane::Editor::ModalError* err = m_modalErrors.Front())
        {
            if (!ImGui::IsPopupOpen(err->title.c_str()))
                ImGui::OpenPopup(err->title.c_str());
            if (ImGui::BeginPopupModal(err->title.c_str(), nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                ImGui::TextUnformatted(err->message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Separator();
                if (ImGui::Button("OK", ImVec2(Ui::Px(120.0f), 0)) ||
                    Arcane::Editor::EditorActions::Get().Pressed("ui.cancel") || Arcane::Editor::EditorActions::Get().Pressed("ui.confirm"))
                {
                    m_modalErrors.Pop();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // Unsaved-scene confirm's parked intent -- read here, ahead of the
        // create dialog below, so the create dialog's starvation gate can see
        // it too. Same snapshot is reused at the "Unsaved Scene" block further
        // down (a TakePending() mid-frame cannot change what either reads).
        const Arcane::Editor::SceneIntent pending = m_scene.Pending();

        // The unified create dialog (asset-manager Plan 1 Task 12). A
        // dockspace-level modal like the two around it -- opened by the ONE
        // entry (BeginCreateAsset), dispatched by the ONE dispatcher
        // (ConsumeCreateResult), which is the whole of spec s7's "no creation
        // path may bypass CreateAssetRequest".
        //
        // GATED ON AN EMPTY ERROR QUEUE **AND** ON NO PARKED SCENE INTENT --
        // both load-bearing, not taste. Every modal in this function re-arms
        // itself the same way ("if (!IsPopupOpen(title)) OpenPopup(title)"),
        // and ImGui allows exactly ONE popup at a given stack level: two such
        // modals up at once close each other every frame (OpenPopupEx ->
        // ClosePopupToLevel(0)), which makes both permanently `Appearing` and
        // therefore -- under AlwaysAutoResize, which hides an appearing window
        // for one frame while it fits itself -- permanently INVISIBLE.
        // Observed live: a queued "Open Project Failed" and this dialog
        // starved each other to a blank screen. The "Unsaved Scene" modal just
        // below has the identical re-arm shape and is reachable while the
        // create dialog is open -- the OS window-close button reaches
        // SceneSession and parks an Exit intent regardless of any ImGui modal
        // on screen -- so it must be covered by the same gate or the two
        // starve each other instead. Both a queued error and a parked scene
        // intent are more urgent than a create dialog and must be resolved
        // first, so the create dialog waits its turn either way (its state is
        // untouched, so it draws again once both are clear).
        if (m_createDialog.open && !m_modalErrors.Front() &&
            pending == Arcane::Editor::SceneIntent::None)
        {
            if (const Arcane::Project* createProj = m_runtime->CurrentProject())
            {
                if (const auto created = Arcane::Editor::DrawCreateAssetDialog(
                        m_createDialog, m_assetModel, *createProj))
                    ConsumeCreateResult(*created);
            }
            else
            {
                // The project closed under an open dialog (a switch, or a
                // failed open): there is no content root left to create into,
                // so the dialog goes with it rather than drawing against
                // nothing.
                m_createDialog.open = false;
            }
        }

        // Unsaved-scene confirm. SceneSession parked the intent (New Scene, Open
        // Scene, Open Project, Exit, LaunchStandalone) because the scene is dirty
        // (or, for LaunchStandalone only, never saved); this popup is where it is
        // answered. Same re-arm shape as the modal above. `pending` was read
        // once above (ahead of the create-dialog gate); every branch below
        // reads that same snapshot, so a TakePending() mid-block cannot change
        // what the rest of the frame's UI thinks is pending.
        const bool isLaunch = (pending == Arcane::Editor::SceneIntent::LaunchStandalone);
        if (pending != Arcane::Editor::SceneIntent::None &&
            !ImGui::IsPopupOpen("Unsaved Scene"))
            ImGui::OpenPopup("Unsaved Scene");
        if (ImGui::BeginPopupModal("Unsaved Scene", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (pending == Arcane::Editor::SceneIntent::None)
            {
                // The intent was taken OUTSIDE this popup: the Save button's
                // never-saved branch launches a file dialog and leaves the popup
                // up, and the save (and with it the action) lands at the top of a
                // later frame. Nothing left to ask about. Also covers a parked
                // LaunchStandalone's dialog-in-flight frames the same way.
                ImGui::CloseCurrentPopup();
            }
            else
            {
                ImGui::TextUnformatted((isLaunch
                    ? "'" + m_scene.DisplayName() +
                      "' must be saved before playing in a separate window."
                    : "'" + m_scene.DisplayName() + "' has unsaved changes.").c_str());
                ImGui::Separator();
                if (ImGui::Button(isLaunch ? "Save and Play" : "Save",
                                  ImVec2(Ui::Px(isLaunch ? 140.f : 90.f), 0)))
                {
                    // Stop Play BEFORE saving, in both branches below. Stop
                    // restores the pre-Play snapshot, which IS the authored
                    // state -- so what reaches disk is exactly what the user
                    // believes they are saving, and DoSaveScene's Play refusal
                    // (which would otherwise turn this button into an error
                    // popup) is satisfied. Not a surprising side effect: no
                    // intent parkable here leaves Play running anyway -- New
                    // Scene, Open Scene and Open Project all stop it through
                    // ClearSceneReferences, Exit ends the process, and a parked
                    // LaunchStandalone only reaches here already saved-or-dirty,
                    // never mid-Play in a way DoLaunchStandalone itself needs.
                    if (InPlayMode())
                        m_play.Stop(m_runtime->Core(), m_plugin ? &*m_plugin : nullptr);
                    m_serverProcess.Stop();   // the session's other half; no-op otherwise
                    if (m_scene.Path().empty())
                    {
                        // Never saved: this needs a filename first. The intent
                        // stays parked and this popup stays up until the dialog
                        // resolves, so cancelling the dialog cancels only the
                        // save, not the action behind it.
                        ShowSceneSaveDialog();
                    }
                    else if (DoSaveScene(m_scene.Path()))
                    {
                        ls.sceneAction = m_scene.TakePending();
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        // The save failed (m_modalErrors carries why). Drop the
                        // action rather than proceed -- proceeding would discard
                        // exactly the work the save was meant to preserve.
                        m_scene.ClearPending();
                        ImGui::CloseCurrentPopup();
                    }
                }
                // Discard: hidden for a launch -- the standalone runtime reads
                // the scene from DISK, so "discard and play" would run a stale
                // file.
                if (!isLaunch)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Discard", ImVec2(Ui::Px(90.0f), 0)))
                    {
                        ls.sceneAction = m_scene.TakePending();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(Ui::Px(90.0f), 0)) ||
                    Arcane::Editor::EditorActions::Get().Pressed("ui.cancel"))
                {
                    m_scene.ClearPending();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    // Phase 16: the Viewport panel itself. It samples the texture the render
    // phases produced, and publishes the rect/active/size the NEXT frame's
    // input phase and resize phase read (the deliberate one-frame lag).
    void EditorApp::DrawViewportPanelPhase(FrameState& fs)
    {
        // THE TEXTURE. 0 IS A REAL, EXPECTED VALUE and is the "draw nothing"
        // signal: a ResizeOffscreen whose replacement could not be created
        // leaves the output null on purpose. DrawViewportPanel already skips
        // its ImGui::Image at 0 (EditorPanels.cpp) and still publishes
        // dockId/desiredW/desiredH, so the panel keeps measuring and the next
        // non-zero size heals it through phase 8.
        //
        // A null `graph` -- a project switch whose rebuild failed
        // (SwitchProject's "render_bridge"), which sets m_requestExit, read at
        // the TOP of the next frame so THIS frame still runs phase 16 --
        // answers 0 for the same reason, and 0 is the correct answer.
        const std::uint64_t vpTexture = m_viewportTargets.graph
                                          ? m_viewportTargets.graph->OffscreenTextureId()
                                          : 0;
        // The overlay's state, by reference (F4 plan 1 T8): the 2D | Persp
        // segments assign m_camera.mode exactly as Alt+J / Alt+G do (Resolve
        // reads it next frame); the settings popup writes the editor.viewport.*
        // / editor.camera.* / editor.gizmo.* cvars (settings S6-29), which the
        // camera and the grids read from the next published snapshot.
        Arcane::Editor::ViewportToolState tools{
            m_gizmoEnabled, m_gizmoMode, m_gizmoSpace,
            m_camera.mode,
        };
        // The gizmo as FOREGROUND chrome over the rendered image (Unreal's
        // SDPG_Foreground for its widget): drawn by the panel right after the
        // image, under the tool buttons, through an ImGui sink. Edit mode with
        // a live gizmo and a selection only -- the same gate the hit-test uses,
        // and the SAME handle mask, size and view as this frame's hit-test.
        const Arcane::Editor::ViewportImageOverlayFn gizmoOverlay =
            [this](ImDrawList& list, ImVec2 origin)
            {
                if (InPlayMode() || !GizmoLive() || !m_selection.HasSelection() || !m_runtime)
                    return;
                Astra::Registry& reg = m_runtime->Registry();
                const Arcane::Transform* lt =
                    std::as_const(reg).GetComponent<Arcane::Transform>(m_selection.Primary());
                if (!lt)
                    return;
                // WORLD pose, matching the interaction block's -- the gizmo draws
                // where it hit-tests, including for a parented primary.
                const Arcane::GizmoTransform gt = Arcane::DecomposeTRS(
                    Arcane::Edit::WorldMatrix(reg, m_selection.Primary()));
                Arcane::Editor::ImGuiGizmoSink sink(list, origin);
                Arcane::Draw(sink, m_gizmoMode, m_gizmoSpace, gt, m_runtime->View(), GizmoHandles(),
                             Arcane::Settings<Arcane::Editor::EditorGizmoSettings>().size, m_gizmoHovered,
                             m_gizmoDrag.active ? m_gizmoDrag.axis : Arcane::GizmoAxis::None,
                             Arcane::Editor::MakeGizmoTuning(),
                             m_gizmoDrag.active && m_gizmoDrag.sweep ? &*m_gizmoDrag.sweep : nullptr);
            };
        fs.vp = Arcane::Editor::DrawViewportPanel(vpTexture,
                                            ViewportWidth(), ViewportHeight(),
                                            tools,
                                            Arcane::Editor::ViewportChrome{ /*showToolOverlay=*/!InPlayMode(),
                                                                            /*playing=*/InPlayMode(),
                                                                            /*sceneDirty=*/CurrentTitleParts().sceneDirty },
                                            gizmoOverlay);
        m_viewportDockId = fs.vp.dockId;
        m_viewportTargets.pendingW = fs.vp.desiredW;
        m_viewportTargets.pendingH = fs.vp.desiredH;
        m_viewportRect     = fs.vp.imageRect;
        m_viewportActive   = Arcane::Editor::SceneInputActive(fs.vp.hovered, fs.vp.focused);
    }

    // Phase 17: click-pick. Must stay after the Viewport panel above (it reads
    // that panel's click) and after the input phase (it reads m_gizmoCapturedClick
    // and gameUiClaims from it).
    void EditorApp::HandleViewportPick(const FrameState& fs)
    {
        // Viewport click-pick: GPU hit-proxy. `view` is the SAME world->canvas
        // transform the scene render uses (m_runtime's camera ==
        // RenderContext2D), so the id silhouettes the graph's PickNode
        // rasterizes register pixel-for-pixel with what is drawn. An invalid
        // result (background / outside the viewport) clears the selection.
        // Suppressed when this frame's click was already consumed by the gizmo
        // (pressed/released a handle) or a drag is still in progress -- otherwise
        // clicking/using a handle would also re-pick and disturb the selection.
        // Also suppressed when the game's viewport debug UI claimed the pointer
        // this frame (gameUiClaims, computed in the input phase above) so a click
        // on a game HUD widget does not also re-pick the editor selection.
        //
        // ============ THE PICK IS DEFERRED, NOT SYNCHRONOUS ============
        // The readback is a graph COPY node whose value lands
        // FramesInFlight() frames later, so the click is ARMED here
        // and the resulting Select/Toggle/Clear is APPLIED here on a LATER
        // frame. That latency is inherent to reading a GPU-rasterised id back
        // without stalling the device, and it is the only pick path there is.
        //
        // TWO STEPS, AND THE ORDER IS DELIBERATE: apply what has landed FIRST,
        // then arm this frame's click. The other order would let a fresh click
        // discard an answer that had already arrived.
        // ---- STEP 1: a landed readback -----------------------------
        // ProbeResult does NOT self-clear (it reports the same pair every
        // frame until the next drain), so DeferredPick::Land is what makes
        // consumption exactly-once -- it refuses anything but the ticket
        // outstanding, and returns to Idle whether or not the answer
        // survived its staleness checks.
        if (m_viewportTargets.graph)
        {
            if (const auto probe = m_viewportTargets.graph->ProbeResult())
            {
                if (const auto hit = m_deferredPick.Land(probe->ticket, probe->id,
                                                         m_sceneEpoch, InPlayMode()))
                {
                    // A THIRD staleness check, on top of Land's epoch and
                    // play-mode ones: the entity must still exist in the
                    // LIVE registry. The retained table can name an entity
                    // that was deleted (Delete key, an undo) without the
                    // scene having been REPLACED, which is a case no epoch
                    // can see. Dropping it is the safe direction -- note it
                    // is NOT folded into the miss branch, because a stale
                    // hit is "we do not know", where a miss is the positive
                    // statement "the user clicked background".
                    if (hit->entity.IsValid())
                    {
                        if (m_runtime->Registry().IsValid(hit->entity))
                        {
                            if (hit->ctrlHeld)
                                m_selection.Toggle(hit->entity);
                            else
                                m_selection.Select(hit->entity);
                        }
                    }
                    else if (!hit->ctrlHeld)
                    {
                        // Ctrl+click on empty space is a miss, not a
                        // deselect-all, and the ctrl state is the one
                        // captured AT THE CLICK rather than whatever is held
                        // now.
                        m_selection.Clear();
                    }
                }
            }
            // Bounded, and loud if it ever fires: the readback is
            // guaranteed to drain after FramesInFlight() RENDERED
            // frames, and ArmGraphViewportFrame keeps the chain declared
            // for exactly as long as this is Busy() -- so the only way to
            // exhaust the budget is a very long run of Skipped frames (a
            // collapsed Viewport panel). Giving up beats a state machine
            // that never returns to Idle.
            const std::uint32_t pickBudget =
                Arcane::Settings<Arcane::Editor::EditorViewportSettings>().pickMaxFramesInFlight;
            if (m_deferredPick.TickAndMaybeAbandon(pickBudget))
            {
                ARC_WARN("Viewport pick: no readback landed within {} frames -- the click was "
                         "dropped (was the Viewport panel collapsed?)",
                         pickBudget);
            }
        }

        // ---- STEP 2: this frame's click ----------------------------
        // Arm replaces any outstanding request: the newest click wins, and
        // the older one's copy is dropped on arrival by its stale ticket.
        // The scene epoch and play mode recorded here describe the scene
        // the USER clicked on, which is what the landing compares against.
        // A click in ANY view mode arms the pick: the id pass projects world
        // drawables through FrameDesc::pickView (F4 spec s7.1), so plan 1's fifth
        // guard (no click-pick without a 2D affine) is gone. ONE modifier
        // rule survives it: in EDIT mode's Perspective view an Alt+LMB press
        // is the ORBIT gesture (UpdateEditorCamera tests Alt+LMB first there,
        // under its own !InPlayMode() gate) and must never arm a click-pick
        // -- an orbit press over background would land as "background" and
        // clear the selection. In the 2D view, and in PLAY (where the scene
        // camera drives and there is no orbit), Alt+click stays the
        // cycle-pick modifier, as today.
        const bool orbitPress = !InPlayMode() && fs.vp.altHeld
                             && m_camera.mode == Arcane::Editor::ViewMode::Perspective;
        if (fs.vp.clicked && !m_gizmoCapturedClick && !m_gizmoDrag.active && !fs.gameUiClaims
            && !orbitPress)
        {
            m_deferredPick.Arm(glm::ivec2((int)fs.vp.clickLocalX, (int)fs.vp.clickLocalY),
                               fs.vp.ctrlHeld, m_sceneEpoch, InPlayMode());
        }
        return;
    }

    // Phase 18: the selection-driven panels. Runs after the click-pick above so
    // they draw THIS frame's selection.
    void EditorApp::DrawSelectionPanels()
    {
        m_selection.Prune([reg = &m_runtime->Registry()](Astra::Entity e)
                          { return reg->IsValid(e); });
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Outliner))
            Arcane::Editor::DrawOutlinerPanel(m_runtime->Registry(), m_selection,
                                              *m_undo, m_editBinding, m_outliner,
                                              m_scene.SavedStateId(),
                                              m_panelVis.OpenFlag(Arcane::Editor::PanelId::Outliner));
        // The Outliner's `Add 3D Object` latch (F4 plan 1 Task 11) -- the
        // app performs it here, after the panel's draw, because the mint
        // touches the project registry + disk and the spawn point is the
        // editor camera's (OutlinerState::addPrimitivePending's own comment).
        ConsumeAddPrimitive();
        // ---- Inspector ownership: sources -> host -> instances ----------
        // Selection EVENTS only (spec s3.1): a scene selection GESTURE (the
        // SelectionContext epoch moved -- Select/Toggle/AddRange/Clear, never
        // the per-frame Prune above) that left a non-empty key, or a document
        // whose SelectionEpoch moved AND now has a selection. Focus, tab
        // activation, background clicks, and a prune that re-primaried after a
        // deletion or a structural undo: none of these reach the host. Re-
        // clicking the already-selected entity IS an event (it brings the
        // Inspector back from a document); Push's echo compare keeps it out of
        // history. The Asset Browser's selection is NOT a scene-page
        // fallback any more: it routes as its own source (m_assetSource,
        // bound in DrawEditorUi with the asset panels; inspector filters s6).
        m_sceneSource.Bind({ &m_runtime->Registry(), &m_selection, m_undo ? &*m_undo : nullptr,
                             &m_editBinding, m_runtime->CurrentProject(), &m_inspector,
                             &m_inspectorServices });
        if (m_sceneSelectionEdge.Observe(m_selection.Epoch(), m_sceneSource.SelectionKey()))
            m_inspectorHost.NotifySelected(m_sceneSource);
        // selectionGesture, not selectionStamp: a re-click of the selected
        // asset IS a selection event (spec s3), and the stamp moves only on change.
        if (m_assetSelectionEdge.Observe(m_assetModel.selectionGesture, m_assetSource.SelectionKey()))
            m_inspectorHost.NotifySelected(m_assetSource);
        m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
        {
            std::uint64_t& last = m_docSelectionEpochs[&d];
            const std::uint64_t epoch = d.SelectionEpoch();
            if (epoch == last) return;
            last = epoch;
            if (!d.SelectionKey().empty())
                m_inspectorHost.NotifySelected(d);
        });
        if (m_undo)   // the scene body needs the stack; DrawInspectorBody takes it by reference
        {
            const Arcane::Editor::InspectorWindowsResult res = Arcane::Editor::DrawInspectorWindows(
                m_inspectorHost, m_inspectorWindows,
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::Inspector));
            for (const int id : res.closed)
            {
                m_inspectorHost.RemoveInstance(id);
                m_inspectorWindows.grids.erase(id);
            }
            m_inspectorFocusedSource = res.focusedSource;
            if (InspectorSaveTarget(m_inspectorFocusedSource) != nullptr)
                Arcane::Editor::EditorActions::Get().MarkContextActive(Arcane::Editor::ActionContext::Document);
            // The document's save GESTURE (RequestSave), never the raw Save:
            // the material's save-with-errors confirm must hold here too. A
            // REFUSED save is reported (a deferred confirm is not a refusal).
            for (Arcane::Editor::InspectorSource* src : res.saveRequested)
                if (const auto out = Arcane::Editor::RequestSaveFromInspector(src);
                    out.doc && out.result == Arcane::Editor::SaveGestureResult::Refused)
                    ARC_WARN("Inspector: save refused for '{}'", out.doc->Title());
        }

        // (The hosted plugin's DrawUI now renders into its OWN ImGui context,
        // composited into the viewport texture above -- not the editor context.)
    }

    // ===================== THE CHROME FRAME ===============================
    // Phase 19: what puts the editor's main window on screen.
    //
    // ---- THE FRAME'S DECLARED SHAPE, and why it is what it is -------------
    // Exactly three nodes: batch2d -> tonemap -> imgui -> present.
    //
    //   batch2d   with NO batcher. AddBatch2DNode drains nothing, and
    //             Batch2DNode::Record's first statement is the canvas CLEAR --
    //             so a null batcher IS the clear, not a degenerate draw ("an
    //             empty frame is a cleared canvas, not an error").
    //   tonemap   is the ONLY thing in the tree that produces a backbuffer
    //             handle at all (AddTonemapNode -> ImportSwapChainTexture), so
    //             it is not decoration on top of the clear -- it is how the
    //             clear reaches the surface, and how a frame acquires one.
    //             That is why "clear + imgui + present" has three nodes rather
    //             than two, and why there is no shorter honest spelling of it
    //             through this machinery.
    //   imgui     draws the EDITOR context's draw data onto that backbuffer,
    //             through the chrome context's ImGuiNriNode (NodeSet::hostHud,
    //             set by NriGraphContext::Create because a host-window context
    //             presents chrome by definition).
    //
    // AND NOTHING ELSE, which is a claim about four fields left DEFAULT:
    //   * `post`        -- the scene's post chain belongs to the VIEWPORT
    //                      frame (phase 10). Chrome is not graded.
    //   * `pickOutline` -- likewise the viewport's; the chrome context was
    //                      built with neither node (CreateGraphVehicles asks
    //                      for them on the offscreen context only), so the
    //                      declaration would be refused even if asked for.
    //   * `gameUi`      -- the plugin HUD lives INSIDE the viewport texture,
    //                      never over the editor chrome. Same node gate.
    //   * `capture`     -- and this one is a RULE, not just an absence. The
    //                      editor's screenshot semantics capture the VIEWPORT
    //                      (WriteAutoScreenshot and RenderSceneToViewport's
    //                      capture block both read the OFFSCREEN output), so
    //                      chrome pixels must never reach a capture path. A
    //                      capture node declared HERE would copy the chromed
    //                      backbuffer and quietly redefine what an editor
    //                      screenshot contains.
    //
    // ===== AND NOT ONE OF THOSE FOUR IS PINNED BY A TEST. ==================
    // ArcaneTests does not compile this TU at all (EditorAppFrame.cpp is not in
    // that project -- see this file's header comment), so nothing in the gate
    // can observe the FrameDesc built below. The [nri] case named "an
    // imgui-only shape (the editor's chrome)..." drives DeclareGraphFrame with
    // a shape it TRANSCRIBES by hand; it protects the ENGINE against declaring
    // nodes a shape did not ask for, and it would stay green if the line below
    // grew `frame.capture = true`. So the four absences above are held by this
    // comment and by review, and nothing else. Anyone adding a field here is
    // the only check there is.
    //
    // WHAT THE `capture` RULE DOES HAVE is a structural, greppable guarantee
    // one level up: every capture code path reads pixels ONLY from
    // `m_viewportTargets`. That is a single path -- RenderSceneToViewport's
    // capture block, over the offscreen graph's ReadCapture -- and it does not
    // name `m_graphChrome`, `PresentChromeFrame`, or the swapchain backbuffer
    // anywhere (`grep -n "m_graphChrome\|PresentChromeFrame" EditorApp*.cpp`
    // inside either capture function returns nothing). So a chrome pixel
    // reaching an editor screenshot would require capture code that does not
    // exist, rather than a gate this function could silently defeat.
    //
    // ---- WHERE THE DRAW DATA COMES FROM ----------------------------------
    // The ImGui frame phases 13-18 already built. RenderToDrawData() is
    // ImGui::Render() plus the layer's SetCurrentContext pin -- the same call
    // RuntimeFrame::RenderGraph makes, and the same reason it goes through the
    // layer rather than calling ImGui::Render() bare: a document, the game
    // context or a plugin may have left another context current, and the pin is
    // what makes this the EDITOR's draw data rather than whoever's was last.
    // The returned data lives until that context's next NewFrame -- a whole
    // frame away -- which is what lets the node copy the geometry at RECORD
    // time (NriGraphContext::FrameDesc::imgui).
    //
    // ---- WHAT IS INTERACTIVE ---------------------------------------------
    // This chrome IS the editor UI, so it is interactive by definition. The
    // SDL event tap ImGuiLayer installs is this context's ONLY source of mouse
    // BUTTON events -- withholding it does not merely cost a drag, it makes
    // the editor unclickable outright: menus, panels, viewport pick, gizmo.
    // ImGuiLayer::Init installs it unconditionally for exactly that reason.
    //
    // ---- THE HEARTBEAT ---------------------------------------------------
    // Published by NriGraphContext::RenderFrame itself, after the present and
    // on Presented frames only (NriDiagnostics::PublishHeartbeat with the
    // swapchain's COMPLETED fence value). It is the only heartbeat there is,
    // and it needs no line here: this call reaches it.
    bool EditorApp::PresentChromeFrame()
    {
        // FIRST, and unconditionally: the ImGui frame DrawEditorUi began owes
        // exactly one Render / RenderToDrawData / EndFrameDiscard, and every
        // path out of this function must have discharged it. Doing it here --
        // ahead of the null check and both non-Presented outcomes below --
        // is what makes that true by structure rather than by three copies.
        ImDrawData* const chrome = m_gpu->Imgui().RenderToDrawData();

        // Defensive: CreateGraphVehicles refuses to continue past a chrome
        // context that could not be created (Main() returns 1 without ever
        // entering MainLoop), so this is unreachable rather than a state a run
        // can sit in. TRUE, not false: a frame that cannot present is not a
        // reason to stop counting frames and stop polling the plugin, which is
        // what a false return means to MainLoop.
        if (!ChromeGraph())
            return true;

        Arcane::NriGraphContext::FrameDesc frame;
        frame.imgui = chrome;

        // ===== THE --headless FORK, AND THE FULL-FRAME CAPTURE ===============
        // The absence table above says an editor screenshot has never
        // contained a chrome pixel: `frame.capture` was never set here, and
        // every capture path read the VIEWPORT's texture instead -- so the
        // Inspector, the asset browser and the docking chrome were invisible in
        // every editor PNG ever taken (main.cpp's flag table records it).
        //
        // Under --headless that is now false ON PURPOSE, and the table above
        // is amended by this block rather than contradicted by it: this context
        // has NO SWAPCHAIN, so "the backbuffer" it captures is the offscreen
        // colour target the whole composited editor was just drawn into --
        // chrome, panels, and the viewport texture sampled inside its panel.
        // That is the only picture that can answer "did the editor draw", which
        // is the entire job of the mode.
        //
        // THE MECHANISM NEEDED NO INVENTION. ImGuiNri::Init takes a device and
        // a pipeline cache, NOT a window, and this host sets only
        // ImGuiConfigFlags_DockingEnable with no ViewportsEnable -- so ImGui
        // never spawns an OS window for a floating panel and every pixel of the
        // editor is inside this one target by construction. The runtime already
        // composites its own ImGui HUD into an offscreen capture the same way.
        //
        // THE LAST BASE FRAME, AND EVERY SETTLE ATTEMPT AFTER IT. By the same
        // `+1` arithmetic RenderSceneToViewport uses and for the same reason:
        // phase 20's EndFrame is what increments m_frameCount, and it has not
        // run yet this iteration. `maxFrames != 0` matches both other hosts --
        // without --frames there is no last frame -- and --headless cannot be
        // passed without --frames anyway (HostConfig refuses it at parse
        // time).
        //
        // --settle N (Task 9): this predicate's NAME still says "the last
        // frame", and under an ordinary (non-settle) run it is exactly that --
        // the ordinary branch below stops the loop (via EndFrame) the moment
        // it fires. Under --settle it stays true for EVERY frame from here
        // on, because m_frameCount only grows (see EndFrame's own comment) --
        // which is exactly what is wanted: each such frame is one more settle
        // attempt, and the settle branch below -- not this predicate -- is
        // what decides when the loop actually ends (converged, or the bail
        // conjunction fired: BOTH the attempt budget and --settle-timeout
        // spent). This is the SAME identity
        // RuntimeFrame::RenderGraph's willBeLastFrame states for the
        // runtime's own capture arm: "arm every frame past the base budget"
        // and "arm the one last frame" are one expression, not two.
        const bool offscreenChrome = ChromeGraph()->IsOffscreen();
        // T3-D6: OR the windowed opt-in -- the capture node copies whichever
        // backbuffer this frame writes, the swapchain's included (the
        // runtime's windowed --screenshot already reads it the same way).
        const bool pastBase = (offscreenChrome || WindowedFrameCapture(m_config)) &&
                              CaptureWanted(m_config) &&
                              m_config.maxFrames != 0 &&
                              (m_frameCount + 1) >= m_config.maxFrames;
        frame.capture = pastBase;

        // A non-null `imgui` is what declares the HUD node at all
        // (DeclareGraphFrame gates the host HUD on exactly that), and the
        // chrome frame always wants one.
        //
        // RenderFrameOffscreen vs RenderFrame is NOT a style choice: each
        // REFUSES on the other's mode (NriGraphContext.hpp), so the fork is
        // mandatory the moment this context can be either.
        const Arcane::NriGraphContext::FrameOutcome outcome =
            offscreenChrome ? ChromeGraph()->RenderFrameOffscreen(frame)
                            : ChromeGraph()->RenderFrame(frame);

        if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)
        {
            // Already latched (the "nri-graph" seam bumped RenderErrorCount),
            // so the exit code would be 2 on its own -- but 1 says WHERE the
            // run died and outranks it. Stops rather than spinning on a broken
            // device, at the top of the NEXT frame: returning false here means
            // MainLoop skips EndFrame and loops, and PumpFrameEvents reads
            // m_requestExit before any phase runs.
            NoteGraphFrameFailure("the chrome frame could not be recorded or presented");
            return false;
        }
        if (outcome == Arcane::NriGraphContext::FrameOutcome::Skipped)
        {
            // Routine: a zero-sized surface or an OUT_OF_DATE acquire, both
            // self-healing on the next resize event. FALSE -- no frame was
            // presented, so nothing downstream should count one. The sleep
            // matches RuntimeFrame::RenderGraph's: without a presented frame
            // there is no pacing wait, and a zero-sized window would
            // otherwise spin. Both read app.window.minimizedSleepMs.
            std::this_thread::sleep_for(std::chrono::milliseconds(Arcane::Settings<Arcane::AppWindowSettings>().minimizedSleepMs));
            return false;
        }

        // THE READBACK, right here rather than in a later phase, for the reason
        // RenderSceneToViewport's own capture block states: ReadCapture is
        // synchronous (it idles the device internally) and the pixels belong to
        // the frame that was JUST submitted -- reading them any later risks the
        // next frame's Begin/Draw overwriting the vehicle's transient state
        // first. Reached only on Presented: both other outcomes returned above.
        //
        // DEFENSIVE (mirrors RuntimeFrame.cpp:710-723 exactly -- fix round 1,
        // item 5, there): HostConfig::Parse already refuses --settle without
        // --screenshot/--report at parse time, so `m_config.settleAttempts
        // != 0 && !CaptureWanted(m_config)` should be unreachable from any
        // real command line. But `pastBase` folds CaptureWanted in as one of
        // its conjuncts, so in that state `pastBase` can NEVER become true no
        // matter how many frames run -- `if (!pastBase) return true;` below
        // would then fire every single iteration forever, m_settleAttemptsUsed
        // would never advance (it lives past that early return), and none of
        // EndFrame's stop conditions -- m_settleConverged, or m_settleBail
        // leaving Keep -- could ever become true either, because BOTH are
        // written by the settle branch that early return skips. That
        // is an unstoppable run -- the exact hazard --headless's own
        // --frames-required refusal exists to prevent -- if some caller ever
        // builds a HostConfig bypassing Parse (a test, a future embedder).
        // Fail CLOSED rather than hang: raise m_settleAborted, which EndFrame
        // reads as an unconditional stop -- the same "stop now" effect
        // RuntimeFrame::CaptureTail's own `return true` (which directly
        // controls its while condition) gets for free there.
        //
        // THIS USED TO FORGE THE COUNTER (m_settleAttemptsUsed =
        // m_config.settleAttempts) so EndFrame's attempts-only check would
        // trip. That trick DIED with the bail conjunction: once the loop also
        // requires the TIME bound to be spent, saturating the attempt count
        // stops nothing at all, and the unstoppable run this branch exists to
        // prevent would come straight back. A dedicated flag also leaves
        // m_settleAttemptsUsed telling the truth about attempts actually taken.
        if (m_config.settleAttempts != 0 && !CaptureWanted(m_config))
        {
            ARC_ERROR("--settle has nothing to compare against (--screenshot and --report are "
                      "both empty) -- this should have been refused at parse time; stopping "
                      "rather than spinning forever");
            m_settleConverged = false;
            m_settleAborted   = true;
            return true;
        }

        if (!pastBase)
            return true;

        // ===== --settle N (Task 9) ==========================================
        // Everything from here down is RuntimeFrame::CaptureTail's settle
        // branch, ported rather than reinvented -- Step 1 of this task's own
        // brief. Two shapes:
        //   * settleAttempts == 0 -- the ORDINARY single-shot tail, unchanged
        //     from Task 8/11: one readback, one screenshot, done.
        //   * settleAttempts != 0 -- one more ATTEMPT: compare this readback
        //     against the PREVIOUS attempt's (byte-equal), conjoin the shader
        //     compiler's IsIdle() (a stability predicate is not the
        //     QUIESCENCE one this mode needs -- see RuntimeFrame.cpp's own
        //     comment on why), and -- when --compare is running -- conjoin a
        //     reference match. Converged: write the screenshot (if any) and
        //     stash the agreed capture. Not yet: stash THIS attempt as the
        //     new baseline and keep going. BOTH bounds spent without
        //     converging: stop with m_settleConverged left false and
        //     m_captureRead left false -- no screenshot, no report SetCapture,
        //     an honest absence rather than a frame this run never agreed on.
        // The loop's actual STOP decision is EndFrame's, reading
        // m_settleConverged / m_settleAborted / m_settleBail -- NEVER the
        // attempt count, which is only ONE HALF of the bail conjunction
        // (Arcane/Host/SettleBound.hpp); re-deriving a bound from it would
        // reinstate the defect that conjunction exists to close. This function
        // always returns true from here down (a chrome frame WAS presented this
        // iteration), exactly like the ordinary tail always did.
        if (m_config.settleAttempts == 0)
        {
            std::uint32_t cw = 0, ch = 0;
            std::vector<unsigned char> pixels;
            if (!ChromeGraph()->ReadCapture(cw, ch, pixels))
            {
                // A WARN, never an exit code -- the same contract
                // RuntimeFrame::CaptureTail and the viewport capture keep, and
                // the same wording, so one grep finds every host's version.
                ARC_WARN("screenshot FAILED: {} (no capture landed)", m_config.screenshotPath);
            }
            else
            {
                if (!m_config.screenshotPath.empty())
                {
                    if (Arcane::WritePngRgba(m_config.screenshotPath, cw, ch, pixels.data()))
                    {
                        ARC_INFO("screenshot written: {} ({}x{}, composited editor frame -- "
                                 "chrome, panels and viewport)", m_config.screenshotPath, cw, ch);
                    }
                    else
                    {
                        ARC_WARN("screenshot FAILED: {}", m_config.screenshotPath);
                    }
                }

                // Stashed for VerifyReport regardless of --screenshot,
                // mirroring RuntimeFrame::CaptureTail's non-settle tail
                // exactly: a bare `--report` with no `--screenshot` still
                // needs these for SetCapture.
                m_captureWidth  = cw;
                m_captureHeight = ch;
                m_captureRgba   = std::move(pixels);
                m_captureRead   = true;
            }
            return true;
        }

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> actual;
        const bool read = ChromeGraph()->ReadCapture(w, h, actual);
        // Counted here, beside the readback -- NEVER off m_frameCount (see
        // EndFrame's own comment on why: a Skipped chrome present returns
        // false above and never reaches this line, so a counter tied to
        // frame advancement could spin forever on exactly that path, where
        // the runtime's cannot).
        ++m_settleAttemptsUsed;

        // Stamp the settle phase's own start on its FIRST attempt, and measure
        // from there. NOT from process start: that would charge boot, project
        // open and the whole --frames budget against the convergence budget,
        // making the timeout depend on how long the scene took to load.
        if (m_settleAttemptsUsed == 1)
            m_settleStartedAt = std::chrono::steady_clock::now();
        m_settleElapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_settleStartedAt).count());

        if (!read)
        {
            ARC_WARN("--settle attempt {}/{}: capture FAILED (no readback landed this frame)",
                     m_settleAttemptsUsed, m_config.settleAttempts);
        }
        else
        {
            const bool byteEqual = m_previousCaptureValid &&
                                    w == m_previousCaptureWidth && h == m_previousCaptureHeight &&
                                    actual == m_previousCaptureRgba;
            const bool shaderIdle = m_shaderCompiler ? m_shaderCompiler->IsIdle() : true;
            // Task 12a fix (task12-rca-report.md, root cause H3): byte-equal
            // + shader-idle proves the RENDER is quiescent, not that the
            // async cook/import-reconciliation pipeline feeding the Asset
            // Browser has finished writing. A project's first open with a
            // freshly-staged Model asset has PollAssetWatch's frame-1 sweep
            // call m_cookQueue->NoteChanged() (EditorAppProject.cpp
            // ~724-735), submitting an async CookSession::CookProject on a
            // JobSystem worker; whenever that lands, Pump() -> OnCookCompleted
            // -> MintOrUpdateCompanionMesh -> MarkAllDirty() changes
            // AssetPanelModel's entry count on a LATER frame -- a race a
            // byte-equal-frames check alone cannot see, because the pixels
            // that changed belong to a frame this loop had not rendered yet.
            // Widen the same idle conjunction to the two other background
            // producers that can still change a later frame's captured UI:
            //   - the cook queue (CookQueue::CookPending() / the editor's own
            //     m_cookQueueSettling window between OnProjectOpened and the
            //     first OnCookCompleted, EditorApp.hpp's own doc comment on
            //     each);
            //   - the material/mesh thumbnail harvester (PendingCount(),
            //     queued + pending + ready -- MaterialPreviewHarvester.hpp),
            //     which also calls back into chrome textures on a later
            //     frame once a harvest lands.
            // PollAssetWatch's own sweep is NOT a fourth conjunct: it runs
            // synchronously, once per tick, inline on the frame that calls it
            // (EditorAppProject.cpp ~416-420) -- it never straddles a frame
            // boundary on its own, only the async CookQueue submission it can
            // trigger does, and that is what cookIdle already waits on. The
            // asset-activity feed (m_assetActivity.Push, same function) is a
            // synchronous side effect of the same tick for the same reason.
            // ContentDiscovery is a one-shot scan at project open, already
            // finished long before the settle loop's first attempt.
            const bool cookPending      = m_cookQueue && m_cookQueue->CookPending();
            const bool harvesterPending = m_materialThumbs && m_materialThumbs->PendingCount() != 0;
            const bool idle = Arcane::SettleProducersIdle(shaderIdle, m_cookQueueSettling,
                                                           cookPending, harvesterPending);
            // Kept apart from `idle` only for the per-attempt WARN below, so a
            // caller can tell which producer is still busy without
            // re-deriving it from SettleProducersIdle's own body.
            const bool cookIdle      = !m_cookQueueSettling && !cookPending;
            const bool harvesterIdle = !harvesterPending;

            // PLAN B (Task 8, ported here by Task 9): the comparison is a
            // THIRD conjunct, evaluated HERE -- inside the loop -- and not
            // after it. m_compareRequested is false whenever --bless was
            // given (EditorApp.hpp's own comment), so `matches` stays its
            // default `true` on any run that never asked for --compare, or
            // that paired it with --bless -- this conjunct degrades to
            // Plan A's `byteEqual && idle` in both cases, exactly like the
            // runtime's.
            bool matches = true;
            if (byteEqual && idle && m_compareRequested)
            {
                Arcane::PixelData actualPixels;
                actualPixels.width  = w;
                actualPixels.height = h;
                actualPixels.rgba   = actual;
                Arcane::ImageCompareOptions compareOptions;
                compareOptions.maxDiffPixels     = m_config.maxDiffPixels;
                compareOptions.maxDiffPixelRatio = m_config.maxDiffPixelRatio;
                m_compareResult    = Arcane::CompareImages(m_referencePixels, actualPixels,
                                                            compareOptions);
                matches            = m_compareResult.passed;
                m_compareEvaluated = true;
            }

            if (Arcane::SettleConverged(byteEqual, idle, matches))
            {
                m_settleConverged = true;
                ARC_INFO("--settle: CONVERGED after {} attempt(s) -- frame {} matches frame {} "
                         "byte-for-byte ({}x{}), compiler/cook-queue/thumbnail-harvester all idle",
                         m_settleAttemptsUsed, m_frameCount + 1, m_frameCount, w, h);

                // The exact pixels the composited editor showed -- same as
                // the ordinary tail above -- this IS the converged capture.
                if (!m_config.screenshotPath.empty())
                {
                    if (Arcane::WritePngRgba(m_config.screenshotPath, w, h, actual.data()))
                    {
                        ARC_INFO("screenshot written: {} ({}x{}, composited editor frame -- "
                                 "chrome, panels and viewport)", m_config.screenshotPath, w, h);
                    }
                    else
                    {
                        ARC_WARN("screenshot FAILED: {}", m_config.screenshotPath);
                    }
                }

                m_captureWidth  = w;
                m_captureHeight = h;
                m_captureRgba   = std::move(actual);
                m_captureRead   = true;
                return true;
            }

            if (byteEqual && idle && !matches)
            {
                // Stable, quiescent, and WRONG -- keep spinning rather than
                // fail fast, the same reasoning RuntimeFrame::CaptureTail
                // gives (a non-_WIN32 IsIdle() stub can be vacuously true,
                // making the reference goal the only honest signal left).
                ARC_WARN("--compare attempt {}/{}: converged pixels do not match reference "
                         "'{}' -- {}", m_settleAttemptsUsed, m_config.settleAttempts,
                         m_config.compareReference, m_compareResult.errorMessage);
            }

            if (byteEqual && !idle)
            {
                // The pixels agree but at least one background producer is
                // not done -- exactly the case Task 10's runtime fix closes
                // for the shader compiler, and Task 12a widens to the cook
                // queue and thumbnail harvester (see the idle conjunction's
                // own comment above). Named individually rather than folded
                // into one generic "not idle" so a caller can tell which
                // knob to look at without re-deriving it from the code.
                ARC_WARN("--settle attempt {}/{}: frame {} matches frame {} byte-for-byte, but "
                         "not idle yet -- shaderCompiler={} cookQueue={} thumbnailHarvester={} "
                         "-- not counted as converged",
                         m_settleAttemptsUsed, m_config.settleAttempts, m_frameCount + 1,
                         m_frameCount, shaderIdle ? "idle" : "busy", cookIdle ? "idle" : "busy",
                         harvesterIdle ? "idle" : "busy");
            }

            // Not settled yet -- either the bytes genuinely differed, or they
            // matched but the compiler still has outstanding work. Either
            // way THIS frame becomes the baseline the NEXT attempt compares
            // against.
            m_previousCaptureWidth  = w;
            m_previousCaptureHeight = h;
            m_previousCaptureRgba   = std::move(actual);
            m_previousCaptureValid  = true;
        }

        // THE BAIL IS A CONJUNCTION (see Arcane/Host/SettleBound.hpp): both the
        // attempt budget AND the timeout must be spent. Line-for-line the same
        // decision RuntimeFrame::CaptureTail makes -- the two loops stay
        // equivalent, which is why the predicate lives in a shared header
        // rather than being spelled out twice.
        //
        // UNLIKE the runtime, this function ALWAYS returns true; the loop's
        // actual stop decision lives in EndFrame, which reads m_settleBail
        // (and m_settleAborted) rather than re-deriving a bound of its own.
        m_settleBail = Arcane::SettleBailDecision(m_settleAttemptsUsed, m_config.settleAttempts,
                                                  m_settleElapsedMs, m_config.settleTimeoutMs,
                                                  Arcane::kSettleIntervalMs);
        if (m_settleBail != Arcane::SettleBail::Keep)
        {
            // No readback EVER landed, so the loop never had two frames to
            // compare -- a different fact from "captured fine, never agreed"
            // (a broken capture path vs a genuinely unstable scene). Line-for-
            // line RuntimeFrame::CaptureTail's own, for the same reason: Task
            // 3's report ranks it ABOVE the governing bound, because it means
            // neither bound ever got a fair test.
            m_settleCaptureFailed = !m_previousCaptureValid;

            // NON-CONVERGENCE. FAIL EXPLICITLY: m_settleConverged stays
            // false (EndFrame reads it to stop the loop; ShutdownGraphPath
            // reads it to fold exitReason "settle-not-converged"/
            // "compare-failed" into m_graphExit) and m_captureRead is
            // deliberately left false -- no --screenshot is written, and the
            // report's SetCapture is never called, so an agent reading
            // either artifact gets an honest ABSENCE rather than a frame
            // this run never actually agreed on.
            //
            // The GOVERNING bound is named, not merely the one that tripped, so
            // the caller knows which knob would actually change the outcome.
            ARC_ERROR("--settle: NOT CONVERGED after {} attempt(s) / {} ms -- two consecutive "
                      "captures never compared byte-equal with the compiler idle ({})",
                      m_settleAttemptsUsed, m_settleElapsedMs,
                      m_settleBail == Arcane::SettleBail::AttemptsBound
                          ? "attempt budget governed -- raise --settle"
                          : "timeout governed -- raise --settle-timeout");
        }
        else
        {
            // PACING. Without this the loop spins as fast as the chrome can
            // present, so the time bound could never be reached by anything but
            // a slow frame -- the interval is what makes the timeout a real
            // budget rather than a formality. 50ms is OURS, not inherited; see
            // kSettleIntervalMs.
            std::this_thread::sleep_for(std::chrono::milliseconds(Arcane::kSettleIntervalMs));
        }

        return true;
    }

    // Phase 19: present. One forward, to PresentChromeFrame -- the editor has
    // one presentation surface and one way to reach it.
    bool EditorApp::PresentFrame()
    {
        return PresentChromeFrame();
    }

    // Phase 20: end of frame -- plugin hot-reload poll + the --frames N budget.
    //
    // ===== THE DLL SWAP'S POSITION IS THE SAFETY ==========================
    //
    // PluginHost::Poll is where a rebuilt module is actually swapped in: it
    // notices the fresh mtime, waits out its own 250 ms debounce, and then
    // runs Reload -> plugin Shutdown, registry reset, FreeLibrary, LoadLibrary,
    // Init. THIS LINE is the only place in the editor that reaches it.
    //
    // It sits AFTER every recorder in the frame, which is what makes the swap
    // unable to race one:
    //   * phase 10 declared, compiled, recorded and SUBMITTED the viewport
    //     graph frame (RenderSceneToViewport -> RenderFrameOffscreen);
    //   * phase 19 did the same for the chrome frame and presented it
    //     (PresentChromeFrame / PresentFrame).
    // So at this statement nothing is recorded-but-unsubmitted.
    //
    // AND WHAT THE GPU MAY STILL BE READING IS NEVER THE PLUGIN'S. Everything
    // a graph frame reads was copied into HOST-OWNED objects at record time or
    // earlier: the plugin's sprites go through Batcher2D -> the upload ring,
    // its HUD through m_gameImgui (a host ImGui context, a host atlas, a host
    // ImGuiNri) whose vertices ImGuiNriNode::Record copies into that same
    // ring, and its textures resolve Guid -> Assets pixels -> NriTextureCache.
    // The plugin owns no nri object, no descriptor and no command buffer, so
    // unmapping its image cannot invalidate anything in flight. That is a
    // property of the boundary (PluginABI's C vtable hands across no GPU
    // handle at all), not of this ordering -- but the ordering is what
    // makes it hold even for the ImDrawData, which lives in the game context
    // until the next NewFrame and is consumed entirely inside phase 10.
    //
    // MOVING THIS CALL EARLIER IN THE FRAME WOULD REOPEN THE QUESTION. Between
    // phase 10 and phase 19 the chrome frame has not been recorded yet while
    // the viewport frame is already submitted; a reload there would run the
    // registry reset between the two recorders, i.e. between a picture and the
    // ImGui pass that samples it. The other half of the build flow (the
    // re-engage + scene reload that PollModuleBuild performs when NO host is
    // watching) is at the frame TOP for the same family of reason.
    //
    // THE VIEWPORT CAPTURE HAS NO FUNCTION OF ITS OWN EITHER. It is armed as a
    // NODE inside RenderSceneToViewport's own FrameDesc (vp.capture) and read
    // back synchronously right there -- there is no separate "read this
    // texture" entry point on the graph recorder for a later phase to use,
    // which is exactly the reasoning CaptureGraphViewportPng's own comment
    // gives for why an auto-screenshot re-renders rather than reading the last
    // frame.

    void EditorApp::EndFrame(LoopState& ls)
    {
        if (m_plugin)
        {
            // Poll decides internally whether to swap (PluginHost.cpp:1003-1006).
            // Clear in the same EndFrame, before any undo can run: no command's
            // destructor calls module code (spec s3.3f, verdict CONFIRMED).
            const std::uint32_t generation = m_plugin->Generation();
            m_plugin->Poll();
            if (m_undo && m_plugin->Generation() != generation)
                Arcane::Editor::ClearHistoryForModuleReload(*m_undo, m_scene);
        }

        ++m_frameCount;

        // --settle N (Task 9). Once a settle loop is running, m_frameCount
        // reaching maxFrames no longer means "stop" -- it means "the base
        // budget is spent and PresentChromeFrame is now taking settle
        // attempts", which keeps happening every further iteration
        // (m_frameCount keeps growing past maxFrames the same way
        // RuntimeFrame::CaptureTail's io.frameCount does; nothing else in
        // this host reads "past maxFrames" as anything but a stop signal,
        // so that growth is harmless). The loop's ACTUAL end is decided by
        // PresentChromeFrame alone, through m_settleConverged / m_settleBail
        // -- both set beside the READBACK there, never off this counter (see
        // PresentChromeFrame's own comment on why: it returns false on a
        // Skipped chrome present, which never reaches EndFrame at all, so
        // tying attempts to frame advancement could spin forever on exactly
        // that path) -- plus m_settleAborted, which that function's defensive
        // fail-closed branch raises BEFORE any attempt is taken. The attempt
        // COUNT is not among them: it is one half of a conjunction, and this
        // is the site where reading it alone would undo the fix.
        if (m_config.settleAttempts != 0)
        {
            // The bail is a CONJUNCTION now, and PresentChromeFrame has already
            // evaluated it into m_settleBail -- re-deriving an attempts-only
            // bound HERE would silently reinstate the very defect the shared
            // predicate exists to close, because this is where the editor's
            // loop actually stops (PresentChromeFrame always returns true).
            // m_settleAborted is the defensive fail-closed path's own stop, and
            // must be honoured unconditionally: it fires before any attempt is
            // ever taken, so neither bound of the conjunction can be spent.
            if (m_settleConverged || m_settleAborted || m_settleBail != Arcane::SettleBail::Keep)
                ls.running = false;
        }
        else if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames)
        {
            ls.running = false;
        }
    }
}
