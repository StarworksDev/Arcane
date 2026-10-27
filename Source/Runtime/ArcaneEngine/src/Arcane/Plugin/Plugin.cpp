#include <Arcane/Plugin/Plugin.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Core/ModuleAttach.hpp>
#include <Arcane/Scene/PhysicsQuery.hpp>
#include <Arcane/Sim/SimHook.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    template<typename T>
    T Resolve(Arcane::Module& module, const char* name) noexcept
    {
        return reinterpret_cast<T>(module.Symbol(name));
    }

    // WHICH of the seven required exports is missing (in declaration order), and
    // BOTH ABI version numbers on a mismatch, so a caller can tell "the DLL isn't
    // an Arcane plugin at all" from "it's an Arcane plugin, built against the
    // wrong engine" instead of one bare bool. `error` may be null (the plain
    // Plugin::Load(path) overload forwards nullptr) -- every write below is
    // guarded on that.
    bool ResolveGamePluginAbi(Arcane::Module& module, Arcane::PluginVTable& out,
                              Arcane::PluginResolveError* error) noexcept
    {
        const auto need = [&](auto& slot, const char* name) -> bool
        {
            if (slot) return true;
            if (error)
            {
                error->kind   = Arcane::PluginResolveError::Kind::MissingExport;
                error->symbol = name;
            }
            return false;
        };

        // Resolve ALL seven first, then check them in declaration order below --
        // so the FIRST missing symbol in declaration order is the one reported,
        // regardless of which symbols the DLL happens to export.
        out.ABIVersion  = Resolve<std::uint32_t(*)()>(module, Arcane::PluginEntry::kABIVersion);
        out.Init        = Resolve<bool(*)(Arcane::EngineContext*)>(module, Arcane::PluginEntry::kInit);
        out.Shutdown    = Resolve<void(*)()>(module, Arcane::PluginEntry::kShutdown);
        out.FixedUpdate = Resolve<void(*)(double)>(module, Arcane::PluginEntry::kFixedUpdate);
        out.Update      = Resolve<void(*)(double, double)>(module, Arcane::PluginEntry::kUpdate);
        out.SaveState   = Resolve<void(*)(Astra::BinaryWriter&)>(module, Arcane::PluginEntry::kSaveState);
        out.LoadState   = Resolve<bool(*)(Astra::BinaryReader&)>(module, Arcane::PluginEntry::kLoadState);

        if (!need(out.ABIVersion,  Arcane::PluginEntry::kABIVersion))  return false;
        if (!need(out.Init,        Arcane::PluginEntry::kInit))        return false;
        if (!need(out.Shutdown,    Arcane::PluginEntry::kShutdown))    return false;
        if (!need(out.FixedUpdate, Arcane::PluginEntry::kFixedUpdate)) return false;
        if (!need(out.Update,      Arcane::PluginEntry::kUpdate))      return false;
        if (!need(out.SaveState,   Arcane::PluginEntry::kSaveState))   return false;
        if (!need(out.LoadState,   Arcane::PluginEntry::kLoadState))   return false;

        // Optional: resolution is lenient, the host null-checks before calling.
        out.DrawUI = Resolve<void(*)()>(module, Arcane::PluginEntry::kDrawUI);

        // The epoch FIRST: a module from an older epoch (HotReloadPluginBad, an
        // old game) fails here without being required to export the digest.
        const std::uint32_t pluginAbi = out.ABIVersion();
        if (pluginAbi != Arcane::kGamePluginABIVersion)
        {
            if (error)
            {
                error->kind      = Arcane::PluginResolveError::Kind::AbiMismatch;
                error->pluginAbi = pluginAbi;
                error->engineAbi = Arcane::kGamePluginABIVersion;
            }
            return false;
        }

        // Epoch matches: now the digest export is required (a missing symbol is
        // the unresolved-symbol failure, not AbiMismatch), then compared.
        out.ABIDigest = Resolve<std::uint64_t(*)()>(module, Arcane::PluginEntry::kABIDigest);
        if (!need(out.ABIDigest, Arcane::PluginEntry::kABIDigest)) return false;
        return Arcane::AcceptPluginAbi(out, error);
    }

    bool ResolveGamePluginAbi(Arcane::Module& module, Arcane::PluginVTable& out) noexcept
    {
        return ResolveGamePluginAbi(module, out, nullptr);
    }

    // Drops everything an image's statics can have registered into the
    // process-wide lists at image load, while the image is still mapped: cvars,
    // settings blocks and log setters (CVarRegistry), the module-attach callback,
    // sim hooks and the 2D physics query pair. Component descriptors are not
    // here: they are created only by a Runtime's RegisterComponents, which a
    // refused load never reaches (PluginHost's TeardownImage purges those).
    void DropImageRegistrations(const void* base, std::size_t size, const std::filesystem::path& image)
    {
        if (!base || size == 0)
            return;
        const std::size_t cvars    = Arcane::CVarRegistry::Get().UnregisterModuleRange(base, size);
        const std::size_t attaches = Arcane::UnregisterModuleAttachInRange(base, size);
        const std::size_t hooks    = Arcane::UnregisterSimHooksInRange(base, size);
        const bool        query    = Arcane::ClearPhysicsQueryFns2DInRange(base, size);
        if (cvars != 0 || attaches != 0 || hooks != 0 || query)
            ARC_TRACE("plugin: refused load dropped {} cvar slot(s), {} module-attach callback(s), {} sim hook(s){} of '{}'",
                      cvars, attaches, hooks, query ? " and the 2D query pair" : "", image.generic_string());
    }

    // Plugin::DependencyCVarModules: the re-attributed dependencies' names.
    std::mutex               g_dependencyModulesMutex;
    std::vector<std::string> g_dependencyModules;

    // The module a shared library's own code names itself (ARC_MODULE_NAME,
    // the premake project name its file is named after): the file name with
    // every extension off ("libArcanePhysics2D.so" and ".dylib" ->
    // "ArcanePhysics2D", "libfoo.so.1" -> "foo") and the ELF/Mach-O "lib"
    // prefix. A game module or project plugin has no prefix ("Foo.so" -> "Foo",
    // the stem PluginHost loads it under).
    std::string OwnModuleName(const std::filesystem::path& image)
    {
        std::string name = image.filename().string();
        if (name.size() > 3 && name.starts_with("lib"))
            name.erase(0, 3);
        if (const std::size_t dot = name.find('.'); dot != std::string::npos)
            name.erase(dot);
        return name;
    }

    // ELF and Mach-O (issue #21 M-P1/M-P2). Every image this load newly mapped
    // besides the module itself is a DEPENDENCY -- the beside-exe retry's
    // (Module::Load), or one the loader found on its own (RUNPATH, dyld's rpath
    // chain) -- and its statics ran inside this load's CVarModuleScope, so its
    // cvars, settings, commands and callbacks carry the GAME's name. Such an
    // image is pinned for the life of the process (Module::PinsForProcessLife:
    // PluginHost's PinNewDependencies, the refusal net below; the retry's
    // RTLD_NODELETE even when the load then fails), and by the game's name its
    // registrations would be (1) stripped by the name drop of a FAILED load of
    // that stem -- this one or any later one -- leaving the library mapped
    // with no cvars, its statics never to run again (M-P1), and (2) layered
    // only with that one stem, so a host that later loads a different game
    // stem keeps the first project's values on them (M-P2). Re-name them to
    // the image's own module instead, BEFORE any drop below: exactly the name
    // they carry in a host that links the library, and PluginHost layers it
    // (DependencyCVarModules). Matched by address, so the module's own and any
    // older registrations under the name are untouched. Windows keeps the
    // game's name: its dependencies leave with the load session
    // (PluginHost::EndLoadSession) and re-register under the next one.
    // The diff assumes the single loader thread (see Plugin::Load).
    void AdoptDependencyRegistrations(const std::vector<Arcane::Module::MappedModule>& before,
                                      const void* loadedBase, const std::string& cvarModule)
    {
        for (const Arcane::Module::MappedModule& mapped : Arcane::Module::MappedModules())
        {
            const void* const base = mapped.image.base;
            if (!base || mapped.image.size == 0 || base == loadedBase)
                continue;
            if (std::ranges::any_of(before, [base](const Arcane::Module::MappedModule& m) { return m.image.base == base; }))
                continue;
            const std::string own = OwnModuleName(mapped.path);
            if (own.empty() || own == cvarModule)
                continue;
            const std::size_t moved =
                Arcane::CVarRegistry::Get().ReattributeModuleRange(base, mapped.image.size, cvarModule, own);
            if (moved == 0)
                continue;
            ARC_TRACE("plugin: re-attributed {} registration(s) of dependency image '{}' from '{}' to '{}'",
                      moved, mapped.path.generic_string(), cvarModule, own);
            const std::lock_guard lock(g_dependencyModulesMutex);
            if (std::ranges::find(g_dependencyModules, own) == g_dependencyModules.end())
                g_dependencyModules.push_back(own);
        }
    }
}

