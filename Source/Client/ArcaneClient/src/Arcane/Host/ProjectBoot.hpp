#pragma once
#include <Arcane/Modules/ArcaneClientApi.hpp>

// Host-boot helpers used by any runtime host (ArcaneRuntime, the Arcane Editor) that
// boots the engine via GpuContext/HostConfig -- all three now live in the
// engine DLL as Arcane/Host, so a host consumes them rather than source-
// compiling its own copy. They turn the engine's layered config + an open
// project into the two boot decisions a host makes: which input map to
// load, and which game module to host.

#include <Arcane/Base/Engine.hpp>        // BuildInfo (engine identity probe)
#include <Arcane/Base/Log.hpp>           // ARC_WARN/ARC_ERROR/ARC_INFO (not pulled in transitively by any of the below)
#include <Arcane/Base/Runtime.hpp>       // Runtime::ResetRegistry/Registry (BootScene)
#include <Arcane/Config/Config.hpp>
#include <Arcane/Host/BootSequence.hpp>  // BootStage/BootThread/BootPolicy (CoreStages)
#include <Arcane/Host/HostConfig.hpp>     // HostConfig (OpenOptionsFor -- the ONE verify-run diag:// rule)
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSettings.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // kGamePluginABIVersion (engine identity probe)
#include <Arcane/Plugin/SystemFactory.hpp>   // SystemFactoryEntry (the --print-registry roster)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectHost.hpp>   // Core-DLL split Task 6: VerifySharedTypeContext/GameModule/
                                             // PluginModules/BootSceneFile/BootSceneResult/BootScene moved
                                             // here (ArcaneCore.dll) so ArcaneServer.exe -- Core-only -- can
                                             // reach them without linking ArcaneClient.dll. Re-exported into
                                             // Arcane::HostBoot below so every existing caller compiles
                                             // unchanged.
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // draw-marker flag, now a cvar read

#include <Json.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Arcane
{
    // Forward declarations for BootContext below. An elaborated-type-specifier
    // written from inside Arcane::HostBoot (e.g. "class GpuContext*") does
    // ordinary unqualified lookup; if no Arcane::GpuContext is visible yet at
    // that point (this header does not include GpuContext.hpp -- deliberately,
    // to stay light for every consumer that only wants the input/asset/scene
    // helpers), the compiler silently declares a NEW, distinct type in the
    // innermost enclosing namespace (Arcane::HostBoot::GpuContext), not
    // Arcane::GpuContext -- and which type you get would then depend on
    // whether GpuContext.hpp happened to be included first in a given TU, an
    // ODR hazard. These forward declarations pin the real Arcane:: types
    // regardless of include order. Runtime is included in full above (line 12)
    // and so is unaffected in practice, but is declared here too for the same
    // reason and for symmetry with GpuContext/BootSplashWindow.
    class Runtime;
    class ClientRuntime;
    class GpuContext;
    class BootSplashWindow;   // Task 7 defines this, in namespace Arcane
}

namespace Arcane::HostBoot
{
    struct GameplayInputLoadResult
    {
        enum class Status { Unconfigured, Loaded, Invalid };
        Status status = Status::Unconfigured;
        std::string diagnostic;
    };

    [[nodiscard]] ARC_CLIENT_API GameplayInputLoadResult LoadGameplayInput(
        ClientRuntime& runtime, const Project& project);

    // VerifySharedTypeContext moved to Arcane::ProjectHost (ArcaneCore.dll,
    // Core-DLL split Task 6) -- re-exported here so every existing
    // `HostBoot::VerifySharedTypeContext(...)` caller compiles unchanged. See
    // Arcane/Project/ProjectHost.hpp for the body and its full doc comment.
    using ProjectHost::VerifySharedTypeContext;

