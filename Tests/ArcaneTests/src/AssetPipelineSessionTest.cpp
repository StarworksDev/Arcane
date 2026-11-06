// F2b Task 5: CookSession -- the shared cook orchestration arccook's CLI and the editor's
// future in-process cook (Task 12) both drive verbatim. Pins, at the SESSION level (a real
// temp-project fixture tree, not the importer/store unit tests Tasks 2-4 already cover):
// idempotence (a second CookProject over an unchanged project cooks nothing, reports every
// source up to date); a `.meta` settings edit recooks EXACTLY the one source it touched, the
// other source untouched; CheckProject answers true before a cook and false immediately after,
// with no side effects of its own (never imports, never writes); and a corrupt (undecodable)
// source memoizes its cook failure across two independent CookSession instances sharing one
// Intermediate/ tree -- the importer runs EXACTLY ONCE for that failing key across both
// sessions (never a retry storm), no artifact is ever produced for it, and the failure is
// externally OBSERVABLE via both SetProgress's watcher and LastFailures().
//
// F2c Task 8 (spec s5.1, R6) extends this file with the MESH half of the same session-level
// contract: a mesh source cooks to a mesh artifact; textures and meshes cook in the SAME
// CookProject pass (the spine iterates the kind TABLE, not one hardcoded extension); an
// external-buffer edit (spec s5.4) restales a .gltf whose own bytes never changed; a mesh
// refusal's diagnostic survives verbatim into CookResult::failures; and a mesh failure
// memoizes exactly like a texture one, driven through SetMeshImporterForTesting so the
// no-retry-storm contract is pinned by counted invocations, not inferred.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/ArtifactStore.hpp>
#include <Arcane/AssetPipeline/CookKey.hpp>
#include <Arcane/AssetPipeline/CookSession.hpp>
#include <Arcane/AssetPipeline/MeshImporter.hpp>
#include <Arcane/AssetPipeline/MeshMetaSettings.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Guid.hpp>

#include <Json.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::AssetPipeline;
using Arcane::Guid;

namespace
{
    fs::path TempProjectDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_pipeline_session_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d / "Content" / "textures");
        return d;
    }

    // F2c Task 8: a BARE project dir -- unlike TempProjectDir above, this pre-creates
    // nothing under Content/, so a mesh (or mixed) test can lay out exactly the Content/
    // subtree it needs (e.g. "Content/meshes" only, so the fixture fails loudly if the
    // spine still enumerates ".png" alone). Same temp root/cleanup discipline as
    // TempProjectDir and AssetPipelineStoreTest.cpp's own TempDir.
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_pipeline_session_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    // The gltf/glb fixture corpus MeshImporterRefusalTest.cpp already established --
    // reused verbatim rather than duplicated (single.glb, nested.gltf+nested.bin,
    // requires_draco.gltf).
    fs::path MeshFixture(const char* name) { return fs::path("data") / "gltf" / name; }

    std::vector<unsigned char> SolidPixels(int width, int height, unsigned char r, unsigned char g,
                                            unsigned char b, unsigned char a)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            px[i + 0] = r;
            px[i + 1] = g;
            px[i + 2] = b;
            px[i + 3] = a;
        }
        return px;
    }

    void WritePngFile(const fs::path& path, int w, int h, const std::vector<unsigned char>& rgba)
    {
        REQUIRE(stbi_write_png(path.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0);
    }

    void WriteCorruptFile(const fs::path& path)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        // Deliberately NOT a PNG (no PNG signature) -- stb_image must fail to decode it,
        // same as any truncated/garbled download or a hand-edited-to-death asset.
        out << "this is not a png file, just some bytes 0123456789 that stb_image cannot decode";
    }

    // `textureOverrides`, when given, becomes the ".meta" "texture" block verbatim -- lets a
    // test change exactly the settings fields it cares about (e.g. {"srgb": false}) while
    // TextureMetaSettings::FromMetaJson's own tolerant defaults fill in the rest.
    void WriteMetaSidecar(const fs::path& pngPath, const Guid& guid,
                           const std::optional<nlohmann::json>& textureOverrides = std::nullopt)
    {
        nlohmann::json doc;
        doc["guid"] = guid.ToString();
        doc["version"] = 1;
        if (textureOverrides) doc["texture"] = *textureOverrides;

        fs::path metaPath = pngPath;
        metaPath += ".meta";
        std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << doc.dump(2);
    }

    std::vector<std::byte> ReadWholeFile(const fs::path& path)
    {
        std::ifstream ifs(path, std::ios::binary);
        REQUIRE(ifs.good());
        ifs.seekg(0, std::ios::end);
        const auto len = static_cast<std::size_t>(ifs.tellg());
        ifs.seekg(0, std::ios::beg);
        std::vector<std::byte> out(len);
        if (len > 0)
            ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
        REQUIRE(ifs.good());
        return out;
    }

    // The exact path CookSession must land an artifact at for `pngPath`, given its CURRENT
    // on-disk bytes and `settings` -- computed the same way CookSession itself does
    // (ArtifactStore::PathFor over ComputeCookKey), so tests can assert existence/absence
    // directly rather than re-deriving CookSession's own internals.
    fs::path ExpectedArtifactPath(const fs::path& projectDir, const fs::path& pngPath,
                                   const TextureMetaSettings& settings)
    {
        const std::vector<std::byte> bytes = ReadWholeFile(pngPath);
        const ArtifactStore store(projectDir / "Intermediate");
        return store.PathFor(ComputeCookKey(bytes, settings, kTextureImporterVersion));
    }
}

