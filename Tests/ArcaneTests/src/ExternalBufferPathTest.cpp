// Fuzz regression for ReadClientExternalBuffers (fuzz/artifact_fuzz.cpp, input in
// fuzz/regressions/artifact/gltf-empty-uri-opens-directory): a glTF buffer uri that
// resolves to a DIRECTORY ("", ".", "..") used to be opened as a file -- on
// libstdc++ that succeeds, tellg() reports LLONG_MAX, and the read buffer's
// allocation threw out of the exception-free asset path. It is a refusal (nullopt),
// the same as any unreadable referenced buffer.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ArtifactReader.hpp>
#include <Arcane/AssetPipeline/GltfSurvey.hpp>
#include <Arcane/AssetPipeline/MeshImporter.hpp>
#include <Arcane/AssetPipeline/MeshMetaSettings.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/IO/PathConfine.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>

namespace
{
    std::span<const std::byte> AsBytes(const std::string& s)
    {
        return std::as_bytes(std::span<const char>(s.data(), s.size()));
    }
}

TEST_CASE("ReadClientExternalBuffers refuses a buffer uri that names a directory", "[artifact][fuzz]")
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "arcane-external-buffer-path-test";
    std::filesystem::create_directories(dir / "sub");
    const std::filesystem::path source = dir / "mesh.gltf";

    for (const char* uri : { "", ".", "..", "sub" })
    {
        INFO("uri = \"" << uri << "\"");
        const std::string gltf = std::string(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4},{"uri":")") +
                                 uri + R"("}]})";
        std::optional<std::vector<std::vector<std::byte>>> buffers;
        REQUIRE_NOTHROW(buffers = Arcane::ReadClientExternalBuffers(AsBytes(gltf), source));
        CHECK_FALSE(buffers.has_value());
    }

    // Control: no external buffers at all is still an empty success.
    const std::string embedded = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4}]})";
    const auto none = Arcane::ReadClientExternalBuffers(AsBytes(embedded), source);
    REQUIRE(none.has_value());
    CHECK(none->empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

namespace
{
    // <tmp>/asset/{mesh.gltf (not written), ok.bin} and <tmp>/outside.bin.
    struct ConfineFixture
    {
        std::filesystem::path root = std::filesystem::temp_directory_path() / "arcane-confine-test";
        std::filesystem::path assetDir = root / "asset";
        std::filesystem::path source = assetDir / "mesh.gltf";
        std::filesystem::path outside = root / "outside.bin";

        ConfineFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(assetDir);
            std::ofstream(assetDir / "ok.bin", std::ios::binary) << "okok";
            std::ofstream(outside, std::ios::binary) << "out!";
        }
        ~ConfineFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }

        static std::string Gltf(const std::string& uri)
        {
            return std::string(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4,"uri":")") + uri + R"("}]})";
        }
    };


}

TEST_CASE("ReadClientExternalBuffers confines a buffer uri to the asset directory", "[artifact][confine]")
{
    ConfineFixture fx;

    const std::string okGltf = ConfineFixture::Gltf("ok.bin");
    const auto ok = Arcane::ReadClientExternalBuffers(AsBytes(okGltf), fx.source);
    REQUIRE(ok.has_value());
    REQUIRE(ok->size() == 1);
    CHECK((*ok)[0].size() == 4);

    const std::string absGltf = ConfineFixture::Gltf(fx.outside.generic_string());
    CHECK_FALSE(Arcane::ReadClientExternalBuffers(AsBytes(absGltf), fx.source).has_value());

    const std::string upGltf = ConfineFixture::Gltf("../outside.bin");
    CHECK_FALSE(Arcane::ReadClientExternalBuffers(AsBytes(upGltf), fx.source).has_value());

    // Rooted without a drive: not is_absolute() on Windows, so only confinement stops it.
    const std::string outsideStr = fx.outside.generic_string();
    const std::string rootedGltf = ConfineFixture::Gltf(outsideStr.size() > 2 && outsideStr[1] == ':' ? outsideStr.substr(2) : outsideStr);
    CHECK_FALSE(Arcane::ReadClientExternalBuffers(AsBytes(rootedGltf), fx.source).has_value());
}

