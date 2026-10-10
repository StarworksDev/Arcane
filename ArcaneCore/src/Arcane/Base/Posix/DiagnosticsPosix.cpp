// Base/Posix/DiagnosticsPosix.cpp -- the POSIX backend of the crash/hang
// capture path (Diagnostics POSIX port, 2026-10-05; inventory 2026-10-01
// section E-prime, Phase 4).
//
// Base/Diagnostics.cpp owns the public API, the watchdog's rules, the exit
// sentinel and the envelope JSON; this file is what its Windows half does
// with Win32, done with POSIX, reached through the Internal::Posix entry
// points (DiagnosticsInternal.hpp). The SHAPE is the Windows one, step for
// step, so the two read side by side:
//
//   Windows                               POSIX (here)
//   -------------------------------------  ------------------------------------
//   SetUnhandledExceptionFilter           sigaction SIGSEGV/SIGBUS/SIGFPE/SIGILL/
//                                         SIGABRT, SA_ONSTACK, on a sigaltstack
//   SetThreadStackGuarantee(64 KiB)       a 64 KiB sigaltstack per thread
//   set_terminate / signal(SIGABRT)       set_terminate / the SIGABRT handler
//   _set_invalid_parameter / _purecall    (no such hooks: glibc reports both a
//                                         fortify failure and a pure call
//                                         through abort() / std::terminate)
//   crash thread + two events             crash thread + two pipes (write() and
//                                         poll() are async-signal-safe; an
//                                         event wait is not a thing in a
//                                         signal handler)
//   g_submitMutex (timed_mutex)           an atomic flag polled in 1 ms
//                                         nanosleeps -- a mutex is not
//                                         async-signal-safe, the flag is
//   SuspendThread + GetThreadContext      the snapshot signal: the target
//                                         parks in its handler (PosixCrashSupport)
//   RtlVirtualUnwind walk                 the frame-pointer walk (PortableStack)
//   MiniDumpWriteDump                     a Breakpad-format minidump (MinidumpWriter)
//   SEH guard on the crash thread -> 13   the fatal handler sees it is the
//                                         crash thread -> _exit(13)
//   TerminateProcess(code)                _exit(code)
//   SetConsoleCtrlHandler                 SIGINT/SIGTERM/SIGHUP -> a pipe -> a
//                                         forwarder thread that runs the SAME
//                                         rule (a hook may not run in a
//                                         signal handler)
//   recovered event / monitor             not ported: there is no Linux
//                                         reporter to wait on them yet
//
// The heap discipline is the Windows one: nothing between a fault and the
// files being on disk allocates or takes a lock another thread may hold.

#include <Arcane/Base/DiagnosticsInternal.hpp>
#include <Arcane/Base/Posix/PosixCrashSupport.hpp>

#include <Arcane/Base/Engine.hpp>         // ExecutablePathUtf8(), BuildInfo()
#include <Arcane/Base/ForeignModules.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ModuleTable.hpp>
#include <Arcane/Base/PortableStack.hpp>
#include <Arcane/Guid.hpp>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>
#include <filesystem>
#include <new>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#if ARC_PLATFORM_MACOS
#include <Arcane/Platform/Process.hpp>   // QueryProcess: the host's start stamp
#include <sys/sysctl.h>                  // KERN_PROC: P_TRACED, the debugger check
#endif

extern char** environ;

namespace Arcane::Diagnostics::Internal::Posix
{
namespace
{
    // ---- state --------------------------------------------------------------

    std::uint32_t g_mainThreadId = 0;

    // A FATAL report has claimed the process (latched, never cleared): every
    // later fatal submitter waits for the report in flight, then exits.
    std::atomic<bool> g_inCrashHandler{false};

    // What a fatal signal hands SubmitReport through
    // ReportRequest::exceptionPointers (the POSIX "native fault record").
    struct FaultInfo
    {
        int           signo   = 0;
        int           code    = 0;
        std::uint64_t address = 0;
        NativeContext context{};
    };

    // The one request in flight. Written by the submitter under
    // g_submitBusy, published by `seq`, read (copied) by the crash thread.
    struct Pending
    {
        char          reason[kReasonMax];
        bool          haveFault;
        int           signo;
        int           sigCode;
        std::uint64_t faultAddress;
        bool          haveContext;     // the fault's ucontext, or the submitter's getcontext
        NativeContext context;
        std::uint32_t contextTid;      // whose context that is
        std::uint32_t walkTid;
        bool          lightweight;
        int           exitCode;
        std::uint64_t seq;
    };
    Pending g_pending{};

    std::atomic<std::uint64_t> g_submitSeq{0};
    std::atomic<std::uint64_t> g_completedSeq{0};
    std::atomic<bool>          g_submitBusy{false};

    int  g_wakePipe[2] = { -1, -1 };   // submitter -> crash thread
    int  g_donePipe[2] = { -1, -1 };   // crash thread -> submitter
    pthread_t                  g_crashThread{};
    bool                       g_crashThreadRunning = false;
    std::atomic<std::uint32_t> g_crashThreadId{0};
    std::atomic<bool>          g_crashThreadStop{false};

    char              g_lastStem[kPathMax]{};
    std::atomic<bool> g_lastStemValid{false};

    // Snapshots taken OFF the crash path (Install / RetargetDumpDir /
    // SetPhase); the crash thread reads only these.
    char g_reportDirSnap[kPathMax]{};
    char g_appNameSnap[128]{};
    char g_productSnap[128]{};
    char g_logPathSnap[kPathMax]{};
    char g_commandLineSnap[4096]{};
    char g_phaseSnap[256]{};
    char g_reporterExe[kPathMax]{};
    char g_hostStartSnap[32]{};        // /proc/self/stat starttime (macOS: proc_pidinfo's): --host-created (pid-recycling guard)
    long g_utcOffsetSeconds = 0;       // local time, without localtime_r on the crash path

    Guid g_guidSeed{};

    // Crash-thread scratch that must not live on its stack.
    StackFrame    g_frames[kMaxFrames]{};
    NativeContext g_walkedContext{};
    bool          g_walkedContextValid = false;
    std::size_t   g_walkedFrameCount   = 0;

    // Reporter children: reaped opportunistically (no zombie outlives a
    // report by more than the next one). The hang-protocol one is also the
    // "while it lives, no second hang reporter" gate (D12).
    pid_t g_hangReporterPid = 0;
    pid_t g_reporterPids[16]{};

    // ---- small helpers ------------------------------------------------------

    std::uint32_t Tid() noexcept
    {
        return KernelThreadId();
    }

    std::uint64_t MonotonicMs() noexcept
    {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
    }

    void SleepMs(long ms) noexcept
    {
        timespec ts{ ms / 1000, (ms % 1000) * 1000000L };
        ::nanosleep(&ts, nullptr);
    }

    // A heap-free, snprintf-free appender for the signal handler's reason.
    struct Text
    {
        char*       buf;
        std::size_t cap;
        std::size_t used = 0;
        void Add(const char* s) noexcept { while (s && *s && used + 1 < cap) buf[used++] = *s++; buf[used] = '\0'; }
    };

    const char* SignalName(int signo) noexcept
    {
        switch (signo)
        {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGTRAP: return "SIGTRAP";
        case SIGINT:  return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGHUP:  return "SIGHUP";
        default:      return "signal";
        }
    }

    // Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm): the
    // crash path's gmtime, with no tz lock and no heap.
    void CivilFromEpoch(std::int64_t secs, int& y, unsigned& mo, unsigned& d,
                        unsigned& h, unsigned& mi, unsigned& s) noexcept
    {
        std::int64_t days = secs / 86400;
        std::int64_t rem  = secs % 86400;
        if (rem < 0) { rem += 86400; --days; }
        h  = static_cast<unsigned>(rem / 3600);
        mi = static_cast<unsigned>((rem % 3600) / 60);
        s  = static_cast<unsigned>(rem % 60);
        days += 719468;
        const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(days - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp  = (5 * doy + 2) / 153;
        d  = doy - (153 * mp + 2) / 5 + 1;
        mo = mp < 10 ? mp + 3 : mp - 9;
        y  = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (mo <= 2 ? 1 : 0));
    }

    // ---- envelope / text IO -------------------------------------------------

    bool WriteTwoParts(const char* path, std::string_view a, std::string_view b) noexcept
    {
        const int fd = OpenForWrite(path, /*append*/false);
        if (fd < 0) return false;
        const bool ok = WriteAll(fd, a) && (b.empty() || WriteAll(fd, b));
        CloseFd(fd);
        return ok;
    }

    [[nodiscard]] EnvelopeWrite WriteEnvelope(CrashArena& arena, const EnvFields& f, const char* path) noexcept
    {
        if (const EnvelopeJson full = BuildEnvelopeJson(arena, f, /*elide*/false, kEnvRsv); full.complete)
            return WriteTwoParts(path, full.text, {}) ? EnvelopeWrite::Written : EnvelopeWrite::NotWritten;

        const EnvelopeJson lean = BuildEnvelopeJson(arena, f, /*elide*/true, kEnvLeanRsv);
        if (!lean.complete)
            return EnvelopeWrite::NotWritten;
        return WriteTwoParts(path, lean.text, {}) ? EnvelopeWrite::WrittenElided : EnvelopeWrite::NotWritten;
    }