// ---- Idempotence -----------------------------------------------------------------------

TEST_CASE("pipeline: CookSession cooks fresh sources once, second call cooks nothing", "[pipeline]")
{
    const fs::path project = TempProjectDir("idempotence");
    const fs::path pngA = project / "Content" / "textures" / "a.png";
    const fs::path pngB = project / "Content" / "textures" / "b.png";
    WritePngFile(pngA, 4, 4, SolidPixels(4, 4, 10, 20, 30, 255));
    WritePngFile(pngB, 4, 4, SolidPixels(4, 4, 40, 50, 60, 255));
    const Guid guidA = Guid::Generate();
    const Guid guidB = Guid::Generate();
    WriteMetaSidecar(pngA, guidA);
    WriteMetaSidecar(pngB, guidB);

    CookSession first;
    const CookResult firstResult = first.CookProject(project);
    CHECK(firstResult.cooked == 2u);
    CHECK(firstResult.upToDate == 0u);
    CHECK(firstResult.failed == 0u);
    CHECK(first.LastFailures().empty());

    CHECK(fs::exists(ExpectedArtifactPath(project, pngA, TextureMetaSettings{})));
    CHECK(fs::exists(ExpectedArtifactPath(project, pngB, TextureMetaSettings{})));

    CookSession second;
    const CookResult secondResult = second.CookProject(project);
    CHECK(secondResult.cooked == 0u);
    CHECK(secondResult.upToDate == 2u);
    CHECK(secondResult.failed == 0u);
}

// ---- A settings edit recooks exactly the source it touched -----------------------------

TEST_CASE("pipeline: a .meta settings change recooks exactly the one source it touched", "[pipeline]")
{
    const fs::path project = TempProjectDir("settings_change");
    const fs::path pngA = project / "Content" / "textures" / "a.png";
    const fs::path pngB = project / "Content" / "textures" / "b.png";
    WritePngFile(pngA, 4, 4, SolidPixels(4, 4, 70, 80, 90, 255));
    WritePngFile(pngB, 4, 4, SolidPixels(4, 4, 100, 110, 120, 255));
    const Guid guidA = Guid::Generate();
    const Guid guidB = Guid::Generate();
    WriteMetaSidecar(pngA, guidA);
    WriteMetaSidecar(pngB, guidB);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 2u);

    const fs::path originalArtifactB = ExpectedArtifactPath(project, pngB, TextureMetaSettings{});
    REQUIRE(fs::exists(originalArtifactB));

    // Flip ONLY source A's srgb setting -- source B's sidecar is untouched.
    nlohmann::json flippedSrgb;
    flippedSrgb["srgb"] = false;
    WriteMetaSidecar(pngA, guidA, flippedSrgb);

    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 1u);
    CHECK(result.upToDate == 1u);
    CHECK(result.failed == 0u);

    TextureMetaSettings changedSettings{};
    changedSettings.srgb = false;
    CHECK(fs::exists(ExpectedArtifactPath(project, pngA, changedSettings)));

    // B's artifact is byte-for-byte untouched -- still exists at its ORIGINAL path.
    CHECK(fs::exists(originalArtifactB));
}

// ---- CheckProject: read-only, true before a cook, false after --------------------------

TEST_CASE("pipeline: CheckProject is true before a cook and false immediately after", "[pipeline]")
{
    const fs::path project = TempProjectDir("check_project");
    const fs::path png = project / "Content" / "textures" / "solo.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 200, 201, 202, 255));
    WriteMetaSidecar(png, Guid::Generate());

    const CookSession checker;   // CheckProject is const -- never imports, never writes
    CHECK(checker.CheckProject(project));

    CookSession cooker;
    REQUIRE(cooker.CookProject(project).cooked == 1u);

    CHECK_FALSE(checker.CheckProject(project));

    // Read-only really means read-only: no artifact directory materialised beyond what the
    // cook itself already wrote (CheckProject alone, on a project with nothing cooked yet,
    // must not create Intermediate/Artifacts).
    const fs::path freshProject = TempProjectDir("check_project_no_side_effects");
    const fs::path freshPng = freshProject / "Content" / "textures" / "solo.png";
    WritePngFile(freshPng, 4, 4, SolidPixels(4, 4, 1, 2, 3, 255));
    WriteMetaSidecar(freshPng, Guid::Generate());
    CHECK(checker.CheckProject(freshProject));
    CHECK_FALSE(fs::exists(freshProject / "Intermediate" / "Artifacts"));
}

