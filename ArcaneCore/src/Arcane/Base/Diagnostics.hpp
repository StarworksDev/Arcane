#pragma once

// Engine diagnostics (Base module): the post-mortem capture seam, sibling to
// Log.hpp and Assert.hpp. Two triggers, ONE report path:
//
//   1. CRASH -- SetUnhandledExceptionFilter. An access violation or an escaped
//      C++ exception writes a report instead of dying mute.
//   2. HANG  -- a watchdog thread. The main thread publishes a Heartbeat(); if
//      it stops advancing for longer than Config::hangSeconds, the watchdog
//      writes the SAME report for a process that is still very much alive.
//
// (2) is the reason this module exists. Windows Error Reporting's LocalDumps
// covers (1) already and is a fine backstop, but WER only ever fires on process
// DEATH -- a wedged main thread ("Not Responding") produces nothing, anywhere,
// forever. Neither does a debugger help much when the defect is intermittent:
// it requires a human attached at the exact moment. The watchdog does not.
//
// A report is written on a DEDICATED CRASH THREAD (crash window plan 1, task
// 5; spec S5.2), never on the thread that faulted -- that thread only freezes
// the log backlog, hands over the fault context, waits, and exits. The steps
// run in UE's order, each independently survivable:
//
//   1. the watchdog is paused so no hang report interleaves;
//   2. the portable stack of the WALKED thread (RtlVirtualUnwind against
//      ModuleTable -- no DbgHelp, no loader lock, so no symbol load can wedge
//      a process that is already misbehaving);
//   3. a MINIMAL .arcdiag envelope, so a reporter can start even if
//      everything after this step wedges;
//   4. the MINIDUMP (.dmp) -- the artifact a debugger opens;
//   5. the TEXT report (.txt): header + `module + 0xoffset` frames. It is no
//      longer symbolized and no longer covers every thread: both were the
//      in-process freeze this arc exists to remove, and the reporter
//      reconstructs them out of process from the minidump;
//   6. the GPU-section provider (DRED, device fault), unchanged;
//   7. the FULL envelope, rewritten atomically over the minimal one;
//   8. the log backlog as `<stem>.log.txt`, a bounded file-sink flush, and
//      the reporter hand-off.
//
// NOTHING on that path may touch the heap (the heap may be what just faulted):
// the arena (Base/CrashArena.hpp) is the only allocator, the envelope JSON is
// hand-written through it, and every string the report needs is snapshotted
// into fixed storage OFF the crash path (at Install/RetargetDumpDir). The two
// documented exceptions both run only after files are already on disk: the
// GPU-section provider, and the ARC_ERROR echo + report-written hook.
//
// Reading it needs no debugger, which is the whole point -- the text file is
// the artifact you paste into a bug report.
//
// Platforms: Windows (SEH, MiniDumpWriteDump; the backend inline in
// Diagnostics.cpp) and Linux (fatal signals on a sigaltstack, a frame-pointer
// walk, a Breakpad-format minidump as the `.dmp`; Base/Posix/, reached through
// Base/DiagnosticsInternal.hpp). The Linux backend has no crash reporter or
// monitor to hand off to yet (ArcaneCrashReporter is Windows-only): the
// hand-off reports "failed" and launchMonitor is ignored with one line. On
// Linux `ReportRequest::exceptionPointers` is the backend's own fault record
// (only its signal handler passes one), and the console handler is
// SIGINT/SIGTERM/SIGHUP. Every other platform compiles and no-ops.

#include <Arcane/Core/Api.hpp>
#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/DiagnosticsSettingsData.hpp>
#include <Arcane/Base/ReporterSettingsData.hpp>
#include <Arcane/Base/ForeignModules.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Config/CVarHandle.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <Arcane/Core/Constant.hpp>

namespace Arcane { class CVarRegistry; }

namespace Arcane::Diagnostics
{
    // Every tunable below takes its default from DiagnosticsSettings (settings
    // arc S6-2: the diagnostics.* cvars) -- there is no second default. A host
    // builds its Config with ConfigFromSettings after the early config rungs;
    // the identity fields (appName, productName, unattended, launchMonitor,
    // commandLine) stay the host's.
    struct Config
    {
        // Names the report files. Set it per host ("ArcaneEditor").
        std::string appName = "Arcane";

        // Where reports land. Empty => "<exe dir>/diagnostics". Chosen over the
        // CWD on purpose: hosts are documented as cd-then-run, and a report that
        // lands wherever the user happened to be is a report nobody finds.
        std::string dumpDir;

        // Main-thread stall that counts as a hang. Generous by default: a cold
        // shader compile or a large project scan can legitimately block the main
        // thread for seconds, and a false report that cries wolf gets ignored.
        std::uint32_t hangSeconds = ::Arcane::Detail::DiagnosticsDefaults().hangSeconds;

