#pragma once
// HostSession: one armed host's remote surface (spec s4.3, s4.4, s4.7). It owns
// the request queue, the loopback listener, the dispatcher, the job table, the
// host gate and the descriptor; the host program owns one session and calls
// Drain once per main-loop iteration, after its event pump and before its sim
// step. Everything here runs on the host main thread except the listener's own
// service thread, which reads only the atoms Drain publishes.
//
// Kept free of socket headers on purpose: the listener, its atoms, the queue
// and the dispatcher are held by pointer, so a host header that includes this
// one (EditorApp.hpp) never pulls in winsock.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Core/NetMode.hpp>
#include <Arcane/Remote/Commands/CaptureCommands.hpp>
#include <Arcane/Remote/Commands/CompareCommands.hpp>
#include <Arcane/Remote/Commands/CvarCommands.hpp>
#include <Arcane/Remote/Commands/PlayCommands.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/FrameStats.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/TickGate.hpp>
#include <Arcane/Remote/Worlds.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Arcane
{
class DiagnosticStore;  // Arcane/Base/DiagnosticStore.hpp
}

namespace Arcane::Remote
{

class RequestQueue;     // Listener.hpp
class RemoteListener;   // Listener.hpp
struct ListenerAtoms;   // Listener.hpp

// Which processes may serve the remote surface (spec 0.4, s4.7): never a Dist
// build, never a process whose primary world is a Client, and never when the
// third input is false. Standalone, DedicatedServer and ListenServer arm when
// the other two inputs allow it. The editor passes "editor.remote.enable is
// published true, or --remote was given" (spec 0.4(b): the editor is armed by
// its cvar, default on; EditorApp::UpdateRemoteSession); runtime and server
// call Allow only under --remote and pass true.
struct ARC_REMOTE_API Arming {
    static bool Allow(NetMode mode, bool distBuild, bool editorEnableCvar);
    // Final fix pass (C-M16, Rule 3): why an explicit --remote cannot apply to
    // a process in `mode` -- a Client primary world never serves the remote
    // surface (spec s10.2) -- or null when it can. A host given --remote
    // refuses it at startup with this reason and exits 2 (its usage refusal),
    // never runs on with the flag ignored.
    static const char* RefusalReason(NetMode mode);
    // Final fix round 2 (M-2): whether that refusal ends the host. Only an
    // explicit --remote, and only on the startup arm: a later re-arm that
    // cannot apply (an editor.remote.enable toggle during ClientOnly Play, a
    // project switch) logs and stays disarmed, never exits.
    static bool RefusalExits(bool explicitRemote, bool startupArm);
    // Final fix round 3 (M-2): one arm attempt, past the caller's own
    // early-return guard. The first attempt (attemptedBefore false) is the
    // startup arm, whether it arms or is refused, and every call marks the
    // attempt. Arm: `mode` may serve. Otherwise RefuseExit (an explicit
    // --remote at startup) or RefuseLog (anything later, or the cvar alone).
    enum class Verdict : std::uint8_t { Arm, RefuseExit, RefuseLog };
    static Verdict Attempt(bool& attemptedBefore, NetMode mode, bool explicitRemote);
    // B7 fix round 1 (C2): the idle exit and the --owner-pid watch belong to
    // a host a client started for itself: an explicit --remote (servitor
    // launch always passes it) on an open-ended run (frameBound 0, no
    // --frames N). Anything else gets 0 for both. That includes the editor a
    // user opens by hand, armed by editor.remote.enable alone (default on): it
    // never ends itself and watches no process.
    static std::uint32_t IdleExitSeconds(bool explicitRemote, std::uint64_t frameBound, std::uint32_t configured);
    static std::uint32_t OwnerPid(bool explicitRemote, std::uint32_t configured);
};

class ARC_REMOTE_API HostSession {
public:
    struct Config {
        HostKind kind;
        CVarContext cvarContext;
        std::filesystem::path projectRoot;
        std::filesystem::path descriptorOverride;
        // 0 = never. Every host sets it through Arming::IdleExitSeconds; a
        // Config site that skips Arming never idles out (final fix pass, b7 m5).
        std::uint32_t idleExitSeconds = 0;
        bool readOnly = false;
        bool holdSim = false;          // Task 12: --remote && maxFrames == 0; Start leaves Tick() Idle
        std::vector<std::string> capabilities;
        // Appended by later tasks, kept last so the fields above keep their
        // positions. Each task that appends a field also wires, in Start, the
        // registration that reads it (ruling F12): Task 19 adds
        // `WorldTable worlds`; Task 22 adds `DiagnosticStore* problemStore`;
        // Tasks 27-29 add their host hook structs. Session-owned state (the
        // cvar command user, FrameStats) is a HostSession member added by the
        // task that needs it.
        //
        // Task 16 (host.quit / host.wait, Commands/SessionCommands.hpp), main
        // thread, each called with hookUser. Plain function pointers, so a
        // Client-module predicate reaches ArcaneRemote without a dependency
        // on it (R8-2). Null = the host has none.
        bool (*dirty)(void*) = nullptr;    // unsaved changes: host.quit answers host.busy{state:"modal"}
        // wait settled (spec s4.7, Task 30): the --settle producer conjunct
        // (ShaderCompiler::IsIdle, and on the editor the cook queue and the
        // thumbnail harvester; SettleProducersIdle). The byte-equal pair is
        // the host.wait job's own: two captures through `capture`'s host pair.
        bool (*producersIdle)(void*) = nullptr;
        void* hookUser = nullptr;
        // Task 19 (ruling F16): the host's worlds, read by world.list and every
        // world-taking command through CommandWorlds(). Start installs it with
        // SetCommandWorlds; Stop installs an empty table. fn is called on the
        // main thread only (Start and inside Drain). P0 hosts list one world,
        // {"main", ToString(mode), &registry}, with user = the host app.
        WorldTable worlds;
        // Task 22 (ruling F12): the store problems.list and problem.follow
        // read. The editor passes its Problems-panel store (already the
        // engine's diagnostics sink). Null (runtime, server): Start creates a
        // session-owned store and installs it as the engine sink; Stop
        // uninstalls it. Main thread only.
        DiagnosticStore* problemStore = nullptr;
        // Task 27 (ruling F12): capture.viewport / capture.window's hooks,
        // forwarding to the host's NriGraphContext capture ring (R8-2: this
        // module includes no render header). The editor installs six pointers
        // (host pair = viewport pair, ruling F20), the runtime its one target
        // three times; a server leaves all six null (capture.no-renderer).
        // An empty outDir is filled by Start with the session's artifact
        // directory (<remote dir>/out). Start installs them with
        // SetCaptureHooks + RegisterCaptureCommands(&m_jobs); Teardown clears both.
        CaptureHooks capture;
        // Task 28 (ruling F12): capture.compare's reference hooks. The editor
        // and the runtime install ResolveFn / BlessFn over ReferenceImages
        // (ResolveReference with the host's adapter set, BlessReference) with
        // their project root; a server leaves them null (it has no renderer,
        // so capture.no-renderer answers first). Start installs them with
        // SetCompareHooks + RegisterCompareCommand(&m_jobs), the same table
        // the capture commands use; Teardown clears both.
        CompareHooks compare;
        // Task 29 (ruling F12): play.start/stop/pause/resume/step. The editor
        // installs its PlaySession / RunLoop functions (editor = true,
        // undoReason "play-mode"); a runtime or server leaves them null and
        // Start points `gate` at Tick() (undoReason "runtime-host"). Start
        // also sets `stats` to the session's FrameStats and installs them with
        // SetPlayHooks + SetPlayGate(&Gate()) + RegisterPlayCommands(&m_jobs);
        // Teardown clears all three.
        PlayHooks play;
        // Final fix pass (C-I3): the editor's host-state facts, main thread,
        // called with hookUser inside Drain. modal: a ModalErrorQueue dialog
        // or another modal popup is open; building: the editor's module
        // rebuild (arcbuild) is running. Null: never. A runtime and a server
        // have neither; their play axis is their tick gate (Tick()).
        bool (*modal)(void*) = nullptr;
        bool (*building)(void*) = nullptr;
        // --client <name> (servitor launch --client NAME): the caller name Start
        // writes into the descriptor and every heartbeat rewrites. Empty = unnamed.
        std::string client;
        // --owner-pid <pid>: opt-in. Non-zero and no longer a live process
        // (Platform::IsProcessLive(pid, 0)) -> DrainFrame takes the same exit
        // as an expired idle timer ("remote-idle"). 0 watches nothing. Start
        // writes it into the descriptor and every heartbeat rewrites it.
        std::uint32_t ownerPid = 0;
        // Final fix pass (I1): the host was armed by an explicit --remote
        // (servitor launch always passes it), not by editor.remote.enable
        // alone. Start writes it into the descriptor (explicitRemote) and
        // every heartbeat keeps it; quit --client's untagged --host fallback
        // takes only a host that has it, so never the user's own editor.
        bool explicitRemote = false;
    };
    explicit HostSession(Config config);
    ~HostSession(); // Stop("destroyed") when still started
    HostSession(const HostSession&) = delete;
    HostSession& operator=(const HostSession&) = delete;

