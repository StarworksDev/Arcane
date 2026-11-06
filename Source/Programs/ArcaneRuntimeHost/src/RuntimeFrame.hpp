#pragma once
// RuntimeFrame: RuntimeApp::MainLoop's frame BODY, as free functions.
//
// MainLoop keeps: the pre-loop setup (locals, the render-half boot + window
// reveal), the while/if-else SKELETON that calls the functions below in
// order, and the post-loop ShutdownGraphPath() call. Everything else lives
// here.
//
// THE SHAPE, so a reader can see it rather than reconstruct it:
//   * ONE WINDOW IS PUMPED, always the host's. Windowed, the render half
//     borrows this exact window and presents into it; under --headless the
//     window exists but is never shown, and the pump keeps running because
//     ImGui and the input stack are wired to it either way.
//   * ONE RESIZE, `io.graph->Resize`, to that window's one presentation
//     surface and to nothing else -- and only in HOST-WINDOW MODE, because
//     that is the only mode with a surface to resize. See PumpAndResize.
//   * BuildHud IS ONLY THE HUD: the ImGui frame, the "ArcaneRuntime" window
//     and DrawUIAll. PrepareFrame, called straight after it, is the scene
//     resolver's Refresh.
//   * THE FRAME'S EXTENT comes from the vehicle's own surface -- the
//     swapchain's, or the offscreen output's, read mode-agnostically through
//     SurfaceWidth/SurfaceHeight (see FrameExtent in the .cpp). There is no
//     second surface that could disagree with it in either mode.
//   * ImGui CALLS GO THROUGH ImGuiLayer (RenderToDrawData / EndFrameDiscard)
//     instead of bare ImGui::Render()/EndFrame(), so the context pin the
//     layer owns covers them too.
//
// RenderGraph is the only per-frame render function here: the frame graph is
// the only render path. It is the ONE function that still names the mode, at
// exactly one line -- the vehicle's RenderFrame/RenderFrameOffscreen pair
// refuses each other's mode by design, so the frame driver has to pick.

#include <Arcane/Assets/ImageCompare.hpp>            // --compare N (Task 8): PixelData/ImageCompareOptions/ImageCompareResult
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Host/FramePerf.hpp>
#include <Arcane/Host/GpuContext.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Host/SettleBound.hpp>               // --settle's bail conjunction: SettleBail/SettleBailDecision/kSettleIntervalMs
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/GpuFaultInjector.hpp>       // dev-only --crash-gpu N (kPassName only; RenderGraph fires through NriDiagnostics::FireFault)
#include <Arcane/Render/GpuSceneTypes.hpp>          // GpuSceneFrameStats (FrameIo::gpuSceneStats, F3 plan 1 T8)
#include <Arcane/Render/Nri/NriGraphContext.hpp>    // the graph vehicle (RenderGraph); unconditional
#include <Arcane/Render/PickEmit.hpp>                // PickDrawable (--pick-probe)
#include <Arcane/Render/RenderFramePacket.hpp>       // PacketRing (FrameIo::packetRing, F5 plan 1 Task 5)
#include <Arcane/Render/ShaderCompiler.hpp>          // --settle N's IsIdle() quiescence check (Task 10 fix round 1)


#include <chrono>
#include <cstdint>
#include <vector>

// Forward declaration only: FrameIo just needs a reference to call the one
// public RuntimeApp method RenderGraph reaches (PushSceneCamera). Full
// definition lives in RuntimeApp.hpp, included by RuntimeFrame.cpp (and by
// RuntimeApp.cpp, which already needed it).
class RuntimeApp;

