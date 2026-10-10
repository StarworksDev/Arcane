#pragma once
// RuntimeApp: the standalone host application object. Constructed in main from
// a HostConfig; Run() drives Init -> the frame loop -> Shutdown and returns the
// process exit code. Member declaration order m_gpu -> m_runtime -> m_plugin is
// the TEARDOWN CONTRACT (destruct reverse: plugin Unload while the DLL is still
// mapped -> runtime -> the render stack inside GpuContext -> window last). PRESENTATION-FREE + C++23-clean.
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <Arcane/Assets/ImageCompare.hpp>   // --compare (Task 8): PixelData/ImageCompareResult
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/GpuContext.hpp>
#include <Arcane/Host/ReferenceImages.hpp>  // --compare/--bless (Task 8): ReferenceResolution
#include <Arcane/Host/OffscreenVehicle.hpp>   // --headless's vehicle: device + NRI wrap + offscreen graph context, no window, no swapchain
#include <Arcane/Host/FramePerf.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Host/SettleBound.hpp>   // --settle's bail conjunction: SettleBail
#include <Arcane/Host/BootSequence.hpp>
#include <Arcane/Host/BootSplashWindow.hpp>
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/GpuFaultInjector.hpp>   // dev-only --crash-gpu N (kPassName only; the injector is NriDiagnostics::FireFault)
#include <Arcane/Render/GpuSceneTypes.hpp>   // GpuSceneFrame -- the opaque 3D pass's per-frame frame (F3 plan 1 T8)
#include <Arcane/Render/Nri/NriGraphContext.hpp>   // the graph vehicle; unconditional
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
class RuntimeApp
{
public:
    // `splash` is a NON-OWNING pointer to main()'s stack-local
    // Arcane::BootSplashWindow -- same ordering
    // contract as EditorApp's ctor: main constructs and ultimately closes it;
    // RuntimeApp only reads it during Run()'s boot sequence. Null tolerated.
    explicit RuntimeApp(Arcane::HostConfig cfg, Arcane::BootSplashWindow* splash = nullptr);
    int Run();   // BootSequence -> MainLoop() -> Shutdown(); process exit code

    // ---- The clean-exit hook (crash window plan 1, task 9; spec S5.7) ------
    // "Begin your ordinary exit now" for the paths the OS gives seconds rather
    // than a frame: Ctrl-C, the console close box, logoff, WM_ENDSESSION.
    // STATIC and installed from main() BEFORE this object exists -- the window
    // it has to cover starts at Diagnostics::Install, which is now the first
    // thing main does, and the hook runs on whatever thread noticed (the OS's
    // console-handler thread), so it may touch nothing thread-affine. It sets
    // one atomic MainLoop reads at the top of the frame, which is the same
    // stop the window's close box already performs.
    static void InstallCleanExitHook() noexcept;

    // Whether that hook has fired. Read once per frame by MainLoop.
    [[nodiscard]] static bool CleanExitRequested() noexcept;

    // Pushes the scene's ACTIVE Camera entity into the plugin's stored camera,
    // with the once-only diagnostics for "no usable camera" / "several".
    // ONE copy of this reasoning, shared: it is the view the plugin's render
    // submission is driven with.
    //
    // PUBLIC because MainLoop's frame body lives in free functions in
    // namespace Arcane::RuntimeFrame (RuntimeFrame.hpp/.cpp): RenderGraph
    // calls this through FrameIo::app, and a free function cannot reach a
    // private RuntimeApp member. See RuntimeFrame.hpp's header comment.
    void PushSceneCamera(float viewportWidth, float viewportHeight);
private:
    // ---- Boot (RuntimeApp.cpp) -------------------------------------------
    // Run() builds Arcane::HostBoot::RuntimeStages(ctx) -- the SAME shared
    // function BootStageParityTest exercises and EditorApp calls for its own
    // list -- then overwrites the ids below with these closures, because
    // their real work touches RuntimeApp's own private members. See
    // ProjectBoot.cpp's CoreStages header comment for the full rationale;
    // type_context_install/project_open/input_config are NOT in this list --
    // their RuntimeStages/CoreStages body is genuinely shared and used as-is
    // (project_open's Fatal-ABI-refusal override lives IN RuntimeStages
    // itself, not here -- see ProjectBoot.cpp). StageFinalize is in this
    // list because it performs the window reveal -- see its own comment.
    bool StageRuntimeCreate(Arcane::HostBoot::BootContext& ctx);
    bool StageGpuCore(Arcane::HostBoot::BootContext& ctx);
    bool StageRenderBridge(Arcane::HostBoot::BootContext& ctx);
    bool StageSpriteTables(Arcane::HostBoot::BootContext& ctx);
    bool StagePluginLoad(Arcane::HostBoot::BootContext& ctx);
    bool StageFinalize(Arcane::HostBoot::BootContext& ctx);

