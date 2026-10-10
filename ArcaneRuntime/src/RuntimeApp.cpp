// RuntimeApp: Init -> MainLoop -> Shutdown. The frame loop is the M4 Playground loop
// (scene content removed) interleaving the plugin's FixedUpdate/Update via the
// RunLoop with the engine schedulers, plus a PluginHost watching the game DLL.
// The render plumbing + teardown order live in GpuContext (m_gpu). The teardown
// CONTRACT is encoded in the RuntimeApp member declaration order -- see RuntimeApp.hpp.

#include "RuntimeApp.hpp"
#include "RuntimeFrame.hpp"   // MainLoop's frame body

#include <Arcane/Host/GpuSceneHost.hpp>  // GpuSceneArmVisibilityReadback / GpuSceneVisibleRows (F3 plan 2 T5)
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Config/CVarConfig.hpp>  // CommandLineCVarContext (integration ruling I3)
#include <Arcane/Host/VerifyReport.hpp>  // Arcane::VerifyReport/ProbeSpec/ParseProbe (Task 8: --report wiring, ShutdownGraphPath)
#include <Arcane/Host/ReferenceImages.hpp>  // Arcane::ResolveReference/BlessReference/DiffArtifactPath (Task 8: --compare/--bless)
#include <Arcane/Assets/Assets.hpp>      // Arcane::Assets (AssetsFacade().PixelsFor -- the pre-loop SetPixelSupply lambda)
#include <Arcane/Assets/ImageCompare.hpp>   // Arcane::CompareImages/ImageCompareOptions (Task 8: --compare)
#include <Arcane/Base/Assert.hpp>        // ARC_ASSERT (ShutdownGraphPath's offscreen-guaranteed invariant, Fix 3)
#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::Heartbeat/SetPhase (pre-loop phase markers)
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/ForeignModules.hpp>   // ForeignModules::Scan / Tier1Names -- the injected-overlay facts (report + the RenderErrorCount line)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>          // Arcane::Guid::FromString (--scene override; not pulled in transitively by any of the below)
#include <Arcane/Platform/Paths.hpp>   // Arcane::Paths::Resolve(DiagnosticsDir) -- the "finalize" dump-dir retarget (settings spec s11.0)
#include <Arcane/Project/AssetId.hpp>    // Arcane::AssetId::FromGuid (--nri-graph asset resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>   // Arcane::GraphicsBackend / ToString (StageGpuCore's boot banner)
#include <Arcane/Render/RenderErrorLatch.hpp>  // RenderErrorCount (the graph latch fold)
#include <Arcane/Render/GpuInstrumentation.hpp>   // Arcane::GpuDeviceLostObserved (Run()'s exit-code tail)
#include <Arcane/Render/PickEmit.hpp>    // PickEntityForId (ShutdownGraphPath's --pick-probe AND pick@x,y reports)
#include <Arcane/Scene/Components.hpp>   // Arcane::Identity (ShutdownGraphPath's pick@x,y readback resolution, Task 9)
#include <Arcane/Scene/SceneCamera.hpp>  // Arcane::ActiveSceneCamera (PushSceneCamera; RenderGraph calls it via FrameIo::app)
// Assets.hpp/AudioDevice.hpp/InputActions.hpp/InputSnapshot.hpp/Batcher2D.hpp
// live in RuntimeFrame.cpp instead: every symbol they are needed for (the
// audio voice reap, input sampling, Batch2DStats) belongs to MainLoop's frame
// body, which is that file.
// NriGraphContext (the graph vehicle) is NOT Dist-guarded here: RuntimeApp.hpp
// holds the member unconditionally and its creation is unconditional too, in
// every configuration -- see that header's comment.

#include <Astra/Core/TypeContext.hpp>

#include <imgui.h>

#include <atomic>   // g_cleanExitRequested -- the clean-exit hook's latch
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>


namespace
{
    // --compare / --bless (Task 8). The CLI's own spelling for the backend
    // axis ("dx12"/"vulkan" -- HostConfig.cpp's --backend Choices()), which
    // is ALSO the directory name ResolveReference/DiffArtifactPath
    // (ReferenceImages.cpp) key their Backend level on -- ReferenceImagesTest
    // .cpp's own fixtures use exactly these two strings. Deliberately NOT
    // Arcane::ToString(backend), which returns "D3D12"/"Vulkan": that
    // capitalised spelling is what the report's top-level "backend" field
    // and the host's own boot banner use. The two strings serve different
    // audiences -- this one is a FILE PATH COMPONENT that has to match the
    // shipped ReferenceProject fixtures byte-for-byte, that one is a
    // human-facing log/report label -- and conflating them would silently
    // resolve every --compare against the wrong directory.
    const char* CompareBackendName(Arcane::GraphicsBackend backend)
    {
        return backend == Arcane::GraphicsBackend::Vulkan ? "vulkan" : "dx12";
    }
}

RuntimeApp::RuntimeApp(Arcane::HostConfig cfg, Arcane::BootSplashWindow* splash)
    : m_config(std::move(cfg)), m_splash(splash),
      m_splashPresenter(m_splash) {}

// ---- Boot stages (Task 8: RuntimeApp::Init folded into RuntimeStages) ----
// Each method below is one block lifted out of the old monolithic Init().
// Run() wires each into the matching id on the vector
// Arcane::HostBoot::RuntimeStages(ctx) returns -- see ProjectBoot.cpp's
// CoreStages header comment for why the body has to live here (host-owned
// members) rather than in Arcane.dll.

bool RuntimeApp::StageRuntimeCreate(Arcane::HostBoot::BootContext& ctx)
{
    // ProcessContext is the process's ONE instance (spec 2026-09-15 s3), owning
    // the TypeContext every module shares -- ArcaneRuntime.exe, ArcaneClient.dll,
    // ArcaneCore.dll, and every loaded plugin. An OWNED TypeContext is leaked on
    // purpose (~ProcessContext): TypeMeta entries registered by the plugin (via
    // ASTRA_REFLECT in Components.hpp) hold std::function thunks compiled into
    // the plugin DLL, and after PluginHost::Unload -> DLClose those thunks point
    // to unmapped memory, so destructing the TypeContext would crash. Heap-
    // leaking is the correct production pattern for a long-running host; the OS
    // reclaims all process memory on exit anyway.
    m_process = Arcane::ProcessContext::Create({});
    if (!m_process) { ARC_ERROR("ArcaneRuntime: ProcessContext refused -- a second host in this process?"); return false; }
    // This exe's OWN per-module Astra slot (unchanged reasoning: the slot is per module).
    // ArcaneCore.dll's slot is installed by ProcessContext::Create itself.
    Astra::SetTypeContext(&m_process->TypeContext());
    // Opt into a real audio device only for an INTERACTIVE run (maxFrames == 0 = run
    // until quit). The scripted "ArcaneRuntime --frames N" GPU-verify is not interactive ->
    // false -> miniaudio's device-less null backend (no real device grabbed on a CI box).
    m_runtime.emplace(*m_process, m_config.maxFrames == 0);
    // The player's settings persist (settings spec s8.2): an INTERACTIVE run writes
    // the User rung's Archive values back to the per-user directory (Paths
    // GameUserDir/Config) at exit, exactly as the editor does. A scripted --frames
    // or --headless run reads them but never writes: a verify run must not.
    m_runtime->Core().SetUserCVarArchiving(m_config.maxFrames == 0 && !m_config.headless);
    // THIS EXE asks about ITS OWN caches (2026-09-16), for the same reason the
    // editor now does: VerifySharedTypeContext is inline, so ProjectBoot.cpp's
    // type_context_install stage answers for ArcaneClient.dll and no other module.
    // ArcaneRuntime.exe has no known early resolver today -- this is the
    // deterministic net that would catch one the moment it appears.
    if (!Arcane::HostBoot::VerifySharedTypeContext(m_runtime->Registry(), "ArcaneRuntime.exe"))
    {
        ARC_ERROR("ArcaneRuntime: refusing to boot -- this exe is not on the engine's "
                  "Astra TypeContext (see the per-type errors above)");
        return false;
    }

    // Populate ctx for the SHARED type_context_install / project_open /
    // input_config stage bodies (ProjectBoot.cpp), which only have `ctx`, not
    // `this` -- "stages populate as they go".
    ctx.runtime = &m_runtime->Core();
    return true;
}

bool RuntimeApp::StageGpuCore(Arcane::HostBoot::BootContext& ctx)
{
    // The whole platform/render/input stack, booted in order. Owned by m_gpu
    // and declared BEFORE m_runtime/m_plugin in RuntimeApp -- so it destructs
    // AFTER them: the render resources it owns (window, batcher, imgui, input)
    // must outlive runtime + plugin.
    //
    // THE BOOT-PATH SPLIT. GpuContext::Create builds the window, a device-less
    // Batcher2D, the ImGuiLayer and the input stack -- AND NO GRAPHICS DEVICE.
    // MainLoop then builds the NriGraphContext that owns the process's ONLY
    // graphics device, over that same window.
    m_gpu = Arcane::GpuContext::Create(m_config);
    if (!m_gpu)
    {
        ARC_ERROR("ArcaneRuntime: GPU context create failed");
        return false;
    }

    ARC_INFO("{} -- ArcaneRuntime host, backend {}", Arcane::BuildInfo(),
             Arcane::ToString(m_config.backend));
    ctx.gpu = m_gpu.get();

    // NO PRESENTER IS CONSTRUCTED HERE. BootSequence's per-stage pump is
    // driven by the pre-device splash (Arcane::BootSplashPresenter, bound for
    // the whole Run() call below) for the ENTIRE boot -- this host has no
    // swapchain-backed presenter at all.
    return true;
}