TEST_CASE("ReadExternalBuffers confines a buffer uri to the asset directory", "[pipeline][confine]")
{
    ConfineFixture fx;

    const std::string okGltf = ConfineFixture::Gltf("ok.bin");
    const auto ok = Arcane::AssetPipeline::ReadExternalBuffers(AsBytes(okGltf), fx.source);
    REQUIRE(ok.has_value());
    REQUIRE(ok->size() == 1);
    CHECK((*ok)[0].size() == 4);

    const std::string absGltf = ConfineFixture::Gltf(fx.outside.generic_string());
    CHECK_FALSE(Arcane::AssetPipeline::ReadExternalBuffers(AsBytes(absGltf), fx.source).has_value());

    const std::string upGltf = ConfineFixture::Gltf("../outside.bin");
    CHECK_FALSE(Arcane::AssetPipeline::ReadExternalBuffers(AsBytes(upGltf), fx.source).has_value());

    // Rooted without a drive: not is_absolute() on Windows, so only confinement stops it.
    const std::string outsideStr = fx.outside.generic_string();
    const std::string rootedGltf = ConfineFixture::Gltf(outsideStr.size() > 2 && outsideStr[1] == ':' ? outsideStr.substr(2) : outsideStr);
    CHECK_FALSE(Arcane::AssetPipeline::ReadExternalBuffers(AsBytes(rootedGltf), fx.source).has_value());
}

TEST_CASE("PathIsInsideDirectory accepts a file strictly inside and refuses the rest", "[artifact][confine]")
{
    ConfineFixture fx;
    CHECK(Arcane::PathIsInsideDirectory(fx.assetDir, fx.assetDir / "ok.bin"));
    CHECK_FALSE(Arcane::PathIsInsideDirectory(fx.assetDir, fx.outside));
    CHECK_FALSE(Arcane::PathIsInsideDirectory(fx.assetDir, fx.assetDir / "."));
    CHECK_FALSE(Arcane::PathIsInsideDirectory(fx.assetDir, fx.assetDir / ".."));
    // A regular file whose lexical form escapes: reaches the relative-path logic.
    CHECK_FALSE(Arcane::PathIsInsideDirectory(fx.assetDir, fx.assetDir / ".." / "outside.bin"));
    CHECK_FALSE(Arcane::PathIsInsideDirectory(fx.assetDir, fx.assetDir / "missing.bin"));
    CHECK_FALSE(Arcane::PathIsInsideDirectory(fx.assetDir, std::filesystem::path{}));
}

// cgltf's own default file reader concatenates dir + uri, so "../outside.bin" was read by
// cgltf_load_buffers inside SurveyGltf (at discovery) and ImportMesh. Both now load buffers
// through a confining reader. Pipeline-side cases live in this file beside the reader cases
// they mirror (not beside the mesh importer tests).
TEST_CASE("SurveyGltf and ImportMesh refuse a buffer uri that escapes the asset directory", "[pipeline][confine]")
{
    ConfineFixture fx;
    std::string rootedUri = fx.outside.generic_string();
    if (rootedUri.size() > 2 && rootedUri[1] == ':') rootedUri = rootedUri.substr(2);

    const auto importRefusal = [&](const std::string& gltf) {
        return Arcane::AssetPipeline::ImportMesh(AsBytes(gltf), {}, fx.source, Arcane::Guid{},
                                                 Arcane::AssetPipeline::MeshMetaSettings{}).refusal;
    };

    // Control: the in-folder sibling loads through both (no buffer-load refusal).
    const std::string okGltf = ConfineFixture::Gltf("ok.bin");
    CHECK(Arcane::AssetPipeline::SurveyGltf(AsBytes(okGltf), fx.source).has_value());
    CHECK(importRefusal(okGltf).find("failed to load buffer") == std::string::npos);

    for (const std::string& uri : { std::string("../outside.bin"), fx.outside.generic_string(), rootedUri })
    {
        INFO("uri = " << uri);
        const std::string gltf = ConfineFixture::Gltf(uri);
        CHECK_FALSE(Arcane::AssetPipeline::SurveyGltf(AsBytes(gltf), fx.source).has_value());
        CHECK(importRefusal(gltf).find("failed to load buffer") != std::string::npos);
    }
}
