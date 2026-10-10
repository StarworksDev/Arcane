#include <Arcane/Project/Project.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>   // ARC_WARN, ARC_ERROR
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Platform/Paths.hpp>   // Arcane::Paths -- Saved/Diagnostics and editor.lock resolve through it (settings spec s11.0)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion
#include <Arcane/Platform/Process.hpp>   // Platform::QueryProcess -- EditorLock liveness off-Windows
#include <Arcane/Project/ProjectPaths.hpp>   // PathsConfigFor, kDistBuild

#include <Json.hpp>

#include <fstream>
#include <functional>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>    // strtoull (EditorLock's /proc reader)
#include <sstream>
#include <unistd.h>   // getpid
#endif

namespace Arcane
{
    namespace
    {
        bool ReplaceConfigFile(const std::filesystem::path& tmp, const std::filesystem::path& file)
        {
#ifdef _WIN32
            if (MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
#else
            std::error_code ec;
            std::filesystem::rename(tmp, file, ec);
            if (!ec)
                return true;
#endif
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            return false;
        }

        // Read a .arcproj, apply ONE edit, write it back atomically. Shared by
        // SetBootScene, the Open-time guid self-heal, and RestampEngineAbi
        // (whose target is NESTED -- engine.abi -- which is why this takes an
        // edit callback rather than a top-level key/value pair).
        //
        // ordered_json, NOT json: the default type is key-sorted, so a
        // read-modify-write would hand back a manifest alphabetised top to
        // bottom for a one-field edit -- a diff nobody asked for in a file that
        // is very likely in git. `who` prefixes the logs so a failure names the
        // operation the user attempted, not this helper.
        bool RewriteManifest(const std::filesystem::path& file, const char* who,
                             const std::function<void(nlohmann::ordered_json&)>& edit)
        {
            nlohmann::ordered_json doc;
            try
            {
                std::ifstream in(file, std::ios::binary);
                if (!in)
                {
                    ARC_ERROR("{}: could not read {}", who, file.generic_string());
                    return false;
                }
                doc = nlohmann::ordered_json::parse(in);
                if (!doc.is_object())
                {
                    ARC_ERROR("{}: {} is not a JSON object", who, file.generic_string());
                    return false;
                }
            }
            catch (const nlohmann::json::exception& e)
            {
                ARC_ERROR("{}: could not parse {}: {}", who, file.generic_string(), e.what());
                return false;
            }

            edit(doc);

            // Every rewrite upgrades the file it touches to the format this
            // engine writes (ProjectManifest::kFormatVersion). v1 -> v2: the
            // on-disk physics.gravity was authored +Y down, so its y is
            // negated HERE too -- the file must mean under v2 what FromJson
            // read it to mean under v1 (a bare stamp change would flip the
            // project's gravity on the next open).
            if (doc.contains("formatVersion") && doc["formatVersion"].is_number_integer()
                && doc["formatVersion"].get<int>() < ProjectManifest::kFormatVersion)
            {
                const int from = doc["formatVersion"].get<int>();
                if (from < 2 && doc.contains("physics") && doc["physics"].is_object()
                    && doc["physics"].contains("gravity") && doc["physics"]["gravity"].is_array()
                    && doc["physics"]["gravity"].size() >= 2 && doc["physics"]["gravity"][1].is_number())
                {
                    doc["physics"]["gravity"][1] = -doc["physics"]["gravity"][1].get<double>();
                }
                doc["formatVersion"] = ProjectManifest::kFormatVersion;
                ARC_INFO("{}: {} upgraded from formatVersion {} to {}", who, file.generic_string(),
                         from, ProjectManifest::kFormatVersion);
            }

            // Temp + rename: a half-written .arcproj is a project that will not open.
            const std::filesystem::path tmp = file.string() + ".tmp";
            try
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                {
                    ARC_ERROR("{}: could not open {} for writing", who, tmp.generic_string());
                    return false;
                }
                // dump() emits no trailing newline; add one so a rewritten
                // manifest stays a conventionally-terminated text file (these
                // files live in git, where the missing byte shows up as noise).
                out << doc.dump(2) << '\n';
                if (!out)
                {
                    // The file exists on disk (open succeeded) but is truncated/partial --
                    // remove it so a failed write does not leave a stray .tmp behind.
                    std::error_code ec;
                    std::filesystem::remove(tmp, ec);
                    ARC_ERROR("{}: could not write {}", who, tmp.generic_string());
                    return false;
                }
            }
            catch (const nlohmann::json::exception& e)
            {
                // dump(2) can throw after `out` already created/truncated tmp -- same
                // cleanup as the write-failure case above.
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                ARC_ERROR("{}: could not serialize {}: {}", who, file.generic_string(), e.what());
                return false;
            }

#ifdef _WIN32
            // ReplaceFileW, not a plain delete+rename: it preserves the destination's NTFS
            // attributes/ACLs and never leaves a window where `file` does not exist (a bare
            // MoveFileEx-style replace has to delete the original first). Note this is not a
            // cure for a held-open destination: like any Windows replace, it still needs
            // DELETE access to `file`, so another handle without FILE_SHARE_DELETE (e.g. a
            // plain ifstream someone forgot to close) will still fail the swap -- that is an
            // NTFS sharing rule, not something this function can paper over.
            if (!ReplaceFileW(file.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
            {
                const DWORD err = GetLastError();   // capture before any other call clobbers it
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                // ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED mean some other handle on
                // `file` lacks FILE_SHARE_DELETE (AV scanner, git, a backup tool, or a second
                // editor window all do this) -- distinguish that from a generic replace
                // failure so the caller can tell "close the other program and retry" apart
                // from a genuinely broken manifest.
                if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED)
                {
                    ARC_ERROR("{}: could not replace {} -- another program may have "
                              "the .arcproj open (err {}); close it and try again",
                              who, file.generic_string(), err);
                }
                else
                {
                    ARC_ERROR("{}: could not replace {} (err {})", who, file.generic_string(), err);
                }
                return false;
            }
#else
            std::error_code ec;
            std::filesystem::rename(tmp, file, ec);
            if (ec)
            {
                std::filesystem::remove(tmp, ec);
                ARC_ERROR("{}: could not replace {}", who, file.generic_string());
                return false;
            }
#endif
            return true;
        }

        // Paths for THIS project's identity (company and game from its
        // manifest), so a Dist build resolves DiagnosticsDir under this
        // project's own per-user folder. Paths::ForProject keeps Current()'s
        // names, and a Runtime opens a project before it configures Paths for
        // it (S7-DIST). The dist rule is Runtime's: always in Dist, else a preset.
        Paths::Config PathsOf(const Project& project)
        {
            const Paths::Config current = Paths::Current();
            return PathsConfigFor(project, current.engineDir, kDistBuild || current.dist);
        }
    }