bool RuntimeApp::StageRenderBridge(Arcane::HostBoot::BootContext&)
{
    // THERE IS NO RENDER-RESOURCES BRIDGE. A plugin is handed no graphics
    // device and no ShaderLibrary -- Runtime has no SetRenderResources at all,
    // and a plugin therefore builds no engine render objects of its own.
    //
    // What matters is that the Assets facade stays device-less:
    // Assets::PixelsFor is the retained, device-FREE decode the graph's
    // NriTextureCache uploads from, so a textured sprite still renders. This
    // stage is kept for the ImGui handoff below.

    // ABI v2: install the host's ImGui context + allocators on the Runtime BEFORE
    // the plugin loads. PluginHost::RefreshContext copies these into the EngineContext
    // at Init time, so the plugin's Init adopts the host's GImGui across the DLL boundary.
    // The ImGuiLayer (in m_gpu) has already created + set the current context by here.
    {
        ImGuiMemAllocFunc allocFn = nullptr; ImGuiMemFreeFunc freeFn = nullptr; void* ud = nullptr;
        ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &ud);
        m_runtime->SetImGui(ImGui::GetCurrentContext(),
                            reinterpret_cast<void*>(allocFn),
                            reinterpret_cast<void*>(freeFn),
                            ud);
    }

    // THE FLAG BEATS THE INI, as in the editor (EditorApp.cpp's headless
    // pin): under --headless the HUD context neither reads nor writes
    // imgui.ini. A windowed session leaves one beside the exe with the HUD
    // wherever it was last dragged (Pos=59,60 on 2026-09-28 -- one pixel off
    // the default), and a headless compare that reads it moves the HUD, never
    // settles, and fails by the HUD's every pixel while the same compare from
    // a fresh scratch copy passes. Witness: W6. Pinned after the layer created
    // the context and before the first NewFrame, which is when ImGui would
    // load the file.
    if (m_config.headless)
        ImGui::GetIO().IniFilename = nullptr;   // never read, never written

    // THE WINDOW REVEAL IS NOT HERE. It runs dead last rather than right
    // after gpu_core -- see StageFinalize's comment for the reveal ordering.
    return true;
}

bool RuntimeApp::StagePluginLoad(Arcane::HostBoot::BootContext&)
{
    if (const Arcane::Project* project = m_runtime->CurrentProject())
    {
        const auto input = Arcane::HostBoot::LoadGameplayInput(*m_runtime, *project);
        if (input.status == Arcane::HostBoot::GameplayInputLoadResult::Status::Invalid)
        {
            ARC_ERROR("ArcaneRuntime: gameplay input failed to load: {}", input.diagnostic);
            return false;
        }
    }
    else
    {
        m_runtime->GameInput().Clear();
    }
    // The runtime hosts whatever the project's manifest names (--plugin overrides,
    // for bare-DLL workflows). NOTHING to host is a refusal, not an empty window:
    // the editor is a workshop and meaningfully opens project-less, but this
    // host's one job is running a game. (Until 2026-08-11 a bare run fell back
    // to the retired physics Sandbox showcase; the in-repo module is now
    // ReferenceProject's -- `ArcaneRuntime --project ReferenceProject`.)
    const std::string gameModule =
        Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
    // Secondary plugins: each enabled project plugin that built a Plugins/<name>/Binaries
    // DLL loads alongside the game module through the same host (content-only plugins are
    // already mounted at Project::Open).
    const auto pluginModules = Arcane::HostBoot::PluginModules(m_runtime->CurrentProject());
    if (gameModule.empty() && pluginModules.empty())
    {
        ARC_ERROR("ArcaneRuntime: nothing to host -- pass --project <dir> (a project whose "
                  "manifest names a gameModule) or --plugin <dll>");
        return false;
    }
    // ABI 30: the host is built on the ProcessContext (the module's TypeContext + the
    // system-factory table) and ATTACHES the worlds the module serves. One here; the
    // first attached is the primary.
    m_plugin.emplace(*m_process, gameModule.empty() ? std::filesystem::path{}
                                                    : std::filesystem::path(gameModule));
    m_plugin->AttachRuntime(m_runtime->Core());
    for (const auto& dll : pluginModules)
        m_plugin->AddPlugin(dll);
    if (!m_plugin->Load())
    {
        ARC_ERROR("ArcaneRuntime: failed to load game module '{}'", gameModule);
        return false;
    }

    // Open into the project's boot scene, so `--project X` shows the SAME world
    // the editor shows for X instead of only whatever the plugin's Init spawned.
    // After the plugin load for the same reason EditorApp does it there: a
    // scene naming a component the game module registers would otherwise be
    // silently dropped -- which is why this stays IN StagePluginLoad rather than
    // moving to StageSpriteTables: sprite_tables and plugin_load are DAG
    // SIBLINGS (both depend only on {project_open, render_bridge}), so nothing
    // stops sprite_tables from running first; keeping the boot-scene load
    // sequenced right after this stage's own Load() call is what preserves
    // the "after plugin load" ordering the original code required. The result
    // is discarded -- ArcaneRuntime has no scene session to adopt it into, and
    // BootScene already logs both the file it loaded and every reason it did
    // not, so a project with no/broken boot scene just keeps what the plugin
    // built rather than failing the host. No --project means no project,
    // hence no call.
    if (const Arcane::Project* proj = m_runtime->CurrentProject())
    {
        // --scene overrides the project's manifest bootScene with an explicit
        // asset Guid (HostConfig::sceneOverride) -- the editor's separate-window
        // Play passes the currently-open scene here so ArcaneRuntime boots the SAME scene
        // instead of whatever the manifest names. Invalid override TEXT fails
        // this stage outright (a typo'd --scene is a launch mistake, not the
        // normal "no boot scene" case); a well-formed Guid that resolves to no
        // asset in this project falls through to BootScene's existing
        // missing-bootScene path unchanged (logged there; the host keeps
        // whatever the plugin built).
        if (!m_config.sceneOverride.empty())
        {
            const std::optional<Arcane::Guid> ov = Arcane::Guid::FromString(m_config.sceneOverride);
            if (!ov || !ov->IsValid())
            {
                ARC_ERROR("ArcaneRuntime: --scene '{}' is not a valid asset id", m_config.sceneOverride);
                return false;
            }
            (void)Arcane::HostBoot::BootScene(m_runtime->Core(), *proj, *ov);
        }
        else
        {
            (void)Arcane::HostBoot::BootScene(m_runtime->Core(), *proj);
        }
    }
    else if (!m_config.sceneOverride.empty())
    {
        // --scene names an asset IN a project, so with no project open (absent or
        // failed --project) there is nothing to resolve it against. Say so rather
        // than dropping it silently -- a silent no-op here looks identical to a
        // scene that booted.
        ARC_WARN("ArcaneRuntime: --scene '{}' ignored -- no project is open",
                 m_config.sceneOverride);
    }
    return true;
}

bool RuntimeApp::StageSpriteTables(Arcane::HostBoot::BootContext&)
{
    // Scene asset resolution (sprite-resolution lift): the sprites, sprite
    // materials and post chain a scene REFERENCES become bindable here.
    // Refresh sweeps the live registry every frame, so a scene loaded (or
    // hot-reloaded) later is picked up all the same regardless of the exact
    // ordering between this stage and StagePluginLoad's boot-scene load
    // (they are DAG siblings -- see StagePluginLoad's comment).
    //
    // The compile service is the same one the editor runs: material assets are
    // HLSL that gets stitched and compiled on demand, and ArcaneRuntime's
    // postbuild already ships dxcompiler.dll + dxil.dll + shaders/materials
    // beside the exe for exactly this (premake5.lua). A missing DXC degrades to
    // a warn: sprites still resolve (no compile step), materials and the post
    // chain simply stay unbound.
    //
    // Debounce is a HOT-RELOAD nicety: the configured quiet window keeps a
    // designer holding Ctrl+S from firing a compile per keystroke.
    if (!m_shaderCompiler.Initialize())
    {
        // Degrades to a warning rather than refusing the boot: the missing
        // material is on screen and recoverable.
        ARC_WARN("ArcaneRuntime: dxcompiler.dll unavailable -- sprite materials "
                 "and the scene post chain will not bind");
    }
    m_shaderSources.AddRoot("data/shaders");

    Arcane::SceneRenderResolver::Services rs;
    rs.runtime  = &*m_runtime;
    rs.batcher  = &m_gpu->Batch();
    // DEVICE-LESS: SpriteMaterialCache registers BYTES-ONLY materials with
    // the device-less batcher, and PostChainCache publishes its PostChainDesc
    // without building any device-side chain. The BACKEND -- which selects
    // the shader flavor the compiles target -- comes from the config, since
    // there is no device to ask.
    rs.backend  = m_config.backend;
    rs.compiler = &m_shaderCompiler;
    rs.sources  = &m_shaderSources;
    // No consumeFirst: a standalone host has no open documents to give first
    // refusal to, so every drained result goes straight to the caches.
    //
    // F2b Task 11: the mesh-albedo bindless resolution seam, the editor's
    // own EditorApp.cpp wiring mirrored -- Graph() at CALL time, since
    // Refresh() runs long after this Services struct is built and Graph()
    // returns null until MainLoop constructs the vehicle (windowed or
    // offscreen).
    rs.resolveMeshAlbedoSlot = [this](const Arcane::Guid& g) -> std::uint32_t
    {
        Arcane::NriGraphContext* graph = Graph();
        return graph ? graph->ResolveMeshAlbedoSlot(g) : 0xFFFFFFFFu;
    };
    rs.invalidateMeshGeometry = [this](const Arcane::Guid& g)
    {
        if (Arcane::NriGraphContext* graph = Graph())
            graph->InvalidateMeshGeometry(g);
    };
    m_resolver.emplace(std::move(rs));
    return true;
}

