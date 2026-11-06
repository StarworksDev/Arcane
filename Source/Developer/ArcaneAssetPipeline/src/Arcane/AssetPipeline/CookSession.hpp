#pragma once
#include <Arcane/Modules/ArcaneAssetPipelineApi.hpp>

// Arcane::AssetPipeline::CookSession -- F2b Task 5: the shared cook orchestration both
// arccook (this task's CLI, Source/Programs/arccook/src/main.cpp) and the editor's in-process cook
// (Task 12) drive VERBATIM, so the CLI and the editor never diverge on what "stale" or
// "cooked" means. A CookSession owns no state across calls beyond the last CookProject
// call's failure map -- every CookProject/CheckProject call opens its own
// Arcane::AssetPipeline::ArtifactStore rooted at <projectDir>/Intermediate and reads the
// project's Content/ tree fresh, so a session is cheap to construct once per process
// (arccook's main.cpp) or reuse across many calls (a future editor cook button).
//
// F2c Task 8 (spec s5.1, R6): generalized the spine to cover TWO cookable kinds --
// Texture and, as of this task, Mesh -- over the CookKinds() table below. R6's recorded
// fallback (generalize only the store/index, keep sessions separate) is NOT taken here,
// and the reason is worth stating: the kind table carries DATA (extensions, meta-block
// key) while the per-kind work stays in per-kind private members/dispatch. Iterating a
// data table and dispatching on a small closed enum IS the generalized spine;
// type-erasing four differently-typed operations behind one std::function is the
// "awkward signatures" outcome the fallback was recorded against, wearing the same name.
// Texture behavior is BYTE-IDENTICAL across this widening -- every texture-facing type,
// string, and file layout is untouched; only the source enumeration/settings-read/cook-
// key/importer/writer calls are now reached through the kind table instead of being
// hardcoded to the texture path.
//
// Source enumeration: every source extension in the kind table (CookKinds() below --
// ".png" for Texture, ".gltf"/".glb" for Mesh) found under <projectDir>/Content
// (recursive) that has a sibling "<file>.meta" sidecar (Unity convention -- matches
// Source/Runtime/ArcaneEngine/src/Arcane/Project/AssetRegistry.cpp's ResolveSidecarId) is a cookable
// source. A source with no sidecar is not yet a registered asset -- CookSession never
// mints one (minting a guid is AssetRegistry's job, host-side only) -- it is silently
// skipped, never a failure. Nothing outside Content/ is ever considered: in particular
// ReferenceProject/Verify/References/*.png (the golden-image ORACLE) is never under
// Content/ and must never cook.
//
// Staleness: a source is up to date iff an artifact already exists at
// ArtifactStore::PathFor(<the source's kind-appropriate cook key over its CURRENT bytes
// and settings> -- ComputeCookKey for Texture, ComputeMeshCookKey for Mesh) -- a PURE,
// content-addressed check that needs no in-memory index and is correct even the very
// first time this process has ever seen the project. CheckProject uses exactly this
// check, and nothing else, to answer `anyStale`.
//
// Failure memoization: when a source's importer refuses it (TextureImporter::
// ImportTexture returning nullopt for corrupt/unrecognised bytes; MeshImporter::
// ImportMesh returning a non-empty MeshImportResult::refusal) for a given cook key, the
// reason is persisted to <projectDir>/Intermediate/Failed/<hex16>.fail (same hash-keyed
// spirit as ArtifactStore, deliberately simpler -- one flat file per key, no 256-way
// sharding, because failures are rare relative to artifacts). A later CookProject call
// -- same OR a different CookSession/process, sharing the same Intermediate/ tree -- for
// the SAME cook key reads that memo and reports the failure again WITHOUT ever
// re-invoking the importer: the "no retry storm" contract, kind-agnostic by construction
// because the memo read/write path is SHARED spine code, never duplicated per kind. The
// memo is itself keyed by the cook key (source bytes + settings + importer version), so
// it self-invalidates the moment any of those three change -- a fixed/edited source or a
// settings change computes a NEW cook key with no memo, and is attempted again.
// ArtifactStore::Commit failures (rare IO errors, not the source's fault) are
// deliberately NOT memoized -- they are retried every call, since the cost of retrying a
// commit is one file write, not a decode+encode. A mesh source whose external buffer
// (spec s5.4) cannot be read is likewise never memoized -- there is no stable cook key
// to memo it under yet (the key itself depends on the buffer's bytes) -- it is reported
// as a failure and simply retried next call, the same "cheap to retry" reasoning Commit
// failures get.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Arcane/AssetPipeline/MeshImporter.hpp"
#include "Arcane/AssetPipeline/TextureImporter.hpp"
#include "Arcane/AssetPipeline/TextureMetaSettings.hpp"
#include "Arcane/Guid.hpp"

namespace Arcane::AssetPipeline
{
    // The cookable kinds this spine covers. A small, CLOSED enum -- adding a third kind
    // is a one-row table edit plus one more switch arm at each of CookSession.cpp's four
    // per-kind dispatch points (settings type, cook-key builder, importer, artifact
    // writer), never a new sibling function.
    enum class CookKind : std::uint8_t { Texture, Mesh };

