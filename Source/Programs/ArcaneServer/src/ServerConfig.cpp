#include "ServerConfig.hpp"

#include <Arcane/Cli/Cli.hpp>
#include <Arcane/Sim/SimSettings.hpp>   // server.tickHz

#include <charconv>      // std::from_chars -- --remote-idle-exit's whole-seconds parse
#include <cmath>
#include <cstdio>
#include <string_view>

namespace Arcane::Server
{
    namespace
    {
#if defined(ARC_BUILD_DIST)
        // Dist: the remote surface is compiled out, so the three flags are not
        // registered. Scanned BEFORE Cli::Parse, which would otherwise refuse
        // them as a generic "unknown argument" with a usage dump. Both Cli
        // spellings. HostConfig.cpp's NamesRemoteFlag is the Client twin; this
        // exe is Core-only and does not link it.
        [[nodiscard]] bool NamesRemoteFlag(int argc, char** argv)
        {
            for (int i = 1; i < argc; ++i)
            {
                std::string_view arg(argv[i]);
                if (!arg.starts_with("--"))
                    continue;
                arg.remove_prefix(2);
                const std::string_view name = arg.substr(0, arg.find('='));
                if (name == "remote" || name == "remote-descriptor" || name == "remote-idle-exit" || name == "client"
                    || name == "owner-pid")
                    return true;
            }
            return false;
        }
#else
        // HostConfig.cpp's ParseIdleExitSeconds, the same rule: a whole number
        // that fits the field, over the WHOLE token.
        [[nodiscard]] bool ParseIdleExitSeconds(const std::string& text, std::uint32_t& out)
        {
            const char* end = text.data() + text.size();
            const auto r = std::from_chars(text.data(), end, out);
            return !text.empty() && r.ec == std::errc{} && r.ptr == end;
        }
#endif
    }

