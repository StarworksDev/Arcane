#include "ServerApp.hpp"

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/ExtractAccounting.hpp>  // ExtractFrameCallCount (census)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>       // NetMode / Arcane::ToString(NetMode)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectHost.hpp>        // VerifySharedTypeContext / GameModule / PluginModules / BootScene
#include <Arcane/Sim/SimHook.hpp>                // SimHooks()::HasWorld (the world is probed through the hook, no solver header)
#include <Arcane/Scene/PhysicsSystem2D.hpp>        // Arcane::PhysicsSystem2D (systems.hasPhysics)
#include <Arcane/Scene/TransformSystems.hpp>     // Arcane::TransformPropagationSystem (systems.hasPropagation)

#include <Astra/Core/TypeContext.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>

// Only for the two GetModuleHandleW census probes below (P10/S1). Windows-only
// until the Linux port: elsewhere the probe has no equivalent worth faking, so
// both report false and the report says nothing rather than something wrong.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Arcane::Server
{
    // ---- The clean-exit hook (crash window plan 1, task 9) ----------------
    // One process-lifetime atomic, not a member: main() installs the hook
    // before this object exists (see ServerApp.hpp), and the hook runs on the
    // OS's console-handler thread.
    namespace
    {
        std::atomic<bool> g_cleanExitRequested{ false };
    }

    void ServerApp::InstallCleanExitHook() noexcept
    {
        Arcane::Diagnostics::SetCleanExitHook(
            [](void*) { g_cleanExitRequested.store(true, std::memory_order_release); }, nullptr);
    }

    bool ServerApp::CleanExitRequested() noexcept
    {
        return g_cleanExitRequested.load(std::memory_order_acquire);
    }

    int ServerApp::Finish(ServerReport& rep, std::string exitReason, int exitCode)
    {
        rep.exitReason = std::move(exitReason);
        if (!m_cfg.reportPath.empty() && !rep.WriteTo(m_cfg.reportPath))
        {
            ARC_ERROR("ArcaneServer: failed to write --report to '{}'", m_cfg.reportPath);
            // A missing report is a failed run: never exit 0 over it.
            if (exitCode == 0)
                return 1;
        }
        return exitCode;
    }

    int ServerApp::Run()
    {
        ServerReport rep;   // every field defaulted honestly (opened=false, loaded=false, ...)
#if !defined(ARC_BUILD_DIST)
        // Task 22 fix round 1: before the project opens, so the remote session's
        // problems.list sees every boot diagnostic (Diagnostics does not replay).
        // Uninstalled once m_remote has stopped; ~DiagnosticStore covers an early Finish.
        if (m_cfg.remote)
            m_problems.InstallAsEngineSink();
#endif
        // The exe's OWN imports, sampled BEFORE any module loads -- P10/S1: this
        // exe's link line never names ArcaneClient, so this must read false on a
        // clean build. If it ever reads true, a Core symbol's export/import got
        // routed through Client somewhere and that is a defect to fix, not a
        // value to relax. Windows-only probe (see the include guard above): it
        // reads false on every other platform, where no host ships yet.
#ifdef _WIN32
        rep.clientDllLoadedAtBoot = ::GetModuleHandleW(L"ArcaneClient.dll") != nullptr;
#endif

        Arcane::ProcessContextDesc d;
        d.isDedicatedServerProcess = true;
        m_process = Arcane::ProcessContext::Create(d);
        if (!m_process)
        {
            ARC_ERROR("ArcaneServer: ProcessContext refused -- a second host in this process?");
            return Finish(rep, "process-context-refused", 1);
        }
        // This exe's OWN per-module Astra slot (ArcaneCore.dll's own slot is
        // installed by ProcessContext::Create itself) -- see ProjectHost.hpp's
        // VerifySharedTypeContext for why every module that touches a
        // component type must do this for itself.
        Astra::SetTypeContext(&m_process->TypeContext());

        // The ONE authoritative world: NetMode::DedicatedServer instantiates only
        // the module's Server-masked system factories (Arcane/Plugin/SystemFactory.hpp).
        m_runtime.emplace(*m_process, Arcane::NetMode::DedicatedServer);
        rep.netMode                  = Arcane::ToString(m_runtime->Mode());
        rep.isDedicatedServerProcess = m_process->IsDedicatedServerProcess();

        if (!m_runtime->OpenProject(m_cfg.projectPath))
            return Finish(rep, "project-open-failed", 1);

        // A dedicated host refuses cheats unless the project or the operator opted
        // in (settings spec s3.2). After OpenProject, so the Project rung is known.
        if (ApplyDedicatedServerDefaults(Arcane::CVarRegistry::Get()))
            Arcane::CVarRegistry::Get().Publish();

        // Review round 1: make --fixed-dt REAL. RunLoop's internal accumulator
        // ticks at RunLoop::Config::fixedHz (default 60), independent of the
        // realDt passed to Advance() below -- so without this call, --fixed-dt
        // would only repace the host loop and never the physics step size.
        // SetFixedHz makes the REQUESTED step the loop's ACTUAL one; the tick
        // loop below still advances by cfg.fixedDtSeconds of wall time per host
        // frame, i.e. one fixed step per frame, as before. rep.fixedDt is
        // derived back FROM THE LOOP, not echoed from m_cfg, so a future
        // refusal/clamp inside SetFixedHz (RunLoop.hpp) is reported honestly
        // rather than optimistically.
        // Settings arc S6-8: with no --fixed-dt the rate is server.tickHz
        // (Restart), read here, AFTER OpenProject applied the project's
        // Config rung, and the wall-clock pacing follows it.
        // Runtime::SetFixedHz, not Loop().SetFixedHz: the physics step
        // follows the tick too, so a server.tickHz != sim.fixedHz no longer
        // runs the simulation at the wrong speed (S6-8 deferral, S6-GATE).
        const double tickHz = m_cfg.FixedHz();
        m_runtime->SetFixedHz(tickHz);
        if (!m_cfg.fixedDtSupplied)
            m_cfg.fixedDtSeconds = 1.0 / tickHz;
        rep.fixedDt = 1.0 / m_runtime->Loop().FixedHz();

        const Arcane::Project* proj = m_runtime->CurrentProject();
        rep.projectOpened = true;
        rep.projectName   = proj->Manifest().name;
        rep.projectAbi    = proj->Manifest().engineAbi;

        if (!Arcane::ProjectHost::VerifySharedTypeContext(m_runtime->Registry(), "ArcaneServer.exe"))
            return Finish(rep, "type-context-mismatch", 1);

        const std::string module = Arcane::ProjectHost::GameModule(proj, m_cfg.pluginPath);
        m_plugin.emplace(*m_process, module.empty() ? std::filesystem::path{} : std::filesystem::path(module));
        m_plugin->AttachRuntime(*m_runtime);
        for (const auto& dll : Arcane::ProjectHost::PluginModules(proj))
            m_plugin->AddPlugin(dll);
        rep.modulePath = module;

        if (!m_plugin->Load())
            return Finish(rep, "module-load-failed", 1);

        rep.moduleLoaded     = true;
        rep.moduleGeneration = m_plugin->Generation();
        // P10: the game module's OWN import of ArcaneClient (the module links
        // both import libs so it can build against the full engine surface),
        // reported honestly rather than hidden -- this exe's own link line
        // still never names ArcaneClient. Windows-only, like its sibling above.
#ifdef _WIN32
        rep.clientDllLoadedAfterModule = ::GetModuleHandleW(L"ArcaneClient.dll") != nullptr;
#endif

        (void)Arcane::ProjectHost::BootScene(*m_runtime, *proj);

        // The operator's console (settings spec s9): stdin lines are RemoteCVarService
        // requests in the ServerAdmin context, answered on stdout between ticks;
        // every set is logged as a cvar-audit line.
        if (m_cfg.adminConsole)
        {
            m_cvarService.emplace(Arcane::CVarRegistry::Get(), &LogAuditRecord, nullptr);
            m_adminConsole.emplace(*m_cvarService, "stdin");
            m_stdin.Start();
            ARC_INFO("ArcaneServer: admin console on stdin (get/set/list/explain; 'help')");
        }

#if !defined(ARC_BUILD_DIST)
        // Servitor P0 (Task 11): --remote arms the loopback surface, after the
        // admin console and outside it (RemoteCVarService above exists only when
        // the console is on). A listener that cannot start ends the run
        // (review round 1, Task 17): an explicit --remote that cannot come up
        // is a startup failure, exit 2 through Finish, like the refusal below.
        // The idle exit applies only
        // to an open-ended run (spec s4.7): a --frames N run ends by itself.
        if (m_cfg.remote && !Arcane::Remote::Arming::Allow(m_runtime->Mode(), false, true))
        {
            // Rule 3 (final fix pass, C-M16): an explicit --remote that cannot
            // apply is a startup refusal, exit 2 (the server's usage exit),
            // never run on as if ignored.
            const char* why = Arcane::Remote::Arming::RefusalReason(m_runtime->Mode());
            ARC_ERROR("ArcaneServer: --remote refused: {}; exiting 2", why ? why : "the remote surface cannot arm here");
            return Finish(rep, "remote-refused", 2);
        }
        else if (m_cfg.remote)
        {
            Arcane::Remote::HostSession::Config rc;
            rc.kind               = Arcane::Remote::HostKind::Server;
            rc.cvarContext        = Arcane::CVarContext::ServerAdmin;
            rc.projectRoot        = proj->Root();
            rc.descriptorOverride = m_cfg.remoteDescriptor;
            rc.client             = m_cfg.remoteClient;
            rc.ownerPid           = Arcane::Remote::Arming::OwnerPid(m_cfg.remote, m_cfg.ownerPid);
            rc.idleExitSeconds    = Arcane::Remote::Arming::IdleExitSeconds(m_cfg.remote, m_cfg.frames, m_cfg.remoteIdleExitSeconds);
            rc.explicitRemote     = m_cfg.remote;   // I1: the server arms only under --remote
            rc.holdSim            = m_cfg.remote && m_cfg.frames == 0;   // AM1: an open-ended run waits for play
            rc.capabilities       = { "console", "cvar", "ecs", "logs", "play", "problems", "stats" };
            rc.problemStore       = &m_problems;   // Task 22: installed as the engine sink at the top of Run()
            // Task 19 (ruling F16): the server's one world.
            rc.worlds.user        = this;
            rc.worlds.fn          = [](void* user) -> std::vector<Arcane::Remote::WorldInfo> {
                auto* app = static_cast<ServerApp*>(user);
                if (!app->m_runtime)
                    return {};
                return { { "main", Arcane::ToString(app->m_runtime->Mode()), &app->m_runtime->Registry() } };
            };
            m_remote = std::make_unique<Arcane::Remote::HostSession>(std::move(rc));
            // Start logs why and that the host "runs without remote"; an
            // explicit --remote that cannot start is a startup failure, so the
            // server says so before the exit (final fix pass, b6 minor).
            if (!m_remote->Start())
            {
                ARC_ERROR("ArcaneServer: --remote did not start (see above); exiting 2");
                return Finish(rep, "remote-failed", 2);
            }
        }
#endif

        // Fixed-step tick, forever or --frames N. Wall-clock paced by sleeping the
        // remainder of each step (spec s6: tick rate is a RUNTIME value); the loop
        // is unpaused (fresh).
        // Whether the loop ended because the OS asked for this session to end
        // rather than because the frame budget ran out -- the census must not
        // report "frames-complete" for a run that was stopped a tenth of the
        // way through.
        bool stoppedByCleanExit = false;
        // --frames N counts SIM TICKS, not loop iterations (AM1, Servitor P0
        // Task 29): rep.framesTicked moves only in the advance branch below, so
        // an iteration a remote pause or stop holds idle never spends the
        // budget. A paused `--frames N --remote` server idles until it is
        // resumed or stepped, and still ticks exactly N. Without remote (and in
        // Dist) every iteration advances, so the count is the same as before.
        while (m_cfg.frames == 0 || rep.framesTicked < m_cfg.frames)
        {
            // Ctrl-C, the console close box, logoff or service shutdown,
            // routed through Diagnostics::RequestCleanExit into the hook
            // main() installed (crash window plan 1, task 9; spec S5.7).
            // Checked at the TOP of the tick so the exit begins on the very
            // next tick boundary -- a console handler has about five seconds,
            // and everything below (the census write, ~PluginHost's module
            // unload, ~Runtime) has to fit inside it.
            if (CleanExitRequested())
            {
                ARC_INFO("ArcaneServer: stopping on a clean-exit request after {} tick(s)", rep.framesTicked);
                stoppedByCleanExit = true;
                break;
            }

            const auto start = std::chrono::steady_clock::now();
#if !defined(ARC_BUILD_DIST)
            // AM1: remote commands run BEFORE the tick advances, never beside the
            // admin stdin block below. DrainFrame notes a module reload the
            // previous tick's Poll performed, so this drain's status carries it;
            // a failed one is noted from the attempt count (Task 30).
            // Final fix pass (C-I3): the module host's reload begin/end is the
            // "reloading" state.
            if (m_remote)
                m_plugin->SetReloadObserver(
                    [](bool reloading, void* user) {
                        auto* app = static_cast<ServerApp*>(user);
                        if (app->m_remote)
                            app->m_remote->NoteReloading(reloading);
                    },
                    this);
            if (m_remote && m_remote->DrainFrame(m_plugin->Generation(), m_plugin->ReloadAttempts(),
                                                 m_plugin->LastReloadOk()))
            {
                ARC_INFO("ArcaneServer: stopping on {} after {} tick(s)", m_remote->ExitReason(), rep.framesTicked);
                break;
            }
            // AM1 tick gate: a held --remote server (open-ended) runs no tick until
            // a play command; an Idle iteration still polls, serves the console,
            // publishes, beats and sleeps below. No remote: Run, as before.
            const auto decision = m_remote ? m_remote->Tick().Consume() : Arcane::Remote::TickDecision::Run;
            const bool advance  = decision != Arcane::Remote::TickDecision::Idle;
#else
            constexpr bool advance = true;
#endif
            if (advance)
            {
#if !defined(ARC_BUILD_DIST)
                const auto advanceStart = std::chrono::steady_clock::now();
#endif
                m_runtime->EnsurePhysics();
                m_runtime->Loop().Advance(m_cfg.fixedDtSeconds,
                                          [&](double dt) { m_plugin->FixedUpdateAll(dt); },
                                          [&](double dt, double a) { m_plugin->UpdateAll(dt, a); });
#if !defined(ARC_BUILD_DIST)
                if (m_remote)
                {
                    m_remote->Gate().frame.fetch_add(1);   // host.frame counts sim ticks only (AM1)
                    // stats.frame (Task 26): renderMs stays 0, a server has no render pass.
                    m_remote->NoteSimAdvance(m_remote->Gate().frame.load(), m_cfg.fixedDtSeconds,
                                             std::chrono::duration<double, std::milli>(
                                                 std::chrono::steady_clock::now() - advanceStart).count());
                }
#endif
                ++rep.framesTicked;
            }
            m_plugin->Poll();
            if (m_adminConsole)
            {
                for (const std::string& line : m_stdin.Drain())
                {
                    const std::string reply = m_adminConsole->Submit(line);
                    if (reply.empty()) continue;
                    std::fputs(reply.c_str(), stdout);
                    std::fputc('\n', stdout);
                    std::fflush(stdout);
                }
            }
            // One publish per tick, as the client hosts do per frame: a Live Server
            // setting set by the module or a console is in effect from the next tick.
            Arcane::CVarRegistry::Get().Publish();
            Arcane::Diagnostics::Heartbeat();
            std::this_thread::sleep_until(start + std::chrono::duration<double>(m_cfg.fixedDtSeconds));
        }

        // THE QUIT SITE (crash window plan 1, task 9; spec S5.7): the loop is
        // over and everything below -- the census, then this object's members
        // unwinding (the module unload, ~Runtime, ~ProcessContext) -- is
        // teardown, the one failure the watchdog cannot otherwise see, because
        // the tick loop that was beating is gone. RequestCleanExit swaps the
        // beat rule for a single deadline (Config::exitSeconds, 30 s), so a
        // module unload that never returns is reported as "hang at exit" and
        // terminated with 12. Idempotent -- a Ctrl-C that already armed it
        // does not restamp the clock.
        Arcane::Diagnostics::RequestCleanExit();
#if !defined(ARC_BUILD_DIST)
        if (m_remote)
            m_remote->MarkStopping();   // /status and the descriptor read "shutting-down" through the census
#endif

        rep.fixedUpdate = m_runtime->Schedulers().fixedUpdate.Size();
        rep.update      = m_runtime->Schedulers().update.Size();
        rep.render      = m_runtime->Schedulers().render.Size();
        rep.hasPhysics     = m_runtime->Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>();
        // The world EnsurePhysics (the tick above) minted: a null resource means no 2D
        // hook ran, which hasPhysics alone (a scheduler entry) cannot tell apart.
        rep.hasPhysicsWorld = false;
        for (const auto& hook : Arcane::SimHooks())
            if (hook.HasWorld && hook.HasWorld(m_runtime->Registry())) { rep.hasPhysicsWorld = true; break; }
        rep.hasPropagation = m_runtime->Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>();
        rep.hasRenderSubmission = false;   // by construction: no ClientRuntime exists in this process
        rep.extractFrameCalls   = Arcane::ExtractFrameCallCount();
        rep.clientAttached      = m_runtime->Client() != nullptr;

        // "clean-exit" is a SUCCESSFUL stop (exit code 0) that simply is not
        // "frames-complete": the run was asked to end and did, which a census
        // reader must be able to tell from a budget that ran out.
        std::string exitReason = stoppedByCleanExit ? "clean-exit" : "frames-complete";
#if !defined(ARC_BUILD_DIST)
        // Servitor P0: a remote quit or an idle timeout is a clean stop too (exit 0).
        // The reason was latched where the loop broke (DrainFrame), never re-derived here.
        if (m_remote && m_remote->ExitReason() != nullptr)
            exitReason = m_remote->ExitReason();
        if (m_remote)
        {
            m_remote->Stop(exitReason);   // the descriptor is deleted before the census is written
            m_remote.reset();
        }
        m_problems.UninstallEngineSink();   // after the session that read it (identity-checked)
#endif
        return Finish(rep, std::move(exitReason), 0);
    }
}
