#pragma once
#include <Arcane/Modules/ArcaneEngineApi.hpp>

// Plugin: the engine's current game-runtime plugin protocol -- BOTH hosts consume
// it (ArcaneRuntime and the editor), which is why it lives here and not in either
// exe. Today there is one
// plugin kind, so this class owns the GamePlugin_* ABI. If editor/tool plugins
// appear later, split this into GamePlugin.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Core/Api.hpp>
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Arcane
{
    // WHICH plugin-resolve cause occurred (a Module::Load failure is reported
    // separately via Module::LastLoadError() -- this struct stays at Kind::None
    // for that case, since neither gate below ran). CrtFlavorMismatch is the
    // one cause detected BEFORE the module loads: a Debug-CRT DLL in a Release
    // host (or vice versa) detonates inside LoadLibrary's static-initializer
    // pass, so it must be refused from the file bytes (Module::ScanFileCrtFlavor).
    // Populated by Plugin::Load's PluginResolveError* overload; the plain
    // single-argument overload is a thin wrapper that discards this detail
    // (existing callers keep compiling).
    struct PluginResolveError
    {
        enum class Kind : std::uint8_t { None, MissingExport, AbiMismatch, CrtFlavorMismatch };
        Kind          kind      = Kind::None;
        std::string   symbol;              // MissingExport: the first missing export, by declaration order.
                                           // CrtFlavorMismatch: the CRT import that decided the verdict (e.g. "ucrtbased.dll").
        std::uint32_t pluginAbi = 0;       // Kind::AbiMismatch: what the plugin was built against
        std::uint32_t engineAbi = 0;       // Kind::AbiMismatch: what this engine enforces
        bool pluginDebugCrt = false;       // Kind::CrtFlavorMismatch: the plugin's CRT family
        bool hostDebugCrt   = false;       // Kind::CrtFlavorMismatch: this host's CRT family
        // Kind::AbiMismatch with pluginAbi == engineAbi: the epoch matched and the
        // layout digest did not (or the module exported none: pluginDigest 0).
        std::uint64_t pluginDigest = 0;
        std::uint64_t engineDigest = 0;
    };

    class ARC_ENGINE_API Plugin
    {
    public:
        Plugin(Plugin&&) noexcept = default;
        Plugin& operator=(Plugin&&) noexcept = default;

        Plugin(const Plugin&) = delete;
        Plugin& operator=(const Plugin&) = delete;

        static std::optional<Plugin> Load(std::filesystem::path path);

        // Same load, plus WHICH resolve cause failed when it returns nullopt
        // (left untouched -- Kind::None -- if `error` is null or the module
        // itself failed to load; check Module::LastLoadError() for that case).
        static std::optional<Plugin> Load(std::filesystem::path path, PluginResolveError* error);

        // A game linked INTO the host (the monolithic layout; engine
        // modularization s3.1): no Module, no file -- the table is
        // ArcGameModule_Get()'s. This only copies the table; the ABI gate for it
        // is AdoptStaticGameModule (below), which PluginHost calls first. `table` must outlive the Plugin (it is
        // the game's function-local static). The cvar module is registered by
        // NAME only: there is no image range, and nothing in a package unloads.
        [[nodiscard]] static Plugin FromStatic(const PluginVTable& table, std::string cvarModule);

        // ELF and Mach-O (Module::PinsForProcessLife; issue #21 M-P1/M-P2):
        // the cvar modules of the DEPENDENCY images Load re-attributed. A
        // dependency a game load maps runs its statics inside that load's
        // CVarModuleScope; Load re-names those registrations to the image's own
        // module (its file name: libArcaneClient.so -> ArcaneClient, the name
        // it registers under in a host that links it), so the game's name drop
        // never strips a library pinned for the life of the process. PluginHost
        // layers each of these with every module it loads (LayerModuleCVars).
        // Process-wide, first-seen order, never shrinks. Empty on Windows.
        [[nodiscard]] static std::vector<std::string> DependencyCVarModules();

        [[nodiscard]] bool IsLoaded() const noexcept { return m_module.IsLoaded() || m_staticTable != nullptr; }
        // True for FromStatic: LoadedModule() is then an empty Module (no image).
        [[nodiscard]] bool IsStatic() const noexcept { return m_staticTable != nullptr; }
        // The system-factory owner key (Arcane/Plugin/SystemFactory.hpp): the
        // loaded image's base, or -- for a static game, which has no image of its
        // own -- the address of its table, equally unique and stable.
        [[nodiscard]] const void* FactoryOwner() const noexcept
        {
            return m_staticTable ? static_cast<const void*>(m_staticTable) : m_module.Image().base;
        }
        [[nodiscard]] const Module& LoadedModule() const noexcept { return m_module; }
        [[nodiscard]] const PluginVTable& VTable() const noexcept { return m_vtable; }
        // The cvar module this image's registrations belong to (settings spec
        // s4.4): the CVarModuleScope open when it loaded (PluginHost names the
        // SOURCE dll's stem), else this file's stem. The Plugin OWNS them:
        // destroying it unregisters them, before the image unmaps.
        [[nodiscard]] const std::string& CVarModule() const noexcept { return m_cvars.module; }

    private:
        // Move-only. Inline, so a Plugin destroyed in any module unregisters
        // through ArcaneCore's exported registry.
        struct CVarOwner
        {
            std::string module;
            const void* imageBase = nullptr;
            std::size_t imageSize = 0;
            CVarOwner() = default;
            CVarOwner(std::string m, const void* base, std::size_t size) noexcept
                : module(std::move(m)), imageBase(base), imageSize(size) {}
            CVarOwner(CVarOwner&& o) noexcept
                : module(std::exchange(o.module, {})), imageBase(std::exchange(o.imageBase, nullptr)),
                  imageSize(std::exchange(o.imageSize, 0)) {}
            CVarOwner& operator=(CVarOwner&& o) noexcept
            {
                if (this != &o)
                {
                    Release();
                    module = std::exchange(o.module, {});
                    imageBase = std::exchange(o.imageBase, nullptr);
                    imageSize = std::exchange(o.imageSize, 0);
                }
                return *this;
            }
            CVarOwner(const CVarOwner&) = delete;
            CVarOwner& operator=(const CVarOwner&) = delete;
            ~CVarOwner() { Release(); }
            void Release() noexcept
            {
                if (imageBase && imageSize != 0)
                    CVarRegistry::Get().UnregisterModuleRange(imageBase, imageSize);
                else if (!module.empty())
                    CVarRegistry::Get().UnregisterModule(module);
                module.clear();
                imageBase = nullptr;
                imageSize = 0;
            }
        };

        Plugin(Module module, PluginVTable vtable, std::string cvarModule) noexcept;

        Module m_module;
        PluginVTable m_vtable{};
        const PluginVTable* m_staticTable = nullptr;   // FromStatic only
        // Declared AFTER m_module: members destroy in reverse, so the module's
        // cvars, commands and callbacks go BEFORE the FreeLibrary.
        CVarOwner m_cvars;
    };

    // The ABI gate shared by Plugin::Load and the static adoption path. Returns
    // false with Kind::AbiMismatch when table.ABIVersion is null, the epoch
    // disagrees with kGamePluginABIVersion, table.ABIDigest is null, or
    // table.ABIDigest() disagrees with kGamePluginABIDigest. It never calls Init.
    // `error` may be null.
    ARC_ENGINE_API bool AcceptPluginAbi(const PluginVTable& table, PluginResolveError* error) noexcept;

    // Adopts a game linked INTO the host (the monolithic layout): runs
    // AcceptPluginAbi and, only on success, FromStatic into *out. On false it
    // returns false, leaves *out empty and does not call Init. Compiled in every
    // configuration (PluginHost's ARC_MONOLITHIC block calls it; so does a test).
    // `table` must outlive the Plugin it produces.
    ARC_ENGINE_API bool AdoptStaticGameModule(const PluginVTable& table, PluginResolveError* error,
                                              std::optional<Plugin>* out, std::string cvarModule);
}