        // GPU-progress stall that counts as a GPU hang (GpuHeartbeat below).
        //
        // The rule fires only while the render path is DEMONSTRABLY ALIVE and
        // the GPU-progress counter has stopped moving -- it disarms outright
        // when publishing stops, because a frozen counter nobody is publishing
        // is a host that is not rendering (minimized), not a stalled GPU. The
        // case that makes it reachable is the swapchain's frame-slot wait,
        // which polls and republishes rather than blocking (see
        // Render/Nri/NriSwapChain.cpp's PollingWaitForTimelineFence)
        // precisely so a wedged GPU is visible as "still waiting, still not
        // retiring".
        //
        // TIGHTER than hangSeconds on purpose, and this is the whole reason
        // both numbers exist -- though NOT as a race between two live rules.
        // The frame-slot wait keeps the main-thread beat flowing, so the hang
        // rule cannot fire during it at all; it only becomes possible once the
        // wait gives up polling and parks, and then only hangSeconds after
        // that. 8 < 12 is what puts the GPU verdict INSIDE the polling window,
        // so a GPU that stopped retiring is named while the evidence is still
        // being published, rather than surfacing much later as a main-thread
        // hang it is not.
        //
        // What it does NOT buy: a GPU-section in the report. The provider runs
        // for EVERY report kind (see WriteReportImpl), so a plain `hang` already
        // carries markers/DRED/device-fault. What the kind changes is the CLAIM
        // the report makes about the cause -- "gpu-stall" vs "hang" -- and which
        // of the two a reader should believe.
        std::uint32_t gpuStallSeconds = ::Arcane::Detail::DiagnosticsDefaults().gpuStallSeconds;

        // Gates SetUnhandledExceptionFilter ONLY (and, from task 7, the
        // fail-fast family). The crash thread and its events are created by
        // Install either way: they are the report ENGINE, not a handler, and
        // WriteReport/the watchdog need them with no handler installed at all.
        bool installCrashHandler = ::Arcane::Detail::DiagnosticsDefaults().installCrashHandler;
        bool startHangWatchdog   = ::Arcane::Detail::DiagnosticsDefaults().hangWatchdog;

        // ---- crash window plan 1 (spec S5.1) ------------------------------

        // Window title the reporter shows. Empty => appName.
        std::string productName;

        // The reporter executable. Empty => "<exe dir>/ArcaneCrashReporter.exe".
        // Missing is the expected case until the reporter itself ships: the
        // spawn fails, says so once, and the report on disk is unaffected.
        std::string reporterPath;

        // Headless host: the reporter must put no window anywhere (spec S9,
        // "Headless / unattended").
        bool unattended = false;

        // Whether to hand off to the reporter at all. Install FORCES this
        // false when ARCANE_BUILD_MACHINE or CI is set in the environment --
        // a build agent must never leave an interactive process behind.
        bool spawnReporter = ::Arcane::Detail::DiagnosticsDefaults().spawnReporter;

        // Pre-launch the reporter in MONITOR mode (spec S5.8): it waits on this
        // process and turns a death the crash path never saw (a __fastfail,
        // /GS, heap corruption, a stack overflow with no room for SEH, an
        // external kill) into an abnormal-exit report. Windowed hosts set it
        // to !headless; headless and server hosts leave it off. Ignored when
        // spawnReporter is false (build machine) or a debugger is attached.
        //
        // What the monitor reads is the SESSION RECORD (plan 2, D15; UE's
        // UECrashContext-<pid>.xml): "<Install-time report dir>/<app>-pid<n>.session",
        // written by Install only when the monitor is actually launched,
        // rewritten IN PLACE by RetargetDumpDir (its contents name the new
        // report dir; its path never moves -- the monitor was told that path),
        // and deleted by Shutdown(), the atexit hook, and the console
        // handler's user-requested terminations. A record left behind when
        // the process is gone is what "abnormal" means; the exit code only
        // names the reason.
        bool launchMonitor = false;

        // The sanitized relaunch command line, carried in the envelope so the
        // reporter can offer "restart". Never derived here: only the host
        // knows which of its own arguments are safe to repeat.
        std::string commandLine;

        // Where the engine log file sink writes. Empty => "<report dir>/../Logs",
        // which FOLLOWS RetargetDumpDir; an explicit path never moves. The
        // setting is log.dir (LogSettings::dir); ConfigFromSettings copies it.
        std::string logDir;

        // Exit sentinel window (task 8): how long a requested exit may take
        // before it is reported as a hang-at-exit.
        std::uint32_t exitSeconds = ::Arcane::Detail::DiagnosticsDefaults().exitSeconds;

        // How long the SUBMITTING thread waits for the crash thread to finish
        // a report before giving up and terminating anyway (UE's 60 s).
        std::uint32_t crashHandlingTimeoutSeconds = ::Arcane::Detail::DiagnosticsDefaults().crashHandlingTimeoutSeconds;

        // ---- crash-path snapshot (settings arc S6-2) ----------------------
        // Copied into g_cfg at Install and read from there: the crash path
        // never reads the registry (inventory Part 1 note 17).

        // MINIDUMP_TYPE: Default = thread info, handles, unloaded modules and
        // indirectly referenced memory; Small = MiniDumpNormal; Full adds the
        // whole address space.
        MinidumpKind  minidumpKind          = ::Arcane::Detail::DiagnosticsDefaults().minidumpKind;
        // The bounded log file-sink flush after a report (step 8).
        std::uint32_t logFlushTimeoutMs     = ::Arcane::Detail::DiagnosticsDefaults().logFlushTimeoutMs;
        // The watchdog's poll period: its detection resolution.
        std::uint32_t watchdogPollMs        = ::Arcane::Detail::DiagnosticsDefaults().watchdogPollMs;
        // How long StopWatchdog waits before orphaning a watchdog parked
        // mid-report.
        std::uint32_t watchdogJoinTimeoutMs = ::Arcane::Detail::DiagnosticsDefaults().watchdogJoinTimeoutMs;
        // The floor under a FATAL report's wait (R49), even past the
        // crashHandlingTimeoutSeconds deadline.
        std::uint32_t minFatalWaitMs        = ::Arcane::Detail::DiagnosticsDefaults().minFatalWaitMs;
    };

