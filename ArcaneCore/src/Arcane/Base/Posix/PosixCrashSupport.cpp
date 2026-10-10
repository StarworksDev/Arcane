// Base/Posix/PosixCrashSupport.cpp -- heap-free IO, safe reads, standalone
// contexts and signal-based thread snapshots for the POSIX crash path. See
// the header for the discipline every function here keeps.

#include <Arcane/Base/Posix/PosixCrashSupport.hpp>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/uio.h>
#include <unistd.h>
#if ARC_PLATFORM_MACOS
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/syscall.h>
#endif

namespace Arcane::Diagnostics::Internal::Posix
{
    // ---- identity + pipes ---------------------------------------------------

    std::uint32_t KernelThreadId() noexcept
    {
#if ARC_PLATFORM_MACOS
        // The Mach port name: what task_threads lists and thread_suspend /
        // thread_get_state take. pthread_mach_thread_np takes no new port
        // reference (mach_thread_self would leak one per call).
        return static_cast<std::uint32_t>(::pthread_mach_thread_np(::pthread_self()));
#else
        return static_cast<std::uint32_t>(::syscall(SYS_gettid));
#endif
    }

    void NameThisThread(const char* name) noexcept
    {
#if ARC_PLATFORM_MACOS
        ::pthread_setname_np(name);
#else
        ::pthread_setname_np(::pthread_self(), name);
#endif
    }

