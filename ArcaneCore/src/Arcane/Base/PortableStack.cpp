// Crash window plan 1 (Task 3): PortableStack implementation. See the
// header for the "why" -- CaptureStackFromContext is deliberately free of
// C++ locals that need unwinding (the CONTEXT copy and the std::span
// parameter are both trivially destructible), because __try/__except may
// not live in a function that has such locals (MSVC C2712).

#include <Arcane/Base/PortableStack.hpp>

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unwind.h>   // _Unwind_Backtrace: the .eh_frame walk (GCC and Clang alike)
#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/uio.h>      // process_vm_readv: a read that FAILS instead of faulting
#include <ucontext.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>   // mach_vm_read_overwrite: a read that FAILS instead of faulting
#include <signal.h>         // _STRUCT_MCONTEXT (the machine context a Darwin ucontext points at)
#endif
#endif

namespace Arcane::Diagnostics
{
#if defined(_WIN32)
    std::size_t CaptureStackFromContext(const void* nativeContext, std::span<StackFrame> out) noexcept
    {
        if (!nativeContext || out.empty())
            return 0;

        CONTEXT ctx = *static_cast<const CONTEXT*>(nativeContext);   // a COPY: unwinding mutates it
        std::size_t n = 0;
        __try
        {
            while (n < out.size() && ctx.Rip != 0)
            {
                out[n].address = ctx.Rip;
                out[n].module  = ModuleTable::Find(ctx.Rip);
                ++n;

                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (fn)
                {
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
                }
                else
                {
                    // Leaf function: the return address is at RSP.
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { /* stop where the walk faulted; n frames are valid */ }
        return n;
    }

    std::size_t CaptureCurrentStack(std::span<StackFrame> out) noexcept
    {
        CONTEXT ctx{};
        RtlCaptureContext(&ctx);
        return CaptureStackFromContext(&ctx, out);
    }
#else
#if defined(__linux__) || defined(__APPLE__)
    // Linux (Diagnostics POSIX port, 2026-10-05): a FRAME-POINTER walk over a
    // ucontext_t -- the ELF reading of "walk a COPY of a foreign context".
    // macOS (2026-10-07): the same walk over the Darwin machine context
    // (_STRUCT_MCONTEXT -- a Darwin ucontext_t only POINTS at one, so the
    // crash path stores the mcontext itself; PosixCrashSupport.hpp's
    // NativeContext). The arm64 Darwin ABI mandates the frame record
    // (x29/x30), so every Mac frame -- system libraries included -- keeps
    // the chain.
    //
    // Why not .eh_frame: _Unwind_Backtrace only walks the CALLING thread from
    // where it stands, and the crash thread walks OTHER threads (the faulting
    // one, parked in its signal handler; the main thread of a hang, parked in
    // a snapshot signal). A DWARF CFI interpreter over an arbitrary register
    // set is what libunwind is -- a dependency this walk does not need,
    // because every ELF module the engine builds keeps its frame chain
    // (premake5.lua: -fno-omit-frame-pointer on every non-Windows target, the
    // same call Ubuntu 24.04 and Fedora made distro-wide for profilers and
    // crash walks). A frame without one (a leaf, a libc routine built
    // without) costs THAT frame, never the walk: rbp still names the nearest
    // ancestor that kept the chain.
    //
    // Every read goes through SafeRead, so a corrupt chain stops the walk
    // instead of faulting the crash thread -- the job the __try/__except
    // above does on Windows.
    namespace
    {
        // process_vm_readv against our OWN pid: the kernel validates the
        // source range and returns EFAULT instead of delivering SIGSEGV
        // (same-thread-group access needs no ptrace permission). A sandbox
        // that filters the syscall (EPERM/ENOSYS) falls back to the pipe
        // trick: write() from an unmapped address is EFAULT too.
        bool SafeRead(std::uint64_t address, void* out, std::size_t size) noexcept
        {
#if defined(__APPLE__)
            // The Mach VM read of our OWN task: an unmapped or unreadable
            // range is KERN_INVALID_ADDRESS / KERN_PROTECTION_FAILURE, never
            // a signal. A plain Mach trap -- safe on the crash thread.
            mach_vm_size_t got = 0;
            return ::mach_vm_read_overwrite(::mach_task_self(), address, size,
                                            reinterpret_cast<mach_vm_address_t>(out), &got) == KERN_SUCCESS
                && got == size;
#else
            iovec local{ out, size };
            iovec remote{ reinterpret_cast<void*>(address), size };
            const ssize_t n = ::syscall(SYS_process_vm_readv, ::getpid(), &local, 1ul, &remote, 1ul, 0ul);
            if (n == static_cast<ssize_t>(size))
                return true;
            if (n >= 0 || (errno != EPERM && errno != ENOSYS))
                return false;

            int fds[2];
            if (::pipe2(fds, O_CLOEXEC) != 0)
                return false;
            const bool ok = ::write(fds[1], reinterpret_cast<const void*>(address), size) == static_cast<ssize_t>(size)
                         && ::read(fds[0], out, size) == static_cast<ssize_t>(size);
            ::close(fds[0]);
            ::close(fds[1]);
            return ok;
#endif
        }

        // Apple arm64: return addresses taken from system code may carry
        // pointer-authentication bits above the 47-bit user address space.
        std::uint64_t StripPointer(std::uint64_t address) noexcept
        {
#if defined(__APPLE__) && defined(__aarch64__)
            return address & 0x00007FFFFFFFFFFFull;
#else
            return address;
#endif
        }

        // The walk proper, over three registers. A chain link is accepted only
        // if it moves UP the stack (frames are pushed downward), stays 8-byte
        // aligned, and does not jump implausibly far -- the guards that turn
        // a garbage rbp into a short walk rather than a long wrong one.
        std::size_t WalkFrameChain(std::uint64_t pc, std::uint64_t sp, std::uint64_t fp,
                                   std::span<StackFrame> out) noexcept
        {
            ARC_CONSTANT("crash-path: a frame-chain link that jumps farther than this is garbage, not a frame")
            constexpr std::uint64_t kMaxFrameBytes = 16ull * 1024 * 1024;
            std::size_t n = 0;
            if (pc == 0)
                return 0;
            out[n].address = pc;
            out[n].module  = ModuleTable::Find(pc);
            ++n;

            std::uint64_t low = sp;
            while (n < out.size())
            {
                if (fp == 0 || (fp & 7u) != 0 || fp < low || fp - low > kMaxFrameBytes)
                    break;
                std::uint64_t link[2] = { 0, 0 };   // [saved fp, return address]
                if (!SafeRead(fp, link, sizeof(link)) || link[1] == 0)
                    break;
                link[1] = StripPointer(link[1]);
                out[n].address = link[1];
                out[n].module  = ModuleTable::Find(link[1]);
                ++n;
                if (link[0] <= fp)
                    break;   // the outermost frame (fp = 0), or a corrupt link
                low = fp + 16;
                fp  = link[0];
            }
            return n;
        }
    }

    std::size_t CaptureStackFromContext(const void* nativeContext, std::span<StackFrame> out) noexcept
    {
        if (!nativeContext || out.empty())
            return 0;
#if defined(__APPLE__)
        const auto* mc = static_cast<const _STRUCT_MCONTEXT*>(nativeContext);
#if defined(__aarch64__)
        return WalkFrameChain(StripPointer(static_cast<std::uint64_t>(__darwin_arm_thread_state64_get_pc(mc->__ss))),
                              static_cast<std::uint64_t>(__darwin_arm_thread_state64_get_sp(mc->__ss)),
                              static_cast<std::uint64_t>(__darwin_arm_thread_state64_get_fp(mc->__ss)), out);
#elif defined(__x86_64__)
        return WalkFrameChain(mc->__ss.__rip, mc->__ss.__rsp, mc->__ss.__rbp, out);
#else
        (void)mc;
        return 0;
#endif
#else
        const auto* uc = static_cast<const ucontext_t*>(nativeContext);
#if defined(__x86_64__)
        const auto& g = uc->uc_mcontext.gregs;
        return WalkFrameChain(static_cast<std::uint64_t>(g[REG_RIP]), static_cast<std::uint64_t>(g[REG_RSP]),
                              static_cast<std::uint64_t>(g[REG_RBP]), out);
#elif defined(__aarch64__)
        const auto& m = uc->uc_mcontext;
        return WalkFrameChain(m.pc, m.sp, m.regs[29], out);
#else
        (void)uc;
        return 0;
#endif
#endif
    }
#else
    // Other POSIX targets: no foreign-context walk yet.
    std::size_t CaptureStackFromContext(const void*, std::span<StackFrame>) noexcept { return 0; }
#endif

    namespace
    {
        struct UnwindState
        {
            std::span<StackFrame> out;
            std::size_t           n    = 0;
            std::size_t           skip = 1;   // CaptureCurrentStack's own frame
        };

        _Unwind_Reason_Code CollectFrame(_Unwind_Context* context, void* user)
        {
            auto& state = *static_cast<UnwindState*>(user);
            const std::uint64_t ip = static_cast<std::uint64_t>(_Unwind_GetIP(context));
            if (ip == 0)
                return _URC_END_OF_STACK;
            if (state.skip > 0)
            {
                --state.skip;
                return _URC_NO_REASON;
            }
            if (state.n >= state.out.size())
                return _URC_END_OF_STACK;
            state.out[state.n].address = ip;
            state.out[state.n].module  = ModuleTable::Find(ip);
            ++state.n;
            return _URC_NO_REASON;
        }
    }

    std::size_t CaptureCurrentStack(std::span<StackFrame> out) noexcept
    {
        if (out.empty())
            return 0;
        UnwindState state{ out };
        _Unwind_Backtrace(&CollectFrame, &state);
        return state.n;
    }
#endif

    std::string_view FormatStackFrame(std::size_t index, const StackFrame& f, std::span<char> buffer) noexcept
    {
        if (buffer.empty())
            return {};

        int written;
        if (f.module != nullptr)
        {
            const std::uint64_t offset = f.address - f.module->base;
            written = std::snprintf(buffer.data(), buffer.size(), "  %02zu  %s + 0x%llx (base 0x%016llx)",
                                     index, f.module->name,
                                     static_cast<unsigned long long>(offset),
                                     static_cast<unsigned long long>(f.module->base));
        }
        else
        {
            written = std::snprintf(buffer.data(), buffer.size(), "  %02zu  0x%016llx <unloaded or unknown module>",
                                     index, static_cast<unsigned long long>(f.address));
        }

        if (written < 0)
        {
            buffer[0] = '\0';
            return std::string_view(buffer.data(), 0);
        }

        const std::size_t len = static_cast<std::size_t>(written) < buffer.size()
                                     ? static_cast<std::size_t>(written)
                                     : buffer.size() - 1;
        return std::string_view(buffer.data(), len);
    }
}