    void MainLoop();
    void Shutdown();

    // Destroy the render vehicle and fold a grown RenderErrorCount into
    // m_graphExit. Idempotent, and a no-op only if NEITHER vehicle was ever
    // created (a boot failure before it) -- that is the sole reason. MainLoop
    // calls it on EVERY exit path, because the latch must be read after the
    // last NRI object is gone.
    void ShutdownGraphPath();

    // THE LIVE GRAPH CONTEXT, whichever of the two vehicles this run built.
    // Exactly one of m_graphContext / m_offscreen is ever non-null (MainLoop's
    // one if/else builds one or the other and returns on failure), so this is
    // a selection, never a preference.
    //
    // A POINTER, not the reference the plan sketched: it feeds
    // FrameIo::graph -- a pointer, in an aggregate-initialised struct that a
    // reference member would fight (RuntimeFrame.hpp) -- and it is called from
    // ShutdownGraphPath, which runs on boot-failure paths where BOTH are null
    // and needs a null to test rather than a reference to nothing.
    [[nodiscard]] Arcane::NriGraphContext* Graph() noexcept
    {
        return m_offscreen ? &m_offscreen->Graph() : m_graphContext.get();
    }

    Arcane::HostConfig                  m_config;
    std::unique_ptr<Arcane::GpuContext> m_gpu;          // destructs LAST among engine state

    // THE WHOLE RENDER HALF: the native device, NRI wrap, swapchain (over
    // the HOST's window, borrowed), upload ring, pipeline cache and
    // RenderGraph. LIVE ON EVERY WINDOWED RUN, in every configuration
    // including Dist -- this is null on a failed boot (MainLoop's
    // `if (!m_graphContext)` returns before the loop starts) AND on every
    // --headless run, where m_offscreen below is the vehicle instead.
    //
    // NOT #if-guarded: the type is compiled into the engine DLL in every
    // configuration (it is ordinary Render/Nri source), and a
    // preprocessor-guarded MEMBER would force every use site in MainLoop's
    // frame body to grow a guard of its own -- which is exactly how a
    // Dist-only compile break gets introduced. --headless is a RUNTIME
    // choice for the same reason: a mode gated on a macro would not exist in
    // Release or Dist, and this arc needs it in all three.
    //
    // Declared AFTER m_gpu so it destructs BEFORE it, and that ordering is
    // LOAD-BEARING rather than merely tidy: this object's swapchain is bound
    // to the window inside m_gpu (NriGraphContext.hpp, THE BORROWED WINDOW),
    // so it must be gone before that window is.
    std::unique_ptr<Arcane::NriGraphContext> m_graphContext;

    // THE OTHER RENDER HALF, and the two are MUTUALLY EXCLUSIVE: --headless
    // builds this one and leaves m_graphContext null. It owns its own native
    // device + NRI wrap + an offscreen NriGraphContext that renders into a
    // texture it owns -- no window handle, no surface, no swapchain, nothing
    // for a compositor to map. Reach the graph inside it through Graph()
    // above, never directly.
    //
    // Declared beside m_graphContext, after m_gpu, for the same teardown
    // reason -- even though this vehicle borrows NO window (that is the whole
    // point of it), the pair must sit on the same side of m_gpu so the rule
    // reads as one rule rather than two that happen to agree.
    std::unique_ptr<Arcane::OffscreenVehicle> m_offscreen;

    // The boot splash: non-owning, see the ctor's doc comment. It is
    // BootSequence::Run's presenter for the WHOLE boot, not merely a
    // pre-device stand-in -- see EditorApp.hpp's matching comment.
    Arcane::BootSplashWindow*             m_splash = nullptr;
    // A class member, not a Run()-local (2026-07-30 review round 2, finding
    // 2): StageFinalize needs to call Disarm() on THIS exact instance right
    // before it closes the splash itself -- see EditorApp.hpp's matching
    // comment for the full reasoning (identical here). Constructed from
    // m_splash in the ctor's init list (declared right after it, so
    // declaration order matches).
    Arcane::BootSplashPresenter           m_splashPresenter;