    // Process exit codes this module produces. Stable and small: the monitor,
    // CI and the reporter all read them.
    namespace ExitCode
    {
        ARC_CONSTANT("wire protocol: host exit code 'crashed'; the monitor, CI and the reporter read it")
        inline constexpr int kCrashed         = 10;   // a report was written, the host died
        ARC_CONSTANT("wire protocol: host exit code 'hang terminated'; the monitor, CI and the reporter read it")
        inline constexpr int kHangTerminated  = 11;   // the reporter terminated a hung host
        ARC_CONSTANT("wire protocol: host exit code 'exit sentinel'; the monitor, CI and the reporter read it")
        inline constexpr int kExitSentinel    = 12;   // the exit sentinel fired (task 8)
        ARC_CONSTANT("wire protocol: host exit code 'crash in the crash path'; the monitor, CI and the reporter read it")
        inline constexpr int kCrashInCrashPath = 13;  // the crash thread itself faulted
    }

    // One report request. `reason`'s PREFIX decides the kind (DeriveReportKind
    // below), so its wording is a contract, not prose.
    struct ReportRequest
    {
        // Copied into fixed storage by SubmitReport on the CALLING thread, so
        // a reason formatted into the crash arena survives the arena reset the
        // crash thread does at the top of every report.
        const char* reason = nullptr;

        // EXCEPTION_POINTERS* for a real fault, or null. Null means the walked
        // thread's context is fetched by suspending it (see LastReportStem's
        // note below on WHICH thread that is).
        void* exceptionPointers = nullptr;

        // A continuable report (ARC_ENSURE): envelope + portable stack only,
        // no minidump, no backlog freeze, and it RETURNS so the caller goes on.
        bool lightweight = false;

        // Non-zero terminates the process with this code once the report is on
        // disk; SubmitReport then never returns. Zero is a SURVIVABLE report
        // (a hang, a gpu-stall, a manual WriteReport): the host keeps running,
        // and the backlog/module table are thawed again on the way out.
        //
        // "Survivable" (exitCode == 0) is NOT the same thing as the HANG
        // PROTOCOL (kind "hang" or "gpu-stall" AND exitCode == 0 --
        // IsHangProtocolReport, R95/R110): a report can be survivable with
        // any kind (an `ensure` is survivable and never hang protocol), and a
        // `hang`-kind report can be non-survivable -- the exit sentinel files
        // "hang at exit" with ExitCode::kExitSentinel (12), a kind of "hang"
        // that terminates the process, so it is neither survivable nor hang
        // protocol. Both the host (IsHangProtocolReport) and the reporter's
        // view (ReportView::isHang) key on the same exitCode == 0 check for
        // exactly this reason.
        int exitCode = ExitCode::kCrashed;
    };

    // The one entry every death path uses. Hands the request to the crash
    // thread, waits up to Config::crashHandlingTimeoutSeconds, and then
    // terminates with `exitCode` when that is non-zero.
    //
    // The WALKED thread (whose stack the report carries) is: the context in
    // `exceptionPointers` when there is one; otherwise the registered MAIN
    // thread when the caller is the watchdog (a hang is about the main thread,
    // not about the watchdog that noticed it); otherwise the calling thread,
    // suspended by the crash thread while it parks in the wait below.
    //
    // Safe with Diagnostics not installed: nothing is written, and the call
    // still terminates when `exitCode` is non-zero rather than deadlocking.
    ARC_CORE_API void SubmitReport(const ReportRequest& request) noexcept;

    // The last report's sibling stem -- the base path with NO extension, which
    // "<stem>.txt", "<stem>.dmp", "<stem>.arcdiag" and "<stem>.log.txt" all
    // hang off. Empty until a report has been written. A test seam and a host
    // convenience; never called from the crash path itself (it allocates).
    [[nodiscard]] ARC_CORE_API std::string LastReportStem();

    // Copies ForeignModules' latest scan into fixed storage the crash thread
    // can read with no lock, no heap and no loader lock (controller note R14).
    // ForeignModules::Scan() calls this beside its ModuleTable::Refresh, and
    // Install() seeds it from LastScan(); the crash path reads ONLY the
    // snapshot, never LastScan() (which returns a heap copy under a mutex).
    ARC_CORE_API void SnapshotInjectedModules(std::span<const ForeignModules::Match> matches) noexcept;

    // Registers the CALLING thread as the main thread and arms both triggers.
    // Call once, early in main(), beside Log::Init(). Idempotent.
    //
    // Also installs the console control handler (Ctrl-C / console close /
    // logoff / shutdown -> RequestCleanExit, below) when the process owns a
    // console window AND Config::installCrashHandler is set (R24) -- it is a
    // process-wide handler, so it takes the same gate the rest of the
    // crash-handler family does -- and registers ONE process-lifetime atexit
    // hook that
    // stops the watchdog thread. The atexit hook is what makes an
    // Install-then-return-from-main() safe: the watchdog is a RAW thread so
    // that it can outlive a host that never reaches Shutdown() (spec S5.7),
    // and something has to stop it when main() simply returns.
    ARC_CORE_API void Install(const Config& cfg);

    // The host's Config from the settings (settings arc S6-2). Every tunable
    // comes from `s`; the identity fields -- appName, productName, unattended,
    // launchMonitor, commandLine -- keep Config's defaults for the host to set.
    // logDir is log.dir (S6-3), read from the published LogSettings: it lives
    // in the log category, not in `s`.
    [[nodiscard]] ARC_CORE_API Config ConfigFromSettings(const DiagnosticsSettings& s);

