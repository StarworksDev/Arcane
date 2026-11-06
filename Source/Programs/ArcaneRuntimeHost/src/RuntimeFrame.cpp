// RuntimeFrame: RuntimeApp::MainLoop's frame body. See RuntimeFrame.hpp's
// header comment for the contract.

#include "RuntimeFrame.hpp"
#include "RuntimeApp.hpp"   // FrameIo::app.PushSceneCamera -- see that method's comment

#include <Arcane/Assets/Assets.hpp>       // Arcane::WritePngRgba (CaptureTail's screenshot write), reached via Assets.hpp -> ImageIo.hpp
#include <Arcane/Audio/AudioDevice.hpp>   // complete type for AudioSystem().Update (AdvanceSim's voice reap)
#include <Arcane/Base/Assert.hpp>         // ARC_ASSERT (FrameExtent's io.graph invariant)
#include <Arcane/Base/Diagnostics.hpp>    // Diagnostics::Heartbeat (PumpAndResize)
#include <Arcane/Config/ConsoleModel.hpp>
#include <Arcane/Config/ConsoleSettings.hpp>   // console.windowWidth/windowHeight (first-open size)
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/PlayerSettings.hpp>
#include <Arcane/ImGui/ConsoleInputLine.hpp>   // the ONE command line (s8.2)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Host/HostSettings.hpp>       // app.window.minimizedSleepMs
#include <Arcane/Host/RuntimeSettings.hpp>    // runtime.hud.show (BuildHud)
#include <Arcane/Host/GpuSceneHost.hpp>   // GpuSceneInvalidate (R-F) and the HUD's GpuSceneVisibleRows
#if defined(ARC_RUNTIME_HOST_VERIFY)
#include <Arcane/Host/VerifyReport.hpp>   // Arcane::FirstPickProbe (Task 9: pick@x,y -> FrameDesc::pickPixel)
#endif
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/ExtractFrame.hpp>            // ExtractFrame / ExtractRequest -- RenderGraph's one extract (F5 plan 1 Task 5)
#include <Arcane/Render/GraphicsBackend.hpp>                 // Arcane::GraphicsBackend / ToString (BuildHud's HUD text)
#include <Arcane/Render/GpuInstrumentation.hpp>     // Arcane::GpuDeviceLostObserved (PumpAndResize)
#include <Arcane/Render/Nri/NriDiagnostics.hpp>      // dev-only --crash-gpu N (RenderGraph)
#include <Arcane/Render/PickEmit.hpp>                // CollectPickables (RenderGraph's --pick-probe)
#include <Arcane/Scene/BoundsSystem.hpp>              // PresentDerivedState (the Hold branch)
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Render/RenderLookSettings.hpp>       // render.mesh.defaultLight.* (the scene light, Live)
#include <Arcane/Sim/SimSettings.hpp>   // ClampFrameDelta / ApplySimStepCap

#include <imgui.h>
#include <cstdio>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace
{
    bool g_runtimeConsoleOpen = false;

    // A held frame still presents the scene: propagate world transforms and
    // refresh world bounds, the same pair EditModeSchedule runs outside Play.
    // Not a sim step: no RunLoop::Advance, no frameCount, no physics.
    void PresentDerivedState(Astra::Registry& reg)
    {
        Arcane::TransformPropagationSystem{}.operator()(reg);
        Arcane::BoundsSystem{}.operator()(reg);
    }
}

namespace
{
    // =============================================================
    // THE FRAME'S EXTENT.
    // =============================================================
    // Everything that needs a viewport size this frame -- the resolver's
    // material globals, the scene camera's fit, the batcher's Begin -- asks
    // HERE, so no two of them can end up fitted to different rectangles. It is
    // one rectangle by construction: the vehicle has exactly ONE target, and
    // the graph's own canvas is a TRANSIENT sized to that target inside
    // BuildFrame -- so the target is the single source of truth, not a proxy
    // for one. Windowed that target is the swapchain over the host's one
    // window; under --headless it is the vehicle's persistent output texture.
    //
    // READ MODE-AGNOSTICALLY, and that is not a style choice: this used to be
    // `io.graph->Swap().Width()/Height()`, and an offscreen context HAS NO
    // SWAPCHAIN, so that read would have faulted on frame 1 of every
    // --headless run. SurfaceWidth/SurfaceHeight are the accessors
    // NriGraphContext documents as "the mode-agnostic reading of
    // Swap().Width()/Height(), which an offscreen context cannot answer" --
    // the same pair PumpAndResize below already reads for its before/after
    // extent report.
    //
    // ASSERT-THEN-READ, not `if (io.graph) ... else fail`. `io.graph` is
    // unconditional: MainLoop refuses to reach the frame loop at all if the
    // vehicle -- either vehicle -- could not be created (RuntimeApp.cpp,
    // ShutdownGraphPath runs and MainLoop returns first), so every call into
    // RuntimeFrame's functions has a live vehicle -- which is why RenderGraph
    // and CaptureTail in this same file dereference it with no guard. The
    // fail-loud stands hoisted above the read rather than left as an
    // unreachable tail.
    //
    // The assert is not decoration. A 0x0 frame would propagate into the
    // resolver's material globals and the batcher's viewport as a quiet
    // wrong-answer instead of a loud one.
    void FrameExtent(const Arcane::RuntimeFrame::FrameIo& io,
                     std::uint32_t& width, std::uint32_t& height)
    {
        ARC_ASSERT(io.graph, "RuntimeFrame::FrameExtent: io.graph is null -- "
                             "the graph vehicle should be unconditional by the "
                             "time the frame loop runs");
        width  = io.graph->SurfaceWidth();
        height = io.graph->SurfaceHeight();
    }
}