// ---- Corrupt-source failure: memoized, no retry storm, observable ----------------------

TEST_CASE("pipeline: a corrupt source memoizes its failure across two sessions, importer runs once", "[pipeline]")
{
    const fs::path project = TempProjectDir("corrupt_memoized");
    const fs::path png = project / "Content" / "textures" / "corrupt.png";
    WriteCorruptFile(png);
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    // Wraps the REAL importer (so the failure is a genuine stb_image decode failure, not a
    // synthetic stand-in) while counting invocations -- a shared_ptr<atomic> so two
    // independently-copied std::function instances (one per session below) still tally the
    // SAME counter, letting the test observe "runs once ACROSS two sessions" directly rather
    // than inferring it from timing or side channels.
    auto importCalls = std::make_shared<std::atomic<int>>(0);
    const CookSession::ImporterFn countingImporter =
        [importCalls](std::span<const std::byte> bytes, const Guid& g, const TextureMetaSettings& settings)
        {
            importCalls->fetch_add(1, std::memory_order_relaxed);
            return ImportTexture(bytes, g, settings);
        };

    const fs::path expectedArtifact = ExpectedArtifactPath(project, png, TextureMetaSettings{});

    std::vector<std::string> firedFailures;   // "watched firing": progress observed each failure

    CookSession sessionOne;
    sessionOne.SetTextureImporterForTesting(countingImporter);
    sessionOne.SetProgress([&](const fs::path& source, bool ok, const std::string& detail)
    {
        if (!ok) firedFailures.push_back(source.filename().string() + ": " + detail);
    });

    const CookResult firstResult = sessionOne.CookProject(project);
    CHECK(firstResult.cooked == 0u);
    CHECK(firstResult.failed == 1u);
    CHECK(importCalls->load() == 1);
    CHECK_FALSE(fs::exists(expectedArtifact));
    REQUIRE(sessionOne.LastFailures().count(guid) == 1u);
    CHECK_FALSE(sessionOne.LastFailures().at(guid).empty());
    REQUIRE(firedFailures.size() == 1u);

    // A SECOND, independent session (a fresh CookSession -- simulating a second arccook
    // process/run over the SAME Intermediate/ tree) must still report the failure but must
    // NOT invoke the importer again for the same failing key.
    CookSession sessionTwo;
    sessionTwo.SetTextureImporterForTesting(countingImporter);
    std::vector<std::string> secondFired;
    sessionTwo.SetProgress([&](const fs::path& source, bool ok, const std::string& detail)
    {
        if (!ok) secondFired.push_back(source.filename().string() + ": " + detail);
    });

    const CookResult secondResult = sessionTwo.CookProject(project);
    CHECK(secondResult.cooked == 0u);
    CHECK(secondResult.failed == 1u);
    CHECK(importCalls->load() == 1);   // THE pin: still 1 -- no retry storm across sessions.
    CHECK_FALSE(fs::exists(expectedArtifact));
    REQUIRE(sessionTwo.LastFailures().count(guid) == 1u);
    CHECK_FALSE(sessionTwo.LastFailures().at(guid).empty());
    REQUIRE(secondFired.size() == 1u);   // the failure is observable again, from the memo alone.
}

// ---- Fix-loop finding: ResolveCurrentArtifactPath must never resolve an orphan --------

