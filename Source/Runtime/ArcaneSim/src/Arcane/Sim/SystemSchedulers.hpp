#pragma once

// The engine's phase layer over Astra's flat SystemScheduler. Astra has no
// stage/phase concept; the engine maintains one scheduler per phase and runs
// them in order. All share one (optional) enkiTS-backed executor.
//
// Header-only by design: the schedulers are a thin Astra wrapper and include no
// solver. AttachAllModules (ModuleAttach.hpp:5-15) is what puts every module
// image on one TypeContext.

#include <Astra/System/SystemScheduler.hpp>
#include <Astra/System/SystemExecutor.hpp>
#include <Astra/Core/WorkScheduler.hpp>

#include <memory>

namespace Arcane
{
    struct SystemSchedulers
    {
        Astra::SystemScheduler fixedUpdate;   // fixed-rate sim (60 Hz)
        Astra::SystemScheduler update;        // once per rendered frame (variable dt)
        // RETIRED (F5 plan 1 Task 5): no host runs it -- sprites are submitted by
        // the frame extract (Arcane/Render/ExtractFrame.hpp). Kept for the ABI; a
        // Render-phase registration lands here and never runs (SystemFactory warns).
        Astra::SystemScheduler render;
        Astra::ParallelExecutor executor;     // enkiTS-backed; null scheduler -> sequential

        // sched may be null (sequential inline execution -- fine for tests/headless).
        explicit SystemSchedulers(std::shared_ptr<Mosaic::IWorkScheduler> sched)
            : executor(std::move(sched)) {}
    };
}
