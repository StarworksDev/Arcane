#include <Arcane/Assets/ArtifactCodec.hpp>

#include <Arcane/IO/ReadRegularFile.hpp>

#include <fstream>
#include <system_error>

namespace Arcane
{
    void WriteCommonPrefix(ByteWriter& w, const ArtifactCommonPrefix& prefix)
    {
        for (std::uint8_t b : kArtifactMagic) w.U8(b);
        w.U32(kArtifactVersion);
        w.U8(prefix.contentKind);
        w.U64(prefix.sourceGuid.hi);
        w.U64(prefix.sourceGuid.lo);
        w.U64(prefix.sourceHash);
        w.U32(prefix.importerVersion);
    }

    bool ParseCommonPrefix(ByteReader& r, ArtifactCommonPrefix& out) noexcept
    {
        std::uint8_t magic[4]{};
        for (std::uint8_t& b : magic)
            if (!r.U8(b)) return false;
        if (magic[0] != kArtifactMagic[0] || magic[1] != kArtifactMagic[1] ||
            magic[2] != kArtifactMagic[2] || magic[3] != kArtifactMagic[3])
            return false;

        std::uint32_t version = 0;
        if (!r.U32(version)) return false;
        if (version != kArtifactVersion) return false;

        // NO contentKind GATE HERE, deliberately: the prefix answers "whose guid is this" for
        // ANY kind. Each kind's own reader applies its fail-closed check.
        if (!r.U8(out.contentKind)) return false;
        if (!r.U64(out.sourceGuid.hi)) return false;
        if (!r.U64(out.sourceGuid.lo)) return false;
        if (!r.U64(out.sourceHash)) return false;
        return r.U32(out.importerVersion);
    }

    bool ReadSectionTable(ByteReader& r, std::size_t fileSize, std::vector<ArtifactSectionEntry>& out)
    {
        std::uint32_t sectionCount = 0;
        if (!r.U32(sectionCount)) return false;

        // Cheap sanity bound before reserving: each entry costs kSectionEntrySize bytes on
        // disk, so a count that could not possibly fit in the file is corrupt.
        if (static_cast<std::uint64_t>(sectionCount) * kSectionEntrySize > fileSize)
            return false;

        out.clear();
        out.reserve(sectionCount);
        for (std::uint32_t i = 0; i < sectionCount; ++i)
        {
            ArtifactSectionEntry e{};
            if (!r.U32(e.tag) || !r.U64(e.offset) || !r.U64(e.size)) return false;
            out.push_back(e);
        }
        return true;
    }

    std::vector<std::byte> AssembleArtifact(const ByteWriter& header, std::span<const ArtifactSectionBody> sections)
    {
        const std::uint64_t sectionTableSize = 4 + sections.size() * kSectionEntrySize;   // + sectionCount

        ByteWriter sectionTable;
        sectionTable.U32(static_cast<std::uint32_t>(sections.size()));
        std::uint64_t runningOffset = header.Size() + sectionTableSize;
        for (const ArtifactSectionBody& s : sections)
        {
            sectionTable.U32(static_cast<std::uint32_t>(s.tag));
            sectionTable.U64(runningOffset);
            sectionTable.U64(static_cast<std::uint64_t>(s.data.size()));
            runningOffset += s.data.size();
        }

        ByteWriter file;
        file.buf.reserve(static_cast<std::size_t>(runningOffset));
        file.Bytes(header.buf);
        file.Bytes(sectionTable.buf);
        for (const ArtifactSectionBody& s : sections)
            file.Bytes(s.data);
        return std::move(file.buf);
    }

    bool WriteArtifactFile(const std::filesystem::path& path, std::span<const std::byte> bytes)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ofs.flush();
        return ofs.good();
    }

    std::optional<std::vector<std::byte>> ReadArtifactBytes(const std::filesystem::path& path)
    {
        // Fuzz finding (fuzz/regressions/artifact/gltf-empty-uri-opens-directory): a path that
        // names a directory threw out of a tellg-sized read. ReadRegularFile reads only a
        // regular file under a size cap and refuses everything else with nullopt.
        return Arcane::ReadRegularFile(path);
    }

    std::optional<std::vector<std::byte>> ReadArtifactPrefixBytes(const std::filesystem::path& path,
                                                                  std::size_t maxBytes)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return std::nullopt;

        std::vector<std::byte> raw(maxBytes);
        ifs.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(maxBytes));
        if (ifs.bad()) return std::nullopt;

        const std::streamsize got = ifs.gcount();
        if (got < 0) return std::nullopt;
        raw.resize(static_cast<std::size_t>(got));
        return raw;
    }

    std::uint64_t HashSourceBytes(std::span<const std::byte> bytes) noexcept
    {
        std::uint64_t h = 14695981039346656037ULL;
        ARC_CONSTANT("math identity: the FNV-1a 64-bit prime (source hashes land on disk)")
        constexpr std::uint64_t prime = 1099511628211ULL;
        for (std::byte b : bytes)
        {
            h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b));
            h *= prime;
        }
        return h;
    }
}