    void ApplyLegacyManifestSettings(CVarRegistry& registry, const ProjectManifest& manifest,
                                     const std::filesystem::path& manifestFile)
    {
        for (const auto& [category, block] : manifest.legacySettings.items())
        {
            // The Project rung's refusals (settings S7-SEC). The block is no
            // folder ValidateCVarLayers reads, so the warning is said here.
            const CVarApplyReport report = ApplyCVarCategory(registry, category, block, SetBy::Project, false, "project-manifest");
            for (const CVarRefusedKey& refused : report.refused)
                ARC_WARN("cvar config: config.cvar.refused '{}' in the settings block of {} (project '{}') -- {}",
                         refused.key, manifestFile.generic_string(), manifest.name, RungRefusalReason(refused.why));
        }
    }

    std::optional<std::filesystem::path> Project::ResolveManifestFile(const std::filesystem::path& pathOrFile)
    {
        std::error_code ec;
        std::filesystem::path manifestFile;

        if (std::filesystem::is_regular_file(pathOrFile, ec)
            && pathOrFile.extension() == ".arcproj")
        {
            manifestFile = pathOrFile;
        }
        else if (std::filesystem::is_directory(pathOrFile, ec))
        {
            // Find exactly one *.arcproj in the folder.
            for (const auto& entry : std::filesystem::directory_iterator(pathOrFile, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".arcproj")
                {
                    if (!manifestFile.empty())
                    {
                        ARC_WARN("Project::ResolveManifestFile: multiple .arcproj in '{}'",
                                 pathOrFile.generic_string());
                        return std::nullopt;   // ambiguous
                    }
                    manifestFile = entry.path();
                }
            }
        }

        if (manifestFile.empty())
        {
            ARC_WARN("Project::ResolveManifestFile: no .arcproj at '{}'", pathOrFile.generic_string());
            return std::nullopt;
        }
        return manifestFile;
    }

