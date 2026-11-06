#pragma once
#include <Arcane/Modules/ArcaneAssetsApi.hpp>

// Arcane::ArtifactReader -- the ArcaneAssets runtime reader for ".arcart" artifacts. As of
// F2c Task 4 this covers BOTH artifact kinds the container format carries: Texture
// (ReadClientArtifact, F2b Task 6) and Mesh (ReadClientMeshArtifact, F2c Task 4).
//
// The byte codec lives in ArcaneAssets (ArtifactCodec.hpp); the pipeline keeps cook policy.
// ArtifactFormat.hpp's writers and this reader both call it, so the two sides share the
// version constant, SectionTag, ContentKind, the common prefix, the container section table
// and the source hash by construction. The cross-lib round-trip tests (ArtifactReaderTest.cpp
// for Texture, MeshArtifactReaderTest.cpp for Mesh) still write through the pipeline and read
// back through this reader.
//
// ON-DISK FORMAT (mirrors ArtifactFormat.hpp's header comment field-for-field; every
// multi-byte field little-endian, one byte at a time -- never a struct memcpy, for the
// same reason ArtifactFormat.cpp's own writer states: struct padding is compiler/ABI
// dependent):
//
//   COMMON PREFIX (every content kind, 37 bytes -- see ParseCommonPrefix in
//   ArtifactReader.cpp; this is the kind-agnostic slice FindArtifactForGuid's directory
//   scan actually reads, WELL inside kHeaderProbeBytes below):
//     magic "ARCA" (4 bytes)
//     artifactVersion   u32  (== 1)
//     contentKind       u8   (1 == Texture, 2 == Mesh)
//     sourceGuid.hi     u64
//     sourceGuid.lo     u64
//     sourceHash        u64  (FNV-1a 64 over the RAW SOURCE bytes -- see HashSourceBytes
//                              below; TextureImporter.cpp/MeshImporter's own hash computes
//                              the SAME fingerprint when it writes an artifact's header.
//                              For a mesh source with external buffers (a .gltf, OR a
//                              .glb whose buffers[1..] carry a `uri` -- I1),
//                              `currentSourceBytes` is the source file's own bytes FOLLOWED
//                              BY every referenced buffer's bytes, in glTF declaration
//                              order -- the exact concatenation ComputeMeshCookKey hashes
//                              (Task 5), so both sides agree by construction.)
//     importerVersion   u32
//
//   TEXTURE TAIL (contentKind == 1 only; ReadClientArtifact's own shape, unchanged since
//   F2b Task 6):
//     format            u8   (ArtifactPixelFormatValue below mirrors this byte's meaning)
//     dimension         u8
//     arrayOrDepth      u32
//     width             u32
//     height            u32
//     mipCount          u32
//     srgb              u8   (0/1)
//     thumbWidth        u32
//     thumbHeight       u32
//   ...then the shared CONTAINER SECTION TABLE below, whose bodies for this kind are:
//     MipTable  (tag 1): mipCount u32, then {offset u64, size u64, width u32, height u32}
//                        per mip, in order (slice-major, mip-minor when arrayOrDepth > 1)
//     Payload   (tag 2): raw texel bytes for every mip, back to back, addressed by each
//                        mip's own offset/size into this section
//     Thumbnail (tag 3): raw, uncompressed RGBA8 bytes, thumbWidth x thumbHeight
//
//   MESH TAIL (contentKind == 2 only; ReadClientMeshArtifact's own shape, F2c Task 4,
//   mirroring AssetPipeline::MeshArtifactDesc field-for-field):
//     vertexCount       u32
//     indexCount        u32
//     sectionCount      u32
//     indexWidth        u8   (v1's only legal value is 4 -- anything else is refused,
//                              never silently reinterpreted; a future 16-bit path is a
//                              reader BRANCH, not a byte-width guess)
//     aabbMin           f32 x3
//     aabbMax           f32 x3
//   ...then the shared CONTAINER SECTION TABLE below, whose bodies for this kind are:
//     VertexData   (tag 4): vertexCount vertices, 8 f32 each (px,py,pz, nx,ny,nz, u,v),
//                            back to back, NOT a struct memcpy -- field by field, exactly
//                            like every other multi-byte field in this format
//     IndexData    (tag 5): indexCount u32 indices, back to back
//     SectionTable (tag 6): sectionCount u32, then per section: nameLen u16, name bytes
//                            (nameLen of them, NOT null-terminated), indexOffset u32,
//                            indexCount u32, slotIndex u32 -- this is the MESH's own
//                            per-drawable-range table, NOT the shared container section
//                            table just above it (that one is a fixed {tag,offset,size}
//                            index with no tag of its own)
//     Tangents     (tag 7): RESERVED, UNWRITTEN in v1 -- a future arc appends it
//                            additively under the skip-unknown rule below
//     Thumbnail    (tag 3): RESERVED for mesh too, UNWRITTEN in v1 (R5 harvests it
//                            editor-side later) -- shares Texture's own tag 3, which is
//                            exactly why the skip-unknown rule matters: a mesh reader
//                            must tolerate a texture-kind tag turning up unused, and vice
//                            versa, since both kinds share ONE tag space
//     SECTION-RANGE VALIDATION: for every decoded SectionTable entry, indexOffset +
//     indexCount must not exceed the header's own indexCount -- a section pointing past
//     the index buffer is refused (Missing), never clamped or silently accepted, since
//     letting it through would hand the draw path an out-of-range index range (the
//     pipeline's ReadMeshArtifact applies the identical check for the identical reason).
//     SECTIONCOUNT AGREEMENT: the header's declared sectionCount must equal the number of
//     SectionTable entries actually decoded (zero, if the SectionTable section itself was
//     absent) -- a mismatch is refused (Missing), the same "declared must match decoded"
//     discipline the count-agreement rule below applies to vertices/indices.
//     SLOTINDEX BOUND (final-review fix, 2026-09-11): every decoded section's slotIndex
//     must be < the header's sectionCount. True by construction for any well-formed
//     artifact (every slot has at least one section pointing at it, so max(slotIndex)+1
//     <= sectionCount); a file violating it is corrupt and would size the editor's
//     SlotNamesFromSections mirror (MeshImportWave.cpp) by an attacker-controlled index.
//     Refused (Missing), never clamped -- the pipeline's ReadMeshArtifact applies the
//     identical rule (ArtifactFormat.hpp's banner states it as a shared rule).
//     The header's declared vertexCount/indexCount must agree with what was actually
//     DECODED: a file declaring nonzero counts but omitting the VertexData and/or
//     IndexData section entirely is refused (Missing) rather than accepted with nonzero
//     declared counts paired with empty arrays -- an absent body disagrees with its
//     declared count maximally, the same class of corruption a truncated section is (the
//     pipeline's ReadMeshArtifact refuses with nullopt for the identical reason; the two
//     readers agree on this rule through this paragraph, not through shared code).
//
//   CONTAINER SECTION TABLE (shared structure, both kinds, no tag of its own):
//     sectionCount      u32
//     sectionCount * { tag u32, offset u64, size u64 }
//   A section tag this reader does not recognise -- including a tag that belongs to the
//   OTHER content kind -- is SKIPPED, not an error -- the same forward-compat rule
//   ArtifactFormat.hpp's reader states (a future artifact kind, or a reserved tag not yet
//   written, can add/occupy a section without breaking either reader).
//
// GUID -> ARTIFACT RESOLUTION (this task's own judgment call): there is no index FILE on
// disk to read -- ArtifactStore's Guid -> cook-key index (ArtifactStore.hpp) is IN-MEMORY
// ONLY, rebuilt by scanning Artifacts/**/*.arcart; nothing in ArcaneAssetPipeline or
// arccook ever persists it. FindArtifactForGuid below therefore does its OWN directory
// scan of <intermediateDir>/Artifacts/**/*.arcart, reading only a BOUNDED PREFIX of each
// candidate (ArtifactReader.cpp's ReadFilePrefix/kHeaderProbeBytes -- 256 bytes, generous
// headroom over the fixed header's own exact 64) to test the sourceGuid match -- a few
// hundred bytes per candidate file, never the whole artifact (payload/thumbnail
// included), even across a project's whole artifact set, and it needs no persistent state
// of its own. CORRECTNESS NOTE: this paragraph used to claim exactly this cost while the
// code underneath (ReadHeaderOnly -> ReadWholeFile) actually read every candidate's ENTIRE
// file every scan -- caught by review, fixed the same task the claim was made in
// (ArtifactReader.cpp's ReadHeaderOnly/ReadFilePrefix carry the fix's own comment); take
// this kind of claim as something to VERIFY against the code, not trust from a comment.
//
// F2c TASK 4 FIX -- THE KIND-AGNOSTIC SCAN: FindArtifactForGuid's per-candidate probe (then
// named ReadHeaderOnly) used to parse the TEXTURE-shaped fixed header (ParseHeader, which
// fails closed on any contentKind != Texture) -- correct for a texture READ, but it made
// FindArtifactForGuid blind to every mesh artifact: a mesh could be cooked, committed to
// disk, and still resolve as Missing forever, no matter how many times it was recooked,
// because the scan itself never got past the contentKind gate to even compare the guid. That
// probe (renamed ReadCommonPrefixOnly) now parses ONLY the kind-agnostic COMMON PREFIX
// (ParseCommonPrefix in ArtifactReader.cpp -- 37 bytes: magic 4 + artifactVersion 4 +
// contentKind 1 + sourceGuid 16 + sourceHash 8 + importerVersion 4 -- comfortably inside the
// same kHeaderProbeBytes-bounded read above, so the cost claim in the paragraph above is
// still true), so the scan recognises every content kind's sourceGuid. Each per-kind FULL
// reader (ReadClientArtifact, ReadClientMeshArtifact) keeps its OWN fail-closed contentKind
// check unchanged -- the scan just no longer gates on it.
//
// C1(b) FIX (final-review wave, 2026-09-04): FindArtifactForGuid returns EVERY
// guid-matching candidate, not just the first the scan happens to visit. The single-match
// version was a real bug: a recook under a NEW cook key can leave the SUPERSEDED old-key
// artifact still on disk for one pass (CookSession's own self-heal, C1a, removes it
// best-effort, but a locked file or an external tool can still leave one behind), and
// BOTH the stale and the fresh artifact carry the SAME sourceGuid header -- so a
// first-match resolve could pick the stale one in unspecified directory-iteration order
// and refuse HashMismatch forever even though a valid artifact sits right next to it. The
// caller (Assets.cpp's ResolveArtifact) is the one that actually VALIDATES each candidate
// via ReadClientArtifact -- this function stays a cheap, header-only, format-agnostic
// scan; it does not itself decide which candidate wins. See Assets.cpp for how the result
// is memoized per-Guid at the facade layer (the SAME decode-once-then-cache shape
// PixelsFor already uses), which is what keeps a repeat lookup for the same guid from
// re-scanning (Missing included -- Task 8 memoized it alongside the other refusals;
// Task 12's cook-completion invalidation is the un-latch).
//
// REFUSAL DISCIPLINE (spec s5, F2b Task 6 ruling; Task 8 completed it -- "refuse, never limp"):
// applies IDENTICALLY to ReadClientMeshArtifact (F2c Task 4) -- SAME three states, SAME
// subsumption clause, just checked against kClientMeshImporterVersionMirror instead of
// kClientTextureImporterVersionMirror.
//   Missing                 -- no artifact at all resolves for this guid. A REAL refusal
//                               at the Assets facade layer since Task 8 (the sprite
//                               cutover): content is artifact-only, the stb fallback is
//                               retired, and a missing artifact refuses by name
//                               ("ArtifactMissing"), memoized and latched like the other
//                               two states below. For the mesh reader this is also the
//                               catch-all for "cannot be used and is not one of the two
//                               named refusals": absent file, unparseable/corrupt bytes,
//                               wrong contentKind, or a header sourceGuid that does not
//                               match the caller's expectedSourceGuid.
//   HashMismatch             -- an artifact EXISTS for this guid, but its header sourceHash
//                               disagrees with the hash of the CURRENT staged source bytes:
//                               the cook is stale (a source was edited without recooking)
//                               or broken. Refuses loudly -- serving stale pixels over a
//                               known-bad artifact would mask exactly the failure the cook
//                               step exists to catch.
//   VersionNewerThanEngine   -- the artifact's importerVersion is NEWER than this build's
//                               own kClientTextureImporterVersionMirror (or, for a mesh
//                               artifact, kClientMeshImporterVersionMirror): THIS ENGINE is
//                               the stale side (a downgrade, or a build-skew cook farm).
//                               Refuses loudly for the same reason.
//   SUBSUMPTION CLAUSE (restated from the spec): an artifact OLDER than the engine
//   (importerVersion < the mirror) is NOT a third refusal kind -- it collapses into Missing.
//   CookSession's own staleness check folds importerVersion into the cook KEY
//   (ComputeCookKey), so an artifact cooked under an older importer version already sits at
//   a DIFFERENT key than what today's importer would produce; the CURRENT key simply has no
//   artifact there, which the ordinary Missing path already covers without a distinct case.
//   Only a NEWER importerVersion is genuinely a different situation (this engine cannot
//   even interpret the format that artifact might be using), which is why it alone earns a
//   named refusal.

