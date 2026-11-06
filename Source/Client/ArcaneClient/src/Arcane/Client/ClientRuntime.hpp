#pragma once
#include <Arcane/Modules/ArcaneClientApi.hpp>

// ClientRuntime: the PRESENTATION half of what Runtime used to be (Core-DLL split,
// spec docs/specs/2026-09-15-core-dll-split-design.md s2, plan 1 Task 4). It OWNS a
// headless Arcane::Runtime (ArcaneCore.dll) and adds the audio device, the host's
// per-frame input snapshot, the ViewTransform, the cross-DLL ImGui handoff and the
// render bridge -- everything a server does not have. It also implements
// Arcane::IClientHooks (privately) and attaches itself to its Runtime at
// construction, which is the ONE way Core reaches back into presentation (P6):
// PluginHost (Core) saves/restores the UI context, drops plugin-created audio
// handles on teardown, reinstalls the render systems after a ClearSystems, and
// fills the EngineContext's four ImGui pointers through this interface.
//
// Every interactive host (ArcaneRuntime, ArcaneEditor) owns one of these; a bare
// Arcane::Runtime is the headless shape (ArcaneServer, tests, the editor's embedded
// server world).

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Client/RuntimePresentation.hpp>
#include <Arcane/Plugin/ClientHooks.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <utility>

namespace Arcane
{
    class Batcher2D;
    struct SpriteEntry;            // Scene/SceneCacheRecords.hpp -- only named here (pointer-to-map param)
    struct MeshEntry;               // Scene/SceneCacheRecords.hpp -- SpriteEntry's F2a (3D) sibling
    struct ResolvedMeshMaterial;    // Scene/SceneCacheRecords.hpp -- mesh-material constants

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // STL members on a dll-exported class: benign under /MD (shared CRT heap)
// 4275: IClientHooks is an un-exported base of an exported class. Deliberate, and
// the same reasoning that disables 4251 workspace-wide (premake5.lua): the warning
// is about a cross-CRT/layout mismatch, and IClientHooks is a DATA-LESS pure
// abstract interface compiled from one header by one toolset into one /MD process.
// Exporting it would emit an ArcaneCore vtable nothing needs -- Core only ever
// calls THROUGH the interface, never constructs one.
#pragma warning(disable: 4275)
#endif
    class ARC_CLIENT_API ClientRuntime final : private IClientHooks
    {
    public:
        // enableAudioDevice: as Runtime's flag was -- false = the null backend; an
        // interactive host passes true. The ProcessContext is the process's ONE
        // (spec s3); it is forwarded to the owned Runtime unchanged.
        explicit ClientRuntime(ProcessContext& process, bool enableAudioDevice = false);
        ~ClientRuntime();

        // Non-copyable AND non-movable: the ctor hands `this` to Runtime::AttachClient,
        // so a moved-from/relocated object would leave Core pointing at dead storage.
        ClientRuntime(const ClientRuntime&)            = delete;
        ClientRuntime& operator=(const ClientRuntime&) = delete;
        ClientRuntime(ClientRuntime&&)                 = delete;
        ClientRuntime& operator=(ClientRuntime&&)      = delete;

        // The headless substrate this client owns. Every API that takes a
        // `Runtime&` (PluginHost, HostBoot, PlaySession, the EngineContext the
        // module receives) gets THIS reference -- the module is handed Core's
        // Runtime, never the client.
        [[nodiscard]] Runtime&       Core()       noexcept { return m_core; }
        [[nodiscard]] const Runtime& Core() const noexcept { return m_core; }

        // --- presentation: audio ---
        Audio::AudioDevice& AudioSystem() noexcept;
        void                ResetAudio() noexcept;   // Drop plugin-created audio handles on reload

