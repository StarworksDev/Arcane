// Fuzz-only stand-in for the one Diagnostics entry point the scene loader
// calls (Source/Runtime/ArcaneCore/src/Arcane/Base/Diagnostics.cpp is Windows-first on `main`
// and pulls in the crash/minidump machinery). The harness only needs the call
// to exist; what was published does not matter to it.

#include <Arcane/Base/Diagnostics.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace Arcane::Diagnostics
{
    void Publish(std::string_view, std::span<const Diagnostic>) {}

    // Assert.cpp (linked for the CVar registry) reports through these. A report is
    // a failed invariant, so SubmitReport aborts and libFuzzer records the input.
    void SubmitReport(const ReportRequest&) noexcept { std::abort(); }

    const char* FormatReason(const char* fmt, ...) noexcept
    {
        static thread_local char buf[512];
        std::va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, args);
        va_end(args);
        return buf;
    }
}