#include <Arcane/Core/Api.hpp>
#include <Arcane/Guid.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <Arcane/Core/Constant.hpp>

namespace Arcane
{
    // Client-local MIRROR of Arcane::AssetPipeline::ArtifactPixelFormat's VALUES
    // (Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactFormat.hpp) -- deliberately
    // NOT shared (this runtime reader does not include pipeline headers). The numeric values below must stay in
    // lockstep with that enum BY HAND: RGBA8=0, BC7=1, BC5_Reserved=2, BC6H_Reserved=3.
    // A drift here is caught immediately by ArtifactReaderTest.cpp's cross-lib
    // byte-contract case: it writes through the pipeline's REAL enum and reads the byte
    // back through this one, so a renumber on either side that the other did not mirror
    // shows up as a wrong `format` in the very first round-trip assertion.
    enum class ArtifactPixelFormatValue : std::uint8_t
    {
        RGBA8        = 0,
        BC7          = 1,
        BC5_Reserved = 2,
        BC6H_Reserved = 3,
    };

    // Mirrors ArtifactFormat.hpp's MipDesc (minus the pipeline type): offset/size are a
    // VIEW into LoadedClientArtifact::payload below, width/height are this mip's own true
    // (unpadded -- see TextureImporter.cpp's own BC7 padding note) dimensions.
    struct MipView
    {
        std::uint64_t offset = 0;
        std::uint64_t size   = 0;
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
    };