bool RuntimeApp::StageFinalize(Arcane::HostBoot::BootContext&)
{
    // THE SPLASH CLOSES HERE, and "finalize" is where that belongs:
    // RuntimeStages appends nothing (BootStageParityTest pins that), so there
    // is no dedicated splash_ready id to hang it on the way the editor has
    // one, and "finalize" is the LAST core stage both hosts run -- matching
    // where the editor's own splash_ready sits (it depends on "finalize" --
    // see ProjectBoot.cpp).
    //
    // THE WINDOW REVEAL IS NOT IN THIS STAGE. MainLoop reveals the window, as
    // soon as the graph vehicle that owns its only swapchain has been created
    // -- see the reveal comment there for why that is the "never show a
    // window nothing can draw into" rule expressed against the one object in
    // this process that can actually draw a frame.
    //
    // Disarm BEFORE Close() -- see EditorApp::StageSplashReady's matching
    // comment for why this specific ordering is required.
    m_splashPresenter.Disarm();
    if (m_splash) m_splash->Close();

    // Diagnostics dump dir (GPU crash diagnostics arc, Task 8; F-6 in the
    // seam-facts survey). The editor's equivalent call sits immediately
    // after RetargetLayoutIni() at each of ITS call sites (EditorApp.cpp /
    // EditorAppProject.cpp) -- ArcaneRuntime has no layout ini to retarget
    // (no ImGui settings file of its own) and no runtime project-switch
    // capability (one --project, decided once at boot, per RuntimeApp.hpp's
    // teardown-contract comment), so there is exactly ONE call site: here,
    // in "finalize", the same terminal core-stage id the editor's own boot
    // path retargets from (ProjectBoot.cpp's CoreStages -- "finalize" is the
    // last stage both hosts run). project_open (a "finalize" DAG ancestor
    // via plugin_load) has already settled CurrentProject() by this point,
    // one way or the other -- see ProjectBoot.cpp's RuntimeStages Fatal-ABI-
    // refusal override for what "settled" means on this host: an opened
    // project, or none, never a partial one.
    Arcane::Diagnostics::RetargetDumpDir(
        m_runtime && m_runtime->CurrentProject()
            ? Arcane::Paths::Resolve(Arcane::Paths::Location::DiagnosticsDir,
                                     Arcane::Paths::ForProject(m_runtime->CurrentProject()->Root()))
            : std::filesystem::path{});
    return true;
}

// ---- The clean-exit hook (crash window plan 1, task 9) --------------------
// One process-lifetime atomic, not a member: main() installs the hook before
// this object exists (see RuntimeApp.hpp), and the hook runs on whatever
// thread noticed the session ending.
namespace
{
    std::atomic<bool> g_cleanExitRequested{ false };
}

void RuntimeApp::InstallCleanExitHook() noexcept
{
    Arcane::Diagnostics::SetCleanExitHook(
        [](void*) { g_cleanExitRequested.store(true, std::memory_order_release); }, nullptr);
}

bool RuntimeApp::CleanExitRequested() noexcept
{
    return g_cleanExitRequested.load(std::memory_order_acquire);
}