        // --- input bridge ---
        // Latest per-frame input snapshot. The host (ArcaneRuntime) stores it each frame
        // via SetInputSnapshot; plugins read it via Input() in their update hooks.
        void                 SetInputSnapshot(const InputSnapshot& snap) noexcept;
        const InputSnapshot& Input() const noexcept;
        [[nodiscard]] LocalInputUser& GameInput() noexcept { return m_pres.gameInput; }
        [[nodiscard]] const LocalInputUser& GameInput() const noexcept { return m_pres.gameInput; }
        [[nodiscard]] bool ConfigureGameInput(const InputActionAsset& asset, const Guid& projectId);
        void UpdateGameInput(double dt, const InputSnapshot& snapshot);
        void BeginGameInputFixedStep();

        // --- ImGui handoff (ABI v2) ---
        // The host installs its ImGui context + allocators here (once, after creating
        // the ImGuiLayer); PluginHost copies them into the EngineContext -- through
        // IClientHooks::FillEngineContext now -- so the plugin can adopt the host's
        // GImGui across the DLL boundary. Stored as void* to keep this header
        // imgui-include-free (ImGuiContext* / ImGuiMemAllocFunc / ...).
        // All null in a headless host (no ImGuiLayer) -> plugins skip the install.
        void  SetImGui(void* context, void* alloc, void* freeFn, void* userData) noexcept;
        void* ImGuiContext()  const noexcept;
        void* ImGuiAlloc()    const noexcept;
        void* ImGuiFree()     const noexcept;
        void* ImGuiUserData() const noexcept;

        // --- camera bridge: ONE ViewTransform (F4 plan 1). The plugin, the scene camera
        // or the editor pushes it; the render bridge, picking and the overlays read it.
        void                 SetView(const ViewTransform& view) noexcept;
        const ViewTransform& View() const noexcept;

        // --- render bridge: the host sets the live batcher each frame, IN this module ---
        // SetRenderContext writes RenderContext2D using the STORED view, so whoever
        // pushed the view (via SetView) owns the camera and the host stays camera-agnostic.
        void SetRenderContext(Batcher2D* batcher);

        // Publish the sprite-material resolution map (Guid -> Batcher2D material
        // id, owned by the host's SpriteMaterialCache) into the registry's
        // SpriteMaterialTable resource. Runs IN this module so the scene TypeID
        // resolves against the shared context (SetRenderContext's rule). Null
        // clears the table (sprites fall back to the plain pipeline).
        void SetSpriteMaterials(const std::unordered_map<Guid, std::uint16_t>* materials);

        // Publish the sprite-asset resolution map (.arcsprite Guid -> the
        // resolved texture/UVs/size/pivot record, owned by the host) into the
        // registry's SpriteTable resource. Same module rule and null semantics
        // as SetSpriteMaterials above: null clears the table, and every sprite
        // falls back to the untextured 1x1 m quad. `generation` is the owning
        // cache's publish counter (SpriteCache::Generation), carried on the
        // resource as SpriteTable::generation so BoundsSystem re-walks when an
        // ASSET changed under an unchanged entity; null = never changes.
        void SetSpriteTable(const std::unordered_map<Guid, SpriteEntry>* sprites,
                            const std::uint64_t* generation = nullptr);

        // F2a (Task 6) siblings of the two methods above, ONE dimension up:
        // .arcmesh Guid -> owned CPU geometry + bounds (MeshTable), and
        // "mesh"-kind .arcmat Guid -> constants-only baseColor
        // (MeshMaterialTable). Same module rule (SetSpriteTable's own
        // comment) and null semantics: null clears the table, and every
        // MeshRenderer draws nothing (there is no untextured-quad-shaped
        // fallback for a mesh -- see MeshTable's own comment,
        // Scene/SceneResources.hpp). `generation` as SetSpriteTable's:
        // MeshCache::Generation, carried as MeshTable::generation.
        void SetMeshTable(const std::unordered_map<Guid, MeshEntry>* meshes,
                          const std::uint64_t* generation = nullptr);
        void SetMeshMaterials(const std::unordered_map<Guid, ResolvedMeshMaterial>* materials);