    ServerConfig::ParseOutcome ServerConfig::Parse(int argc, char** argv)
    {
#if defined(ARC_BUILD_DIST)
        if (NamesRemoteFlag(argc, argv))
        {
            std::fprintf(stderr, "Servitor is compiled out of Dist builds\n");
            return { std::nullopt, 2 };
        }
#endif
        Cli cli{ "ArcaneServer", "Core-only dedicated-server host" };
        cli.Option("project", "", "project folder or .arcproj to open (REQUIRED unless --print-engine-info)");
        cli.Option("plugin",  "", "game DLL to host (empty = the project's gameModule; a server with nothing to host refuses boot)");
        cli.Option("frames",  "0", "tick N fixed steps then exit (0 = run until terminated)").Type(CliType::Uint);
        cli.Option("fixed-dt", "", "seconds per fixed tick (empty = 1/server.tickHz)").Type(CliType::Double);
        // The --report caveat is stated in the help text, not only in a doc: the
        // report is written when the tick loop ENDS (ServerApp::Finish), so with
        // --frames 0 a server that is killed writes none. A console-control
        // handler that finishes on Ctrl+C/terminate is the follow-up.
        cli.Option("report",  "", "write the census to this JSON path (written when the "
                                  "loop ends -- with --frames 0 a killed server writes none)");
        cli.Flag  ("print-engine-info", "print engine identity JSON to stdout and exit");
        cli.Flag  ("no-admin-console", "do not read admin commands (get/set/list/explain) from stdin");
#if !defined(ARC_BUILD_DIST)
        // Servitor P0 (Task 10): HostConfig's three remote flags, same help.
        cli.Flag  ("remote", std::string(kRemoteFlagHelp));   // states the AM1 hold (Task 29)
        cli.Option("remote-descriptor", "", "DEV: write the remote attach descriptor to this path "
                                            "(empty = under <project>/Saved/Remote/)");
        cli.Option("remote-idle-exit", std::to_string(ServerConfig{}.remoteIdleExitSeconds),
                   "DEV: with --remote, exit after this many seconds with no remote request (0 = never)");
        cli.Option("client", "", "DEV: with --remote, the caller name recorded in the attach descriptor "
                                 "(servitor launch --client NAME; empty = unnamed)");
        cli.Option("owner-pid", "0", "DEV: with --remote, exit when this process id is no longer running "
                                     "(0 = watch none; opt-in, never defaulted)");
#endif

        const Cli::Result r = cli.Parse(argc, argv);
        if (!r.ok) return { std::nullopt, r.exitCode };

        ServerConfig cfg;
        cfg.projectPath     = r.Get("project");
        cfg.pluginPath      = r.Get("plugin");
        cfg.frames          = r.GetAs<std::uint64_t>("frames");
        cfg.fixedDtSupplied = r.Supplied("fixed-dt");
        cfg.fixedDtSeconds  = cfg.fixedDtSupplied ? r.GetAs<double>("fixed-dt") : 0.0;
        cfg.reportPath      = r.Get("report");
        cfg.printEngineInfo = r.Flag("print-engine-info");
        cfg.adminConsole    = !r.Flag("no-admin-console");
#if !defined(ARC_BUILD_DIST)
        cfg.remote           = r.Flag("remote");
        cfg.remoteDescriptor = r.Get("remote-descriptor");
        // The same refusals, and the same sentences, as HostConfig::Parse.
        if (r.Supplied("remote-descriptor")
            && (cfg.remoteDescriptor.empty() || cfg.remoteDescriptor.starts_with("--")))
        {
            std::fprintf(stderr, "error: --remote-descriptor wants a path (an empty value or another "
                                 "flag is not a descriptor path)\n");
            return { std::nullopt, 2 };
        }
        cfg.remoteClient     = r.Get("client");
        // M2 (B7 fix round 1): Cli takes the NEXT token as the value even when
        // it is another flag (--client --headless would swallow --headless).
        if (r.Supplied("client") && (cfg.remoteClient.empty() || cfg.remoteClient.starts_with("--")))
        {
            std::fprintf(stderr, "error: --client wants a caller name (an empty value or another "
                                 "flag is not a name)\n");
            return { std::nullopt, 2 };
        }
        if (r.Supplied("owner-pid") && !ParseIdleExitSeconds(r.Get("owner-pid"), cfg.ownerPid))
        {
            std::fprintf(stderr, "error: --owner-pid wants a process id (a whole number, 0 = none), "
                                 "got '%s'\n", r.Get("owner-pid").c_str());
            return { std::nullopt, 2 };
        }
        cfg.remoteIdleExitSupplied = r.Supplied("remote-idle-exit");
        if (cfg.remoteIdleExitSupplied
            && !ParseIdleExitSeconds(r.Get("remote-idle-exit"), cfg.remoteIdleExitSeconds))
        {
            std::fprintf(stderr, "error: --remote-idle-exit wants a whole number of seconds "
                                 "(0 = never), got '%s'\n", r.Get("remote-idle-exit").c_str());
            return { std::nullopt, 2 };
        }
#endif

        // Same NaN reasoning HostConfig::Parse's --fixed-dt refusal documents:
        // `<= 0.0` alone does not reject NaN (every comparison against NaN is
        // false), so isfinite is checked explicitly. Refused, not clamped --
        // rule 3 (no silently inert/wrong flags): a caller who typed a bad
        // step size typed something they did not mean.
        if (cfg.fixedDtSupplied && (!std::isfinite(cfg.fixedDtSeconds) || cfg.fixedDtSeconds <= 0.0))
        {
            std::fprintf(stderr, "error: --fixed-dt wants a positive, finite number of seconds\n");
            return { std::nullopt, 2 };
        }

        // A server with nothing to host refuses boot, like ArcaneRuntime --
        // unless the caller only wants the engine-identity probe, which needs
        // no project at all.
        if (cfg.projectPath.empty() && !cfg.printEngineInfo)
        {
            std::fprintf(stderr, "error: --project <dir> is required (a server with nothing to host "
                                 "refuses boot; pass --print-engine-info if you only want the engine "
                                 "identity probe)\n");
            return { std::nullopt, 2 };
        }

        return { cfg, 0 };
    }

    std::string SanitizedRelaunchLine(const std::string& exePath, int argc, char** argv)
    {
        // --remote takes no value; the loop below consumes the next token only
        // when it does not start with "--", so `--remote --project P` keeps
        // --project. A bare --remote followed by a positional would lose that
        // positional, but this CLI has no positionals (Cli refuses them).
        static constexpr std::string_view kStripWithValue[] = {
            "frames", "report", "remote", "remote-descriptor", "remote-idle-exit", "client", "owner-pid",
        };
        std::string out = (exePath.find(' ') != std::string::npos || exePath.empty())
                        ? "\"" + exePath + "\""
                        : exePath;
        for (int i = 1; i < argc; ++i)   // 1: argv[0] is `exePath` above
        {
            std::string_view arg(argv[i]);
            if (arg.starts_with("--"))
            {
                std::string_view body = arg.substr(2);
                const std::size_t eq = body.find('=');           // Cli accepts --name=value too
                const bool inlineValue = eq != std::string_view::npos;
                const std::string_view name = inlineValue ? body.substr(0, eq) : body;
                bool strip = false;
                for (std::string_view s : kStripWithValue) strip = strip || (s == name);
                if (strip)
                {
                    if (!inlineValue && i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--"))
                        ++i;   // its value
                    continue;
                }
            }
            if (!out.empty()) out.push_back(' ');
            if (arg.find(' ') != std::string_view::npos || arg.empty())
                out += "\"" + std::string(arg) + "\"";
            else
                out += std::string(arg);
        }
        return out;
    }

    double ServerConfig::FixedHz() const
    {
        return fixedDtSupplied ? 1.0 / fixedDtSeconds : Settings<ServerSettings>().tickHz;
    }
}
