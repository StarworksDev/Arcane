#pragma once

// Play-in-editor state machine (Epic 04, Task 8). Play() snapshots the scene and
// unpauses the RunLoop; Stop() restores the snapshot and re-pauses -- so play-time
// mutation is discarded and edit-mode edits (the snapshot content) are the authored
// state that survives a Play/Stop cycle. Arcane Editor boots in Edit (paused); see
// EditorApp::Init.
//
// Snapshot path: when a plugin vtable exporting SaveState is supplied, Play/Stop route
// through the plugin's GamePlugin_SaveState/LoadState so the plugin captures AND
// re-establishes its own native resources (e.g. the Manifold2D physics world), which
// the raw registry snapshot omits -- restoring via Runtime::RestoreRegistry alone would
// leave those resources gone and the plugin's systems dereferencing null. With no
// plugin (a pure-ECS or test host) Play/Stop fall back to Runtime::SnapshotRegistry/
// RestoreRegistry. Play/Stop take the PluginHost (not a bare vtable) since the
// Core-DLL split's Task 7: the same object is what attaches/detaches the embedded
// server world below, so one parameter carries both jobs.
//
// TOPOLOGIES (Core-DLL split, spec docs/specs/2026-09-15-core-dll-split-design.md
// s7, plan 1 Task 7). The snapshot/restore above is unchanged in ALL of them -- what
// a topology decides is how many WORLDS the Play session runs and what NET ROLE each
// one plays:
//   Standalone     -- one world, both roles. Today's PIE, unchanged.
//   ListenServer   -- one world, ListenServer: it presents AND has authority (R3).
//   EmbeddedServer -- TWO worlds in THIS process: the editor's, re-roled Client, and
//                     a second DedicatedServer Runtime on the same ProcessContext,
//                     seeded from a registry-only snapshot of the authored scene.
//   ClientOnly     -- one world, Client. The authority lives in a SEPARATE
//                     ArcaneServer.exe the EDITOR spawns (Project/ServerLaunch.hpp);
//                     this class knows nothing about that process.

#include <Arcane/Base/Runtime.hpp>   // std::optional<Runtime> m_server needs the COMPLETE type
#include <Arcane/Reflection.hpp>     // PlayLaunchMode is the editor.play.launchMode enum cvar

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace Arcane { class PluginHost; struct PluginVTable; }

namespace Arcane::Editor
{
    enum class EditorMode { Edit, Play };

    // Play-mode dropdown (Task 6, runtime-host-fold arc): where the transport's
    // Play button sends the click. Viewport = PlaySession above, unchanged
    // (snapshot/restore, tinted toggle). SeparateWindow = the SceneSession
    // LaunchStandalone intent, whose effect is EditorApp::DoLaunchStandalone
    // -- a fire-and-forget spawn of ArcaneRuntime.exe on the active scene;
    // PlaySession/the toggle are never touched by it (there is nothing for
    // Stop to restore). Declared here (rather than inline in EditorApp.hpp)
    // so EditorPanels.hpp's DrawSimTimeToolbar can see it without EditorPanels
    // depending on EditorApp.hpp (which itself includes EditorPanels.hpp --
    // that would invert the panel/app layering). EditorApp.hpp and
    // EditorPanels.cpp both already include this header.
    //
    // THE LAST THREE ARE THE Task-7 ROWS, and they are PERSISTED AS AN
    // ORDINAL -- the editor.play.launchMode cvar (settings S6-32) and the old
    // "[EditorPlayMode][State]" ini line it imports once -- so append new
    // ones at the END and never renumber: an existing desk's saved value
    // would otherwise silently name a different mode. The old line's range
    // check (ReadPlayModeIniLine) has SeparateServerProcess as its upper
    // bound for the same reason.
    // ListenServer/EmbeddedServer stay IN the viewport (they are PlaySession
    // topologies); SeparateServerProcess is the viewport world as a CLIENT
    // plus a spawned ArcaneServer.exe.
    enum class PlayLaunchMode
    {
        Viewport,
        SeparateWindow,
        ListenServer,
        EmbeddedServer,
        SeparateServerProcess,
    };

    // editor.play.launchMode (settings S6-32) stores the declared ORDINAL, so
    // the reflected order is the declaration order above (append only).
    ARC_REFLECT_ENUM(PlayLaunchMode)
        ARC_REFLECT_ENUM_VALUE(PlayLaunchMode, Viewport)
        ARC_REFLECT_ENUM_VALUE(PlayLaunchMode, SeparateWindow)
        ARC_REFLECT_ENUM_VALUE(PlayLaunchMode, ListenServer)
        ARC_REFLECT_ENUM_VALUE(PlayLaunchMode, EmbeddedServer)
        ARC_REFLECT_ENUM_VALUE(PlayLaunchMode, SeparateServerProcess)
    ARC_END_REFLECT_ENUM()

    // What Play() is being asked to STAND UP -- distinct from PlayLaunchMode,
    // which is a UI row. SeparateWindow has no topology at all (it never enters
    // Play), and SeparateServerProcess maps onto ClientOnly here plus a spawn
    // the EditorApp performs; the other three map one-to-one.
    enum class PlayTopology { Standalone, ListenServer, EmbeddedServer, ClientOnly };

    class PlaySession
    {
    public:
        [[nodiscard]] EditorMode Mode() const noexcept { return m_mode; }
        [[nodiscard]] bool IsPlaying() const noexcept { return m_mode == EditorMode::Play; }
        [[nodiscard]] PlayTopology Topology() const noexcept { return m_topology; }

