// 3D-readiness Plan B Task 2: the disposable child for the plugin ABI-digest
// load gate. Usage: AbiDigestChild <module> <sentinel>
// Calls Plugin::Load on the module and prints the PluginResolveError as
// `key=value` lines (loaded, kind, symbol, pluginAbi, engineAbi, pluginDigest,
// engineDigest; digests as 16 hex digits), so the parent can tell a refusal at
// the digest compare (equal epochs, different digests) from one at the epoch.
// Exits 0 exactly when the load was refused with AbiMismatch. If the load
// SUCCEEDS, the child calls Init(nullptr): every fixture's Init writes the
// sentinel, so a wrong accept leaves the sentinel the parent checks for. The
// parent test spawns it once per fixture DLL; a module's header registrars may
// run inside this process at LoadLibrary, which is why the load never happens
// in ArcaneTests.
//
// Second mode (3D-readiness Plan A Task 8 fix round 1): AbiDigestChild
// --sim-hook <module>. This exe links ArcaneEngine only; ArcanePhysics2D is
// staged beside it, not linked, so a game module that imports it maps it at
// load, AFTER ProcessContext::Create sealed the sim hooks. The child loads the
// module through a real PluginHost, prints the hook count before the load,
// after it and after the unload, whether ArcanePhysics2D was mapped, then
// builds a fresh Runtime and runs ClearSystems, EnsurePhysics and one 1/60
// Advance. Exits 0 when the hook list never changed and nothing faulted.
// Between the two, a SECOND load session in the same process counts
// Physics2D's module-attach callbacks and cvar registrations while loaded
// (physics2dAttachSecondSession, physics2dCVarsSecondSession): exactly one
// attach callback and some cvars on every platform -- re-registered by a fresh
// map on Windows, never dropped on Linux and macOS, where a game load pins its fresh
// engine dependencies for the life of the process (Module::PinsForProcessLife).
//
// Third form (Task 8 fix round 2): AbiDigestChild <module> <sentinel>
// --then-runtime. The default mode, then the proof that a REFUSED load left
// nothing behind from a dependency image it mapped and unmapped (AbiDigestStale
// imports ArcanePhysics2D, which this exe does not link): it prints whether
// ArcanePhysics2D is still mapped, then creates a ProcessContext and a Runtime
// (AttachAllModules), runs ClearSystems, EnsurePhysics and one 1/60 Advance,
// and walks every module's log-level setter (ApplyLogSettings). Each of those
// calls a registration the dependency image may have left in Core. Last, when
// Physics2D is still mapped (Linux and macOS: pinned for process life), it prints how
// many module-attach callbacks it still has registered
// (physics2dAttachAfterRefusal; 1 = intact, not purged).
//
// Fourth form (issue #21): AbiDigestChild <module> <sentinel> --pin. Whether a
// Module::PinMapped pin keeps an image loaded once every other reference is
// gone (RunPinMode).
//
// The --sim-hook mode also takes an optional SECOND module (issue #21: what
// the second session loads, a different stem that also imports
// ArcanePhysics2D) and prints, for the PR #20 re-review minors:
//   pluginMappedAtLoad / pluginMappedAfterUnload (M-P5): the game's own temp
//     copy, matched by its file name, is mapped while loaded and gone after
//     Unload on every platform -- the dependency rule never pins the game;
//   physics2dCVarModule: the module Physics2D's settings cvar is declared by
//     after the first load (ArcanePhysics2D on Linux and macOS, where
//     Plugin::Load re-attributes a dependency's registrations; the loading
//     module's name on Windows);
//   physics2dLayeredAtLoad: that cvar after the first load, which layered
//     --set physics.ground.minNormalY=0.25 (the dependency is layered);
//   physics2dCVarAfterFailedLoad (M-P1): whether that cvar survives a FAILED
//     Plugin::Load under the first module's scope name (its name drop);
//   physics2dLayeredSecondSession (M-P2): that cvar after the second module
//     loaded with --set ...=0.75.

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/Bindings/LogBinding.hpp>
#include <Arcane/Config/CVarRegistry.hpp>   // UnregisterModuleRange: the registration count
#include <Arcane/Core/ModuleAttach.hpp>    // UnregisterModuleAttachInRange: the registration count
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Plugin/Plugin.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Platform/Platform.hpp>   // SharedLibraryFileName
#include <Arcane/Sim/SimHook.hpp>
#include <Astra/Core/TypeContext.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    // ArcanePhysics2D's mapped image, or {nullptr, 0} when it is not mapped.
    Arcane::Module::ImageSpan Physics2DImage()
    {
        // A LINKED engine library: ArcanePhysics2D.dll, libArcanePhysics2D.so.
        const std::string file = Arcane::Platform::SharedLibraryFileName("ArcanePhysics2D");
        for (const Arcane::Module::MappedModule& m : Arcane::Module::MappedModules())
            if (m.path.filename().string() == file)
                return m.image;
        return {};
    }

    bool Physics2DMapped() { return Physics2DImage().base != nullptr; }

    // A Physics2D settings cvar (PhysicsQuerySettings.hpp, ARC_SETTINGS): its
    // declaring module and its published value.
    constexpr const char* kPhysics2DCVar = "physics.ground.minNormalY";

    std::string Physics2DCVarModule()
    {
        return Arcane::CVarRegistry::Get().ModuleOf(Arcane::CVarRegistry::Get().Find(kPhysics2DCVar));
    }

    std::string Physics2DCVarValue()
    {
        const auto value = Arcane::CVarRegistry::Get().Get(Arcane::CVarRegistry::Get().Find(kPhysics2DCVar));
        if (!value || value->type != Arcane::CVarType::Float32)
            return "none";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(value->AsFloat32()));
        return buf;
    }

    // The file names of the images mapped now that were not in `before` and
    // whose name starts with `prefix`: the host's temp copy of a module
    // (PluginHost's CopyVersioned, "<stem>_<gen>.dll").
    std::vector<std::string> FreshImageNames(const std::vector<Arcane::Module::MappedModule>& before,
                                             const std::string& prefix)
    {
        std::vector<std::string> names;
        for (const Arcane::Module::MappedModule& m : Arcane::Module::MappedModules())
        {
            const std::string name = m.path.filename().string();
            if (name.rfind(prefix, 0) != 0)
                continue;
            bool old = false;
            for (const Arcane::Module::MappedModule& b : before)
                old = old || b.image.base == m.image.base;
            if (!old)
                names.push_back(name);
        }
        return names;
    }

    bool AnyImageNamed(const std::vector<std::string>& names)
    {
        for (const Arcane::Module::MappedModule& m : Arcane::Module::MappedModules())
            for (const std::string& name : names)
                if (m.path.filename().string() == name)
                    return true;
        return false;
    }

    int RunSimHookMode(const char* module, const char* secondModule)
    {
        const std::string stem = std::filesystem::path(module).stem().string();
        auto process = Arcane::ProcessContext::Create({});
        if (!process)
            return 10;
        // This exe's own Astra slot, as every host sets its own (R13): the inline
        // RunLoop / Registry code below runs in this image.
        Astra::SetTypeContext(&process->TypeContext(), Astra::ModuleResidency::Resident);
        const std::size_t before = Arcane::SimHooks().size();
        const bool mappedBefore = Physics2DMapped();
        std::size_t afterLoad = 0, afterUnload = 0;
        bool loaded = false, mappedAtLoad = false, mappedAfterUnload = true;
        bool pluginMappedAtLoad = false, pluginMappedAfterUnload = true;
        std::string cvarModule, layeredAtLoad;
        {
            Arcane::Runtime rt(*process);
            rt.SetCVarCommandLine({ std::string(kPhysics2DCVar) + "=0.25" }, Arcane::CVarContext::Editor);
            Arcane::PluginHost host(*process, std::filesystem::path(module));
            if (!host.AttachRuntime(rt))
                return 11;
            const std::vector<Arcane::Module::MappedModule> imagesBefore = Arcane::Module::MappedModules();
            loaded = host.Load();
            mappedAtLoad = Physics2DMapped();
            const std::vector<std::string> pluginImages = FreshImageNames(imagesBefore, stem + "_");
            pluginMappedAtLoad = !pluginImages.empty();
            cvarModule = Physics2DCVarModule();
            layeredAtLoad = Physics2DCVarValue();
            afterLoad = Arcane::SimHooks().size();
            host.Unload();
            afterUnload = Arcane::SimHooks().size();
            // Windows: unmapped (the session pinned, disowned, let go). Linux and macOS:
            // still mapped, pinned for the life of the process with its
            // registrations intact (Module::PinsForProcessLife).
            mappedAfterUnload = Physics2DMapped();
            // The game's OWN image is never pinned (PR #20 re-review M-P5):
            // its temp copy is gone with its last reference on every platform.
            pluginMappedAfterUnload = AnyImageNamed(pluginImages);
            host.DetachRuntime(rt);
        }
        std::printf("loaded=%d\nsealed=%d\nphysics2dMappedBefore=%d\nphysics2dMappedAtLoad=%d\n"
                    "physics2dMappedAfterUnload=%d\nhooksBefore=%zu\nhooksAfterLoad=%zu\nhooksAfterUnload=%zu\n"
                    "pluginMappedAtLoad=%d\npluginMappedAfterUnload=%d\nphysics2dCVarModule=%s\n"
                    "physics2dLayeredAtLoad=%s\n",
                    loaded ? 1 : 0, Arcane::SimHooksSealed() ? 1 : 0, mappedBefore ? 1 : 0, mappedAtLoad ? 1 : 0,
                    mappedAfterUnload ? 1 : 0, before, afterLoad, afterUnload, pluginMappedAtLoad ? 1 : 0,
                    pluginMappedAfterUnload ? 1 : 0, cvarModule.c_str(), layeredAtLoad.c_str());
        std::fflush(stdout);
        // A FAILED load under the first module's name (M-P1): a hot reload of
        // a broken build ends the same way, in Plugin::Load's name drop. It
        // must not strip a dependency that stays mapped.
        {
            const Arcane::CVarModuleScope scope(stem);
            const std::filesystem::path missing = std::filesystem::path(module).parent_path() /
                (stem + "_missing" + std::filesystem::path(module).extension().string());
            const bool failed = !Arcane::Plugin::Load(missing).has_value();
            std::printf("failedLoadFailed=%d\nphysics2dCVarAfterFailedLoad=%d\n", failed ? 1 : 0,
                        Arcane::CVarRegistry::Get().Find(kPhysics2DCVar).IsStale() ? 0 : 1);
            std::fflush(stdout);
        }
        // A SECOND load session in the same process (re-review I-R1): the
        // dependency must have its registrations, exactly once. A dependency
        // that stayed mapped after its registrations were dropped would be
        // found pre-mapped and run without them (its statics never re-run);
        // one whose statics re-ran over live registrations would show two
        // attach callbacks. Counted (destructively, with the host's range nets)
        // while the second session still holds it.
        std::size_t attachSecondSession = 0, cvarsSecondSession = 0;
        bool loadedSecond = false;
        std::string layeredSecondSession;
        {
            Arcane::Runtime rt(*process);
            rt.SetCVarCommandLine({ std::string(kPhysics2DCVar) + "=0.75" }, Arcane::CVarContext::Editor);
            Arcane::PluginHost host(*process, std::filesystem::path(secondModule));
            if (!host.AttachRuntime(rt))
                return 12;
            loadedSecond = host.Load();
            layeredSecondSession = Physics2DCVarValue();   // before the destructive count below
            const Arcane::Module::ImageSpan image = Physics2DImage();
            if (image.base)
            {
                attachSecondSession = Arcane::UnregisterModuleAttachInRange(image.base, image.size);
                cvarsSecondSession = Arcane::CVarRegistry::Get().UnregisterModuleRange(image.base, image.size);
            }
            host.Unload();
            host.DetachRuntime(rt);
        }
        std::printf("loadedSecondSession=%d\nphysics2dLayeredSecondSession=%s\nphysics2dAttachSecondSession=%zu\n"
                    "physics2dCVarsSecondSession=%zu\n",
                    loadedSecond ? 1 : 0, layeredSecondSession.c_str(), attachSecondSession, cvarsSecondSession);
        std::fflush(stdout);
        // A fresh world after the unload: nothing the module left may run.
        {
            Arcane::Runtime rt(*process);
            rt.ClearSystems();
            rt.EnsurePhysics();
            (void)rt.Loop().Advance(1.0 / 60.0);
        }
        std::printf("advanced=1\n");
        std::fflush(stdout);
        const bool ok = loaded && loadedSecond && before == afterLoad && before == afterUnload;
        return ok ? 0 : 1;
    }

    // After a refused load: nothing a dependency image registered may run.
    int RunAfterRefusal()
    {
        std::printf("physics2dMappedAfterRefusal=%d\n", Physics2DMapped() ? 1 : 0);
        std::fflush(stdout);
        auto process = Arcane::ProcessContext::Create({});
        if (!process)
            return 10;
        Astra::SetTypeContext(&process->TypeContext(), Astra::ModuleResidency::Resident);
        {
            Arcane::Runtime rt(*process);   // AttachAllModules
            rt.ClearSystems();
            rt.EnsurePhysics();
            (void)rt.Loop().Advance(1.0 / 60.0);
        }
        Arcane::ApplyLogSettings(Arcane::LogSettings{});   // every module's level setter
        std::printf("runtimeAfterRefusal=1\n");
        // Still mapped (pinned for process life on Linux and macOS): its module-attach
        // callback must still be registered, not purged by the refusal net.
        const Arcane::Module::ImageSpan image = Physics2DImage();
        const std::size_t attaches = image.base ? Arcane::UnregisterModuleAttachInRange(image.base, image.size) : 0;
        std::printf("physics2dAttachAfterRefusal=%zu\n", attaches);
        std::fflush(stdout);
        return 0;
    }

    // Fourth form (issue #21): AbiDigestChild <module> <sentinel> --pin, on a
    // fixture that imports nothing this exe does not link (AbiDigestOldEpoch).
    // CONTROL: Plugin::Load refuses it (dropping its registrations) and lets it
    // go; with nothing pinning it the image must unload, so the pin check
    // below is not vacuous (the fixture is built unloadable: -fno-gnu-unique,
    // no thread-locals on macOS). PIN: the shape PluginHost and Plugin::Load's
    // refusal net use -- the image listed by MappedModules at its Image()
    // base, PinMapped on that entry, the returned Module discarded -- then the
    // loading reference goes too. Whether it is still loaded is the verdict:
    // ELF and Mach-O pin for process life, Windows hands back one reference.
    // Ends with _Exit: on Windows the image has just unmapped under its
    // header registrars' entries, and no exit-time walk may reach them.
    int RunPinMode(const char* modulePath)
    {
        const std::filesystem::path path(modulePath);
        {
            const auto refused = Arcane::Plugin::Load(path);
            std::printf("controlLoaded=%d\n", refused.has_value() ? 1 : 0);
        }
        std::printf("controlMappedAfterRelease=%d\n", Arcane::Module::IsMapped(path) ? 1 : 0);
        bool listed = false, pinned = false;
        {
            const auto module = Arcane::Module::Load(path);
            if (!module)
            {
                std::printf("pinLoadError=%s\n", Arcane::Module::LastLoadError().c_str());
                std::fflush(stdout);
                std::_Exit(3);
            }
            const Arcane::Module::ImageSpan image = module->Image();
            for (const Arcane::Module::MappedModule& m : Arcane::Module::MappedModules())
                if (image.base && m.image.base == image.base)
                {
                    listed = true;
                    pinned = Arcane::Module::PinMapped(m).has_value();   // discarded, as the callers do
                    break;
                }
        }   // the loading reference goes
        std::printf("pinListed=%d\npinTaken=%d\npinMappedAfterRelease=%d\n",
                    listed ? 1 : 0, pinned ? 1 : 0, Arcane::Module::IsMapped(path) ? 1 : 0);
        std::fflush(stdout);
        std::_Exit(0);
    }
}