    // The crash reporter Install arms (settings S7-SEC): `configured`
    // (diagnostics.reporterPath, Config::reporterPath) when it is a launchable
    // file (CheckLaunchPath), else the bundled <exeDir>/ArcaneCrashReporter.exe.
    // A configured path that is refused fills `refusal` with why (the caller
    // warns); an empty one is the default, not a refusal.
    [[nodiscard]] ARC_CORE_API std::filesystem::path ReporterExeFor(std::string_view configured,
                                                                    const std::filesystem::path& exeDir,
                                                                    std::string* refusal = nullptr);

    // The report directory Install and RetargetDumpDir arm (settings S7-SEC
    // fix round 1): `configured` (diagnostics.dumpDir, Config::dumpDir) unless
    // it is empty or holds a '"', CR or LF -- the report stem rides the crash
    // reporter's command line inside quotes, so such a path could add
    // arguments of its own (a project may suggest dumpDir). Otherwise
    // <exeDir>/diagnostics. A refused path fills `refusal` (the caller warns).
    [[nodiscard]] ARC_CORE_API std::filesystem::path ReportDirFor(std::string_view configured,
                                                                  const std::filesystem::path& exeDir,
                                                                  std::string* refusal = nullptr);

    // The crash thread's gate before it spawns the reporter (settings S7-SEC
    // fix round 1): true when `stemUtf8` is non-empty and holds no '"', CR or
    // LF, so the plain quote wrap around <stem>.arcdiag parses back exactly (the
    // stem never ends in a backslash: it ends in the report's file name). No
    // heap, noexcept: SpawnReporter skips the spawn when it is false.
    [[nodiscard]] ARC_CORE_API bool ReporterStemSafe(const char* stemUtf8) noexcept;

    // The crash monitor's command line (settings S7-SEC fix round 1), split
    // around the host-handle digits LaunchMonitor appends once the handle
    // exists: head + <digits> + tail. The exe and the session path are quoted
    // by QuoteWindowsArg, so CommandLineToArgvW reads each back exactly.
    struct MonitorCommand
    {
        std::wstring head;   // <exe> --monitor <pid> --host-handle (with the trailing space)
        std::wstring tail;   // " --session <session>[ --unattended]<settingsArgs>"
    };
    [[nodiscard]] ARC_CORE_API MonitorCommand MonitorCommandFor(const std::wstring& exe, unsigned long pid,
                                                                const std::wstring& session, bool unattended,
                                                                std::wstring_view settingsArgs);

    // The crash reporter's settings as the tail of its command line (settings
    // arc S6-4): " --deadline <s> --max-frames-thread <n> ... --copy-flash <s>",
    // every flag ReporterArgs parses back, the double printed round-trip
    // exact (%.17g). The reporter has no registry, so this IS how
    // diagnostics.reporter.*, diagnostics.logTailLines and ui.copyFlashSeconds
    // reach it. Install formats it once from the published settings (and
    // RetargetDumpDir again, after a project's rungs) into fixed storage; the
    // crash thread only appends that buffer, never calls this.
    [[nodiscard]] ARC_CORE_API std::wstring ReporterSettingsArgs(const DiagnosticsReporterSettings& s,
                                                                 std::uint32_t logTailLines, double copyFlashSeconds);

    // The tail the NEXT reporter spawn appends (S6-4 carried gap): Install's
    // snapshot, re-formatted when diagnostics.logTailLines or
    // ui.copyFlashSeconds (both Live) publish a change. A test seam; it
    // allocates, so never the crash path. Empty until the first Install,
    // RetargetDumpDir or watched Live publish fills it: RetargetDumpDir
    // attaches the Live watch and snapshots even before any Install.
    [[nodiscard]] ARC_CORE_API std::wstring CurrentReporterSettingsArgs();

    // How the Live reporter-settings watch attaches (S6-5 carried follow-up):
    // adds `fn` to every names[i] whose attached[i] is still false and that
    // `reg` has registered, marking it attached. A name not registered yet is
    // left false for the next call, so a watch armed before a cvar's
    // registration is not silently latched. True once every name is attached.
    // Install and RetargetDumpDir call it on the engine's registry; a test
    // seam on a private one. Main thread (AddCallback's rule).
    [[nodiscard]] ARC_CORE_API bool AttachMissingCVarCallbacks(CVarRegistry& reg, std::span<const std::string_view> names,
                                                               std::span<bool> attached,
                                                               void (*fn)(CVarHandle, void*), void* user);

    // The one-line helper spec S5.1 item 4 asks every WORKER thread to call as
    // its first statement (crash window plan 1, R23). Install already does this
    // for the thread that called it -- the main thread -- and the reason is the
    // same on a worker: a stack overflow leaves only the guard page between the
    // faulting thread and the filter that has to run on it, and the filter (it
    // only signals the crash thread and waits) needs 64 KiB below that page to
    // get that far. Without it, an overflow on a job worker is a silent death
    // with no report at all.
    //
    // Call sites: the enkiTS worker entry (Jobs/JobSystem.cpp, through the
    // scheduler's threadStart callback) and ServiceThread's thread body
    // (Base/ServiceThread.cpp). Idempotent, cheap, and a no-op off Windows.
    ARC_CORE_API void GuaranteeStackForThisThread() noexcept;

    // Disarms both triggers, stops the watchdog (a bounded wait on its raw
    // thread handle, which is then closed), restores the previous
    // unhandled-exception filter and the console handler, and stops and joins
    // the crash thread (closing its events) -- an Install/Shutdown cycle
    // leaves no running thread and no live handle behind. Also thaws the
    // module table and the log backlog, so a survivable report that was
    // interrupted cannot leave either frozen for the rest of the process.
    //
    // It is also the END of the exit-sentinel window (below): the clean-exit
    // request, its deadline and the Ctrl-C step counter are all reset here, so
    // a process that disarms and keeps running -- the test binary does this
    // ~20 times -- is genuinely disarmed and can never be terminated by a
    // sentinel armed in a previous arming.
    //
    // Idempotent; safe to skip (the process exiting is also fine). Does NOT
    // reset Config::dumpDir --
    // a dumpDir retargeted live (RetargetDumpDir, below) is host state, not
    // arming state, and must survive a Shutdown/Install cycle the same way
    // appName does.
    ARC_CORE_API void Shutdown() noexcept;

