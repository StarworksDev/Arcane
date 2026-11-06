// Task 4 of the review fix round: the glTF node walk is iterative. A 4096-deep
// chain of single-child nodes must bake without a stack overflow, and the
// walk body must not call itself.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/MeshImporter.hpp>
#include <Arcane/AssetPipeline/MeshMetaSettings.hpp>
#include <Arcane/Guid.hpp>

#include "Helpers/ConstantScan.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::string ReadRepoFile(const char* relative)
    {
        std::ifstream in(Arcane::Test::RepoRoot() / relative, std::ios::binary);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // A triangle (0,0,0) (1,0,0) (0,1,0), no NORMAL, as an embedded base64 buffer.
    constexpr const char* kTriangleBase64 = "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA";
}

TEST_CASE("WalkNodeTree does not call itself", "[pipeline][mesh]")
{
    const auto text = ReadRepoFile("Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/MeshImporter.cpp");
    const auto fn = text.find("void WalkNodeTree");
    REQUIRE(fn != std::string::npos);
    const auto brace = text.find('{', fn);
    const auto next = text.find("struct RawSection", brace);
    REQUIRE(brace != std::string::npos);
    REQUIRE(next != std::string::npos);
    REQUIRE(next > brace);
    const auto body = text.substr(brace + 1, next - (brace + 1));
    CHECK(body.find("WalkNodeTree(") == std::string::npos);
    CHECK(body.find("std::vector") != std::string::npos);
    CHECK(body.find("std::unordered_set") != std::string::npos);
    CHECK(body.find("std::find") == std::string::npos);
}

TEST_CASE("a 4096-deep node chain bakes parent-times-local positions", "[pipeline][mesh]")
{
    constexpr int kDepth = 4096;
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[)";
    for (int i = 0; i < kDepth; ++i)
    {
        if (i > 0) json += ",";
        json += R"({"translation":[1,0,0])";
        if (i + 1 < kDepth) json += ",\"children\":[" + std::to_string(i + 1) + "]";
        else json += R"(,"mesh":0)";
        json += "}";
    }
    json += R"(],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)";
    json += R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],)";
    json += R"("bufferViews":[{"buffer":0,"byteLength":36}],)";
    json += std::string(R"("buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,)") + kTriangleBase64 + R"("}]})";

    const auto* first = reinterpret_cast<const std::byte*>(json.data());
    const Arcane::AssetPipeline::MeshImportResult r = Arcane::AssetPipeline::ImportMesh(
        std::span<const std::byte>(first, json.size()), {}, std::filesystem::path("deep.gltf"),
        Arcane::Guid{}, Arcane::AssetPipeline::MeshMetaSettings{});

    INFO("refusal: " << r.refusal);
    REQUIRE(r.refusal.empty());
    REQUIRE(r.mesh.has_value());

    std::set<float> xs;
    for (const auto& v : r.mesh->vertices)
        xs.insert(v.px);
    const float lo = static_cast<float>(kDepth);
    const float hi = static_cast<float>(kDepth + 1);
    REQUIRE(xs.size() == 2);
    CHECK(std::fabs(*xs.begin() - lo) <= 1e-3f);
    CHECK(std::fabs(*xs.rbegin() - hi) <= 1e-3f);
    CHECK(std::fabs(r.mesh->desc.aabbMin[0] - lo) <= 1e-3f);
    CHECK(std::fabs(r.mesh->desc.aabbMax[0] - hi) <= 1e-3f);
    CHECK(std::fabs(r.mesh->desc.aabbMin[1] - 0.0f) <= 1e-3f);
    CHECK(std::fabs(r.mesh->desc.aabbMax[1] - 1.0f) <= 1e-3f);
}
