#pragma once

// RunLoop: fixed-timestep accumulator + render alpha. Mirrors the proven Lua
// Application cadence (fixed 60 UPS, alpha on draw). Header-only so it operates
// on the host-owned Registry (see SystemSchedulers.hpp for the ownership rule).
//
// Advance() runs >=0 FixedUpdate steps then Update once, through the parallel
// executor. SubmitRender() runs the Render scheduler SEQUENTIALLY on the calling
// thread -- but the Render phase is RETIRED (F5 plan 1 Task 5): no host calls
// SubmitRender any more. Sprites are submitted once per frame by the frame
// extract (Arcane/Render/ExtractFrame.hpp), which runs RenderSubmissionSystem
// itself between the host's Batcher2D::Begin and the drain it copies.
//
// Sim-time control (Epic 04): pause / single-step / time-scale gate the FIXED
// phase only; Update and the frame (extract + render) keep running so an editor's camera/UI/input stay
// live while the sim is frozen. Consumers reach these through Runtime::Loop();
// there is no separate Runtime API and no plugin-ABI surface. TIME-SCALE scales
// the SIM CLOCK by scaling the accumulator input -- the fixed step stays canonical
// (1/fixedHz), so the physics step is byte-identical at any scale (determinism is
// preserved; a variable step dt would defeat the fixed-timestep contract). Smooth
// slow-mo is a render-interpolation concern (lerp by Alpha()), not a change to the
// time model.

#include <Arcane/Sim/SimSettingsData.hpp>   // the Config defaults (settings arc S6-8)
#include <Arcane/Sim/SystemSchedulers.hpp>
#include <Arcane/Sim/Time.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cstdint>
#include <functional>
#include <utility>

namespace Arcane
{
    class RunLoop
    {
    public:
        struct Config
        {
            double fixedHz = SimSettings{}.fixedHz;
            int    maxStepsPerFrame = SimSettings{}.maxStepsPerFrame;   // clamp to avoid the spiral of death
        };

        // Two constructors rather than one with `Config cfg = {}`: GCC (16)
        // rejects a default argument that default-constructs the NESTED
        // Config here ("default member initializer for 'Config::fixedHz'
        // required before the end of its enclosing class" -- it processes
        // the outer class's default arguments before the nested class's
        // member initializers; `= Config{}` fails the same way). A
        // delegating constructor's mem-initializer is evaluated where GCC,
        // MSVC and Clang all agree Config is complete; the call sites
        // (`RunLoop(reg, sched)` / `RunLoop(reg, sched, cfg)`) are unchanged
        // (multibackend hardening, review F2).
        RunLoop(Astra::Registry& registry, SystemSchedulers& schedulers)
            : RunLoop(registry, schedulers, Config{}) {}

        RunLoop(Astra::Registry& registry, SystemSchedulers& schedulers, Config cfg)
            : m_registry(&registry), m_schedulers(&schedulers), m_cfg(cfg) {}

        // ---- sim-time control -------------------------------------------------
        // Freeze/thaw the fixed phase. While paused the sim clock does not advance
        // (no accumulation, zero fixed steps) but Update and the frame still run.
        void SetPaused(bool p) noexcept { m_paused = p; }
        [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }

        // Sim-clock rate relative to real time (1.0 == real time). Clamped to >= 0;
        // 0 stalls the sim like a pause but keeps accumulating nothing. Scales the
        // accumulator input only -- the fixed step stays canonical.
        void SetTimeScale(double s) noexcept { m_timeScale = s < 0.0 ? 0.0 : s; }
        [[nodiscard]] double TimeScale() const noexcept { return m_timeScale; }

        // Request exactly one canonical fixed step on the next Advance, even while
        // paused; one-shot (auto-clears after it runs). On a running loop that
        // Advance runs the one step INSTEAD of accumulating its realDt, so it is
        // one tick whatever the dt, backlog or time scale, and the backlog is
        // left as it was (StepFixed; the runtime's remote `step` relies on it).
        void RequestSingleStep() noexcept { m_singleStep = true; }
        // A single step is requested and the next Advance will run it (the
        // editor counts that Advance as a sim advance for host.frame, Servitor
        // P0 Task 29).
        [[nodiscard]] bool SingleStepPending() const noexcept { return m_singleStep; }

        // The fixed step's rate (Hz) -- a RUNTIME value (Core-DLL split, plan 1
        // Task 6 review round 1): StepFixed's canonical `fixedDt` (`1.0 /
        // m_cfg.fixedHz`) used to be fixed at construction, so a host whose
        // --fixed-dt names a non-default step (Source/Programs/ArcaneServer/src/ServerApp.cpp)
        // had no way to make that step size real -- only realDt, the Advance()
        // PARAMETER, was under its control, and realDt only decides how many
        // fixed steps ACCUMULATE per Advance() call, never their size. The two
        // are independent by design (time-scale's own header comment: "the fixed
        // step stays canonical... determinism is preserved"), which is exactly
        // why changing one alone silently left the other at Config::fixedHz's
        // default of 60. SetFixedHz is the setter that makes the rate itself
        // change. Refused (a no-op) for a non-positive hz: StepFixed divides by
        // it, so zero or negative would be a divide-by-zero or a nonsensical
        // negative step -- the same "refuse the impossible request" contract
        // SetTimeScale's own clamp upholds, just declining rather than clamping
        // (there is no sane default to clamp TO here).
        void SetFixedHz(double hz) noexcept { if (hz > 0.0) m_cfg.fixedHz = hz; }
        [[nodiscard]] double FixedHz() const noexcept { return m_cfg.fixedHz; }

