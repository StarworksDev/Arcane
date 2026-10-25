// 3D-readiness Plan A, Task 3: the v1 .arcart fixtures. A texture and a mesh
// artifact are written from fixed guids (never Guid::Generate) and checked in
// under Tests/ArcaneTests/data/artifacts/. Plan B compares a fresh write of the
// moved codec against these bytes. The capture case is hidden ([.]) and armed
// by the same environment switch as the physics baselines.
#include "Helpers/ArtifactV1Fixture.hpp"

#include <Arcane/Crypto/Crypto.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace Arcane::Test;
using Arcane::Test::V1Fixture::WriteV1MeshToTemp;
using Arcane::Test::V1Fixture::WriteV1TextureToTemp;

TEST_CASE("v1 texture and mesh artifacts match the checked-in bytes", "[artifact][baseline]")
{
    const auto root = RepoRoot() / "Tests/ArcaneTests/data/artifacts";
    for (const char* name : { "v1-texture.arcart", "v1-mesh.arcart" })
    {
        const auto path = root / name;
        REQUIRE(fs::exists(path));
        const std::string disk = ReadFileBinary(path);
        std::string expect = ReadFileBinary(root / (std::string(name) + ".sha256"));
        if (!expect.empty() && expect.back() == '\n') expect.pop_back();
        CHECK(Arcane::Crypto::Sha256Hex(disk) == expect);
        const auto fresh = (std::string(name) == "v1-texture.arcart")
            ? WriteV1TextureToTemp() : WriteV1MeshToTemp();
        CHECK(fresh == disk);
    }
}

TEST_CASE("capture v1 artifacts", "[.][artifact-baseline]")
{
    const char* arm = std::getenv("ARCANE_CAPTURE_PHYSICS_BASELINE");
    if (!arm || arm[0] != '1') return;
    const auto root = RepoRoot() / "Tests/ArcaneTests/data/artifacts";
    fs::create_directories(root);
    const std::string tex = WriteV1TextureToTemp();
    const std::string mesh = WriteV1MeshToTemp();
    WriteFileBinary(root / "v1-texture.arcart", tex);
    WriteFileBinary(root / "v1-texture.arcart.sha256", Arcane::Crypto::Sha256Hex(tex) + "\n");
    WriteFileBinary(root / "v1-mesh.arcart", mesh);
    WriteFileBinary(root / "v1-mesh.arcart.sha256", Arcane::Crypto::Sha256Hex(mesh) + "\n");
}
