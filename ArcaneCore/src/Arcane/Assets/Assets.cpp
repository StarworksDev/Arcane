#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Engine.hpp>   // ExecutablePathUtf8: the exe directory off-Windows

#include <Arcane/Assets/ArtifactReader.hpp>
#include <Arcane/Assets/AssetCache.hpp>
#include <Arcane/Assets/AssetsSettings.hpp>   // assets.material.maxParentDepth
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Serialization/IdentityFieldRule.hpp>   // Arcane::IsIdentityGuidFieldName

#include <Json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // Resolve a path exe-relative when relative -- the engine-wide anchor
        // (Render/ShaderPaths.hpp carries the shader-side copy). Relative
        // paths anchor to the executable directory so tests pass regardless of CWD.
        std::filesystem::path ExeRelative(const std::filesystem::path& path)
        {
            if (path.is_absolute())
                return path;
#ifdef _WIN32
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                return std::filesystem::path(modulePath).parent_path() / path;
#else
            if (const std::string self = ExecutablePathUtf8(); !self.empty())
                return std::filesystem::path(self).parent_path() / path;
#endif
            return path;
        }

        // Read all bytes from a file. Returns empty on any failure (kept
        // local -- not exported).
        // A legitimately empty file is treated as missing and callers memoize
        // it as a failure -- empty assets are unsupported. (This mirrored
        // ShaderLibrary::ReadFileBytes, which was deleted at ABI v15.)
        std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const std::streamsize size = file.tellg();
            if (size <= 0)
                return {};
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes((size_t)size);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
                return {};
            return bytes;
        }

        // Cache key: normalised absolute path string so that relative and
        // absolute references to the same file share one entry.
        std::string CacheKey(const std::filesystem::path& resolved)
        {
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(resolved, ec);
            if (ec)
                return resolved.string();
            return canon.string();
        }

        // Asset-manager arc (ABI v22, Task 2): the resolved path's extension,
        // lowercased -- the format-classification key ListAssetReferences
        // switches on. Kept local: Assets.cpp has no existing extension-
        // classification helper to reuse (Panels/AssetPanelModel.hpp's
        // AssetKindOf is an EDITOR-side classifier in a different translation
        // unit; this facade must not reach for it).
        std::string LowerExt(const std::filesystem::path& path)
        {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        // Asset-manager arc (ABI v22, Plan 2 Task 2): shape gate for a v4
        // scene manifest entry, applied BEFORE it ever reaches
        // Guid::FromString below. Canonical form only: 36 chars, hex in
        // every position except the four dashes at 8/13/18/23 (braces are
        // NOT accepted here, unlike FromString's own tolerance -- a save-time
        // manifest entry is never brace-wrapped, so this stays the narrower,
        // cheaper check). Load-bearing, not merely defensive: Plan-1 Task 4
        // hit a blocked Debug-CRT assertion box (no piped-stdout evidence --
        // indistinguishable from a true hang in an automated run) from an
        // UNCHECKED `*Guid::FromString(garbage)` dereference elsewhere in
        // this arc. FromString itself returns nullopt cleanly on garbage
        // (Guid.cpp) and every call site below still checks the optional --
        // but gating on shape first means a malformed manifest string is
        // rejected on sight rather than round-tripped through the parser at
        // all, so that failure class can never be reached from this loop.
        bool IsCanonicalGuidString(const std::string& s)
        {
            if (s.size() != 36)
                return false;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (i == 8 || i == 13 || i == 18 || i == 23)
                {
                    if (s[i] != '-')
                        return false;
                }
                else if (!std::isxdigit(static_cast<unsigned char>(s[i])))
                    return false;
            }
            return true;
        }

        // True when a sprite's JSON carries a non-default sub-rect --
        // SaveSpriteAsset (SpriteAsset.cpp) writes "sourceSize" only when it
        // differs from the (0,0) "whole texture" default, so its presence
        // (with a non-zero pair) IS the sliced/whole-texture discriminator.
        // A whole-texture sprite's texture reference is DerivesFrom (the
        // sprite IS that texture, in effect); a sliced sprite merely
        // References a source shared among however many other slices --
        // see AssetRefKind's own doc comment in Assets.hpp.
        bool IsSlicedSpriteJson(const nlohmann::json& sprite)
        {
            const auto it = sprite.find("sourceSize");
            if (it == sprite.end() || !it->is_array() || it->size() != 2 ||
                !(*it)[0].is_number() || !(*it)[1].is_number())
                return false;
            return (*it)[0].get<float>() != 0.0f || (*it)[1].get<float>() != 0.0f;
        }

        // Task 3 (asset-manager arc): hi/lo -> Guid reconstruction for the
        // scene structural scan below. This PROVABLY mirrors the reflection-
        // >JSON bridge's own wire encoding rather than guessing a byte order:
        // Components.hpp registers `ASTRA_REFLECT_TYPE(Guid)` with exactly
        // two reflected scalar fields, `hi` and `lo` -- Guid.hpp's own public
        // members, nothing else -- and ReflectionJson.hpp's WriteScalar/
        // ReadScalar for a `uint64_t` field copy the raw scalar value
        // verbatim (see its uint64_t branch): no byte-swap, no repacking.
        // So a scene's {"hi":H,"lo":L} literally IS Guid{H,L}; there is
        // nothing left to get wrong here.
        Guid GuidFromHiLo(std::uint64_t hi, std::uint64_t lo)
        {
            return Guid{ hi, lo };
        }

        // Task 3 (asset-manager arc): recursive structural walk of a parsed
        // .arcscene document, collecting every guid-shaped field except an
        // identity field. BOTH questions are answered by the shared engine
        // rules in Serialization/IdentityFieldRule.hpp -- IsGuidShapedJson for
        // the {"hi":u64,"lo":u64} value shape, IsIdentityGuidFieldName for the
        // field name -- and the v4 save-time manifest collector
        // (ReflectionJson.hpp) calls the SAME FUNCTIONS, which is what makes
        // this fallback scan and a v4 manifest agree on which guids count.
        // That two-field shape is exactly what Components.hpp's
        // ASTRA_REFLECT_TYPE(Guid) writes for every Guid-typed component field
        // (SpriteRenderer::material/sprite, MeshRenderer::mesh/
        // materialOverride, PostProcess::material, Identity::id, ...) --
        // verified against the real ReferenceProject/Content/scenes/
        // main.arcscene fixture. A nil guid ({"hi":0,"lo":0}) and a guid the
        // installed resolver cannot place are both dropped here via
        // `resolvable` -- see ScanSceneReferences below for what that
        // predicate means.
        //
        // No visited-set / depth bound: a JSON document is a tree by
        // construction (unlike MaterialSurfaceFor's parent-chain walk, which
        // follows Guid links that COULD cycle), so this walk always
        // terminates on its own.
        static void ScanSceneJson(const nlohmann::json& node,
                                  const std::function<bool(const Guid&)>& resolvable,
                                  std::vector<AssetRef>& out)
        {
            if (node.is_object())
            {
                for (const auto& [key, value] : node.items())
                {
                    if (IsGuidShapedJson(value))
                    {
                        if (IsIdentityGuidFieldName(key))
                            continue;   // identity, not a reference
                        const Guid g = GuidFromHiLo(value["hi"].get<std::uint64_t>(),
                                                    value["lo"].get<std::uint64_t>());
                        if (g.IsValid() && resolvable(g))
                            out.push_back({ g, AssetRefKind::References });
                        continue;   // a guid-shaped leaf has nothing further to walk into
                    }
                    ScanSceneJson(value, resolvable, out);
                }
            }
            else if (node.is_array())
                for (const auto& v : node) ScanSceneJson(v, resolvable, out);
        }

        // Task 3 (asset-manager arc): the scene structural reference scan --
        // fulfils ListAssetReferences's .arcscene branch, a stub until now.
        // `resolvable` = "the installed AssetResolver answers for this guid"
        // (ResolveId succeeds); the CALLER (ListAssetReferences below) is
        // the one that actually has a resolver to ask, so it builds and
        // hands in the predicate rather than this free function reaching
        // for one of its own. Every surviving edge is
        // AssetRefKind::References: a scene structurally NAMES another
        // asset, it never DERIVES its own identity from one (AssetRefKind's
        // own doc comment in Assets.hpp). Deduplicated by target guid -- a
        // scene routinely names the SAME material/mesh from several
        // component fields or several entities, and the caller wants one
        // edge per distinct target, not one per mention.
        std::optional<std::vector<AssetRef>> ScanSceneReferences(
            const nlohmann::json& scene, const std::function<bool(const Guid&)>& resolvable)
        {
            std::vector<AssetRef> raw;
            ScanSceneJson(scene, resolvable, raw);

            std::unordered_set<Guid> seen;
            std::vector<AssetRef> out;
            out.reserve(raw.size());
            for (const AssetRef& r : raw)
                if (seen.insert(r.target).second)
                    out.push_back(r);
            return out;
        }

        using BytesPtr       = std::shared_ptr<const std::vector<uint8_t>>;
        using JsonPtr        = std::shared_ptr<const nlohmann::json>;
        using PixelDataPtr   = std::shared_ptr<PixelData>;
        using TextureInfoPtr = std::shared_ptr<TextureInfo>;
        using ArtifactPtr    = std::shared_ptr<LoadedClientArtifact>;
        using MeshPtr        = std::shared_ptr<LoadedClientMesh>;   // Task 11

        // F2c Task 11: the C1(b) candidate walk (ResolveArtifact's own comment names the
        // fix this codifies), FACTORED so it exists exactly once regardless of content
        // kind -- rather than copying ResolveArtifact's 30-line candidate-walk loop+comment
        // a second time for the mesh half. `Result` is ArtifactReadResult or
        // MeshArtifactReadResult; `read` is ReadClientArtifact or ReadClientMeshArtifact's
        // exact signature: (path, currentSourceBytes, expectedGuid) -> a Result carrying its
        // own `.refusal`. Iterates every guid-matching candidate (ordinarily one; more than
        // one means a stale, superseded artifact is still on disk alongside the fresh one --
        // FindArtifactForGuid's own C1(b) paragraph) and accepts the FIRST that reads back
        // clean, remembering the MOST INFORMATIVE refusal among the rest (HashMismatch/
        // VersionNewerThanEngine over a bare Missing) if none do.
        template <typename Result, typename ReadFn>
        Result ResolveArtifactCandidates(const std::vector<std::filesystem::path>& candidates,
                                         std::span<const std::byte> currentSourceBytes,
                                         const Guid& id, ReadFn&& read)
        {
            Result best;
            best.refusal = ArtifactRefusal::Missing;
            for (const std::filesystem::path& candidate : candidates)
            {
                Result result = read(candidate, currentSourceBytes, id);
                if (result.refusal == ArtifactRefusal::None)
                    return result;   // THE fix: first candidate that validates wins

                if (best.refusal == ArtifactRefusal::Missing && result.refusal != ArtifactRefusal::Missing)
                    best.refusal = result.refusal;   // remember the most informative refusal seen
            }
            return best;
        }

        // The process-wide content-artifact-refusal latch's storage (see Assets.hpp's own
        // "process-wide content-artifact-refusal latch" banner for the contract). One slot,
        // first-refusal-wins -- same idiom as GpuInstrumentation.cpp's device-lost latch:
        // g_contentRefusalObserved is the atomic flag callers poll cheaply and often,
        // g_contentRefusalDetailMutex guards the (rarely read, written at most once) detail
        // string separately so a poller never pays a lock for the common "nothing to see"
        // case.
        std::atomic<bool> g_contentRefusalObserved{ false };
        std::mutex        g_contentRefusalDetailMutex;
        std::string       g_contentRefusalDetail;

        // F2b Task 12: the per-refusal observer (Assets.hpp's own doc
        // comment) -- fires on EVERY refusal, not just the process's first.
        // Guarded by its own mutex (never g_contentRefusalDetailMutex): the
        // observer callback itself may take arbitrarily long (a Diagnostics::
        // Publish call, say), and it must never be held while some OTHER
        // caller is blocked reading ContentArtifactRefusalDetail().
        std::mutex                 g_refusalObserverMutex;
        ArtifactRefusalObserver    g_refusalObserver = nullptr;
        void*                      g_refusalObserverUser = nullptr;

        // Latches the FIRST refusal only (compare_exchange_strong guards the detail write
        // too -- a second, different refusal on a later guid must not overwrite the first
        // one a host is about to report). `kind` is "ArtifactMissing", "HashMismatch" or
        // "VersionNewerThanEngine" -- as of Task 8 (the sprite cutover) a source with no
        // cooked artifact refuses by name like the other two (see RefuseArtifact below).
        void NoteContentArtifactRefusal(const Guid& id, const char* kind)
        {
            bool expected = false;
            if (g_contentRefusalObserved.compare_exchange_strong(expected, true,
                                                                  std::memory_order_acq_rel))
            {
                std::lock_guard<std::mutex> lock(g_contentRefusalDetailMutex);
                g_contentRefusalDetail = std::string(kind) + ": " + id.ToString();
            }

            // Unconditional -- unlike the latch above, EVERY call reaches the
            // observer (Task 12's Problems pane wants every refused guid it
            // has ever seen this session, not just the first).
            ArtifactRefusalObserver observer;
            void* user;
            {
                std::lock_guard<std::mutex> lock(g_refusalObserverMutex);
                observer = g_refusalObserver;
                user = g_refusalObserverUser;
            }
            if (observer)
                observer(id, kind, user);
        }

        class AssetsImpl final : public Assets
        {
        public:
            explicit AssetsImpl(const AssetsDesc& desc)
                : m_byteBudget(desc.byteBudget)
            {
            }

            void SetContentRoot(const std::filesystem::path& root) override
            {
                m_contentRoot = root;
            }

            void SetAssetResolver(AssetResolver resolver) override
            {
                m_resolver = std::move(resolver);
                // A new resolver (project open / registry rescan) may know ids that
                // previously failed -- drop the warn-once memos for a clean retry.
                m_idFailures.clear();
                // KEY OWNERSHIP: "assets.unresolved" -- this producer's OWN key,
                // distinct from AssetRegistry::ScanContent's "assets" key
                // (AssetRegistry.cpp) -- ResolveId's warn-once memo is not a
                // whole-tree walk, so it must never touch "assets". A retry may
                // resolve ids that previously failed, so retract every row rather
                // than leave stale "unresolved" diagnostics for ids that are about
                // to be re-checked.
                m_unresolvedDiagnosticIds.clear();
                Diagnostics::Clear("assets.unresolved");
            }

            // THE ANCHORING CONVENTION, for all three id-shaped overloads below
            // -- PixelsFor(Guid), GetBytes(AssetId), GetJson(AssetId). There
            // were FOUR until ABI v15 deleted GetTexture(AssetId).
            // A path that came OUT of the installed AssetResolver is already
            // LOAD-READY and must go straight to the resolved-path workers --
            // never back through ResolveAssetPath. The registry turned the Guid
            // into a mount path and the MountTable joined it onto THAT MOUNT's
            // own root (game:// = <project>/Content, diag:// =
            // <project>/Saved/Diagnostics, plugin/<x>:// = <plugin>/Content), so
            // there is nothing left for this facade to anchor. Re-anchoring it
            // under m_contentRoot is not merely redundant -- for a relative
            // project root it produces <root>/Content/<root>/Content/... (the
            // desk-observed "failed to load ReferenceProject\Content\
            // ReferenceProject\Content\textures\uv_marker.png"), and for the
            // diag://+plugin:// mounts it is wrong even when the root is
            // absolute, because those mounts do not live under Content/ at all.
            // Every OTHER consumer of the same resolver -- SpriteCache,
            // SpriteMaterialCache, PostChainCache (via SceneRenderResolver),
            // ProjectBoot's boot-scene lookup, the editor, the NRI graph
            // vehicle's own SetAssetResolver -- opens the returned path verbatim
            // with an ifstream, so verbatim IS the convention and this facade was
            // the sole violator of it. See ResolveAssetPath for the other half of
            // the rule (who DOES anchor).
            const PixelData* PixelsFor(const Guid& id) override
            {
                const auto p = ResolveId(AssetId::FromGuid(id));
                return p ? PixelsForResolved(id, *p, CacheKey(*p)) : nullptr;
            }

            // ABI v21: header dims/mipCount/srgb, artifact-backed -- see Assets.hpp's own
            // doc comment for the full contract (this is the dimension source; PixelsFor
            // above now serves a THUMBNAIL for artifact-backed content).
            const TextureInfo* TextureInfoFor(const Guid& id) override
            {
                const auto p = ResolveId(AssetId::FromGuid(id));
                return p ? TextureInfoForResolved(id, *p, CacheKey(*p)) : nullptr;
            }

            // Task 7: the compiled-texture supply -- see Assets.hpp's own doc comment
            // (appended at the end of the interface; same-arc ABI v21 addition).
            const LoadedClientArtifact* ArtifactFor(const Guid& id) override
            {
                const auto p = ResolveId(AssetId::FromGuid(id));
                return p ? ArtifactForResolved(id, *p, CacheKey(*p)) : nullptr;
            }

            // F2c Task 11: the mesh half of ArtifactFor -- see Assets.hpp's own doc
            // comment (appended at the end of the interface).
            const LoadedClientMesh* MeshArtifactFor(const Guid& id) override
            {
                const auto p = ResolveId(AssetId::FromGuid(id));
                return p ? MeshArtifactForResolved(id, *p, CacheKey(*p)) : nullptr;
            }

            BytesPtr GetBytes(const AssetId& id) override
            {
                const auto p = ResolveId(id);
                return p ? BytesForResolved(*p) : nullptr;
            }

            JsonPtr GetJson(const AssetId& id) override
            {
                const auto p = ResolveId(id);
                return p ? JsonForResolved(*p) : nullptr;
            }

            // The PATH overloads: a caller-supplied LOOSE relative path is the
            // OTHER half of the convention -- the half the content root does
            // anchor. Both routes converge on the same *Resolved workers below,
            // so a file reached by Guid and the same file reached by path still
            // share ONE cache entry (CacheKey canonicalises both).
            BytesPtr GetBytes(const std::filesystem::path& path) override
            {
                return BytesForResolved(ResolveAssetPath(path));
            }

            JsonPtr GetJson(const std::filesystem::path& path) override
            {
                return JsonForResolved(ResolveAssetPath(path));
            }

            AssetStats Stats() const override
            {
                AssetStats s;
                s.totalBytes = TotalBytes();
                s.count = (uint32_t)(m_bytes.Count() +
                                     m_json.Count() +
                                     m_pixels.Count() +
                                     m_textureInfo.Count() +
                                     m_artifacts.Count() +
                                     m_meshes.Count());   // Task 11
                return s;
            }

        private:
            // The loaders, keyed on an ALREADY-RESOLVED path -- whichever of the
            // two routes above produced it. They never anchor anything
            // themselves: the anchoring decision is made exactly once, by the
            // caller, which is what makes the id route's "already load-ready"
            // contract structural instead of incidental.

            BytesPtr BytesForResolved(const std::filesystem::path& resolved)
            {
                const std::string key = CacheKey(resolved);

                if (m_bytes.Has(key))
                {
                    if (m_bytes.IsFailure(key))
                        return nullptr;
                    return m_bytes.Get(key);
                }

                auto raw = ReadFileBytes(resolved);
                if (raw.empty())
                {
                    ARC_WARN("Assets: file not found or empty: {}",
                             resolved.string());
                    m_bytes.PutFailure(key);
                    return nullptr;
                }

                const uint64_t sz = (uint64_t)raw.size();
                auto ptr = std::make_shared<const std::vector<uint8_t>>(
                    std::move(raw));
                m_bytes.Put(key, ptr, sz);
                EnforceBudget();
                return ptr;
            }

            JsonPtr JsonForResolved(const std::filesystem::path& resolved)
            {
                const std::string key = CacheKey(resolved);

                if (m_json.Has(key))
                {
                    if (m_json.IsFailure(key))
                        return nullptr;
                    return m_json.Get(key);
                }

                auto raw = ReadFileBytes(resolved);
                if (raw.empty())
                {
                    ARC_WARN("Assets: JSON file not found or empty: {}",
                             resolved.string());
                    m_json.PutFailure(key);
                    return nullptr;
                }

                auto doc = nlohmann::json::parse(
                    raw.begin(), raw.end(),
                    /*cb=*/nullptr,
                    /*allow_exceptions=*/false);
                if (doc.is_discarded())
                {
                    ARC_WARN("Assets: JSON parse failed: {}",
                             resolved.string());
                    m_json.PutFailure(key);
                    return nullptr;
                }

                const uint64_t sz = (uint64_t)raw.size();
                auto ptr = std::make_shared<const nlohmann::json>(
                    std::move(doc));
                m_json.Put(key, ptr, sz);
                EnforceBudget();
                return ptr;
            }

            // Asset-manager arc, final fix wave: the UNCACHED JSON read, for the
            // two panel queries ONLY (MaterialSurfaceFor / ListAssetReferences
            // below). Spec s3 pins them as "parse-on-call, no engine-side cache
            // (the editor's index and model are the caches)", and
            // JsonForResolved above structurally cannot serve that: it keys on
            // the canonical path, never consults the file's mtime, and memoizes
            // FAILURES for the process lifetime (nothing evicts m_json --
            // InvalidateArtifact touches pixels/textureInfo/artifacts only). A
            // query routed through it answers every later re-ask with the FIRST
            // parse -- subkind pills, isInstance, fold/sliced state and the
            // material-picker filter frozen for the session -- and a file caught
            // MID-SAVE latches broken forever, which makes s3.2's "keeps the
            // last-known refs and retries on the next change event" impossible to
            // implement above this facade.
            //
            // Deliberately NOT an mtime check bolted onto m_json: every other
            // consumer of that cache (both public GetJson overloads and their
            // callers) is written against today's parse-once semantics, and
            // widening the blast radius to the whole facade to fix two queries is
            // the wrong trade. This reads, parses, and inserts nothing on either
            // the success or the failure side -- so it never evicts a live entry
            // through EnforceBudget either.
            std::optional<nlohmann::json> ParseJsonUncached(const std::filesystem::path& resolved)
            {
                auto raw = ReadFileBytes(resolved);
                if (raw.empty())
                {
                    ARC_WARN("Assets: JSON file not found or empty: {}",
                             resolved.string());
                    return std::nullopt;
                }

                auto doc = nlohmann::json::parse(
                    raw.begin(), raw.end(),
                    /*cb=*/nullptr,
                    /*allow_exceptions=*/false);
                if (doc.is_discarded())
                {
                    ARC_WARN("Assets: JSON parse failed: {}",
                             resolved.string());
                    return std::nullopt;
                }

                return doc;
            }

            // Decode-once pixel supply behind PixelsFor(Guid). `resolved`/
            // `key` are the already-resolved path + CacheKey every route
            // through this facade canonicalises to, so a Guid and the loose
            // path naming the same file share ONE decode and ONE cache entry.
            //
            // It was shared with GetTexture (both overloads), whose upload
            // keyed off the same pair; GetTexture and the texture cache were
            // deleted at ABI v15 (see the tombstone above), leaving this the
            // single consumer.
            // Artifact-preferred resolve, shared by PixelsForResolved and
            // TextureInfoForResolved below: does THIS guid's source have a cooked
            // .arcart, and if so, is it still valid against the CURRENT staged source
            // bytes? Returns ArtifactRefusal::Missing (the default-constructed result)
            // whenever there is nothing to serve from an artifact -- no project is open
            // (m_contentRoot empty, so there is no Intermediate/ to derive), or the
            // store has no artifact for this guid at all. Since Task 8 (the sprite
            // cutover) Missing IS a refusal at this layer, exactly like HashMismatch/
            // VersionNewerThanEngine: callers refuse loudly on all three, never fall
            // back (see ArtifactReader.hpp's REFUSAL DISCIPLINE banner).
            //
            // INTERMEDIATE ROOT: derived as m_contentRoot's PARENT directory -- every
            // installer of this facade's content root sets it to exactly
            // `<project>/Content` (Runtime::OpenProject, and every test/host that
            // mirrors that wiring), and ArtifactStore.hpp's own contract roots the
            // store at `<project>/Intermediate`, i.e. the SAME parent, one directory
            // over. This needs no new setter on the facade: a content root that is
            // NOT literally `<project>/Content` (a synthetic single-directory test
            // fixture, for instance) simply derives a directory that does not exist,
            // which FindArtifactForGuid already treats as "no artifacts here" --
            // degrading safely to the legacy fallback rather than misbehaving.
            // C1(b) FIX (final-review wave, 2026-09-04): a same-guid recook under a NEW
            // cook key can leave the SUPERSEDED old-key artifact still on disk for one
            // pass (CookSession's own self-heal, C1a, removes it best-effort, but a
            // locked file or an external tool can still leave one behind), and BOTH the
            // stale and the fresh artifact carry the SAME sourceGuid header --
            // FindArtifactForGuid now returns EVERY guid-matching candidate rather than
            // the first the scan happens to visit; the walk itself (accept the FIRST that
            // reads back clean, else the most informative refusal) is now FACTORED into
            // the ResolveArtifactCandidates template above this class (Task 11 -- the
            // mesh half needed the exact same walk against a different ReadClient*Artifact,
            // and this is the shared point rather than a second 30-line copy). The
            // header-only probe inside FindArtifactForGuid stays cheap regardless of how
            // many candidates exist; only guid-MATCHED candidates ever pay for a full
            // ReadClientArtifact parse.
            ArtifactReadResult ResolveArtifact(const Guid& id, const std::filesystem::path& resolved)
            {
                if (m_contentRoot.empty())
                    return ArtifactReadResult{};

                const std::filesystem::path intermediateRoot =
                    m_contentRoot.parent_path() / "Intermediate";
                const std::vector<std::filesystem::path> candidates =
                    FindArtifactForGuid(intermediateRoot, id);
                if (candidates.empty())
                    return ArtifactReadResult{};

                // Raw (undecoded) bytes of the CURRENT staged source -- the same file
                // the legacy fallback would decode -- so ReadClientArtifact can hash-
                // compare against each candidate's own sourceHash. An unreadable source
                // reads as empty here, which will not match any real artifact's hash,
                // so it correctly surfaces as HashMismatch rather than silently passing.
                const std::vector<uint8_t> raw = ReadFileBytes(resolved);
                const std::span<const std::byte> rawBytes(
                    reinterpret_cast<const std::byte*>(raw.data()), raw.size());

                return ResolveArtifactCandidates<ArtifactReadResult>(
                    candidates, rawBytes, id, ReadClientArtifact);
            }

            // F2c Task 11: the mesh half of ResolveArtifact -- SAME candidate walk (the
            // shared ResolveArtifactCandidates template above), against a MESH-shaped
            // currentSourceBytes instead of a texture's raw file bytes.
            //
            // `currentSourceBytes` for a mesh: the source file's own bytes, followed by
            // every EXTERNAL BUFFER's bytes, in glTF declaration order -- the exact
            // concatenation Task 5's ComputeMeshCookKey hashes and the pipeline's
            // MeshImporter.cpp (Step 7) writes into the artifact's sourceHash (see
            // ArtifactReader.hpp's own MESH TAIL banner). EVERY EXTERNAL BUFFER, WHICHEVER
            // CONTAINER (final-review fix I1): ReadClientExternalBuffers runs for BOTH
            // mesh extensions -- a .glb's buffers[1..] may carry a `uri` exactly like a
            // .gltf's (only buffer 0 is the BIN chunk), and the pipeline side
            // (CookSession -> ReadExternalBuffers -> cgltf) hashes such a file as
            // .glb ++ .bin. This used to be gated on `LowerExt(resolved) == ".gltf"` under
            // the false premise that a .glb never references an external buffer; that
            // gate made a legal .glb refuse HashMismatch permanently. The common case (a
            // plain .glb, everything in its BIN chunk) still reads exactly one file: the
            // reader finds an empty/absent uri list and appends nothing.
            MeshArtifactReadResult ResolveMeshArtifact(const Guid& id, const std::filesystem::path& resolved)
            {
                if (m_contentRoot.empty())
                    return MeshArtifactReadResult{};

                const std::filesystem::path intermediateRoot =
                    m_contentRoot.parent_path() / "Intermediate";
                const std::vector<std::filesystem::path> candidates =
                    FindArtifactForGuid(intermediateRoot, id);
                if (candidates.empty())
                    return MeshArtifactReadResult{};

                const std::vector<uint8_t> raw = ReadFileBytes(resolved);
                std::vector<std::byte> sourceBytes(
                    reinterpret_cast<const std::byte*>(raw.data()),
                    reinterpret_cast<const std::byte*>(raw.data()) + raw.size());

                {
                    const std::span<const std::byte> fileBytes(sourceBytes.data(), sourceBytes.size());
                    if (const auto buffers = ReadClientExternalBuffers(fileBytes, resolved))
                    {
                        for (const std::vector<std::byte>& buffer : *buffers)
                            sourceBytes.insert(sourceBytes.end(), buffer.begin(), buffer.end());
                    }
                    // An unreadable/unparseable source (nullopt) leaves `sourceBytes` at the
                    // file's own bytes alone -- the resulting hash simply will not match any
                    // real artifact's sourceHash, surfacing as HashMismatch (or Missing, if
                    // the guid has no artifact at all) rather than a silent pass, mirroring
                    // ResolveArtifact's own "an unreadable source reads as empty" posture.
                }

                const std::span<const std::byte> currentSourceBytes(sourceBytes.data(), sourceBytes.size());
                return ResolveArtifactCandidates<MeshArtifactReadResult>(
                    candidates, currentSourceBytes, id, ReadClientMeshArtifact);
            }

            // Refuse LOUDLY (ERROR, not WARN) and memoize -- "refuse, never limp"
            // (Assets.hpp's PixelsFor/TextureInfoFor doc comments). Shared by every
            // accessor's artifact-refusal branch so the log line, the memo and the
            // process-wide latch (Assets.hpp's ContentArtifactRefusalObserved) can
            // never drift between the call sites. As of Task 8 (the sprite cutover)
            // Missing IS a refusal here too: content is artifact-only, and a source
            // with no cooked artifact refuses by name rather than limping to stb.
            // Task 12's cook-completion invalidation is what clears a Missing memo
            // once a cook lands -- the editor's drop-a-png flow depends on that.
            // Desk-fix 2: does a MISSING resolution for `id` deserve the quiet
            // treatment instead of RefuseArtifact's loud one? Shared by all three
            // accessors' refusal branches so the check never drifts between them --
            // same "one function, every call site" idiom RefuseArtifact itself
            // follows. Deliberately gated on ArtifactRefusal::Missing ONLY:
            // HashMismatch/VersionNewerThanEngine never reach this branch true,
            // because a present-but-invalid artifact is broken regardless of
            // whether a cook is queued (see SetCookPendingProbe's own doc comment,
            // "the never-quieted pin").
            bool QuietlyPending(const Guid& id, ArtifactRefusal refusal) const
            {
                return refusal == ArtifactRefusal::Missing &&
                       m_cookPendingProbe && m_cookPendingProbe(id);
            }

            template <typename T>
            void RefuseArtifact(AssetCache<T>& cache, const std::string& key,
                                const Guid& id, ArtifactRefusal refusal)
            {
                const char* kind = (refusal == ArtifactRefusal::HashMismatch)
                    ? "HashMismatch"
                    : (refusal == ArtifactRefusal::VersionNewerThanEngine)
                        ? "VersionNewerThanEngine" : "ArtifactMissing";
                ARC_ERROR("Assets: content artifact REFUSED for {} -- {} (refuse, never "
                          "limp; see ArtifactReader.hpp's refusal discipline)",
                          id.ToString(), kind);
                cache.PutFailure(key);
                NoteContentArtifactRefusal(id, kind);
            }

            // Decode-once pixel supply behind PixelsFor(Guid). `resolved`/`key` are the
            // already-resolved path + CacheKey every route through this facade
            // canonicalises to.
            //
            // ABI v21 NARROWS this to a PREVIEW-pixel contract for artifact-backed
            // content (Assets.hpp's own doc comment): a guid whose source has a valid
            // cooked artifact is served that artifact's own THUMBNAIL (small,
            // uncompressed RGBA8) rather than a full decode of the source -- cheap, and
            // exactly what a sprite-picker/inspector preview needs. As of Task 8 (the
            // sprite cutover) content is ARTIFACT-ONLY: a guid with no cooked artifact
            // is the ArtifactMissing refusal, loud and memoized, exactly like a guid
            // whose artifact is PRESENT but INVALID -- see RefuseArtifact. The stb
            // fallback this branch once had is retired; LoadPngRgba survives in this
            // TU for the verify/compare ORACLE only.
            const PixelData* PixelsForResolved(const Guid& id, const std::filesystem::path& resolved,
                                               const std::string& key)
            {
                if (m_pixels.Has(key))
                {
                    if (m_pixels.IsFailure(key))
                        return nullptr;
                    return m_pixels.Get(key).get();
                }

                const ArtifactReadResult art = ResolveArtifact(id, resolved);
                if (art.refusal != ArtifactRefusal::None || !art.artifact)
                {
                    // Desk-fix 2: a probe-quieted Missing returns null with NO log,
                    // NO memo and NO latch -- see QuietlyPending's own comment.
                    if (QuietlyPending(id, art.refusal))
                        return nullptr;
                    // Missing lands here too (Task 8): no cooked artifact == refusal
                    // by name, never a silent stb decode of the source.
                    RefuseArtifact(m_pixels, key, id, art.refusal);
                    return nullptr;
                }

                auto pixels = std::make_shared<PixelData>();
                // Artifact-backed: serve the THUMBNAIL. width/height here are the
                // THUMBNAIL's dims (<=64px), never the source's -- TextureInfoFor
                // is the true-dims source (the wrong-dims bug class this split
                // exists to prevent -- see SpriteCache.cpp's own retarget).
                pixels->width  = art.artifact->thumbWidth;
                pixels->height = art.artifact->thumbHeight;
                {
                    const auto* p = reinterpret_cast<const unsigned char*>(art.artifact->thumbRgba.data());
                    pixels->rgba.assign(p, p + art.artifact->thumbRgba.size());
                }

                const uint64_t bytes = (uint64_t)pixels->rgba.size();
                const PixelData* raw = pixels.get();
                m_pixels.Put(key, pixels, bytes);
                // Pin the fresh entry for the duration of the sweep below:
                // unlike GetTexture/GetBytes/GetJson (which return an owning
                // handle/shared_ptr that survives its own cache eviction),
                // PixelsFor returns a bare pointer -- if EnforceBudget evicted
                // THIS entry before we return (the "bigger than the whole
                // budget" edge case other caches tolerate), `raw` would dangle
                // the instant this function returns. Released immediately
                // after: on every LATER call this is an ordinary, evictable
                // LRU citizen like any other entry.
                m_pixels.Acquire(key);
                EnforceBudget();
                m_pixels.Release(key);
                return raw;
            }

            // Header dims/mipCount/srgb behind TextureInfoFor(Guid) -- ABI v21, new.
            // Artifact-backed content reads the artifact's own HEADER (no decode at
            // all). As of Task 8 content is ARTIFACT-ONLY: a guid with no cooked
            // artifact is the ArtifactMissing refusal, loud and memoized -- the old
            // stbi_info dimension probe is retired with the rest of the content stb
            // route. A PRESENT-but-invalid artifact refuses the same way, same as
            // PixelsForResolved.
            const TextureInfo* TextureInfoForResolved(const Guid& id, const std::filesystem::path& resolved,
                                                       const std::string& key)
            {
                if (m_textureInfo.Has(key))
                {
                    if (m_textureInfo.IsFailure(key))
                        return nullptr;
                    return m_textureInfo.Get(key).get();
                }

                const ArtifactReadResult art = ResolveArtifact(id, resolved);
                if (art.refusal != ArtifactRefusal::None || !art.artifact)
                {
                    // Desk-fix 2: a probe-quieted Missing returns null with NO log,
                    // NO memo and NO latch -- see QuietlyPending's own comment.
                    if (QuietlyPending(id, art.refusal))
                        return nullptr;
                    // Missing lands here too (Task 8): artifact-only, no stbi_info probe.
                    RefuseArtifact(m_textureInfo, key, id, art.refusal);
                    return nullptr;
                }

                auto info = std::make_shared<TextureInfo>();
                *info = art.artifact->info;

                const TextureInfo* raw = info.get();
                // Tiny, fixed-size metadata -- weighted at sizeof(TextureInfo) in the
                // shared budget rather than 0, so it participates honestly in the
                // facade-wide accounting this file's banner describes, but never comes
                // close to dominating it (this cache's total is at most a few hundred
                // bytes even with thousands of distinct textures resident).
                m_textureInfo.Put(key, info, sizeof(TextureInfo));
                m_textureInfo.Acquire(key);
                EnforceBudget();
                m_textureInfo.Release(key);
                return raw;
            }

            // The compiled-texture supply behind ArtifactFor(Guid) -- Task 7, ABI v21.
            // Shares ResolveArtifact/RefuseArtifact with the two accessors above, so the
            // refusal discipline can never drift between the three. As of Task 8 the
            // three accessors agree on Missing too: no cooked artifact == the
            // ArtifactMissing refusal, loud and MEMOIZED (the Task 7-era unmemoized
            // re-scan is retired with the content stb route). The editor's drop-a-png
            // flow still promotes PendingCook -> Resident: Task 12's cook-completion
            // callback INVALIDATES this facade's entry for the cooked guid, which
            // clears the Missing memo and lets the next re-poll resolve the fresh
            // artifact -- promotion rides invalidation now, not perpetual re-scan.
            const LoadedClientArtifact* ArtifactForResolved(const Guid& id,
                                                             const std::filesystem::path& resolved,
                                                             const std::string& key)
            {
                if (m_artifacts.Has(key))
                {
                    if (m_artifacts.IsFailure(key))
                        return nullptr;
                    return m_artifacts.Get(key).get();
                }

                const ArtifactReadResult art = ResolveArtifact(id, resolved);
                if (art.refusal != ArtifactRefusal::None || !art.artifact)
                {
                    // Desk-fix 2: a probe-quieted Missing returns null with NO log,
                    // NO memo and NO latch -- see QuietlyPending's own comment.
                    if (QuietlyPending(id, art.refusal))
                        return nullptr;
                    // Missing included (Task 8) -- memoized like the other two accessors;
                    // Task 12's cook-completion invalidation is the un-latch.
                    RefuseArtifact(m_artifacts, key, id, art.refusal);
                    return nullptr;
                }

                auto artifact = std::make_shared<LoadedClientArtifact>(std::move(*art.artifact));
                const uint64_t bytes =
                    (uint64_t)artifact->payload.size() + (uint64_t)artifact->thumbRgba.size();
                const LoadedClientArtifact* raw = artifact.get();
                m_artifacts.Put(key, artifact, bytes);
                // Pin for the sweep below -- same reason PixelsForResolved pins its own
                // fresh entry (EnforceBudget could otherwise evict THIS entry before the
                // caller ever sees `raw`, since a texture artifact larger than the whole
                // budget is swept right back out).
                m_artifacts.Acquire(key);
                EnforceBudget();
                m_artifacts.Release(key);
                return raw;
            }

            // F2c Task 11: the mesh half of ArtifactForResolved -- MIRRORS it exactly: its
            // own cache (m_meshes), the SAME ResolveMeshArtifact candidate walk (which
            // itself shares ResolveArtifactCandidates with the texture path), the SAME
            // QuietlyPending/RefuseArtifact refusal discipline.
            const LoadedClientMesh* MeshArtifactForResolved(const Guid& id,
                                                             const std::filesystem::path& resolved,
                                                             const std::string& key)
            {
                if (m_meshes.Has(key))
                {
                    if (m_meshes.IsFailure(key))
                        return nullptr;
                    return m_meshes.Get(key).get();
                }

                const MeshArtifactReadResult art = ResolveMeshArtifact(id, resolved);
                if (art.refusal != ArtifactRefusal::None || !art.mesh)
                {
                    // Desk-fix 2 (mirrored for mesh): a probe-quieted Missing returns null
                    // with NO log, NO memo and NO latch -- see QuietlyPending's own comment.
                    if (QuietlyPending(id, art.refusal))
                        return nullptr;
                    RefuseArtifact(m_meshes, key, id, art.refusal);
                    return nullptr;
                }

                auto mesh = std::make_shared<LoadedClientMesh>(std::move(*art.mesh));
                const uint64_t bytes =
                    (uint64_t)mesh->vertices.size() * sizeof(float) +
                    (uint64_t)mesh->indices.size() * sizeof(std::uint32_t);
                const LoadedClientMesh* raw = mesh.get();
                m_meshes.Put(key, mesh, bytes);
                // Pin for the sweep below -- same reason ArtifactForResolved pins its own
                // fresh entry.
                m_meshes.Acquire(key);
                EnforceBudget();
                m_meshes.Release(key);
                return raw;
            }

            // F2b Task 12: the un-latch behind Assets.hpp's InvalidateArtifact
            // doc comment. Resolves `id` through the SAME ResolveId path every
            // accessor above does, then evicts that one resolved key from all
            // three caches at once -- a success memo and a memoized refusal
            // (RefuseArtifact's PutFailure) are evicted identically, since
            // both can be stale after a fresh cook (a `.meta` settings edit
            // recooking an already-successful guid needs the OLD success
            // memo gone too, not just a Missing memo). A guid ResolveId
            // cannot place (never resolved, or the resolver itself refused
            // it) leaves nothing to evict -- a harmless no-op.
            void InvalidateArtifact(const Guid& id) override
            {
                const auto p = ResolveId(AssetId::FromGuid(id));
                if (!p)
                    return;
                const std::string key = CacheKey(*p);
                m_pixels.Evict(key);
                m_textureInfo.Evict(key);
                m_artifacts.Evict(key);
            }

            // Desk-fix 2: see Assets.hpp's own doc comment on SetCookPendingProbe for
            // the full contract. A plain assignment -- like m_resolver, "last writer
            // wins" -- and an empty std::function (the default: nothing has ever
            // installed one) restores today's loud-always behavior exactly.
            void SetCookPendingProbe(std::function<bool(const Guid&)> probe) override
            {
                m_cookPendingProbe = std::move(probe);
            }

            // F2c Task 11: the mesh half of InvalidateArtifact -- SAME shape, on the ONE
            // cache this facade keeps for mesh artifacts (m_meshes), never the texture
            // trio: a mesh cook completing has nothing to say about a texture guid's memo,
            // and vice versa.
            void InvalidateMeshArtifact(const Guid& id) override
            {
                const auto p = ResolveId(AssetId::FromGuid(id));
                if (!p)
                    return;
                const std::string key = CacheKey(*p);
                m_meshes.Evict(key);
            }

            // F2c Task 11: a two-line forward to the SAME probe SetCookPendingProbe
            // installs above -- generic rather than mesh-specific (Assets.hpp's own doc
            // comment on why), so this is the ONLY new state this virtual needs: none.
            bool CookPending(const Guid& id) const override
            {
                return m_cookPendingProbe && m_cookPendingProbe(id);
            }

            // T5 s7.2: see Assets.hpp. Evict refuses a pinned entry (refs > 0), which
            // only exists for the span of one accessor call on this thread.
            void EvictPath(const std::filesystem::path& resolved) override
            {
                const std::string key = CacheKey(resolved);
                m_bytes.Evict(key);
                m_json.Evict(key);
                m_pixels.Evict(key);
                m_textureInfo.Evict(key);
                m_artifacts.Evict(key);
                m_meshes.Evict(key);
            }

            // T5 s7.2. KEY OWNERSHIP: "assets.unresolved" (this producer's own key) --
            // republished whole-set, the same as every addition in ResolveId.
            void ForgetUnresolved(const Guid& id) override
            {
                m_idFailures.erase(id);
                std::erase(m_unresolvedDiagnosticIds, id);
                PublishUnresolvedDiagnostics();
            }

            // Asset-manager arc (ABI v22): see Assets.hpp's own doc comment for the
            // full contract. Takes the SAME resolution step (ResolveId, the SAME
            // installed resolver, the SAME warn-once memo on an unresolvable id) as
            // every other accessor on this facade, then parses through
            // ParseJsonUncached -- NOT the cached JsonForResolved -- because spec
            // s3 pins this query parse-on-call; see that helper's own comment for
            // why the cache cannot serve it and why the cache is left alone.
            // Bounded depth (assets.material.maxParentDepth, read per walk), not
            // a visited-set: a two-hop cycle just alternates for a few
            // iterations and then hits the bound, which is cheaper than
            // tracking a chain and gives the same "never hangs" guarantee.
            std::optional<MaterialSurface> MaterialSurfaceFor(const Guid& id) override
            {
                Guid current = id;
                const int maxDepth = Settings<AssetsMaterialSettings>().maxParentDepth;
                for (int depth = 0; depth < maxDepth; ++depth)
                {
                    const auto resolved = ResolveId(AssetId::FromGuid(current));
                    if (!resolved)
                        return std::nullopt;
                    const auto json = ParseJsonUncached(*resolved);
                    if (!json)
                        return std::nullopt;
                    // The "type" discriminator, hoisted AHEAD of the "kind" read
                    // (Task 1's deferred minor): the kind branch used to answer
                    // before confirming this file is a material at all, asymmetric
                    // with the neither-kind-nor-parent fallback's own gate below.
                    // It rejects a CONTRADICTING type rather than requiring a
                    // present one -- SaveMaterialAsset writes "type":"material"
                    // (MaterialAsset.cpp:147) but hand-authored .arcmat files
                    // predating it (ReferenceProject's own three) carry only
                    // "kind", and those must keep resolving.
                    if (auto it = json->find("type");
                        it != json->end() && it->is_string() &&
                        it->get<std::string>() != "material")
                        return std::nullopt;
                    if (auto it = json->find("kind"); it != json->end() && it->is_string())
                        return MaterialSurfaceForKind(it->get<std::string>());
                    if (auto it = json->find("parent"); it != json->end() && it->is_string())
                    {
                        const auto parent = Guid::FromString(it->get<std::string>());
                        if (!parent || !parent->IsValid() || *parent == current)
                            return std::nullopt;
                        current = *parent;
                        continue;
                    }
                    // A material file with neither kind nor parent: LoadMaterialAsset
                    // defaults such a file to "fullscreen" (MaterialAsset.cpp's own
                    // load default), but only if it IS a material -- gate on the
                    // "type" discriminator so an unrelated JSON asset (no kind, no
                    // parent, not a material) never silently reads as Fullscreen.
                    if (auto it = json->find("type"); it != json->end() && *it == "material")
                        return MaterialSurface::Fullscreen;
                    return std::nullopt;
                }
                return std::nullopt;   // chain too deep / cyclic
            }

            // Asset-manager arc (ABI v22, Task 2): see Assets.hpp's own doc
            // comment for the full contract. Reuses ResolveId (the SAME
            // resolution step MaterialSurfaceFor above takes, and the SAME
            // warn-once memo on an unresolvable id every other accessor on
            // this facade shares) -- to learn the FORMAT and, for the JSON
            // formats, to open the file. The parse goes through
            // ParseJsonUncached, NOT the cached JsonForResolved, because spec
            // s3 pins this query parse-on-call; see that helper's own comment.
            std::optional<std::vector<AssetRef>> ListAssetReferences(const Guid& id) override
            {
                const auto resolved = ResolveId(AssetId::FromGuid(id));
                if (!resolved)
                    return std::nullopt;
                const std::string ext = LowerExt(*resolved);

                // Leaf/opaque formats: readable, but structurally incapable
                // of naming another asset -- an empty list is the honest
                // answer, never nullopt (nullopt means "could not even read
                // this asset", a different fact than "read it; it has no
                // outgoing edges").
                static constexpr std::string_view kLeaf[] = {
                    ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr",
                    ".wav", ".ogg", ".mp3", ".flac", ".ttf", ".otf",
                    // F2c Task 12: an imported original names nothing of its own --
                    // its embedded textures become SEPARATE assets at extraction
                    // (s5.5) -- and a .glb is binary, not JSON, so routing it
                    // through the JSON-parse path below would refuse it outright
                    // rather than answer "no outgoing edges".
                    ".gltf", ".glb" };
                static constexpr std::string_view kOpaque[] = { ".json", ".arcdiag" };
                // Source/ in the Asset Browser: C/C++ source (registered under
                // source:// by AssetRegistry's IsSourceFile rule -- this list
                // mirrors it exactly). Code may well NAME a guid in a string
                // literal, but no index here can honestly see that, so "no
                // outgoing edges" is the same answer .json data gets -- and
                // never the parse path, which would log "JSON parse failed"
                // once per source file on every project open.
                static constexpr std::string_view kSource[] = {
                    ".cpp", ".hpp", ".h", ".c", ".inl", ".cc", ".cxx", ".hxx" };
                for (std::string_view e : kLeaf)   if (ext == e) return std::vector<AssetRef>{};
                for (std::string_view e : kOpaque) if (ext == e) return std::vector<AssetRef>{};
                for (std::string_view e : kSource) if (ext == e) return std::vector<AssetRef>{};

                // Parse-on-call, and off the ALREADY-resolved path above --
                // no second ResolveId round trip for the same guid.
                const auto json = ParseJsonUncached(*resolved);
                if (!json)
                    return std::nullopt;

                std::vector<AssetRef> out;
                auto addGuid = [&out](const nlohmann::json& v, AssetRefKind kind)
                {
                    if (!v.is_string())
                        return;
                    const auto g = Guid::FromString(v.get<std::string>());
                    if (g && g->IsValid())
                        out.push_back({ *g, kind });
                };

                if (ext == ".arcsprite")
                {
                    if (auto it = json->find("texture"); it != json->end())
                        addGuid(*it, IsSlicedSpriteJson(*json) ? AssetRefKind::References
                                                                : AssetRefKind::DerivesFrom);
                    return out;
                }
                if (ext == ".arcmat")
                {
                    if (auto it = json->find("parent"); it != json->end())
                        addGuid(*it, AssetRefKind::DerivesFrom);
                    if (auto params = json->find("params"); params != json->end() && params->is_object())
                        for (const auto& [name, p] : params->items())
                        {
                            if (!p.is_object())
                                continue;
                            const auto typeIt = p.find("type");
                            const auto valueIt = p.find("value");
                            if (typeIt != p.end() && typeIt->is_string() && *typeIt == "texture" &&
                                valueIt != p.end())
                                addGuid(*valueIt, AssetRefKind::References);
                        }
                    return out;
                }
                if (ext == ".arcmesh")
                {
                    // F2c Task 12 (spec s4.2/s4.4): an IMPORTED mesh's companion
                    // carries two kinds of outgoing edge now -- importedSource
                    // (the .gltf/.glb it was extracted from) as DerivesFrom, and
                    // every named slot's material as References. addGuid already
                    // skips a nil/invalid guid (line ~1067 above), so an absent or
                    // unassigned importedSource, and a nil slot, both contribute
                    // NOTHING -- no phantom entry, same discipline as every other
                    // branch here.
                    if (auto it = json->find("importedSource"); it != json->end())
                        addGuid(*it, AssetRefKind::DerivesFrom);
                    if (auto slots = json->find("slots"); slots != json->end() && slots->is_array())
                    {
                        for (const auto& slot : *slots)
                        {
                            if (!slot.is_object())
                                continue;
                            if (auto it = slot.find("material"); it != slot.end())
                                addGuid(*it, AssetRefKind::References);
                        }
                    }
                    else if (auto it = json->find("material"); it != json->end())
                    {
                        // Legacy F2a shape (Task 10's tolerant load path, reused
                        // here): no "slots" key at all -- the scalar "material"
                        // is the ENTIRE existing corpus's only material edge.
                        addGuid(*it, AssetRefKind::References);
                    }
                    return out;
                }
                if (ext == ".arcscene")
                {
                    // v4 fast path (asset-manager arc, Plan 2 Task 2; spec
                    // s3.3): a save-time "assets" manifest, when present, is
                    // exact -- no shape heuristics, and deliberately NO
                    // resolvability filter. A dangling target must surface
                    // here so the editor's index can tombstone it (spec
                    // s9.1; Task 3 of THIS plan depends on it). Task 3 of the
                    // PRIOR plan's structural scan below keeps its own
                    // filter -- for a shape heuristic walking arbitrary
                    // JSON, that filter is the false-positive killer (spec
                    // s3.4). The two therefore agree only when every
                    // referenced target happens to be resolvable; the
                    // manifest is a strict superset of the scan's answer
                    // otherwise, by design.
                    //
                    // `>= 4`, not `== 4`, and deliberately open-ended
                    // upward: this function only lists references for the
                    // asset panel/index, so a future v5+ file's manifest is
                    // still read here even on a build whose scene LOADER
                    // refuses that version -- the panel can describe an
                    // asset's references before the engine can load it.
                    const auto vit = json->find("version");
                    const auto ait = json->find("assets");
                    if (vit != json->end() && vit->is_number_integer() && vit->get<int>() >= 4 &&
                        ait != json->end() && ait->is_array())
                    {
                        std::unordered_set<Guid> seen;
                        for (const auto& entry : *ait)
                        {
                            if (!entry.is_string())
                                continue;
                            const std::string s = entry.get<std::string>();
                            if (!IsCanonicalGuidString(s))   // shape-gate BEFORE FromString
                                continue;
                            const auto g = Guid::FromString(s);
                            // Always well-formed after the shape gate above,
                            // so `g` is never nullopt here in practice -- the
                            // check stays as the same defense-in-depth every
                            // other FromString call site in this function
                            // applies (see addGuid above). IsValid() drops a
                            // literal nil-guid manifest entry; Task 1's
                            // SaveJson never writes one, but a hand-edited
                            // file could carry one.
                            if (g && g->IsValid() && seen.insert(*g).second)
                                out.push_back({ *g, AssetRefKind::References });
                        }
                        return out;
                    }

                    // Pre-v4, or a v4 file whose manifest was hand-stripped:
                    // Task 3's structural scan. "resolvable" IS "ResolveId
                    // succeeds" -- the same resolution step every other
                    // accessor on this facade takes, just probed here rather
                    // than opened.
                    auto resolvable = [this](const Guid& g)
                    { return ResolveId(AssetId::FromGuid(g)).has_value(); };
                    return ScanSceneReferences(*json, resolvable);
                }

                return std::vector<AssetRef>{};   // unrecognised format: documented empty (Task 3 pins the table)
            }

            uint64_t TotalBytes() const
            {
                return m_bytes.TotalBytes() +
                       m_json.TotalBytes() +
                       m_pixels.TotalBytes() +
                       m_textureInfo.TotalBytes() +
                       m_artifacts.TotalBytes() +
                       m_meshes.TotalBytes();   // Task 11
            }

            // Budget sweep, run after every insert: evict the globally
            // least-recently-used entry -- across ALL SIX caches (three from
            // ABI v15 (the texture cache went with GetTexture); ABI v21 added
            // m_textureInfo, then m_artifacts in the SAME arc, Task 7; F2c Task 11
            // added m_meshes) -- comparable via the shared recency clock -- until
            // the total is back under budget. Pinned (refcounted) entries are never
            // offered by LeastRecentEvictable and Evict refuses them; memoized
            // failures are ~zero cost and skipped (evicting them frees nothing and
            // destroys their do-not-retry memo). The budget is strict, so an
            // entry larger than the whole budget is swept right back out --
            // the caller keeps its handle (shared ownership), the cache just
            // never settles above budget. byteBudget == 0 disables the sweep.
            void EnforceBudget()
            {
                if (m_byteBudget == 0)
                    return;
                while (TotalBytes() > m_byteBudget)
                {
                    std::string key, candKey;
                    uint64_t used = UINT64_MAX, candUsed = 0;
                    int which = -1;
                    if (m_bytes.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 0;
                    }
                    if (m_json.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 1;
                    }
                    if (m_pixels.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 2;
                    }
                    if (m_textureInfo.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 3;
                    }
                    if (m_artifacts.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 4;
                    }
                    if (m_meshes.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 5;   // Task 11
                    }

                    bool evicted = false;
                    switch (which)
                    {
                    case 0: evicted = m_bytes.Evict(key); break;
                    case 1: evicted = m_json.Evict(key); break;
                    case 2: evicted = m_pixels.Evict(key); break;
                    case 3: evicted = m_textureInfo.Evict(key); break;
                    case 4: evicted = m_artifacts.Evict(key); break;
                    case 5: evicted = m_meshes.Evict(key); break;
                    default: break;
                    }
                    if (!evicted)
                        break;   // nothing evictable left (pinned/failures only)
                }
            }

            // Content-root-aware resolve, shadowing the free ExeRelative helper:
            // absolute paths pass through; a set content root anchors relatives
            // under it; otherwise the legacy exe-relative anchor.
            //
            // WHO ANCHORS: this function, and ONLY for a path the CALLER handed
            // us (the three path overloads). It is NOT applied to anything the
            // AssetResolver produced -- those are already anchored at their own
            // mount's root. One anchor per path, decided at the entry point.
            std::filesystem::path ResolveAssetPath(const std::filesystem::path& path) const
            {
                if (path.is_absolute())
                    return path;
                if (!m_contentRoot.empty())
                    return m_contentRoot / path;
                return ExeRelative(path);
            }

            // AssetId -> physical file through the installed resolver. Unresolved ids
            // warn ONCE (per id, until a new resolver is installed) -- a per-frame
            // caller polling a dead reference must not storm the log.
            //
            // WHO DOES NOT ANCHOR: this one. What comes back is LOAD-READY and is
            // handed to the *Resolved workers verbatim -- never through
            // ResolveAssetPath. A relative result (the host opened the project by
            // a relative path, e.g. `--project ReferenceProject`) is relative to
            // the process CWD, which is exactly how every other consumer of this
            // same resolver opens it (a plain ifstream), so the two agree by
            // construction. Pinned by AssetsTest.cpp's "registry-resolved ids are
            // load-ready" case (a real Project opened by a RELATIVE path, with a
            // decoy planted at the doubled location).
            std::optional<std::filesystem::path> ResolveId(const AssetId& id)
            {
                if (!id.IsValid())
                {
                    if (m_idFailures.insert(Guid::Nil()).second)
                        ARC_WARN("Assets: load requested for an invalid (nil) AssetId");
                    return std::nullopt;
                }
                if (!m_resolver)
                {
                    if (m_idFailures.insert(id.Value()).second)
                        ARC_WARN("Assets: no asset resolver installed (open a project first); "
                                 "id {}", id.Value().ToString());
                    return std::nullopt;
                }
                auto p = m_resolver(id);
                if (!p && m_idFailures.insert(id.Value()).second)
                {
                    ARC_WARN("Assets: unresolved asset id {}", id.Value().ToString());
                    // KEY OWNERSHIP: "assets.unresolved" -- this producer's OWN
                    // key. AssetRegistry::ScanContent owns the whole-tree-walk
                    // "assets" key (AssetRegistry.cpp); this memo is a per-id
                    // warn-once gate on the resolve path, not a walk, so it must
                    // NEVER Publish("assets", ...) -- that would let a single
                    // unresolved lookup clobber every row the last full scan
                    // found. m_idFailures.insert(...).second is true only the
                    // FIRST time this id fails, so this id is genuinely new here.
                    m_unresolvedDiagnosticIds.push_back(id.Value());
                    PublishUnresolvedDiagnostics();
                }
                return p;
            }

            // Rebuilds and republishes the FULL "assets.unresolved" set from
            // m_unresolvedDiagnosticIds. Diagnostics::Publish is a publication-
            // group REPLACE (see Diagnostics.hpp) -- publishing only the newest
            // id would retract every id a prior addition surfaced, so every
            // addition (ResolveId above) and every clear (SetAssetResolver)
            // republishes the whole accumulated set, not a delta.
            void PublishUnresolvedDiagnostics()
            {
                std::vector<Diagnostic> diags;
                diags.reserve(m_unresolvedDiagnosticIds.size());
                for (const Guid& g : m_unresolvedDiagnosticIds)
                {
                    Diagnostic d;
                    d.severity = DiagSeverity::Warning;
                    d.scope    = DiagScope::Assets;
                    d.code     = "assets.unresolved";
                    d.message  = "Unresolved asset id " + g.ToString();
                    d.locator  = DiagLocator::Asset(g);
                    diags.push_back(std::move(d));
                }
                Diagnostics::Publish("assets.unresolved", diags);
            }

            uint64_t m_byteBudget;
            std::filesystem::path m_contentRoot;   // empty => exe-relative (legacy)
            AssetResolver m_resolver;              // empty => AssetId loads fail
            // Desk-fix 2: empty => every Missing resolution refuses loudly and
            // memoizes, exactly as before this seam existed (see
            // SetCookPendingProbe's own doc comment in Assets.hpp).
            std::function<bool(const Guid&)> m_cookPendingProbe;
            std::unordered_set<Guid> m_idFailures; // warn-once memo per unresolved id
            // Diagnostic mirror of the SLICE of m_idFailures that came from an
            // actually-installed resolver failing to resolve (ResolveId's third
            // branch only) -- kept separate because m_idFailures ALSO carries
            // nil-id and no-resolver-installed failures, which are different
            // problems this task does not diagnose structurally. Order-preserving
            // vector (not a set): republished whole on every addition, so
            // insertion order is the row order the user sees.
            std::vector<Guid> m_unresolvedDiagnosticIds;

            // ONE recency clock across the six caches (declared first: the
            // caches capture its address) so the budget sweep can compare LRU
            // candidates cross-cache. See AssetCache's shared-clock ctor.
            // THERE IS NO GPU TEXTURE CACHE HERE: this facade holds no device --
            // m_textureInfo and m_artifacts (both ABI v21) are CPU-side data
            // (header metadata; mips + payload bytes), never device objects --
            // NriTextureCache (Task 7) is what puts an m_artifacts entry on one.
            // m_meshes (F2c Task 11) is the same CPU-side shape for the mesh kind.
            uint64_t m_lruClock = 0;
            AssetCache<BytesPtr>                 m_bytes{&m_lruClock};
            AssetCache<JsonPtr>                  m_json{&m_lruClock};
            AssetCache<PixelDataPtr>             m_pixels{&m_lruClock};
            AssetCache<TextureInfoPtr>           m_textureInfo{&m_lruClock};
            AssetCache<ArtifactPtr>              m_artifacts{&m_lruClock};
            AssetCache<MeshPtr>                  m_meshes{&m_lruClock};
        };
    }

    std::unique_ptr<Assets> Assets::Create(const AssetsDesc& desc)
    {
        return std::make_unique<AssetsImpl>(desc);
    }

    bool ContentArtifactRefusalObserved() noexcept
    {
        return g_contentRefusalObserved.load(std::memory_order_acquire);
    }

    std::string ContentArtifactRefusalDetail()
    {
        std::lock_guard<std::mutex> lock(g_contentRefusalDetailMutex);
        return g_contentRefusalDetail;
    }

    void ResetContentArtifactRefusal() noexcept
    {
        g_contentRefusalObserved.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g_contentRefusalDetailMutex);
        g_contentRefusalDetail.clear();
    }

    void SetArtifactRefusalObserver(ArtifactRefusalObserver observer, void* user)
    {
        std::lock_guard<std::mutex> lock(g_refusalObserverMutex);
        g_refusalObserver = observer;
        g_refusalObserverUser = user;
    }

    namespace
    {
        // High-quality area-average (box) downscale of an RGBA8 image to dw x dh, in ONE
        // pass: each destination texel averages the full block of source texels it covers.
        // This is the correct anti-aliased downsample for an arbitrary ratio (unlike
        // repeated 2x halving, which only hits powers of two and leaves ImGui's single-tap
        // bilinear to finish an odd residual ratio -> aliasing). Downscale only (dw<=sw).
        void DownsampleRGBA(const unsigned char* src, int sw, int sh,
                            std::vector<unsigned char>& dst, int dw, int dh)
        {
            dst.resize((size_t)dw * dh * 4);
            for (int dy = 0; dy < dh; ++dy)
            {
                int sy0 = (int)((int64_t)dy * sh / dh);
                int sy1 = (int)((int64_t)(dy + 1) * sh / dh);
                if (sy1 <= sy0) sy1 = sy0 + 1;
                for (int dx = 0; dx < dw; ++dx)
                {
                    int sx0 = (int)((int64_t)dx * sw / dw);
                    int sx1 = (int)((int64_t)(dx + 1) * sw / dw);
                    if (sx1 <= sx0) sx1 = sx0 + 1;

                    uint32_t acc[4] = {0, 0, 0, 0};
                    uint32_t n = 0;
                    for (int sy = sy0; sy < sy1; ++sy)
                        for (int sx = sx0; sx < sx1; ++sx)
                        {
                            const unsigned char* p = src + ((size_t)sy * sw + sx) * 4;
                            acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                            ++n;
                        }
                    unsigned char* d = dst.data() + ((size_t)dy * dw + dx) * 4;
                    d[0] = (unsigned char)(acc[0] / n);
                    d[1] = (unsigned char)(acc[1] / n);
                    d[2] = (unsigned char)(acc[2] / n);
                    d[3] = (unsigned char)(acc[3] / n);
                }
            }
        }
    }

    bool LoadDisplayPixels(const std::filesystem::path& path, uint32_t maxSize,
                           PixelData& out)
    {
        out = PixelData{};

        const std::filesystem::path resolved = ExeRelative(path);

        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(resolved.string().c_str(), &w, &h, &comp, 4);
        if (!data || w <= 0 || h <= 0)
        {
            // ARC_WARN (not ARC_ERROR): a missing UI image must not trip the GPU test's
            // RenderErrorCount()==0 assertion (that counter tracks render-layer validation).
            ARC_WARN("LoadDisplayPixels: not found or decode failed: {}", resolved.string());
            if (data) stbi_image_free(data);
            return false;
        }

        // Optional area-average downscale so the larger dimension fits maxSize (aspect
        // preserved). Callers size maxSize to ~2x the on-screen draw size so
        // the UI's own bilinear minification stays clean (see LoadDisplayPixels' doc).
        if (maxSize > 0 && ((uint32_t)w > maxSize || (uint32_t)h > maxSize))
        {
            int dw, dh;
            if (w >= h) { dw = (int)maxSize; dh = (h * (int)maxSize + w / 2) / w; }
            else        { dh = (int)maxSize; dw = (w * (int)maxSize + h / 2) / h; }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            DownsampleRGBA(data, w, h, out.rgba, dw, dh);
            w = dw; h = dh;
        }
        else
        {
            out.rgba.assign(data, data + (size_t)w * h * 4);
        }
        stbi_image_free(data);

        out.width  = (uint32_t)w;
        out.height = (uint32_t)h;
        return out.Valid();
    }

    void RepackStagingToRgba(const unsigned char* src, size_t rowPitch,
                             uint32_t width, uint32_t height, bool bgraSource,
                             std::vector<unsigned char>& out)
    {
        out.resize((size_t)width * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            const unsigned char* s = src + (size_t)y * rowPitch;
            unsigned char* d = out.data() + (size_t)y * width * 4;
            for (uint32_t x = 0; x < width; ++x)
            {
                const unsigned char* p = s + (size_t)x * 4;
                if (bgraSource)
                {
                    d[x * 4 + 0] = p[2];
                    d[x * 4 + 1] = p[1];
                    d[x * 4 + 2] = p[0];
                }
                else
                {
                    d[x * 4 + 0] = p[0];
                    d[x * 4 + 1] = p[1];
                    d[x * 4 + 2] = p[2];
                }
                // Opaque ALWAYS -- see the header: the render target's alpha
                // holds coverage math, not a statement about the picture.
                d[x * 4 + 3] = 255;
            }
        }
    }

    // ReadTexturePixels' body -- the PickBuffer idiom (staging copy, execute,
    // waitForIdle, map) feeding RepackStagingToRgba -- is deleted at ABI v15
    // with its declaration. Its only caller was SaveTexturePng, deleted with
    // it; the graph path gets the same bytes from
    // NriGraphContext::ReadCapture.

    bool WriteThumbnailPngRgba(const std::filesystem::path& path,
                               std::uint32_t width, std::uint32_t height,
                               std::vector<unsigned char> rgba, uint32_t maxWidth)
    {
        if (width == 0 || height == 0 ||
            rgba.size() < (std::size_t)width * height * 4)
        {
            ARC_WARN("WriteThumbnailPngRgba: {}x{} does not describe {} bytes: {}",
                     width, height, rgba.size(), path.generic_string());
            return false;
        }

        // OPAQUE, before the downscale so the box filter cannot average a
        // transparent texel back in: a screenshot is a picture of the screen,
        // and whatever coverage math left in the source's alpha channel must
        // not punch holes in it. The same rule RepackStagingToRgba states, so
        // every route into a PNG writes the same alpha.
        for (std::size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 0xFF;

        if (maxWidth > 0 && width > maxWidth)
        {
            // Width-capped (not larger-dimension like the loader): the
            // consumer is a fixed-width thumbnail tile, and the source is a
            // viewport whose aspect the user chose.
            const std::uint32_t dw = maxWidth;
            std::uint32_t dh = (height * dw + width / 2) / width;
            if (dh < 1) dh = 1;
            std::vector<unsigned char> scaled;
            DownsampleRGBA(rgba.data(), (int)width, (int)height, scaled, (int)dw, (int)dh);
            rgba = std::move(scaled);
            width = dw;
            height = dh;
        }

        return WritePngRgba(path, width, height, rgba.data());
    }

    // SaveTexturePng -- ReadTexturePixels joined to WriteThumbnailPngRgba --
    // is deleted at ABI v15 with both its declaration and its GPU half. It had
    // no callers: the editor's cover-thumbnail write goes straight to
    // WriteThumbnailPngRgba from the graph capture (EditorApp.cpp).

    bool LoadPngRgba(const std::filesystem::path& path,
                     std::uint32_t& width, std::uint32_t& height,
                     std::vector<unsigned char>& rgba)
    {
        width = height = 0;
        rgba.clear();
        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
        if (!data || w <= 0 || h <= 0)
        {
            if (data) stbi_image_free(data);
            ARC_WARN("LoadPngRgba: failed to load {}", path.string());
            return false;
        }
        width  = static_cast<std::uint32_t>(w);
        height = static_cast<std::uint32_t>(h);
        rgba.assign(data, data + (static_cast<std::size_t>(w) * h * 4));
        stbi_image_free(data);
        return true;
    }

    bool WritePngRgba(const std::filesystem::path& path,
                      std::uint32_t width, std::uint32_t height,
                      const unsigned char* rgba)
    {
        if (!rgba || width == 0 || height == 0)
        {
            ARC_WARN("WritePngRgba: nothing to write for {}", path.string());
            return false;
        }
        std::error_code ec;
        if (const auto parent = path.parent_path(); !parent.empty())
            std::filesystem::create_directories(parent, ec);   // best-effort
        if (!stbi_write_png(path.string().c_str(),
                            static_cast<int>(width), static_cast<int>(height), 4,
                            rgba, static_cast<int>(width) * 4))
        {
            ARC_WARN("WritePngRgba: write failed: {}", path.string());
            return false;
        }
        return true;
    }
}