    // Header dims -- ALWAYS the artifact's own authoritative (possibly maxSize-clamped;
    // see TextureImporter.cpp) TOP-LEVEL dims, independent of the thumbnail's own (always
    // <=64px) dims. This is Assets::TextureInfoFor's payload (Assets.hpp) -- the dims a
    // sprite's geometry should use, never the thumbnail's.
    struct TextureInfo
    {
        std::uint32_t width    = 0;
        std::uint32_t height   = 0;
        std::uint32_t mipCount = 0;
        bool          srgb     = false;
    };

    // Task 7 consumes this EXACT name/shape (F2b Task 6 brief) -- do not rename or reshape
    // a member without checking Task 7's consumer first.
    struct LoadedClientArtifact
    {
        TextureInfo                 info;
        ArtifactPixelFormatValue    format = ArtifactPixelFormatValue::RGBA8;
        std::vector<MipView>        mips;
        std::vector<std::byte>      payload;     // every mip's encoded texels, back to back
        std::vector<std::byte>      thumbRgba;   // uncompressed RGBA8, thumbWidth x thumbHeight
        std::uint32_t               thumbWidth  = 0;
        std::uint32_t               thumbHeight = 0;
    };

    // The refusal states a caller must handle per this file's header banner above.
    enum class ArtifactRefusal
    {
        None,
        Missing,
        HashMismatch,
        VersionNewerThanEngine,
    };