    // Switches WHERE reports land, live -- no Shutdown()/Install() cycle
    // needed (GPU crash diagnostics arc, Task 8; F-6 in the seam-facts
    // survey). A host calls this immediately after whatever per-project state
    // it already retargets on the same event -- the editor's
    // RetargetLayoutIni (ArcaneEditor/src/App/EditorApp.cpp) and its
    // ArcaneRuntime equivalent -- with `<project>/Saved/Diagnostics` on
    // project open. An empty path reverts to Config::dumpDir's own default
    // (empty => "<exe dir>/diagnostics", see the Config comment above) --
    // ReportDir() already implements that fallback for an empty string, so
    // this never re-derives it; it is exactly what a project-less
    // convergence (a failed project switch, or no --project at all) passes.
    //
    // Thread-safe against a concurrent watchdog/crash report: takes the same
    // lock the crash thread holds for its ENTIRE report body, so a live
    // retarget can never race a report already mid-write. The report
    // directory is re-derived and re-snapshotted here (the crash path reads
    // only the snapshot -- it may not touch std::filesystem).
    //
    // The engine log follows too, but only when Config::logDir is EMPTY (the
    // derived case): the file sink is re-attached at "<dir>/../Logs/<appName>.log"
    // so a project's log lands beside that project's reports. An explicitly
    // configured logDir is never retargeted.
    ARC_CORE_API void RetargetDumpDir(const std::filesystem::path& dir);

    // -------------------------------------------------------------------
    // Exit sentinel and clean-exit handlers (crash window plan 1, task 8;
    // spec S5.7)
    // -------------------------------------------------------------------
    // The exit sentinel is NOT a new thread: it is the watchdog with its rule
    // swapped. From RequestCleanExit() until Shutdown(), the main-thread beat
    // and GPU-progress rules are replaced by a single deadline
    // (Config::exitSeconds). A host that asked to quit and then never got out
    // -- a Vulkan teardown that never returns, a module-build join that never
    // completes -- is the one failure nothing else in this module can name: it
    // keeps beating right up to the last frame and then disappears into a
    // teardown with no frame loop left to observe. The sentinel writes a
    // `hang` report ("hang at exit") through the crash thread and terminates
    // with ExitCode::kExitSentinel (12).
    //
    // What the hook is for: the OS gives a console handler about five seconds
    // and a session-end message not much more, so the host's own quit path has
    // to be STARTED from whatever noticed (Ctrl-C, console close, logoff,
    // WM_ENDSESSION) rather than waited for. The host installs one function
    // that begins its ordinary exit -- the editor autosaves everything dirty
    // first, which is fast -- and every one of those paths routes here.

    // The host's "begin your ordinary exit now" callback. A raw pointer pair
    // (same shape as SetReportWrittenHook): the DLL boundary stays free of
    // std::function's allocator coupling, and nothing on this path allocates.
    // Runs on WHATEVER thread requested the exit -- the OS's console-handler
    // thread for Ctrl-C and close, the host's own thread for a window
    // message -- so a hook that touches thread-affine state must marshal.
    using CleanExitHook = void (*)(void* user);

    // Install (or, with nullptr, clear) the process-wide clean-exit hook.
    // Last writer wins; one call per host lifetime is the expected shape.
    //
    // Installing one is ALSO what opts a console host into the two-step
    // Ctrl-C. With the slot empty there is nothing for a first press to
    // start, so claiming it would only swallow it: the handler declines the
    // event instead and Windows terminates the process exactly as it always
    // did. Console close/logoff/shutdown are not affected -- those end the
    // process whatever we return.
    ARC_CORE_API void SetCleanExitHook(CleanExitHook hook, void* user) noexcept;

    // "The host has been asked to quit." Arms the exit deadline and calls the
    // hook EXACTLY ONCE, however many paths request the same exit (a Ctrl-C
    // and a close and the host's own File->Exit). The deadline is armed BEFORE
    // the hook runs, on purpose: a hook that itself wedges is precisely what
    // the sentinel exists to name.
    //
    // Safe to call with Diagnostics not installed, and safe from any thread.
    ARC_CORE_API void RequestCleanExit() noexcept;

    // Test seam: runs the console control handler's rule for `ctrlType`
    // (CTRL_C_EVENT = 0, CTRL_BREAK_EVENT = 1, CTRL_CLOSE_EVENT = 2, ...) and
    // returns whether it handled it -- exactly what Windows would call, with
    // the ONE difference that the second Ctrl-C's TerminateProcess is real
    // here too, so a test simulates the first press only. Works with no
    // console attached, with the handler not installed (R24 gates that on
    // Config::installCrashHandler) and with the watchdog not running: the
    // console rule depends on none of the three. False for a Ctrl-C with no
    // clean-exit hook installed -- see SetCleanExitHook.
    [[nodiscard]] ARC_CORE_API bool SimulateConsoleCtrl(unsigned long ctrlType) noexcept;

    // "The main thread is alive." One relaxed atomic store -- cheap enough for
    // every frame, which is where it belongs. A hang is DEFINED as this not
    // being called, so it must sit on every path the main thread loops through:
    // the frame loop AND the boot/project-switch pump.
    ARC_CORE_API void Heartbeat() noexcept;

