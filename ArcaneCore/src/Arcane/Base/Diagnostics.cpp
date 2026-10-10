#include <Arcane/Base/Diagnostics.hpp>

#include <Arcane/Base/CrashArena.hpp>     // the ONLY allocator the crash thread may use (spec S5.5)
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Base/DiagEnvelope.hpp>   // Diag::Envelope -- the GPU provider's own field carrier
#include <Arcane/Base/DiagnosticsInternal.hpp>   // the seam to the POSIX backend (Base/Posix/)
#include <Arcane/Base/Engine.hpp>         // ExecutablePathUtf8(), BuildInfo()
#include <Arcane/Base/ForeignModules.hpp> // ForeignModules::LastScan -- snapshotted OFF the crash path (R14)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ModuleTable.hpp>    // address -> module+offset, lock-free (Task 3)
#include <Arcane/Base/PortableStack.hpp>  // RtlVirtualUnwind walk -- no DbgHelp anywhere below
#include <Arcane/Base/DiagnosticsSettings.hpp>     // Settings<DiagnosticsSettings>() -- the reporter args, off the crash path
#include <Arcane/Base/ReporterSettings.hpp>        // diagnostics.reporter.* (S6-4) -- likewise
#include <Arcane/Config/Bindings/LogBinding.hpp>   // log.dir (ConfigFromSettings) -- never read on the crash path
#include <Arcane/Config/UiSettings.hpp>            // ui.copyFlashSeconds (S6-4) -- likewise
#include <Arcane/Platform/LaunchPath.hpp>          // CheckLaunchPath -- the configured reporter, at Install (S7-SEC)

#include <Json.hpp>                        // the session record (plan 2, task 9) -- written OFF the crash path only

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>       // signal(SIGABRT) -- a third party's abort()
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>       // _set_abort_behavior, _set_invalid_parameter_handler, _set_purecall_handler
#include <cstring>
#include <cwchar>       // std::swprintf -- ReporterSettingsArgs
#include <exception>     // set_terminate, current_exception, rethrow_exception
#include <iterator>
#include <new>           // std::bad_alloc -- the terminate handler's OOM arm
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
// dbghelp is still needed for MiniDumpWriteDump -- and ONLY for that. Every
// Sym* call is gone from this file: DbgHelp takes the process symbol-handler
// lock and may load symbols from disk, which a thread that just faulted can
// already be holding locks against (spec S5.2 step 2). The reporter
// symbolizes the minidump out of process instead.
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <crtdbg.h>   // _CrtSetReportMode/_CrtSetReportFile -- the Debug CRT's box, redirected to stderr
#endif

// The platform-neutral state and helpers live in Internal (declared in
// DiagnosticsInternal.hpp) so the POSIX backend's TUs share them; the Win32
// backend below keeps its own state in the anonymous namespace as before.
namespace Arcane::Diagnostics::Internal
{
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] std::int64_t NowTicks() noexcept
    {
        return Clock::now().time_since_epoch().count();
    }

    [[nodiscard]] double SecondsSince(std::int64_t ticks) noexcept
    {
        const Clock::duration d{NowTicks() - ticks};
        return std::chrono::duration<double>(d).count();
    }

    // Monotone seconds in an arbitrary epoch -- the clock ProgressStallRule is
    // polled with. Its epoch is irrelevant; only differences are used.
    [[nodiscard]] double NowSeconds() noexcept
    {
        return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    }

    // ---- shared state -----------------------------------------------------
    // Deliberately file-static rather than a class: the crash filter is a raw
    // C callback and the watchdog outlives any owner we could hand it.

    Config             g_cfg;
    std::atomic<bool>  g_installed{false};

    // Main-thread liveness. g_beatSeen gates the WHOLE hang trigger: a host
    // that never calls Heartbeat() (a headless tool, a test) must get silence,
    // not a spurious report hangSeconds after boot.
    std::atomic<std::int64_t> g_lastBeat{0};
    std::atomic<bool>         g_beatSeen{false};

    // GPU-side progress (Task 7). Same arming discipline as the pair above and
    // for the same reason: a host that never renders publishes nothing here and
    // must get silence. The VALUE is what matters (a monotone count of GPU sync
    // points passed) -- its staleness, not its age, is the signal, so unlike
    // g_lastBeat this is not a timestamp.
    std::atomic<std::uint64_t> g_gpuFence{0};
    std::atomic<bool>          g_gpuBeatSeen{false};
    std::atomic<std::int64_t>  g_lastGpuBeat{0};

    // How stale the last GpuHeartbeat CALL may be before the GPU rule stops
    // evaluating. A frozen counter means "the GPU stopped" only while somebody
    // is still publishing it; a minimized host renders no frames at all, and its
    // frozen counter must read as "no data", not as a stall. Two seconds is ~120
    // frames of grace at 60Hz.
    ARC_CONSTANT("a change would be a bug: the GPU freshness gate must trip before the stall rule; the window is DERIVED, min(2 s, diagnostics.gpuStallSeconds / 2)")
    constexpr double kGpuBeatFreshnessCapSeconds = 2.0;

    // The gate has to trip BEFORE the stall threshold could, or a host that
    // published once and then stopped rendering would be reported as stalled in
    // the window between the two. Deriving the window from the configured
    // threshold makes that structural for ANY gpuStallSeconds instead of an
    // accident of the shipped default (which is 8, so this returns the 2s cap).
    [[nodiscard]] double GpuBeatFreshnessSeconds(double stallSeconds) noexcept
    {
        return std::min(kGpuBeatFreshnessCapSeconds, stallSeconds * 0.5);
    }

    std::mutex  g_phaseMutex;
    std::string g_phase;

    std::atomic<std::uint32_t> g_reportCount{0};

    // Serializes report writing. Two triggers can race (the watchdog fires while
    // the main thread faults); interleaved reports are worse than a late one.
    //
    // RECURSIVE on purpose: a fault inside the GPU-section provider re-enters
    // SubmitReport ON THE CRASH THREAD, which then runs the nested report
    // directly (spec S9, "crash inside the crash path") while the outer
    // report still holds this lock. A plain std::mutex would self-deadlock
    // there and the 60 s wait would be the only thing left to save the
    // process. FenceReports() is unaffected: it is called from OTHER threads
    // (a device destructor), where a recursive mutex blocks exactly like a
    // plain one.
    std::recursive_mutex g_reportMutex;

    // GPU-section provider slot (Task 5/6 install their backend here). A
    // separate mutex from g_reportMutex: the report only holds this one
    // long enough to copy the two pointers out, then calls the provider
    // unlocked -- an unknown callback must never run while holding a lock
    // another thread might need in order to install/clear it.
    std::mutex         g_gpuProviderMutex;
    GpuSectionProvider g_gpuProvider     = nullptr;
    void*              g_gpuProviderUser = nullptr;

    // Report-written hook slot (Task 9). Same shape/rationale as the
    // GPU-section provider slot immediately above: a separate mutex from
    // g_reportMutex, held only long enough to copy the pointer pair out,
    // then the hook runs unlocked -- an unknown callback must never run
    // while holding a lock another thread might need to install/clear it.
    std::mutex        g_reportWrittenMutex;
    ReportWrittenHook g_reportWrittenHook = nullptr;
    void*             g_reportWrittenUser = nullptr;

    std::atomic<bool> g_watchdogStop{false};

    // ---- exit sentinel + clean-exit hook (task 8, spec S5.7) --------------

    // "The host has been asked to quit." From here until Shutdown() the
    // watchdog's beat rules are replaced by the deadline below.
    std::atomic<bool>         g_exitRequested{false};
    std::atomic<std::int64_t> g_exitRequestedAt{0};

    // "The host finished its exit." Set by Shutdown() and by the atexit hook;
    // the only reader is the console CLOSE arm, which waits on it inside the
    // OS's budget before letting Windows terminate us.
    //
    // An atomic polled in short steps rather than a Win32 event, deliberately:
    // the waiter is the OS's console-handler thread and the setter is the main
    // thread mid-teardown, so an event handle would have to be created once
    // and never closed (a wait on a handle another thread just closed is
    // undefined). One bool and a 25 ms poll have neither problem and are
    // indistinguishable at this timescale.
    std::atomic<bool> g_exitedCleanly{false};

    // Ctrl-C is two-step (UE's rule): the first press requests the clean exit,
    // the second gives up on it and terminates. Reset per arming.
    std::atomic<int> g_ctrlCPresses{0};

    // The host's clean-exit callback. Same slot shape (and same reasoning) as
    // the report-written hook above: raw pointer pair, its own mutex, held
    // only long enough to copy the pair out.
    std::mutex    g_cleanExitMutex;
    CleanExitHook g_cleanExitHook = nullptr;
    void*         g_cleanExitUser = nullptr;

    // "...and a host actually installed one." Mirrors the slot above so the
    // console handler can ask that question WITHOUT taking g_cleanExitMutex:
    // it runs on an OS-created thread, on a deadline, while the main thread
    // may be anywhere -- including inside SetCleanExitHook.
    std::atomic<bool> g_haveCleanExitHook{false};

    // The crash thread raises this for the lifetime of a report so no hang
    // rule can interleave a second report with the one being written (spec
    // S5.2 step 1, UE stops its heartbeat first). Task 8 is what makes the
    // watchdog loop READ it; a survivable report lowers it again on the way
    // out so a later stall is still reported.
    std::atomic<bool> g_watchdogPaused{false};

    // The watchdog's own thread id, published when WatchdogMain starts. Read
    // by SubmitReport to decide WHICH thread the report walks: a hang is
    // about the main thread, not about the watchdog that noticed it (R6).
    std::atomic<std::uint32_t> g_watchdogThreadId{0};

    // The watchdog runs on a RAW thread (task 8), not a std::thread. The
    // reason is the exit sentinel: the thread has to be stoppable from an
    // atexit hook, and a std::thread that is still joinable when its
    // destructor runs calls std::terminate -- which is exactly what a main()
    // that returned without Shutdown() used to do (see the death fixture's
    // `ensure` comment). A HANDLE has no destructor and no such opinion.
#if defined(_WIN32)
    HANDLE g_watchdogThread = nullptr;
#endif

    // "The render layer has already CONFIRMED the GPU device is gone."
    // Set once by Render's NoteGpuDeviceLost (GpuInstrumentation.cpp); the
    // only reader is OnUnhandledException's fail-fast branch below. Never
    // cleared, on purpose: after a confirmed loss, everything the process
    // does with that device is post-mortem, and a re-armed device gets a
    // fresh process on the paths that matter (the hosts quit on the latch).
    std::atomic<bool> g_gpuDeviceLost{false};

    // kReasonMax (DiagnosticsInternal.hpp) is platform-neutral: FormatReason
    // sizes its per-thread buffer with it.

    // The injected third-party modules (R14): one rendered line for the .txt
    // and the base names for the envelope array. Guarded by its own mutex,
    // which the crash thread only ever TRY-locks.
    std::mutex  g_injectedMutex;
    bool        g_injectedScanned = false;
    char        g_injectedLine[2048]{};
    char        g_injectedNames[kInjectedMax][64]{};
    std::size_t g_injectedCount = 0;
}   // namespace Arcane::Diagnostics::Internal

namespace Arcane::Diagnostics
{
namespace
{
    using namespace Internal;

#if defined(_WIN32)
    DWORD  g_mainThreadId = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

    // A FATAL report has claimed the process. Latched, never cleared: every
    // later fatal submitter waits on the crash thread's event and terminates
    // instead of queuing a second report nobody will read (spec S9, "second
    // faulting thread while a report is in flight").
    std::atomic<bool> g_inCrashHandler{false};

    // ---- crash thread -----------------------------------------------------

    // kPathMax, kMaxFrames and the arena reserves: DiagnosticsInternal.hpp
    // (ARC_CONSTANT lives on those declarations).

    // The one request in flight. Written by the SUBMITTING thread before it
    // signals, read by the crash thread after; the event pair is the
    // happens-before edge, and g_submitMutex is what keeps it single.
    struct Pending
    {
        char   reason[kReasonMax];
        EXCEPTION_POINTERS* ep;
        DWORD  walkThreadId;
        bool   lightweight;
        int    exitCode;
    };
    Pending g_pending{};

    HANDLE            g_crashEvent   = nullptr;   // submitter -> crash thread
    HANDLE            g_handledEvent = nullptr;   // crash thread -> submitter
    HANDLE            g_crashThread  = nullptr;
    std::atomic<std::uint32_t> g_crashThreadId{0};
    std::atomic<bool> g_crashThreadStop{false};

    // Serializes SUBMISSION (not the report body -- that is g_reportMutex).
    // timed_ so a submitter that arrives while another report is in flight
    // waits BOUNDEDLY and then does the right thing for its kind, instead of
    // parking forever behind a crash thread that may itself be wedged.
    std::timed_mutex g_submitMutex;

    // Written by the crash thread once every file exists; read by
    // LastReportStem (off the crash path). The release/acquire pair is the
    // whole synchronisation -- the bytes never change after the store.
    char              g_lastStem[kPathMax]{};
    std::atomic<bool> g_lastStemValid{false};

    // ---- snapshots taken OFF the crash path -------------------------------
    // Everything the report needs as text, copied into fixed storage at
    // Install()/SetPhase()/RetargetDumpDir()/Scan(). The crash thread reads
    // only these: std::string, std::filesystem::path and Log::FileSinkPath()
    // all allocate, and the heap may be exactly what faulted.

    char g_reportDirSnap[kPathMax]{};
    char g_appNameSnap[128]{};
    char g_productSnap[128]{};
    char g_logPathSnap[kPathMax]{};
    char g_commandLineSnap[4096]{};
    char g_phaseSnap[256]{};

    // The injected-module snapshot lives in Internal (shared with POSIX).

    // The reporter hand-off, prepared at Install: CreateProcessW needs a
    // WRITABLE command line, so the prefix lives here and the per-report
    // suffix is appended into g_spawnCmd on the crash thread.
    wchar_t g_reporterExe[kPathMax]{};
    wchar_t g_productWide[128]{};
    wchar_t g_spawnCmd[8192]{};
    // The reporter's settings flags (S6-4; ReporterSettingsArgs), formatted
    // from the published settings at Install, at RetargetDumpDir and when a
    // Live one publishes -- before the crash thread exists or under
    // g_reportMutex, which it holds for a whole report. SpawnReporter appends
    // the CURRENT buffer, so a Live change reaches the next crash reporter;
    // the monitor (LaunchMonitor) was handed the buffer as it stood at its
    // launch and keeps that tail for its lifetime -- it is not restarted for a
    // settings change. The reporter has no registry of its own.
    wchar_t g_reporterSettingsArgs[512]{};

    // The hang protocol (plan 2, D11/D12/D7). The event is created at Install
    // so the reporter can OpenEventW it by name; the reporter handle is kept
    // for hang/gpu-stall spawns only (R95); the creation time rides on every spawn so
    // the reporter never terminates a pid that was recycled (spec s9).
    //
    // Threads: g_reporterProcess is touched only by the crash thread
    // (SpawnReporter) and by Shutdown AFTER that thread is joined;
    // g_recoveredEvent is reset by the crash thread and set by the watchdog,
    // both of which Shutdown stops before it closes the handle.
    wchar_t            g_recoveredName[64]{};
    HANDLE             g_recoveredEvent  = nullptr;
    HANDLE             g_reporterProcess = nullptr;
    unsigned long long g_hostCreated     = 0;

    // Monitor mode (plan 2, task 9; spec S5.8, D15). The session record's path
    // is fixed ONCE per process -- at the first Install that launches a
    // monitor -- because the monitor was handed that path on its command
    // line: RetargetDumpDir rewrites the record's CONTENTS in place (so they
    // name the project's report dir, D8) but never moves the file, or a
    // monitor still watching the old path would read "deleted" -- a clean
    // exit -- for every death after a project opened.
    //
    // `g_sessionRecordLive` is the ONE gate on deletion: exchange(false) then
    // DeleteFileW, so the paths that delete it (Shutdown, the atexit hook,
    // the console handler's thread) each do so at most once, on a static wide
    // path, with no heap and no lock -- the atexit hook runs during
    // DLL_PROCESS_DETACH, where a lock another (already-terminated) thread
    // held would never be released.
    wchar_t           g_sessionPathWide[kPathMax]{};
    std::atomic<bool> g_sessionRecordLive{false};
    bool              g_monitorLaunched = false;   // one monitor per PROCESS, whatever Install/Shutdown cycles follow

    // Deletes the session record -- "this process recorded its clean exit".
    // Heap-free and lock-free (see above); safe when none was ever written.
    void DeleteSessionRecord() noexcept
    {
        if (g_sessionRecordLive.exchange(false, std::memory_order_acq_rel))
            DeleteFileW(g_sessionPathWide);
    }

    // Seed for the per-report envelope guid. Guid::Generate() draws from the
    // Core CSPRNG (a lock, and possibly the heap), so it runs ONCE here and
    // the crash thread mixes it with the clock + the report counter instead.
    Guid g_guidSeed{};

    // Crash-thread scratch that must not live on a 256 KiB stack.
    StackFrame g_frames[kMaxFrames]{};
    wchar_t    g_wideScratch[kPathMax]{};

    // Crash-thread scratch (crash window plan 2, task 5): the walked thread's
    // captured context, kept so the minidump of a HANG or MANUAL report still
    // carries an EXCEPTION stream.
    //
    // Without one, MiniDumpWriteDump is handed a null MINIDUMP_EXCEPTION_
    // INFORMATION, the dump has no stored event, and dbgeng's
    // GetStoredEventInformation fails -- so the reporter has no way to know
    // which thread to walk and lands on thread 0. UE never hits that because
    // it builds a synthetic EXCEPTION_POINTERS around a SUSPENDED thread's
    // context, with ExceptionCode = STILL_ACTIVE (WindowsPlatformCrashContext
    // .cpp:1925-1966): "not a fault, a snapshot".
    //
    // Fixed storage and file scope for the same reason as g_frames: this is
    // written on the crash thread, under g_reportMutex, and must not sit on
    // its 256 KiB stack.
    CONTEXT          g_walkedContext{};
    EXCEPTION_RECORD g_walkedRecord{};
    bool             g_walkedContextValid = false;
#endif
}   // namespace

namespace Internal
{
    // The host exe's folder (the default report dir's parent), or "." when
    // the exe path is unknown. ReportDirFor (S7-SEC) refuses a dumpDir that
    // could break the reporter command line and falls back to this folder.
    [[nodiscard]] std::filesystem::path HostExeDir()
    {
        const std::string exe = ExecutablePathUtf8();
        return exe.empty() ? std::filesystem::path(".") : std::filesystem::path(exe).parent_path();
    }

