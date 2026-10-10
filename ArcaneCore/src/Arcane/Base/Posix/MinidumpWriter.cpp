// Base/Posix/MinidumpWriter.cpp -- the POSIX `.dmp` (Diagnostics POSIX port,
// 2026-10-05).
//
// WHAT IT IS. Windows writes its `.dmp` with MiniDumpWriteDump. Linux has no
// OS minidump, and the two obvious substitutes are both wrong for this path:
// a kernel core dump happens only on a default-action death (the crash path
// exits with its own code, 10) and lands wherever core_pattern says (often a
// pipe to apport/systemd-coredump), and gcore is an external debugger. So the
// POSIX `.dmp` is a BREAKPAD-FORMAT MINIDUMP: the same Microsoft MDMP
// container MiniDumpWriteDump produces, with Breakpad's Linux streams -- the
// format Chromium, Firefox, Crashpad and Sentry all write on Linux, and the
// one rust-minidump's minidump-stackwalk, Breakpad's minidump_stackwalk and
// minidump-2-core (-> a core gdb opens) all read. The envelope's siblingDmp
// contract is therefore unchanged: a `.dmp` is the artifact a debugger opens.
//
// WHAT IS IN IT. Every thread of the process (the crash thread aside) with
// its full register context and its stack memory; the module list with each
// ELF's GNU build id as a Breakpad `BpEL` CodeView record (what a symbol
// server matches on); the exception stream (the signal for a fault, or
// DUMP_REQUESTED around the walked thread for a hang/manual report -- the
// Windows path's STILL_ACTIVE record); SystemInfo; 256 bytes around the
// faulting pc; and verbatim copies of /proc/self/{maps,status,cmdline,auxv}
// and /etc/lsb-release. Deliberately NOT /proc/self/environ: an environment
// is where secrets live, and a crash report is a file people attach.
//
// HOW. Heap-free and lock-free, on the crash thread: raw open/write, fixed
// static buffers, memory read with checked process_vm_readv, other threads'
// contexts taken with the snapshot signal (PosixCrashSupport.hpp) and their
// stacks copied WHILE they are parked. Streams are appended in order and the
// directory is written last, then the header is patched at offset 0.
//
// Structure layouts are the Microsoft/Breakpad ones (dbghelp.h /
// google_breakpad/common/minidump_format.h), packed to 4 as dbghelp packs
// them; written field by field at explicit offsets so no compiler padding
// can leak in. x86-64 contexts only (the workspace's one architecture); on
// another architecture threads carry no context.
//
// macOS (2026-10-07): the same container with Breakpad's Mac conventions --
// MD_OS_MAC_OS_X, ARM64 (MS ARM64_NT_CONTEXT layout, Breakpad's
// MD_CONTEXT_ARM64) or AMD64 thread contexts from the Mach thread state, the
// module list read lock-free from dyld's own dyld_all_image_infos (what
// Breakpad's Mac writer reads) with each image's LC_UUID as an RSDS CodeView
// record (Breakpad's Mac debug-id), stacks bounded by mach_vm_region, and
// the exception as a Mach exception type (EXC_BAD_ACCESS, ...) rather than a
// signal. No /proc streams exist to copy.

#include <Arcane/Base/Posix/PosixCrashSupport.hpp>
#include <Arcane/Core/Constant.hpp>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <cpuid.h>
#endif
#if ARC_PLATFORM_MACOS
#include <Arcane/Platform/Process.hpp>   // MachImageExtent
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld_images.h>          // dyld_all_image_infos: the image list, lock-free
#include <mach-o/loader.h>               // LC_UUID
#include <sys/sysctl.h>
#endif

namespace Arcane::Diagnostics::Internal::Posix
{
    namespace
    {
        // ---- format constants ----------------------------------------------
        ARC_CONSTANT("file format: MDMP signature of a Breakpad minidump")
        constexpr std::uint32_t kSignature = 0x504d444du;   // "MDMP"
        ARC_CONSTANT("file format: MINIDUMP_VERSION")
        constexpr std::uint32_t kVersion   = 0x0000a793u;   // MINIDUMP_VERSION

        ARC_CONSTANT("file format: ThreadListStream type in a minidump directory")
        constexpr std::uint32_t kThreadListStream = 3;
        ARC_CONSTANT("file format: ModuleListStream type in a minidump directory")
        constexpr std::uint32_t kModuleListStream = 4;
        ARC_CONSTANT("file format: MemoryListStream type in a minidump directory")
        constexpr std::uint32_t kMemoryListStream = 5;
        ARC_CONSTANT("file format: ExceptionStream type in a minidump directory")
        constexpr std::uint32_t kExceptionStream  = 6;
        ARC_CONSTANT("file format: SystemInfoStream type in a minidump directory")
        constexpr std::uint32_t kSystemInfoStream = 7;
        ARC_CONSTANT("file format: Breakpad Linux ProcStatus stream type")
        constexpr std::uint32_t kLinuxProcStatus  = 0x47670004u;
        ARC_CONSTANT("file format: Breakpad Linux LsbRelease stream type")
        constexpr std::uint32_t kLinuxLsbRelease  = 0x47670005u;
        ARC_CONSTANT("file format: Breakpad Linux CmdLine stream type")
        constexpr std::uint32_t kLinuxCmdLine     = 0x47670006u;
        ARC_CONSTANT("file format: Breakpad Linux Auxv stream type")
        constexpr std::uint32_t kLinuxAuxv        = 0x47670008u;
        ARC_CONSTANT("file format: Breakpad Linux Maps stream type")
        constexpr std::uint32_t kLinuxMaps        = 0x47670009u;