    struct ArtifactReadResult
    {
        ArtifactRefusal                       refusal = ArtifactRefusal::Missing;
        std::optional<LoadedClientArtifact>   artifact;   // set iff refusal == None
    };

    // This engine's own copy of the pipeline's kTextureImporterVersion
    // (Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/CookKey.hpp) -- mirrored BY HAND,
    // never included (the runtime reader does not include pipeline headers; the value is
    // pipeline-owned cook policy). Bump this number IN
    // LOCKSTEP with that one; a mismatch left standing would make every artifact THIS
    // engine's own cook produces look "newer than the engine" to itself the moment the
    // pipeline-side constant moves without a matching edit here.
    ARC_CONSTANT("file format: mirrors the asset pipeline's texture importer version, a cook-key input")
    inline constexpr std::uint32_t kClientTextureImporterVersionMirror = 1;

    // Reads and validates ONE .arcart file at `path` against the CURRENT staged source
    // bytes (`currentSourceBytes` -- e.g. a fresh read of the .png this guid resolves to)
    // and `expectedSourceGuid`. `ArtifactRefusal::Missing` covers every "this artifact
    // cannot be used" case that is not one of the two named refusals: the file does not
    // exist, fails to parse (bad magic / truncated / corrupt section table -- the exact
    // conditions ArtifactFormat.hpp's own ReadTextureArtifact treats as nullopt), or its
    // header sourceGuid does not match `expectedSourceGuid` (a stray or renamed artifact
    // file sitting where the caller expected a different one).
    [[nodiscard]] ARC_ASSETS_API ArtifactReadResult ReadClientArtifact(
        const std::filesystem::path& path,
        std::span<const std::byte> currentSourceBytes,
        const Guid& expectedSourceGuid);