    [[nodiscard]] std::filesystem::path ReportDir()
    {
        // Beside the exe unless configured, never the CWD: the hosts are
        // documented as cd-then-run, so a CWD-relative report is a report
        // nobody finds. A configured dir that could break the reporter's
        // command line falls back the same way (ReportDirFor, S7-SEC).
        const std::filesystem::path dir = ReportDirFor(g_cfg.dumpDir, HostExeDir());
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // Install's and RetargetDumpDir's warning for a dumpDir ReportDirFor
    // refused (S7-SEC fix round 1). Off the crash path.
    void WarnIfDumpDirRefused()
    {
        std::string refusal;
        (void)ReportDirFor(g_cfg.dumpDir, HostExeDir(), &refusal);
        if (!refusal.empty())
            ARC_WARN("Diagnostics: {}", refusal);
    }

    // Both stamp helpers write into a caller-supplied buffer: neither may
    // allocate, because both are called from the crash thread.
    void TimeStampForFilename(char* out, std::size_t cap) noexcept
    {
#if defined(_WIN32)
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::snprintf(out, cap, "%04u%02u%02u-%02u%02u%02u",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
        Posix::TimeStampForFilename(out, cap);
#endif
    }

    // ISO-8601 UTC, for the .arcdiag envelope's timestampUtc -- distinct
    // from TimeStampForFilename's local-time, filename-safe stamp above. A
    // report a teammate opens in another timezone needs an unambiguous
    // instant, not the reporter's local clock.
    void TimestampUtcIso8601(char* out, std::size_t cap) noexcept
    {
#if defined(_WIN32)
        SYSTEMTIME st{};
        GetSystemTime(&st);
        std::snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
        Posix::TimestampUtcIso8601(out, cap);
#endif
    }

    // The kind vocabulary, most specific first. Static literals: what
    // DeriveKindCStr hands back has to outlive the report.
    constexpr const char* kReportKinds[] = {
        "gpu-stall", "gpu-crash", "assert", "ensure", "terminate",
        "out-of-memory", "abnormal-exit", "hang", "crash",
    };

    // Kind derivation, HEAP-FREE: the exported DeriveReportKind returns a
    // std::string for callers' convenience, but the crash thread needs the
    // same decision without an allocation, so the rule itself lives here and
    // hands back a static literal. One rule, two skins -- never two rules.
    //
    // PREFIX FIRST (final review, finding I3). Every reason this engine
    // writes is "<kind>: <detail>", and the detail is attacker-ish text we do
    // not control: a stringized ARC_ENSURE condition, a what() string, a
    // source path. Classifying by substring let that detail vote, so
    // "ensure: gpu != nullptr (X.cpp:1)" filed as a gpu-crash and
    // "ensure: !asserted (X.cpp:1)" as an assert. So: take what stands before
    // the first ':' or space, and if it IS one of the kinds, that is the
    // kind, full stop. Only when the prefix is not a kind do we fall back to
    // the old substring rule, which is what still classifies the free-form
    // legacy wordings ("crash (unhandled exception)", "hang (main thread has
    // not ticked for 12.0s)", "hang at exit (...)").
    [[nodiscard]] const char* DeriveKindCStr(const char* reason) noexcept
    {
        const std::string_view r = reason ? reason : "";

        const std::size_t cut = r.find_first_of(": ");
        const std::string_view prefix = (cut == std::string_view::npos) ? r : r.substr(0, cut);
        for (const char* kind : kReportKinds)
            if (prefix == kind)
                return kind;

        if (r.find("gpu") != std::string_view::npos)
            return r.find("stall") != std::string_view::npos ? "gpu-stall" : "gpu-crash";
        if (r.find("assert") != std::string_view::npos)        return "assert";
        if (r.find("terminate") != std::string_view::npos)     return "terminate";
        if (r.find("ensure") != std::string_view::npos)        return "ensure";
        if (r.find("out-of-memory") != std::string_view::npos) return "out-of-memory";
        if (r.find("abnormal-exit") != std::string_view::npos) return "abnormal-exit";
        if (r.find("hang") != std::string_view::npos)          return "hang";
        return "crash";
    }

    // ---- fixed-storage snapshot helpers (all OFF the crash path) ----------

    void CopyInto(char* dst, std::size_t cap, const char* src) noexcept
    {
        if (cap == 0) return;
        if (!src) { dst[0] = '\0'; return; }
        std::size_t n = 0;
        while (n + 1 < cap && src[n] != '\0') { dst[n] = src[n]; ++n; }
        dst[n] = '\0';
    }

    // ---- hand-written envelope JSON ---------------------------------------
    // nlohmann allocates on every node, so the envelope the crash thread
    // writes is built by hand through the arena. Diag::Parse (lenient, and
    // the reporter's only reader) accepts exactly these keys -- keep this
    // in step with DiagEnvelope.cpp's Serialize. (EnvFields, EnvelopeJson
    // and EnvelopeWrite are declared in DiagnosticsInternal.hpp.)

    [[nodiscard]] std::string_view SV(const char* s) noexcept
    {
        return s ? std::string_view(s) : std::string_view{};
    }

    // Length of the valid UTF-8 sequence at `p`, or 0 when the bytes are not
    // valid UTF-8. Needed because nlohmann's parser REJECTS a string with an
    // invalid sequence in it -- one bad byte anywhere would make the whole
    // envelope unreadable, so those bytes are replaced instead.
    [[nodiscard]] std::size_t Utf8SeqLen(const unsigned char* p, std::size_t avail) noexcept
    {
        const unsigned char c = p[0];
        if (c < 0x80) return 1;

        std::size_t need = 0;
        if      ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else return 0;

        if (need > avail) return 0;
        for (std::size_t i = 1; i < need; ++i)
            if ((p[i] & 0xC0) != 0x80) return 0;
        if (need == 2 && c < 0xC2) return 0;   // overlong
        if (need == 4 && c > 0xF4) return 0;   // beyond U+10FFFF
        return need;
    }

    void AppendJsonString(CrashArena::Builder& b, std::string_view s) noexcept
    {
        b.Append("\"");

        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        const std::size_t n = s.size();
        std::size_t i = 0;
        std::size_t runStart = 0;
        const auto flush = [&](std::size_t end) noexcept {
            if (end > runStart) b.Append(std::string_view(s.data() + runStart, end - runStart));
        };

        while (i < n)
        {
            const unsigned char c = p[i];
            if (c == '"' || c == '\\')
            {
                flush(i);
                const char esc[2] = { '\\', static_cast<char>(c) };
                b.Append(std::string_view(esc, 2));
                runStart = ++i;
                continue;
            }
            if (c < 0x20)
            {
                flush(i);
                switch (c)
                {
                    case '\n': b.Append("\\n"); break;
                    case '\r': b.Append("\\r"); break;
                    case '\t': b.Append("\\t"); break;
                    case '\b': b.Append("\\b"); break;
                    case '\f': b.Append("\\f"); break;
                    default:
                    {
                        char u[8];
                        std::snprintf(u, sizeof(u), "\\u%04x", static_cast<unsigned>(c));
                        b.Append(u);
                        break;
                    }
                }
                runStart = ++i;
                continue;
            }
            if (c < 0x80) { ++i; continue; }

            const std::size_t seq = Utf8SeqLen(p + i, n - i);
            if (seq == 0)
            {
                flush(i);
                b.Append("?");
                runStart = ++i;
                continue;
            }
            i += seq;
        }
        flush(n);

        b.Append("\"");
    }


    // The injected-module snapshot, copied once per report so every step of
    // it reads the same list even if a Scan() lands mid-report (R14).
    char        g_rptInjectedLine[sizeof(g_injectedLine)]{};
    char        g_rptInjectedNames[kInjectedMax][64]{};
    std::size_t g_rptInjectedCount = 0;

    // What BuildEnvelopeJson produced. `complete` is FALSE when the JSON did
    // not fit its arena reserve -- Builder::Append copies min(remaining,
    // size) and only flags the arena, so an overrun is SILENTLY TRUNCATED,
    // syntactically invalid JSON. A caller that writes that over a good
    // envelope destroys the only parseable report on disk, so the flag is
    // load-bearing, not advisory. Detected as "not one spare byte left":
    // cursor == end means Append ran out (and a failed carve gives
    // begin == cursor == end, which reads the same way). A body that fills
    // the reserve to the last byte exactly is treated as an overrun too --
    // conservative in the safe direction.

    // `elide` drops the two unbounded fields -- the stack text and
    // everything the GPU provider supplies -- so a LEAN envelope stays valid
    // JSON in a few hundred bytes. The stack is not lost: it is in the .txt
    // sibling this envelope names.
    [[nodiscard]] EnvelopeJson BuildEnvelopeJson(CrashArena& arena, const EnvFields& f,
                                                 bool elide, std::size_t reserveBytes) noexcept
    {
        CrashArena::Builder b = arena.OpenBuilder(reserveBytes);
        const Diag::Envelope* const gpu = elide ? nullptr : f.gpu;

        b.Append("{\n  \"formatVersion\": 1,\n  \"guid\": ");
        AppendJsonString(b, SV(f.guid));
        b.Append(",\n  \"kind\": ");             AppendJsonString(b, SV(f.kind));
        b.Append(",\n  \"reason\": ");           AppendJsonString(b, SV(f.reason));
        b.Append(",\n  \"timestampUtc\": ");     AppendJsonString(b, SV(f.timestampUtc));
        b.Append(",\n  \"appName\": ");          AppendJsonString(b, SV(f.appName));
        b.Append(",\n  \"phase\": ");            AppendJsonString(b, SV(f.phase));
        b.Append(",\n  \"buildInfo\": ");        AppendJsonString(b, SV(f.buildInfo));
        b.Append(",\n  \"cpuThreadSummary\": ");
        AppendJsonString(b, elide
            ? std::string_view("<elided: the crash arena could not hold this report's stack text; "
                               "see the .txt sibling named in siblingTxt>")
            : f.cpuThreadSummary);

        b.Append(",\n  \"queues\": [");
        if (gpu)
        {
            bool firstQueue = true;
            for (const Diag::Envelope::Queue& q : gpu->queues)
            {
                b.Append(firstQueue ? "\n    {\"name\": " : ",\n    {\"name\": ");
                firstQueue = false;
                AppendJsonString(b, q.name);
                b.Append(", \"lastCompleted\": ");
                AppendJsonString(b, q.lastCompleted);
                b.Append(", \"inFlight\": [");
                bool firstFlight = true;
                for (const std::string& s : q.inFlight)
                {
                    if (!firstFlight) b.Append(", ");
                    firstFlight = false;
                    AppendJsonString(b, s);
                }
                b.Append("]}");
            }
            if (!firstQueue) b.Append("\n  ");
        }
        b.Append("]");

        // The fault block is three short strings -- kept even when eliding,
        // because it is the classification a reader acts on.
        b.Append(",\n  \"fault\": {\"type\": ");
        AppendJsonString(b, f.gpu ? std::string_view(f.gpu->fault.type) : std::string_view{});
        b.Append(", \"address\": ");
        AppendJsonString(b, f.gpu ? std::string_view(f.gpu->fault.address) : std::string_view{});
        b.Append(", \"resource\": ");
        AppendJsonString(b, f.gpu ? std::string_view(f.gpu->fault.resource) : std::string_view{});
        b.Append("}");

        b.Append(",\n  \"siblingTxt\": ");     AppendJsonString(b, SV(f.siblingTxt));
        b.Append(",\n  \"siblingDmp\": ");     AppendJsonString(b, SV(f.siblingDmp));
        b.Append(",\n  \"siblingGpuDump\": "); AppendJsonString(b, SV(f.siblingGpuDump));

        b.Append(",\n  \"activeLayers\": [");
        if (gpu)
        {
            bool first = true;
            for (const std::string& s : gpu->activeLayers)
            {
                if (!first) b.Append(", ");
                first = false;
                AppendJsonString(b, s);
            }
        }
        b.Append("]");

        // Base names only -- the envelope's contract (DiagEnvelope.hpp). The
        // .txt sibling's `injected` line is what tells "none" and "not
        // scanned" apart.
        b.Append(",\n  \"foreignModules\": [");
        for (std::size_t i = 0; i < (elide ? 0u : g_rptInjectedCount); ++i)
        {
            if (i != 0) b.Append(", ");
            AppendJsonString(b, SV(g_rptInjectedNames[i]));
        }
        b.Append("]");

        b.Append(",\n  \"logPath\": ");     AppendJsonString(b, SV(f.logPath));
        b.Append(",\n  \"commandLine\": "); AppendJsonString(b, SV(f.commandLine));

        char exitBuf[32];
        std::snprintf(exitBuf, sizeof(exitBuf), ",\n  \"exitCode\": %d", f.exitCode);
        b.Append(exitBuf);

        b.Append("\n}\n");
        return { b.View(), b.cursor < b.end };
    }



    // One consistent injected-module list for the whole report. TRY-lock
    // only: a report must never block on a scan that is running, and a
    // missing decoration is not worth a wedged crash path.
    void CopyInjectedForReport() noexcept
    {
        std::unique_lock<std::mutex> lock(g_injectedMutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            CopyInto(g_rptInjectedLine, sizeof(g_rptInjectedLine), "<snapshot unavailable>");
            g_rptInjectedCount = 0;
        }
        else
        {
            CopyInto(g_rptInjectedLine, sizeof(g_rptInjectedLine),
                     g_injectedScanned ? g_injectedLine : "<not scanned>");
            g_rptInjectedCount = g_injectedCount;
            for (std::size_t i = 0; i < g_rptInjectedCount; ++i)
                CopyInto(g_rptInjectedNames[i], 64, g_injectedNames[i]);
        }
    }

    // R95: the reports the hang protocol belongs to -- the host survives them
    // (exit code 0) AND they are about a stall (spec s5.4's hang|gpu-stall).
    // `kind` is DeriveKindCStr's static string, so strcmp is heap-free.
    [[nodiscard]] bool IsHangProtocolReport(const char* kind, int exitCode) noexcept
    {
        return exitCode == 0 && kind &&
               (std::strcmp(kind, "hang") == 0 || std::strcmp(kind, "gpu-stall") == 0);
    }

}   // namespace Internal

namespace
{
#if defined(_WIN32)

    // UTF-8 -> UTF-16 into a caller buffer. No heap, no throw; an empty
    // result on failure so a bad path simply fails the file open below.
    const wchar_t* ToWide(const char* utf8, wchar_t* buf, int cap) noexcept
    {
        buf[0] = L'\0';
        if (utf8 && *utf8)
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, cap);
        return buf;
    }
    // ---- heap-free file IO ------------------------------------------------
    // CreateFileW over a UTF-8 path converted in place: fopen's narrow path
    // is ANSI on Windows, and std::ofstream/std::filesystem::path both
    // allocate. Everything below runs on the crash thread.

    HANDLE OpenForWrite(const char* utf8Path, bool append) noexcept
    {
        wchar_t wide[kPathMax];
        ToWide(utf8Path, wide, static_cast<int>(kPathMax));
        if (!wide[0]) return INVALID_HANDLE_VALUE;
        return CreateFileW(wide,
                           append ? FILE_APPEND_DATA : GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // Append to a file ANOTHER writer already holds open for writing -- the
    // log file, which spdlog's live basic_file_sink_mt owns. OpenForWrite's
    // FILE_SHARE_READ is not enough for that: a second open of a file held
    // for WRITE must itself allow sharing for write, or CreateFileW fails
    // with ERROR_SHARING_VIOLATION. FILE_APPEND_DATA keeps every write
    // atomic at the current end of file, so interleaving with spdlog's own
    // writes cannot overwrite them (plan 2, D9).
    HANDLE OpenForAppendShared(const char* utf8Path) noexcept
    {
        wchar_t wide[kPathMax];
        ToWide(utf8Path, wide, static_cast<int>(kPathMax));
        if (!wide[0]) return INVALID_HANDLE_VALUE;
        return CreateFileW(wide,
                           FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    bool WriteAll(HANDLE h, std::string_view s) noexcept
    {
        if (h == INVALID_HANDLE_VALUE) return false;
        const char* p = s.data();
        std::size_t left = s.size();
        while (left > 0)
        {
            const DWORD chunk = left > 0x04000000u ? 0x04000000u : static_cast<DWORD>(left);
            DWORD wrote = 0;
            if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) return false;
            p    += wrote;
            left -= wrote;
        }
        return true;
    }

    bool WriteTwoParts(const char* utf8Path, std::string_view a, std::string_view b) noexcept
    {
        const HANDLE h = OpenForWrite(utf8Path, /*append*/false);
        if (h == INVALID_HANDLE_VALUE) return false;
        const bool ok = WriteAll(h, a) && (b.empty() || WriteAll(h, b));
        CloseHandle(h);
        return ok;
    }

    // Build-and-write, with ONE bounded retry. Spec S5.5: on exhaustion the
    // crash thread writes what it has and SAYS SO -- what it must never do
    // is write something that does not parse.
    [[nodiscard]] EnvelopeWrite WriteEnvelope(CrashArena& arena, const EnvFields& f,
                                              const char* path) noexcept
    {
        if (const EnvelopeJson full = BuildEnvelopeJson(arena, f, /*elide*/false, kEnvRsv);
            full.complete)
        {
            return WriteTwoParts(path, full.text, {}) ? EnvelopeWrite::Written
                                                      : EnvelopeWrite::NotWritten;
        }

        const EnvelopeJson lean = BuildEnvelopeJson(arena, f, /*elide*/true, kEnvLeanRsv);
        if (!lean.complete)
            return EnvelopeWrite::NotWritten;
        return WriteTwoParts(path, lean.text, {}) ? EnvelopeWrite::WrittenElided
                                                  : EnvelopeWrite::NotWritten;
    }

    // ---- the report's own text --------------------------------------------

    // ONE thread section, not every thread: the in-process all-thread walk
    // was the freeze this arc removes (spec S5.2 step 5). The reporter
    // reconstructs the other threads from the minidump, which carries every
    // stack anyway.
    [[nodiscard]] std::string_view BuildThreadSection(CrashArena::Builder& b, DWORD tid,
                                                      std::size_t frameCount) noexcept
    {
        char line[512];
        std::snprintf(line, sizeof(line), "--- thread %lu%s ---",
                      tid, tid == g_mainThreadId ? " (MAIN)" : "");
        b.Append(line);
        b.Append("\n");

        if (frameCount == 0)
            b.Append("  <no frames recovered>\n");

        for (std::size_t i = 0; i < frameCount; ++i)
        {
            b.Append(FormatStackFrame(i, g_frames[i], std::span<char>(line, sizeof(line))));
            b.Append("\n");
        }
        b.Append("\n");
        return b.View();
    }

    void FillHeader(CrashArena::Builder& b, const Pending& p,
                    const char* dmpPath, bool dumpOk, bool exhausted,
                    bool envelopeElided) noexcept
    {
        char line[1024];

        b.Append("=== Arcane diagnostic report ===\n");
        b.Append("reason      : "); b.Append(p.reason[0] ? p.reason : "unspecified"); b.Append("\n");
        b.Append("app         : "); b.Append(g_appNameSnap); b.Append("\n");

        std::snprintf(line, sizeof(line), "pid         : %lu\n", GetCurrentProcessId());
        b.Append(line);
        std::snprintf(line, sizeof(line), "main thread : %lu\n", g_mainThreadId);
        b.Append(line);

        if (g_phaseSnap[0] != '\0')
        {
            b.Append("phase       : "); b.Append(g_phaseSnap); b.Append("\n");
        }
        if (g_beatSeen.load(std::memory_order_acquire))
        {
            std::snprintf(line, sizeof(line), "since beat  : %.2f s\n",
                          SecondsSince(g_lastBeat.load(std::memory_order_acquire)));
            b.Append(line);
        }

        b.Append("minidump    : ");
        b.Append(dumpOk ? dmpPath
                        : (p.lightweight ? "<not written (lightweight report)>" : "<failed to write>"));
        b.Append("\n");

        // The injected third-party modules the process had found by its LAST
        // scan, read from the fixed snapshot -- never enumerated here, where
        // the loader lock is off limits (R14). "not scanned" (no device was
        // ever created) and "none" are different facts and are spelled apart.
        b.Append("injected    : "); b.Append(g_rptInjectedLine); b.Append("\n");

        if (p.ep && p.ep->ExceptionRecord)
        {
            std::snprintf(line, sizeof(line), "exception   : code 0x%08lx at 0x%llx\n",
                          p.ep->ExceptionRecord->ExceptionCode,
                          reinterpret_cast<unsigned long long>(p.ep->ExceptionRecord->ExceptionAddress));
            b.Append(line);
        }
        if (exhausted)
            b.Append("arena       : EXHAUSTED -- this report is truncated (spec S5.5)\n");
        if (envelopeElided)
            b.Append("envelope    : ELIDED -- the .arcdiag dropped the stack text and the GPU "
                     "arrays to stay valid JSON; this file is the full stack\n");

        b.Append("\n");
    }

    // ---- the individual steps ---------------------------------------------

    // R6: with a fault context, walk that. Otherwise the walked thread is
    // parked (in SubmitReport's wait, or -- for a hang -- in whatever wedged
    // it), so suspend it, copy its context, and resume it immediately: one
    // thread held at a time, and never this one.
    [[nodiscard]] std::size_t CaptureWalkedStack(const Pending& p) noexcept
    {
        const std::span<StackFrame> out(g_frames, kMaxFrames);

        // Plan 2 task 5: stale from a previous report is worse than absent --
        // a synthetic exception record built on another report's context would
        // point the reporter's walk at a thread this one never touched.
        g_walkedContextValid = false;

        if (p.ep && p.ep->ContextRecord)
            return CaptureStackFromContext(p.ep->ContextRecord, out);

        if (p.walkThreadId == GetCurrentThreadId())
            return CaptureCurrentStack(out);

        const HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME,
                                     FALSE, p.walkThreadId);
        if (!th) return 0;

        std::size_t n = 0;
        if (SuspendThread(th) != static_cast<DWORD>(-1))
        {
            CONTEXT ctx{};
            // CONTEXT_ALL, not CONTEXT_FULL (UE :1935): the context is now
            // ALSO what rides into the minidump as the synthetic exception's
            // ContextRecord, and a dump whose context is missing the segment
            // and debug registers is one dbgeng can refuse to walk from.
            ctx.ContextFlags = CONTEXT_ALL;
            if (GetThreadContext(th, &ctx))
            {
                n = CaptureStackFromContext(&ctx, out);
                g_walkedContext      = ctx;
                g_walkedContextValid = true;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
        return n;
    }

    bool WriteMiniDump(const char* utf8Path, EXCEPTION_POINTERS* ep, DWORD threadId) noexcept
    {
        const HANDLE file = OpenForWrite(utf8Path, /*append*/false);
        if (file == INVALID_HANDLE_VALUE) return false;

        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = threadId;
        mei.ExceptionPointers = ep;
        mei.ClientPointers    = FALSE;

        // MinidumpKind::Default (diagnostics.minidumpKind): not WithFullMemory,
        // which for a wedged GPU host is hundreds of MB and takes long enough
        // that the user gives up. This set keeps every thread's stack, the
        // handle table (which lock is held -- the question a deadlock actually
        // asks), and memory the stacks point at. Full is the opt-in for deep
        // debugging; Small is MiniDumpNormal.
        MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithThreadInfo | MiniDumpWithHandleData |
            MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
        if (g_cfg.minidumpKind == MinidumpKind::Small)
            type = MiniDumpNormal;
        else if (g_cfg.minidumpKind == MinidumpKind::Full)
            type = static_cast<MINIDUMP_TYPE>(type | MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo);

        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                          type, ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(file);
        return ok != FALSE;
    }

    // A fresh, non-nil, v4-shaped guid without touching Guid::Generate()
    // (which takes the Core CSPRNG's lock). The seed is drawn once at
    // Install; the clock and the report counter make each report distinct.
    void MakeReportGuid(char* out, std::size_t cap) noexcept
    {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);

        std::uint64_t hi = g_guidSeed.hi ^ (static_cast<std::uint64_t>(qpc.QuadPart) * 0x9E3779B97F4A7C15ull);
        std::uint64_t lo = g_guidSeed.lo
                         ^ ((static_cast<std::uint64_t>(GetCurrentProcessId()) << 32)
                            + (static_cast<std::uint64_t>(g_reportCount.load(std::memory_order_relaxed)) + 1)
                              * 0xBF58476D1CE4E5B9ull);

        // RFC-4122 v4 layout, so the string form reads as a real v4 UUID to
        // anything that inspects it -- and can never come out nil, which
        // Diag::Parse refuses.
        hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
        lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

        std::snprintf(out, cap, "%08x-%04x-%04x-%04x-%012llx",
                      static_cast<unsigned>(hi >> 32),
                      static_cast<unsigned>((hi >> 16) & 0xFFFFull),
                      static_cast<unsigned>(hi & 0xFFFFull),
                      static_cast<unsigned>(lo >> 48),
                      static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
    }

    void DumpBacklog(const char* utf8Path) noexcept
    {
        const HANDLE h = OpenForWrite(utf8Path, /*append*/false);
        if (h == INVALID_HANDLE_VALUE) return;

        char line[Log::kBacklogLineBytes];
        const std::size_t count = Log::BacklogLineCount();
        for (std::size_t i = 0; i < count; ++i)
        {
            std::size_t len = Log::BacklogLine(i, std::span<char>(line, sizeof(line)));
            // spdlog's formatted record already ends in the pattern's eol;
            // trim it so the dump is one line per record, not one blank
            // line between each.
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;
            WriteAll(h, std::string_view(line, len));
            WriteAll(h, "\n");
        }
        CloseHandle(h);
    }

    // Plan 2 (D9): a FATAL report's echo may not go through spdlog. The
    // faulting thread may have died holding a sink mutex, and the crash thread
    // would then wedge on ARC_ERROR until the submitter's deadline expired --
    // after every file was already on disk. Straight to the log file (append,
    // through OpenForAppendShared: spdlog holds that file open for WRITE, so a
    // second handle must share write access) and to the stderr HANDLE (not the
    // CRT stream, whose lock is the same hazard).
    void FatalEcho(std::initializer_list<std::string_view> parts) noexcept
    {
        HANDLE log = INVALID_HANDLE_VALUE;
        if (g_logPathSnap[0]) log = OpenForAppendShared(g_logPathSnap);
        const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        for (std::string_view part : parts)
        {
            if (log != INVALID_HANDLE_VALUE) WriteAll(log, part);
            if (err && err != INVALID_HANDLE_VALUE) WriteAll(err, part);
        }
        if (log != INVALID_HANDLE_VALUE) { WriteAll(log, "\n"); CloseHandle(log); }
        if (err && err != INVALID_HANDLE_VALUE) WriteAll(err, "\n");
    }

    // The hand-off. Missing exe is the EXPECTED case in this plan (the
    // reporter does not exist yet), so a failure is one line, after the
    // files are already on disk, and never anything louder.
    //
    // Plan 2, task 8 (spec s5.4): `hangProtocol` switches on the hang
    // protocol's host half: the recovered event is reset for the reporter
    // about to wait on it (D11), its name rides on the line, and the
    // reporter's process handle is KEPT so that while it lives no second
    // hang reporter is spawned for this host (D12). Every spawn carries
    // --host-created (D7). Runs on the crash thread: fixed storage and Win32
    // calls only, no heap.
    //
    // R95 (fix round 1): the caller keys it on the KIND -- hang or gpu-stall,
    // with exit code 0 (IsHangProtocolReport, Internal) -- never on the exit code
    // alone. Spec s5.4 scopes the protocol to the `--kind hang|gpu-stall`
    // spawn. A device-removed WriteReport("gpu-crash: ...") is exit-0 too, and
    // keyed on the exit code it was swallowed by a gpu-stall window's D12 gate
    // (the canonical TDR sequence) or, spawned first, blocked a later hang's.
    // Every other report gets a plain detached reporter whose handle is
    // closed: it is never gated by a hang window, and never gates one.
    [[nodiscard]] bool SpawnReporter(const char* stemUtf8, const char* kind, bool hangProtocol) noexcept
    {
        // Reset BEFORE the spawn gate, so a host with the spawn disabled (the
        // [diag] recovered-event case, a build machine) still runs the
        // protocol's observable half -- and so the event a live reporter from
        // an EARLIER stall is waiting on is re-armed by this report too.
        if (hangProtocol && g_recoveredEvent) ResetEvent(g_recoveredEvent);
        if (!g_cfg.spawnReporter) return true;   // disabled (or a build machine, Install's gate): not a failure
        if (!g_reporterExe[0])    return false;

        if (hangProtocol && g_reporterProcess)
        {
            // One window per host (spec s5.4, D12): a reporter spawned for an
            // earlier hang/gpu-stall report is still up for this pid. The report
            // was still written; it just gets no second window. A dead one's
            // handle is released and the spawn proceeds.
            if (WaitForSingleObject(g_reporterProcess, 0) == WAIT_TIMEOUT) return true;
            CloseHandle(g_reporterProcess);
            g_reporterProcess = nullptr;
        }

        // The stem rides the line inside a plain quote wrap: a '"', CR or LF
        // in it could add arguments of its own (the reporter honours
        // --relaunch), and the crash thread may not call the allocating
        // QuoteWindowsArg. ReportDirFor already refuses such a dumpDir at
        // Install/RetargetDumpDir; this scan is the crash-thread backstop
        // (S7-SEC fix round 1). No spawn: the caller says where the report is.
        if (!ReporterStemSafe(stemUtf8)) return false;

        wchar_t wideStem[kPathMax];
        wchar_t wideKind[64];
        ToWide(stemUtf8, wideStem, static_cast<int>(kPathMax));
        ToWide(kind, wideKind, 64);

        _snwprintf_s(g_spawnCmd, sizeof(g_spawnCmd) / sizeof(g_spawnCmd[0]), _TRUNCATE,
                     L"\"%s\" \"%s.arcdiag\" --pid %lu --kind %s --product \"%s\" --host-created %llu%s%s%s%s",
                     g_reporterExe, wideStem,
                     static_cast<unsigned long>(GetCurrentProcessId()),
                     wideKind, g_productWide, g_hostCreated,
                     g_cfg.unattended ? L" --unattended" : L"",
                     (hangProtocol && g_recoveredEvent) ? L" --recovered-event " : L"",
                     (hangProtocol && g_recoveredEvent) ? g_recoveredName : L"",
                     g_reporterSettingsArgs);

        STARTUPINFOW        si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        // lpApplicationName pins the exact binary Install checked
        // (ReporterExeFor, S7-SEC): never re-parsed from the line, never searched.
        if (!CreateProcessW(g_reporterExe, g_spawnCmd, nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
            return false;
        CloseHandle(pi.hThread);

        // D14 (UE: WindowsPlatformCrashContext.cpp:913-915): Windows lets a
        // background process take the foreground only when the foreground one
        // grants it. Without this the reporter's window can open BEHIND the
        // host's own. A user32 call, no heap; UE makes it from its crash
        // thread too.
        AllowSetForegroundWindow(pi.dwProcessId);

        // Hang protocol: kept -- "while it lives no second reporter" (D12).
        // Everything else: closed -- a fatal host is about to die and the
        // reporter outlives it on purpose; a survivable non-hang report's
        // reporter is independent of any hang window (R95).
        if (hangProtocol) g_reporterProcess = pi.hProcess;
        else            CloseHandle(pi.hProcess);
        return true;
    }

    // ---- the report, in UE's order (spec S5.2) -----------------------------

    void RunReportOnCrashThread(const Pending& p) noexcept
    {
        // Held for the WHOLE body, exactly as WriteReportImpl used to be, so
        // FenceReports() stays a sound teardown fence and RetargetDumpDir
        // cannot move the directory out from under a report mid-write.
        std::lock_guard<std::recursive_mutex> reportLock(g_reportMutex);

        // Step 1: no hang report may interleave with this one.
        g_watchdogPaused.store(true, std::memory_order_release);

        CrashArena& arena = CrashArena::Instance();
        arena.Reset();
        g_lastStemValid.store(false, std::memory_order_release);

        // One consistent injected-module list for the whole report.
        CopyInjectedForReport();

        // Paths. The report directory was snapshotted off-path; the stem is
        // "<dir>\<app>-<stamp>-pid<n>", the same spelling operator/ produced
        // before, so every existing consumer of the sibling stem is unmoved.
        char stamp[32];
        TimeStampForFilename(stamp, sizeof(stamp));

        char stem[kPathMax], txtPath[kPathMax], dmpPath[kPathMax];
        char diagPath[kPathMax], tmpPath[kPathMax], logTxtPath[kPathMax];
        std::snprintf(stem, sizeof(stem), "%s\\%s-%s-pid%lu",
                      g_reportDirSnap, g_appNameSnap, stamp,
                      static_cast<unsigned long>(GetCurrentProcessId()));
        std::snprintf(txtPath,    sizeof(txtPath),    "%s.txt",         stem);
        std::snprintf(dmpPath,    sizeof(dmpPath),    "%s.dmp",         stem);
        std::snprintf(diagPath,   sizeof(diagPath),   "%s.arcdiag",     stem);
        std::snprintf(tmpPath,    sizeof(tmpPath),    "%s.arcdiag.tmp", stem);
        std::snprintf(logTxtPath, sizeof(logTxtPath), "%s.log.txt",     stem);

        // Step 2: the portable stack of the walked thread. The header's span
        // is carved FIRST so an arena that runs out while formatting the
        // stack still has room for the header that says so.
        const std::size_t frameCount = CaptureWalkedStack(p);
        CrashArena::Builder header  = arena.OpenBuilder(kHeaderRsv);
        CrashArena::Builder sectionB = arena.OpenBuilder(kSectionRsv);
        const std::string_view section = BuildThreadSection(sectionB, p.walkThreadId, frameCount);

        char guid[48];
        char tsUtc[48];
        MakeReportGuid(guid, sizeof(guid));
        TimestampUtcIso8601(tsUtc, sizeof(tsUtc));
        const char* const kind = DeriveKindCStr(p.reason);

        EnvFields fields;
        fields.guid             = guid;
        fields.kind             = kind;
        fields.reason           = p.reason[0] ? p.reason : "unspecified";
        fields.timestampUtc     = tsUtc;
        fields.appName          = g_appNameSnap;
        fields.phase            = g_phaseSnap;
        fields.buildInfo        = BuildInfo();          // a static literal; no allocation
        fields.cpuThreadSummary = section;
        fields.siblingTxt       = txtPath;
        fields.siblingDmp       = p.lightweight ? "" : dmpPath;
        fields.siblingGpuDump   = "";
        fields.logPath          = g_logPathSnap;
        fields.commandLine      = g_commandLineSnap;
        fields.exitCode         = p.exitCode;
        fields.gpu              = nullptr;

        // Step 3: the MINIMAL envelope, before anything that can wedge, so a
        // reporter can start on a report whose later steps never finished.
        const EnvelopeWrite minimalWrite = WriteEnvelope(arena, fields, diagPath);

        // Step 4: the minidump -- the artifact a debugger opens. Skipped for
        // a continuable (ensure) report, which must resume quickly.
        //
        // Plan 2 task 5: a hang or manual report has no fault, so without help
        // its dump carries NO exception stream, dbgeng finds no stored event,
        // and the reporter cannot tell which thread wedged -- it lands on
        // thread 0. The walked thread's captured context rides in a synthetic
        // record (ExceptionCode = STILL_ACTIVE, UE
        // WindowsPlatformCrashContext.cpp:1925-1966) so EVERY Arcane dump
        // opens with a stored event the reporter walks from, uniformly.
        //
        // FillHeader still prints `exception :` only for a REAL fault: it
        // reads p.ep, which is untouched here.
        EXCEPTION_POINTERS  synthetic{};
        EXCEPTION_POINTERS* dumpEp = p.ep;
        if (!dumpEp && g_walkedContextValid)
        {
            g_walkedRecord                  = {};
            g_walkedRecord.ExceptionCode    = STILL_ACTIVE;
            g_walkedRecord.ExceptionAddress = reinterpret_cast<void*>(g_walkedContext.Rip);
            synthetic.ExceptionRecord       = &g_walkedRecord;
            synthetic.ContextRecord         = &g_walkedContext;
            dumpEp                          = &synthetic;
        }
        const bool dumpOk = !p.lightweight && WriteMiniDump(dmpPath, dumpEp, p.walkThreadId);

        // Step 5: the .txt. Exhaustion is sampled HERE, not at the end:
        // the envelope retry below can exhaust the arena long after this
        // file is complete, and a report that cried "truncated" over a whole
        // text file would be lying about the one artifact a human reads.
        const bool textTruncated = arena.Exhausted();
        FillHeader(header, p, dmpPath, dumpOk, textTruncated,
                   minimalWrite == EnvelopeWrite::WrittenElided);
        const bool txtOk = WriteTwoParts(txtPath, header.View(), section);

        bool           spawnOk   = true;
        bool           haveGpu   = false;
        EnvelopeWrite  fullWrite = EnvelopeWrite::NotWritten;
        // Empty strings/vectors allocate nothing; it only reaches the heap
        // once the provider (below) fills it, which is the step the plan
        // tolerates allocations in.
        Diag::Envelope gpuEnv;

        if (!p.lightweight)
        {
            // Step 6: the GPU-section provider. THE one step allowed to
            // allocate (a real backend retrieves DRED, names resources, may
            // write its own .gpudump) -- which is exactly why it runs here,
            // after the envelope and the minidump are already on disk.
            GpuSectionProvider gpuProvider     = nullptr;
            void*              gpuProviderUser = nullptr;
            {
                std::lock_guard gpuLock(g_gpuProviderMutex);
                gpuProvider     = g_gpuProvider;
                gpuProviderUser = g_gpuProviderUser;
            }
            if (gpuProvider)
            {
                // The provider's CONTRACT (Diagnostics.hpp, GpuSectionProvider)
                // is that the envelope handed to it ALREADY carries
                // guid/kind/timestamp/appName/phase/buildInfo/cpuThreadSummary
                // -- NriDiagnostics' backend classifies device-removed vs
                // device-alive from `kind` alone, because it is the only
                // evidence it has (Render/Nri/NriDiagnostics.cpp). Filled
                // here, inside the one step the plan already tolerates
                // allocations in, after the minimal envelope and the minidump
                // are both durable.
                if (const auto parsed = Guid::FromString(guid)) gpuEnv.guid = *parsed;
                gpuEnv.kind             = kind;
                gpuEnv.timestampUtc     = tsUtc;
                gpuEnv.appName          = g_appNameSnap;
                gpuEnv.phase            = g_phaseSnap;
                gpuEnv.buildInfo        = fields.buildInfo;
                gpuEnv.cpuThreadSummary = section;

                std::string                 gpuText;
                const std::filesystem::path stemPath(stem);
                gpuProvider(gpuEnv, gpuText, stemPath, gpuProviderUser);
                haveGpu = true;

                // Append-only: the CPU portion is already durable from the
                // write above, so a provider that blocks or faults never
                // re-risks the report that already succeeded.
                if (txtOk)
                {
                    const HANDLE h = OpenForWrite(txtPath, /*append*/true);
                    if (h != INVALID_HANDLE_VALUE)
                    {
                        WriteAll(h, "=== GPU ===\n");
                        if (!gpuText.empty()) { WriteAll(h, gpuText); WriteAll(h, "\n"); }
                        CloseHandle(h);
                    }
                }
            }

            // Step 7: the FULL envelope, rewritten ATOMICALLY over the
            // minimal one. Sibling paths now reflect what actually landed --
            // never claim a path the editor's open button would fail on.
            fields.siblingTxt     = txtOk  ? txtPath : "";
            fields.siblingDmp     = dumpOk ? dmpPath : "";
            fields.siblingGpuDump = haveGpu ? gpuEnv.siblingGpuDump.c_str() : "";
            fields.gpu            = haveGpu ? &gpuEnv : nullptr;
            fullWrite = WriteEnvelope(arena, fields, tmpPath);
            if (fullWrite != EnvelopeWrite::NotWritten)
            {
                // Only ever replace the minimal envelope with something that
                // PARSES. A truncated full envelope moved over a good
                // minimal one would leave the report unreadable -- the
                // single worst outcome this whole path can produce.
                wchar_t wFrom[kPathMax], wTo[kPathMax];
                ToWide(tmpPath,  wFrom, static_cast<int>(kPathMax));
                ToWide(diagPath, wTo,   static_cast<int>(kPathMax));
                MoveFileExW(wFrom, wTo, MOVEFILE_REPLACE_EXISTING);
            }
            else
            {
                // Withheld. Clean up the unparseable temp so nothing
                // downstream mistakes it for a report.
                wchar_t wTmp[kPathMax];
                ToWide(tmpPath, wTmp, static_cast<int>(kPathMax));
                DeleteFileW(wTmp);
            }

            // Step 8: the log backlog beside the report, a BOUNDED flush (the
            // faulting thread may have died holding spdlog's mutex), and the
            // hand-off.
            DumpBacklog(logTxtPath);
            Log::FlushFileSinkBounded(g_cfg.logFlushTimeoutMs);
            spawnOk = SpawnReporter(stem, kind, IsHangProtocolReport(kind, p.exitCode));
        }

        g_reportCount.fetch_add(1, std::memory_order_acq_rel);
        CopyInto(g_lastStem, sizeof(g_lastStem), stem);
        g_lastStemValid.store(true, std::memory_order_release);

        // ---------------------------------------------------------------
        // Every file is on disk from here on, so the heap is fair game
        // again -- which is the whole reason the log echo and the hook moved
        // down here from where WriteReportImpl used to run them.
        // ---------------------------------------------------------------
        // ...with ONE exception (plan 2, D9): a FATAL report's echo still may
        // not touch spdlog. The heap is safe by now, but the sink mutex is
        // not -- the thread that died may have been holding it -- so a report
        // the process will not survive echoes through FatalEcho instead. A
        // SURVIVABLE one keeps the macros: its log is still flowing.
        const char* const reasonText = p.reason[0] ? p.reason : "report";
        if (p.exitCode != 0)
        {
            FatalEcho({ "Diagnostics: ", reasonText, " -- report written\n", header.View(), section });
            if (!spawnOk)
                FatalEcho({ "Arcane: crash reporter hand-off failed; the report is at ", stem, ".arcdiag" });
            if (textTruncated)
                FatalEcho({ "Diagnostics: the crash arena was exhausted before the text report was built; '",
                            stem, ".txt' is truncated" });
            // Spec S5.5: on exhaustion the crash thread writes what it has and
            // SAYS SO. Both of these mean the .arcdiag on disk carries less
            // than the report does -- and both mean the .txt sibling is the
            // complete record.
            if (minimalWrite == EnvelopeWrite::WrittenElided ||
                (!p.lightweight && fullWrite == EnvelopeWrite::WrittenElided))
                FatalEcho({ "Diagnostics: '", stem, ".arcdiag' was ELIDED so it would still parse; '",
                            stem, ".txt' carries the full report" });
            if (!p.lightweight && fullWrite == EnvelopeWrite::NotWritten)
                FatalEcho({ "Diagnostics: the full envelope for '", stem, "' was WITHHELD; the earlier envelope stands" });
        }
        else
        {
            ARC_ERROR("Diagnostics: {} -- report written\n{}{}",
                      reasonText, header.View(), section);
            if (!spawnOk)
            {
                std::fprintf(stderr, "Arcane: crash reporter hand-off failed; the report is at %s.arcdiag\n", stem);
                ARC_WARN("Diagnostics: could not spawn the crash reporter; the report is at '{}.arcdiag'", stem);
            }
            if (textTruncated)
            {
                ARC_WARN("Diagnostics: the crash arena was exhausted before the text report was "
                         "built; '{}.txt' is truncated", stem);
            }

            if (minimalWrite == EnvelopeWrite::WrittenElided ||
                (!p.lightweight && fullWrite == EnvelopeWrite::WrittenElided))
            {
                ARC_WARN("Diagnostics: '{}.arcdiag' was ELIDED (stack text and GPU arrays dropped) "
                         "so it would still parse; '{}.txt' carries the full report", stem, stem);
            }
            if (!p.lightweight && fullWrite == EnvelopeWrite::NotWritten)
            {
                ARC_WARN("Diagnostics: the full envelope for '{}' was WITHHELD -- it would not fit "
                         "the crash arena intact, and replacing a valid envelope with a truncated "
                         "one is never an improvement; the earlier envelope stands", stem);
            }
        }

        // Report-written hook (GPU crash diagnostics arc, Task 9): fires
        // LAST, with the .arcdiag path, whether or not every write
        // succeeded -- best-effort, same as the rest of this function. It
        // runs while g_reportMutex is still held, which is what makes
        // FenceReports() sound.
        ReportWrittenHook reportHook     = nullptr;
        void*             reportHookUser = nullptr;
        {
            std::lock_guard reportHookLock(g_reportWrittenMutex);
            reportHook     = g_reportWrittenHook;
            reportHookUser = g_reportWrittenUser;
        }
        if (reportHook)
            reportHook(std::filesystem::path(diagPath), reportHookUser);

        // A SURVIVABLE report hands the process back: the module table and
        // the log backlog must start moving again, or every later line in
        // the session is lost. A fatal one deliberately leaves both frozen --
        // that process is already on its way down (R11).
        if (p.exitCode == 0 || p.lightweight)
        {
            ModuleTable::SetFrozen(false);
            if (!p.lightweight) Log::ThawBacklog();
            g_watchdogPaused.store(false, std::memory_order_release);
        }
    }

    // The crash thread's own SEH guard (spec S9): a fault inside the report
    // path terminates AT ONCE with 13 rather than letting the submitting
    // thread's 60 s wait be the only thing left. No C++ object with a
    // destructor may live in a function carrying __try/__except (MSVC C2712),
    // which is why this is a wrapper and not part of the body above.
    void RunReportGuarded(const Pending& p) noexcept
    {
        __try
        {
            RunReportOnCrashThread(p);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            TerminateProcess(GetCurrentProcess(), ExitCode::kCrashInCrashPath);
        }
    }

    DWORD WINAPI CrashThreadProc(LPVOID)
    {
        SetThreadDescription(GetCurrentThread(), L"Arcane-CrashReporter");
        for (;;)
        {
            WaitForSingleObject(g_crashEvent, INFINITE);
            if (g_crashThreadStop.load(std::memory_order_acquire))
                break;

            // COPY, never a reference to the global. A submitter whose wait
            // expires (WAIT_TIMEOUT on a survivable report) releases
            // g_submitMutex and returns while this report is still running,
            // and the NEXT submitter then overwrites g_pending -- which would
            // change the reason, the walked thread and the exit code
            // mid-report. Pending is a ~1 KB trivially-copyable POD; this
            // costs one memcpy on a 256 KB stack and no heap.
            const Pending local = g_pending;
            RunReportGuarded(local);
            SetEvent(g_handledEvent);
        }
        return 0;
    }

    void FillPending(const ReportRequest& r, DWORD walkTid) noexcept
    {
        // R3: the reason is COPIED here, on the submitting thread, because
        // the crash thread resets the arena at the top of every report and a
        // reason formatted INTO that arena (task 7's handlers) would die with
        // the reset.
        CopyInto(g_pending.reason, sizeof(g_pending.reason), r.reason ? r.reason : "unspecified");
        g_pending.ep           = static_cast<EXCEPTION_POINTERS*>(r.exceptionPointers);
        g_pending.walkThreadId = walkTid;
        g_pending.lightweight  = r.lightweight;
        g_pending.exitCode     = r.exitCode;
    }

    // R6, in one place: a hang is about the MAIN thread, not about the
    // watchdog that noticed it.
    [[nodiscard]] DWORD WalkTargetFor(const ReportRequest& r) noexcept
    {
        const DWORD self = GetCurrentThreadId();
        if (r.exceptionPointers) return self;
        if (g_mainThreadId != 0 && g_watchdogThreadId.load(std::memory_order_acquire) == self)
            return g_mainThreadId;
        return self;
    }
#endif  // _WIN32

#if defined(_WIN32)
    // The D3D12 debug layer's fail-fast.
    //
    // D3D12SDKLayers.dll reports conditions it considers unrecoverable with
    // RaiseFailFastException, code 0x87D -- the same signature
    // RenderDeviceDesc.hpp already records for the Nahimic-OSD window-hook
    // hazard at device creation. A fail-fast bypasses every frame-based
    // __except and every vectored handler and is NONCONTINUABLE, so THIS
    // FILTER IS THE ONLY PLACE IN THE PROCESS THAT CAN SEE IT, and it can
    // only decide how to die -- never whether to.
    ARC_CONSTANT("hardware/OS/API limit: the D3D12 debug layer's fail-fast exception code, defined by the API")
    constexpr DWORD kD3D12DebugLayerFailFast = 0x0000087dul;

    // MSVC's C++ exception code ('msc' | 0xE0000000). A `throw` that nothing
    // catches is raised as THIS, and -- measured on this toolchain, not
    // assumed -- it arrives HERE, at the top-level filter, rather than at
    // std::terminate: with a filter installed, the CRT's own unhandled-C++
    // filter never gets to call terminate(). So the classification has to
    // live on both paths (see ActiveExceptionReason below), or every uncaught
    // exception -- an OOM included -- would be filed as a plain `crash`.
    ARC_CONSTANT("hardware/OS/API limit: MSVC's C++ exception code ('msc' | 0xE0000000), defined by the toolchain")
    constexpr DWORD kMsvcCppException = 0xE06D7363ul;

    // Both defined with the fail-fast family below; ActiveExceptionReason is
    // shared with OnTerminate.
    const char* ActiveExceptionReason(const char* fallback) noexcept;
    const char* CppExceptionReason(const EXCEPTION_RECORD* record) noexcept;

    LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep)
    {
        // No re-entry guard here any more: SubmitReport owns it (a second
        // faulting thread waits for the report in flight, then terminates),
        // and the filter's only job is to name the kind.
        //
        // A 0x87D fail-fast AFTER the render layer has already confirmed the
        // device is gone is not a new incident -- it is the debug layer's
        // account of the loss we are already reporting, raised from whatever
        // post-mortem Release happened to touch the dead device first. Say
        // that, and die the way a device loss is supposed to die.
        //
        // WHY BOTH CONJUNCTS. 0x87D on its own is NOT a device-removal signal
        // (RenderDeviceDesc.hpp's Nahimic case is a startup window-hook incompatibility
        // with a live device), so mapping the code alone would relabel a real
        // crash. And a device loss on its own is already handled by the
        // ordinary chain. Only the pair is unambiguous, and the pair cannot
        // occur before a confirmed loss -- which is exactly the Nahimic case's
        // exemption.
        if (ep && ep->ExceptionRecord &&
            ep->ExceptionRecord->ExceptionCode == kD3D12DebugLayerFailFast &&
            g_gpuDeviceLost.load(std::memory_order_acquire))
        {
            // "gpu" in the reason is load-bearing: DeriveKind classifies on it
            // and it is what gets the `.gpudump` sibling written. Same wording
            // rule as ObserveDeviceRemoved (Render/DeviceCreationD3D12.cpp).
            // Exit code 1 is what the hosts already exit with on a device
            // loss; SubmitReport terminates with it once the report is on
            // disk, so the TerminateProcess below is only a belt-and-braces
            // backstop for a report path that could not run at all.
            SubmitReport({ "gpu-crash: device removed "
                           "(D3D12 debug-layer fail-fast 0x87d after the loss)", ep, false, 1 });

            // Exit the way the hosts exit on a device loss, and exit NOW.
            // Falling through would hand the fail-fast to WER, which takes
            // long enough for the hang watchdog to file a SECOND, misleading
            // report about a main thread that is not hung but dying -- and
            // would end the process with 0x87D (2173) rather than 1. The
            // report above is already on disk; nothing after this point can
            // add to it. TerminateProcess rather than exit(): every remaining
            // destructor in this process would run against the dead device.
            TerminateProcess(GetCurrentProcess(), 1);
        }

        // An uncaught C++ exception is a TERMINATE, not a fault: same kind,
        // same reason and the same OOM classification it would have got from
        // std::terminate (spec S5.3), because which of the two paths the
        // toolchain happens to route it down is not something a crash report
        // should be able to disagree about. `ep` still travels, so the
        // minidump is taken at the throw site.
        if (ep && ep->ExceptionRecord &&
            ep->ExceptionRecord->ExceptionCode == kMsvcCppException)
        {
            // The throw record first (it carries the type even here, where
            // std::current_exception() is empty because nothing has caught
            // anything), then the terminate-path classifier, then a plain
            // statement of what happened.
            const char* reason = CppExceptionReason(ep->ExceptionRecord);
            if (!reason)
                reason = ActiveExceptionReason("terminate: unhandled C++ exception");
            SubmitReport({ reason, ep, false, ExitCode::kCrashed });
            return EXCEPTION_EXECUTE_HANDLER;
        }

        // Never chains to g_prevFilter for our OWN kinds: that chain is what
        // let WER's dialog appear on top of a report we had already written
        // (spec S5.2). SubmitReport terminates with kCrashed, so the return
        // below is unreachable in practice -- it exists because the compiler
        // needs one, and because a Diagnostics that was never installed must
        // still leave the process to whatever handled faults before us.
        SubmitReport({ "crash (unhandled exception)", ep, false, ExitCode::kCrashed });
        return EXCEPTION_EXECUTE_HANDLER;
    }
#endif

#if defined(_WIN32)
    // ---- the fail-fast family (spec S5.1 items 1-4, S5.3) -----------------
    //
    // SEH is not the only way a process dies. A failing assert aborts, an
    // uncaught exception terminates, a CRT contract violation fail-fasts, a
    // pure virtual call traps -- and none of those reaches the unhandled
    // exception filter above. Each handler below is the SAME entry into the
    // crash thread, differing only in the reason it hands over (DeriveKindCStr
    // reads the kind straight out of that reason, which is why the prefixes
    // are load-bearing).
    //
    // All of them run ON the faulting thread, after the fault: noexcept,
    // heap-free, and never returning -- SubmitReport terminates the process
    // once the report is on disk. Reasons are formatted with FormatReason,
    // into a PER-THREAD buffer, and SubmitReport copies them into the pending
    // record on this same thread (R3), so no reason ever dangles. They are
    // deliberately NOT formatted into the crash arena: these handlers fire on
    // arbitrary threads and the arena belongs to the crash thread alone
    // (finding I2).

    // The one piece of this family that may touch C++ machinery: classifying
    // the in-flight exception is the whole point (a std::bad_alloc is an OOM,
    // not a generic terminate -- UE gives OOM its own crash type), and the
    // only way to see its type is to rethrow it into a catch. Every arm ends
    // in a literal or an arena slot, and catch(...) means nothing escapes.
    // `fallback` is what the caller means by "no exception was active", which
    // differs between the two callers.
    const char* ActiveExceptionReason(const char* fallback) noexcept
    {
        const char* reason = fallback;
        if (const std::exception_ptr ex = std::current_exception())
        {
            try { std::rethrow_exception(ex); }
            catch (const std::bad_alloc& e) { reason = FormatReason("out-of-memory: %s", e.what()); }
            catch (const std::exception& e) { reason = FormatReason("terminate: %s", e.what()); }
            catch (...)                     { reason = "terminate: non-std exception"; }
        }
        return reason;
    }

    void OnTerminate() noexcept
    {
        SubmitReport({ ActiveExceptionReason("terminate: (no active exception)"),
                       nullptr, false, ExitCode::kCrashed });
    }

    // ---- the throw record, decoded ----------------------------------------
    //
    // An exception that nothing catches arrives at the unhandled-exception
    // filter with the C++ machinery still in front of it: nothing has been
    // caught, so std::current_exception() is empty (measured, not assumed) and
    // the rethrow classification above has nothing to work with. The raised
    // record DOES carry the type, in the form the compiler emits for every
    // `throw`: parameters {magic, object, ThrowInfo, image base}, and the
    // ThrowInfo names every type the throw is catchable as. That list is how
    // `catch (const std::bad_alloc&)` would have matched -- so reading it is
    // the same question a catch asks, asked without unwinding anything.
    //
    // Layouts are the compiler's (vcruntime's ehdata.h), restated here because
    // that header is not public. Every offset is an RVA against the image base
    // on x64.
    struct ThrowPmd            { int mdisp; int pdisp; int vdisp; };
    struct ThrowCatchableType  { unsigned int properties; int pType; ThrowPmd thisDisplacement;
                                 int sizeOrOffset; int copyFunction; };
    struct ThrowCatchableArray { int count; int types[1]; };
    struct ThrowInfoLayout     { unsigned int attributes; int pmfnUnwind; int pForwardCompat;
                                 int pCatchableTypeArray; };
    struct ThrowTypeDescriptor { const void* vftable; void* spare; char name[1]; };

    template <typename T>
    const T* FromRva(const char* base, int rva) noexcept
    {
        return rva != 0 ? reinterpret_cast<const T*>(base + rva) : nullptr;
    }

    bool NameContains(const char* name, const char* needle) noexcept
    {
        return name && needle && std::strstr(name, needle) != nullptr;
    }

    const char* CppExceptionReason(const EXCEPTION_RECORD* record) noexcept
    {
        if (!record || record->NumberParameters < 4) return nullptr;

        const char* const objectPtr = reinterpret_cast<const char*>(record->ExceptionInformation[1]);
        const auto* const info      = reinterpret_cast<const ThrowInfoLayout*>(record->ExceptionInformation[2]);
        const char* const imageBase = reinterpret_cast<const char*>(record->ExceptionInformation[3]);
        // A bare `throw;` with nothing in flight carries no ThrowInfo.
        if (!info || !objectPtr) return nullptr;

        const char* typeName   = nullptr;
        const char* whatText   = nullptr;
        bool        isBadAlloc = false;

        // Structure-walking memory handed to us by a process that is already
        // dying: SEH, so a torn record degrades to "unclassified" instead of
        // faulting the crash path. No C++ object with a destructor may be in
        // scope here -- hence the raw pointers above.
        __try
        {
            const auto* const types = FromRva<ThrowCatchableArray>(imageBase, info->pCatchableTypeArray);
            const int count = types ? types->count : 0;
            int stdExceptionDisp = -1;

            for (int i = 0; i < count && i < 16; ++i)
            {
                const auto* const ct = FromRva<ThrowCatchableType>(imageBase, types->types[i]);
                if (!ct) continue;
                const auto* const td = FromRva<ThrowTypeDescriptor>(imageBase, ct->pType);
                if (!td) continue;

                const char* const name = td->name;   // mangled: ".?AVbad_alloc@std@@"
                if (!typeName) typeName = name;      // [0] is the most-derived type
                if (NameContains(name, "bad_alloc") || NameContains(name, "bad_array_new_length"))
                    isBadAlloc = true;
                // The std::exception subobject, if this throw has one: pdisp
                // < 0 means a plain (non-virtual-base) offset, which is all
                // we are willing to follow.
                if (std::strcmp(name, ".?AVexception@std@@") == 0 && ct->thisDisplacement.pdisp < 0)
                    stdExceptionDisp = ct->thisDisplacement.mdisp;
            }

            // what() through the SAME offset a catch would have applied. The
            // object is alive: the exception is still in flight.
            if (stdExceptionDisp >= 0)
                whatText = reinterpret_cast<const std::exception*>(objectPtr + stdExceptionDisp)->what();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }

        if (isBadAlloc)
            return FormatReason("out-of-memory: %s", whatText ? whatText : "std::bad_alloc");
        if (whatText)
            return FormatReason("terminate: %s", whatText);
        if (typeName)
            return FormatReason("terminate: unhandled C++ exception %s", typeName);
        return nullptr;
    }

    // A third party's abort() -- or our own, if anything still reaches one.
    // _set_abort_behavior below has already stripped the banner and the WER
    // hand-off, so this is now the whole of what abort() does.
    void OnAbortSignal(int) noexcept
    {
        SubmitReport({ "terminate: abort() called", nullptr, false, ExitCode::kCrashed });
    }

    void OnInvalidParameter(const wchar_t* expr, const wchar_t* fn, const wchar_t* file,
                            unsigned int line, std::uintptr_t) noexcept
    {
        // The CRT passes nulls in Release (those strings are Debug-only), so
        // format what exists rather than what the signature promises.
        (void)expr;
        const char* reason = FormatReason(
            "crash: CRT invalid parameter in %ls (%ls:%u)",
            fn ? fn : L"?", file ? file : L"?", line);
        SubmitReport({ reason, nullptr, false, ExitCode::kCrashed });
    }

    void OnPureCall() noexcept
    {
        SubmitReport({ "crash: pure virtual function call", nullptr, false, ExitCode::kCrashed });
    }

    // What Install() replaced, so Shutdown() can put every slot back (R2). An
    // Install/Shutdown cycle must leave the process exactly as it found it --
    // the test suite arms and disarms Diagnostics dozens of times and has to
    // keep its OWN fault handling in between.
    using SigHandler = void (*)(int);

    constexpr unsigned int kAbortBehaviorMask = _WRITE_ABORT_MSG | _CALL_REPORTFAULT;

    bool                       g_failFastInstalled = false;
    UINT                       g_prevErrorMode     = 0;
    unsigned int               g_prevAbortBehavior = 0;
    std::terminate_handler     g_prevTerminate     = nullptr;
    SigHandler                 g_prevSigAbrt       = nullptr;
    _invalid_parameter_handler g_prevInvalidParam  = nullptr;
    _purecall_handler          g_prevPureCall      = nullptr;

    void InstallFailFastHandlers(const Config& cfg) noexcept
    {
        if (g_failFastInstalled) return;

        // 1. No OS error box. SEM_NOGPFAULTERRORBOX ONLY when unattended --
        //    UE's rule (LaunchWindows.cpp:211): an interactive run keeps WER
        //    reachable as the backstop for the fail-fasts SEH never sees
        //    (ntdll's __fastfail, /GS cookie failures), while our own filter
        //    terminates before WER's dialog could appear for everything else.
        UINT mode = SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX;
        if (cfg.unattended) mode |= SEM_NOGPFAULTERRORBOX;
        g_prevErrorMode = SetErrorMode(mode);

        // 2. abort() writes no banner and hands nothing to WER; the Debug CRT
        //    reports to stderr instead of a modal box. AFTER THIS LINE NO CRT
        //    DIALOG CAN APPEAR -- which is what makes an unattended run (CI,
        //    the death fixture, a cooked build on a build agent) die in
        //    milliseconds instead of wedging on a box nobody can click.
        g_prevAbortBehavior = _set_abort_behavior(0, kAbortBehaviorMask);
#if defined(_DEBUG)
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
#endif

        // 3. The family proper.
        g_prevTerminate    = std::set_terminate(&OnTerminate);
        g_prevSigAbrt      = std::signal(SIGABRT, &OnAbortSignal);
        g_prevInvalidParam = _set_invalid_parameter_handler(&OnInvalidParameter);
        g_prevPureCall     = _set_purecall_handler(&OnPureCall);

        // 4. Room for the exception filter to run on a stack that has just
        //    overflowed. EXCEPTION_STACK_OVERFLOW needs no handler of its own
        //    -- the filter only signals the crash thread and waits, and the
        //    crash thread has its own 256 KiB stack -- but it does need these
        //    64 KiB below the guard page to get that far.
        ULONG guarantee = 64 * 1024;
        SetThreadStackGuarantee(&guarantee);

        g_failFastInstalled = true;
    }

    void RestoreFailFastHandlers() noexcept
    {
        if (!g_failFastInstalled) return;
        g_failFastInstalled = false;

        SetErrorMode(g_prevErrorMode);
        _set_abort_behavior(g_prevAbortBehavior, kAbortBehaviorMask);
        std::set_terminate(g_prevTerminate);
        // signal() reports FAILURE as SIG_ERR, which is not a handler to put
        // back; anything else (SIG_DFL included) is.
        if (g_prevSigAbrt != SIG_ERR) std::signal(SIGABRT, g_prevSigAbrt);
        _set_invalid_parameter_handler(g_prevInvalidParam);
        _set_purecall_handler(g_prevPureCall);

        g_prevErrorMode     = 0;
        g_prevAbortBehavior = 0;
        g_prevTerminate    = nullptr;
        g_prevSigAbrt      = nullptr;
        g_prevInvalidParam = nullptr;
        g_prevPureCall     = nullptr;
        // The _CrtSetReportMode redirection and the stack guarantee stay: both
        // are harmless on their own, and neither has a "previous value" worth
        // restoring -- a modal CRT box is never what a caller wanted back.
    }
#endif

    // D11 (plan 2, task 8): the stall a hang/gpu-stall report was written for is
    // over. Wakes the attended reporter's waiter, which closes its window --
    // the report stays on disk. Watchdog thread only; Shutdown stops that
    // thread before it closes the handle. The stop-flag test covers the one
    // thread Shutdown does NOT join -- a watchdog orphaned mid-report
    // (StopWatchdog's 5 s bound) must not touch a handle Shutdown is closing.
    void SignalRecovered() noexcept
    {
#if defined(_WIN32)
        if (g_recoveredEvent && !g_watchdogStop.load(std::memory_order_acquire)) SetEvent(g_recoveredEvent);
#endif
    }

    // ONE thread, TWO rules (Task 7). The GPU-progress rule is a sibling loop
    // body here rather than a second thread: it needs the same 250ms cadence,
    // the same debugger suppression, and the same report serialization as the
    // hang rule, and a second thread would buy nothing but a second way for the
    // two to interleave reports for what is usually one incident.
    void WatchdogMain()
    {
#if defined(_WIN32)
        SetThreadDescription(GetCurrentThread(), L"Arcane-HangWatchdog");
        // R6: published so SubmitReport knows a report raised from HERE is
        // about the registered main thread, not about this one.
        g_watchdogThreadId.store(GetCurrentThreadId(), std::memory_order_release);
#elif ARC_PLATFORM_POSIX
        Posix::OnWatchdogThreadStart();
        g_watchdogThreadId.store(Posix::CurrentThreadId(), std::memory_order_release);
#endif
        const auto threshold = static_cast<double>(g_cfg.hangSeconds);

        // The exit sentinel's deadline (spec S5.7). Zero DISABLES it -- a host
        // that says "no deadline" gets none, rather than a deadline of nothing
        // that fires on the first poll after the request.
        const auto exitThreshold = static_cast<double>(g_cfg.exitSeconds);

        // Which beat value we already reported on, so one stall yields one
        // report -- and a NEW stall later still yields another.
        std::int64_t reportedBeat = 0;
        bool         reported     = false;

        // The GPU rule's state, in the extracted+tested form. Same shape --
        // report once, re-arm on progress -- over a VALUE rather than an age.
        ProgressStallRule gpuRule(static_cast<double>(g_cfg.gpuStallSeconds));

        // Main-thread liveness rule. Unchanged in substance; lifted into a
        // lambda only so its early-outs stop being `continue`s that would also
        // skip the GPU rule below.
        const auto checkMainThreadBeat = [&]()
        {
            if (!g_beatSeen.load(std::memory_order_acquire)) return;

            const std::int64_t beat = g_lastBeat.load(std::memory_order_acquire);

            if (reported && beat != reportedBeat)
            {
                reported = false;   // main thread moved again: re-arm
                SignalRecovered();  // D11: a reporter waiting on this stall closes itself
            }

            if (reported) return;
            if (SecondsSince(beat) < threshold) return;

            char msg[160];
            std::snprintf(msg, sizeof(msg), "hang (main thread has not ticked for %.1fs)",
                          SecondsSince(beat));
            SubmitReport({ msg, nullptr, /*lightweight*/false, /*exitCode*/0 });

            reported     = true;
            reportedBeat = beat;
        };

        // GPU-progress rule. The signal it is built on is narrow and stated
        // plainly: the render path is STILL PUBLISHING a counter that has
        // STOPPED MOVING. Both halves matter --
        //
        //   - a frozen counter that nobody is publishing (a minimized host
        //     renders no frames) is no evidence at all, so the freshness gate
        //     below disarms rather than inventing a stall;
        //   - a wedged GPU that has parked the render path in the swapchain's
        //     frame-slot wait IS this rule's primary case, and the reason the
        //     wait polls instead of blocking (NriSwapChain.cpp's
        //     PollingWaitForTimelineFence): the
        //     loop republishes both beats, so the counter stays visibly frozen
        //     under a visibly live render path for as long as the poll window
        //     lasts. Past that window the wait parks for real and publishing
        //     stops, at which point this rule disarms and an ordinary hang
        //     report -- with the main thread's parked stack -- is the honest
        //     remaining signal.
        //
        // What this rule must NEVER claim is a main thread that stopped for its
        // own reasons: an ordinary CPU hang also stops the render path, so
        // "nobody is publishing" has to mean silence here, not a GPU verdict.
        const double gpuThreshold = static_cast<double>(g_cfg.gpuStallSeconds);
        const double gpuFreshness = GpuBeatFreshnessSeconds(gpuThreshold);

        const auto checkGpuProgress = [&]()
        {
            if (!g_gpuBeatSeen.load(std::memory_order_acquire)) return;

            if (SecondsSince(g_lastGpuBeat.load(std::memory_order_acquire)) >= gpuFreshness)
            {
                // Not rendering. Drop the accumulated stall age so a host that
                // resumes starts a fresh clock instead of firing on the gap.
                gpuRule.Reset();
                return;
            }

            const std::uint64_t fence = g_gpuFence.load(std::memory_order_acquire);
            const double        now   = NowSeconds();
            const bool          wasReported = gpuRule.WasReported();
            if (!gpuRule.Poll(fence, now))
            {
                // D11: progress resumed after a gpu-stall report. (A Reset()
                // above -- the render path stopped publishing -- is NOT
                // recovery and deliberately signals nothing.)
                if (wasReported && !gpuRule.WasReported()) SignalRecovered();
                return;
            }

            const double beatAge = g_beatSeen.load(std::memory_order_acquire)
                                 ? SecondsSince(g_lastBeat.load(std::memory_order_acquire))
                                 : -1.0;

            // "gpu-stall" is a CONTRACT, not prose: Diagnostics::DeriveKind
            // reads this string and only a lowercase "gpu" + "stall" yields the
            // .arcdiag kind that makes the GPU-section provider's output mean
            // "the GPU stopped" rather than "the GPU died".
            char msg[224];
            if (beatAge >= 0.0)
            {
                std::snprintf(msg, sizeof(msg),
                              "gpu-stall: GPU progress counter %llu has not advanced for %.1fs "
                              "(main thread last ticked %.1fs ago)",
                              static_cast<unsigned long long>(fence),
                              gpuRule.StalledSeconds(now), beatAge);
            }
            else
            {
                std::snprintf(msg, sizeof(msg),
                              "gpu-stall: GPU progress counter %llu has not advanced for %.1fs "
                              "(main thread never ticked)",
                              static_cast<unsigned long long>(fence),
                              gpuRule.StalledSeconds(now));
            }
            SubmitReport({ msg, nullptr, /*lightweight*/false, /*exitCode*/0 });
        };

        // The exit sentinel (spec S5.7). Not a rule about a beat: once the
        // host has ASKED to quit, "still beating" is no longer evidence of
        // anything -- a teardown legitimately stops pumping frames -- so the
        // only question left is whether it got out, and the only answer is a
        // clock. This is the one report in the module that fires on a host
        // doing nothing wrong right up to the moment it was told to stop.
        const auto checkExitDeadline = [&]()
        {
            if (exitThreshold <= 0.0) return;

            // Belt and braces against the one input that could make this rule
            // fire on nothing: an unset stamp would read as "since the clock's
            // epoch". RequestCleanExit publishes it before the flag, so this
            // is unreachable -- and worth one compare anyway, because the
            // consequence is TerminateProcess.
            const std::int64_t requestedAt = g_exitRequestedAt.load(std::memory_order_acquire);
            if (requestedAt == 0) return;

            const double elapsed = SecondsSince(requestedAt);
            if (elapsed <= exitThreshold) return;

            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "hang at exit (%.1fs after the exit request)", elapsed);
            // FATAL, unlike every other rule on this thread: a host that
            // cannot finish exiting cannot be handed back to either. This call
            // does not return -- SubmitReport terminates once the report is on
            // disk (and terminates anyway if it could not be written).
            SubmitReport({ msg, nullptr, /*lightweight*/false, ExitCode::kExitSentinel });
        };

        while (!g_watchdogStop.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.watchdogPollMs));

#if defined(_WIN32)
            // A breakpoint stops the main thread by design -- and with it every
            // submission, so the GPU counter freezes too. Firing then would
            // train the user to ignore the one signal that matters. Hoisted
            // above both rules for that second reason. It covers the exit
            // deadline too: stepping through a teardown is not a hang at exit.
            if (IsDebuggerPresent()) continue;
#elif ARC_PLATFORM_POSIX
            if (Posix::DebuggerAttached()) continue;   // a ptrace tracer: gdb, lldb, rr
#endif

            // A report is being written RIGHT NOW (the crash thread raises
            // this for the whole body, spec S5.2 step 1). Nothing below may
            // interleave a second one -- including the sentinel, whose report
            // would otherwise terminate the process out from under a hang
            // report that is still on its way to disk.
            if (g_watchdogPaused.load(std::memory_order_acquire)) continue;

            // The exit request REPLACES the beat rules rather than joining
            // them: after it, a frozen main thread is the expected shape of a
            // teardown and reporting it as a hang would be noise.
            if (g_exitRequested.load(std::memory_order_acquire))
            {
                checkExitDeadline();
                continue;
            }

            // The two rules are disjoint by construction -- both signals are
            // published from the main thread, so "still publishing GPU
            // progress" and "has not ticked for hangSeconds" cannot be true at
            // once -- which is why running both every poll costs nothing and
            // cannot double-report one incident. GPU first anyway: the more
            // specific rule ahead of the general one.
            checkGpuProgress();
            checkMainThreadBeat();
        }
    }

#if defined(_WIN32)
    DWORD WINAPI WatchdogThreadProc(LPVOID)
    {
        WatchdogMain();
        return 0;
    }
#endif

#if defined(_WIN32)
    // A watchdog that outlived StopWatchdog's bounded wait (parked mid-report
    // on the crash thread). Kept so StartWatchdog cannot start a SECOND thread
    // beside it -- two watchdogs on one g_watchdogThreadId was plan 1's
    // deferred minor -- and closed once it has actually exited.
    HANDLE g_watchdogOrphan = nullptr;
#endif

    // THE FLAGS ARE CLEARED BY WHOEVER ACTUALLY STARTS A THREAD, never at the
    // top of this function -- and the orphan check runs before them. Plan 2's
    // brief put the two stores first and this block after the early-out; that
    // order RESURRECTS the very thread it then refuses to replace, because the
    // orphan's loop condition is `!g_watchdogStop` (WatchdogMain, below) and
    // `g_watchdogPaused` is what a report parked it on. The orphan would then
    // never exit -- so every later StartWatchdog refuses at the line below and
    // hang detection is off for the rest of the process (the suite alone runs
    // ~15 Install/Shutdown cycles) -- and a hang it did report would walk its
    // OWN stack, since StopWatchdog already zeroed g_watchdogThreadId and
    // WatchdogMain publishes that id once, at entry (R6). Do not "restore" the
    // brief's order.
    void StartWatchdog() noexcept
    {
#if defined(_WIN32)
        if (g_watchdogThread) return;   // already running (Install is idempotent)
        if (g_watchdogOrphan)
        {
            if (WaitForSingleObject(g_watchdogOrphan, 0) != WAIT_OBJECT_0)
            {
                std::fprintf(stderr, "Diagnostics: a previous hang watchdog is still parked; "
                                     "not starting another\n");
                return;
            }
            CloseHandle(g_watchdogOrphan);
            g_watchdogOrphan = nullptr;
        }
        g_watchdogStop.store(false, std::memory_order_release);
        g_watchdogPaused.store(false, std::memory_order_release);
        g_watchdogThread = CreateThread(nullptr, 128 * 1024, &WatchdogThreadProc,
                                        nullptr, 0, nullptr);
#elif ARC_PLATFORM_POSIX
        // A raw pthread with the same orphan rule (Base/Posix/).
        Posix::StartWatchdog(&WatchdogMain);
#endif
    }

    // Stops the watchdog and closes its handle. Idempotent and a NO-OP once
    // the thread is gone, which is what lets both Shutdown() and the atexit
    // hook call it unconditionally.
    void StopWatchdog() noexcept
    {
        g_watchdogStop.store(true, std::memory_order_release);
#if defined(_WIN32)
        if (g_watchdogThread)
        {
            // BOUNDED, unlike the old join. The loop observes the stop flag
            // within its poll (watchdogPollMs) -- the same latency the join had
            // -- but a watchdog that is mid-report may be parked on the crash
            // thread for as long as crashHandlingTimeoutSeconds, and a teardown
            // that blocks on that is a second hang nobody asked for.
            // watchdogJoinTimeoutMs (5 s by default) covers a report that is
            // actually writing. Past it the thread is
            // ORPHANED, not abandoned: the handle moves to g_watchdogOrphan and
            // StartWatchdog refuses to run a second watchdog until it is gone.
            if (WaitForSingleObject(g_watchdogThread, g_cfg.watchdogJoinTimeoutMs) == WAIT_OBJECT_0)
            {
                CloseHandle(g_watchdogThread);
            }
            else
            {
                if (g_watchdogOrphan) CloseHandle(g_watchdogOrphan);
                g_watchdogOrphan = g_watchdogThread;
                std::fprintf(stderr, "Diagnostics: the hang watchdog is still parked mid-report; "
                                     "it will finish on its own\n");
            }
            g_watchdogThread = nullptr;
        }
#elif ARC_PLATFORM_POSIX
        Posix::StopWatchdog();   // bounded, orphaning a watchdog parked mid-report
#endif
        g_watchdogThreadId.store(0, std::memory_order_release);
    }

    // Registered ONCE per process by Install (std::atexit). Two jobs, both of
    // them about a main() that RETURNS without calling Shutdown():
    //
    //   - it stops the watchdog, so the raw thread that exists to outlive a
    //     host's teardown does not outlive the host itself and cannot fire a
    //     sentinel into a process that is already on its way out;
    //   - it publishes "exited cleanly", which is what a console CLOSE
    //     handler is waiting on before it lets Windows terminate us.
    //
    // A no-op when Shutdown() already ran, and safe to run when Install never
    // did (nothing below touches state that must exist).
    //
    // Plan 2, task 9: a main() that returns IS a clean exit, so this is also
    // one of the two places the session record is deleted (D15). Heap-free
    // (DeleteSessionRecord's contract), since this runs at DLL detach.
    void AtExitStopWatchdog() noexcept
    {
        StopWatchdog();
#if defined(_WIN32)
        DeleteSessionRecord();
#endif
        g_exitedCleanly.store(true, std::memory_order_release);
    }

    std::atomic<bool> g_atExitRegistered{false};

#if defined(_WIN32)
    // Whether Install actually registered the console handler, so Shutdown
    // removes exactly what was added (a console-less host adds nothing).
    bool g_consoleHandlerInstalled = false;

    // The rule behind BOTH the real console handler and SimulateConsoleCtrl.
    // UE's shape (WindowsPlatformMisc.cpp): Ctrl-C is two-step, everything
    // else is a session ending on the OS's clock, not ours.
    BOOL WINAPI OnConsoleCtrl(DWORD ctrlType)
    {
        switch (ctrlType)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            // NO HOOK, NO TWO-STEP. Without a host hook there is nothing for
            // the first press to start, so claiming the event would SWALLOW
            // it: the user would press Ctrl-C, watch nothing happen, and have
            // to press again to get a kill. Returning FALSE hands the press
            // back to Windows' default handler, which terminates exactly as
            // it did before this module existed -- the right answer for a
            // console tool that never opted in.
            if (!g_haveCleanExitHook.load(std::memory_order_acquire))
                return FALSE;

            // The same gesture, counted together: the FIRST asks the host to
            // quit the way it knows how, and the SECOND is the user saying
            // that did not work. Not a graceful exit and not pretending to
            // be one -- 0xC000013A is STATUS_CONTROL_C_EXIT, exactly what
            // Windows' own default handler reports.
            if (g_ctrlCPresses.fetch_add(1, std::memory_order_acq_rel) == 0)
            {
                RequestCleanExit();
                return TRUE;
            }
            // Task 9: the user ASKED, twice -- a requested termination, not a
            // death the monitor should report. Deleting the record is how the
            // host says so (D15); without it the monitor would file an
            // abnormal-exit report for STATUS_CONTROL_C_EXIT.
            DeleteSessionRecord();
            TerminateProcess(GetCurrentProcess(), 0xC000013A);
            return TRUE;   // unreachable

        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            // No second chance on these: Windows terminates the process as
            // soon as this returns (and unconditionally after ~5 s). So we
            // start the host's exit and then SPEND that budget rather than
            // returning into a kill -- 4 s, one under the documented cap, in
            // 25 ms steps so a host that finishes early is not held up.
            RequestCleanExit();
            for (int i = 0; i < 160 && !g_exitedCleanly.load(std::memory_order_acquire); ++i)
                Sleep(25);
            // Task 9: the user (or the session) closed us -- whether or not the
            // host's own exit finished inside the budget, Windows ends the
            // process when this returns, and that is not an abnormal exit for
            // the monitor to report. A no-op when Shutdown already deleted it.
            DeleteSessionRecord();
            return TRUE;

        default:
            return FALSE;
        }
    }
#endif

#if defined(_WIN32)
    // ---- off-path preparation ---------------------------------------------
    // Everything below runs on an ORDINARY thread (Install/RetargetDumpDir),
    // where std::filesystem, the heap and the loader lock are all fair game.
    // Their whole job is to leave fixed-storage snapshots behind so the crash
    // thread never has to.

    [[nodiscard]] bool EnvIsSet(const wchar_t* name) noexcept
    {
        wchar_t probe[8];
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetEnvironmentVariableW(name, probe, static_cast<DWORD>(std::size(probe)));
        // n == 0 AND "not found" is the only "unset" answer: a value longer
        // than the probe returns the required size, which is > 0.
        return n > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    }

    void SnapshotReportDir()
    {
        std::string dir = ReportDir().string();
        // operator/ never doubles a separator, and neither may the snprintf
        // that replaces it on the crash path.
        while (dir.size() > 3 && (dir.back() == '\\' || dir.back() == '/'))
            dir.pop_back();
        CopyInto(g_reportDirSnap, sizeof(g_reportDirSnap), dir.c_str());
    }

    // R5. The derived log directory FOLLOWS the report directory; an explicit
    // Config::logDir never moves. Called once at Install and once per
    // retarget (R13: AttachFileSink rotates and replaces, never stacks two
    // sinks on one path).
    void AttachLogSink()
    {
        // R16 (binding): this must NOT bootstrap the engine logger. An
        // earlier cut called Log::Engine() here so a host that installed
        // before Log::Init() still got a log file -- but Log::Init is
        // call_once and THE FIRST CALLER'S LEVEL WINS, so bootstrapping here
        // would silently pin the whole process to the default level and a
        // later Log::Init(debug) would be a no-op. Hosts call Log::Init()
        // before Install (spec S5.1: "beside Log::Init"); when they have
        // not, AttachFileSink refuses, ONE stderr line says so, and the
        // process continues with no file sink and no backlog.
        std::filesystem::path file = g_cfg.logDir.empty()
                                   ? std::filesystem::path(g_reportDirSnap).parent_path() / "Logs"
                                   : std::filesystem::path(g_cfg.logDir);
        file /= (g_cfg.appName.empty() ? std::string("Arcane") : g_cfg.appName) + ".log";

        if (Log::AttachFileSink(file))
        {
            // Log::FileSinkPath() allocates and is unsynchronised, so the
            // envelope's logPath comes from this snapshot, never from it.
            CopyInto(g_logPathSnap, sizeof(g_logPathSnap), file.string().c_str());
        }
        else
        {
            // stderr, NOT ARC_DEBUG: an ARC_ macro here would itself Init the
            // logger, which is the very thing R16 forbids.
            g_logPathSnap[0] = '\0';
            std::fprintf(stderr, "Diagnostics: no engine logger yet; log file sink not attached\n");
        }
    }

    // Why Install refused a configured reporter (S7-SEC), warned once the
    // log sink is attached. Install's thread only.
    std::string g_reporterRefusal;

    void ResolveReporterPath()
    {
        const std::string self = ExecutablePathUtf8();
        const std::filesystem::path exeDir = self.empty() ? std::filesystem::path(".")
                                                          : std::filesystem::path(self).parent_path();
        g_reporterRefusal.clear();
        const std::filesystem::path exe = ReporterExeFor(g_cfg.reporterPath, exeDir, &g_reporterRefusal);

        const std::wstring w = exe.wstring();
        const std::size_t n = w.size() < kPathMax - 1 ? w.size() : kPathMax - 1;
        std::wmemcpy(g_reporterExe, w.c_str(), n);
        g_reporterExe[n] = L'\0';
    }

    // S6-4: the reporter's settings, read from the published snapshot HERE
    // (Install, RetargetDumpDir) so the crash thread only copies a buffer.
    void SnapshotReporterSettingsArgs()
    {
        const std::wstring tail = ReporterSettingsArgs(Settings<DiagnosticsReporterSettings>(),
                                                       Settings<DiagnosticsSettings>().logTailLines,
                                                       Settings<UiSettings>().copyFlashSeconds);
        wcsncpy_s(g_reporterSettingsArgs, tail.c_str(), _TRUNCATE);
    }

    // S6-4 carried gap (Live apply): diagnostics.logTailLines and
    // ui.copyFlashSeconds are Live, so a change re-formats the tail for the
    // NEXT spawn. Publish runs on the main thread; the lock is the one the
    // crash thread holds for a whole report, so a report never reads a
    // half-written buffer and the crash path still only copies it.
    void OnReporterSettingPublished(CVarHandle, void*)
    {
        std::lock_guard<std::recursive_mutex> reportLock(g_reportMutex);
        SnapshotReporterSettingsArgs();
    }

    // S6-5 carried follow-up: attach the callback at every Install and
    // RetargetDumpDir until BOTH cvars carry it -- a cvar not registered yet
    // (an Install that ran before its TU's static init) is retried on the next
    // call instead of being latched as watched. Dist compiles both Dev cvars
    // out, so there nothing ever attaches and Install's snapshot stands.
    std::mutex g_reporterWatchMutex;
    bool       g_reporterWatchAttached[2]{};
    void WatchLiveReporterSettings()
    {
        static constexpr std::string_view kNames[] = { "diagnostics.logTailLines", "ui.copyFlashSeconds" };
        std::lock_guard<std::mutex> watchLock(g_reporterWatchMutex);
        (void)AttachMissingCVarCallbacks(CVarRegistry::Get(), kNames, g_reporterWatchAttached,
                                         &OnReporterSettingPublished, nullptr);
    }

    // ---- monitor mode (plan 2, task 9; spec S5.8, D15/D16) ----------------
    // Everything below runs at Install / RetargetDumpDir time, on an ordinary
    // thread: the heap, nlohmann and std::filesystem are all fine here. Only
    // the DELETION (DeleteSessionRecord, above) runs on the exit paths.

    // The session record (D15; UE's UECrashContext-<pid>.xml,
    // GenericPlatformCrashContext.cpp:877-880, 949-984). Its PRESENCE after
    // the process is gone is what the monitor reads as "died without a clean
    // shutdown"; its contents are what the synthesized report needs. Written
    // to a temp sibling and renamed over the old one, so a host that dies
    // mid-rewrite leaves either the old record or the new one, never half.
    //
    // A rewrite racing a deletion on the console-handler thread cannot
    // resurrect the record: the deleter clears g_sessionRecordLive BEFORE its
    // DeleteFileW, and the writer re-checks the flag AFTER its rename -- so
    // whichever order the two land in, the file is gone at the end.
    //
    // R103 (task 9 fix round 1): returns false when the record could NOT be
    // (re)written -- the old file, if any, is then untouched -- and true when
    // it was written or there was nothing to write (no monitor watching, or
    // the record already deleted by a clean-exit path).
    [[nodiscard]] bool WriteSessionRecord()
    {
        if (!g_sessionPathWide[0] || !g_sessionRecordLive.load(std::memory_order_acquire)) return true;

        // ABSOLUTE paths in the record: a host opened with a relative
        // `--project` retargets to a relative report dir, and the monitor --
        // another process, reading this after the host is gone -- must not
        // have to resolve it against a working directory it merely happened
        // to inherit.
        auto absoluteUtf8 = [](const char* utf8) -> std::string
        {
            if (!utf8 || !*utf8) return {};
            wchar_t wide[kPathMax];
            ToWide(utf8, wide, static_cast<int>(kPathMax));
            std::error_code ec;
            const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(wide), ec);
            const std::wstring w = (ec ? std::filesystem::path(wide) : abs).lexically_normal().wstring();
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string out(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
            return out;
        };

        nlohmann::json doc;
        doc["pid"]            = static_cast<std::uint32_t>(GetCurrentProcessId());
        doc["app"]            = g_appNameSnap;
        doc["product"]        = g_productSnap;
        doc["logPath"]        = absoluteUtf8(g_logPathSnap);
        doc["reportDir"]      = absoluteUtf8(g_reportDirSnap);
        doc["commandLine"]    = g_commandLineSnap;
        doc["hostCreated"]    = g_hostCreated;
        // The D11 event's NAME, so the monitor can ask "is a hang window up
        // for this host that never saw it recover?" without re-deriving the
        // host's spelling of it.
        char recovered[64]{};
        WideCharToMultiByte(CP_UTF8, 0, g_recoveredName, -1, recovered, static_cast<int>(sizeof(recovered)), nullptr, nullptr);
        doc["recoveredEvent"] = g_recoveredEvent ? recovered : "";
        char stamp[48];
        TimestampUtcIso8601(stamp, sizeof(stamp));
        doc["launchedUtc"]    = stamp;
        const std::string text = doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";

        const std::wstring tmp = std::wstring(g_sessionPathWide) + L".tmp";
        const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const bool ok = WriteAll(h, text);
        CloseHandle(h);
        if (!ok || !MoveFileExW(tmp.c_str(), g_sessionPathWide, MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        if (!g_sessionRecordLive.load(std::memory_order_acquire)) DeleteFileW(g_sessionPathWide);
        return true;
    }

    // R98 / R33 (spec S5.8 "waits on the host's process HANDLE"): the monitor
    // is handed an inheritable duplicate of THIS process's handle, and it
    // inherits that handle and nothing else. A pid the monitor opened for
    // itself could name a stranger once this process is gone and the pid is
    // recycled -- a handle cannot. PROC_THREAD_ATTRIBUTE_HANDLE_LIST, not a
    // blanket bInheritHandles: a test harness hands a host its stdout/stderr
    // as inheritable handles, and a monitor that inherited those would hold
    // the harness's capture files (or pipes) open for as long as it lives.
    // STARTF_USESTDHANDLES with null handles keeps the child's std slots
    // empty rather than filled with this process's handle VALUES, which name
    // nothing (or something else) over there.
    void LaunchMonitor()
    {
        if (!g_reporterExe[0] || !g_sessionPathWide[0]) return;

        // R104(b): everything that can THROW (the attribute-list storage, the
        // command line) is built BEFORE the handle exists, so an allocation
        // failure cannot leak it; after DuplicateHandle nothing below throws
        // until CloseHandle(self).
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<unsigned char> attrStorage(attrSize);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
        // The exe and the session path go through QuoteWindowsArg (S7-SEC fix
        // round 1): the session path is derived from diagnostics.dumpDir,
        // which a project may suggest, so it is quoted by the rules the
        // reporter's CommandLineToArgvW reads back, never by a plain wrap.
        const MonitorCommand line = MonitorCommandFor(g_reporterExe, static_cast<unsigned long>(GetCurrentProcessId()),
                                                      g_sessionPathWide, g_cfg.unattended, g_reporterSettingsArgs);
        std::wstring cmd = line.head;
        const std::wstring& cmdTail = line.tail;
        cmd.reserve(cmd.size() + 24 + cmdTail.size());

        HANDLE self = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                             SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, /*inherit*/TRUE, 0))
        {
            ARC_WARN("Diagnostics: could not duplicate the host handle for the crash monitor ({}); abnormal exits will go unreported",
                     GetLastError());
            DeleteSessionRecord();   // nobody will ever reap it
            return;
        }

        // The handle's digits go into the capacity reserved above, and
        // swprintf writes them without allocating (at most 20 digits for a
        // u64); the quoted parts were composed before the handle existed.
        wchar_t digits[24];
        _snwprintf_s(digits, std::size(digits), _TRUNCATE, L"%llu",
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(self)));
        cmd.append(digits).append(cmdTail);

        bool                launched = false;
        DWORD               error    = 0;
        PROCESS_INFORMATION pi{};
        if (InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize))
        {
            if (UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self, sizeof(self), nullptr, nullptr))
            {
                STARTUPINFOEXW si{};
                si.StartupInfo.cb      = sizeof(si);
                si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;   // all three null: see above
                si.lpAttributeList     = attrs;
                launched = CreateProcessW(g_reporterExe, cmd.data(), nullptr, nullptr, /*bInheritHandles*/TRUE,
                                          EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | DETACHED_PROCESS,
                                          nullptr, nullptr, &si.StartupInfo, &pi) != FALSE;
            }
            if (!launched) error = GetLastError();
            DeleteProcThreadAttributeList(attrs);
        }
        else
        {
            error = GetLastError();
        }
        CloseHandle(self);   // the monitor holds its own copy now (or never will)