        // The spiral-of-death cap (sim.maxStepsPerFrame, Live): the hosts set
        // it each frame through ApplySimStepCap (SimSettings.hpp). Refused for
        // a non-positive count, which would run no fixed step and drop every
        // frame's backlog.
        void SetMaxStepsPerFrame(int steps) noexcept { if (steps > 0) m_cfg.maxStepsPerFrame = steps; }
        [[nodiscard]] int MaxStepsPerFrame() const noexcept { return m_cfg.maxStepsPerFrame; }

        // Called first in every Advance, before any fixed step (spec 2026-10-08
        // s7.2, amendment A7). Physics-agnostic: Runtime installs the physics
        // frame-window clear, so this header never includes PhysicsSystem.hpp.
        void SetFrameBeginHook(std::function<void(Astra::Registry&)> hook) { m_frameBegin = std::move(hook); }

        // Advance one real frame. Returns the render alpha in [0,1) for interpolation.
        double Advance(double realDt)
        {
            if (m_frameBegin) m_frameBegin(*m_registry);
            StepFixed(realDt, nullptr);
            PublishTime(realDt, /*inFixedStep*/ false);
            m_schedulers->update.Execute(*m_registry, &m_schedulers->executor);
            return m_alpha;
        }

        // Advance stays on std::function (NOT FunctionRef): RunLoop.hpp is in the
        // plugin-facing Runtime.hpp include chain, which is kept Core-free so the
        // minimal/ABI plugins build without Core/src. Advance is a once-per-frame
        // host call, not a hot path, so std::function is fine here.
        //
        // Host-driven variant: pluginFixed(fixedDt) runs each fixed step BEFORE the engine
        // fixedUpdate scheduler (gameplay moves transforms; propagation reads them after).
        // pluginUpdate(dt, alpha) runs once after the Update scheduler. Same spiral-of-death
        // clamp as Advance(realDt).
        double Advance(double realDt,
                       const std::function<void(double)>& pluginFixed,
                       const std::function<void(double, double)>& pluginUpdate)
        {
            if (m_frameBegin) m_frameBegin(*m_registry);
            StepFixed(realDt, &pluginFixed);
            PublishTime(realDt, /*inFixedStep*/ false);
            m_schedulers->update.Execute(*m_registry, &m_schedulers->executor);
            if (pluginUpdate) pluginUpdate(realDt, m_alpha);
            return m_alpha;
        }

        // Run the Render scheduler on the calling thread (single-threaded).
        // RETIRED (F5 plan 1 Task 5): no host calls this; kept for the ABI.
        void SubmitRender()
        {
            m_schedulers->render.Execute(*m_registry);
        }

        double Alpha() const noexcept { return m_alpha; }

        // Rebind to a different registry WITHOUT recreating the loop. Runtime::
        // RestoreRegistry / ResetRegistry swap the live registry and must keep this
        // SAME RunLoop object, so a cached RunLoop* (e.g. a plugin that stored
        // Runtime::Loop() at init, or a host toolbar) does not dangle across a restore.
        // Resets the state that belonged to the OLD registry's run -- the step
        // backlog (accumulator), the render alpha derived from it, a pending
        // single-step, the time scale -- to a fresh loop's defaults. The kept
        // schedulers + Config are unchanged (a restore keeps both).
        //
        // `paused` is deliberately NOT reset: it is the HOST'S MODE (the editor's
        // Edit vs Play), not per-registry sim state, and a registry swap must never
        // silently flip the host into Play. It used to reset here ("a fresh loop's
        // defaults", running included), which the editor's Stop masked by
        // re-pausing right after its RestoreRegistry -- and which its scene-open
        // (ResetRegistry, nothing after) and its hot-reload paths did not, so
        // opening a physics scene in Edit mode unpaused the loop, fixedUpdate's
        // Arcane::PhysicsSystem2D stepped it every frame, and the bodies fell before Play was
        // pressed and never came back (Play snapshotted the fallen poses). Pinned
        // by RuntimeTest ("keeps the RunLoop object stable") and
        // EditorPlayModeTest ("opening a scene in Edit mode does not simulate it").
        //
        // The frame-begin hook is kept too. It is host policy (Runtime installs
        // the physics window clear), not per-registry sim state, and the call
        // goes through the current m_registry.
        void Rebind(Astra::Registry& registry)
        {
            m_registry    = &registry;
            m_accumulator = 0.0;
            m_alpha       = 0.0;
            m_timeScale   = 1.0;
            m_singleStep  = false;
            // The sim clock belongs to the registry's run too (input-seam spec s3):
            // Play starts at step 0.
            m_fixedStep       = 0;
            m_elapsed         = 0.0;
            m_elapsedBase     = 0.0;
            m_elapsedBaseStep = 0;
            m_elapsedDt       = 0.0;
            // Republish at once (IN-8 ruling): Time is transient, so the swapped-in
            // registry has none, and nothing between the swap and the next
            // Advance may see it absent or stale. Step 0, elapsed 0, dt 0,
            // `paused` as kept above.
            PublishTime(/*realDt*/ 0.0, /*inFixedStep*/ false);
        }