    // One-line JSON describing this engine build, for `--print-engine-info`.
    //
    // This exists so the Arcane Hub never HARDCODES a plugin ABI. A .arcproj
    // requires `engine.abi` (ProjectManifest.hpp), so a hub that guessed it
    // would mint stale-ABI projects the moment the engine bumps, and those
    // crash on open. The Hub probes, then stamps whatever the engine reports.
    //
    // Single line on purpose: the caller reads one line from stdout.
    //
    // `exePathUtf8` is UTF-8 BYTES, not a std::filesystem::path: callers pass
    // Arcane::ExecutablePathUtf8(). Taking a path here and calling
    // generic_string() would re-encode through the implementation's native narrow
    // encoding -- the ANSI-codepage round-trip that made this throw on non-ASCII
    // install paths in the first place. Backslashes are normalised here since we
    // no longer get generic_string()'s normalisation for free.
    inline std::string EngineInfoJson(std::string exePathUtf8)
    {
        std::replace(exePathUtf8.begin(), exePathUtf8.end(), '\\', '/');
        nlohmann::json j;
        // PluginABIVersion(), NOT the kGamePluginABIVersion header constant: the
        // constant is whatever THIS module was compiled against, while the gate
        // that rejects a plugin lives in Arcane.dll. A partially-updated install
        // would otherwise publish a number the runtime refuses.
        j["engineAbi"] = Arcane::PluginABIVersion();
        j["build"]     = Arcane::BuildInfo();
        j["exePath"]   = std::move(exePathUtf8);
        // error_handler_t::replace, not the default throw: this probe is the ONE
        // thing the Hub relies on to learn the ABI, so a malformed byte must
        // degrade to U+FFFD in the output, never an exception escaping main()
        // into terminate(). ExecutablePathUtf8 already yields well-formed UTF-8;
        // this is the belt for every other caller.
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    // One-line JSON of the booted roster, for `--print-registry` (engine
    // modularization s6, registrar survival): every component the registry
    // holds and every system factory, by name, each list sorted: a modular host
    // and the monolithic package must print the same line (names and counts).
    inline std::string RegistryJson(const Astra::ComponentRegistry& components,
                                    std::span<const SystemFactoryEntry> systems)
    {
        std::vector<std::string> componentNames;
        components.ForEachComponent([&](Astra::ComponentID, const Astra::ComponentDescriptor& d)
        {
            componentNames.emplace_back(d.name ? d.name : "");
        });
        std::vector<std::string> systemNames;
        systemNames.reserve(systems.size());
        for (const SystemFactoryEntry& e : systems)
            systemNames.push_back(e.name);
        std::ranges::sort(componentNames);
        std::ranges::sort(systemNames);
        nlohmann::json j;
        j["components"] = componentNames;
        j["systems"]    = systemNames;
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    // Load the input action maps from the layered config's "input" category (engine
    // default EngineConfig/input.json, deep-merged with the project's Config/input.json).
    // Sets the input.baseContext base context on success. Returns false if the category is
    // absent/malformed (the host logs and continues -- input stays inert).
    inline bool LoadInputConfig(Arcane::InputActions& input, const Arcane::Config& config)
    {
        if (!input.LoadJson(config.Category("input")))
            return false;
        input.SetBaseContext(Arcane::Settings<Arcane::InputSettings>().baseContext);
        return true;
    }

    // Apply the layered config's "diagnostics" category (engine default
    // EngineConfig/diagnostics.json, deep-merged with a project's
    // Config/diagnostics.json) to the render-side instrumentation.
    //
    // Only `drawMarkers` today: per-draw GPU markers, off by default because
    // they are a debugging aid, not telemetry -- they cost a marker pair per
    // batch run and exist for the sessions where someone is chasing a hang with
    // PIX/RenderDoc open. PASS-level scopes are unconditional and are NOT
    // configurable: they are what a crash report is built from, so a config file
    // must never be able to turn the diagnostics off.
    inline void ApplyDiagnosticsConfig(const Arcane::Config&, CVarContext ctx,
                                       const std::vector<std::string>& sets)
    {
        // Config layers were applied when the project opened (Runtime::OpenProject).
        // This stage only adds the command line, which outranks those layers.
        ApplyCVarCommandLine(CVarRegistry::Get(), sets, ctx);
        CVarRegistry::Get().Publish();
    }

    // GameModule, PluginModules, BootSceneFile (x2), BootSceneResult,
    // Detail::ApplySceneFile and BootScene (x2) all moved to
    // Arcane::ProjectHost (ArcaneCore.dll, Core-DLL split Task 6) -- see
    // Arcane/Project/ProjectHost.hpp for every body and its full doc comment.
    // Re-exported here so every existing `HostBoot::GameModule(...)` /
    // `HostBoot::PluginModules(...)` / `HostBoot::BootScene(...)` /
    // `HostBoot::BootSceneFile(...)` caller compiles unchanged; `Detail` is a
    // ProjectHost-only implementation namespace and is NOT re-exported (no
    // caller outside BootScene itself ever named it).
    using ProjectHost::GameModule;
    using ProjectHost::PluginModules;
    using ProjectHost::BootSceneFile;
    using ProjectHost::BootSceneResult;
    using ProjectHost::BootScene;

    // THE ONE RULE for whether a host wants diag:// mounted, derived from its
    // parsed command line. Lives here, as a shared free function, because it
    // has THREE population sites -- RuntimeApp::Run, EditorApp::Init and
    // EditorApp::SwitchProject -- and three hand-copied predicates would drift
    // silently: the editor lane is the one that actually matters, and a miss
    // there leaves the golden gate exactly as red as it was while every unit
    // test still passes.
    //
    // A run declines the mount only when it is a VERIFY run: headless AND
    // producing an artifact that a comparison or a report will read
    // (--compare or --report). An ordinary headless run keeps diag:// -- it is
    // real content a developer may want -- and no windowed session is ever
    // affected. See ProjectOpenOptions.hpp for the defect this closes.
    inline ProjectOpenOptions OpenOptionsFor(const HostConfig& cfg)
    {
        ProjectOpenOptions opts;
        opts.mountDiagnostics =
            !(cfg.headless && (!cfg.compareReference.empty() || !cfg.reportPath.empty()));
        return opts;
    }

    // What a boot stage needs to do its work. Pointers are host-owned and
    // outlive the sequence; null members mean "that facility is absent in this
    // host", which stages must tolerate (the parity tests build one with all
    // members null).
    struct BootContext
    {
        Runtime*          runtime     = nullptr;
        GpuContext*       gpu         = nullptr;
        BootSplashWindow* splash      = nullptr;   // pre-device splash; closed by splash_ready
        const char*       projectPath = nullptr;
        const char*       pluginPath  = nullptr;
        // "ArcaneEditor.exe" / "ArcaneRuntime.exe" (Task 8): the identity the
        // genuinely-shared stage bodies (type_context_install, project_open,
        // input_config) log under -- VerifySharedTypeContext's diagnostic and
        // the --project-failed warning both need to say which host they ran
        // in. Null degrades to a generic "HostBoot" label, never a crash.
        const char*       moduleName  = nullptr;

        // Per-open engine settings forwarded to Runtime::OpenProject by the
        // project_open stage body (and by its RuntimeStages override). Populated
        // by each host from its own HostConfig via OpenOptionsFor above; the
        // default here is the ordinary every-mount open, so a context built
        // without one (the parity tests) behaves exactly as before.
        ProjectOpenOptions openOptions{};

        // `--set` context (settings spec s3.2). The editor sets Editor; the
        // runtime passes CommandLineCVarContext() (CVarConfig.hpp): Editor in a
        // Debug/Release build, the local host of its own session in Dist
        // (integration ruling I3; spec s8.3).
        // Null hostConfig means there is no command line (parity tests).
        const HostConfig* hostConfig = nullptr;
        CVarContext cvarContext = CVarContext::LocalHost;
    };

    // THE CANONICAL BOOT SEQUENCE. Both hosts take this LIST whole: the ids,
    // dependsOn, thread and weight below may not be omitted, reordered, or
    // rewritten by a host -- so divergence between the editor and the runtime
    // has to be written deliberately instead of forgotten. Three shipped bugs
    // (camera, sprite tables, Astra TypeContext) were exactly that forgetting.
    //
    // A stage's BODY is a different story (Task 8, 2026-07-30 review). Some
    // ids get a real, shared implementation right here (type_context_install,
    // project_open, input_config, and the editor-only editor_lock) because
    // their work is fully expressible through BootContext alone. The rest
    // are declared here with NO run callable (Make's `run` parameter
    // defaults to an empty std::function) because their real work needs a
    // HOST-OWNED object (EditorApp::m_gpu/m_runtime/m_plugin/m_resolver/
    // m_splash, ArcaneRuntime's equivalents) or an editor-exe-only type
    // (EditorTheme/EditorFonts/ShaderEditorDocument) that this module,
    // compiled into Arcane.dll, cannot reach or see -- Arcane.dll cannot
    // depend on ArcaneEditor.exe. Each host is REQUIRED to overwrite that
    // stage's `.run` by id, after calling EditorStages/RuntimeStages, before
    // constructing a BootSequence (see EditorApp::Run / RuntimeApp::Run).
    // The editor-only splash_ready is one of these host-owned ids: its
    // splash members are host state. See EditorStages' splash_ready push_back
    // for the reveal-ordering reasoning and EditorApp::StageSplashReady for
    // the body.
    //
    // An id with no host override left empty is NOT tolerated silently: Make()
    // substitutes a sentinel body that logs ARC_ERROR naming the exact id and
    // returns false, so a Fatal stage hard-aborts boot instead of the host
    // quietly skipping the step and reporting success -- the sentinel exists
    // BECAUSE "a host forgot a step and nothing said so" is the literal shape
    // of all three shipped bugs above, and BootStageParityTest's id-only
    // comparison cannot catch a host that received the right id but never
    // patched it. A host for which a given id is LEGITIMATELY a no-op (e.g.
    // ArcaneRuntime's edit_core, which has no scene-session/undo-history
    // analog) must patch it to an explicit `[]{ return true; }` with a
    // comment saying so -- relying on the sentinel to happen to look like
    // success is exactly what this paragraph exists to forbid.
    //
    // Adding an engine-wide install/publish step? Add the id HERE (with a
    // shared body if one is possible) and both hosts' lists gain it;
    // BootStageParityTest fails if a host's id list drops one, and the
    // sentinel fails loudly if a host's id list keeps it but never patches
    // (or renames/typos) it.
    [[nodiscard]] ARC_CLIENT_API std::vector<BootStage> CoreStages(BootContext& ctx);

    // Ids only -- no context needed, so tests and tooling can ask "what is the
    // canonical list?" without constructing a host.
    [[nodiscard]] ARC_CLIENT_API std::vector<std::string> CoreStageIds();

    // Exactly what each host builds, exposed for BootStageParityTest. These must
    // be the SAME functions the hosts call, not reimplementations -- a parallel
    // copy would test itself and prove nothing.
    [[nodiscard]] ARC_CLIENT_API std::vector<BootStage> EditorStages(BootContext& ctx);
    [[nodiscard]] ARC_CLIENT_API std::vector<BootStage> RuntimeStages(BootContext& ctx);
    [[nodiscard]] ARC_CLIENT_API std::vector<std::string> EditorStageIdsForTest(BootContext& ctx);
    [[nodiscard]] ARC_CLIENT_API std::vector<std::string> RuntimeStageIdsForTest(BootContext& ctx);
}