        if (!launched)
        {
            ARC_WARN("Diagnostics: could not launch the crash monitor ({}); abnormal exits will go unreported", error);
            DeleteSessionRecord();
            return;
        }
        // D14: the first instance may need the foreground to pass on to its
        // respawn (D16), which is the one that shows a window.
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);   // D16: the host keeps no handle to either instance
        g_monitorLaunched = true;
        ARC_INFO("Diagnostics: crash monitor launched (pid {})", pi.dwProcessId);
    }

    // Install's half: fix the record's path (once per process), write it, and
    // launch the monitor (once per process). The record must exist BEFORE the
    // monitor starts -- a monitor that found nothing would read it as a clean
    // exit. Written ONLY when a monitor will read it: a record nobody watches
    // is never reaped and would pile up beside the reports.
    void ArmMonitor()
    {
        if (!g_cfg.launchMonitor || !g_cfg.spawnReporter || IsDebuggerPresent() || !g_reporterExe[0]) return;
        if (!g_sessionPathWide[0])
        {
            char pathUtf8[kPathMax];
            std::snprintf(pathUtf8, sizeof(pathUtf8), "%s\\%s-pid%lu.session",
                          g_reportDirSnap, g_appNameSnap, static_cast<unsigned long>(GetCurrentProcessId()));
            ToWide(pathUtf8, g_sessionPathWide, static_cast<int>(kPathMax));
            if (!g_sessionPathWide[0]) return;
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(g_sessionPathWide).parent_path(), ec);
        g_sessionRecordLive.store(true, std::memory_order_release);
        if (!WriteSessionRecord() || GetFileAttributesW(g_sessionPathWide) == INVALID_FILE_ATTRIBUTES)
        {
            g_sessionRecordLive.store(false, std::memory_order_release);
            ARC_WARN("Diagnostics: could not write the session record; the crash monitor is not launched");
            return;
        }
        if (!g_monitorLaunched) LaunchMonitor();
    }
