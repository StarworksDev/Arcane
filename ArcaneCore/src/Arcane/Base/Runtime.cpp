#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Base/Engine.hpp>   // ExecutablePathUtf8: the exe directory off-Windows

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Config/Bindings/AstraBinding.hpp>
#include <Arcane/Config/Bindings/JobsBinding.hpp>
#include <Arcane/Config/Bindings/Physics2DBinding.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Config.hpp>
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Platform/Paths.hpp>   // Arcane::Paths -- the engine dir and the open project are configured here (settings spec s11.0)
#include <Arcane/Plugin/ClientHooks.hpp>   // IClientHooks -- the ONE Core->Client reach-back (plan 1 P6)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectPaths.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>        // BoundsSystem (engine-owned, instantiated IN this module; F3 plan 1 T2)
#include <Arcane/Scene/Components.hpp>          // Transform / .../MeshRenderer (engine roster types)
#include <Arcane/Scene/EngineRoster.hpp>        // EngineComponentRoster -- THE roster list (registered below, verified in ProjectHost.hpp)
#include <Arcane/Scene/PhysicsComponents.hpp>   // Arcane::RigidBody2D/Arcane::Collider2D/Arcane::PhysicsBodyRef2D (engine roster types)
#include <Arcane/Scene/PhysicsSystem.hpp>       // Arcane::PhysicsSystem2D/Arcane::PhysicsWorld2D (instantiated IN this module)
#include <Arcane/Scene/SceneResources.hpp>   // SceneRoot (ResolvedGravity's scene-root lookup)
#include <Arcane/Scene/TransformSystems.hpp>    // TransformPropagationSystem (engine-owned, instantiated IN this module)
#include <Arcane/Serialization/RegistrySnapshot.hpp>
#include <Arcane/Serialization/ResourceSerialization.hpp>
#include <Arcane/Sim/NetDriver.hpp>   // INetDriver::IsActive (the hot-reload refusal asks it)
#include <Arcane/Sim/SimSettings.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/WorkScheduler.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <cmath>       // Runtime::SetFixedHz refuses a non-finite rate
#include <exception>   // ~Runtime / ReleaseProjectCVarLayers guard the release (S4-GATE)
#include <mutex>      // the process-wide snapshot SaveConfig (S6-45)
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // Register the named roster into `m`. The list itself lives in
        // Scene/EngineRoster.hpp so this registration and ProjectHost.hpp's
        // VerifySharedTypeContext expand the SAME pack and cannot drift.
        template<typename... Ts>
        void RegisterRoster(Astra::ComponentModule& m, TypeList<Ts...>)
        {
            m.Register<Ts...>();
        }

        // Directory of the running executable, so exe-relative engine assets (here the
        // shipped data/EngineConfig defaults) resolve regardless of CWD -- mirrors the
        // exe-relative pattern in Assets.cpp / Render/ShaderPaths.cpp.
        std::filesystem::path ExeDir()
        {
#ifdef _WIN32
            wchar_t buf[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, buf, MAX_PATH) != 0)
                return std::filesystem::path(buf).parent_path();
#else
            if (const std::string self = ExecutablePathUtf8(); !self.empty())
                return std::filesystem::path(self).parent_path();
