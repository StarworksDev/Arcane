#pragma once

// GameModule: the SDK's game-module boilerplate, written ONCE (spec docs/specs/
// 2026-09-13-game-module-boilerplate-design.md) -- Unreal's
// IMPLEMENT_PRIMARY_GAME_MODULE shaped for Arcane's plugin ABI. A module is a
// class deriving GameModule plus one line:
//
//   struct Module final : Arcane::GameModule
//   {
//       void OnDrawUI() override { /* HUD */ }
//   };
//   ARC_GAME_MODULE(MyGame::Module)
//
// The macro emits the nine exports the host resolves (PluginEntry::k*,
// PluginABI.hpp) and everything a module used to copy: the shared TypeContext
// pin, the Mosaic log-sink + assert-handler installs, the ImGui context/
// allocator adoption, this module's Arcane::ComponentModule with the
// ARC_COMPONENT drain (GameComponents.hpp), and the registry Save/LoadState
// round-trip for hot reload. Every hook has a default; override what the
// module needs. THE ENGINE OWNS ITS STANDARD SYSTEMS (Runtime::
// InstallEngineSystems: Arcane::PhysicsSystem2D -> TransformPropagationSystem ->
// BoundsSystem in fixedUpdate; RenderSubmissionSystem runs inside the frame
// extract, Arcane/Render/ExtractFrame.hpp) -- a module registers ONLY its own
// systems, in FixedUpdate or Update. The Render phase is RETIRED (F5 plan 1
// Task 5): no host runs it, so a Render-phase system never runs and its
// registration warns once (SystemFactory.cpp). Default-constructible systems use ARC_SYSTEM in one .cpp;
// systems needing runtime constructor values use RegisterSystem<T>(mask, phase)
// in OnInit. Both paths place them with Arcane::Before<...> / Arcane::After<...>
// against the engine's types (Astra keys systems by a hash of the type NAME, so
// that works across the DLL boundary). Registration declares a FACTORY, not an instance: each of the
// N Runtimes the host attached instantiates the subset its NetMode matches
// (Arcane/Plugin/SystemFactory.hpp), which is what lets one module image serve a
// server world and a client world in one process.
//
// EVERYTHING HERE INSTANTIATES IN THE MODULE IMAGE, on purpose: SetTypeContext,
// the Mosaic installs and RegisterComponents each act on THIS module's own
// per-module state, and the ComponentModule handle owns descriptors that point
// into this image. That is why the bodies are inline in a header rather than
// exported from ArcaneClient.dll.

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ProcessContext.hpp>   // Process()/RegisterSystem reach SystemFactories()
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Core/ModuleApi.hpp>
#include <Arcane/Plugin/GameComponents.hpp>
#include <Arcane/Plugin/GameSystems.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>   // RoleMask / SystemPhase
#include <Arcane/Scene/SceneResources.hpp>   // SceneRoot: SceneRootEntity() + the Save/LoadState root id

#if defined(ARC_MONOLITHIC)
#include <Arcane/Core/ModuleAttach.hpp>
#endif

#include <Arcane/Ecs.hpp>   // the Arcane:: names (aliases) this header's declarations spell

#if defined(ARC_GAME_HAS_IMGUI)
#include <imgui.h>   // ABI v2: adopt the host's ImGui context/allocators
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

// Modular layout: the nine GamePlugin_* entry points (and ArcGameModule_Get)
// leave the game DLL. Monolithic layout (ARC_MONOLITHIC, the packaged exe):
// the game is a static library linked into the host, so nothing is exported --
// the host calls ArcGameModule_Get() as a linked symbol (Plugin/PluginHost.cpp).
#if defined(ARC_MONOLITHIC)
  #define ARC_GAME_MODULE_EXPORT
#elif defined(_WIN32)
  #define ARC_GAME_MODULE_EXPORT __declspec(dllexport)
#else
  #define ARC_GAME_MODULE_EXPORT __attribute__((visibility("default")))
#endif

namespace Arcane
{
    class GameModule
    {
    public:
        virtual ~GameModule() = default;

        // ---- hooks (every one defaulted; override what the module needs) ----

