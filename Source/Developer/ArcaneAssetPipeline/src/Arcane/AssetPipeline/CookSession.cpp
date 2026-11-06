#include "Arcane/AssetPipeline/CookSession.hpp"

#include "Arcane/AssetPipeline/ArtifactFormat.hpp"
#include "Arcane/AssetPipeline/ArtifactStore.hpp"
#include "Arcane/AssetPipeline/CookKey.hpp"

#include <Arcane/Core/Constant.hpp>
#include <Arcane/IO/ReadRegularFile.hpp>

#include <Json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

// Only for CurrentProcessId() below -- the failure-memo writer's per-invocation tmp path,
// same reasoning/mechanism as ArtifactStore.cpp's own (duplicated rather than shared: that
// helper lives in ArtifactStore.cpp's anonymous namespace, not exported).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace Arcane::AssetPipeline
{
    namespace fs = std::filesystem;

    namespace
    {
        // ---- Kind table (F2c Task 8: spec s5.1, R6) --------------------------------------
        // DATA ONLY -- see CookKindEntry's own doc comment (CookSession.hpp) for why the
        // per-kind WORK deliberately stays out of this table. Backed by static storage so
        // CookKinds() can return a span valid for the process lifetime.

        constexpr std::string_view kTextureExtensions[] = { ".png" };
        constexpr std::string_view kMeshExtensions[] = { ".gltf", ".glb" };

        constexpr std::array<CookKindEntry, 2> kCookKindTable{ {
            { CookKind::Texture, std::span<const std::string_view>(kTextureExtensions), "texture" },
            { CookKind::Mesh,    std::span<const std::string_view>(kMeshExtensions),    "mesh" },
        } };

        // ---- Source enumeration --------------------------------------------------------

        std::string LowerExt(const fs::path& p)
        {
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        bool MatchesAnyExtension(const fs::path& p, std::span<const std::string_view> extensions)
        {
            const std::string ext = LowerExt(p);
            for (const std::string_view& candidate : extensions)
                if (ext == candidate) return true;
            return false;
        }

        // Every source extension in the kind table (CookSession.hpp's CookKinds(), e.g.
        // ".png" for Texture or ".gltf"/".glb" for Mesh) under `contentDir` (recursive)
        // that has a sibling "<file>.meta" sidecar -- see CookSession.hpp's file header
        // for the "why" (Unity convention, never minted here). Sorted for deterministic
        // iteration order (no dependence on filesystem enumeration order across
        // runs/platforms). F2c Task 8: generalized from EnumerateTextureSources's own
        // hardcoded ".png" check to an arbitrary extension set -- the ".png" list is now
        // just Texture's own row of the kind table.
        std::vector<fs::path> EnumerateSources(const fs::path& contentDir,
                                                std::span<const std::string_view> extensions)
        {
            std::vector<fs::path> sources;

            std::error_code ec;
            if (!fs::exists(contentDir, ec) || ec) return sources;

            fs::recursive_directory_iterator it(contentDir, ec);
            if (ec) return sources;
            const fs::recursive_directory_iterator end;
            for (; it != end; it.increment(ec))
            {
                if (ec) break;

                const fs::directory_entry& entry = *it;
                std::error_code fileEc;
                if (!entry.is_regular_file(fileEc) || fileEc) continue;
                if (!MatchesAnyExtension(entry.path(), extensions)) continue;

                fs::path meta = entry.path();
                meta += ".meta";
                std::error_code metaEc;
                if (!fs::exists(meta, metaEc) || metaEc) continue;   // not a registered source -- skip

                sources.push_back(entry.path());
            }

            std::sort(sources.begin(), sources.end());
            return sources;
        }

        // ---- .meta reading ----------------------------------------------------------------
        // F2c Task 8 split: ReadSourceMeta used to hardcode the "texture" block. Split into
        // a KIND-AGNOSTIC guid read (every source, regardless of CookKind, carries its guid
        // at this same top-level key) and a per-kind settings-block read keyed by
        // CookKindEntry::metaBlockKey ("texture"/"mesh"). An absent block still means "all
        // defaults", unchanged -- that unchanged-ness is what keeps every existing
        // ".png.meta" in every project cooking to the same key.

        std::optional<nlohmann::json> ParseMetaDoc(const fs::path& metaPath)
        {
            std::ifstream in(metaPath, std::ios::binary);
            if (!in) return std::nullopt;

            nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
            in.close();
            if (!doc.is_object()) return std::nullopt;

            return doc;
        }

        // Tolerant read: absent/malformed guid never throws -- an invalid sidecar just
        // means "not a cookable source yet" (nullopt), same tolerant contract
        // AssetRegistry.cpp's own ReadGuidField/FromMetaJson both already keep.
        std::optional<Guid> ReadSourceGuid(const fs::path& metaPath)
        {
            const std::optional<nlohmann::json> doc = ParseMetaDoc(metaPath);
            if (!doc) return std::nullopt;

            const auto guidIt = doc->find("guid");
            if (guidIt == doc->end() || !guidIt->is_string()) return std::nullopt;
            const std::optional<Guid> guid = Guid::FromString(guidIt->get<std::string>());
            if (!guid || !guid->IsValid()) return std::nullopt;

            return guid;
        }

        // Reads the settings block at `blockKey` ("texture"/"mesh"), tolerant to it being
        // absent, the sidecar itself being unreadable/malformed, or the block being the
        // wrong JSON shape -- every one of those returns an empty object, which each
        // kind's own *MetaSettings::FromMetaJson already treats as "all defaults" (the
        // exact behavior ReadSourceMeta's own texture-only read had before this split).
        nlohmann::json ReadMetaBlock(const fs::path& metaPath, const char* blockKey)
        {
            const std::optional<nlohmann::json> doc = ParseMetaDoc(metaPath);
            if (!doc) return nlohmann::json::object();

            if (const auto it = doc->find(blockKey); it != doc->end() && it->is_object())
                return *it;
            return nlohmann::json::object();
        }

        std::optional<std::vector<std::byte>> ReadWholeFileBytes(const fs::path& path)
        {
            return Arcane::ReadRegularFile(path);
        }

        // Flattens ReadExternalBuffers' own result into the span-of-spans shape
        // ComputeMeshCookKey/ImportMesh both take. `storage` owns the bytes; the returned
        // spans view into it, so `storage` must outlive every use of the result.
        std::vector<std::span<const std::byte>> ToSpans(const std::vector<std::vector<std::byte>>& storage)
        {
            std::vector<std::span<const std::byte>> spans;
            spans.reserve(storage.size());
            for (const std::vector<std::byte>& buffer : storage)
                spans.emplace_back(buffer);
            return spans;
        }

        // ---- Failure memo: <intermediateDir>/Failed/<hex16>.fail -------------------------
        // One flat file per failing cook key -- no 256-way sharding like ArtifactStore's
        // artifacts (failures are rare relative to successful artifacts, so the directory
        // never grows large enough to need it). The file's whole content is the human-
        // readable reason string; existence alone is the memo signal. Kind-agnostic: the
        // cook key already encodes which kind's importer/settings produced it, and this
        // file only ever reads/writes by key, never by kind.

        ARC_CONSTANT("file format: a cook key (u64) as 16 hex digits in memo file names")
        constexpr std::size_t kHexDigits = 16;

        std::string ToHex16(std::uint64_t v)
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string s(kHexDigits, '0');
            for (std::size_t i = 0; i < kHexDigits; ++i)
            {
                const int shift = 4 * static_cast<int>(kHexDigits - 1 - i);
                s[i] = kDigits[(v >> shift) & 0xFu];
            }
            return s;
        }

        [[nodiscard]] std::uint32_t CurrentProcessId() noexcept
        {
#if defined(_WIN32)
            return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
            return static_cast<std::uint32_t>(::getpid());
#endif
        }

        [[nodiscard]] std::uint64_t NextInvocationId() noexcept
        {
            static std::atomic<std::uint64_t> counter{ 0 };
            return counter.fetch_add(1, std::memory_order_relaxed);
        }

        fs::path FailedDir(const fs::path& intermediateDir) { return intermediateDir / "Failed"; }

        fs::path FailedPathFor(const fs::path& intermediateDir, std::uint64_t cookKey)
        {
            return FailedDir(intermediateDir) / (ToHex16(cookKey) + ".fail");
        }

        std::optional<std::string> ReadFailureMemo(const fs::path& intermediateDir, std::uint64_t cookKey)
        {
            std::ifstream in(FailedPathFor(intermediateDir, cookKey), std::ios::binary);
            if (!in) return std::nullopt;
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        // Best-effort: a failure memo is a diagnostic convenience, not correctness-critical
        // (worst case on a write failure is one extra importer run next session). Written
        // via a per-invocation tmp + rename, same mechanism as ArtifactStore::Commit, so two
        // stagers racing to memoize the SAME failing key (both computing the SAME reason
        // string, since the reason is a pure function of the same source bytes + settings)
        // never interleave/corrupt each other's bytes.
        void WriteFailureMemo(const fs::path& intermediateDir, std::uint64_t cookKey, const std::string& reason)
        {
            const fs::path dir = FailedDir(intermediateDir);
            std::error_code ec;
            fs::create_directories(dir, ec);

            const fs::path finalPath = FailedPathFor(intermediateDir, cookKey);
            const fs::path tmpPath = dir
                / (ToHex16(cookKey) + "." + std::to_string(CurrentProcessId()) + "-"
                   + std::to_string(NextInvocationId()) + ".tmp");

            {
                std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
                if (!out) return;
                out << reason;
                if (!out.good()) { out.close(); fs::remove(tmpPath, ec); return; }
            }

            fs::rename(tmpPath, finalPath, ec);
            if (ec) fs::remove(tmpPath, ec);
        }
    }

    namespace
    {
        // True iff the file at `path` parses with the reader for `kind`. The static_assert fails
        // the build when a kind row is added to the table without a case below (C4062 is off by
        // default on MSVC, so the switch alone would not catch it).
        static_assert(kCookKindTable.size() == 2, "a new CookKind needs a case in CurrentArtifactParses");

        bool CurrentArtifactParses(CookKind kind, const fs::path& path)
        {
            switch (kind)
            {
            case CookKind::Texture: return ReadTextureArtifact(path).has_value();
            case CookKind::Mesh:    return ReadMeshArtifact(path).has_value();
            }
            return false;
        }
    }

    std::span<const CookKindEntry> CookKinds() noexcept
    {
        return std::span<const CookKindEntry>(kCookKindTable);
    }

    void CookSession::SetProgress(ProgressFn fn) { m_progress = std::move(fn); }
    void CookSession::SetTextureImporterForTesting(ImporterFn fn) { m_textureImporter = std::move(fn); }
    void CookSession::SetMeshImporterForTesting(MeshImporterFn fn) { m_meshImporter = std::move(fn); }

    const std::unordered_map<Guid, std::string>& CookSession::LastFailures() const { return m_lastFailures; }

    CookResult CookSession::CookProject(const fs::path& projectDir)
    {
        CookResult result;
        m_lastFailures.clear();

        const fs::path contentDir = projectDir / "Content";
        const fs::path intermediateDir = projectDir / "Intermediate";
        ArtifactStore store(intermediateDir);
        store.RebuildIndexFromScan();   // warms the index for future guid->key consumers (e.g. --dump-dds)

        for (const CookKindEntry& kindEntry : CookKinds())
        {
            for (const fs::path& source : EnumerateSources(contentDir, kindEntry.extensions))
            {
                // CookQueue's destructor asks a pass on a worker to stop; one import is the
                // longest the caller then waits.
                if (m_cancel.load(std::memory_order_acquire))
                    return result;

                fs::path metaPath = source;
                metaPath += ".meta";

                const std::optional<Guid> guid = ReadSourceGuid(metaPath);
                if (!guid) continue;   // no valid sidecar -- not a cookable source

                const std::optional<std::vector<std::byte>> bytes = ReadWholeFileBytes(source);
                if (!bytes) continue;   // unreadable file -- an I/O condition, not an import failure

                // ---- Per-kind leaf 1/4: settings + cook key. TYPES differ (TextureMetaSettings
                // vs MeshMetaSettings), so this stays a switch on the closed enum rather than a
                // shared template/std::function (R6's own judgment call -- see CookSession.hpp's
                // file header). Mesh additionally needs its external buffers (spec s5.4) for
                // BOTH the key and the importer call further below, so they are read here once
                // and carried through via `meshBufferSpans`.
                TextureMetaSettings textureSettings{};
                MeshMetaSettings meshSettings{};
                std::vector<std::vector<std::byte>> meshBufferStorage;
                std::vector<std::span<const std::byte>> meshBufferSpans;
                std::optional<std::uint64_t> cookKeyOpt;

                switch (kindEntry.kind)
                {
                case CookKind::Texture:
                {
                    textureSettings = TextureMetaSettings::FromMetaJson(ReadMetaBlock(metaPath, kindEntry.metaBlockKey),
                                                                        m_textureDefaults);
                    cookKeyOpt = ComputeCookKey(*bytes, textureSettings, kTextureImporterVersion);
                    break;
                }
                case CookKind::Mesh:
                {
                    meshSettings = MeshMetaSettings::FromMetaJson(ReadMetaBlock(metaPath, kindEntry.metaBlockKey));
                    const std::optional<std::vector<std::vector<std::byte>>> externalBuffers =
                        ReadExternalBuffers(*bytes, source);
                    if (!externalBuffers)
                    {
                        // An unreadable external buffer is a cook REFUSAL naming the file --
                        // unlike the silent "source bytes unreadable" skip above, this source
                        // WAS recognised as a registered mesh; it just can't be fully read.
                        // Not memoized: there is no stable cook key to memo it under yet (the
                        // key itself depends on the buffer's own bytes) -- same "cheap to
                        // retry" reasoning ArtifactStore::Commit failures get.
                        const std::string reason = "mesh import failed: '" + source.filename().string()
                            + "' references an external buffer that could not be read";
                        m_lastFailures[*guid] = reason;
                        result.failures.emplace_back(*guid, reason);
                        ++result.failed;
                        if (m_progress) m_progress(source, false, reason);
                        continue;
                    }
                    meshBufferStorage = std::move(*externalBuffers);
                    meshBufferSpans = ToSpans(meshBufferStorage);
                    cookKeyOpt = ComputeMeshCookKey(*bytes, meshBufferSpans, meshSettings, kMeshImporterVersion);
                    break;
                }
                }

                const std::uint64_t cookKey = *cookKeyOpt;
                const fs::path finalPath = store.PathFor(cookKey);

                // ---- Everything below is the SHARED spine -- written ONCE, per spec R6/this
                // task's own mandate: the up-to-date existence check, the failure memo (read
                // and write), the supersede-the-old-key removal, PutIndex, the progress
                // callback, and the result accounting are IDENTICAL for every kind. Only the
                // import+write dispatch further down switches on kindEntry.kind again (leaf
                // 2/4 through 4/4) -- this is what makes the mesh memoization case pass with
                // zero mesh-specific memo code.

                // C1 fix (final-review wave, 2026-09-04): captured BEFORE this pass touches
                // the index, from whatever RebuildIndexFromScan warmed it with above -- i.e.
                // this guid's key as of the LAST successful cook. A source edit changes
                // sourceHash (a new key); a SETTINGS-ONLY edit changes the key too even though
                // sourceHash stays the SAME -- either way, once this pass lands a DIFFERENT
                // key for the same guid below, the OLD key's artifact is superseded and must
                // not survive: SweepOrphans is guid-keyed and would never catch it (the guid
                // is still live, only the KEY moved), and a client-side directory scan by guid
                // (ArcaneClient's FindArtifactForGuid) can otherwise still find the stale file
                // -- for a settings-only edit, the stale artifact's sourceHash still matches
                // the CURRENT source bytes (only the settings changed), so it would even
                // hash-validate as if it were current, silently masking the settings change.
                // Review fix round 1: delete EVERY artifact for this guid except the current
                // key, not just the one key the index held (it is last-writer-wins, so a
                // stray same-guid file that was never the indexed key survived, and a
                // client-side directory scan by guid could still resolve it). Returns false
                // and records a failure when a stale file could not be removed: the guid is
                // then NOT up to date / cooked, since the stale bytes may still be served.
                auto removeNonCurrent = [&]() -> bool
                {
                    std::vector<fs::path> left;
                    store.RemoveAllExcept(*guid, cookKey, &left);
                    if (left.empty())
                        return true;

                    const std::string reason = "superseded artifact could not be removed";
                    m_lastFailures[*guid] = reason;
                    result.failures.emplace_back(*guid, reason);
                    ++result.failed;
                    if (m_progress) m_progress(source, false, reason);
                    return false;
                };

                // The up-to-date outcome needs the current-key file to exist AND parse with
                // the reader for its kind; a truncated or garbled file falls through into the
                // import path below, which rewrites it.
                std::error_code existsEc;
                if (fs::exists(finalPath, existsEc))
                {
                    if (CurrentArtifactParses(kindEntry.kind, finalPath))
                    {
                        store.PutIndex(*guid, cookKey);
                        if (!removeNonCurrent())
                            continue;
                        result.upToDateGuids.push_back(*guid);   // I3: every kind, beside the count
                        ++result.upToDate;
                        if (m_progress) m_progress(source, true, "up to date");
                        continue;
                    }
                }

                if (const std::optional<std::string> memo = ReadFailureMemo(intermediateDir, cookKey))
                {
                    m_lastFailures[*guid] = *memo;
                    result.failures.emplace_back(*guid, *memo);
                    ++result.failed;
                    if (m_progress) m_progress(source, false, *memo);
                    continue;   // memoized -- the importer NEVER runs again for this exact key
                }

                // ---- Per-kind leaves 2-4/4: importer call + artifact writer. TYPES differ
                // (ImportedTexture/WriteTextureArtifact vs MeshImportResult/WriteMeshArtifact),
                // so this is the spine's other, and only other, dispatch point.
                std::string failureReason;
                bool committed = false;

                switch (kindEntry.kind)
                {
                case CookKind::Texture:
                {
                    const std::optional<ImportedTexture> imported = m_textureImporter
                        ? m_textureImporter(*bytes, *guid, textureSettings)
                        : ImportTexture(*bytes, *guid, textureSettings);

                    if (!imported)
                    {
                        failureReason = "texture import failed: '" + source.filename().string()
                            + "' did not decode as a supported image (corrupt or unrecognised source bytes)";
                        break;
                    }

                    committed = store.Commit(cookKey, [&](const fs::path& tmp)
                    {
                        return WriteTextureArtifact(tmp, imported->desc, imported->payload, imported->thumbRgba);
                    });
                    break;
                }
                case CookKind::Mesh:
                {
                    const MeshImportResult imported = m_meshImporter
                        ? m_meshImporter(*bytes, meshBufferSpans, source, *guid, meshSettings)
                        : ImportMesh(*bytes, meshBufferSpans, source, *guid, meshSettings);

                    if (!imported.refusal.empty())
                    {
                        // s4.5's diagnostics survive VERBATIM into CookResult::failures -- the
                        // importer's OWN reason, never flattened to a generic "mesh import
                        // failed", which is what keeps the Problems pane actionable.
                        failureReason = imported.refusal;
                        break;
                    }

                    committed = store.Commit(cookKey, [&](const fs::path& tmp)
                    {
                        return WriteMeshArtifact(tmp, imported.mesh->desc, imported.mesh->vertices, imported.mesh->indices);
                    });
                    break;
                }
                }

                if (!failureReason.empty())
                {
                    WriteFailureMemo(intermediateDir, cookKey, failureReason);
                    m_lastFailures[*guid] = failureReason;
                    result.failures.emplace_back(*guid, failureReason);
                    ++result.failed;
                    if (m_progress) m_progress(source, false, failureReason);
                    continue;
                }

                if (!committed)
                {
                    // Deliberately NOT memoized -- see CookSession.hpp's file header: a commit
                    // failure is a rare IO condition independent of the source's own bytes, and
                    // retrying it next call costs one file write, not a decode+encode.
                    const std::string reason = "artifact commit failed for '" + source.filename().string()
                        + "' (disk write error)";
                    m_lastFailures[*guid] = reason;
                    result.failures.emplace_back(*guid, reason);
                    ++result.failed;
                    if (m_progress) m_progress(source, false, reason);
                    continue;
                }

                store.PutIndex(*guid, cookKey);
                if (!removeNonCurrent())
                    continue;
                result.cookedGuids.push_back(*guid);
                ++result.cooked;
                if (m_progress) m_progress(source, true, "cooked");
            }
        }

        return result;
    }

    bool CookSession::CheckProject(const fs::path& projectDir) const
    {
        const fs::path contentDir = projectDir / "Content";
        const fs::path intermediateDir = projectDir / "Intermediate";
        const ArtifactStore store(intermediateDir);

        for (const CookKindEntry& kindEntry : CookKinds())
        {
            for (const fs::path& source : EnumerateSources(contentDir, kindEntry.extensions))
            {
                fs::path metaPath = source;
                metaPath += ".meta";

                const std::optional<Guid> guid = ReadSourceGuid(metaPath);
                if (!guid) continue;

                const std::optional<std::vector<std::byte>> bytes = ReadWholeFileBytes(source);
                if (!bytes) continue;

                std::optional<std::uint64_t> cookKeyOpt;
                switch (kindEntry.kind)
                {
                case CookKind::Texture:
                {
                    const TextureMetaSettings settings =
                        TextureMetaSettings::FromMetaJson(ReadMetaBlock(metaPath, kindEntry.metaBlockKey), m_textureDefaults);
                    cookKeyOpt = ComputeCookKey(*bytes, settings, kTextureImporterVersion);
                    break;
                }
                case CookKind::Mesh:
                {
                    const MeshMetaSettings settings =
                        MeshMetaSettings::FromMetaJson(ReadMetaBlock(metaPath, kindEntry.metaBlockKey));
                    const std::optional<std::vector<std::vector<std::byte>>> externalBuffers =
                        ReadExternalBuffers(*bytes, source);
                    if (!externalBuffers)
                    {
                        // Can't even compute today's key -- CheckProject's whole contract is
                        // "does the CURRENT key have an artifact", and a source whose key
                        // cannot be computed certainly has no artifact for it: stale.
                        return true;
                    }
                    const std::vector<std::span<const std::byte>> spans = ToSpans(*externalBuffers);
                    cookKeyOpt = ComputeMeshCookKey(*bytes, spans, settings, kMeshImporterVersion);
                    break;
                }
                }

                // Same test CookProject's up-to-date branch applies: present AND parses. A
                // truncated file at the current key is stale, not clean.
                std::error_code ec;
                const fs::path current = store.PathFor(*cookKeyOpt);
                if (!fs::exists(current, ec) || !CurrentArtifactParses(kindEntry.kind, current))
                    return true;   // at least one stale source -- short-circuit
            }
        }

        return false;
    }

    std::optional<fs::path> CookSession::ResolveCurrentArtifactPath(const fs::path& projectDir,
                                                                      const Guid& guid) const
    {
        const fs::path contentDir = projectDir / "Content";
        const fs::path intermediateDir = projectDir / "Intermediate";
        const ArtifactStore store(intermediateDir);

        // Deliberately NOT store.RebuildIndexFromScan()/Lookup() -- see the header
        // comment: that index resolves a Guid shared by an orphaned artifact (an old
        // cook key still on disk after a `.meta` settings edit) and the current one
        // non-deterministically. Scanning sources and recomputing today's key directly
        // sidesteps the ambiguity entirely, same discipline CookProject/CheckProject
        // already use for staleness.
        for (const CookKindEntry& kindEntry : CookKinds())
        {
            for (const fs::path& source : EnumerateSources(contentDir, kindEntry.extensions))
            {
                fs::path metaPath = source;
                metaPath += ".meta";

                const std::optional<Guid> sourceGuid = ReadSourceGuid(metaPath);
                if (!sourceGuid || *sourceGuid != guid) continue;   // wrong/unreadable source -- keep scanning

                const std::optional<std::vector<std::byte>> bytes = ReadWholeFileBytes(source);
                if (!bytes) return std::nullopt;   // the one matching source is unreadable

                std::optional<std::uint64_t> cookKeyOpt;
                switch (kindEntry.kind)
                {
                case CookKind::Texture:
                {
                    const TextureMetaSettings settings =
                        TextureMetaSettings::FromMetaJson(ReadMetaBlock(metaPath, kindEntry.metaBlockKey), m_textureDefaults);
                    cookKeyOpt = ComputeCookKey(*bytes, settings, kTextureImporterVersion);
                    break;
                }
                case CookKind::Mesh:
                {
                    const MeshMetaSettings settings =
                        MeshMetaSettings::FromMetaJson(ReadMetaBlock(metaPath, kindEntry.metaBlockKey));
                    const std::optional<std::vector<std::vector<std::byte>>> externalBuffers =
                        ReadExternalBuffers(*bytes, source);
                    if (!externalBuffers) return std::nullopt;   // the matching source's buffer is unreadable
                    const std::vector<std::span<const std::byte>> spans = ToSpans(*externalBuffers);
                    cookKeyOpt = ComputeMeshCookKey(*bytes, spans, settings, kMeshImporterVersion);
                    break;
                }
                }

                const fs::path artifactPath = store.PathFor(*cookKeyOpt);
                std::error_code ec;
                if (!fs::exists(artifactPath, ec))
                    return std::nullopt;   // uncooked/stale -- NEVER fall back to another key

                return artifactPath;
            }
        }

        return std::nullopt;   // no source under Content/ carries this guid
    }
}