#endif
            return std::filesystem::current_path();
        }

        // The ONE engine-config folder (S2-H): Arcane::Paths names the engine
        // dir once per process (settings spec s11.0) -- the exe dir, where the
        // shipped data/EngineConfig defaults sit, unless a host already set
        // one. The cold-boot EngineConfig rung, the JSON Config layer and
        // CVarLayerSources (what a module reload re-layers) all read this
        // folder, so a host-configured engine dir is honoured by all three.
        // A Dist build also sets Paths' dist here, host engine dir or not
        // (ApplyEngineDirDefaults), so HostBoot's early User rung already
        // resolves GameUserDir under the OS per-user dir (settings S7).
        std::filesystem::path EngineConfigDir()
        {
            Paths::Config paths = Paths::Current();
            if (ApplyEngineDirDefaults(paths, ExeDir(), kDistBuild))
                Paths::Configure(paths);
            return Paths::Get(Paths::Location::EngineConfig);
        }

        // Whether Paths resolves user data the Dist way: always in a Dist
        // build; in a dev build only when a host or test preset it. A project
        // open, switch or close keeps that preset, as the EngineConfig rung
        // does (ApplyEngineDirDefaults), so one boot never splits the User
        // rung between the per-user OS dir and Saved/Config (S7-GATE).
        bool DistPaths()
        {
            return kDistBuild || Paths::Current().dist;
        }

        // The folder the EngineConfig rung was last applied from, by
        // ApplyEngineConfigRung (HostBoot's early rungs or a Runtime ctor).
        // Main thread only.
        std::filesystem::path g_engineRungDir;

        // The EngineConfig cvar rung, applied BEFORE Impl reads any setting
        // (settings arc S2). JobSystem's size (jobs.workerThreads, Restart) and
        // the first registry's Astra config (astra.memory.*, NextWorld) are read
        // while Impl constructs, so the engine-config values must already be
        // published. Returns the JobSystem ctor argument.
        // Once per process (S2-H): ApplyEngineConfigRung layers the folder
        // only when it is not the one already applied -- by HostBoot's early
        // rungs or an earlier Runtime -- so the first Runtime after HostBoot,
        // the editor's embedded server and a PIE world neither re-read the
        // folder nor publish. A module that (re)loads still gets the rung
        // through ApplyLayersFor (CVarLayerSources names the same folder).
        std::uint32_t ApplyEngineRungAndResolveWorkers()
        {
            if (ApplyEngineConfigRung())
                CVarRegistry::Get().PublishImmediate();   // asserts the main thread (settings spec s4.6)
            return ResolveWorkerThreads(Settings<JobsSettings>());
        }

        // The process's project state -- Arcane::Paths' project (settings spec
        // s11.0) and the User, Project and Plugin cvar rungs on the
        // process-wide registry (s4.5) -- follows ONE Runtime (S2-H). The
        // first Runtime to open a project owns it until it closes it or dies;
        // a project switch keeps the owner. Every host opens its project in
        // exactly one Runtime today (the editor's, ProjectBoot's, ServerApp's;
        // the embedded server and PIE worlds never open one), so this only
        // decides what a second Runtime that does open one may touch: nothing
        // process-wide. Its OpenProject neither configures Paths nor layers
        // its rungs over the owner's, and its CloseProject or destruction
        // releases nothing of the owner's. The Impl address is the identity.
        // Touched only by OpenProject, CloseProject and ~Runtime, which a host
        // already serializes (ProjectBoot's project_open stage runs on a boot
        // worker, ordered after runtime_create and before any close).
        const void* g_projectOwner = nullptr;

        // Paths follows the OWNER's project. It is cleared only when the
        // project being dropped is still the configured one (a host may have
        // re-pointed Paths itself).
        void ForgetProjectPaths(const std::filesystem::path& root)
        {
            Paths::Config paths = Paths::Current();
            if (!paths.projectDir || *paths.projectDir != root) return;
            Paths::Configure(PathsConfigWithoutProject(paths.engineDir, DistPaths()));
        }

        // ASCII name for a SerializationError so a Save failure logs a readable
        // cause rather than an opaque integer (the enum has no library to_string).
        const char* SerializationErrorName(Astra::SerializationError e) noexcept
        {
            switch (e)
            {
                case Astra::SerializationError::None:               return "None";
                case Astra::SerializationError::InvalidMagic:       return "InvalidMagic";
                case Astra::SerializationError::UnsupportedVersion: return "UnsupportedVersion";
                case Astra::SerializationError::CorruptedData:      return "CorruptedData";
                case Astra::SerializationError::UnknownComponent:   return "UnknownComponent";
                case Astra::SerializationError::SizeMismatch:       return "SizeMismatch";
                case Astra::SerializationError::EndiannessMismatch: return "EndiannessMismatch";
                case Astra::SerializationError::ChecksumMismatch:   return "ChecksumMismatch";
                case Astra::SerializationError::IOError:            return "IOError";
                case Astra::SerializationError::OutOfMemory:        return "OutOfMemory";
            }
            return "Unknown";
        }
    }

    bool ApplyEngineConfigRung()
    {
        const std::filesystem::path dir = EngineConfigDir();
        if (dir == g_engineRungDir)
            return false;
        CVarRegistry& cvars = CVarRegistry::Get();
        if (!g_engineRungDir.empty())
            cvars.RevertLayer(SetBy::EngineConfig);   // the previous folder's records leave with it
        ApplyCVarDirectory(cvars, dir, SetBy::EngineConfig, "engine-config");
        g_engineRungDir = dir;
        return true;
    }

    struct Runtime::Impl
    {
        JobSystem                                   jobs;
        std::shared_ptr<Mosaic::IWorkScheduler>     sched;
        Astra::TypeContext*                         context = nullptr;   // the ProcessContext's -- Runtime owns nothing
        std::shared_ptr<Astra::ComponentRegistry>   components;
        std::optional<Astra::ComponentModule>       engineModule;   // the engine roster's RAII owner; destructs AFTER registry (declared before it), BEFORE components
        std::unique_ptr<Astra::Registry>            registry;
        std::unique_ptr<SystemSchedulers>           schedulers;
        std::unique_ptr<RunLoop>                    loop;
        RunLoop::Config                             loopCfg;   // reused by Restore/ResetRegistry when rebuilding the loop
        std::unique_ptr<Assets>                     assets;
        Config                                      config;          // layered engine+project config (Slice 3)
        std::filesystem::path                       engineConfigDir; // EngineConfigDir(): <engine dir>/data/EngineConfig
        std::optional<Project>                      project;   // open project (Slice 1b); empty = none
        bool                                        archiveUserCVars = false;   // SetUserCVarArchiving (T3-D2)
        std::filesystem::path                       editorUserConfigDir; // settings arc S2; empty = no EditorUser rung
        std::vector<std::string>                    cvarCommandLine;                                   // SetCVarCommandLine
        CVarContext                                 cvarCommandLineContext = CVarContext::Editor;
        // The client seam (spec s2, plan 1 P6). Both null on a headless host; a
        // ClientRuntime sets them from its own ctor and clears them from its dtor,
        // so neither can outlive the object it points at.
        ClientRuntime*                              client = nullptr;
        IClientHooks*                               hooks  = nullptr;
        // The network role of THIS world + the process object whose factory table it
        // instantiates from (spec s4). `net` is the replication driver, non-owning.
        ProcessContext*                             process = nullptr;
        NetMode                                     mode    = NetMode::Standalone;
        INetDriver*                                 net     = nullptr;

        // True when THIS Runtime's open project is the one the process's
        // Paths and project cvar rungs follow (g_projectOwner, S2-H).
        bool OwnsProject() const noexcept { return project && g_projectOwner == this; }
        // CloseProject's and ~Runtime's one release path; defined below,
        // beside the rung helpers it calls.
        void ReleaseProject();

        // `sharedComponents` non-null = a SECONDARY world built on the PRIMARY's
        // ComponentRegistry (spec s4; Runtime.hpp's three-argument ctor explains
        // why a per-world registry would break every module-defined type).
        Impl(ProcessContext& proc, NetMode netMode, std::shared_ptr<Astra::ComponentRegistry> sharedComponents)
            : jobs(ApplyEngineRungAndResolveWorkers()), sched(jobs.WorkScheduler()), process(&proc), mode(netMode)
        {
            context = &proc.TypeContext();

            // Install the shared context in THIS module (ArcaneCore.dll) BEFORE any
            // TypeID/Registry use. ProcessContext::Create (Base/ProcessContext.cpp)
            // already installed exactly this slot -- it is Core's TU too -- so this
            // call is idempotent and kept only so the invariant is stated where the
            // Registry work begins. ArcaneClient.dll's own slot is ClientRuntime's
            // ctor's (Client/ClientRuntime.cpp).
            Astra::SetTypeContext(context, Astra::ModuleResidency::Resident);
            components = sharedComponents ? std::move(sharedComponents)
                                          : std::make_shared<Astra::ComponentRegistry>();

            // The engine's OWN component roster, registered here so every host
            // has it before any plugin loads. Previously nothing registered it
            // outside tests: the live roster was whatever the hosted game
            // plugin happened to ReRegisterComponent<T>() in its Init, which
            // (a) left the editor's Add Component catalog offering only the
            // plugin's handful of types, and (b) silently DROPPED Identity /
            // Hidden when a runtime host loaded an editor-saved scene --
            // SceneSerializer skips a type that is reflected but not
            // REGISTERED as a component.
            //
            // Plugins register only the types they themselves implement, through
            // their own RAII Astra::ComponentModule (PluginHost.cpp /
            // HotReloadPlugin.cpp). The engine's roster is module-owned too, since
            // 2026-09-11 (spec docs/specs/2026-09-11-astra-adoption-design.md s5),
            // through the Runtime-held handle below -- which REVERSES the
            // 2026-08-10 ratification that kept it anonymous. That ratification
            // rested on ComponentModule.hpp's old "one registry per context"
            // caveat: ReleaseModule erased a type's TypeMeta from the SHARED
            // TypeContext whenever the releasing registry was its sole owner, so
            // the test suite's many short-lived Runtimes (73 construction sites)
            // wiped the metas out from under each other, and retiring the handle
            // into a longer-lived container traded that for a static-destruction
            // crash at exit. The 2026-09-10 binder-stack vendor (ABI v24) removed
            // the caveat -- several registries per context is now the supported
            // shape -- and the residency declaration at SetTypeContext above is
            // what makes the module-owned roster SAFE here: Arcane.dll never
            // unmaps, so its binders are PINNED and every Reset reports Retained;
            // registry-less GetMeta keeps resolving after the last Runtime dies
            // (pinned by RuntimeTest.cpp's "[residency]" case). No plugin-side
            // static is involved: Impl is pimpl-held and reset from ~Runtime.
            //
            // A plugin's InstallOwned shadows whatever is live for an id,
            // module-owned or not, and its own unload restores the shadow -- the
            // "plugin overrides it, unload restores it" behaviour is unchanged.
            // RegisterSceneComponents / Arcane::RegisterPhysicsComponents2D survive for the
            // tests that register on bare registries; Runtime no longer calls them.
            //
            // ComponentID NUMBERING (corrected 2026-07-26 -- the previous comment
            // here claimed ids are resolved BY HASH and therefore order-
            // independent, which is FALSE): the type HASH is only the lookup key.
            // The id itself is a monotonic per-process counter assigned in
            // first-touch order -- TypeContext.hpp:93, `const ComponentID id =
            // m_next++`. Registration order fully determines numbering, and
            // registering the engine roster here DOES shift it (a plugin that
            // registered Transform + SpriteRenderer used to get 0,1; it now gets
            // 0,2). Ids are process-local; only the hash is stable across
            // processes. (2026-09-11: the roster below dropped its per-entity
            // previous-pose slot -- Astra adoption plan 2 deleted the component
            // end to end -- so every id after it shifted down by one; the
            // worked example above already reflects the post-drop numbering.)
            //
            // A SECONDARY world sharing the primary's registry opens its OWN roster
            // handle here too, and that is deliberate, not an oversight. VERIFIED
            // against the vendored Astra: ComponentRegistry::OpenModuleId always
            // mints a fresh owner id, so the second handle's Register<Ts...> hits
            // InstallOwned's "live owner != owner" case (ComponentRegistry.hpp
            // semantics 4) -- the primary's identical entry is PUSHED onto the id's
            // shadow stack and one meta ref is acquired on the same binder. Both
            // descriptors come from ArcaneCore.dll, which never unmaps, so the
            // shadowed copy can never dangle; and ReleaseModule drops "this owner's
            // SHADOWED entries wherever they sit" and otherwise restores the newest
            // shadow, so the two handles may Reset in EITHER order (a secondary
            // outliving its primary included). Benign, and it keeps every Runtime's
            // teardown symmetric -- which is worth more than saving one shadow slot.
            engineModule.emplace(Astra::ComponentModule::Open(components, "Arcane"));
            ARC_ASSERT(*engineModule, "Runtime: ComponentModule::Open refused -- the slot above must be installed first");
            // The roster -- and the ORDER that is the id numbering -- is named
            // once, in Scene/EngineRoster.hpp, because ProjectHost.hpp's
            // VerifySharedTypeContext must check the same thirteen types this
            // registers (2026-09-16: a one-type probe missed the editor's
            // early WorldTransform resolve). Ids are a first-touch counter, so
            // same order == same numbering as before.
            RegisterRoster(*engineModule, EngineComponentRoster{});

            registry   = std::make_unique<Astra::Registry>(components, ToAstraConfig(Settings<AstraMemorySettings>(), sched));
            schedulers = std::make_unique<SystemSchedulers>(sched);
            // sim.fixedHz (settings arc S2, NextWorld): this Runtime's step,
            // read once. InstallEngineSystems and PhysicsEditPass read loopCfg.
            // Restore/ResetRegistry REBIND this loop, so a changed fixedHz
            // waits for the next Runtime. sim.maxStepsPerFrame (S6-8) is Live:
            // read here for the first frame, then each frame by the hosts
            // (ApplySimStepCap).
            loopCfg.fixedHz          = Settings<SimSettings>().fixedHz;
            loopCfg.maxStepsPerFrame = Settings<SimSettings>().maxStepsPerFrame;
            loop       = std::make_unique<RunLoop>(*registry, *schedulers, loopCfg);

            // assets.cache.byteBudget (Restart): read once, here, after the
            // early config rungs.
            assets = Assets::Create(AssetsDesc{ .byteBudget = Settings<AssetsCacheSettings>().byteBudget });
            // Engine-default config layer (shipped beside the exe). A host with no
            // project still gets this base (e.g. input bindings for bare ArcaneRuntime);
            // OpenProject re-layers the project + user files on top.
            // The EngineConfig cvar rung was applied at the top of Impl (before
            // JobSystem and the first registry) from this same folder
            // (EngineConfigDir), so only the JSON Config layer remains here.
            engineConfigDir = EngineConfigDir();
            config.LoadEngineDefaults(engineConfigDir);
            // The audio device that used to be initialized here is ClientRuntime's
            // (its RuntimePresentation member, initialized from its own ctor with
            // the enableAudioDevice flag that moved there with it).
        }
    };

    Runtime::Runtime(ProcessContext& process, NetMode mode)
        : Runtime(process, mode, nullptr) {}

    Runtime::Runtime(ProcessContext& process, NetMode mode,
                     std::shared_ptr<Astra::ComponentRegistry> sharedComponents)
        : m_impl(std::make_unique<Impl>(process, mode, std::move(sharedComponents)))
    {
        // Mosaic diagnostics: install the log sink + assert handler into THIS module
        // (ArcaneCore.dll) so Astra/Manifold2D/Mosaic code running here routes to the
        // engine logger. Each module installs its own (per-module Mosaic storage);
        // ClientRuntime's ctor does the same for ArcaneClient.dll.
        Arcane::Log::InstallMosaicSink();
        Arcane::Assert::InstallMosaicHandler();
        InstallEngineSystems();
        // The 2D physics per-frame event window clears at frame begin (spec
        // 2026-10-08 s7.2): every reader (Update, OnUpdate, render) ran before.
        m_impl->loop->SetFrameBeginHook([](Astra::Registry& reg)
        {
            if (Arcane::PhysicsWorld2D* res = reg.GetResource<Arcane::PhysicsWorld2D>()) res->BeginFrame();
        });
        // ...and then whatever the LOADED module already registered, for THIS mode:
        // a Runtime built after the module loaded (the editor's embedded server
        // world, a second PIE world) must not come up system-less. Empty table when
        // no module is loaded -- the common case -- so this costs nothing.
        InstantiateModuleSystems();
    }
    Runtime::~Runtime()   // do not reset the module slot: a later Runtime re-installs
    {
        // Dying with its project open releases it exactly as CloseProject
        // does (S2-H): the User layer archived when this host archives, the
        // project rungs dropped and published, Paths' project forgotten -- so
        // the next Runtime starts from the files alone.
        if (!m_impl) return;
        // A destructor is noexcept: an exception out of the release (a cvar
        // callback during Publish, say) would std::terminate the host at exit.
        // Log it and let the rest of teardown run (S4-GATE).
        try
        {
            m_impl->ReleaseProject();
        }
        catch (const std::exception& e)
        {
            ARC_ERROR("Runtime: releasing the project at teardown threw ({}) -- teardown continues", e.what());
        }
        catch (...)
        {
            ARC_ERROR("Runtime: releasing the project at teardown threw a non-standard exception -- teardown continues");
        }
    }

    ProcessContext& Runtime::Process()      noexcept { return *m_impl->process; }
    NetMode         Runtime::Mode()   const noexcept { return m_impl->mode; }
    bool            Runtime::HasAuthority() const noexcept { return m_impl->mode != NetMode::Client; }

    void Runtime::SetNetDriver(INetDriver* d) noexcept { m_impl->net = d; }
    INetDriver* Runtime::NetDriver() const noexcept { return m_impl->net; }

    std::size_t Runtime::InstantiateModuleSystems()
    {
        return m_impl->process->SystemFactories().InstantiateInto(*m_impl->schedulers, m_impl->mode);
    }

    void Runtime::SetNetMode(NetMode m)
    {
        if (m == m_impl->mode)
            return;
        ARC_INFO("Runtime: net mode {} -> {}", ToString(m_impl->mode), ToString(m));
        m_impl->mode = m;
        // ClearSystems reinstalls the engine pair and fires OnSystemsCleared (the
        // client's presentation systems); the module's systems then come back for
        // the NEW mode only -- which is the whole point of the re-role.
        ClearSystems();
        InstantiateModuleSystems();
    }

    void Runtime::AttachClient(ClientRuntime* client, IClientHooks* hooks) noexcept
    {
        m_impl->client = client;
        m_impl->hooks  = hooks;
    }
    ClientRuntime* Runtime::Client()      const noexcept { return m_impl->client; }
    IClientHooks*  Runtime::ClientHooks() const noexcept { return m_impl->hooks; }

    Astra::Registry&  Runtime::Registry()   noexcept { return *m_impl->registry; }
    SystemSchedulers& Runtime::Schedulers() noexcept { return *m_impl->schedulers; }
    RunLoop&          Runtime::Loop()       noexcept { return *m_impl->loop; }

    void Runtime::SetFixedHz(double hz)
    {
        if (!(hz > 0.0) || !std::isfinite(hz)) return;
        // loopCfg is what InstallEngineSystems (a ClearSystems reinstall) and
        // PhysicsEditPass read, so every later step agrees with the loop.
        m_impl->loopCfg.fixedHz = hz;
        m_impl->loop->SetFixedHz(hz);
        // Arcane::PhysicsSystem2D captured its dt at AddSystem: re-add it at the new
        // rate (its Before<TransformPropagationSystem> keeps it ordered).
        m_impl->schedulers->fixedUpdate.RemoveSystem<Arcane::PhysicsSystem2D>();
        InstallEngineSystems();
    }

    Astra::TypeContext*    Runtime::TypeContext()   noexcept { return m_impl->context; }
    Mosaic::IWorkScheduler* Runtime::WorkScheduler() noexcept { return m_impl->sched.get(); }
    ITaskExecutor*         Runtime::TaskExecutor()  noexcept { return m_impl->jobs.TaskExecutor(); }
    JobSystem&             Runtime::Jobs()          noexcept { return m_impl->jobs; }
    std::shared_ptr<Astra::ComponentRegistry> Runtime::Components() noexcept { return m_impl->components; }
    Assets& Runtime::AssetsFacade() noexcept { return *m_impl->assets; }
    // The presentation surface -- AudioSystem/SetInputSnapshot/Input/SetImGui/
    // ImGui*/SetView/View (SetCamera/CameraOffset/CameraZoom until ABI v32)/SetRenderContext/SetSpriteMaterials/
    // SetSpriteTable/SetMeshTable/SetMeshMaterials/ResetAudio -- stood here. It moved
    // to ClientRuntime (Client/ClientRuntime.cpp) BODY-FOR-BODY at the Core-DLL split
    // (plan 1 Task 4): every SetResource call still runs in ArcaneClient.dll, whose
    // Astra slot that ctor installs, so the "scene TypeID resolves against the shared
    // context" rule each of them documented is unchanged.
    //
    // SetRenderResources / Device() / Shaders() stood here too. The setter also bound
    // the device into the Assets facade (Assets::SetDevice) so GetTexture could
    // resolve a texture; both hosts have passed nullptr since Task 6, so that
    // bind has been a no-op and the facade stays device-less for its whole life.
    // Deleted at Task 9 -- ABI 14.

    namespace
    {
        std::mutex& SnapshotSaveConfigMutex()
        {
            static std::mutex m;
            return m;
        }
        Astra::Registry::SaveConfig& SnapshotSaveConfigSlot()
        {
            static Astra::Registry::SaveConfig config{};   // Astra's default until the editor pushes its setting
            return config;
        }
    }

    void Runtime::SetSnapshotSaveConfig(const Astra::Registry::SaveConfig& config)
    {
        std::lock_guard lock(SnapshotSaveConfigMutex());
        SnapshotSaveConfigSlot() = config;
    }

    Astra::Registry::SaveConfig Runtime::SnapshotSaveConfig()
    {
        std::lock_guard lock(SnapshotSaveConfigMutex());
        return SnapshotSaveConfigSlot();
    }

    Astra::Result<std::vector<std::byte>, Astra::SerializationError> Runtime::SnapshotRegistry() const
    {
        // A real Save failure must be named at its source: an empty-but-"ok"
        // snapshot would resurface much later as a generic "reload lost state"
        // with the root cause erased. FinishSnapshot propagates the exact
        // SerializationError; log it here so the hot-reload path names the cause.
        // The Save configuration is the process-wide one (SetSnapshotSaveConfig:
        // the editor's astra.snapshot.compression, S6-45).
        auto save = Serialization::FinishSnapshot(m_impl->registry->Save(SnapshotSaveConfig()));
        if (save.IsErr())
        {
            const Astra::SerializationError err =
                save.GetError() ? *save.GetError() : Astra::SerializationError::None;
            ARC_ERROR("Runtime: SnapshotRegistry: registry Save failed ({})",
                      SerializationErrorName(err));
            return save;
        }

        // Astra's Save carries resources only as their binary Serialize (and
        // skips the transient ones -- Time, GameInput, the physics pair); frame
        // the engine's serializable-resource section (SceneRoot + any
        // host/test-registered types) alongside the blob.
        std::vector<std::byte> section =
            Serialization::WriteResourceSection(*m_impl->registry, Serialization::SerializableResources());
        return Serialization::SnapshotResult::Ok(
            Serialization::FrameBytes(*save.GetValue(), section));
    }

    bool Runtime::RestoreRegistry(std::span<const std::byte> bytes)
    {
        // Split the frame into the registry blob + resource section. Transactional:
        // load into a local registry, apply resources, and only swap the live
        // registry + rebind the loop on FULL success, so a corrupt frame leaves
        // the running world untouched.
        auto frame = Serialization::ParseSnapshot(bytes);
        if (frame.IsErr())
            return false;

        auto r = Astra::Registry::Load(frame.GetValue()->registry, m_impl->components,
                                       ToAstraConfig(Settings<AstraMemorySettings>(), m_impl->sched));   // 3.3 Config overload
        if (r.IsErr())
            return false;
        std::unique_ptr<Astra::Registry> loaded = std::move(*r.GetValue());

        auto resources = Serialization::ReadResourceSection(
            *loaded, frame.GetValue()->resources, Serialization::SerializableResources());
        if (resources.IsErr())
            return false;

        // No strip needed: Arcane::PhysicsWorld2D/Arcane::PhysicsInterpBuffer2D (like Time and
        // GameInput) are AstraTransientResource, so the blob never carried them
        // and the loaded registry has none -- the next EnsurePhysics mints both
        // fresh, the way propagation re-derives WorldTransform. (They used to be
        // stripped by hand here, IN-8.)

        m_impl->registry = std::move(loaded);
        // Rebind the EXISTING loop to the swapped registry rather than recreating it:
        // a cached RunLoop* (a plugin that stored Loop() at init, a host toolbar) must
        // not dangle across a restore. Same observable loop state as a fresh loop.
        m_impl->loop->Rebind(*m_impl->registry);
        return true;
    }

    void Runtime::ResetRegistry()
    {
        // Fresh-boot reload: replace the registry with an empty one (same shared
        // ComponentRegistry + scheduler) so the plugin's Init rebuilds its scene.
        m_impl->registry = std::make_unique<Astra::Registry>(m_impl->components,
                                                             ToAstraConfig(Settings<AstraMemorySettings>(), m_impl->sched));
        // Rebind the existing loop (keep the object stable so cached RunLoop* holders
        // do not dangle) -- see RestoreRegistry.
        m_impl->loop->Rebind(*m_impl->registry);
    }

    void Runtime::ClearSystems()
    {
        m_impl->schedulers->fixedUpdate.Clear();
        m_impl->schedulers->update.Clear();
        m_impl->schedulers->render.Clear();
        InstallEngineSystems();   // the module's systems are gone; the engine's are back
        // ...and the client's presentation systems with them: RenderSubmissionSystem
        // is ClientRuntime's to (re)install, and this is the ONE place that knows a
        // clear just happened. Null on a headless host -- nothing to reinstall.
        if (m_impl->hooks)
            m_impl->hooks->OnSystemsCleared();
    }

    void Runtime::InstallEngineSystems()
    {
        // The engine's HEADLESS trio, owned here (spec docs/specs/2026-09-13-
        // game-module-boilerplate-design.md s4.1) -- the UE/DOTS shape: the engine
        // ticks the world; a game module registers only its own systems and
        // places them with Astra::Before/After against these types. Each behind
        // its own HasSystem guard: AlreadyRegistered is the only failure and
        // this runs from the ctor AND after every ClearSystems. Order within a
        // scheduler: Arcane::PhysicsSystem2D declares Before<TransformPropagationSystem>,
        // BoundsSystem declares After<TransformPropagationSystem> (F3 plan 1 T2:
        // it reads the composed WorldTransform); insertion order carries the
        // rest (Astra's reorder is stable).
        // RenderSubmissionSystem was the third; it is presentation, so it is
        // ClientRuntime's now (Client/ClientRuntime.cpp installs it at
        // construction and on every OnSystemsCleared) and a Core-only host has
        // exactly the systems it can execute.
        auto& fixed  = m_impl->schedulers->fixedUpdate;
        if (!fixed.HasSystem<Arcane::PhysicsSystem2D>())
        {
            const float fixedDt = static_cast<float>(1.0 / m_impl->loopCfg.fixedHz);
            std::ignore = fixed.AddSystem<Arcane::PhysicsSystem2D>(fixedDt, /*stepWorld*/ true);
        }
        if (!fixed.HasSystem<TransformPropagationSystem>())
            std::ignore = fixed.AddSystem<TransformPropagationSystem>();
        if (!fixed.HasSystem<BoundsSystem>())
            std::ignore = fixed.AddSystem<BoundsSystem>();
    }

    glm::vec2 Runtime::ResolvedGravity() const
    {
        const CVarVec2 p = Settings<Arcane::PhysicsWorldSettings2D>().gravity;
        glm::vec2 g{p.x, p.y};
        if (const SceneRoot* sr = m_impl->registry->GetResource<SceneRoot>())
            if (const Arcane::PhysicsSettings2D* ps = std::as_const(*m_impl->registry).GetComponent<Arcane::PhysicsSettings2D>(sr->entity))
                g = ps->gravity;
        return g;
    }

    void Runtime::EnsurePhysics()
    {
        Astra::Registry& reg = *m_impl->registry;
        const glm::vec2 g = ResolvedGravity();
        Arcane::PhysicsWorld2D* res = reg.GetResource<Arcane::PhysicsWorld2D>();
        if (res && Arcane::Detail::Physics2D::Access::Solver(*res))
        {
            const auto cur = Arcane::Detail::Physics2D::Access::Solver(*res)->Gravity();
            if (static_cast<float>(cur.x) == g.x && static_cast<float>(cur.y) == g.y)
                return;
            // Gravity changed: replace the world. Bodies re-mint from their
            // current Transforms on the next pass (PASS 1 sees every handle
            // invalid against the new world; PASS 2 self-heals).
        }
        // physics.* (settings arc S2): NextWorld -- read here, where a world is minted.
        const Arcane::PhysicsWorldSettings2D& settings = Settings<Arcane::PhysicsWorldSettings2D>();
        Manifold2D::Physics::WorldDef wd = Arcane::Detail::Physics2D::ToWorldDef(settings);
        wd.gravityX = g.x;
        wd.gravityY = g.y;
        auto world = std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd);
        // physics.parallelSolver (default OFF: the serial solver, bit-identical
        // to before). The pool is this Runtime's JobSystem, which outlives every
        // registry (Impl declares it first).
        if (settings.parallelSolver)
            world->SetExecutor(m_impl->sched.get());
        reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::move(world)));
        reg.SetResource(Arcane::PhysicsInterpBuffer2D{});
    }

    void Runtime::PhysicsEditPass()
    {
        const float fixedDt = static_cast<float>(1.0 / m_impl->loopCfg.fixedHz);
        Arcane::PhysicsSystem2D{ fixedDt, /*stepWorld*/ false }(*m_impl->registry);
    }

    void Runtime::ResetPhysics()
    {
        // Drop the transient physics pair on the LIVE registry (a restore never
        // carries it either): the next EnsurePhysics sees neither and mints both. PASS 2 then
        // re-mints every body from its components -- and PASS 1/2 clear any
        // Arcane::PhysicsBodyRef2D the fresh world does not track, so nothing here can
        // leave a handle behind for the fresh world to reissue to someone else.
        m_impl->registry->RemoveResource<Arcane::PhysicsWorld2D>();
        m_impl->registry->RemoveResource<Arcane::PhysicsInterpBuffer2D>();
    }

    namespace
    {
        // The cvar User layer's home: read by OpenProject, written back by
        // the archive (T3-D2). One definition, so the two can never disagree.
        // Resolved through Arcane::Paths (settings spec s11.1): <project>/Saved/
        // Config in dev, byte-identical to before; the per-user OS dir in Dist.
        // The project's own identity resolves it (PathsConfigFor, settings S7),
        // not whatever project Paths is configured for, so the outgoing project
        // of a switch archives to its own folder. Empty when the location is.
        std::filesystem::path UserCVarDir(const Project& project)
        {
            return Paths::Join(Paths::Location::GameUserDir,
                               PathsConfigFor(project, Paths::Current().engineDir, DistPaths()), "Config");
        }

        // The cvar Project layer's home, resolved through Arcane::Paths the same
        // way (settings spec s11.0): <project>/Config. One definition shared by
        // OpenProject's LayerProject and CVarLayerSources, so the two can never
        // disagree (lane P merge, S1-29 x S1-32).
        std::filesystem::path ProjectCVarDir(const Project& project)
        {
            return Paths::Resolve(Paths::Location::ProjectConfig, Paths::ForProject(project.Root()));
        }

        // The outgoing project's rungs leave with it (settings spec s4.5): its
        // User layer (archived first when this host archives, T3-D2), its
        // Project layer and its plugins' layers, so the next project starts from
        // its own files and never inherits a key only the old one set.
        // The machine-wide EditorUser layer is written too when this host
        // archives, but it STAYS (settings arc S2).
        void ReleaseProjectCVarLayers(const Project& outgoing, bool archive, const std::filesystem::path& editorUserDir)
        {
            CVarRegistry& cvars = CVarRegistry::Get();
            if (archive)
            {
                // A failed archive write must not leave the layers half released
                // (S4-GATE): nlohmann::json::dump throws on a string value that is
                // not UTF-8, and this also runs from ~Runtime. One guard PER
                // RUNG (S6-GATE): a project User value that cannot be written
                // must not cost the machine-wide EditorUser edits too.
                // An empty User dir (Dist with no per-user OS base) archives
                // nothing (settings S7).
                const std::filesystem::path userDir = UserCVarDir(outgoing);
                if (!userDir.empty())
                {
                    try
                    {
                        WriteCVarArchive(cvars, userDir);
                    }
                    catch (const std::exception& e)
                    {
                        ARC_ERROR("cvar: archiving the user settings of '{}' failed ({}) -- unsaved edits are lost; the project still closes",
                                  outgoing.Manifest().name, e.what());
                    }
                }
                if (!editorUserDir.empty())
                {
                    try
                    {
                        WriteCVarArchive(cvars, editorUserDir, SetBy::EditorUser);
                    }
                    catch (const std::exception& e)
                    {
                        ARC_ERROR("cvar: archiving the editor-wide settings to '{}' failed ({}) -- unsaved editor edits are lost",
                                  editorUserDir.string(), e.what());
                    }
                }
            }
            cvars.RevertLayer(SetBy::User);
            cvars.RevertLayer(SetBy::Project);
            cvars.RevertLayer(SetBy::Plugin);
        }

        // The EditorUser layer from disk, dropped first so a key removed from a
        // file does not linger.
        void ReapplyEditorUserLayer(const std::filesystem::path& dir)
        {
            CVarRegistry& cvars = CVarRegistry::Get();
            cvars.RevertLayer(SetBy::EditorUser);
            if (dir.empty()) return;
            ApplyCVarDirectory(cvars, dir, SetBy::EditorUser, "editor-user");
        }
    }

    void Runtime::Impl::ReleaseProject()
    {
        if (!OwnsProject()) return;   // a second Runtime's project: nothing process-wide is its to release
        // The project's paths and ownership are dropped however the release
        // exits (S6-GATE): a cvar callback throwing out of Publish must not
        // leave a destroyed Runtime as g_projectOwner, or the next Runtime
        // would layer nothing for its own project.
        struct DropOwnership
        {
            Impl& impl;
            ~DropOwnership()
            {
                ForgetProjectPaths(impl.project->Root());
                g_projectOwner = nullptr;
            }
        } drop{ *this };
        ReleaseProjectCVarLayers(*project, archiveUserCVars, editorUserConfigDir);
        CVarRegistry::Get().Publish();
    }

    LayerSources Runtime::CVarLayerSources() const
    {
        LayerSources layers;
        layers.dirs.push_back(CVarLayerDir{ SetBy::EngineConfig, m_impl->engineConfigDir, "engine-config" });
        // The project's rungs only for the Runtime that owns the process's
        // project state (S2-H): a second Runtime's module reload must not
        // layer its project over the owner's.
        if (m_impl->OwnsProject())
        {
            for (const auto& pluginRoot : m_impl->project->ActivePluginRoots())
                layers.dirs.push_back(CVarLayerDir{ SetBy::Plugin, pluginRoot / "Config", pluginRoot.filename().string() });
            layers.dirs.push_back(CVarLayerDir{ SetBy::Project, ProjectCVarDir(*m_impl->project), "project" });
        }
        // EditorUser sits between Project and User and survives a missing
        // project (the start page is themed too). Empty = no EditorUser rung.
        if (!m_impl->editorUserConfigDir.empty())
            layers.dirs.push_back(CVarLayerDir{ SetBy::EditorUser, m_impl->editorUserConfigDir, "editor-user" });
        if (m_impl->OwnsProject())
            layers.dirs.push_back(CVarLayerDir{ SetBy::User, UserCVarDir(*m_impl->project), "user" });
        layers.commandLine = m_impl->cvarCommandLine;
        layers.commandLineContext = m_impl->cvarCommandLineContext;
        // Project territory (settings S7-SEC): no rung folder in here names a program.
        if (m_impl->project)
        {
            layers.projectRoots.push_back(m_impl->project->Root());
            for (const auto& pluginRoot : m_impl->project->ActivePluginRoots())
                layers.projectRoots.push_back(pluginRoot);
        }
        return layers;
    }

    void Runtime::SetCVarCommandLine(std::vector<std::string> sets, CVarContext context)
    {
        m_impl->cvarCommandLine = std::move(sets);
        m_impl->cvarCommandLineContext = context;
    }

    bool Runtime::OpenProject(const std::filesystem::path& pathOrFile, AssetRegistry::ScanProgressFn onProgress,
                              ProjectOpenOptions opts)
    {
        auto proj = Project::Open(pathOrFile, onProgress, opts);
        if (!proj)
            return false;   // Project::Open already logged the cause

        // Engine/ABI stamp check: WARN, never refuse. This was a hard refusal
        // ("belt-and-suspenders over the DLL gate"), and it is what locked
        // every host out of every existing project at each engine ABI bump --
        // the v10 bump made it bite everywhere at once. The manifest stamp
        // only describes what the game DLL was built against; the project's
        // DATA is ABI-agnostic, and the one dangerous act -- loading that
        // stale DLL -- is refused by the plugin ABI gate (Plugin.cpp), which
        // reports plugin.abi.mismatch with both versions and the fix. Host
        // strictness is a STAGE policy, not this function's: the runtime host
        // keeps plugin_load Fatal (a game host whose module cannot load still
        // refuses to boot), the editor keeps it Optional (the editor is where
        // the developer goes to FIX a stale project). The editor additionally
        // self-heals content-only stamps before this runs (EditorAppProject).
        if (proj->Manifest().engineAbi != static_cast<int>(kGamePluginABIVersion))
        {
            ARC_WARN("Runtime::OpenProject: project '{}' targets engine ABI {} but this "
                     "engine is ABI {} -- opening; its game module will be refused "
                     "until rebuilt", proj->Manifest().name,
                     proj->Manifest().engineAbi, static_cast<int>(kGamePluginABIVersion));
        }

        // Only the owner of the process's project state configures Paths and
        // layers the cvar rungs (g_projectOwner, S2-H): the first Runtime to
        // open a project, and the same Runtime on a switch.
        const bool owner = g_projectOwner == nullptr || g_projectOwner == m_impl.get();
        if (!owner)
            ARC_WARN("Runtime::OpenProject: '{}' opens in a second Runtime while another Runtime's project "
                     "owns Paths and the project cvar rungs -- they stay the owner's", proj->Manifest().name);
        // A switch: the outgoing project's settings are archived (if this host
        // archives) and its rungs dropped before the incoming one layers.
        if (m_impl->OwnsProject())
            ReleaseProjectCVarLayers(*m_impl->project, m_impl->archiveUserCVars, m_impl->editorUserConfigDir);
        m_impl->project = std::move(*proj);
        if (owner)
        {
            g_projectOwner = m_impl.get();
            // The project's identity (company, game) and the Dist flag come
            // from PathsConfigFor (settings S7): a Dist build's GameUserDir
            // resolves under the OS per-user config dir. The engine dir stays
            // the one already configured (a host's, else the exe dir).
            Paths::Configure(PathsConfigFor(*m_impl->project, Paths::Current().engineDir, DistPaths()));
        }
        // Route loose-file content loads under the project's game:// mount (Content/).
        m_impl->assets->SetContentRoot(m_impl->project->Root() / "Content");
        // GUID loads resolve through THIS project's registry (Assets AssetId seam).
        // The raw pointer is safe: the optional's storage is stable, Runtime owns
        // both objects, and every OpenProject reinstalls the resolver.
        m_impl->assets->SetAssetResolver(
            [proj = &*m_impl->project](const AssetId& id) { return proj->ResolveAsset(id); });
        // Re-layer config: engine defaults (kept) + each enabled plugin's Config/ + this
        // project's Config/ + user overrides (Saved/Config/). Precedence engine -> plugins ->
        // project -> user (a project overrides the plugins it enables). Rebuild-from-defaults
        // so re-opening a project never accumulates a previous project's layers.
        m_impl->config.LoadEngineDefaults(m_impl->engineConfigDir);
        for (const auto& pluginRoot : m_impl->project->ActivePluginRoots())
            m_impl->config.LayerDir(pluginRoot / "Config");
        m_impl->config.LayerProject(ProjectCVarDir(*m_impl->project), UserCVarDir(*m_impl->project));
        if (!owner)
            return true;
        // The cvar rungs come from the ONE source a module that loads later is
        // re-layered from (CVarLayerSources; settings spec s4.4), so the two
        // can never disagree.
        CVarRegistry& cvars = CVarRegistry::Get();
        // Preserve edits made before the first project opens as well as edits
        // made during a project. Then drop the in-memory EditorUser records so
        // a key removed from a file does not linger; CVarLayerSources re-reads
        // the folder
        // between Project and User (S1-30: one rung list for OpenProject,
        // ApplyLayersFor and ValidateCVarLayers).
        if (m_impl->archiveUserCVars && !m_impl->editorUserConfigDir.empty())
            WriteCVarArchive(cvars, m_impl->editorUserConfigDir, SetBy::EditorUser);
        cvars.RevertLayer(SetBy::EditorUser);
        const LayerSources layers = CVarLayerSources();
        for (const CVarLayerDir& layer : layers.dirs)
        {
            ApplyCVarDirectory(cvars, layer.dir, layer.by, layer.sourceModule, {}, layers.projectRoots);
            // A shipped/read-only legacy manifest could not be rewritten.
            // Its named values retain migration's merge-patch precedence over
            // an older Config key, then EditorUser/User/--set may still win.
            if (layer.by == SetBy::Project && !m_impl->project->Manifest().legacySettings.empty())
                ApplyLegacyManifestSettings(cvars, m_impl->project->Manifest(), m_impl->project->ManifestFile());
        }
        cvars.Publish();
        // Unknown keys and type mismatches in any rung's files go to the
        // Problems panel (settings spec s4.8, s12); the whole set is replaced.
        // The LOG is deferred when the manifest declares a game module or
        // enabled plugins: both hosts load them synchronously after this call
        // (EditorAppProject, RuntimeApp), so their keys look unknown until
        // LayerModuleCVars republishes -- that publish is the first to log, and
        // names only what survived. A project declaring none logs now (nothing
        // would later).
        const bool modulesLoadLater = !m_impl->project->Manifest().gameModule.empty()
                                   || !m_impl->project->ActivePluginRoots().empty();
        PublishCVarConfigDiagnostics(ValidateCVarLayers(cvars, layers),
                                     modulesLoadLater ? CVarConfigLog::Deferred : CVarConfigLog::Now);
        return true;
    }

    const Project* Runtime::CurrentProject() const noexcept
    {
        return m_impl->project ? &*m_impl->project : nullptr;
    }

    void Runtime::CloseProject()
    {
        // Mirrors exactly what Impl's ctor leaves Assets/Config in (see above:
        // `assets = Assets::Create(nullptr)` installs no content root and no
        // resolver; `config.LoadEngineDefaults(engineConfigDir)` is the only
        // config call the ctor makes) -- so a project-less Runtime looks the
        // same whether it never opened a project or just closed one.
        // The cvar User, Project and Plugin rungs leave with the project (the
        // User layer archived first when this host archives); the engine,
        // EditorUser, command-line, code and console rungs are untouched.
        // Only the owner releases (ReleaseProject, shared with ~Runtime).
        m_impl->ReleaseProject();
        m_impl->project.reset();
        m_impl->assets->SetContentRoot({});
        m_impl->assets->SetAssetResolver({});
        // Rebuild-from-defaults (LoadEngineDefaults clears m_categories first),
        // same call OpenProject makes before layering -- discards the plugin/
        // project/user layers entirely rather than leaving them shadowed by
        // nothing once nothing re-layers over them.
        m_impl->config.LoadEngineDefaults(m_impl->engineConfigDir);
        // The project's config rows go away with it; only the engine rung remains
        // (a shrinking set logs nothing: the log is a delta against the last logged set).
        // The set is process-wide: while another Runtime owns a project, its
        // rows stay.
        if (g_projectOwner == nullptr)
            PublishCVarConfigDiagnostics(ValidateCVarLayers(CVarRegistry::Get(), CVarLayerSources()), CVarConfigLog::Now);
    }

    void Runtime::SetUserCVarArchiving(bool enabled) noexcept
    {
        m_impl->archiveUserCVars = enabled;
    }

    void Runtime::SetEditorUserConfigDir(std::filesystem::path dir)
    {
        // Leaving a folder with archiving on: keep its unsaved edits.
        if (!m_impl->editorUserConfigDir.empty() && m_impl->archiveUserCVars)
            WriteCVarArchive(CVarRegistry::Get(), m_impl->editorUserConfigDir, SetBy::EditorUser);
        m_impl->editorUserConfigDir = std::move(dir);
        if (m_impl->editorUserConfigDir.empty())
            CVarRegistry::Get().RevertLayer(SetBy::EditorUser);
        else
            ReapplyEditorUserLayer(m_impl->editorUserConfigDir);
        CVarRegistry::Get().Publish();
    }

    const std::filesystem::path& Runtime::EditorUserConfigDir() const noexcept
    {
        return m_impl->editorUserConfigDir;
    }

    bool Runtime::SaveUserCVars()
    {
        if (!m_impl->archiveUserCVars)
            return false;
        bool wrote = false;
        if (m_impl->OwnsProject())   // the User rung holds the OWNER's project values (S2-H)
        {
            // Empty in Dist with no per-user OS base (settings S7): nothing to write.
            const std::filesystem::path userDir = UserCVarDir(*m_impl->project);
            if (!userDir.empty())
            {
                WriteCVarArchive(CVarRegistry::Get(), userDir);
                wrote = true;
            }
        }
        if (!m_impl->editorUserConfigDir.empty())
        {
            WriteCVarArchive(CVarRegistry::Get(), m_impl->editorUserConfigDir, SetBy::EditorUser);
            wrote = true;
        }
        return wrote;
    }

    std::optional<Guid> Runtime::RegisterCreatedAsset(const std::filesystem::path& file)
    {
        if (!m_impl->project)
        {
            ARC_WARN("Runtime::RegisterCreatedAsset: no project open -- '{}' not registered",
                     file.generic_string());
            return std::nullopt;
        }
        return m_impl->project->RegisterAsset(file);
    }

    bool Runtime::UnregisterAsset(const Guid& id)
    {
        if (!m_impl->project)
        {
            ARC_WARN("Runtime::UnregisterAsset: no project open -- {} not unregistered", id.ToString());
            return false;
        }
        return m_impl->project->UnregisterAsset(id);
    }

    RebindResult Runtime::RebindMovedAsset(const Guid& id, const std::filesystem::path& newFile)
    {
        if (!m_impl->project)
        {
            ARC_WARN("Runtime::RebindMovedAsset: no project open -- '{}' not rebound", newFile.generic_string());
            return RebindResult::NoProject;
        }
        return m_impl->project->RebindAsset(id, newFile);
    }

    bool Runtime::SetProjectBootScene(const Guid& id)
    {
        if (!m_impl->project)
        {
            ARC_WARN("Runtime::SetProjectBootScene: no project open -- boot scene not set");
            return false;
        }
        return m_impl->project->SetBootScene(id);
    }

    bool Runtime::SetProjectInputActionsAsset(const Guid& id)
    {
        if (!m_impl->project)
        {
            ARC_WARN("Runtime::SetProjectInputActionsAsset: no project open");
            return false;
        }
        return m_impl->project->SetInputActionsAsset(id);
    }

    bool Runtime::RestampProjectEngineAbi(int abi)
    {
        if (!m_impl->project)
        {
            ARC_WARN("Runtime::RestampProjectEngineAbi: no project open -- manifest not restamped");
            return false;
        }
        return m_impl->project->RestampEngineAbi(abi);
    }

    Config& Runtime::Configuration() noexcept
    {
        return m_impl->config;
    }
}