    // Sets the registry's host kind (remembering the previous one), starts the
    // listener on 127.0.0.1, publishes status + catalogue and writes the
    // descriptor; true when the session is live. A listener that could not
    // bind (Port() == 0 afterwards) or a descriptor that could not be written
    // is a failed start: logged, nothing left running, the host kind
    // restored, false returned, and the host carries on without remote (or,
    // for an open-ended headless run, exits 2: it has no other stop path). A
    // session object starts at most once: a second Start (while live, after
    // Stop, or after a failed Start) is refused with false and a log line;
    // a host that wants a new session makes a new object.
    bool Start();
    // One main-loop iteration's work: the host state refreshed (below),
    // dispatcher.Drain within the frame budget, the state refreshed again (a
    // play command may have moved it), then HostStatus and the catalogue
    // republished into the listener atoms, and the descriptor rewritten with a
    // fresh heartbeat once a second. Never sleeps.
    //
    // The host state (spec s4.3; final fix pass, C-I3), highest first:
    // shutting-down (MarkStopping/Stop latch it), reloading (NoteReloading),
    // building and modal (Config hooks), paused, playing, minimized
    // (NoteMinimized), ready. The play axis is the editor's PlaySession and
    // RunLoop (Config::play.editorPlaying / editorPaused) or, on a runtime or
    // server, the tick gate: held is ready, running is playing, a paused run is
    // paused. Gate().inPlay carries the play axis whatever the state shows.
    void Drain();
    // The host's whole per-iteration remote step, one spelling for every host:
    // notes a module reload when `pluginGeneration` (the host's own
    // m_plugin->Generation(), ruling F10) differs from the last one seen,
    // Drains, then checks for a remote quit and the idle timeout. The first
    // generation it sees is adopted silently (the module the host booted with). True means
    // the host should leave its loop now; the reason is latched (ExitReason).
    // This form notes no failed reload: it is the three-argument form with
    // the attempt count last seen and lastReloadOk true.
    bool DrainFrame(std::uint32_t pluginGeneration);
    // Every host's form (Task 30): also the host's PluginHost::ReloadAttempts()
    // and LastReloadOk(). A reload that failed or was refused leaves the
    // generation alone, so an attempt count that moved with lastReloadOk false
    // is noted as a failed reload (NoteReload ok=false), which answers a
    // pending `wait reload` with module.reload-failed. The first call adopts
    // the count silently, as it adopts the generation.
    bool DrainFrame(std::uint32_t pluginGeneration, std::uint64_t reloadAttempts, bool lastReloadOk);
    // The reason DrainFrame latched when it told the host to stop:
    // "remote-quit" or "remote-idle". Null when the loop did not end on the
    // remote surface. Latched at the break site, so a later Ctrl-C or a long
    // teardown is never relabelled.
    const char* ExitReason() const;
    // The host is leaving its loop: state "shutting-down" (spec s4.3) is published to /status
    // and to the descriptor at once, so a client polling a shutting-down host
    // does not read "ready". Stop follows later.
    void MarkStopping();
    // Stops the listener (parked calls answer host.died), deletes the
    // descriptor and restores the registry's host kind. Idempotent.
    void Stop(std::string exitReason);
    // idleExitSeconds > 0 and the last completed command (or Start, if none) is
    // older than that. 0 never expires. False before Start.
    bool IdleExpired() const;
    bool OwnerGone() const;   // ownerPid is set and that process is no longer live
    // Main thread, once per loop iteration, from the predicate the host's own
    // pump uses to skip a frame (the editor's IsMinimized()): publishes it as
    // the gate's `minimized`, so Drain refuses needsFrame commands with
    // host.busy{state:"minimized"} while the rest still run. A no-op before Start.
    void NoteMinimized(bool minimized);
    // Final fix pass (C-I3): a module reload is synchronous on the main
    // thread, so no Drain sees it. The host calls this when its PluginHost
    // begins (true) and ends (false) a reload (PluginHost::SetReloadObserver):
    // the state "reloading" is published to /status and the descriptor at once.
    void NoteReloading(bool reloading);
    HostGate& Gate();
    const HostGate& Gate() const;       // ruling F9: WaitPoll reads it through a const session
    const Config& GetConfig() const;    // ruling F9: the const hooks accessor (dirty, producersIdle, hookUser, ...)
    // AM1: the fixed-tick hosts' run/pause/step gate. Start sets it from
    // Config::holdSim: held = not running and paused (Consume is Idle until a
    // play command), otherwise running (Consume is Run, the pre-remote loop).
    TickGate& Tick();
    // ruling F10: the host passes its own m_plugin->Generation(); HostSession holds no PluginHost
    void NoteReload(std::uint32_t generation, bool ok, std::string reason, std::uint64_t frame);
    // Task 30: how many reloads NoteReload has recorded (ok or not), and the
    // last one's outcome. host.wait reload records the count when it arms, so
    // a failure noted after that answers module.reload-failed (exit 6).
    std::uint64_t ReloadSerial() const;
    // Task 30 fix round 1 (spec s4.7: frozen-clock captures): true while a
    // `wait settled` on this session is in its capture phase. Each host ORs it
    // into the freeze its --settle hold already applies (frame and sim dt 0):
    // the editor's SettleHoldActive, the runtime's settleHold through
    // FrameIo::remoteSettleHold. False before Start and after Stop, so a wait
    // that ends, fails, is cancelled or is torn down restores the clock.
    bool SettleClockFrozen() const;
    bool LastReloadOk() const;
    const std::string& LastReloadReason() const;
    std::uint64_t LastReloadFrame() const;
    std::uint16_t Port() const;          // 0 unless started
    const std::filesystem::path& DescriptorFile() const; // where Start wrote (or would write) the descriptor
    bool TakeQuit() const;              // host.quit sets it; Task 16
    void RequestQuit();

