#pragma once
#include <Arcane/Modules/ArcaneAssetsApi.hpp>

// Arcane::ArtifactCodec -- the one ".arcart" byte codec. The byte codec lives in ArcaneAssets;
// the asset pipeline keeps cook policy (importers, BC7, glTF, maxSize, the supersede rule).
//
// This header owns the container's constants (kArtifactVersion, SectionTag, ContentKind, the
// magic and the fixed entry widths), the byte-explicit ByteWriter/ByteReader, the common
// prefix layout (magic, version, contentKind, sourceGuid, sourceHash, importerVersion), the
// container section table, the file-assembly order (header, section table, bodies back to
// back), the bounded file reads, and the FNV-1a source hash. The pipeline's writers and the
// runtime reader both call it, so the two sides cannot drift.
//
// Every multi-byte field is written EXPLICITLY, one byte at a time, little-endian -- never a
// raw struct memcpy (struct padding is toolchain dependent and would make the artifact
// non-deterministic, which breaks content-hash caching).

#include <Arcane/Core/Constant.hpp>
#include <Arcane/Guid.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace Arcane
{
    ARC_CONSTANT("file format: the cooked-artifact container version")
    inline constexpr std::uint32_t kArtifactVersion = 1;

    inline constexpr std::array<std::uint8_t, 4> kArtifactMagic{
        static_cast<std::uint8_t>('A'), static_cast<std::uint8_t>('R'),
        static_cast<std::uint8_t>('C'), static_cast<std::uint8_t>('A'),
    };

    // On-disk width of one container section table entry: tag(u32) + offset(u64) + size(u64).
    ARC_CONSTANT("file format: on-disk width of a container section table entry")
    inline constexpr std::uint64_t kSectionEntrySize = 4 + 8 + 8;
    // On-disk width of one texture MipTable entry: offset(u64) + size(u64) + width(u32) + height(u32).
    ARC_CONSTANT("file format: on-disk width of a MipTable entry")
    inline constexpr std::uint64_t kMipEntrySize = 8 + 8 + 4 + 4;
    // Minimum on-disk width of one MESH SectionTable entry (a zero-length name):
    // nameLen(u16) + indexOffset(u32) + indexCount(u32) + slotIndex(u32).
    ARC_CONSTANT("file format: minimum on-disk width of a mesh SectionTable entry")
    inline constexpr std::uint64_t kMeshSectionEntrySize = 2 + 4 + 4 + 4;

    // Tagged-section table entries. Values are stable on disk -- never renumber an existing
    // tag; a future section kind adds a NEW value. Every reader skips a tag it does not
    // recognise (forward-compat), including a tag that belongs to the other content kind.
    // 1-3 are the texture kind's; 4-7 are the mesh kind's, in ONE shared space on purpose.
    enum class SectionTag : std::uint32_t
    {
        MipTable     = 1,
        Payload      = 2,
        Thumbnail    = 3,   // MESH ALSO: reserved, UNWRITTEN (R5 harvests editor-side)
        VertexData   = 4,
        IndexData    = 5,
        SectionTable = 6,   // the MESH's per-section records -- NOT the container's own table
        Tangents     = 7,   // reserved, UNWRITTEN
    };

    // Discriminates which artifact kind a .arcart file holds.
    enum class ContentKind : std::uint8_t
    {
        Texture = 1,
        Mesh    = 2,
    };

    // Appends multi-byte integers little-endian, one explicit byte at a time.
    class ByteWriter
    {
    public:
        std::vector<std::byte> buf;

        void U8(std::uint8_t v) { buf.push_back(static_cast<std::byte>(v)); }
        void U16(std::uint16_t v) { for (int i = 0; i < 2; ++i) buf.push_back(ByteOf(v, 8 * i)); }
        void U32(std::uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back(ByteOf(v, 8 * i)); }
        void U64(std::uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back(ByteOf(v, 8 * i)); }
        // Floats go through their EXACT bit pattern, never a numeric conversion.
        void F32(float v) { U32(std::bit_cast<std::uint32_t>(v)); }
        void Bytes(std::span<const std::byte> s) { buf.insert(buf.end(), s.begin(), s.end()); }
        [[nodiscard]] std::size_t Size() const noexcept { return buf.size(); }

    private:
        [[nodiscard]] static constexpr std::byte ByteOf(std::uint64_t v, int shift) noexcept
        {
            return static_cast<std::byte>(static_cast<unsigned char>((v >> shift) & 0xFFu));
        }
    };

    // Bounds-checked little-endian reader over an in-memory buffer. Every accessor returns
    // false (never throws/UB) on a short read so a caller turns ANY truncation into one refusal.
    class ByteReader
    {
    public:
        ByteReader(const std::byte* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

        [[nodiscard]] bool U8(std::uint8_t& out) noexcept
        {
            if (m_pos + 1 > m_size) return false;
            out = static_cast<std::uint8_t>(m_data[m_pos]);
            m_pos += 1;
            return true;
        }
        [[nodiscard]] bool U16(std::uint16_t& out) noexcept
        {
            if (m_pos + 2 > m_size) return false;
            out = 0;
            for (int i = 0; i < 2; ++i)
                out |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(m_data[m_pos + i])) << (8 * i);
            m_pos += 2;
            return true;
        }
        [[nodiscard]] bool U32(std::uint32_t& out) noexcept
        {
            if (m_pos + 4 > m_size) return false;
            out = 0;
            for (int i = 0; i < 4; ++i)
                out |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(m_data[m_pos + i])) << (8 * i);
            m_pos += 4;
            return true;
        }
        [[nodiscard]] bool U64(std::uint64_t& out) noexcept
        {
            if (m_pos + 8 > m_size) return false;
            out = 0;
            for (int i = 0; i < 8; ++i)
                out |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(m_data[m_pos + i])) << (8 * i);
            m_pos += 8;
            return true;
        }
        [[nodiscard]] bool F32(float& out) noexcept
        {
            std::uint32_t bits = 0;
            if (!U32(bits)) return false;
            out = std::bit_cast<float>(bits);
            return true;
        }
        // Copies `count` bytes starting at ABSOLUTE offset `at` against the whole backing
        // buffer (not the sequential cursor) -- section bodies are addressed by the section
        // table's own offset/size.
        [[nodiscard]] bool Slice(std::uint64_t at, std::uint64_t count, std::vector<std::byte>& out) const
        {
            if (at > m_size || count > m_size - at) return false;
            const auto* begin = m_data + static_cast<std::size_t>(at);
            out.assign(begin, begin + static_cast<std::size_t>(count));
            return true;
        }
        [[nodiscard]] std::size_t Pos() const noexcept { return m_pos; }

    private:
        const std::byte* m_data;
        std::size_t m_pos = 0;
        std::size_t m_size;
    };

    // The KIND-AGNOSTIC COMMON PREFIX every artifact kind shares (37 bytes). contentKind is
    // the raw byte: the prefix parse does NOT gate on it, so a scan can recognise any kind.
    struct ArtifactCommonPrefix
    {
        std::uint8_t  contentKind = 0;
        Guid          sourceGuid;
        std::uint64_t sourceHash = 0;
        std::uint32_t importerVersion = 0;
    };

    // One container section table entry as read from disk.
    struct ArtifactSectionEntry
    {
        std::uint32_t tag = 0;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    // One section body to write.
    struct ArtifactSectionBody
    {
        SectionTag tag;
        std::span<const std::byte> data;
    };

    // Writes magic, kArtifactVersion, then the common prefix fields.
    ARC_ASSETS_API void WriteCommonPrefix(ByteWriter& w, const ArtifactCommonPrefix& prefix);

    // Reads magic, checks kArtifactVersion, then the common prefix fields. False on a bad
    // magic, an unrecognised version, or a short read.
    [[nodiscard]] ARC_ASSETS_API bool ParseCommonPrefix(ByteReader& r, ArtifactCommonPrefix& out) noexcept;

    // Reads the container section table (count, then count * {tag, offset, size}) at the
    // reader's cursor. `fileSize` is the whole file's length: a count that could not fit in
    // it is refused BEFORE any reserve, so a crafted count cannot ask for tens of GB.
    [[nodiscard]] ARC_ASSETS_API bool ReadSectionTable(ByteReader& r, std::size_t fileSize,
                                                       std::vector<ArtifactSectionEntry>& out);

    // Assembles a whole file: `header` (prefix + kind tail), the container section table, then
    // every body back to back in the order given.
    [[nodiscard]] ARC_ASSETS_API std::vector<std::byte> AssembleArtifact(
        const ByteWriter& header, std::span<const ArtifactSectionBody> sections);

    // Writes `bytes` to `path`, truncating. False on any IO failure.
    [[nodiscard]] ARC_ASSETS_API bool WriteArtifactFile(const std::filesystem::path& path,
                                                        std::span<const std::byte> bytes);

    // Reads a whole artifact through ReadRegularFile (regular file only, under the size cap).
    [[nodiscard]] ARC_ASSETS_API std::optional<std::vector<std::byte>> ReadArtifactBytes(
        const std::filesystem::path& path);

    // Reads at most `maxBytes` from the START of a REGULAR file -- never the whole file. A
    // shorter file reads what exists (the bounds-checked ByteReader refuses a truncated
    // prefix); a genuine IO error is nullopt.
    [[nodiscard]] ARC_ASSETS_API std::optional<std::vector<std::byte>> ReadArtifactPrefixBytes(
        const std::filesystem::path& path, std::size_t maxBytes);

    // FNV-1a 64 over raw source bytes -- the header's sourceHash fingerprint. For a mesh
    // source with external buffers the caller passes the source bytes followed by each
    // buffer's bytes in glTF declaration order.
    [[nodiscard]] ARC_ASSETS_API std::uint64_t HashSourceBytes(std::span<const std::byte> bytes) noexcept;
}
