// arccook -- F2b Task 5: the offline asset cook CLI. Cooks a project's Content/ sources
// (F2c Task 8: every kind in CookSession's own kind table -- texture and mesh) into
// Intermediate/Artifacts via Arcane::AssetPipeline::CookSession, the SAME session type
// the editor's future in-process cook (Task 12) drives -- the CLI never diverges from
// the editor on what "stale" or "cooked" means.
//
// Exit codes: cook path -- 0 on success (failed == 0), 1 on ANY cook failure.
// `--check` -- 0 clean (nothing stale), 2 stale (DISTINCT from a cook failure, so a CI
// script can tell "needs a cook" apart from "the cook itself is broken").
// `--dump-dds` -- 0 on a successful DDS write, 1 otherwise; cooks first so the artifact
// it dumps is current, then writes purely for eyeballing (never a load path).

#include "DdsDump.hpp"

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/CookSession.hpp>
#include <Arcane/Cli/Cli.hpp>
#include <Arcane/Guid.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>

namespace
{
    void PrintLine(const std::filesystem::path& source, bool ok, const std::string& detail)
    {
        std::printf("%s %s%s%s\n", ok ? "OK  " : "FAIL", source.generic_string().c_str(),
                     detail.empty() ? "" : " -- ", detail.c_str());
    }
}

int main(int argc, char** argv)
{
    using namespace Arcane;
    using namespace Arcane::AssetPipeline;

    Cli cli{ "arccook", "Arcane offline asset cook (F2b Task 5; F2c Task 8 adds meshes)" };
    cli.Option("project", "", "project folder to cook (required)").Required();
    cli.Flag("check", "report whether any source is stale without cooking "
                       "(exit 0 clean / 2 stale)");
    cli.Flag("verbose", "print one line per attempted source (OK/FAIL, cooked/up-to-date/failed)");
    cli.Option("dump-dds", "", "cook, then write the artifact for this source Guid to "
                               "<guid>.dds beside the current directory, for eyeballing only "
                               "(never a load path)");

    const Cli::Result r = cli.Parse(argc, argv);
    if (!r.ok) return r.exitCode;

    const std::filesystem::path projectDir = r.Get("project");
    std::error_code dirEc;
    if (projectDir.empty() || !std::filesystem::is_directory(projectDir, dirEc))
    {
        std::fprintf(stderr, "arccook: --project '%s' is not a directory\n", projectDir.string().c_str());
        return 1;
    }

    // Settings arc S6-6: the project's own assets.import.texture.* (its Project
    // rung, <project>/Config) is what a .meta field left out resolves to -- the same
    // values the editor cooks with, so both produce the same cook keys.
    CookSession session;
    session.SetTextureDefaults(ApplyProjectCookConfig(projectDir));
    if (r.Flag("verbose"))
        session.SetProgress(&PrintLine);

    const std::string dumpGuidArg = r.Get("dump-dds");
    if (!dumpGuidArg.empty())
    {
        const std::optional<Guid> guid = Guid::FromString(dumpGuidArg);
        if (!guid || !guid->IsValid())
        {
            std::fprintf(stderr, "arccook: --dump-dds '%s' is not a valid Guid\n", dumpGuidArg.c_str());
            return 1;
        }

        // Cook first -- --dump-dds is a debug AID over a current artifact, not a bare
        // reader of whatever happens to already be on disk. The result itself is
        // unused here: a failed cook for THIS guid surfaces below as "no CURRENT
        // cooked artifact for guid", which is the more specific, actionable message.
        try
        {
            [[maybe_unused]] const CookResult dumpCookResult = session.CookProject(projectDir);
        }
        catch (const std::exception& ex)
        {
            std::fprintf(stderr, "arccook: the cook threw: %s\n", ex.what());
            return 1;
        }
        catch (...)
        {
            std::fprintf(stderr, "arccook: the cook threw\n");
            return 1;
        }

        // ResolveCurrentArtifactPath, NEVER an ArtifactStore::RebuildIndexFromScan +
        // Lookup round-trip: that index maps a Guid to ONE cook key by last-write-wins
        // over an undefined directory-iteration order, so if an earlier `.meta`
        // settings edit left an orphaned artifact on disk under its OLD key (same
        // sourceGuid header as the current one), Lookup could non-deterministically
        // return the STALE key instead of today's. ResolveCurrentArtifactPath
        // recomputes today's key directly from the source's current bytes + settings
        // and never falls back to any other artifact -- see CookSession.hpp.
        const std::optional<std::filesystem::path> artifactPath =
            session.ResolveCurrentArtifactPath(projectDir, *guid);
        if (!artifactPath)
        {
            std::fprintf(stderr, "arccook: no CURRENT cooked artifact for guid %s "
                                  "(uncooked or stale)\n", guid->ToString().c_str());
            return 1;
        }

        // F2c Task 8: --dump-dds is a TEXTURE-only debug aid (it writes a DDS, which only
        // ever makes sense for a texture's own compressed/uncompressed mip payload). A
        // guid whose CURRENT artifact is a mesh is refused with a named message here --
        // better than handing ReadTextureArtifact a mesh-shaped file and letting its own
        // contentKind gate print "failed to load", which would read as corruption rather
        // than "wrong tool for this guid". The prefix read is the same bounded,
        // kind-agnostic probe ArtifactStore::RebuildIndexFromScan uses.
        const std::optional<ArtifactPrefix> prefix = ReadArtifactPrefix(*artifactPath);
        if (prefix && prefix->contentKind == ContentKind::Mesh)
        {
            std::fprintf(stderr, "arccook: --dump-dds is a texture-only debug aid; guid %s "
                                  "resolves to a MESH artifact\n", guid->ToString().c_str());
            return 1;
        }

        const std::optional<LoadedArtifact> artifact = ReadTextureArtifact(*artifactPath);
        if (!artifact)
        {
            std::fprintf(stderr, "arccook: artifact for guid %s failed to load\n", guid->ToString().c_str());
            return 1;
        }

        const std::filesystem::path ddsPath = guid->ToString() + ".dds";
        if (!arccook::WriteDds(ddsPath, *artifact))
        {
            std::fprintf(stderr, "arccook: failed to write %s\n", ddsPath.string().c_str());
            return 1;
        }

        std::printf("arccook: wrote %s\n", ddsPath.string().c_str());
        return 0;
    }

    if (r.Flag("check"))
    {
        const bool stale = session.CheckProject(projectDir);
        std::printf("arccook --check: %s\n", stale ? "stale" : "clean");
        return stale ? 2 : 0;
    }

    try
    {
        const CookResult result = session.CookProject(projectDir);
        std::printf("arccook: cooked=%zu upToDate=%zu failed=%zu\n", result.cooked, result.upToDate, result.failed);
        return (result.failed > 0) ? 1 : 0;
    }
    catch (const std::exception& ex)
    {
        std::fprintf(stderr, "arccook: the cook threw: %s\n", ex.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "arccook: the cook threw\n");
        return 1;
    }
}