namespace Arcane::RuntimeFrame
{
    // Everything MainLoop's body reaches through `this` or a loop-local,
    // bundled by reference/pointer so the functions below can read and write
    // it without becoming RuntimeApp members. NOTHING here is NEW state:
    // every field is bound, once, in MainLoop to a RuntimeApp member, a
    // MainLoop-scope local, or -- for `graph` -- the vehicle
    // RuntimeApp::Graph() resolved out of the two mutually exclusive members
    // that could hold one. Pointer fields mirror how MainLoop reaches the
    // same object (m_gpu via `->`, the resolved graph likewise, m_resolver/
    // m_runtime/m_plugin the same); reference fields mirror members it
    // accesses with `.` (m_config, m_perf, the scalar/vector perf locals).
    struct FrameIo
    {
        // ---- the pointers MainLoop already owned ----------------------------
        Arcane::GpuContext*          gpu;       // never null once MainLoop is running
        // THE LIVE VEHICLE, and never null: MainLoop builds exactly one of
        // m_graphContext (windowed) / m_offscreen (--headless) and RETURNS
        // before the frame loop if that build failed, so by the time any
        // function below runs there is a vehicle. Which of the two it is, is
        // resolved once at the binding site (RuntimeApp::Graph()) -- the frame
        // body does not branch on --headless to find its graph, and asks
        // IsOffscreen() only where the vehicle's own API genuinely differs.
        Arcane::NriGraphContext*     graph;
        Arcane::SceneRenderResolver* resolver;  // null iff the boot scene published none
        // ClientRuntime: the frame body drives presentation (input snapshot,
        // audio tick, camera, render context) as well as the headless substrate,
        // which it reaches through the P5 aliases.
        Arcane::ClientRuntime*       runtime;
        Arcane::PluginHost*          plugin;
        // PushSceneCamera is RuntimeApp's own, made public for exactly this
        // call (see RuntimeApp.hpp's comment on it).
        RuntimeApp&                  app;

        // ---- HostConfig& + the perf locals ----------------------------------
        Arcane::HostConfig&    config;
        Arcane::FramePerf&     perf;
        std::uint64_t&         frameCount;
        int&                   graphExit;
        Arcane::GlobalParams&  frameGlobals;
        double&                hostClock;
        double&                lastFrameDt;
        std::vector<Arcane::PickDrawable>& pickDrawables;
        std::vector<std::uint32_t>&        pickSelectedIds;

        // GPU-scene figures for the HUD and ShutdownGraphPath (F2a Task 10;
        // F3 plan 1 T8). The graph borrows the extract packet's mesh frame.
        // RenderGraph is a free function, so the copy lives on RuntimeApp and
        // is reached through here. Only the stats and the transparent-draw
        // count are copied, so the copy allocates nothing.
        Arcane::GpuSceneFrameStats& gpuSceneStats;

        // ---- THE LAST-FRAME CAPTURE (Task 8: --report wiring) ---------------
        // CaptureTail fills these on the run's LAST frame whenever --screenshot
        // or --report was requested (see that function's own comment on the
        // widened read gate) -- the SAME readback --screenshot already takes,
        // just retained here instead of written straight to a PNG and
        // discarded, because a report's Brightness/Luma/Rgba probes need the
        // identical pixels. RuntimeApp::ShutdownGraphPath is where they are
        // consumed, once the loop has ended and backend/frameCount/exitReason
        // are all settled -- see that method's own comment for why the WRITE
        // happens there rather than here.
        //
        // captureRead stays at its bound member's default (false) on any run
        // that broke out early (device lost, render failure, an interactive
        // quit) or that asked for neither flag; a VerifyReport built from that
        // simply never calls SetCapture rather than reporting a phantom frame.
        bool&                       captureRead;
        std::uint32_t&              captureWidth;
        std::uint32_t&              captureHeight;
        std::vector<unsigned char>& captureRgba;