namespace Arcane::RuntimeFrame
{

bool PumpAndResize(FrameIo& io)
{
    // FIRST statement in the frame, before anything can block. Mirrors
    // EditorApp::MainLoop -- the two hosts must not diverge on liveness.
    Arcane::Diagnostics::Heartbeat();

    // Device-lost exit: the latch is set only AFTER the gpu-crash report
    // was written, so breaking here is "report captured, stop cleanly" --
    // the alternative was spinning in a Present-fail loop forever.
    // Mirrors EditorApp::MainLoop.
    if (Arcane::GpuDeviceLostObserved())
    {
        ARC_ERROR("GPU device lost -- crash report written; shutting down");
        return true;
    }

    // EXACTLY ONE PUMP PER FRAME, OF THE ONE WINDOW THIS PROCESS HAS.
    // There is one graphics device, and AT MOST one swapchain: windowed, it
    // binds THIS window -- DXGI allows only one flip-model swapchain per
    // HWND, so a second presentation surface would force a second window
    // and this line would have to choose between them. Under --headless
    // there is no swapchain at all, and the window below is never mapped --
    // but it still EXISTS and is still the object ImGui's platform backend
    // and the input stack were initialised against, so it is still pumped,
    // and it is still the only one there is. SDL's event queue is
    // process-wide and Window::PumpEvents drains all of it, so "exactly one
    // pump" is the rule in both modes, and there is one window to name.
    Arcane::Window& eventWindow = io.gpu->Win();
    const Arcane::WindowEvents events = eventWindow.PumpEvents();
    if (events.quitRequested) return true;
    // A RESIZE IS A PRESENTATION EVENT, AND --headless HAS NO PRESENTATION.
    //
    // NriGraphContext::Resize is documented HOST-WINDOW MODE ONLY and refuses
    // an offscreen context through the LATCHED error seam -- which is a grown
    // RenderErrorCount and therefore a nonzero exit for the whole run, not a
    // warning. So this must be gated rather than left to "a hidden window
    // never resizes": SDL posts SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED (the one
    // event Window::PumpEvents turns into `resized`) when a window's pixel
    // size is first established and again on a display-scale change, and
    // NEITHER requires the window to have been mapped. The offscreen output's
    // extent is fixed at Create and only ResizeOffscreen can change it, so
    // there is nothing here to forward the event to.
    if (events.resized && !io.graph->IsOffscreen())
    {
        // ONE resize, to this run's one presentation surface.
        //
        // Strictly BETWEEN frames -- never between the graph's acquire and
        // its present, which both live inside one Execute() call
        // (RgExecuteDesc::swapChain). A frame driver that resizes at the
        // top of its loop satisfies that structurally, and owes nothing
        // else: the graph rebuilds imported-texture views every Execute.
        //
        // NOTHING ELSE NEEDS RESIZING ALONGSIDE IT. The ImGui platform
        // backend reads the same window the swapchain binds, so this one
        // call leaves every consumer on one extent -- there is no second
        // surface to hold in lockstep and no device-level resize burst.
        //
        // ===== A RESIZE IS NEVER SILENT =====================================
        // This host CAPTURES ITS SWAPCHAIN, so a --screenshot artifact carries
        // the window's client extent -- which the OS owns, not us. Following a
        // resize without saying so once produced a run that started at 1280x720
        // and wrote a 1280x712 image with nothing in the log to explain it.
        //
        // The extent is deliberately NOT pinned: Vulkan's surface currentExtent
        // is authoritative, and refusing to follow it produces a
        // surface/swapchain mismatch -- trading a legible outcome for an
        // illegible one. Report it instead.
        const std::uint32_t wasW = io.graph->SurfaceWidth();
        const std::uint32_t wasH = io.graph->SurfaceHeight();
        io.graph->Resize(events.width, events.height);
        if (wasW != events.width || wasH != events.height)
        {
            if (!io.config.screenshotPath.empty())
            {
                ARC_WARN("[nri-graph] the window was RESIZED mid-run: {}x{} -> {}x{}. This run "
                         "captures its swapchain, so the screenshot will carry the NEW extent.",
                         wasW, wasH, events.width, events.height);
            }
            else
            {
                ARC_INFO("[nri-graph] window resized: {}x{} -> {}x{}",
                         wasW, wasH, events.width, events.height);
            }
        }
    }
    // Left ungated for --headless, deliberately: IsMinimized reads
    // SDL_WINDOW_MINIMIZED, which a never-shown window does not carry (hidden
    // and minimized are different SDL window states), so this is false for the
    // whole of a --headless run and the skip below is unreachable there. It
    // would be harmless even if it fired -- a skipped frame is routine -- so
    // there is nothing to gate.
    if (eventWindow.IsMinimized())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(Arcane::Settings<Arcane::AppWindowSettings>().minimizedSleepMs));
        io.skipFrame = true;
        return false;
    }
    io.skipFrame = false;
    return false;
}

void AdvanceSim(FrameIo& io, SimPace pace)
{
    Arcane::CVarRegistry::Get().Publish();
    io.perf.FrameStart();

    // --settle N (Task 10): once the ordinary --frames budget is spent,
    // FREEZE the render/sim clock instead of advancing it -- extra frames
    // from here on exist only to give the async shader compiler more REAL
    // wall-clock time before the next capture attempt (RuntimeFrame.cpp's
    // CaptureTail), not to advance content. This is load-bearing, not an
    // optimisation: ReferenceProject's PulseSprite material shades as a
    // function of Time (Content/materials/pulse_sprite.arcmat's `sin(Time *
    // PulseSpeed)`), so an UN-frozen clock would make every settle-hold
    // frame legitimately differ from the last regardless of whether the
    // compile race has resolved, and CaptureTail's byte-equal comparison
    // would never converge on anything. Computed once, read by the Hold pass
    // and by both the frameDt and simDt branches below.
    //
    // Task 30 fix round 1: a remote `wait settled` in its capture phase holds
    // the same freeze (io.remoteSettleHold), on a held, running, headless or
    // windowed runtime alike: spec s4.7's frozen-clock captures.
    const bool settleHold = io.remoteSettleHold ||
                            (io.config.headless && io.config.settleAttempts != 0 &&
                             io.config.maxFrames != 0 && io.frameCount >= io.config.maxFrames);

    // AM1 Hold (the remote tick gate is Idle): see SimPace's comment in the
    // header. The host clock advances by wall time even under --headless (a
    // held run has no fixed cadence to stand in for it), and the frame dt the
    // material globals read is that same wall delta, so Time and DeltaTime
    // stay consistent -- except under the settle hold, which freezes it.
    // No input, no RunLoop::Advance, no audio tick.
    if (pace == SimPace::Hold)
    {
        const auto now = std::chrono::steady_clock::now();
        const double wallDt = settleHold ? 0.0 : std::chrono::duration<double>(now - io.lastFrameTime).count();
        io.lastFrameTime = now;
        io.simPrev       = now;
        io.lastFrameDt   = wallDt;
        io.hostClock    += wallDt;
        PresentDerivedState(io.runtime->Registry());
        return;
    }

    // Input: sample SDL state, evaluate actions. Must precede ImGui BeginFrame
    // so capture flags are set before the evaluator reads them.
    {
        // FIXED under --headless, wall-clock otherwise -- and steady_clock::
        // now() is not even CALLED in the fixed branch, deliberately: this
        // frameDt does not stay local to input. It becomes io.lastFrameDt/
        // io.hostClock below, which PrepareFrame hands the scene resolver as
        // FrameInfo::dt/now -- the Time/DeltaTime the material globals report
        // to every bound shader (SceneRenderResolver.cpp, Batcher2D::
        // SetGlobals). A wall-clock frameDt here would leak straight into
        // anything an .arcmat animates by time, so two --headless runs of
        // the same `--frames N` could still render different pixels even
        // with the RunLoop sim held fixed below -- this is every bit as
        // sim-advancing as that one is, just for the render side instead of
        // gameplay.
        double frameDt;
        if (settleHold)
        {
            frameDt = 0.0;   // see settleHold's own comment above
            // A remote settle hold ends (Task 30): a windowed clock resumes
            // from here, not with the whole hold as one frame's dt.
            if (!io.config.headless)
                io.lastFrameTime = std::chrono::steady_clock::now();
        }
        else if (io.config.headless)
        {
            frameDt = io.config.fixedDtSeconds;
        }
        else
        {
            const auto now = std::chrono::steady_clock::now();
            frameDt = std::chrono::duration<double>(now - io.lastFrameTime).count();
            io.lastFrameTime = now;
        }
        // The host clock the compile service debounces against + the frame dt
        // the material globals report. Advanced exactly once per frame, here,
        // because this is where the frame's wall-clock delta is measured (the
        // editor's m_editorClock does the same job in its phase 9).
        io.lastFrameDt = frameDt;
        io.hostClock += frameDt;
        const Arcane::InputSnapshot snap =
            io.gpu->InDevices().Sample(io.gpu->Imgui().WantCaptureKeyboard(),
                                      io.gpu->Imgui().WantCaptureMouse());
        io.runtime->SetInputSnapshot(snap);   // plugins read it via ClientRuntime::Input()
        io.runtime->UpdateGameInput(frameDt, snap);
        io.gpu->Input().Update(frameDt, snap);
        if (!io.config.headless && io.gpu->Input().Pressed("console_toggle"))
            g_runtimeConsoleOpen = !g_runtimeConsoleOpen;
        if (!g_runtimeConsoleOpen)
        {
            if (io.gpu->Input().Pressed("quit"))                { io.quit = true; return; }
            if (io.gpu->Input().Pressed("reload_plugin"))       io.plugin->ForceReload();
            if (io.gpu->Input().Pressed("reload_plugin_fresh")) io.plugin->ReloadFresh();
        }
    }

    // Sim advance: clamp dt, drive RunLoop with plugin callbacks interleaved.
    {
        // THE SITE the plan calls out by name ("the sim-advance site that
        // consumes simPrev"): FIXED under --headless, for the same reason
        // HostConfig.hpp's fixedDtSeconds comment gives. simDt reaches
        // RunLoop::Advance below, i.e. FixedUpdateAll/UpdateAll -- gameplay
        // -- so a wall-clock simDt means `--frames 5` advances the sim by
        // however long five frames happened to take on THIS run's machine,
        // and no two runs agree. The sim.maxFrameDeltaSeconds spiral-of-death clamp stays
        // wall-clock-only: fixedDtSeconds is refused at parse time unless
        // positive (HostConfig.cpp) and is a deliberate per-run choice, not a
        // stall to guard against.
        double simDt;
        if (pace == SimPace::SingleTick)
        {
            // AM1 Step: exactly one canonical fixed step. RequestSingleStep runs
            // one tick without touching the accumulator (RunLoop::StepFixed), so
            // neither a wall-clock dt nor a leftover backlog nor the time scale
            // can turn it into zero or two; Update sees one fixed dt.
            io.runtime->Loop().RequestSingleStep();
            simDt = 1.0 / io.runtime->Loop().FixedHz();
            if (!io.config.headless)
                io.simPrev = std::chrono::steady_clock::now();   // the next Run step starts from here
        }
        else if (settleHold)
        {
            simDt = 0.0;   // see settleHold's own comment above AdvanceSim's input block
            if (!io.config.headless)
                io.simPrev = std::chrono::steady_clock::now();   // as frameDt's hold branch
        }
        else if (io.config.headless)
        {
            simDt = io.config.fixedDtSeconds;
        }
        else
        {
            const auto now = std::chrono::steady_clock::now();
            simDt = std::chrono::duration<double>(now - io.simPrev).count();
            io.simPrev = now;
            simDt = Arcane::ClampFrameDelta(simDt);   // sim.maxFrameDeltaSeconds: the one clamp editor Play shares
        }
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.runtime->EnsurePhysics();   // engine-owned physics (spec s4.3): mint/refresh the world before the step
        Arcane::ApplySimStepCap(io.runtime->Loop());   // sim.maxStepsPerFrame (Live)
        io.runtime->Loop().Advance(simDt,
            [&](double dt)          { io.runtime->BeginGameInputFixedStep(); io.plugin->FixedUpdateAll(dt); },
            [&](double dt, double a){ io.plugin->UpdateAll(dt, a); });
        // Reclaim finished fire-and-forget SFX voices each frame (and, on the
        // device-less null backend, advance audio time so one-shots actually end).
        io.runtime->AudioSystem().Update(simDt);
        io.perf.Add(io.perf.accSim, t0, io.perf.Now());
    }
}