void RuntimeApp::MainLoop()
{
    // WALL-CLOCK BASELINES, consulted only in HOST-WINDOW mode: RuntimeFrame
    // ::AdvanceSim branches on io.config.headless and does not call
    // steady_clock::now() at all under --headless, using
    // m_config.fixedDtSeconds for both frameDt and simDt instead (see that
    // function's comments) -- so these two starting stamps go unread for the
    // whole of a --headless run. Still initialised unconditionally: the
    // branch lives in RuntimeFrame.cpp, not here, and an uninitialised
    // steady_clock::time_point read on the windowed path's first frame would
    // be undefined behavior, not merely wrong.
    auto simPrev       = std::chrono::steady_clock::now();
    auto lastFrameTime = simPrev;
    // DEAD STATE, deliberately left alone rather than removed by this task:
    // bound into FrameIo below and never once read or written by any
    // function in RuntimeFrame.cpp -- PrepareFrame's own comment says why
    // ("There is no shader hot-reload poll beside it: the render path reads
    // offline artifacts through NriGraphContext::ShaderBytecode and has no
    // ShaderLibrary to poll"). It predates that comment (compare
    // docs/superpowers/plans/2026-06-14-arcane-m5-plugin-host.md's
    // lastShaderPoll, which DID poll a ShaderLibrary once a second). Carries
    // no timing decision either way, so it needs no --headless branch.
    auto lastShaderPoll = simPrev;
    bool running = true;

    // THE RENDER HALF: one graph frame per iteration, built here.
    //
    // THIS VEHICLE OWNS THE PROCESS'S ONLY DEVICE. The boot above ran
    // GpuContext::Create, which builds no graphics device at all, so the
    // vehicle below creates the first and only one. WINDOWED, that device
    // carries a swapchain over the HOST's window, which it borrows and must
    // not outlive (m_graphContext is declared after m_gpu, so it is destroyed
    // first). OFFSCREEN (--headless), the vehicle is an OffscreenVehicle
    // instead: still one device, still the real frame graph, but with no
    // window handle and no swapchain anywhere in it. Exactly one of the two
    // is built per run, and everything downstream reaches it through Graph().
    //
    // The latch baseline is taken HERE, not at process start: boot-time errors
    // belong to the boot, and everything from this point until the vehicle is
    // destroyed belongs to the graph. ShutdownGraphPath() reads it back after
    // the last NRI object is gone -- a teardown-only validation error must
    // still fail the run. This whole vehicle boot is unconditional, in every
    // configuration including Dist.
    m_graphErrorBaseline = Arcane::RenderErrorCount();
    Arcane::Diagnostics::SetPhase("nri graph vehicle boot");

    // THE REVEAL, which StageFinalize could never have done: its reveal sat
    // behind an `if (!m_gpu->GraphFlavor())` branch that was dead long before
    // the predicate and the BootPresenter it built were deleted -- see that
    // method's own comment. It happens BEFORE the create below, which is
    // the ORDER the Phase-2 vehicle proved at three desk checkpoints: its
    // own window was created VISIBLE and its swapchain built over an
    // already-shown window. Keeping that order rather than showing
    // afterwards is deliberate -- a surface created against a window the
    // compositor has never mapped is exactly the kind of backend-specific
    // corner a desk-only machine cannot pre-clear. The window is undrawn
    // for the handful of milliseconds until the first graph frame, the
    // same gap the vehicle always had. Show() also RAISES, which is the
    // launch reveal this host owes exactly once.
    //
    // ...AND IT IS SKIPPED ENTIRELY UNDER --headless. The window still
    // EXISTS -- GpuContext creates it hidden (GpuContext.hpp's Create
    // comment) -- so ImGuiLayer, InputDevices and the event pump keep
    // working unchanged; it is simply never mapped, and no swapchain is ever
    // built over it.
    //
    // Deliberately NOT "hidden window + ordinary swapchain": the paragraph
    // above is the warning against exactly that -- a surface created against
    // a window the compositor has never mapped is the backend-specific corner
    // a desk-only machine cannot pre-clear. The offscreen vehicle builds no
    // surface at all, which sidesteps the corner instead of walking into it.
    if (!m_config.headless)
        m_gpu->Win().Show();

    if (m_config.headless)
    {
        // THE EXTENT, from the same place the windowed path's initial extent
        // comes from: the window's own pixel size. NriGraphContext::Init
        // hands the borrowed window to the swapchain, which resolves its
        // extent off that HWND -- so reading it here is the SAME number,
        // hidden or shown (SDL tracks a window's size regardless of
        // visibility; GpuContext's 1280x720 WindowDesc default is what both
        // modes therefore start at, and that default is already documented as
        // load-bearing for anything comparing captured frames).
        //
        // There is no Window::Width()/Height() -- GetPixelSize(w, h) is the
        // accessor, and inventing a second one to fit a call shape would be a
        // worse trade than an out-parameter pair.
        std::uint32_t offscreenW = 0, offscreenH = 0;
        m_gpu->Win().GetPixelSize(offscreenW, offscreenH);

        // THE SAME NODE SET THE WINDOWED PATH BUILDS. NriGraphContext::Init
        // sets hostHud unconditionally ("a host-window context presents chrome
        // by definition"), and this host's frame body hands the graph its HUD
        // draw data every frame (RuntimeFrame.cpp's RenderGraph) without
        // knowing which vehicle it is talking to -- so an offscreen vehicle
        // built WITHOUT the node would silently drop the HUD and make the two
        // modes' captures differ for a reason that is not the renderer.
        //
        // pickOutline (Task 9): --pick-probe's arming is STILL refused on an
        // offscreen context by construction (NriGraphContext.cpp's
        // `m_pickArmed = config.pickProbe && !IsOffscreen()`) -- that flag
        // remains windowed-only, unconditionally. But a `pick@x,y` --report
        // probe drives the SAME pick+outline node pair per frame through
        // FrameDesc::pickPixel instead (RuntimeFrame.cpp's RenderGraph),
        // which "needs no arming at all" (NriGraphContext.cpp) beyond the
        // node pair existing -- so THIS is the one place that has to build
        // it. Gated on both a pick@ request AND --report having somewhere to
        // put the answer: "not asking costs nothing" is structural
        // (NriGraphContext.cpp's own comment on NodeSet), and a bare
        // `--probe pick@..` with no `--report` already writes nothing
        // (ShutdownGraphPath's own precedent for --probe/--report). This
        // MUST agree exactly with RuntimeFrame.cpp's own gate, or that block
        // could arm a chain this vehicle never built.
        const bool wantsPickReport = !m_config.reportPath.empty() &&
                                      Arcane::FirstPickProbe(m_config.probes).has_value();

        Arcane::NriGraphContext::NodeSet offscreenNodes;
        offscreenNodes.hostHud     = true;
        offscreenNodes.pickOutline = wantsPickReport;

        m_offscreen = Arcane::OffscreenVehicle::Create(m_config, offscreenW, offscreenH,
                                                       offscreenNodes);
        if (!m_offscreen)
        {
            // NO SECOND ERROR LINE: Create logs the step that actually failed
            // (adapter / wrap / CreateOffscreen), and a generic restatement
            // here would only push the real cause further up the log.
            m_graphExit = 1;
            ShutdownGraphPath();
            return;
        }

        // THE GPU-STALL WATCHDOG, which would otherwise be silently off for
        // this whole run. Diagnostics' GPU-progress rule arms only once
        // something has PUBLISHED a fence value, and until now the engine's
        // sole publisher was the swapchain present path -- which this mode
        // does not have. Nothing failed loudly: g_gpuBeatSeen stayed false,
        // the rule returned before it looked at anything, and a wedged GPU
        // produced no diagnostics capture at all. Silence rather than a false
        // positive -- the worse of the two for the mode whose entire job is to
        // be the gate.
        //
        // THIS HOST IS ENTITLED TO ASSERT IT because it knows its own
        // topology: under --headless it built exactly ONE graph context and
        // there is no presenting one (the reveal above was skipped and
        // m_graphContext stays null). The editor, which holds a chrome context
        // AND a viewport context over one device, must never call this -- and
        // does not.
        m_offscreen->Graph().SetGpuHeartbeatPublisher(true);
    }
    else
    {
        m_graphContext = Arcane::NriGraphContext::Create(m_config, m_gpu->Win());
        if (!m_graphContext)
        {
            ARC_ERROR("the graph render half could not be created");
            m_graphExit = 1;
            ShutdownGraphPath();
            return;
        }
    }

    // From here down the mode is GONE: everything reaches the live vehicle
    // through Graph(), which selects between the two exactly once.
    Arcane::NriGraphContext& graph = *Graph();

    // Guid -> asset file, so the graph path can make a REGISTERED sprite
    // material's declared TEXTURES resident on its own device (Task 9).
    // Deliberately the same lambda SceneRenderResolver builds
    // (SceneRenderResolver.cpp's constructor): re-reads CurrentProject()
    // per call, so it survives a project switch, and resolves through the
    // one registry both render paths already agree on.
    graph.SetAssetResolver(
        [rt = &*m_runtime](const Arcane::Guid& id)
            -> std::optional<std::filesystem::path>
        {
            const Arcane::Project* project = rt ? rt->CurrentProject() : nullptr;
            return project ? project->ResolveAsset(Arcane::AssetId::FromGuid(id))
                           : std::nullopt;
        });
    // ...and the SAME seam extended to COMPILED ARTIFACTS (Task 7):
    // NriTextureCache uploads content's own BC7/RGBA8 mips from the engine's
    // RETAINED, device-free artifact resolve (Assets::ArtifactFor) instead of
    // a raw RGBA8 decode -- see NriTextureCache::ColorSpace's own
    // routing-rule comment. That is still what makes a textured sprite
    // render without the asset layer ever holding a device handle; only the
    // shape of what crosses the seam changed. PixelSupplyFn is deliberately
    // NOT installed on this vehicle any more: this host builds exactly ONE
    // graph context (see the mode split above) and it is content, Srgb-space,
    // with no Display-space chrome of its own to serve.
    graph.SetArtifactSupply(
        [rt = &*m_runtime](const Arcane::Guid& id) -> const Arcane::LoadedClientArtifact*
        {
            return rt ? rt->AssetsFacade().ArtifactFor(id) : nullptr;
        });
    // F2c Plan 2 Task 3: CPU mesh geometry for NriMeshBufferCache. Looked up
    // live -- m_resolver is set once at boot beside this vehicle.
    graph.SetMeshSupply(
        [this](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
        {
            if (!m_resolver)
                return {};
            const auto s = m_resolver->MeshSupply(id);
            return { s.mesh, s.state };
        });

    // --compare / --bless (Task 8, FINDING 3 of the dispatch audit):
    // resolve the reference BEFORE the settle loop starts, so a reference
    // that does not exist fails FAST -- rather than entering the loop with
    // an invalid m_referencePixels, which would make CompareImages report
    // "could not compare" on every one of up to --settle N attempts, never
    // converge, spam N WARNs, and arrive at the same verdict N attempts
    // later than it had to. The vehicle already exists by this point (the
    // if/else above), so ShutdownGraphPath()'s `if (!graph) return;` guard
    // does not swallow the report this fail-fast path still owes.
    Arcane::ImageCompareOptions compareOptions;
    compareOptions.maxDiffPixels     = m_config.maxDiffPixels;
    compareOptions.maxDiffPixelRatio = m_config.maxDiffPixelRatio;

    if (!m_config.compareReference.empty())
    {
        const std::filesystem::path projectRoot =
            (m_runtime && m_runtime->CurrentProject()) ? m_runtime->CurrentProject()->Root()
                                                        : std::filesystem::path{};
        const char* const backendName = CompareBackendName(m_config.backend);
        const Arcane::NriDeviceCaps& caps = graph.Device().Caps();
        m_compareAdapterSet = Arcane::ReferenceAdapterSet(backendName, caps.adapterName,
                                                          caps.softwareAdapter);
        if (!m_compareAdapterSet.empty())
            ARC_INFO("--compare: software adapter '{}' -> reference set '{}'",
                     caps.adapterName, m_compareAdapterSet);
        m_compareResolution = Arcane::ResolveReference(projectRoot, m_config.compareReference,
                                                        backendName, m_compareAdapterSet);

        if (m_compareResolution.level == Arcane::ReferenceLevel::None)
        {
            if (!m_config.bless)
            {
                // Fail fast: a reference that does not exist will not start
                // existing after N wasted settle attempts, and this is the
                // same treatment a parse-time refusal already gets for a
                // comparably "this can never work" shape.
                ARC_ERROR("--compare '{}': no reference image on disk for backend '{}' -- "
                          "re-run with --bless to create one",
                          m_config.compareReference, backendName);
                m_compareMissingFatal = true;
                m_graphExit = 4;
                ShutdownGraphPath();
                return;
            }
            // else: --bless is present -- the normal FIRST bless. Fall
            // through with compareRequested false below (Finding 4): there
            // is nothing yet to compare against, and the conjunct would be
            // disabled for the whole run regardless.
        }
        else if (!m_config.bless)
        {
            // A real reference exists and this run is not overwriting it --
            // load it for the settle loop's compare conjunct.
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
        // nothing to load; compareRequested stays false either way (Finding 4).
    }

    // Finding 4: disabled outright whenever --bless is set, regardless of
    // whether m_compareResolution actually found anything -- see
    // FrameIo::compareRequested's own comment for why blessing must not
    // also gate convergence.
    const bool compareRequested = !m_config.compareReference.empty() && !m_config.bless;

    // THE VISIBILITY READBACK RING (F3 plan 2 T5), armed UNCONDITIONALLY on
    // this host -- and the unconditional part is the ruling, not an oversight.
    // This host has TWO consumers of the count and both are live on every run:
    // the HUD line (BuildHud, below -- built on every frame of every run,
    // windowed and headless alike) and, when one was asked for, the report's
    // `visibility.gpuVisible`. Spec s9.5's desk pass is "orbit a scene and
    // watch the stats line", which is not a --report run at all, so gating the
    // ring on --report would leave the overlay permanently reading
    // "unavailable" for the exact workflow it exists to serve.
    //
    // "Opt-in" still holds where it matters: the ring is armed by a CALL, and
    // an unarmed GpuScene (every other consumer of this engine -- the editor
    // outside --report, MeshDocument's preview, the thumbnail harvester, the
    // [gpu] tests) declares no copy node and pays nothing. What this line says
    // is that a host which SHOWS the number is a legitimate place to ask for
    // it. The cost on this host is one small copy per frame (the slot's args
    // plus rowCount * 4 bytes) into a HOST_READBACK buffer, published from the
    // graveyard with no wait of any kind.
    if (!Arcane::GpuSceneArmVisibilityReadback(graph.Scene()))
        ARC_WARN("the GPU-scene visibility readback could not be armed -- the HUD's "
                  "\"GPU visible\" line, and any --report's visibility.gpuVisible, stay unavailable");

    // Boot is over; anything the watchdog reports from here on belongs to the
    // frame loop, not to a stale boot stage.
    Arcane::Diagnostics::SetPhase("runtime frame loop");

    // Bundles the pointers/locals MainLoop owns so the six RuntimeFrame
    // functions below (its old body, extracted verbatim -- see
    // RuntimeFrame.hpp/.cpp) can read and write them without being RuntimeApp
    // members. Constructed once: none of these bindings change identity
    // frame-to-frame (the live vehicle/m_resolver are set once at boot; the
    // rest are references, so mutations through `io` land on the real
    // members/locals directly).
    Arcane::RuntimeFrame::FrameIo io
    {
        .gpu             = m_gpu.get(),
        // THE LIVE VEHICLE, resolved once, here -- so the frame body never
        // branches on --headless to find its graph. See Graph().
        .graph           = &graph,
        .resolver        = m_resolver ? &*m_resolver : nullptr,
        .runtime         = &*m_runtime,
        .plugin          = &*m_plugin,
        .app             = *this,
        .config          = m_config,
        .perf            = m_perf,
        .frameCount      = m_frameCount,
        .graphExit       = m_graphExit,
        .frameGlobals    = m_frameGlobals,
        .hostClock       = m_hostClock,
        .lastFrameDt     = m_lastFrameDt,
        .pickDrawables   = m_pickDrawables,
        .pickSelectedIds = m_pickSelectedIds,
        .gpuSceneFrame   = m_gpuSceneFrame,
        .captureRead     = m_captureRead,
        .captureWidth    = m_captureWidth,
        .captureHeight   = m_captureHeight,
        .captureRgba     = m_captureRgba,
        .previousCaptureRgba   = m_previousCaptureRgba,
        .previousCaptureWidth  = m_previousCaptureWidth,
        .previousCaptureHeight = m_previousCaptureHeight,
        .previousCaptureValid  = m_previousCaptureValid,
        .settleAttemptsUsed    = m_settleAttemptsUsed,
        .settleConverged       = m_settleConverged,
        .settleStartedAt       = m_settleStartedAt,
        .settleElapsedMs       = m_settleElapsedMs,
        .settleBail            = m_settleBail,
        .settleCaptureFailed   = m_settleCaptureFailed,
        // Fix round 1, item 1: the SAME instance SceneRenderResolver::Services
        // already points at (StageSpriteTables's `rs.compiler = &m_shaderCompiler`)
        // -- CaptureTail reads its IsIdle() to conjoin quiescence into
        // convergence, never a second compiler.
        .compiler              = m_shaderCompiler,
        // --compare / --bless (Task 8) -- see FrameIo's own field comments.
        // compareRequested/compareOptions are MainLoop locals (computed
        // just above, before this struct); referencePixels/compareResult/
        // compareEvaluated are RuntimeApp members so ShutdownGraphPath can
        // read them once the loop has ended.
        .compareRequested      = compareRequested,
        .referencePixels       = m_referencePixels,
        .compareOptions        = compareOptions,
        .compareResult         = m_compareResult,
        .compareEvaluated      = m_compareEvaluated,
#if !defined(ARC_BUILD_DIST)
        .gpuFaultFired   = m_gpuFaultFired,
        .hangMainFired   = m_hangMainFired,
#endif
        .simPrev         = simPrev,
        .lastFrameTime   = lastFrameTime,
        .lastShaderPoll  = lastShaderPoll,
    };

    while (running)
    {
        // The OS asked for this session to end -- Ctrl-C, the console close
        // box, logoff or shutdown, all routed through
        // Diagnostics::RequestCleanExit into the hook main() installed (crash
        // window plan 1, task 9; spec S5.7). First in the frame, and an
        // unconditional break: a session-end handler has about five seconds,
        // so the ordinary exit has to start on the very next frame boundary.
        if (CleanExitRequested())
        {
            ARC_INFO("ArcaneRuntime: exiting on a clean-exit request (Ctrl-C, console close, or session end)");
            break;
        }

        // Heartbeat, device-lost check, this frame's window-event pump and
        // resize handling -- true means the window's close was requested or
        // the device was lost; io.skipFrame (minimized window) asks for the
        // ordinary `continue` a bare bool return cannot express by itself.
        if (Arcane::RuntimeFrame::PumpAndResize(io)) break;
        if (io.skipFrame) continue;

        // Per-frame timing + input sampling + the fixed-step sim advance.
        // io.quit covers the Pressed("quit") input-action exit, the one quit
        // path PumpAndResize's own return does not cover.
        Arcane::RuntimeFrame::AdvanceSim(io);
        if (io.quit) break;

        // ImGui BeginFrame + the host HUD + DrawUIAll...
        Arcane::RuntimeFrame::BuildHud(io);
        // ...then the scene resolver's Refresh -- the prep the render half
        // below needs. Kept separate from BuildHud so each call says what it
        // does.
        Arcane::RuntimeFrame::PrepareFrame(io);
        if (io.skipFrame) continue;

        // =============================================================
        // THE RENDER HALF. RenderGraph's body lives in RuntimeFrame.cpp; the
        // call stays here because only MainLoop's own loop can break/continue.
        // =============================================================
        const Arcane::NriGraphContext::FrameOutcome outcome = Arcane::RuntimeFrame::RenderGraph(io);
        if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)  break;
        if (outcome == Arcane::NriGraphContext::FrameOutcome::Skipped) continue;

        // The shared tail: the plugin's hot-reload poll, the perf tick, the
        // frame counter, and the screenshot bookkeeping on the final frame.
        // The while condition above ends the loop, not a break.
        if (Arcane::RuntimeFrame::CaptureTail(io))
            running = false;
    }

    // Destroys the vehicle and folds a grown RenderErrorCount into the exit
    // code -- read AFTER the last NRI object is gone, so a teardown-only
    // validation error still fails the run.
    ShutdownGraphPath();
}

void RuntimeApp::PushSceneCamera(float viewportWidth, float viewportHeight)
{
    // Scene camera: the ACTIVE Camera entity owns the view. Pushed HERE, after
    // the plugin's update ran, so a scene that ships a camera beats a plugin
    // that also pushes one -- the scene is the authored artifact. No camera
    // leaves the stored view untouched (a plugin that drives the camera
    // itself via ClientRuntime::SetView therefore still works) and says so
    // once, rather than substituting an identity view that would render an
    // older scene as an unexplained black window.
    int camCount = 0;
    const auto view = Arcane::ActiveSceneCamera(m_runtime->Registry(),
                                                glm::uvec2{ static_cast<std::uint32_t>(viewportWidth),
                                                            static_cast<std::uint32_t>(viewportHeight) },
                                                &camCount);
    if (view)
        m_runtime->SetView(view->view);
    else if (!m_warnedNoSceneCamera)
    {
        // The old message said "no active Camera entity" for THREE
        // different situations and cost a debugging round: no Camera
        // component in the scene at all, one present but active==false or
        // orthographicSize<=0, or a component view that is empty in THIS
        // module because it disagrees with the loader about the component
        // id. Count the components here, host-side, and say which it is --
        // the resolver logs its own census from inside Arcane.dll, so the
        // two lines together localise the last case.
        int total = 0;
        m_runtime->Registry().CreateView<const Arcane::Camera>().ForEach(
            [&](Astra::Entity, const Arcane::Camera&) { ++total; });
        if (total == 0)
            ARC_WARN("scene has no Camera component at all -- nothing sets the view. "
                     "Add a Camera component to an entity (a New Scene ships one).");
        else
            ARC_WARN("scene carries {} Camera component(s) but none is usable "
                     "(active == false, or orthographicSize <= 0) -- nothing sets "
                     "the view", total);
        m_warnedNoSceneCamera = true;
    }
    if (camCount > 1 && !m_warnedMultiSceneCamera)
    {
        ARC_WARN("scene carries {} active Camera entities -- the first found wins", camCount);
        m_warnedMultiSceneCamera = true;
    }
}

void RuntimeApp::ShutdownGraphPath()
{
    // EITHER vehicle, never both (Graph()'s own comment). Null here means no
    // vehicle was ever created -- a boot failure ahead of the one that would
    // have built it -- which is the sole no-op case this function has.
    Arcane::NriGraphContext* const graph = Graph();
    if (!graph)
        return;

#if !defined(ARC_BUILD_DIST)
    // --pick-probe's ANSWER, read while the vehicle is still alive (the reset
    // below takes the readback buffer with it) and reported as an exit code so
    // a desk battery item is one scriptable line.
    //
    // 0/1 on hit/miss is the flag's own contract, and it is deliberately
    // SUBORDINATE to the run's: a probe that never got to report because the
    // run died already has m_graphExit == 1, and the errors-grew case sets 2
    // below. So this only ever turns a CLEAN run into a miss.
    if (m_config.pickProbe)
    {
        const std::optional<std::uint32_t> id = graph->ProbeId();
        if (!id.has_value())
        {
            ARC_ERROR("[nri-graph] --pick-probe ({}, {}): NO READBACK LANDED -- the run was too "
                      "short (the copy lands a couple of frames after the pass that wrote it) or "
                      "the probe pixel was outside the surface",
                      m_config.pickProbeX, m_config.pickProbeY);
            if (m_graphExit == 0)
                m_graphExit = 1;
        }
        else if (*id == 0u)
        {
            ARC_INFO("[nri-graph] --pick-probe ({}, {}): id 0 -- BACKGROUND (miss); {} pickable(s) "
                     "were in the scene", m_config.pickProbeX, m_config.pickProbeY,
                     m_pickDrawables.size());
            if (m_graphExit == 0)
                m_graphExit = 1;
        }
        else
        {
            const Astra::Entity hit = Arcane::PickEntityForId(m_pickDrawables, *id);
            ARC_INFO("[nri-graph] --pick-probe ({}, {}): id {} -- HIT (entity {}) of {} pickable(s)",
                     m_config.pickProbeX, m_config.pickProbeY, *id,
                     hit.IsValid() ? std::to_string(hit.GetID()) : std::string("<unmapped>"),
                     m_pickDrawables.size());
        }
    }
#endif

    // ---- pick@x,y readback (Task 9), read BEFORE the vehicle resets -----
    // (same reason the --pick-probe block above reads early: ProbeId() reads
    // state that dies with the vehicle). NOT ARC_BUILD_DIST-gated, unlike that
    // block: this is the report's pick channel, not a dev-only exit-code
    // check, so it exists in every build configuration. Uses the SAME
    // Arcane::FirstPickProbe lookup RuntimeFrame.cpp's RenderGraph used to
    // arm FrameDesc::pickPixel every frame, so the pixel resolved against
    // here can never disagree with the one that was actually probed.
    //
    // Stored as a local rather than pushed straight into a VerifyReport,
    // because that object is not constructed until the --report block
    // further down (it needs m_frameCount/m_graphExit settled first) -- this
    // captures the FACTS while the vehicle is still alive, and the block
    // below hands them to report.SetPick() once the report exists.
    struct PickResolution
    {
        std::int32_t  armedX = 0, armedY = 0;
        bool          landed = false;
        // The pick surface's own extent, stashed here for the SAME reason
        // armedX/armedY are: graph is about to be reset, and VerifyReport
        // needs this to tell "out of range" apart from "too short" itself
        // (fix round 1) rather than trusting a bool computed here.
        std::uint32_t surfaceWidth = 0, surfaceHeight = 0;
        std::uint32_t hitProxyId = 0;
        bool          resolved = false;
        std::string   entityName;
        std::string   entityGuid;
    };
    std::optional<PickResolution> pickResolution;
    if (!m_config.reportPath.empty())
    {
        if (const std::optional<Arcane::ProbeSpec> pickSpec = Arcane::FirstPickProbe(m_config.probes))
        {
            // GUARANTEED offscreen (Fix 3, final fix wave): HostConfig::Parse's
            // wantsOffscreenOnly gate refuses --report without --headless,
            // unconditionally, for every host, so this function can never be
            // reached with a windowed run. A WINDOWED branch used to sit here
            // (a pick@ probe on a windowed run) -- it was already unreachable
            // by the same reasoning its own comment gave ("reasoned from
            // source... not run windowed to confirm (process constraint)"),
            // and the plan's Task 9 desk item F that was written to exercise
            // it describes a command line this gate refuses at parse time,
            // before any of this code runs. Deleted rather than kept
            // explainable-away; asserted, not just assumed, so a future change
            // to that gate fails loudly here instead of silently reviving a
            // mode this code no longer understands.
            ARC_ASSERT(m_config.headless, "ShutdownGraphPath's pick@ resolution reached with a "
                                          "windowed run -- HostConfig::Parse's --report-requires-"
                                          "--headless gate should make this unreachable");

            PickResolution res;
            res.armedX = pickSpec->x;
            res.armedY = pickSpec->y;
            // Always known once the vehicle exists (unconditional on
            // in-range-ness) -- lets VerifyReport itself distinguish
            // "out of range" from "too short" later (fix round 1's
            // design-question answer: these are different facts).
            res.surfaceWidth  = graph->SurfaceWidth();
            res.surfaceHeight = graph->SurfaceHeight();

            // Fix round 1, item 2: --pick-probe's OWN out-of-range latch
            // (NriGraphContext.cpp's m_probeOutOfRange) is keyed to
            // config.pickProbe -- the OLD flag's state, always false here
            // -- so ProbeId() would otherwise happily return the CLAMPED
            // EDGE TEXEL's hit-proxy id for e.g. `pick@9999,9999`: a
            // confident answer for a pixel nobody asked about. Checked
            // BEFORE calling ProbeId() at all, so that value is never
            // trusted.
            const bool inRange = Arcane::PickPixelInRange(pickSpec->x, pickSpec->y,
                                                          graph->SurfaceWidth(), graph->SurfaceHeight());
            const std::optional<std::uint32_t> id = inRange ? graph->ProbeId() : std::nullopt;
            if (!inRange)
            {
                ARC_ERROR("[nri-graph] pick@{},{}: NO READBACK LANDED -- the probe pixel is "
                          "outside the {}x{} surface",
                          pickSpec->x, pickSpec->y, graph->SurfaceWidth(), graph->SurfaceHeight());
                // res.landed stays false, same as the "too short" case
                // below -- VerifyReport's shared "NO READBACK LANDED"
                // wording already names both causes disjunctively,
                // matching --pick-probe's own precedent
                // (NriGraphContext.cpp's ProbeId(), which folds "no node"
                // and "out of range" into the same nullopt).
            }
            else if (!id.has_value())
            {
                ARC_ERROR("[nri-graph] pick@{},{}: NO READBACK LANDED -- the run was too short "
                          "(the copy lands a couple of frames after the pass that wrote it) or "
                          "the probe pixel was outside the surface",
                          pickSpec->x, pickSpec->y);
                // res.landed stays false -- the report itself carries
                // this as an honest error entry (VerifyReport::Evaluate's
                // Pick case), never a silently-dropped probe. Deliberately
                // does NOT touch m_graphExit: the report's job is to state
                // facts, not to change what "did the host run" means for
                // this process.
            }
            else
            {
                res.landed     = true;
                res.hitProxyId = *id;
                if (*id == 0u)
                {
                    ARC_INFO("[nri-graph] pick@{},{}: hit-proxy 0 -- BACKGROUND (miss); {} "
                             "pickable(s) were in the scene",
                             pickSpec->x, pickSpec->y, m_pickDrawables.size());
                }
                else
                {
                    const Astra::Entity hit = Arcane::PickEntityForId(m_pickDrawables, *id);
                    if (hit.IsValid())
                    {
                        if (const Arcane::Identity* identity =
                                std::as_const(m_runtime->Registry()).GetComponent<Arcane::Identity>(hit))
                        {
                            res.resolved   = true;
                            res.entityName = identity->name;
                            res.entityGuid = identity->id.ToString();
                            ARC_INFO("[nri-graph] pick@{},{}: hit-proxy {} -- HIT (\"{}\", id {}) "
                                     "of {} pickable(s)",
                                     pickSpec->x, pickSpec->y, *id, res.entityName, res.entityGuid,
                                     m_pickDrawables.size());
                        }
                        else
                        {
                            ARC_WARN("[nri-graph] pick@{},{}: hit-proxy {} -- HIT but the entity "
                                     "carries no Identity component; the report will carry an "
                                     "honest error rather than a stable-looking id that would "
                                     "silently churn between runs",
                                     pickSpec->x, pickSpec->y, *id);
                        }
                    }
                    else
                    {
                        ARC_WARN("[nri-graph] pick@{},{}: hit-proxy {} does not map to any entity "
                                 "in this run's {} pickable(s) -- stale id?",
                                 pickSpec->x, pickSpec->y, *id, m_pickDrawables.size());
                    }
                }
            }
            pickResolution = res;
        }
    }

    // THE GPU CULL'S OWN VISIBLE-ROW COUNT, read here for the same reason the
    // pick answer above is: the reset below takes the readback ring with it.
    // Nullopt when the ring was never armed (no --report) or when no frame's
    // readback had completed -- the report says so rather than substituting
    // the CPU count (VerifyReport::SetVisibility's own contract).
    const std::optional<std::uint32_t> gpuVisibleRows = Arcane::GpuSceneVisibleRows(graph->Scene());

    // This reset is what destroys every NRI object (graph, cache, ring,
    // swapchain, NRI device, native device), and teardown ordering is exactly
    // the class of mistake a validation layer exists to catch -- so the latch
    // is sampled strictly after it, never from inside a still-live vehicle.
    //
    // The WINDOW is NOT in that list: the vehicle borrows the host's, and
    // this reset is precisely what guarantees the swapchain naming its
    // HWND/surface dies while that window is still alive -- MainLoop calls
    // this on every exit path, and m_gpu outlives it regardless (member
    // order).
    //
    // BOTH are reset, unconditionally, because exactly one of them is live
    // and a `if (offscreen) ... else ...` here would be a second copy of a
    // decision Graph() already made. The offscreen vehicle owns its OWN
    // device rather than borrowing anything from the window, so it has no
    // ordering debt against m_gpu -- but the latch below still has to be
    // sampled after its last NRI object is gone, which is what this line is.
    m_graphContext.reset();
    m_offscreen.reset();

    // A present Tier 1 overlay is NAMED on the summary line: the drains tag
    // every debug-layer message by producer, but "[d3d12]" cannot say whether
    // the message was ours or the overlay's, and a desk reading this line
    // after a red run must not start hunting an Arcane bug that is GPU Tweak
    // III's. Fresh scan (microseconds), never per frame: a module that
    // injected after device creation is still named here.
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
        ARC_ERROR("[nri-graph] FAILED: {} validation/render error(s) fired during the run "
                  "(teardown included)", errorsNow - m_graphErrorBaseline);
        // Precedence 1 > 2: a run failure says WHERE the run died and outranks
        // the errors it produced on the way out.
        if (m_graphExit == 0)
            m_graphExit = 2;
    }

    // --settle N (Task 10): FAIL EXPLICITLY on non-convergence -- checked
    // HERE, not only inside the --report block below, because an agent that
    // checks only the PROCESS EXIT CODE (never opens the JSON) must still see
    // this fail. m_settleConverged/m_settleAttemptsUsed are only meaningful
    // when settle was actually requested (m_config.settleAttempts != 0); a
    // run that never asked for it leaves both at their defaults, which must
    // never read as a failure.
    //
    // Precedence 1/2 > 3: a run that already failed for a REAL reason
    // (render-failed or validation-errors, folded into m_graphExit just
    // above) keeps that reason. Settle non-convergence is downstream of both
    // -- CaptureTail's settle branch is only ever reached on a Presented
    // outcome, past any point either of those could have fired -- so this
    // only ever promotes an otherwise-clean exit code.
    const bool settleFailed = m_config.settleAttempts != 0 && !m_settleConverged;
    if (settleFailed && m_graphExit == 0)
        m_graphExit = 3;

    // --compare / --bless (Task 8). NOT nested inside the `--report` block
    // below: --settle only requires --screenshot OR --report (HostConfig.cpp),
    // so a run can legally pair --compare/--bless with --screenshot and no
    // --report at all -- the PNG this writes must land regardless of whether
    // a JSON report was ever asked for.
    //
    // FINDING 4 (Task 8 dispatch audit): a --bless run's settle loop
    // converges on byteEqual && idle ALONE -- the compare conjunct is
    // disabled for the whole run (FrameIo::compareRequested) -- so
    // m_captureRead here means exactly "the settle loop converged", and
    // THIS is where that converged capture actually becomes the new
    // reference. Never attempted when settleFailed (nothing converged to
    // bless) or when --bless was not given at all.
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

    // THE DIFF ARTIFACT (Task 8; HOISTED OUT of the --report block by
    // final-review I-2). It sits here, beside the bless write, for the SAME
    // reason the comment above gives that write: --settle requires --screenshot
    // OR --report (HostConfig.cpp), so `--compare X --settle N --screenshot
    // out.png` with no --report is a fully legal invocation. It used to live
    // inside the --report block, which meant a mismatch on such a run produced
    // NO diff image at all -- while ImageCompare.hpp:221-223 calls diffRgba
    // "the artifact that makes a failure diagnosable rather than a number".
    // The diagnostic was conditional on the wrong flag; scripts/golden-gate.ps1
    // always passes --report, which is the only reason nobody had hit it.
    //
    // The resulting path is carried into SetCompare below WHEN a report was
    // asked for; when it was not, the PNG still lands on disk and the run's
    // stderr/exit code is what names the failure.
    //
    // The guard reproduces the report block's if/else-if chain exactly rather
    // than just testing m_compareEvaluated: m_compareMissingFatal /
    // compareBlessed / compareWriteFailed each shadow the evaluated branch
    // there, so spelling all four out keeps the two sites provably in step
    // (they are mutually exclusive today -- a --bless run never evaluates a
    // comparison at all, Finding 4 -- but this file's own convention is to
    // encode that agreement rather than rely on it).
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
        // Empty means DiffArtifactPath REFUSED the name (path-traversal guard);
        // both hosts check it before writing -- never write a path it declined.
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

    // THE REPORT (Task 8), written LAST in this function so exitReason below
    // can read the FINAL m_graphExit -- including the fold just above, which
    // only settles after the vehicle is gone. WriteTo never touches
    // m_graphExit itself, so this call only DESCRIBES what already happened;
    // Run()'s tail still decides the process exit code exactly as it did
    // before this task.
    //
    // Guarded on reportPath alone (HostConfig.hpp's "Empty = off"), not on
    // --probe: a bare `--report` with no probes still writes a valid report
    // (run identity + whatever AddCensus below always contributes), and a
    // bare `--probe` with no `--report` intentionally writes nothing -- there
    // is nowhere for it to go.
    if (!m_config.reportPath.empty())
    {
        const bool completedAllFrames =
            m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames;

        // Reuses the SAME vocabulary Run()'s own tail comment already
        // documents for m_graphExit (1 = the graph run failed, 2 =
        // RenderErrorCount grew) and the device-lost check it makes right
        // alongside -- this is not a new classification, just the existing
        // one spelled into the exit-reason string VerifyReport carries.
        // "frames-complete" is what a normal run reports; "stopped-early"
        // covers any exit this function was not told the reason for (e.g. an
        // interactive quit -- unreachable under --headless today, but this
        // call site is unconditional and must not lie about a mode it does
        // not recognise).
        std::string exitReason = "frames-complete";
        if (Arcane::GpuDeviceLostObserved())
            exitReason = "device-lost";
        else if (m_graphExit == 1)
            exitReason = "render-failed";
        else if (m_graphExit == 2)
            exitReason = "validation-errors";
        // --compare / --bless (Task 8): checked BEFORE settleFailed below.
        // m_compareMissingFatal's fail-fast path also leaves settleAttempts
        // != 0 and m_settleConverged false (the loop never ran at all), so
        // settleFailed reads true there too -- this branch has to win, or a
        // refused/absent reference would misreport as "the run simply never
        // converged" instead of naming --bless as the fix.
        else if (m_compareMissingFatal)
            exitReason = "compare-missing-reference";
        // --settle N (Task 10): checked BEFORE completedAllFrames below --
        // a non-converged settle run has m_frameCount >= m_config.maxFrames
        // by construction (settle-hold frames only ever run AFTER the base
        // budget is spent), so completedAllFrames reads true regardless of
        // whether the run actually converged. Left unguarded here it would
        // silently fall through to "frames-complete", which is exactly the
        // "hands back an unconverged frame without admitting it" failure
        // mode the plan calls out by name.
        else if (settleFailed)
        {
            // Task 8: settleFailed alone now conflates two different
            // causes, now that the convergence predicate has grown a third
            // conjunct -- "the pixels never stabilised/finished loading"
            // and "they stabilised, but did not match the reference" both
            // leave m_settleConverged false. m_compareEvaluated only ever
            // latches true from an ACTUAL CompareImages() call, which
            // Finding 4 means can never happen on a --bless run (the
            // conjunct is disabled there entirely) -- so this can only ever
            // read "compare-failed" when --bless was NOT given.
            exitReason = m_compareEvaluated ? "compare-failed" : "settle-not-converged";
        }
        else if (compareBlessed)
            exitReason = "compare-blessed";
        else if (compareWriteFailed)
            exitReason = "compare-failed";
        else if (!completedAllFrames)
            exitReason = "stopped-early";

        Arcane::VerifyReport report;
        // No `offscreen` argument (Fix 3, final fix wave): SetRun no longer
        // takes one -- this whole block is only ever reached with
        // m_config.headless == true, guaranteed by HostConfig::Parse's
        // wantsOffscreenOnly gate (--report requires --headless,
        // unconditionally, for every host). See VerifyReport.hpp's SetRun
        // comment for the full reasoning.
        report.SetRun(Arcane::ToString(m_config.backend), m_frameCount, exitReason);

        // Only set when CaptureTail (RuntimeFrame.cpp) actually landed a
        // readback this run -- an early exit, or a run that asked for
        // neither --report-needing probe nor --screenshot, leaves this
        // false, and SetCapture is skipped so Brightness/Luma/Rgba probes
        // report an honest "no capture set" rather than a phantom frame.
        if (m_captureRead)
            report.SetCapture(m_captureWidth, m_captureHeight, m_captureRgba);

        // The --settle verdict (Task 3), previously visible ONLY in this
        // process's log. TWO conditions, not one. Settle has to have been asked
        // for -- the members below are meaningless otherwise -- AND the loop
        // has to have reached a verdict.
        //
        // THE SECOND CONDITION IS NOT REDUNDANT. This function also runs for
        // runs that died AHEAD of the settle loop: MainLoop fails fast on a
        // missing or undecodable --compare reference (see m_compareMissingFatal
        // in the exitReason chain above), and device-lost / render-failed /
        // validation-errors exit the same way. Those runs still have
        // settleAttempts != 0 while m_settleBail is untouched at Keep, so
        // guarding on the count alone would report settleAttemptsUsed 0 with
        // settleBailReason "timeout-bound" -- telling an agent to raise
        // --settle-timeout for a run that never spent one and whose real fault
        // exitReason already names. VerifyReport::SetSettle refuses such a call
        // itself; this guard states the intent where the facts live.
        //
        // m_settleAttemptsUsed is passed AS MEASURED, never m_config.settleAttempts.
        // Neither host may substitute the budget for the count; the editor host
        // used to (it forged the counter to stop its loop), which is why that
        // was reconciled in this same commit rather than after it -- an absent
        // field is an honest gap, a present wrong one is a false report an
        // agent will trust.
        if (m_config.settleAttempts != 0
            && Arcane::SettleVerdictReached(m_settleConverged, m_settleBail))
        {
            report.SetSettle(m_settleAttemptsUsed, m_settleConverged, m_settleBail,
                              m_settleCaptureFailed);
        }

        // The census is carried whether or not a `census` PROBE was asked
        // for -- ToJson's top-level "census" field and a `census` probe
        // entry both read AddCensus's same values, and Materials() is a
        // live, on-demand read (SceneRenderResolver.hpp's own comment on
        // why it re-walks the registry rather than replaying a stale
        // Refresh) -- so populating it always costs nothing extra.
        if (m_resolver)
        {
            const Arcane::SceneRenderResolver::MaterialCensus census = m_resolver->Materials();
            report.AddCensus(census.spriteReferenced, census.spriteBound,
                              census.postReferenced, census.postBound,
                              census.meshReferenced, census.meshBound);
        }

        // THE VISIBILITY COUNTS (schemaVersion 8, F3 plan 1 T8): the last
        // frame's GpuSceneFrame::Stats -- every live GPU-scene row, the
        // coarse-visible ones against the mesh view, the batches and draws
        // emitted. Carried unconditionally like the census: a 2D scene
        // honestly reports zeros, and so does a scene with no active
        // perspective camera -- no mesh view means no frame was built at
        // all (PrepareSceneForRender's early return), not "everything culled".
        //
        // `gpuVisible` (plan 2 T5) is the LAST COMPLETED readback's count, or
        // nullopt -- never the coarse count wearing the GPU's name: a run that
        // never armed the ring, or one whose frames were all still in flight,
        // has not measured it and says so.
        report.SetVisibility(m_gpuSceneFrame.stats.total, m_gpuSceneFrame.stats.coarseVisible,
                             m_gpuSceneFrame.stats.batches, m_gpuSceneFrame.stats.draws,
                             static_cast<std::uint32_t>(m_gpuSceneFrame.transparentDraws.size()),
                             gpuVisibleRows);

        // THE INJECTED MODULES (schemaVersion 10): the scan taken above for
        // the RenderErrorCount line, carried unconditionally like the census
        // -- a clean desk reports `[]`, and a red lane on a desk with an
        // overlay is attributable from this file alone.
        report.SetForeignModules(foreignModules);

        // THE --set ECHO (schemaVersion 14, settings S6-GATE): each --set name
        // with the value the registry PUBLISHED, so a witness proves its --set
        // landed on a declared cvar.
        report.SetCVarSets(Arcane::VerifyReport::EchoCVarSets(m_config.cvarSets));

        // The pick@x,y readback (Task 9), captured above while the vehicle
        // was still alive. Only set when a `pick@` probe was actually
        // present -- a run with none leaves this unset, and Evaluate's Pick
        // case (unreachable then, since no Pick spec exists to evaluate)
        // would report "no pick set" if it somehow were.
        if (pickResolution)
        {
            report.SetPick(pickResolution->armedX, pickResolution->armedY, pickResolution->landed,
                            pickResolution->hitProxyId, pickResolution->resolved,
                            pickResolution->entityName, pickResolution->entityGuid,
                            pickResolution->surfaceWidth, pickResolution->surfaceHeight);
        }

        // The --compare verdict (Task 8). Emitted ONLY when a comparison
        // was actually requested, so an agent can tell "not asked" from
        // "asked and passed" -- VerifyReport::SetCompare's own contract.
        // Every --compare outcome (converged-and-matched, converged-and-
        // mismatched, budget-exhausted, blessed, missing-reference) funnels
        // through this ONE call with different arguments, rather than each
        // growing its own reporting path.
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

            // Schema 5: `compare.triedPaths` -- the ordered candidate list
            // ResolveReference actually probed, stringified with
            // generic_string() (forward slashes on every host OS). Mirrors
            // resolvedLevel/referencePath's own pattern below: seeded from
            // m_compareResolution (the fail-fast resolve above), and
            // re-seeded from `after` in the bless branch for the same
            // staleness reason -- the report must describe the resolution
            // that actually applied, not the one captured before the write.
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
            std::vector<std::string> triedPaths = stringifyTriedPaths(m_compareResolution.triedPaths);
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
            else if (compareBlessed)
            {
                // m_compareResolution was captured BEFORE the write
                // (Finding 3), so its level is stale for a first bless (was
                // None, is now Shared) -- re-resolve so the report
                // describes where the reference actually ended up, not
                // where it started. Cheap: an fs::exists check, nothing
                // more.
                const Arcane::ReferenceResolution after =
                    Arcane::ResolveReference(projectRoot, m_config.compareReference, backendName,
                                             m_compareAdapterSet);
                resolvedLevel = levelName(after.level);
                referencePath = after.path.string();
                triedPaths    = stringifyTriedPaths(after.triedPaths);
                passed        = true;   // the reference now IS the capture, by construction
            }
            else if (compareWriteFailed)
            {
                errorMessage = "the settle loop converged, but writing the blessed reference to "
                               "'" + m_compareResolution.blessTarget.string() + "' failed";
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

                // The write itself happens ABOVE, outside this --report block
                // (final-review I-2) -- the diff PNG must land whether or not a
                // JSON report was asked for. All that is left here is to REPORT
                // where it went; empty means it was never written (a pass, or a
                // refused/failed path), which is exactly what this field meant
                // before the hoist.
                diffPath = compareDiffPath;
            }
            else
            {
                // --settle exhausted its budget without ever reaching
                // byteEqual&&idle once -- the comparison never actually
                // ran (settleFailed's own "settle-not-converged" exitReason
                // above already explains why).
                errorMessage = "the settle loop never reached a stable, idle frame, so no "
                               "comparison ever ran";
            }

            report.SetCompare(m_config.compareReference, resolvedLevel, referencePath, passed,
                               diffCount, diffRatio, maxDiffPixelsUsed, sizesMismatch, diffPath,
                               errorMessage, maxLocalDifference, triedPaths);
        }

        // Parse-then-evaluate, never silently drop a malformed --probe: an
        // agent that typo'd a probe kind gets a loud ARC_ERROR rather than a
        // report that just never mentions it (ParseProbe's own header
        // comment on why an unparseable probe is a refusal, not a skip).
        std::vector<Arcane::ProbeSpec> specs;
        specs.reserve(m_config.probes.size());
        for (const std::string& raw : m_config.probes)
        {
            std::string error;
            if (std::optional<Arcane::ProbeSpec> spec = Arcane::ParseProbe(raw, error))
                specs.push_back(std::move(*spec));
            else
                ARC_ERROR("--probe '{}': {}", raw, error);
        }
        report.Evaluate(specs);

        if (report.WriteTo(m_config.reportPath))
            ARC_INFO("report written: {} ({} probe(s))", m_config.reportPath, specs.size());
        else
            ARC_ERROR("failed to write report to '{}'", m_config.reportPath);
    }
}

