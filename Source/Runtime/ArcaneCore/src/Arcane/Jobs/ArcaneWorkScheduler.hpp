#pragma once

// ArcaneWorkScheduler: bridges the engine's Arcane::ITaskExecutor to the shared
// Mosaic::IWorkScheduler data-parallel seam (Mosaic owns the canonical threading
// interface; Manifold2D consumes it, and Arcane supplies the enkiTS-backed
// implementation). The dependency arrow runs one way -- host (Arcane) -> Mosaic
// -- so this small forwarding copy lives on the Arcane side. It wraps a
// (nullable) Arcane::ITaskExecutor and forwards ParallelFor, translating the
// callback because the FunctionRef types differ across the two libraries.
//
// Null executor == the deterministic serial default: it runs the whole range
// inline as worker 0, byte-identical to Mosaic::SerialWorkScheduler and to
// PhysicsWorld's built-in serial fallback. This lets a consumer hand the world a
// single persistent adapter (whose backing executor may be null in a headless
// host) without a separate null branch at every call site.
//
// Consumed by: Scene/Arcane::PhysicsSystem2D (engine), the Sandbox physics-world wiring,
// and the physics-MT invariance tests (which drive Manifold2D with the engine's
// enki pool).

#include <Arcane/Jobs/TaskExecutor.hpp>

#include <Mosaic/Jobs/WorkScheduler.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Arcane
{
    class ArcaneWorkScheduler final : public Mosaic::IWorkScheduler
    {
    public:
        ArcaneWorkScheduler() noexcept = default;
        explicit ArcaneWorkScheduler(Arcane::ITaskExecutor* exec) noexcept : m_exec(exec) {}
        explicit ArcaneWorkScheduler(Arcane::ITaskExecutor& exec) noexcept : m_exec(&exec) {}

        // Re-point the backing executor (persistent-adapter pattern: one adapter
        // instance outlives many freshly-minted physics worlds).
        void SetExecutor(Arcane::ITaskExecutor* exec) noexcept { m_exec.store(exec, std::memory_order_release); }

        void ParallelFor(std::size_t count, std::size_t minBatch,
                         Mosaic::FunctionRef<void(std::size_t, std::size_t,
                                                  std::uint32_t)> fn) override
        {
            Arcane::ITaskExecutor* const exec = m_exec.load(std::memory_order_acquire);
            if (exec == nullptr)
            {
                // Serial fallback == Mosaic::SerialWorkScheduler: run the whole
                // range inline as worker 0. count==0 is a no-op.
                if (count != 0) fn(0, count, 0);
                return;
            }
            exec->ParallelFor(count, minBatch,
                [&](std::size_t b, std::size_t e, std::uint32_t w) { fn(b, e, w); });
        }

        std::uint32_t WorkerCount() const noexcept override
        {
            Arcane::ITaskExecutor* const exec = m_exec.load(std::memory_order_acquire);
            return exec ? exec->WorkerCount() : 1u;
        }

    private:
        std::atomic<Arcane::ITaskExecutor*> m_exec{nullptr};
    };
}