    // Optional label for what the main thread is currently doing (a boot stage
    // id, say). Reproduced verbatim at the top of a report -- it turns "it hung"
    // into "it hung in switch_plugin_load". Cheap but not free (takes a lock);
    // call it per phase, never per frame.
    ARC_CORE_API void SetPhase(std::string phase);

    // Writes a SURVIVABLE report immediately, whatever the process state, and
    // returns the .txt path (empty if nothing could be written). Exactly
    // SubmitReport({reason, nullptr, false, /*exitCode*/0}) plus the path --
    // the process keeps running, which is what the hang watchdog and the GPU
    // observer need (spec S5.4). Public because a manual trigger is useful,
    // and because it is how the self-test proves the capture path works BEFORE
    // an intermittent bug depends on it.
    ARC_CORE_API std::string WriteReport(const char* reason);

    // The .arcdiag `kind` for a report reason. The reason's PREFIX -- what
    // stands before the first ':' or space -- is matched against the kind
    // table first (gpu-stall, gpu-crash, assert, ensure, terminate,
    // out-of-memory, abnormal-exit, hang, crash), because every reason this
    // engine produces is written "<kind>: <detail>" and the detail embeds
    // caller expressions and file paths that must not be allowed to vote
    // ("ensure: gpu != nullptr" is an ensure, not a gpu-crash). Only a reason
    // with no recognised prefix falls back to the older substring rule, for
    // the free-form legacy wordings ("hang (main thread ...)").
    [[nodiscard]] ARC_CORE_API std::string DeriveReportKind(const char* reason);

    // printf-formats a fail-fast reason into a per-thread 1 KiB buffer (the
    // same size as the pending report's reason field) and returns it. This
    // exists so that NOTHING formats a reason into the CrashArena: the arena
    // is bump-allocated with a plain m_used += and is reset by the crash
    // thread at the top of every report, so a fail-fast handler on some other
    // thread formatting into it could corrupt a report already in flight
    // (final review, finding I2). The arena is crash-thread-only. The
    // returned pointer stays valid until the SAME thread formats another
    // reason -- which is exactly long enough, because SubmitReport copies the
    // reason into fixed storage on the calling thread (R3).
    [[nodiscard]] ARC_CORE_API const char* FormatReason(const char* fmt, ...) noexcept;

    // Reports written this process. The observable the watchdog test asserts on.
    [[nodiscard]] ARC_CORE_API std::uint32_t ReportCount() noexcept;

    // "The GPU device is confirmed gone." Told to this module by the render
    // layer's own latch -- Arcane::NoteGpuDeviceLost (Render/
    // GpuInstrumentation.hpp) calls this, so the two never disagree and the
    // render layer stays the single place that DECIDES a device is lost.
    //
    // Base cannot include Render, and this is the whole reason the fact has
    // to be pushed down rather than pulled up. Its one consumer is the
    // top-level exception filter: a D3D12 debug-layer fail-fast (code 0x87D)
    // raised AFTER a confirmed loss is that loss being re-narrated by the
    // debug layer, and must be reported and exited as a device loss rather
    // than as an unrelated crash. Without this flag the filter would have to
    // classify on the exception code alone, which would mislabel the
    // window-hook fail-fast Device.hpp records (a live device, no loss).
    //
    // Idempotent, never cleared, safe from any thread and from inside a
    // crash handler.
    ARC_CORE_API void NoteGpuDeviceLost() noexcept;

    // What NoteGpuDeviceLost last stored. Exists so the classification rule
    // above is testable without a GPU, a device, or an exception.
    [[nodiscard]] ARC_CORE_API bool GpuDeviceLostNoted() noexcept;

    // -------------------------------------------------------------------
    // GPU-progress watchdog (GPU crash diagnostics arc, Task 7)
    // -------------------------------------------------------------------

    // "The GPU is still retiring work." One relaxed atomic store, called once
    // per frame by the render path with a MONOTONE count of GPU-side sync
    // points the device has actually passed. The graph path publishes
    // NriSwapChain::CompletedFrameValue() -- its pacing timeline fence's
    // completed value -- from NriGraphContext::RenderFrame. NriSwapChain.hpp
    // explains why that fence is an exact count and not an approximation.
    //
    // Deliberately NOT folded into Heartbeat(): that one says only that the
    // main thread is alive, which is exactly what a GPU hang can leave true.
    // A frame loop that keeps ticking while the GPU has stopped retiring
    // anything is invisible to the hang watchdog and is the case this exists
    // for. Like Heartbeat(), the FIRST call arms the trigger -- a host that
    // never renders (a test, a headless tool) gets silence, not a spurious
    // report gpuStallSeconds after boot.
    ARC_CORE_API void GpuHeartbeat(std::uint64_t fenceValue) noexcept;

    // "The render path is still alive and still watching the SAME counter."
    // Refreshes the freshness stamp GpuHeartbeat sets, without changing the
    // value and without arming the rule (a host that has never published a
    // value stays silent no matter how often this is called).
    //
    // Exists for exactly one caller shape: a render path that is BLOCKED
    // waiting for the GPU to retire work, and therefore cannot publish a new
    // value -- but is emphatically not idle. Without it, the frame-slot wait
    // would look identical to a minimized host (frozen counter, no publisher)
    // and the GPU rule would disarm on the one case it exists to catch. See
    // Render/GpuInstrumentation.hpp, GpuFrameSlot::WaitAndReset.
    ARC_CORE_API void GpuHeartbeatRefresh() noexcept;