    std::unique_ptr<Arcane::ProcessContext> m_process;   // the process's ONE (spec s3); declared before m_runtime so it outlives it
    // engaged by the boot sequence before MainLoop()/Shutdown() touch them (bare -> deref is safe).
    std::optional<Arcane::ClientRuntime> m_runtime;     // destructs before m_gpu
    std::optional<Arcane::PluginHost>   m_plugin;       // destructs before m_runtime
    Arcane::FramePerf                   m_perf;
    std::uint64_t                       m_frameCount = 0;
    // The graph path's exit code, read by Run()'s tail: 1 = the graph run
    // failed, 2 = RenderErrorCount grew during it, 3 = --settle N never
    // converged (Task 10). It can be set on ANY run -- the graph path is
    // unconditional -- so 0 means no graph failure occurred. The latch baseline it
    // is measured against is taken at the top of MainLoop -- boot-time errors
    // belong to the boot, not to the vehicle.
    int                                  m_graphExit  = 0;
    std::uint64_t                        m_graphErrorBaseline = 0;

    // --pick-probe. THIS is the id<->entity table: the
    // k-th drawable's hit-proxy id is k+1 (CollectPickables' ordering
    // contract), so the vector the graph's pick node rasterised is the same one
    // PickEntityForId inverts when the readback lands. Rebuilt every frame of a
    // probe run and NOT cleared afterwards -- ShutdownGraphPath reads it to name
    // the entity behind the id.
    //
    // NOT #if-guarded, for the same reason m_graphContext is not: a
    // preprocessor-guarded member forces a guard at every use site, which is
    // exactly how a Dist-only compile break gets introduced. Only the code that
    // FILLS them is guarded, so a Dist build carries two empty vectors.
    std::vector<Arcane::PickDrawable>    m_pickDrawables;
    std::vector<std::uint32_t>           m_pickSelectedIds;

    // THE OPAQUE 3D PASS'S PER-FRAME GPU-SCENE FRAME (F2a Task 10; F3 plan 1
    // T8): the staged rows, the batch table, the indirect args and the
    // CPU-written visible-index list. Same ownership shape as m_pickDrawables
    // just above and for the identical reason: FrameDesc::mesh ->
    // MeshSceneDesc::scene is a POINTER BORROWED for the duration of the
    // RenderFrame call (MeshNode.hpp), so the object it names must outlive
    // that call -- and RenderGraph (RuntimeFrame.cpp) is a free function, not
    // a method, so it has nowhere of its own to keep one. Rebuilt every frame
    // by PrepareSceneForRender (Host/GpuSceneHost.hpp; its containers are
    // reused), reached from RenderGraph through FrameIo::gpuSceneFrame.
    Arcane::GpuSceneFrame                m_gpuSceneFrame;

    // THE LAST-FRAME CAPTURE (Task 8: --report wiring). CaptureTail
    // (RuntimeFrame.cpp) fills these on the run's last frame whenever
    // --screenshot or --report was requested -- the SAME readback
    // --screenshot already takes, just retained here rather than written
    // straight to a PNG and discarded, because a report's Brightness/Luma/
    // Rgba probes need the identical pixels. ShutdownGraphPath is where they
    // are consumed, once the loop has ended and backend/frameCount/
    // exitReason are all settled. m_captureRead false means either the run
    // never reached its last frame (an early exit) or neither flag was
    // passed -- either way, a report built from this simply omits SetCapture
    // rather than reporting a phantom empty frame.
    bool                                  m_captureRead   = false;
    std::uint32_t                         m_captureWidth  = 0, m_captureHeight = 0;
    std::vector<unsigned char>            m_captureRgba;

    // --settle N (Task 10). RuntimeFrame.cpp's CaptureTail owns the whole
    // comparison loop; these are just its persistent home, following the
    // exact same "bound through FrameIo, not a RuntimeFrame.cpp static" shape
    // m_captureRead/m_captureWidth/... just above already established for
    // Task 8. m_previousCapture* is the WORKING comparison baseline (churns
    // every attempt); m_captureRead/m_captureRgba/... above stay the FINAL,
    // agreed-on capture, only ever written once convergence actually happens.
    // m_settleConverged is what ShutdownGraphPath reads to decide exitReason
    // "settle-not-converged" and the process exit code -- see that method.
    std::vector<unsigned char>            m_previousCaptureRgba;
    std::uint32_t                         m_previousCaptureWidth = 0, m_previousCaptureHeight = 0;
    bool                                  m_previousCaptureValid = false;
    // uint64_t, matching HostConfig::settleAttempts' own width (fix round 1,
    // item 4) -- a narrower counter compared against a wider budget could
    // silently WRAP on an absurd --settle value, reintroducing the
    // unstoppable-run hazard --headless's own --frames-required refusal
    // exists to prevent.
    std::uint64_t                         m_settleAttemptsUsed   = 0;
    bool                                  m_settleConverged      = false;
    // The TIME half of the bail conjunction -- see FrameIo's own field
    // comments in RuntimeFrame.hpp for why elapsed is measured from the FIRST
    // settle attempt rather than from process start.
    std::chrono::steady_clock::time_point m_settleStartedAt{};
    std::uint64_t                         m_settleElapsedMs      = 0;
    Arcane::SettleBail                    m_settleBail           = Arcane::SettleBail::Keep;
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

