#include "Arcane/AssetPipeline/ArtifactFormat.hpp"

#include <array>
#include <string>
#include <Arcane/Core/Constant.hpp>

namespace Arcane::AssetPipeline
{
    // The codec's names, in this namespace's spelling.
    using Arcane::ArtifactCommonPrefix;
    using Arcane::ArtifactSectionBody;
    using Arcane::ArtifactSectionEntry;
    using Arcane::ByteReader;
    using Arcane::ByteWriter;
    using Arcane::kMeshSectionEntrySize;
    using Arcane::kMipEntrySize;

    namespace
    {
        // Mip/mesh-section widths, the version, the magic and the section entry width live in
        // the codec (ArtifactCodec.hpp).
        // Generous headroom over the fixed 37-byte common prefix ReadArtifactPrefix actually
        // parses, so a future prefix field does not silently start under-reading -- still
        // minuscule next to a real artifact's payload: RebuildIndexFromScan must never pay
        // for anything past the header.
        ARC_CONSTANT("file format: covers every prefix field ReadArtifactPrefix parses, with headroom")
        constexpr std::size_t kPrefixProbeBytes = 256;
    }

    std::optional<ArtifactPrefix> ReadArtifactPrefix(const std::filesystem::path& path)
    {
        const std::optional<std::vector<std::byte>> raw = ReadArtifactPrefixBytes(path, kPrefixProbeBytes);
        if (!raw) return std::nullopt;

        ByteReader r(raw->data(), raw->size());
        ArtifactCommonPrefix common;
        if (!ParseCommonPrefix(r, common)) return std::nullopt;

        // NO contentKind GATE HERE, deliberately -- see this function's own doc comment
        // (ArtifactFormat.hpp). Each kind's own full reader keeps its fail-closed check.
        ArtifactPrefix prefix;
        prefix.contentKind = static_cast<ContentKind>(common.contentKind);
        prefix.sourceGuid = common.sourceGuid;
        prefix.sourceHash = common.sourceHash;
        prefix.importerVersion = common.importerVersion;
        return prefix;
    }

    bool WriteTextureArtifact(const std::filesystem::path& path, const TextureArtifactDesc& desc,
                               std::span<const std::byte> payload, std::span<const std::byte> thumbRgba)
    {
        // MipTable section body: count + {offset, size, width, height} per mip, IN ORDER --
        // slice-major, mip-minor when arrayOrDepth > 1; the vector's own order IS that order,
        // never resorted.
        ByteWriter mipTable;
        mipTable.U32(static_cast<std::uint32_t>(desc.mips.size()));
        for (const MipDesc& mip : desc.mips)
        {
            mipTable.U64(mip.offset);
            mipTable.U64(mip.size);
            mipTable.U32(mip.width);
            mipTable.U32(mip.height);
        }

        // Fixed header: the common prefix, then the desc fields in declaration order.
        ByteWriter header;
        WriteCommonPrefix(header, ArtifactCommonPrefix{ static_cast<std::uint8_t>(desc.contentKind),
                                                        desc.sourceGuid, desc.sourceHash, desc.importerVersion });
        header.U8(static_cast<std::uint8_t>(desc.format));
        header.U8(static_cast<std::uint8_t>(desc.dimension));
        header.U32(desc.arrayOrDepth);
        header.U32(desc.width);
        header.U32(desc.height);
        header.U32(desc.mipCount);
        header.U8(desc.srgb ? 1 : 0);
        header.U32(desc.thumbWidth);
        header.U32(desc.thumbHeight);

        const std::array<ArtifactSectionBody, 3> sections{ {
            { SectionTag::MipTable,  std::span<const std::byte>(mipTable.buf) },
            { SectionTag::Payload,   payload },
            { SectionTag::Thumbnail, thumbRgba },
        } };

        const std::vector<std::byte> file = AssembleArtifact(header, sections);
        return WriteArtifactFile(path, file);
    }