#endif
}   // namespace

// Kind derivation for the .arcdiag envelope: read off the REASON string,
// never the trigger site. The reason's PREFIX (up to the first ':' or space)
// decides when it names a kind, and only an unrecognised prefix falls back to
// the older substring order -- see DeriveKindCStr above for why the detail
// text must not get a vote. Covers every phrasing the report path writes
// today: the free-form "crash (unhandled exception)" (the crash filter) and
// "hang (main thread has not ticked for ...)" (WatchdogMain), the GPU
// vocabulary from docs/specs/2026-08-11-gpu-crash-diagnostics-design.md
// ("gpu-stall:"/"gpu-crash:") and the crash-window vocabulary the Task 7
// fail-fast handlers write (crash window plan 1): assert/terminate/ensure/
// out-of-memory/abnormal-exit, each the exact word followed by a colon.
std::string DeriveReportKind(const char* reason)
{
    // ONE rule, in DeriveKindCStr above: this is only its std::string skin,
    // for callers that are not on the crash path.
    return DeriveKindCStr(reason);
}

// See the header: a per-thread reason buffer, so no fail-fast handler has to
// touch the crash arena before SubmitReport has copied the reason (I2).
// kReasonMax is deliberately the SAME size as Pending::reason -- a reason
// that fits here fits the report.
const char* FormatReason(const char* fmt, ...) noexcept
{
    static thread_local char t_reason[kReasonMax];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(t_reason, sizeof(t_reason), fmt, args);
    va_end(args);
    t_reason[sizeof(t_reason) - 1] = '\0';   // NUL even on a truncated write
    return t_reason;
}

