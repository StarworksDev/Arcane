#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Platform/Process.hpp>

#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>   // _getpid
#else
#include <unistd.h>    // getpid
#endif

namespace Arcane::Platform
{
    std::filesystem::path UserDataDirectory()
    {
#if ARC_PLATFORM_WINDOWS
        if (const wchar_t* localAppData = ::_wgetenv(L"LOCALAPPDATA"); localAppData && *localAppData)
            return std::filesystem::path(localAppData);
        return {};
#elif ARC_PLATFORM_MACOS
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / "Library" / "Application Support";
        return {};
#else
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
            return std::filesystem::path(xdg);
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".local" / "share";
        return {};
#endif
    }
}

namespace Arcane::Paths
{
    namespace
    {
        std::mutex& StateMutex() { static std::mutex m; return m; }
        Config& State() { static Config c; return c; }

        // An environment directory, read fresh every call (tests change it);
        // empty when unset or blank.
        std::filesystem::path EnvDir(const char* name)
        {
#if defined(_WIN32)
            const std::wstring wide(name, name + std::char_traits<char>::length(name));
            if (const wchar_t* v = _wgetenv(wide.c_str()); v && *v) return std::filesystem::path(v);
#else
            if (const char* v = std::getenv(name); v && *v) return std::filesystem::path(v);
#endif
            return {};
        }

        // LocalData is UserDataDirectory: %LOCALAPPDATA% on Windows,
        // $XDG_DATA_HOME (else ~/.local/share) on Linux, ~/Library/Application
        // Support on macOS. SessionLayoutDir and every other Paths consumer
        // follow that root.
        std::filesystem::path LocalData()
        {
            return Arcane::Platform::UserDataDirectory();
        }

        std::string ProcessTag()
        {
#if defined(_WIN32)
            return std::to_string(static_cast<long>(_getpid()));
#else
            return std::to_string(static_cast<long>(::getpid()));
#endif
        }

        bool HasProject(const Config& c) { return c.projectDir.has_value() && !c.projectDir->empty(); }

        std::filesystem::path Below(const std::filesystem::path& base, const std::filesystem::path& rel)
        {
            return base.empty() ? base : base / rel;
        }
    }

    void Configure(const Config& config)
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        State() = config;
    }

    Config Current()
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        return State();
    }

    Config ForProject(const std::filesystem::path& projectRoot)
    {
        Config c = Current();
        c.projectDir = projectRoot;
        return c;
    }

    std::filesystem::path UserRoot()
    {
        return Below(LocalData(), "Arcane");
    }

    std::string SanitizePathSegment(std::string_view name)
    {
        std::string out;
        out.reserve(name.size());
        for (const char c : name)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            const bool bad = u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                             c == '\\' || c == '|' || c == '?' || c == '*';
            out.push_back(bad ? '_' : c);
        }
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
        while (!out.empty() && out.front() == ' ') out.erase(out.begin());
        if (out == "." || out == "..") out.clear();
        std::string stem = out.substr(0, out.find('.'));
        for (char& c : stem) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        static constexpr std::string_view kReserved[] = {
            "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
        for (const std::string_view r : kReserved)
            if (stem == r) { out.insert(out.begin(), '_'); break; }
        return out;
    }

    std::filesystem::path ResolveGameUserDir(const Config& config, HostPlatform platform, const PlatformDirs& dirs)
    {
        if (!config.dist)
            return config.projectDir ? *config.projectDir / "Saved" : std::filesystem::path{};
        std::filesystem::path base;
        if (platform == HostPlatform::Windows)
            base = dirs.localAppData;
        else if (dirs.xdgConfigHome.generic_string().starts_with('/'))
            base = dirs.xdgConfigHome;
        else if (!dirs.home.empty())
            base = dirs.home / ".config";
        if (base.empty()) return {};
        const std::string company = SanitizePathSegment(config.companyName);
        std::string game = SanitizePathSegment(config.gameName);
        if (game.empty()) game = "ArcaneGame";
        if (!company.empty()) base /= std::filesystem::path(std::u8string(company.begin(), company.end()));
        return base / std::filesystem::path(std::u8string(game.begin(), game.end()));
    }

    PlatformDirs CurrentPlatformDirs()
    {
        PlatformDirs dirs;
        dirs.localAppData = EnvDir("LOCALAPPDATA");
        dirs.xdgConfigHome = EnvDir("XDG_CONFIG_HOME");
        dirs.home = EnvDir("HOME");
        return dirs;
    }

    std::filesystem::path Resolve(Location location, const Config& c)
    {
        const bool project = HasProject(c);
        const std::filesystem::path root = project ? *c.projectDir : std::filesystem::path{};
        switch (location)
        {
        case Location::EngineDir:           return c.engineDir;
        case Location::EngineData:          return Below(c.engineDir, "data");
        case Location::EngineConfig:        return Below(Below(c.engineDir, "data"), "EngineConfig");
        case Location::ProjectDir:          return root;
        case Location::ProjectConfig:       return Below(root, "Config");
        case Location::ProjectContent:      return Below(root, "Content");
        case Location::ProjectSaved:        return c.dist ? std::filesystem::path{} : Below(root, "Saved");
        case Location::ProjectIntermediate: return c.dist ? std::filesystem::path{} : Below(root, "Intermediate");
        case Location::ProjectCache:        return c.dist ? std::filesystem::path{} : Below(Below(root, "Saved"), "Cache");
        case Location::EditorUserDir:       return c.dist ? std::filesystem::path{} : Below(UserRoot(), "Editor");
        case Location::GameUserDir:
            return ResolveGameUserDir(c, kHostPlatform, CurrentPlatformDirs());
        case Location::DiagnosticsDir:
            if (c.dist) return Below(Resolve(Location::GameUserDir, c), "Diagnostics");
            if (project) return root / "Saved" / "Diagnostics";
            return Below(c.engineDir, "diagnostics");
        case Location::TempDir:
        {
            std::error_code ec;
            const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
            return ec ? std::filesystem::path{} : tmp / "Arcane" / ProcessTag();
        }
        }
        return {};
    }

    std::filesystem::path Join(Location location, const Config& config, const std::filesystem::path& rel)
    {
        return Below(Resolve(location, config), rel);
    }

    std::filesystem::path Get(Location location)
    {
        return Resolve(location, Current());
    }

    std::filesystem::path EnsureDir(Location location)
    {
        const Config c = Current();
        const std::filesystem::path dir = Resolve(location, c);
        if (dir.empty()) return dir;
        const bool readOnly = location == Location::EngineDir || location == Location::EngineData
                           || location == Location::EngineConfig
                           || (c.dist && (location == Location::ProjectDir || location == Location::ProjectConfig
                                          || location == Location::ProjectContent));
        if (!readOnly)
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
        }
        return dir;
    }
}
