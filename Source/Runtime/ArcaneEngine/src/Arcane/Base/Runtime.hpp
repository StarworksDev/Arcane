#pragma once
#include <Arcane/Modules/ArcaneEngineApi.hpp>

// Runtime: the engine facade handed to plugins via EngineContext. Owns the substrate
// that MUST outlive plugin reloads -- the shared TypeContext (installed in THIS module,
// ArcaneCore.dll), the persistent ComponentRegistry, the (swappable) Registry, the
// per-phase schedulers, the RunLoop, and the JobSystem. ARC_ENGINE_API: the plugin and
// the host both call it.
//
// HEADLESS (Core-DLL split, spec docs/specs/2026-09-15-core-dll-split-design.md s2,
// plan 1 Task 4): this class carries no audio device, no input snapshot, no camera,
// no ImGui handoff and no render bridge -- that half is ClientRuntime's
// (Arcane/Client/ClientRuntime.hpp, ArcaneClient.dll), which owns a Runtime and
// attaches itself here. ArcaneServer, a bare Runtime in a test, and the editor's
// embedded server world run with no client at all.

#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Core/Api.hpp>
#include <Arcane/Ecs.hpp>   // the Arcane:: names (aliases) this header's declarations spell; Result is by value
#include <Arcane/Guid.hpp>
#include <Arcane/Project/AssetRegistry.hpp>   // AssetRegistry::ScanProgressFn (OpenProject's progress param) -- light header, not Project.hpp
#include <Arcane/Project/ProjectOpenOptions.hpp>   // ProjectOpenOptions (OpenProject's opts param) -- also light, also not Project.hpp
#include <Arcane/Plugin/SystemFactory.hpp>   // NetMode (a ctor default argument) + the factory table this Runtime instantiates from
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Arcane
{
    class Assets;
    class JobSystem;
    class ProcessContext;
    struct ITaskExecutor;
    class Project;
    class Config;
    // The client seam (spec s2 + plan 1 P6). ClientRuntime is an ArcaneClient.dll
    // type Core NEVER dereferences -- it only stores and hands the pointer on --
    // so a forward declaration is the whole dependency. IClientHooks is Core's own
    // (Arcane/Plugin/ClientHooks.hpp); consumers that CALL through it include that
    // header themselves.
    class ClientRuntime;
    struct IClientHooks;
    // The replication seam (Arcane/Sim/NetDriver.hpp). Runtime stores it, never
    // owns it, and asks it exactly one question -- see SetNetDriver below.
    struct INetDriver;
    // The cvar rungs (Arcane/Config/CVarRegistry.hpp, settings spec s4.4).
    // Runtime only builds and stores them; CVarRegistry::ApplyLayersFor
    // consumes them, so callers of CVarLayerSources include that header.
    struct LayerSources;
    enum class CVarContext : std::uint8_t;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unique_ptr<Impl> member on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARC_ENGINE_API Runtime
    {
    public:
        // Every Runtime is built on the process's ONE ProcessContext (spec 2026-09-15
        // s3): its TypeContext is the shared component-ID space; there is no
        // Runtime-owned context any more. The ctor installs that context in THIS
        // module's Astra slot (Resident) and registers the engine roster, so the
        // FIRST Runtime in a process still pins the numbering (the TypeContext-theft
        // note in ArcaneTests stands).
        // The audio-device flag that used to live here is ClientRuntime's now
        // (there is no audio device below the client seam).
        //
        // `mode` is THIS world's network role (spec s4). It decides which of the
        // loaded module's registered system factories this Runtime instantiates,
        // and it is entirely independent of how the process was LAUNCHED
        // (ProcessContext::IsDedicatedServerProcess) -- an editor process hosts a
        // DedicatedServer world beside a Client one.
        explicit Runtime(ProcessContext& process, NetMode mode = NetMode::Standalone);

        // SECONDARY-WORLD ctor: build this world on an EXISTING ComponentRegistry --
        // the PRIMARY Runtime's (spec s4, the N-worlds-on-one-module invariant).
        //
        // WHY IT EXISTS. A game module opens its Arcane::ComponentModule on the
        // primary Runtime's registry and nowhere else (GameModule.hpp), so that is
        // the only registry its component descriptors reach. A secondary world with
        // a registry of its own would resolve NONE of the module's types: scene
        // load and AddComponentByTypeName would miss, default-construct would have
        // no descriptor, and a snapshot taken in one world would fail to load in the
        // other (Astra's Registry::Load answers UnknownComponent for a type the
        // target registry lacks). Component types are registered ONCE per DLL load
        // and every world is stamped from that one registration; only registry DATA
        // and the instantiated SYSTEM set are per-world.
        //
        // Pass `primary.Components()`. PluginHost::AttachRuntime REFUSES a secondary
        // whose Components() is not the primary's, so the invariant is enforced at
        // the one place it can be.
        Runtime(ProcessContext& process, NetMode mode,
                std::shared_ptr<::Arcane::ComponentRegistry> sharedComponents);
        ~Runtime();

        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        // --- network role + the module's systems (spec s4) ---
        [[nodiscard]] ProcessContext& Process()      noexcept;
        [[nodiscard]] NetMode         Mode()         const noexcept;
        // True for every mode but Client: this world owns the authoritative
        // simulation. The ONE predicate gameplay code should branch on.
        [[nodiscard]] bool            HasAuthority() const noexcept;

        // Re-role this world: clear the systems (which reinstalls the engine pair
        // and, through the client hooks, the presentation ones) and re-instantiate
        // the module's factories for the new mode. Valid with no module loaded --
        // the table is simply empty and nothing is instantiated.
        void SetNetMode(NetMode m);

        // Instantiate every factory in Process().SystemFactories() whose mask
        // matches Mode(), into the matching phase scheduler. Idempotent: a second
        // call re-adds nothing (Astra answers AlreadyRegistered, which is ignored).
        // Called at the end of the ctor -- so a Runtime built AFTER the module
        // loaded still gets its systems -- and by PluginHost on load, attach and
        // hot reload.
        std::size_t InstantiateModuleSystems();

        // The replication driver for this world, or null. Set by the (future) net
        // layer; PluginHost asks IsActive() before a hot reload and refuses while
        // it answers true. Non-owning.
        void                       SetNetDriver(INetDriver* d) noexcept;
        [[nodiscard]] INetDriver*  NetDriver() const noexcept;

        // --- substrate the plugin registers into / the host drives ---
        ::Arcane::Registry&       Registry()      noexcept;
        SystemSchedulers&         Schedulers()    noexcept;
        RunLoop&                  Loop()          noexcept;
        // Re-rate this Runtime's fixed step (a dedicated server's tick, settings
        // arc S6-GATE): the loop, the installed Arcane::PhysicsSystem2D's step and the
        // rate a ClearSystems reinstall uses all follow `hz`, so physics
        // advances by the step the loop actually runs. Call before a game
        // module loads (the Arcane::PhysicsSystem2D is re-added). Ignores hz <= 0 or
        // non-finite.
        void                      SetFixedHz(double hz);
        ::Arcane::TypeContext*    TypeContext()   noexcept;
        ::Arcane::IWorkScheduler* WorkScheduler() noexcept;
        ITaskExecutor*            TaskExecutor()  noexcept;   // enki pool, worker-index ParallelFor face
        // The shared background job queue (F2b Task 12): JobSystem::Submit for
        // fire-and-forget CPU work off the main thread -- the editor's
        // background texture cook (Arcane::Editor::CookQueue) is the first
        // production consumer. Same instance WorkScheduler()/TaskExecutor()
        // already expose two other faces of; this is the third. Reference,
        // not pointer: the JobSystem is the process pool, owned jointly by the
        // Runtimes on this ProcessContext (ProcessContext::AcquireJobSystem), so
        // the reference stays valid as long as any Runtime on that context is
        // alive -- including after this Runtime is destroyed if a sibling lives.
        // Submit work is drained only by the LAST Runtime's drop; a task that
        // captures one Runtime's state must be drained by its owner before that
        // Runtime dies.
        JobSystem&                Jobs() noexcept;
        std::shared_ptr<::Arcane::ComponentRegistry> Components() noexcept;
        Assets&                   AssetsFacade() noexcept;
        Config&                   Configuration() noexcept;   // layered engine+project config (Slice 3)

        // --- the client seam (Core-DLL split, spec s2 + plan 1 P6) ---
        // A ClientRuntime (ArcaneClient.dll) attaches itself here at construction. Core
        // never dereferences `client` (it is a forward-declared Client type) -- it hands
        // it to the module through EngineContext (Task 5) -- and reaches presentation
        // ONLY through `hooks`. Both null on a headless host (ArcaneServer, a bare
        // Runtime in a test, the editor's embedded server world).
        void           AttachClient(ClientRuntime* client, IClientHooks* hooks) noexcept;
        ClientRuntime* Client()      const noexcept;
        IClientHooks*  ClientHooks() const noexcept;

        // --- project (Slice 1b) ---
        // ONE Runtime owns the process's project state -- Arcane::Paths' project
        // and the User/Project/Plugin cvar rungs on the process-wide registry
        // (S2-H): the first to open a project, until it closes it or is
        // destroyed (which releases it as CloseProject does). A second Runtime
        // may open a project for its own assets and Config, but configures no
        // Paths, layers no cvar rungs and releases nothing of the owner's.
        //
        // Open a project folder or .arcproj: validate-then-commit. On success the
        // Project is adopted and the Assets facade's content root is set to the
        // project's game:// mount (root/Content); returns false and leaves ALL state
        // untouched only on a missing/invalid manifest (Project::Open failure). An
        // engineAbi that does not match this engine (kGamePluginABIVersion) does NOT
        // block the open -- it ARC_WARNs and proceeds, since the manifest stamp only
        // describes what the game DLL was built against and the project's data is
        // ABI-agnostic. The one dangerous act, loading that stale DLL, is refused
        // separately by the module-level ABI gate at plugin load (Plugin.cpp), not
        // by this function. Both hosts open a project through here.
        //
        // `onProgress`, when non-empty, is forwarded straight through to
        // Project::Open (and from there to AssetRegistry::ScanContent) -- this
        // is the seam project_open's boot-stage body (ProjectBoot.cpp) uses to
        // surface "Scanning content... N / M" while a project's content tree
        // is being scanned.
        //
        // `opts` is likewise forwarded verbatim to Project::Open -- the seam a
        // host uses to decline the diag:// mount for a deterministic verify
        // capture (HostBoot::OpenOptionsFor, ProjectBoot.hpp). Defaulted, so no
        // existing caller changes.
        bool OpenProject(const std::filesystem::path& pathOrFile,
                         AssetRegistry::ScanProgressFn onProgress = {},
                         ProjectOpenOptions opts = {});

        // The open project, or nullptr when none is open (no-project fallback mode).
        const Project* CurrentProject() const noexcept;

        // Discard the open project and return to the SAME no-project state a
        // freshly constructed Runtime starts in: no content root (Assets falls
        // back to exe-relative), no asset resolver (AssetId loads fail), and
        // Configuration() rebuilt from engine defaults only -- every plugin/
        // project config layer dropped, not merely shadowed.
        //
        // Exists because OpenProject's own "returns false and leaves ALL state
        // untouched" contract (above) means two different things depending on
        // when it fires: at boot, with no project open yet, "untouched" is
        // already no-project. Mid-session, after a project WAS open, a failed
        // re-open leaves CurrentProject() pointing at the OUTGOING project --
        // stale, not merely old -- even after a caller has torn down every-
        // thing downstream of it (plugin, documents, scene). This is the call
        // that makes "project-less after a failed switch" the SAME state as
        // "project-less at boot", rather than a second, ad hoc definition.
        // A no-op is not offered for "already project-less" -- callers that
        // reach here always just tore something down.
        void CloseProject();

        // --- the user cvar archive (T3-D2) ---
        // OpenProject reads the project's Saved/Config/ as the cvar User layer.
        // A project's rungs leave with it: CloseProject, and OpenProject
        // replacing an open project (a switch), drop every SetBy::User, Project
        // and Plugin record (CVarRegistry::RevertLayer) so the next project
        // starts from its own files. With archiving ON (off by default) they
        // first WRITE the User layer back (WriteCVarArchive: Archive cvars the
        // User rung holds, nothing else).
        // The editor turns it on for a windowed session; ArcaneRuntime and
        // ArcaneServer leave it off -- neither host sets the User rung itself,
        // and a shipped game's settings belong in a per-user directory, not
        // inside the project tree. The DROP is unconditional all the same:
        // every host's OpenProject LOADS the User rung from Saved/Config, so
        // without it project A's User records would outlive A in any host,
        // shadow B's weaker rungs and (when archiving) land in B's file.
        void SetUserCVarArchiving(bool enabled) noexcept;
        // Write the open project's User archive and, when configured, the
        // EditorUser archive NOW (no layer is dropped). The editor calls it
        // before unloading its game module (whose cvars would then be gone)
        // and at exit. False when archiving is off, or when there is neither
        // a project nor an EditorUser folder.
        bool SaveUserCVars();

        // --- the EditorUser rung (settings arc S2, spec s11.1) ---
        // Machine-wide editor preferences: SettingScope::PreferencesMachine
        // cvars (theme, fonts, shortcuts, layouts) live in `dir`. The editor
        // passes Paths::Get(EditorUserDir) / "Config" for an INTERACTIVE session
        // only, so a scripted, headless or golden run never reads or writes
        // machine state.
        // Setting it applies the layer now (SetBy::EditorUser, between Project
        // and User). Every OpenProject re-applies it, and it survives
        // CloseProject and project switches. With archiving on, SaveUserCVars
        // and a switch also write it (WriteCVarArchive(..., SetBy::EditorUser)).
        // Empty = no EditorUser rung (the default).
        void SetEditorUserConfigDir(std::filesystem::path dir);
        [[nodiscard]] const std::filesystem::path& EditorUserConfigDir() const noexcept;

        // --- the cvar rungs (settings spec s4.4) ---
        // Every config rung this Runtime layers -- the engine rung, then each
        // active plugin, the project, the optional EditorUser folder, and the
        // user dir of the open project --
        // plus the host's --set list. That is exactly what
        // CVarRegistry::ApplyLayersFor needs to give a module that (re)loads
        // later the values a cold boot would.
        [[nodiscard]] LayerSources CVarLayerSources() const;
        // The host's --set list and its context (the input_config boot stage
        // records them), kept for the modules that load after that stage.
        void SetCVarCommandLine(std::vector<std::string> sets, CVarContext context);

        // Register an editor-created asset file with the open project's registry
        // (Project::RegisterAsset). Idempotent. nullopt when no project is open or
        // the file lies outside every content root.
        std::optional<Guid> RegisterCreatedAsset(const std::filesystem::path& file);

        // T5 s7.2 (asset file ops), narrow seams beside RegisterCreatedAsset: unmap a
        // deleted asset / rebind a moved one in the open project's registry. GUID loads
        // see both at once (the installed resolver reads the live registry).
        bool UnregisterAsset(const Guid& id);
        RebindResult RebindMovedAsset(const Guid& id, const std::filesystem::path& newFile);

        // Point the open project's boot scene at `id` (Project::SetBootScene),
        // rewriting its .arcproj in place. False when no project is open or the
        // write failed; Project::SetBootScene already logs the specific cause in
        // the latter case. Narrow seam (mirrors RegisterCreatedAsset) rather than
        // a mutable CurrentProject accessor: this is the one mutation the editor
        // needs, and it keeps Project's mutability from leaking to every caller.
        bool SetProjectBootScene(const Guid& id);
        bool SetProjectInputActionsAsset(const Guid& id);

        // Re-stamp the open project's manifest engine.abi (Project::
        // RestampEngineAbi). Same narrow-seam shape as SetProjectBootScene
        // above -- and the same reason: it keeps Project's mutability from
        // leaking into a mutable CurrentProject accessor. False when no
        // project is open or the rewrite failed (Project logs the cause).
        // The editor's TWO legitimate call moments are documented on
        // Project::RestampEngineAbi -- a content-only project at open, and a
        // module project immediately after its module was REBUILT against
        // this engine (Build -> Rebuild Game Module), the one moment the
        // stamp and the DLL agree again.
        bool RestampProjectEngineAbi(int abi);

        // THE RENDER / CAMERA / INPUT / IMGUI BRIDGES LIVE ON ClientRuntime NOW
        // (Arcane/Client/ClientRuntime.hpp). They are presentation: a Batcher2D, the
        // sprite/mesh resolution tables, the 2D camera, the host's input snapshot and
        // the cross-DLL ImGui handoff. Core reaches the last of those -- and only that
        // one -- back through IClientHooks::FillEngineContext.
        //
        // NO RENDER-RESOURCES BRIDGE LIVES THERE EITHER. A plugin is handed no
        // graphics device and no shader library, so it builds no engine
        // render objects of its own.
        //
        // If a plugin ever needs GPU resources, that is a new, deliberate API
        // over the render graph -- not a backend pointer handed across the
        // plugin boundary.

        // --- hot-reload support (plugin Save/LoadState + the host call these) ---
        // Registry::Save() -> framed snapshot bytes. Returns a Result so a Save
        // failure surfaces as an actionable error at the call site rather than an
        // empty-but-"ok" vector that masks data loss as a later reload failure.
        ::Arcane::Result<std::vector<std::byte>, ::Arcane::SerializationError> SnapshotRegistry() const;

        // The Save configuration every SnapshotRegistry uses, process-wide
        // (settings arc S6-45). Core's default is Astra's SaveConfig{}. The knob
        // that changes it, astra.snapshot.compression, is an Editor setting, so
        // ArcaneEditor declares it (spec s3.2: a shipped game holds no Editor
        // settings) and pushes its choice here from the setting's publish
        // callback; a game keeps the default. Guarded: any thread may read.
        static void SetSnapshotSaveConfig(const ::Arcane::Registry::SaveConfig& config);
        [[nodiscard]] static ::Arcane::Registry::SaveConfig SnapshotSaveConfig();

        // Swaps in a registry deserialized from bytes (3.3 Load keeps the workScheduler) and rebinds the
        // RunLoop. The SystemSchedulers are KEPT; the host clears + re-registers systems around a reload
        // (ClearSystems before the plugin's Init). Engine systems receive Registry& per Execute, so running
        // the kept schedulers against the swapped registry is safe.
        bool RestoreRegistry(std::span<const std::byte> bytes);

        // Swaps in a FRESH empty registry (same shared ComponentRegistry + scheduler) and rebinds the
        // RunLoop; SystemSchedulers are kept. Used for a fresh-boot reload so the plugin's Init rebuilds
        // its scene. Caller clears systems first (ClearSystems).
        void ResetRegistry();

        // Clear() all three phase schedulers, reinstall the engine's headless pair,
        // then give the client (if any) its OnSystemsCleared hook so the
        // presentation-side systems come back too.
        void ClearSystems();

        // --- engine-owned physics (2026-09-11, spec docs/specs/2026-09-11-physics-2d-wiring-design.md s4-s5) ---
        // Manifold2D-free surface: hosts and modules never see Arcane::PhysicsSystem2D or
        // PhysicsWorld. InstallEngineSystems adds the engine's HEADLESS pair --
        // Arcane::PhysicsSystem2D then TransformPropagationSystem into fixedUpdate.
        // RenderSubmissionSystem is presentation and is ClientRuntime's to
        // install (it does, at construction and on every OnSystemsCleared), so
        // a Core-only host has exactly the systems it can execute. The ctor
        // calls this, and ClearSystems calls it again after clearing, so every
        // PluginHost load/reload/unload path keeps them. Idempotent (per-system
        // HasSystem guards). A game module registers ONLY its own systems and
        // places them with Arcane::Before/After against these types
        // (GameModule.hpp).
        // EnsurePhysics runs once per frame before Loop().Advance
        // (beside SetRenderContext): it mints Arcane::PhysicsWorld2D + PhysicsInterp
        // Buffer when the current registry lacks them -- scene open,
        // RestoreRegistry (Play -> Stop, structural undo) and hot reload all
        // replace the registry, and the next frame's Ensure is the reset --
        // and re-mints the world when ResolvedGravity changed (the vendored
        // PhysicsWorld has no SetGravity; a settings edit is authoring, not
        // gameplay). PhysicsEditPass is Edit mode's only physics: a bare
        // stepWorld=false pass (mint / destroy / reconcile, no step).
        void      InstallEngineSystems();
        void      EnsurePhysics();
        void      PhysicsEditPass();
        // Drop the physics world (and the interpolation history) so the NEXT
        // EnsurePhysics mints a fresh one from the authored components. The
        // editor calls this on Play: the world the Edit passes minted and
        // reconciled is authoring state (the paused reconcile zeroes a body's
        // velocity on every author move, by design), and Play must start the
        // way ArcaneRuntime boots -- bodies at their authored poses WITH their
        // authored Arcane::RigidBody2D::velocity, applied by PASS 2's mint. A restore
        // on Stop never carries the pair either (both are transient resources),
        // so Play and Stop are symmetric. Lives here rather than in the editor because destroying
        // Arcane::PhysicsWorld2D destroys the PhysicsWorld, and ArcaneEditor.exe does
        // not link Manifold2D. Nothing to do when no world exists yet.
        void      ResetPhysics();
        // Resolved by Arcane::ResolveGravity2D (Scene/PhysicsGravity2D.hpp), the one function
        // the 2D physics hook's EnsureWorld reads too; Engine passes no gravity to a hook.
        // The scene-root Arcane::PhysicsSettings2D component when present, else the
        // `physics.gravity` setting (its project rung, else
        // its default (0, -9.81; +Y up, F4)). Layered on purpose: gravity is
        // authored content, so the built-in default yields to the project and
        // the project to the per-scene Arcane::PhysicsSettings2D component.
        [[nodiscard]] glm::vec2 ResolvedGravity() const;

    private:
        friend struct ::Arcane::Detail::AbiManifest::Access;   // hashes these private members (Plugin/AbiManifestEngine.hpp)
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    // The EngineConfig cvar rung from the ONE engine-config folder (settings
    // arc S2-H): <Paths engine dir>/data/EngineConfig, where the engine dir
    // defaults to the exe dir (and Paths is configured so) unless a host
    // already set one. Layered once per process per folder: a call whose
    // folder is the one already applied does nothing and returns false; a new
    // folder drops the previous folder's EngineConfig records first.
    // HostBoot::ApplyEarlyConfigRungs and every Runtime ctor go through here,
    // so the first Runtime after HostBoot does not re-read the folder. Does
    // NOT publish: returns true when it layered, and the caller publishes.
    // Main thread only.
    ARC_ENGINE_API bool ApplyEngineConfigRung();
}
