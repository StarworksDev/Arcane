#pragma once
#include <Arcane/Modules/ArcaneAssetPipelineApi.hpp>

// Arcane::AssetPipeline::ArtifactStore -- the hash-flat artifact store, rooted at
// <project>/Intermediate/. Artifacts live at Artifacts/<hh>/<hex>.arcart, where <hex> is the
// cook key as 16 lowercase hex digits (zero-padded) and <hh> is its first byte (its first two
// hex digits) -- a flat 256-way shard, deliberately NOT mirroring source paths (spec s4: hash-
// flat over path mirroring, for staleness -- an artifact's location depends only on its cook
// key, never on where its source asset happens to live).
//
// Concurrency contract (load-bearing for Task 5's parallel arccook stagers, run under MSBuild
// /m against the same ReferenceProject): concurrent Commit() calls for the SAME cook key --
// same process or different processes -- are safe. Each invocation writes to its OWN tmp path
// (process id + a process-local monotonic counter, never the cook key alone), so two racing
// writers can never share a file and interleave/corrupt each other's bytes -- each writer's
// bytes land intact at a private location before either side ever attempts a rename. Whichever
// invocation's rename lands LAST simply wins the final path; because the cook key is itself a
// content hash, two Commits racing for the same key are, by construction, writing byte-
// identical content, so "last rename wins" only has to be SAFE, never has to be
// deterministic about which racer wins.
//
// The in-memory index (Guid -> cook key) is a CACHE, never authoritative: every artifact header
// already carries its own source Guid (ArtifactFormat's TextureArtifactDesc::sourceGuid), and
// the cook key is recoverable from the artifact's own filename -- so RebuildIndexFromScan can
// always reconstruct the index from the Artifacts/ directory alone. Losing it is never data
// loss.
//
// Cross-process note: the cook key includes the texture settings, so two cookers that resolve
// DIFFERENT project defaults for the same source (arccook's ApplyProjectCookConfig vs the
// editor's Settings<TextureMetaSettings>) disagree about which artifact is current, and each
// one's RemoveAllExcept deletes the other's on every pass. Keep the defaults identical.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Arcane/Guid.hpp"

namespace Arcane::AssetPipeline
{
    class ARC_ASSET_PIPELINE_API ArtifactStore
    {
    public:
        // `intermediateDir` is the project's Intermediate/ directory itself (e.g.
        // "<project>/Intermediate"); artifacts live under intermediateDir/Artifacts/.
        explicit ArtifactStore(std::filesystem::path intermediateDir);

        // Artifacts/<hh>/<hex>.arcart under the store root. Pure -- does not touch disk, does
        // not require the file to exist.
        [[nodiscard]] std::filesystem::path PathFor(std::uint64_t cookKey) const;

        // Atomic commit. `writer` is handed a tmp path PRIVATE TO THIS INVOCATION -- never
        // shared with any other Commit() call, even a concurrent one for the SAME cook key (see
        // the concurrency contract above) -- and must write a complete file there, returning
        // true on success. Commit renames the tmp file into place ONLY when writer returns true;
        // on writer returning false, on writer THROWING (the exception is caught here -- Commit
        // never propagates it, matching its plain-bool contract), or on the rename itself
        // failing, the tmp file is removed and Commit returns false. Either way, on any false
        // return there is NO file at the final path and no stray tmp file left behind.
        [[nodiscard]] bool Commit(std::uint64_t cookKey,
                                   const std::function<bool(const std::filesystem::path& tmpPath)>& writer);

        // Guid -> cook key, via the in-memory index only (populated by PutIndex/
        // RebuildIndexFromScan) -- does not touch disk.
        [[nodiscard]] std::optional<std::uint64_t> Lookup(const Guid& guid) const;

        // Records/overwrites one Guid -> cook key mapping in the in-memory index.
        void PutIndex(const Guid& guid, std::uint64_t cookKey);

        // Clears the in-memory index and repopulates it by scanning Artifacts/**/*.arcart: the
        // cook key comes from each file's own name (the store's own naming contract), the Guid
        // from that artifact's own header via ArtifactFormat::ReadArtifactPrefix -- a BOUNDED,
        // KIND-AGNOSTIC read (headers are self-describing, so nothing but the artifact files
        // themselves is needed). F2c Task 8: this used to call ReadTextureArtifact, which fails
        // closed on any contentKind != Texture -- correct for a texture READ, but it made this
        // scan blind to every mesh artifact (a mesh could be cooked and committed to disk and
        // still never appear in the index, no matter how many times it was recooked). Reading
        // only the common prefix recognises every content kind's sourceGuid without committing
        // to one. A file that fails to parse as a 16-hex-digit cook key, or fails to load as a
        // valid artifact prefix, is skipped rather than aborting the scan.
        void RebuildIndexFromScan();

        // Removes every on-disk artifact currently in the in-memory index whose Guid is absent
        // from `liveGuids`, and drops those entries from the index. Artifacts for a live Guid are
        // left completely untouched. Returns the number of artifact files actually removed. Call
        // RebuildIndexFromScan first for a sweep grounded in the current disk state.
        std::size_t SweepOrphans(const std::unordered_set<Guid>& liveGuids);

        // Deletes every artifact file under Artifacts/ whose prefix names `guid` and whose
        // file stem is not `keepKey` -- the files RebuildIndexFromScan's scan recorded, removing
        // EVERY such file, not just the one key the index happened to hold (last writer wins
        // there). Files that are still present afterwards (a locked file, a failed remove) are
        // appended to `*remaining` when it is non-null. Returns the number removed. Prunes the
        // removed files from the recorded scan list (m_allByGuid); the key index is untouched.
        //
        // It works off the file list RebuildIndexFromScan recorded (call that first, once per
        // pass): the removals never walk the directory again, so a pass over N sources costs
        // one scan, not N. A file written after that scan is not seen. If the scan itself
        // could not be completed, `*remaining` receives the Artifacts directory so the caller
        // fails the guid instead of reporting it clean. On Windows a reader that holds a stale
        // file open through std::ifstream (no FILE_SHARE_DELETE) makes the remove fail; the
        // file lands in `*remaining` and the next pass retries.
        std::size_t RemoveAllExcept(const Guid& guid, std::uint64_t keepKey,
                                    std::vector<std::filesystem::path>* remaining);

    private:
        std::filesystem::path m_intermediateDir;
        std::unordered_map<Guid, std::uint64_t> m_index;
        // Every parsed artifact file the last scan saw, by source guid (the index keeps one
        // key per guid; this keeps them all). RemoveAllExcept reads and prunes it.
        std::unordered_map<Guid, std::vector<std::filesystem::path>> m_allByGuid;
        bool m_scanComplete = false;   // false until a scan finishes cleanly (and when one hit an I/O error part-way)
    };
}
