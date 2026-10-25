// F2c Task 4: the ArcaneClient reader mirror for MESH artifacts -- Arcane::ReadClientMeshArtifact
// (ArcaneClient-local, the byte-contract PEER of Task 3's Arcane::AssetPipeline::WriteMeshArtifact/
// ReadMeshArtifact; ArcaneClient itself never links/includes ArcaneAssetPipeline -- see
// ArtifactReader.hpp's own header banner). This suite is the CROSS-LIB byte-contract test for the
// mesh half, exactly the discipline ArtifactReaderTest.cpp already established for the texture
// half: every fixture is written through the PIPELINE's real WriteMeshArtifact and read back
// through THIS reader, so a drift between the two independent implementations of the SAME on-disk
// format shows up here, not at a later consumer.
//
// Pins: the round trip (header + vertices + indices + sections, byte-exact); the three client
// refusal states each watched firing on their own (HashMismatch/VersionNewerThanEngine/Missing);
// and FindArtifactForGuid's kind-agnostic common-prefix scan -- THE SEAM this task exists for
// (ParseHeader's pre-split fail-closed non-Texture gate made a mesh artifact resolve as Missing
// forever no matter how many times it was cooked). Tagged [artifact], same tag ArtifactReaderTest.cpp
// uses (the reader half of the contract, even though this file links the pipeline lib to build its
// fixtures). Swept by both `"~[gpu]"` (the required full-suite run) and any future `"[artifact]"`-
// scoped run.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ArtifactReader.hpp>