TEST_CASE("pipeline: ResolveCurrentArtifactPath selects the CURRENT cook key, never an orphaned one", "[pipeline]")
{
    // A `.meta` settings edit used to leave the ORIGINAL artifact on disk under its OLD
    // cook key -- an orphan Task 5 explicitly permitted (no sweep that task) -- while
    // BOTH the orphan and the fresh artifact carried the SAME sourceGuid header
    // (ArtifactFormat never changes a texture's identity, only its cook key, on a
    // settings edit). The fix-loop finding: --dump-dds used to resolve via
    // ArtifactStore::RebuildIndexFromScan + Lookup, whose Guid -> key index is
    // last-write-wins over an undefined directory-iteration order across exactly this
    // pair of files -- so it could non-deterministically hand back the STALE artifact.
    // This pins the fix: ResolveCurrentArtifactPath must select the artifact at TODAY's
    // recomputed cook key, and nothing else, every time -- now doubly true since the
    // FINAL-REVIEW WAVE's C1(a) fix makes CookProject itself remove the superseded old
    // key's artifact on the very same recook that supersedes it (see the settings-edit
    // recook test below for that removal pinned directly); this test's own value is
    // unchanged either way, since it never depended on the orphan's continued existence
    // to make its point -- it just no longer has one to work around.
    const fs::path project = TempProjectDir("resolve_current_artifact");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 5, 6, 7, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 1u);

    const fs::path orphanPath = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(fs::exists(orphanPath));

    // Flip srgb -- a NEW cook key.
    nlohmann::json flipped;
    flipped["srgb"] = false;
    WriteMetaSidecar(png, guid, flipped);
    REQUIRE(session.CookProject(project).cooked == 1u);

    TextureMetaSettings newSettings{};
    newSettings.srgb = false;
    const fs::path currentPath = ExpectedArtifactPath(project, png, newSettings);
    REQUIRE(fs::exists(currentPath));
    // C1(a) fix (final-review wave): the OLD key's artifact is now REMOVED by the same
    // CookProject call that superseded it -- self-healing, not merely tolerated as an
    // orphan (see the dedicated settings-edit-recook test below for this pinned in
    // isolation).
    CHECK_FALSE(fs::exists(orphanPath));
    REQUIRE(orphanPath != currentPath);       // the two paths were always distinct values

    const std::optional<fs::path> resolved = session.ResolveCurrentArtifactPath(project, guid);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == currentPath);
    CHECK(*resolved != orphanPath);           // THE pin: never the orphan, deterministically

    // A guid with no matching source at all resolves to nothing.
    CHECK_FALSE(session.ResolveCurrentArtifactPath(project, Guid::Generate()).has_value());

    // A guid WITH a matching source that has never been cooked also resolves to
    // nothing -- "current key has no artifact" must never fall back to any other file.
    const fs::path uncookedPng = project / "Content" / "textures" / "uncooked.png";
    WritePngFile(uncookedPng, 4, 4, SolidPixels(4, 4, 9, 9, 9, 255));
    const Guid uncookedGuid = Guid::Generate();
    WriteMetaSidecar(uncookedPng, uncookedGuid);
    CHECK_FALSE(session.ResolveCurrentArtifactPath(project, uncookedGuid).has_value());
}

// ---- C1(a) fix (final-review wave, 2026-09-04): a recook removes the superseded --------
// ---- same-guid artifact, both halves of the finding: settings-only edits (sourceHash ---
// ---- UNCHANGED) and source-content edits (sourceHash CHANGED). --------------------------
//
// The defect: a recook under a NEW cook key never removed the OLD key's artifact for the
// SAME guid. SweepOrphans is guid-keyed and never caught it (the guid stays live -- only
// the key moved). Two distinct consequences, both closed by the same fix in CookProject's
// success path (both the "up to date" and "freshly committed" branches): a SOURCE edit
// leaves a stale artifact whose header sourceHash no longer matches the current source
// (the client's FindArtifactForGuid/ResolveArtifact fix, C1b, covers a client still
// finding this one in a directory scan); a SETTINGS-ONLY edit leaves a stale artifact
// whose header sourceHash STILL matches the current source (only the settings changed,
// not the bytes) -- meaning that stale artifact would hash-VALIDATE as if it were current,
// which no amount of client-side validation logic can tell apart from the real one. Only
// removing it at the STORE level (this fix) closes that half.

TEST_CASE("pipeline: a settings-only-edit recook removes the superseded old-key artifact "
          "(C1a) -- the half a client CANNOT detect by hash validation alone", "[pipeline]")
{
    const fs::path project = TempProjectDir("c1a_settings_edit_removes_superseded");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 12, 34, 56, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 1u);
    const fs::path oldPath = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(fs::exists(oldPath));

    // Settings-only edit -- the PNG bytes on disk are completely untouched, only the
    // sidecar's "texture" block changes, so the stale artifact's sourceHash will still
    // match the current source bytes exactly.
    nlohmann::json flipped;
    flipped["srgb"] = false;
    WriteMetaSidecar(png, guid, flipped);

    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 1u);

    TextureMetaSettings newSettings{};
    newSettings.srgb = false;
    const fs::path newPath = ExpectedArtifactPath(project, png, newSettings);
    REQUIRE(fs::exists(newPath));
    REQUIRE(newPath != oldPath);

    // THE pin: the old key's artifact is gone -- not merely superseded in the index, but
    // physically removed, which is the only thing that closes the settings-only-edit half
    // of C1 (a stale artifact here would otherwise hash-validate as current forever).
    CHECK_FALSE(fs::exists(oldPath));
}

