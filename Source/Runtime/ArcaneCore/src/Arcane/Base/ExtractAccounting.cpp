#include <Arcane/Base/ExtractAccounting.hpp>
#include <atomic>
#include <cstdint>

namespace Arcane
{
    namespace
    {
        std::atomic<std::uint64_t> g_extractFrameCalls{ 0 };
    }

    void NoteExtractFrameCall() noexcept
    {
        g_extractFrameCalls.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t ExtractFrameCallCount() noexcept
    {
        return g_extractFrameCalls.load(std::memory_order_relaxed);
    }

    void ResetExtractFrameCallCount() noexcept
    {
        g_extractFrameCalls.store(0, std::memory_order_relaxed);
    }
}