    // --compare / --bless (Task 8). Resolved ONCE, before the settle loop
    // starts (Finding 3 of the Task 8 dispatch audit -- see MainLoop's own
    // comment at the resolve site). Default-constructed (level None, both
    // paths empty) on a run that never asked for --compare at all, which
    // ShutdownGraphPath must never mistake for "the reference was looked
    // for and not found".
    Arcane::ReferenceResolution           m_compareResolution;
    // The software adapter's own reference set (Arcane::ReferenceAdapterSet),
    // "" on hardware. Fixed at the first resolve so the post-bless re-resolve
    // in ShutdownGraphPath walks the same levels.
    std::string                           m_compareAdapterSet;
    // The reference pixels the settle loop's compare conjunct reads,
    // loaded ONCE from m_compareResolution.path. Left default/invalid
    // whenever --compare was not given, or --bless disables the conjunct
    // entirely (Finding 4 -- see FrameIo::compareRequested's comment): a
    // bless run never loads or reads this.
    Arcane::PixelData                     m_referencePixels;
    // The MOST RECENT CompareImages() verdict -- see FrameIo::compareResult's
    // own comment for why "most recent" is exactly what ShutdownGraphPath
    // should report.
    Arcane::ImageCompareResult            m_compareResult;
    // Whether m_compareResult was ever actually written by a real
    // CompareImages() call this run -- see FrameIo::compareEvaluated.
    bool                                  m_compareEvaluated     = false;
    // Finding 3: set true ONLY by MainLoop's pre-loop resolution, when
    // --compare named a reference that does not exist on disk (or exists
    // but failed to decode) and --bless was not given -- the fail-fast
    // path. ShutdownGraphPath reads this to report exitReason
    // "compare-missing-reference" rather than falling through the ordinary
    // settle/frames-complete classification, which would misreport a
    // refused/absent name as "the run simply never converged".
    bool                                  m_compareMissingFatal  = false;

#if !defined(ARC_BUILD_DIST)
    // --crash-gpu N (GPU crash diagnostics arc, Task 11): the desk battery's
    // item-2 trigger -- the same deliberate fault the editor's Build ->
    // Diagnostics menu item fires, on the host that has no menu.
    //
    // ONLY THE FIRED-ONCE LATCH IS NEEDED: RenderGraph calls the stateless
    // NriDiagnostics::FireFault, which owns its objects for the length of one
    // dispatch, so nothing is held between frames.
    bool                                      m_gpuFaultFired = false;

    // --hang-main N (crash window plan 2, D10). Same shape as
    // m_gpuFaultFired directly above.
    bool                                      m_hangMainFired = false;
#endif

    // Scene asset resolution (sprite-resolution lift, 2026-07-29): the sprite,
    // sprite-material and post-chain caches, plus the compile drain, in ONE
    // engine-side service the Arcane Editor drives identically. Before this the
    // host published NEITHER resolution table, so a textured sprite was
    // structurally undrawable here whatever the scene contained -- see
    // docs/superpowers/plans/2026-07-29-sprite-resolution-lift.md. It also
    // retires the INTERIM post-chain wiring this block used to hold (the
    // 2026-07-23 fold-into-Arcane directive named this host as the convergence
    // vehicle, and this is that convergence).
    //
    // DECLARED LAST = DESTRUCTS FIRST, which the resolver's header requires: the
    // registry inside m_runtime holds non-owning pointers to its tables, so
    // the resolver must un-publish them before ~Runtime runs. There is no
    // "before the render device" half to this rule: the resolver holds no
    // device-bound handle at all (see SpriteCache.cpp's own note). The
    // compile service + template source roots feed it and are declared
    // before it for the same reason.
    Arcane::ShaderCompiler       m_shaderCompiler;
    Arcane::ShaderSourceProvider m_shaderSources;
    std::optional<Arcane::SceneRenderResolver> m_resolver;
    Arcane::GlobalParams                       m_frameGlobals{};
    double m_hostClock   = 0.0;   // seconds since the loop started (compile debounce clock)
    double m_lastFrameDt = 0.0;   // last frame's wall-clock delta, for the material globals

    // Scene-camera warnings, latched so a per-frame condition is reported ONCE and
    // does not bury the log it would be found in (the same warn-once shape the
    // resolver's PostProcess sweep uses). Never reset: a host run is one scene's
    // worth of complaint, and the log is truncated per launch anyway.
    bool m_warnedNoSceneCamera    = false;
    bool m_warnedMultiSceneCamera = false;
};