TEST_CASE("pipeline: a source-content-edit recook removes the superseded old-key artifact "
          "(C1a), the OTHER half alongside the client's own multi-candidate resolution (C1b)",
          "[pipeline]")
{
    const fs::path project = TempProjectDir("c1a_source_edit_removes_superseded");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 1, 1, 1, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 1u);
    const fs::path oldPath = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(fs::exists(oldPath));

    // A genuine SOURCE edit this time -- different pixel bytes, same settings -- so the
    // new cook key differs because sourceHash changed, not because settings did.
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 250, 250, 250, 255));
    const fs::path newPath = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(newPath != oldPath);

    // Asset-manager Plan 2 desk fix (2026-09-08): the editor's cook-pending
    // oracle (EditorApp::IsCookPending) now answers a row-less guid with
    // exactly this call, so the CURRENT-KEY discipline has to hold across the
    // whole edit->cook cycle, not just after it. BETWEEN the source edit and
    // the recook there is no artifact for today's key -- the pre-edit one is
    // still on disk at this instant -- and the honest answer is nullopt
    // ("queued"), never the stale file.
    CHECK_FALSE(session.ResolveCurrentArtifactPath(project, guid).has_value());

    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 1u);
    REQUIRE(fs::exists(newPath));

    // ...and once the cook lands, the SAME ask resolves -- the "Queued ->
    // Cooked" transition the editor badge reads off this function.
    const std::optional<fs::path> afterCook = session.ResolveCurrentArtifactPath(project, guid);
    REQUIRE(afterCook.has_value());
    CHECK(*afterCook == newPath);

    // THE pin: the pre-edit artifact is gone, same as the settings-only case above --
    // C1(a) does not distinguish WHY the key changed, only THAT it did.
    CHECK_FALSE(fs::exists(oldPath));
}

// ---- F2c Task 8 (spec s5.1, R6): the MESH half of the session-level contract -----------

TEST_CASE("cook session: a mesh source under Content/ cooks to a mesh artifact",
          "[pipeline]")
{
    // A project tree with ONE .glb + its sidecar and nothing else -- so this fails if
    // the spine still enumerates only .png.
    const fs::path project = TempDir("cook_mesh");
    fs::create_directories(project / "Content" / "meshes");
    fs::copy_file(MeshFixture("single.glb"), project / "Content" / "meshes" / "single.glb");
    const Guid meshGuid = Guid::Generate();
    WriteMetaSidecar(project / "Content" / "meshes" / "single.glb", meshGuid);

    CookSession session;
    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 1u);
    CHECK(result.failed == 0u);
    REQUIRE(result.cookedGuids.size() == 1u);
    CHECK(result.cookedGuids[0] == meshGuid);

    // And the committed bytes really are a MESH artifact, not a texture one.
    const auto path = session.ResolveCurrentArtifactPath(project, meshGuid);
    REQUIRE(path.has_value());
    CHECK(ReadMeshArtifact(*path).has_value());
}

TEST_CASE("cook session: textures and meshes cook in the SAME pass", "[pipeline]")
{
    // The spine iterates the kind TABLE, not one hardcoded extension.
    const fs::path project = TempDir("cook_mixed_pass");
    fs::create_directories(project / "Content" / "textures");
    fs::create_directories(project / "Content" / "meshes");

    const fs::path png = project / "Content" / "textures" / "uv_marker.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 11, 22, 33, 255));
    const Guid texGuid = Guid::Generate();
    WriteMetaSidecar(png, texGuid);

    const fs::path glb = project / "Content" / "meshes" / "single.glb";
    fs::copy_file(MeshFixture("single.glb"), glb);
    const Guid meshGuid = Guid::Generate();
    WriteMetaSidecar(glb, meshGuid);

    CookSession session;
    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 2u);
    CHECK(result.failed == 0u);
    REQUIRE(result.cookedGuids.size() == 2u);
    CHECK(std::find(result.cookedGuids.begin(), result.cookedGuids.end(), texGuid)
          != result.cookedGuids.end());
    CHECK(std::find(result.cookedGuids.begin(), result.cookedGuids.end(), meshGuid)
          != result.cookedGuids.end());
}

TEST_CASE("cook session: editing an external .bin restales the .gltf (spec s5.4)",
          "[pipeline]")
{
    // The END-TO-END form of Task 5's key case: CheckProject must go clean -> stale
    // when ONLY the buffer changed. A key over the .gltf alone passes this wrongly.
    const fs::path project = TempDir("cook_mesh_external_buffer_restale");
    fs::create_directories(project / "Content" / "meshes");

    const fs::path gltf = project / "Content" / "meshes" / "nested.gltf";
    const fs::path bin = project / "Content" / "meshes" / "nested.bin";
    fs::copy_file(MeshFixture("nested.gltf"), gltf);
    fs::copy_file(MeshFixture("nested.bin"), bin);
    const Guid meshGuid = Guid::Generate();
    WriteMetaSidecar(gltf, meshGuid);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 1u);
    CHECK_FALSE(session.CheckProject(project));

    // Append ONE byte to the EXTERNAL buffer -- the .gltf's own bytes are untouched.
    {
        std::ofstream out(bin, std::ios::binary | std::ios::app);
        REQUIRE(out.good());
        out.put(static_cast<char>(0x7F));
    }

    CHECK(session.CheckProject(project));
}