        // --- headless aliases (plan 1 ruling P5) ---------------------------------
        // Pure forwarders so host call sites stay `m_runtime->X()`; the module still
        // gets ctx->engine (Core) separately. Nothing below adds behaviour -- read
        // each one's contract on Arcane::Runtime (Arcane/Base/Runtime.hpp).
        ::Arcane::Registry&       Registry()      noexcept { return m_core.Registry(); }
        SystemSchedulers&         Schedulers()    noexcept { return m_core.Schedulers(); }
        RunLoop&                  Loop()          noexcept { return m_core.Loop(); }
        ::Arcane::TypeContext*    TypeContext()   noexcept { return m_core.TypeContext(); }
        ::Arcane::IWorkScheduler* WorkScheduler() noexcept { return m_core.WorkScheduler(); }
        ITaskExecutor*            TaskExecutor()  noexcept { return m_core.TaskExecutor(); }
        JobSystem&                Jobs()          noexcept { return m_core.Jobs(); }
        std::shared_ptr<::Arcane::ComponentRegistry> Components() noexcept { return m_core.Components(); }
        Assets&                   AssetsFacade()  noexcept { return m_core.AssetsFacade(); }
        Config&                   Configuration() noexcept { return m_core.Configuration(); }

        bool OpenProject(const std::filesystem::path& pathOrFile,
                         AssetRegistry::ScanProgressFn onProgress = {},
                         ProjectOpenOptions opts = {}) { return m_core.OpenProject(pathOrFile, std::move(onProgress), std::move(opts)); }
        const Project* CurrentProject() const noexcept { return m_core.CurrentProject(); }
        void CloseProject() { m_pres.gameInput.Clear(); m_core.CloseProject(); }
        std::optional<Guid> RegisterCreatedAsset(const std::filesystem::path& file) { return m_core.RegisterCreatedAsset(file); }
        bool SetProjectBootScene(const Guid& id) { return m_core.SetProjectBootScene(id); }
        bool SetProjectInputActionsAsset(const Guid& id) { return m_core.SetProjectInputActionsAsset(id); }
        bool RestampProjectEngineAbi(int abi) { return m_core.RestampProjectEngineAbi(abi); }

        ::Arcane::Result<std::vector<std::byte>, ::Arcane::SerializationError> SnapshotRegistry() const { return m_core.SnapshotRegistry(); }
        bool RestoreRegistry(std::span<const std::byte> bytes) { return m_core.RestoreRegistry(bytes); }
        void ResetRegistry() { m_core.ResetRegistry(); }
        void ClearSystems() { m_core.ClearSystems(); }
        void InstallEngineSystems() { m_core.InstallEngineSystems(); }
        void EnsurePhysics() { m_core.EnsurePhysics(); }
        void PhysicsEditPass() { m_core.PhysicsEditPass(); }
        void ResetPhysics() { m_core.ResetPhysics(); }
        [[nodiscard]] glm::vec2 ResolvedGravity() const { return m_core.ResolvedGravity(); }

    private:
        friend struct ::Arcane::Detail::AbiManifest::ClientAccess;   // hashes m_core / m_pres (Client/AbiManifestClient.hpp)

        // IClientHooks -- Core's ONE reach-back. Private: only PluginHost (through
        // Runtime::ClientHooks()) is meant to call these, never a host directly.
        void* SaveUiContext() noexcept override;
        void  RestoreUiContext(void* saved) noexcept override;
        void  OnModuleTeardown() noexcept override;
        void  OnSystemsCleared() noexcept override;
        void  FillEngineContext(EngineContext& ctx) noexcept override;

        // The presentation's render-scheduler systems. Called at construction and
        // from OnSystemsCleared -- the presentation counterpart of
        // Runtime::InstallEngineSystems. Installs nothing since F5 plan 1 Task 5:
        // RenderSubmissionSystem runs inside ExtractFrame, once per frame.
        void InstallRenderSystems();

        // ORDER MATTERS: the presentation is declared SECOND so it destructs FIRST,
        // before the Runtime whose Assets facade its audio handles came from.
        Runtime             m_core;
        RuntimePresentation m_pres;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