#include <Arcane/Assets/ArtifactCodec.hpp>
#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/ArtifactStore.hpp>
#include <Arcane/Guid.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using Arcane::Guid;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_mesh_artifact_reader_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    // Same generator every AssetPipeline*Test.cpp file duplicates locally rather than sharing
    // (no test-only helper library crosses the same ArcaneClient/ArcaneAssetPipeline boundary
    // the production code itself refuses to cross).
    std::vector<std::byte> PatternBytes(std::size_t n, std::uint8_t seed)
    {
        std::vector<std::byte> out(n);
        for (std::size_t i = 0; i < n; ++i)
            out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + i * 7));
        return out;
    }

    // The source hash is the shared codec's function -- the same one the importers and the
    // runtime reader call, so this fixture cannot drift from either.
    std::uint64_t FnvOfSource(std::span<const std::byte> bytes) noexcept
    {
        return Arcane::HashSourceBytes(bytes);
    }

    // A minimal-but-valid mesh fixture: 3 zeroed vertices, indices {0,1,2}, one section
    // spanning them -- just enough for WriteMeshArtifact/ReadClientMeshArtifact to round-trip
    // cleanly, used by the refusal cases below where the SHAPE of the mesh is not the point.
    Arcane::AssetPipeline::MeshArtifactDesc MinimalMeshDesc(const Guid& guid, std::uint64_t sourceHash)
    {
        Arcane::AssetPipeline::MeshArtifactDesc desc{};
        desc.sourceGuid = guid;
        desc.sourceHash = sourceHash;
        desc.importerVersion = Arcane::kClientMeshImporterVersionMirror;
        desc.vertexCount = 3;
        desc.indexCount = 3;
        desc.sectionCount = 1;
        desc.sections = { { "Slot", 0, 3, 0 } };
        return desc;
    }

    // ---- Independent raw encoder for the hand-rolled corruption cases (final-review fix
    // wave, 2026-09-11) -- the SAME documented-layout encoder MeshArtifactFormatTest.cpp
    // carries, duplicated per this suite's no-shared-helper convention. Mirrors the
    // CONTRACT (ArtifactReader.hpp's MESH TAIL banner), not either implementation.
    void PutU8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(static_cast<std::byte>(v)); }
    void PutU16(std::vector<std::byte>& b, std::uint16_t v)
    {
        for (int i = 0; i < 2; ++i) PutU8(b, static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void PutU32(std::vector<std::byte>& b, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) PutU8(b, static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void PutU64(std::vector<std::byte>& b, std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i) PutU8(b, static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void PutF32(std::vector<std::byte>& b, float v) { PutU32(b, std::bit_cast<std::uint32_t>(v)); }
    void PutBytes(std::vector<std::byte>& b, const std::vector<std::byte>& data)
    {
        b.insert(b.end(), data.begin(), data.end());
    }

    // magic + artifactVersion + the mesh desc's fixed fields in declaration order (74 bytes),
    // stamped with `guid`, `sourceHash` and the client's own importer-version mirror so the
    // ONLY thing a hand-rolled file can be refused for is the corruption the case plants.
    std::vector<std::byte> EncodeMeshHeader(const Guid& guid, std::uint64_t sourceHash,
                                             std::uint32_t vertexCount, std::uint32_t indexCount,
                                             std::uint32_t sectionCount)
    {
        std::vector<std::byte> h;
        PutU8(h, static_cast<std::uint8_t>('A')); PutU8(h, static_cast<std::uint8_t>('R'));
        PutU8(h, static_cast<std::uint8_t>('C')); PutU8(h, static_cast<std::uint8_t>('A'));
        PutU32(h, 1);                                                          // artifactVersion
        PutU8(h, static_cast<std::uint8_t>(Arcane::AssetPipeline::ContentKind::Mesh));
        PutU64(h, guid.hi);
        PutU64(h, guid.lo);
        PutU64(h, sourceHash);
        PutU32(h, Arcane::kClientMeshImporterVersionMirror);
        PutU32(h, vertexCount);
        PutU32(h, indexCount);
        PutU32(h, sectionCount);
        PutU8(h, 4);                                                           // indexWidth
        PutF32(h, 0.0f); PutF32(h, 0.0f); PutF32(h, 0.0f);                     // aabbMin
        PutF32(h, 0.0f); PutF32(h, 0.0f); PutF32(h, 0.0f);                     // aabbMax
        return h;
    }

    void WriteFile(const fs::path& path, const std::vector<std::byte>& bytes)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        REQUIRE(ofs.good());
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(ofs.good());
    }
}

// ---- round trip (the cross-lib byte contract) -----------------------------------------------

TEST_CASE("mesh artifact: pipeline writer -> client reader round-trip", "[artifact]")
{
    const fs::path dir = TempDir("client_mesh_roundtrip");
    const Guid guid = Guid::Generate();
    const std::vector<std::byte> source = PatternBytes(64, 0x11);

    Arcane::AssetPipeline::MeshArtifactDesc desc{};
    desc.sourceGuid = guid;
    desc.sourceHash = FnvOfSource(source);   // the same FNV-1a 64 both sides use
    desc.importerVersion = Arcane::kClientMeshImporterVersionMirror;
    desc.vertexCount = 4; desc.indexCount = 9; desc.sectionCount = 2;
    desc.aabbMin[0] = -1.0f; desc.aabbMax[1] = 2.5f;
    desc.sections = { { "Metal", 0, 6, 0 }, { "Metal", 6, 3, 0 } };

    // Vertices/indices as in MeshArtifactFormatTest's own round-trip case: a 4-vertex quad
    // (two triangles sharing an edge, 9 total emitted indices across the two sections above).
    const std::vector<Arcane::AssetPipeline::MeshArtifactVertex> vertices = {
        { 0,0,0,  0,1,0,  0,0 }, { 1,0,0,  0,1,0,  1,0 },
        { 1,0,1,  0,1,0,  1,1 }, { 0,0,1,  0,1,0,  0,1 },
    };
    const std::vector<std::uint32_t> indices = { 0,1,2, 0,2,3, 1,2,3 };

    REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(dir / "m.arcart", desc,
                                                      vertices, indices));

    const Arcane::MeshArtifactReadResult r =
        Arcane::ReadClientMeshArtifact(dir / "m.arcart", source, guid);
    REQUIRE(r.refusal == Arcane::ArtifactRefusal::None);
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->vertices.size() == 4u * 8u);
    CHECK(r.mesh->indices == indices);
    REQUIRE(r.mesh->sections.size() == 2u);
    CHECK(r.mesh->sections[0].name == "Metal");
    CHECK(r.mesh->sections[1].slotIndex == 0u);
    CHECK(r.mesh->aabbMin[0] == -1.0f);
    CHECK(r.mesh->aabbMax[1] == 2.5f);

    // The interleaved layout itself: vertex 2's own 8 floats (pos/normal/uv), spot-checked --
    // proves the client's independent field-by-field decode lands in the SAME order the
    // pipeline's independent field-by-field encode wrote them in.
    REQUIRE(r.mesh->vertices.size() >= 3u * 8u + 8u);
    const float* v2 = &r.mesh->vertices[2 * 8];
    CHECK(v2[0] == 1.0f);   // px
    CHECK(v2[1] == 0.0f);   // py
    CHECK(v2[2] == 1.0f);   // pz
    CHECK(v2[6] == 1.0f);   // u
    CHECK(v2[7] == 1.0f);   // v
}

