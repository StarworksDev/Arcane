// Crash window plan 1 (Task 3): ModuleTable implementation. See the header
// for the "why" -- this file is the double buffer: two fixed arrays, a
// mutex on the write side, an atomic index and a lock-free linear scan on
// the read side, plus the freeze flag that closes the two-flip torn-read
// window a double buffer alone cannot. Nothing here allocates from the heap
// or throws.

#include <Arcane/Base/ModuleTable.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace Arcane::Diagnostics
{
    namespace
    {
        // Two fixed-size buffers; g_active names which one readers see.
        // File-scope (not function-local statics) so Refresh can index them
        // directly under the write mutex without a static-init race on the
        // hot Find() path. g_counts is atomic (not just guarded by the
        // active/inactive split) so a Find() reading the count alongside a
        // concurrent Refresh() writing the OTHER slot's count is never a
        // plain-read/plain-write data race, independent of the freeze flag.
        ModuleEntry                  g_tables[2][ModuleTable::kMax];
        std::atomic<std::size_t>     g_counts[2] = { 0, 0 };
        std::atomic<int>             g_active{0};
        std::atomic<bool>            g_frozen{false};
        std::mutex                   g_writeMutex;
    }

    void ModuleTable::Refresh(std::span<const ForeignModules::LoadedModule> modules) noexcept
    {
        // Fast-path check before taking the lock (frozen is the steady
        // state during a whole crash report, so most calls in that window
        // should not contend for the mutex at all), then re-checked under
        // the lock so a SetFrozen(true) that lands exactly while this call
        // was blocked on the mutex still wins -- no write, no flip, ever,
        // once frozen is observed true.
        if (g_frozen.load(std::memory_order_acquire))
            return;

        std::lock_guard lock(g_writeMutex);

        if (g_frozen.load(std::memory_order_acquire))
            return;

        const int active   = g_active.load(std::memory_order_relaxed);
        const int inactive = 1 - active;

        const std::size_t count = modules.size() < kMax ? modules.size() : kMax;
        for (std::size_t i = 0; i < count; ++i)
        {
            ModuleEntry& e = g_tables[inactive][i];
            e.base = modules[i].base;
            e.size = modules[i].size;
            // Portable spelling of strncpy_s(..., _TRUNCATE) (MSVC CRT only):
            // copy at most sizeof-1 bytes and always NUL-terminate.
            const std::size_t n = std::min(modules[i].name.size(), sizeof(e.name) - 1);
            std::memcpy(e.name, modules[i].name.data(), n);
            e.name[n] = '\0';
        }
        g_counts[inactive].store(count, std::memory_order_release);

        // Publishes the new buffer: every write above is visible to any
        // Find() that observes this store (its load is memory_order_acquire).
        g_active.store(inactive, std::memory_order_release);
    }

    const ModuleEntry* ModuleTable::Find(std::uint64_t address) noexcept
    {
        const int active = g_active.load(std::memory_order_acquire);
        const std::size_t count = g_counts[active].load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i)
        {
            const ModuleEntry& e = g_tables[active][i];
            if (address >= e.base && address < e.base + e.size)
                return &e;
        }
        return nullptr;
    }

    std::size_t ModuleTable::Count() noexcept
    {
        const int active = g_active.load(std::memory_order_acquire);
        return g_counts[active].load(std::memory_order_acquire);
    }

    void ModuleTable::SetFrozen(bool frozen) noexcept
    {
        g_frozen.store(frozen, std::memory_order_release);
    }

    bool ModuleTable::Frozen() noexcept
    {
        return g_frozen.load(std::memory_order_acquire);
    }
}