    [[nodiscard]] std::string_view BuildThreadSection(CrashArena::Builder& b, std::uint32_t tid,
                                                      std::size_t frameCount) noexcept
    {
        char line[512];
        std::snprintf(line, sizeof(line), "--- thread %u%s ---", tid, tid == g_mainThreadId ? " (MAIN)" : "");
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

    void FillHeader(CrashArena::Builder& b, const Pending& p, const char* dmpPath, bool dumpOk,
                    bool exhausted, bool envelopeElided) noexcept
    {
        char line[1024];
        b.Append("=== Arcane diagnostic report ===\n");
        b.Append("reason      : "); b.Append(p.reason[0] ? p.reason : "unspecified"); b.Append("\n");
        b.Append("app         : "); b.Append(g_appNameSnap); b.Append("\n");
        std::snprintf(line, sizeof(line), "pid         : %d\n", static_cast<int>(::getpid()));
        b.Append(line);
        std::snprintf(line, sizeof(line), "main thread : %u\n", g_mainThreadId);
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
        b.Append(dumpOk ? dmpPath : (p.lightweight ? "<not written (lightweight report)>" : "<failed to write>"));
        b.Append("\n");
        b.Append("injected    : "); b.Append(g_rptInjectedLine); b.Append("\n");
        if (p.haveFault)
        {
            std::snprintf(line, sizeof(line), "exception   : %s (signal %d, code %d) at 0x%llx\n",
                          SignalName(p.signo), p.signo, p.sigCode,
                          static_cast<unsigned long long>(p.faultAddress));
            b.Append(line);
        }
        if (exhausted)
            b.Append("arena       : EXHAUSTED -- this report is truncated (spec S5.5)\n");
        if (envelopeElided)
            b.Append("envelope    : ELIDED -- the .arcdiag dropped the stack text and the GPU "
                     "arrays to stay valid JSON; this file is the full stack\n");
        b.Append("\n");
    }

    // ---- the walked thread ----------------------------------------------------

    void OnWalkedThreadParked(std::uint32_t, const NativeContext& uc, void*) noexcept
    {
        // Walk WHILE it is parked: its frame chain holds still.
        CopyContext(g_walkedContext, uc);
        g_walkedContextValid = true;
        g_walkedFrameCount   = CaptureStackFromContext(&g_walkedContext, std::span<StackFrame>(g_frames, kMaxFrames));
    }

    // R6: with a fault (or a submitter's own getcontext) walk that; the crash
    // thread walks itself; anything else is snapshotted (a hang's main thread).
    [[nodiscard]] std::size_t CaptureWalkedStack(const Pending& p) noexcept
    {
        const std::span<StackFrame> out(g_frames, kMaxFrames);
        g_walkedContextValid = false;
        g_walkedFrameCount   = 0;

        if (p.haveContext && p.walkTid == p.contextTid)
        {
            CopyContext(g_walkedContext, p.context);
            g_walkedContextValid = true;
            return CaptureStackFromContext(&g_walkedContext, out);
        }
        if (p.walkTid == Tid())
            return CaptureCurrentStack(out);

        SnapshotThread(p.walkTid, 1000, &OnWalkedThreadParked, nullptr);
        return g_walkedFrameCount;
    }

    bool WriteMiniDump(const char* path, const Pending& p) noexcept
    {
        DumpThread known[1];
        std::size_t knownCount = 0;
        if (p.haveContext) known[knownCount++] = { p.contextTid, &p.context };

        DumpRequest r;
        r.path       = path;
        r.excludeTid = Tid();
        r.known      = known;
        r.knownCount = knownCount;
        r.exceptionTid = p.walkTid;
        if (p.haveFault)
        {
            r.exceptionContext = &p.context;
            r.exceptionCode    = static_cast<std::uint32_t>(p.signo);
            r.exceptionFlags   = static_cast<std::uint32_t>(p.sigCode);
            r.exceptionAddress = p.faultAddress;
        }
        else
        {
            // A snapshot, not a fault (UE's STILL_ACTIVE): the walked thread's
            // context so a reader opens the dump on the thread that matters.
            r.exceptionContext = g_walkedContextValid ? &g_walkedContext : nullptr;
            r.exceptionCode    = kDumpRequested;
            r.exceptionAddress = g_walkedContextValid ? ContextPc(g_walkedContext) : 0;
        }
        return WriteMinidump(r);
    }

    void MakeReportGuid(char* out, std::size_t cap) noexcept
    {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        const std::uint64_t ns = static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
        std::uint64_t hi = g_guidSeed.hi ^ (ns * 0x9E3779B97F4A7C15ull);
        std::uint64_t lo = g_guidSeed.lo
                         ^ ((static_cast<std::uint64_t>(::getpid()) << 32)
                            + (static_cast<std::uint64_t>(g_reportCount.load(std::memory_order_relaxed)) + 1)
                              * 0xBF58476D1CE4E5B9ull);
        hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
        lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
        std::snprintf(out, cap, "%08x-%04x-%04x-%04x-%012llx",
                      static_cast<unsigned>(hi >> 32),
                      static_cast<unsigned>((hi >> 16) & 0xFFFFull),
                      static_cast<unsigned>(hi & 0xFFFFull),
                      static_cast<unsigned>(lo >> 48),
                      static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
    }

    void DumpBacklog(const char* path) noexcept
    {
        const int fd = OpenForWrite(path, /*append*/false);
        if (fd < 0) return;
        char line[Log::kBacklogLineBytes];
        const std::size_t count = Log::BacklogLineCount();
        for (std::size_t i = 0; i < count; ++i)
        {
            std::size_t len = Log::BacklogLine(i, std::span<char>(line, sizeof(line)));
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;
            WriteAll(fd, std::string_view(line, len));
            WriteAll(fd, "\n");
        }
        CloseFd(fd);
    }

    // Plan 2 (D9): a FATAL report's echo bypasses spdlog (the dead thread may
    // hold its sink mutex) -- an O_APPEND write to the log file spdlog has
    // open, and a raw write to fd 2 (not the stdio stream, whose lock is the
    // same hazard).
    void FatalEcho(std::initializer_list<std::string_view> parts) noexcept
    {
        const int log = g_logPathSnap[0] ? OpenForWrite(g_logPathSnap, /*append*/true) : -1;
        for (std::string_view part : parts)
        {
            if (log >= 0) WriteAll(log, part);
            WriteAll(STDERR_FILENO, part);
        }
        if (log >= 0) { WriteAll(log, "\n"); CloseFd(log); }
        WriteAll(STDERR_FILENO, "\n");
    }

    void ReapReporters() noexcept
    {
        for (pid_t& pid : g_reporterPids)
        {
            if (pid > 0 && ::waitpid(pid, nullptr, WNOHANG) == pid) pid = 0;
        }
        if (g_hangReporterPid > 0 && ::waitpid(g_hangReporterPid, nullptr, WNOHANG) == g_hangReporterPid)
            g_hangReporterPid = 0;
    }

    // The hand-off (spec S5.1/S6). Same command line as the Windows spawn,
    // minus --recovered-event (no named event on POSIX). A missing reporter
    // is the EXPECTED case on Linux today: false, one line, files unaffected.
    [[nodiscard]] bool SpawnReporter(const char* stem, const char* kind, bool hangProtocol) noexcept
    {
        if (!g_cfg.spawnReporter) return true;
        if (!g_reporterExe[0] || ::access(g_reporterExe, X_OK) != 0) return false;

        ReapReporters();
        if (hangProtocol && g_hangReporterPid > 0) return true;   // D12: one hang window per host

        static char s_diag[kPathMax + 16];
        static char s_pid[24];
        static char s_kind[32];
        std::snprintf(s_diag, sizeof(s_diag), "%s.arcdiag", stem);
        std::snprintf(s_pid, sizeof(s_pid), "%d", static_cast<int>(::getpid()));
        CopyInto(s_kind, sizeof(s_kind), kind);

        char* argv[16];
        int   argc = 0;
        argv[argc++] = g_reporterExe;
        argv[argc++] = s_diag;
        argv[argc++] = const_cast<char*>("--pid");
        argv[argc++] = s_pid;
        argv[argc++] = const_cast<char*>("--kind");
        argv[argc++] = s_kind;
        argv[argc++] = const_cast<char*>("--product");
        argv[argc++] = g_productSnap;
        argv[argc++] = const_cast<char*>("--host-created");
        argv[argc++] = g_hostStartSnap;
        if (g_cfg.unattended) argv[argc++] = const_cast<char*>("--unattended");
        argv[argc] = nullptr;

        pid_t pid = 0;
        if (::posix_spawn(&pid, g_reporterExe, nullptr, nullptr, argv, environ) != 0)
            return false;
        if (hangProtocol)
        {
            g_hangReporterPid = pid;
        }
        else
        {
            for (pid_t& slot : g_reporterPids)
                if (slot == 0) { slot = pid; break; }
        }
        return true;
    }

    // ---- the report, in UE's order (spec S5.2) -- Diagnostics.cpp's
    // RunReportOnCrashThread, step for step -------------------------------------

    void RunReportOnCrashThread(const Pending& p) noexcept
    {
        std::lock_guard<std::recursive_mutex> reportLock(g_reportMutex);

        // Step 1: no hang report may interleave with this one.
        g_watchdogPaused.store(true, std::memory_order_release);

        CrashArena& arena = CrashArena::Instance();
        arena.Reset();
        g_lastStemValid.store(false, std::memory_order_release);

        CopyInjectedForReport();

        char stamp[32];
        TimeStampForFilename(stamp, sizeof(stamp));

        // "<dir>/<app>-<stamp>-pid<n>": the Windows spelling with '/'. Built
        // with the bounded appender (a path that does not fit is truncated,
        // exactly as the Windows snprintf truncates it).
        char pid[16];
        std::snprintf(pid, sizeof(pid), "%d", static_cast<int>(::getpid()));
        char stem[kPathMax], txtPath[kPathMax], dmpPath[kPathMax];
        char diagPath[kPathMax], tmpPath[kPathMax], logTxtPath[kPathMax];
        {
            Text t{ stem, sizeof(stem) };
            t.Add(g_reportDirSnap); t.Add("/"); t.Add(g_appNameSnap); t.Add("-"); t.Add(stamp); t.Add("-pid"); t.Add(pid);
        }
        const auto sibling = [&stem](char* out, std::size_t cap, const char* ext) noexcept
        {
            Text t{ out, cap };
            t.Add(stem);
            t.Add(ext);
        };
        sibling(txtPath,    sizeof(txtPath),    ".txt");
        sibling(dmpPath,    sizeof(dmpPath),    ".dmp");
        sibling(diagPath,   sizeof(diagPath),   ".arcdiag");
        sibling(tmpPath,    sizeof(tmpPath),    ".arcdiag.tmp");
        sibling(logTxtPath, sizeof(logTxtPath), ".log.txt");

        // Step 2: the portable stack of the walked thread.
        const std::size_t frameCount = CaptureWalkedStack(p);
        CrashArena::Builder header   = arena.OpenBuilder(kHeaderRsv);
        CrashArena::Builder sectionB = arena.OpenBuilder(kSectionRsv);
        const std::string_view section = BuildThreadSection(sectionB, p.walkTid, frameCount);

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
        fields.buildInfo        = BuildInfo();
        fields.cpuThreadSummary = section;
        fields.siblingTxt       = txtPath;
        fields.siblingDmp       = p.lightweight ? "" : dmpPath;
        fields.siblingGpuDump   = "";
        fields.logPath          = g_logPathSnap;
        fields.commandLine      = g_commandLineSnap;
        fields.exitCode         = p.exitCode;
        fields.gpu              = nullptr;

        // Step 3: the MINIMAL envelope, before anything that can wedge.
        const EnvelopeWrite minimalWrite = WriteEnvelope(arena, fields, diagPath);

        // Step 4: the minidump (skipped for a continuable report).
        const bool dumpOk = !p.lightweight && WriteMiniDump(dmpPath, p);

        // Step 5: the .txt (exhaustion sampled HERE -- see the Windows half).
        const bool textTruncated = arena.Exhausted();
        FillHeader(header, p, dmpPath, dumpOk, textTruncated, minimalWrite == EnvelopeWrite::WrittenElided);
        const bool txtOk = WriteTwoParts(txtPath, header.View(), section);

        bool           spawnOk   = true;
        bool           haveGpu   = false;
        EnvelopeWrite  fullWrite = EnvelopeWrite::NotWritten;
        Diag::Envelope gpuEnv;

        if (!p.lightweight)
        {
            // Step 6: the GPU-section provider -- the one step allowed to
            // allocate, after the envelope and the minidump are on disk.
            GpuSectionProvider gpuProvider     = nullptr;
            void*              gpuProviderUser = nullptr;
            {
                std::lock_guard gpuLock(g_gpuProviderMutex);
                gpuProvider     = g_gpuProvider;
                gpuProviderUser = g_gpuProviderUser;
            }
            if (gpuProvider)
            {
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

                if (txtOk)
                {
                    const int fd = OpenForWrite(txtPath, /*append*/true);
                    if (fd >= 0)
                    {
                        WriteAll(fd, "=== GPU ===\n");
                        if (!gpuText.empty()) { WriteAll(fd, gpuText); WriteAll(fd, "\n"); }
                        CloseFd(fd);
                    }
                }
            }

            // Step 7: the FULL envelope, renamed ATOMICALLY over the minimal one
            // -- and only ever with something that parses.
            fields.siblingTxt     = txtOk  ? txtPath : "";
            fields.siblingDmp     = dumpOk ? dmpPath : "";
            fields.siblingGpuDump = haveGpu ? gpuEnv.siblingGpuDump.c_str() : "";
            fields.gpu            = haveGpu ? &gpuEnv : nullptr;
            fullWrite = WriteEnvelope(arena, fields, tmpPath);
            if (fullWrite != EnvelopeWrite::NotWritten)
                ::rename(tmpPath, diagPath);
            else
                ::unlink(tmpPath);

            // Step 8: backlog, bounded flush, hand-off.
            DumpBacklog(logTxtPath);
            Log::FlushFileSinkBounded(2000);
            spawnOk = SpawnReporter(stem, kind, IsHangProtocolReport(kind, p.exitCode));
        }

        g_reportCount.fetch_add(1, std::memory_order_acq_rel);
        CopyInto(g_lastStem, sizeof(g_lastStem), stem);
        g_lastStemValid.store(true, std::memory_order_release);

        // Every file is on disk; the echo and the hook follow (see the
        // Windows half for why a FATAL echo still bypasses spdlog).
        const char* const reasonText = p.reason[0] ? p.reason : "report";
        if (p.exitCode != 0)
        {
            FatalEcho({ "Diagnostics: ", reasonText, " -- report written\n", header.View(), section });
            if (!spawnOk)
                FatalEcho({ "Arcane: crash reporter hand-off failed; the report is at ", stem, ".arcdiag" });
            if (textTruncated)
                FatalEcho({ "Diagnostics: the crash arena was exhausted before the text report was built; '",
                            stem, ".txt' is truncated" });
            if (minimalWrite == EnvelopeWrite::WrittenElided ||
                (!p.lightweight && fullWrite == EnvelopeWrite::WrittenElided))
                FatalEcho({ "Diagnostics: '", stem, ".arcdiag' was ELIDED so it would still parse; '",
                            stem, ".txt' carries the full report" });
            if (!p.lightweight && fullWrite == EnvelopeWrite::NotWritten)
                FatalEcho({ "Diagnostics: the full envelope for '", stem, "' was WITHHELD; the earlier envelope stands" });
        }
        else
        {
            ARC_ERROR("Diagnostics: {} -- report written\n{}{}", reasonText, header.View(), section);
            if (!spawnOk)
            {
                std::fprintf(stderr, "Arcane: crash reporter hand-off failed; the report is at %s.arcdiag\n", stem);
                ARC_WARN("Diagnostics: could not spawn the crash reporter; the report is at '{}.arcdiag'", stem);
            }
            if (textTruncated)
                ARC_WARN("Diagnostics: the crash arena was exhausted before the text report was "
                         "built; '{}.txt' is truncated", stem);
            if (minimalWrite == EnvelopeWrite::WrittenElided ||
                (!p.lightweight && fullWrite == EnvelopeWrite::WrittenElided))
                ARC_WARN("Diagnostics: '{}.arcdiag' was ELIDED (stack text and GPU arrays dropped) "
                         "so it would still parse; '{}.txt' carries the full report", stem, stem);
            if (!p.lightweight && fullWrite == EnvelopeWrite::NotWritten)
                ARC_WARN("Diagnostics: the full envelope for '{}' was WITHHELD -- it would not fit "
                         "the crash arena intact, and replacing a valid envelope with a truncated "
                         "one is never an improvement; the earlier envelope stands", stem);
        }

        ReportWrittenHook reportHook     = nullptr;
        void*             reportHookUser = nullptr;
        {
            std::lock_guard reportHookLock(g_reportWrittenMutex);
            reportHook     = g_reportWrittenHook;
            reportHookUser = g_reportWrittenUser;
        }
        if (reportHook)
            reportHook(std::filesystem::path(diagPath), reportHookUser);

        if (p.exitCode == 0 || p.lightweight)
        {
            ModuleTable::SetFrozen(false);
            if (!p.lightweight) Log::ThawBacklog();
            g_watchdogPaused.store(false, std::memory_order_release);
        }
    }

    // ---- the crash thread -----------------------------------------------------

    void* CrashThreadProc(void*)
    {
        BlockAsyncSignalsOnThisThread();
        NameThisThread("Arcane-CrashRep");
        g_crashThreadId.store(Tid(), std::memory_order_release);

        static Pending s_local;   // ~2 KiB: never on this thread's stack
        std::uint64_t lastSeq = 0;
        for (;;)
        {
            char byte = 0;
            const ssize_t n = ::read(g_wakePipe[0], &byte, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || g_crashThreadStop.load(std::memory_order_acquire))
                break;

            std::atomic_thread_fence(std::memory_order_acquire);
            const std::uint64_t seq = g_submitSeq.load(std::memory_order_acquire);
            if (seq == lastSeq) continue;   // a stale wake: nothing new to write
            lastSeq = seq;

            // COPY, never the global (see the Windows half): a submitter
            // that timed out may already be filling the next one.
            std::memcpy(&s_local, &g_pending, sizeof(Pending));
            if (s_local.haveContext) CopyContext(s_local.context, g_pending.context);
            RunReportOnCrashThread(s_local);

            g_completedSeq.store(s_local.seq, std::memory_order_release);
            const char done = 1;
            (void)!::write(g_donePipe[1], &done, 1);
        }
        return nullptr;
    }

    bool StartCrashThread() noexcept
    {
        if (!MakePipe(g_wakePipe, O_CLOEXEC)) return false;
        if (!MakePipe(g_donePipe, O_CLOEXEC | O_NONBLOCK))
        {
            ::close(g_wakePipe[0]); ::close(g_wakePipe[1]);
            g_wakePipe[0] = g_wakePipe[1] = -1;
            return false;
        }
        g_crashThreadStop.store(false, std::memory_order_release);
        pthread_attr_t attr;
        ::pthread_attr_init(&attr);
        ::pthread_attr_setstacksize(&attr, 512 * 1024);
        g_crashThreadRunning = ::pthread_create(&g_crashThread, &attr, &CrashThreadProc, nullptr) == 0;
        ::pthread_attr_destroy(&attr);
        if (g_crashThreadRunning)
        {
            // Published by the thread itself; wait for it so a fault in the
            // first microseconds still recognizes the crash thread.
            for (int i = 0; i < 1000 && g_crashThreadId.load(std::memory_order_acquire) == 0; ++i) SleepMs(1);
        }
        return g_crashThreadRunning;
    }

    void StopCrashThread() noexcept
    {
        if (g_crashThreadRunning)
        {
            g_crashThreadStop.store(true, std::memory_order_release);
            const char stop = 0;
            (void)!::write(g_wakePipe[1], &stop, 1);
            ::pthread_join(g_crashThread, nullptr);
            g_crashThreadRunning = false;
        }
        g_crashThreadId.store(0, std::memory_order_release);
        for (int* fds : { g_wakePipe, g_donePipe })
        {
            if (fds[0] >= 0) ::close(fds[0]);
            if (fds[1] >= 0) ::close(fds[1]);
            fds[0] = fds[1] = -1;
        }
    }

    // ---- the fatal-signal family (spec S5.1 items 1-4, S5.3) ------------------

#if ARC_PLATFORM_MACOS
    // Apple silicon traps with brk (EXC_BREAKPOINT -> SIGTRAP): __builtin_trap,
    // libc's fortify (__chk_fail) and Swift/ObjC runtime traps all die by it.
    // A debugger takes EXC_BREAKPOINT as a Mach exception before any signal.
    constexpr int kFatalSignals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP };
#else
    constexpr int kFatalSignals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
#endif
    struct sigaction        g_prevFatal[std::size(kFatalSignals)]{};
    bool                    g_fatalInstalled = false;
    std::terminate_handler  g_prevTerminate  = nullptr;

    void OnFatalSignal(int signo, siginfo_t* info, void* ucv)
    {
        // Spec S9: a fault inside the report path ends the process at once.
        if (Tid() == g_crashThreadId.load(std::memory_order_acquire))
            ::_exit(ExitCode::kCrashInCrashPath);

        FaultInfo fault;
        fault.signo   = signo;
        fault.code    = info ? info->si_code : 0;
        fault.address = info ? reinterpret_cast<std::uint64_t>(info->si_addr) : 0;
        if (ucv) CopySignalContext(fault.context, ucv);

        char reason[96];
        Text t{ reason, sizeof(reason) };
        if (signo == SIGABRT)
        {
            // A third party's abort(), a failed C assert(), a glibc fortify
            // or heap-check failure: the Windows OnAbortSignal reason.
            t.Add("terminate: abort() called");
        }
        else
        {
            t.Add("crash (fatal signal ");
            t.Add(SignalName(signo));
            t.Add(")");
        }
        Diagnostics::SubmitReport({ reason, ucv ? &fault : nullptr, false, ExitCode::kCrashed });

        // Unreachable (a fatal SubmitReport exits). Belt and braces: die the
        // way this signal would have killed us.
        ::signal(signo, SIG_DFL);
        ::raise(signo);
    }

    // Shared shape with the Windows family (ActiveExceptionReason there): the
    // in-flight exception decides the kind. libstdc++ and libc++ both
    // __cxa_begin_catch an UNCAUGHT exception before calling terminate, so
    // std::current_exception() sees it here.
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
        Diagnostics::SubmitReport({ ActiveExceptionReason("terminate: (no active exception)"),
                                    nullptr, false, ExitCode::kCrashed });
    }