// ---- refusals, each watched firing on its own -------------------------------------------------

TEST_CASE("mesh artifact: the three client refusals", "[artifact]")
{
    // HashMismatch: the artifact is fine, the source moved underneath it.
    // VersionNewerThanEngine: importerVersion == mirror + 1.
    // Missing: expectedSourceGuid does not match the header's.
    // Mirrors ArtifactReaderTest.cpp's own refusal case structure exactly (one input varied
    // per SECTION, the rest held at a known-good baseline) -- write through the pipeline
    // writer, vary one input each time, assert the named refusal and that `mesh` stays nullopt.
    const fs::path dir = TempDir("client_mesh_refusals");
    const Guid guid = Guid::Generate();
    const std::vector<std::byte> source = PatternBytes(48, 0x22);

    const Arcane::AssetPipeline::MeshArtifactDesc desc = MinimalMeshDesc(guid, FnvOfSource(source));
    const std::vector<Arcane::AssetPipeline::MeshArtifactVertex> vertices(3);
    const std::vector<std::uint32_t> indices = { 0, 1, 2 };

    SECTION("HashMismatch: the artifact is fine, the source moved underneath it")
    {
        const fs::path path = dir / "hash_mismatch.arcart";
        REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(path, desc, vertices, indices));

        // Baseline: the SAME bytes the artifact was cooked from still validate.
        REQUIRE(Arcane::ReadClientMeshArtifact(path, source, guid).refusal
                == Arcane::ArtifactRefusal::None);

        std::vector<std::byte> editedSource = source;
        editedSource[0] = static_cast<std::byte>(static_cast<unsigned char>(editedSource[0]) ^ 0xFF);

        const auto r = Arcane::ReadClientMeshArtifact(path, editedSource, guid);
        CHECK(r.refusal == Arcane::ArtifactRefusal::HashMismatch);
        CHECK_FALSE(r.mesh.has_value());
    }

    SECTION("VersionNewerThanEngine: importerVersion == mirror + 1")
    {
        const fs::path path = dir / "version_newer.arcart";
        Arcane::AssetPipeline::MeshArtifactDesc bumped = desc;
        bumped.importerVersion = Arcane::kClientMeshImporterVersionMirror + 1;
        REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(path, bumped, vertices, indices));

        const auto r = Arcane::ReadClientMeshArtifact(path, source, guid);
        CHECK(r.refusal == Arcane::ArtifactRefusal::VersionNewerThanEngine);
        CHECK_FALSE(r.mesh.has_value());
    }

    SECTION("Missing: expectedSourceGuid does not match the header's")
    {
        const fs::path path = dir / "wrong_guid.arcart";
        REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(path, desc, vertices, indices));

        const auto r = Arcane::ReadClientMeshArtifact(path, source, Guid::Generate());
        CHECK(r.refusal == Arcane::ArtifactRefusal::Missing);
        CHECK_FALSE(r.mesh.has_value());
    }
}

// ---- structural corruptions, each refused Missing (final-review fix wave, 2026-09-11) ------
// The pipeline's ReadMeshArtifact pins the same three rules on its own side
// (MeshArtifactFormatTest.cpp); these are the CLIENT reader's own pins, so the two
// independent implementations are each held to the written contract rather than one
// being assumed to mirror the other. The first goes through the pipeline WRITER (which
// does not validate, so it will happily write a desc the reader must refuse); the other
// two are hand-rolled against the documented layout (ledger T4-8's named pins).

TEST_CASE("client mesh reader: a section whose slotIndex is >= sectionCount is refused "
          "Missing (the SLOTINDEX BOUND)", "[artifact]")
{
    const fs::path dir = TempDir("client_mesh_slotindex_bound");
    const Guid guid = Guid::Generate();
    const std::vector<std::byte> source = PatternBytes(32, 0x33);

    Arcane::AssetPipeline::MeshArtifactDesc desc = MinimalMeshDesc(guid, FnvOfSource(source));
    desc.sections = { { "", 0, 3, 7 } };   // slotIndex 7 against sectionCount 1
    const std::vector<Arcane::AssetPipeline::MeshArtifactVertex> vertices(3);
    const std::vector<std::uint32_t> indices = { 0, 1, 2 };

    const fs::path path = dir / "slotindex_bound.arcart";
    REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(path, desc, vertices, indices));   // the writer does not validate

    const auto r = Arcane::ReadClientMeshArtifact(path, source, guid);
    CHECK(r.refusal == Arcane::ArtifactRefusal::Missing);
    CHECK_FALSE(r.mesh.has_value());

    // Control: slotIndex 0 with everything else identical loads -- the refusal IS the bound.
    desc.sections = { { "", 0, 3, 0 } };
    REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(path, desc, vertices, indices));
    CHECK(Arcane::ReadClientMeshArtifact(path, source, guid).refusal == Arcane::ArtifactRefusal::None);
}