TEST_CASE("cook session: a refused mesh reports the importer's OWN reason", "[pipeline]")
{
    // s4.5's diagnostics must survive the trip into CookResult::failures -- a spine
    // that flattened them to "mesh import failed" would make the extensionsRequired
    // rule useless in the Problems pane, which is the only place a user reads it.
    const fs::path project = TempDir("cook_mesh_refusal");
    fs::create_directories(project / "Content" / "meshes");

    const fs::path gltf = project / "Content" / "meshes" / "requires_draco.gltf";
    fs::copy_file(MeshFixture("requires_draco.gltf"), gltf);
    const Guid meshGuid = Guid::Generate();
    WriteMetaSidecar(gltf, meshGuid);

    CookSession session;
    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 0u);
    REQUIRE(result.failed == 1u);
    REQUIRE(result.failures.size() == 1u);
    CHECK(result.failures[0].first == meshGuid);
    CHECK(result.failures[0].second.find("KHR_draco_mesh_compression") != std::string::npos);
}

TEST_CASE("cook session: a mesh failure memoizes exactly like a texture failure",
          "[pipeline]")
{
    // The no-retry-storm contract is kind-agnostic, and it is SHARED code that makes
    // it so -- no mesh-specific memo logic exists. Driven through the injected
    // importer seam so invocations are COUNTED, not inferred, the same shape the
    // existing texture memoization case (above) uses.
    const fs::path project = TempDir("cook_mesh_memoized");
    fs::create_directories(project / "Content" / "meshes");

    const fs::path gltf = project / "Content" / "meshes" / "requires_draco.gltf";
    fs::copy_file(MeshFixture("requires_draco.gltf"), gltf);
    const Guid meshGuid = Guid::Generate();
    WriteMetaSidecar(gltf, meshGuid);

    auto importCalls = std::make_shared<std::atomic<int>>(0);
    const CookSession::MeshImporterFn countingImporter =
        [importCalls](std::span<const std::byte> bytes,
                       std::span<const std::span<const std::byte>> externalBuffers,
                       const fs::path& sourcePath, const Guid& g, const MeshMetaSettings& settings)
        {
            importCalls->fetch_add(1, std::memory_order_relaxed);
            return ImportMesh(bytes, externalBuffers, sourcePath, g, settings);
        };

    CookSession sessionOne;
    sessionOne.SetMeshImporterForTesting(countingImporter);
    const CookResult firstResult = sessionOne.CookProject(project);
    CHECK(firstResult.cooked == 0u);
    CHECK(firstResult.failed == 1u);
    CHECK(importCalls->load() == 1);

    // A SECOND, independent session (a fresh CookSession -- simulating a second arccook
    // process/run over the SAME Intermediate/ tree) must still report the failure but must
    // NOT invoke the importer again for the same failing key.
    CookSession sessionTwo;
    sessionTwo.SetMeshImporterForTesting(countingImporter);
    const CookResult secondResult = sessionTwo.CookProject(project);
    CHECK(secondResult.cooked == 0u);
    CHECK(secondResult.failed == 1u);
    CHECK(importCalls->load() == 1);   // THE pin: still 1 -- no retry storm across sessions.
}

// ---- Final-review fix I3 (2026-09-11): CookResult::upToDateGuids -----------------------------

TEST_CASE("cook session: a second pass over an unchanged source reports its guid in "
          "upToDateGuids (and not in cookedGuids)", "[pipeline]")
{
    // The editor's companion .arcmesh mint used to fire over cookedGuids ONLY -- so a
    // project cooked headlessly by arccook before its first editor open, or a
    // byte-identical second drop (same bytes, same cook key, artifact already there),
    // reported upToDate and never minted until the source was touched. The additive
    // field this case pins is what OnCookCompleted's up-to-date mint loop reads.
    const fs::path project = TempDir("cook_mesh_uptodate_guids");
    fs::create_directories(project / "Content" / "meshes");
    fs::copy_file(MeshFixture("single.glb"), project / "Content" / "meshes" / "single.glb");
    const Guid meshGuid = Guid::Generate();
    WriteMetaSidecar(project / "Content" / "meshes" / "single.glb", meshGuid);

    CookSession session;
    const CookResult first = session.CookProject(project);
    REQUIRE(first.cooked == 1u);
    CHECK(first.upToDate == 0u);
    CHECK(first.upToDateGuids.empty());
    REQUIRE(first.cookedGuids.size() == 1u);
    CHECK(first.cookedGuids[0] == meshGuid);

    const CookResult second = session.CookProject(project);
    CHECK(second.cooked == 0u);
    CHECK(second.cookedGuids.empty());
    CHECK(second.failed == 0u);
    REQUIRE(second.upToDate == 1u);
    REQUIRE(second.upToDateGuids.size() == 1u);   // the count and the list agree
    CHECK(second.upToDateGuids[0] == meshGuid);
}

// ---- Project texture defaults (settings arc S6-6) -----------------------------------------

