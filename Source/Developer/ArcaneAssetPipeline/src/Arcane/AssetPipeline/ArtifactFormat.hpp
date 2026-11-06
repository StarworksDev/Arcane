#pragma once
#include <Arcane/Modules/ArcaneAssetPipelineApi.hpp>

// Arcane::AssetPipeline::ArtifactFormat -- the ".arcart" artifact container: a small,
// stable-on-disk binary format the offline cook (arccook, later tasks) writes and the runtime
// content loader reads. Two artifact kinds share this one container, discriminated via
// ContentKind: Texture (WriteTextureArtifact/ReadTextureArtifact) and, as of F2c, Mesh
// (WriteMeshArtifact/ReadMeshArtifact). Fixed little-endian header + a tagged section table
// (MipTable/Payload/Thumbnail for Texture; VertexData/IndexData/SectionTable/Tangents for
// Mesh -- SectionTag values 1-7 all live in ONE shared space) so each kind's reader SKIPS a
// section tag it does not recognise -- including a tag that belongs to the OTHER kind --
// rather than failing on it (forward-compat).
//
// Every multi-byte field is written EXPLICITLY, one byte at a time, little-endian -- never a
// raw struct memcpy. Struct padding is compiler/ABI dependent; a memcpy'd struct would make the
// artifact non-deterministic across toolchains/configs, which breaks content-hash-based caching
// in later tasks (spec s4).
//
// The byte codec lives in ArcaneAssets (Source/Runtime/ArcaneAssets/src/Arcane/Assets/
// ArtifactCodec.hpp): the version constant, SectionTag, ContentKind, the common prefix, the
// container section table and the source hash. The pipeline keeps cook policy.
//
// MESH READER VALIDATION RULES shared by BOTH readers, restated here so neither side drifts
// (ReadMeshArtifact below; ReadClientMeshArtifact on the client): indexWidth == 4;
// every section's indexOffset + indexCount <= indexCount (SECTION-RANGE); decoded
// SectionTable entries == declared sectionCount; decoded vertices/indices == declared
// counts (an ABSENT body disagrees maximally); and -- final-review fix, 2026-09-11 -- every
// section's slotIndex < sectionCount (SLOTINDEX BOUND: true by construction, since every
// slot has at least one section pointing at it, so max(slotIndex) + 1 <= sectionCount; a
// file violating it is corrupt and would hand SlotNamesFromSections / the editor's companion
// mint a slot table sized by an attacker-controlled index). Each is refused with nullopt
// (pipeline) / Missing (client), never clamped.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Arcane/Guid.hpp"
#include <Arcane/Assets/ArtifactCodec.hpp>

namespace Arcane::AssetPipeline
{
    enum class ArtifactPixelFormat : std::uint8_t
    {
        RGBA8,
        BC7,
        BC5_Reserved,
        BC6H_Reserved,
    };

    enum class ArtifactDimension : std::uint8_t
    {
        Tex2D,
        Tex2DArray_Reserved,
        TexCube_Reserved,
        TexCubeArray_Reserved,
        Tex3D_Reserved,
    };

    // SectionTag and ContentKind are owned by the codec header; re-exported here so the
    // pipeline's writers, importers and tools keep their spelling.
    using Arcane::ContentKind;
    using Arcane::SectionTag;

    // The common prefix every artifact kind shares, read WITHOUT committing to a kind:
    // exactly what ArtifactStore::RebuildIndexFromScan needs to recover a Guid, and
    // nothing more. That scan called ReadTextureArtifact, which fails closed on any
    // other kind (this file's own contentKind gate), so before F2c every mesh artifact
    // was invisible to the index -- and CookProject's supersede-the-old-key self-heal,
    // which reads Lookup, could never fire for one.
    struct ARC_ASSET_PIPELINE_API ArtifactPrefix
    {
        ContentKind   contentKind = ContentKind::Texture;
        Guid          sourceGuid;
        std::uint64_t sourceHash = 0;
        std::uint32_t importerVersion = 0;
    };