    std::optional<LoadedArtifact> ReadTextureArtifact(const std::filesystem::path& path)
    {
        auto rawOpt = ReadArtifactBytes(path);
        if (!rawOpt) return std::nullopt;
        std::vector<std::byte> raw = std::move(*rawOpt);

        ByteReader r(raw.data(), raw.size());

        ArtifactCommonPrefix prefix;
        if (!ParseCommonPrefix(r, prefix)) return std::nullopt;

        LoadedArtifact loaded;
        TextureArtifactDesc& desc = loaded.desc;

        // Fail closed on anything but Texture rather than "parse" (misread) another
        // artifact kind's bytes just because the fixed header widths happen to still fit.
        if (prefix.contentKind != static_cast<std::uint8_t>(ContentKind::Texture))
            return std::nullopt;
        desc.contentKind = static_cast<ContentKind>(prefix.contentKind);
        desc.sourceGuid = prefix.sourceGuid;
        desc.sourceHash = prefix.sourceHash;
        desc.importerVersion = prefix.importerVersion;

        std::uint8_t format = 0, dimension = 0;
        if (!r.U8(format)) return std::nullopt;
        desc.format = static_cast<ArtifactPixelFormat>(format);
        if (!r.U8(dimension)) return std::nullopt;
        desc.dimension = static_cast<ArtifactDimension>(dimension);

        if (!r.U32(desc.arrayOrDepth)) return std::nullopt;
        if (!r.U32(desc.width)) return std::nullopt;
        if (!r.U32(desc.height)) return std::nullopt;
        if (!r.U32(desc.mipCount)) return std::nullopt;

        std::uint8_t srgb = 0;
        if (!r.U8(srgb)) return std::nullopt;
        desc.srgb = (srgb != 0);

        if (!r.U32(desc.thumbWidth)) return std::nullopt;
        if (!r.U32(desc.thumbHeight)) return std::nullopt;

        // The container's top-level {tag, offset, size} index, shared by both kinds; the
        // codec refuses a count that could not fit in the file before it reserves.
        std::vector<ArtifactSectionEntry> entries;
        if (!ReadSectionTable(r, raw.size(), entries)) return std::nullopt;

        for (const ArtifactSectionEntry& e : entries)
        {
            std::vector<std::byte> body;
            if (!r.Slice(e.offset, e.size, body))
                return std::nullopt;   // section runs past EOF -- truncated/corrupt file

            switch (static_cast<SectionTag>(e.tag))
            {
            case SectionTag::MipTable:
            {
                ByteReader mr(body.data(), body.size());
                std::uint32_t mipCount = 0;
                if (!mr.U32(mipCount)) return std::nullopt;

                // I3 fix (final-review wave): same cheap sanity bound as the section
                // table's own check above, same rationale -- each mip entry costs at
                // least kMipEntrySize bytes on disk, so a mip count that could not
                // possibly fit in THIS section's own body is corrupt. Without this, a
                // crafted mipCount (e.g. 0xFFFFFFFF) would ask std::vector::reserve for
                // ~64 GB and THROW std::bad_alloc out of this read instead of refusing
                // it cleanly (nullopt).
                if (static_cast<std::uint64_t>(mipCount) * kMipEntrySize > body.size())
                    return std::nullopt;

                desc.mips.clear();
                desc.mips.reserve(mipCount);
                for (std::uint32_t i = 0; i < mipCount; ++i)
                {
                    MipDesc mip{};
                    if (!mr.U64(mip.offset) || !mr.U64(mip.size) || !mr.U32(mip.width) || !mr.U32(mip.height))
                        return std::nullopt;
                    desc.mips.push_back(mip);
                }
                break;
            }
            case SectionTag::Payload:
                loaded.payload = std::move(body);
                break;
            case SectionTag::Thumbnail:
                loaded.thumbRgba = std::move(body);
                break;
            default:
                // Forward-compat: a section tag this reader doesn't recognise (e.g. a future
                // artifact kind's section) is skipped, not an error.
                break;
            }
        }

        return loaded;
    }