void BuildHud(FrameIo& io)
{
    io.gpu->Imgui().BeginFrame();
    // runtime.hud.show (S6-25, inventory R4): on in Debug/Release, where the
    // goldens are captured; off in Dist unless the player turns it on.
    if (Arcane::Settings<Arcane::RuntimeHudSettings>().show)
    {
        ImGui::Begin("ArcaneRuntime");
        // There is no RenderDevice to ask -- GpuContext builds none -- so
        // the HUD reads the backend out of the config it was selected with.
        ImGui::Text("Backend: %s", Arcane::ToString(io.config.backend));
        ImGui::Text("Plugin gen: %u", io.plugin->Generation());
        // The last extracted packet's sprite batch (F5 plan 1 Task 5): draws
        // are its spans, quads its vertices / 4. io.frameCount > 0 means a
        // frame has been extracted and presented; before that there is no
        // packet and the counts are 0.
        std::uint32_t quads = 0, draws = 0;
        if (io.frameCount > 0)
        {
            const Arcane::RenderFramePacket& last = io.packetRing.Slot(0);
            quads = static_cast<std::uint32_t>(last.batchVertices.size() / 4u);
            draws = static_cast<std::uint32_t>(last.batchSpans.size());
        }
        ImGui::Text("Quads: %u  Draws: %u", quads, draws);

        // THE 3D PASS'S VISIBILITY, beside the 2D batcher's counts (F3 plan 2
        // T5): the CPU's own figures, then what the GPU CULL PASS itself
        // emitted. Both describe the LAST frame, not this one -- the HUD is
        // built before this frame's extract copies the GPU-scene stats, and
        // the GPU's answer is asynchronous by construction -- which is the same
        // one-frame lag the batcher's counts above already carry.
        //
        // This host arms the ring at boot, UNCONDITIONALLY (RuntimeApp's own
        // comment carries the reasoning), so on an ordinary run -- windowed,
        // no --report -- the second line carries a real number within a couple
        // of frames of the first draw. That is what spec s9.5's desk pass
        // ("orbit a scene with meshes leaving and entering the view and watch
        // the stats line") reads. It says "unavailable" only while nothing has
        // completed yet: the opening frames, or a scene with no mesh pass at
        // all. A HUD that printed the coarse count there instead would be
        // showing the CPU's expectation under the GPU's name -- exactly the
        // conflation this arc removed from the report.
        const Arcane::GpuSceneFrame::Stats& vis = io.gpuSceneStats.stats;
        ImGui::Text("Rows: %u  Coarse: %u  Batches: %u  Draws: %u (+%u transparent)",
                    vis.total, vis.coarseVisible, vis.batches, vis.draws,
                    io.gpuSceneStats.transparentDraws);
        if (const std::optional<std::uint32_t> gpuVisible =
                Arcane::GpuSceneVisibleRows(io.graph ? io.graph->Scene() : nullptr))
            ImGui::Text("GPU visible: %u", *gpuVisible);
        else
            ImGui::TextUnformatted("GPU visible: unavailable");
        ImGui::End();
    }

    // DrawUI reads ClientRuntime::View(). Resolve this frame's scene camera
    // the way RenderGraph's ExtractFrame will -- no editor view, this frame's
    // extent -- and push it first. warn is false, so the no-camera,
    // multi-camera, and degenerate-basis diagnostics still fire once, inside
    // ExtractFrame. No scene camera leaves a plugin-driven view alone.
    {
        std::uint32_t viewW = 0, viewH = 0;
        FrameExtent(io, viewW, viewH);
        Arcane::ExtractRequest viewReq;
        viewReq.viewport = { viewW, viewH };
        const Arcane::ResolvedFrameViews views =
            Arcane::ResolveFrameViews(io.runtime->Registry(), viewReq, false);
        if (views.spriteViewFromScene && views.spriteView)
            io.runtime->SetView(views.spriteView->view);
    }

    // ABI v2: the game module + any secondary plugins draw their own ImGui between
    // BeginFrame and Render. Each entry point is null-checked inside DrawUIAll.
    io.plugin->DrawUIAll();

    // The engine console sits above anything the game module drew. Headless
    // has no window to put it on; --set still reaches the same registry.
    if (!io.config.headless)
    {
        static Arcane::ConsoleModel console;
        if (g_runtimeConsoleOpen)
        {
            const Arcane::ConsoleSettings& consoleSettings = Arcane::Settings<Arcane::ConsoleSettings>();
            ImGui::SetNextWindowSize(ImVec2(float(consoleSettings.windowWidth), float(consoleSettings.windowHeight)),
                                     ImGuiCond_FirstUseEver);
            bool open = g_runtimeConsoleOpen;
            if (ImGui::Begin("Console##runtime", &open))
            {
                for (const Arcane::ConsoleLine& line : console.Lines())
                    ImGui::TextUnformatted(line.text.c_str());
                // The session role decides who is typing (settings spec s8.3): LocalHost
                // in single-player and on a listen-server host -- Server settings are
                // settable, Cheat ones once server.cheats is on -- and Client when this
                // world is connected elsewhere. Read every frame, so a re-role is honoured;
                // the game's CVarPolicy applies on top inside the registry.
                const Arcane::CVarContext context = Arcane::CVarContextFor(io.runtime->Core().Mode());
                (void)Arcane::DrawConsoleInputLine("##cvar", console, Arcane::CVarRegistry::Get(), context);
            }
            ImGui::End();
            g_runtimeConsoleOpen = open;
        }
    }
}

