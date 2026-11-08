// Generated engine constants (3D-readiness Plan C, Task 1).
//
// build/engine-constants.lua is the one source of the bindless material table
// capacity. scripts/generate-engine-constants.{ps1,sh} write the checked-in
// BindlessCapacity.gen.hpp and bindless-capacity.hlsli from it. These cases pin
// the lua field, both outputs (byte-for-byte after folding CRLF to LF, because
// .gitattributes has no rule for *.hlsli) and the mesh.hlsl include.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Generated/BindlessCapacity.gen.hpp>

#include "Helpers/ConstantScan.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
    // ReadFileBinary is file-local in AbiDigestTest.cpp, so this file carries its
    // own copy. Every CRLF is folded to LF so a Windows checkout of an
    // unpinned-EOL file compares equal.
    std::string ReadFileBinary(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
            out.push_back(raw[i]);
        }
        return out;
    }
}

TEST_CASE("generated bindless capacity matches engine-constants.lua", "[render][constants]")
{
    const auto root = Arcane::Test::RepoRoot();
    const std::string lua = ReadFileBinary(root / "build/engine-constants.lua");
    REQUIRE(lua.find("bindlessCapacity = 256") != std::string::npos);
    const std::string hpp = ReadFileBinary(root / "Source/Client/ArcaneRender/src/Arcane/Render/Generated/BindlessCapacity.gen.hpp");
    const std::string hlsl = ReadFileBinary(root / "data/shaders/bindless-capacity.hlsli");
    CHECK(hpp.find("kBindlessCapacity = 256") != std::string::npos);
    CHECK(hlsl.find("kMeshBindlessCapacity 256") != std::string::npos);
    const std::string mesh = ReadFileBinary(root / "data/shaders/mesh.hlsl");
    CHECK(mesh.find("#include \"bindless-capacity.hlsli\"") != std::string::npos);
    CHECK(mesh.find("#define kMeshBindlessCapacity") == std::string::npos);
    CHECK(Arcane::kBindlessCapacity == 256u);
}

TEST_CASE("generated bindless capacity files equal what the generator would write", "[render][constants]")
{
    // The temp-dir compare of generate-engine-constants -Check, replicated: the
    // expected text is built from the lua value and compared with the checked-in
    // files. It never writes them.
    const auto root = Arcane::Test::RepoRoot();
    const std::string lua = ReadFileBinary(root / "build/engine-constants.lua");
    const std::string key = "bindlessCapacity = ";
    const auto at = lua.find(key);
    REQUIRE(at != std::string::npos);
    const std::string value = std::to_string(std::stoi(lua.substr(at + key.size())));

    const std::string expectedHpp =
        "#pragma once\n"
        "#include <cstdint>\n"
        "#include <Arcane/Core/Constant.hpp>\n"
        "namespace Arcane {\n"
        "    ARC_CONSTANT(\"shader/GPU contract: bindless material table capacity; source of truth is build/engine-constants.lua\")\n"
        "    inline constexpr std::uint32_t kBindlessCapacity = " + value + ";\n"
        "}\n";
    const std::string expectedHlsl =
        "#ifndef ARC_BINDLESS_CAPACITY_HLSLI\n"
        "#define ARC_BINDLESS_CAPACITY_HLSLI\n"
        "#define kMeshBindlessCapacity " + value + "\n"
        "#endif\n";
    CHECK(ReadFileBinary(root / "Source/Client/ArcaneRender/src/Arcane/Render/Generated/BindlessCapacity.gen.hpp") == expectedHpp);
    CHECK(ReadFileBinary(root / "data/shaders/bindless-capacity.hlsli") == expectedHlsl);
}