        // After the prologue (TypeContext, Mosaic, ImGui, this module's
        // component and automatic-system registrars drained). Register only
        // systems needing runtime constructor values here.
        // false aborts the load (the host reports "initial load failed").
        virtual bool OnInit(EngineContext& ctx) { (void)ctx; return true; }
        // Before the ComponentModule handle closes and before the image unmaps;
        // Context()/Registry()/Components() are still valid here.
        virtual void OnShutdown() {}
        // Runs each fixed step before the fixedUpdate scheduler. Gameplay belongs
        // in SYSTEMS, which read time and input as resources (Arcane::Res<Arcane::
        // Time>, Arcane::Res<Arcane::GameInput>) -- never copy values into
        // components from here (input-seam spec 2026-10-02).
        virtual void OnFixedUpdate(double dt) { (void)dt; }
        virtual void OnUpdate(double dt, double alpha) { (void)dt; (void)alpha; }
        // Between the host's ImGui BeginFrame and Render. Not called in a headless
        // host (no ImGui context) -- no guard needed in the override.
        virtual void OnDrawUI() {}
        // Module extras, written AFTER the registry blob / read AFTER the registry
        // restore. The registry round-trip itself is the macro's (never overridden).
        virtual void OnSaveState(Arcane::BinaryWriter& w) { (void)w; }
        virtual bool OnLoadState(Arcane::BinaryReader& r) { (void)r; return true; }

        // ---- what the prologue established (valid from OnInit to OnShutdown) ----

        [[nodiscard]] EngineContext& Context() const noexcept
        {
            ARC_ASSERT(m_ctx != nullptr, "GameModule::Context() outside the OnInit..OnShutdown window");
            return *m_ctx;
        }
        [[nodiscard]] Runtime&           Engine()   const noexcept { return *Context().engine; }
        [[nodiscard]] ::Arcane::Registry& Registry() const noexcept { return Engine().Registry(); }
        // The process object (ABI 30): the shared TypeContext and the system-factory
        // table. Engine() is the PRIMARY world; Process() is what every world shares.
        [[nodiscard]] ProcessContext&  Process()  const noexcept { return *Context().process; }
        // The presentation extension, or NULL on a headless host (ArcaneServer, an
        // embedded server world, a headless test). Always null-check it.
        [[nodiscard]] ClientRuntime*   Client()   const noexcept { return Context().client; }
        [[nodiscard]] Arcane::ComponentModule& Components() const noexcept
        {
            ARC_ASSERT(m_components != nullptr, "GameModule::Components() outside the OnInit..OnShutdown window");
            return *m_components;
        }
        // The scene's root entity, read from the registry ON DEMAND -- never
        // cached: Init runs before the host loads the boot scene, and File > Open
        // Scene swaps the whole registry. Invalid when no scene is loaded.
        [[nodiscard]] Arcane::Entity SceneRootEntity() const
        {
            const SceneRoot* sr = Registry().GetResource<SceneRoot>();
            return sr ? sr->entity : Arcane::Entity::Invalid();
        }

        // Register one of this module's systems ONCE per DLL load when it needs
        // runtime constructor values. Default-constructible systems should use
        // ARC_SYSTEM in their .cpp. Static registration order is unspecified;
        // Astra's Before/After traits are the sole semantic ordering contract. Every
        // Runtime whose NetMode matches `mask` instantiates it: the primary right
        // after OnInit, any other attached Runtime at attach, and all of them again
        // after a hot reload. The std::function lives in THIS module and PluginHost
        // clears it before the image unmaps.
        //
        // THIS IS ALSO THE ONLY WAY A SYSTEM SURVIVES A MODE FLIP: Runtime::SetNetMode
        // clears the schedulers and re-instantiates from this table, so a module that
        // still adds systems DIRECTLY in OnInit (the pre-ABI-30 idiom) loses them the
        // moment its world is re-roled -- the editor's play-mode picker does exactly
        // that -- and does not get them back until the next reload.
        template <class System, class... Args>
        void RegisterSystem(RoleMask mask, SystemPhase phase, Args... args)
        {
            Game::Detail::AddSystemFactory<System>(
                Process().SystemFactories(), mask, phase, args...);
        }

        // Bound by ARC_GAME_MODULE's Init before OnInit runs. Not for modules.
        void BindForMacro_(EngineContext* ctx, Arcane::ComponentModule* components) noexcept
        {
            m_ctx        = ctx;
            m_components = components;
        }

