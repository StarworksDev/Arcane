#pragma once

// The v1 .arcart fixtures' writers (3D-readiness Plan A Task 3, shared by Plan B Task 3).
// One definition of the fixed guids, the PNG encoder, the gradient pixels and the two fresh
// writes; ArtifactV1FixtureTest.cpp and ArtifactCodecGateTest.cpp both include this header.
// Do not add a third copy.

#include "ConstantScan.hpp"
#include "PhysicsV1Baseline.hpp"   // ReadFileBinary / WriteFileBinary

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/TextureImporter.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Guid.hpp>

#include <catch2/catch_test_macros.hpp>
#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace Arcane::Test::V1Fixture
{
    inline constexpr Arcane::Guid kTextureGuid{ 0x1111111122224333ull, 0x8444555555555555ull };
    inline constexpr Arcane::Guid kMeshGuid{ 0xAAAAAAAABBBBCCCCull, 0xDDDDEEEEFFFF0000ull };

    inline std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d = std::filesystem::temp_directory_path() / "arcane_artifact_v1_fixture_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }

    inline void CollectPngBytes(void* ctx, void* data, int size)
    {
        auto* out = static_cast<std::vector<std::byte>*>(ctx);
        const auto* p = static_cast<std::byte*>(data);
        out->insert(out->end(), p, p + size);
    }

    inline std::vector<std::byte> EncodePng(int width, int height, const std::vector<unsigned char>& rgba)
    {
        std::vector<std::byte> out;
        REQUIRE(stbi_write_png_to_func(&CollectPngBytes, &out, width, height, 4, rgba.data(), width * 4) != 0);
        return out;
    }

    inline std::vector<unsigned char> GradientPixels(int width, int height)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                unsigned char* p = px.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                p[0] = static_cast<unsigned char>((x * 255) / (width > 1 ? width - 1 : 1));
                p[1] = static_cast<unsigned char>((y * 255) / (height > 1 ? height - 1 : 1));
                p[2] = 128;
                p[3] = 255;
            }
        }
        return px;
    }

    inline Arcane::AssetPipeline::TextureMetaSettings DefaultRgba8Settings()
    {
        Arcane::AssetPipeline::TextureMetaSettings settings;
        settings.format = Arcane::AssetPipeline::TextureMetaSettings::Format::Rgba8;
        return settings;
    }

    inline std::string WriteV1TextureToTemp()
    {
        using namespace Arcane::AssetPipeline;
        const std::filesystem::path path = TempDir("texture") / "v1-texture.arcart";
        const auto png = EncodePng(17, 9, GradientPixels(17, 9));
        auto imported = ImportTexture(png, kTextureGuid, DefaultRgba8Settings());
        REQUIRE(imported.has_value());
        REQUIRE(WriteTextureArtifact(path, imported->desc, imported->payload, imported->thumbRgba));
        return ReadFileBinary(path);
    }

    inline std::string WriteV1MeshToTemp()
    {
        using namespace Arcane::AssetPipeline;
        const std::filesystem::path path = TempDir("mesh") / "v1-mesh.arcart";
        MeshArtifactDesc desc{};
        desc.contentKind = ContentKind::Mesh;
        desc.sourceGuid = kMeshGuid;
        desc.sourceHash = 0x0123456789ABCDEFULL;
        desc.importerVersion = 3;
        desc.vertexCount = 4;
        desc.indexCount = 9;
        desc.sectionCount = 2;
        desc.indexWidth = 4;
        desc.aabbMin[0] = -1.5f; desc.aabbMin[1] = -2.25f; desc.aabbMin[2] = -3.125f;
        desc.aabbMax[0] =  4.5f; desc.aabbMax[1] =  5.25f; desc.aabbMax[2] =  6.125f;
        desc.sections = { { "Metal", 0, 6, 0 }, { "Metal", 6, 3, 0 } };
        const std::vector<MeshArtifactVertex> vertices = {
            { 0,0,0,  0,1,0,  0,0 }, { 1,0,0,  0,1,0,  1,0 },
            { 1,0,1,  0,1,0,  1,1 }, { 0,0,1,  0,1,0,  0,1 },
        };
        const std::vector<std::uint32_t> indices = { 0,1,2, 0,2,3, 1,2,3 };
        REQUIRE(WriteMeshArtifact(path, desc, vertices, indices));
        return ReadFileBinary(path);
    }

    // The sourceHash the checked-in texture fixture carries, read through the pipeline's
    // prefix reader.
    inline std::uint64_t ReadV1TextureSourceHash(const std::filesystem::path& path)
    {
        const auto prefix = Arcane::AssetPipeline::ReadArtifactPrefix(path);
        REQUIRE(prefix.has_value());
        return prefix->sourceHash;
    }
}