    bool Project::MigrateLegacySettingsAt(const std::filesystem::path& manifestFile)
    {
        const auto manifest = ProjectManifest::LoadFile(manifestFile);
        if (!manifest || manifest->legacySettings.empty())
            return false;

        const std::filesystem::path config = manifestFile.parent_path() / "Config";
        std::error_code ec;
        std::filesystem::create_directories(config, ec);
        if (ec)
            return false;

        for (const auto& [category, block] : manifest->legacySettings.items())
        {
            const std::filesystem::path file = config / (category + ".json");
            nlohmann::json doc = nlohmann::json::object();
            if (std::ifstream in(file); in)
            {
                try { doc = nlohmann::json::parse(in); }
                catch (...) { doc = nlohmann::json::object(); }
            }
            doc.merge_patch(block);

            const std::filesystem::path tmp = file.string() + ".tmp";
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << doc.dump(2) << '\n';
            if (!out.good())
            {
                out.close();
                std::error_code ignored;
                std::filesystem::remove(tmp, ignored);
                return false;
            }
            out.close();
            if (!ReplaceConfigFile(tmp, file))
                return false;
        }

        return RewriteManifest(manifestFile, "Project::MigrateLegacySettings",
                               [](nlohmann::ordered_json& doc)
                               {
                                   doc.erase("physics");
                                   doc.erase("splash");
                               });
    }

    bool Project::MigrateLegacySettings()
    {
        if (!MigrateLegacySettingsAt(m_manifestFile))
            return false;
        m_manifest.legacySettings = nlohmann::json::object();
        m_manifest.formatVersion = ProjectManifest::kFormatVersion;
        return true;
    }

