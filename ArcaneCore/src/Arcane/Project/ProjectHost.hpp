#pragma once

// ProjectHost: the Core-DLL split's Task 6 extraction of five host-boot helpers
// out of ArcaneClient's ProjectBoot.hpp (Arcane::HostBoot) into Core, so a
// Core-only host (ArcaneServer.exe) can open a project, resolve its game
// module, and load its boot scene WITHOUT linking ArcaneClient.dll.
//
// Moved VERBATIM (spec docs/specs/2026-09-15-core-dll-split-design.md, plan 1
// Task 6): VerifySharedTypeContext, GameModule, PluginModules, both
// BootSceneFile overloads, BootSceneResult, Detail::ApplySceneFile, both
// BootScene overloads. Every one of these already only touched Core headers
// (Project.hpp, AssetId.hpp, SceneAsset.hpp, Components.hpp, Log.hpp,
// Runtime.hpp -- all Core) -- ProjectBoot.hpp's five bodies never actually
// needed anything ArcaneClient-side, so the move is a pure relocation, not a
// rewrite.
//
// NOT MOVED: EngineInfoJson, LoadInputConfig, ApplyDiagnosticsConfig,
// OpenOptionsFor, BootContext, CoreStages/EditorStages/RuntimeStages. Those
// stay in Arcane::HostBoot (ArcaneClient.dll) -- EngineInfoJson because every
// host (including this one) still probes it via its OWN three-key JSON dump
// rather than linking Client just for one string function; the rest because
// they are GpuContext/BootSplashWindow/HostConfig-shaped, i.e. presentation.
//
// ProjectBoot.hpp keeps `using ProjectHost::X;` re-exports inside
// Arcane::HostBoot for every symbol moved here, so every existing
// `HostBoot::GameModule(...)` etc. caller compiles unchanged.

#include <Arcane/Base/Log.hpp>           // ARC_WARN/ARC_ERROR/ARC_INFO
#include <Arcane/Base/Runtime.hpp>       // Runtime::ResetRegistry/Registry (BootScene)
#include <Arcane/Platform/Platform.hpp>  // NativeModuleFileName/ModuleFileName (GameModule, PluginModules)
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (BootSceneFile)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/Components.hpp>            // Arcane::Transform (VerifySharedTypeContextFor's default probe)
#include <Arcane/Scene/EngineRoster.hpp>          // EngineComponentRoster (VerifySharedTypeContext checks EVERY roster type)
#include <Arcane/Serialization/SceneAsset.hpp>    // ReadSceneFile/ApplySceneDocument/CreateEmpty (BootScene)