TEST_CASE("client mesh reader: a declared vertexCount with the VertexData section absent is "
          "refused Missing", "[artifact]")
{
    // Ledger T4-8: the COUNT-AGREEMENT rule on the client side. Every present section is
    // internally consistent; only the VertexData body is missing outright, so the decoded
    // vertex array (empty) disagrees with the declared count (3) maximally.
    const fs::path dir = TempDir("client_mesh_missing_vertexdata");
    const fs::path path = dir / "missing_vertexdata.arcart";
    const Guid guid = Guid::Generate();
    const std::vector<std::byte> source = PatternBytes(32, 0x44);

    std::vector<std::byte> indexDataBody;
    PutU32(indexDataBody, 0); PutU32(indexDataBody, 1); PutU32(indexDataBody, 2);

    std::vector<std::byte> sectionTableBody;
    PutU32(sectionTableBody, 1);   // 1 record
    PutU16(sectionTableBody, 0);   // name length 0
    PutU32(sectionTableBody, 0);   // indexOffset
    PutU32(sectionTableBody, 3);   // indexCount
    PutU32(sectionTableBody, 0);   // slotIndex

    std::vector<std::byte> file = EncodeMeshHeader(guid, FnvOfSource(source), /*vertexCount*/ 3, /*indexCount*/ 3, /*sectionCount*/ 1);

    constexpr std::uint32_t kContainerSectionCount = 2;   // IndexData + SectionTable ONLY
    const std::uint64_t headerSize = file.size();
    const std::uint64_t tableSize = 4 + kContainerSectionCount * 20ULL;
    std::uint64_t offset = headerSize + tableSize;
    const std::uint64_t indexOffset = offset; offset += indexDataBody.size();
    const std::uint64_t sectionTableOffset = offset; offset += sectionTableBody.size();

    PutU32(file, kContainerSectionCount);
    PutU32(file, 5u /* IndexData */);    PutU64(file, indexOffset);        PutU64(file, indexDataBody.size());
    PutU32(file, 6u /* SectionTable */); PutU64(file, sectionTableOffset); PutU64(file, sectionTableBody.size());
    REQUIRE(file.size() == headerSize + tableSize);
    PutBytes(file, indexDataBody);
    PutBytes(file, sectionTableBody);
    WriteFile(path, file);

    const auto r = Arcane::ReadClientMeshArtifact(path, source, guid);
    CHECK(r.refusal == Arcane::ArtifactRefusal::Missing);
    CHECK_FALSE(r.mesh.has_value());
}