    std::optional<Project> Project::Open(const std::filesystem::path& pathOrFile,
                                         AssetRegistry::ScanProgressFn onProgress,
                                         ProjectOpenOptions opts)
    {
        std::error_code ec;

        // The ONE place a project root resolves to its .arcproj -- shared with
        // the pre-open splash.showProgress peek in ProjectBoot.cpp's
        // RuntimeStages (HostBoot::RuntimeStages' project_open override), so
        // that peek can never drift from what Open() itself would actually
        // open. `root` is deliberately NOT derived from manifestFile->
        // parent_path() (which is lexically equal in both branches today, but
        // relying on that equality would make root's correctness hostage to
        // std::filesystem::path normalization of a trailing separator/"." in
        // whatever pathOrFile a caller happens to pass) -- it stays computed
        // the exact same way this function always computed it, just after the
        // (now shared) resolution step instead of interleaved with it.
        const auto manifestFile = ResolveManifestFile(pathOrFile);
        if (!manifestFile)
            return std::nullopt;   // ResolveManifestFile already logged the cause

        const std::filesystem::path root =
            (std::filesystem::is_regular_file(pathOrFile, ec) && pathOrFile.extension() == ".arcproj")
                ? pathOrFile.parent_path()
                : pathOrFile;   // ResolveManifestFile only succeeds here if pathOrFile is the directory

        auto manifest = ProjectManifest::LoadFile(*manifestFile);
        if (!manifest)
            return std::nullopt;   // LoadFile already logged

        // A read-only (unmigratable) project keeps legacySettings populated;
        // Open itself applies NOTHING to the global CVarRegistry, so a probe
        // Open (the editor's project-switch validation) has no side effects.
        // The in-memory application belongs to the rung appliers:
        // Runtime::OpenProject (right after the Project Config dir) and
        // HostBoot's ApplyEarlyConfigRungs.
        if (!manifest->legacySettings.empty() && MigrateLegacySettingsAt(*manifestFile))
        {
            manifest->legacySettings = nlohmann::json::object();
            manifest->formatVersion = ProjectManifest::kFormatVersion;
        }

        // Self-heal the project's durable identity: a manifest that predates
        // the guid field (or carries a mangled one) gets a fresh Guid stamped
        // in place, once. Best-effort -- an unwritable manifest (read-only
        // media, VCS lock) must not fail the open, and the in-memory guid
        // stays empty on failure so each open cannot invent a DIFFERENT
        // identity than the disk records.
        if (!Guid::FromString(manifest->guid))
        {
            const std::string fresh = Guid::Generate().ToString();
            if (RewriteManifest(*manifestFile, "Project::Open",
                                [&](nlohmann::ordered_json& doc) { doc["guid"] = fresh; }))
                manifest->guid = fresh;
        }

        Project proj;
        proj.m_root         = root;
        proj.m_manifestFile = *manifestFile;
        proj.m_manifest     = std::move(*manifest);
        // Default mounts + the asset identity map (Guid -> mount path). game:// is the
        // project's own Content/; plugin content folds in below. (engine:// stays reserved
        // until the engine ships built-in content -- spec Q2.)
        proj.m_mounts.Mount("game", root / "Content");
        proj.m_registry.ScanContent(root / "Content", "game", onProgress);

        // diag:// -- GPU crash diagnostics arc, Task 9. Crash/hang/gpu-stall
        // reports (.arcdiag, minted by Diagnostics::WriteReportImpl) land
        // under <root>/Saved/Diagnostics once Task 8's RetargetDumpDir has
        // pointed a live host's dumpDir there. Mounted + folded into the
        // SAME registry (AddContent, not a second ScanContent -- this must
        // not clear the game:// scan already done above) ONLY when the
        // directory already exists: a project that has never crashed has no
        // Saved/Diagnostics yet, and that must stay silent -- no mount, no
        // scan attempt, no warning -- rather than an empty-but-mounted
        // scheme. (A report written mid-session, the FIRST time this
        // project ever crashes, cannot go through this gate -- Open() only
        // runs once at boot/switch -- so it registers incrementally instead
        // via Runtime::RegisterCreatedAsset/EditorApp::PollDiagnosticReports;
        // RegisterAsset lists this same root as a candidate unconditionally,
        // regardless of whether it existed at open time, and mounts it
        // there if this branch never did.)
        // opts.mountDiagnostics == false declines this WHOLE branch -- both the
        // mount and the AddContent below. A verify run opts out so its capture
        // cannot show the machine's own crash history in the Assets panel; see
        // ProjectOpenOptions.hpp. Deliberately gated HERE, above the
        // is_directory probe, so an opted-out open does not even stat the
        // directory. NOTE the boundary this does NOT cover: RegisterAsset
        // (below) still lists this root unconditionally, so a run that ITSELF
        // writes a report mid-session can still mount diag:// -- out of scope,
        // the defect is enumeration of PRE-EXISTING crash history at open time.
        const std::filesystem::path diagDir = Paths::Resolve(Paths::Location::DiagnosticsDir, PathsOf(proj));
        if (opts.mountDiagnostics && std::filesystem::is_directory(diagDir, ec))
        {
            proj.m_mounts.Mount("diag", diagDir);
            proj.m_registry.AddContent(diagDir, "diag");
        }

        // source:// -- the project's C++ source tree (<root>/Source, the same
        // directory build/arcane.lua's game-module glob compiles), folded into
        // the SAME registry so the Asset Browser lists it as a peer root of
        // Content/. Source files register under a path-derived guid with no
        // sidecar (AssetRegistry's fourth identity rule). Gated on the directory
        // existing, exactly like diag:// above: Project::Create scaffolds
        // Source/, but a content-only project (or one whose author removed it)
        // stays silent -- no mount, no scan, no warning.
        // The source:// MOUNT -- deliberately NOT Manifest().sourceDir (2026-09-16 ruling S2:
        // the browser shows every module under one Source/ root; the module's own subdirectory
        // is the manifest's sourceDir, read by build/arcane.lua and the Create C++ Class default only).
        const std::filesystem::path sourceDir = root / "Source";
        if (std::filesystem::is_directory(sourceDir, ec))
        {
            proj.m_mounts.Mount("source", sourceDir);
            proj.m_registry.AddContent(sourceDir, "source");
        }

        // KEY OWNERSHIP: "project" (fixed key) -- accumulated across the
        // WHOLE plugin loop below and published ONCE, unconditionally, right
        // before returning (so a clean re-open with no plugin problems
        // retracts a previous open's rows -- the empty-vector-is-a-retraction
        // rule shared by every other producer in this arc).
        //
        // ProjectManifest::LoadFile (ProjectManifest.cpp) DECIDES the content
        // of every manifest-shaped-file diagnostic (it is the only code that
        // knows which of open/parse/schema failed), but WHO PUBLISHES depends
        // on the call site. The TOP-LEVEL manifest load a few lines above
        // passes no outDiag, so LoadFile publishes directly under "project"
        // itself -- safe, because this function already returned std::nullopt
        // in that failure branch and never reaches here to stomp it. The
        // PER-PLUGIN descriptor validated in the loop below passes `&diag` and
        // captures LoadFile's filled-but-unpublished diagnostic instead: were
        // LoadFile to publish directly there too, this function's own
        // unconditional publish at the end would silently retract it (Publish()
        // replaces the whole key, and this function has no way to read back
        // what LoadFile published to fold it in). Appending LoadFile's filled
        // diagnostic verbatim (not re-deriving code/message here) is also what
        // keeps an unreadable descriptor from mis-surfacing as "invalid".
        std::vector<Diagnostic> diagnostics;

        // Slice 4 -- project plugins. Each enabled plugin in the manifest lives at
        // <root>/Plugins/<name>/ with a <name>.arcplugin descriptor (same shape as
        // .arcproj). Its Content/ (if any) mounts as "plugin/<name>://" and its assets
        // join the SAME registry, so a plugin asset is addressed by its Guid exactly like
        // a game asset. Dependency direction stays one-way (project -> plugin -> engine).
        // (Loading a plugin's Source/-built DLL through the host is a follow-up; the host
        // loads one game module today. Engine-plugin discovery is reserved -- spec Q2.)
        for (const auto& ref : proj.m_manifest.plugins)
        {
            if (!ref.enabled)
                continue;

            const std::filesystem::path pluginRoot = root / "Plugins" / ref.name;
            const std::filesystem::path descriptor = pluginRoot / (ref.name + ".arcplugin");
            if (!std::filesystem::is_regular_file(descriptor, ec))
            {
                ARC_WARN("Project::Open: plugin '{}' is enabled but has no descriptor at '{}'",
                         ref.name, descriptor.generic_string());
                Diagnostic d;
                d.severity = DiagSeverity::Warning;
                d.scope    = DiagScope::Project;
                d.code     = "project.plugin.no-descriptor";
                d.message  = "Plugin '" + ref.name + "' is enabled but has no descriptor at '" +
                             descriptor.generic_string() + "'.";
                d.locator  = DiagLocator::File(descriptor.generic_string());
                diagnostics.push_back(std::move(d));
                continue;
            }
            // Validate the descriptor (well-formed .arcproj-shaped manifest). LoadFile logs
            // on failure; a malformed descriptor disables the plugin rather than the project.
            Diagnostic descriptorDiag;
            if (!ProjectManifest::LoadFile(descriptor, &descriptorDiag))
            {
                // `descriptorDiag` was FILLED (not published) by LoadFile --
                // the true code (project.manifest.unreadable for an open/parse
                // failure, project.manifest.invalid for a schema failure) with
                // its own message and File locator. Appended VERBATIM, never
                // re-derived here, so this row survives past this function's
                // own unconditional "project" publish at the end -- see the
                // KEY OWNERSHIP comment above `diagnostics`'s declaration.
                diagnostics.push_back(std::move(descriptorDiag));
                continue;
            }

            // Record the activated plugin (valid + enabled), whether or not it has Content --
            // its Config/ still layers and its Source/ DLL still loads. ActivePluginRoots().
            proj.m_activePluginRoots.push_back(pluginRoot);

            const std::filesystem::path content = pluginRoot / "Content";
            if (std::filesystem::is_directory(content, ec))
            {
                const std::string scheme = "plugin/" + ref.name;   // -> "plugin/<name>://<rel>"
                proj.m_mounts.Mount(scheme, content);
                proj.m_registry.AddContent(content, scheme);
            }
        }

        Diagnostics::Publish("project", diagnostics);
        return proj;
    }