        // ---- --settle N (Task 10) --------------------------------------------
        // The PREVIOUS settle attempt's capture -- kept only long enough to
        // compare against the NEXT one (CaptureTail's byte-equal check) -- plus
        // the running state of BOTH halves of the bail conjunction (the attempt
        // counter here, the clock below). Distinct storage from
        // captureRead/captureWidth/captureHeight/captureRgba just above: those
        // four hold the FINAL, agreed-on capture (only ever written once
        // convergence -- or the ordinary Task 8 single-shot capture -- has
        // actually happened); these hold the WORKING comparison baseline, which
        // churns every attempt until then and is never itself a claim about
        // anything converged.
        //
        // previousCaptureValid stays false until the FIRST settle attempt lands
        // a readback, so the very first attempt (which has nothing yet to
        // compare against) can never spuriously "match".
        std::vector<unsigned char>& previousCaptureRgba;
        std::uint32_t&              previousCaptureWidth;
        std::uint32_t&              previousCaptureHeight;
        bool&                       previousCaptureValid;
        // How many settle attempts have been taken so far (bounded by
        // HostConfig::settleAttempts) and whether one of them converged.
        // settleConverged is what RuntimeApp::ShutdownGraphPath reads to decide
        // exitReason "settle-not-converged" and the process exit code -- see
        // that method's comment. Both stay at their defaults (0/false) on a run
        // that never asked for --settle at all, which must never read as a
        // failure.
        //
        // uint64_t, matching HostConfig::settleAttempts' own width (fix round 1,
        // item 4): a uint32_t counter compared against a uint64_t budget would
        // silently WRAP on an absurd `--settle` value larger than 2^32, turning
        // "run until convergence or N attempts" back into the unstoppable-process
        // hazard --headless's own --frames-required refusal exists to prevent.
        std::uint64_t&              settleAttemptsUsed;
        bool&                       settleConverged;
        // THE TIME HALF of settle's bail conjunction (see Arcane/Host/
        // SettleBound.hpp). settleStartedAt is stamped on the FIRST attempt and
        // settleElapsedMs measured from there -- NOT from process start: that
        // would charge boot, project open and the whole --frames budget against
        // the convergence budget, making the timeout depend on how long the
        // scene took to load. settleBail records WHICH bound governed, so the
        // caller is told which knob to turn (and Task 3 can report it).
        std::chrono::steady_clock::time_point& settleStartedAt;
        std::uint64_t&              settleElapsedMs;
        Arcane::SettleBail&         settleBail;
        // Set at the bail: true iff no readback EVER landed, so the loop never
        // had two frames to compare and neither bound got a fair test. Bound
        // through here (rather than derived later from previousCaptureValid,
        // which CaptureTail keeps mutating) so ShutdownGraphPath reports the
        // state AS IT WAS when the loop gave up -- Task 3's
        // VerifyReport::SetSettle reads it.
        bool&                       settleCaptureFailed;
        // ShaderCompiler::IsIdle() (fix round 1, item 1): byte-equal consecutive
        // captures are a STABILITY predicate ("nothing rendered differently
        // between these two samples"), not the QUIESCENCE predicate this mode
        // actually needs ("nothing is still loading"). Collection (Drain, via
        // SceneRenderResolver::Refresh every PrepareFrame) is NOT gated on
        // io.hostClock -- only DISPATCH is (ShaderCompiler::Poll's readyAt
        // check) -- so two frozen-clock frames can compare byte-equal while a
        // dispatched-but-undrained compile is still genuinely in flight, simply
        // because it had no chance to finish (or be drained) in the ~1ms
        // between two back-to-back offscreen frames. CaptureTail conjoins
        // `compiler.IsIdle()` into the convergence check for exactly this
        // reason -- see that function's comment. A REFERENCE to RuntimeApp's
        // own m_shaderCompiler (RuntimeApp.hpp), the same object
        // SceneRenderResolver's Services already point at -- not a second
        // instance, so this reads the SAME pending/in-flight/undrained state
        // the resolver's own Refresh just drained from.
        Arcane::ShaderCompiler&     compiler;

        // ---- --compare / --bless (Task 8) ------------------------------------
        // Whether the settle loop's convergence predicate grows its THIRD
        // conjunct THIS run -- true only when --compare was given AND
        // --bless was NOT (RuntimeApp::MainLoop computes it once, before the
        // loop starts, from HostConfig::compareReference/::bless). FINDING 4
        // of the Task 8 dispatch audit: blessing exists to REPLACE the
        // reference, so the OLD one on disk must never also gate
        // convergence -- an intentional visual change could otherwise never
        // converge (the fresh capture can never match the stale reference it
        // is meant to replace), and a first bless could never converge
        // either (there is nothing yet to compare against). A --bless run
        // therefore converges on byteEqual && idle ALONE, exactly like a
        // plain --settle run with no --compare at all -- see CaptureTail's
        // own comment on the conjunct.
        bool compareRequested;

        // The reference pixels the compare conjunct reads, resolved and
        // loaded ONCE before the loop starts (FINDING 3 of the dispatch
        // audit: a missing reference cannot start existing after N wasted
        // attempts, so RuntimeApp::MainLoop fails fast -- exitReason
        // "compare-missing-reference" -- before ever reaching this loop,
        // rather than deferring the discovery to here and spending the
        // whole --settle budget spinning CompareImages against invalid
        // pixels). Only ever READ when compareRequested is true -- a
        // --bless run may leave this default/invalid, and that is fine,
        // because nothing reads it (see compareRequested above).
        const Arcane::PixelData& referencePixels;
        // The two aggregate diff-pixel budgets (HostConfig::maxDiffPixels /
        // ::maxDiffPixelRatio), already folded into one options struct --
        // CompareImages' own "the smaller budget wins" rule applies inside
        // it, not here.
        const Arcane::ImageCompareOptions& compareOptions;