#include <Astra/Component/ComponentRegistry.hpp>  // GetComponentIDFromHash (VerifySharedTypeContext)
#include <Astra/Core/TypeID.hpp>                  // TypeID<T>::Value/Hash/Name (ditto)

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Arcane::ProjectHost
{
    // Is THIS module on the same Astra TypeContext as the engine's registry?
    //
    // Astra resolves GetTypeContext()/SetTypeContext() through a PER-MODULE static
    // slot by design, so every binary that touches a component type must install the
    // shared context itself (Runtime's ctor covers Arcane.dll only). A host that
    // forgets gets its own empty DefaultTypeContext, and its TypeID<T>::Value()
    // assigns ids from a private counter -- which silently ALIAS the shared ids.
    // The failure is not a crash and not a miss: a view returns the wrong
    // component's entities and reinterprets its bytes.
    //
    // That cost an evening (2026-07-30): ArcaneRuntime.exe lacked the install, so
    // its TypeID<Camera> aliased Transform, ActiveSceneCamera read position.x as
    // orthographicSize, saw 0, reported "no usable camera", and every scene rendered
    // at 1 px per metre -- indistinguishable from "my sprite is missing".
    //
    // Being inline, this compiles into the CALLER's module, which is the only place
    // the question can be asked. Call it once at boot, after the Runtime exists.
    // Compares the caller-module id for a known engine component against the id the
    // registry (populated inside Arcane.dll) holds for the same STABLE name hash.
    // Returns false and logs ARC_ERROR on mismatch; true when correct or when the
    // component is not registered yet (nothing to contradict).
    //
    // ONE PROBE WAS NOT ENOUGH (2026-09-16, the second lesson). A module does not
    // get its private ids all at once: TypeID<T>::Value() caches PER TYPE in a magic
    // static, so a module that resolved SOME types too early is wrong about exactly
    // those and right about the rest. ArcaneEditor.exe declared EditModeSchedule as
    // a plain EditorApp member; its ctor's AddSystem<TransformPropagationSystem>
    // resolved WorldTransform (and the rest of that system's mask) in the exe's
    // private DefaultTypeContext BEFORE StageRuntimeCreate installed the shared one,
    // while Transform -- the old single probe -- still matched by first-touch luck.
    // The viewport rendered EMPTY with RenderErrorCount 0. So the check below walks
    // the WHOLE EngineComponentRoster and reports every mismatching type; use
    // VerifySharedTypeContextFor<T> only when a single named type is the question.
    template<typename Probe = Arcane::Transform>
    inline bool VerifySharedTypeContextFor(const Astra::Registry& reg, const char* moduleName)
    {
        const auto* creg = reg.GetComponentRegistry();
        if (!creg)
            return true;
        const auto shared = creg->GetComponentIDFromHash(Astra::TypeID<Probe>::Hash());
        if (shared.IsErr())
            return true;   // not registered yet -- no contradiction to report
        const Astra::ComponentID mine = Astra::TypeID<Probe>::Value();
        if (mine == *shared.GetValue())
            return true;
        ARC_ERROR("{}: this module is NOT on the engine's Astra TypeContext "
                  "({} resolves to id {} here but id {} in the registry). Call "
                  "Astra::SetTypeContext(ctx) in this module at boot -- every "
                  "component view and GetComponent in it is reading the WRONG "
                  "component's memory.",
                  moduleName, Astra::TypeID<Probe>::Name(),
                  (unsigned)mine, (unsigned)*shared.GetValue());
        return false;
    }

    namespace Detail
    {
        // Expand the roster pack: check EVERY type, do not stop at the first bad
        // one, so the log names all of them (which type mismatched is the whole
        // diagnostic -- it points straight at what resolved early).
        template<typename... Ts>
        inline bool VerifyRoster(const Astra::Registry& reg, const char* moduleName, Arcane::TypeList<Ts...>)
        {
            bool ok = true;
            ((ok = VerifySharedTypeContextFor<Ts>(reg, moduleName) && ok), ...);
            return ok;
        }
    }

    // The boot check every host calls FROM ITS OWN TRANSLATION UNIT (being inline,
    // it asks about the calling module's caches and no other -- ArcaneClient.dll's
    // call in ProjectBoot.cpp cannot answer for ArcaneEditor.exe). Checks all thirteen
    // engine component types named in Scene/EngineRoster.hpp.
    inline bool VerifySharedTypeContext(const Astra::Registry& reg, const char* moduleName)
    {
        return Detail::VerifyRoster(reg, moduleName, Arcane::EngineComponentRoster{});
    }

    // The game module to host: the project's gameModule when a project is open and it
    // names one, else the fallback --plugin path.
    //
    // A project builds its own game DLL into <project>/Binaries/ (engine-as-SDK model),
    // so when that built copy exists we return its absolute path -- the host loads the
    // project's OWN module rather than a same-named DLL sitting beside the exe. If the
    // Binaries/ copy isn't there (a demo project that BORROWS a host-adjacent DLL, e.g.
    // ReferenceProject -> Sandbox.dll), we fall through to the bare name resolved beside the
    // host exe -- keeping the borrowing path working.
    inline std::string GameModule(const Arcane::Project* project, const std::string& fallback)
    {
        if (project && !project->Manifest().gameModule.empty())
        {
            // The manifest's authored name, in this platform's spelling
            // (Name.dll -> Name.so off-Windows; identity on Windows).
            const std::string mod = Arcane::Platform::NativeModuleFileName(project->Manifest().gameModule);
            std::error_code ec;
            const std::filesystem::path built = project->Root() / "Binaries" / mod;
            if (std::filesystem::exists(built, ec))
                return built.string();
            return mod;
        }
        return fallback;
    }

    // The secondary plugin modules a host should load: each enabled manifest plugin that
    // has built a DLL at <root>/Plugins/<name>/Binaries/<name>.dll. A content-only plugin
    // (no Source/ -> no DLL) contributes only its plugin:// content mount (added at
    // Project::Open) and is skipped here. Feed each to PluginHost::AddPlugin before Load().
    inline std::vector<std::filesystem::path> PluginModules(const Arcane::Project* project)
    {
        std::vector<std::filesystem::path> out;
        if (!project)
            return out;
        std::error_code ec;
        for (const auto& ref : project->Manifest().plugins)
        {
            if (!ref.enabled)
                continue;
            std::filesystem::path dll = project->Root() / "Plugins" / ref.name / "Binaries" / Arcane::Platform::ModuleFileName(ref.name);
            if (std::filesystem::exists(dll, ec))
                out.push_back(std::move(dll));
        }
        return out;
    }

    // The project's boot scene as a physical file, or empty when it has none /
    // the id names nothing this project contains.
    //
    // Split out from BootScene so the RESOLUTION is unit-testable without a
    // Runtime: it is the part with the interesting failure modes.
    inline std::filesystem::path BootSceneFile(const Arcane::Project& project)
    {
        const std::string& text = project.Manifest().bootScene;
        if (text.empty()) return {};

        const std::optional<Arcane::Guid> id = Arcane::Guid::FromString(text);
        if (!id || !id->IsValid())
        {
            ARC_WARN("bootScene '{}' is not a valid asset id", text);
            return {};
        }

        const std::optional<std::filesystem::path> file =
            project.ResolveAsset(Arcane::AssetId::FromGuid(*id));
        if (!file)
        {
            ARC_WARN("bootScene {} does not resolve to a file in this project", text);
            return {};
        }
        return *file;
    }

    // Resolve `id` directly to a physical file in `project`'s AssetRegistry -- the
    // Guid-known counterpart to BootSceneFile(project) above, for a caller that
    // already HAS a Guid instead of the manifest's bootScene text (a runtime
    // host's `--scene` override, HostConfig::sceneOverride). Same failure mode/
    // message as the manifest path's "does not resolve to a file" case.
    inline std::filesystem::path BootSceneFile(const Arcane::Project& project, const Arcane::Guid& id)
    {
        const std::optional<std::filesystem::path> file =
            project.ResolveAsset(Arcane::AssetId::FromGuid(id));
        if (!file)
        {
            ARC_WARN("bootScene {} does not resolve to a file in this project", id.ToString());
            return {};
        }
        return *file;
    }

    // What BootScene loaded, handed back so the caller (the editor's
    // SceneSession::Adopt) can record the session's file + id WITHOUT a second
    // ReadSceneFile of the same path just to recover the Guid, which is what
    // the original plan for this function would have made every caller do.
    struct BootSceneResult
    {
        std::filesystem::path file;   // the .arcscene BootScene just applied
        Arcane::Guid          id;     // its asset id, straight from the parsed document
    };

    namespace Detail
    {
        // Shared body of both BootScene overloads below: read + apply an
        // already-resolved scene file into `runtime`. `file` empty means the
        // caller's resolution step (either BootSceneFile overload) already
        // failed and already logged its own ARC_WARN reason -- this returns
        // nullopt silently here rather than logging again.
        //
        // Read before reset, same ordering rule as the editor's Open Scene: a
        // boot scene that fails to parse must not leave the host holding a
        // half-built registry. There is less to protect at boot than Open
        // Scene protects (no prior authored scene, only whatever the plugin's
        // Init happened to spawn), but the order is the same regardless.
        inline std::optional<BootSceneResult> ApplySceneFile(Arcane::Runtime& runtime,
                                                              const std::filesystem::path& file)
        {
            if (file.empty()) return std::nullopt;

            std::string err;
            const auto doc = Arcane::Scene::ReadSceneFile(file, &err);
            if (!doc)
            {
                ARC_ERROR("bootScene: {}", err);
                return std::nullopt;
            }

            runtime.ResetRegistry();
            if (!Arcane::Scene::ApplySceneDocument(*doc, runtime.Registry()))
            {
                // Validated but unloadable -- the failure mode ReadSceneFile's
                // structural gate cannot see: the reflection reader latched
                // while walking a component's data. Since Task 3 (F1) that is
                // usually MALFORMED DATA (a stale or hand-edited field value in
                // a shape the field cannot take), with an unsupported reflected
                // field type (E02-3) the rarer, code-defect case -- so a reader
                // of this line should look in the FILE first, not the code.
                // LoadJson has already named the component and the field, which
                // is why this does not try to restate it. The registry is
                // already reset by contract, so leave a well-formed empty scene
                // rather than an empty-but-rootless one.
                ARC_ERROR("bootScene: {} parsed but could not be loaded -- see the "
                          "scene-load warning above for the component and field "
                          "that refused", file.generic_string());
                Arcane::Scene::CreateEmpty(runtime.Registry());
                return std::nullopt;
            }

            ARC_INFO("Loaded boot scene {}", file.generic_string());
            return BootSceneResult{file, doc->id};
        }
    }

    // Load the project's boot scene into `runtime`, replacing whatever the
    // registry holds. nullopt when there is no boot scene or it could not be
    // loaded -- the reason is logged HERE, and callers simply continue with
    // whatever the registry already held (a project with no boot scene, or one
    // whose boot scene fails to
    // resolve/parse, is left exactly as it was -- nothing is reset until a
    // valid document is in hand) rather than refusing to open the project,
    // because the editor is how a broken boot scene gets fixed.
    //
    // Call AFTER the plugin loads: a scene naming a component the game module
    // registers would otherwise silently drop it.
    inline std::optional<BootSceneResult> BootScene(Arcane::Runtime& runtime, const Arcane::Project& project)
    {
        return Detail::ApplySceneFile(runtime, BootSceneFile(project));
    }

    // Load `id`'s scene file into `runtime` -- the Guid-known counterpart to
    // BootScene(runtime, project) above, for a runtime host's `--scene` override
    // (HostConfig::sceneOverride) once the override text has already parsed as a
    // Guid. Shares Detail::ApplySceneFile with the manifest-path overload above
    // for the read/reset/apply/log body; only the resolution step differs
    // (BootSceneFile(project, id) instead of the manifest's bootScene text), so
    // an override Guid that resolves to no asset in this project hits the exact
    // same "does not resolve to a file" path.
    inline std::optional<BootSceneResult> BootScene(Arcane::Runtime& runtime, const Arcane::Project& project,
                                                     const Arcane::Guid& id)
    {
        return Detail::ApplySceneFile(runtime, BootSceneFile(project, id));
    }
}