TEST_CASE("pipeline: an absent .meta texture field cooks with the session's project defaults",
          "[pipeline][sweep][texture-import]")
{
    const fs::path project = TempProjectDir("project-texture-defaults");
    const fs::path pngAbsent = project / "Content" / "textures" / "absent.png";
    const fs::path pngSet = project / "Content" / "textures" / "set.png";
    WritePngFile(pngAbsent, 4, 4, SolidPixels(4, 4, 10, 20, 30, 255));
    WritePngFile(pngSet, 4, 4, SolidPixels(4, 4, 40, 50, 60, 255));
    WriteMetaSidecar(pngAbsent, Guid::Generate());                                    // no "texture" block
    WriteMetaSidecar(pngSet, Guid::Generate(), nlohmann::json{ { "srgb", true } });  // overrides srgb only

    TextureMetaSettings projectDefaults{};
    projectDefaults.srgb = false;
    CookSession session;
    session.SetTextureDefaults(projectDefaults);
    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 2u);
    CHECK(result.failed == 0u);

    TextureMetaSettings overridden = projectDefaults;
    overridden.srgb = true;
    CHECK(fs::exists(ExpectedArtifactPath(project, pngAbsent, projectDefaults)));   // resolved to the project
    CHECK(fs::exists(ExpectedArtifactPath(project, pngSet, overridden)));          // the .meta wins
    CHECK_FALSE(fs::exists(ExpectedArtifactPath(project, pngAbsent, TextureMetaSettings{})));

    // A session with the same defaults agrees the project is current; one with
    // other defaults sees the absent-field texture as needing a cook.
    CookSession same;
    same.SetTextureDefaults(projectDefaults);
    CHECK_FALSE(same.CheckProject(project));
    CookSession structDefaults;
    CHECK(structDefaults.CheckProject(project));
}

// The out-of-editor bootstrap arccook runs (ApplyProjectCookConfig): the project's own
// Config rung reaches Settings<TextureMetaSettings>(), and a session seeded with it cooks
// an absent-field texture under a different key than a struct-default cook.
TEST_CASE("pipeline: ApplyProjectCookConfig applies the project's Config rung to the cook defaults",
          "[pipeline][sweep][texture-import][settings]")
{
    using Arcane::CVarRegistry;
    using Arcane::SetBy;

    // The global registry outlives this case: drop the Project rung it adds, even on failure.
    struct ProjectRungGuard
    {
        ~ProjectRungGuard()
        {
            CVarRegistry& reg = CVarRegistry::Get();
            (void)reg.RevertRung("assets.import.texture.maxSize", SetBy::Project);
            (void)reg.RevertRung("assets.import.texture.srgb", SetBy::Project);
            reg.PublishImmediate();
        }
    } guard;

    const fs::path project = TempProjectDir("project-cook-config");
    fs::create_directories(project / "Config");
    {
        std::ofstream out(project / "Config" / "assets.json", std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << R"({ "import": { "texture": { "maxSize": 2, "srgb": false } } })";
    }
    const fs::path png = project / "Content" / "textures" / "solo.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 10, 20, 30, 255));
    WriteMetaSidecar(png, Guid::Generate());   // no "texture" block: every field is the project's

    const TextureMetaSettings resolved = ApplyProjectCookConfig(project);
    CHECK(resolved.maxSize == 2u);
    CHECK_FALSE(resolved.srgb);
    CHECK(resolved.format == TextureMetaSettings{}.format);        // fields the file leaves out keep the default
    CHECK(resolved.generateMips == TextureMetaSettings{}.generateMips);
    CHECK(Arcane::Settings<TextureMetaSettings>() == resolved);   // published, not merely pending
    const auto explain = CVarRegistry::Get().Explain("assets.import.texture.maxSize");
    REQUIRE(explain.has_value());
    CHECK(explain->setBy == SetBy::Project);

    CookSession session;
    session.SetTextureDefaults(resolved);
    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 1u);
    CHECK(result.failed == 0u);

    const fs::path projectArtifact = ExpectedArtifactPath(project, png, resolved);
    const fs::path defaultArtifact = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    CHECK(projectArtifact != defaultArtifact);   // the cook key carries the project's values
    CHECK(fs::exists(projectArtifact));
    CHECK_FALSE(fs::exists(defaultArtifact));

    // A struct-default session (a cook that skipped the rung) sees the texture as stale.
    CookSession structDefaults;
    CHECK(structDefaults.CheckProject(project));
}

// ---- Review fix round 1, Task 10: every non-current artifact for a guid goes, and an ----
// ---- "up to date" artifact must still parse. --------------------------------------------

namespace
{
    std::size_t CountArtifactFiles(const fs::path& project)
    {
        std::size_t n = 0;
        std::error_code ec;
        const fs::path dir = project / "Intermediate" / "Artifacts";
        if (!fs::exists(dir, ec)) return 0;
        for (const auto& e : fs::recursive_directory_iterator(dir, ec))
            if (e.is_regular_file() && e.path().extension() == ".arcart") ++n;
        return n;
    }
}

