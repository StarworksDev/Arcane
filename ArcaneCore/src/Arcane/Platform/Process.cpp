// Arcane/Platform/Process.cpp -- see Process.hpp.

#include <Arcane/Platform/Process.hpp>

#include <cstdlib>

#if ARC_PLATFORM_WINDOWS
    #include <windows.h>
#else
    #include <dlfcn.h>
    #include <unistd.h>
    #include <climits>
    #include <cstdlib>
    #include <fstream>
    #include <sstream>
    #include <string>
    #include <vector>
#endif

#if ARC_PLATFORM_LINUX
    #include <link.h>   // dlinfo(RTLD_DI_LINKMAP)
#endif

#if ARC_PLATFORM_MACOS
    #include <libproc.h>
    #include <mach-o/dyld.h>
    #include <mach-o/loader.h>
    #include <cstring>
    #include <mach/mach_time.h>
    #include <sys/proc.h>       // SZOMB
    #include <sys/proc_info.h>
#endif

namespace Arcane::Platform
{
    std::filesystem::path ExecutablePath()
    {
#if ARC_PLATFORM_WINDOWS
        std::wstring buf(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (n == 0)
                return {};
            if (n < buf.size())
            {
                buf.resize(n);
                return std::filesystem::path(buf);
            }
            buf.resize(buf.size() * 2);
        }
#elif ARC_PLATFORM_MACOS
        // _NSGetExecutablePath may name the exe through a symlink or with
        // "./" components; realpath gives the one canonical spelling (what
        // /proc/self/exe yields on Linux).
        std::uint32_t size = 0;
        ::_NSGetExecutablePath(nullptr, &size);
        std::vector<char> raw(size + 1, '\0');
        if (::_NSGetExecutablePath(raw.data(), &size) != 0)
            return {};
        char resolved[PATH_MAX] = {};
        if (::realpath(raw.data(), resolved))
            return std::filesystem::path(resolved);
        return std::filesystem::path(raw.data());
#else
        std::error_code ec;
        std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
        return ec ? std::filesystem::path() : self;
#endif
    }

    std::filesystem::path LoadedLibraryPath(void* handle)
    {
        if (!handle)
            return {};
#if ARC_PLATFORM_WINDOWS
        wchar_t buf[MAX_PATH] = {};
        const DWORD n = ::GetModuleFileNameW(static_cast<HMODULE>(handle), buf, MAX_PATH);
        return n ? std::filesystem::path(std::wstring(buf, n)) : std::filesystem::path();
#elif ARC_PLATFORM_MACOS
        // dyld has no handle -> path query. A handle for an ALREADY-loaded
        // image is the same handle dlopen returned (refcounted), so match the
        // image list against RTLD_NOLOAD opens of each image's own path.
        const std::uint32_t count = ::_dyld_image_count();
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const char* name = ::_dyld_get_image_name(i);
            if (!name)
                continue;
            void* probe = ::dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
            if (!probe)
                continue;
            const bool match = probe == handle;
            ::dlclose(probe);
            if (match)
                return std::filesystem::path(name);
        }
        return {};
#else
        link_map* map = nullptr;
        if (::dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map && map->l_name)
            return std::filesystem::path(map->l_name);
        return {};
#endif
    }

#if ARC_PLATFORM_POSIX
    ProcessStat QueryProcess(std::uint32_t pid)
    {
        ProcessStat st;
#if ARC_PLATFORM_MACOS
        proc_bsdinfo bsd{};
        if (::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd)) != static_cast<int>(sizeof(bsd)))
            return st;
        st.found  = true;
        st.exited = bsd.pbi_status == SZOMB;
        st.start  = static_cast<std::uint64_t>(bsd.pbi_start_tvsec) * 1000000ull + bsd.pbi_start_tvusec;
        proc_taskinfo task{};
        if (::proc_pidinfo(static_cast<int>(pid), PROC_PIDTASKINFO, 0, &task, sizeof(task)) == static_cast<int>(sizeof(task)))
        {
            // pti_total_* are Mach absolute-time units (nanoseconds on Intel,
            // 24 MHz ticks on Apple Silicon): convert through the timebase.
            mach_timebase_info_data_t tb{};
            ::mach_timebase_info(&tb);
            const std::uint64_t ticks = task.pti_total_user + task.pti_total_system;
            st.cpuNs = tb.denom ? static_cast<std::uint64_t>(static_cast<unsigned __int128>(ticks) * tb.numer / tb.denom) : ticks;
        }
        return st;