    // The pure staleness rule the GPU watchdog runs, extracted so the part
    // that can actually be WRONG -- one report per stall, re-armed on progress
    // -- is testable without threads, timers, or a GPU. (The 2026-08-11
    // hosted-CI flake was a watchdog test timed against a deadline; here the
    // clock is a parameter, so the cases are exact instead of generous.)
    //
    // Not thread-safe and not meant to be: one instance lives on the watchdog
    // thread and is polled only from there.
    class ARC_CORE_API ProgressStallRule
    {
    public:
        // `stallSeconds` -- how long the counter must sit unchanged before the
        // stall is real. Values <= 0 make every repeat poll a stall.
        explicit ProgressStallRule(double stallSeconds) noexcept
            : m_stallSeconds(stallSeconds) {}

        // One observation. `value` is the latest counter, `nowSeconds` a
        // monotone clock reading in the caller's own epoch. Returns true on
        // EXACTLY the poll that should write a report; false on every other,
        // including every subsequent poll of the same stall. A changed value
        // re-arms and restarts the stall clock from THIS poll.
        [[nodiscard]] bool Poll(std::uint64_t value, double nowSeconds) noexcept;

        // How long the current value has been unchanged, for the report's
        // reason string. Zero before the first Poll().
        [[nodiscard]] double StalledSeconds(double nowSeconds) const noexcept;

        // Back to the never-polled state. The watchdog calls this when the
        // producer stops publishing altogether (a minimized host renders no
        // frames): the counter is frozen, but nobody is claiming otherwise, and
        // resuming must start a fresh stall clock rather than inherit the age of
        // a gap during which nothing was expected to move.
        void Reset() noexcept;

        // Whether the CURRENT stall already produced its report. The watchdog
        // reads it on both sides of a Poll(): true before and false after means
        // the counter moved again after a gpu-stall report, which is when the
        // hang protocol signals the recovered event (plan 2, D11). Inline, so
        // it adds no export.
        [[nodiscard]] bool WasReported() const noexcept { return m_reported; }

    private:
        double        m_stallSeconds;
        std::uint64_t m_value    = 0;
        double        m_since    = 0.0;
        bool          m_seeded   = false;   // a first Poll only seeds; it never reports
        bool          m_reported = false;   // this stall already produced its one report
    };

    // -------------------------------------------------------------------
    // GPU-section provider seam (GPU crash diagnostics arc, Task 4)
    // -------------------------------------------------------------------
    // Installed by a GPU crash backend so WriteReport can fill an .arcdiag
    // envelope's gpu-side fields without Base/Diagnostics knowing anything
    // about D3D12 or Vulkan. Called at
    // most once per report, with an envelope that already carries
    // guid/kind/timestamp/appName/phase/buildInfo/cpuThreadSummary: the
    // provider fills `envelope`'s queues/fault/activeLayers (and, if it
    // wrote its own <reportStem>.gpudump, envelope.siblingGpuDump) and
    // appends human-readable text to `humanText`, which WriteReport folds
    // into the .txt report as a "=== GPU ===" block. `reportStem` is the
    // same base path (no extension) that mints the .txt/.dmp/.arcdiag
    // siblings (F-6b) -- e.g. a provider writes `<reportStem>.gpudump`.
    using GpuSectionProvider = void (*)(Diag::Envelope& envelope,
                                         std::string& humanText,
                                         const std::filesystem::path& reportStem,
                                         void* user);

    // Install (or replace) the process-wide GPU-section provider. Last
    // writer wins, mirroring the structured-diagnostics Sink slot below.
    ARC_CORE_API void SetGpuSectionProvider(GpuSectionProvider provider, void* user) noexcept;

    // Uninstall it. Idempotent; safe to call with none installed.
    ARC_CORE_API void ClearGpuSectionProvider() noexcept;

    // Teardown fence: returns only after any WriteReport already in flight
    // (watchdog thread or crash filter) has finished. WriteReportImpl holds
    // g_reportMutex for its ENTIRE body -- report write, GPU-section
    // provider call, and report-written hook call all included -- so this is
    // simply an empty critical section on that same mutex.
    //
    // A device dtor calling ClearGpuSectionProvider() only uninstalls the
    // slot for the NEXT report; a report that copied the provider pointer
    // out before the clear can still be mid-FillReport against the backend
    // as the dtor goes on to free it (gpu-stall report in progress + host
    // shutdown). Call this immediately after clearing the provider slot and
    // BEFORE destroying anything the provider touches -- see
    // DeviceD3D12::~DeviceD3D12 / DeviceVulkan::~DeviceVulkan.
    ARC_CORE_API void FenceReports() noexcept;

    // -------------------------------------------------------------------
    // Report-written hook (GPU crash diagnostics arc, Task 9)
    // -------------------------------------------------------------------
    // Fired once, at the very end of WriteReportImpl -- after every sibling
    // (.dmp/.txt/.arcdiag, F-6b) has finished writing and the report count/
    // log echo above are done -- with the path of the .arcdiag that was
    // just written. Exists so a host can register the new report as an
    // asset (AssetRegistry::AddFile, via Project::RegisterAsset/
    // Runtime::RegisterCreatedAsset -- F-7's single-asset call) and surface
    // it in Problems, without polling ReportCount() and re-deriving the
    // path itself.
    //
    // Runs on WHATEVER thread called WriteReportImpl: the watchdog thread
    // for hang/gpu-stall (WatchdogMain -- SURVIVABLE, the process keeps
    // running afterward) or the faulting thread for a crash (about to
    // terminate -- registering an asset at that point is moot: there is no
    // next frame left for a host to drain a queue into). The hook still
    // fires uniformly for every report kind, the same way GpuSectionProvider
    // runs for every report kind above -- it is the CONSUMER's job to
    // recognize the crash case is moot, not this seam's.
    //
    // A hook that touches anything not itself thread-safe (an AssetRegistry
    // has no lock of its own -- see AssetRegistry.hpp/.cpp; ImGui state;
    // ...) MUST marshal to its own safe thread first. This seam does no
    // marshaling itself, mirroring GpuSectionProvider immediately above.
    using ReportWrittenHook = void (*)(const std::filesystem::path& diagPath, void* user);

