#pragma once

// Where a project's per-user locations resolve in THIS build, and the suite's
// private per-user root (settings S7-DIST).
//
// The locations go through the product's own resolver (PathsConfigFor +
// Paths, as Runtime's UserCVarDir does): a dev build keeps them in the
// project's Saved/; a Dist build keeps them in the per-user OS dir named by
// the project's company and game (settings S7, spec s8.2, s11.0).
//
// In Dist that per-user dir is the player's profile, so test_main installs a
// PrivateUserDataRoot before anything resolves a path: this process and its
// children read LOCALAPPDATA (XDG_DATA_HOME and XDG_CONFIG_HOME off Windows)
// as a folder under TEMP, removed when the run ends. A Dist run never writes
// the real profile. ScopedUserDataBase redirects the same bases for one case.

#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectOpenOptions.hpp>
#include <Arcane/Project/ProjectPaths.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>   // _getpid
#include <stdlib.h>    // _wputenv_s
#else
#include <unistd.h>    // getpid
#endif

namespace Arcane::Test
{
    // The Paths config for the project at `root`, with the identity from its
    // manifest. Opens the project without the diag:// mount: an identity
    // read, nothing else.
    inline Paths::Config ProjectPathsConfig(const std::filesystem::path& root)
    {
        ProjectOpenOptions opts;
        opts.mountDiagnostics = false;
        const std::optional<Project> project = Project::Open(root, {}, opts);
        INFO("project " << root.string());
        REQUIRE(project.has_value());
        const Paths::Config current = Paths::Current();
        return PathsConfigFor(*project, current.engineDir, kDistBuild || current.dist);
    }

    // The User cvar rung's folder for the project at `root`.
    inline std::filesystem::path UserConfigDir(const std::filesystem::path& root)
    {
        return Paths::Join(Paths::Location::GameUserDir, ProjectPathsConfig(root), "Config");
    }

    // UserConfigDir, emptied. In Dist every project with one game name shares
    // that folder, so a case that did not empty it would read an earlier
    // case's archive. REQUIREs a folder (never a relative path), a clean
    // removal and the folder's absence: a stale archive fails the case.
    inline std::filesystem::path FreshUserConfigDir(const std::filesystem::path& root)
    {
        const std::filesystem::path dir = UserConfigDir(root);
        REQUIRE_FALSE(dir.empty());
        INFO("user config dir " << dir.string());
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        INFO("remove_all: " << ec.message());
        REQUIRE_FALSE(ec);
        const bool present = std::filesystem::exists(dir, ec);
        INFO("exists: " << ec.message());
        REQUIRE_FALSE(ec);
        REQUIRE_FALSE(present);
        return dir;
    }

    // The project's DiagnosticsDir: <root>/Saved/Diagnostics in dev, the
    // per-user <game>/Diagnostics in Dist.
    inline std::filesystem::path DiagnosticsDirFor(const std::filesystem::path& root)
    {
        return Paths::Resolve(Paths::Location::DiagnosticsDir, ProjectPathsConfig(root));
    }

    // The OS per-user bases Paths reads: LOCALAPPDATA on Windows; XDG_DATA_HOME
    // (UserRoot) and XDG_CONFIG_HOME (a Dist GameUserDir) on Linux. macOS
    // UserRoot is ~/Library/Application Support/Arcane, so HOME is redirected
    // too; GameUserDir still follows XDG_CONFIG_HOME.
    using EnvString = std::filesystem::path::string_type;
#if defined(_WIN32)
    inline constexpr const wchar_t* kUserDataEnv[] = { L"LOCALAPPDATA" };
    inline std::optional<EnvString> GetUserDataEnv(const wchar_t* name)
    {
        if (const wchar_t* v = _wgetenv(name)) return EnvString(v);
        return std::nullopt;
    }
    inline bool SetUserDataEnv(const wchar_t* name, const std::optional<EnvString>& value)
    {
        return _wputenv_s(name, value ? value->c_str() : L"") == 0;   // L"" removes it
    }
#elif defined(__APPLE__)
    inline constexpr const char* kUserDataEnv[] = { "XDG_DATA_HOME", "XDG_CONFIG_HOME", "HOME" };
#else
    inline constexpr const char* kUserDataEnv[] = { "XDG_DATA_HOME", "XDG_CONFIG_HOME" };
#endif
#if !defined(_WIN32)
    inline std::optional<EnvString> GetUserDataEnv(const char* name)
    {
        if (const char* v = std::getenv(name)) return EnvString(v);
        return std::nullopt;
    }
    inline bool SetUserDataEnv(const char* name, const std::optional<EnvString>& value)
    {
        return (value ? ::setenv(name, value->c_str(), 1) : ::unsetenv(name)) == 0;
    }
#endif