void PrepareFrame(FrameIo& io)
{
    // Scene asset resolution, BEFORE the batcher's Begin and before
    // ExtractFrame: the drain inside registers compiled materials with the
    // batcher (a pipeline/binding-set table mutation that does not belong
    // inside a recording), and the SpriteTable/SpriteMaterialTable it
    // publishes are what THIS frame's submission reads. Without this call the
    // host published no tables at all, so every sprite fell back to the 1x1 m
    // untextured quad and every material to the plain pipeline -- which is
    // exactly what "the game window draws nothing but the editor viewport
    // looks right" was.
    if (io.resolver)
    {
        std::uint32_t width = 0, height = 0;
        FrameExtent(io, width, height);

        Arcane::SceneRenderResolver::FrameInfo frame;
        // --fixed-time overrides ONLY what the scene sees (sceneTime). now is
        // io.hostClock, untouched, because it is also the clock the shader
        // compile service debounces against (see this file's dispatch gate) --
        // a pinned compile clock would stall every compile, which is not what
        // pinning scene time means.
        frame.now            = io.hostClock;
        frame.sceneTime      = io.config.fixedTimeSeconds;
        frame.dt             = io.lastFrameDt;
        frame.viewportWidth  = (float)width;
        frame.viewportHeight = (float)height;
        io.resolver->Refresh(frame);
        io.frameGlobals = io.resolver->Globals();
    }

    io.skipFrame = false;
}