    std::optional<Project> Project::Create(const std::filesystem::path& dir, std::string name)
    {
        std::error_code ec;

        // Reject a bad name BEFORE touching disk: empty, or containing a path
        // separator, would otherwise let the manifest filename (dir / (name +
        // ".arcproj")) write outside `dir`, or silently produce a nameless file.
        if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        {
            ARC_WARN("Project::Create: invalid project name '{}'", name);
            return std::nullopt;
        }

        // Refuse a non-empty existing dir (avoid clobbering); an absent dir is fine.
        if (std::filesystem::exists(dir, ec) && !std::filesystem::is_empty(dir, ec))
        {
            ARC_WARN("Project::Create: target '{}' exists and is not empty", dir.generic_string());
            return std::nullopt;
        }

        for (const char* sub : { "Source", "Content", "Config", "Plugins" })
        {
            std::filesystem::create_directories(dir / sub, ec);
            if (ec)
            {
                ARC_WARN("Project::Create: mkdir '{}' failed: {}", (dir / sub).generic_string(), ec.message());
                return std::nullopt;
            }
        }

        // Minimal manifest. ABI comes from the engine's plugin-ABI constant so a freshly
        // created project always targets the engine that created it. Built via nlohmann
        // so `name` is escaped correctly (rather than hand-concatenated into JSON text).
        nlohmann::json manifestJson;
        manifestJson["formatVersion"] = ProjectManifest::kFormatVersion;
        manifestJson["name"]          = name;
        manifestJson["engine"]        = { { "abi", static_cast<int>(Arcane::kGamePluginABIVersion) } };
        manifestJson["gameModule"]    = "";
        manifestJson["plugins"]       = nlohmann::json::array();
        manifestJson["bootScene"]     = "";
        // Stamped at birth so the Open() below never needs its self-heal write.
        manifestJson["guid"]          = Guid::Generate().ToString();

        const std::filesystem::path manifestPath = dir / (name + ".arcproj");
        {
            std::ofstream out(manifestPath, std::ios::binary);
            out << manifestJson.dump(2) << '\n';
            if (!out.good())
            {
                ARC_WARN("Project::Create: failed writing manifest '{}'", manifestPath.generic_string());
                return std::nullopt;
            }
        }

        static const char* kGitignore =
            "# Derived / generated -- never commit\n"
            "Binaries/\n"
            "Intermediate/\n"
            "Saved/\n"
            "\n"
            "# Plugin generated output\n"
            "Plugins/**/Binaries/\n"
            "Plugins/**/Intermediate/\n"
            "\n"
            "# Generated IDE project files\n"
            "*.sln\n"
            "*.vcxproj\n"
            "*.vcxproj.filters\n"
            "*.vcxproj.user\n"
            ".vs/\n";

        const std::filesystem::path gitignorePath = dir / ".gitignore";
        {
            std::ofstream out(gitignorePath, std::ios::binary);
            out << kGitignore;
            if (!out.good())
            {
                ARC_WARN("Project::Create: failed writing '{}'", gitignorePath.generic_string());
                return std::nullopt;
            }
        }

        return Open(dir);
    }