        // The MOST RECENT CompareImages() verdict -- overwritten every
        // settle attempt that actually evaluates the conjunct (byteEqual &&
        // idle && compareRequested). RuntimeApp::ShutdownGraphPath reads
        // whatever is here once the loop ends, which is exactly the attempt
        // that decided the run's outcome, whether that is a converged pass
        // or a budget-exhausted mismatch. Left at its default (passed =
        // false) on any run that never evaluated it -- compareEvaluated
        // below is the flag that tells "never got the chance" apart from
        // "compared, and this is a genuine failing verdict"; this result
        // alone cannot.
        Arcane::ImageCompareResult& compareResult;
        // Whether compareResult was ever actually written by a real
        // CompareImages() call this run. Stays false forever on a run with
        // no --compare, on a --bless run (the conjunct is disabled -- see
        // compareRequested), or on one where the settle budget was spent
        // without ever reaching a single byteEqual&&idle attempt.
        bool& compareEvaluated;

#if !defined(ARC_BUILD_DIST)
        // --crash-gpu N. Same Dist guard as the RuntimeApp member this is
        // bound to -- see RuntimeApp.hpp. The fired-once latch is the only
        // state needed: RenderGraph fires through the stateless
        // NriDiagnostics::FireFault, which holds nothing between frames.
        bool&                                      gpuFaultFired;

        // --hang-main N (crash window plan 2, D10). Same shape as
        // gpuFaultFired directly above -- a fired-once latch, nothing else
        // to carry between frames.
        bool&                                      hangMainFired;
#endif

        // ---- MainLoop-scope locals, hoisted one level up (before the while
        // loop) so more than one of the functions below can see them across a
        // single iteration. ------------------------------------------------
        std::chrono::steady_clock::time_point& simPrev;
        std::chrono::steady_clock::time_point& lastFrameTime;
        std::chrono::steady_clock::time_point& lastShaderPoll;

        // ---- control-flow outputs -------------------------------------------
        // A called free function cannot `break`/`continue` MainLoop's while
        // loop itself, so the two early-exit paths set one of these and
        // return; MainLoop checks it right after the call.
        //
        // quit: set by AdvanceSim on the Pressed("quit") input action -- the
        // one quit path PumpAndResize's own bool return does not cover.
        // MainLoop: `AdvanceSim(io); if (io.quit) break;`.
        bool quit = false;
        // skipFrame: set true by PumpAndResize on a minimized window, which
        // sends MainLoop back to the top of its loop. PrepareFrame only ever
        // clears it. MainLoop checks it after both calls and `continue`s in
        // turn.
        bool skipFrame = false;

        // ---- the extract stage's packets (F5 plan 1 Task 5) -----------------
        // RenderGraph's one ExtractFrame call publishes into this ring and binds
        // the returned slot on io.graph; FramesInFlight() slots, so the packet
        // the graph reads outlives the RenderFrame call. Owned HERE, by value,
        // as the one FrameIo member that is not a binding (MainLoop's
        // designated initializer leaves it default-constructed).
        Arcane::PacketRing packetRing{};
        // remoteSettleHold (Task 30 fix round 1): a remote `wait settled` is
        // in its capture phase (HostSession::SettleClockFrozen), set by
        // MainLoop after each drain. AdvanceSim ORs it into its --settle
        // `settleHold`, so the captures see one frozen clock, held or running.
        bool remoteSettleHold = false;
    };

    // Heartbeat + device-lost check + this frame's ONE window-event pump +
    // resize handling. Returns true when MainLoop must `break` (device lost,
    // or the window's close was requested) -- see FrameIo::quit's sibling
    // comment. A minimized window sleeps 1ms, sets io.skipFrame and returns
    // false (MainLoop `continue`s).
    //
    // ONE window: the host's. Its resize goes to that window's one
    // presentation surface and to nothing else -- and under --headless there
    // IS no such surface, so the resize is dropped rather than forwarded.
    bool PumpAndResize(FrameIo& io);

