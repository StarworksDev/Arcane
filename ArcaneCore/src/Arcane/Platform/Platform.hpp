#pragma once

// Arcane/Platform/Platform.hpp -- the engine's platform alias header (Linux
// port, 2026-10-05). Header-only, no dependencies, safe to include from any
// module (Core, Client, hosts, tools, tests, game modules).
//
// New platform guards key off ARC_PLATFORM_* instead of raw _WIN32 /
// __linux__, so a later platform (macOS, a console) is one edit here rather
// than a sweep. Pre-existing `#if defined(_WIN32)` blocks are NOT rewritten
// wholesale (no churn for its own sake); they migrate when touched.
//
// Exactly one of WINDOWS / LINUX / MACOS is defined to 1; POSIX is 1 on
// every non-Windows target. Test with `#if ARC_PLATFORM_WINDOWS` (an
// undefined macro reads as 0 in #if).
#if defined(_WIN32)
    #define ARC_PLATFORM_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
    #define ARC_PLATFORM_MACOS 1
    #define ARC_PLATFORM_POSIX 1
#elif defined(__linux__)
    #define ARC_PLATFORM_LINUX 1
    #define ARC_PLATFORM_POSIX 1
#elif defined(__unix__)
    #define ARC_PLATFORM_POSIX 1
#else
    #error "Arcane: unknown platform"
#endif

#include <string>
#include <string_view>

namespace Arcane::Platform
{
    // The file-name shape of a LOADED module (a game module, a test plugin)
    // and of an executable. A host names a module by stem + this extension;
    // the build (premake5.lua's arcane_module / arcane_exe) emits the same
    // spelling, so the two cannot disagree. ArcaneCore/ArcaneClient
    // themselves keep the toolchain's lib prefix on ELF (they are LINKED,
    // never named by a host), see SharedLibraryFileName.
#if ARC_PLATFORM_WINDOWS
    inline constexpr std::string_view kModuleExtension   = ".dll";
    inline constexpr std::string_view kExecutableSuffix  = ".exe";
    inline constexpr std::string_view kSharedLibPrefix   = "";
#elif ARC_PLATFORM_MACOS
    inline constexpr std::string_view kModuleExtension   = ".dylib";
    inline constexpr std::string_view kExecutableSuffix  = "";
    inline constexpr std::string_view kSharedLibPrefix   = "lib";
#else
    inline constexpr std::string_view kModuleExtension   = ".so";
    inline constexpr std::string_view kExecutableSuffix  = "";
    inline constexpr std::string_view kSharedLibPrefix   = "lib";
#endif

    // "HotReloadPluginV1" -> "HotReloadPluginV1.dll" / "HotReloadPluginV1.so".
    inline std::string ModuleFileName(std::string_view stem)
    {
        std::string out(stem);
        out += kModuleExtension;
        return out;
    }

    // "ArcaneServer" -> "ArcaneServer.exe" / "ArcaneServer".
    inline std::string ExecutableFileName(std::string_view stem)
    {
        std::string out(stem);
        out += kExecutableSuffix;
        return out;
    }

    // A LINKED engine shared library: "ArcaneCore" -> "ArcaneCore.dll" /
    // "libArcaneCore.so".
    inline std::string SharedLibraryFileName(std::string_view stem)
    {
        std::string out(kSharedLibPrefix);
        out += stem;
        out += kModuleExtension;
        return out;
    }

    // A manifest names its game module with the platform it was authored on
    // ("ReferenceGame.dll" in a .arcproj written on Windows). The module's
    // identity is its STEM; the extension is the running platform's. So
    // "ReferenceGame.dll" -> "ReferenceGame.so" on Linux. On Windows this is
    // the identity -- the authored name is used exactly as it always was. A
    // name without a known module extension is returned as-is.
    inline std::string NativeModuleFileName(std::string_view authored)
    {
#if ARC_PLATFORM_WINDOWS
        return std::string(authored);
#else
        for (std::string_view ext : { std::string_view(".dll"), std::string_view(".so"), std::string_view(".dylib") })
        {
            if (authored.size() > ext.size() && authored.substr(authored.size() - ext.size()) == ext)
                return ModuleFileName(authored.substr(0, authored.size() - ext.size()));
        }
        return std::string(authored);
#endif
    }
}