TEST_CASE("client mesh reader: a section range past the header's indexCount is refused "
          "Missing", "[artifact]")
{
    // Ledger T4-8: the SECTION-RANGE rule on the client side. Hand-rolled with every body
    // present and sized right; the ONE section claims indexOffset 0 + indexCount 5 against
    // a 3-index buffer, which would hand Plan 2's draw path an out-of-range range.
    const fs::path dir = TempDir("client_mesh_section_range");
    const fs::path path = dir / "section_range.arcart";
    const Guid guid = Guid::Generate();
    const std::vector<std::byte> source = PatternBytes(32, 0x55);

    std::vector<std::byte> vertexDataBody;
    for (int v = 0; v < 3; ++v)
        for (int f = 0; f < 8; ++f)
            PutF32(vertexDataBody, 0.0f);

    std::vector<std::byte> indexDataBody;
    PutU32(indexDataBody, 0); PutU32(indexDataBody, 1); PutU32(indexDataBody, 2);

    std::vector<std::byte> sectionTableBody;
    PutU32(sectionTableBody, 1);   // 1 record
    PutU16(sectionTableBody, 0);   // name length 0
    PutU32(sectionTableBody, 0);   // indexOffset
    PutU32(sectionTableBody, 5);   // indexCount 5 > the header's 3 -- THE corruption
    PutU32(sectionTableBody, 0);   // slotIndex

    std::vector<std::byte> file = EncodeMeshHeader(guid, FnvOfSource(source), /*vertexCount*/ 3, /*indexCount*/ 3, /*sectionCount*/ 1);

    constexpr std::uint32_t kContainerSectionCount = 3;
    const std::uint64_t headerSize = file.size();
    const std::uint64_t tableSize = 4 + kContainerSectionCount * 20ULL;
    std::uint64_t offset = headerSize + tableSize;
    const std::uint64_t vertexOffset = offset; offset += vertexDataBody.size();
    const std::uint64_t indexOffset  = offset; offset += indexDataBody.size();
    const std::uint64_t sectionTableOffset = offset; offset += sectionTableBody.size();

    PutU32(file, kContainerSectionCount);
    PutU32(file, 4u /* VertexData */);   PutU64(file, vertexOffset);       PutU64(file, vertexDataBody.size());
    PutU32(file, 5u /* IndexData */);    PutU64(file, indexOffset);        PutU64(file, indexDataBody.size());
    PutU32(file, 6u /* SectionTable */); PutU64(file, sectionTableOffset); PutU64(file, sectionTableBody.size());
    REQUIRE(file.size() == headerSize + tableSize);
    PutBytes(file, vertexDataBody);
    PutBytes(file, indexDataBody);
    PutBytes(file, sectionTableBody);
    WriteFile(path, file);

    const auto r = Arcane::ReadClientMeshArtifact(path, source, guid);
    CHECK(r.refusal == Arcane::ArtifactRefusal::Missing);
    CHECK_FALSE(r.mesh.has_value());

    // Control: the same hand-rolled encoding with indexCount 3 loads clean, so the
    // refusal above is the range rule and not a slip in this encoder.
    const std::size_t sectionIndexCountAt = static_cast<std::size_t>(sectionTableOffset) + 4 + 2 + 4;
    REQUIRE(sectionIndexCountAt + 4 <= file.size());
    file[sectionIndexCountAt] = std::byte{ 3 };
    for (int i = 1; i < 4; ++i) file[sectionIndexCountAt + static_cast<std::size_t>(i)] = std::byte{ 0 };
    WriteFile(path, file);
    const auto control = Arcane::ReadClientMeshArtifact(path, source, guid);
    REQUIRE(control.refusal == Arcane::ArtifactRefusal::None);
    CHECK(control.mesh->sections[0].indexCount == 3u);
}

// ---- FindArtifactForGuid: the kind-agnostic common-prefix scan (THE SEAM) --------------------

TEST_CASE("mesh artifact: FindArtifactForGuid finds a MESH artifact", "[artifact]")
{
    // THE SEAM THIS TASK EXISTS FOR. Before the ParseCommonPrefix split, ParseHeader's
    // fail-closed `contentKind != Texture` check made this scan blind to every mesh artifact
    // -- so a mesh could be cooked, committed, and still resolve as Missing forever. A texture
    // artifact in the SAME store is written alongside it, so this also proves the split did
    // not make the scan kind-BLIND in the other direction: both are found, each by its own
    // guid.
    const fs::path store = TempDir("find_mesh") / "Intermediate";
    Arcane::AssetPipeline::ArtifactStore artifactStore(store);

    const Guid meshGuid = Guid::Generate();
    const Guid texGuid = Guid::Generate();

    // One mesh artifact, placed via the real store's PathFor (Artifacts/<hh>/<hex>.arcart).
    const Arcane::AssetPipeline::MeshArtifactDesc meshDesc = MinimalMeshDesc(meshGuid, 0x1122334455667788ULL);
    const std::vector<Arcane::AssetPipeline::MeshArtifactVertex> meshVertices(3);
    const std::vector<std::uint32_t> meshIndices = { 0, 1, 2 };
    const fs::path meshPath = artifactStore.PathFor(0xAAAAAAAA11111111ULL);
    fs::create_directories(meshPath.parent_path());
    REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(meshPath, meshDesc, meshVertices, meshIndices));

    // One texture artifact, placed the same way.
    Arcane::AssetPipeline::TextureArtifactDesc texDesc{};
    texDesc.contentKind = Arcane::AssetPipeline::ContentKind::Texture;
    texDesc.sourceGuid = texGuid;
    texDesc.sourceHash = 0x99887766554433ULL;
    texDesc.importerVersion = 1;
    texDesc.format = Arcane::AssetPipeline::ArtifactPixelFormat::RGBA8;
    texDesc.dimension = Arcane::AssetPipeline::ArtifactDimension::Tex2D;
    texDesc.arrayOrDepth = 1; texDesc.width = 1; texDesc.height = 1; texDesc.mipCount = 1;
    texDesc.mips = { Arcane::AssetPipeline::MipDesc{ 0, 4, 1, 1 } };
    texDesc.thumbWidth = 1; texDesc.thumbHeight = 1;
    const std::vector<std::byte> texel(4, std::byte{ 0xFF });
    const fs::path texPath = artifactStore.PathFor(0xBBBBBBBB22222222ULL);
    fs::create_directories(texPath.parent_path());
    REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(texPath, texDesc, texel, texel));

    CHECK(Arcane::FindArtifactForGuid(store, meshGuid).size() == 1u);
    CHECK(Arcane::FindArtifactForGuid(store, texGuid).size() == 1u);
    CHECK(Arcane::FindArtifactForGuid(store, Guid::Generate()).empty());
}