    private:
        friend struct ::Arcane::Detail::AbiManifest::Access;   // hashes these private members (Plugin/AbiManifestEngine.hpp)
        EngineContext*           m_ctx        = nullptr;
        Arcane::ComponentModule* m_components = nullptr;
    };

    namespace GameModuleDetail
    {
        // The per-module state behind the exports: three RAW pointers, so the
        // object is trivially destructible. The ComponentModule contract's
        // "NEVER a plugin-side static/global object" rule: a static whose
        // destructor does live cleanup (a ComponentModule by value, an optional,
        // a unique_ptr) runs it during FreeLibrary at DLL_PROCESS_DETACH, under
        // the loader lock. Raw pointers have no destructor; a skipped Shutdown
        // degrades to the contract's forget semantics (the handle leaks and the
        // host's UnregisterModuleRange net catches the descriptor half).
        struct State
        {
            EngineContext*           ctx        = nullptr;
            Arcane::ComponentModule* components = nullptr;
            GameModule*              instance   = nullptr;

            void DrawUI()
            {
#if defined(ARC_GAME_HAS_IMGUI)
                if (instance && ctx && ctx->imguiContext) instance->OnDrawUI();
#endif
            }

            template <typename Type>
            bool Init(EngineContext* c, const char* name)
            {
                static_assert(std::is_base_of_v<GameModule, Type>,
                              "ARC_GAME_MODULE(Type): Type must derive from Arcane::GameModule");

                // 1. The shared reflection context in THIS module, and this
                // module's Mosaic log/assert routing into the engine's.
                // ARC_INTERNAL_BEGIN: the module-local reflection pin is Astra's own per-image state
                Astra::SetTypeContext(c->typeContext);
                // ARC_INTERNAL_END
                Log::InstallMosaicSink();
                Assert::InstallMosaicHandler();
                ctx = c;

                // ABI v2: adopt the host's ImGui context + allocators so OnDrawUI
                // draws into the host's single GImGui. Null in a headless host.
                #if defined(ARC_GAME_HAS_IMGUI)
                if (c->imguiContext)
                {
                    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(c->imguiContext));
                    ImGui::SetAllocatorFunctions(
                        reinterpret_cast<ImGuiMemAllocFunc>(c->imguiAlloc),
                        reinterpret_cast<ImGuiMemFreeFunc>(c->imguiFree),
                        c->imguiUserData);
                }
                #endif

                // 2. This module's own component types: open the handle, drain the
                // ARC_COMPONENT registrar into it. Every component added under
                // Source/ (Assets -> Create -> C++ Class, or one ARC_COMPONENT
                // line by hand) registers here with no edit to the module.
                components = new Arcane::ComponentModule(
                    Arcane::ComponentModule::Open(c->engine->Components(), name));
                if (!*components)
                {
                    delete components;   // empty handle: safe to destroy here (still mapped)
                    components = nullptr;
                    ctx        = nullptr;
                    return false;        // SetTypeContext above makes this unreachable; fail loudly if not
                }
                const std::size_t count = Game::RegisterComponents(*components);
                ARC_INFO("{}: registered {} module component type(s)", name, count);

                // 3. The module itself. Drain automatic system factories after
                // binding so they share this image's open owner bracket. OnInit
                // follows for systems whose construction needs runtime values.
                instance = new Type();
                instance->BindForMacro_(c, components);
                const std::size_t systemCount = Game::RegisterSystems(c->process->SystemFactories());
                ARC_INFO("{}: registered {} automatic module system(s)", name, systemCount);
                if (!instance->OnInit(*c))
                {
                    delete instance;   instance   = nullptr;
                    delete components; components = nullptr;
                    ctx = nullptr;
                    return false;
                }
                return true;
            }

            void Shutdown()
            {
                // Instance first (its destructor may still touch its own
                // components), then the handle -- which releases this module's
                // descriptors + meta BEFORE the image unmaps.
                if (instance)
                {
                    instance->OnShutdown();
                    delete instance;
                    instance = nullptr;
                }
                delete components;
                components = nullptr;
                ctx        = nullptr;
                // This module's Mosaic level setter (Log.hpp, settings arc S2)
                // must not be called after the image unmaps.
                Log::UninstallMosaicLevelTarget();
            }