    private:
        // Republish Arcane::Time (input-seam spec s3). Called before every fixed
        // step and before Update, so a registry swapped in between frames gets
        // it on the very next pass and no system ever runs without it.
        void PublishTime(double realDt, bool inFixedStep)
        {
            Time t;
            t.realDt      = realDt;
            t.paused      = m_paused;
            t.timeScale   = m_timeScale;
            t.dt          = m_paused ? 0.0 : realDt * m_timeScale;
            t.fixedDt     = 1.0 / m_cfg.fixedHz;
            t.alpha       = m_alpha;
            t.elapsed     = m_elapsed;
            t.fixedStep   = m_fixedStep;
            t.inFixedStep = inFixedStep;
            m_registry->SetResource<Time>(std::move(t));
        }

        // Time::elapsed after step m_fixedStep: fixedStep * fixedDt EXACTLY (spec s3)
        // while the rate is constant -- a running `+= fixedDt` drifts from that
        // product within a few dozen steps -- and rebased at a SetFixedHz change so
        // it stays monotonic: the steps before the change keep the time they took.
        void AdvanceElapsed(double fixedDt) noexcept
        {
            if (fixedDt != m_elapsedDt)
            {
                m_elapsedBase     = m_elapsed;
                m_elapsedBaseStep = m_fixedStep - 1;
                m_elapsedDt       = fixedDt;
            }
            m_elapsed = m_elapsedBase + static_cast<double>(m_fixedStep - m_elapsedBaseStep) * fixedDt;
        }

        // The fixed phase for one real frame, under sim-time control. pluginFixed may
        // be null (the no-callback Advance) or point at the host's std::function.
        void StepFixed(double realDt, const std::function<void(double)>* pluginFixed)
        {
            const double fixedDt = 1.0 / m_cfg.fixedHz;
            const auto runFixed = [&]
            {
                ++m_fixedStep;
                AdvanceElapsed(fixedDt);
                PublishTime(realDt, /*inFixedStep*/ true);
                if (pluginFixed && *pluginFixed) (*pluginFixed)(fixedDt);
                m_schedulers->fixedUpdate.Execute(*m_registry, &m_schedulers->executor);
            };

            if (m_singleStep)
            {
                m_singleStep = false;               // one-shot
                runFixed();                          // exactly one canonical tick
                // Deliberately do NOT touch the accumulator: a manual step advances
                // the sim clock by one tick without disturbing the real-time backlog.
            }
            else if (!m_paused)
            {
                m_accumulator += realDt * m_timeScale;  // time-scale scales the SIM CLOCK
                int steps = 0;
                while (m_accumulator >= fixedDt && steps < m_cfg.maxStepsPerFrame)
                {
                    runFixed();
                    m_accumulator -= fixedDt;
                    ++steps;
                }
                if (steps == m_cfg.maxStepsPerFrame && m_accumulator >= fixedDt)
                    m_accumulator = 0.0;             // dropped the backlog; do not accumulate debt
            }
            // paused (and not single-stepping): no accumulation, no fixed steps.

            m_alpha = m_accumulator / fixedDt;
        }

        Astra::Registry*  m_registry;
        SystemSchedulers* m_schedulers;
        Config m_cfg;
        double m_accumulator = 0.0;
        double m_alpha = 0.0;

        // Sim-time control (Epic 04). Defaults == real-time, running, so Advance is
        // byte-identical to the pre-sim-time-control behavior.
        bool   m_paused    = false;
        double m_timeScale = 1.0;
        bool   m_singleStep = false;

        // The sim clock published as Arcane::Time; reset by Rebind. elapsed is
        // derived from the step count since the last rate change (AdvanceElapsed).
        std::uint64_t m_fixedStep       = 0;
        double        m_elapsed         = 0.0;
        double        m_elapsedBase     = 0.0;   // elapsed when the current rate took effect
        std::uint64_t m_elapsedBaseStep = 0;     // steps already run when the current rate took effect
        double        m_elapsedDt       = 0.0;   // the rate in effect (0 = none yet)

        // Frame begin (spec 2026-10-08 s7.2). Empty until a host installs one.
        // Rebind does not clear it.
        std::function<void(Astra::Registry&)> m_frameBegin;
    };
}