    std::optional<Project::ContentRootMatch> Project::FindContentRoot(const std::filesystem::path& file) const
    {
        std::error_code ec;
        const auto canon = std::filesystem::weakly_canonical(file, ec);
        const std::filesystem::path target = ec ? file : canon;

        // Candidate content roots, game:// first (mirrors Open's mount order).
        //
        // diag:// (GPU crash diagnostics arc, Task 9) is listed
        // UNCONDITIONALLY, not gated on Saved/Diagnostics existing on disk:
        // Open()'s own diag:// mount IS gated on existence (see its
        // comment), so a project's FIRST-EVER report -- written mid-session,
        // after Open() already ran with no Saved/Diagnostics on disk yet --
        // would otherwise match no candidate here and warn "outside every
        // content root" even though the file plainly sits under this
        // project's own Saved/ tree. The path math below (weakly_canonical +
        // relative) needs no directory to actually exist, so listing it
        // unconditionally is safe.
        std::vector<std::pair<std::string, std::filesystem::path>> roots;
        roots.emplace_back("game", m_root / "Content");
        roots.emplace_back("diag", Paths::Resolve(Paths::Location::DiagnosticsDir, PathsOf(*this)));
        // source:// listed unconditionally for the same reason diag:// is: a
        // Source/ created after Open() (a first New C++ Class, one day) must
        // still find its root here rather than warn "outside every content
        // root". Same existence-free path math as above.
        roots.emplace_back("source", m_root / "Source");
        for (const auto& pluginRoot : m_activePluginRoots)
            roots.emplace_back("plugin/" + pluginRoot.filename().string(), pluginRoot / "Content");

        for (const auto& [scheme, dir] : roots)
        {
            std::error_code canonEc, relEc;
            const auto rootCanon = std::filesystem::weakly_canonical(dir, canonEc);
            const std::filesystem::path rootDir = canonEc ? dir : rootCanon;
            const auto rel = std::filesystem::relative(target, rootDir, relEc);
            if (relEc || rel.empty() || rel.is_absolute() || *rel.begin() == "..")
                continue;   // not under this root
            return ContentRootMatch{ scheme, dir, rootDir, target };
        }
        return std::nullopt;
    }

    std::optional<Guid> Project::RegisterAsset(const std::filesystem::path& file)
    {
        const auto match = FindContentRoot(file);
        if (!match)
        {
            ARC_WARN("Project::RegisterAsset: '{}' is outside every content root -- it will "
                     "not appear in the asset registry or resolve by GUID",
                     file.generic_string());
            return std::nullopt;
        }
        // Ensure the winning root is mounted before registering, but
        // ONLY if it is not ALREADY mounted -- and with the ORIGINAL
        // listed dir (Open()'s own convention, e.g. "game" -> root /
        // "Content"), never the weakly_canonical'd rootDir used for the
        // relative-path match in FindContentRoot. "game"/"plugin" roots are
        // always already mounted by Open() (Content/ exists from
        // Project::Create's scaffold), so HasMount short-circuits this
        // for them and their Open()-time mount form is left untouched
        // -- re-mounting with a canonicalized (possibly short-name,
        // e.g. 8.3 "ETHANT~1") path previously broke ResolveAsset()
        // callers comparing against the original long-form path. Only
        // diag:// and source:// can reach this Mount() call in practice,
        // the two roots that may not have existed yet at Open() time (see
        // the roots comment in FindContentRoot).
        if (!m_mounts.HasMount(match->scheme))
            m_mounts.Mount(match->scheme, match->listedDir);
        return m_registry.AddFile(match->target, match->rootDir, match->scheme);
    }