            void SaveState(Arcane::BinaryWriter& w)
            {
                // Persist the scene-root entity id explicitly -- resources are not
                // part of the registry snapshot, so LoadState must re-set SceneRoot
                // after the restore. Read live: whatever scene is loaded NOW is the
                // one a hot reload has to bring back.
                const Arcane::Entity root = instance ? instance->SceneRootEntity() : Arcane::Entity::Invalid();
                w(static_cast<uint64_t>(root));

                auto snap = ctx->engine->SnapshotRegistry();
                if (snap.IsErr())
                {
                    // Snapshot failed: write a zero-length blob so LoadState fails
                    // cleanly (RestoreRegistry rejects an empty frame) instead of
                    // masking the loss. No extras follow a failed blob.
                    w(static_cast<uint64_t>(0));
                    return;
                }
                const std::vector<std::byte>& blob = *snap.GetValue();
                w(static_cast<uint64_t>(blob.size()));
                w.WriteBytes(blob.data(), blob.size());

                if (instance)
                    instance->OnSaveState(w);
            }

            bool LoadState(Arcane::BinaryReader& r)
            {
                uint64_t rootRaw = 0; r(rootRaw);
                const Arcane::Entity savedRoot(static_cast<Arcane::Entity::StorageType>(rootRaw));

                uint64_t n = 0; r(n);
                std::vector<std::byte> blob(static_cast<std::size_t>(n));
                r.ReadBytes(blob.data(), static_cast<std::size_t>(n));
                if (r.HasError()) return false;
                if (!ctx->engine->RestoreRegistry(blob)) return false;

                // SceneRoot is a resource; the snapshot does not carry it. A zero id
                // means the reload happened with no scene loaded -- a legitimate
                // state; leave the resource unset rather than publish an invalid root.
                if (savedRoot.IsValid())
                    ctx->engine->Registry().SetResource<SceneRoot>(SceneRoot{savedRoot});

                return instance ? instance->OnLoadState(r) : true;
            }
        };
    }
}

// The one-argument face: the module reports the SDK's own ABI version.
#define ARC_GAME_MODULE(Type) ARC_GAME_MODULE_ABI(Type, ::Arcane::kGamePluginABIVersion)

// ARC_MODULE supplies the anchor and attach callback for a resident static
// game. A hot-reloadable DLL must emit only the anchor: Core keeps attach
// callbacks for the process lifetime, beyond the game's unload. The game
// pins its TypeContext in Init instead. Two levels expand ARC_MODULE_NAME.
#define ARC_GAME_MODULE_ANCHOR_(Name) extern "C" ARC_GAME_MODULE_EXPORT void ArcModule_##Name() {}
#define ARC_GAME_MODULE_ANCHOR(Name) ARC_GAME_MODULE_ANCHOR_(Name)
#define ARC_GAME_MODULE_STATIC_(Name) ARC_MODULE(Name)
#define ARC_GAME_MODULE_STATIC(Name) ARC_GAME_MODULE_STATIC_(Name)
#if defined(ARC_MODULE_NAME)
  #if defined(ARC_MONOLITHIC)
    #define ARC_GAME_MODULE_DECLARATION_ ARC_GAME_MODULE_STATIC(ARC_MODULE_NAME)
  #else
    #define ARC_GAME_MODULE_DECLARATION_ ARC_GAME_MODULE_ANCHOR(ARC_MODULE_NAME)
  #endif
#else
  #define ARC_GAME_MODULE_DECLARATION_
#endif

// Monolithic: engine and game build together, so the runtime ABI gate becomes a
// compile-time one (HotReloadPluginBad, the one deliberate mismatch, is never
// packaged).
#if defined(ARC_MONOLITHIC)
  #define ARC_GAME_MODULE_ABI_CHECK_(Abi)                                                          \
      static_assert(static_cast<uint32_t>(Abi) == ::Arcane::kGamePluginABIVersion,                 \
                    "ARC_GAME_MODULE: a monolithic game must report the SDK's ABI version"); \
      static_assert(::Arcane::AbiGameMix() == ::Arcane::kGamePluginABIDigest,                \
                    "ARC_GAME_MODULE: digest does not match build/abi-digest.txt");