        // Edit -> Play: snapshots the scene, then unpauses the RunLoop. Routes through
        // `host`'s vtable SaveState when it is non-null and exports one; otherwise uses
        // Runtime::SnapshotRegistry. No-op success (does not re-snapshot) if already
        // playing. `topology` additionally decides the world set -- see the enum above.
        //
        // FALSE means NOTHING WAS ENTERED: a failed snapshot, a failed restore into
        // the embedded server world, or a PluginHost that REFUSED to attach it
        // (AttachRuntime enforces the one-ComponentRegistry invariant). The session
        // stays in Edit and any half-built server world is destroyed.
        bool Play(Arcane::Runtime& runtime, Arcane::PluginHost* host = nullptr,
                  PlayTopology topology = PlayTopology::Standalone);

        // Play -> Edit: tears down the embedded server world (if any), returns the
        // editor's world to Standalone, restores the snapshot via the SAME path Play
        // used (plugin LoadState or Runtime::RestoreRegistry), then re-pauses the
        // RunLoop. No-op success if already stopped (Edit).
        //
        // FALSE (the restore failed) STAYS IN PLAY: Mode() is still Play, the snapshot
        // is kept, the paused flag is set, and a second Stop retries. Callers that go on
        // to write or swap the scene must check it.
        bool Stop(Arcane::Runtime& runtime, Arcane::PluginHost* host = nullptr);

        // Test seam: the NEXT Stop's restore fails (one shot, cleared by that Stop).
        // editor.play.failnextrestore arms it for Servitor.
#if !defined(ARC_BUILD_DIST)
        void FailNextRestoreForTesting() noexcept { m_failNextRestore = true; }
#endif

        // The embedded DedicatedServer world while playing EmbeddedServer, else null.
        // Non-owning view of an optional this object owns -- never outlives Stop().
        [[nodiscard]] Arcane::Runtime* ServerWorld() noexcept
        { return m_server ? &*m_server : nullptr; }

        // Advance the embedded server world by one host frame. No-op when there is
        // none. It is the SERVER world's own loop: the module's FixedUpdate hook is
        // bound to the PRIMARY (the editor's world) and is deliberately NOT re-run
        // here, the same rule MultiRuntimeReloadTest's StepAll documents -- what this
        // world runs is its own instantiated (Server-masked) system set.
        //
        // Final fix round 2 (I-3): the server world's loop mirrors the primary's
        // every frame -- paused when it is paused, and one single step when the
        // primary took one this frame (read before the primary's Advance
        // consumed it). Toolbar and servitor pause, resume and step therefore
        // keep both worlds in lockstep, whoever drove them.
        void TickServer(double realDt, bool primaryPaused = false, bool primarySingleStep = false);

    private:
        EditorMode              m_mode = EditorMode::Edit;
        std::vector<std::byte>  m_snapshot;
        bool                    m_usedPlugin = false;   // which path Play used; Stop matches it
#if !defined(ARC_BUILD_DIST)
        bool                    m_failNextRestore = false;   // FailNextRestoreForTesting
#endif

        PlayTopology            m_topology = PlayTopology::Standalone;
        // The embedded DedicatedServer world and the registry-only seed it was
        // built from. The seed is kept for the session's life purely as the bytes
        // that produced this world -- Stop drops both together.
        std::optional<Arcane::Runtime> m_server;
        std::vector<std::byte>         m_serverSeed;
    };

    // --play-as's scripted Play (EditorApp::Init, last in boot; Servitor final
    // fix round 3, N-3): `armFirst` (the editor's remote arm; null = none)
    // runs BEFORE Play, so the remote session arms in the edit-mode topology
    // as it does when the same session is entered interactively, then Play
    // enters `playAs` (listen-server | embedded-server | client | standalone).
    // Returns Play's result.
    bool EnterScriptedPlay(PlaySession& play, Arcane::Runtime& runtime, Arcane::PluginHost* host,
                           std::string_view playAs, void (*armFirst)(void*), void* user);

    // `servitor play --topology` (Servitor P0 Task 29, spec s7.3): standalone ->
    // Standalone, listen -> ListenServer, client+server -> EmbeddedServer.
    // ClientOnly has no remote spelling in P0. nullopt for anything else.
    [[nodiscard]] std::optional<PlayTopology> PlayTopologyFromRemote(std::string_view spelling);
    // The running topology as servitor reports it: the three spellings above,
    // and "client" for ClientOnly (reported, never accepted as input in P0).
    [[nodiscard]] std::string_view PlayTopologyRemoteName(PlayTopology topology);

    // What follows a Play/Edit flip, wherever it happened: the toolbar, servitor
    // play/stop, or a scene/save Stop (Task 29 fix round 1: one rule, so the
    // sites cannot drift). EditorApp::AfterPlayFlip applies it.
    struct PlayFlipFollowUps
    {
        bool stopServerProcess    = false;   // Play -> Edit: the separate server process is the session's other half
        bool settingsWorldCreated = false;   // Edit -> Play: NextWorld settings apply with the new world (spec s6.5)
        bool clearPlayRefusal     = false;   // Edit -> Play: retract the "editor:play" Problems row a refused Play left
        bool sceneReplaced        = false;   // either way: Play and Stop swap the registry OBJECT (scene epoch)
    };
    [[nodiscard]] PlayFlipFollowUps PlanPlayFlip(bool wasPlaying, bool nowPlaying);

    // Whether this frame's RunLoop::Advance is a sim advance the remote
    // surface counts in host.frame (Task 29, ruling A; AM1): playing, and
    // either running or holding a requested single step. Ask BEFORE Advance
    // (it consumes the step). A paused Play frame and every Edit frame (Edit's
    // loop is paused; EditModeSchedule owns its pass) are not advances.
    [[nodiscard]] bool CountsAsSimAdvance(bool playing, const Arcane::RunLoop& loop);
}