        ARC_CONSTANT("file format: MD_OS_LINUX in the minidump SystemInfo stream")
        constexpr std::uint32_t kOsLinux        = 0x8201u;        // MD_OS_LINUX
        ARC_CONSTANT("file format: MD_OS_MAC_OS_X in the minidump SystemInfo stream")
        constexpr std::uint32_t kOsMac          = 0x8101u;        // MD_OS_MAC_OS_X
        ARC_CONSTANT("file format: Breakpad ELF CodeView signature BpEL")
        constexpr std::uint32_t kCvSignatureElf = 0x4270454cu;    // "BpEL"
        ARC_CONSTANT("file format: PDB70 CodeView signature, the Mac LC_UUID record")
        constexpr std::uint32_t kCvSignaturePdb70 = 0x53445352u;  // "RSDS" (Breakpad Mac: LC_UUID as the GUID, age 0)

        ARC_CONSTANT("file format: MDRawContextAMD64 byte size")
        constexpr std::size_t kContextSize    = 1232;   // MDRawContextAMD64 / CONTEXT (x64); the buffer size
        ARC_CONSTANT("file format: MDRawContextARM64 byte size")
        constexpr std::size_t kContextArm64Size = 912;  // MDRawContextARM64 / ARM64_NT_CONTEXT
#if ARC_PLATFORM_MACOS && defined(__aarch64__)
        constexpr std::size_t kContextBytes   = kContextArm64Size;
#else
        constexpr std::size_t kContextBytes   = kContextSize;
#endif
        ARC_CONSTANT("file format: MINIDUMP_THREAD byte size")
        constexpr std::size_t kThreadSize     = 48;
        ARC_CONSTANT("file format: MINIDUMP_MODULE byte size")
        constexpr std::size_t kModuleSize     = 108;
        ARC_CONSTANT("file format: MINIDUMP_MEMORY_DESCRIPTOR byte size")
        constexpr std::size_t kMemDescSize    = 16;
        ARC_CONSTANT("file format: MINIDUMP_EXCEPTION_STREAM byte size")
        constexpr std::size_t kExceptionSize  = 168;
        ARC_CONSTANT("file format: MINIDUMP_SYSTEM_INFO byte size")
        constexpr std::size_t kSystemInfoSize = 56;

        ARC_CONSTANT("crash-path capacity: threads named in one POSIX minidump")
        constexpr std::size_t kMaxThreads  = 512;
        ARC_CONSTANT("crash-path capacity: modules named in one POSIX minidump")
        constexpr std::size_t kMaxModules  = 1024;
        ARC_CONSTANT("crash-path capacity: memory ranges in one POSIX minidump, one stack per thread plus the faulting thread")
        constexpr std::size_t kMaxMemory   = kMaxThreads + 1;
        ARC_CONSTANT("crash-path capacity: streams in one POSIX minidump directory")
        constexpr std::size_t kMaxStreams  = 16;
        ARC_CONSTANT("crash-path capacity: stack bytes captured per thread in a POSIX minidump")
        constexpr std::uint64_t kStackCap  = 256 * 1024;   // per thread
        ARC_CONSTANT("crash-path: System V red zone below rsp, still live, included in the stack capture")
        constexpr std::uint64_t kRedZone   = 128;          // System V: below rsp is still live

        // ---- little-endian field writers ------------------------------------
        void Put16(unsigned char* b, std::size_t off, std::uint16_t v) noexcept { std::memcpy(b + off, &v, 2); }
        void Put32(unsigned char* b, std::size_t off, std::uint32_t v) noexcept { std::memcpy(b + off, &v, 4); }
        void Put64(unsigned char* b, std::size_t off, std::uint64_t v) noexcept { std::memcpy(b + off, &v, 8); }

        // ---- the writer (crash thread only, so plain statics) --------------
        struct Location { std::uint32_t size = 0; std::uint32_t rva = 0; };
        struct Directory { std::uint32_t type; Location loc; };
        struct ThreadRec { std::uint32_t tid; std::uint64_t stackStart; Location stack; Location context; };
        struct MemoryRec { std::uint64_t start; Location data; };

        int           g_fd     = -1;
        std::uint32_t g_cursor = 0;
        bool          g_ok     = true;

        Directory g_dirs[kMaxStreams];
        std::size_t g_dirCount = 0;
        ThreadRec g_threads[kMaxThreads];
        std::size_t g_threadCount = 0;
        MemoryRec g_memory[kMaxMemory];
        std::size_t g_memoryCount = 0;

        alignas(16) unsigned char g_chunk[64 * 1024];
        char          g_maps[512 * 1024];
        std::size_t   g_mapsLen = 0;
        char          g_procFile[64 * 1024];

        // SystemInfo, snapshotted off the crash path.
        struct SysInfo
        {
            std::uint16_t arch = 0, level = 0, revision = 0;
            std::uint8_t  cpus = 0;
            std::uint32_t major = 0, minor = 0, build = 0;
            std::uint32_t vendor[3] = { 0, 0, 0 };
            std::uint32_t versionInfo = 0, featureInfo = 0, amdExt = 0;
            char          csd[512] = {};
        } g_sys;

        std::uint32_t Append(const void* data, std::size_t size) noexcept
        {
            const std::uint32_t rva = g_cursor;
            if (size == 0) return rva;
            if (!WriteAll(g_fd, data, size)) g_ok = false;
            g_cursor += static_cast<std::uint32_t>(size);
            return rva;
        }