    // ---- Mesh artifacts (F2c) --------------------------------------------------------------
    // Writer/reader pair over the ArcaneAssets byte codec (ArtifactCodec.hpp) -- see
    // ArtifactFormat.hpp's file banner for why the two
    // kinds share ONE container format and ONE SectionTag space, and ArtifactFormat.hpp's own
    // struct comments for the A1/A5 design comparisons cited below.

    bool WriteMeshArtifact(const std::filesystem::path& path, const MeshArtifactDesc& desc,
                            std::span<const MeshArtifactVertex> vertices, std::span<const std::uint32_t> indices)
    {
        // The MESH's own SectionTable body (tag SectionTable): count + {nameLen(u16), name
        // bytes, indexOffset(u32), indexCount(u32), slotIndex(u32)} per section, IN ORDER.
        // This is NOT the container's own section table below (that one has no tag and is a
        // fixed {tag,offset,size} index over the three top-level sections).
        ByteWriter sectionRecords;
        sectionRecords.U32(static_cast<std::uint32_t>(desc.sections.size()));
        for (const MeshArtifactSection& s : desc.sections)
        {
            // A name longer than 65535 bytes is clamped with one comment saying so -- glTF
            // material names are identifiers, not documents.
            const std::size_t clampedLen = s.name.size() > 0xFFFFu ? 0xFFFFu : s.name.size();
            sectionRecords.U16(static_cast<std::uint16_t>(clampedLen));
            for (std::size_t k = 0; k < clampedLen; ++k)
                sectionRecords.U8(static_cast<std::uint8_t>(s.name[k]));
            sectionRecords.U32(s.indexOffset);
            sectionRecords.U32(s.indexCount);
            sectionRecords.U32(s.slotIndex);
        }

        // Vertex/index data are emitted through F32/U32 field by field, NOT a std::span
        // memcpy of the structs -- MeshArtifactVertex is eight floats with no padding on
        // every toolchain we build, but the artifact's whole determinism contract is "never
        // trust a struct's memory layout", and honouring it here costs one loop.
        ByteWriter vertexData;
        for (const MeshArtifactVertex& v : vertices)
        {
            vertexData.F32(v.px); vertexData.F32(v.py); vertexData.F32(v.pz);
            vertexData.F32(v.nx); vertexData.F32(v.ny); vertexData.F32(v.nz);
            vertexData.F32(v.u);  vertexData.F32(v.v);
        }

        ByteWriter indexData;
        for (std::uint32_t idx : indices)
            indexData.U32(idx);

        // Fixed header: the common prefix, then the desc fields in declaration order.
        ByteWriter header;
        WriteCommonPrefix(header, ArtifactCommonPrefix{ static_cast<std::uint8_t>(desc.contentKind),
                                                        desc.sourceGuid, desc.sourceHash, desc.importerVersion });
        header.U32(desc.vertexCount);
        header.U32(desc.indexCount);
        header.U32(desc.sectionCount);
        header.U8(desc.indexWidth);
        header.F32(desc.aabbMin[0]); header.F32(desc.aabbMin[1]); header.F32(desc.aabbMin[2]);
        header.F32(desc.aabbMax[0]); header.F32(desc.aabbMax[1]); header.F32(desc.aabbMax[2]);

        const std::array<ArtifactSectionBody, 3> sections{ {
            { SectionTag::VertexData,   std::span<const std::byte>(vertexData.buf) },
            { SectionTag::IndexData,    std::span<const std::byte>(indexData.buf) },
            { SectionTag::SectionTable, std::span<const std::byte>(sectionRecords.buf) },
        } };

        const std::vector<std::byte> file = AssembleArtifact(header, sections);
        return WriteArtifactFile(path, file);
    }

