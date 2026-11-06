#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::string ReadDeviceCreationSource()
{
    std::ifstream in(Arcane::Test::RepoRoot() /
                     "Source/Client/ArcaneRender/src/Arcane/Render/DeviceCreationD3D12.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

// One process may create several D3D12 devices (hot reload, tests); the
// QueryInterface miss is identical each time, so it logs once. The guard
// wraps the warn only: CreateCommandQueue must still run on every call.
TEST_CASE("the InfoQueue1 QueryInterface warning is once per process", "[render][d3d12]")
{
    const std::string text = ReadDeviceCreationSource();
    const auto warn = text.find("ID3D12InfoQueue1 QueryInterface failed");
    REQUIRE(warn != std::string::npos);
    const std::string before = text.substr(warn > 400 ? warn - 400 : 0, warn > 400 ? 400 : warn);
    CHECK(before.find("exchange") != std::string::npos);
    CHECK(before.find("return") == std::string::npos);
    const auto queue = text.find("CreateCommandQueue", warn);
    CHECK(queue != std::string::npos);
}
