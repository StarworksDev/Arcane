#pragma once

// Arcane/Base/Posix/PosixCrashSupport.hpp -- PRIVATE to ArcaneCore's POSIX
// crash path (Diagnostics POSIX port, 2026-10-05). Excluded from Windows
// builds by premake5.lua (removefiles Base/Posix/**).
//
// The POSIX twins of the Win32 primitives Base/Diagnostics.cpp's Windows half
// uses, and the same discipline: everything below may run on the crash thread
// while the process is already misbehaving, so none of it touches the heap,
// takes a lock another thread could be holding, or calls anything that might
// (no stdio streams, no localtime, no dl_iterate_phdr). Raw syscalls, fixed
// storage, and reads that FAIL instead of faulting.

#include <Arcane/Core/Constant.hpp>
#include <Arcane/Platform/Platform.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

#if ARC_PLATFORM_MACOS
#include <signal.h>   // _STRUCT_MCONTEXT; <ucontext.h> itself needs _XOPEN_SOURCE on Darwin
#else
#include <ucontext.h>
#endif

namespace Arcane::Diagnostics::Internal::Posix
{
    // ---- the stored register context -----------------------------------------
    // Linux: a ucontext_t (its fpregs pointer re-pointed into the copy, see
    // CopyContext). macOS: the Darwin MACHINE context itself -- a Darwin
    // ucontext_t only POINTS at its mcontext (in the signal frame), so a
    // stored copy keeps the pointee, the same reason Linux re-points fpregs.
    // Thread ids are the kernel's: a Linux tid, or on macOS the thread's Mach
    // port name in this task (what task_threads lists and thread_get_state
    // takes -- the SuspendThread/GetThreadContext handle).
#if ARC_PLATFORM_MACOS
    using NativeContext = _STRUCT_MCONTEXT;
#else
    using NativeContext = ucontext_t;
#endif

    // The calling thread's id in the form above.
    [[nodiscard]] std::uint32_t KernelThreadId() noexcept;

    // Names the CALLING thread (Linux: pthread_setname_np(self, n), 15 chars
    // max; macOS: pthread_setname_np(n), which only names the caller).
    void NameThisThread(const char* name) noexcept;

    // pipe2(fds, flags) where it exists; pipe + fcntl on macOS (no pipe2).
    // flags: O_CLOEXEC and/or O_NONBLOCK.
    [[nodiscard]] bool MakePipe(int fds[2], int flags) noexcept;

    // ---- heap-free file IO (open/write/close; the CreateFileW twins) --------
    [[nodiscard]] int OpenForWrite(const char* path, bool append) noexcept;   // -1 on failure
    bool WriteAll(int fd, const void* data, std::size_t size) noexcept;
    inline bool WriteAll(int fd, std::string_view s) noexcept { return WriteAll(fd, s.data(), s.size()); }
    void CloseFd(int fd) noexcept;

    // Reads a whole (small) file into `buf`, NUL-terminated; returns the byte
    // count (0 on failure). /proc files report size 0, so this reads until EOF.
    std::size_t ReadFileInto(const char* path, char* buf, std::size_t cap) noexcept;

    // Reads [address, address + size) of THIS process without risking a
    // fault: process_vm_readv, falling back to the pipe trick where a sandbox
    // filters it. Returns the bytes actually read (a prefix on a partial).
    std::size_t SafeRead(std::uint64_t address, void* out, std::size_t size) noexcept;

    // ---- contexts -----------------------------------------------------------
    // A ucontext_t that stands alone: a kernel signal frame's (or
    // getcontext's) uc_mcontext.fpregs points OUTSIDE the struct, so a plain
    // copy would dangle the moment that frame is gone. This copies the FP
    // state into the copy's own __fpregs_mem and re-points it there.
    void CopyContext(NativeContext& dst, const NativeContext& src) noexcept;

    // From a SA_SIGINFO handler's third argument (a ucontext_t*).
    void CopySignalContext(NativeContext& dst, const void* signalUcontext) noexcept;

    // The CALLER's context, as getcontext() reports it on Linux. Darwin has
    // no getcontext on arm64, so pc/fp/sp/lr of the calling frame are taken
    // directly -- enough for the frame-chain walk, which is all a stored
    // context is used for besides the minidump's register block.
    [[nodiscard]] bool CaptureOwnContext(NativeContext& out) noexcept;