int main(int argc, char** argv)
{
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "--sim-hook") == 0)
        return RunSimHookMode(argv[2], argc == 4 ? argv[3] : argv[2]);
    if (argc == 4 && std::strcmp(argv[3], "--pin") == 0)
        return RunPinMode(argv[1]);
    const bool thenRuntime = argc == 4 && std::strcmp(argv[3], "--then-runtime") == 0;
    if (argc < 3 || (argc > 3 && !thenRuntime))
    {
        std::fputs("usage: AbiDigestChild <module> <sentinel> [--then-runtime | --pin]\n", stderr);
        return 2;
    }
#if defined(_WIN32)
    _putenv_s("ARC_ABI_DIGEST_SENTINEL", argv[2]);
#else
    setenv("ARC_ABI_DIGEST_SENTINEL", argv[2], 1);
#endif
    Arcane::PluginResolveError error{};
    auto plugin = Arcane::Plugin::Load(std::filesystem::path(argv[1]), &error);

    const char* kind = "None";
    switch (error.kind)
    {
        case Arcane::PluginResolveError::Kind::None:              kind = "None"; break;
        case Arcane::PluginResolveError::Kind::MissingExport:     kind = "MissingExport"; break;
        case Arcane::PluginResolveError::Kind::AbiMismatch:       kind = "AbiMismatch"; break;
        case Arcane::PluginResolveError::Kind::CrtFlavorMismatch: kind = "CrtFlavorMismatch"; break;
    }
    std::printf("loaded=%d\nkind=%s\nsymbol=%s\npluginAbi=%u\nengineAbi=%u\npluginDigest=%016llx\nengineDigest=%016llx\n",
                plugin.has_value() ? 1 : 0, kind, error.symbol.c_str(),
                static_cast<unsigned>(error.pluginAbi), static_cast<unsigned>(error.engineAbi),
                static_cast<unsigned long long>(error.pluginDigest),
                static_cast<unsigned long long>(error.engineDigest));
    std::fflush(stdout);
    // A wrong accept: run Init so the fixture writes the sentinel. The fixtures'
    // Init ignores the context.
    if (plugin.has_value() && plugin->VTable().Init)
        plugin->VTable().Init(nullptr);
    const bool refused = !plugin.has_value() && error.kind == Arcane::PluginResolveError::Kind::AbiMismatch;
    if (thenRuntime && refused)
        return RunAfterRefusal();
    return refused ? 0 : 1;
}
