// EditorApp: Init -> MainLoop -> Shutdown. Consumes the engine's host-boot
// helpers (Arcane::GpuContext/FramePerf/HostConfig, exported from Arcane.dll)
// and hosts Sandbox.dll via the lifted Arcane::PluginHost. The frame loop
// advances the sim through the RunLoop, renders the scene through the
// offscreen NriGraphContext viewport vehicle into a panel texture instead of
// the window's own backbuffer, and draws an editor shell -- a full-viewport
// dockspace (Arcane::Editor::BeginDockSpace)
// hosting a Sim toolbar (play/pause/step + time-scale), a Console panel fed by a
// callback sink on Arcane::Log::Engine(), and a dockable Viewport panel showing
// the scene texture. Scene input (camera pan/zoom, click-pick) is gated on the
// Viewport panel's hover/focus and the cursor is remapped into viewport-local
// pixels before the plugin sees it (see ViewportInput.hpp). The render plumbing
// + teardown order live in GpuContext (m_gpu). The teardown CONTRACT is encoded
// in the EditorApp member declaration order -- see EditorApp.hpp.
//
// EditorApp is ONE class across four translation units:
//   EditorApp.cpp         boot + teardown (this file): Init, Shutdown, Run,
//                         the console sink, the window title.
//   EditorAppFrame.cpp    MainLoop and its per-frame phases. The frame ORDER
//                         there is load-bearing and untested -- read its header.
//   EditorAppScene.cpp    the .arcscene effects + scene dialogs + framing.
//   EditorAppProject.cpp  Open Project, material/instance creation, the watcher.

#include "App/EditorApp.hpp"
#include <Arcane/Platform/Platform.hpp>

#include "App/HostPresentation.hpp"   // HostPresentationFor: the splash/activation rule (T3-D6 fix round 1)
#include "Settings/SettingsHost.hpp"
#include "Settings/EditorRestart.hpp"
#include "Settings/LayoutSettings.hpp"   // editor.layout.default / openPanelsAtStart (S4-18)
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Input/EditorActions.hpp"
#include <Arcane/Input/KeyLayout.hpp>
#include "Panels/AssetGraphPanel.hpp"   // DestroyAssetGraphPanelCanvas (Task 5, panel-split)
#include "Panels/LayoutLibrary.hpp"     // the session layout dir + its seed (S4-18)
#include "Panels/PanelRegistry.hpp"
#include "Documents/CrashReportDocument.hpp"
#include "Documents/MeshDocument.hpp"
#include "Documents/SpriteDocument.hpp"