    bool MakePipe(int fds[2], int flags) noexcept
    {
#if ARC_PLATFORM_MACOS
        if (::pipe(fds) != 0) return false;
        for (int i = 0; i < 2; ++i)
        {
            if ((flags & O_CLOEXEC) && ::fcntl(fds[i], F_SETFD, FD_CLOEXEC) != 0) goto fail;
            if (flags & O_NONBLOCK)
            {
                const int fl = ::fcntl(fds[i], F_GETFL);
                if (fl < 0 || ::fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) != 0) goto fail;
            }
        }
        return true;
    fail:
        ::close(fds[0]);
        ::close(fds[1]);
        fds[0] = fds[1] = -1;
        return false;
#else
        return ::pipe2(fds, flags) == 0;
#endif
    }

    // ---- IO -----------------------------------------------------------------

    int OpenForWrite(const char* path, bool append) noexcept
    {
        if (!path || !*path) return -1;
        const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
        int fd;
        do { fd = ::open(path, flags, 0644); } while (fd < 0 && errno == EINTR);
        return fd;
    }

    bool WriteAll(int fd, const void* data, std::size_t size) noexcept
    {
        if (fd < 0) return false;
        const char* p = static_cast<const char*>(data);
        while (size > 0)
        {
            const ssize_t n = ::write(fd, p, size);
            if (n < 0)
            {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;
            p    += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }

    void CloseFd(int fd) noexcept
    {
        if (fd >= 0) ::close(fd);
    }

    std::size_t ReadFileInto(const char* path, char* buf, std::size_t cap) noexcept
    {
        if (cap == 0) return 0;
        buf[0] = '\0';
        int fd;
        do { fd = ::open(path, O_RDONLY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
        if (fd < 0) return 0;
        std::size_t used = 0;
        while (used + 1 < cap)
        {
            const ssize_t n = ::read(fd, buf + used, cap - 1 - used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            used += static_cast<std::size_t>(n);
        }
        ::close(fd);
        buf[used] = '\0';
        return used;
    }

    std::size_t SafeRead(std::uint64_t address, void* out, std::size_t size) noexcept
    {
        if (size == 0) return 0;
#if ARC_PLATFORM_MACOS
        // A Mach VM read of our own task fails (KERN_INVALID_ADDRESS /
        // KERN_PROTECTION_FAILURE) instead of faulting. Page by page, so a
        // range that runs into unmapped memory still yields its prefix.
        std::size_t done = 0;
        while (done < size)
        {
            const std::uint64_t at   = address + done;
            const std::size_t   room = static_cast<std::size_t>(4096 - (at & 4095));
            const std::size_t   want = (size - done) < room ? (size - done) : room;
            mach_vm_size_t got = 0;
            if (::mach_vm_read_overwrite(::mach_task_self(), at, want,
                                         reinterpret_cast<mach_vm_address_t>(static_cast<char*>(out) + done), &got) != KERN_SUCCESS
                || got != want)
                break;
            done += want;
        }
        return done;
#else
#if defined(__linux__)
        iovec local{ out, size };
        iovec remote{ reinterpret_cast<void*>(address), size };
        const ssize_t n = ::syscall(SYS_process_vm_readv, ::getpid(), &local, 1ul, &remote, 1ul, 0ul);
        if (n >= 0)
            return static_cast<std::size_t>(n);
        if (errno != EPERM && errno != ENOSYS)
            return 0;
#endif
        // Sandboxed: write() FROM an unmapped address is EFAULT, not SIGSEGV.
        // Page by page, so a range that runs into unmapped memory still yields
        // its readable prefix. A pipe holds at least 4 KiB.
        int fds[2];
        if (!MakePipe(fds, O_CLOEXEC)) return 0;
        std::size_t done = 0;
        while (done < size)
        {
            const std::uint64_t at   = address + done;
            const std::size_t   room = static_cast<std::size_t>(4096 - (at & 4095));
            const std::size_t   want = (size - done) < room ? (size - done) : room;
            if (::write(fds[1], reinterpret_cast<const void*>(at), want) != static_cast<ssize_t>(want)) break;
            if (::read(fds[0], static_cast<char*>(out) + done, want) != static_cast<ssize_t>(want)) break;
            done += want;
        }
        ::close(fds[0]);
        ::close(fds[1]);
        return done;
#endif
    }

    // ---- contexts -----------------------------------------------------------

    void CopyContext(NativeContext& dst, const NativeContext& src) noexcept
    {
        std::memcpy(&dst, &src, sizeof(NativeContext));
#if defined(__linux__) && defined(__x86_64__)
        // The first 512 bytes of the kernel's XSAVE area (or getcontext's
        // fnstenv block) ARE the FXSAVE layout _libc_fpstate describes.
        if (src.uc_mcontext.fpregs && src.uc_mcontext.fpregs != &src.__fpregs_mem)
            std::memcpy(&dst.__fpregs_mem, src.uc_mcontext.fpregs, sizeof(dst.__fpregs_mem));
        dst.uc_mcontext.fpregs = src.uc_mcontext.fpregs ? &dst.__fpregs_mem : nullptr;
#endif
    }

    void CopySignalContext(NativeContext& dst, const void* signalUcontext) noexcept
    {
        if (!signalUcontext) return;
#if ARC_PLATFORM_MACOS
        const auto* uc = static_cast<const ucontext_t*>(signalUcontext);
        if (uc->uc_mcontext)
            std::memcpy(&dst, uc->uc_mcontext, sizeof(NativeContext));
        else
            std::memset(&dst, 0, sizeof(NativeContext));
#else
        CopyContext(dst, *static_cast<const ucontext_t*>(signalUcontext));
#endif
    }

    // noinline: "the caller's frame" must be a real frame, not this one
    // folded into it.
    __attribute__((noinline)) bool CaptureOwnContext(NativeContext& out) noexcept
    {
#if ARC_PLATFORM_MACOS
        std::memset(&out, 0, sizeof(NativeContext));
        // This function's frame record: [0] = the caller's fp, [1] = our
        // return address (a pc inside the caller). The caller's sp is just
        // above our frame record.
        const auto* frame = static_cast<const std::uint64_t*>(__builtin_frame_address(0));
        const std::uint64_t pc = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
#if defined(__aarch64__)
        __darwin_arm_thread_state64_set_fp(out.__ss, reinterpret_cast<void*>(frame[0]));
        __darwin_arm_thread_state64_set_lr_fptr(out.__ss, reinterpret_cast<void*>(pc));
        __darwin_arm_thread_state64_set_pc_fptr(out.__ss, reinterpret_cast<void*>(pc));
        __darwin_arm_thread_state64_set_sp(out.__ss, reinterpret_cast<std::uint64_t>(frame + 2));
#elif defined(__x86_64__)
        out.__ss.__rbp = frame[0];
        out.__ss.__rip = pc;
        out.__ss.__rsp = reinterpret_cast<std::uint64_t>(frame + 2);
#endif
        return true;
#else
        return ::getcontext(&out) == 0;
#endif
    }

    std::uint64_t ContextPc(const NativeContext& uc) noexcept
    {
#if ARC_PLATFORM_MACOS && defined(__aarch64__)
        return reinterpret_cast<std::uint64_t>(__darwin_arm_thread_state64_get_pc_fptr(uc.__ss)) & 0x00007FFFFFFFFFFFull;
#elif ARC_PLATFORM_MACOS && defined(__x86_64__)
        return uc.__ss.__rip;
#elif defined(__linux__) && defined(__x86_64__)
        return static_cast<std::uint64_t>(uc.uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
        return uc.uc_mcontext.pc;
#else
        (void)uc; return 0;
#endif
    }

    std::uint64_t ContextSp(const NativeContext& uc) noexcept
    {
#if ARC_PLATFORM_MACOS && defined(__aarch64__)
        return static_cast<std::uint64_t>(__darwin_arm_thread_state64_get_sp(uc.__ss));
#elif ARC_PLATFORM_MACOS && defined(__x86_64__)
        return uc.__ss.__rsp;
#elif defined(__linux__) && defined(__x86_64__)
        return static_cast<std::uint64_t>(uc.uc_mcontext.gregs[REG_RSP]);
#elif defined(__linux__) && defined(__aarch64__)
        return uc.uc_mcontext.sp;
#else
        (void)uc; return 0;
#endif
    }

    // ---- threads ------------------------------------------------------------

    std::size_t ListThreads(std::uint32_t* out, std::size_t cap) noexcept
    {
#if ARC_PLATFORM_MACOS
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        if (::task_threads(::mach_task_self(), &threads, &count) != KERN_SUCCESS || !threads)
            return 0;
        std::size_t n = 0;
        for (mach_msg_type_number_t i = 0; i < count; ++i)
        {
            if (n < cap) out[n++] = static_cast<std::uint32_t>(threads[i]);
            // task_threads added a send-right reference per thread; drop it.
            // The NAME stays valid: libpthread holds its own reference for
            // every live thread.
            ::mach_port_deallocate(::mach_task_self(), threads[i]);
        }
        ::vm_deallocate(::mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                        static_cast<vm_size_t>(count * sizeof(thread_act_t)));
        return n;
#elif defined(__linux__)
        int fd;
        do { fd = ::open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
        if (fd < 0) return 0;

        struct LinuxDirent64
        {
            std::uint64_t  d_ino;
            std::int64_t   d_off;
            unsigned short d_reclen;
            unsigned char  d_type;
            char           d_name[1];
        };

        alignas(8) static char s_buf[8192];   // crash thread only
        std::size_t n = 0;
        for (;;)
        {
            const long got = ::syscall(SYS_getdents64, fd, s_buf, sizeof(s_buf));
            if (got <= 0) break;
            for (long off = 0; off < got;)
            {
                const auto* d = reinterpret_cast<const LinuxDirent64*>(s_buf + off);
                off += d->d_reclen;
                std::uint32_t tid = 0;
                bool digits = d->d_name[0] != '\0';
                for (const char* c = d->d_name; *c; ++c)
                {
                    if (*c < '0' || *c > '9') { digits = false; break; }
                    tid = tid * 10u + static_cast<std::uint32_t>(*c - '0');
                }
                if (digits && n < cap) out[n++] = tid;
            }
        }
        ::close(fd);
        return n;
#else
        (void)out; (void)cap;
        return 0;
#endif
    }

    namespace
    {
        // The one snapshot in flight. Request = (generation << 32) | tid;
        // only the crash thread writes it. The handler publishes the
        // generation it captured and parks until that generation is released.
        std::atomic<std::uint64_t> g_snapRequest{0};
        std::atomic<std::uint64_t> g_snapCaptured{0};
        std::atomic<std::uint64_t> g_snapReleased{0};
        std::uint64_t              g_snapGeneration = 0;
        NativeContext              g_snapContext{};

        int              g_snapSignal = 0;
        bool             g_snapInstalled = false;
        struct sigaction g_snapPrevious{};

        static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                      "the snapshot handshake must be lock-free to be async-signal-safe");

        std::uint32_t RawTid() noexcept
        {
            return KernelThreadId();
        }

        void SleepMs(long ms) noexcept
        {
            timespec ts{ ms / 1000, (ms % 1000) * 1000000L };
            ::nanosleep(&ts, nullptr);
        }

        void OnSnapshotSignal(int, siginfo_t*, void* ucv)
        {
            const int savedErrno = errno;
            const std::uint64_t request = g_snapRequest.load(std::memory_order_acquire);
            if (request != 0 && static_cast<std::uint32_t>(request) == RawTid() && ucv)
            {
                const std::uint64_t generation = request >> 32;
                CopySignalContext(g_snapContext, ucv);
                g_snapCaptured.store(generation, std::memory_order_release);

                // Parked: this thread's stack must not move until the
                // requester is done with it. Bounded, so a requester that
                // died cannot leave a host thread parked forever.
                for (int i = 0; i < 5000; ++i)
                {
                    if (g_snapReleased.load(std::memory_order_acquire) >= generation) break;
                    SleepMs(1);
                }
            }
            errno = savedErrno;
        }
    }

    int SnapshotSignal() noexcept
    {
        return g_snapSignal;
    }

    bool InstallSnapshotSignal() noexcept
    {
        if (g_snapInstalled) return true;
#if ARC_PLATFORM_MACOS
        // No signal on macOS (and no SIGRTMIN): SnapshotThread suspends the
        // target with Mach.
        g_snapInstalled = true;
        return true;
#else
        g_snapSignal = SIGRTMIN + 4;
        struct sigaction sa{};
        sa.sa_sigaction = &OnSnapshotSignal;
        sigemptyset(&sa.sa_mask);
        // SA_RESTART: the interrupted thread's restartable syscalls resume as
        // if nothing happened. SA_ONSTACK: a thread near its stack limit still
        // gets captured on its alternate stack.
        sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
        if (::sigaction(g_snapSignal, &sa, &g_snapPrevious) != 0) return false;
        g_snapInstalled = true;
        return true;
#endif
    }

    void RemoveSnapshotSignal() noexcept
    {
        if (!g_snapInstalled) return;
#if !ARC_PLATFORM_MACOS
        ::sigaction(g_snapSignal, &g_snapPrevious, nullptr);
#endif
        g_snapInstalled = false;
    }

    bool SnapshotThread(std::uint32_t tid, std::uint32_t timeoutMs, ParkedFn whileParked, void* user) noexcept
    {
#if ARC_PLATFORM_MACOS
        (void)timeoutMs;   // a Mach suspend is synchronous
        if (!g_snapInstalled || tid == 0 || tid == RawTid()) return false;
        const thread_act_t thread = static_cast<thread_act_t>(tid);
        if (::thread_suspend(thread) != KERN_SUCCESS) return false;

        std::memset(&g_snapContext, 0, sizeof(g_snapContext));
#if defined(__aarch64__)
        mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
        bool captured = ::thread_get_state(thread, ARM_THREAD_STATE64,
                                           reinterpret_cast<thread_state_t>(&g_snapContext.__ss), &n) == KERN_SUCCESS;
        n = ARM_NEON_STATE64_COUNT;
        (void)::thread_get_state(thread, ARM_NEON_STATE64, reinterpret_cast<thread_state_t>(&g_snapContext.__ns), &n);
        n = ARM_EXCEPTION_STATE64_COUNT;
        (void)::thread_get_state(thread, ARM_EXCEPTION_STATE64, reinterpret_cast<thread_state_t>(&g_snapContext.__es), &n);
#elif defined(__x86_64__)
        mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
        bool captured = ::thread_get_state(thread, x86_THREAD_STATE64,
                                           reinterpret_cast<thread_state_t>(&g_snapContext.__ss), &n) == KERN_SUCCESS;
        n = x86_FLOAT_STATE64_COUNT;
        (void)::thread_get_state(thread, x86_FLOAT_STATE64, reinterpret_cast<thread_state_t>(&g_snapContext.__fs), &n);
        n = x86_EXCEPTION_STATE64_COUNT;
        (void)::thread_get_state(thread, x86_EXCEPTION_STATE64, reinterpret_cast<thread_state_t>(&g_snapContext.__es), &n);
#else
        bool captured = false;
#endif
        if (captured && whileParked)
            whileParked(tid, g_snapContext, user);
        ::thread_resume(thread);
        return captured;
#elif defined(__linux__)
        if (!g_snapInstalled || tid == 0 || tid == RawTid()) return false;

        const std::uint64_t generation = ++g_snapGeneration;
        g_snapRequest.store((generation << 32) | tid, std::memory_order_release);

        bool captured = false;
        if (::syscall(SYS_tgkill, ::getpid(), static_cast<pid_t>(tid), g_snapSignal) == 0)
        {
            for (std::uint32_t waited = 0; waited <= timeoutMs; ++waited)
            {
                if (g_snapCaptured.load(std::memory_order_acquire) == generation) { captured = true; break; }
                SleepMs(1);
            }
        }

        if (captured && whileParked)
            whileParked(tid, g_snapContext, user);

        // Close the request BEFORE releasing: a late handler from this
        // generation that has not read the request yet then finds nothing.
        g_snapRequest.store(0, std::memory_order_release);
        g_snapReleased.store(generation, std::memory_order_release);
        return captured;
#else
        (void)tid; (void)timeoutMs; (void)whileParked; (void)user;
        return false;
#endif
    }

    void BlockAsyncSignalsOnThisThread() noexcept
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGTERM);
        sigaddset(&set, SIGHUP);
        sigaddset(&set, SIGQUIT);
        sigaddset(&set, SIGPIPE);
        sigaddset(&set, SIGCHLD);
        // NOT the snapshot signal: the watchdog and the console forwarder
        // belong in a dump like any thread (a blocked one would only cost the
        // dump its 100 ms timeout), and the crash thread is excluded by tid.
        ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
    }
}