#else
  #define ARC_GAME_MODULE_ABI_CHECK_(Abi)
#endif

// The two-argument form exists for ONE caller: the HotReloadPluginBad test
// build, which must report a version the host's gate refuses. A real module
// never passes anything but the SDK's constant.
//
// Besides the nine GamePlugin_* entry points (the eight plus GamePlugin_ABIDigest)
// it emits ArcGameModule_Get(): the same nine as one Arcane::PluginVTable
// (the game table, s3.1). Modular hosts resolve the GamePlugin_* exports by name (Plugin::Load); a monolithic host
// adopts ArcGameModule_Get() (Plugin::FromStatic). Both layouts emit it so one
// acquisition test covers both.
#define ARC_GAME_MODULE_ABI(Type, Abi)                                                          \
    ARC_GAME_MODULE_ABI_CHECK_(Abi)                                                               \
    ARC_GAME_MODULE_DECLARATION_                                                                  \
    namespace { ::Arcane::GameModuleDetail::State arcane_game_module_state_; }                     \
    extern "C"                                                                                     \
    {                                                                                              \
        ARC_GAME_MODULE_EXPORT uint32_t GamePlugin_ABIVersion()                                 \
        { return static_cast<uint32_t>(Abi); }                                                     \
        ARC_GAME_MODULE_EXPORT bool GamePlugin_Init(::Arcane::EngineContext* ctx)               \
        { return arcane_game_module_state_.Init<Type>(ctx, #Type); }                               \
        ARC_GAME_MODULE_EXPORT void GamePlugin_Shutdown()                                       \
        { arcane_game_module_state_.Shutdown(); }                                                  \
        ARC_GAME_MODULE_EXPORT void GamePlugin_FixedUpdate(double dt)                           \
        { if (auto* m = arcane_game_module_state_.instance) m->OnFixedUpdate(dt); }                \
        ARC_GAME_MODULE_EXPORT void GamePlugin_Update(double dt, double alpha)                  \
        { if (auto* m = arcane_game_module_state_.instance) m->OnUpdate(dt, alpha); }              \
        ARC_GAME_MODULE_EXPORT std::uint64_t GamePlugin_ABIDigest()                             \
        { static constexpr std::uint64_t digest = ::Arcane::AbiGameMix(); return digest; }         \
        ARC_GAME_MODULE_EXPORT void GamePlugin_DrawUI()                                         \
        {                                                                                          \
            arcane_game_module_state_.DrawUI();                                                   \
        }                                                                                          \
        ARC_GAME_MODULE_EXPORT void GamePlugin_SaveState(::Arcane::BinaryWriter& w)             \
        { arcane_game_module_state_.SaveState(w); }                                                \
        ARC_GAME_MODULE_EXPORT bool GamePlugin_LoadState(::Arcane::BinaryReader& r)             \
        { return arcane_game_module_state_.LoadState(r); }                                         \
        ARC_GAME_MODULE_EXPORT const ::Arcane::PluginVTable* ArcGameModule_Get()                  \
        {                                                                                         \
            static const ::Arcane::PluginVTable table{                                            \
                &GamePlugin_ABIVersion, &GamePlugin_Init, &GamePlugin_Shutdown,                   \
                &GamePlugin_FixedUpdate, &GamePlugin_Update, &GamePlugin_SaveState,               \
                &GamePlugin_LoadState, &GamePlugin_DrawUI,       \
                &GamePlugin_ABIDigest };                                                          \
            return &table;                                                                        \
        }                                                                                         \
    }

// The macro above evaluates ::Arcane::AbiGameMix() in the GAME's translation
// unit, at compile time, so the digest it exports is the one THAT TU computed (a
// game built with -DASTRA_ENTITY_BITS=64 exports a different integer).
// AbiManifestGame.hpp reaches only this game's module closure: it hashes the
// Engine group always and the Input / Client groups when their headers are
// visible, else folds the committed sub-digests. The Engine group manifest
// includes this header, so it is pulled in last, after every definition it
// needs; AbiManifestGame.hpp declares the group mixes before it includes the
// group headers, so the cycle resolves in either include order.
#include <Arcane/Plugin/AbiManifestGame.hpp>