    // ---- Mesh artifacts (F2c Task 4) ----------------------------------------------------
    // The byte codec lives in ArcaneAssets (ArtifactCodec.hpp); the pipeline keeps cook policy.
    // See this file's header banner (the MESH TAIL on-disk layout) and ArtifactFormat.hpp's own
    // MeshArtifactDesc/MeshArtifactSection, which this mirrors field for field without
    // including that header.

    // Mirrors AssetPipeline::MeshArtifactSection field for field (the runtime reader does not
    // include pipeline headers). `indexOffset`/`indexCount` are in INDICES.
    struct MeshSectionView
    {
        std::string   name;
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount  = 0;
        std::uint32_t slotIndex   = 0;
    };

    struct LoadedClientMesh
    {
        std::vector<float>         vertices;   // 8 floats per vertex: pos, normal, uv
        std::vector<std::uint32_t> indices;
        std::vector<MeshSectionView> sections;
        float aabbMin[3]{ 0.0f, 0.0f, 0.0f };
        float aabbMax[3]{ 0.0f, 0.0f, 0.0f };
    };

    // This engine's own copy of AssetPipeline's kMeshImporterVersion -- mirrored BY
    // HAND, never included, exactly like kClientTextureImporterVersionMirror above.
    // Bump IN LOCKSTEP with that one.
    ARC_CONSTANT("file format: mirrors the asset pipeline's mesh importer version, a cook-key input")
    inline constexpr std::uint32_t kClientMeshImporterVersionMirror = 1;

    struct MeshArtifactReadResult
    {
        ArtifactRefusal                  refusal = ArtifactRefusal::Missing;
        std::optional<LoadedClientMesh>  mesh;   // set iff refusal == None
    };

    // The mesh half of ReadClientArtifact, with the SAME refusal discipline: Missing
    // covers "cannot be used and is not one of the two named refusals" (absent,
    // unparseable, wrong contentKind, wrong sourceGuid); HashMismatch when the
    // header's sourceHash disagrees with the CURRENT source bytes; VersionNewer
    // ThanEngine when importerVersion exceeds the mirror above. `currentSourceBytes`
    // for a .gltf with external buffers is the .gltf file's bytes FOLLOWED BY every
    // referenced buffer's, in glTF declaration order -- the exact concatenation
    // ComputeMeshCookKey hashes (Task 5), so the two sides agree by construction.
    [[nodiscard]] ARC_ASSETS_API MeshArtifactReadResult ReadClientMeshArtifact(
        const std::filesystem::path& path,
        std::span<const std::byte> currentSourceBytes,
        const Guid& expectedSourceGuid);

    // Guid -> EVERY matching artifact path via a DIRECTORY SCAN of
    // intermediateDir/Artifacts/**/*.arcart (this task's resolution choice -- see this
    // file's header banner for why no index file exists to read instead, and its C1(b)
    // paragraph for why this returns ALL matches rather than the first). Empty when no
    // artifact under the store carries this guid -- NOT itself a refusal; see
    // ArtifactRefusal::Missing's own doc above for what IS a refusal. Ordinarily a single
    // element (the common case: exactly one artifact per guid); more than one means a
    // stale, superseded artifact is still on disk alongside the current one (C1a/C1b) --
    // the caller (Assets.cpp's ResolveArtifact) is responsible for validating candidates
    // and picking the first clean one, never this function.
    [[nodiscard]] ARC_ASSETS_API std::vector<std::filesystem::path> FindArtifactForGuid(
        const std::filesystem::path& intermediateDir, const Guid& guid);