// =============================================================
// THE RENDER HALF -- the only recorder in the process.
// One RenderGraph frame: Reset, declare, Compile, Execute -- and
// Execute is what acquires the backbuffer, records every node, submits
// and presents. The frame is batch2d -> [post chain] -> tonemap ->
// [pick/outline] -> [imgui] -> [capture] -> present, declared inside
// DeclareGraphFrame; each bracketed stage is dropped by passing the
// FrameDesc field that carries it as null (see that struct).
//
// UNDER --headless the frame's SHAPE is identical -- same declaration,
// same node census -- and only its two ends differ: the tonemap writes
// the vehicle's imported output instead of an acquired backbuffer, and
// Execute runs with no swapchain, so it acquires nothing and presents
// nothing. That is the vehicle's business, not this function's; the one
// thing this function owes it is calling the right member of the
// RenderFrame/RenderFrameOffscreen pair. See that call below.
//
// The RENDER submission: the batcher is Begin()'d with the canvas
// extent alone, ExtractFrame runs the sprite sweep into it and copies
// the drained batch into this frame's packet, and the graph's
// Batch2DNode draws the bound packet -- one batcher, one batching
// algorithm, one recorder. End() is deliberately NOT called: that batcher is
// DEVICE-LESS by construction (GpuContext::Create builds no render
// device), so End() would refuse anyway and records nothing.
//
// The scene POST CHAIN runs here too -- one node per pass, between the
// canvas and the tonemap. So does the HUD, from the same ImGui frame
// BuildHud left current -- see the block below.
//
// NriTextureCache makes a span's and a material's declared images
// resident on this device, so sprites and materials sample the textures
// they name rather than a fallback.
// The SIM half (FixedUpdate/Update) runs above, in AdvanceSim.
// =============================================================
Arcane::NriGraphContext::FrameOutcome RenderGraph(FrameIo& io)
{
    Arcane::NriGraphContext::FrameDesc graphFrame;

#if !defined(ARC_BUILD_DIST)
    // --crash-gpu N. Fires exactly once, and the latch is set FIRST -- a
    // failed build must not retry the injection every frame.
    //
    // TWO properties, both forced by the recording topology and neither
    // observable in the report:
    //   * the fault goes out on its OWN one-off command buffer rather than
    //     nested inside this frame's recording -- the graph's command buffers
    //     belong to RenderGraph::Execute, which owns their whole open/record/
    //     close/submit cycle, and a deliberate TDR must not be threaded
    //     through the very machinery the crash report has to survive to
    //     describe;
    //   * it therefore fires BEFORE the frame is declared rather than inside
    //     it, which keeps `pass:gpu-fault` the last scope the GPU begins.
    // FireFault is stateless -- it owns its objects for the length of one
    // dispatch -- so FrameIo carries nothing for it but the fired latch.
    if (io.config.crashGpuFrame != 0 && !io.gpuFaultFired &&
        io.frameCount >= io.config.crashGpuFrame)
    {
        io.gpuFaultFired = true;   // set FIRST: a failed build must not retry every frame
        nri::Queue* const queue = io.graph->Device().GraphicsQueue();
        if (!queue || !Arcane::NriDiagnostics::FireFault(io.graph->Device(), *queue))
            ARC_ERROR("--crash-gpu: fault injector unavailable -- nothing dispatched");
    }

    // --hang-main N (crash window plan 2, D10): stop beating on purpose. The
    // sleep sits here, after the beat PumpAndResize published for this frame,
    // so the watchdog sees a beat that then goes stale for kHangMainSeconds.
    if (io.config.hangMainFrame != 0 && !io.hangMainFired && io.frameCount >= io.config.hangMainFrame)
    {
        io.hangMainFired = true;   // set FIRST: exactly once
        ARC_WARN("--hang-main: blocking the main thread for {} s without a heartbeat", Arcane::kHangMainSeconds);
        std::this_thread::sleep_for(std::chrono::seconds(Arcane::kHangMainSeconds));
    }
#endif

    // ============================================================
    // THE HUD. The "ArcaneRuntime" window and the plugin's DrawUIAll
    // are recorded ABOVE, into the one ImGui frame this host builds;
    // this line only hands the finished draw data to the graph.
    //
    // It goes THROUGH THE LAYER rather than calling ImGui::Render()/
    // EndFrame() bare: ImGuiLayer pins its own context in every entry
    // point, and a bare call here would end whatever context an editor
    // document or a plugin last left current. The frame IS ended either
    // way, so ImGuiLayer's "every BeginFrame is paired exactly once"
    // contract holds here regardless.
    //
    // The draw data lives in the ImGui context until the next
    // NewFrame, i.e. for the whole of the RenderFrame call below --
    // which is what lets the node copy the vertices at RECORD time.
    // ============================================================
    graphFrame.imgui = io.gpu->Imgui().RenderToDrawData();

    // THE FRAME'S EXTENT, from the one surface this run has (see
    // FrameExtent): the graph swapchain's, or -- under --headless -- the
    // vehicle's persistent output texture. Read once and used for the
    // batcher's viewport AND the scene camera's fit, so those two can
    // never be fitted to different rectangles -- and it is the same
    // number PrepareFrame gave the resolver for the material globals.
    std::uint32_t frameWidth = 0, frameHeight = 0;
    FrameExtent(io, frameWidth, frameHeight);

    // The 2D submission. Begin takes only the canvas extent (ABI v15). This
    // batch reaches the GPU through the graph's Batch2DNode, which drains it.
    io.gpu->Batch().Begin(frameWidth, frameHeight);
    io.gpu->Batch().SetGlobals(io.frameGlobals);
    // The batcher the sprite sweep submits into, published for ExtractFrame
    // below. The sweep itself runs there, once (F5 plan 1 Task 5). Extract
    // sets this batcher's view-projection from the published sprite view
    // before that submit.
    io.runtime->SetRenderContext(&io.gpu->Batch());

    graphFrame.batch = &io.gpu->Batch();
    // The scene post chain as BYTES (SceneRenderResolver::PostDesc).
    // Re-read every frame for the same reason PostChain() is above:
    // a drain may swap the bound instance under an asset re-save.
    // Null (no PostProcess assignment, or the compile has not landed
    // yet) simply renders canvas -> tonemap -- which is also how a caller
    // asks for the frame WITHOUT a post chain at all (see FrameDesc).
    graphFrame.post    = io.resolver ? io.resolver->PostDesc() : nullptr;
    // The same GlobalParams the batcher got via SetGlobals, so the post
    // chain and the batch cannot bind different b1 contents.
    graphFrame.globals = &io.frameGlobals;
    // The capture is taken on the LAST frame, which has to be known
    // BEFORE the frame is declared (the readback is a graph NODE, not
    // an after-the-fact copy). Hence `+ 1`: io.frameCount is bumped in
    // CaptureTail, i.e. after this arm returns Presented.
    //
    // --settle N (Task 10): this predicate's NAME says "last frame", and
    // under an ordinary (non-settle) run it is exactly that -- CaptureTail
    // stops the loop the moment it fires. Under --settle it stays true for
    // EVERY frame from here on (io.frameCount only grows), which is exactly
    // what is wanted: each such frame is one more settle attempt, and
    // CaptureTail -- not this predicate -- is what decides when the loop
    // actually ends (converged, or the bail conjunction fired: BOTH the
    // attempt budget and --settle-timeout spent). No settle-
    // specific arming logic is needed here as a result: MainLoop's while
    // condition already stops calling this function once CaptureTail says
    // so, so "arm every frame past the base budget" and "arm the one last
    // frame" are the SAME expression.
    const bool willBeLastFrame =
        io.config.maxFrames != 0 && (io.frameCount + 1) >= io.config.maxFrames;
    // Armed for --report too (Task 8), not --screenshot alone: a report's
    // Brightness/Luma/Rgba probes read this SAME readback, and CaptureTail
    // (this file) widens its own read gate to match -- without this, a run
    // that passed --report/--probe but no --screenshot would never declare
    // the readback node, and every positional probe would report an honest
    // but avoidable "no capture set".
    graphFrame.capture = willBeLastFrame &&
                          (!io.config.screenshotPath.empty() || !io.config.reportPath.empty());

    // THE ONE EXTRACT (F5 plan 1 Task 5, spec s4.1): the sprite sweep runs
    // into the host batcher Begin()'d above and the drained batch is copied
    // into this frame's packet, which the graph's batch node draws instead of
    // draining the batcher again. graphFrame.batch stays that same batcher,
    // so the registered sprite materials resolve where they were registered.
    Arcane::ExtractRequest req;
    req.viewport = { frameWidth, frameHeight };
    req.device = io.graph->Scene();
    // The scene light, read once per frame (Live) and published into the
    // packet. The mesh block below copies the three vectors back out.
    Arcane::MeshSceneDesc light{};
    Arcane::ApplyDefaultLight(light, Arcane::Settings<Arcane::RenderMeshDefaultLightSettings>());
    req.lightDirection = light.lightDirection;
    req.lightColor = light.lightColor;
    req.ambient = light.ambient;
    req.imgui = graphFrame.imgui;
    req.gameUi = graphFrame.gameUi;
    req.post = graphFrame.post;
    const auto extractT0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(io.runtime->Registry(), req, io.packetRing);
    io.perf.Add(io.perf.accRec, extractT0, io.perf.Now());   // --perf's "extract" column
    // After extract, before the mesh block and both pick blocks, so
    // graphFrame.pickView = io.runtime->View() reads the view just installed.
    io.app.PushSceneCamera(packet);

    // The HUD and ShutdownGraphPath read these scalars. The ring holds the
    // packet, which outlives this call.
    io.gpuSceneStats.stats = packet.mesh.stats;
    io.gpuSceneStats.transparentDraws =
        static_cast<std::uint32_t>(packet.mesh.transparentDraws.size());

    // THE OPAQUE 3D PASS (F2a Task 10; F3 plan 1 T8). The frame is the one
    // ExtractFrame just published. `meshScene` only needs to outlive the
    // RenderFrame* call below, inside this function. THE GATE IS Empty(),
    // NOT HasDraws() (ruling R-D): a frame whose meshes are all culled, or
    // with no perspective camera at all, can still carry staged rows or a
    // full rebuild, and the mirror's lastModel history has already advanced
    // past them -- the pass must declare itself so GpuSceneSyncNode uploads
    // them; MeshNode then records only its depth clear. Without a mesh view
    // the matrices stay at their defaults (no batch reads them).
    // graphFrame.mesh is left null for an Empty() scene.
    Arcane::MeshSceneDesc meshScene;
    meshScene.scene = &packet.mesh;
    if (packet.meshView)
    {
        meshScene.view       = packet.meshView->view;
        meshScene.projection = packet.meshView->projection;
    }
    // NO LIGHT COMPONENT YET: this engine has none anywhere --
    // Scene/Components.hpp declares none and SceneModule.hpp registers
    // none -- so the scene's one light is render.mesh.defaultLight.*
    // (RenderLookSettings.hpp; Live, read once per frame into the packet
    // above). A light component is future work and a reviewed scene-schema
    // change.
    meshScene.lightDirection = packet.lightDirection;
    meshScene.lightColor = packet.lightColor;
    meshScene.ambient = packet.ambient;
    if (!meshScene.Empty())
        graphFrame.mesh = &meshScene;

    // After ExtractFrame, so CollectPickables reads this frame's MainVisibleSet.
#if !defined(ARC_BUILD_DIST)
    // --pick-probe. CollectPickables ran inside ExtractFrame; the k-th
    // drawable it appended IS hit-proxy id k+1, which is what the graph's
    // pick node rasterises and what RuntimeApp::ShutdownGraphPath inverts
    // through PickEntityForId. This block copies that list onto the frame.
    // It lives here, not in the vehicle, because this is the object that
    // owns a registry and a camera -- NriGraphContext is a render vehicle
    // and must not grow either.
    //
    // The view handed over (pickView) is the SCENE camera PushSceneCamera
    // just installed, so the id pass projects the same world silhouettes the
    // batch node drew. They cannot disagree even in principle: both are fitted
    // to frameWidth/frameHeight above, which IS the vehicle's own surface
    // extent (FrameExtent -- the swapchain's, or the offscreen output's).
    // In practice this block is windowed-only regardless: --pick-probe is
    // refused at parse time alongside --headless (HostConfig.cpp), because
    // an offscreen context declines to arm the fixed probe pixel.
    if (io.config.pickProbe)
    {
        io.pickDrawables = packet.pickDrawables;

        // THE SCRIPTED SELECTION -- a stand-in for the editor selection
        // this host does not have: the FIRST pickable entity in the
        // scene, whose hit-proxy id is 1 by CollectPickables' own
        // ordering contract.
        io.pickSelectedIds.clear();
        if (!io.pickDrawables.empty())
            io.pickSelectedIds.push_back(1u);

        graphFrame.pickOutline = true;
        graphFrame.pickables   = packet.pickDrawables;
        graphFrame.pickView    = io.runtime->View();
        if (!packet.pickDrawables.empty())
            io.packetRing.Latest().selectedIds = { 1u };
    }
#endif

    // ---- pick@x,y probes (Task 9), driven through FrameDesc::pickPixel --
    // NOT the --pick-probe flag above, and deliberately NOT ARC_BUILD_DIST-gated
    // like it: this is the report's pick channel (VerifyReport's `pick@x,y`
    // probe kind), not a dev-only exit-code check, so it must exist in every
    // build configuration the same way every other probe kind does.
    //
    // Gated on reportPath too, matching the offscreen vehicle's own gate
    // (RuntimeApp.cpp's offscreenNodes.pickOutline): a bare `--probe pick@..`
    // with no `--report` has nowhere for the answer to go, so arming the
    // readback here would cost a copy nobody reads -- and worse, the pick
    // node itself was never BUILT for that run (NriGraphContext.cpp only
    // builds it when NodeSet::pickOutline asked at Create time), so setting
    // graphFrame.pickOutline here without that node existing would be
    // arming a chain that is not there. The two gates must agree exactly.
    //
    // Armed at the SAME pixel every frame (FirstPickProbe's answer does not
    // change frame to frame -- a run has exactly one `pick@x,y` request that
    // matters), which is what lets the readback -- landing
    // FramesInFlight() frames after the pass that wrote it -- settle
    // well before ShutdownGraphPath reads NriGraphContext::ProbeId() after
    // the loop ends. A run combining this with the (windowed-only) dev
    // --pick-probe flag above has this block win FrameDesc::pickPixel for
    // the frame -- the flag's own fixed pixel is superseded rather than
    // silently ignored, which is an acceptable trade for an untested
    // combination neither mechanism was designed to share.
    // Verify-only (RuntimeApp.hpp's ARC_RUNTIME_HOST_VERIFY note): a package has no --report.
#if defined(ARC_RUNTIME_HOST_VERIFY)
    if (!io.config.reportPath.empty())
    {
        const std::optional<Arcane::ProbeSpec> pickSpec = Arcane::FirstPickProbe(io.config.probes);
        if (pickSpec)
        {
            io.pickDrawables = packet.pickDrawables;

            graphFrame.pickOutline = true;
            graphFrame.pickables   = packet.pickDrawables;
            graphFrame.pickView    = io.runtime->View();   // the scene camera, as above
            graphFrame.pickPixel   = glm::ivec2(pickSpec->x, pickSpec->y);
            // selectedIds left empty (default): this probe reports FACTS, not
            // a visible outline -- a --screenshot cross-check must show the
            // scene exactly as an ordinary run would, not a pick-highlighted
            // one.
            //
            // hoverPixel MUST be set explicitly (fix round 1, item 1): left
            // unset, it falls back to ProbeX()/ProbeY() -- the OLD flag's
            // fixed pixel, which defaults to (0, 0) when the flag was never
            // armed (NriGraphContext.cpp's m_currentHover init). outline_seed.hlsl
            // seeds a HOVER silhouette there in ADDITION to the (empty)
            // selection one, and the outline composites a cyan ring over the
            // tonemapped backbuffer -- BEFORE the capture node -- so any scene
            // with a pickable at (0, 0), or any run also combining the dev
            // --pick-probe flag, would silently corrupt every other probe and
            // the screenshot. It reads clean today only because (0, 0) is
            // empty background in ReferenceProject -- that is an accident of
            // this scene, not a property of the mechanism, so the no-hover
            // sentinel is spelled out here rather than left to the default.
            graphFrame.hoverPixel = glm::ivec2(-1, -1);
        }
    }
#endif


    // Timed into accPresent rather than left unmeasured, so `--perf`
    // reports something real on this path too (the spec's waiting-
    // frame budget is mostly the pacing wait inside
    // AcquireNextTexture, which is inside this call). It is the whole
    // record + submit + present cost, not just the present: Execute()
    // does all three, and splitting one call across three
    // accumulators would be a fiction. accRec IS real -- it times the
    // ExtractFrame call above, the sprite submission included (F5 plan 1
    // Task 5) -- but accEnd/accTone stay 0: there is no separate flush or
    // tonemap step left to time, both being inside this one call.
    //
    // THE ONE LINE IN THIS FILE THAT NAMES THE MODE, and it is forced. The
    // vehicle deliberately exposes a PAIR that refuses each other's mode --
    // RenderFrame latches an error on an offscreen context and
    // RenderFrameOffscreen latches one on a host-window context, because
    // "a vehicle that quietly did the wrong thing for its mode would be a
    // wrong picture, not an error" (NriGraphContext.hpp). There is no
    // mode-agnostic entry point to route through, and inventing one in the
    // engine would erase exactly the refusal that design is buying. So the
    // frame driver picks, once, here -- everything above and below this line
    // is identical in both modes, which is the property Task 14's parity check
    // rests on.
    const auto graphT0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
    // ExtractFrame -> write selection (both pick blocks above) -> BindPacket
    // -> render. The vehicle nulls the pointer on return.
    io.graph->BindPacket(&packet);
    const Arcane::NriGraphContext::FrameOutcome outcome =
        io.graph->IsOffscreen() ? io.graph->RenderFrameOffscreen(graphFrame)
                                : io.graph->RenderFrame(graphFrame);
    io.perf.Add(io.perf.accPresent, graphT0, io.perf.Now());
    // Any outcome but Presented means GpuSceneSyncNode did not run, so the
    // rows the extract staged above never reached the GPU and would
    // otherwise never be re-staged (ruling R-F): give the mirror a new
    // generation so the next sync is a full rebuild.
    if (outcome != Arcane::NriGraphContext::FrameOutcome::Presented)
        Arcane::GpuSceneInvalidate(io.runtime->Registry());
    if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)
    {
        // Already reported through the "nri-graph" seam (so the latch
        // grew and the exit code would be nonzero anyway) -- stop
        // rather than spin on a broken device.
        ARC_ERROR("the frame could not be recorded or submitted; stopping");
        io.graphExit = 1;
        return outcome;
    }
    if (outcome == Arcane::NriGraphContext::FrameOutcome::Skipped)
    {
        // Routine: a zero-sized surface or an OUT_OF_DATE acquire,
        // both self-healing on the next resize event. m_frameCount
        // deliberately does NOT advance -- the vehicle's own frame
        // counter did not either, and the two must stay in lockstep
        // for the command-slot recycling to be safe.
        // app.window.minimizedSleepMs: the editor's skipped-frame twin reads it too.
        std::this_thread::sleep_for(std::chrono::milliseconds(Arcane::Settings<Arcane::AppWindowSettings>().minimizedSleepMs));
        return outcome;
    }
    return outcome;
}

