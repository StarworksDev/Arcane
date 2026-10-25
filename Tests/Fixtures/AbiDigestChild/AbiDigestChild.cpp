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
// map on Windows, never dropped on Linux, where a game load pins its fresh
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
// Physics2D is still mapped (Linux: pinned for process life), it prints how
// many module-attach callbacks it still has registered
// (physics2dAttachAfterRefusal; 1 = intact, not purged).

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

    int RunSimHookMode(const char* module)
    {
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
        {
            Arcane::Runtime rt(*process);
            Arcane::PluginHost host(*process, std::filesystem::path(module));
            if (!host.AttachRuntime(rt))
                return 11;
            loaded = host.Load();
            mappedAtLoad = Physics2DMapped();
            afterLoad = Arcane::SimHooks().size();
            host.Unload();
            afterUnload = Arcane::SimHooks().size();
            // Windows: unmapped (the session pinned, disowned, let go). Linux:
            // still mapped, pinned for the life of the process with its
            // registrations intact (Module::PinsForProcessLife).
            mappedAfterUnload = Physics2DMapped();
            host.DetachRuntime(rt);
        }
        std::printf("loaded=%d\nsealed=%d\nphysics2dMappedBefore=%d\nphysics2dMappedAtLoad=%d\n"
                    "physics2dMappedAfterUnload=%d\nhooksBefore=%zu\nhooksAfterLoad=%zu\nhooksAfterUnload=%zu\n",
                    loaded ? 1 : 0, Arcane::SimHooksSealed() ? 1 : 0, mappedBefore ? 1 : 0, mappedAtLoad ? 1 : 0,
                    mappedAfterUnload ? 1 : 0, before, afterLoad, afterUnload);
        std::fflush(stdout);
        // A SECOND load session in the same process (re-review I-R1): the
        // dependency must have its registrations, exactly once. A dependency
        // that stayed mapped after its registrations were dropped would be
        // found pre-mapped and run without them (its statics never re-run);
        // one whose statics re-ran over live registrations would show two
        // attach callbacks. Counted (destructively, with the host's range nets)
        // while the second session still holds it.
        std::size_t attachSecondSession = 0, cvarsSecondSession = 0;
        bool loadedSecond = false;
        {
            Arcane::Runtime rt(*process);
            Arcane::PluginHost host(*process, std::filesystem::path(module));
            if (!host.AttachRuntime(rt))
                return 12;
            loadedSecond = host.Load();
            const Arcane::Module::ImageSpan image = Physics2DImage();
            if (image.base)
            {
                attachSecondSession = Arcane::UnregisterModuleAttachInRange(image.base, image.size);
                cvarsSecondSession = Arcane::CVarRegistry::Get().UnregisterModuleRange(image.base, image.size);
            }
            host.Unload();
            host.DetachRuntime(rt);
        }
        std::printf("loadedSecondSession=%d\nphysics2dAttachSecondSession=%zu\nphysics2dCVarsSecondSession=%zu\n",
                    loadedSecond ? 1 : 0, attachSecondSession, cvarsSecondSession);
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
        // Still mapped (pinned for process life on Linux): its module-attach
        // callback must still be registered, not purged by the refusal net.
        const Arcane::Module::ImageSpan image = Physics2DImage();
        const std::size_t attaches = image.base ? Arcane::UnregisterModuleAttachInRange(image.base, image.size) : 0;
        std::printf("physics2dAttachAfterRefusal=%zu\n", attaches);
        std::fflush(stdout);
        return 0;
    }
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--sim-hook") == 0)
        return RunSimHookMode(argv[2]);
    const bool thenRuntime = argc == 4 && std::strcmp(argv[3], "--then-runtime") == 0;
    if (argc < 3 || (argc > 3 && !thenRuntime))
    {
        std::fputs("usage: AbiDigestChild <module> <sentinel> [--then-runtime]\n", stderr);
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