    // ONE ROW PER COOKABLE KIND (spec s5.1 / R6). DATA ONLY: the extensions that
    // identify a source, and the ".meta" object key its settings block lives under.
    // The per-kind WORK -- settings type, cook-key builder, importer, artifact writer
    // -- stays in CookSession's own per-kind private members, because those four
    // differ in their TYPES, not merely their behaviour.
    struct ARC_ASSET_PIPELINE_API CookKindEntry
    {
        CookKind                          kind;
        std::span<const std::string_view> extensions;    // {".png"} / {".gltf", ".glb"}
        const char*                       metaBlockKey;  // "texture" / "mesh"
    };

    // The kind table itself -- iterated by CookProject/CheckProject/
    // ResolveCurrentArtifactPath, each `for (kind : CookKinds()) for (source :
    // EnumerateSources(contentDir, kind.extensions))`. Backed by static storage; the
    // returned span is valid for the whole process lifetime.
    [[nodiscard]] ARC_ASSET_PIPELINE_API std::span<const CookKindEntry> CookKinds() noexcept;

    struct ARC_ASSET_PIPELINE_API CookResult
    {
        std::size_t cooked = 0;      // freshly imported + committed this call
        std::size_t upToDate = 0;    // already had a current artifact for that key
        std::size_t failed = 0;      // import failed (fresh or memoized) or commit failed

        // F2b Task 12: WHICH guids landed in the `cooked` bucket this call --
        // the editor's background cook queue needs this to invalidate exactly
        // the textures that changed (the Assets facade's per-guid memos, the
        // NriTextureCache GPU residency entry, the mesh-albedo bindless slot
        // memo), rather than sweeping every known guid in the project on
        // every cook pass. `arccook`'s CLI and CookSession's own [pipeline]
        // tests never read this -- purely additive, existing callers that
        // only look at the three counts above are unaffected.
        std::vector<Guid> cookedGuids;

        // Final-review fix I3 (2026-09-11): WHICH guids landed in the `upToDate`
        // bucket this call -- every kind, filled in the shared spine's up-to-date
        // branch beside `++upToDate`. The editor's companion mint
        // (EditorApp::OnCookCompleted -> MintOrUpdateCompanionMesh, spec s4.2)
        // runs over cookedGuids UNION upToDateGuids: an artifact that is already
        // current (a project cooked headlessly by arccook before its first editor
        // open, or a byte-identical second drop sharing the first's cook key)
        // reports here and NEVER in cookedGuids, and before this field existed
        // such a Model never got its .arcmesh until its source was touched. The
        // two cache INVALIDATIONS stay cookedGuids-only (nothing changed on disk
        // for an up-to-date guid). Additive: existing callers that read only the
        // counts or cookedGuids are unaffected.
        std::vector<Guid> upToDateGuids;

        // Guid -> human reason, for every guid that landed in the `failed`
        // bucket this call -- the SAME strings LastFailures() carries,
        // duplicated into the result itself (not read back out of
        // LastFailures() later) so a caller that consumes CookResult on a
        // DIFFERENT thread than the one that called CookProject (the
        // editor's cook queue runs CookProject on a JobSystem worker and
        // drains the result on the main thread) never races a LATER
        // CookProject call already reusing the same CookSession/
        // m_lastFailures.
        std::vector<std::pair<Guid, std::string>> failures;
    };

    class ARC_ASSET_PIPELINE_API CookSession
    {
    public:
        // Fires once per ATTEMPTED source (never for a skipped, sidecar-less ".png"):
        // `source` is the source file's path, `ok` is true for a fresh cook OR an
        // up-to-date hit, false for any failure (fresh or memoized); `detail` is a
        // short, human-readable reason on false (empty on true). Optional -- arccook's
        // --verbose wires this to stdout. CheckProject never calls it: it never
        // attempts an import, so there is nothing to report per-source.
        using ProgressFn = std::function<void(const std::filesystem::path& source, bool ok, const std::string& detail)>;
        void SetProgress(ProgressFn fn);

        // Test-only seam: overrides the TEXTURE importer CookProject calls (default is
        // Arcane::AssetPipeline::ImportTexture) so a test can COUNT invocations across
        // two separate CookSession instances/processes sharing one Intermediate/ tree,
        // instead of inferring the count indirectly. NEVER called by production code --
        // arccook's main.cpp and the future editor caller (Task 12) always get the real
        // importer. F2c Task 8: renamed from SetImporterForTesting now that there are
        // two importers to inject -- the unqualified name stopped being meaningful the
        // moment a second one existed, and leaving it is how a test ends up injecting
        // the wrong one.
        using ImporterFn = std::function<std::optional<ImportedTexture>(std::span<const std::byte>, const Guid&, const TextureMetaSettings&)>;
        void SetTextureImporterForTesting(ImporterFn fn);