    bool Project::UnregisterAsset(const Guid& id)
    {
        return m_registry.Remove(id);
    }

    RebindResult Project::RebindAsset(const Guid& id, const std::filesystem::path& newFile)
    {
        const auto match = FindContentRoot(newFile);
        if (!match)
            return RebindResult::OutsideContent;
        return m_registry.Rebind(id, match->target, match->rootDir, match->scheme);
    }

    std::optional<std::filesystem::path> Project::ResolveAsset(const AssetId& id) const
    {
        if (!id.IsValid())
            return std::nullopt;
        // Guid -> mount path (AssetRegistry) -> physical file (MountTable).
        auto mountPath = m_registry.Resolve(id.Value());
        if (!mountPath)
            return std::nullopt;
        return m_mounts.Resolve(*mountPath);
    }

    bool Project::SetBootScene(const Guid& id)
    {
        const std::filesystem::path file = m_manifestFile;
        if (file.empty())
        {
            ARC_ERROR("SetBootScene: this project has no manifest file on disk");
            return false;
        }

        const std::string value = id.IsValid() ? id.ToString() : std::string{};
        if (!RewriteManifest(file, "SetBootScene",
                             [&](nlohmann::ordered_json& doc) { doc["bootScene"] = value; }))
            return false;

        m_manifest.bootScene = value;
        return true;
    }

    bool Project::SetInputActionsAsset(const Guid& id)
    {
        const std::filesystem::path file = m_manifestFile;
        if (file.empty())
        {
            ARC_ERROR("SetInputActionsAsset: this project has no manifest file on disk");
            return false;
        }
        const std::string value = id.IsValid() ? id.ToString() : std::string{};
        if (!RewriteManifest(file, "SetInputActionsAsset",
                             [&](nlohmann::ordered_json& doc) { doc["inputActions"] = value; }))
            return false;
        m_manifest.inputActions = value;
        return true;
    }

    bool Project::RestampEngineAbi(int abi)
    {
        const std::filesystem::path file = m_manifestFile;
        if (file.empty())
        {
            ARC_ERROR("RestampEngineAbi: this project has no manifest file on disk");
            return false;
        }

        if (!RewriteManifest(file, "RestampEngineAbi",
                             [&](nlohmann::ordered_json& doc) { doc["engine"]["abi"] = abi; }))
            return false;

        m_manifest.engineAbi = abi;
        return true;
    }

    namespace EditorLock
    {
        std::filesystem::path FileFor(const std::filesystem::path& projectRoot)
        {
            return Paths::Join(Paths::Location::ProjectSaved, Paths::ForProject(projectRoot), "editor.lock");
        }

        std::string ToJson(const Info& info)
        {
            nlohmann::json doc;
            doc["pid"] = info.pid;
            doc["start"] = info.start;
            return doc.dump();
        }

        std::optional<Info> Parse(const std::string& text)
        {
            const auto doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
            if (!doc.is_object() || !doc.contains("pid") || !doc["pid"].is_number_unsigned())
                return std::nullopt;
            Info info;
            info.pid = doc["pid"].get<uint32_t>();
            // `start` missing or malformed reads as 0 = "no time recorded";
            // IsAlive-side validation then degrades to pid-exists, which is
            // still better than trusting the file outright.
            if (doc.contains("start") && doc["start"].is_number_unsigned())
                info.start = doc["start"].get<uint64_t>();
            if (info.pid == 0)
                return std::nullopt;
            return info;
        }

#ifndef _WIN32
        namespace
        {
            // The liveness oracle (Linux: /proc/<pid>/stat; macOS:
            // proc_pidinfo -- Arcane/Platform/Process.hpp): the pid exists,
            // its creation stamp (a recycled pid has a different one), and
            // whether it has exited awaiting its parent's reap -- the POSIX
            // twin of Windows' lingering process OBJECT.
            Platform::ProcessStat ReadProcStat(uint32_t pid)
            {
                return Platform::QueryProcess(pid);
            }
        }
#endif

        Info Self()
        {
            Info info;
#ifdef _WIN32
            info.pid = static_cast<uint32_t>(::GetCurrentProcessId());
            FILETIME created{}, exited{}, kernel{}, user{};
            if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
                info.start = (static_cast<uint64_t>(created.dwHighDateTime) << 32) |
                             created.dwLowDateTime;
#else
            info.pid = static_cast<uint32_t>(::getpid());
            info.start = ReadProcStat(info.pid).start;
#endif
            return info;
        }