namespace Arcane
{
    Plugin::Plugin(Module module, PluginVTable vtable, std::string cvarModule) noexcept
        : m_module(std::move(module)), m_vtable(vtable)
    {
        const Module::ImageSpan image = m_module.Image();
        m_cvars = CVarOwner(std::move(cvarModule), image.base, image.size);
    }

    bool AcceptPluginAbi(const PluginVTable& table, PluginResolveError* error) noexcept
    {
        const auto refuse = [&](std::uint32_t pluginAbi, std::uint64_t pluginDigest) -> bool
        {
            if (error)
            {
                error->kind         = PluginResolveError::Kind::AbiMismatch;
                error->pluginAbi    = pluginAbi;
                error->engineAbi    = kGamePluginABIVersion;
                error->pluginDigest = pluginDigest;
                error->engineDigest = kGamePluginABIDigest;
            }
            return false;
        };
        if (!table.ABIVersion) return refuse(0, 0);
        const std::uint32_t pluginAbi = table.ABIVersion();
        if (pluginAbi != kGamePluginABIVersion) return refuse(pluginAbi, 0);
        if (!table.ABIDigest) return refuse(pluginAbi, 0);
        const std::uint64_t pluginDigest = table.ABIDigest();
        if (pluginDigest != kGamePluginABIDigest) return refuse(pluginAbi, pluginDigest);
        return true;
    }