Config ConfigFromSettings(const DiagnosticsSettings& s)
{
    Config c;
    c.dumpDir                     = s.dumpDir;
    c.hangSeconds                 = s.hangSeconds;
    c.gpuStallSeconds             = s.gpuStallSeconds;
    c.installCrashHandler         = s.installCrashHandler;
    c.startHangWatchdog           = s.hangWatchdog;
    c.reporterPath                = s.reporterPath;
    c.spawnReporter               = s.spawnReporter;
    c.exitSeconds                 = s.exitSeconds;
    c.crashHandlingTimeoutSeconds = s.crashHandlingTimeoutSeconds;
    c.minidumpKind                = s.minidumpKind;
    c.logFlushTimeoutMs           = s.logFlushTimeoutMs;
    c.watchdogPollMs              = s.watchdogPollMs;
    c.watchdogJoinTimeoutMs       = s.watchdogJoinTimeoutMs;
    c.minFatalWaitMs              = s.minFatalWaitMs;
    c.logDir                      = Settings<LogSettings>().dir;
    return c;
}

std::filesystem::path ReporterExeFor(std::string_view configured, const std::filesystem::path& exeDir, std::string* refusal)
{
    const std::filesystem::path bundled = exeDir / "ArcaneCrashReporter.exe";
    if (configured.empty())
        return bundled;
    const std::filesystem::path exe(configured);
    const LaunchPathStatus status = CheckLaunchPath(exe);
    if (status == LaunchPathStatus::Ok)
        return exe;
    if (refusal)
        *refusal = "diagnostics.reporterPath '" + std::string(configured) + "' " + std::string(LaunchPathStatusText(status))
                 + "; the bundled reporter '" + bundled.string() + "' is used instead";
    return bundled;
}

