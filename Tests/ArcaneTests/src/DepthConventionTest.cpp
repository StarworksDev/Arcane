#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/DepthConventionMap.hpp>
#include <Arcane/Scene/DepthConvention.hpp>

#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>
#include <string>

TEST_CASE("ForwardZ clear is 1 and nearer-wins is LESS", "[nri][depth]")
{
    CHECK(Arcane::kSelectedDepthConvention == Arcane::DepthConvention::ForwardZ);
    CHECK(Arcane::DepthClearValue(Arcane::DepthConvention::ForwardZ) == 1.0f);
    CHECK(Arcane::NearerWinsOp(Arcane::DepthConvention::ForwardZ) == nri::CompareOp::LESS);
    CHECK(Arcane::NearerOrEqualOp(Arcane::DepthConvention::ForwardZ) == nri::CompareOp::LESS_EQUAL);
}

TEST_CASE("ReverseZ is the other mapping and is not selected", "[nri][depth]")
{
    CHECK(Arcane::DepthClearValue(Arcane::DepthConvention::ReverseZ) == 0.0f);
    CHECK(Arcane::NearerWinsOp(Arcane::DepthConvention::ReverseZ) == nri::CompareOp::GREATER);
    CHECK(Arcane::NearerOrEqualOp(Arcane::DepthConvention::ReverseZ) == nri::CompareOp::GREATER_EQUAL);
    CHECK(Arcane::kSelectedDepthConvention != Arcane::DepthConvention::ReverseZ);
}

static std::string ReadRepoFile(const char* relative)
{
    std::ifstream in(Arcane::Test::RepoRoot() / relative);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Removing the call and writing CompareOp::LESS again goes green on the GPU
// and red here. The two sites are the mesh pipelines and the pick-mesh pipeline.
TEST_CASE("mesh and pick compares call NearerWinsOp", "[nri][depth]")
{
    const std::string mesh = ReadRepoFile("Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/MeshNode.cpp");
    const std::string pick = ReadRepoFile("Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp");
    CHECK(mesh.find("desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);")
          != std::string::npos);
    CHECK(pick.find("desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);")
          != std::string::npos);
}