    // Install (or replace) the process-wide report-written hook. Last
    // writer wins, mirroring GpuSectionProvider -- one call per host
    // lifetime is the expected shape.
    ARC_CORE_API void SetReportWrittenHook(ReportWrittenHook hook, void* user) noexcept;

    // Uninstall it. Idempotent; safe to call with none installed.
    ARC_CORE_API void ClearReportWrittenHook() noexcept;
}

// =============================================================================
// Structured diagnostics: the publish/sink seam
// =============================================================================
//
// Distinct from the post-mortem capture above (crash/hang reports written to
// disk after the fact). This seam is for problems a USER must act on RIGHT
// NOW -- a broken material reference, an unresolved asset, a plugin load
// failure -- surfaced live in an editor Problems/Console panel. Deliberately
// separate from Arcane::Log (Base/Log.hpp) too: a log line is an event that
// happened, a diagnostic is an assertion about how things are right now and
// stops being true when the underlying problem is fixed.
//
// PUBLICATION GROUPS: a producer owns a `key` and republishes its ENTIRE set
// for that key; the consumer replaces that key's contents atomically. Retraction
// is not a special case -- it is Publish(key, {}) (or Clear(key)). Individual
// rows are never added or removed, so there is no reconciliation to get wrong.
//
// The sink slot lives once, in Arcane.dll (Diagnostics.cpp), and is
// mutex-guarded: producers publish from worker threads (the async-boot arc runs
// the asset-registry scan off the main thread).

namespace Arcane
{
    enum class DiagSeverity : std::uint8_t { Info, Warning, Error };

    enum class DiagScope : std::uint8_t { Project, Assets, Scene, Plugin, Material, Shader };

    // What clicking the row should do. A tagged union in spirit; only the
    // members belonging to `kind` are meaningful.
    struct DiagLocator
    {
        enum class Kind : std::uint8_t { None, Entity, Asset, File, GraphNode };

        Kind          kind   = Kind::None;
        std::uint64_t entity = 0;    // Kind::Entity
        Guid          asset;         // Kind::Asset
        std::string   file;          // Kind::File
        int           line   = 0;    // Kind::File
        int           col    = 0;    // Kind::File
        Guid          ownerAsset;    // Kind::GraphNode -- the owning material
        std::uint32_t nodeId = 0;    // Kind::GraphNode

        [[nodiscard]] static DiagLocator Entity(std::uint64_t id) noexcept
        {
            DiagLocator l; l.kind = Kind::Entity; l.entity = id; return l;
        }
        [[nodiscard]] static DiagLocator Asset(const Guid& id) noexcept
        {
            DiagLocator l; l.kind = Kind::Asset; l.asset = id; return l;
        }
        [[nodiscard]] static DiagLocator File(std::string path, int lineNo = 0, int colNo = 0)
        {
            DiagLocator l; l.kind = Kind::File; l.file = std::move(path);
            l.line = lineNo; l.col = colNo; return l;
        }
        [[nodiscard]] static DiagLocator GraphNode(const Guid& owner, std::uint32_t node) noexcept
        {
            DiagLocator l; l.kind = Kind::GraphNode; l.ownerAsset = owner; l.nodeId = node; return l;
        }
    };

    struct Diagnostic
    {
        DiagSeverity severity = DiagSeverity::Warning;
        DiagScope    scope    = DiagScope::Project;
        // Stable and dotted ("scene.component.unknown"). This is the identity a
        // future suppression UI, a docs link, or a lint rule id hangs off --
        // never localize it and never reword it casually.
        std::string  code;
        std::string  message;
        // Optional consequence line, rendered dimmed beneath the message.
        std::string  detail;
        DiagLocator  locator;
    };

    namespace Diagnostics
    {
        // Raw function pointer + user data, mirroring Mosaic::SetLogSink. Keeps
        // the DLL boundary free of std::function's allocator coupling.
        using Sink = void (*)(std::string_view key, std::span<const Diagnostic> diags, void* user);

        // Install (or clear, with nullptr) the process-wide sink. Last writer wins.
        ARC_CORE_API void SetSink(Sink sink, void* user) noexcept;

        // Clear the slot ONLY if it still holds exactly (sink, user); returns
        // whether it cleared. A stale consumer's teardown must not silently
        // unslot whoever registered AFTER it -- the slot is process-wide and
        // last-writer-wins, so an unconditional SetSink(nullptr, nullptr) from
        // an old owner's destructor would disconnect a live, unrelated one
        // (same stale-registration hazard as a dangling plugin descriptor).
        // Prefer this over SetSink(nullptr, nullptr) in any owner's teardown path.
        [[nodiscard]] ARC_CORE_API bool ClearSinkIfCurrent(Sink sink, void* user) noexcept;

        // Replace `key`'s entire diagnostic set. Safe with no sink installed.
        ARC_CORE_API void Publish(std::string_view key, std::span<const Diagnostic> diags);

        // Retract everything under `key`. Exactly Publish(key, {}).
        ARC_CORE_API void Clear(std::string_view key);
    }
}