    // Task 26 (stats.frame): the session's one snapshot. The sim-advance
    // branch of each host calls NoteSimAdvance, and only that branch (idle,
    // minimized, held and render-only iterations do not). `frame` is the
    // host's own host.frame value after the advance, `dtSeconds` its frame dt,
    // `tickMs` the wall time of the advance; renderMs is the last value given
    // to NoteRenderMs (0 until a render pass completes; a server never gives
    // one). No-ops before Start.
    FrameStats& Stats();
    void NoteSimAdvance(std::uint64_t frame, double dtSeconds, double tickMs);
    void NoteRenderMs(double renderMs);

private:
    void PublishStatus(std::int64_t nowUnixMs);
    void RefreshState();                         // the state rule above; a no-op once shutting-down is latched
    void WriteHeartbeat(std::int64_t nowUnixMs); // the whole descriptor, owner-only, a missing file included
    void PublishCatalogue();
    void Teardown(); // the shared tail of Stop and of a failed Start

    Config m_config;
    // Task 20 (ruling F12): the user of the cvar.* commands and console.exec (Task 21),
    // {CVarRegistry::Get(), config.cvarContext}; Start installs it, Teardown releases it.
    CvarCommandUser m_cvarUser;
    // Task 22: the store a runtime or server session owns (Config::problemStore
    // null); Start installs it as the engine sink, Teardown uninstalls it.
    std::unique_ptr<DiagnosticStore> m_ownedStore;
    FrameStats m_stats;       // Task 26: Start registers stats.frame on it, Teardown releases it
    double m_lastRenderMs = 0;
    HostGate m_gate;
    TickGate m_tick;
    JobTable m_jobs;
    std::unique_ptr<ListenerAtoms> m_atoms;
    std::unique_ptr<RequestQueue> m_queue;       // sized by Start (QueueDepthSetting)
    std::unique_ptr<RemoteDispatcher> m_dispatcher;
    std::unique_ptr<RemoteListener> m_listener;

