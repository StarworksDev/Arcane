#include <Arcane/Base/Engine.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Version.hpp>
#include <Arcane/Platform/Process.hpp>   // Platform::ExecutablePath (POSIX)

#include <algorithm>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <filesystem>
#include <system_error>
#endif

namespace Arcane
{
    const char* BuildInfo()
    {
        // Composed from Core's VersionString so the two can never diverge.
        static const std::string s_info = std::string(VersionString())
#if defined(ARC_BUILD_DEBUG)
            + " [Debug]";
#elif defined(ARC_BUILD_RELEASE)
            + " [Release]";
#else
            + " [Dist]";
#endif
        return s_info.c_str();
    }

    uint32_t PluginABIVersion()
    {
        // Compiled INTO Arcane.dll, so the value returned is the DLL's own -- see
        // the declaration for why a host must not report its own header copy.
        return kGamePluginABIVersion;
    }

    std::string ExecutablePathUtf8()
    {
#ifdef _WIN32
        // Grow-and-retry: GetModuleFileNameW truncates and returns the buffer
        // size (setting ERROR_INSUFFICIENT_BUFFER) rather than the needed length,
        // so a path longer than the current buffer is only detectable by
        // n == size. MAX_PATH is not a real ceiling under long-path awareness.
        std::wstring wide(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, wide.data(),
                                               static_cast<DWORD>(wide.size()));
            if (n == 0)
                return {};                       // OS could not answer
            if (n < wide.size())
            {
                wide.resize(n);
                break;
            }
            if (wide.size() >= 65536)
                return {};                       // absurd; refuse to keep doubling
            wide.resize(wide.size() * 2);
        }

        // Explicit CP_UTF8, NOT path::string(): the narrow conversion
        // std::filesystem performs is the implementation's "native narrow
        // encoding", which is exactly the ANSI-codepage behaviour this function
        // exists to avoid.
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                              static_cast<int>(wide.size()),
                                              nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
            return {};
        std::string out(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                out.data(), bytes, nullptr, nullptr) <= 0)
            return {};
#else
        // POSIX (/proc/self/exe on Linux, _NSGetExecutablePath on macOS --
        // Arcane/Platform/Process.hpp): narrow paths are already byte
        // strings, so no re-encoding step.
        const std::filesystem::path self = Platform::ExecutablePath();
        if (self.empty())
            return {};
        std::string out = self.generic_string();
#endif
        // Forward slashes so consumers (the Hub's JSON, config files) never have
        // to unescape backslashes.
        std::replace(out.begin(), out.end(), '\\', '/');
        return out;
    }
}