    // Points the per-user bases at `base` for the scope (this process and the
    // children it starts), then restores them. Ok() is false unless every
    // base was set and reads back as `base`.
    class ScopedUserDataBase
    {
    public:
        explicit ScopedUserDataBase(const std::filesystem::path& base)
        {
            for (std::size_t i = 0; i < std::size(kUserDataEnv); ++i)
            {
                m_saved[i] = GetUserDataEnv(kUserDataEnv[i]);
                const bool set = SetUserDataEnv(kUserDataEnv[i], base.native());
                const std::optional<EnvString> now = GetUserDataEnv(kUserDataEnv[i]);
                m_ok = m_ok && set && now && *now == base.native();
            }
        }
        ~ScopedUserDataBase()
        {
            for (std::size_t i = 0; i < std::size(kUserDataEnv); ++i)
                (void)SetUserDataEnv(kUserDataEnv[i], m_saved[i]);
        }
        ScopedUserDataBase(const ScopedUserDataBase&) = delete;
        ScopedUserDataBase& operator=(const ScopedUserDataBase&) = delete;

        [[nodiscard]] bool Ok() const { return m_ok; }

    private:
        std::optional<EnvString> m_saved[std::size(kUserDataEnv)];
        bool m_ok = true;
    };

    // True when `p` is `dir` or lies below it (lexically).
    inline bool IsUnder(const std::filesystem::path& p, const std::filesystem::path& dir)
    {
        if (p.empty() || dir.empty()) return false;
        const std::filesystem::path rel = p.lexically_normal().lexically_relative(dir.lexically_normal());
        return !rel.empty() && *rel.begin() != "..";
    }

    struct SuiteUserData
    {
        Paths::PlatformDirs real;            // the bases the process started with
        std::filesystem::path privateRoot;   // empty unless a PrivateUserDataRoot is live
    };

    inline SuiteUserData& SuiteUserDataState()
    {
        static SuiteUserData state;
        return state;
    }

    // Dist only: a dev build keeps game data in the project's Saved/. Points
    // the per-user bases at <TEMP>/arcane-tests-userdata-<pid> for the whole
    // run (ScopedUserDataBase) and removes that folder on destruction.
    //
    // Every step is checked: the temp dir, clearing a stale root, creating
    // it, setting each base, and Paths resolving UserRoot and a Dist
    // GameUserDir under it. Ok() is false and Error() says which step failed
    // otherwise; test_main then runs no case (a run without the redirect
    // could write the real profile).
    class PrivateUserDataRoot
    {
    public:
        PrivateUserDataRoot()
        {
            if (!kDistBuild) return;
            m_error = Install();
            if (!m_error.empty())
            {
                m_base.reset();   // restore the real bases before anything else runs
                SuiteUserDataState().privateRoot.clear();
            }
        }

        ~PrivateUserDataRoot()
        {
            m_base.reset();
            if (!m_root.empty())
            {
                std::error_code ec;
                std::filesystem::remove_all(m_root, ec);
            }
            SuiteUserDataState().privateRoot.clear();
        }

        PrivateUserDataRoot(const PrivateUserDataRoot&) = delete;
        PrivateUserDataRoot& operator=(const PrivateUserDataRoot&) = delete;

        [[nodiscard]] bool Ok() const { return m_error.empty(); }
        [[nodiscard]] const std::string& Error() const { return m_error; }

    private:
        // Empty on success, else the failed step.
        std::string Install()
        {
            SuiteUserData& state = SuiteUserDataState();
            state.real = Paths::CurrentPlatformDirs();
            std::error_code ec;
            const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
            if (ec) return "no temp directory: " + ec.message();
            if (temp.empty() || !temp.is_absolute()) return "temp directory is not absolute: '" + temp.string() + "'";
#if defined(_WIN32)
            const std::string tag = std::to_string(_getpid());
#else
            const std::string tag = std::to_string(::getpid());
#endif
            const std::filesystem::path root = temp / ("arcane-tests-userdata-" + tag);
            std::filesystem::remove_all(root, ec);
            if (ec) return "cannot clear a stale " + root.string() + ": " + ec.message();
            m_root = root;   // ours from here: the destructor removes it
            std::filesystem::create_directories(root, ec);
            if (ec) return "cannot create " + root.string() + ": " + ec.message();
            if (!std::filesystem::is_directory(root, ec) || ec) return "not a directory after create: " + root.string();

            m_base.emplace(root);
            if (!m_base->Ok()) return "cannot point the per-user environment at " + root.string();

            // Paths reads the bases fresh every call: prove it now resolves the
            // per-user locations under the private root.
            const std::filesystem::path userRoot = Paths::UserRoot();
            if (!IsUnder(userRoot, root))
                return "Paths::UserRoot() is '" + userRoot.string() + "', not under " + root.string();
            Paths::Config probe;
            probe.dist = true;
            probe.companyName = "ArcaneTests";
            probe.gameName = "PrivateRootProbe";
            const std::filesystem::path gameUser = Paths::Resolve(Paths::Location::GameUserDir, probe);
            if (!IsUnder(gameUser, root))
                return "the Dist GameUserDir is '" + gameUser.string() + "', not under " + root.string();

            state.privateRoot = root;
            return {};
        }

        std::filesystem::path m_root;
        std::optional<ScopedUserDataBase> m_base;
        std::string m_error;
    };
}