    // Reads a BOUNDED PREFIX, never the whole file: this runs once per artifact in the
    // store on every scan, and a texture artifact's payload is megabytes. Goes through
    // the codec's ReadArtifactPrefixBytes/ParseCommonPrefix (the same calls the runtime
    // reader's FindArtifactForGuid makes) -- a generous fixed-size read from
    // the start of the file, well inside which the 37-byte common prefix (magic 4 +
    // artifactVersion 4 + contentKind 1 + sourceGuid 16 + sourceHash 8 + importerVersion
    // 4) always lives. Deliberately does NOT gate on contentKind -- that is the whole
    // point: this function answers "whose guid is this, and what kind is it" for ANY
    // artifact kind, kind-agnostically, so a scan built on it can never go blind to a
    // kind it doesn't already know about.
    [[nodiscard]] ARC_ASSET_PIPELINE_API std::optional<ArtifactPrefix> ReadArtifactPrefix(
        const std::filesystem::path& path);

    struct ARC_ASSET_PIPELINE_API MipDesc
    {
        std::uint64_t offset;
        std::uint64_t size;
        std::uint32_t width, height;
    };

    // Header, serialized little-endian, explicit field order (see WriteTextureArtifact's body
    // for the exact on-disk order -- it mirrors this declaration order field-for-field).
    struct ARC_ASSET_PIPELINE_API TextureArtifactDesc
    {
        ContentKind contentKind = ContentKind::Texture;   // spec s4 pins it; do not drop

        Guid sourceGuid;
        std::uint64_t sourceHash;
        std::uint32_t importerVersion;

        ArtifactPixelFormat format;
        ArtifactDimension dimension;   // Tex2D this slice
        std::uint32_t arrayOrDepth;    // 1 this slice

        std::uint32_t width, height, mipCount;
        bool srgb;

        std::vector<MipDesc> mips;     // slice-major, mip-minor when arrayOrDepth > 1

        std::uint32_t thumbWidth, thumbHeight;
    };

    // Writes a complete .arcart file at `path`: fixed header, then the section table, then the
    // MipTable/Payload/Thumbnail sections themselves. `payload` is the (already-encoded, e.g.
    // BC7-compressed) texel data for every mip back to back, addressed by each MipDesc's own
    // offset/size; `thumbRgba` is a small uncompressed RGBA8 preview. Returns false on any IO
    // failure (bad path, disk full, etc.) -- the file is not guaranteed to exist on false.
    [[nodiscard]] ARC_ASSET_PIPELINE_API bool WriteTextureArtifact(const std::filesystem::path& path,
                                             const TextureArtifactDesc& desc,
                                             std::span<const std::byte> payload,
                                             std::span<const std::byte> thumbRgba);

    struct ARC_ASSET_PIPELINE_API LoadedArtifact
    {
        TextureArtifactDesc desc;
        std::vector<std::byte> payload;
        std::vector<std::byte> thumbRgba;
    };

    // Reads and validates a .arcart file. Returns nullopt on bad magic, an unrecognised
    // artifactVersion, or a truncated/corrupt section table (a section's offset+size runs past
    // EOF). A SectionTag this reader does not recognise is skipped, not treated as an error --
    // that is what keeps future artifact sections forward-compatible with this reader.
    [[nodiscard]] ARC_ASSET_PIPELINE_API std::optional<LoadedArtifact> ReadTextureArtifact(const std::filesystem::path& path);

    // ---- Mesh artifacts (F2c) ---------------------------------------------------------------
    // Writer/reader pair over the byte codec in ArcaneAssets (ArtifactCodec.hpp), which owns the
    // byte-explicit ByteWriter/ByteReader primitives and the SectionTag space -- see the file
    // banner for why one shared tag space is load-bearing rather than incidental.

    // One drawable range with its material slot. `slotIndex` is comparison A1: two primitives
    // sharing one glTF material become TWO sections pointing at ONE slot, never two identically
    // -named slots.
    struct ARC_ASSET_PIPELINE_API MeshArtifactSection
    {
        std::string   name;          // the primitive's material name; empty when absent
        std::uint32_t indexOffset;   // into IndexData, in INDICES (not bytes)
        std::uint32_t indexCount;
        std::uint32_t slotIndex;
    };