    bool AdoptStaticGameModule(const PluginVTable& table, PluginResolveError* error,
                               std::optional<Plugin>* out, std::string cvarModule)
    {
        if (!AcceptPluginAbi(table, error)) return false;
        if (out) out->emplace(Plugin::FromStatic(table, std::move(cvarModule)));
        return true;
    }

    std::vector<std::string> Plugin::DependencyCVarModules()
    {
        const std::lock_guard lock(g_dependencyModulesMutex);
        return g_dependencyModules;
    }

    Plugin Plugin::FromStatic(const PluginVTable& table, std::string cvarModule)
    {
        // Module{} is the empty handle: Image() reports {}, so the ctor's
        // CVarOwner falls back to name-only ownership (UnregisterModule).
        Plugin plugin(Module{}, table, std::move(cvarModule));
        plugin.m_staticTable = &table;
        return plugin;
    }

    std::optional<Plugin> Plugin::Load(std::filesystem::path path)
    {
        return Load(std::move(path), nullptr);
    }

    std::optional<Plugin> Plugin::Load(std::filesystem::path path, PluginResolveError* error)
    {
#if defined(_WIN32)
        // Build-flavor gate, from the FILE bytes and BEFORE Module::Load: a
        // config-mismatched DLL's static initializers run inside LoadLibrary
        // itself, so by the time an export could be checked the damage is done
        // (the desk signature was an AV inside VCRUNTIME140_1 at plugin_load).
        // Unknown fails OPEN -- only a positive cross-flavor verdict refuses.
        {
#if defined(_DEBUG)
            constexpr bool hostDebugCrt = true;
#else
            constexpr bool hostDebugCrt = false;
#endif
            std::string matched;
            const CrtFlavor flavor = Module::ScanFileCrtFlavor(path, &matched);
            if (flavor != CrtFlavor::Unknown && (flavor == CrtFlavor::Debug) != hostDebugCrt)
            {
                if (error)
                {
                    error->kind           = PluginResolveError::Kind::CrtFlavorMismatch;
                    error->symbol         = std::move(matched);
                    error->pluginDebugCrt = (flavor == CrtFlavor::Debug);
                    error->hostDebugCrt   = hostDebugCrt;
                }
                return std::nullopt;
            }
        }
#endif

        // The image's ARC_CVAR/ARC_COMMAND statics run INSIDE Module::Load, on
        // this thread (settings spec s4.3). Attribute them to the open
        // CVarModuleScope (PluginHost opens one naming the source dll), else to
        // this file's stem.
        const std::string cvarModule = CVarRegistry::ScopedModule().empty()
            ? path.stem().string()
            : std::string(CVarRegistry::ScopedModule());
        // An image this process has ALREADY mapped (an engine DLL named as a
        // plugin by mistake, say ArcaneClient.dll) runs no statics in this
        // Load: the loader only bumps its reference. Compare the pre-load
        // image span to the post-load span: a match means this Load owns
        // none of the registrations in that range and must not drop them.
        const Module::ImageSpan already = Module::MappedImage(path);
        // The mapped set before the load (Task 8 fix round 2): a refusal below
        // must also clean every DEPENDENCY image this load newly mapped (an
        // engine module the game imports but the host does not link). Their
        // statics ran inside Module::Load and they unmap with `module`.
        // Every platform, like MappedModules (macOS: shared-cache images left out).
        //
        // SINGLE LOADER THREAD (PR #20 review M8): "fresh" means "mapped between
        // this snapshot and the walk after the load", so an image ANOTHER thread
        // maps in that window counts as this load's. That is benign today, and
        // the contract is that it stays so: every engine load of a module that
        // registers into Core (game modules, project plugins, the engine
        // libraries they import) goes through Plugin::Load / Module::Load on the
        // host's main thread, and nothing else in the engine maps one -- the
        // only off-thread maps are system and toolchain libraries that register
        // nothing (glibc's NSS modules under a worker's getaddrinfo, libgcc_s at
        // a first unwind, driver libraries). Such an image is at worst pinned
        // for the life of the process on ELF and Mach-O (a leak, no strip:
        // there is nothing in its range to drop or re-name), and on Windows a
        // refusal or a load session's end drops nothing from it. A future
        // loader of an Arcane module on another thread must serialize with
        // this one; it would otherwise have its registrations re-named, pinned
        // or (Windows) dropped as this load's.
        const std::vector<Module::MappedModule> mappedBefore = Module::MappedModules();
        std::optional<Module> module;
        {
            const CVarModuleScope scope(cvarModule);
            module = Module::Load(std::move(path));
        }
        // Before the name drop just below: a dependency the load mapped keeps
        // its registrations under its own name (AdoptDependencyRegistrations).
        if constexpr (Module::PinsForProcessLife())
            AdoptDependencyRegistrations(mappedBefore, module ? module->Image().base : nullptr, cvarModule);
        if (!module)
        {
            // Load failed with no handle: statics may have run then the OS
            // unmapped. Name-drop only when this path was not already mapped.
            // A dependency the beside-exe retry mapped stays (RTLD_NODELETE /
            // a kept reference on ELF and Mach-O) and was re-named above, so
            // the drop does not reach it (M-P1).
            if (!already.base)
                CVarRegistry::Get().UnregisterModule(cvarModule);
            return std::nullopt;   // Kind::None; caller reads Module::LastLoadError()
        }

        const Module::ImageSpan loaded = module->Image();
        const bool alreadyMapped = already.base && already.base == loaded.base
            && already.size == loaded.size;

        PluginVTable vtable{};
        if (!ResolveGamePluginAbi(*module, vtable, error))
        {
            // Refused AFTER its statics ran: drop THIS image's registrations
            // by range (not by stem) before `module` unmaps the code they
            // point into. An already-mapped engine DLL shares this span with
            // the live owner -- leave it.
            if (!alreadyMapped)
                DropImageRegistrations(loaded.base, loaded.size, module->Path());
            // ...and the same for each image this load newly mapped, while
            // `module` still holds them: a registration left in Core would call
            // into freed code at the next Runtime constructor (AttachAllModules),
            // cvar or settings walk, or exit.
            for (const Module::MappedModule& mapped : Module::MappedModules())
            {
                const void* const base = mapped.image.base;
                if (!base || base == loaded.base)
                    continue;
                const bool fresh = std::ranges::none_of(mappedBefore, [base](const Module::MappedModule& m) {
                    return m.image.base == base;
                });
                if (!fresh)
                    continue;
                // ELF and Mach-O: the same rule as PluginHost's -- a fresh
                // dependency is pinned for the life of the process
                // (Module::PinsForProcessLife), so it does not unmap with
                // `module` and its registrations stay valid: dropping them would
                // strip a live engine library. Only when it cannot be pinned is
                // it treated as unmapping.
                if constexpr (Module::PinsForProcessLife())
                    if (Module::PinMapped(mapped))
                        continue;
                DropImageRegistrations(base, mapped.image.size, mapped.path);
            }
            return std::nullopt;
        }

        CVarRegistry::Get().RegisterModuleImage(cvarModule, loaded.base, loaded.size);
        return Plugin(std::move(*module), vtable, cvarModule);
    }
}