std::filesystem::path ReportDirFor(std::string_view configured, const std::filesystem::path& exeDir, std::string* refusal)
{
    const std::filesystem::path fallback = exeDir / "diagnostics";
    if (configured.empty())
        return fallback;
    if (!HasCommandLineBreaker(configured))
        return std::filesystem::path(std::string(configured));
    if (refusal)
        *refusal = "diagnostics.dumpDir '" + std::string(configured) + "' contains a quote or a line break, which the "
                   "crash reporter's command line cannot carry; reports go to '" + fallback.string() + "' instead";
    return fallback;
}

bool ReporterStemSafe(const char* stemUtf8) noexcept
{
    // std::string_view over a C string does not allocate, and '"', CR and LF
    // are single bytes that never occur inside a UTF-8 multibyte sequence.
    return stemUtf8 && stemUtf8[0] && !HasCommandLineBreaker(stemUtf8);
}

MonitorCommand MonitorCommandFor(const std::wstring& exe, unsigned long pid, const std::wstring& session,
                                 bool unattended, std::wstring_view settingsArgs)
{
    MonitorCommand line;
    line.head = QuoteWindowsArg(exe) + L" --monitor " + std::to_wstring(pid) + L" --host-handle ";
    line.tail = L" --session " + QuoteWindowsArg(session) + (unattended ? L" --unattended" : L"")
              + std::wstring(settingsArgs);
    return line;
}