    std::filesystem::path m_descriptorPath;
    std::string m_project;                       // projectRoot, UTF-8, as the descriptor and status carry it
    std::uint64_t m_processStart = 0;
    std::vector<std::string> m_capabilities;     // config.capabilities, sorted
    HostKind m_kindBefore = HostKind::Editor;    // restored by Stop (ruling S3-9)
    bool m_started = false;
    bool m_startAttempted = false;               // a session object starts at most once
    const char* m_exitReason = nullptr;          // latched by DrainFrame
    bool m_generationKnown = false;              // DrainFrame's first generation is the boot module
    bool m_descriptorWritten = false;            // Teardown deletes only a descriptor this session wrote
    Descriptor m_descriptor;                     // what Start wrote; each heartbeat rewrites it whole (C-M3)
    bool m_reloading = false;                    // NoteReloading (C-I3)
    bool m_stopping = false;                     // shutting-down is latched (MarkStopping / Stop)
    std::uint64_t m_requestsSeen = 0;            // ListenerAtoms::requests at the last Drain (C-M4)

    bool m_reloadOk = true;
    std::string m_reloadReason;
    std::uint64_t m_reloadAtFrame = 0;
    std::uint64_t m_reloadSerial = 0;           // NoteReload calls, ok or not (Task 30)
    std::uint64_t m_reloadAttemptsSeen = 0;     // the host's PluginHost::ReloadAttempts() at the last DrainFrame
    bool m_reloadAttemptsKnown = false;         // the first DrainFrame adopts the count silently
    std::uint32_t m_publishedCatalogue = 0;
    bool m_cataloguePublished = false;

    std::chrono::steady_clock::time_point m_lastActivity{};
    std::chrono::steady_clock::time_point m_lastHeartbeat{};
    std::atomic<bool> m_quit{false};
};

} // namespace Arcane::Remote

#endif
