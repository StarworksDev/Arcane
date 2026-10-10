#pragma once

// Arcane/Platform/Process.hpp -- the per-OS answers to "who am I, where was I
// loaded from, is that process still alive" (macOS port, 2026-10-07). One
// place for what used to be /proc reads scattered across Core, Client and the
// tests, so a platform is a branch HERE rather than a sweep:
//
//                      Windows               Linux                    macOS
//   ExecutablePath     GetModuleFileNameW    /proc/self/exe           _NSGetExecutablePath + realpath
//   LoadedLibraryPath  GetModuleFileNameW    dlinfo(RTLD_DI_LINKMAP)  dyld image list x dlopen(RTLD_NOLOAD)
//   QueryProcess       (not provided)        /proc/<pid>/stat         proc_pidinfo(PIDTBSDINFO/PIDTASKINFO)
//   UserDataDirectory  %LOCALAPPDATA%        $XDG_DATA_HOME|~/.local/share  ~/Library/Application Support
//
// None of these is async-signal-safe; the crash path has its own primitives
// (Base/Posix/PosixCrashSupport.hpp).

#include <Arcane/Core/Api.hpp>
#include <Arcane/Platform/Platform.hpp>

#include <cstdint>
#include <filesystem>

namespace Arcane::Platform
{
    // The running executable's absolute path; empty if the OS will not say.
    ARC_CORE_API std::filesystem::path ExecutablePath();

    // The file a dlopen()/LoadLibrary handle was loaded from; empty when the
    // handle is null or unknown to the loader.
    ARC_CORE_API std::filesystem::path LoadedLibraryPath(void* handle);

    // The per-user, machine-local data root the engine's own files live
    // under (as <root>/Arcane/...): %LOCALAPPDATA% on Windows, $XDG_DATA_HOME
    // (else ~/.local/share) on Linux, ~/Library/Application Support on macOS.
    // Empty when the environment names none.
    ARC_CORE_API std::filesystem::path UserDataDirectory();

#if ARC_PLATFORM_POSIX
    // A snapshot of another (or this) process.
    //   found:  the pid names a process the caller may inspect.
    //   exited: it has terminated and awaits its parent's reap (a zombie) --
    //           the POSIX twin of a Windows process object with an exit time.
    //   start:  an opaque, per-OS creation stamp that differs for a recycled
    //           pid (Linux: clock ticks since boot, /proc stat field 22;
    //           macOS: microseconds since the epoch). Compare, never interpret.
    //   cpuNs:  user + system CPU time consumed, in nanoseconds.
    struct ProcessStat
    {
        bool          found  = false;
        bool          exited = false;
        std::uint64_t start  = 0;
        std::uint64_t cpuNs  = 0;
    };

    ARC_CORE_API ProcessStat QueryProcess(std::uint32_t pid);
#endif

#if ARC_PLATFORM_MACOS
    // The mapped extent of a loaded Mach-O image: the union of its
    // LC_SEGMENT_64 ranges (minus an executable's __PAGEZERO guard), slid --
    // the Mach-O reading of PE's [base, base + SizeOfImage) and ELF's PT_LOAD
    // union. {0, 0} for anything that is not a 64-bit Mach-O header. Reads
    // only the mapped header (no allocation, no locks), so the crash path may
    // call it too.
    struct ImageExtent
    {
        std::uint64_t base = 0;
        std::uint64_t size = 0;
    };
    ARC_CORE_API ImageExtent MachImageExtent(const void* machHeader, std::intptr_t slide) noexcept;

    // The same for an image known only by its MAPPED header address (dyld's
    // dyld_all_image_infos list): the slide is the header's distance from
    // its __TEXT segment's link-time address.
    ARC_CORE_API ImageExtent MachImageExtentAt(const void* mappedHeader) noexcept;

    // The image's LC_UUID (its debug identifier: what dsymutil, atos and a
    // symbol server match on). False when the image carries none.
    ARC_CORE_API bool MachImageUuid(const void* mappedHeader, unsigned char (&uuid)[16]) noexcept;
#endif
}