#else
        // Field 3 is the state ('Z' = exited, awaiting its parent's reap; 'X'
        // = dead), fields 14/15 utime/stime in clock ticks, field 22 the start
        // time in clock ticks since boot -- a recycled pid has a different one.
        std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
        if (!f.is_open())
            return st;
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const std::size_t close = all.rfind(')');   // comm may contain spaces and ')'
        if (close == std::string::npos || close + 2 >= all.size())
            return st;
        std::istringstream rest(all.substr(close + 2));
        std::string field;
        std::uint64_t utime = 0, stime = 0;
        char state = '?';
        for (int i = 3; i <= 22 && rest >> field; ++i)
        {
            if (i == 3)  state = field.empty() ? '?' : field[0];
            if (i == 14) utime = std::strtoull(field.c_str(), nullptr, 10);
            if (i == 15) stime = std::strtoull(field.c_str(), nullptr, 10);
            if (i == 22) st.start = std::strtoull(field.c_str(), nullptr, 10);
        }
        st.found  = true;
        st.exited = state == 'Z' || state == 'X';
        const long hz = ::sysconf(_SC_CLK_TCK);
        st.cpuNs = (utime + stime) * (1000000000ull / static_cast<std::uint64_t>(hz > 0 ? hz : 100));
        return st;
#endif
    }
#endif

#if ARC_PLATFORM_MACOS
    ImageExtent MachImageExtent(const void* machHeader, std::intptr_t slide) noexcept
    {
        const auto* header = static_cast<const mach_header_64*>(machHeader);
        if (!header || header->magic != MH_MAGIC_64)
            return {};
        std::uint64_t lo = ~std::uint64_t{ 0 };
        std::uint64_t hi = 0;
        const auto* cmd = reinterpret_cast<const load_command*>(header + 1);
        for (std::uint32_t c = 0; c < header->ncmds; ++c)
        {
            if (cmd->cmd == LC_SEGMENT_64)
            {
                const auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
                if (std::strncmp(seg->segname, SEG_PAGEZERO, sizeof(seg->segname)) != 0 && seg->vmsize != 0)
                {
                    if (seg->vmaddr < lo) lo = seg->vmaddr;
                    if (seg->vmaddr + seg->vmsize > hi) hi = seg->vmaddr + seg->vmsize;
                }
            }
            if (cmd->cmdsize == 0)
                break;
            cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const unsigned char*>(cmd) + cmd->cmdsize);
        }
        if (hi <= lo)
            return {};
        return ImageExtent{ lo + static_cast<std::uint64_t>(slide), hi - lo };
    }

    namespace
    {
        // Calls fn(cmd) for every load command of a 64-bit Mach-O header.
        template <class Fn>
        void ForEachLoadCommand(const void* machHeader, Fn&& fn) noexcept
        {
            const auto* header = static_cast<const mach_header_64*>(machHeader);
            if (!header || header->magic != MH_MAGIC_64)
                return;
            const auto* cmd = reinterpret_cast<const load_command*>(header + 1);
            for (std::uint32_t c = 0; c < header->ncmds; ++c)
            {
                if (fn(cmd))
                    return;
                if (cmd->cmdsize == 0)
                    return;
                cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const unsigned char*>(cmd) + cmd->cmdsize);
            }
        }
    }

    ImageExtent MachImageExtentAt(const void* mappedHeader) noexcept
    {
        std::intptr_t slide = 0;
        bool found = false;
        ForEachLoadCommand(mappedHeader, [&](const load_command* cmd) noexcept
        {
            if (cmd->cmd != LC_SEGMENT_64)
                return false;
            const auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
            if (std::strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) != 0)
                return false;
            slide = static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(mappedHeader) - seg->vmaddr);
            found = true;
            return true;
        });
        return found ? MachImageExtent(mappedHeader, slide) : ImageExtent{};
    }

    bool MachImageUuid(const void* mappedHeader, unsigned char (&uuid)[16]) noexcept
    {
        bool found = false;
        ForEachLoadCommand(mappedHeader, [&](const load_command* cmd) noexcept
        {
            if (cmd->cmd != LC_UUID)
                return false;
            std::memcpy(uuid, reinterpret_cast<const uuid_command*>(cmd)->uuid, 16);
            found = true;
            return true;
        });
        return found;
    }
#endif
}
