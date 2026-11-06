#pragma once

// ServerConfig: ArcaneServer's own typed CLI result over Arcane::Cli (Core-DLL
// split, spec docs/specs/2026-09-15-core-dll-split-design.md s6: "mirrors the
// other hosts"). HostConfig (Source/Client/ArcaneClient/src/Arcane/Host/HostConfig.hpp) is
// Client -- it names a GraphicsBackend and a dozen presentation-only flags
// (--headless, --screenshot, --settle, --compare, ...) this host has no use
// for -- so ArcaneServer owns its own, narrower vocabulary over the same
// Arcane::Cli rather than dragging Client in for one parse.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Arcane::Server
{
    struct ServerConfig
    {
        std::string   projectPath;                  // --project (REQUIRED: a server with nothing to host refuses, like ArcaneRuntime)
        std::string   pluginPath;                    // --plugin override
        std::uint64_t frames         = 0;             // --frames N; 0 = run until terminated (P13)
        // --fixed-dt. Supplied, it overrides server.tickHz; otherwise the
        // tick is 1/server.tickHz (settings arc S6-8) and ServerApp fills
        // fixedDtSeconds in once the project's Config rung is applied.
        double        fixedDtSeconds  = 0.0;
        bool          fixedDtSupplied = false;
        std::string   reportPath;                     // --report <json>
        bool          printEngineInfo = false;        // --print-engine-info (same probe as the other hosts, Core-side)
        bool          adminConsole    = true;         // --no-admin-console turns off the stdin admin console (settings spec s9)

        // Servitor P0 (Task 10): the same three remote flags HostConfig has.
        // --remote arms the loopback listener (wired by Task 11);
        // --remote-descriptor <path> overrides where the attach descriptor is
        // written; --remote-idle-exit <seconds> ends a host no client has
        // talked to (0 = never). remoteIdleExitSupplied is true only when that
        // flag was typed (the fixedDtSupplied pattern). Present in every
        // configuration -- this struct is the server exe's own, not the game
        // ABI -- but a Dist Parse refuses all three flags with exit 2.
        bool          remote                 = false;
        std::string   remoteDescriptor;
        std::uint32_t remoteIdleExitSeconds  = 600;
        bool          remoteIdleExitSupplied = false;
        std::uint32_t ownerPid = 0;   // --owner-pid <pid>: with --remote, exit when that process is gone (0 = watch none)
        std::string   remoteClient;   // --client <name>: the caller name recorded in the descriptor ("" = unnamed)

        // Forward-declared here so it names ServerConfig::ParseOutcome and can be
        // the return type of Parse; DEFINED below (after the class closes) because
        // its std::optional<ServerConfig> member needs ServerConfig to be a
        // complete type, which it is not yet inside this member specification
        // (MSVC enforces this) -- same shape as HostConfig::ParseOutcome
        // (Source/Client/ArcaneClient/src/Arcane/Host/HostConfig.hpp).
        struct ParseOutcome;

        // {help} => {nullopt, 0}; a parse/validation refusal => {nullopt, 2};
        // otherwise {config, 0}. See ServerConfig.cpp for the exact refusals.
        [[nodiscard]] static ParseOutcome Parse(int argc, char** argv);

        // The server's tick rate: 1/--fixed-dt when supplied, else the
        // published server.tickHz.
        [[nodiscard]] double FixedHz() const;
    };

    struct ServerConfig::ParseOutcome { std::optional<ServerConfig> config; int exitCode = 0; };

#if !defined(ARC_BUILD_DIST)
    // Servitor P0 Task 29 (ruling B): --remote's help line, HostConfig's
    // kRemoteFlagHelp rule in this host's words (this exe never links Client).
    // An open-ended --remote server starts held at frame 0 and logs
    // "remote: holding at frame 0"; a --frames N server runs. No opt-out flag.
    inline constexpr std::string_view kRemoteFlagHelp =
        "DEV: arm the loopback remote surface a Servitor client attaches to; an open-ended run "
        "starts held at frame 0; use servitor play/resume, or pass --frames N";
#endif

    // THE RELAUNCH LINE (Diagnostics::Config::commandLine, spec S5.1), this
    // host's own copy of the rule ArcaneClient's SanitizeRelaunchLine
    // implements for the two windowed hosts.
    //
    // NOT a call into that one: this exe is Core-only BY CONSTRUCTION (see
    // main.cpp's header comment and premake5.lua's ArcaneServer block -- it
    // links ArcaneCore, never ArcaneClient), and dragging Client in for one
    // string function would break the very property ServerReport's
    // clientDllLoadedAtBoot probe exists to police. The vocabulary is this
    // host's own too (ServerConfig, not HostConfig): --frames N and --report
    // <json> are the scripted-run half, the three --remote flags are the
    // Servitor arming (a relaunch is a fresh session, and the descriptor path
    // is not the reporter's to repeat), and everything else -- --project,
    // --plugin, --fixed-dt -- describes the SESSION and is kept.
    //
    // `exePath` REPLACES argv[0], for the same reason the --print-engine-info
    // probe in main.cpp prints ExecutablePathUtf8() rather than argv[0]:
    // argv[0] is whatever the launcher typed -- a bare relative name under the
    // documented cd-then-run workflow, in ANSI-codepage bytes under MSVC --
    // which neither relaunches from the reporter's own working directory nor
    // survives a strict-UTF-8 envelope. Pure, so it is unit-tested
    // (Tests/ArcaneTests/src/ServerConfigTest.cpp).
    [[nodiscard]] std::string SanitizedRelaunchLine(const std::string& exePath, int argc, char** argv);
}