    void InstallFatalHandlers() noexcept
    {
        if (g_fatalInstalled) return;
        struct sigaction sa{};
        sa.sa_sigaction = &OnFatalSignal;
        sigemptyset(&sa.sa_mask);
        // SA_ONSTACK: a stack overflow's SIGSEGV runs on the sigaltstack.
        // No SA_NODEFER: a second fault of the same kind INSIDE the handler
        // is then a default-action death, never a recursion.
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        for (std::size_t i = 0; i < std::size(kFatalSignals); ++i)
            ::sigaction(kFatalSignals[i], &sa, &g_prevFatal[i]);
        g_prevTerminate = std::set_terminate(&OnTerminate);
        Diagnostics::GuaranteeStackForThisThread();   // the main thread's altstack
        g_fatalInstalled = true;
    }

    void RestoreFatalHandlers() noexcept
    {
        if (!g_fatalInstalled) return;
        g_fatalInstalled = false;
        for (std::size_t i = 0; i < std::size(kFatalSignals); ++i)
            ::sigaction(kFatalSignals[i], &g_prevFatal[i], nullptr);
        std::set_terminate(g_prevTerminate);
        g_prevTerminate = nullptr;
    }

    // ---- the console family (spec S5.7) ----------------------------------------
    // CTRL_* numbers, so SimulateConsoleCtrl takes exactly what it takes on
    // Windows.
    ARC_CONSTANT("wire protocol: CTRL_* values SimulateConsoleCtrl takes, matching the Windows console events")
    constexpr unsigned long kCtrlC = 0, kCtrlBreak = 1, kCtrlClose = 2, kCtrlLogoff = 5, kCtrlShutdown = 6;