void RuntimeApp::Shutdown()
{
    // NO DEVICE IDLE IS OWED HERE: ~NriGraphContext already idled the one
    // device this process has, and drained its graveyard, before MainLoop
    // returned.
    ARC_INFO("ArcaneRuntime exiting after {} frames", m_frameCount);

    // Written while the game module -- and every Archive cvar it declared -- is
    // still loaded (the member destructors below unload it). A no-op unless this
    // run archives (StageRuntimeCreate).
    if (m_runtime) (void)m_runtime->Core().SaveUserCVars();

    // The member destructors then run (after Run returns + ~RuntimeApp), in reverse
    // declaration order -- the load-bearing TEARDOWN CONTRACT:
    //   m_resolver -> ~SceneRenderResolver: un-publishes the registry's sprite
    //                tables (whose pointers are non-owning). Declared LAST so
    //                it runs FIRST, while the runtime it publishes through is
    //                still alive. It drops no device-bound handle of its own
    //                -- see SceneRenderResolver.hpp's destructor contract.
    //   m_plugin  -> ~PluginHost: Unload (TeardownLive -> ClearSystems +
    //                ResetRegistry) while the plugin DLL is STILL mapped.
    //   m_runtime -> ~Runtime: destroys JobSystem + the now-empty Registry.
    //   m_gpu     -> ~GpuContext: the render/input stack, window LAST. So gpu
    //                outlives runtime + plugin. See GpuContext's header.
    // m_process (and the ProcessContext it holds) is declared BEFORE m_runtime,
    // so it destructs AFTER it; its owned TypeContext is intentionally NOT freed
    // (heap-leaked, see ~ProcessContext / StageRuntimeCreate).
}