    std::optional<LoadedMeshArtifact> ReadMeshArtifact(const std::filesystem::path& path)
    {
        auto rawOpt = ReadArtifactBytes(path);
        if (!rawOpt) return std::nullopt;
        std::vector<std::byte> raw = std::move(*rawOpt);

        ByteReader r(raw.data(), raw.size());

        ArtifactCommonPrefix prefix;
        if (!ParseCommonPrefix(r, prefix)) return std::nullopt;

        LoadedMeshArtifact loaded;
        MeshArtifactDesc& desc = loaded.desc;

        // Fail closed on anything but Mesh rather than "parse" (misread) another
        // artifact kind's bytes just because the fixed header widths happen to still fit.
        if (prefix.contentKind != static_cast<std::uint8_t>(ContentKind::Mesh))
            return std::nullopt;
        desc.contentKind = static_cast<ContentKind>(prefix.contentKind);
        desc.sourceGuid = prefix.sourceGuid;
        desc.sourceHash = prefix.sourceHash;
        desc.importerVersion = prefix.importerVersion;

        if (!r.U32(desc.vertexCount)) return std::nullopt;
        if (!r.U32(desc.indexCount)) return std::nullopt;
        if (!r.U32(desc.sectionCount)) return std::nullopt;

        if (!r.U8(desc.indexWidth)) return std::nullopt;
        // Comparison A5: the byte is DECLARED, not inferred. v1's only legal value is 4; a
        // file claiming otherwise is refused rather than read as though its indices were u32
        // anyway -- a future 16-bit path is a reader BRANCH, not a silent reinterpretation.
        if (desc.indexWidth != 4)
            return std::nullopt;

        for (float& c : desc.aabbMin) { if (!r.F32(c)) return std::nullopt; }
        for (float& c : desc.aabbMax) { if (!r.F32(c)) return std::nullopt; }

        // The container's top-level {tag, offset, size} index, shared by both kinds; the
        // codec refuses a count that could not fit in the file before it reserves.
        std::vector<ArtifactSectionEntry> entries;
        if (!ReadSectionTable(r, raw.size(), entries)) return std::nullopt;

        for (const ArtifactSectionEntry& e : entries)
        {
            std::vector<std::byte> body;
            if (!r.Slice(e.offset, e.size, body))
                return std::nullopt;   // section runs past EOF -- truncated/corrupt file

            switch (static_cast<SectionTag>(e.tag))
            {
            case SectionTag::VertexData:
            {
                if (body.size() != static_cast<std::uint64_t>(desc.vertexCount) * sizeof(MeshArtifactVertex))
                    return std::nullopt;
                ByteReader vr(body.data(), body.size());
                loaded.vertices.clear();
                loaded.vertices.reserve(desc.vertexCount);
                for (std::uint32_t i = 0; i < desc.vertexCount; ++i)
                {
                    MeshArtifactVertex v{};
                    if (!vr.F32(v.px) || !vr.F32(v.py) || !vr.F32(v.pz) ||
                        !vr.F32(v.nx) || !vr.F32(v.ny) || !vr.F32(v.nz) ||
                        !vr.F32(v.u)  || !vr.F32(v.v))
                        return std::nullopt;
                    loaded.vertices.push_back(v);
                }
                break;
            }
            case SectionTag::IndexData:
            {
                if (body.size() != static_cast<std::uint64_t>(desc.indexCount) * 4)
                    return std::nullopt;
                ByteReader ir(body.data(), body.size());
                loaded.indices.clear();
                loaded.indices.reserve(desc.indexCount);
                for (std::uint32_t i = 0; i < desc.indexCount; ++i)
                {
                    std::uint32_t idx = 0;
                    if (!ir.U32(idx)) return std::nullopt;
                    if (idx >= desc.vertexCount) return std::nullopt;   // would read past the vertex buffer
                    loaded.indices.push_back(idx);
                }
                break;
            }
            case SectionTag::SectionTable:
            {
                ByteReader mr(body.data(), body.size());
                std::uint32_t meshSectionCount = 0;
                if (!mr.U32(meshSectionCount)) return std::nullopt;

                // Same reserve-bound guard the mip table has (I3): the minimum on-disk width
                // of one entry is kMeshSectionEntrySize (a zero-length name), so a count that
                // could not possibly fit in THIS section's own body is corrupt -- refuse
                // cleanly rather than let a crafted count throw bad_alloc out of the reserve
                // below.
                if (static_cast<std::uint64_t>(meshSectionCount) * kMeshSectionEntrySize > body.size())
                    return std::nullopt;

                desc.sections.clear();
                desc.sections.reserve(meshSectionCount);
                for (std::uint32_t i = 0; i < meshSectionCount; ++i)
                {
                    std::uint16_t nameLen = 0;
                    if (!mr.U16(nameLen)) return std::nullopt;

                    std::string name;
                    name.reserve(nameLen);   // nameLen is u16-bounded (<=65535) -- safe to reserve directly
                    for (std::uint16_t k = 0; k < nameLen; ++k)
                    {
                        std::uint8_t ch = 0;
                        if (!mr.U8(ch)) return std::nullopt;   // a truncated name IS a bounds failure
                        name.push_back(static_cast<char>(ch));
                    }

                    MeshArtifactSection section{};
                    section.name = std::move(name);
                    if (!mr.U32(section.indexOffset) || !mr.U32(section.indexCount) || !mr.U32(section.slotIndex))
                        return std::nullopt;
                    desc.sections.push_back(std::move(section));
                }
                break;
            }
            default:
                // Forward-compat: a section tag this reader doesn't recognise (e.g. a future
                // Tangents/Thumbnail body, or even one of the TEXTURE kind's own tags) is
                // skipped, not an error -- see the file banner's shared-tag-space rationale.
                break;
            }
        }

        // One extra validation the texture reader has no analogue for: a section pointing
        // past the index buffer is a corrupt file, and letting it through would hand Plan 2's
        // draw path an out-of-range CmdDrawIndexed.
        for (const MeshArtifactSection& section : desc.sections)
        {
            if (static_cast<std::uint64_t>(section.indexOffset) + section.indexCount > desc.indexCount)
                return std::nullopt;
            // SLOTINDEX BOUND (final-review fix, 2026-09-11; ArtifactFormat.hpp's banner): a
            // slotIndex at or past sectionCount cannot occur in a well-formed artifact
            // (every slot has >= 1 section, so max(slotIndex) + 1 <= sectionCount) and
            // would size SlotNamesFromSections' table -- and the editor's companion slot
            // array -- by a corrupt value. Refused, never clamped; the client reader
            // applies the identical rule.
            if (section.slotIndex >= desc.sectionCount)
                return std::nullopt;
        }
        // The header's declared sectionCount must agree with what the SectionTable section
        // actually carried (or, if that section was absent entirely, with zero).
        if (static_cast<std::uint32_t>(desc.sections.size()) != desc.sectionCount)
            return std::nullopt;

        // The header's declared vertexCount/indexCount must likewise agree with what was
        // actually DECODED -- a file that declares nonzero counts but omits the VertexData
        // and/or IndexData section entirely would otherwise pass every check above and come
        // back as a LoadedMeshArtifact with nonzero declared counts paired with empty arrays:
        // silently inconsistent output. An absent body disagrees with its declared count
        // maximally, the same class of corruption the per-section size checks above already
        // refuse for a body that is merely the WRONG size.
        if (loaded.vertices.size() != desc.vertexCount) return std::nullopt;
        if (loaded.indices.size() != desc.indexCount) return std::nullopt;

        return loaded;
    }

    std::vector<std::string> SlotNamesFromSections(std::span<const MeshArtifactSection> sections)
    {
        bool any = false;
        std::uint32_t maxSlot = 0;
        for (const MeshArtifactSection& s : sections)
        {
            any = true;
            if (s.slotIndex > maxSlot) maxSlot = s.slotIndex;
        }
        if (!any) return {};

        // Dedup BY NAME (A1): the first section that points at a given slot names it; every
        // later section pointing at the same slot agrees by construction (the two sections
        // sharing one glTF material share one name too), so "first wins" is equivalent to
        // "they all agree".
        std::vector<std::string> names(static_cast<std::size_t>(maxSlot) + 1);
        std::vector<bool> filled(names.size(), false);
        for (const MeshArtifactSection& s : sections)
        {
            if (!filled[s.slotIndex])
            {
                names[s.slotIndex] = s.name;
                filled[s.slotIndex] = true;
            }
        }
        return names;
    }
}
