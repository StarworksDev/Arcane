#pragma once

// ServerApp: the dedicated-server host application object (Core-DLL split,
// spec docs/specs/2026-09-15-core-dll-split-design.md s6, plan 1 Task 6).
// Owns the process's ONE ProcessContext, the ONE authoritative Runtime
// (NetMode::DedicatedServer) and the PluginHost that watches the project's
// game module. Run() opens the project, loads the module, boots the boot
// scene, then ticks a fixed-step loop -- forever or --frames N -- and always
// returns having tried to write the `--report` census, whichever step it
// died on.
//
// CORE-ONLY, BY CONSTRUCTION: this type includes nothing from ArcaneClient
// (no HostConfig, no GpuContext, no ClientRuntime) -- see main.cpp and
// premake5.lua's ArcaneServer block for the link-line half of that proof.

#include "ServerConfig.hpp"
#include "ServerReport.hpp"
#include "ServerAdminConsole.hpp"

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/RemoteCVarService.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#if !defined(ARC_BUILD_DIST)
#include <Arcane/Base/DiagnosticStore.hpp> // Servitor P0 --remote: problems.list's store (Task 22)
#include <Arcane/Remote/HostSession.hpp>   // Servitor P0: the --remote surface (a Runtime module, never Client)
#endif

#include <memory>
#include <optional>

namespace Arcane::Server
{
    class ServerApp
    {
    public:
        explicit ServerApp(ServerConfig cfg) : m_cfg(std::move(cfg)) {}

        // Opens the project, loads the module, ticks the fixed-step loop, and
        // writes the `--report` census on every exit path (when --report was
        // given). Returns the process exit code (0 on a clean "frames-complete"
        // finish, non-zero on any refusal).
        int Run();

        // ---- The clean-exit hook (crash window plan 1, task 9; spec S5.7) --
        // A dedicated server's ORDINARY stop is a Ctrl-C or a service
        // shutdown: without --frames its tick loop is open-ended, so this hook
        // is the only thing that can end it cleanly rather than by
        // termination. STATIC and installed from main() before this object
        // exists -- the window starts at Diagnostics::Install, and the hook
        // runs on the OS's console-handler thread, so it may touch nothing
        // thread-affine. It sets one atomic the tick loop reads.
        static void InstallCleanExitHook() noexcept;

        // Whether that hook has fired. Read once per tick.
        [[nodiscard]] static bool CleanExitRequested() noexcept;

    private:
        // Finish: stamp the census's exitReason, write it to m_cfg.reportPath
        // (if any), and return `exitCode` -- the ONE tail every Run() return
        // path funnels through, so a report is attempted on every outcome,
        // not only the happy path.
        int Finish(ServerReport& rep, std::string exitReason, int exitCode);

        ServerConfig m_cfg;

        // Declaration order is the teardown contract, same shape as every
        // other host's (RuntimeApp.hpp's own comment): m_process outlives
        // m_runtime, and m_runtime outlives m_plugin -- so m_plugin's dtor
        // (Unload while the module DLL is still mapped) runs first, then
        // ~Runtime, then ~ProcessContext.
        std::unique_ptr<Arcane::ProcessContext> m_process;
        std::optional<Arcane::Runtime>          m_runtime;
        std::optional<Arcane::PluginHost>       m_plugin;
        // The admin console (settings spec s9). Declared after m_plugin, so it is
        // torn down first; it touches the process registry only, never the module.
        std::optional<Arcane::RemoteCVarService> m_cvarService;
        std::optional<AdminConsole>              m_adminConsole;
        StdinLines                               m_stdin;
#if !defined(ARC_BUILD_DIST)
        // Servitor P0 (Task 11): armed by --remote after the admin console,
        // drained at the top of every tick before the sim advances, stopped
        // before Finish. Declared last, so it is torn down first; it touches
        // the process command registry and its own listener only.
        //
        // Task 22 fix round 1: the store problems.list reads. Under --remote,
        // Run() installs it as the engine's diagnostics sink first thing, so
        // project-open, module-load and boot-scene diagnostics are in it when
        // the session arms. Declared before m_remote, so it outlives the session.
        Arcane::DiagnosticStore                      m_problems;
        std::unique_ptr<Arcane::Remote::HostSession> m_remote;
#endif
    };
}