#include <Arcane/Host/GpuSceneHost.hpp>      // GpuSceneArmVisibilityReadback / GpuSceneVisibleRows (F3 plan 2 T5)
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Host/ReferenceImages.hpp>   // --compare/--bless (Task 9): ResolveReference/BlessReference/DiffArtifactPath
#include <Arcane/Host/VerifyReport.hpp>      // --report (Task 9): VerifyReport
#include <Arcane/Base/Assert.hpp>   // ARC_ASSERT (StageEditorShell's context tripwire)
#include <Arcane/Base/DiagEnvelope.hpp>   // Diag::ReadFile (crashReportFactory/Peek, beside materialFactory)
#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::RetargetDumpDir (RetargetDumpDir, beside RetargetLayoutIni)
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/ForeignModules.hpp>   // ForeignModules::Scan / LastScan / Tier1Names -- the injected-overlay facts (Problems notice, report, RenderErrorCount line)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Jobs/JobSystem.hpp>   // F2b Task 12: m_runtime->Jobs().Submit (CookQueue's SubmitFn)
#include <Panels/ConsoleModel.hpp>   // ConsoleEntry / CategoryForMessage (ConsoleDiagnostics::Install)
#include <Arcane/Material/MaterialAsset.hpp>   // Save/LoadMaterialAsset (New/Open Material flows)
#include <Arcane/Mesh/MeshAsset.hpp>   // Save/LoadMeshAsset (MeshDocument factory + peek)
#include <Arcane/Config/Settings.hpp>   // Settings<EditorUiStyleSettings>: the boot style metrics (settings S6-28)
#include "Settings/AssetBrowserSettings.hpp"     // EditorOpenOptions: editor.assets.mountDiagnostics (settings S6-38)
#include "Settings/EditorPlaySettings.hpp"       // ReadPlayModeIniLine: the old [EditorPlayMode] section (settings S6-32)
#include "Settings/EditorViewportSettings.hpp"   // EditorGizmoSettings: the session's starting gizmo tool (settings S6-31)
#include "Settings/GraphCanvasSettings.hpp"        // CaptureGraphNodePaddingAtBoot: editor.graph.nodePadding (Restart; S6-44)
#include <Arcane/Platform/Paths.hpp>   // Arcane::Paths -- Saved/, Diagnostics and the layouts dir resolve through it (settings spec s11.0)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion (StagePluginLoad's failure banner)
#include "App/EditorTitle.hpp"   // TitleParts / FormatOsTitle (UpdateWindowTitle, CurrentTitleParts)
#include "App/PluginLoadFailure.hpp"   // DescribePluginLoadFailure (StagePluginLoad's failure banner)
#include "Documents/InputActionsDocument.hpp"
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (sprite-material resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>   // Arcane::GraphicsBackend / ToString (HUD)
#include <Arcane/Render/RenderErrorLatch.hpp>  // RenderErrorCount (the graph latch fold)
#include <Arcane/Render/GpuInstrumentation.hpp>   // Arcane::GpuDeviceLostObserved (Run()'s exit-code tail)
#include <Arcane/Render/Nri/NriCommon.hpp>   // ARC_NRI_CHECK (TeardownGraphForSwitch's idle)
#include <Arcane/Sprite/SpriteAsset.hpp>  // Save/LoadSpriteAsset (SpriteDocument factory + peek)
#include <Arcane/Scene/Components.hpp>    // Identity (--select-name boot seed)

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <imgui.h>
// AddSettingsHandler / FindSettingsHandler, ImGuiSettingsHandler and
// ImHashStr are internal-only -- ImGui's ini extension point has never been
// in the public header (same cite as ShaderEditorDocument.cpp, which
// registers its own handler the same way).
#include <imgui_internal.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <atomic>   // g_cleanExitRequested -- the clean-exit hook's latch
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        // The OLD persistence of the Play launch mode: an ImGuiSettingsHandler
        // section "[EditorPlayMode][State]", one "Mode=%d" line. Now the
        // editor.play.launchMode cvar (settings S6-32); the handler is still
        // registered in Init (right after ImGui's context exists and before the
        // first NewFrame reads the ini) so an old section is read and imported
        // once -- it is never written again.
        constexpr const char* kPlayModeIniType = "EditorPlayMode";
        constexpr const char* kPlayModeIniName = "State";
    }

    EditorApp::EditorApp(HostConfig cfg, Arcane::BootSplashWindow* splash)
        : m_config(std::move(cfg)), m_splash(splash),
          m_splashPresenter(m_splash) {}

    void* EditorApp::PlayModeSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler,
                                              const char* name)
    {
        // handler->UserData is `this` (set in RegisterPlayModeSettings) -- exactly
        // one EditorApp per process, so it doubles as the "entry" ReadLine receives.
        return std::strcmp(name, kPlayModeIniName) == 0 ? handler->UserData : nullptr;
    }

    void EditorApp::PlayModeSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*,
                                             void* entry, const char* line)
    {
        // Malformed or out-of-range: refused (ReadPlayModeIniLine), so nothing
        // is imported. A valid line waits in m_legacyPlayMode for the next
        // frame's ImportLegacyPlayMode.
        auto* self = static_cast<EditorApp*>(entry);
        (void)Arcane::Editor::ReadPlayModeIniLine(line, self->m_legacyPlayMode);
    }

    void EditorApp::PlayModeSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer*)
    {
        // Writes nothing: the mode is a cvar now (settings S6-32), so the old
        // section drops out of the ini on its next save. ImGui calls every
        // handler's WriteAllFn unconditionally, so this stays registered.
    }

    // ImGui::ClearIniSettings -- a WINDOWED project switch (RetargetLayoutIni)
    // before it reads the incoming file: an outgoing file's unimported mode
    // must not be imported into the incoming project.
    void EditorApp::PlayModeSettingsClearAll(ImGuiContext*, ImGuiSettingsHandler* handler)
    {
        auto* self = static_cast<EditorApp*>(handler->UserData);
        self->m_legacyPlayMode.reset();
    }

    void EditorApp::RegisterPlayModeSettings()
    {
        // No context (headless) or already registered: nothing to do.
        if (ImGui::GetCurrentContext() == nullptr ||
            ImGui::FindSettingsHandler(kPlayModeIniType) != nullptr)
            return;

        ImGuiSettingsHandler handler;
        handler.TypeName   = kPlayModeIniType;
        handler.TypeHash   = ImHashStr(kPlayModeIniType);
        handler.UserData   = this;   // one EditorApp per process (see m_legacyPlayMode's decl)
        handler.ReadOpenFn = &EditorApp::PlayModeSettingsReadOpen;
        handler.ReadLineFn = &EditorApp::PlayModeSettingsReadLine;
        handler.WriteAllFn = &EditorApp::PlayModeSettingsWriteAll;
        handler.ClearAllFn = &EditorApp::PlayModeSettingsClearAll;
        ImGui::AddSettingsHandler(&handler);
    }

    // ---- [EditorViewport][Camera] (F4 plan 1 T7) ----------------------------
    // The PlayMode handler above, line for line, over ViewportSettings::WriteIni
    // / ReadIniLine (the pure, unit-tested line format -- see that header for
    // the refusal table). The section name constants live on ViewportSettings
    // so the test and the handler cannot drift apart.
    void* EditorApp::ViewportSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler,
                                              const char* name)
    {
        return std::strcmp(name, Arcane::Editor::ViewportSettings::kIniName) == 0 ? handler->UserData : nullptr;
    }

    void EditorApp::ViewportSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*,
                                             void* entry, const char* line)
    {
        auto* self = static_cast<EditorApp*>(entry);
        // A refused line (malformed, out of range, unknown key) leaves the
        // defaults; an ACCEPTED transform line (Ortho= / Orbit=) marks the
        // camera as restored and cancels the boot-time SceneOpen framing
        // (F4 plan 1 final review, F3 -- see m_cameraRestoredFromIni). The
        // request always precedes this read: OnProjectOpened records it, and
        // the ini is read afterwards (RetargetLayoutIni under --headless, the
        // first NewFrame on a windowed run, RetargetLayoutIni's reload on a
        // windowed project switch). The committed verify-layout.ini
        // carries no [EditorViewport] block, so gate/witness runs still frame.
        const bool accepted =
            Arcane::Editor::ViewportSettings::ReadIniLine(line, self->m_camera, self->m_legacyViewport);
        if (accepted && (std::strncmp(line, "Ortho=", 6) == 0 || std::strncmp(line, "Orbit=", 6) == 0))
        {
            self->m_cameraRestoredFromIni = true;
            if (self->m_editSchedule)
                self->m_editSchedule->CancelFrame(Arcane::Editor::FrameRequest::SceneOpen);
        }
        // THE FLAG BEATS THE INI. On a windowed run ImGui reads io.IniFilename
        // at the FIRST NewFrame (ImGui::NewFrame -> UpdateSettings), which is
        // AFTER StageFinalize applied the seed -- so a persisted Mode= line
        // would otherwise silently overrule --view-mode. Re-applying the seed
        // after every line is cheap (a string compare) and makes the ini's
        // read order irrelevant. Under --headless the seed layout is loaded
        // explicitly in RetargetLayoutIni (inside StageFinalize) and carries
        // no [EditorViewport] block anyway; StageFinalize's own call covers
        // the "no block at all" case on both paths. A windowed project switch
        // re-reads the incoming project's ini (RetargetLayoutIni: Clear +
        // LoadIniSettingsFromDisk), so the seed is re-applied there too.
        Arcane::Editor::ApplyViewModeSeed(self->m_config.viewMode, self->m_camera);
    }

    void EditorApp::ViewportSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
                                             ImGuiTextBuffer* buf)
    {
        auto* self = static_cast<EditorApp*>(handler->UserData);
        // WriteIni appends the "[Type][Name]" header itself from the same
        // constants handler.TypeName was registered from.
        Arcane::Editor::ViewportSettings::WriteIni(*buf, self->m_camera);
    }

    // ImGui::ClearIniSettings (a windowed project switch, RetargetLayoutIni):
    // the camera and any not-yet-imported legacy preferences go back to a
    // fresh EditorApp's (the preferences themselves are cvars, untouched),
    // and the camera counts as NOT restored. The SceneOpen framing request is
    // deliberately left alone: the reload's ReadLine cancels it when the
    // incoming file carries a camera, exactly as at boot. The --view-mode seed
    // is re-applied (THE FLAG BEATS THE INI, see ReadLine): without it an
    // incoming file with no [EditorViewport] block would drop the flag's mode
    // while a file WITH one keeps it.
    void EditorApp::ViewportSettingsClearAll(ImGuiContext*, ImGuiSettingsHandler* handler)
    {
        auto* self = static_cast<EditorApp*>(handler->UserData);
        self->m_camera                = decltype(self->m_camera){};
        Arcane::Editor::ApplyFreshPose(self->m_camera);   // editor.camera.default* (NextWorld)
        self->m_legacyViewport        = {};
        self->m_cameraRestoredFromIni = false;
        Arcane::Editor::ApplyViewModeSeed(self->m_config.viewMode, self->m_camera);
        // editor.gizmo.default* (NextWorld, Pref-P): the incoming project's
        // starting tool / mode / space, then --tool on top as at boot.
        // RetargetLayoutIni runs after OnProjectOpened, so the snapshot
        // already carries the incoming project's User rung.
        self->ApplyGizmoSessionDefaults();
        self->ApplyGizmoToolSeed();
    }

    void EditorApp::ApplyGizmoSessionDefaults()
    {
        Arcane::Editor::GizmoSessionState state;
        Arcane::Editor::ApplyGizmoSessionDefaults(state);
        m_gizmoMode    = state.mode;
        m_gizmoSpace   = state.space;
        m_gizmoEnabled = state.enabled;
    }

    void EditorApp::ApplyGizmoToolSeed()
    {
        Arcane::Editor::GizmoSessionState state{ m_gizmoMode, m_gizmoSpace, m_gizmoEnabled };
        Arcane::Editor::ApplyGizmoToolSeed(m_config.tool, state);
        m_gizmoMode    = state.mode;
        m_gizmoEnabled = state.enabled;
    }

    void EditorApp::RegisterViewportSettings()
    {
        if (ImGui::GetCurrentContext() == nullptr ||
            ImGui::FindSettingsHandler(Arcane::Editor::ViewportSettings::kIniType) != nullptr)
            return;

        ImGuiSettingsHandler handler;
        handler.TypeName   = Arcane::Editor::ViewportSettings::kIniType;
        handler.TypeHash   = ImHashStr(Arcane::Editor::ViewportSettings::kIniType);
        handler.UserData   = this;   // one EditorApp per process (see m_legacyPlayMode's decl)
        handler.ReadOpenFn = &EditorApp::ViewportSettingsReadOpen;
        handler.ReadLineFn = &EditorApp::ViewportSettingsReadLine;
        handler.WriteAllFn = &EditorApp::ViewportSettingsWriteAll;
        handler.ClearAllFn = &EditorApp::ViewportSettingsClearAll;
        ImGui::AddSettingsHandler(&handler);
    }

    namespace
    {
        constexpr const char* kPanelsIniType = "EditorPanels";
        constexpr const char* kPanelsIniName = "Visibility";
    }

    void* EditorApp::PanelVisibilitySettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler,
                                                     const char* name)
    {
        return std::strcmp(name, kPanelsIniName) == 0 ? handler->UserData : nullptr;
    }

    void EditorApp::PanelVisibilitySettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*,
                                                    void* entry, const char* line)
    {
        auto* self = static_cast<EditorApp*>(entry);
        // Unknown names and junk are ignored (never trust a hand-edited ini);
        // missing lines keep the default (visible).
        if (const auto parsed = Arcane::Editor::ParsePanelVisibilityLine(line))
            self->m_panelVis.visible[static_cast<std::size_t>(parsed->first)] = parsed->second;
    }

    void EditorApp::PanelVisibilitySettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
                                                    ImGuiTextBuffer* buf)
    {
        auto* self = static_cast<EditorApp*>(handler->UserData);
        buf->reserve(buf->size() + 128);
        buf->appendf("[%s][%s]\n", handler->TypeName, kPanelsIniName);
        for (const Arcane::Editor::PanelInfo& p : Arcane::Editor::kPanels)
            if (!p.permanent)
                buf->appendf("%s=%d\n", p.name,
                             self->m_panelVis.visible[static_cast<std::size_t>(p.id)] ? 1 : 0);
        buf->append("\n");
    }

    // ImGui::ClearIniSettings (a windowed project switch, RetargetLayoutIni):
    // every panel back to a fresh EditorApp's visibility -- the configured
    // editor.layout.openPanelsAtStart ("*" = all, today's default).
    void EditorApp::PanelVisibilitySettingsClearAll(ImGuiContext*, ImGuiSettingsHandler* handler)
    {
        auto* self = static_cast<EditorApp*>(handler->UserData);
        self->m_panelVis = Arcane::Editor::ParseOpenPanels(Arcane::Editor::cvar_layoutOpenPanelsAtStart.Get());
    }

    void EditorApp::RegisterPanelVisibilitySettings()
    {
        if (ImGui::GetCurrentContext() == nullptr ||
            ImGui::FindSettingsHandler(kPanelsIniType) != nullptr)
            return;

        ImGuiSettingsHandler handler;
        handler.TypeName   = kPanelsIniType;
        handler.TypeHash   = ImHashStr(kPanelsIniType);
        handler.UserData   = this;
        handler.ReadOpenFn = &EditorApp::PanelVisibilitySettingsReadOpen;
        handler.ReadLineFn = &EditorApp::PanelVisibilitySettingsReadLine;
        handler.WriteAllFn = &EditorApp::PanelVisibilitySettingsWriteAll;
        handler.ClearAllFn = &EditorApp::PanelVisibilitySettingsClearAll;
        ImGui::AddSettingsHandler(&handler);
    }

    // [EditorInspector][Instances] (the Inspector instance ID list) lives
    // beside the Inspector windows: RegisterInspectorInstancesSettings,
    // Panels/InspectorWindows.cpp (arc-1 debt F moved it there so the test
    // exe can drive it on a bare context).

    // ---- Boot stages (Task 8: EditorApp::Init folded into CoreStages) -------
    // Each method below is one block lifted verbatim (or near-verbatim; noted
    // where not) out of the old monolithic Init(). Run() wires each into the
    // matching id on the vector Arcane::HostBoot::EditorStages(ctx) returns --
    // see that function's header comment in ProjectBoot.cpp for why the body
    // has to live here rather than in Arcane.dll (host-owned members,
    // editor-exe-only types).

    bool EditorApp::StageRuntimeCreate(Arcane::HostBoot::BootContext& ctx)
    {
        // ProcessContext is the process's ONE instance (spec 2026-09-15 s3), owning
        // the TypeContext every module shares -- ArcaneEditor.exe, ArcaneClient.dll,
        // ArcaneCore.dll, and every loaded plugin. An OWNED TypeContext is leaked on
        // purpose (~ProcessContext): TypeMeta entries registered by the plugin (via
        // ASTRA_REFLECT in Components.hpp) hold std::function thunks compiled into
        // the plugin DLL, and after PluginHost::Unload -> DLClose those thunks point
        // to unmapped memory, so destructing the TypeContext would crash. Heap-
        // leaking is the correct production pattern for a long-running host; the OS
        // reclaims all process memory on exit anyway.
        m_process = Arcane::ProcessContext::Create({});
        if (!m_process) { ARC_ERROR("ArcaneEditor: ProcessContext refused -- a second host in this process?"); return false; }
        // This exe's OWN per-module Astra slot (unchanged reasoning: the slot is per
        // module). Required BEFORE the gizmo interaction code's
        // TypeID<Arcane::Transform>::Value() lookups (Registry::GetComponent<Transform>
        // in the frame loop) -- without this, ArcaneEditor.exe's first TypeID<T>::Value()
        // call would silently fall back to its own empty module-local
        // DefaultTypeContext() instead of the shared one, so GetComponent<Transform>
        // would resolve against the WRONG ComponentID (always-miss at best, aliasing a
        // different component's bytes at worst). ArcaneCore.dll's slot is installed by
        // ProcessContext::Create itself.
        Astra::SetTypeContext(&m_process->TypeContext());
        // NOW, and not a line earlier (2026-09-16): EditModeSchedule's ctor resolves
        // the TransformPropagationSystem's component mask, and TypeID<T>::Value()
        // caches per module for the process's life -- as a plain EditorApp member it
        // ran before the install above and pinned this exe to its own private ids.
        // See EditorApp.hpp's declaration comment for the incident.
        m_editSchedule.emplace();
        // Opt into a real audio device only for an INTERACTIVE run (maxFrames == 0 = run
        // until quit). The scripted "ArcaneEditor --frames N" GPU-verify is not interactive
        // -> false -> miniaudio's device-less null backend (no real device grabbed on a CI box).
        m_runtime.emplace(*m_process, m_config.maxFrames == 0);
        // The user cvar archive (T3-D2, Runtime.hpp): an INTERACTIVE windowed
        // session writes the open project's User-rung Archive cvars (the pin
        // legend's fold, the undo budgets, ...) back to its Saved/Config/ on a
        // project switch and at exit. A scripted --frames or --headless run
        // reads them but never writes: a verify run must not rewrite them.
        const bool interactive = m_config.maxFrames == 0 && !m_config.headless;
        m_runtime->Core().SetUserCVarArchiving(interactive);
        // The EditorUser rung (settings arc S2): machine-wide preferences (theme,
        // fonts, shortcuts, layouts) for an INTERACTIVE session only. A scripted
        // or golden run must not depend on, or write, this machine's preferences.
        if (interactive)
            m_runtime->Core().SetEditorUserConfigDir(Arcane::Paths::Get(Arcane::Paths::Location::EditorUserDir) / "Config");
        // THIS EXE asks about ITS OWN caches (2026-09-16). VerifySharedTypeContext
        // is inline, so it only ever answers for the module holding the call --
        // ProjectBoot.cpp's type_context_install stage compiles into
        // ArcaneClient.dll and speaks for ArcaneClient.dll alone. Nothing had ever
        // asked ArcaneEditor.exe about ArcaneEditor.exe, which is why the editor's
        // early WorldTransform resolve (EditModeSchedule, constructed below now,
        // a plain member before) was invisible to the check even in principle.
        // Fatal: a wrong id here means every view in this exe reads the wrong bytes.
        if (!Arcane::HostBoot::VerifySharedTypeContext(m_runtime->Registry(), "ArcaneEditor.exe"))
        {
            ARC_ERROR("ArcaneEditor: refusing to boot -- this exe is not on the engine's "
                      "Astra TypeContext (see the per-type errors above)");
            return false;
        }
        // Edit mode's only physics (spec 2026-09-11-physics-2d-wiring s6.1):
        // through `this` rather than a captured Runtime*, so a later runtime
        // re-creation on project switch keeps the binding valid.
        m_editSchedule->SetPhysicsEditPass([this] { m_runtime->PhysicsEditPass(); });

        // Populate ctx for the SHARED type_context_install / project_open /
        // input_config / editor_lock stage bodies (ProjectBoot.cpp), which only
        // have `ctx`, not `this` -- "stages populate as they go".
        ctx.runtime = &m_runtime->Core();
        return true;
    }

    bool EditorApp::StageGpuCore(Arcane::HostBoot::BootContext& ctx)
    {
        // The whole platform/render/input stack, booted in order. Owned by m_gpu and
        // declared BEFORE m_runtime/m_plugin in EditorApp -- so it destructs AFTER
        // them: the render resources it owns (window, batcher, imgui, input)
        // must outlive runtime + plugin.
        //
        // THE BOOT-PATH SPLIT, mirroring RuntimeApp::StageGpuCore line for
        // line. GpuContext::Create builds the window, a device-less Batcher2D,
        // the ImGuiLayer and the input stack -- AND NO GRAPHICS DEVICE. Main()
        // then builds the NriGraphContext PAIR that owns the process's ONLY
        // graphics device: the chrome one over this same window, the viewport
        // one offscreen on that same device.
        m_gpu = GpuContext::Create(m_config);
        if (!m_gpu)
        {
            ARC_ERROR("Arcane Editor: GPU context create failed");
            return false;
        }

        ARC_INFO("{} -- Arcane Editor host, backend {}", Arcane::BuildInfo(),
                 Arcane::ToString(m_config.backend));
        ctx.gpu = m_gpu.get();

        // Capture the editor's own ImGui context right after GpuContext::
        // Create built it -- the ONLY ImGui context in existence at this
        // point (the plugin has not loaded yet; StageEditorShell asserts
        // against this, see its own body).
        m_editorImguiContext = ImGui::GetCurrentContext();

        // NO PRESENTER IS CONSTRUCTED HERE. BootSequence's per-stage pump is
        // driven by the pre-device splash (Arcane::BootSplashPresenter, bound
        // for the whole Run() call below) for the ENTIRE boot -- this host has
        // no swapchain-backed presenter at all, so there is no "next automatic
        // present()" that could land on an unready ImGui context.
        return true;
    }

    bool EditorApp::StageEditorFonts(Arcane::HostBoot::BootContext&)
    {
        // Editor fonts: Roboto base + merged lucide icons, on the editor context
        // (the only ImGui context created so far, see StageGpuCore's GpuContext::
        // Create -> ImGuiLayer::Create), before the first frame and before the
        // game ImGui context is created in StageRenderBridge. Zero engine change.
        Arcane::Editor::InstallEditorFonts();
        Arcane::Editor::SetActiveKeyLayout(&Arcane::SystemKeyLayout());
        return true;
    }

    bool EditorApp::StageEditorShell(Arcane::HostBoot::BootContext&)
    {
        // Regression tripwire (2026-07-30 review, Fix 1's verification
        // requirement): everything below touches ImGui global state (fonts,
        // style, ConfigFlags, settings handlers) and MUST land on the
        // editor's own context, captured in StageGpuCore right after
        // GpuContext::Create built it. If this ever fires, some boot-stage
        // reordering let plugin code (PluginHost::Load -> the plugin's Init,
        // e.g. Sandbox.cpp:102's ImGui::SetCurrentContext) run before this
        // stage again -- see EditorStages' ordering comment in
        // ProjectBoot.cpp for the full incident this guards against.
        // PluginHost itself now also brackets every entry point against
        // leaking a context change (Fix 3, PluginHost.cpp) -- this assert is
        // the second, independent line of defense, verifying the OUTCOME
        // rather than trusting that guard alone.
        ARC_ASSERT(ImGui::GetCurrentContext() == m_editorImguiContext,
                   "StageEditorShell is not running on the editor's own ImGui "
                   "context -- something switched GImGui (a plugin's Init?) "
                   "before this stage ran. See ProjectBoot.hpp's EditorStages "
                   "ordering comment.");

        // Title the window as the editor. GpuContext defaults to "Arcane Runtime" (the
        // shared host helper ArcaneRuntime also uses); override it here so only this host
        // reads "Arcane Editor" -- ArcaneRuntime keeps its own title.
        //
        // Updates m_windowTitle too, not just the OS title (2026-07-30 review,
        // Fix 4): UpdateWindowTitle's `if (title == m_windowTitle) return;`
        // (below) compares against this cache, and StageFinalize's own
        // UpdateWindowTitle() call runs at a DIFFERENT point in the boot
        // sequence depending on how editor_shell and finalize happen to be
        // scheduled. If this call set the OS title WITHOUT updating the
        // cache, whichever of these two stages ran SECOND would either
        // silently stomp the other's title (cache never noticed the OS title
        // changed under it) or -- if finalize's real title had already been
        // cached first -- the OS title would get stomped to "Arcane Editor"
        // by this line and then STAY that way forever, since the cache still
        // held the (now wrong) real title and every subsequent per-frame
        // UpdateWindowTitle call (EditorAppFrame.cpp) would recompute the
        // same real title, compare equal to the stale-but-matching cache,
        // and never call SetTitle again. Keeping cache and OS title in sync
        // at every write site makes this self-correcting on the very next
        // frame regardless of which stage runs first.
        m_windowTitle = "Arcane Editor";
        m_gpu->Win().SetTitle(m_windowTitle);

        // Editor branding (Arcane Editor only -- ArcaneRuntime does neither): the Arcane logo as the
        // OS window/taskbar icon, and the SAME art as a display-referred texture for the
        // transport toolbar's top-left mark (Unity-style). One shared source PNG, copied
        // next to the exe at build. A missing file degrades quietly (default icon / no mark).
        constexpr const char* kLogoPath = "data/images/arcane_logo.png";
        m_gpu->Win().SetIcon(kLogoPath);
        // maxSize 64 ~= 2x the toolbar mark's on-screen size (~32px = 1.35x the button row):
        // the area-average makes a clean 64px texture, then ImGui's own bilinear only minifies
        // ~2x (a clean box). A much larger texture would leave ImGui doing a >2x single-tap
        // minify -> the aliased outline the raw 550px source produced.
        //
        // THE TOOLBAR MARK IS NOT SET HERE, because no device exists yet --
        // CreateGraphVehicles decodes the same PNG at the same maxSize
        // through LoadDisplayPixels and uploads it through the chrome
        // context's NriTextureCache. It cannot happen HERE because that
        // context does not exist yet: this stage runs inside boot, and the
        // graph vehicles are built strictly after it (Main -> CreateGraphVehicles).
        // The toolbar reads ToolbarLogoTextureId, and 0 is the
        // degraded-but-never-broken "no mark" path a missing PNG takes.

        // Editor shell: enable ImGui docking (the placeholder single window becomes
        // a dockspace + panels in MainLoop) and route the engine logger into the
        // Console panel's ring buffer.
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // The editor's own look, on the editor's context and nothing else: the
        // three-tone monochrome ramp with near-black field wells (EditorTheme.hpp).
        // Before this call the editor ran on ImGui's stock dark style, whose whole
        // interactive family is bright blue. It must run before the first frame --
        // ImGuiStyle is read live during widget submission, not latched.
        // The style metrics are the published editor.ui block (settings S6-28):
        // the early config rungs have run, so a saved value lands at boot.
        const Arcane::Editor::EditorUiStyleSettings& uiStyle = Arcane::Settings<Arcane::Editor::EditorUiStyleSettings>();
        Arcane::Editor::ApplyEditorTheme(ImGui::GetStyle(), uiStyle);
        m_appearance.Init(ImGui::GetStyle(), uiStyle);   // the boot look IS the defaults: the first per-frame update applies nothing
        // editor.graph.nodePadding is Restart: the shader canvas reads THIS
        // capture (the same early-rung point as the style above) until the
        // next restart, never the live snapshot.
        Arcane::Editor::CaptureGraphNodePaddingAtBoot();
        // The ini handlers register HERE -- after the context exists (GpuContext::
        // Create's ImGuiLayer::Create in StageGpuCore) and before the first
        // NewFrame, which is where ImGui reads the ini; a handler added later
        // would never see the saved entry.
        RegisterPlayModeSettings();
        // A fresh layout shows editor.layout.openPanelsAtStart; a saved ini's
        // [ArcaneEditor][Panels] entry then overrides it ("*" = today's all-visible).
        m_panelVis = Arcane::Editor::ParseOpenPanels(Arcane::Editor::cvar_layoutOpenPanelsAtStart.Get());
        RegisterPanelVisibilitySettings();
        Arcane::Editor::RegisterInspectorInstancesSettings(m_inspectorHost);
        // Inspector filters s6: the Asset Browser's selection is a PERMANENT
        // source -- registered once, never closed, and kept across a project
        // switch's ReleaseAll (its history entries and pins still drop).
        m_inspectorHost.AddSource(m_assetSource, /*permanent*/ true);
        // editor.camera.default* (NextWorld): the boot camera's pose, before
        // the handler below lets a saved [EditorViewport] block restore one.
        Arcane::Editor::ApplyFreshPose(m_camera);
        // editor.gizmo.default* (NextWorld): the tool, mode and space this
        // session starts in. Session state from here on (W/E/R, the toolbar)
        // until a windowed project switch re-applies them (ViewportSettingsClearAll).
        ApplyGizmoSessionDefaults();
        RegisterViewportSettings();
        // Settings arc S4: Preferences > Appearance > Theme.
        m_themePage.requestImport = [this]
        {
            m_gpu->Win().ShowOpenFileDialog(&PathPickedThunk,
                new PathDialogRequest{ &m_dialogs.themeImport, m_dialogs.themeImport.Arm() }, "Arcane Theme", "arctheme");
        };
        m_themePage.requestExport = [this]
        {
            m_gpu->Win().ShowSaveFileDialog(&PathPickedThunk,
                new PathDialogRequest{ &m_dialogs.themeExport, m_dialogs.themeExport.Arm() }, "Arcane Theme", "arctheme");
        };
        Arcane::Editor::RegisterSettingsPage(Arcane::SettingScope::PreferencesMachine, "Appearance/Theme", "Theme",
                                             &Arcane::Editor::DrawThemePage, &m_themePage);
        // Settings arc S4: Preferences > Keyboard (the editor.keys.* cvars live on this node).
        Arcane::Editor::RegisterSettingsPage(Arcane::SettingScope::PreferencesMachine, "Keyboard", "Keyboard Shortcuts",
                                             &Arcane::Editor::DrawShortcutsPage, &m_shortcutsPage);
        // Settings arc S4: Preferences > Appearance > Fonts and Scale (the editor.ui.* cvars live on this node).
        Arcane::Editor::RegisterSettingsPage(Arcane::SettingScope::PreferencesMachine, std::string(Arcane::Editor::kFontsPageCategory),
                                             "Fonts and Scale", &Arcane::Editor::DrawFontsPage, &m_fontsPage);
        Arcane::Editor::RegisterSettingsPage(Arcane::SettingScope::PreferencesMachine, "Layout", "Layouts",
                                             &Arcane::Editor::DrawLayoutPage, &m_layoutPage);

        // Does NOT construct or bind the swapchain-backed m_presenter (Task
        // 8c, 2026-07-30 correction): that presenter's ImGui::NewFrame() now
        // happens for the first time ever inside StageSplashReady, which runs
        // strictly after finalize -- i.e. after fonts/theme/docking-flag/
        // settings-handlers above AND everything else boot does. The old
        // concern this comment used to describe (binding too early lets the
        // NEXT automatic present() call reach an unready ImGui context) no
        // longer applies: BootSequence's per-stage pump is driven by the
        // pre-device splash for this entire run (see Run() below), which
        // never touches ImGui at all.
        return true;
    }

    bool EditorApp::StageRenderBridge(Arcane::HostBoot::BootContext&)
    {
        // THERE IS NO RENDER-RESOURCES BRIDGE -- same as
        // RuntimeApp::StageRenderBridge states. A plugin is handed no graphics
        // device and no shader library; Runtime has no SetRenderResources at
        // all.
        //
        // The Assets facade stays device-less either way, and Assets::PixelsFor
        // is the retained, device-FREE decode the graph's NriTextureCache uploads
        // from, so a textured sprite still renders. The stage is kept for the
        // game-context ImGui handoff below.

        // The hosted plugin draws its debug UI into its OWN "game" ImGui context,
        // composited INTO the viewport texture (see MainLoop), instead of the
        // editor context where a HUD would float over the editor chrome. Created
        // here, AFTER the editor ImGui layer is up (StageGpuCore) and BEFORE
        // the plugin is loaded/adopts it in StagePluginLoad. (Create leaves
        // the current ImGui context null; harmless -- the editor's ImGuiLayer
        // re-pins its own context on every BeginFrame/WantCapture*.)
        //
        // ONE FACTORY: OffscreenImGuiLayer::Create() builds a bare CONTEXT --
        // own atlas, io.IniFilename = nullptr, the same pinning discipline in
        // every entry point -- and no renderer. The renderer is a node inside
        // the viewport's frame graph (ImGuiNriNode over FrameDesc::gameUi).
        //
        // NON-OPTIONAL: see the SetImGui block below for what handing the
        // plugin the EDITOR's context instead would cost.
        m_gameImgui = Arcane::OffscreenImGuiLayer::Create();
        if (!m_gameImgui)
        {
            ARC_ERROR("Arcane Editor: OffscreenImGuiLayer creation failed");
            return false;
        }

        // ABI v2: install the ImGui context + allocators on the Runtime BEFORE the
        // plugin loads. PluginHost::RefreshContext copies these into the EngineContext
        // at Init time, so the plugin's Init adopts THIS GImGui across the DLL boundary.
        // The context is the GAME context (not the editor's), so the plugin's DrawUI
        // renders into the viewport pass in MainLoop.
        {
            // Same process-global ImGui allocators the editor context uses (ImGui's
            // allocator functions are global, not per-context); only the context
            // pointer differs from the editor redirect.
            ImGuiMemAllocFunc allocFn = nullptr; ImGuiMemFreeFunc freeFn = nullptr; void* ud = nullptr;
            ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &ud);
            // ===== THE GAME CONTEXT, UNCONDITIONALLY =====
            // There is deliberately NO `m_gameImgui ? Context() :
            // m_editorImguiContext` fallback here.
            //
            // WHAT SUCH A FALLBACK WOULD COST -- and it is NOT only "the HUD
            // draws over the chrome". OffscreenImGuiLayer sets
            // io.IniFilename = nullptr on the game context ("no imgui.ini for
            // the game context", OffscreenImGuiLayer.cpp) -- while the EDITOR
            // context's IniFilename is the PER-PROJECT LAYOUT FILE
            // (RetargetLayoutIni, below). A plugin window submitted through the
            // fallback would have had its position, size and dock node
            // PERSISTED into the user's project layout, permanently, and per-id
            // ini state silently overrides authored UI on every later boot --
            // the imgui.ini veto class, which is the worst kind of bug this
            // file can produce because it survives a rebuild.
            //
            // Holding that line by CONTROL FLOW -- gating the call site that
            // submits plugin windows -- is a weaker guarantee than holding it
            // by STRUCTURE. It is structural here: there is no path on which
            // the plugin sees m_editorImguiContext.
            m_runtime->SetImGui(m_gameImgui->Context(),
                                reinterpret_cast<void*>(allocFn),
                                reinterpret_cast<void*>(freeFn),
                                ud);
        }

        // Scene-in-a-panel viewport vehicle. The device is up by here in both
        // the interactive host and a `--frames N` run (which only
        // differs in the audio backend).
        //
        // THIS STAGE CREATES NOTHING. The
        // offscreen NriGraphContext that is the viewport vehicle
        // (ViewportTargets::graph) borrows the NriDevice the CHROME context
        // owns, and that context cannot exist yet: its swapchain must be
        // built over an already-Show()n window, which is strictly after boot.
        // Main() -> CreateGraphVehicles() builds both, in that order, before
        // the first frame. Nothing between here and there renders, so the gap
        // is not observable.
        //
        // sprite_tables is unaffected by that: its batcher is m_gpu->Batch()
        // -- the device-less Batcher2D GpuContext::Create built -- exactly as
        // RuntimeApp::StageSpriteTables does it.
        //
        // THE STAGE IS STILL INSTALLED even though its body creates nothing,
        // and removing it is its own change with its own blast radius:
        // "render_bridge" is pinned by name in ArcaneTests/src/
        // BootStageParityTest.cpp's id list (:118), and BOTH sprite_tables and
        // plugin_load declare a dependsOn edge to it (ProjectBoot.cpp:238,
        // :239), so deleting the stage re-parents them.
        return true;
    }

    bool EditorApp::StageEditCore(Arcane::HostBoot::BootContext&)
    {
        // edit_core (2026-07-30 review, Fix 5): m_undo/m_editBinding need
        // nothing but m_runtime, so this stage depends only on runtime_create
        // -- moved OUT of the old monolithic StageSpriteTables, which gated
        // them behind project_open AND render_bridge for no reason and, being
        // Optional-policy at the time, could leave m_undo never constructed
        // while StageFinalize still unconditionally dereferenced it.
        //
        // Editor undo/redo history. The resolver re-reads Runtime::Registry()
        // EVERY call rather than capturing a Registry& up front: Runtime swaps
        // out the registry object on Play/Stop (PlaySession -> Runtime::
        // SnapshotRegistry/RestoreRegistry) and on plugin hot-reload, so a
        // cached reference would dangle. `rt` is a raw Runtime* into m_runtime
        // (stable across that swap -- only the registry INSIDE it is replaced),
        // safe because m_undo destructs before m_runtime (declaration order).
        m_undo.emplace([rt = &*m_runtime]() -> Astra::Registry& { return rt->Registry(); });
        m_undoLimitsApplied = Arcane::Editor::ReadUndoLimits();
        m_undo->SetLimits(m_undoLimitsApplied);

        // Structural-edit binding: whole-registry snapshot/restore through the
        // SAME Runtime the resolver reads, so the memento survives registry swaps.
        m_editBinding.snapshot = [rt = &*m_runtime]() -> std::vector<std::byte>
        {
            auto r = rt->SnapshotRegistry();
            return r.IsOk() ? std::move(*r) : std::vector<std::byte>{};
        };
        m_editBinding.restore = [rt = &*m_runtime](std::span<const std::byte> bytes)
        {
            return rt->RestoreRegistry(bytes);
        };
        return true;
    }

    bool EditorApp::StageSpriteTables(Arcane::HostBoot::BootContext&)
    {
        // Shader-editor services (Slice 5): the shared compile service, the
        // template source root, and the .arcmat -> ShaderEditorDocument routing.
        // A missing dxcompiler.dll degrades to a warn (documents show
        // status).
        //
        // THE DEBOUNCE IS A LIVE CLOCK DEPENDENCY, worth knowing before any
        // caller pins `frame.now`: ShaderCompiler::Submit stamps `readyAt =
        // now + debounce`, so a driver that holds `now` CONSTANT can never
        // cross it, IsIdle() never returns true, and anything waiting for the
        // compile service to settle spins until its own timeout. A caller that
        // pins the clock must zero the debounce too.
        m_shaderCompiler = std::make_unique<Arcane::ShaderCompiler>();
        if (!m_shaderCompiler->Initialize())
        {
            ARC_WARN("Arcane Editor: dxcompiler.dll unavailable -- material editing disabled");
        }
        m_shaderSources.AddRoot("data/shaders");
        const auto materialFactory =
            [this](const std::filesystem::path& p)
                -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto data = Arcane::LoadMaterialAsset(p);
                if (!data)
                    return nullptr;
                return std::make_unique<Arcane::Editor::ShaderEditorDocument>(
                    MakeDocServices(), p, std::move(*data));
            };
        // Peek: focus an already-open doc WITHOUT constructing a duplicate
        // (whose ctor would submit compiles on the live doc's coalesce keys).
        const auto materialPeek =
            [](const std::filesystem::path& p) -> Arcane::Guid
            {
                const auto data = Arcane::LoadMaterialAsset(p);
                return data ? data->id : Arcane::Guid::Nil();
            };
        m_documents.RegisterFactory(".arcmat", materialFactory, materialPeek);

        // GPU crash diagnostics arc, Task 10: .arcdiag -> CrashReportDocument
        // routing, registered right beside the .arcmat route above (same
        // factory+peek shape -- Diag::ReadFile stands in for LoadMaterialAsset,
        // both independently re-read the file, same as materialFactory/
        // materialPeek do today). CrashReportDocument is read-only (never
        // dirty); unlike the sprite/material routes it borrows one service,
        // OpenSourceAtLine (node-page phase s8.1).
        const auto crashReportFactory =
            [this](const std::filesystem::path& p)
                -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto envelope = Arcane::Diag::ReadFile(p);
                if (!envelope)
                    return nullptr;
                Arcane::Editor::CrashReportDocument::Services services;
                services.openSourceAtLine = [this](const std::filesystem::path& f, int line) { OpenSourceAtLine(f, line); };
                return std::make_unique<Arcane::Editor::CrashReportDocument>(p, std::move(*envelope), std::move(services));
            };
        const auto crashReportPeek =
            [](const std::filesystem::path& p) -> Arcane::Guid
            {
                const auto envelope = Arcane::Diag::ReadFile(p);
                return envelope ? envelope->guid : Arcane::Guid::Nil();
            };
        m_documents.RegisterFactory(".arcdiag", crashReportFactory, crashReportPeek);

        // Sprite-asset arc, Task 5: .arcsprite -> SpriteDocument routing,
        // registered right beside the .arcmat route above (same
        // factory+peek shape). `this`-captures resolve m_sprites at CALL
        // time, not here -- m_sprites itself isn't constructed until the
        // block below, but nothing calls Save() (the only path that reaches
        // invalidateSprite) until well after this stage returns, by which
        // point it exists. Without this route, a double-clicked/minted
        // .arcsprite hit DocumentHost's "no editor registered" warn-and-no-op
        // (DocumentHost.cpp:56) -- EditorAppFrame.cpp:1165 already calls
        // m_documents.OpenPath on a freshly minted sprite and expected this.
        const auto spriteFactory =
            [this](const std::filesystem::path& p)
                -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto data = Arcane::LoadSpriteAsset(p);
                if (!data)
                    return nullptr;
                Arcane::Editor::SpriteDocument::Services spriteDocServices;
                spriteDocServices.assets = &m_runtime->AssetsFacade();
                // The SAME shared stack MakeDocServices hands the material
                // documents (through the same DocumentUndo resolver) and the
                // Inspector/gizmo push to -- one editor-wide history, so Ctrl+Z
                // walks back through sprite field edits in the order they
                // happened alongside everything else. Null in Play (s3.3b).
                spriteDocServices.undo = [this]() { return DocumentUndo(); };
                spriteDocServices.assetRefs = &m_assetRefServices;
                // Evict-then-re-resolve on a sprite re-save. The no-gap
                // requirement (a frame must never render the 1x1 placeholder in
                // between) is the resolver's contract now, so this is one call:
                // SceneRenderResolver::InvalidateSprite.
                spriteDocServices.invalidateSprite = [this](const Arcane::Guid& g)
                {
                    if (m_resolver)
                        m_resolver->InvalidateSprite(g);
                };
                // The window's sprite image + texture line (final fix D): the
                // SAME chrome thumbnail seam and model the Asset Browser rows
                // read, looked up at call time (both outlive every document).
                spriteDocServices.resolveThumb = [this](const Arcane::Guid& g) -> std::uint64_t
                {
                    return m_assetServices.resolveAssetThumb ? m_assetServices.resolveAssetThumb(g) : 0;
                };
                spriteDocServices.assetName = [this](const Arcane::Guid& g) -> std::string
                {
                    const auto* e = m_assetModel.Find(g);
                    return e ? e->fileName : std::string{};
                };
                return std::make_unique<Arcane::Editor::SpriteDocument>(
                    std::move(spriteDocServices), p, std::move(*data));
            };
        const auto spritePeek =
            [](const std::filesystem::path& p) -> Arcane::Guid
            {
                const auto data = Arcane::LoadSpriteAsset(p);
                return data ? data->id : Arcane::Guid::Nil();
            };
        m_documents.RegisterFactory(".arcsprite", spriteFactory, spritePeek);

        // F2a, Task 9: .arcmesh -> MeshDocument routing, registered right
        // beside the .arcsprite route above (same factory+peek shape). The
        // preview-seam wiring (chromeGraph/hostConfig/retireGraphPreview)
        // is the same three fields MakeDocServices sets
        // for the shader documents (EditorAppProject.cpp:57-94, read there
        // for the full ordering argument) -- inlined here rather than shared,
        // because MeshDocument::Services is deliberately its own small
        // struct (not a DocServices), matching SpriteDocument::Services'
        // precedent of not reusing the shader editor's shape either.
        const auto meshFactory =
            [this](const std::filesystem::path& p)
                -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto data = Arcane::LoadMeshAsset(p);
                if (!data)
                    return nullptr;
                Arcane::Editor::MeshDocument::Services meshDocServices;
                meshDocServices.undo = [this]() { return DocumentUndo(); };   // null in Play (s3.3b)
                meshDocServices.assetRefs = &m_assetRefServices;
                // Evict-then-re-resolve on a mesh re-save OR an undo/redo,
                // the same one-call route the .arcsprite factory above takes
                // for its own asset: the no-gap requirement (no frame may
                // render between the erase and the re-resolve -- and for a
                // mesh that would mean drawing NOTHING, since there is no
                // placeholder) is SceneRenderResolver::InvalidateMesh's
                // contract, not this lambda's.
                meshDocServices.invalidateMesh = [this](const Arcane::Guid& g)
                {
                    if (m_resolver)
                        m_resolver->InvalidateMesh(g);
                };
                // The late-bound preview seam (s3.2), set UNCONDITIONALLY: a
                // mesh opened by --open-asset is built inside StageFinalize,
                // before CreateGraphVehicles makes the chrome context, so the
                // document resolves ChromeGraph() at each use and retries its
                // vehicle from Tick (the harvester's hs.chromeGraph precedent).
                meshDocServices.chromeGraph = [this] { return ChromeGraph(); };
                meshDocServices.hostConfig  = &m_config;
                meshDocServices.retireGraphPreview =
                    [this](std::unique_ptr<Arcane::NriGraphContext> v)
                {
                    RetireDocPreview(std::move(v));
                };
                return std::make_unique<Arcane::Editor::MeshDocument>(
                    std::move(meshDocServices), p, std::move(*data));
            };
        const auto meshPeek =
            [](const std::filesystem::path& p) -> Arcane::Guid
            {
                const auto data = Arcane::LoadMeshAsset(p);
                return data ? data->id : Arcane::Guid::Nil();
            };
        m_documents.RegisterFactory(".arcmesh", meshFactory, meshPeek);
        m_documents.RegisterFactory(".arcinput",
            [this](const std::filesystem::path& path) -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto doc = Arcane::Editor::InputActionsDocument::Open(path, [this]() { return DocumentUndo(); });   // null in Play (s3.3b)
                if (doc)
                    doc->SetOnSaved([this](const Arcane::Guid& g, const Arcane::InputActionAsset& a)
                    { if (IsDesignatedInputAsset(g)) m_pendingInputRepublish.emplace(g, a); });   // a non-designated save never displaces the designated one (Save All)
                return doc;
            },
            [](const std::filesystem::path& path) -> Arcane::Guid
            { return Arcane::Editor::InputActionsDocument::PeekGuid(path); });
        // Every opened document is an Inspector source until it closes
        // (inspector-ownership spec s3.1: "a source that closes releases").
        m_documents.SetObserver({
            [this](Arcane::Editor::EditorDocument& d) { m_inspectorHost.AddSource(d); },
            [this](Arcane::Editor::EditorDocument& d)
            {
                m_inspectorHost.RemoveSource(d);
                m_docSelectionEpochs.erase(&d);
                if (m_inspectorFocusedSource == &d) m_inspectorFocusedSource = nullptr;   // the latch is a raw pointer into m_documents
            } });

        // Scene asset resolution (sprite-resolution lift): ONE engine-side
        // service resolves everything a scene references into what the
        // submission path can bind -- sprites (.arcsprite -> texture/UVs/size/
        // pivot), sprite materials and the post chain (.arcmat -> a registered
        // batcher material / a bound post chain, published as PostChainDesc
        // bytes for the graph recorder). It owns the three caches, the asset
        // resolver, and the compile drain site; the editor keeps only the
        // compile SERVICE (documents submit through it) and hands the resolver
        // the document routing below.
        {
            Arcane::SceneRenderResolver::Services rs;
            rs.runtime  = &*m_runtime;
            // THE SCENE BATCHER (material binds): the device-less GpuContext
            // one, the only one there is. The lines below are
            // RuntimeApp::StageSpriteTables' gates verbatim, and for its
            // stated reasons: SpriteMaterialCache registers BYTES-ONLY
            // materials with a device-less batcher, PostChainCache publishes
            // its PostChainDesc without building any device-side chain, and
            // the BACKEND -- which selects the shader flavor the compiles
            // target -- comes from the config because there is no device to
            // ask.
            rs.batcher  = &m_gpu->Batch();
            // Only the backend is set: Services carries no device field.
            rs.backend  = m_config.backend;
            rs.compiler = m_shaderCompiler.get();
            rs.sources  = &m_shaderSources;
            // Open shader documents get first refusal on every drained result,
            // the order the editor has always used: a document's compiles and a
            // cache's compiles for the SAME asset ride disjoint coalesce keys,
            // so neither claims the other's.
            rs.consumeFirst = [this](const Arcane::ShaderCompileResult& r) -> bool
            {
                bool consumed = false;
                m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
                {
                    if (auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d))
                        consumed = doc->ConsumeResult(r) || consumed;
                });
                // Asset-manager Task 8: the thumbnail harvester rides the SAME
                // one drain site -- this process has exactly one, by design
                // (SceneRenderResolver.hpp:22-28) -- as a PASSIVE OBSERVER.
                // Its answer is deliberately NOT OR'd into `consumed`:
                // reporting a consume here would swallow the result before the
                // scene's own caches ever saw it. It cannot claim theirs
                // either, for the same reason a document cannot -- its jobs
                // ride their own disjoint coalesce keys (0x40/0x80, see
                // MaterialPreviewHarvester.cpp's ThumbStageKey and the
                // scheme-wide table in that class's header).
                if (m_materialThumbs)
                    m_materialThumbs->OfferCompileResult(r);
                return consumed;
            };
            // F2b Task 11: the mesh-albedo bindless resolution seam. Routed
            // through the VIEWPORT vehicle specifically, never ChromeGraph()
            // -- m_viewportTargets.graph is the ONE graph context whose
            // MeshNode ever draws the open scene (ChromeGraph() renders
            // editor UI only and declares no mesh scene at all). Looked up
            // live, at CALL time: this Services struct is built here, at
            // stage time, long before m_viewportTargets.graph exists (it is
            // (re)built per project, later) -- the same [this]-capture idiom
            // rs.consumeFirst just above already relies on.
            rs.resolveMeshAlbedoSlot = [this](const Arcane::Guid& g) -> std::uint32_t
            {
                Arcane::NriGraphContext* graph = m_viewportTargets.graph.get();
                return graph ? graph->ResolveMeshAlbedoSlot(g) : 0xFFFFFFFFu;
            };
            rs.invalidateMeshGeometry = [this](const Arcane::Guid& g)
            {
                if (Arcane::NriGraphContext* graph = m_viewportTargets.graph.get())
                    graph->InvalidateMeshGeometry(g);
            };
            m_resolver =
                std::make_unique<Arcane::SceneRenderResolver>(std::move(rs));
        }

        // Spec 2026-09-30 s4.2: the asset-reference cell's services, built
        // ONCE. Every callable reads app state at CALL time (a project switch,
        // a document opened during a boot stage). reveal/open only QUEUE into
        // m_assetPageActions: ConsumeAssetPanelActions performs them next
        // frame, because opening mid-draw would mutate DocumentHost's list
        // while a page from that list is being drawn. tombstoneName (T5
        // s7.12): a deleted asset's last name, from the session activity log.
        m_assetRefServices.model = &m_assetModel;
        m_assetRefServices.project = [this]() -> const Arcane::Project*
        { return m_runtime ? m_runtime->CurrentProject() : nullptr; };
        m_assetRefServices.resolveThumb = [this](const Arcane::Guid& g) -> std::uint64_t
        { return m_assetServices.resolveAssetThumb ? m_assetServices.resolveAssetThumb(g) : 0; };
        m_assetRefServices.canReveal = [this]
        { return m_panelVis.IsVisible(Arcane::Editor::PanelId::AssetBrowser); };
        m_assetRefServices.reveal = [this](const Arcane::Guid& g) { m_assetPageActions.revealInBrowse = g; };
        m_assetRefServices.open = [this](const Arcane::Guid& g) { m_assetPageActions.openAsset = g; };
        m_assetRefServices.mintSpriteForTexture =
            [this](const Arcane::Guid& textureGuid) { return MintOrReuseSpriteForTexture(textureGuid); };
        m_assetRefServices.tombstoneName = [this](const Arcane::Guid& g) { return Arcane::Editor::TombstoneName(m_assetActivity, g); };
        m_inspectorServices.assetRefs = &m_assetRefServices;

        // Asset-manager redesign, Plan 1 Task 7: the Assets panel's thumbnail
        // resolver, built here because ChromeGraph() doesn't exist yet at
        // this stage -- looked up LIVE at call time, the [this]-capture idiom
        // resolveMeshAlbedoSlot above relies on. Resolves through the CHROME
        // context's texture cache (OnCookCompleted's InvalidateContentTexture
        // call on the chrome context keeps it fresh -- no new cache here).
        // It is also the Asset Browser page's and the sprite document
        // window's thumbnail seam (the Inspector's old resolveTexturePreview
        // service lost its last reader with the scene page's texture
        // fallback and was deleted, final fix m3). Textures resolve directly;
        // sprites resolve through their single texture ref (FirstTextureRefOf);
        // materials route through Task 8's MaterialPreviewHarvester;
        // everything else is 0 -- the caller falls back to the kind icon.
        //
        // ===== TASK 8: THE HARVESTER ITSELF ==============================
        // Constructed HERE, at stage time, for the same reason the two
        // lambdas above are built here: everything device-adjacent it needs
        // (the chrome vehicle, its NriTextureCache, its ImGuiNriNode) is
        // reached through a live `[this]`-capture, so it does not need a
        // device to exist yet -- and it must exist before the FIRST project
        // opens, because OnProjectOpened is what primes it from disk.
        {
            Arcane::Editor::MaterialPreviewHarvester::Services hs;
            hs.chromeGraph = [this] { return ChromeGraph(); };
            hs.hostConfig  = &m_config;
            hs.compiler    = m_shaderCompiler.get();
            hs.sources     = &m_shaderSources;
            hs.backend     = m_config.backend;
            // Both seams re-read CurrentProject() per call, so they survive a
            // project switch -- the same shape ShaderEditorDocument's preview
            // installs on its own vehicle.
            hs.resolveAsset = [this](const Arcane::Guid& id)
                -> std::optional<std::filesystem::path>
            {
                const Arcane::Project* p = m_runtime ? m_runtime->CurrentProject() : nullptr;
                return p ? p->ResolveAsset(Arcane::AssetId::FromGuid(id)) : std::nullopt;
            };
            hs.pixelSupply = [this](const Arcane::Guid& id) -> const Arcane::PixelData*
            {
                return m_runtime ? m_runtime->AssetsFacade().PixelsFor(id) : nullptr;
            };
            // Saved/ is the project's scratch directory -- the same one the
            // Hub's cover screenshot (Saved/AutoScreenshot.png) already lives
            // in. Empty (no project) means "no persistence right now", which
            // the harvester reads as harvest-but-do-not-write.
            hs.thumbnailDir = [this]() -> std::filesystem::path
            {
                const Arcane::Project* p = m_runtime ? m_runtime->CurrentProject() : nullptr;
                return p ? Arcane::Paths::Join(Arcane::Paths::Location::ProjectSaved, Arcane::Paths::ForProject(p->Root()), "Thumbnails")
                         : std::filesystem::path{};
            };
            // Task 12a fix (RCA H1): mirror SceneRenderResolver.cpp's
            // MeshCache::Services wiring (meshArtifactFor/cookPending) verbatim,
            // off the same Assets facade every other lambda in this block
            // already reaches through m_runtime. Never wired here before, so
            // an Imported .arcmesh's mesh-ASSET branch (StartOneMesh ->
            // ResolveMeshData) always saw two null callbacks and permanently
            // took MeshAsset.cpp's "no cooked artifact" Failed branch --
            // an imported mesh's Asset Browser thumbnail could never resolve.
            hs.meshArtifactFor = [this](const Arcane::Guid& id) -> const Arcane::LoadedClientMesh*
            {
                return m_runtime ? m_runtime->AssetsFacade().MeshArtifactFor(id) : nullptr;
            };
            hs.cookPending = [this](const Arcane::Guid& id) -> bool
            {
                return m_runtime ? m_runtime->AssetsFacade().CookPending(id) : false;
            };
            m_materialThumbs =
                std::make_unique<Arcane::Editor::MaterialPreviewHarvester>(std::move(hs));
        }
        m_assetServices.resolveAssetThumb =
            [this](const Arcane::Guid& guid) -> std::uint64_t
        {
            Arcane::NriGraphContext* chrome = ChromeGraph();
            Arcane::NriTextureCache* cache = chrome ? chrome->Textures() : nullptr;
            if (!cache)
                return 0;
            const auto* e = m_assetModel.Find(guid);
            if (!e)
                return 0;
            Arcane::Guid tex;
            if (e->kind == Arcane::Editor::AssetKind::Texture)
                tex = guid;
            else if (e->kind == Arcane::Editor::AssetKind::Sprite)
                tex = FirstTextureRefOf(guid);
            else if (Arcane::Editor::ThumbnailEligible(e->kind))
            {
                // Task 8: a REAL rendered preview, not a texture lookup --
                // 0 until this asset has been harvested (the caller falls
                // back to the kind icon for that window). Task 10 (controller
                // ruling, 2026-09-06): on a miss, ALSO push a harvest request
                // right here -- "visible" is exactly what the Browse draw's
                // clipper resolved this frame, this seam is the one place
                // every thumbnail consumer (row, tooltip, later the preview
                // pane) funnels through, and Request()/RequestMesh() are
                // cheap no-ops once queued/harvested/given-up (dedupe +
                // give-up latch, MaterialPreviewHarvester.hpp's own doc
                // comment), so calling either from a tooltip/peek resolve too
                // is harmless.
                //
                // F2c Plan 2 Task 10 (spec s8): AssetKind::Model is
                // deliberately NOT ThumbnailEligible -- it has no material
                // assignment of its own, its appearance IS its companion
                // .arcmesh's, and harvesting both would spend two device
                // idles on two near-identical pictures (the ordinary shape
                // has the companion right beneath it, wearing the picture).
                // A Model row therefore falls straight through to the
                // kind-icon fallback below, `tex` never set, same as any
                // other ineligible kind.
                if (!m_materialThumbs)
                    return 0;
                const std::uint64_t id = m_materialThumbs->ThumbTextureId(guid);
                if (id != 0)
                    return id;
                if (e->kind == Arcane::Editor::AssetKind::Mesh)
                    m_materialThumbs->RequestMesh(guid);
                else
                    m_materialThumbs->Request(guid);
                return id;
            }
            if (!tex.IsValid())
                return 0;
            nri::Texture* t = cache->Resolve(tex, Arcane::NriTextureCache::ColorSpace::Display);
            return t ? (std::uint64_t)(std::intptr_t)t : 0;
        };
        return true;
    }

    // Asset-manager redesign, Plan 1 Task 7: see this method's own
    // declaration (EditorApp.hpp) for the "exactly one ref" contract this
    // relies on. m_assetPanelProviders is the SAME cached facade seam
    // AssetPanelModel::RebuildIfDirty drives (MakeAssetPanelProviders,
    // EditorAppProject.cpp) -- reused here rather than calling
    // m_runtime->AssetsFacade() a second time, so there is exactly one path
    // from a guid to its outgoing refs in the whole editor.
    Arcane::Guid EditorApp::FirstTextureRefOf(const Arcane::Guid& guid) const
    {
        if (!m_assetPanelProviders.refsFor)
            return Arcane::Guid{};
        const std::optional<std::vector<Arcane::AssetRef>> refs = m_assetPanelProviders.refsFor(guid);
        if (!refs || refs->empty())
            return Arcane::Guid{};
        return (*refs)[0].target;
    }

    bool EditorApp::StagePluginLoad(Arcane::HostBoot::BootContext&)
    {
        if (const Arcane::Project* project = m_runtime->CurrentProject())
        {
            const auto input = Arcane::HostBoot::LoadGameplayInput(*m_runtime, *project);
            if (input.status == Arcane::HostBoot::GameplayInputLoadResult::Status::Invalid)
            {
                ARC_ERROR("Arcane Editor: gameplay input failed to load: {}", input.diagnostic);
                m_modalErrors.Push("Gameplay Input Invalid", input.diagnostic);
            }
        }
        else
        {
            m_runtime->GameInput().Clear();
        }
        // The editor loads a game module only when one is specified -- a project's
        // gameModule, or an explicit --plugin. Bare `ArcaneEditor` (no --project, no
        // --plugin) starts with NO game loaded (an empty editor) rather than the physics
        // Sandbox: pluginPath defaults empty (HostConfig), so GameModule returns empty
        // here and the plugin host is left disengaged. Every m_plugin-> use in MainLoop
        // is optional-guarded, so a disengaged plugin is safe. Sandbox stays available on
        // demand via --plugin Sandbox.dll or --project ReferenceProject.
        const std::string gameModule =
            Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
        const auto pluginModules = Arcane::HostBoot::PluginModules(m_runtime->CurrentProject());
        if (!gameModule.empty() || !pluginModules.empty())
        {
            // A game module OR just project plugin modules is enough to host: an empty
            // gameModule makes a plugins-only host (open a plugin-only project to work on it
            // before its game DLL exists). PluginHost handles the primary-less case.
            // ABI 30: built on the ProcessContext; the editor's one world is ATTACHED
            // (and is therefore the primary -- the module's own world).
            m_plugin.emplace(*m_process,
                gameModule.empty() ? std::filesystem::path{} : std::filesystem::path(gameModule));
            m_plugin->AttachRuntime(m_runtime->Core());
            for (const auto& dll : pluginModules)
                m_plugin->AddPlugin(dll);
            if (!m_plugin->Load())
            {
                // Defined-state failure, not a bare stage-failed abort (2026-07-30
                // review, boot-corestages Task 9b): plugin_load is Optional for the
                // editor (ProjectBoot.cpp's EditorStages -- see that override's
                // comment for the full "Optional for the editor, Fatal for the
                // runtime" ruling), so a `return false;` here would no longer abort
                // boot, but it would still leave m_plugin holding a PluginHost whose
                // Load() failed partway through and only a generic "optional stage
                // 'plugin_load' failed; continuing" line in the log -- worse than
                // the Fatal path it replaced, which at least failed loudly even
                // though it never let the developer in to fix anything. Instead:
                // m_plugin.reset() leaves EXACTLY the same safe, disengaged state
                // the "no game module" branch below produces on purpose (every
                // m_plugin-> use in MainLoop is optional-guarded, per this
                // function's opening comment), and a detailed banner -- naming
                // the required ABI -- surfaces through m_modalErrors as a modal
                // (EditorAppFrame.cpp; "Open Project Failed" with a project open,
                // "Game Module Failed to Load" without one) once MainLoop
                // starts, rather than only a Console line. Since Task 12
                // (EditorAppProject.cpp) SwitchProject no longer has its own
                // switch_plugin_load stage -- it MOVES this exact StagePluginLoad
                // body (this whole function) and runs it verbatim, so a switch
                // failure hits this SAME branch rather than a mirrored copy.
                ARC_ERROR("Arcane Editor: failed to load the game module / project plugins");
                // The banner follows what was attempted: a bare --plugin launch
                // has no project, so it must not claim one opened.
                auto failure = Arcane::Editor::DescribePluginLoadFailure(
                    m_runtime->CurrentProject() != nullptr, gameModule,
                    static_cast<int>(Arcane::kGamePluginABIVersion));
                m_modalErrors.Push(std::move(failure.title), std::move(failure.body));
                m_plugin.reset();
            }
        }
        else
        {
            ARC_INFO("Arcane Editor: no --project/--plugin -- starting with no game loaded");
        }

        // Task 8: Arcane Editor boots in Edit mode -- the sim starts paused. Play (m_play)
        // unpauses it; Stop restores the snapshot and re-pauses.
        m_runtime->Loop().SetPaused(true);
        return true;
    }

    bool EditorApp::StageFinalize(Arcane::HostBoot::BootContext&)
    {
        // Surface a failed --project open as a blocking modal -- the console
        // line alone was missed twice. project_open's SHARED body (ProjectBoot.cpp)
        // already warned and kept booting project-less; this derives the same
        // fact from STATE rather than duplicating the OpenProject call:
        // Runtime::OpenProject leaves CurrentProject() null on any failure and
        // never clears an already-open one, so "a project was requested and
        // none is open now" is exactly "the open failed".
        if (!m_config.projectPath.empty() && m_runtime && !m_runtime->CurrentProject())
        {
            m_modalErrors.Push("Open Project Failed", "--project '" + m_config.projectPath +
                                 "' failed to open.\nRunning with the data/ + --plugin "
                                 "fallback instead (see Console).");
        }

        // Task 7's boot-scene handoff / EnsureScene / title / recents-record --
        // the project-open success tail. SwitchProject's plugin_load stage
        // (EditorAppProject.cpp, which since Task 12 reuses this exact
        // StagePluginLoad body -- there is no separate switch_plugin_load
        // stage anymore) calls this same OnProjectOpened function directly,
        // not a duplicated copy. See OnProjectOpened.
        OnProjectOpened();

        // Point ImGui's ini at this project's appdata layout file BEFORE the
        // first NewFrame reads it (MainLoop starts after boot) -- ImGui then
        // auto-loads the per-project layout on frame one. Not folded into
        // OnProjectOpened: the switch failure fallback also calls that tail
        // (recordRecents=false) but must retarget onto a DIFFERENT key (the
        // reverted/project-less state, not "this boot's project"), so every
        // call site keeps its own RetargetLayoutIni() immediately after.
        RetargetLayoutIni();
        // --view-mode (F4 plan 1 T7): seed the editor camera's mode AFTER the
        // layout ini has been targeted (and, under --headless, already read
        // by RetargetLayoutIni's explicit LoadIniSettingsFromDisk), so the
        // flag beats a persisted [EditorViewport][Camera] Mode. On a windowed
        // run the ini is read later still (the first NewFrame), which is why
        // ViewportSettingsReadLine re-applies this same seed per line. Empty
        // flag = no seed; every other spelling was refused at parse time.
        Arcane::Editor::ApplyViewModeSeed(m_config.viewMode, m_camera);
        // --select-name / --tool: scripted selection + viewport tool, AFTER
        // the boot scene is in the registry (OnProjectOpened above). A gizmo
        // capture is `--select-name MeshCube --tool move --view-mode perspective`.
        if (!m_config.selectName.empty() && m_runtime)
        {
            Astra::Entity found = Astra::Entity::Invalid();
            m_runtime->Registry().CreateView<Arcane::Identity>().ForEach(
                [&](Astra::Entity e, Arcane::Identity& id)
                {
                    if (id.name == m_config.selectName)
                        found = e;
                });
            if (!found.IsValid())
            {
                ARC_ERROR("--select-name '{}': no entity with that Identity.name in the boot scene",
                          m_config.selectName);
            }
            else
            {
                m_selection.Select(found);
                FrameCamera(/*selectionOnly=*/true);
            }
        }
        // --open-asset: a scripted document open, the Asset Browser's own
        // path (OpenAssetDocument -> DocumentManager::OpenPath), so a headless
        // capture can show an .arcinput / shader / mesh document without any
        // desk input. Same loudness rule as --select-name.
        if (!m_config.openAsset.empty())
        {
            const auto guid = Arcane::Guid::FromString(m_config.openAsset);
            if (!guid || !OpenAssetDocument(*guid))
            {
                ARC_ERROR("--open-asset '{}': not a Guid, not in the project, or no document opens it",
                          m_config.openAsset);
            }
            else
            {
                m_scriptedOpenFocusFrames = 3;   // see the member's comment
                // --select-in-document (inspector-ownership spec A s4): a
                // scripted selection INSIDE the document just opened. The
                // selection bumps the document's SelectionEpoch, which routes
                // it to the Inspector like a click would. Unresolvable = a
                // loud ERROR; the run still completes (the report then names
                // whatever the Inspector actually resolved to).
                if (!m_config.selectInDocument.empty())
                {
                    Arcane::Editor::EditorDocument* doc = m_documents.FindByGuid(*guid);
                    if (!doc || !doc->SelectByPath(m_config.selectInDocument))
                        ARC_ERROR("--select-in-document '{}': the opened document has no such path",
                                  m_config.selectInDocument);
                }
            }
        }
        // --select-asset (inspector filters s6): a scripted Asset Browser
        // selection. Validated through the PROJECT, the way OpenAssetDocument
        // does (EditorAppProject.cpp:182-192): the model is not rebuilt until
        // the first frame's DrawEditorUi, so its Find() misses every guid here.
        if (!m_config.selectAsset.empty())
        {
            const auto guid = Arcane::Guid::FromString(m_config.selectAsset);
            const Arcane::Project* p = m_runtime ? m_runtime->CurrentProject() : nullptr;
            if (!guid || !p || !p->ResolveAsset(Arcane::AssetId::FromGuid(*guid)))
                ARC_ERROR("--select-asset '{}': not a Guid or not in the project", m_config.selectAsset);
            else
                m_assetModel.Select(*guid);           // the asset edge routes it next frame, like a click
        }
        ApplyGizmoToolSeed();   // the flag beats editor.gizmo.default* (StageEditorShell)
        // Same call-site family (GPU crash diagnostics arc, Task 8): a crash/
        // hang report from THIS boot must land under THIS project's own
        // Saved/Diagnostics, not the exe-relative default a project-less
        // boot would otherwise leave armed. See RetargetDumpDir's own
        // comment below for the <project>/Saved/Diagnostics vs default split.
        RetargetDumpDir();

        // --play-as (Core-DLL split, plan 1 Task 7): the SCRIPTED half of the
        // play-mode picker. Last thing in boot, and that placement is the whole
        // correctness argument -- OnProjectOpened above has already loaded the
        // manifest's bootScene (HostBoot::BootScene) and run EnsureScene, so the
        // registry Play snapshots here IS the authored scene. Entering Play any
        // earlier would seed an embedded server world from a pre-scene registry
        // and then watch the editor's own world diverge from it.
        if (!m_config.playAs.empty())
        {
            using Topo = Arcane::Editor::PlayTopology;
            const Topo topo = m_config.playAs == "listen-server"   ? Topo::ListenServer
                             : m_config.playAs == "embedded-server" ? Topo::EmbeddedServer
                             : m_config.playAs == "client"          ? Topo::ClientOnly
                                                                    : Topo::Standalone;
            // HostConfig::Parse already refused every other spelling, so the
            // fall-through above is "standalone" and nothing else.
            m_documents.FlushGestures();
            if (!m_play.Play(m_runtime->Core(), m_plugin ? &*m_plugin : nullptr, topo))
                ARC_ERROR("--play-as {}: Play refused", m_config.playAs);
        }
        return true;
    }

    // The project-open SUCCESS tail (architecture pass sec 5) -- previously
    // duplicated verbatim across StageFinalize and a hand-rolled
    // switch_plugin_load stage, with a partial third copy in the switch
    // failure fallback. Now a single function called from all three sites:
    // StageFinalize, SwitchProject's plugin_load stage (which since Task 12
    // reuses this same StagePluginLoad body -- switch_plugin_load no longer
    // exists as a separate stage), and the switch failure fallback.
    // recordRecents=false is the fallback's case: it re-establishes the
    // PROJECT-LESS baseline (or the old project), and a refused open must
    // never reorder the recents lists.
    void EditorApp::OnProjectOpened(bool recordRecents)
    {
        RetargetUndoCache(m_runtime->CurrentProject());

        // Settings arc S3-12: Project Settings' identity page (idempotent: same path, replaced).
        Arcane::Editor::RegisterSettingsPage(Arcane::SettingScope::Project, "Project", "Project",
                                             &EditorApp::ProjectPageThunk, this);
        ConfigureSettings();

        // Build -> Open Visual Studio needs to know whether devenv exists
        // BEFORE its first draw (it greys with a tooltip otherwise); resolve
        // here, rather than spawning vswhere from the first menu draw. The
        // frame re-checks build.ideExecutable before each menu draw (Live).
        // See OpenInIde (EditorAppProject.cpp).
        RefreshDevenv();

        // Task 7: open into the project's boot scene, now that the plugin has
        // loaded (a scene naming a component the game module registers would
        // otherwise silently drop it) and m_undo exists (Adopt records the
        // clean baseline against it). A project with no boot scene, or one
        // that fails to resolve/load, keeps whatever the plugin's Init built --
        // code-spawned scenes are legacy, but nothing clears them unless a
        // scene actually takes ownership (BootScene already logged the reason).
        if (m_undo)
        {
            if (const Arcane::Project* proj = m_runtime->CurrentProject())
            {
                if (const auto boot = Arcane::HostBoot::BootScene(m_runtime->Core(), *proj))
                {
                    m_scene.Adopt(boot->file, boot->id, *m_undo);
                    // Cancelled again by ViewportSettingsReadLine if the
                    // persisted [EditorViewport][Camera] restores a transform
                    // (F3): the ini is read AFTER this on both boot paths.
                    m_editSchedule->RequestFrame(Arcane::Editor::FrameRequest::SceneOpen);
                }
            }
        }

        // F2b Task 12: the background texture cook -- per-project (its
        // CookSession's staleness memo and its projectDir are meaningless
        // across a switch), so it is constructed fresh here rather than
        // ever surviving into a different project. m_runtime always has a
        // JobSystem (Runtime::Jobs()); the SubmitFn just forwards to it.
        // ResetPerProjectState (the switch/close path) is what tears this
        // back down (m_cookQueue.reset()).
        if (const Arcane::Project* proj = m_runtime->CurrentProject())
        {
            // Desk-fix 2: arm the settling window BEFORE the queue exists, so
            // there is no gap between "m_cookQueue is live" and "the window
            // says still settling" that a resolve landing on this exact frame
            // could slip through.
            m_cookQueueSettling = true;

            m_cookQueue.emplace(proj->Root(),
                [this](std::function<void()> job) { m_runtime->Jobs().Submit(std::move(job)); },
                Arcane::Settings<Arcane::AssetPipeline::TextureMetaSettings>());   // S6-6: the project's import defaults
            m_cookQueue->SetOnCookComplete(
                [this](const Arcane::AssetPipeline::CookResult& r) { OnCookCompleted(r); });

            // Desk-fix 2 (revised, review fix): the Assets-facade half of the
            // cook-pending quiet seam (Assets.hpp's own doc comment on
            // SetCookPendingProbe has the full contract). Reinstalled on every
            // project open/switch -- harmless, since it is the SAME closure
            // logic reading live `this` state each time, not a fresh capture
            // of per-project data. `!m_cookQueue` guards the gap after
            // ResetPerProjectState has torn the queue down but before a NEW
            // project's OnProjectOpened reaches this point (a project-less
            // interval, or mid-switch) -- answering false there preserves the
            // "no probe installed" default (loud, as always) rather than
            // stalling a genuine refusal on stale state.
            //
            // POST-SETTLING, THIS IS A POSITIVE SIGNAL -- review finding
            // (critical, fixed here): the first cut fell through to
            // IsCookPending(id) here, which at the time answered "pending"
            // whenever m_cookDiagnostics had NO ROW for the guid. That
            // default was safe for NriTextureCache's oracle (a wrong guess
            // there only costs a checkerboard-vs-refused VISUAL choice, and
            // RefuseArtifact fires independently regardless), but it was NOT
            // safe reused HERE, because RefuseArtifact is what CREATES a row in the first
            // place (via OnArtifactRefused) -- gating RefuseArtifact itself
            // on a "no row means pending" default is a closed loop with no
            // exit for any guid CookSession will NEVER attempt (its source
            // renamed/deleted after registration, or a format
            // EnumerateTextureSources skips): no row ever forms, so it stays
            // "presumed pending" -- quiet, unmemoized, unlatched, un-reported
            // -- forever. That is a regression against "refusals stay loud"
            // for exactly the broken-reference case that matters most, so
            // row-ABSENCE must never imply pending here. Quiet now requires
            // an ACTUAL positive signal: the cook queue has active work
            // (CookQueue::CookPending() -- a pass submitted, running, or
            // marked dirty for a follow-up; see CookQueue.hpp) AND the guid's
            // OWN registered source still exists on disk right now (a
            // filesystem exists() call per ask -- cheap enough to pay
            // unconditionally: the only callers that ask about a guid stuck
            // in this branch at all are ones ResolveArtifact already found
            // Missing, and the highest-frequency one, NriTextureCache's own
            // PendingCook re-poll, already throttles ITS OWN calls into this
            // facade via assets.cook.pendingRepollInterval, so this exists() rides
            // an already-throttled ask, not a hot per-frame one). A deleted/
            // renamed source answers false here even while the queue is mid-
            // pass, which is exactly the fix: that guid refuses LOUDLY again,
            // matching pre-seam behavior. IsCookPending/SetCookPendingOracle
            // (the RENDER-layer oracle) stays INDEPENDENT of this closure --
            // and, since the 2026-09-08 desk fix, answers a row-less guid by
            // asking the ARTIFACT STORE rather than presuming anything (see
            // its own definition). This probe must still never be routed
            // through it: RefuseArtifact no longer depends on it for
            // anything, and keeping it that way is what stops the facade's
            // refusal path from asking the cook-pending seam to answer
            // itself.
            m_runtime->AssetsFacade().SetCookPendingProbe(
                [this](const Arcane::Guid& id)
                {
                    if (!m_cookQueue)
                        return false;
                    if (m_cookQueueSettling)
                        return true;   // (a) still-settling window

                    // (b) POSITIVE signal only, post-settling: active queue
                    // work AND the guid's own source still on disk. No queue
                    // activity at all -> nothing legitimately pending -> a
                    // Missing resolution refuses loudly, exactly pre-seam.
                    if (!m_cookQueue->CookPending())
                        return false;

                    const Arcane::Project* liveProj = m_runtime->CurrentProject();
                    if (!liveProj)
                        return false;
                    const auto sourcePath =
                        liveProj->ResolveAsset(Arcane::AssetId::FromGuid(id));
                    if (!sourcePath)
                        return false;   // not even registered -- nothing pending
                    std::error_code existsEc;
                    return std::filesystem::exists(*sourcePath, existsEc);
                });

            // Task 5 deferral, closed here (SweepArtifactOrphans' own
            // comment): once, at project open -- not on every cook pass.
            SweepArtifactOrphans();

            // ===== ASSET-MANAGER TASK 8: THE PERSISTED THUMBNAILS ==========
            // Load <project>/Saved/Thumbnails/<guid>.png straight into the
            // harvester's pixel supply, and queue a harvest ONLY for a
            // material with no PNG or one whose .arcmat is NEWER than it. For
            // an unchanged project that is ZERO harvests -- the whole point:
            // every harvest costs a ReadCapture device idle, and a naive
            // harvest-everything-at-open would pay N of them at every boot
            // (UE's package-header FObjectThumbnail + OnAssetSave frequency is
            // the same lesson, learned the same way).
            //
            // SAFE THIS EARLY, unlike the cook pass the block below refuses to
            // force here: PrimeFromDisk is pure CPU (a stat, a PNG decode) and
            // touches no device and no background job at all. The harvester's
            // own upload is lazy -- ThumbTextureId resolves the texture on
            // first ask, which is necessarily after CreateGraphVehicles.
            //
            // F2c Plan 2 Task 10 (spec s8): `materials` now ALSO carries the
            // project's ThumbnailEligible(Mesh) guids -- PrimeFromDisk's own
            // doc comment (MaterialPreviewHarvester.hpp) is what tells a
            // stale/missing mesh apart from a stale/missing material, off the
            // resolved source's extension, so a bare guid list is all this
            // call site needs to hand over. Model is not in this list: it is
            // not ThumbnailEligible (see resolveAssetThumb's Model comment
            // just above), so priming it would only ever queue a harvest this
            // asset can never serve a picture for.
            if (m_materialThumbs)
            {
                std::vector<Arcane::Guid> materials;
                for (const Arcane::Editor::AssetEntry& e :
                     Arcane::Editor::BuildAssetEntries(proj->Registry()))
                    if (Arcane::Editor::ThumbnailEligible(e.kind))
                        materials.push_back(e.guid);
                m_materialThumbs->PrimeFromDisk(materials);
            }

            // Desk-fix 2 does NOT force a CookQueue pass here, even though the
            // settling window above needs SOME pass to eventually close it
            // for a project with ZERO texture sources. A first cut did submit
            // one right here (JobSystem::Submit, fire-and-forget) and a desk
            // check caught why that is unsafe: this whole block runs during
            // BOOT STAGES, strictly before Main() -> CreateGraphVehicles()
            // has created the D3D12/Vulkan device -- starting REAL background
            // work (texture import + BC7 compression + a file write) that
            // early raced device creation on this machine and reproducibly
            // took the device down (DEVICE_LOST inside the first
            // --headless run, confirmed reproducible, confirmed absent the
            // moment the forced call was removed and confirmed absent again
            // on an already-cooked project that also runs a -- trivial --
            // pass via the watcher). PollAssetWatch (EditorAppProject.cpp) is
            // where the equivalent forced call now lives instead: it runs
            // from inside the main loop's PumpEditorDocuments, unconditionally
            // AFTER the device exists and AFTER this project's own frame 1
            // has already rendered, so it cannot repeat this race.
        }

        // Asset-manager redesign, Plan 1 Task 5: a project switch invalidates
        // both halves of the model -- the cached entries (a fresh
        // AssetRegistry underneath) and the facade seam they were built
        // through (m_cookDiagnostics/m_cookQueue were just re-armed above,
        // for a DIFFERENT project). Rebuild the seam, drop every cached
        // entry, and arm a full rebuild for whenever DrawEditorUi's
        // RebuildIfDirty next runs -- see m_assetModel's own declaration.
        m_assetModel.ResetForProjectSwitch();
        m_assetSelectionEdge.lastEpoch = m_assetModel.selectionGesture;   // re-arm, as the scene edge does (EditorAppScene.cpp:161)
        m_assetPanelProviders = MakeAssetPanelProviders();
        m_assetModel.MarkAllDirty();
        // Asset-manager Plan 3 Task 3: the Graph lens's canvas context dies
        // WITH the project it was showing. It holds per-node-id view state
        // (positions, selection, pan/zoom) for a reference topology the new
        // project shares nothing with, and the panel's node ids are per-build
        // indices -- carrying it across would resurrect the old project's
        // layout under the new one's nodes. Same seam, same frame and the
        // same reason as the model reset immediately above; the panel owns
        // the actual teardown so no `ed::` call has to leave AssetGraphPanel.cpp
        // (plan ruling 1; that file since Task 5, panel-split). The Asset
        // Browser's own mirrors are deliberately NOT reset here (spec s6):
        // absent == default, and a stale key is never queried.
        Arcane::Editor::DestroyAssetGraphPanelCanvas(m_assetGraphUi);
        // Asset-manager Plan 2 Task 5: session-only, same as the model
        // itself -- a switched-to project starts with an empty feed, not
        // the outgoing project's history.
        m_assetActivity.Clear();

        EnsureScene();
        // Compute the real title now that project/scene state is final,
        // rather than the "Untitled" placeholder a mid-boot call would have
        // shown for one MainLoop tick (UpdateWindowTitle also runs every
        // frame -- EditorAppFrame.cpp -- so this is a courtesy, not the only
        // call site).
        UpdateWindowTitle();
        // Record every call except a refused switch -- recordRecents == false
        // remains the refused-open case (must never reorder Open Recent). A
        // null project (project-less boot) now flows through to
        // NoteProjectOpened's null-project prefill branch (EditorRecents.cpp),
        // restoring the pre-pass behavior: the first File-menu frame is
        // already populated instead of showing an empty cache.
        if (recordRecents)
            m_recents.NoteProjectOpened(m_runtime->CurrentProject());
    }

    void EditorApp::ConfigureSettings()
    {
        Arcane::Editor::SettingsHostConfig config;
        if (const Arcane::Project* proj = m_runtime->CurrentProject())
        {
            config.roles = Arcane::Editor::RolesForManifest(proj->Manifest());
            config.projectOpen = true;
        }
        config.assetRefs = &m_assetRefServices;
        config.browsePath = [this](const std::string& cvar, bool folder) { BrowseSettingsPath(cvar, folder); };
        config.restartEditor = [this] { m_restartRequested = true; };
        config.restartBlockedReason = [this]() -> std::string
        {
            if (InPlayMode()) return "Stop Play first";
            if (m_undo && m_scene.IsDirty(*m_undo)) return "Save the scene first (File > Save Scene)";
            if (m_documents.AnyDirty()) return "Save or close the open documents first";
            return {};
        };
        Arcane::Editor::ConfigureSettingsHost(std::move(config));
    }

    void EditorApp::BrowseSettingsPath(const std::string& cvar, bool folder)
    {
        Arcane::Editor::LaunchSettingsPathBrowse(
            m_settingsPathCvar, m_dialogs.settingsPath, cvar, folder,
            [this](bool folderDialog, PathDialogRequest* req)
            {
                if (folderDialog)
                    m_gpu->Win().ShowOpenFolderDialog(&PathPickedThunk, req);
                else
                    m_gpu->Win().ShowOpenFileDialog(&PathPickedThunk, req, nullptr, nullptr);
            });
    }

    void EditorApp::RetargetLayoutIni()
    {
        // ===== PINNED UNDER --headless, AHEAD OF EVERYTHING ELSE =============
        // The per-project layout is USER STATE: it differs per machine, it is
        // whatever this desk last arranged, and the flush below WRITES IT BACK
        // on every project switch (and Shutdown() saves it again at exit, while
        // every member is alive, then disables ImGui's destructor save). An
        // agent run would therefore both READ a layout it did not choose and
        // MUTATE it, making run N+1 differ from run N for a reason that has
        // nothing to do with the engine. That is the imgui.ini veto class this
        // file already calls "the worst kind of bug" (see the game-context
        // block above); the pinning precedent is OffscreenImGuiLayer's own
        // io.IniFilename = nullptr.
        //
        // FIRST STATEMENT IN THE FUNCTION on purpose: every path below carries
        // an early `return` (no LOCALAPPDATA, an unwritable dir, an unchanged
        // target), and a pin placed after any of them would be silently
        // skipped on exactly the machines least able to notice.
        //
        // BOTH HALVES ARE REQUIRED. A null IniFilename ALONE leaves the editor
        // with ImGui's built-in defaults -- deterministic, but every panel a
        // free-floating window over an empty dockspace. So exactly one
        // COMMITTED seed is loaded explicitly instead, and nothing is written
        // back: ImGui auto-saves only to io.IniFilename, which is null, and
        // both explicit SaveIniSettingsToDisk call sites in this host are
        // unreachable from here.
        //
        // A MISSING SEED IS NOT AN ERROR -- LoadIniSettingsFromDisk on an
        // absent path is a silent no-op inside ImGui, landing on those same
        // built-in defaults. Degraded, never broken, and still deterministic:
        // the file is either committed or absent for every run alike.
        if (m_config.headless)
        {
            ImGuiIO& io    = ImGui::GetIO();
            io.IniFilename = nullptr;               // never read, never written
            m_layoutIniPath.clear();                // and nothing for a flush to name

            const Arcane::Project* pinProj = m_runtime ? m_runtime->CurrentProject() : nullptr;
            if (!pinProj)
                return;                             // project-less: defaults, and no seed to find

            const std::filesystem::path seed = Arcane::Paths::Join(Arcane::Paths::Location::ProjectSaved,
                                                                   Arcane::Paths::ForProject(pinProj->Root()), "verify-layout.ini");
            std::error_code seedEc;
            if (std::filesystem::exists(seed, seedEc))
            {
                ImGui::LoadIniSettingsFromDisk(seed.string().c_str());
                ARC_INFO("--headless: layout PINNED to the committed seed {} "
                         "(io.IniFilename null -- never read, never written)", seed.string());
            }
            else
            {
                ARC_WARN("--headless: no layout seed at {} -- falling back to ImGui's built-in "
                         "defaults (deterministic, but not the authored layout)", seed.string());
            }
            return;
        }

        // Every ImGui call below (GetIO, the flush, the clear + reload) must
        // land on the EDITOR context: the offscreen layer can leave the game
        // context current (Shutdown's save pins it for the same reason).
        if (m_editorImguiContext)
            ImGui::SetCurrentContext(m_editorImguiContext);

        // <EditorUserDir>\Layouts\Session\<project-guid>.ini ("default" for a
        // project-less session), resolved through Arcane::Paths (settings spec
        // s11.0). Settings S4 (spec s7.4): session layouts live in
        // Layouts/Session; named layouts own Layouts/. Paths::UserRoot follows
        // Platform::UserDataDirectory, so Linux is XDG and macOS is
        // ~/Library/Application Support. Degraded, never broken: with the user
        // dir unset or the folder unwritable, ImGui's exe-dir imgui.ini
        // default stands.
        const std::filesystem::path dir = Arcane::Editor::SessionLayoutDir();
        if (dir.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return;

        const Arcane::Project* proj = m_runtime ? m_runtime->CurrentProject() : nullptr;
        const std::string key = (proj && !proj->Manifest().guid.empty())
                                    ? proj->Manifest().guid : std::string("default");
        const std::filesystem::path target = dir / (key + ".ini");
        if (m_layoutIniPath == target.string())
            return;   // same project (or a re-open of it): nothing to retarget

        ImGuiIO& io = ImGui::GetIO();
        // Flush the OUTGOING layout first -- but only on a real project
        // SWITCH (m_layoutIniPath already set). At boot nothing has drawn
        // yet, and saving here would write an EMPTY settings file over the
        // legacy exe-dir imgui.ini before the seed below could read it.
        //
        // UNREACHABLE UNDER --headless: the pin at the top of this function
        // returns before this line, and it leaves m_layoutIniPath EMPTY and
        // io.IniFilename NULL -- so even a hypothetical fall-through fails
        // both halves of the condition. That is the Step 2 guard, held by an
        // early return plus the state it leaves behind rather than by a third
        // clause here.
        if (!m_layoutIniPath.empty() && io.IniFilename && *io.IniFilename)
            ImGui::SaveIniSettingsToDisk(io.IniFilename);

        // One-time migration: seed a project's first session layout -- from its
        // pre-S4 file (<EditorUserDir>\layouts\<key>.ini), else the named
        // layout editor.layout.default, else the legacy exe-dir imgui.ini, so a
        // hand-tuned layout survives the move. Every source is COPIED and left
        // in place (bin/ is untracked scratch; the pre-S4 file stays for a
        // rollback).
        if (!std::filesystem::exists(target, ec))
        {
            const std::filesystem::path preS4 = Arcane::Paths::Join(Arcane::Paths::Location::EditorUserDir,
                                                                    Arcane::Paths::Current(), "layouts")
                                                / (key + ".ini");
            const Arcane::Editor::LayoutSeed seed = Arcane::Editor::SeedSessionLayout(
                target, preS4, Arcane::Editor::LayoutLibrary(Arcane::Editor::NamedLayoutDir()),
                Arcane::Editor::cvar_layoutDefault.Get(), "imgui.ini");
            ARC_INFO("layout: session layout {} seeded from {}", target.string(),
                     seed == Arcane::Editor::LayoutSeed::PreS4File      ? "the pre-S4 file"
                   : seed == Arcane::Editor::LayoutSeed::NamedDefault   ? "the default named layout"
                   : seed == Arcane::Editor::LayoutSeed::LegacyExeIni   ? "the exe-dir imgui.ini"
                                                                         : "nothing (factory layout)");
        }

        const bool switching = !m_layoutIniPath.empty();   // boot: ImGui's first NewFrame autoloads; a switch must reload by hand
        // io.IniFilename is a BORROWED pointer (ImGui never copies it) -- the
        // member string is its stable storage for the context's lifetime.
        m_layoutIniPath = target.string();
        io.IniFilename  = m_layoutIniPath.c_str();
        if (switching)
        {
            // ImGui reads the ini only while !SettingsLoaded (its first
            // NewFrame), so without this the OUTGOING layout -- docking,
            // windows, panel visibility, camera, play mode, Inspector
            // instances -- stays live and then overwrites the incoming file.
            // Safe point: every switch runs at the top of the main loop,
            // before the editor's ImGui frame. ClearIniSettings runs every
            // handler's ClearAllFn (defaults), then the incoming file is read;
            // with no dock nodes in it, EndDockSpace builds the default
            // layout, as on a first run.
            ImGui::ClearIniSettings();
            if (std::filesystem::exists(target, ec))
                ImGui::LoadIniSettingsFromDisk(m_layoutIniPath.c_str());
            ARC_INFO("layout: switched to {}", m_layoutIniPath);
        }
    }

    void EditorApp::RetargetDumpDir()
    {
        // <project>/Saved/Diagnostics (Paths::DiagnosticsDir): same "Saved/ is
        // per-project, untracked scratch" precedent WriteAutoScreenshot already
        // uses (Saved/AutoScreenshot.png, below) -- a crash/hang report keeps
        // company with the project it came from. project-less (boot with no
        // --project, or a failed switch's fallback -- see every call site of
        // this function) converges on an EMPTY path, which
        // Diagnostics::RetargetDumpDir forwards straight into
        // Config::dumpDir; ReportDir() already treats an empty dumpDir as
        // "<exe dir>/diagnostics" (Diagnostics.hpp's Config comment), so this
        // never re-derives that fallback itself.
        const Arcane::Project* proj = m_runtime ? m_runtime->CurrentProject() : nullptr;
        Arcane::Diagnostics::RetargetDumpDir(
            proj ? Arcane::Paths::Resolve(Arcane::Paths::Location::DiagnosticsDir, Arcane::Paths::ForProject(proj->Root()))
                 : std::filesystem::path{});
    }

    bool EditorApp::StageSplashReady(Arcane::HostBoot::BootContext&)
    {
        // THE SPLASH CARRIES THE LOADING UI, not the editor window. This
        // stage depends on "finalize" (ProjectBoot.cpp's
        // EditorStages), so it is the LAST stage the editor runs -- the
        // window is revealed only once boot is actually finished, matching
        // UnrealEdGlobals.cpp:215-236 ("Hide the splash screen now that
        // everything is ready to go" -> Hide() -> "Do final set up on the
        // editor frame and show it" -> CreateDefaultMainFrame).
        //
        // THE REVEAL IS NOT IN THIS STAGE. CreateGraphVehicles() reveals the
        // window, immediately before building the vehicle that owns its only
        // swapchain -- which satisfies both constraints at once: never show a
        // window nothing can draw into, and never build a surface against a
        // window the compositor has not mapped (the Show()-then-create
        // ordering three desk checkpoints proved).
        //
        // What this stage does is the splash Disarm/Close below: the end of
        // boot.

        // Disarm BEFORE Close(): m_splashPresenter's own quit detection
        // (Run()'s comment / BootSplashPresenter::Present) would otherwise
        // see the splash we are about to close OURSELVES transition
        // open->closed on the very next present() call and mistake it for
        // the user closing it -- aborting a boot that actually just finished.
        m_splashPresenter.Disarm();
        if (m_splash) m_splash->Close();
        return true;
    }

    Arcane::Editor::TitleParts EditorApp::CurrentTitleParts() const
    {
        Arcane::Editor::TitleParts parts;
        if (const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr)
            parts.project = project->Manifest().name;
        parts.scene = m_scene.DisplayName();
        // m_undo is built later in Init than the first title push; a session with
        // no command stack yet has nothing authored, so it reads as clean.
        parts.sceneDirty = m_undo && m_scene.IsDirty(*m_undo);
        return parts;
    }

    void EditorApp::UpdateWindowTitle()
    {
        // THE BACKEND NAME: there is no RenderDevice to ask -- GpuContext
        // builds none -- so this reads the config, the same substitution
        // RuntimeApp::StageSpriteTables makes for the resolver's shader
        // flavor.
        std::string title = Arcane::Editor::FormatOsTitle(
            CurrentTitleParts(), Arcane::BuildInfo(),
            !m_gpu ? "" : Arcane::ToString(m_config.backend));
        if (title == m_windowTitle)
            return;
        m_windowTitle = std::move(title);
        m_gpu->Win().SetTitle(m_windowTitle);
    }

    // Installed here -- before HostBoot::EditorStages(ctx) builds the stage
    // list and before any BootSequence exists -- rather than from inside
    // StageEditorShell as it used to be (2026-07-31 review, Critical 1).
    // project_open (Worker) logs from ScanContent/plugin-descriptor warnings,
    // and editor_lock (Worker) logs on a failed lock write; both run BEFORE
    // editor_shell in the DAG. spdlog's vendored callback_sink_mt protects
    // ITS OWN invocation, but Log::Engine()->sinks() is a plain unlocked
    // std::vector (logger-inl.h's broadcast loop takes no lock) -- pushing
    // onto it from the main thread while a worker is mid-iteration over the
    // same vector is a data race (reallocation frees memory the worker is
    // still walking). Installing the sink before any worker stage exists
    // removes the race structurally instead of trying to serialize around
    // it. Needs only Log::Engine() and `console`, both live at this point, so
    // it has no stage dependency of its own. Bonus: this also means the
    // Console now captures runtime_create/gpu_core's banner lines, which it
    // used to miss because the sink installed after they ran.
    //
    // Both sinks are installed from Create(), not Init(), and Uninstall()
    // (called from the top of Shutdown()) removes them unconditionally -- so
    // a Create-failed path still gets its ARC_ERROR captured by the Console
    // and the diagnostics store.
    //
    // The diagnostics-store half (the store.InstallAsEngineSink() call
    // below): Arcane::Diagnostics' sink slot is mutex-guarded (unlike
    // Log::Engine()->sinks()), but a worker stage (e.g. project_open's
    // content scan) can still publish diagnostics before any BootSequence
    // exists, so it installs from the same early, dependency-free point.
    void EditorApp::ConsoleDiagnostics::Install()
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [this](const spdlog::details::log_msg& m)
            {
                // Everything below was already on the log_msg and was previously
                // thrown away one line later -- capturing it is what makes
                // severity colouring, filtering, and the category column possible.
                ConsoleEntry e;
                switch (m.level)
                {
                    case spdlog::level::err:
                    case spdlog::level::critical: e.level = Arcane::DiagSeverity::Error;   break;
                    case spdlog::level::warn:     e.level = Arcane::DiagSeverity::Warning; break;
                    default:                      e.level = Arcane::DiagSeverity::Info;    break;
                }
                e.timestampMs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        m.time.time_since_epoch()).count());
                e.message  = std::string(m.payload.data(), m.payload.size());
                e.category = std::string(CategoryForMessage(e.message));
                if (m.source.filename) { e.file = m.source.filename; e.line = m.source.line; }
                console.Push(std::move(e));
            });
        sink = cb;
        Arcane::Log::Engine()->sinks().push_back(cb);
        store.InstallAsEngineSink();
    }

    // Deregister the diagnostics sink FIRST, same reason as the console sink
    // erase right below: nothing may dispatch into a half-torn-down editor.
    // UninstallEngineSink is identity-guarded (ClearSinkIfCurrent), so this
    // is a no-op if some other DiagnosticStore has since become the
    // process-wide sink.
    //
    // Deregister the console sink SECOND, before anything below can log
    // through Arcane::Log::Engine(). ConsoleDiagnostics is declared after
    // m_runtime/m_plugin/m_gpu in EditorApp.hpp, so it destructs BEFORE them;
    // if the sink outlived this point, a log emitted during ~GpuContext's
    // Vulkan device teardown (validation messages) would invoke the callback
    // and Push into an already-destroyed console deque.
    void EditorApp::ConsoleDiagnostics::Uninstall()
    {
        store.UninstallEngineSink();

        if (sink)
        {
            auto& sinks = Arcane::Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
            sink.reset();
        }
    }

    // The Hub reads <root>/Saved/AutoScreenshot.png as the project's cover
    // (a hand-placed <Name>.png beside the .arcproj outranks it -- see the
    // Hub's resolve.rs). Refreshed on scene save and clean exit, so the tile
    // shows roughly what the project looked like when it was last worked on.
    // The capture is the viewport output AS COMPOSITED: post-tonemap LDR,
    // including the selection outline in Edit mode and the game HUD in Play.
    // Accepted deliberately -- an outline-free shot would need a second scene
    // render, and this is a memory aid, not a render product. Saved/ sits
    // OUTSIDE Content/ on purpose: RegisterCreatedAsset would (correctly)
    // refuse to register it, and the asset registry has no business knowing.
    void EditorApp::WriteAutoScreenshot()
    {
        if (m_frameCount == 0) return;
        if (!m_runtime || !m_gpu) return;

        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj) return;

        const std::filesystem::path file = Arcane::Paths::Join(Arcane::Paths::Location::ProjectSaved,
                                                               Arcane::Paths::ForProject(proj->Root()), "AutoScreenshot.png");
        if (file.empty()) return;   // no Saved/ for this project (Dist): nothing to write a cover to

        // ===== THE COVER CAPTURE =============================================
        // The vehicle's own capture path, FrameDesc::capture + ReadCapture,
        // taken on the VIEWPORT context and NEVER on the chrome one -- an
        // editor cover is a picture of the SCENE, and a capture node on the
        // chrome frame would copy a CHROMED backbuffer and silently redefine
        // what a cover contains (see PresentChromeFrame's four absences).
        //
        // A null `graph` means a vehicle that failed to build (a project
        // switch's rebuild failure): such a session simply writes no cover,
        // and the Hub keeps the previous one -- the same degradation a failed
        // PNG encode already takes.
        if (m_viewportTargets.graph && CaptureGraphViewportPng(file))
            ARC_INFO("Auto-screenshot {}", file.generic_string());
    }

    // ONE MORE VIEWPORT FRAME, ARMED FOR CAPTURE, then read it back.
    //
    // WHY A FRESH FRAME RATHER THAN A READ OF THE LAST ONE. The graph has no
    // "read this texture" entry point at all: a capture is a NODE
    // (RgUsage::CopySrc into an imported HOST_READBACK buffer), declared with
    // the frame it belongs to, and ReadCapture refuses outright unless a
    // submitted frame recorded one (m_captureRecorded). Arming capture on
    // EVERY viewport frame instead would pay a full-surface copy per frame for
    // an image taken twice a session.
    //
    // AND THE FRESH FRAME IS THE BETTER PICTURE, which is the part worth
    // stating: it carries the scene, the post chain and the Edit-mode overlays
    // (SubmitSceneToBatcher's gizmo + camera rect) but NOT the selection
    // outline and NOT the game HUD, because it does not call
    // ArmGraphViewportFrame. WriteAutoScreenshot's own comment calls the
    // outline-free shot the one it would rather have, and rejects it because
    // "an outline-free shot would need a second scene render" -- on this arm
    // the second scene render is what a capture costs anyway.
    //
    // BOTH CALL SITES ARE RARE EVENTS (a scene save, shutdown), which is what
    // pays for ReadCapture's DeviceWaitIdle -- the same stall SaveTexturePng
    // makes on the other arm, for the same reason.
    bool EditorApp::CaptureGraphViewportPng(const std::filesystem::path& file)
    {
        Arcane::NriGraphContext* graph = m_viewportTargets.graph.get();
        if (!graph)
            return false;

        // The SAME submission phase 10 makes, through the same function, so
        // the cover cannot drift from what the viewport shows. Re-Begin is
        // safe at either call site: phase 10's own frame already drained this
        // batcher, and the next frame's phase 10 re-Begins it again.
        Arcane::Batcher2D& b = m_gpu->Batch();
        b.Begin(graph->SurfaceWidth(), graph->SurfaceHeight());
        SubmitSceneToBatcher(b);

        const Arcane::GlobalParams globals =
            m_resolver ? m_resolver->Globals() : Arcane::GlobalParams{};

        Arcane::NriGraphContext::FrameDesc vp;
        vp.batch   = &b;
        vp.post    = m_resolver ? m_resolver->PostDesc() : nullptr;
        vp.globals = &globals;
        vp.capture = true;
        if (graph->RenderFrameOffscreen(vp) != Arcane::NriGraphContext::FrameOutcome::Presented)
            return false;   // Skipped (collapsed panel) or Failed (already latched + logged)

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        // ReadCapture hands back TIGHT RGBA8, already swizzled out of the
        // output's BGRA, so these bytes are directly the ones a PNG wants.
        // A false here is NOT a run failure (its own contract says so).
        if (!graph->ReadCapture(w, h, rgba))
            return false;

        // 512 wide, and every other rule about what a cover is, through the
        // ONE shared definition (Arcane::WriteThumbnailPngRgba).
        return Arcane::WriteThumbnailPngRgba(file, w, h, std::move(rgba), 512);
    }

    namespace
    {
        using StageFn = bool (Arcane::Editor::EditorApp::*)(Arcane::HostBoot::BootContext&);
        struct HostStagePatch { std::string_view id; StageFn fn; };
    }

    // THE host-owned stage patch (architecture pass sec 5): Create() applies it
    // to the boot list; SwitchProject applies it to the same shared
    // EditorStages(ctx) list before cherry-picking its subset -- ONE patch
    // path, both directions of drift still fail loudly (see the table comment).
    bool EditorApp::PatchHostStages(std::vector<Arcane::BootStage>& stages)
    {
        // THE host-owned stage set: every id whose real work touches an EditorApp
        // private member or an editor-exe-only type (EditorTheme/EditorFonts/
        // ShaderEditorDocument, none of which Arcane.dll can see), paired with the
        // method that supplies it. This table IS the list EditorApp.hpp's boot
        // comment describes -- prefer editing it over restating it in prose.
        //
        // Membership here is the whole contract: an id in this table is patched
        // below, an id NOT in it keeps whatever body EditorStages built. Both
        // directions of drift now fail loudly rather than silently:
        //   - id in EditorStages but missing here -> Make()'s Unpatched(id)
        //     sentinel fires when the stage runs (ProjectBoot.cpp).
        //   - id here but gone from EditorStages (renamed/removed upstream, with
        //     a live Stage* method left behind) -> the lookup below fails and
        //     Create() aborts. Nothing used to catch this half; BootStageParityTest
        //     cannot see it either, since it compares ids and never invokes `.run`.
        static constexpr HostStagePatch kHostStages[] =
        {
            { "runtime_create", &EditorApp::StageRuntimeCreate },
            { "gpu_core",       &EditorApp::StageGpuCore },
            { "editor_fonts",   &EditorApp::StageEditorFonts },
            { "editor_shell",   &EditorApp::StageEditorShell },
            { "render_bridge",  &EditorApp::StageRenderBridge },
            { "edit_core",      &EditorApp::StageEditCore },
            { "sprite_tables",  &EditorApp::StageSpriteTables },
            { "plugin_load",    &EditorApp::StagePluginLoad },
            { "finalize",       &EditorApp::StageFinalize },
            { "splash_ready",   &EditorApp::StageSplashReady },
        };

        // Iterate the TABLE, not the stage list: a patch that matches nothing is
        // then just a failed lookup at the point it mattered, with no parallel
        // "did I apply this one" array to keep in sync.
        for (const HostStagePatch& patch : kHostStages)
        {
            const auto it = std::ranges::find(stages, patch.id,
                [](const Arcane::BootStage& s) { return std::string_view(s.id); });
            if (it == stages.end())
            {
                ARC_ERROR("EditorApp::PatchHostStages: patch '{}' matched no stage in "
                          "EditorStages() -- renamed/removed in ProjectBoot.cpp without "
                          "updating this table", patch.id);
                return false;
            }
            it->run = [this, fn = patch.fn] { return (this->*fn)(m_bootCtx); };
        }
        return true;
    }

    bool EditorApp::Create()
    {
        // Installed before HostBoot::EditorStages(ctx) builds the stage list
        // and before any BootSequence exists -- see
        // ConsoleDiagnostics::Install()'s doc comment for why this early,
        // dependency-free point matters, and ConsoleDiagnostics::Uninstall()'s
        // for why Shutdown() removes both sinks unconditionally.
        m_consoleDiag.Install();

        // Report-written hook (GPU crash diagnostics arc, Task 9): mirrors
        // GpuSectionProvider's install pattern (Diagnostics.hpp) -- one call
        // per host lifetime, installed at this same early, dependency-free
        // point so a report written during boot (a boot-stage hang) is
        // still caught, not just one written mid-session. OnReportWritten
        // only ever touches m_pendingReportsMutex/m_pendingReports (see
        // EditorApp.hpp's Report-written notify section), so it is safe to
        // arm before m_runtime or anything else below exists.
        Arcane::Diagnostics::SetReportWrittenHook(&EditorApp::OnReportWritten, this);

        // F2b Task 12: the per-refusal observer (Assets.hpp), same early
        // dependency-free install point and the same reasoning as the
        // report-written hook above -- OnArtifactRefused only ever touches
        // m_cookDiagnostics + calls PublishCookDiagnostics(), so it is safe
        // to arm before m_runtime or any project exists (a refusal literally
        // cannot fire before the Assets facade has a content root, but
        // arming early costs nothing and matches the hook's own precedent).
        Arcane::SetArtifactRefusalObserver(&EditorApp::OnArtifactRefused, this);

        m_bootCtx.runtime     = nullptr;              // stages populate as they go
        m_bootCtx.splash      = m_splash;
        m_bootCtx.projectPath = m_config.projectPath.c_str();
        m_bootCtx.pluginPath  = m_config.pluginPath.c_str();
        m_bootCtx.moduleName  = "ArcaneEditor.exe";
        // Per-open engine settings, from THIS host's parsed command line. THE
        // EDITOR LANE OF THE GOLDEN GATE RUNS THROUGH HERE: a verify run
        // (--headless with --compare/--report) declines the diag:// mount, so
        // the Assets panel in the captured frame cannot enumerate this
        // machine's own crash history -- the 24-pixel scrollbar-thumb diff
        // that demoted editor-ui to advisory. Same shared rule the runtime
        // host uses (HostBoot::OpenOptionsFor), never a second copy.
        m_bootCtx.openOptions = Arcane::Editor::EditorOpenOptions(Arcane::HostBoot::OpenOptionsFor(m_config));
        m_bootCtx.hostConfig = &m_config;
        m_bootCtx.cvarContext = Arcane::CVarContext::Editor;

        // Spec sec 6: the editor ALWAYS shows boot progress, regardless of any
        // opened project's manifest (project_open's shared CoreStages body
        // never touches showProgress at all -- only RuntimeStages' override
        // does, see ProjectBoot.cpp). BootSplashWindow::ShowProgress already
        // defaults true, so this is redundant with that default today; set
        // explicitly anyway so the editor's intent reads from this file
        // rather than depending on a default it does not own.
        if (m_splash) m_splash->SetShowProgress(true);

        // HostBoot::EditorStages(ctx) is the SAME shared function
        // BootStageParityTest exercises and RuntimeApp calls for its own list
        // (RuntimeApp::Run) -- this is the literal call that keeps the two hosts
        // from silently diverging on which steps exist. This is the one deviation
        // from the brief's literal one-line Run(): a direct
        // `BootSequence seq(EditorStages(ctx))` cannot reach EditorApp's private
        // members from inside Arcane.dll, so the vector is captured, patched via
        // PatchHostStages, then moved into BootSequence -- ids/deps/policy/thread/
        // weight (the actual DAG shape the parity test polices) are untouched.
        std::vector<Arcane::BootStage> stages = Arcane::HostBoot::EditorStages(m_bootCtx);

        if (!PatchHostStages(stages))
            return false;

        m_bootSeq.emplace(std::move(stages));
        return true;
    }

    EditorApp::InitResult EditorApp::Init()
    {
        ARC_ASSERT(m_bootSeq.has_value(), "EditorApp::Init called before Create()");

        // m_splashPresenter is BootSequence's presenter for the WHOLE run
        // -- from runtime_create through finalize, every per-stage
        // present() call reports into m_splash's status text + taskbar
        // progress rather than the swapchain, AND (2026-07-30 review round 2,
        // finding 2) arms/checks the splash's own open/closed state so
        // IBootPresenter's documented quit contract (BootSequence.hpp:65)
        // still fires if the user closes the splash mid-boot -- see
        // BootSplashPresenter::Present's own comment. Safe to run
        // unconditionally: it tolerates m_splash == nullptr, same never-fail
        // contract as BootSplashWindow itself. It is a class member, not
        // constructed here, so StageSplashReady can Disarm() it before
        // closing the splash intentionally -- see that method.
        const Arcane::BootResult boot = m_bootSeq->Run(&m_splashPresenter);
        if (boot.ok)
        {
            m_bootCompleted = true;   // see Shutdown()'s EditorLock::Clear
            return InitResult::Ok;
        }

        // Boot did not reach StageSplashReady, so nothing has closed the splash
        // and nothing ever will draw into it again. Close it HERE rather than
        // leaving it to Destroy(): Run() calls Shutdown() first, and Shutdown()
        // can block for a long time (m_moduleBuild.Join waits on a child
        // msbuild; WriteAutoScreenshot encodes a PNG), during all of which the
        // splash would sit on screen showing a stale status line -- the exact
        // unresponsive-window shape this whole arc exists to prevent.
        // Idempotent: Destroy() repeats it as a backstop, and Close() is a
        // no-op on a window that was never created or is already gone.
        if (m_splash)
            m_splash->Close();

        return boot.quitRequested ? InitResult::Quit : InitResult::Failed;
    }

    // ---- The render vehicles ------------------------------------------------
    // The editor's counterpart of the block at the top of RuntimeApp::MainLoop,
    // and deliberately the same shape: reveal the window, then build the
    // host-window ("chrome") context that owns this process's ONLY graphics
    // device, then -- unlike the runtime, which has no panel -- build the
    // OFFSCREEN context the Viewport panel samples, on that same device.
    //
    // WHY BOTH LIVE HERE RATHER THAN IN A BOOT STAGE. The reveal has to precede
    // the swapchain create (a surface built against a window the compositor has
    // never mapped is a backend corner a desk-only machine cannot pre-clear --
    // the order three Phase-2 desk checkpoints proved), and the editor's reveal
    // is the LAST boot stage. So the earliest honest point for the chrome
    // context is after boot, and the offscreen one cannot precede it: it
    // BORROWS the device the chrome context creates.
    // See the declaration: one place resolves the live chrome context, so no
    // call site branches on --headless to find it. Exactly one member is ever
    // non-null; both null before CreateGraphVehicles and after ShutdownGraphPath.
    Arcane::NriGraphContext* EditorApp::ChromeGraph() const noexcept
    {
        return m_offscreenChrome ? &m_offscreenChrome->Graph() : m_graphChrome.get();
    }

    bool EditorApp::CreateGraphVehicles()
    {
        // UNCONDITIONAL, AND NOT #if-GUARDED. This body runs in every
        // configuration, Dist included: a preprocessor guard here would leave
        // Dist with m_graphChrome and m_viewportTargets.graph never built
        // while every use site still dereferenced them -- a silent, Dist-only
        // null dereference, which is exactly the failure class an
        // unconditional body forecloses.
        Arcane::Diagnostics::SetPhase("nri graph vehicle boot");

        // THE REVEAL, which no boot stage can do: it has to precede the
        // swapchain create (per the ordering above) and follow everything
        // else. Show() also RAISES, which is the launch reveal this host owes
        // exactly once.
        //
        // ...AND IT IS SKIPPED ENTIRELY UNDER --headless, exactly as
        // RuntimeApp::MainLoop skips its own. The window still EXISTS --
        // GpuContext creates it hidden -- so ImGuiLayer, InputDevices and the
        // event pump keep working unchanged, and io.DisplaySize still resolves
        // off that HWND's pixel size; it is simply never mapped, and no
        // swapchain is ever built over it.
        //
        // Deliberately NOT "hidden window + ordinary swapchain": the ordering
        // note above is the warning against exactly that -- a surface created
        // against a window the compositor has never mapped is the
        // backend-specific corner a desk-only machine cannot pre-clear. The
        // offscreen vehicle below builds no surface at all, which sidesteps
        // the corner instead of walking into it.
        //
        // An AUTOMATION run (--frames N, or the windowed self-capture cvar)
        // maps the window without activating or raising it (T3-D6 fix round
        // 1, HostPresentation.hpp): a person may be at the desk.
        if (const HostPresentation pres = HostPresentationFor(m_config); pres.showWindow)
            m_gpu->Win().Show(pres.activateOnShow);

        // THE LATCH BASELINE, taken HERE rather than at
        // process start for the reason RuntimeApp::MainLoop states for its own:
        // boot-time errors belong to the boot, and everything from this point
        // until the last NRI object is gone belongs to the graph.
        // ShutdownGraphPath reads it back after both contexts are destroyed --
        // a teardown-only validation error must still fail the run.
        m_graphErrorBaseline = Arcane::RenderErrorCount();

        // WHOEVER CREATES THE DEVICE ARMS THE CRASH CHAIN, exactly as in the
        // runtime: windowed, the chrome context (NriGraphContext::Create ->
        // NriDiagnostics::Arm); under --headless, the chrome OffscreenVehicle
        // (OffscreenVehicle::Create, G1/R109). Every offscreen context that
        // BORROWS the device -- the viewport, each document preview -- never
        // arms: Arm/Disarm name one process-wide slot with no owner identity,
        // so a second armer would be harmless but a second DISARMER would
        // unplug the owner's chain. That gating lives inside CreateOffscreen;
        // nothing here may disturb it.
        if (m_config.headless)
        {
            // THE EXTENT, from the same place the windowed path's comes from:
            // the window's own pixel size. NriGraphContext::Init hands the
            // borrowed window to the swapchain, which resolves its extent off
            // that HWND -- so reading it here is the SAME number, hidden or
            // shown (SDL tracks a window's size regardless of visibility;
            // GpuContext's 1280x720 WindowDesc default is what both modes
            // therefore start at). It is ALSO what ImGuiLayer reports as
            // io.DisplaySize, so the chrome geometry and the render target
            // agree by construction rather than by coincidence.
            std::uint32_t chromeW = 0, chromeH = 0;
            m_gpu->Win().GetPixelSize(chromeW, chromeH);

            // hostHud ALONE, which is the whole of what a chrome context
            // draws. The host-window twin below declares neither pickOutline
            // nor gameUi ("it draws chrome, and a node it never declares is a
            // readback buffer and a descriptor pool nobody reads"), and this
            // one must match it or the two modes' captures would differ for a
            // reason that is not the editor.
            Arcane::NriGraphContext::NodeSet chromeNodes;
            chromeNodes.hostHud = true;

            m_offscreenChrome = Arcane::OffscreenVehicle::Create(m_config, chromeW, chromeH,
                                                                 chromeNodes);
            if (!m_offscreenChrome)
            {
                // NO SECOND ERROR LINE: Create logs the step that actually
                // failed (adapter / wrap / CreateOffscreen), and a generic
                // restatement here would only push the real cause further up
                // the log. Same contract as RuntimeApp::MainLoop's.
                return false;
            }

            // ===== THE GPU-STALL WATCHDOG IS DELIBERATELY LEFT UNARMED ======
            // RuntimeApp::MainLoop calls SetGpuHeartbeatPublisher(true) on its
            // offscreen vehicle. THIS HOST MUST NOT, and the reason is stated
            // at that call site: the contract is "this offscreen context is
            // the ONLY graph context in this process", and the editor can
            // never satisfy it. Even under --headless it holds FOUR graph
            // contexts -- this chrome one, the viewport
            // (BuildGraphViewportContext), and one apiece for any open
            // ShaderEditorDocument / MeshDocument preview -- so publishing
            // here would make the process's single heartbeat slot a four-way
            // alternation between unrelated fences, which is worse than
            // silence: Diagnostics' GPU-progress rule would compare a value
            // from one device timeline against a threshold set by another.
            //
            // The class CANNOT enforce this; it is caller contract only. So
            // the cost is named rather than papered over: under --headless
            // the editor's GPU-stall watchdog is OFF, a wedged GPU produces
            // no diagnostics capture, and closing that gap needs a different
            // rule ("the frame-driving context publishes") that does not
            // exist yet. The CRASH chain is unaffected -- the vehicle armed
            // it on the device it created, and nothing here disturbs it.
        }
        else
        {
            m_graphChrome = Arcane::NriGraphContext::Create(m_config, m_gpu->Win());
        }
        if (!ChromeGraph())
        {
            ARC_ERROR("the editor's chrome graph context could not be created");
            return false;
        }

        // THE ONE-TIME PROBLEMS NOTICE for an injected Tier 1 overlay. The
        // device creation just above ran ForeignModules::Report (the log's
        // WARN line, once per process); this is the same fact where a desk
        // operator is actually looking. Windowed only -- the headless path
        // gets nothing beyond the log and the report field, so a CI lane
        // never grows a row nobody can click -- and NEVER a modal: a Problems
        // row is dismissable, a message box would stall every automated run.
        // Tier 2 stays out of the pane: Steam's and ShadowPlay's hooks are on
        // most desks and are attribution, not a problem to act on. Published
        // under its own key, so a project switch that recreates the vehicles
        // replaces the same rows rather than accumulating them.
        if (!m_config.headless)
        {
            if (const std::optional<std::vector<Arcane::ForeignModules::Match>> scan = Arcane::ForeignModules::LastScan())
            {
                std::vector<Arcane::Diagnostic> rows;
                for (const Arcane::ForeignModules::Match& m : *scan)
                {
                    if (m.tier != Arcane::ForeignModules::kTierDestabilising)
                        continue;
                    Arcane::Diagnostic d;
                    d.severity = Arcane::DiagSeverity::Warning;
                    d.scope    = Arcane::DiagScope::Project;
                    d.code     = "process.foreignModule.destabilising";
                    d.message  = m.product + " is injected into this editor (" + m.module + ")";
                    d.detail   = "It " + m.consequence + ". Remedy: " + m.remedy + ".";
                    rows.push_back(std::move(d));
                }
                if (!rows.empty())
                    Arcane::Diagnostics::Publish("diagnostics:foreign-modules", rows);
            }
        }

        // ===== THE CHROME BACKEND'S ADOPTION, MADE EXPLICIT (Task 10) ========
        // It was already CORRECT before this line and it is still correct
        // without it: ImGuiNri::Init installs its backend identity on whatever
        // ImGui context is CURRENT, and that is the editor's -- StageGpuCore
        // left it pinned and every layer in this host re-pins its own context
        // in every entry point, so nothing between there and here can leave
        // another current for the duration of the Create above.
        //
        // IT IS SPELLED OUT ANYWAY, because as of THIS task that implicit
        // adoption stops being inert bookkeeping and starts deciding real
        // behaviour: this backend now RECORDS, and it now services the editor
        // atlas's ImTextureData (create/update/destroy) every frame. Two things
        // hang off which context it believes it serves -- the backend flags
        // (ImGuiBackendFlags_RendererHasTextures, without which a draw list
        // carries no Textures array and the atlas never reaches the GPU) and
        // ImGuiNri::Release, which walks THAT context's platform texture list
        // on teardown. A backend that silently adopted the wrong one would draw
        // nothing and disown someone else's live atlas, with no error anywhere.
        // Task 9's forward note asked for exactly this if the adoption moved;
        // it has not moved, so this is a restatement rather than a fix.
        // Idempotent by construction (AdoptContext pins, re-ORs two flags,
        // restores) and null-safe.
        if (Arcane::ImGuiNriNode* chromeNode = ChromeGraph()->ImGuiHud())
            chromeNode->AdoptImGuiContext(m_editorImguiContext);

        // ===== THE TOOLBAR MARK ==============================================
        // Uploaded through the CHROME context's NriTextureCache, which is the
        // only uploader there is. It cannot happen in StageEditorShell,
        // because no device exists during boot.
        //
        // A SYNTHETIC PER-RUN GUID, because this image is not a project asset
        // -- it is a file beside the exe -- and the cache's whole vocabulary is
        // Guids. Generated rather than hardcoded so it cannot collide with a
        // real asset id in any project.
        //
        // ColorSpace::Display is load-bearing -- see NriTextureCache::ColorSpace.
        // maxSize 64 is StageEditorShell's own number, and the same reasoning:
        // ~2x the ~32px on-screen mark, so ImGui's single-tap bilinear only
        // minifies cleanly instead of aliasing a 550px source.
        if (Arcane::LoadDisplayPixels("data/images/arcane_logo.png", 64, m_graphLogoPixels))
            m_graphLogoId = Arcane::Guid::Generate();

        // F2b Task 13: the supply now answers TWO things, not one -- the logo
        // (checked first, an exact-guid match) and, for any OTHER guid, the
        // Inspector's texture-asset preview via Assets::PixelsFor. Installed
        // UNCONDITIONALLY (moved out of the `if` above) so the preview still
        // resolves even on the rare boot where the logo PNG itself fails to
        // load -- the two concerns are unrelated and one's failure must not
        // silently take out the other. `id == m_graphLogoId` on a nil
        // m_graphLogoId (the logo-failed case) never matches a real asset
        // guid, so this degrades exactly like the old logo-only lambda did.
        // Deliberately Display, not Srgb/the artifact route the viewport uses
        // for scene content (NriTextureCache.hpp's own routing-rule comment):
        // the preview wants PixelsFor's small THUMBNAIL as raw RGBA8, never a
        // full cooked BC7/mip chain -- a real asset guid arriving here when
        // m_runtime is null (never happens post-boot, but this lambda outlives
        // any one project) returns null, the cache's own "not resident" path.
        //
        // ASSET-MANAGER TASK 8 makes it THREE. The material-thumbnail
        // harvester uploads its 64px captures through this very cache, under
        // SYNTHETIC per-material guids (the toolbar mark's own idiom, and for
        // its stated reason: a rendered thumbnail is not a project asset, and
        // the cache's whole vocabulary is Guids). Checked BEFORE PixelsFor for
        // the same reason the logo is -- a synthetic guid is ours by
        // construction and must never take the asset path -- and it answers
        // null for every guid that is not one of its own, so a texture asset
        // still falls through to PixelsFor exactly as before.
        ChromeGraph()->SetPixelSupply(
            [this](const Arcane::Guid& id) -> const Arcane::PixelData*
            {
                if (id == m_graphLogoId)
                    return &m_graphLogoPixels;
                if (m_materialThumbs)
                    if (const Arcane::PixelData* thumb = m_materialThumbs->PixelsForThumb(id))
                        return thumb;
                return m_runtime ? m_runtime->AssetsFacade().PixelsFor(id) : nullptr;
            });
        if (m_graphLogoId.IsValid())
            if (Arcane::NriTextureCache* cache = ChromeGraph()->Textures())
                if (nri::Texture* logo = cache->Resolve(
                        m_graphLogoId, Arcane::NriTextureCache::ColorSpace::Display))
                {
                    m_graphLogoTexture = (std::uint64_t)(std::intptr_t)logo;
                }

        // THE VIEWPORT, at the boot extent -- 0/0 asks for the default that
        // function names (1280x720; see BuildGraphViewportContext's own
        // comment for why that number).
        //
        // ONE BODY, TWO CALLERS as of Task 12 -- see BuildGraphViewportContext.
        if (!BuildGraphViewportContext(0, 0))
            return false;
        return true;
    }

    // THE VIEWPORT CONTEXT AND EVERY SEAM IT NEEDS, in one body with TWO
    // callers: CreateGraphVehicles above, at boot, and
    // SwitchProject's "render_bridge" stage, which rebuilds this context from
    // scratch on every project switch (see TeardownGraphForSwitch for why the
    // outgoing one cannot be kept).
    //
    // IT IS SHARED RATHER THAN COPIED for the reason every other two-caller
    // body in this host is: a second copy of the wiring is how the switch and
    // the boot silently drift apart, and three of the four statements below
    // are exactly the kind that fail SILENTLY when missed -- a missing
    // AdoptImGuiContext is a blank HUD with no error, a missing resolver or
    // pixel supply is a white texel on every sprite with one WARN a session.
    //
    // NOT #if-guarded, for the reason m_graphChrome's declaration states: a
    // preprocessor-guarded definition forces a guard at every call site, and
    // SwitchProject compiles in every configuration. Both callers reach this
    // body in EVERY configuration, Dist included.
    bool EditorApp::BuildGraphViewportContext(std::uint32_t width, std::uint32_t height)
    {
        if (!ChromeGraph())
        {
            ARC_ERROR("no chrome context -- the viewport context has no device to borrow");
            return false;
        }

        // THE BOOT EXTENT, named ONCE, here, because both callers can want it:
        // boot has no measurement to offer and a switch may have had no
        // outgoing context to measure. The panel's real extent is not known
        // until it has drawn once, and phase 8's deferred resize adopts it on
        // the next frame -- so this number decides one frame's worth of
        // picture and nothing else.
        if (width == 0 || height == 0)
        {
            // editor.viewport.fallbackExtentW/H (settings S6-32; 1280x720).
            const auto& prefs = Arcane::Settings<Arcane::Editor::EditorViewportSettings>();
            width  = prefs.fallbackExtentW;
            height = prefs.fallbackExtentH;
        }


        // BOTH OPTIONAL NODE SETS. This context is the whole viewport, so it
        // needs both:
        //   * pickOutline -- PickNode + OutlineNode. Asked for here rather
        //     than inferred from --pick-probe because the editor's probe pixel
        //     is a click that has not happened yet and its hover cursor moves
        //     every frame; that flag arms a FIXED pixel and is a different
        //     question.
        //   * gameUi -- the second ImGuiNriNode, which draws the plugin HUD's
        //     ImDrawData between the tonemap and the outline composite. That
        //     is phase 11 then phase 12's order against this recorder.
        // The CHROME context asks for neither: it draws chrome, and a node it
        // never declares is a readback buffer and a descriptor pool nobody
        // reads.
        Arcane::NriGraphContext::NodeSet viewportNodes;
        viewportNodes.pickOutline = true;
        viewportNodes.gameUi      = true;
        m_viewportTargets.graph = Arcane::NriGraphContext::CreateOffscreen(
            m_config, ChromeGraph()->Device(), width, height, viewportNodes);
        if (!m_viewportTargets.graph)
        {
            ARC_ERROR("the editor's offscreen viewport context could not be created");
            return false;
        }

        // THE ONE OBLIGATION A gameUi CONTEXT CARRIES, discharged here because
        // this is the first moment both halves exist. ImGuiNri installs
        // ImGuiBackendFlags_RendererHasTextures on whatever ImGui context is
        // CURRENT when it is built -- which is the EDITOR's, since that is what
        // StageGpuCore left pinned and nothing since has moved it. Without this
        // call the GAME context would never get the flag, its draw lists would
        // carry no `Textures` array for the node to upload from, and the HUD
        // would render as nothing at all -- silently, with no error anywhere.
        // See ImGuiNri::AdoptContext.
        //
        // AND IT IS OWED AGAIN ON EVERY REBUILD, which is the whole reason this
        // body is shared: the node above is a BRAND NEW ImGuiNri that adopted
        // whatever context was current at its Create (the editor's), while
        // m_gameImgui -- deliberately NOT rebuilt by a switch -- is still the
        // context whose draw data it will be handed. Miss this on the switch
        // path and the plugin HUD is blank from the first switch onward, with
        // nothing in the log. Stated at m_gameImgui's declaration too.
        if (Arcane::ImGuiNriNode* gameNode = m_viewportTargets.graph->ImGuiGame())
            gameNode->AdoptImGuiContext(m_gameImgui ? m_gameImgui->Context() : nullptr);

        // THE VISIBILITY READBACK RING (F3 plan 2 T5), armed ONLY on a
        // --report run -- the run whose report carries visibility.gpuVisible.
        // UNLIKE THE RUNTIME HOST, which arms on every run: that one prints the
        // count in its HUD on every frame, and this one has no such line yet,
        // so outside a report nothing here would read the answer
        // (Host/GpuSceneHost.hpp states the split). An unarmed run declares no
        // copy node and pays nothing.
        //
        // Owed again on every rebuild of this context, like the adoption above:
        // the ring belongs to the GpuScene this vehicle owns, and a backend
        // switch builds a new one.
        if (!m_config.reportPath.empty()
            && !Arcane::GpuSceneArmVisibilityReadback(m_viewportTargets.graph->Scene()))
        {
            ARC_WARN("--report: the GPU-scene visibility readback could not be armed -- "
                      "the report's visibility.gpuVisible will be null");
        }

        // The two injected seams the graph path needs to draw REAL content,
        // both copied from RuntimeApp::MainLoop's create block and both on the
        // VIEWPORT context only -- it is the one that renders the scene. The
        // chrome context draws ImGui geometry over textures ImGuiNri owns and
        // resolves no assets at all.
        //
        // Guid -> asset file: the same lambda SceneRenderResolver builds, so it
        // re-reads CurrentProject() per call and survives a project switch.
        // (It survives one on the OBJECT level too, and that is not what makes
        // a rebuild unnecessary -- the CACHES behind these seams are what a
        // switch has to shed. See TeardownGraphForSwitch.)
        m_viewportTargets.graph->SetAssetResolver(
            [rt = &*m_runtime](const Arcane::Guid& id)
                -> std::optional<std::filesystem::path>
            {
                const Arcane::Project* project = rt ? rt->CurrentProject() : nullptr;
                return project ? project->ResolveAsset(Arcane::AssetId::FromGuid(id))
                               : std::nullopt;
            });
        // ...and the same seam extended to COMPILED ARTIFACTS (Task 7):
        // NriTextureCache uploads content's own BC7/RGBA8 mips from the
        // engine's RETAINED, device-free artifact resolve (Assets::ArtifactFor)
        // instead of a raw RGBA8 decode -- see NriTextureCache::ColorSpace's
        // own routing-rule comment. PixelSupplyFn is deliberately NOT installed
        // here any more: this vehicle's content is Srgb-space, which now
        // always prefers the artifact supply once one is installed, and the
        // viewport context has no Display-space chrome of its own to serve
        // (that stays the CHROME graph's job -- see CreateGraphVehicles above,
        // unchanged).
        m_viewportTargets.graph->SetArtifactSupply(
            [rt = &*m_runtime](const Arcane::Guid& id) -> const Arcane::LoadedClientArtifact*
            {
                return rt ? rt->AssetsFacade().ArtifactFor(id) : nullptr;
            });
        // F2c Plan 2 Task 3: CPU mesh geometry for NriMeshBufferCache. Looked
        // up live -- m_resolver is built at stage time, this vehicle later.
        m_viewportTargets.graph->SetMeshSupply(
            [this](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
            {
                if (!m_resolver)
                    return {};
                const auto s = m_resolver->MeshSupply(id);
                return { s.mesh, s.state };
            });
        // F2b Task 12: which of a null artifact answer's two meanings applies
        // (still cooking vs. permanently refused) -- see NriTextureCache::
        // SetCookPendingOracle's own doc comment for the full contract this
        // closes. Captures `this`, safe for the same reason every other
        // lambda captured into this vehicle is: the vehicle is destroyed
        // (ShutdownGraphPath / TeardownGraphForSwitch) well before ~EditorApp.
        //
        // AssetKind::Texture, always (review round 1's kind gate needs a kind
        // here): NriTextureCache only ever resolves TEXTURE guids -- a sprite
        // is re-targeted to its texture ref before it can reach this cache
        // (resolveAssetThumb's own Sprite branch, and scene resolution's
        // sprite -> texture path). If some future caller did hand the cache a
        // non-texture guid, the oracle would find no .png carrying it and
        // answer "pending", which is exactly the pre-fix default this seam
        // already tolerated.
        m_viewportTargets.graph->SetCookPendingOracle(
            [this](const Arcane::Guid& id)
            { return IsCookPending(id, Arcane::Editor::AssetKind::Texture); });
        return true;
    }

    int EditorApp::Main()
    {
        if (!CreateGraphVehicles())
            return 1;
        MainLoop();
        return 0;
    }

    std::uint32_t EditorApp::ViewportWidth() const noexcept
    {
        // A null `graph` (a vehicle that failed to build) answers 0, which
        // every caller already tolerates.
        return m_viewportTargets.graph ? m_viewportTargets.graph->SurfaceWidth() : 0u;
    }

    std::uint32_t EditorApp::ViewportHeight() const noexcept
    {
        // See ViewportWidth().
        return m_viewportTargets.graph ? m_viewportTargets.graph->SurfaceHeight() : 0u;
    }

    void EditorApp::NoteGraphFrameFailure(const char* what)
    {
        // Reported through the "nri-graph" seam already, which means the latch
        // grew and ShutdownGraphPath's read would have produced exit 2 on its
        // own. 1 is the stronger statement -- it says WHERE the run died -- and
        // it is what RuntimeFrame::RenderGraph sets in the same situation, so
        // the two hosts report one vocabulary. Precedence 1 > 2: FIRST failure
        // wins and a later latch growth cannot demote it.
        ARC_ERROR("{}; stopping", what);
        if (m_graphExit == 0)
            m_graphExit = 1;
        // The frame loop's own exit channel (see PumpFrameEvents, which reads
        // this before any phase runs). Used rather than a break because the
        // failure is discovered mid-frame, and the frame's ImGui pairing and
        // its remaining phases have to finish before the loop may leave.
        m_requestExit = true;
    }

    void EditorApp::RetireDocPreview(std::unique_ptr<Arcane::NriGraphContext> vehicle)
    {
        if (vehicle)
            m_retiredDocPreviews.push_back(std::move(vehicle));
    }

    // Destroy every vehicle a document handed over, invalidate FIRST.
    //
    // WHY THE DEFERRAL EXISTS, in one line: the document died at phase 14 and
    // the chrome frame naming its texture was recorded at phase 19. Draining
    // here -- at the top of a LATER frame -- means that frame has been
    // recorded and submitted, and InvalidateUserTextureNow's own
    // DeviceWaitIdle is what makes "submitted" into "retired" before the view
    // is destroyed. See DocServices::retireGraphPreview.
    //
    // The invalidate is the app's rather than the document's because by now
    // the document is gone -- and because it always was the CHROME context's
    // node that had to be told, which only the app holds.
    void EditorApp::DrainRetiredDocPreviews()
    {
        if (m_retiredDocPreviews.empty())
            return;
        Arcane::ImGuiNriNode* chrome = ChromeGraph() ? ChromeGraph()->ImGuiHud() : nullptr;
        for (std::unique_ptr<Arcane::NriGraphContext>& vehicle : m_retiredDocPreviews)
        {
            if (!vehicle)
                continue;
            // Same three-part order as ShutdownGraphPath: view (chrome's lane)
            // before texture (this vehicle's lane), then the borrower dies.
            // Unconditional and idempotent -- a preview that never drew is a
            // routine miss.
            if (chrome)
                (void)chrome->InvalidateUserTextureNow(vehicle->OffscreenOutput());
            vehicle.reset();
        }
        m_retiredDocPreviews.clear();
    }

    namespace
    {
        // --compare / --bless (Task 9). Own private copy -- see
        // EditorAppFrame.cpp's identical helper (used by MainLoop's pre-loop
        // resolve) for the full reasoning; ShutdownGraphPath below needs the
        // same file-path-component spelling to re-resolve a just-blessed
        // reference and to build the diff-artifact path.
        const char* CompareBackendName(Arcane::GraphicsBackend backend)
        {
            return backend == Arcane::GraphicsBackend::Vulkan ? "vulkan" : "dx12";
        }
    }

    // ===================================================================
    // THE PROJECT SWITCH'S RENDER TEARDOWN -- SwitchProject's
    // "switch_teardown" stage, and it is deliberately the
    // sibling of ShutdownGraphPath below rather than a body inside
    // EditorAppProject.cpp: the two sequences must be read against each other,
    // and every clause one of them gets right is a clause the other owes.
    //
    // ============ WHAT IS TORN DOWN AND WHAT IS KEPT, AND WHY ============
    // The rule the split follows: NOTHING PROJECT-SCOPED SURVIVES A SWITCH,
    // NOTHING WINDOW- OR DEVICE-SCOPED IS DESTROYED BY ONE.
    //
    // KEPT -- m_graphChrome, the host-window context. Not one thing it holds
    // belongs to the outgoing project, and that is checkable rather than
    // asserted (CreateGraphVehicles is the whole of its wiring):
    //   * it owns the process's ONE NriDevice and the swapchain bound to the
    //     host window -- both window/session-scoped, and a switch does not
    //     touch the window;
    //   * it ARMED the process-wide crash chain, and NriDiagnostics keeps one
    //     slot with no owner identity -- tearing this context down mid-session
    //     would disarm it and rebuild it, for nothing;
    //   * its ImGuiNri serves the EDITOR ImGui context, which survives;
    //   * its NriTextureCache has exactly ONE possible tenant: the toolbar
    //     logo, keyed by a synthetic per-run Guid answered by a pixel supply
    //     that returns null for every other id. No project asset can enter it.
    //     (The VIEWPORT context is the one that gets the project's resolver +
    //     Assets pixel supply -- BuildGraphViewportContext, and only there.)
    // Destroying it would mean destroying and re-creating a graphics device
    // and a flip-model swapchain on a live HWND mid-session, taking every
    // borrower (the viewport context, every document preview) with it, in
    // exchange for nothing at all.
    //
    // TORN DOWN -- m_viewportTargets.graph, the offscreen viewport context.
    // Its SIZE is panel-scoped (so the replacement is created at the outgoing
    // one's extent, below, and the panel does not snap back to 1280x720), but
    // its CONTENT is project-scoped through and through, and -- this is the
    // part that decides it -- the content lives in caches that HAVE NO
    // INVALIDATION HOOK BY DESIGN:
    //   * NriTextureCache holds nri::Textures uploaded from the OUTGOING
    //     project's asset pixels, keyed by asset Guid. Two projects can carry
    //     the same Guid (copying a project copies its asset ids), so a kept
    //     cache does not merely leak VRAM -- it can serve project A's pixels
    //     for project B's asset. It does have Release(graves, fence)...
    //   * ...but Batch2DNode::EnsureSpriteSet caches a DESCRIPTOR SET per
    //     sprite Guid naming that cache's view, written ONCE and never
    //     rewritten -- the discipline that keeps ResetDescriptorPool and its
    //     fence rules out of that file. Flushing the texture cache under it
    //     would leave those sets naming destroyed views, which is a fault
    //     rather than a stale pixel. Same shape for the material sets and for
    //     PostChainNode's declared-param views.
    // So a "flush the content, keep the object" switch would have to re-open a
    // deliberately-closed discipline in three nodes. Rebuilding the context
    // achieves the same state by construction -- new pool, new sets, new cache
    // -- with no new invalidation surface, and it is what this arc's two
    // contract blocks (NriGraphContext.hpp's TWO CONTEXTS, TWO LANES item (2);
    // ImGuiNri.hpp's caller list) already named as the switch's shape.
    //
    // KEPT, AND IT MUST BE -- m_gameImgui. The viewport context's game
    // ImGuiNriNode ADOPTED it, and ImGuiNri::Release PINS the adopted context
    // to walk its platform texture list, which is a DEREFERENCE. At process
    // exit member order guarantees it; here it has to be written down, because
    // this function destroys a context while every ImGui context in the
    // process survives. Nothing below resets it. The other half of that
    // obligation -- re-adopting it on the REBUILT node -- is discharged in
    // BuildGraphViewportContext, which is why the switch calls that body
    // rather than CreateOffscreen directly.
    //
    // AND THE SPLIT KEEPS "ONE ImGuiNri PER ImGui CONTEXT" TRUE AT EVERY
    // INSTANT, which is NodeSet's invariant and not a tidiness claim: a
    // graph-mode editor holds exactly two backends -- the chrome hub over the
    // EDITOR context and the viewport's game node over the GAME context
    // (document previews are built with NodeSet{} and have none). This
    // function destroys the second, and the rebuild in a LATER stage creates
    // its replacement, so the game context is never served by two. Rebuilding
    // the chrome context would have had to make the same argument for the
    // editor context; keeping it means there is nothing to argue.
    //
    // WHAT THE GAME CONTEXT LOSES ACROSS THE GAP, stated because it looks like
    // damage and is the protocol working: ImGuiNri::Release DISOWNS the
    // adopted context's RefCount==1 ImTextureData, so the game atlas comes out
    // of this marked WantCreate with its CPU pixels intact -- and the rebuilt
    // backend re-uploads it on its first frame through the same 1.92 texture
    // path that created it originally. That is precisely why Release must run
    // while the context is ALIVE, i.e. why obligation (a) exists.
    //
    // ================= THE ORDER, WHICH IS THE POINT =================
    //   1. IDLE. Explicit and unconditional, and owed for one reason: the stage
    //      that runs immediately after this one unloads the plugin, and a
    //      plugin torn down under a GPU still reading resources is the hazard
    //      that line has always existed to prevent. Not folded into the
    //      invalidate below: InvalidateUserTextureNow early-outs on a null
    //      texture BEFORE its own idle, so a session where nothing ever drew
    //      the output would silently skip it.
    //   2. INVALIDATE, on the CHROME hub, for every texture about to die --
    //      the retired document previews (drained here) and the viewport
    //      output. The chrome backend caches by RAW nri::Texture* and NRI does
    //      not ref-count, so the replacement may land on the freed address;
    //      the entry must be evicted while the texture is still alive, and
    //      through the `Now` variant, because view and texture die through
    //      DIFFERENT graveyard lanes and nothing orders two lanes.
    //   3. RELEASE. ~NriGraphContext then runs its own idle -> bury nodes,
    //      caches, graph -> drain over its OWN lane, which is step 4 as well:
    //      the lane is emptied inside that destructor, so nothing of the dead
    //      context is left pending anywhere.
    //
    // AND NOTHING RENDERS BETWEEN STEP 2 AND STEP 3 -- the adjacency rule the
    // resize path states as "(iii) THE PAIR IS ONE OPERATION". A chrome frame
    // recorded in that window would take ImGuiNri's CREATE path and build a
    // fresh view + set over a texture about to be destroyed. It holds here for
    // a stronger reason than adjacency: SwitchProject drives its stages with a
    // NULL presenter on this arm (see the seq.Run call site), so no frame is
    // recorded anywhere between this function and the rebuild.
    //
    // WHAT THIS FUNCTION DELIBERATELY DOES NOT DO: close documents or clear
    // caches. ResetPerProjectState owns that list, runs immediately before
    // this, and is where a new project-scoped member must be registered. This
    // function consumes the RESULT of its CloseAll (the retire list) and adds
    // no second reset list -- audit defect A3 was three of those.
    void EditorApp::TeardownGraphForSwitch(std::uint32_t& keepWidth, std::uint32_t& keepHeight)
    {
        keepWidth  = 0;
        keepHeight = 0;
        if (!ChromeGraph())
            return;   // never built a vehicle -- nothing of this arm exists to tear down

        // The panel's CURRENT extent, carried to the rebuild so the replacement
        // is created at the size the user is looking at rather than at the boot
        // default. Read BEFORE anything is destroyed, obviously, and 0 when
        // there is no viewport context (a rebuild that already failed once) --
        // which the rebuild reads as "use the boot default".
        if (m_viewportTargets.graph)
        {
            keepWidth  = m_viewportTargets.graph->SurfaceWidth();
            keepHeight = m_viewportTargets.graph->SurfaceHeight();
        }

        // ---- 1. IDLE ------------------------------------------------------
        Arcane::NriDevice& device = ChromeGraph()->Device();
        const nri::CoreInterface& core = device.Core();
        if (core.DeviceWaitIdle)
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&device.Device()));

        // ---- 2. INVALIDATE ------------------------------------------------
        // The documents ResetPerProjectState just closed handed their preview
        // vehicles to the retire list rather than destroying them inline
        // (DocServices::retireGraphPreview). Draining HERE -- while
        // m_graphChrome, whose node the drain invalidates against, is still
        // alive and before any frame is recorded -- is the same pairing
        // ShutdownGraphPath makes, and for the same reason. Without it the
        // vehicles would sit in the list until the NEXT frame's phase 13
        // drain, i.e. across a plugin unload and a project open, holding
        // device memory for a project that no longer exists.
        DrainRetiredDocPreviews();

        // ...and the viewport output itself, the one descriptor that inherently
        // spans both contexts. Unconditional and idempotent: a miss (nothing
        // ever drew the panel) returns false and buries nothing, a null
        // early-outs.
        if (ChromeGraph()->ImGuiHud() && m_viewportTargets.graph)
        {
            ChromeGraph()->ImGuiHud()->InvalidateUserTextureNow(
                m_viewportTargets.graph->OffscreenOutput());
        }

        // ---- 3. RELEASE (+ its own drain) ---------------------------------
        // The borrower dies; the owner of the device it borrowed does not.
        m_viewportTargets.graph.reset();

        // NO LATCH READ HERE, unlike ShutdownGraphPath -- and the omission is
        // deliberate. m_graphErrorBaseline spans the whole graph session and is
        // read exactly once, at exit; sampling it mid-session would either have
        // to re-baseline (losing errors a switch fired) or report a growth the
        // exit read is about to report again. A validation error raised by this
        // teardown is still caught, at exit, by that one read.
    }

    void EditorApp::ShutdownGraphPath()
    {
        if (!ChromeGraph() && !m_viewportTargets.graph)
            return;   // a run that never built a vehicle

        // ===== OPEN DOCUMENTS FIRST ==========================================
        // A shader document owns its OWN offscreen context, built
        // over the device m_graphChrome owns, and holds m_graphChrome's
        // ImGuiNriNode to invalidate against on the way out. Both of those are
        // gone the moment the two resets below run.
        //
        // MEMBER ORDER DOES NOT COVER IT, and it looks like it should:
        // m_documents is declared AFTER m_graphChrome, so reverse-order
        // destruction genuinely does close documents first -- but that happens
        // in ~EditorApp, long after THIS function has already destroyed both
        // contexts. The window between them is the whole hazard, and it is the
        // same shape as the one this function exists to close: the fix is to
        // close the borrowers at the point the owner is about to die, not to
        // reason about a declaration order that runs later.
        //
        // CloseAll is the document host's own teardown (it is what a project
        // switch runs), so this is not a special exit path -- it is the
        // ordinary one, moved earlier.
        //
        // THE INSPECTOR IS READ BEFORE THE CLOSE (schemaVersion 11, the
        // report's `inspector` block below): closing a document releases it
        // as an Inspector source, and the host then falls back to the scene --
        // so a read at report time would always say "Scene" and never what the
        // run actually showed. Read from the host itself, never from the flags.
        std::string inspectorSource;
        std::string inspectorBreadcrumb;
        {
            Arcane::Editor::InspectorSource& src = m_inspectorHost.Current();
            inspectorSource     = src.SourceName();
            inspectorBreadcrumb = Arcane::Editor::InspectorCrumbText(src, src.Page());
        }
        // Each instance's filter and routed source (schemaVersion 12, the
        // inspector filters arc) -- snapshotted here for the same reason.
        std::vector<Arcane::VerifyReport::InspectorInstance> inspectorInstances;
        for (const auto& i : m_inspectorHost.Instances())
        {
            // The breadcrumb THAT instance shows: its pinned page or its
            // routed source's live page (the draw's own resolve).
            Arcane::Editor::InspectorSource* s = m_inspectorHost.SourceFor(i.id);
            Arcane::Editor::InspectorPage* page = s ? (i.pinned ? s->PageFor(i.pinnedKey) : s->Page()) : nullptr;
            inspectorInstances.push_back({ i.id, i.filter.excluded, s ? s->SourceName() : std::string{},
                                           s ? Arcane::Editor::InspectorCrumbText(*s, page) : std::string{} });
        }
        // The open preview documents (schemaVersion 13, s3.2) -- snapshotted
        // here for the inspector's reason: CloseAll destroys them, and the
        // chrome context their status reads is still alive at this point.
        std::vector<Arcane::VerifyReport::DocumentPreview> documentPreviews;
        m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
        {
            std::optional<Arcane::Editor::PreviewStatus> st;
            if (auto* shader = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d))
                st = shader->ComputeStatus();
            else if (auto* mesh = dynamic_cast<Arcane::Editor::MeshDocument*>(&d))
                st = mesh->ComputeStatus();
            if (!st)
                return;   // only shader and mesh documents preview
            documentPreviews.push_back({ d.AssetGuid().ToString(), std::string(d.Kind()), d.Title(),
                                         Arcane::Editor::CompileStatusId(st->compile),
                                         Arcane::Editor::PreviewAvailabilityId(st->preview),
                                         st->image });
        });
        m_documents.CloseAll();
        // ...which hands their preview vehicles to the retire list rather than
        // destroying them inline, so the list has to be drained HERE, while
        // m_graphChrome (whose ImGuiNri node the drain invalidates against) is
        // still alive. No frame is recorded between the two, which is the one
        // condition the deferral exists to satisfy.
        DrainRetiredDocPreviews();

        // ===== AND THE THUMBNAIL HARVESTER'S VEHICLE (Task 8) ================
        // Exactly the same shape and exactly the same reason as the documents
        // above: it owns an offscreen context BORROWING the device
        // m_graphChrome owns, so the borrower has to die at the point the
        // owner is about to -- not at a member-declaration order that runs in
        // ~EditorApp, long after the resets below. It needs no
        // InvalidateUserTextureNow of its own: its output texture is never
        // handed to ImGui (it is only ever ReadCapture'd), and the THUMBNAIL
        // textures live in the CHROME context's own cache, which is the same
        // context whose ImGuiNri caches them -- the same-context case
        // ~NriGraphContext's ordering already covers (m_graphLogoTexture's own
        // declaration states this distinction in full).
        if (m_materialThumbs)
            m_materialThumbs->Shutdown();

        // ===== THE TEARDOWN HALF OF THE VIEW-BEFORE-TEXTURE RULE =============
        // A RESIZE is not the only moment
        // the viewport's output texture dies -- PROCESS EXIT is the other, and
        // unlike a resize it happens on every single run.
        //
        // WHY MEMBER ORDER CANNOT COVER IT. Reverse-declaration destruction runs
        // m_viewportTargets.graph -- the offscreen context, which OWNS the
        // output -- hundreds of declarations BEFORE m_graphChrome. That order is
        // correct and required for the borrowed DEVICE (the borrower must die
        // first). But the chrome context's ImGuiNri caches that output by RAW
        // nri::Texture*, so ~ImGuiNri disposes its descriptor + set hundreds of
        // declarations LATER: DestroyDescriptor after DestroyTexture, which is
        // exactly the inversion NriGraphContext.hpp's TWO CONTEXTS, TWO LANES
        // calls the part of the rule that "does not go away" (on Vulkan the
        // error raises at image-destroy time). NO declaration order fixes it --
        // the device wants one order and the view wants the opposite.
        //
        // So the caller closes it with the same hook the resize uses, for the
        // same reason: destroy the view while the texture it views is still
        // alive. Unconditional and idempotent -- a routine miss (nothing ever
        // drew the output) returns false and buries nothing, and a null pointer
        // early-outs ahead of the idle.
        //
        // LIVE AS OF TASK 10 -- it was inert while the chrome context rendered
        // nothing, and this is the task that makes it record: the chrome frame
        // draws the Viewport panel's ImGui::Image over OffscreenTextureId(), so
        // there IS a pointer-keyed entry to evict now, on EVERY run. A grown
        // RenderErrorCount here is precisely what the D3b/D3c watch lists read
        // as a finding, and the line below is what makes it legible.
        if (ChromeGraph() && ChromeGraph()->ImGuiHud() && m_viewportTargets.graph)
        {
            ChromeGraph()->ImGuiHud()->InvalidateUserTextureNow(
                m_viewportTargets.graph->OffscreenOutput());
        }

        // THE GPU CULL'S OWN VISIBLE-ROW COUNT (F3 plan 2 T5), read while the
        // viewport context is still alive -- the reset below takes the
        // visibility readback ring with it, exactly as it takes the pick
        // readback buffer in the runtime's twin of this function. Nullopt when
        // the ring was never armed (no --report) or when no frame's readback
        // had completed; the report then says `null` rather than substituting
        // the CPU count.
        const std::optional<std::uint32_t> gpuVisibleRows =
            Arcane::GpuSceneVisibleRows(m_viewportTargets.graph ? m_viewportTargets.graph->Scene() : nullptr);

        // ===== THEN THE CONTEXTS, BORROWER FIRST =============================
        // Explicit resets rather than member destruction, for the reason
        // RuntimeApp::ShutdownGraphPath states: the latch has to be sampled
        // strictly AFTER the last NRI object is gone (teardown ordering is
        // exactly the class of mistake a validation layer exists to catch), and
        // member destructors run after Run() has already returned its code.
        //
        // The order is the SAME one declaration order gives, restated because
        // it is now this function's to get right: the viewport context BORROWS
        // the device the chrome context owns, so the borrower dies first. Two
        // other lifetimes bracket this and both are satisfied HERE and not in
        // ~EditorApp: m_gameImgui (whose context the viewport's game
        // ImGuiNriNode adopted, and whose platform texture list ImGuiNri::
        // Release DEREFERENCES) and m_gpu's ImGuiLayer (whose context the
        // chrome node adopted) are both still fully alive at this point in
        // Shutdown -- which is a strictly stronger guarantee than the
        // declaration-order one it replaces.
        m_viewportTargets.graph.reset();
        m_graphChrome.reset();
        // The --headless twin, released in the same statement group and for
        // the same reason -- exactly one of the two was ever built, and
        // resetting the null one is a no-op. It is destroyed AFTER
        // m_viewportTargets.graph because the viewport context BORROWS the
        // device this vehicle owns.
        m_offscreenChrome.reset();

        // ===== AND ONLY THEN THE LATCH =======================================
        // The editor's equivalent of the runtime's line, and the reason it
        // exists: "RenderErrorCount must not grow across a clean --nri-graph
        // editor exit" is the observable proof that the invalidate above
        // actually ordered the view ahead of the texture. Without the pair
        // printed, a desk operator has a rule with nothing to read it against.
        // A present Tier 1 overlay is NAMED on the summary line -- the same
        // reasoning as RuntimeApp::ShutdownGraphPath's: the drains tag every
        // debug-layer message by producer, but "[d3d12]" cannot say whether
        // it was ours or the overlay's. Fresh scan (microseconds), reused by
        // the report block below; never per frame.
        const std::vector<Arcane::ForeignModules::Match> foreignModules = Arcane::ForeignModules::Scan();
        const std::string overlayNote = [&]() -> std::string {
            const std::string tier1 = Arcane::ForeignModules::Tier1Names(foreignModules);
            return tier1.empty() ? std::string{}
                                 : " (Tier 1 overlay injected: " + tier1 +
                                       " -- its own D3D12/DXGI errors are counted here too)";
        }();
        const std::uint64_t errorsNow = Arcane::RenderErrorCount();
        ARC_INFO("[nri-graph] RenderErrorCount {} -> {}{}", m_graphErrorBaseline, errorsNow, overlayNote);
        if (errorsNow > m_graphErrorBaseline)
        {
            ARC_ERROR("[nri-graph] FAILED: {} validation/render error(s) fired during the editor "
                      "run (teardown included)", errorsNow - m_graphErrorBaseline);
            // Precedence 1 > 2, the runtime's: a frame failure says WHERE the
            // run died and outranks the errors it produced on the way out.
            if (m_graphExit == 0)
                m_graphExit = 2;
        }

        // --settle N (Task 9): FAIL EXPLICITLY on non-convergence -- checked
        // HERE, not only inside the --report block below, because an agent
        // that checks only the PROCESS EXIT CODE (never opens the JSON) must
        // still see this fail. m_settleConverged/m_settleAttemptsUsed are
        // only meaningful when settle was actually requested
        // (m_config.settleAttempts != 0); a run that never asked for it
        // leaves both at their defaults, which must never read as a
        // failure. Precedence 1/2 > 3, matching RuntimeApp::ShutdownGraphPath:
        // a run that already failed for a REAL reason keeps that reason --
        // this only ever promotes an otherwise-clean exit code.
        const bool settleFailed = m_config.settleAttempts != 0 && !m_settleConverged;
        if (settleFailed && m_graphExit == 0)
            m_graphExit = 3;

        // --compare / --bless (Task 9). NOT nested inside the `--report`
        // block below: --settle only requires --screenshot OR --report
        // (HostConfig.cpp), so a run can legally pair --compare/--bless with
        // --screenshot and no --report at all -- the PNG this writes must
        // land regardless of whether a JSON report was ever asked for.
        //
        // Finding 4 of the Task 8 dispatch audit (mirrored here verbatim): a
        // --bless run's settle loop converges on byteEqual && idle ALONE --
        // the compare conjunct is disabled for the whole run
        // (m_compareRequested) -- so m_captureRead here means exactly "the
        // settle loop converged", and THIS is where that converged capture
        // actually becomes the new reference. Never attempted when
        // settleFailed (nothing converged to bless) or when --bless was not
        // given at all.
        bool compareBlessed     = false;
        bool compareWriteFailed = false;
        if (m_config.bless && !m_config.compareReference.empty() && m_captureRead)
        {
            if (Arcane::BlessReference(m_compareResolution, m_captureWidth, m_captureHeight,
                                       m_captureRgba.data()))
            {
                compareBlessed = true;
                ARC_INFO("--bless: wrote reference '{}' ({}x{}) -> {}", m_config.compareReference,
                         m_captureWidth, m_captureHeight, m_compareResolution.blessTarget.string());
            }
            else
            {
                compareWriteFailed = true;
                ARC_ERROR("--bless: failed to write reference to {}",
                          m_compareResolution.blessTarget.string());
                if (m_graphExit == 0)
                    m_graphExit = 4;
            }
        }

        // THE DIFF ARTIFACT (Task 9; HOISTED OUT of the --report block by
        // final-review I-2, in lockstep with RuntimeApp::ShutdownGraphPath --
        // see that function's fuller comment for the reasoning). It sits here,
        // beside the bless write, because --settle requires --screenshot OR
        // --report, so `--compare X --settle N --screenshot out.png` with no
        // --report is legal and used to produce NO diff image at all. The path
        // is carried into SetCompare below when a report WAS asked for; when it
        // was not, the PNG still lands on disk.
        //
        // The guard reproduces the report block's if/else-if chain exactly, so
        // the two sites stay provably in step.
        std::string compareDiffPath;
        if (!m_config.compareReference.empty() && !m_compareMissingFatal && !compareBlessed
            && !compareWriteFailed && m_compareEvaluated && !m_compareResult.passed
            && !m_compareResult.diffRgba.empty())
        {
            const std::filesystem::path diffProjectRoot =
                (m_runtime && m_runtime->CurrentProject()) ? m_runtime->CurrentProject()->Root()
                                                           : std::filesystem::path{};
            const std::filesystem::path artifact =
                Arcane::DiffArtifactPath(diffProjectRoot, m_config.compareReference,
                                         CompareBackendName(m_config.backend));
            // Empty means DiffArtifactPath REFUSED the name (path-traversal
            // guard) -- never write a path it declined.
            if (!artifact.empty())
            {
                if (Arcane::WritePngRgba(artifact, m_compareResult.width, m_compareResult.height,
                                         m_compareResult.diffRgba.data()))
                {
                    compareDiffPath = artifact.string();
                    ARC_INFO("--compare: diff artifact written to {}", compareDiffPath);
                }
                else
                {
                    ARC_ERROR("--compare: failed to write diff artifact to {}", artifact.string());
                }
            }
        }

        // THE REPORT (Task 9), written LAST in this function so exitReason
        // below can read the FINAL m_graphExit -- including the fold just
        // above, which only settles after the vehicle is gone. WriteTo never
        // touches m_graphExit itself, so this call only DESCRIBES what
        // already happened; Run()'s tail still decides the process exit code
        // exactly as it did before this task.
        //
        // Guarded on reportPath alone (HostConfig.hpp's "Empty = off"): a
        // bare `--report` still writes a valid report (run identity +
        // whatever AddCensus below always contributes). This host writes no
        // `probes` entries and no `pick` field, ever -- --probe stays
        // refused at main.cpp, so ParseProbe/Evaluate have nothing to do
        // here; the report's `probes` array is simply left at its default
        // empty state, matching what an agent would see from a runtime run
        // that also passed no --probe.
        if (!m_config.reportPath.empty())
        {
            const bool completedAllFrames =
                m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames;

            // Reuses the SAME vocabulary this function's own m_graphExit
            // comment (EditorApp.hpp) documents (1 = a graph frame failed,
            // 2 = RenderErrorCount grew) -- this is not a new
            // classification, just the existing one spelled into the
            // exit-reason string VerifyReport carries. Precedence mirrors
            // RuntimeApp::ShutdownGraphPath's exactly, including checking
            // m_compareMissingFatal and settleFailed BEFORE
            // completedAllFrames: a non-converged settle run has
            // m_frameCount >= m_config.maxFrames by construction (settle
            // attempts only ever run after the base budget is spent), so
            // completedAllFrames reads true regardless of whether the run
            // actually converged -- left unguarded this would silently fall
            // through to "frames-complete", hiding exactly the failure this
            // mode exists to catch.
            std::string exitReason = "frames-complete";
            if (Arcane::GpuDeviceLostObserved())
                exitReason = "device-lost";
            else if (m_graphExit == 1)
                exitReason = "render-failed";
            else if (m_graphExit == 2)
                exitReason = "validation-errors";
            else if (m_compareMissingFatal)
                exitReason = "compare-missing-reference";
            else if (m_cookRefusedFatal)
                exitReason = "compare-cook-refused";
            else if (settleFailed)
                exitReason = m_compareEvaluated ? "compare-failed" : "settle-not-converged";
            else if (compareBlessed)
                exitReason = "compare-blessed";
            else if (compareWriteFailed)
                exitReason = "compare-failed";
            else if (!completedAllFrames)
                exitReason = "stopped-early";

            Arcane::VerifyReport report;
            report.SetRun(Arcane::ToString(m_config.backend), m_frameCount, exitReason);

            // Only set when a readback actually landed and became the
            // agreed-on capture this run (the ordinary single-shot tail, or
            // a converged settle attempt) -- an early exit, a run that asked
            // for neither --screenshot nor --report, or an unconverged
            // settle run all leave this false, and SetCapture is skipped so
            // Brightness/Luma/Rgba-style consumers see an honest "no capture
            // set" rather than a phantom frame. (This host evaluates no such
            // probes itself -- see the `probes` note above -- but the same
            // capture section feeds Servitor-side tooling the runtime's
            // report already does.)
            if (m_captureRead)
                report.SetCapture(m_captureWidth, m_captureHeight, m_captureRgba);

            // The --settle verdict (Task 3), ported from RuntimeApp::
            // ShutdownGraphPath's twin call site -- BOTH hosts must emit these
            // keys or the schema-3 fields are simply absent from every editor
            // report, silently, because absence is a legal state in this
            // contract.
            //
            // TWO conditions, mirroring the runtime's: settle asked for, AND
            // the loop actually reached a verdict. This host has the identical
            // ahead-of-the-loop exits (m_compareMissingFatal in the exitReason
            // chain just above, device-lost, render-failed,
            // validation-errors), which leave m_settleBail at Keep -- guarding
            // on the count alone would report "timeout-bound" for a run that
            // never spent a timeout.
            //
            // m_settleAttemptsUsed is passed AS MEASURED. It is honest now:
            // PresentChromeFrame's defensive branch used to stop the loop by
            // forging it to m_config.settleAttempts, and Task 2 replaced that
            // with m_settleAborted -- `++m_settleAttemptsUsed` is the only
            // write left. Reporting the forged value would have made this JSON
            // LIE on this host, which is worse than the field's absence: an
            // absent field is an honest gap, a present wrong one is a false
            // report an agent will trust.
            if (m_config.settleAttempts != 0
                && Arcane::SettleVerdictReached(m_settleConverged, m_settleBail))
            {
                report.SetSettle(m_settleAttemptsUsed, m_settleConverged, m_settleBail,
                                  m_settleCaptureFailed);
            }

            // The census is carried whether or not a `census` PROBE was
            // asked for, matching RuntimeApp::ShutdownGraphPath's own
            // comment on why: ToJson's top-level "census" field reads
            // AddCensus's values directly, and Materials() is a live,
            // on-demand read -- so populating it always costs nothing
            // extra, and it is unaffected by --probe staying refused on
            // this host.
            if (m_resolver)
            {
                const Arcane::SceneRenderResolver::MaterialCensus census = m_resolver->Materials();
                report.AddCensus(census.spriteReferenced, census.spriteBound,
                                  census.postReferenced, census.postBound,
                                  census.meshReferenced, census.meshBound);
            }

            // THE VISIBILITY COUNTS (schemaVersion 8, F3 plan 1 T8): the last
            // viewport frame's GpuSceneFrame::Stats -- every live GPU-scene
            // row, the coarse-visible ones against the mesh view, the batches
            // and draws emitted. Carried unconditionally like the census: a
            // 2D scene honestly reports zeros, and a 3D scene that culled
            // everything reports total > 0 with coarseVisible == 0.
            // `gpuVisible` (schemaVersion 9, plan 2 T5) is the last COMPLETED
            // GPU-cull readback -- nullopt on a run that never armed the ring
            // (no --report), or one whose frames were all still in flight.
            // NEVER the coarse count under the GPU's name: see
            // VerifyReport::SetVisibility's own contract.
            report.SetVisibility(m_gpuSceneFrame.stats.total, m_gpuSceneFrame.stats.coarseVisible,
                                 m_gpuSceneFrame.stats.batches, m_gpuSceneFrame.stats.draws,
                                 static_cast<std::uint32_t>(m_gpuSceneFrame.transparentDraws.size()),
                                 gpuVisibleRows);

            // THE INJECTED MODULES (schemaVersion 10): the scan taken above
            // for the RenderErrorCount line, carried unconditionally like
            // the census -- a clean desk reports `[]`.
            report.SetForeignModules(foreignModules);

            // THE --set ECHO (schemaVersion 14, settings S6-GATE): each --set
            // name with the value the registry PUBLISHED, so a witness proves
            // its --set landed on a declared cvar.
            report.SetCVarSets(Arcane::VerifyReport::EchoCVarSets(m_config.cvarSets));

            // The WORLD SET (schemaVersion 6, Core-DLL split plan 1 Task 7). A
            // process is no longer a world: --play-as embedded-server runs the
            // editor's world as a Client beside a DedicatedServer one, and from
            // outside the process there is otherwise no way to tell whether the
            // second came up -- let alone whether it got the same scene. Carried
            // unconditionally, like the census above: both facts are live reads
            // that cost nothing, and a run with one world reports one entry.
            {
                std::vector<Arcane::WorldFact> worlds;
                auto fact = [](Arcane::Runtime& rt)
                {
                    Arcane::WorldFact w;
                    w.role         = Arcane::ToString(rt.Mode());
                    w.hasAuthority = rt.HasAuthority();
                    std::uint64_t n = 0;
                    for (Astra::Entity e : rt.Registry().GetEntityManager()) { (void)e; ++n; }
                    w.entities           = n;
                    w.fixedUpdateSystems = rt.Schedulers().fixedUpdate.Size();
                    return w;
                };
                worlds.push_back(fact(m_runtime->Core()));
                if (Arcane::Runtime* s = m_play.ServerWorld()) worlds.push_back(fact(*s));
                report.SetWorlds(std::move(worlds));
            }

            // The VIEW MODE (schemaVersion 7, F4 plan 1 T12). Read from the
            // editor camera itself -- m_camera.mode, AFTER the --view-mode
            // seed and any persisted [EditorViewport][Camera] have been
            // applied -- never echoed from m_config.viewMode: the report
            // states what the capture was rendered THROUGH, so a seed that
            // silently failed to apply would show up here as "2d" against a
            // witness expecting "perspective" (EditorWitnessTest E2). Spelled
            // exactly as --view-mode takes it (HostConfig.cpp's Choices).
            // Carried unconditionally, like the census and worlds above: the
            // camera always has a mode, and only this host has a camera.
            report.SetViewMode(m_camera.mode == Arcane::Editor::ViewMode::Perspective
                                   ? "perspective" : "2d");

            // THE INSPECTOR (schemaVersion 11, inspector-ownership spec s5):
            // which source the Inspector resolved to and the breadcrumb it
            // shows -- read from the host itself, never from the flags, so a
            // --select-in-document that failed to apply reports what the
            // Inspector really showed (the document's own opening selection,
            // e.g. "Player.arcinput > Player > Move"), never the asked path.
            // Snapshotted above, BEFORE m_documents.CloseAll() released the
            // document as a source (a read here would always say "Scene").
            // `instances` (schemaVersion 12): every instance's filter + the
            // source it routed to, snapshotted beside the two above.
            report.SetInspector(inspectorSource, inspectorBreadcrumb, std::move(inspectorInstances));

            // documents[] (schemaVersion 13, s3.2): every open shader/mesh
            // document's PreviewStatus, snapshotted above before CloseAll.
            report.SetDocumentPreviews(std::move(documentPreviews));

            // The --compare verdict (Task 9), ported from RuntimeApp::
            // ShutdownGraphPath verbatim (structure and field meanings
            // unchanged -- see that function's own comments for the full
            // reasoning on each branch). Emitted ONLY when a comparison was
            // actually requested, so an agent can tell "not asked" from
            // "asked and passed".
            if (!m_config.compareReference.empty())
            {
                const char* const backendName = CompareBackendName(m_config.backend);
                const std::filesystem::path projectRoot =
                    (m_runtime && m_runtime->CurrentProject()) ? m_runtime->CurrentProject()->Root()
                                                                : std::filesystem::path{};

                const auto levelName = [](Arcane::ReferenceLevel level) -> std::string
                {
                    switch (level)
                    {
                        case Arcane::ReferenceLevel::Shared:  return "shared";
                        case Arcane::ReferenceLevel::Backend: return "backend";
                        case Arcane::ReferenceLevel::Adapter: return "adapter";
                        default:                               return "none";
                    }
                };

                // Schema 5: `compare.triedPaths` -- ported from RuntimeApp::
                // ShutdownGraphPath verbatim (see that function's own
                // comment). Seeded from m_compareResolution, re-seeded from
                // `after` in the bless branch, exactly mirroring
                // resolvedLevel/referencePath's own staleness handling below.
                const auto stringifyTriedPaths =
                    [](const std::vector<std::filesystem::path>& paths) -> std::vector<std::string>
                {
                    std::vector<std::string> out;
                    out.reserve(paths.size());
                    for (const std::filesystem::path& p : paths)
                        out.push_back(p.generic_string());
                    return out;
                };

                std::string   resolvedLevel = levelName(m_compareResolution.level);
                std::string   referencePath = m_compareResolution.path.string();
                std::vector<std::string> triedPaths =
                    stringifyTriedPaths(m_compareResolution.triedPaths);
                bool          passed            = false;
                std::uint64_t diffCount         = 0;
                double        diffRatio         = 0.0;
                std::uint64_t maxDiffPixelsUsed = 0;
                bool          sizesMismatch     = false;
                std::string   diffPath;
                std::string   errorMessage;
                double        maxLocalDifference = 0.0;

                if (m_compareMissingFatal)
                {
                    errorMessage = "no reference image on disk; re-run with --bless to create one";
                }
                else if (m_cookRefusedFatal)
                {
                    // F2b Task 12: fired BEFORE m_compareResolution was ever
                    // computed (the cook gate runs ahead of reference
                    // resolution) -- resolvedLevel/referencePath/triedPaths
                    // above are therefore left at their honest "nothing
                    // resolved yet" defaults, exactly as intended for a run
                    // that exited at zero frames.
                    errorMessage = "one or more content textures failed to cook; see the Console "
                                    "'Texture cook failed' lines and the Problems pane";
                }
                else if (compareBlessed)
                {
                    // m_compareResolution was captured BEFORE the write, so
                    // its level is stale for a first bless (was None, is now
                    // Shared) -- re-resolve so the report describes where
                    // the reference actually ended up.
                    const Arcane::ReferenceResolution after =
                        Arcane::ResolveReference(projectRoot, m_config.compareReference,
                                                  backendName, m_compareAdapterSet);
                    resolvedLevel = levelName(after.level);
                    referencePath = after.path.string();
                    triedPaths    = stringifyTriedPaths(after.triedPaths);
                    passed        = true;   // the reference now IS the capture, by construction
                }
                else if (compareWriteFailed)
                {
                    errorMessage = "the settle loop converged, but writing the blessed reference "
                                   "to '" + m_compareResolution.blessTarget.string() + "' failed";
                }
                else if (m_compareEvaluated)
                {
                    passed             = m_compareResult.passed;
                    diffCount          = m_compareResult.diffCount;
                    diffRatio          = m_compareResult.diffRatio;
                    maxDiffPixelsUsed  = m_compareResult.maxDiffPixelsUsed;
                    sizesMismatch      = m_compareResult.sizesMismatch;
                    errorMessage       = m_compareResult.errorMessage;
                    maxLocalDifference = m_compareResult.maxLocalDifference;

                    // The write itself happens ABOVE, outside this --report
                    // block (final-review I-2) -- the diff PNG must land
                    // whether or not a JSON report was asked for. All that is
                    // left here is to REPORT where it went; empty means it was
                    // never written (a pass, or a refused/failed path), exactly
                    // what this field meant before the hoist.
                    diffPath = compareDiffPath;
                }
                else
                {
                    errorMessage = "the settle loop never reached a stable, idle frame, so no "
                                   "comparison ever ran";
                }

                report.SetCompare(m_config.compareReference, resolvedLevel, referencePath, passed,
                                   diffCount, diffRatio, maxDiffPixelsUsed, sizesMismatch, diffPath,
                                   errorMessage, maxLocalDifference, triedPaths);
            }

            // No probe specs, ever, on this host -- Evaluate() is
            // deliberately not called: m_probes stays its default empty
            // array either way (ToJson emits `"probes": []`), and calling
            // it with nothing to evaluate would only dress up a no-op as a
            // step. --probe stays refused at main.cpp (Task 9, Finding B).
            if (report.WriteTo(m_config.reportPath))
                ARC_INFO("report written: {}", m_config.reportPath);
            else
                ARC_ERROR("failed to write report to '{}'", m_config.reportPath);
        }
    }

    void EditorApp::Shutdown()
    {
        // Deregister both sinks FIRST, before anything below can dispatch into
        // a half-torn-down editor -- see ConsoleDiagnostics::Uninstall()'s doc
        // comment for the ordering rationale (diagnostics store, then console).
        m_consoleDiag.Uninstall();

        // Uninstall the report-written hook too, same reason and same
        // ordering: the hang/gpu-stall watchdog thread keeps running until
        // Diagnostics::Shutdown() (called from main(), after this object is
        // gone) joins it, so a report written in that window must not
        // dispatch into an EditorApp mid-teardown or already destroyed.
        // OnReportWritten's `user` is `this` -- clearing the slot here,
        // before any member below starts tearing down, is what makes that
        // pointer safe to have handed out.
        Arcane::Diagnostics::ClearReportWrittenHook();

        // Same reasoning, the per-refusal observer (F2b Task 12):
        // OnArtifactRefused's `user` is also `this`.
        Arcane::SetArtifactRefusalObserver(nullptr, nullptr);
        RetargetUndoCache(nullptr);   // project close: wipe Saved/UndoCache

        // Reclaim the module-rebuild worker before member teardown: it only
        // touches its own mutex-guarded queue, but a thread outliving the
        // object that owns that queue is a use-after-free waiting for a
        // reorder. Blocks until the child cmd/msbuild exits -- closing the
        // editor mid-build waits for the build, by design (there is no child
        // handle to terminate through _wpopen).
        m_moduleBuild.Join();

        // While the device and viewport are still alive (m_gpu destructs after
        // Run returns): leave the Hub a fresh cover of the last thing seen.
        WriteAutoScreenshot();

        // --dump-layout: write the LIVE ImGui layout (real EditorDockHost
        // dock-node ids and sizes, as of whatever this run actually
        // submitted over its --frames budget) to this .ini, right before
        // teardown -- the authoring half of the committed
        // ReferenceProject/Saved/verify-layout.ini seed.
        //
        // NOT placed beside RetargetLayoutIni's own SaveIniSettingsToDisk
        // call (the guard `!m_layoutIniPath.empty() && io.IniFilename` a
        // project switch flushes through): that call fires at OnProjectOpened,
        // BEFORE the frame loop has drawn a single panel, and it is flatly
        // UNREACHABLE under --headless (RetargetLayoutIni's headless branch
        // clears m_layoutIniPath, nulls io.IniFilename, and returns ahead of
        // it). Dumping there would capture whatever was on disk before this
        // run touched anything -- not "the live layout", and not reachable
        // headless at all, which is the one mode this flag exists for.
        //
        // Runs UNCONDITIONALLY relative to --headless/windowed, matching
        // HostConfig.hpp's own comment on dumpLayoutPath:
        // ImGui::SaveIniSettingsToDisk(path) never reads io.IniFilename (which
        // IS null here under --headless) -- it always writes to the path it
        // is GIVEN. Gated on m_gpu, not m_frameCount: a null m_gpu means no
        // ImGui context was ever created (a failed Create()), and
        // SaveIniSettingsToDisk dereferences the current context
        // unconditionally -- a crash, not a degraded result. A dump with zero
        // frames rendered is merely uninteresting, not unsafe, so that case
        // is not refused.
        if (!m_config.dumpLayoutPath.empty() && m_gpu)
        {
            ImGui::SaveIniSettingsToDisk(m_config.dumpLayoutPath.c_str());
            ARC_INFO("--dump-layout: wrote the live ImGui layout to {}", m_config.dumpLayoutPath);
        }

        // Settings arc S3-13: the settings windows' pending edits go to disk
        // now (spec s6.3), while the project's folders are still known; then
        // the Project page stops pointing at this EditorApp.
        Arcane::Editor::FlushSettingsArchives();
        Arcane::Editor::RegisterSettingsPage(Arcane::SettingScope::Project, "Project", "Project", nullptr, nullptr);

        // The user cvar archive (T3-D2): written while the game module -- and
        // every Archive cvar it declared -- is still loaded. A no-op unless
        // this session archives (StageRuntimeCreate sets SetUserCVarArchiving).
        if (m_bootCompleted && m_runtime)
            (void)m_runtime->Core().SaveUserCVars();

        // Release the editor lock: this project is no longer open anywhere.
        if (m_bootCompleted && m_runtime)
        {
            if (const Arcane::Project* proj = m_runtime->CurrentProject())
            {
                Arcane::EditorLock::Clear(proj->Root());
            }
        }

        // Relaunch only after the old editor has released the project lock.
        if (!m_relaunchRoot.empty())
        {
            namespace R = Arcane::Editor::EditorRestart;
            const std::filesystem::path exe = R::CurrentExe();
            if (exe.empty() || !R::Spawn(exe, R::Args(m_relaunchRoot)))
                ARC_ERROR("Restart editor: could not start a new editor for '{}' -- open it again by hand",
                          m_relaunchRoot.generic_string());
        }

        // Asset-manager Plan 3 Task 3: the Asset Graph panel's canvas
        // context, released while an ImGui context is still current -- the
        // ImGui context lives in m_gpu, which destructs only after Run()
        // returns, so here is inside its lifetime. Ahead of the render
        // teardown for the same "borrower first, GPU last" reason
        // ShutdownGraphPath states: this is pure ImGui/CPU state, and it
        // should be gone before anything below starts dismantling the
        // device. Idempotent and a no-op when the lens was never opened.
        Arcane::Editor::DestroyAssetGraphPanelCanvas(m_assetGraphUi);

        // ===== THE LAYOUT INI IS SAVED HERE, NOT BY ImGui::Shutdown() ========
        // ImGui's own final save runs inside ImGui::Shutdown() (reached from
        // ~ImGuiLayerImpl, owned by m_gpu), and m_gpu is destroyed AFTER the
        // members the editor's settings handlers read (m_inspectorHost's
        // instance list, m_panelVis, ...). Left to ImGui, every WriteAll
        // would iterate already-destroyed members -- the Inspector's `Ids=`
        // line came out empty and the instance list never survived a restart.
        // So: save now, while every member is alive, then null IniFilename so
        // the DestroyContext-time save cannot run at all. A --headless run
        // already has IniFilename == nullptr (RetargetLayoutIni) and is left
        // untouched; a failed Create() has no context (m_gpu null).
        //
        // SettingsLoaded mirrors ImGui's own shutdown guard (imgui.cpp,
        // "CreateContext/DestroyContext without a call to NewFrame shouldn't
        // save an empty file"): a boot that fails or quits before the first
        // NewFrame (a fatal stage after gpu_core, or closing the splash right
        // after finalize) never read the ini, so saving would clobber it --
        // the cwd imgui.ini or the user's per-project layout -- with only the
        // handlers' lines. The EDITOR context is pinned first: the offscreen
        // layer can leave the game context current.
        if (m_gpu && m_editorImguiContext != nullptr)
        {
            ImGui::SetCurrentContext(m_editorImguiContext);
            ImGuiIO& io = ImGui::GetIO();
            if (m_editorImguiContext->SettingsLoaded && io.IniFilename && *io.IniFilename)
                ImGui::SaveIniSettingsToDisk(io.IniFilename);
            io.IniFilename = nullptr;   // unconditional: ImGui::Shutdown must never save over destroyed members
        }

        // The whole render teardown -- the view-before-texture invalidate,
        // both contexts, and the latch read-back -- in the one order that is
        // correct. See ShutdownGraphPath. It runs BEFORE the ReleaseAll below
        // (input editor plan T12): the report's `inspector` block reads the
        // host's Current() there, before its CloseAll, and a ReleaseAll first
        // would always leave the scene fallback to report. Its CloseAll
        // releases each document through the `closing` observer
        // (RemoveSource), the ordinary path, so no raw pointer outlives a
        // document either way.
        ShutdownGraphPath();

        // Inspector ownership: drop every document source (and the pins and
        // history naming them) BEFORE any document can be destroyed.
        // DocumentHost fires its `closing` observer only from Close/CloseAll,
        // and ShutdownGraphPath's CloseAll above is skipped on a run that
        // never built a vehicle -- ~DocumentHost would then destroy the
        // documents with the host still holding raw pointers to them. The
        // host is never consulted after this point (no frame is drawn), and
        // the instance LIST stays for the ini writer.
        m_inspectorHost.ReleaseAll();
        m_inspectorFocusedSource = nullptr;

        // NO DEVICE IDLE IS OWED HERE: ShutdownGraphPath above has already
        // destroyed both contexts, and each idles the shared device and drains
        // its own lane on the way out.
        ARC_INFO("Arcane Editor exiting after {} frames", m_frameCount);

        // ===== END THE PLAY SESSION WHILE ITS HOST IS STILL ALIVE ============
        // Core-DLL split plan 1 Task 7, review round 1 (C1). Since this task
        // m_play OWNS A Runtime -- the embedded DedicatedServer world -- and
        // PluginHost holds a RAW pointer to it for the whole attach. Member
        // order NOW covers that too (m_play is declared BEFORE m_plugin since
        // the final-review fix wave, I2, so ~PluginHost runs first over a
        // still-alive world), and this call is the belt to that braces: it is
        // what covers the paths that never reach member teardown at all, and
        // it keeps the report below honest. Exiting mid-EmbeddedServer Play is
        // an ordinary exit (--play-as embedded-server, or the picker's row
        // plus Alt+F4), not an exotic one.
        //
        // This is the doctrine ShutdownGraphPath already states for the render
        // stack: close the BORROWER at the point the owner is about to die,
        // instead of resting the whole guarantee on a declaration order that
        // runs later. Stop() detaches the server world from the host and
        // destroys it, in that order, so ~PluginHost sees exactly the one world
        // it started with.
        //
        // AFTER ShutdownGraphPath, and that is load-bearing too: the
        // VerifyReport block inside it reads m_play.ServerWorld() for the
        // report's `worlds` array. Stopping first would silently drop the
        // second world from every --play-as embedded-server report.
        //
        // `m_runtime &&` is not defensive noise: Shutdown() also runs after a
        // FAILED Create()/Init (see Run()), where no ClientRuntime was ever
        // constructed. InPlayMode() is false on that path too, but the guard
        // states the precondition rather than leaning on that coincidence.
        if (m_runtime && InPlayMode())
            m_play.Stop(m_runtime->Core(), m_plugin ? &*m_plugin : nullptr);
        // And the separate-server topology's child, for the same reason in the
        // process dimension: ~ServerProcess would get it, but an editor that
        // dies before member teardown (an abort) would not. No-op otherwise.
        m_serverProcess.Stop();

        // The member destructors then run (after Run returns + ~EditorApp), in
        // reverse declaration order -- the load-bearing TEARDOWN CONTRACT:
        //   m_plugin  -> ~PluginHost: Unload (TeardownLive -> ClearSystems +
        //                ResetRegistry) while the plugin DLL is STILL mapped, and
        //                over the `runtimes` it points at -- all of which must
        //                therefore still be alive. It runs FIRST of this group.
        //   m_play    -> ~PlaySession: owns the EMBEDDED SERVER Runtime since
        //                plan 1 Task 7. Declared BEFORE m_plugin (final-review fix
        //                wave, I2) precisely so it destructs AFTER it: the host's
        //                teardown loop can no longer walk a freed world. The Stop()
        //                directly above is the belt to this braces -- it leaves an
        //                ALREADY-EMPTY session here -- and is kept for the paths
        //                that never reach member teardown (review round 1, C1).
        //   m_runtime -> ~Runtime: destroys JobSystem + the now-empty Registry.
        //   m_gpu     -> ~GpuContext: the render/input stack, window LAST. So
        //                gpu outlives runtime + plugin exactly as
        //                ArcaneRuntime's does. See GpuContext's header.
        // m_process (and the ProcessContext it holds) is declared BEFORE m_runtime,
        // so it destructs AFTER it; its owned TypeContext is intentionally NOT freed
        // (heap-leaked, see ~ProcessContext / StageRuntimeCreate).
    }

    void EditorApp::Destroy()
    {
        // Runs after Shutdown(), and after a failed Create() where it is empty.
        // Ordered before member teardown deliberately: the stage closures inside
        // hold `this` and reference m_bootCtx, and while nothing re-invokes them
        // at this point, releasing them here keeps that lifetime question from
        // ever being load-bearing on member destruction order.
        m_bootSeq.reset();

        // Belt and braces for the paths that never reached StageSplashReady --
        // a failed Create(), or a Fatal stage aborting before the reveal. Close()
        // is idempotent and a no-op when the window was never created, so the
        // normal success path (where splash_ready already closed it) is unaffected.
        // Without this, a boot that fails at, say, gpu_core leaves the splash on
        // screen with a stale status line until the process unwinds.
        if (m_splash)
        {
            m_splash->Close();
        }
    }

    // ---- The clean-exit hook (crash window plan 1, task 9) -----------------
    // One process-lifetime atomic, not a member: main() installs the hook
    // BEFORE this object exists (see EditorApp.hpp for why the window has to
    // start that early), and the hook runs on whatever thread noticed the
    // session ending -- the OS's console-handler thread for Ctrl-C and the
    // close box -- which may not touch editor state.
    namespace
    {
        std::atomic<bool> g_cleanExitRequested{ false };
    }

    void EditorApp::InstallCleanExitHook() noexcept
    {
        Arcane::Diagnostics::SetCleanExitHook(
            [](void*) { g_cleanExitRequested.store(true, std::memory_order_release); }, nullptr);
    }

    bool EditorApp::CleanExitRequested() noexcept
    {
        return g_cleanExitRequested.load(std::memory_order_acquire);
    }

    int EditorApp::Run()
    {
        int exitCode = 1;

        if (Create())
        {
            switch (Init())
            {
            case InitResult::Ok:     exitCode = Main(); break;
            case InitResult::Quit:   exitCode = 0;      break;   // user closed the splash
            case InitResult::Failed: exitCode = 1;      break;
            }
        }

        // THE QUIT SITE (crash window plan 1, task 9; spec S5.7). Everything
        // below this line is teardown, and teardown is the one failure the
        // watchdog cannot otherwise see: the frame loop beat right up to its
        // last frame, and there is no loop left to observe afterwards.
        // RequestCleanExit swaps the watchdog's beat rule for a single
        // deadline (Config::exitSeconds, 30 s) that covers Shutdown() +
        // Destroy() + ~EditorApp, so a Vulkan teardown that never returns or a
        // module-build join that never completes is reported as "hang at exit"
        // and terminated with 12 rather than sitting there forever. Idempotent:
        // a Ctrl-C or WM_ENDSESSION that already armed it does not restamp the
        // clock (Diagnostics.cpp's own note on why that matters).
        Arcane::Diagnostics::RequestCleanExit();

        Shutdown();
        Destroy();

        // ===== THE EXIT-CODE FOLD ============================================
        // Mirrors RuntimeApp::Run's tail, code for code, so one desk battery
        // item reads the same against either host. Everything below is 0 on an
        // ordinary run -- the contract this fold keeps ("0 clean, 1 boot/init
        // failed").
        //
        // A device-loss exit is an abnormal end even though it was orderly: the
        // crash report exists, but the session did not do what it was asked to.
        // First, because it outranks every other explanation -- a lost device is
        // WHY the frames afterwards failed. This is also the fold the editor
        // simply did not have: MainLoop has broken on GpuDeviceLostObserved
        // since long before this phase, and reported it as a clean exit 0.
        if (Arcane::GpuDeviceLostObserved())
            return 1;
        // A boot/init failure keeps its own code and outranks the graph's for
        // the same reason: it says where the run died, and it died earlier.
        if (exitCode != 0)
            return exitCode;
        // The graph path's own codes: 1 = a graph frame FAILED (either context
        // -- the viewport's, phase 10, or the chrome's, phase 19), 2 =
        // RenderErrorCount GREW across the run, teardown included. Precedence
        // 1 > 2, set at the two sites that produce them (NoteGraphFrameFailure
        // and ShutdownGraphPath). They can fire on ANY run -- the render path
        // is unconditional -- so 0 here means no graph failure occurred.
        if (m_graphExit != 0)
            return m_graphExit;
        // See ArcaneEditor/src/main.cpp's exit-code table (right above `int
        // main`) for how the Hub's launch.rs decodes these.
        return 0;
    }
}