std::wstring ReporterSettingsArgs(const DiagnosticsReporterSettings& s, std::uint32_t logTailLines, double copyFlashSeconds)
{
    // One flag per ReporterArgs field, in the struct's order. 512 holds the
    // widest line (every number at its maximum digits) with room to spare.
    wchar_t buf[512];
    const int n = std::swprintf(buf, std::size(buf),
        L" --deadline %u --max-frames-thread %u --max-frames-fault %u --max-threads %u --dbgeng-wait-ms %u"
        L" --log-tail %u --hang-log-tail %u --flush-ms %u --ui-poll-ms %u --window %ux%u --window-ready-ms %u --copy-flash %.17g",
        s.deadlineSeconds, s.maxFramesPerThread, s.maxFramesFaultingThread, s.maxThreads, s.dbgengWaitMs,
        logTailLines, s.hangLogTailLines, s.flushTimeoutMs, s.uiPollMs, s.windowWidth, s.windowHeight,
        s.windowReadyMs, copyFlashSeconds);
    return n > 0 ? std::wstring(buf, static_cast<std::size_t>(n)) : std::wstring{};
}

std::wstring CurrentReporterSettingsArgs()
{
#if defined(_WIN32)
    std::lock_guard<std::recursive_mutex> reportLock(g_reportMutex);
    return std::wstring(g_reporterSettingsArgs);
#else
    return {};
#endif
}

bool AttachMissingCVarCallbacks(CVarRegistry& reg, std::span<const std::string_view> names,
                                std::span<bool> attached, void (*fn)(CVarHandle, void*), void* user)
{
    bool all = true;
    for (std::size_t i = 0; i < names.size() && i < attached.size(); ++i)
    {
        if (attached[i]) continue;
        const CVarHandle h = reg.Find(names[i]);
        if (h.IsStale()) { all = false; continue; }
        reg.AddCallback(h, fn, user);
        attached[i] = true;
    }
    return all && names.size() <= attached.size();
}

void Install(const Config& cfg)
{
    if (g_installed.exchange(true, std::memory_order_acq_rel)) return;

    g_cfg = cfg;

    // Arm from a CLEAN slate. Heartbeat()/GpuHeartbeat() set their seen-flags
    // unconditionally -- they are one relaxed store each and deliberately do
    // not test whether diagnostics are installed -- so a beat published before
    // this arming (or by a previous arming that has since shut down) would
    // otherwise be inherited here as a live, and by now ancient, timestamp. The
    // watchdog would then report a hang seconds after boot for something that
    // happened before it existed. Shutdown() clears these too; doing it on BOTH
    // edges is what makes the state per-arming rather than per-process.
    g_beatSeen.store(false, std::memory_order_release);
    g_gpuBeatSeen.store(false, std::memory_order_release);

    // ...and so is the exit-sentinel state, for the same reason plus a
    // sharper one: a clean-exit request inherited from a PREVIOUS arming
    // would arm this one's sentinel against a deadline that has already
    // passed, and the first poll of the new watchdog would terminate a
    // process that has only just started. Shutdown() clears these too.
    g_exitRequested.store(false, std::memory_order_release);
    g_exitRequestedAt.store(0, std::memory_order_release);
    g_exitedCleanly.store(false, std::memory_order_release);
    g_ctrlCPresses.store(0, std::memory_order_release);

    // ONE per process, whatever a host does with Install/Shutdown afterwards
    // (atexit has no deregister, so registering per arming would stack N
    // copies). See AtExitStopWatchdog: this is what makes returning from
    // main() without Shutdown() safe.
    if (!g_atExitRegistered.exchange(true, std::memory_order_acq_rel))
        std::atexit(&AtExitStopWatchdog);

#if defined(_WIN32)
    // FIRST, and gated (R2). Spec S5.1 items 1-4: no OS or CRT dialog can
    // appear after this call, and every non-SEH death has a handler -- which
    // is worth having in place before the lines below (the log sink, the
    // module table, a CSPRNG draw) get a chance to die. Gated on
    // installCrashHandler because this IS the crash-handler family: a host
    // that asked us not to take the fault path (every [diag] case does) keeps
    // its own terminate/abort/CRT handling too.
    if (cfg.installCrashHandler)
        InstallFailFastHandlers(g_cfg);

    g_mainThreadId = GetCurrentThreadId();

    // A build agent must never be left with an interactive process on it --
    // unless it says so: the unattended reporter is bounded by its deadline,
    // and a CI lane that wants the hand-off proven opts in explicitly. The
    // override is UE's -AllowCrashReportClientOnBuildMachine
    // (WindowsPlatformCrashContext.cpp:1053) as an environment variable, and
    // the [diag] cases that need a SPAWNED reporter honour the same three
    // names (CrashPathTest.cpp's SkipIfBuildMachine).
    // Checked here rather than at spawn time so the decision is visible in
    // the "Diagnostics armed" line below.
    if ((EnvIsSet(L"ARCANE_BUILD_MACHINE") || EnvIsSet(L"CI")) && !EnvIsSet(L"ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE"))
        g_cfg.spawnReporter = false;

    // ---- fixed-storage snapshots (spec S5.5 -- the crash path reads only
    // these, because std::string/std::filesystem::path both allocate) ----
    CopyInto(g_appNameSnap, sizeof(g_appNameSnap), g_cfg.appName.c_str());
    CopyInto(g_productSnap, sizeof(g_productSnap),
             g_cfg.productName.empty() ? g_cfg.appName.c_str() : g_cfg.productName.c_str());
    CopyInto(g_commandLineSnap, sizeof(g_commandLineSnap), g_cfg.commandLine.c_str());
    {
        std::lock_guard phaseLock(g_phaseMutex);
        CopyInto(g_phaseSnap, sizeof(g_phaseSnap), g_phase.c_str());
    }
    SnapshotReportDir();
    ToWide(g_productSnap, g_productWide, static_cast<int>(std::size(g_productWide)));
    // The product name comes from the project's manifest and rides the
    // reporter's command line as "--product \"<name>\"" (S7-SEC): a quote, a
    // backslash (it would escape the closing quote) or a line break could add
    // arguments of its own, so each becomes '_'. The reporter only shows it.
    for (wchar_t& c : g_productWide)
        if (c == L'"' || c == L'\\' || c == L'\r' || c == L'\n') c = L'_';
    ResolveReporterPath();
    SnapshotReporterSettingsArgs();   // before ArmMonitor: the monitor's line carries them too
    WatchLiveReporterSettings();      // ...and a Live change re-formats it for later spawns

    // The hang protocol's host half (plan 2, D11/D7), prepared here so the
    // crash thread only ever resets an existing event and prints a number.
    // Manual-reset: the reporter's waiter must see a SetEvent that landed
    // before it started waiting. Created unconditionally -- the [diag] case
    // observes it with the spawn disabled. A failed create leaves the handle
    // null and the protocol simply absent (no --recovered-event on the line).
    _snwprintf_s(g_recoveredName, std::size(g_recoveredName), _TRUNCATE,
                 L"Local\\Arcane-Recovered-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    g_recoveredEvent = CreateEventW(nullptr, /*manualReset*/TRUE, FALSE, g_recoveredName);
    {
        // D7 / spec s9: the reporter compares this against GetProcessTimes on
        // the handle it opened, so a recycled pid can never be terminated.
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            g_hostCreated = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }

    // The engine log file sink, before anything that might want to warn.
    AttachLogSink();
    if (!g_reporterRefusal.empty())
        ARC_WARN("Diagnostics: {}", g_reporterRefusal);
    WarnIfDumpDirRefused();

    // The module table the portable stack resolves against. Unfrozen first:
    // a report interrupted in a previous arming could otherwise have left it
    // frozen, and Refresh is a no-op while it is (R11).
    ModuleTable::SetFrozen(false);
    ModuleTable::Refresh(ForeignModules::EnumerateProcessModules());
    if (const auto scan = ForeignModules::LastScan())
        SnapshotInjectedModules(*scan);

    // One CSPRNG draw, here, so the crash thread never takes its lock (R14's
    // sibling problem: every convenience API on this path allocates or locks).
    g_guidSeed = Guid::Generate();

    // The crash thread and its events come up REGARDLESS of
    // installCrashHandler/startHangWatchdog (R2): they are the report
    // ENGINE, and WriteReport works with no handler installed at all.
    g_crashThreadStop.store(false, std::memory_order_release);
    g_lastStemValid.store(false, std::memory_order_release);
    g_crashEvent   = CreateEventW(nullptr, /*manualReset*/FALSE, FALSE, nullptr);
    // MANUAL-reset: the submitting thread resets it before signalling, so a
    // stale signal from a previous report can never satisfy the next wait.
    g_handledEvent = CreateEventW(nullptr, /*manualReset*/TRUE,  FALSE, nullptr);
    if (g_crashEvent && g_handledEvent)
    {
        DWORD crashTid = 0;
        g_crashThread = CreateThread(nullptr, 256 * 1024, &CrashThreadProc, nullptr, 0, &crashTid);
        if (g_crashThread)
            g_crashThreadId.store(crashTid, std::memory_order_release);
    }

    if (cfg.installCrashHandler)
        g_prevFilter = SetUnhandledExceptionFilter(&OnUnhandledException);

    // Two gates, both load-bearing (R24).
    //
    // UE's rule (WindowsPlatformMisc.cpp) is the second one: only a process
    // that OWNS a console takes the console handler. A windowed host inherits
    // nothing to be Ctrl-C'd, and a GUI process that registered here would sit
    // in the handler for events it can never receive -- its session-end path
    // is WM_QUERYENDSESSION/WM_ENDSESSION, which routes to the same
    // RequestCleanExit from the host's own window procedure (task 9).
    //
    // installCrashHandler is the first, and it is the SAME gate the fail-fast
    // family takes (R2): this IS a process-wide handler, and a host that asked
    // us not to take its death paths keeps its own Ctrl-C too. Without it the
    // test runner -- every [diag] case -- would install one for the whole
    // process. SimulateConsoleCtrl is unaffected either way: it calls the rule
    // directly, which is the point of the seam.
    if (cfg.installCrashHandler && GetConsoleWindow() != nullptr
        && SetConsoleCtrlHandler(&OnConsoleCtrl, TRUE))
    {
        g_consoleHandlerInstalled = true;
    }
#elif ARC_PLATFORM_POSIX
    // The same steps, the POSIX way (Base/Posix/DiagnosticsPosix.cpp): the
    // fatal-signal family, the snapshots, the crash thread, the console
    // signals. Reads g_cfg, already copied above.
    Posix::Install();
#endif

    if (cfg.startHangWatchdog)
        StartWatchdog();

#if defined(_WIN32)
    // Monitor mode (plan 2, task 9; spec S5.8): LAST, once the snapshots the
    // session record carries (app, product, log path, report dir, relaunch
    // line, creation time, the D11 event name) all exist.
    ArmMonitor();
    const bool monitorOn = g_sessionRecordLive.load(std::memory_order_acquire) && g_monitorLaunched;
#else
    const bool monitorOn = false;
#endif

    ARC_INFO("Diagnostics armed (crash handler {}, hang watchdog {} @ {}s, gpu-stall @ {}s, "
             "reporter {}, monitor {}) -> {}",
             cfg.installCrashHandler ? "on" : "off",
             cfg.startHangWatchdog ? "on" : "off",
             cfg.hangSeconds,
             cfg.gpuStallSeconds,
             g_cfg.spawnReporter ? "on" : "off",
             monitorOn ? "on" : "off",
             ReportDir().string());
}

void Shutdown() noexcept
{
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;

    // R1: Shutdown DISARMS the watchdog -- it does not hand it on. The exit
    // sentinel's window is RequestCleanExit() -> Shutdown(), and every host
    // calls Shutdown() as its last line, so the teardown the sentinel exists
    // to name (a host that never gets there) is covered exactly as spec S5.7
    // intends. What the window may NOT be is "forever": a process that
    // disarms and keeps running -- the test binary does this ~20 times, and a
    // tool that installs diagnostics for one job does it once -- would
    // otherwise be terminated with 12 by a thread nobody can see,
    // exitSeconds after any quit request.
    StopWatchdog();

#if defined(_WIN32)
    // Plan 2, task 9 (D15): reaching Shutdown IS the clean exit -- deleting
    // the session record is how this process tells its monitor so.
    DeleteSessionRecord();
#endif

#if defined(_WIN32)
    if (g_consoleHandlerInstalled)
    {
        SetConsoleCtrlHandler(&OnConsoleCtrl, FALSE);
        g_consoleHandlerInstalled = false;
    }

    // R2: Shutdown RESTORES whatever filter was there before us. (What the
    // spec deletes is the CHAIN -- OnUnhandledException never calls the
    // previous filter for our own kinds -- not this restore.)
    if (g_prevFilter)
    {
        SetUnhandledExceptionFilter(g_prevFilter);
        g_prevFilter = nullptr;
    }

    // ...and everything ELSE Install replaced: the terminate handler, SIGABRT,
    // the invalid-parameter and purecall handlers, the error mode. Symmetric
    // with InstallFailFastHandlers, and a no-op when it never ran (R2).
    RestoreFailFastHandlers();

    // The crash thread, and its handles. An Install/Shutdown cycle must
    // leave nothing joinable and no handle open: the event wakes the loop,
    // the stop flag makes it exit instead of reporting, and only then do the
    // handles close.
    if (g_crashThread)
    {
        g_crashThreadStop.store(true, std::memory_order_release);
        SetEvent(g_crashEvent);
        WaitForSingleObject(g_crashThread, INFINITE);
        CloseHandle(g_crashThread);
        g_crashThread = nullptr;
    }
    g_crashThreadId.store(0, std::memory_order_release);
    if (g_crashEvent)   { CloseHandle(g_crashEvent);   g_crashEvent   = nullptr; }
    if (g_handledEvent) { CloseHandle(g_handledEvent); g_handledEvent = nullptr; }

    // The hang protocol's handles (plan 2, task 8) -- the HANDLES only. A
    // reporter mid-symbolization keeps running by design; it holds its own
    // handle to the event, so the named object outlives this close for as
    // long as it needs it. Safe here: the crash thread (SpawnReporter) was
    // joined just above and the watchdog (SignalRecovered) at the top.
    if (g_reporterProcess) { CloseHandle(g_reporterProcess); g_reporterProcess = nullptr; }
    if (g_recoveredEvent)  { CloseHandle(g_recoveredEvent);  g_recoveredEvent  = nullptr; }
#elif ARC_PLATFORM_POSIX
    // Console signals, the fatal-signal family, the snapshot signal and the
    // crash thread -- restored, stopped and joined in the same order.
    Posix::Shutdown();
#endif

    // Neither the module table nor the log backlog may outlive this arming
    // frozen: a survivable report thaws them itself, but a report that timed
    // out (or never reached its crash thread at all) would otherwise silence
    // the logger for the rest of the process (R11).
    ModuleTable::SetFrozen(false);
    Log::ThawBacklog();
    g_watchdogPaused.store(false, std::memory_order_release);

    // Beat state is per-arming: a later Install() must not inherit a stale
    // "already beating" flag from this one. Both triggers, for both reasons --
    // the GPU one would otherwise re-arm against a dead device's last counter.
    g_beatSeen.store(false, std::memory_order_release);
    g_gpuBeatSeen.store(false, std::memory_order_release);

    // The sentinel's window closes HERE (R1), so its state goes with it: the
    // request, its deadline and the Ctrl-C step counter are all per-arming,
    // and a process that Installs again starts from "nobody has asked to
    // quit" rather than inheriting a request the previous arming served.
    g_exitRequested.store(false, std::memory_order_release);
    g_exitRequestedAt.store(0, std::memory_order_release);
    g_ctrlCPresses.store(0, std::memory_order_release);

    // Reaching this line IS the clean exit a console CLOSE handler is waiting
    // on -- the host ran its own teardown and got here -- so release it now
    // instead of burning the rest of the OS's budget. (The atexit hook sets
    // the same flag for a main() that never called Shutdown at all.)
    g_exitedCleanly.store(true, std::memory_order_release);
}

void RetargetDumpDir(const std::filesystem::path& dir)
{
#if defined(_WIN32)
    // S6-5 carried follow-up: retry a watch an early Install could not attach
    // (the registry is main-thread state, as is this call).
    WatchLiveReporterSettings();
#endif
    // Same lock RunReportOnCrashThread holds for its ENTIRE body -- a live
    // retarget from a host's main thread must never race a report the crash
    // thread is mid-way through writing.
    std::lock_guard<std::recursive_mutex> reportLock(g_reportMutex);
    g_cfg.dumpDir = dir.string();
    // A dir holding '"', CR or LF is refused for the default (ReportDirFor,
    // S7-SEC fix round 1); say so once per retarget.
    WarnIfDumpDirRefused();
#if defined(_WIN32)
    // What the session record names until the rewrite below lands (R103).
    const std::string previousReportDir = g_reportDirSnap;
    // The crash path may not touch std::filesystem, so the new directory is
    // re-derived (and created) HERE and only the snapshot travels.
    SnapshotReportDir();
    // S6-4: a project's own diagnostics.reporter.* / logTailLines /
    // copyFlashSeconds rungs are applied by the time a host retargets onto
    // it, so later crash spawns carry them. The monitor keeps its launch line.
    SnapshotReporterSettingsArgs();
    // R5: a DERIVED log directory follows the reports, so a project's log
    // lands beside that project's crash reports. An explicit Config::logDir
    // is host state and never moves.
    if (g_cfg.logDir.empty())
        AttachLogSink();
    // Plan 2, task 9 (D8/D15): the session record now names the new report
    // dir and log -- rewritten IN PLACE (the monitor was told its path at
    // launch); a no-op when no monitor is watching or the record is gone.
    //
    // R103 (task 9 fix round 1): a failed rewrite (an AV scanner or indexer
    // holding the file across the rename, a full disk) is RETRIED once and
    // then reported, never swallowed. On a second failure the OLD record is
    // KEPT on purpose: it still says "no clean exit yet", so the monitor
    // still reports a fail-fast or a kill. Deleting it instead would make the
    // monitor read every later death as a clean exit -- total silence, the
    // worse failure. What the stale record costs is named in the warning: it
    // points the monitor at the OLD report dir, so it cannot see the crash
    // path's own report in the new one and may synthesize a second window.
    if (!WriteSessionRecord())
    {
        ARC_WARN("Diagnostics: could not rewrite the session record for the new report dir; retrying once");
        Sleep(50);
        if (!WriteSessionRecord())
            ARC_WARN("Diagnostics: the session record still names '{}' (not '{}'); a crash from here on may show "
                     "two windows (the crash reporter's and the monitor's abnormal-exit report)",
                     previousReportDir, g_reportDirSnap);
    }
#elif ARC_PLATFORM_POSIX
    Posix::RetargetDumpDir();
#endif
}

void Heartbeat() noexcept
{
    g_lastBeat.store(NowTicks(), std::memory_order_release);
    // Release-store after the beat so the watchdog never sees "beating" with an
    // unset timestamp and fires on a zero.
    g_beatSeen.store(true, std::memory_order_release);
}

void GpuHeartbeat(std::uint64_t fenceValue) noexcept
{
    g_gpuFence.store(fenceValue, std::memory_order_release);
    g_lastGpuBeat.store(NowTicks(), std::memory_order_release);
    // Same ordering discipline as Heartbeat(): arm only after the value is
    // visible, so the watchdog never seeds its rule on a zero the render path
    // never published.
    g_gpuBeatSeen.store(true, std::memory_order_release);
}

void GpuHeartbeatRefresh() noexcept
{
    // Time only. Deliberately does NOT touch g_gpuFence -- there is no new
    // value, and that IS the signal -- and does NOT set g_gpuBeatSeen: a
    // refresh must never arm a rule that has no published value to judge.
    g_lastGpuBeat.store(NowTicks(), std::memory_order_release);
}

bool ProgressStallRule::Poll(std::uint64_t value, double nowSeconds) noexcept
{
    // A first observation only seeds. Reporting here would call every armed
    // watchdog's very first poll a stall.
    if (!m_seeded)
    {
        m_seeded = true;
        m_value  = value;
        m_since  = nowSeconds;
        return false;
    }

    if (value != m_value)
    {
        // Progress: re-arm, and restart the stall clock from HERE rather than
        // from when this value was first seen -- otherwise a counter that moved
        // once after a long quiet period would look instantly stale.
        m_value    = value;
        m_since    = nowSeconds;
        m_reported = false;
        return false;
    }

    if (m_reported) return false;                        // one stall, one report
    if (nowSeconds - m_since < m_stallSeconds) return false;

    m_reported = true;
    return true;
}

double ProgressStallRule::StalledSeconds(double nowSeconds) const noexcept
{
    return m_seeded ? nowSeconds - m_since : 0.0;
}

void ProgressStallRule::Reset() noexcept
{
    m_seeded   = false;
    m_reported = false;
    m_value    = 0;
    m_since    = 0.0;
}

void SetPhase(std::string phase)
{
    std::lock_guard lock(g_phaseMutex);
    g_phase = std::move(phase);
#if defined(_WIN32)
    // The crash path reads the snapshot, never g_phase: copying the
    // std::string out would allocate and would take this same lock, and a
    // report must do neither.
    CopyInto(g_phaseSnap, sizeof(g_phaseSnap), g_phase.c_str());
#elif ARC_PLATFORM_POSIX
    Posix::SnapshotPhase(g_phase.c_str());
#endif
}

std::string WriteReport(const char* reason)
{
    // A SURVIVABLE report: exitCode 0 means "write it and hand the process
    // back", which is exactly what the hang watchdog and the GPU observer
    // need (spec S5.4).
    SubmitReport({ reason, nullptr, /*lightweight*/false, /*exitCode*/0 });
    const std::string stem = LastReportStem();
    return stem.empty() ? std::string{} : stem + ".txt";
}

std::string LastReportStem()
{
#if defined(_WIN32)
    if (!g_lastStemValid.load(std::memory_order_acquire)) return {};
    return std::string(g_lastStem);
#elif ARC_PLATFORM_POSIX
    return Posix::LastReportStem();
#else
    return {};
#endif
}

void SnapshotInjectedModules(std::span<const ForeignModules::Match> matches) noexcept
{
    // Platform-neutral (the storage lives in Internal): every backend's
    // report reads the same snapshot.
    std::lock_guard lock(g_injectedMutex);
    g_injectedScanned = true;
    g_injectedCount   = 0;

    if (matches.empty())
    {
        CopyInto(g_injectedLine, sizeof(g_injectedLine), "none");
        return;
    }

    std::size_t used = 0;
    g_injectedLine[0] = '\0';
    for (const ForeignModules::Match& m : matches)
    {
        if (used + 1 < sizeof(g_injectedLine))
        {
            // A catalogued row names its product; an uncatalogued one (tier
            // 3) has only its path to be known by.
            const int n = std::snprintf(g_injectedLine + used, sizeof(g_injectedLine) - used,
                                        "%s%s (tier %d, %s)",
                                        used == 0 ? "" : ", ",
                                        m.module.c_str(), m.tier,
                                        m.product.empty() ? m.path.c_str() : m.product.c_str());
            if (n > 0)
            {
                used += static_cast<std::size_t>(n);
                if (used >= sizeof(g_injectedLine)) used = sizeof(g_injectedLine) - 1;
            }
        }
        if (g_injectedCount < kInjectedMax)
            CopyInto(g_injectedNames[g_injectedCount++], 64, m.module.c_str());
    }
}

void SubmitReport(const ReportRequest& request) noexcept
{
#if defined(_WIN32)
    const DWORD self = GetCurrentThreadId();

    // Already ON the crash thread: the GPU-section provider or the
    // report-written hook faulted inside a report (spec S9). Signalling
    // ourselves would wait forever, so the nested report runs directly --
    // g_reportMutex is recursive precisely for this.
    if (self != 0 && g_crashThreadId.load(std::memory_order_acquire) == self)
    {
        Pending nested{};
        CopyInto(nested.reason, sizeof(nested.reason),
                 request.reason ? request.reason : "unspecified");
        nested.ep           = static_cast<EXCEPTION_POINTERS*>(request.exceptionPointers);
        nested.walkThreadId = self;
        nested.lightweight  = request.lightweight;
        nested.exitCode     = request.exitCode;
        RunReportOnCrashThread(nested);
        if (request.exitCode != 0)
            TerminateProcess(GetCurrentProcess(), request.exitCode);
        return;
    }

    const bool fatal = (request.exitCode != 0);
    const std::uint32_t timeoutMs = g_cfg.crashHandlingTimeoutSeconds * 1000u;
    // ONE deadline for the lock AND the wait (plan 2, seam 2): plan 1 spent
    // the timeout twice in the worst case -- 60 s behind another submitter's
    // lock, then 60 s more on the crash thread -- and the spec's number is 60.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const auto remainingMs = [&]() noexcept -> DWORD
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        return left > 0 ? static_cast<DWORD>(left) : 0u;
    };
    // R49: ...but a FATAL report never gets a budget of ZERO. A submitter that
    // wins the lock near the deadline would otherwise signal the crash thread,
    // not wait at all, and TerminateProcess it mid-write. Seam 2 exists to stop
    // the 60 + 60 = 120 s worst case, not to hand the report nothing: the floor
    // makes the ceiling 65 s, which keeps the seam's value and still guarantees
    // the crash thread a real chance to finish. Defence in depth -- plan 1's R4
    // writes the MINIMAL envelope (portable stack included) before anything
    // that can wedge, so a kill mid-report already degrades the report rather
    // than leaving an unparsable one. The floor is Config::minFatalWaitMs
    // (diagnostics.minFatalWaitMs, 5 s by default).
    const DWORD minFatalWaitMs = g_cfg.minFatalWaitMs;

    if (fatal)
    {
        // A second faulting thread while a report is in flight (spec S9):
        // it does not queue a report nobody will read -- it waits for the
        // one being written (holding g_submitMutex IS that report) and then
        // dies with its own code, exactly like the first.
        if (g_inCrashHandler.exchange(true, std::memory_order_acq_rel))
        {
            if (g_submitMutex.try_lock_until(deadline))
                g_submitMutex.unlock();
            TerminateProcess(GetCurrentProcess(), request.exitCode);
            return;   // unreachable
        }
    }
    else if (g_inCrashHandler.load(std::memory_order_acquire))
    {
        // A fatal report already owns the process; a survivable one written
        // on top of it would only race the teardown.
        return;
    }

    if (!g_submitMutex.try_lock_until(deadline))
    {
        // The report in flight never finished. Nothing left to do but die
        // with the code we were asked for.
        if (fatal)
            TerminateProcess(GetCurrentProcess(), request.exitCode);
        return;
    }

    {
        std::lock_guard<std::timed_mutex> submitted(g_submitMutex, std::adopt_lock);

        // R11, on THIS thread, before anything else -- UE's GLog->Panic()
        // rule. A lightweight (ensure) report does NOT freeze the backlog:
        // the process continues and its log must keep flowing.
        ModuleTable::SetFrozen(true);
        if (!request.lightweight)
            Log::FreezeBacklog();

        FillPending(request, WalkTargetFor(request));

        if (g_crashThread && g_crashEvent && g_handledEvent)
        {
            DWORD waitMs = remainingMs();
            if (fatal && waitMs < minFatalWaitMs)
                waitMs = minFatalWaitMs;

            ResetEvent(g_handledEvent);
            SetEvent(g_crashEvent);
            WaitForSingleObject(g_handledEvent, waitMs);
        }
        else
        {
            // Diagnostics was never installed (or the crash thread could not
            // be created). Nothing can be written -- but this must not
            // deadlock, and it must not leave the table frozen either.
            ModuleTable::SetFrozen(false);
            if (!request.lightweight)
                Log::ThawBacklog();
        }
    }

    if (request.exitCode != 0)
        TerminateProcess(GetCurrentProcess(), request.exitCode);
#elif ARC_PLATFORM_POSIX
    // The same contract, the same steps -- crash thread, one deadline, a
    // fatal report never returns (Base/Posix/DiagnosticsPosix.cpp).
    Posix::SubmitReport(request);
#else
    // No report path on this platform; the CONTRACT still holds -- a fatal
    // submission ends the process rather than returning into a caller that
    // believes it died.
    if (request.exitCode != 0)
        std::_Exit(request.exitCode);
#endif
}

