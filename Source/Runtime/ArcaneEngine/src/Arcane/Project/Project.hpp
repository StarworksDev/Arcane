#pragma once
#include <Arcane/Modules/ArcaneEngineApi.hpp>

// Project: an opened Arcane project -- its manifest, root directory, and mount table.
// Open() discovers/loads a .arcproj and registers default mounts; ResolveAsset() is the
// asset-reference seam (AssetId -> filesystem path via the mounts). Create() scaffolds a
// new project skeleton on disk.

#include <Arcane/Core/Api.hpp>
#include <Arcane/Assets/AssetId.hpp>
#include <Arcane/Project/AssetRegistry.hpp>
#include <Arcane/Project/MountTable.hpp>
#include <Arcane/Project/ProjectManifest.hpp>
#include <Arcane/Project/ProjectOpenOptions.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Arcane
{
    class CVarRegistry;

    // `manifestFile` is the .arcproj the block was read from: each refusal's
    // warning names it, with the key and the reason (settings S7-SEC).
    ARC_ENGINE_API void ApplyLegacyManifestSettings(CVarRegistry& registry, const ProjectManifest& manifest,
                                                  const std::filesystem::path& manifestFile);

    class ARC_ENGINE_API Project
    {
    public:
        // Open a project folder (finds the single *.arcproj inside) or a direct
        // .arcproj file. Loads the manifest and mounts game:// -> <root>/Content,
        // plus diag:// -> <root>/Saved/Diagnostics WHEN that directory already
        // exists (GPU crash diagnostics arc, Task 9) -- a project that has never
        // crashed has no Saved/Diagnostics yet, and stays unmounted rather than
        // an empty-but-mounted scheme; see Project.cpp for the incremental path
        // a report written mid-session takes instead (RegisterAsset).
        // nullopt on: no/multiple .arcproj, or an invalid manifest.
        //
        // `onProgress`, when non-empty, is forwarded to the primary game://
        // content scan (AssetRegistry::ScanContent) -- see that method's own
        // comment for the cost/contract. Not forwarded to each plugin's
        // AddContent below: that folding is comparatively small, and the
        // primary Content/ scan is what a boot splash's "Scanning content..."
        // line is about.
        //
        // `opts` (defaulted) carries the per-open engine settings that are NOT
        // properties of the project itself -- today just mountDiagnostics, the
        // diag:// opt-out a deterministic verify capture needs; see
        // ProjectOpenOptions.hpp. Defaulted so all 41 existing call sites keep
        // their old behaviour untouched.
        static std::optional<Project> Open(const std::filesystem::path& pathOrFile,
                                           AssetRegistry::ScanProgressFn onProgress = {},
                                           ProjectOpenOptions opts = {});

        // Resolve `pathOrFile` (a project folder OR a direct .arcproj file) to
        // the single .arcproj it names -- the exact rule Open() itself uses
        // internally (Open() calls this), exposed so a caller that needs to
        // know WHICH manifest file a path resolves to WITHOUT paying for a
        // full Open() (mounts, plugin discovery, a content scan) can reuse the
        // ambiguity-detection/directory-search logic instead of reimplementing
        // it -- e.g. HostBoot's early config pass resolves the Project rung
        // before Runtime construction and project open.
        // nullopt on the same failure modes as Open(): no .arcproj, or more
        // than one (both logged).
        static std::optional<std::filesystem::path> ResolveManifestFile(
            const std::filesystem::path& pathOrFile);

        // Scaffold a new project at `dir` named `name`: creates the folder skeleton
        // (Source/ Content/ Config/ Plugins/), writes <name>.arcproj and .gitignore,
        // then opens it. nullopt if `dir` exists non-empty or on IO error.
        static std::optional<Project> Create(const std::filesystem::path& dir, std::string name);

        // Move legacy physics/splash blocks to Config/*.json and remove them
        // from .arcproj. The path form is exposed for the migration test and
        // tools that have not opened a full Project.
        static bool MigrateLegacySettingsAt(const std::filesystem::path& manifestFile);
        bool MigrateLegacySettings();

        const ProjectManifest&       Manifest() const { return m_manifest; }
        const std::filesystem::path& Root()     const { return m_root; }
        const std::filesystem::path& ManifestFile() const { return m_manifestFile; }
        const MountTable&            Mounts()   const { return m_mounts; }
        const AssetRegistry&         Registry() const { return m_registry; }

        // The roots (<root>/Plugins/<name>) of the plugins Open() actually activated: those
        // that were manifest-enabled AND had a valid descriptor. The single source of truth
        // for "which plugins are live" -- content mounts key off the same gate, and callers
        // (config layering, host module loading) should iterate THIS, not the raw manifest
        // list (which still lists enabled-but-invalid plugins). In manifest order.
        const std::vector<std::filesystem::path>& ActivePluginRoots() const { return m_activePluginRoots; }

        // The asset-reference seam: AssetId -> physical file. Slice 1 resolves via the
        // MountTable (AssetId backing = a logical mount path). Slice 2 reroutes this
        // through the AssetRegistry (GUID) WITHOUT changing the signature.
        std::optional<std::filesystem::path> ResolveAsset(const AssetId& id) const;

        // Register a file CREATED after the open-time scan (editor New Material /
        // New Instance / save) into the registry: finds which content root contains
        // it (game://, then each active plugin's Content) and registers it under
        // that scheme. Idempotent. nullopt (warn) when the file lies outside every
        // content root -- such a file cannot resolve by GUID by design.
        std::optional<Guid> RegisterAsset(const std::filesystem::path& file);

        // T5 s7.2: the editor deleted `id`'s file -- unmap it. False when unknown.
        bool UnregisterAsset(const Guid& id);
        // T5 s7.2: the editor moved `id` to `newFile` -- rebind under whichever root
        // contains it (RegisterAsset's root list). OutsideContent when none does.
        RebindResult RebindAsset(const Guid& id, const std::filesystem::path& newFile);

        // Point this project's bootScene at `id` (nil clears it), rewriting the
        // .arcproj in place. False on read/parse/write failure, leaving both the
        // file and the in-memory manifest untouched.
        //
        // The Guid, NOT a mount path: the AssetRegistry rebuilds its Guid ->
        // mount-path map on every open, so a scene that moves on disk keeps
        // working. Written atomically via a temp file plus a platform atomic
        // replace (see Project.cpp for the per-platform mechanism) so an
        // interrupted write cannot leave a project with a truncated manifest.
        bool SetBootScene(const Guid& id);
        bool SetInputActionsAsset(const Guid& id);

        // Re-stamp the manifest's engine.abi (same atomic rewrite as
        // SetBootScene) and mirror it in memory. The EDITOR calls this to
        // self-heal a CONTENT-ONLY project across an engine ABI bump -- with
        // no gameModule there is no compiled code behind the stamp, so the
        // old number is inert metadata that would otherwise lock the user
        // out. Callers must NOT restamp a project that has a game module
        // OVER A STALE DLL: the plugin gate compares the DLL's exported ABI,
        // and a restamped manifest over a stale DLL would make the Hub report
        // compatible while the module still refuses to load. The one moment a
        // module project may be restamped is immediately after its module was
        // REBUILT against this engine (the editor's Build -> Rebuild Game
        // Module success path) -- then the stamp and the DLL agree again.
        bool RestampEngineAbi(int abi);

    private:
        // Open()/Create() are the only construction paths (they are static members and
        // can reach this). std::optional<Project> in Runtime move-constructs, never
        // default-constructs, so a private default ctor is safe.
        Project() = default;

        // The content root containing `file` (RegisterAsset's candidate list, in its
        // order): the scheme, the dir as listed (the mount form), the canonical dir the
        // relative match ran against, and the canonical file.
        struct ContentRootMatch { std::string scheme; std::filesystem::path listedDir, rootDir, target; };
        std::optional<ContentRootMatch> FindContentRoot(const std::filesystem::path& file) const;

        std::filesystem::path m_root;
        std::filesystem::path m_manifestFile;   // the .arcproj this project was opened from
        ProjectManifest       m_manifest;
        MountTable            m_mounts;
        AssetRegistry         m_registry;
        std::vector<std::filesystem::path> m_activePluginRoots;   // valid + enabled plugins, in manifest order
    };

    // EditorLock: "which process has this project open", answered from the
    // project itself so it survives launcher restarts. The editor writes
    // <root>/Saved/editor.lock while it holds a project (boot, project
    // switch) and clears it on clean exit; the Arcane Hub reads it to focus
    // the running editor instead of spawning a rival, and to show running
    // badges after ITS process restarted.
    //
    // Unity's lockfile model, minus the flaw its users hate: the lock names
    // {pid, process CREATION time}, and IsAlive() only believes it when that
    // exact pid with that exact start time still runs -- so a crash's stale
    // lock fails validation and is simply ignored, never a false "already
    // open". MIRRORED FORMAT: the Hub parses the same JSON in its
    // editorlock.rs; change BOTH or hub-side detection goes blind.
    namespace EditorLock
    {
        struct Info
        {
            uint32_t pid = 0;
            uint64_t start = 0;   // process creation FILETIME as u64 (0 on non-Windows)
        };

        ARC_ENGINE_API std::filesystem::path FileFor(const std::filesystem::path& projectRoot);

        // Pure halves, exported so the format is pinned by tests.
        ARC_ENGINE_API std::string ToJson(const Info& info);
        ARC_ENGINE_API std::optional<Info> Parse(const std::string& text);

        // THIS process's identity. start is 0 where the platform query fails.
        ARC_ENGINE_API Info Self();

        // Write/clear the lock for a project root. Best-effort: a lock that
        // cannot be written must not fail a project open (WARN only).
        ARC_ENGINE_API void Write(const std::filesystem::path& projectRoot);
        ARC_ENGINE_API void Clear(const std::filesystem::path& projectRoot);

        // Read + validate: Some(pid) only when the named process is STILL the
        // process the lock described AND is still running (pid opens, creation
        // time matches, and its exit time is zero -- a handle held elsewhere
        // keeps a dead pid reserved, so "opens" alone proves nothing).
        ARC_ENGINE_API std::optional<uint32_t> ReadLive(const std::filesystem::path& projectRoot);

        // The liveness half of ReadLive on its own: `info` names a process
        // that is STILL running with that creation time. One line over
        // Platform::IsProcessLive (ArcaneCore), which holds the three tells.
        ARC_ENGINE_API bool IsLive(const Info& info);

        // ReadLive minus ourselves: the pid of ANOTHER live editor holding this
        // project, or nullopt. The direct-launch guard rides this -- the editor
        // refuses to double-open a rival's project (main.cpp boot gate, exit 3;
        // SwitchProject refusal) -- and the self-exemption is what lets a
        // same-project re-open proceed over our own lock.
        ARC_ENGINE_API std::optional<uint32_t> RivalPid(const std::filesystem::path& projectRoot);

        // Bring the first visible top-level window of `pid` to the foreground.
        // MIRROR of the Hub's spawn.rs focus_process_window: the honest answer
        // to "already open" is to surface the editor that has it. False when
        // the process has no visible window yet (mid-boot) -- callers still
        // refuse; the user gets the message instead of the window.
        ARC_ENGINE_API bool FocusWindowOfProcess(uint32_t pid);
    }
}