        // The mesh half of the seam above -- same contract, same NEVER-called-by-
        // production-code rule, mirroring ImportMesh's own signature exactly (source
        // bytes, external buffers, source path, guid, settings) so a test's fake can
        // wrap the real ImportMesh verbatim, the same shape the texture seam's own
        // counting-fake tests already use.
        using MeshImporterFn = std::function<MeshImportResult(std::span<const std::byte>,
                                                                std::span<const std::span<const std::byte>>,
                                                                const std::filesystem::path&,
                                                                const Guid&,
                                                                const MeshMetaSettings&)>;
        void SetMeshImporterForTesting(MeshImporterFn fn);

        // Cooks every stale source (every kind in CookKinds()) under projectDir/Content,
        // writing artifacts to projectDir/Intermediate/Artifacts. See the file header
        // above for the full staleness/failure/memoization contract. Clears
        // LastFailures() at the start.
        [[nodiscard]] CookResult CookProject(const std::filesystem::path& projectDir);

        // Asks a CookProject running on another thread to stop: it checks this flag between
        // sources, so it returns (with the partial result) after at most the import in
        // flight. Sticky, thread-safe, never cleared -- a session that was told to stop is
        // being torn down (CookQueue's destructor), and a cancel that lands before the pass
        // starts must still win. The only unbounded wait left is a stuck importer.
        void RequestCancel() noexcept { m_cancel.store(true, std::memory_order_release); }
        [[nodiscard]] bool CancelRequested() const noexcept { return m_cancel.load(std::memory_order_acquire); }

        // Read-only: true iff at least one source (any kind in CookKinds()) under
        // projectDir/Content has no current artifact yet. Never imports, never writes to
        // disk -- the `--check` core, shared verbatim with CookProject's own staleness
        // test. Does not touch LastFailures() (it never imports, so it never has
        // anything to report).
        [[nodiscard]] bool CheckProject(const std::filesystem::path& projectDir) const;

        // Guid -> failure reason from the most recent CookProject call ONLY (cleared at
        // the start of every CookProject call).
        [[nodiscard]] const std::unordered_map<Guid, std::string>& LastFailures() const;

        // Resolves the CURRENT on-disk artifact path for `guid`: finds its source under
        // projectDir/Content, recomputes TODAY's cook key from the source's CURRENT
        // bytes + settings (the exact same key math CookProject/CheckProject use), and
        // returns store.PathFor(that key) IFF a file already exists there. Deliberately
        // NEVER goes through ArtifactStore::RebuildIndexFromScan/Lookup: that index maps
        // one Guid -> ONE cook key by last-write-wins over recursive_directory_iterator
        // order (undefined by contract), so when an orphaned artifact from an earlier
        // `.meta` settings edit still sits on disk under its OLD key -- both it and the
        // current artifact carry the SAME sourceGuid header -- the index can resolve to
        // either one non-deterministically. This function sidesteps that ambiguity
        // entirely by never consulting the index, exactly the same "existence-check on
        // the CURRENT key only" discipline CookProject/CheckProject already use for
        // staleness. Returns nullopt when no source matches `guid`, OR when the source's
        // CURRENT artifact hasn't been cooked yet (uncooked/stale) -- NEVER falls back to
        // any other artifact, orphaned or otherwise. Read-only: never imports, never
        // writes. arccook's --dump-dds is this function's only production caller today.
        //
        // FINAL-REVIEW FIX (C1a, 2026-09-04): CookProject's own success path now removes
        // an old-key artifact it supersedes for the SAME guid (best-effort -- see
        // CookProject's own comment), so the "orphan" case above is now the SHORT-LIVED
        // exception rather than the steady state. This function's defense stays anyway:
        // the removal is best-effort (a locked file, a concurrent reader, or an external
        // tool can leave one behind) and this function's whole point is to never need the
        // index to be clean to answer correctly.
        [[nodiscard]] std::optional<std::filesystem::path> ResolveCurrentArtifactPath(
            const std::filesystem::path& projectDir, const Guid& guid) const;

        // Settings arc S6-6: the values a texture's .meta "texture" block does NOT set
        // resolve to (TextureMetaSettings::FromMetaJson's defaults) -- the project's
        // assets.import.texture.*, Settings<TextureMetaSettings>(). Every session that
        // cooks or answers "is it current" for one project must carry the SAME defaults,
        // or the cook keys disagree. Not thread-safe against a concurrent CookProject:
        // set it on the thread that drives this session (CookQueue does so inside its pass).
        void SetTextureDefaults(const TextureMetaSettings& defaults) { m_textureDefaults = defaults; }
        [[nodiscard]] const TextureMetaSettings& TextureDefaults() const { return m_textureDefaults; }

    private:
        std::atomic<bool> m_cancel{ false };
        TextureMetaSettings m_textureDefaults{};
        ProgressFn m_progress;
        ImporterFn m_textureImporter;
        MeshImporterFn m_meshImporter;
        std::unordered_map<Guid, std::string> m_lastFailures;
    };
}
