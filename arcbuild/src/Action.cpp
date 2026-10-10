#include "Action.hpp"

namespace arcbuild
{
    HostPlatform CurrentHostPlatform() noexcept
    {
#if defined(_WIN32)
        return HostPlatform::Windows;
#elif defined(__APPLE__)
        return HostPlatform::MacOS;
#else
        return HostPlatform::Linux;
#endif
    }

    std::string_view DefaultActionFor(
        HostPlatform platform) noexcept
    {
        switch (platform)
        {
        case HostPlatform::Windows: return "vs2026";
        case HostPlatform::Linux:   return "gmake";
        // macOS port (2026-10-07): gmake, the same Make backend as Linux --
        // one POSIX build path, and the one the engine itself is built with.
        // xcode4 stays a supported --action (an IDE build), not the default.
        case HostPlatform::MacOS:   return "gmake";
        }

        return "";
    }

    BuildBackend BackendForAction(
        std::string_view action) noexcept
    {
        if (action == "vs2022" || action == "vs2026")
            return BuildBackend::MsBuild;

        if (action == "gmake" || action == "gmakelegacy")
            return BuildBackend::Make;

        if (action == "ninja")
            return BuildBackend::Ninja;

        if (action == "xcode4")
            return BuildBackend::XcodeBuild;

        return BuildBackend::None;
    }

    const char* BuildBackendName(BuildBackend backend)
    {
        switch (backend)
        {
        case BuildBackend::None:       return "none";
        case BuildBackend::MsBuild:    return "MSBuild";
        case BuildBackend::Make:       return "Make";
        case BuildBackend::Ninja:      return "Ninja";
        case BuildBackend::XcodeBuild: return "xcodebuild";
        }

        return "???";
    }

    const char* BuildBackendPrefix(BuildBackend backend)
    {
        switch (backend)
        {
        case BuildBackend::MsBuild:    return "[msbuild]";
        case BuildBackend::Make:       return "[make]";
        case BuildBackend::Ninja:      return "[ninja]";
        case BuildBackend::XcodeBuild: return "[xcodebuild]";
        case BuildBackend::None:       return "[builder]";
        }

        return "[builder]";
    }
}