    constexpr int    kConsoleSignals[] = { SIGINT, SIGTERM, SIGHUP };
    struct sigaction g_prevConsole[std::size(kConsoleSignals)]{};
    bool             g_consoleInstalled = false;
    int              g_consolePipe[2] = { -1, -1 };
    pthread_t        g_consoleThread{};
    bool             g_consoleThreadRunning = false;

    // Die the way this signal would have killed us before Diagnostics
    // existed -- the "hand the press back to the default handler" of the
    // Windows rule, and the POSIX reading of TerminateProcess(0xC000013A).
    [[noreturn]] void RaiseDefault(int signo) noexcept
    {
        ::signal(signo, SIG_DFL);
        ::kill(::getpid(), signo);   // process-directed: lands on a thread that has it unblocked
        SleepMs(1000);
        ::_exit(128 + signo);
    }

    // The rule behind BOTH the forwarder and SimulateConsoleCtrl -- the
    // Windows OnConsoleCtrl, minus the termination the OS does there.
    bool ConsoleRule(unsigned long ctrlType) noexcept
    {
        switch (ctrlType)
        {
        case kCtrlC:
        case kCtrlBreak:
            // NO HOOK, NO TWO-STEP: decline, and the default disposition ends
            // the process exactly as before (see the Windows rule).
            if (!g_haveCleanExitHook.load(std::memory_order_acquire))
                return false;
            if (g_ctrlCPresses.fetch_add(1, std::memory_order_acq_rel) == 0)
            {
                RequestCleanExit();
                return true;
            }
            RaiseDefault(SIGINT);   // the second press: the user asked twice

        case kCtrlClose:
        case kCtrlLogoff:
        case kCtrlShutdown:
            // Start the host's exit, then spend the same 4 s budget Windows
            // grants before it terminates us.
            RequestCleanExit();
            for (int i = 0; i < 160 && !g_exitedCleanly.load(std::memory_order_acquire); ++i)
                SleepMs(25);
            return true;

        default:
            return false;
        }
    }

    void OnConsoleSignal(int signo)
    {
        const int savedErrno = errno;
        const char byte = static_cast<char>(signo);
        (void)!::write(g_consolePipe[1], &byte, 1);
        errno = savedErrno;
    }