    [[nodiscard]] std::uint64_t ContextPc(const NativeContext& uc) noexcept;
    [[nodiscard]] std::uint64_t ContextSp(const NativeContext& uc) noexcept;

    // ---- other threads ------------------------------------------------------
    // The thread ids of this process, read from /proc/self/task with raw
    // getdents64 (opendir allocates); on macOS from task_threads (a Mach
    // call that allocates with vm_allocate, never malloc). Returns the count
    // written (<= cap).
    std::size_t ListThreads(std::uint32_t* out, std::size_t cap) noexcept;

    // The POSIX reading of SuspendThread + GetThreadContext. A real-time
    // signal (SIGRTMIN + 4, engine-reserved) is sent to `tid`; its handler
    // copies the thread's ucontext into fixed storage and PARKS -- spinning
    // on an atomic in 1 ms nanosleeps, both async-signal-safe -- until the
    // requester calls `whileParked` and releases it. The stack of a parked
    // thread does not move, which is what lets the minidump copy it.
    //
    // macOS needs no signal: thread_suspend + thread_get_state IS
    // SuspendThread + GetThreadContext, so the target is suspended (not
    // parked in a handler) for `whileParked`, then resumed.
    //
    // One thread is held at a time, never the caller. A thread that has the
    // signal blocked (some libraries block everything in their own workers)
    // is simply not captured: false after `timeoutMs`. Requests are
    // serialized by the caller (the crash thread is the only one).
    using ParkedFn = void (*)(std::uint32_t tid, const NativeContext& context, void* user);
    bool SnapshotThread(std::uint32_t tid, std::uint32_t timeoutMs, ParkedFn whileParked, void* user) noexcept;

    // Installed at Install REGARDLESS of installCrashHandler: like the crash
    // thread, it is part of the report ENGINE (a hang report walks the main
    // thread through it), not a fault handler. Restored at Shutdown.
    bool InstallSnapshotSignal() noexcept;
    void RemoveSnapshotSignal() noexcept;
    [[nodiscard]] int SnapshotSignal() noexcept;

    // Blocks the process-directed ASYNCHRONOUS signals (SIGINT, SIGTERM,
    // SIGHUP, SIGQUIT, SIGPIPE, SIGCHLD) on the calling thread. The engine's
    // own service threads (crash thread, watchdog, console forwarder) call it
    // first, so such a signal lands on a host thread, never on one of them.
    void BlockAsyncSignalsOnThisThread() noexcept;

    // ---- the minidump (MinidumpWriter.cpp) ---------------------------------
    struct DumpThread
    {
        std::uint32_t     tid     = 0;
        const NativeContext* context = nullptr;
    };

    struct DumpRequest
    {
        const char* path = nullptr;

        // Never in the dump: the crash thread itself.
        std::uint32_t excludeTid = 0;

        // Threads whose context the report already holds (the faulting
        // thread, the parked submitter); everything else is snapshotted.
        const DumpThread* known      = nullptr;
        std::size_t       knownCount = 0;

        // The exception stream. A real fault carries the signal; a hang or a
        // manual report carries Breakpad's DUMP_REQUESTED code around the
        // walked thread's context -- the Windows path's STILL_ACTIVE record,
        // and for the same reason: every dump opens on a stored event.
        std::uint32_t     exceptionTid     = 0;
        const NativeContext* exceptionContext = nullptr;
        std::uint32_t     exceptionCode    = 0;   // signal number, or kDumpRequested
        std::uint32_t     exceptionFlags   = 0;   // si_code
        std::uint64_t     exceptionAddress = 0;   // si_addr (or the pc)
    };

    ARC_CONSTANT("file format: Breakpad DUMP_REQUESTED, the exception code of a hang or manual report")
    inline constexpr std::uint32_t kDumpRequested = 0xFFFFFFFFu;   // MD_EXCEPTION_CODE_LIN_DUMP_REQUESTED

    // Writes a Breakpad-format minidump (the Microsoft MDMP container with
    // Breakpad's Linux streams) -- see MinidumpWriter.cpp for the layout.
    bool WriteMinidump(const DumpRequest& request) noexcept;

    // Off the crash path, at Install: uname, CPU count/vendor for the
    // SystemInfo stream.
    void SnapshotSystemInfoForDump() noexcept;
}
