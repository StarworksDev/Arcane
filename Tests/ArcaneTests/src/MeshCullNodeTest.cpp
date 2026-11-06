// Device-free contracts for the F3 GPU cull node.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/GpuSceneSync.hpp>
#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>

#include <unordered_map>

TEST_CASE("mesh cull: dispatch covers every row in 64-thread groups", "[meshcull][render]")
{
    CHECK(Arcane::MeshCullDispatchGroups(0u) == 0u);
    CHECK(Arcane::MeshCullDispatchGroups(1u) == 1u);
    CHECK(Arcane::MeshCullDispatchGroups(63u) == 1u);
    CHECK(Arcane::MeshCullDispatchGroups(64u) == 1u);
    CHECK(Arcane::MeshCullDispatchGroups(65u) == 2u);
}

TEST_CASE("mesh cull: registry readiness gates dispatch after reserve or apply refusal", "[meshcull][render]")
{
    Arcane::GpuSceneFrame frame;
    frame.rowCount = 65u;
    frame.cullBatches.resize(2u);
    Arcane::GpuSceneFrameReadiness readiness;

    CHECK_FALSE(Arcane::MeshCullShouldDispatch(&frame, readiness));
    readiness.registryReady = true;
    CHECK(Arcane::MeshCullShouldDispatch(&frame, readiness));
    CHECK(Arcane::MeshCullBatchCount(frame) == 2u);
    frame.rowCount = 0u;
    CHECK_FALSE(Arcane::MeshCullShouldDispatch(&frame, readiness));
}

TEST_CASE("mesh cull: frame builder feeds stable batch metadata, zeroed args, and GPU-owned visible rows", "[meshcull][render]")
{
    const Arcane::Guid opaqueMesh{ 1, 1 };
    const Arcane::Guid transparentMesh{ 2, 2 };

    Arcane::GpuSceneMirror mirror;
    mirror.batchKeys = {
        Arcane::GpuBatchKey{ opaqueMesh, 0u, Arcane::MaterialBlendMode::Opaque, false },
        Arcane::GpuBatchKey{ transparentMesh, 0u, Arcane::MaterialBlendMode::Transparent, false },
        Arcane::GpuBatchKey{ Arcane::Guid{ 3, 3 }, 0u, Arcane::MaterialBlendMode::Masked, false },
    };
    mirror.batchRowCount = { 2u, 1u, 0u };
    mirror.allocator.highWater = 3u;
    mirror.rows.resize(3u);
    mirror.rows[0].live = true; mirror.rows[0].mesh = opaqueMesh; mirror.rows[0].batch = 0u;
    mirror.rows[1].live = true; mirror.rows[1].mesh = opaqueMesh; mirror.rows[1].batch = 0u;
    mirror.rows[2].live = true; mirror.rows[2].mesh = transparentMesh; mirror.rows[2].batch = 1u;

    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    auto addMesh = [&](Arcane::Guid id)
    {
        Arcane::MeshEntry entry;
        entry.data = Arcane::BuildCube(2.0f);
        entry.bounds = Arcane::ComputeMeshBounds(entry.data);
        entry.data.sections = { Arcane::MeshSection{ "main", 0u, static_cast<std::uint32_t>(entry.data.indices.size()), 0u } };
        meshes.emplace(id, entry);
    };
    addMesh(opaqueMesh);
    addMesh(transparentMesh);
    Arcane::MeshTable table{ &meshes };

    Arcane::GpuSceneFrame frame;
    Arcane::BuildGpuSceneFrame(mirror, nullptr, Arcane::MeshSectionRanges(&table), Arcane::ViewTransform{}, frame);

    REQUIRE(frame.cullBatches.size() == 3u);
    CHECK(frame.cullBatches[0].firstOutput == 0u);
    CHECK(frame.cullBatches[0].capacity == 2u);
    CHECK(frame.cullBatches[0].argIndex == 0u);
    CHECK(frame.cullBatches[0].emitted == 1u);
    CHECK(frame.cullBatches[1].firstOutput == 2u);
    CHECK(frame.cullBatches[1].capacity == 1u);
    CHECK(frame.cullBatches[1].emitted == 0u);
    CHECK(frame.cullBatches[2].firstOutput == 3u);
    CHECK(frame.cullBatches[2].capacity == 0u);
    CHECK(frame.cullBatches[2].emitted == 0u);

    REQUIRE(frame.args.size() == 1u);
    CHECK(frame.args[0].indexNum == static_cast<std::uint32_t>(meshes[opaqueMesh].data.indices.size()));
    CHECK(frame.args[0].instanceNum == 0u);
    CHECK(frame.visibleIndices == std::vector<std::uint32_t>(3u, 0xFFFFFFFFu));
    REQUIRE(frame.transparentDraws.size() == 1u);
    CHECK(frame.transparentDraws[0].row == 2u);
}

TEST_CASE("mesh cull: rebuilding a frame zeroes args and visible indices every time", "[meshcull][render]")
{
    const Arcane::Guid mesh{ 4, 4 };
    Arcane::GpuSceneMirror mirror;
    mirror.batchKeys = { Arcane::GpuBatchKey{ mesh, 0u, Arcane::MaterialBlendMode::Opaque, false } };
    mirror.batchRowCount = { 1u };
    mirror.allocator.highWater = 1u;
    mirror.rows.resize(1u);
    mirror.rows[0].live = true;
    mirror.rows[0].mesh = mesh;
    mirror.rows[0].batch = 0u;

    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    Arcane::MeshEntry entry;
    entry.data = Arcane::BuildCube(2.0f);
    entry.bounds = Arcane::ComputeMeshBounds(entry.data);
    entry.data.sections = { Arcane::MeshSection{ "main", 0u, static_cast<std::uint32_t>(entry.data.indices.size()), 0u } };
    meshes.emplace(mesh, entry);
    Arcane::MeshTable table{ &meshes };

    Arcane::GpuSceneFrame frame;
    Arcane::BuildGpuSceneFrame(mirror, nullptr, Arcane::MeshSectionRanges(&table), Arcane::ViewTransform{}, frame);
    REQUIRE(frame.args.size() == 1u);
    REQUIRE(frame.visibleIndices.size() == 1u);
    frame.args[0].instanceNum = 77u;
    frame.visibleIndices[0] = 123u;

    Arcane::BuildGpuSceneFrame(mirror, nullptr, Arcane::MeshSectionRanges(&table), Arcane::ViewTransform{}, frame);

    REQUIRE(frame.args.size() == 1u);
    CHECK(frame.args[0].instanceNum == 0u);
    CHECK(frame.visibleIndices == std::vector<std::uint32_t>{ 0xFFFFFFFFu });
}