    // A hook may run arbitrary host code, which a signal handler may not:
    // the handler above only writes the signal number down a pipe, and this
    // ordinary thread runs the rule -- the shape of Windows' own console
    // handler, which the OS runs on a thread it creates for the purpose.
    void* ConsoleThreadProc(void*)
    {
        BlockAsyncSignalsOnThisThread();
        NameThisThread("Arcane-Console");
        for (;;)
        {
            char byte = 0;
            const ssize_t n = ::read(g_consolePipe[0], &byte, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || byte == 0) break;
            const int signo = static_cast<unsigned char>(byte);

            if (signo == SIGINT)
            {
                if (!ConsoleRule(kCtrlC)) RaiseDefault(SIGINT);
                continue;
            }

            // SIGTERM/SIGHUP are the session ending (systemd stop, logout, the
            // terminal closing). A process that never installed a clean-exit
            // hook has no exit to start: it dies as it always did, at once --
            // not 4 s later, which is all the Windows rule could add there.
            if (!g_haveCleanExitHook.load(std::memory_order_acquire))
                RaiseDefault(signo);
            ConsoleRule(signo == SIGHUP ? kCtrlClose : kCtrlShutdown);
            // Windows ends the process when its handler returns. Here a host
            // that finished its exit inside the budget leaves on its own; one
            // that did not is ended as the signal would have ended it.
            if (!g_exitedCleanly.load(std::memory_order_acquire))
                RaiseDefault(signo);
        }
        return nullptr;
    }

    void InstallConsoleHandlers() noexcept
    {
        if (g_consoleInstalled) return;
        if (!MakePipe(g_consolePipe, O_CLOEXEC)) return;
        if (::pthread_create(&g_consoleThread, nullptr, &ConsoleThreadProc, nullptr) != 0)
        {
            ::close(g_consolePipe[0]); ::close(g_consolePipe[1]);
            g_consolePipe[0] = g_consolePipe[1] = -1;
            return;
        }
        g_consoleThreadRunning = true;
        struct sigaction sa{};
        sa.sa_handler = &OnConsoleSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        for (std::size_t i = 0; i < std::size(kConsoleSignals); ++i)
            ::sigaction(kConsoleSignals[i], &sa, &g_prevConsole[i]);
        g_consoleInstalled = true;
    }

    void RestoreConsoleHandlers() noexcept
    {
        if (!g_consoleInstalled) return;
        g_consoleInstalled = false;
        for (std::size_t i = 0; i < std::size(kConsoleSignals); ++i)
            ::sigaction(kConsoleSignals[i], &g_prevConsole[i], nullptr);
        if (g_consoleThreadRunning)
        {
            const char stop = 0;
            (void)!::write(g_consolePipe[1], &stop, 1);
            ::pthread_join(g_consoleThread, nullptr);
            g_consoleThreadRunning = false;
        }
        ::close(g_consolePipe[0]); ::close(g_consolePipe[1]);
        g_consolePipe[0] = g_consolePipe[1] = -1;
    }

    // ---- off-path preparation ---------------------------------------------------

    [[nodiscard]] bool EnvIsSet(const char* name) noexcept
    {
        return std::getenv(name) != nullptr;
    }

    void SnapshotReportDir()
    {
        std::string dir = ReportDir().string();
        while (dir.size() > 1 && dir.back() == '/')
            dir.pop_back();
        CopyInto(g_reportDirSnap, sizeof(g_reportDirSnap), dir.c_str());
    }

    // R5 / R16: the Windows AttachLogSink, verbatim in substance.
    void AttachLogSink()
    {
        std::filesystem::path file = g_cfg.logDir.empty()
                                   ? std::filesystem::path(g_reportDirSnap).parent_path() / "Logs"
                                   : std::filesystem::path(g_cfg.logDir);
        file /= (g_cfg.appName.empty() ? std::string("Arcane") : g_cfg.appName) + ".log";
        if (Log::AttachFileSink(file))
        {
            CopyInto(g_logPathSnap, sizeof(g_logPathSnap), file.string().c_str());
        }
        else
        {
            g_logPathSnap[0] = '\0';
            std::fprintf(stderr, "Diagnostics: no engine logger yet; log file sink not attached\n");
        }
    }

    void ResolveReporterPath()
    {
        std::filesystem::path exe;
        if (!g_cfg.reporterPath.empty())
        {
            exe = std::filesystem::path(g_cfg.reporterPath);
        }
        else
        {
            const std::string self = ExecutablePathUtf8();
            exe = (self.empty() ? std::filesystem::path(".") : std::filesystem::path(self).parent_path())
                / Platform::ExecutableFileName("ArcaneCrashReporter");
        }
        CopyInto(g_reporterExe, sizeof(g_reporterExe), exe.string().c_str());
    }

    void SnapshotClock() noexcept
    {
        const time_t now = ::time(nullptr);
        tm local{};
        g_utcOffsetSeconds = ::localtime_r(&now, &local) ? local.tm_gmtoff : 0;
    }

    // starttime (field 22 of /proc/self/stat, clock ticks since boot): with
    // the pid it names this process uniquely, as GetProcessTimes' creation
    // time does on Windows (D7).
    void SnapshotHostStart() noexcept
    {
        g_hostStartSnap[0] = '0';
        g_hostStartSnap[1] = '\0';
#if ARC_PLATFORM_MACOS
        // proc_pidinfo's start time (microseconds since the epoch) -- the
        // same per-OS stamp Arcane::Platform::QueryProcess reports.
        std::snprintf(g_hostStartSnap, sizeof(g_hostStartSnap), "%llu",
                      static_cast<unsigned long long>(Platform::QueryProcess(static_cast<std::uint32_t>(::getpid())).start));
        return;
#endif
        char stat[1024];
        if (ReadFileInto("/proc/self/stat", stat, sizeof(stat)) == 0) return;
        const char* p = std::strrchr(stat, ')');   // the comm field may hold spaces
        if (!p) return;
        int field = 2;
        for (; *p && field < 22; ++p)
            if (*p == ' ') ++field;
        std::size_t n = 0;
        while (p[n] >= '0' && p[n] <= '9' && n + 1 < sizeof(g_hostStartSnap)) { g_hostStartSnap[n] = p[n]; ++n; }
        if (n) g_hostStartSnap[n] = '\0';
    }

    // ---- the watchdog thread ------------------------------------------------------

    pthread_t g_watchdog{};
    bool      g_watchdogRunning = false;
    pthread_t g_watchdogOrphan{};
    bool      g_haveWatchdogOrphan = false;

#if ARC_PLATFORM_MACOS
    // macOS has no pthread_tryjoin_np / pthread_timedjoin_np: each watchdog
    // thread raises its slot's flag as its LAST act, and a join is only
    // attempted once the flag is up (it then returns at once). Two slots:
    // the running watchdog and at most one orphan, which is all the
    // bounded-join rule below ever tracks.
    std::atomic<bool> g_watchdogExited[2]{};
    int               g_watchdogSlot = 0;
    int               g_orphanSlot   = 0;

    int TryJoinWatchdog(pthread_t t, int slot) noexcept
    {
        if (!g_watchdogExited[slot].load(std::memory_order_acquire)) return EBUSY;
        return ::pthread_join(t, nullptr);
    }

    int TimedJoinWatchdog(pthread_t t, int slot, const timespec& deadline) noexcept
    {
        for (;;)
        {
            if (g_watchdogExited[slot].load(std::memory_order_acquire)) return ::pthread_join(t, nullptr);
            timespec now{};
            ::clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
                return ETIMEDOUT;
            const timespec step{ 0, 1000000 };
            ::nanosleep(&step, nullptr);
        }
    }
#endif
    void    (*g_watchdogBody)() = nullptr;

