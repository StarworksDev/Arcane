#pragma once

// EXTRACT ACCOUNTING (F5 plan 1 Task 5, spec s4.1): a process-wide count of
// ExtractFrame calls (Arcane/Render/ExtractFrame.hpp). Every frame builder
// runs extract exactly once per frame; tests and the host gates read this
// counter to prove it. It lives in ArcaneCore rather than beside
// ExtractFrame so a host without a Client module (ArcaneServer) reaches it
// too. Relaxed atomics: a count, not a synchronisation point.

#include <Arcane/Core/Api.hpp>

#include <cstdint>

namespace Arcane
{
    // ExtractFrame's first statement.
    ARC_CORE_API void NoteExtractFrameCall() noexcept;
    // Calls since process start or the last ResetExtractFrameCallCount.
    ARC_CORE_API std::uint64_t ExtractFrameCallCount() noexcept;
    ARC_CORE_API void ResetExtractFrameCallCount() noexcept;
}