TEST_CASE("pipeline: a settings-only recook deletes EVERY non-current artifact for the guid, "
          "including one that was never the indexed key", "[pipeline]")
{
    const fs::path project = TempProjectDir("t10_removes_all_non_current");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 12, 34, 56, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 1u);
    const fs::path oldPath = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(fs::exists(oldPath));

    // A second file for the same guid under a different key name: its prefix still names the
    // guid, so the store's scan can index it, but it is not the key the recook supersedes.
    const fs::path stray = oldPath.parent_path() / "00000000000000aa.arcart";
    std::error_code ec;
    fs::copy_file(oldPath, stray, fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(CountArtifactFiles(project) == 2u);

    nlohmann::json flipped;
    flipped["srgb"] = false;
    WriteMetaSidecar(png, guid, flipped);

    CookSession again;   // a fresh session: its store scan sees both files
    const CookResult result = again.CookProject(project);
    CHECK(result.cooked == 1u);
    CHECK(result.failed == 0u);

    TextureMetaSettings newSettings{};
    newSettings.srgb = false;
    const fs::path newPath = ExpectedArtifactPath(project, png, newSettings);
    REQUIRE(fs::exists(newPath));
    CHECK_FALSE(fs::exists(oldPath));
    CHECK_FALSE(fs::exists(stray));
    CHECK(CountArtifactFiles(project) == 1u);
}

TEST_CASE("pipeline: a truncated artifact at the current key is re-cooked, not counted up to date",
          "[pipeline]")
{
    const fs::path project = TempProjectDir("t10_truncated_current");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 7, 8, 9, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    {
        CookSession first;
        REQUIRE(first.CookProject(project).cooked == 1u);
    }
    const fs::path current = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(fs::exists(current));

    // An unrelated unparseable file in the bucket must not matter either way.
    const fs::path junk = current.parent_path() / "00000000000000bb.arcart";
    {
        std::ofstream out(junk, std::ios::binary | std::ios::trunc);
        out << "junk";
    }

    {
        std::ofstream out(current, std::ios::binary | std::ios::trunc);   // truncate in place
        out << "ARC";
    }
    REQUIRE_FALSE(ReadTextureArtifact(current).has_value());

    CookSession second;
    const CookResult result = second.CookProject(project);
    CHECK(result.upToDate == 0u);
    CHECK(result.cooked == 1u);
    CHECK(result.failed == 0u);
    CHECK(ReadTextureArtifact(current).has_value());

    // And a healthy current artifact is still up to date.
    CookSession third;
    const CookResult healthy = third.CookProject(project);
    CHECK(healthy.upToDate == 1u);
    CHECK(healthy.cooked == 0u);
}

// ---- Review fix round 1, B4 fix 1: --check validates the current artifact too ------------

TEST_CASE("pipeline: CheckProject reports a truncated current artifact as needing a cook",
          "[pipeline]")
{
    const fs::path project = TempProjectDir("t10_check_truncated_current");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 3, 6, 9, 255));
    WriteMetaSidecar(png, Guid::Generate());

    CookSession session;
    REQUIRE(session.CookProject(project).cooked == 1u);
    CHECK_FALSE(session.CheckProject(project));   // healthy -> clean

    const fs::path current = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    REQUIRE(fs::exists(current));
    {
        std::ofstream out(current, std::ios::binary | std::ios::trunc);
        out << "ARC";
    }
    REQUIRE_FALSE(ReadTextureArtifact(current).has_value());

    CHECK(session.CheckProject(project));   // exists but does not parse -> stale

    REQUIRE(session.CookProject(project).cooked == 1u);   // the cook heals it
    CHECK_FALSE(session.CheckProject(project));
}

#if defined(_WIN32)
TEST_CASE("pipeline: a superseded artifact held open by a reader fails the guid instead of reporting it cooked",
          "[pipeline]")
{
    const fs::path project = TempProjectDir("t10_stray_held_open");
    const fs::path png = project / "Content" / "textures" / "a.png";
    WritePngFile(png, 4, 4, SolidPixels(4, 4, 21, 22, 23, 255));
    const Guid guid = Guid::Generate();
    WriteMetaSidecar(png, guid);

    {
        CookSession first;
        REQUIRE(first.CookProject(project).cooked == 1u);
    }
    const fs::path current = ExpectedArtifactPath(project, png, TextureMetaSettings{});
    const fs::path stray = current.parent_path() / "00000000000000cc.arcart";
    std::error_code ec;
    fs::copy_file(current, stray, fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);

    // An ifstream opens without FILE_SHARE_DELETE, so the remove cannot succeed.
    std::ifstream holder(stray, std::ios::binary);
    REQUIRE(holder.good());

    CookSession second;
    const CookResult result = second.CookProject(project);
    CHECK(result.failed == 1u);
    CHECK(result.upToDate == 0u);
    CHECK(result.cooked == 0u);
    REQUIRE(result.failures.size() == 1u);
    CHECK(result.failures[0].first == guid);
    CHECK(fs::exists(stray));
}
#endif