    void* WatchdogThreadProc(void* arg)
    {
        g_watchdogBody();
#if ARC_PLATFORM_MACOS
        g_watchdogExited[reinterpret_cast<std::intptr_t>(arg)].store(true, std::memory_order_release);
#else
        (void)arg;
#endif
        return nullptr;
    }

    // ---- per-thread alternate signal stack ---------------------------------------

    struct AltStack
    {
        void*       mapping = nullptr;
        std::size_t size    = 0;
        ~AltStack()
        {
            if (!mapping) return;
            stack_t off{};
            off.ss_flags = SS_DISABLE;
            ::sigaltstack(&off, nullptr);
            ::munmap(mapping, size);
        }
    };
    thread_local AltStack t_altStack;
}   // namespace

// ============================================================================
// The Internal::Posix entry points (DiagnosticsInternal.hpp)
// ============================================================================

void TimeStampForFilename(char* out, std::size_t cap) noexcept
{
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    int y; unsigned mo, d, h, mi, s;
    CivilFromEpoch(static_cast<std::int64_t>(ts.tv_sec) + g_utcOffsetSeconds, y, mo, d, h, mi, s);
    std::snprintf(out, cap, "%04d%02u%02u-%02u%02u%02u", y, mo, d, h, mi, s);
}

void TimestampUtcIso8601(char* out, std::size_t cap) noexcept
{
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    int y; unsigned mo, d, h, mi, s;
    CivilFromEpoch(static_cast<std::int64_t>(ts.tv_sec), y, mo, d, h, mi, s);
    std::snprintf(out, cap, "%04d-%02u-%02uT%02u:%02u:%02uZ", y, mo, d, h, mi, s);
}

std::uint32_t CurrentThreadId() noexcept
{
    return Tid();
}

void StartWatchdog(void (*body)()) noexcept
{
    if (g_watchdogRunning) return;
    if (g_haveWatchdogOrphan)
    {
#if ARC_PLATFORM_MACOS
        if (TryJoinWatchdog(g_watchdogOrphan, g_orphanSlot) != 0)
#else
        if (::pthread_tryjoin_np(g_watchdogOrphan, nullptr) != 0)
#endif
        {
            std::fprintf(stderr, "Diagnostics: a previous hang watchdog is still parked; not starting another\n");
            return;
        }
        g_haveWatchdogOrphan = false;
    }
    // Cleared by whoever STARTS a thread, after the orphan check -- see the
    // long note on StartWatchdog in Diagnostics.cpp.
    g_watchdogStop.store(false, std::memory_order_release);
    g_watchdogPaused.store(false, std::memory_order_release);
    g_watchdogBody = body;
    pthread_attr_t attr;
    ::pthread_attr_init(&attr);
    ::pthread_attr_setstacksize(&attr, 256 * 1024);
#if ARC_PLATFORM_MACOS
    g_watchdogSlot = g_haveWatchdogOrphan ? 1 - g_orphanSlot : 0;
    g_watchdogExited[g_watchdogSlot].store(false, std::memory_order_release);
    g_watchdogRunning = ::pthread_create(&g_watchdog, &attr, &WatchdogThreadProc,
                                         reinterpret_cast<void*>(static_cast<std::intptr_t>(g_watchdogSlot))) == 0;
#else
    g_watchdogRunning = ::pthread_create(&g_watchdog, &attr, &WatchdogThreadProc, nullptr) == 0;
#endif
    ::pthread_attr_destroy(&attr);
}

void StopWatchdog() noexcept
{
    if (!g_watchdogRunning) return;
    // BOUNDED, as on Windows: a watchdog parked mid-report is orphaned, not
    // waited on for crashHandlingTimeoutSeconds.
    timespec deadline{};
    ::clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
#if ARC_PLATFORM_MACOS
    if (TimedJoinWatchdog(g_watchdog, g_watchdogSlot, deadline) != 0)
#else
    if (::pthread_timedjoin_np(g_watchdog, nullptr, &deadline) != 0)
#endif
    {
        if (g_haveWatchdogOrphan) ::pthread_detach(g_watchdogOrphan);
#if ARC_PLATFORM_MACOS
        g_orphanSlot         = g_watchdogSlot;
#endif
        g_watchdogOrphan     = g_watchdog;
        g_haveWatchdogOrphan = true;
        std::fprintf(stderr, "Diagnostics: the hang watchdog is still parked mid-report; it will finish on its own\n");
    }
    g_watchdogRunning = false;
}

void OnWatchdogThreadStart() noexcept
{
    BlockAsyncSignalsOnThisThread();
    NameThisThread("Arcane-Watchdog");
}

bool DebuggerAttached() noexcept
{
    // IsDebuggerPresent's POSIX reading: a ptrace tracer (gdb, lldb, rr).
#if ARC_PLATFORM_MACOS
    // Apple's documented check (QA1361): P_TRACED in the kinfo_proc.
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid() };
    if (::sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return false;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#endif
    char status[4096];
    if (ReadFileInto("/proc/self/status", status, sizeof(status)) == 0) return false;
    const char* p = std::strstr(status, "TracerPid:");
    if (!p) return false;
    p += 10;
    while (*p == ' ' || *p == '\t') ++p;
    return *p >= '1' && *p <= '9';
}

void Install()
{
    // FIRST, and gated (R2): the fatal-signal family is the crash-handler
    // family; a host that asked us not to take its death paths keeps its own.
    if (g_cfg.installCrashHandler)
        InstallFatalHandlers();

    g_mainThreadId = Tid();

    if ((EnvIsSet("ARCANE_BUILD_MACHINE") || EnvIsSet("CI")) && !EnvIsSet("ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE"))
        g_cfg.spawnReporter = false;

    CopyInto(g_appNameSnap, sizeof(g_appNameSnap), g_cfg.appName.c_str());
    CopyInto(g_productSnap, sizeof(g_productSnap),
             g_cfg.productName.empty() ? g_cfg.appName.c_str() : g_cfg.productName.c_str());
    CopyInto(g_commandLineSnap, sizeof(g_commandLineSnap), g_cfg.commandLine.c_str());
    {
        std::lock_guard phaseLock(g_phaseMutex);
        CopyInto(g_phaseSnap, sizeof(g_phaseSnap), g_phase.c_str());
    }
    SnapshotReportDir();
    ResolveReporterPath();
    SnapshotClock();
    SnapshotHostStart();
    SnapshotSystemInfoForDump();

    AttachLogSink();

    ModuleTable::SetFrozen(false);
    ModuleTable::Refresh(ForeignModules::EnumerateProcessModules());
    if (const auto scan = ForeignModules::LastScan())
        Diagnostics::SnapshotInjectedModules(*scan);

    g_guidSeed = Guid::Generate();

    // The report ENGINE, regardless of installCrashHandler (R2): the crash
    // thread, and the snapshot signal a hang report walks the main thread with.
    g_lastStemValid.store(false, std::memory_order_release);
    InstallSnapshotSignal();
    StartCrashThread();

    // The console family takes the crash-handler gate (R24) and nothing
    // else: unlike a Windows console, every POSIX process can be sent
    // SIGINT/SIGTERM/SIGHUP, windowed or not.
    if (g_cfg.installCrashHandler)
        InstallConsoleHandlers();

    if (g_cfg.launchMonitor && g_cfg.spawnReporter)
        std::fprintf(stderr, "Diagnostics: the crash monitor is not available on this platform yet; "
                             "abnormal exits the crash path never sees go unreported\n");
}

void Shutdown() noexcept
{
    RestoreConsoleHandlers();
    RestoreFatalHandlers();
    StopCrashThread();
    RemoveSnapshotSignal();
    ReapReporters();
}

void RetargetDumpDir()
{
    SnapshotReportDir();
    if (g_cfg.logDir.empty())
        AttachLogSink();
}

void SnapshotPhase(const char* phase) noexcept
{
    CopyInto(g_phaseSnap, sizeof(g_phaseSnap), phase);
}

std::string LastReportStem()
{
    if (!g_lastStemValid.load(std::memory_order_acquire)) return {};
    return std::string(g_lastStem);
}

void SubmitReport(const ReportRequest& request) noexcept
{
    const std::uint32_t self = Tid();
    const auto* fault = static_cast<const FaultInfo*>(request.exceptionPointers);

    // Already ON the crash thread (the GPU provider or the hook asserted):
    // run the nested report directly; g_reportMutex is recursive for this.
    if (self != 0 && g_crashThreadId.load(std::memory_order_acquire) == self)
    {
        static Pending s_nested;
        std::memset(&s_nested, 0, sizeof(s_nested));
        CopyInto(s_nested.reason, sizeof(s_nested.reason), request.reason ? request.reason : "unspecified");
        s_nested.walkTid     = self;
        s_nested.lightweight = request.lightweight;
        s_nested.exitCode    = request.exitCode;
        if (fault)
        {
            s_nested.haveFault = true; s_nested.signo = fault->signo; s_nested.sigCode = fault->code;
            s_nested.faultAddress = fault->address; s_nested.haveContext = true; s_nested.contextTid = self;
            CopyContext(s_nested.context, fault->context);
        }
        RunReportOnCrashThread(s_nested);
        if (request.exitCode != 0)
            ::_exit(request.exitCode);
        return;
    }

    const bool fatal = (request.exitCode != 0);
    const std::uint64_t timeoutMs = g_cfg.crashHandlingTimeoutSeconds != 0
                                  ? std::uint64_t(g_cfg.crashHandlingTimeoutSeconds) * 1000u : 60000u;
    // ONE deadline for the lock AND the wait (plan 2, seam 2); R49's 5 s
    // floor for a fatal report's wait.
    const std::uint64_t deadline = MonotonicMs() + timeoutMs;
    ARC_CONSTANT("crash-path: floor under a fatal report's wait, matching the Windows path")
    constexpr std::uint64_t kMinFatalWaitMs = 5000;

    if (fatal)
    {
        if (g_inCrashHandler.exchange(true, std::memory_order_acq_rel))
        {
            // A second fatal submitter (spec S9): wait for the report in
            // flight, then die with its own code.
            while (g_submitBusy.load(std::memory_order_acquire) && MonotonicMs() < deadline)
                SleepMs(1);
            ::_exit(request.exitCode);
        }
    }
    else if (g_inCrashHandler.load(std::memory_order_acquire))
    {
        return;
    }

    // The submission lock: an atomic flag, because this runs in signal
    // handlers, where a mutex is not safe to take.
    for (;;)
    {
        bool expected = false;
        if (g_submitBusy.compare_exchange_weak(expected, true, std::memory_order_acq_rel)) break;
        if (MonotonicMs() >= deadline)
        {
            if (fatal) ::_exit(request.exitCode);
            return;
        }
        SleepMs(1);
    }

    // R11, on THIS thread, before anything else.
    ModuleTable::SetFrozen(true);
    if (!request.lightweight)
        Log::FreezeBacklog();

    // R3: the reason is COPIED here, on the submitting thread.
    Pending& p = g_pending;
    CopyInto(p.reason, sizeof(p.reason), request.reason ? request.reason : "unspecified");
    p.haveFault   = fault != nullptr;
    p.signo       = fault ? fault->signo : 0;
    p.sigCode     = fault ? fault->code : 0;
    p.faultAddress= fault ? fault->address : 0;
    p.lightweight = request.lightweight;
    p.exitCode    = request.exitCode;
    p.contextTid  = self;

    // R6: a fault walks itself; a report raised by the watchdog is about the
    // MAIN thread; anything else walks the caller.
    if (fault)
        p.walkTid = self;
    else if (g_mainThreadId != 0 && g_watchdogThreadId.load(std::memory_order_acquire) == self)
        p.walkTid = g_mainThreadId;
    else
        p.walkTid = self;

    if (fault)
    {
        CopyContext(p.context, fault->context);
        p.haveContext = true;
    }
    else
    {
        // Captured HERE, in the frame that is about to park in the wait
        // below, so its frame chain stays intact for the crash thread's walk
        // (the Windows half suspends this thread for the same picture).
        p.haveContext = CaptureOwnContext(p.context);
    }

    const std::uint64_t seq = g_submitSeq.load(std::memory_order_relaxed) + 1;
    p.seq = seq;
    std::atomic_thread_fence(std::memory_order_release);
    g_submitSeq.store(seq, std::memory_order_release);

    if (g_crashThreadRunning && g_wakePipe[1] >= 0)
    {
        std::uint64_t waitUntil = deadline;
        if (fatal && waitUntil < MonotonicMs() + kMinFatalWaitMs)
            waitUntil = MonotonicMs() + kMinFatalWaitMs;

        const char wake = 1;
        (void)!::write(g_wakePipe[1], &wake, 1);
        while (g_completedSeq.load(std::memory_order_acquire) < seq)
        {
            const std::uint64_t now = MonotonicMs();
            if (now >= waitUntil) break;
            pollfd pfd{ g_donePipe[0], POLLIN, 0 };
            const std::uint64_t left = waitUntil - now;
            ::poll(&pfd, 1, static_cast<int>(left > 250 ? 250 : left));   // EINTR: just loop
            char drain[16];
            while (::read(g_donePipe[0], drain, sizeof(drain)) > 0) {}
        }
    }
    else
    {
        // Never installed: nothing written, nothing left frozen, no deadlock.
        ModuleTable::SetFrozen(false);
        if (!request.lightweight)
            Log::ThawBacklog();
    }

    g_submitBusy.store(false, std::memory_order_release);

    if (request.exitCode != 0)
        ::_exit(request.exitCode);
}

void GuaranteeStackForThisThread() noexcept
{
    if (t_altStack.mapping) return;
    // A thread that already runs with someone else's altstack keeps it.
    stack_t current{};
    if (::sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE))
        return;

    const long page = ::sysconf(_SC_PAGESIZE);
    const std::size_t pageSize = page > 0 ? static_cast<std::size_t>(page) : 4096u;
    const std::size_t usable   = 64 * 1024;
    const std::size_t total    = usable + pageSize;   // plus a guard page below it
    void* mapping = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return;
    ::mprotect(mapping, pageSize, PROT_NONE);

    stack_t ss{};
    ss.ss_sp    = static_cast<char*>(mapping) + pageSize;
    ss.ss_size  = usable;
    ss.ss_flags = 0;
    if (::sigaltstack(&ss, nullptr) != 0)
    {
        ::munmap(mapping, total);
        return;
    }
    t_altStack.mapping = mapping;
    t_altStack.size    = total;
}

bool SimulateConsoleCtrl(unsigned long ctrlType) noexcept
{
    return ConsoleRule(ctrlType);
}
}   // namespace Arcane::Diagnostics::Internal::Posix
