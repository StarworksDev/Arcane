#pragma once
// FramePerf: opt-in per-phase frame timing for a runtime host's frame loop.
// Mirrors the prior inline acc* accumulators + the [PERF] dump, lifted out of
// main. A no-op unless diagnostics.perfLog is on (--perf sets it; settings arc
// S6-2). Both settings are Live: FrameStart reads them once per frame, so the
// console can toggle the log or change diagnostics.perfLogIntervalFrames
// mid-session. Header-only. PRESENTATION-FREE + C++23-clean.
#include <chrono>
#include <cstdint>
#include <Arcane/Base/DiagnosticsSettings.hpp>
#include <Arcane/Base/Log.hpp>
namespace Arcane
{
    class FramePerf
    {
    public:
        using Clock = std::chrono::steady_clock;

        // On() is stable from one FrameStart to the next.
        [[nodiscard]] bool On() const noexcept { return m_on; }
        [[nodiscard]] Clock::time_point Now() const { return Clock::now(); }
        // ms between two stamps (caller takes stamps only when On()).
        [[nodiscard]] static double Ms(Clock::time_point a, Clock::time_point b)
        { return std::chrono::duration<double, std::milli>(b - a).count(); }

        // Call first thing each frame. Picks up the settings; switching the log
        // off drops the partial window, so switching it back on starts clean.
        void FrameStart()
        {
            const DiagnosticsSettings& s = Settings<DiagnosticsSettings>();
            if (m_on && !s.perfLog) Reset();
            m_on = s.perfLog;
            m_interval = s.perfLogIntervalFrames > 0 ? s.perfLogIntervalFrames : 1u;
            if (m_on) m_frameStart = Clock::now();
        }
        void Add(double& acc, Clock::time_point a, Clock::time_point b) const { if (m_on) acc += Ms(a, b); }

        // Accumulators (public for the loop to add into; only touched when On()).
        // accRec times the frame extract (ExtractFrame, F5 plan 1 Task 5) and
        // prints as "extract"; it timed the sprite submission before that.
        double accFrame=0, accSim=0, accRec=0, accEnd=0, accTone=0, accImgui=0, accPresent=0, accPoll=0;

        // Call once per frame end with the frame's batcher stats. Emits + resets
        // every diagnostics.perfLogIntervalFrames frames.
        void Tick(std::uint32_t quads, std::uint32_t draws)
        {
            if (!m_on) return;
            accFrame += Ms(m_frameStart, Clock::now());
            if (++m_frames < m_interval) return;
            ARC_INFO("[PERF] {:.2f} ms ({:.1f} FPS) | sim {:.2f} extract {:.2f} end {:.2f} "
                     "tone {:.2f} imgui {:.2f} present {:.2f} poll {:.2f} | quads {} draws {}",
                     accFrame/m_frames, 1000.0*m_frames/accFrame, accSim/m_frames, accRec/m_frames,
                     accEnd/m_frames, accTone/m_frames, accImgui/m_frames, accPresent/m_frames,
                     accPoll/m_frames, quads, draws);
            Reset();
        }
    private:
        void Reset() { accFrame=accSim=accRec=accEnd=accTone=accImgui=accPresent=accPoll=0; m_frames=0; }

        bool m_on = false;
        std::uint64_t m_frames = 0;
        std::uint64_t m_interval = Detail::DiagnosticsDefaults().perfLogIntervalFrames;
        // Per-frame start stamp: only FrameStart()/Tick() read/write it, so it lives
        // private (the loop touches the public acc* accumulators, never this directly).
        Clock::time_point m_frameStart{};
    };
}