bool CaptureTail(FrameIo& io)
{
    // Debounced watcher: rebuild PlaygroundGame -> auto hot reload.
    {
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.plugin->Poll();
        io.perf.Add(io.perf.accPoll, t0, io.perf.Now());
    }

    if (io.perf.On())
    {
        // This frame's packet (RenderGraph extracted it, and CaptureTail
        // runs only after a Presented frame): quads = vertices / 4, draws =
        // spans.
        const Arcane::RenderFramePacket& last = io.packetRing.Slot(0);
        io.perf.Tick(static_cast<std::uint32_t>(last.batchVertices.size() / 4u),
                     static_cast<std::uint32_t>(last.batchSpans.size()));
    }

    ++io.frameCount;
    const bool wantsCapture = !io.config.screenshotPath.empty() || !io.config.reportPath.empty();
    // TRUE from the moment the ordinary --frames budget is spent, and stays
    // true forever after (io.frameCount only grows) -- the non-settle tail
    // below reads this as "the last frame", which it only ever IS in that
    // mode, because CaptureTail itself is what stops the loop the moment it
    // fires there.
    const bool pastBase = io.config.maxFrames != 0 && io.frameCount >= io.config.maxFrames;

    // =========================================================================
    // --settle N (Task 10). Once the base --frames budget is reached, every
    // further call to this function is ONE SETTLE ATTEMPT: AdvanceSim has
    // frozen the render/sim clock for these frames (see its own comment), so
    // the ONLY thing that can still change frame to frame is asynchronous
    // resource state -- a shader compile landing, a texture upload completing
    // -- which is exactly the race this mode exists to pin down. Each
    // attempt's capture is compared to the IMMEDIATELY PRECEDING one; two
    // byte-equal in a row is convergence. How long a NON-converging run spins
    // is bounded by a CONJUNCTION, not by the attempt count alone: it gives up
    // only once io.config.settleAttempts attempts AND io.config.settleTimeoutMs
    // milliseconds are BOTH spent, paced at kSettleIntervalMs between attempts
    // (Arcane/Host/SettleBound.hpp). Bounding it by attempts alone WAS the
    // defect -- the quiescence being waited on is denominated in milliseconds,
    // so an attempt count is the wrong unit to express the wait in.
    // =========================================================================
    if (io.config.settleAttempts != 0)
    {
        if (!wantsCapture)
        {
            // DEFENSIVE (fix round 1, item 5): HostConfig::Parse refuses
            // --settle without --screenshot/--report already, so this should
            // be unreachable from any real command line. But `return false`
            // unconditionally here would spin the loop forever if some other
            // caller ever built a HostConfig bypassing Parse (a test, a
            // future embedder) -- fail CLOSED rather than hang.
            ARC_ERROR("--settle has nothing to compare against (--screenshot and --report are "
                      "both empty) -- this should have been refused at parse time; stopping "
                      "rather than spinning forever");
            io.settleConverged = false;
            return true;
        }
        if (!pastBase)
            return false;   // still running the ordinary --frames N budget

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> actual;
        const bool read = io.graph->ReadCapture(w, h, actual);
        ++io.settleAttemptsUsed;

        // Stamp the settle phase's own start on its FIRST attempt, and measure
        // from there. NOT from process start: that would charge boot, project
        // open and the whole --frames budget against the convergence budget,
        // making the timeout depend on how long the scene took to load.
        if (io.settleAttemptsUsed == 1)
            io.settleStartedAt = std::chrono::steady_clock::now();
        io.settleElapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - io.settleStartedAt).count());

        if (!read)
        {
            // Same "a miss is not a bad frame" contract ReadCapture documents
            // elsewhere (the non-settle branch below) -- still counts against
            // the attempt budget (this loop MUST terminate even if every
            // readback misses), but is neither a match nor a new comparison
            // baseline: io.previousCapture* is left exactly as it was, so the
            // NEXT attempt still compares against the last frame that
            // actually landed rather than against nothing.
            ARC_WARN("--settle attempt {}/{}: capture FAILED (no readback landed this frame)",
                     io.settleAttemptsUsed, io.config.settleAttempts);
        }
        else
        {
            const bool byteEqual = io.previousCaptureValid &&
                                    w == io.previousCaptureWidth && h == io.previousCaptureHeight &&
                                    actual == io.previousCaptureRgba;
            // FIX ROUND 1, ITEM 1. Byte-equal alone is a STABILITY predicate
            // ("nothing rendered differently between these two samples"), not
            // the QUIESCENCE predicate this mode actually needs ("nothing is
            // still loading"). Collection is NOT clock-gated: Drain() runs
            // inside SceneRenderResolver::Refresh every PrepareFrame,
            // regardless of io.hostClock -- only DISPATCH is gated, by
            // ShaderCompiler::Poll's readyAt check. So two frozen-clock
            // attempts can compare byte-equal while a dispatched-but-
            // undrained compile is still genuinely in flight: it simply had
            // no chance to finish (or be drained) in the ~1ms two
            // back-to-back offscreen frames take. IsIdle() ("nothing
            // pending, in flight, or waiting to be drained" --
            // ShaderCompiler.hpp) is what actually answers whether async
            // work this capture could still be missing is outstanding, so
            // convergence requires BOTH.
            const bool idle = io.compiler.IsIdle();

            // PLAN B (Task 8): the comparison is a THIRD conjunct, evaluated
            // HERE -- inside the loop -- and not after it. Settle without a
            // goal cannot detect "stably wrong"; a goal without settle
            // flakes on tearing. They are two halves of one mechanism.
            //
            // FINDING 4 (Task 8 dispatch audit): io.compareRequested is
            // false whenever --bless was given, REGARDLESS of whether a
            // reference already exists -- see FrameIo::compareRequested's
            // own comment for why blessing must disable this conjunct
            // rather than gate on it. `matches` therefore stays its default
            // `true` on any run that never asked for --compare, or that did
            // ask but paired it with --bless, so this conjunct degrades to
            // Plan A's `byteEqual && idle` exactly in both cases.
            bool matches = true;
            if (byteEqual && idle && io.compareRequested)
            {
                Arcane::PixelData actualPixels;
                actualPixels.width  = w;
                actualPixels.height = h;
                actualPixels.rgba   = actual;
                io.compareResult   = Arcane::CompareImages(io.referencePixels, actualPixels,
                                                            io.compareOptions);
                matches             = io.compareResult.passed;
                io.compareEvaluated = true;
            }

            // THE SAME RULE Arcane::SettleConverged spells (Host/SettleBound.hpp),
            // left inline here deliberately: the runtime's `idle` is just its own
            // shaderIdle (no cook queue, no thumbnail harvester to widen with), which
            // is the case that header's own comment carves out. Spelled inline, but
            // NOT a second rule -- change one and change both.
            if (byteEqual && idle && matches)
            {
                io.settleConverged = true;
                ARC_INFO("--settle: CONVERGED after {} attempt(s) -- frame {} matches frame {} "
                         "byte-for-byte ({}x{}), compiler idle", io.settleAttemptsUsed,
                         io.frameCount, io.frameCount - 1, w, h);

                // The exact pixels a player sees, same as the non-settle tail
                // below -- this IS the converged capture (either of the two
                // equal frames would do; this one, the later, is what gets
                // written/stashed).
                if (!io.config.screenshotPath.empty())
                {
                    if (Arcane::WritePngRgba(io.config.screenshotPath, w, h, actual.data()))
                        ARC_INFO("screenshot written: {} ({}x{})", io.config.screenshotPath, w, h);
                    else
                        ARC_WARN("screenshot FAILED: {}", io.config.screenshotPath);
                }

                io.captureWidth  = w;
                io.captureHeight = h;
                io.captureRgba   = std::move(actual);
                io.captureRead   = true;
                return true;   // converged -- stop the loop
            }

            if (byteEqual && idle && !matches)
            {
                // Stable, quiescent, and WRONG. Do NOT fail fast here: on a
                // platform where ShaderCompiler::IsIdle()'s non-_WIN32 stub
                // returns true unconditionally, `idle` is vacuous and the
                // reference goal is the ONLY signal left that can separate
                // "still loading" from "genuinely wrong" -- so spending the
                // remaining attempts is what keeps that platform honest. The
                // bail conjunction below still bounds how long this can spin
                // (attempt budget AND --settle-timeout, never either alone).
                ARC_WARN("--compare attempt {}/{}: converged pixels do not match reference "
                         "'{}' -- {}", io.settleAttemptsUsed, io.config.settleAttempts,
                         io.config.compareReference, io.compareResult.errorMessage);
            }

            if (byteEqual && !idle)
            {
                // The pixels agree but the compiler is not done -- exactly
                // the case this fix closes: NOT counted as converged.
                ARC_WARN("--settle attempt {}/{}: frame {} matches frame {} byte-for-byte, but "
                         "the shader compiler is not idle yet (still pending/in-flight/"
                         "undrained) -- not counted as converged",
                         io.settleAttemptsUsed, io.config.settleAttempts, io.frameCount,
                         io.frameCount - 1);
            }

            // Not settled yet -- either the bytes genuinely differed, or they
            // matched but the compiler still has outstanding work. Either
            // way THIS frame becomes the baseline the NEXT attempt compares
            // against.
            io.previousCaptureWidth  = w;
            io.previousCaptureHeight = h;
            io.previousCaptureRgba   = std::move(actual);
            io.previousCaptureValid  = true;
        }

        // THE BAIL IS A CONJUNCTION (see Arcane/Host/SettleBound.hpp): both the
        // attempt budget AND the timeout must be spent. Counting attempts alone
        // was the defect -- the condition being waited on (compiler quiescence)
        // is denominated in MILLISECONDS, and with no pacing a 30-attempt budget
        // was spent in ~100ms, long before background compilation could drain.
        io.settleBail = Arcane::SettleBailDecision(io.settleAttemptsUsed, io.config.settleAttempts,
                                                   io.settleElapsedMs, io.config.settleTimeoutMs,
                                                   Arcane::kSettleIntervalMs);
        if (io.settleBail != Arcane::SettleBail::Keep)
        {
            // No readback EVER landed, so the loop never had two frames to
            // compare. That is a different fact from "captured fine, never
            // agreed", and an agent needs to tell them apart: one is a broken
            // capture path, the other a genuinely unstable scene. Recorded HERE
            // -- at the bail, before anything else can touch the baseline --
            // and ranked ABOVE the governing bound by the report (Task 3,
            // VerifyReport::SetSettle), precisely because it means neither
            // bound ever got a fair test.
            io.settleCaptureFailed = !io.previousCaptureValid;

            // NON-CONVERGENCE. FAIL EXPLICITLY rather than hand back
            // whatever the last attempt happened to render: io.settleConverged
            // stays false (RuntimeApp::ShutdownGraphPath reads it to set
            // exitReason "settle-not-converged" and a nonzero process exit
            // code) and io.captureRead is deliberately left false -- no
            // --screenshot is written, and the report's SetCapture is never
            // called, so an agent reading either artifact gets an honest
            // ABSENCE rather than a frame this run never actually agreed on.
            //
            // The GOVERNING bound is named, not merely the one that tripped, so
            // the caller knows which knob would actually change the outcome.
            ARC_ERROR("--settle: NOT CONVERGED after {} attempt(s) / {} ms -- two consecutive "
                      "captures never compared byte-equal with the compiler idle ({})",
                      io.settleAttemptsUsed, io.settleElapsedMs,
                      io.settleBail == Arcane::SettleBail::AttemptsBound
                          ? "attempt budget governed -- raise --settle"
                          : "timeout governed -- raise --settle-timeout");
            return true;   // stop the loop -- the run failed, but it must still stop
        }
        // PACING. Without this the loop spins as fast as the GPU can render, so
        // the time bound could never be reached by anything but a slow frame --
        // the interval is what makes the timeout a real budget rather than a
        // formality. 50ms is OURS, not inherited; see kSettleIntervalMs.
        std::this_thread::sleep_for(std::chrono::milliseconds(Arcane::kSettleIntervalMs));
        return false;   // keep going: one more attempt next frame
    }

    // =========================================================================
    // The ordinary (non-settle) tail, unchanged from Task 8: reached on the
    // final frame only, AFTER the present -- so the pixels are the ones that
    // were actually shown and the capture cannot race the recording that
    // produced it.
    //
    // GATED on --screenshot OR --report (Task 8), not --screenshot alone: a
    // report's Brightness/Luma/Rgba probes need this same readback, and
    // RenderGraph's graphFrame.capture arm (above) is widened identically --
    // this read only ever finds pixels when that node was actually declared
    // this frame, so the two gates must agree.
    // =========================================================================
    const bool lastFrame = pastBase;
    if (lastFrame && wantsCapture)
    {
        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> actual;
        // The graph's readback NODE ran inside this frame's own command
        // buffer (declared up front, because a graph capture is a node and
        // not an afterthought) -- ReadCapture maps it and normalizes
        // BGRA -> RGBA off m_format, not a swapchain read, which is exactly
        // what makes this call MODE-AGNOSTIC: windowed resolves m_format
        // from the swapchain, offscreen sets it to kGraphOffscreenFormat,
        // and this call site never has to know which.
        const bool read = io.graph->ReadCapture(w, h, actual);

        if (!read)
        {
            // A WARN, never an exit code: ReadCapture's own contract is
            // explicit that "a capture that could not be read is not a bad
            // frame" -- so a miss here must not trip the GPU tests'
            // RenderErrorCount()==0 gate, nor flip io.graphExit, nor fail a
            // --report run (io.captureRead simply stays false below, so
            // VerifyReport::SetCapture is never called).
            if (!io.config.screenshotPath.empty())
                ARC_WARN("screenshot FAILED: {} (no capture landed)", io.config.screenshotPath);
            else
                ARC_WARN("capture FAILED: no capture landed this frame -- the report's pixel "
                         "probes (brightness/luma/rgba) will report 'no capture set'");
        }
        else
        {
            // The exact pixels a player sees (post-tonemap, post-ImGui).
            if (!io.config.screenshotPath.empty())
            {
                if (Arcane::WritePngRgba(io.config.screenshotPath, w, h, actual.data()))
                    ARC_INFO("screenshot written: {} ({}x{})", io.config.screenshotPath, w, h);
                else
                    ARC_WARN("screenshot FAILED: {}", io.config.screenshotPath);
            }

            // Stashed for VerifyReport regardless of --screenshot:
            // RuntimeApp::ShutdownGraphPath hands these to SetCapture() once
            // the loop has ended and the run's identity (backend/frameCount/
            // exitReason) is settled -- see that method's own comment.
            io.captureWidth  = w;
            io.captureHeight = h;
            io.captureRgba   = std::move(actual);
            io.captureRead   = true;
        }
    }

    return lastFrame;
}

}