    // Per-frame timing (wall dt for input, clamped sim dt), input sampling +
    // the reload_plugin/reload_plugin_fresh actions, and the fixed-step
    // RunLoop::Advance (FixedUpdateAll/UpdateAll) + the audio voice reap.
    // Sets io.quit true on the Pressed("quit") input action.
    //
    // --settle N (Task 10): once io.frameCount reaches the configured
    // --frames budget, this FREEZES frameDt/simDt at 0 rather than advancing
    // them -- see the .cpp's own comment for why that is load-bearing (a
    // Time-driven material would otherwise never let CaptureTail's
    // byte-equal comparison converge on anything).
    //
    // `pace` is the remote tick gate's answer for this frame (AM1; MainLoop
    // maps Arcane::Remote::TickDecision onto it, and a host without a remote
    // session always passes Run):
    //   Run        -- the paragraph above, unchanged.
    //   SingleTick -- exactly ONE canonical fixed step (RunLoop::
    //                 RequestSingleStep, 1 / FixedHz) on every runtime shape,
    //                 windowed or --headless, whatever the wall-clock dt or the
    //                 loop's backlog: `servitor step N` is N sim ticks.
    //   Hold       -- no input sampling and no sim step. The frame preamble
    //                 (cvar publish, perf window) still runs, and the HOST clock
    //                 keeps advancing by wall time: it is the shader-compile
    //                 debounce clock, so a frozen one would never dispatch a
    //                 compile submitted while held. The wall stamps are
    //                 re-taken, so the next windowed Run step is not handed the
    //                 held interval as its sim dt.
    enum class SimPace : std::uint8_t { Run, SingleTick, Hold };
    void AdvanceSim(FrameIo& io, SimPace pace = SimPace::Run);

    // ImGui BeginFrame + the host's own "ArcaneRuntime" HUD window + the
    // plugin's DrawUIAll. Nothing else. RenderGraph renders the ImGui frame
    // this begins, so every BeginFrame is paired exactly once.
    void BuildHud(FrameIo& io);

    // The scene asset resolver's per-frame Refresh, which is what fills
    // io.frameGlobals. There is no shader hot-reload poll beside it: the
    // render path reads offline artifacts through
    // NriGraphContext::ShaderBytecode and has no ShaderLibrary to poll.
    // io.skipFrame is unconditionally cleared.
    void PrepareFrame(FrameIo& io);

    // The render arm, and the ONLY recorder in the process: the HUD
    // draw-data capture, the 2D submission, the dev-only --crash-gpu fault
    // and --pick-probe collection, the graph FrameDesc, and one
    // NriGraphContext::RenderFrame call. Returns the outcome so MainLoop --
    // the only party that can actually `break`/`continue` its own loop --
    // performs Failed -> break / Skipped -> continue.
    Arcane::NriGraphContext::FrameOutcome RenderGraph(FrameIo& io);

    // The frame's tail, reached from RenderGraph: the plugin's debounced
    // hot-reload Poll(), the perf Tick(), the frame counter, and the
    // capture/screenshot bookkeeping on the final frame only. Returns true
    // (`stop`) on the final frame -- MainLoop does
    // `if (CaptureTail(io)) running = false;`, so the while condition, not an
    // explicit break, is what ends the loop.
    //
    // --settle N (Task 10): "the final frame only" above describes the
    // ordinary (non-settle) tail unchanged from Task 8. When
    // io.config.settleAttempts != 0, this instead keeps returning false
    // (keep going) once the ordinary --frames budget is reached, one more
    // settle attempt per call, until two consecutive captures compare
    // byte-equal AND io.compiler.IsIdle() (fix round 1, item 1 -- byte-equal
    // alone is a stability predicate, not the quiescence one this mode needs;
    // see FrameIo::compiler's comment) -- converged: true is returned and the
    // captured pixels are published the same way Task 8's tail does -- or the
    // bail CONJUNCTION fires: BOTH the attempt budget AND --settle-timeout
    // spent, never either alone (Arcane/Host/SettleBound.hpp's
    // SettleBailDecision, which this calls, is what decides, and it also names
    // WHICH bound governed). On that non-convergence true is returned too --
    // the loop MUST still stop -- but io.captureRead is deliberately left
    // false and no --screenshot is written, so nothing downstream can mistake
    // an unconverged run for a converged one.
    bool CaptureTail(FrameIo& io);
}