    // 32 bytes, interleaved pos/normal/uv -- the pipeline's fixed vertex stride (MeshNode.cpp's
    // vertex input). DELIBERATELY NOT Arcane::MeshVertex: that type lives in ArcaneClient
    // (Mesh/MeshBuilder.hpp) and this library must never include ArcaneClient. The two are
    // hand-mirrored, and the mirror is pinned by MeshArtifactReaderTest.cpp's cross-lib
    // round-trip plus the static_assert on sizeof below.
    struct ARC_ASSET_PIPELINE_API MeshArtifactVertex
    {
        float px, py, pz;
        float nx, ny, nz;
        float u, v;
    };
    static_assert(sizeof(MeshArtifactVertex) == 32,
                  "the artifact's vertex stride is the pipeline's binding stride");

    // Header, serialized little-endian in DECLARATION ORDER (WriteMeshArtifact's body mirrors
    // this field for field). The first four fields are the KIND-AGNOSTIC COMMON PREFIX every
    // artifact kind shares -- see ArtifactStore::RebuildIndexFromScan and ArtifactReader's
    // ParseCommonPrefix, both of which read only this much.
    struct ARC_ASSET_PIPELINE_API MeshArtifactDesc
    {
        ContentKind   contentKind = ContentKind::Mesh;
        Guid          sourceGuid;
        std::uint64_t sourceHash;
        std::uint32_t importerVersion;

        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t sectionCount = 0;
        // Comparison A5: DECLARED, not inferred. 4 in v1, asserted == 4 on read, so a future
        // 16-bit path is a reader BRANCH rather than a tag-presence dance duplicated across
        // the pipeline reader and the runtime reader.
        std::uint8_t  indexWidth = 4;

        // The cooked AABB (s7.1): taken from the artifact rather than recomputed on load.
        // ComputeMeshBounds stays the path for GENERATED primitives.
        float aabbMin[3]{ 0.0f, 0.0f, 0.0f };
        float aabbMax[3]{ 0.0f, 0.0f, 0.0f };

        std::vector<MeshArtifactSection> sections;
    };

    // Writes a complete .arcart mesh artifact: the header above, the container's section
    // table, then VertexData / IndexData / SectionTable. Tangents and Thumbnail are NOT
    // written (spec s5.2: reserved) -- a later arc appends them additively under skip-unknown.
    // Returns false on any IO failure.
    [[nodiscard]] ARC_ASSET_PIPELINE_API bool WriteMeshArtifact(const std::filesystem::path& path,
                                          const MeshArtifactDesc& desc,
                                          std::span<const MeshArtifactVertex> vertices,
                                          std::span<const std::uint32_t> indices);

    struct ARC_ASSET_PIPELINE_API LoadedMeshArtifact
    {
        MeshArtifactDesc desc;
        std::vector<MeshArtifactVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    // Reads and validates a mesh .arcart. nullopt on bad magic, an unrecognised
    // artifactVersion, a contentKind that is not Mesh, indexWidth != 4, a truncated section, or
    // a section whose declared counts disagree with its body's size. An unrecognised
    // SectionTag is SKIPPED, not an error. The header's declared vertexCount/indexCount must
    // also agree with what was actually DECODED: a file that declares nonzero counts but
    // omits the VertexData and/or IndexData section entirely is refused rather than returned
    // with nonzero declared counts paired with empty arrays -- an absent body disagrees with
    // its declared count maximally, the same class of corruption a truncated section is.
    // A section whose slotIndex >= sectionCount is refused too (the SLOTINDEX BOUND in this
    // file's banner -- the client reader applies the identical rule).
    [[nodiscard]] ARC_ASSET_PIPELINE_API std::optional<LoadedMeshArtifact> ReadMeshArtifact(
        const std::filesystem::path& path);

    // Slot names, derived from the section table: slot k's name is the name of any section
    // pointing at k (they agree by construction -- the slot array dedups BY NAME, A1). The
    // artifact stores no separate slot list because it needs none; this is the function the
    // editor's companion mint reads authoritative names through (spec s4.2). Sized to
    // max(slotIndex)+1, empty when there are no sections.
    [[nodiscard]] ARC_ASSET_PIPELINE_API std::vector<std::string> SlotNamesFromSections(
        std::span<const MeshArtifactSection> sections);
}