        void Write(const std::filesystem::path& projectRoot)
        {
            const std::filesystem::path file = FileFor(projectRoot);
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                ARC_WARN("EditorLock: could not write {}", file.generic_string());
                return;
            }
            out << ToJson(Self());
        }

        void Clear(const std::filesystem::path& projectRoot)
        {
            std::error_code ec;
            std::filesystem::remove(FileFor(projectRoot), ec);
        }

        std::optional<uint32_t> ReadLive(const std::filesystem::path& projectRoot)
        {
            const std::filesystem::path file = FileFor(projectRoot);
            std::ifstream in(file, std::ios::binary);
            if (!in.is_open())
                return std::nullopt;
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            const auto info = Parse(text);
            if (!info)
                return std::nullopt;
#ifdef _WIN32
            // The validation that defeats staleness, in THREE tells -- all of
            // which must hold before the lock is believed:
            //   1. the pid still OPENS (a pid nobody holds is simply gone),
            //   2. its creation time MATCHES what the lock recorded (a
            //      recycled pid has a different birth; a crashed editor has
            //      none), and
            //   3. it has NOT exited -- zero exit time.
            HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info->pid);
            if (!h)
                return std::nullopt;
            FILETIME created{}, exited{}, kernel{}, user{};
            const bool ok = ::GetProcessTimes(h, &created, &exited, &kernel, &user);
            ::CloseHandle(h);
            if (!ok)
                return std::nullopt;
            const uint64_t start = (static_cast<uint64_t>(created.dwHighDateTime) << 32) |
                                   created.dwLowDateTime;
            if (info->start != 0 && info->start != start)
                return std::nullopt;
            // Tell 3, and why tells 1+2 alone were not enough (2026-09-08 desk
            // pass): a process OBJECT outlives its process for as long as any
            // handle to it survives, and the pid is reserved that whole time.
            // A parent holding a dead editor's handle -- the Hub does exactly
            // this -- kept OpenProcess succeeding with the ORIGINAL creation
            // time intact, so the project read as "already open" forever. Exit
            // time is the tell that cannot be faked by a lingering handle:
            // nonzero means it exited, however many handles remain.
            if (exited.dwHighDateTime != 0 || exited.dwLowDateTime != 0)
                return std::nullopt;
            return info->pid;
#else
            // The same three tells via the OS process table (see ReadProcStat): the pid still
            // has a stat entry, its start time matches, and it is not a zombie.
            const Platform::ProcessStat st = ReadProcStat(info->pid);
            if (!st.found)
                return std::nullopt;
            if (info->start != 0 && info->start != st.start)
                return std::nullopt;
            if (st.exited)
                return std::nullopt;
            return info->pid;
#endif
        }

        std::optional<uint32_t> RivalPid(const std::filesystem::path& projectRoot)
        {
#ifdef _WIN32
            const auto pid = ReadLive(projectRoot);
            if (pid && *pid != ::GetCurrentProcessId())
                return pid;
            return std::nullopt;
#else
            const auto pid = ReadLive(projectRoot);
            if (pid && *pid != static_cast<uint32_t>(::getpid()))
                return pid;
            return std::nullopt;
#endif
        }

#ifdef _WIN32
        namespace
        {
            struct FocusTarget
            {
                DWORD pid;
                HWND  hwnd;
            };

            BOOL CALLBACK FindProcessWindow(HWND hwnd, LPARAM lparam)
            {
                auto* t = reinterpret_cast<FocusTarget*>(lparam);
                DWORD owner = 0;
                ::GetWindowThreadProcessId(hwnd, &owner);
                if (owner == t->pid && ::IsWindowVisible(hwnd))
                {
                    t->hwnd = hwnd;
                    return FALSE;   // found -- stop enumerating
                }
                return TRUE;
            }
        }

        bool FocusWindowOfProcess(uint32_t pid)
        {
            FocusTarget target{ pid, nullptr };
            ::EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&target));
            if (!target.hwnd)
                return false;
            // Restore first: SetForegroundWindow on a minimized window succeeds
            // without actually surfacing it (same order as the Hub's mirror).
            ::ShowWindow(target.hwnd, SW_RESTORE);
            return ::SetForegroundWindow(target.hwnd) != 0;
        }
#else
        bool FocusWindowOfProcess(uint32_t)
        {
            return false;
        }
#endif
    }
}
