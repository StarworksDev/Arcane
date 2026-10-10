#pragma once

// Arcane/Base/DiagnosticsInternal.hpp -- PRIVATE to ArcaneCore (never
// installed into the SDK include surface, never included by a host).
//
// The seam between the platform-neutral half of the crash/hang capture path
// (Base/Diagnostics.cpp: the public API, the watchdog's rules, the exit
// sentinel, the hand-written envelope JSON, the shared state) and a
// platform backend. Windows' backend still lives inline in Diagnostics.cpp,
// exactly as it did; the POSIX backend is its own TUs under Base/Posix/
// (Linux port, 2026-10-05), reached only through the Posix:: entry points
// declared at the bottom -- each one replaces what used to be a no-op
// `#else` stub in Diagnostics.cpp.
//
// Everything declared here is DEFINED in Diagnostics.cpp except the
// Posix:: block. Nothing here is exported (ArcaneCore builds with hidden
// visibility on ELF and these carry no ARC_CORE_API).

#include <Arcane/Base/CrashArena.hpp>
#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Platform/Platform.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace Arcane::Diagnostics::Internal
{
    // ---- fixed sizes (shared by every backend) ------------------------------
    ARC_CONSTANT("crash-path capacity: reason text bytes in the static crash arena")
    constexpr std::size_t kReasonMax   = 1024;
    ARC_CONSTANT("crash-path capacity: path bytes in the static crash arena")
    constexpr std::size_t kPathMax     = 1024;        // UTF-8 bytes, generous vs MAX_PATH / PATH_MAX
    ARC_CONSTANT("crash-path capacity: walked stack frames kept in the static crash arena")
    constexpr std::size_t kMaxFrames   = 96;
    ARC_CONSTANT("crash-path capacity: the walked thread's text in the static crash arena")
    constexpr std::size_t kSectionRsv  = 32 * 1024;   // the walked thread's text
    ARC_CONSTANT("crash-path capacity: the .txt header in the static crash arena")
    constexpr std::size_t kHeaderRsv   = 8 * 1024;    // the .txt header
    ARC_CONSTANT("crash-path capacity: one envelope's JSON in the static crash arena")
    constexpr std::size_t kEnvRsv      = 64 * 1024;   // one envelope's JSON
    ARC_CONSTANT("crash-path capacity: one lean envelope in the static crash arena")
    constexpr std::size_t kEnvLeanRsv  = 8 * 1024;    // ...with the unbounded fields elided
    ARC_CONSTANT("crash-path capacity: injected third-party modules named in one report")
    constexpr std::size_t kInjectedMax = 32;
    // Worst case 8 + 32 + (64 + 8) + (64 + 8) = 184 KiB of
    // CrashArena::kCapacity (256 KiB) -- both envelopes overrunning and
    // both retrying lean -- which still leaves headroom rather than
    // budgeting to the edge.

    // ---- shared state (Diagnostics.cpp) -------------------------------------
    extern Config g_cfg;

    extern std::atomic<std::int64_t> g_lastBeat;
    extern std::atomic<bool>         g_beatSeen;

    extern std::mutex  g_phaseMutex;
    extern std::string g_phase;

    extern std::atomic<std::uint32_t> g_reportCount;
    extern std::recursive_mutex        g_reportMutex;

    extern std::mutex         g_gpuProviderMutex;
    extern GpuSectionProvider g_gpuProvider;
    extern void*              g_gpuProviderUser;

    extern std::mutex        g_reportWrittenMutex;
    extern ReportWrittenHook g_reportWrittenHook;
    extern void*             g_reportWrittenUser;

    extern std::atomic<bool> g_watchdogStop;
    extern std::atomic<bool> g_watchdogPaused;
    extern std::atomic<std::uint32_t> g_watchdogThreadId;

    extern std::atomic<bool> g_exitedCleanly;
    extern std::atomic<int>  g_ctrlCPresses;
    extern std::atomic<bool> g_haveCleanExitHook;

    // The injected third-party module snapshot (R14): written by
    // SnapshotInjectedModules under g_injectedMutex, copied ONCE per report
    // into the g_rpt* mirror by CopyInjectedForReport (try-lock only).
    extern std::mutex  g_injectedMutex;
    extern bool        g_injectedScanned;
    extern char        g_injectedLine[2048];
    extern char        g_injectedNames[kInjectedMax][64];
    extern std::size_t g_injectedCount;
    extern char        g_rptInjectedLine[2048];
    extern char        g_rptInjectedNames[kInjectedMax][64];
    extern std::size_t g_rptInjectedCount;

    // ---- shared helpers (Diagnostics.cpp) -----------------------------------
    [[nodiscard]] std::int64_t NowTicks() noexcept;
    [[nodiscard]] double SecondsSince(std::int64_t ticks) noexcept;

    // Creates it. Off the crash path only (std::filesystem allocates).
    [[nodiscard]] std::filesystem::path ReportDir();

    // The heap-free kind rule (a static literal). See Diagnostics.cpp.
    [[nodiscard]] const char* DeriveKindCStr(const char* reason) noexcept;

    void CopyInto(char* dst, std::size_t cap, const char* src) noexcept;
    [[nodiscard]] std::string_view SV(const char* s) noexcept;

    // The per-report injected-module copy (crash thread, try-lock only).
    void CopyInjectedForReport() noexcept;

    // Everything one envelope carries (see BuildEnvelopeJson).
    struct EnvFields
    {
        const char*      guid           = nullptr;
        const char*      kind           = nullptr;
        const char*      reason         = nullptr;
        const char*      timestampUtc   = nullptr;
        const char*      appName        = nullptr;
        const char*      phase          = nullptr;
        const char*      buildInfo      = nullptr;
        std::string_view cpuThreadSummary;
        const char*      siblingTxt     = nullptr;
        const char*      siblingDmp     = nullptr;
        const char*      siblingGpuDump = nullptr;
        const char*      logPath        = nullptr;
        const char*      commandLine    = nullptr;
        int              exitCode       = 0;
        const Diag::Envelope* gpu       = nullptr;
    };

    struct EnvelopeJson
    {
        std::string_view text;
        bool             complete;
    };

    [[nodiscard]] EnvelopeJson BuildEnvelopeJson(CrashArena& arena, const EnvFields& f,
                                                 bool elide, std::size_t reserveBytes) noexcept;

    enum class EnvelopeWrite { Written, WrittenElided, NotWritten };

    [[nodiscard]] bool IsHangProtocolReport(const char* kind, int exitCode) noexcept;

#if ARC_PLATFORM_POSIX
    // ---- the POSIX backend (Base/Posix/DiagnosticsPosix.cpp) ----------------
    // Each is the body of what used to be a no-op `#else` in Diagnostics.cpp.
    namespace Posix
    {
        void TimeStampForFilename(char* out, std::size_t cap) noexcept;
        void TimestampUtcIso8601(char* out, std::size_t cap) noexcept;

        [[nodiscard]] std::uint32_t CurrentThreadId() noexcept;

        // The watchdog thread: started with `body` (Diagnostics.cpp's
        // WatchdogMain), stopped with a BOUNDED join that orphans a thread
        // parked mid-report, as the Windows half does.
        void StartWatchdog(void (*body)()) noexcept;
        void StopWatchdog() noexcept;
        void OnWatchdogThreadStart() noexcept;   // name + signal mask, on the watchdog thread
        [[nodiscard]] bool DebuggerAttached() noexcept;

        void Install();          // reads g_cfg (already copied)
        void Shutdown() noexcept;
        void RetargetDumpDir();  // under g_reportMutex, after g_cfg.dumpDir changed
        void SnapshotPhase(const char* phase) noexcept;
        [[nodiscard]] std::string LastReportStem();
        void SubmitReport(const ReportRequest& request) noexcept;
        void GuaranteeStackForThisThread() noexcept;
        [[nodiscard]] bool SimulateConsoleCtrl(unsigned long ctrlType) noexcept;
    }
#endif
}