        void AlignTo(std::uint32_t a) noexcept
        {
            static const unsigned char zeros[16] = {};
            const std::uint32_t pad = (a - (g_cursor % a)) % a;
            if (pad) Append(zeros, pad);
        }

        void AddStream(std::uint32_t type, Location loc) noexcept
        {
            if (g_dirCount < kMaxStreams) g_dirs[g_dirCount++] = { type, loc };
        }

        // UTF-8 -> MINIDUMP_STRING (u32 byte length + UTF-16 + NUL), heap-free.
        // Invalid bytes become U+FFFD; nothing on this path may refuse a name.
        std::uint32_t AppendString(const char* utf8) noexcept
        {
            static char16_t s_wide[4096];
            std::size_t n = 0;
            const auto* p = reinterpret_cast<const unsigned char*>(utf8 ? utf8 : "");
            while (*p && n + 2 < sizeof(s_wide) / sizeof(s_wide[0]))
            {
                std::uint32_t cp = 0xFFFD;
                std::size_t   len = 1;
                if (p[0] < 0x80) { cp = p[0]; }
                else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); len = 2; }
                else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
                { cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); len = 3; }
                else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
                { cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu); len = 4; }
                p += len;
                if (cp >= 0x10000)
                {
                    cp -= 0x10000;
                    s_wide[n++] = static_cast<char16_t>(0xD800 + (cp >> 10));
                    s_wide[n++] = static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
                }
                else
                {
                    s_wide[n++] = static_cast<char16_t>(cp);
                }
            }
            s_wide[n] = 0;
            AlignTo(4);
            const std::uint32_t bytes = static_cast<std::uint32_t>(n * 2);
            const std::uint32_t rva = Append(&bytes, 4);
            Append(s_wide, (n + 1) * 2);
            return rva;
        }

        // ---- /proc/self/maps -------------------------------------------------
        std::uint64_t ParseHex(const char*& p) noexcept
        {
            std::uint64_t v = 0;
            for (;; ++p)
            {
                const char c = *p;
                if      (c >= '0' && c <= '9') v = (v << 4) | static_cast<std::uint64_t>(c - '0');
                else if (c >= 'a' && c <= 'f') v = (v << 4) | static_cast<std::uint64_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v = (v << 4) | static_cast<std::uint64_t>(c - 'A' + 10);
                else break;
            }
            return v;
        }

        struct MapLine
        {
            std::uint64_t start = 0, end = 0, offset = 0, inode = 0;
            const char*   path  = nullptr;   // into g_maps, NUL-terminated by Next()
            std::size_t   pathLen = 0;
        };

        // Iterates g_maps line by line (the buffer is edited in place: each
        // line's '\n' becomes '\0' the first time it is visited).
        bool NextMapLine(std::size_t& at, MapLine& out) noexcept
        {
            while (at < g_mapsLen)
            {
                char* line = g_maps + at;
                std::size_t len = 0;
                while (at + len < g_mapsLen && line[len] != '\n' && line[len] != '\0') ++len;
                at += len + 1;
                line[len] = '\0';

                const char* p = line;
                out = MapLine{};
                out.start = ParseHex(p); if (*p != '-') continue; ++p;
                out.end   = ParseHex(p); if (*p != ' ') continue; ++p;
                while (*p && *p != ' ') ++p;                 // perms
                while (*p == ' ') ++p;
                out.offset = ParseHex(p);
                while (*p == ' ') ++p;
                while (*p && *p != ' ') ++p;                 // dev
                while (*p == ' ') ++p;
                while (*p >= '0' && *p <= '9') out.inode = out.inode * 10 + static_cast<std::uint64_t>(*p++ - '0');
                while (*p == ' ') ++p;
                out.path = p;
                out.pathLen = std::strlen(p);
                return true;
            }
            return false;
        }

        // [start, end) of the mapping holding `address`, or {0,0}.
        void FindMapping(std::uint64_t address, std::uint64_t& start, std::uint64_t& end) noexcept
        {
            start = end = 0;
#if ARC_PLATFORM_MACOS
            // mach_vm_region: the first region at or above `address`.
            mach_vm_address_t       regionStart = address;
            mach_vm_size_t          regionSize  = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t  count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t             object = MACH_PORT_NULL;
            if (::mach_vm_region(::mach_task_self(), &regionStart, &regionSize, VM_REGION_BASIC_INFO_64,
                                 reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS
                && address >= regionStart && address < regionStart + regionSize)
            {
                start = regionStart;
                end   = regionStart + regionSize;
            }
            return;
#endif
            std::size_t at = 0;
            MapLine m;
            while (NextMapLine(at, m))
            {
                if (address >= m.start && address < m.end) { start = m.start; end = m.end; return; }
            }
        }

        // ---- contexts ------------------------------------------------------
        void BuildContext(const NativeContext& uc, unsigned char (&b)[kContextSize]) noexcept
        {
            std::memset(b, 0, sizeof(b));
#if ARC_PLATFORM_MACOS && defined(__aarch64__)
            // ARM64_NT_CONTEXT: flags, cpsr, x0..x28, fp, lr, sp, pc, v0..v31,
            // fpcr, fpsr (debug registers left zero).
            Put32(b, 0, 0x00400007u);   // ARM64 | CONTROL | INTEGER | FLOATING_POINT
            Put32(b, 4, uc.__ss.__cpsr);
            for (int i = 0; i < 29; ++i)
                Put64(b, 8 + static_cast<std::size_t>(i) * 8, uc.__ss.__x[i]);
            Put64(b, 240, static_cast<std::uint64_t>(__darwin_arm_thread_state64_get_fp(uc.__ss)));
            Put64(b, 248, reinterpret_cast<std::uint64_t>(__darwin_arm_thread_state64_get_lr_fptr(uc.__ss)) & 0x00007FFFFFFFFFFFull);
            Put64(b, 256, static_cast<std::uint64_t>(__darwin_arm_thread_state64_get_sp(uc.__ss)));
            Put64(b, 264, ContextPc(uc));
            std::memcpy(b + 272, &uc.__ns.__v, 512);
            Put32(b, 784, uc.__ns.__fpcr);
            Put32(b, 788, uc.__ns.__fpsr);
#elif ARC_PLATFORM_MACOS && defined(__x86_64__)
            Put32(b, 48, 0x0010000Fu);   // AMD64 | CONTROL | INTEGER | SEGMENTS | FLOATING_POINT
            const auto& t = uc.__ss;
            Put16(b, 56, static_cast<std::uint16_t>(t.__cs));
            Put16(b, 62, static_cast<std::uint16_t>(t.__fs));
            Put16(b, 64, static_cast<std::uint16_t>(t.__gs));
            Put32(b, 68, static_cast<std::uint32_t>(t.__rflags));
            const std::uint64_t regs[16] = { t.__rax, t.__rcx, t.__rdx, t.__rbx, t.__rsp, t.__rbp, t.__rsi, t.__rdi,
                                             t.__r8, t.__r9, t.__r10, t.__r11, t.__r12, t.__r13, t.__r14, t.__r15 };
            for (int i = 0; i < 16; ++i)
                Put64(b, 120 + static_cast<std::size_t>(i) * 8, regs[i]);
            Put64(b, 248, t.__rip);
            Put32(b, 52, uc.__fs.__fpu_mxcsr);
            std::memcpy(b + 256, &uc.__fs.__fpu_fcw, 512);   // the FXSAVE image starts at __fpu_fcw
#elif defined(__linux__) && defined(__x86_64__)
            const auto& g = uc.uc_mcontext.gregs;
            const bool  haveFp = uc.uc_mcontext.fpregs != nullptr;
            Put32(b, 48, 0x0010000Fu);   // AMD64 | CONTROL | INTEGER | SEGMENTS | FLOATING_POINT
            Put32(b, 52, haveFp ? uc.uc_mcontext.fpregs->mxcsr : 0u);
            const std::uint64_t csgsfs = static_cast<std::uint64_t>(g[REG_CSGSFS]);
            Put16(b, 56, static_cast<std::uint16_t>(csgsfs & 0xFFFF));           // cs
            Put16(b, 64, static_cast<std::uint16_t>((csgsfs >> 16) & 0xFFFF));   // gs
            Put16(b, 62, static_cast<std::uint16_t>((csgsfs >> 32) & 0xFFFF));   // fs
            Put16(b, 66, static_cast<std::uint16_t>((csgsfs >> 48) & 0xFFFF));   // ss (kernel >= 4.8 stores it here)
            Put32(b, 68, static_cast<std::uint32_t>(g[REG_EFL]));
            const int order[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                    REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
            for (int i = 0; i < 16; ++i)
                Put64(b, 120 + static_cast<std::size_t>(i) * 8, static_cast<std::uint64_t>(g[order[i]]));
            Put64(b, 248, static_cast<std::uint64_t>(g[REG_RIP]));
            if (haveFp)
                std::memcpy(b + 256, uc.uc_mcontext.fpregs, 512);   // FXSAVE == XMM_SAVE_AREA32
#else
            (void)uc;
#endif
        }

        Location AppendContext(const NativeContext* uc) noexcept
        {
#if (defined(__linux__) && defined(__x86_64__)) || (ARC_PLATFORM_MACOS && (defined(__x86_64__) || defined(__aarch64__)))
            if (!uc) return {};
            static unsigned char s_ctx[kContextSize];
            BuildContext(*uc, s_ctx);
            AlignTo(16);
            return { static_cast<std::uint32_t>(kContextBytes), Append(s_ctx, kContextBytes) };
#else
            (void)uc;
            return {};
#endif
        }

        // Copies [start, start + size) of this process into the file; the
        // descriptor names what was actually readable (a prefix).
        Location AppendMemory(std::uint64_t start, std::uint64_t size) noexcept
        {
            AlignTo(16);
            Location loc{ 0, g_cursor };
            while (size > 0)
            {
                const std::size_t want = size < sizeof(g_chunk) ? static_cast<std::size_t>(size) : sizeof(g_chunk);
                const std::size_t got  = SafeRead(start, g_chunk, want);
                if (got == 0) break;
                Append(g_chunk, got);
                loc.size += static_cast<std::uint32_t>(got);
                start += got;
                size  -= got;
                if (got < want) break;
            }
            return loc;
        }

        void AddThread(std::uint32_t tid, const NativeContext& uc) noexcept
        {
            if (g_threadCount >= kMaxThreads) return;
            ThreadRec& t = g_threads[g_threadCount];
            t = ThreadRec{ tid, 0, {}, {} };

            const std::uint64_t sp = ContextSp(uc);
            if (sp != 0)
            {
                std::uint64_t lo = 0, hi = 0;
                FindMapping(sp, lo, hi);
                std::uint64_t start = sp > kRedZone ? sp - kRedZone : sp;
                if (lo != 0 && start < lo) start = lo;
                std::uint64_t end = start + kStackCap;
                if (hi != 0 && end > hi) end = hi;
                t.stackStart = start;
                t.stack      = AppendMemory(start, end - start);
                if (t.stack.size != 0 && g_memoryCount < kMaxMemory)
                    g_memory[g_memoryCount++] = { start, t.stack };
            }
            t.context = AppendContext(&uc);
            ++g_threadCount;
        }

        void OnParkedThread(std::uint32_t tid, const NativeContext& uc, void*) noexcept
        {
            AddThread(tid, uc);   // the thread is parked: its stack holds still for the copy
        }

        // ---- modules -------------------------------------------------------
        struct ModuleRec
        {
            std::uint64_t base = 0, end = 0;
            const char*   path = nullptr;
            std::size_t   pathLen = 0;
        };
        ModuleRec g_modules[kMaxModules];
        std::size_t g_moduleCount = 0;

        void CollectModules() noexcept
        {
            g_moduleCount = 0;
#if ARC_PLATFORM_MACOS
            // dyld's own record of every loaded image, read through checked
            // reads and without dyld's lock (dyld clears infoArray while it
            // edits it: a null array is "no modules", never a stale one).
            task_dyld_info_data_t dyldInfo{};
            mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
            if (::task_info(::mach_task_self(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&dyldInfo), &count) != KERN_SUCCESS)
                return;
            dyld_all_image_infos all{};
            if (SafeRead(dyldInfo.all_image_info_addr, &all, sizeof(all)) < offsetof(dyld_all_image_infos, notification)
                || !all.infoArray)
                return;
            static char s_paths[kMaxModules][512];
            for (std::uint32_t i = 0; i < all.infoArrayCount && g_moduleCount < kMaxModules; ++i)
            {
                dyld_image_info image{};
                if (SafeRead(reinterpret_cast<std::uint64_t>(all.infoArray + i), &image, sizeof(image)) != sizeof(image)
                    || !image.imageLoadAddress)
                    continue;
                char* path = s_paths[g_moduleCount];
                const std::size_t got = image.imageFilePath
                    ? SafeRead(reinterpret_cast<std::uint64_t>(image.imageFilePath), path, sizeof(s_paths[0]) - 1) : 0;
                path[got] = '\0';
                std::size_t len = 0;
                while (len < got && path[len] != '\0') ++len;
                path[len] = '\0';
                const Arcane::Platform::ImageExtent extent = Arcane::Platform::MachImageExtentAt(image.imageLoadAddress);
                if (extent.size == 0)
                    continue;
                g_modules[g_moduleCount++] = { extent.base, extent.base + extent.size, path, len };
            }
            return;
#endif
            std::size_t at = 0;
            MapLine m;
            while (NextMapLine(at, m))
            {
                if (m.inode == 0 || m.pathLen == 0 || m.path[0] != '/') continue;
                if (g_moduleCount > 0)
                {
                    ModuleRec& last = g_modules[g_moduleCount - 1];
                    if (last.pathLen == m.pathLen && std::memcmp(last.path, m.path, m.pathLen) == 0)
                    {
                        if (m.end > last.end) last.end = m.end;
                        continue;
                    }
                }
                if (m.offset != 0) continue;   // a module starts at its file's first page
                unsigned char magic[4] = {};
                if (SafeRead(m.start, magic, 4) != 4 || magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F')
                    continue;                  // a mapped font, a cache file: not code
                if (g_moduleCount < kMaxModules)
                    g_modules[g_moduleCount++] = { m.start, m.end, m.path, m.pathLen };
            }
        }

        // The GNU build id of the ELF image mapped at `base` (NT_GNU_BUILD_ID
        // in a PT_NOTE), read through checked reads. Returns its length.
        std::size_t ReadBuildId(std::uint64_t base, unsigned char* out, std::size_t cap) noexcept
        {
            unsigned char eh[64];
            if (SafeRead(base, eh, sizeof(eh)) != sizeof(eh) || eh[4] != 2 /*ELFCLASS64*/) return 0;
            std::uint64_t phoff = 0; std::uint16_t phentsize = 0, phnum = 0;
            std::memcpy(&phoff, eh + 32, 8);
            std::memcpy(&phentsize, eh + 54, 2);
            std::memcpy(&phnum, eh + 56, 2);
            if (phentsize != 56 || phnum == 0 || phnum > 64) return 0;

            unsigned char ph[64 * 56];
            if (SafeRead(base + phoff, ph, std::size_t(phnum) * 56) != std::size_t(phnum) * 56) return 0;

            // Load bias: the first PT_LOAD's vaddr is what `base` maps.
            std::uint64_t bias = base;
            for (std::uint16_t i = 0; i < phnum; ++i)
            {
                std::uint32_t type; std::uint64_t vaddr;
                std::memcpy(&type, ph + i * 56, 4);
                std::memcpy(&vaddr, ph + i * 56 + 16, 8);
                if (type == 1 /*PT_LOAD*/) { bias = base - (vaddr & ~std::uint64_t(0xFFF)); break; }
            }
            for (std::uint16_t i = 0; i < phnum; ++i)
            {
                std::uint32_t type; std::uint64_t vaddr, memsz;
                std::memcpy(&type, ph + i * 56, 4);
                std::memcpy(&vaddr, ph + i * 56 + 16, 8);
                std::memcpy(&memsz, ph + i * 56 + 40, 8);
                if (type != 4 /*PT_NOTE*/ || memsz == 0) continue;
                unsigned char notes[2048];
                const std::size_t n = SafeRead(bias + vaddr, notes, memsz < sizeof(notes) ? memsz : sizeof(notes));
                for (std::size_t at = 0; at + 12 <= n;)
                {
                    std::uint32_t namesz, descsz, ntype;
                    std::memcpy(&namesz, notes + at, 4);
                    std::memcpy(&descsz, notes + at + 4, 4);
                    std::memcpy(&ntype,  notes + at + 8, 4);
                    const std::size_t nameAt = at + 12;
                    const std::size_t descAt = nameAt + ((namesz + 3u) & ~3u);
                    const std::size_t next   = descAt + ((descsz + 3u) & ~3u);
                    if (next > n) break;
                    if (ntype == 3 /*NT_GNU_BUILD_ID*/ && namesz == 4 && std::memcmp(notes + nameAt, "GNU", 4) == 0)
                    {
                        const std::size_t len = descsz < cap ? descsz : cap;
                        std::memcpy(out, notes + descAt, len);
                        return len;
                    }
                    at = next;
                }
            }
            return 0;
        }

        Location WriteModuleList() noexcept
        {
            static unsigned char s_rows[kMaxModules][kModuleSize];
            static char          s_path[4096];
            for (std::size_t i = 0; i < g_moduleCount; ++i)
            {
                const ModuleRec& m = g_modules[i];
                unsigned char* row = s_rows[i];
                std::memset(row, 0, kModuleSize);
                Put64(row, 0, m.base);
                const std::uint64_t size = m.end - m.base;
                Put32(row, 8, static_cast<std::uint32_t>(size > 0xFFFFFFFFull ? 0xFFFFFFFFull : size));

                const std::size_t len = m.pathLen < sizeof(s_path) - 1 ? m.pathLen : sizeof(s_path) - 1;
                std::memcpy(s_path, m.path, len);
                s_path[len] = '\0';
                Put32(row, 20, AppendString(s_path));

#if ARC_PLATFORM_MACOS
                // RSDS: signature, the LC_UUID as the GUID, age 0, then the
                // file name -- Breakpad's Mac debug identifier.
                unsigned char uuid[16];
                if (Arcane::Platform::MachImageUuid(reinterpret_cast<const void*>(m.base), uuid))
                {
                    unsigned char cv[4 + 16 + 4 + 256] = {};
                    std::memcpy(cv, &kCvSignaturePdb70, 4);
                    std::memcpy(cv + 4, uuid, 16);
                    const char* leaf = s_path;
                    for (const char* c = s_path; *c; ++c) if (*c == '/') leaf = c + 1;
                    std::size_t leafLen = std::strlen(leaf);
                    if (leafLen > 255) leafLen = 255;
                    std::memcpy(cv + 24, leaf, leafLen);
                    AlignTo(4);
                    const std::uint32_t cvSize = static_cast<std::uint32_t>(24 + leafLen + 1);
                    const std::uint32_t rva = Append(cv, cvSize);
                    Put32(row, 76, cvSize);
                    Put32(row, 80, rva);
                }
                continue;
#endif
                unsigned char cv[4 + 64];
                const std::size_t idLen = ReadBuildId(m.base, cv + 4, 64);
                if (idLen != 0)
                {
                    std::memcpy(cv, &kCvSignatureElf, 4);
                    AlignTo(4);
                    const std::uint32_t rva = Append(cv, 4 + idLen);
                    Put32(row, 76, static_cast<std::uint32_t>(4 + idLen));
                    Put32(row, 80, rva);
                }
            }
            AlignTo(8);
            const std::uint32_t count = static_cast<std::uint32_t>(g_moduleCount);
            const std::uint32_t rva = Append(&count, 4);
            for (std::size_t i = 0; i < g_moduleCount; ++i) Append(s_rows[i], kModuleSize);
            return { static_cast<std::uint32_t>(4 + g_moduleCount * kModuleSize), rva };
        }

        Location WriteThreadList() noexcept
        {
            AlignTo(8);
            const std::uint32_t count = static_cast<std::uint32_t>(g_threadCount);
            const std::uint32_t rva = Append(&count, 4);
            for (std::size_t i = 0; i < g_threadCount; ++i)
            {
                unsigned char row[kThreadSize] = {};
                const ThreadRec& t = g_threads[i];
                Put32(row, 0, t.tid);
                Put64(row, 24, t.stackStart);
                Put32(row, 32, t.stack.size);
                Put32(row, 36, t.stack.rva);
                Put32(row, 40, t.context.size);
                Put32(row, 44, t.context.rva);
                Append(row, kThreadSize);
            }
            return { static_cast<std::uint32_t>(4 + g_threadCount * kThreadSize), rva };
        }

        Location WriteMemoryList() noexcept
        {
            AlignTo(8);
            const std::uint32_t count = static_cast<std::uint32_t>(g_memoryCount);
            const std::uint32_t rva = Append(&count, 4);
            for (std::size_t i = 0; i < g_memoryCount; ++i)
            {
                unsigned char row[kMemDescSize] = {};
                Put64(row, 0, g_memory[i].start);
                Put32(row, 8, g_memory[i].data.size);
                Put32(row, 12, g_memory[i].data.rva);
                Append(row, kMemDescSize);
            }
            return { static_cast<std::uint32_t>(4 + g_memoryCount * kMemDescSize), rva };
        }

        Location WriteSystemInfo() noexcept
        {
            const std::uint32_t csdRva = AppendString(g_sys.csd);
            unsigned char b[kSystemInfoSize] = {};
            Put16(b, 0, g_sys.arch);
            Put16(b, 2, g_sys.level);
            Put16(b, 4, g_sys.revision);
            b[6] = g_sys.cpus;
            Put32(b, 8, g_sys.major);
            Put32(b, 12, g_sys.minor);
            Put32(b, 16, g_sys.build);
#if ARC_PLATFORM_MACOS
            Put32(b, 20, kOsMac);
#else
            Put32(b, 20, kOsLinux);
#endif
            Put32(b, 24, csdRva);
            Put32(b, 32, g_sys.vendor[0]);
            Put32(b, 36, g_sys.vendor[1]);
            Put32(b, 40, g_sys.vendor[2]);
            Put32(b, 44, g_sys.versionInfo);
            Put32(b, 48, g_sys.featureInfo);
            Put32(b, 52, g_sys.amdExt);
            AlignTo(8);
            return { static_cast<std::uint32_t>(kSystemInfoSize), Append(b, kSystemInfoSize) };
        }

        void AddFileStream(std::uint32_t type, const char* path) noexcept
        {
            const std::size_t n = ReadFileInto(path, g_procFile, sizeof(g_procFile));
            if (n == 0) return;
            AlignTo(4);
            AddStream(type, { static_cast<std::uint32_t>(n), Append(g_procFile, n) });
        }

        std::uint32_t ParseUInt(const char*& p) noexcept
        {
            std::uint32_t v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + static_cast<std::uint32_t>(*p++ - '0');
            return v;
        }

        void AppendText(char* dst, std::size_t cap, std::size_t& used, const char* s) noexcept
        {
            while (s && *s && used + 1 < cap) dst[used++] = *s++;
            dst[used] = '\0';
        }
    }

    void SnapshotSystemInfoForDump() noexcept
    {
        g_sys = SysInfo{};
#if defined(__x86_64__)
        g_sys.arch = 9;   // PROCESSOR_ARCHITECTURE_AMD64
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (__get_cpuid(0, &a, &b, &c, &d)) { g_sys.vendor[0] = b; g_sys.vendor[1] = d; g_sys.vendor[2] = c; }
        if (__get_cpuid(1, &a, &b, &c, &d))
        {
            g_sys.versionInfo = a;
            g_sys.featureInfo = d;
            const unsigned family = ((a >> 8) & 0xF) + ((((a >> 8) & 0xF) == 0xF) ? ((a >> 20) & 0xFF) : 0);
            const unsigned model  = ((a >> 4) & 0xF) | (((((a >> 8) & 0xF) == 6) || (((a >> 8) & 0xF) == 0xF)) ? (((a >> 16) & 0xF) << 4) : 0);
            g_sys.level    = static_cast<std::uint16_t>(family);
            g_sys.revision = static_cast<std::uint16_t>((model << 8) | (a & 0xF));
        }
        if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) g_sys.amdExt = d;
#elif defined(__aarch64__)
        g_sys.arch = 12;  // PROCESSOR_ARCHITECTURE_ARM64
#endif
        const long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
        g_sys.cpus = static_cast<std::uint8_t>(cpus > 255 ? 255 : (cpus < 1 ? 1 : cpus));

#if ARC_PLATFORM_MACOS
        // Breakpad's Mac SystemInfo: the PRODUCT version (15.6.1) in
        // major/minor/build, the OS build string ("24G90") as the CSD.
        {
            char product[64] = {};
            std::size_t len = sizeof(product) - 1;
            if (::sysctlbyname("kern.osproductversion", product, &len, nullptr, 0) == 0)
            {
                const char* p = product;
                g_sys.major = ParseUInt(p); if (*p == '.') ++p;
                g_sys.minor = ParseUInt(p); if (*p == '.') ++p;
                g_sys.build = ParseUInt(p);
            }
            len = sizeof(g_sys.csd) - 1;
            if (::sysctlbyname("kern.osversion", g_sys.csd, &len, nullptr, 0) != 0)
                g_sys.csd[0] = '\0';
        }
#else
        utsname u{};
        if (::uname(&u) == 0)
        {
            const char* p = u.release;
            g_sys.major = ParseUInt(p); if (*p == '.') ++p;
            g_sys.minor = ParseUInt(p); if (*p == '.') ++p;
            g_sys.build = ParseUInt(p);
            // Breakpad's spelling of the CSD string: "<sysname> <release> <version> <machine>"
            std::size_t used = 0;
            AppendText(g_sys.csd, sizeof(g_sys.csd), used, u.sysname);
            AppendText(g_sys.csd, sizeof(g_sys.csd), used, " ");
            AppendText(g_sys.csd, sizeof(g_sys.csd), used, u.release);
            AppendText(g_sys.csd, sizeof(g_sys.csd), used, " ");
            AppendText(g_sys.csd, sizeof(g_sys.csd), used, u.version);
            AppendText(g_sys.csd, sizeof(g_sys.csd), used, " ");
            AppendText(g_sys.csd, sizeof(g_sys.csd), used, u.machine);
        }
#endif
    }

    bool WriteMinidump(const DumpRequest& r) noexcept
    {
        g_fd = OpenForWrite(r.path, /*append*/false);
        if (g_fd < 0) return false;
        g_cursor = 0;
        g_ok = true;
        g_dirCount = g_threadCount = g_memoryCount = g_moduleCount = 0;

        // Header placeholder; patched last.
        unsigned char header[32] = {};
        Append(header, sizeof(header));

#if ARC_PLATFORM_MACOS
        g_mapsLen = 0;   // no /proc: FindMapping asks mach_vm_region instead
#else
        g_mapsLen = ReadFileInto("/proc/self/maps", g_maps, sizeof(g_maps));
#endif

        // Threads: the ones the report already holds first (their stacks are
        // parked in the crash path's own waits), then every other thread,
        // each snapshotted and copied while it is parked.
        for (std::size_t i = 0; i < r.knownCount; ++i)
            if (r.known[i].context) AddThread(r.known[i].tid, *r.known[i].context);

        static std::uint32_t s_tids[kMaxThreads];
        const std::size_t tidCount = ListThreads(s_tids, kMaxThreads);
        timespec begin{};
        ::clock_gettime(CLOCK_MONOTONIC, &begin);
        for (std::size_t i = 0; i < tidCount; ++i)
        {
            const std::uint32_t tid = s_tids[i];
            if (tid == r.excludeTid) continue;
            bool known = false;
            for (std::size_t k = 0; k < r.knownCount; ++k) known = known || r.known[k].tid == tid;
            if (known) continue;

            // A total budget as well as a per-thread one: a process full of
            // threads that block the signal must not stretch the report.
            timespec now{};
            ::clock_gettime(CLOCK_MONOTONIC, &now);
            const long elapsedMs = (now.tv_sec - begin.tv_sec) * 1000 + (now.tv_nsec - begin.tv_nsec) / 1000000;
            if (elapsedMs > 3000) break;
            SnapshotThread(tid, 100, &OnParkedThread, nullptr);
        }

        // 256 bytes around the faulting pc: enough to disassemble the fault.
        if (r.exceptionContext && g_memoryCount < kMaxMemory)
        {
            const std::uint64_t pc = ContextPc(*r.exceptionContext);
            if (pc > 128)
            {
                const Location code = AppendMemory(pc - 128, 256);
                if (code.size != 0) g_memory[g_memoryCount++] = { pc - 128, code };
            }
        }

        AddStream(kThreadListStream, WriteThreadList());

        CollectModules();
        AddStream(kModuleListStream, WriteModuleList());
        AddStream(kMemoryListStream, WriteMemoryList());

        {
            const Location ctx = AppendContext(r.exceptionContext);
            unsigned char ex[kExceptionSize] = {};
            Put32(ex, 0, r.exceptionTid);
#if ARC_PLATFORM_MACOS
            // A Mac minidump names a MACH exception (Breakpad's
            // MD_EXCEPTION_MAC_*): the signal the BSD layer delivered is
            // mapped back to the exception type it came from; the signal
            // itself rides in ExceptionInformation[0] for a reader that
            // wants it. A requested dump is MD_EXCEPTION_MAC_SIMULATED.
            std::uint32_t machCode = 0x43507378u;   // MD_EXCEPTION_MAC_SIMULATED ("CPsx")
            switch (r.exceptionCode)
            {
                case SIGSEGV: case SIGBUS: machCode = 1; break;   // EXC_BAD_ACCESS
                case SIGILL:               machCode = 2; break;   // EXC_BAD_INSTRUCTION
                case SIGFPE:               machCode = 3; break;   // EXC_ARITHMETIC
                case SIGTRAP:              machCode = 6; break;   // EXC_BREAKPOINT
                case SIGABRT:              machCode = 5; break;   // EXC_SOFTWARE (an abort is EXC_CRASH's SIGABRT; Breakpad files it here)
                default: break;
            }
            Put32(ex, 8, machCode);
            Put32(ex, 12, r.exceptionFlags);
            if (r.exceptionCode != kDumpRequested)
            {
                Put32(ex, 32, 1);                    // NumberParameters
                Put64(ex, 40, r.exceptionCode);      // ExceptionInformation[0]: the signal
            }
#else
            Put32(ex, 8, r.exceptionCode);
            Put32(ex, 12, r.exceptionFlags);
#endif
            Put64(ex, 24, r.exceptionAddress);
            Put32(ex, 160, ctx.size);
            Put32(ex, 164, ctx.rva);
            AlignTo(8);
            AddStream(kExceptionStream, { static_cast<std::uint32_t>(kExceptionSize), Append(ex, kExceptionSize) });
        }

        AddStream(kSystemInfoStream, WriteSystemInfo());

#if !ARC_PLATFORM_MACOS
        // The maps text was edited in place by the parser; re-read it whole.
        AddFileStream(kLinuxMaps,       "/proc/self/maps");
        AddFileStream(kLinuxProcStatus, "/proc/self/status");
        AddFileStream(kLinuxCmdLine,    "/proc/self/cmdline");
        AddFileStream(kLinuxAuxv,       "/proc/self/auxv");
        AddFileStream(kLinuxLsbRelease, "/etc/lsb-release");
#endif

        // Directory, then the header that points at it.
        AlignTo(4);
        const std::uint32_t dirRva = g_cursor;
        for (std::size_t i = 0; i < g_dirCount; ++i)
        {
            unsigned char row[12];
            Put32(row, 0, g_dirs[i].type);
            Put32(row, 4, g_dirs[i].loc.size);
            Put32(row, 8, g_dirs[i].loc.rva);
            Append(row, sizeof(row));
        }

        Put32(header, 0, kSignature);
        Put32(header, 4, kVersion);
        Put32(header, 8, static_cast<std::uint32_t>(g_dirCount));
        Put32(header, 12, dirRva);
        timespec wall{};
        ::clock_gettime(CLOCK_REALTIME, &wall);
        Put32(header, 20, static_cast<std::uint32_t>(wall.tv_sec));
        if (::pwrite(g_fd, header, sizeof(header), 0) != static_cast<ssize_t>(sizeof(header))) g_ok = false;

        CloseFd(g_fd);
        g_fd = -1;
        return g_ok;
    }
}
