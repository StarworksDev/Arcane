#pragma once
#include <Arcane/Modules/ArcaneEngineApi.hpp>

#include <Arcane/Core/Api.hpp>
#include <Arcane/EcsFwd.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace Arcane
{
    class ProcessContext;
    class Runtime;

    // Watches a game DLL, loads versioned copies (PDB-lock dodge), checks the ABI,
    // and rolls back to the last-good image on any failure (never a lost session).
    //
    // N RUNTIMES, ONE MODULE IMAGE (Core-DLL split, spec docs/specs/2026-09-15-core-
    // dll-split-design.md s4/s5, plan 1 P9): a host attaches every world this module
    // serves. The FIRST attached is the PRIMARY -- the world handed to the module as
    // EngineContext::engine, the one whose SaveState/LoadState carries state across a
    // hot reload; every other attached Runtime is snapshot/restored through its
    // registry alone and repopulated from the module's re-registered system factories.
    //
    // WHAT THE N WORLDS SHARE, AND WHAT THEY DO NOT:
    //   * ONE ComponentRegistry -- the PRIMARY's. Component types are registered ONCE
    //     per DLL load (spec R1), and a module opens its Astra::ComponentModule on the
    //     primary's registry alone (GameModule.hpp) -- so a secondary MUST be built as
    //     Runtime(process, mode, primary.Components()), and AttachRuntime REFUSES one
    //     that is not. Otherwise the module's component types resolve in the primary
    //     world and nowhere else, and a registry snapshot moved between the two worlds
    //     fails with UnknownComponent.
    //   * SYSTEMS are PER-WORLD: one factory table, and each world's own NetMode
    //     decides what it instantiates (Arcane/Plugin/SystemFactory.hpp).
    //   * REGISTRY DATA is PER-WORLD: its own entities and resources, its own
    //     snapshot/restore across a hot reload.
    //
    // A PluginHost must NOT outlive its ProcessContext or any attached Runtime (the
    // dtor calls into them); DetachRuntime is the seam for a world that dies first.
    class ARC_ENGINE_API PluginHost
    {
    public:
        // sourceDllPath is the PRIMARY game module (watched + hot-reloaded). Pass an EMPTY
        // path for a plugins-only host: Load() then brings up only the AddPlugin() secondaries
        // (no primary, no hot-reload watch) -- used by the editor when a project has plugin
        // modules but no gameModule. Vtable()/IsLoaded() report no primary; the *All drivers
        // and Unload() already handle a primary-less host.
        //
        // `process` is the process's ONE ProcessContext: the module's TypeContext and
        // the system-factory table the module registers into. At least one Runtime
        // must be attached before Load() -- Load() REFUSES (logs and returns false)
        // a host with none, in every configuration.
        PluginHost(ProcessContext& process, std::filesystem::path sourceDllPath);
        ~PluginHost();

        PluginHost(const PluginHost&) = delete;
        PluginHost& operator=(const PluginHost&) = delete;

        // Attach a world this module serves. The FIRST attach is the PRIMARY and always
        // succeeds; a LATER one is REFUSED (false, host unchanged) unless its
        // Components() is the primary's -- see the shared-ComponentRegistry invariant
        // above. Attaching to an already-loaded host instantiates the module's matching
        // factories into the new Runtime immediately, so a world can join mid-session.
        // Re-attaching an already-attached Runtime is a no-op success.
        bool AttachRuntime(Runtime& rt);
        // Detach a world: its module-registered systems are cleared, its registry is
        // RESET (it holds entities whose descriptors point into the module image, and
        // after this call the teardown paths no longer cover it), and it is dropped.
        // Detaching the PRIMARY of a loaded host is refused (ARC_ERROR, no-op) -- it is
        // the module's own world; Unload() first.
        void DetachRuntime(Runtime& rt) noexcept;
        [[nodiscard]] std::span<Runtime* const> Runtimes() const noexcept;

        // Register a SECONDARY plugin module (a project Plugins/<name>/Binaries DLL) to
        // load alongside the primary game module. Call BEFORE Load(). Load order = call
        // order. Secondaries share the Runtime, load once (no independent hot-reload), are
        // torn down with the primary, and are re-established across a primary hot-reload.
        void AddPlugin(std::filesystem::path dll);

        bool Load();                       // initial load: copy+load+ABI+Init primary, then secondaries
        void Unload();                     // Shutdown + ClearSystems + ResetRegistry + unload + delete copies
        bool Reload(bool restoreState);    // full sequence; rollback on any failure (re-establishes secondaries)
        void Poll();                       // debounced mtime watch -> Reload(true) on a stable change
        // ARC_MONOLITHIC (a packaged game, engine modularization s3.2): the game is
        // linked in -- Load adopts ArcGameModule_Get()'s table, Reload/ReloadFresh/
        // ForceReload log and return true having done nothing, Poll watches nothing.

        bool ForceReload() { return Reload(true);  }
        bool ReloadFresh() { return Reload(false); }

        // Drive an entry point across the primary module AND every loaded secondary
        // (primary first). Each call is null-guarded, so a disengaged primary or a plugin
        // that omits DrawUI is simply skipped. A multi-module host calls these instead of
        // Vtable()->FixedUpdate/Update/DrawUI directly.
        void FixedUpdateAll(double dt);
        void UpdateAll(double dt, double alpha);
        void DrawUIAll();

        // Re-derive the EngineContext's PRIMARY-dependent fields (engine, client,
        // workScheduler/taskExecutor, netMode and the ImGui handoff) from the primary
        // Runtime as it is NOW. The load/reload/attach paths already do this for
        // themselves; this is the seam for a host that changes the primary's state
        // BETWEEN those points -- specifically Runtime::SetNetMode, which the editor's
        // PlaySession calls on every Play/Stop flip (final-review fix wave, I1).
        // Without it a module reading ctx->netMode sees Standalone ("I have
        // authority") while its world is a Client. A no-op when no Runtime is
        // attached. Cheap: a handful of pointer reads.
        void RefreshEngineContext();

        [[nodiscard]] bool                IsLoaded()   const noexcept;
        [[nodiscard]] const PluginVTable* Vtable()     const noexcept;
        [[nodiscard]] std::uint32_t       Generation() const noexcept;
        // Every Reload that tried (Poll's included, whose bool nobody sees),
        // succeeded or not, and whether the last one succeeded. A failed or
        // refused reload leaves Generation() alone, so these are its only
        // record: a host hands them to its remote session (HostSession::
        // DrainFrame), which answers `servitor wait reload` with the failure.
        // A plugins-only host and a monolithic one never try: 0 and true.
        [[nodiscard]] std::uint64_t       ReloadAttempts() const noexcept;
        [[nodiscard]] bool                LastReloadOk()   const noexcept;
        // Called with true when a Reload begins trying (the attempt ReloadAttempts
        // counts) and with false when it ends, on every outcome. A reload is
        // synchronous on the caller's thread, so this is the only moment a host
        // can publish that it is reloading (Servitor's "reloading" host state).
        // Null clears it; the last call wins. A no-op reload (plugins-only or
        // monolithic) calls nothing.
        using ReloadObserver = void (*)(bool reloading, void* user);
        void SetReloadObserver(ReloadObserver fn, void* user) noexcept;

        // PlaySession (and any other caller that would otherwise invoke the
        // primary vtable's SaveState/LoadState directly) goes through these so
        // a CVarModuleScope is open, matching every other PluginHost vtable
        // call. False when there is no primary, the entry is missing, or
        // SaveState latched an error / LoadState returned false.
        bool SaveStatePrimary(BinaryWriter& w);
        bool LoadStatePrimary(BinaryReader& r);

        // READ-ONLY / DIAGNOSTIC. The very EngineContext struct handed to every
        // loaded module's Init -- exposed so a test (or a host's census) can see what
        // the module sees. Never a mutation seam: the fields are derived, and
        // RefreshEngineContext above is the only supported way to move them. The
        // pointer is stable for this host's lifetime.
        [[nodiscard]] const EngineContext* Context() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