std::uint32_t ReportCount() noexcept
{
    return g_reportCount.load(std::memory_order_acquire);
}

void NoteGpuDeviceLost() noexcept
{
    g_gpuDeviceLost.store(true, std::memory_order_release);
}

bool GpuDeviceLostNoted() noexcept
{
    return g_gpuDeviceLost.load(std::memory_order_acquire);
}

void GuaranteeStackForThisThread() noexcept
{
#if defined(_WIN32)
    // The SAME 64 KiB InstallFailFastHandlers reserves for the main thread --
    // see step 4 there for why the number is what it is. Deliberately NOT
    // gated on Diagnostics being installed: a worker pool can outlive an
    // Install/Shutdown cycle (the test binary does ~20 of them), the call is
    // idempotent, and a guarantee that is already in place costs nothing to
    // re-request.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
#elif ARC_PLATFORM_POSIX
    // The POSIX reading of the same 64 KiB: a sigaltstack for this thread,
    // so a SIGSEGV from an overflowed stack still has a stack to run on.
    Posix::GuaranteeStackForThisThread();
#endif
}

void SetGpuSectionProvider(GpuSectionProvider provider, void* user) noexcept
{
    std::lock_guard lock(g_gpuProviderMutex);
    g_gpuProvider     = provider;
    g_gpuProviderUser = user;
}

void ClearGpuSectionProvider() noexcept
{
    std::lock_guard lock(g_gpuProviderMutex);
    g_gpuProvider     = nullptr;
    g_gpuProviderUser = nullptr;
}

void FenceReports() noexcept
{
    // Empty critical section, deliberately. RunReportOnCrashThread takes
    // g_reportMutex at the very top of its body and holds it until it
    // returns -- every file write, the GPU-section provider call, and the
    // report-written hook call all happen inside that one lock. Acquiring
    // and immediately releasing the same mutex here therefore cannot return
    // until any report already in flight has fully finished.
    //
    // Unchanged by the report moving to its own thread: the fence's callers
    // (a device destructor) are OTHER threads, where a recursive mutex
    // blocks exactly like a plain one.
    std::lock_guard<std::recursive_mutex> lock(g_reportMutex);
}

void SetReportWrittenHook(ReportWrittenHook hook, void* user) noexcept
{
    std::lock_guard lock(g_reportWrittenMutex);
    g_reportWrittenHook = hook;
    g_reportWrittenUser = user;
}

void ClearReportWrittenHook() noexcept
{
    std::lock_guard lock(g_reportWrittenMutex);
    g_reportWrittenHook = nullptr;
    g_reportWrittenUser = nullptr;
}

void SetCleanExitHook(CleanExitHook hook, void* user) noexcept
{
    std::lock_guard lock(g_cleanExitMutex);
    g_cleanExitHook = hook;
    g_cleanExitUser = user;
    // The console handler's lock-free mirror of "is there one?" -- see
    // g_haveCleanExitHook. Published last, so a handler that sees `true`
    // cannot then read a half-written pair.
    g_haveCleanExitHook.store(hook != nullptr, std::memory_order_release);
}

void RequestCleanExit() noexcept
{
    // Already armed: do NOT restamp the deadline. A host that requests the
    // same exit twice (Ctrl-C, then the window's close box) must not push the
    // sentinel's clock out each time -- that is how a deadline quietly
    // becomes no deadline at all.
    if (g_exitRequested.load(std::memory_order_acquire)) return;

    // The timestamp is published BEFORE the flag that arms it -- Heartbeat()'s
    // ordering discipline, and for the same failure: the watchdog polls these
    // two independently, and "requested" with a still-unset stamp reads as a
    // deadline that expired at the clock's epoch, which would terminate the
    // process with 12 on the very next poll.
    g_exitRequestedAt.store(NowTicks(), std::memory_order_release);

    // ...and the deadline is armed before the HOOK runs, which is the other
    // half of the same idea: a hook that wedges -- the editor's autosave
    // against a dead network drive, say -- is exactly the failure the
    // sentinel exists to name, and arming afterwards could never name it.
    //
    // Two genuinely concurrent first callers both stamp (nanoseconds apart,
    // which no deadline can tell apart) and exactly one wins the exchange and
    // calls the hook; every later caller took the fast path above.
    if (g_exitRequested.exchange(true, std::memory_order_acq_rel)) return;

    CleanExitHook hook = nullptr;
    void*         user = nullptr;
    {
        // Copy the pair out and call UNLOCKED: an unknown callback must never
        // run while holding a lock another thread needs to install one (same
        // rule as the GPU provider and report-written slots).
        std::lock_guard lock(g_cleanExitMutex);
        hook = g_cleanExitHook;
        user = g_cleanExitUser;
    }
    if (hook) hook(user);
}

bool SimulateConsoleCtrl(unsigned long ctrlType) noexcept
{
#if defined(_WIN32)
    return OnConsoleCtrl(static_cast<DWORD>(ctrlType)) != FALSE;
#elif ARC_PLATFORM_POSIX
    return Posix::SimulateConsoleCtrl(ctrlType);
#else
    (void)ctrlType;
    return false;
#endif
}
}   // namespace Arcane::Diagnostics

// =============================================================================
// Structured diagnostics: the publish/sink seam
// =============================================================================
// Separate anonymous namespace + separate statics from the capture module
// above: this slot is the live Problems/Console feed, unrelated to crash/hang
// report state. Named g_sink* (not g_mutex) to keep grep-for-a-static
// unambiguous between the two halves of this file.

namespace Arcane::Diagnostics
{
namespace
{
    // One slot, in Arcane.dll. The mutex guards BOTH the slot and the
    // dispatch: a worker publishing while the main thread swaps sinks (at
    // editor shutdown) must never call a half-torn-down consumer.
    std::mutex g_sinkMutex;
    Sink       g_sink     = nullptr;
    void*      g_sinkUser = nullptr;
}   // namespace

void SetSink(Sink sink, void* user) noexcept
{
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    g_sink     = sink;
    g_sinkUser = user;
}

bool ClearSinkIfCurrent(Sink sink, void* user) noexcept
{
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    if (g_sink != sink || g_sinkUser != user)
        return false;
    g_sink     = nullptr;
    g_sinkUser = nullptr;
    return true;
}

void Publish(std::string_view key, std::span<const Diagnostic> diags)
{
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    if (g_sink)
        g_sink(key, diags, g_sinkUser);
}

void Clear(std::string_view key)
{
    Publish(key, {});
}
}   // namespace Arcane::Diagnostics