TEST_CASE("mesh readers refuse an index that is not less than vertexCount", "[artifact]")
{
    // Both independent readers (the client's ReadClientMeshArtifact and the pipeline's
    // ReadMeshArtifact) must refuse an index >= vertexCount: it would reach the GPU index
    // buffer and read past the vertex buffer. vertexCount == 1 here, so only index 0 is valid.
    const fs::path dir = TempDir("mesh_index_bound");
    const Guid guid = Guid::Generate();
    const std::vector<std::byte> source = PatternBytes(32, 0x66);
    const std::vector<Arcane::AssetPipeline::MeshArtifactVertex> vertices(1);

    auto descFor = [&](std::uint32_t indexCount)
    {
        Arcane::AssetPipeline::MeshArtifactDesc d{};
        d.sourceGuid = guid;
        d.sourceHash = FnvOfSource(source);
        d.importerVersion = Arcane::kClientMeshImporterVersionMirror;
        d.vertexCount = 1;
        d.indexCount = indexCount;
        d.sectionCount = indexCount ? 1u : 0u;
        if (indexCount) d.sections = { { "Slot", 0, indexCount, 0 } };
        return d;
    };

    SECTION("index 0xFFFFFFFF is refused by both readers")
    {
        const fs::path path = dir / "bad_index.arcart";
        const std::vector<std::uint32_t> indices = { 0xFFFFFFFFu };
        REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(path, descFor(1), vertices, indices));
        CHECK(Arcane::ReadClientMeshArtifact(path, source, guid).refusal != Arcane::ArtifactRefusal::None);
        CHECK_FALSE(Arcane::ReadClientMeshArtifact(path, source, guid).mesh.has_value());
        CHECK_FALSE(Arcane::AssetPipeline::ReadMeshArtifact(path).has_value());
    }

    SECTION("index == vertexCount is refused (the boundary)")
    {
        const fs::path path = dir / "boundary_index.arcart";
        const std::vector<std::uint32_t> indices = { 1u };
        REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(path, descFor(1), vertices, indices));
        CHECK(Arcane::ReadClientMeshArtifact(path, source, guid).refusal != Arcane::ArtifactRefusal::None);
        CHECK_FALSE(Arcane::AssetPipeline::ReadMeshArtifact(path).has_value());
    }

    SECTION("control: index 0 with vertexCount 1 still loads")
    {
        const fs::path path = dir / "good_index.arcart";
        const std::vector<std::uint32_t> indices = { 0u };
        REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(path, descFor(1), vertices, indices));
        CHECK(Arcane::ReadClientMeshArtifact(path, source, guid).refusal == Arcane::ArtifactRefusal::None);
        CHECK(Arcane::AssetPipeline::ReadMeshArtifact(path).has_value());
    }

    SECTION("control: indexCount 0 still loads")
    {
        const fs::path path = dir / "no_indices.arcart";
        const std::vector<std::uint32_t> indices;
        REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(path, descFor(0), vertices, indices));
        CHECK(Arcane::ReadClientMeshArtifact(path, source, guid).refusal == Arcane::ArtifactRefusal::None);
        CHECK(Arcane::AssetPipeline::ReadMeshArtifact(path).has_value());
    }
}