int RuntimeApp::Run()
{
    Arcane::HostBoot::BootContext ctx{};
    ctx.runtime     = nullptr;              // stages populate as they go
    ctx.gpu         = nullptr;
    ctx.splash      = m_splash;
    ctx.projectPath = m_config.projectPath.c_str();
    ctx.pluginPath  = m_config.pluginPath.c_str();
    ctx.moduleName  = "ArcaneRuntime.exe";
    // Per-open engine settings, from THIS host's parsed command line. One
    // shared rule (HostBoot::OpenOptionsFor) so the runtime and the editor
    // cannot drift on when a verify run declines the diag:// mount.
    ctx.openOptions = Arcane::HostBoot::OpenOptionsFor(m_config);
    ctx.hostConfig = &m_config;
    // The command-line rung is Editor in Debug/Release for developer Game settings,
    // and LocalHost in Dist. The console overlay uses the live session role.
    ctx.cvarContext = Arcane::CommandLineCVarContext();

    // Spec sec 6 default: the runtime host shows no boot progress until an
    // opened project's own manifest opts in (project_open's ProjectBoot.cpp
    // override flips this once that manifest is known -- see its own
    // comment). Set here, BEFORE BootSequence::Run is even constructed below,
    // so not even the very FIRST present() call (which fires as soon as
    // "runtime_create" completes, before project_open's Worker body has had
    // any chance to run at all -- BootSequence.cpp's dispatch order) can show
    // progress a player never asked to see. BootSplashWindow::SetShowProgress
    // tolerates m_splash == nullptr, same never-fail contract as every other
    // splash call.
    if (m_splash) m_splash->SetShowProgress(false);

    // HostBoot::RuntimeStages(ctx) is the SAME shared function
    // BootStageParityTest exercises and EditorApp calls for its own list
    // (EditorApp::Run) -- this is the literal call that keeps the two hosts
    // from silently diverging on which steps exist. RuntimeStages already
    // carries the Fatal-ABI-refusal override for project_open (ProjectBoot.cpp),
    // so nothing about that stage needs patching here. Everything else that
    // needs RuntimeApp's own private members (m_gpu/m_runtime/m_plugin/
    // m_resolver/...) gets its `run` overwritten below -- ids/deps/policy/
    // thread/weight (the DAG shape the parity test polices) are untouched.
    std::vector<Arcane::BootStage> stages = Arcane::HostBoot::RuntimeStages(ctx);
    for (Arcane::BootStage& stage : stages)
    {
        if (stage.id == "runtime_create")       stage.run = [this, &ctx] { return StageRuntimeCreate(ctx); };
        else if (stage.id == "gpu_core")         stage.run = [this, &ctx] { return StageGpuCore(ctx); };
        else if (stage.id == "render_bridge")    stage.run = [this, &ctx] { return StageRenderBridge(ctx); };
        else if (stage.id == "sprite_tables")    stage.run = [this, &ctx] { return StageSpriteTables(ctx); };
        else if (stage.id == "plugin_load")      stage.run = [this, &ctx] { return StagePluginLoad(ctx); };
        // "finalize" carries real work in this host -- closing the boot
        // splash. See StageFinalize's own comment for why this stage
        // specifically (RuntimeStages appends nothing, so "finalize", the
        // last core stage, is where the editor's own splash_ready points
        // too).
        else if (stage.id == "finalize")         stage.run = [this, &ctx] { return StageFinalize(ctx); };
        // "edit_core" (2026-07-30 review, Fix 5): the editor's undo/redo
        // history and structural-edit binding have no runtime analog -- the
        // runtime has no scene session to undo/redo against. Explicit
        // no-op, stated as one rather than relying on the Unpatched sentinel
        // to happen to look like success.
        else if (stage.id == "edit_core")        stage.run = [] { return true; };
    }

    Arcane::BootSequence seq(std::move(stages));
    // m_splashPresenter is BootSequence's presenter for the WHOLE run --
    // from runtime_create through finalize, every per-stage present()
    // call reports into m_splash's status text + taskbar progress rather
    // than the swapchain, AND (2026-07-30 review round 2, finding 2)
    // arms/checks the splash's own open/closed state so IBootPresenter's
    // documented quit contract (BootSequence.hpp:65) still fires if the user
    // closes the splash mid-boot -- see BootSplashPresenter::Present's own
    // comment. Safe to run unconditionally: it tolerates m_splash ==
    // nullptr, same never-fail contract as BootSplashWindow itself. It is a
    // class member, not constructed here, so StageFinalize can Disarm() it
    // before closing the splash intentionally -- see that method. It is also
    // the ONLY presenter this host has: there is no swapchain-backed
    // presenter, and the window reveal belongs to MainLoop's graph-vehicle
    // creation.
    const Arcane::BootResult boot = seq.Run(&m_splashPresenter);
    if (!boot.ok)
        return boot.quitRequested ? 0 : 1;

    MainLoop();
    // THE QUIT SITE (crash window plan 1, task 9; spec S5.7): the loop is over
    // and everything below is teardown, which is the one failure the watchdog
    // cannot otherwise see -- the loop beat right up to its last frame and
    // there is nothing left to observe. RequestCleanExit swaps the beat rule
    // for a single deadline (Config::exitSeconds, 30 s) covering Shutdown()
    // and the destructors after it, so a render-stack teardown that never
    // returns is reported as "hang at exit" and terminated with 12.
    // Idempotent -- a Ctrl-C that already armed it does not restamp the clock.
    Arcane::Diagnostics::RequestCleanExit();
    Shutdown();
    // F2b Task 6, spec s5 -- "refuse, never limp": a content texture whose cooked
    // artifact is missing or invalid (ArtifactMissing since Task 8's sprite
    // cutover, HashMismatch, or VersionNewerThanEngine -- see Assets.hpp's
    // ContentArtifactRefusalObserved doc comment)
    // latches during resolve (SpriteCache::Request -> Assets::PixelsFor/
    // TextureInfoFor, driven every frame by SceneRenderResolver::Refresh -- see
    // RuntimeFrame.cpp). Checked HERE, after MainLoop, mirroring exactly where the
    // device-lost check below polls its own latch: a game host's one job is
    // running the game correctly, and a cook that is broken or stale is a stop-
    // the-run condition here even though it degrades to a Problems-pane entry in
    // the editor (Task 12) rather than an exit. 5 is a fresh code: 1/2/3/4 are
    // already the graph path's own vocabulary (m_graphExit, documented below) and
    // GpuDeviceLostObserved's own return 1 two lines down -- a content refusal is
    // neither, and naming it distinctly is the whole point of Step 3's "exits
    // nonzero with the refusal named" contract (the refusal ITSELF is named in the
    // ARC_ERROR Assets.cpp already logged when it fired; this print restates it at
    // the point the process is about to exit over it, for a caller that only kept
    // the tail of a long log).
    if (Arcane::ContentArtifactRefusalObserved())
    {
        ARC_ERROR("ArcaneRuntime: exiting -- content artifact refusal: {}",
                  Arcane::ContentArtifactRefusalDetail());
        return 5;
    }
    // A device-loss exit is an abnormal end even though it was orderly: the
    // report exists, but the session did not do what it was asked to.
    if (Arcane::GpuDeviceLostObserved()) return 1;
    // The render path's own codes: 1 = the graph run FAILED (it says WHERE
    // the run died), 2 = RenderErrorCount GREW (a validation error fired,
    // which explains a bad capture rather than the reverse), 3 = --settle N
    // never converged (Task 10 -- two consecutive captures never compared
    // byte-equal before the bail CONJUNCTION fired: BOTH the attempt budget
    // and --settle-timeout spent, never either alone). The lower number
    // wins -- see ShutdownGraphPath. They can fire on ANY run, the render
    // path being unconditional, so 0 here means no graph failure occurred.
    if (m_graphExit != 0) return m_graphExit;
    return 0;
}