    // F2c Task 11: the CLIENT-SIDE re-derivation of a .gltf's external buffer list --
    // `currentSourceBytes` for a mesh candidate is the source file's own bytes FOLLOWED BY
    // every referenced buffer's bytes, in glTF declaration order (see this file's MESH TAIL
    // banner and the sourceHash field's own comment). `AssetsImpl` needs that exact
    // concatenation to validate a candidate artifact, but must NEVER link ArcaneAssetPipeline
    // (a runtime library does not depend on the developer pipeline) -- so this reads the buffer LIST straight out
    // of the .gltf's own JSON text (`buffers[].uri`) instead of going through cgltf.
    //
    // BYTE-CONTRACT PEER: Arcane::AssetPipeline::ReadExternalBuffers
    // (Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/MeshImporter.cpp) is the OTHER half of
    // this contract. It is not part of the byte codec (ArtifactCodec.hpp owns the artifact
    // bytes); it is source-side cook policy, so the two agree BY HAND. Both
    // sides: skip a buffer with no `uri` (embedded -- a GLB's BIN chunk, or cgltf's own
    // `.data` set some other way) and skip a `data:` URI (inline, already resolved wherever
    // it is consumed, checked against the RAW still-encoded uri on both sides); PERCENT-
    // DECODE every other uri BEFORE opening it (a reserved character -- a space, non-ASCII,
    // common in an artist's export -- is percent-escaped in a glTF buffer uri; skipping the
    // decode looks for a file literally named "my%20mesh.bin", which was never written) --
    // this side's `DecodeUriPercentEscapes` (ArtifactReader.cpp) is a byte-for-byte port of
    // `cgltf_decode_uri` (ThirdParty/cgltf/cgltf.h), the SAME function the pipeline side
    // calls (via `DecodeUriToPath`, MeshImporter.cpp) before it opens a referenced file;
    // then read every other referenced file, relative to the source's own directory, in
    // array order.
    //
    // EVERY EXTERNAL BUFFER, WHICHEVER CONTAINER (final-review fix I1, 2026-09-11): this
    // is called for BOTH mesh extensions. A .glb is NOT exempt -- the GLB container only
    // requires buffer 0 to be the BIN chunk; buffers[1..] may carry a `uri` exactly like a
    // .gltf's, and the pipeline peer (cgltf_parse sniffs the container, then reads
    // `buffers[].uri` identically) hashes such a file as .glb ++ .bin. So this function is
    // CONTAINER-AWARE: when `sourceBytes` starts with the "glTF" magic it reads the 12-byte
    // GLB header (version must be 2), then chunk 0's header (type must be "JSON",
    // 0x4E4F534A) and parses THAT chunk's slice as the JSON document; otherwise the whole
    // file is the document. Every header read is bounds-checked; a malformed GLB is nullopt.
    // (This paragraph used to say "a .glb carries no external buffers" and the caller gated
    // on the .gltf extension -- a legal .glb with a `uri` buffer refused HashMismatch
    // forever; that rule was wrong and is retired.)
    //
    // nullopt on a source that does not even parse as JSON (or whose GLB container is
    // malformed), or a referenced buffer file that cannot be read -- both collapse into the
    // SAME "this candidate's currentSourceBytes could not be built" outcome its caller turns
    // into a hash that will not match any real artifact (surfacing as HashMismatch or
    // Missing, never a silent pass -- see ResolveArtifact's own "an unreadable source reads
    // as empty" comment for the texture path's identical posture). An empty (or absent)
    // "buffers" array is NOT a failure -- zero external buffers is the common everything-
    // embedded case (a plain .glb, or a .gltf with data: URIs).
    [[nodiscard]] ARC_ASSETS_API std::optional<std::vector<std::vector<std::byte>>> ReadClientExternalBuffers(
        std::span<const std::byte> sourceBytes, const std::filesystem::path& sourcePath);
}
