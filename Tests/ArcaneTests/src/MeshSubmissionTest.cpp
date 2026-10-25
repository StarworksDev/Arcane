// MeshSubmissionTest.cpp -- Tasks 4 and 5 of the F2a arc (3D vocabulary in
// the scene). Two groups, all CPU-only (no device, no compiler, no
// batcher, no Runtime):
//   [1] MeshCache resolves .arcmesh Guids into owned geometry (MeshEntry).
//   [2] MeshMaterialCache resolves "mesh"-kind .arcmat Guids into constants
//       (ResolvedMeshMaterial).
// Neither cache is wired into SceneRenderResolver here, so [1] and [2] drive
// each cache directly, mirroring SceneRenderResolverTest.cpp's SpriteCache
// group: a temp asset on disk plus a lambda resolver, no mocks of the asset
// layer. The former [3] group -- the CollectMeshInstances sweep's material
// chain and skip conditions -- moved to GpuSceneSyncTest.cpp when the GPU
// scene's GpuSceneSync replaced that sweep (F3 plan 1 T5); the chain itself
// is unchanged (override, if it resolves, wins; else the section slot's
// material; else white).
//
// Both caches follow SpriteMaterialCache's failure discipline, not
// SpriteCache's: a broken Guid stays OUT of the published table (memoized in
// a private `failed` set) rather than getting a visible placeholder entry --
// there is no meaningful "placeholder mesh" or "placeholder material", so
// Resolve() returning null is the correct outcome for the GPU scene's row
// staging to act on.
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialTypes.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/MeshCache.hpp>
#include <Arcane/Render/MeshMaterialCache.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <glm/glm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    fs::path MakeTempDir(const char* tag)
    {
        std::error_code ec;
        fs::path d = fs::temp_directory_path() / (std::string("arcane_mesh_submission_") + tag);
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    // A valid, minimal mesh asset -- Cube reads none of the topology fields
    // (MeshAssetTest.cpp: "a cube reads nothing -- valid under every
    // parameter combination"), so it is the cheapest way to get a resolvable
    // .arcmesh on disk. `material` rides along as ONE unnamed slot (F2c Task
    // 10: the F2a scalar `material` retired into `slots[]`) so
    // MeshEntry::slots[0] (the loaded asset's OWN default material Guid, the
    // second link in the submission sweep's resolution chain) has something
    // non-nil to assert on.
    Arcane::Guid WriteCubeMesh(const fs::path& file, const Arcane::Guid& material)
    {
        Arcane::MeshAssetData data;
        data.id     = Arcane::Guid::Generate();
        data.name   = "probe-cube";
        data.source = Arcane::MeshSource::Cube;
        data.slots  = { { std::string(), material } };
        REQUIRE(Arcane::SaveMeshAsset(file, data));
        return data.id;
    }

    // A Plane, whose vertex count (subdivisions+1)^2 -- MeshBuilder.cpp's
    // BuildPlane -- makes an edit OBSERVABLE without touching bytes, which is
    // what the Invalidate case needs: subdivisions 1 -> 4 vertices, 3 -> 16.
    Arcane::Guid WritePlaneMesh(const fs::path& file, std::uint32_t subdivisions)
    {
        Arcane::MeshAssetData data;
        data.id           = Arcane::Guid::Generate();
        data.name         = "probe-plane";
        data.source       = Arcane::MeshSource::Plane;
        data.subdivisions = subdivisions;
        REQUIRE(Arcane::SaveMeshAsset(file, data));
        return data.id;
    }

    // A structurally well-formed .arcmesh whose CONTENT ValidateMeshAsset
    // refuses (UvSphere needs rings >= 3; MeshAssetTest.cpp pins the same
    // threshold) -- SaveMeshAsset itself never validates, so this writes
    // cleanly and only BuildMeshData's internal validate call rejects it.
    Arcane::Guid WriteInvalidUvSphereMesh(const fs::path& file)
    {
        Arcane::MeshAssetData data;
        data.id       = Arcane::Guid::Generate();
        data.name     = "probe-bad-sphere";
        data.source   = Arcane::MeshSource::UvSphere;
        data.rings    = 1;    // < 3: refused
        data.segments = 32;
        REQUIRE(Arcane::SaveMeshAsset(file, data));
        return data.id;
    }

    // A BASE "mesh"-kind material carrying one saved "baseColor" Color param
    // and NO "albedo" -- matching the F2a design (a mesh material carries
    // two params, baseColor and albedo, both self-typed) minus the second
    // one. No snippet at all, either way. WriteMeshMaterialWithAlbedo below
    // is F2b Task 11's variant that also declares "albedo".
    Arcane::Guid WriteMeshMaterial(const fs::path& file, const glm::vec4& baseColor)
    {
        Arcane::MaterialAssetData data;
        data.id   = Arcane::Guid::Generate();
        data.name = "probe-material";
        data.kind = "mesh";
        data.params.emplace_back(
            "baseColor",
            Arcane::MatParamValue::MakeColor(baseColor.r, baseColor.g, baseColor.b, baseColor.a));
        REQUIRE(Arcane::SaveMaterialAsset(file, data));
        return data.id;
    }

    // WriteMeshMaterial plus a Texture-typed "albedo" param (F2b Task 11) --
    // the shape a mesh material referencing a real cooked texture authors.
    Arcane::Guid WriteMeshMaterialWithAlbedo(const fs::path& file, const glm::vec4& baseColor,
                                             const Arcane::Guid& albedo)
    {
        Arcane::MaterialAssetData data;
        data.id   = Arcane::Guid::Generate();
        data.name = "probe-material-albedo";
        data.kind = "mesh";
        data.params.emplace_back(
            "baseColor",
            Arcane::MatParamValue::MakeColor(baseColor.r, baseColor.g, baseColor.b, baseColor.a));
        data.params.emplace_back("albedo", Arcane::MatParamValue::MakeTexture(albedo));
        REQUIRE(Arcane::SaveMaterialAsset(file, data));
        return data.id;
    }

    // An INSTANCE of `parent` -- no kind, no snippet (both come from the base
    // at the end of the chain, MaterialAsset.hpp:5-9) -- with an OPTIONAL
    // sparse "baseColor" override.
    Arcane::Guid WriteMeshMaterialInstance(const fs::path& file, const Arcane::Guid& parent,
                                           std::optional<glm::vec4> baseColorOverride)
    {
        Arcane::MaterialAssetData data;
        data.id     = Arcane::Guid::Generate();
        data.name   = "probe-instance";
        data.parent = parent;
        if (baseColorOverride)
            data.params.emplace_back(
                "baseColor",
                Arcane::MatParamValue::MakeColor(baseColorOverride->r, baseColorOverride->g,
                                                 baseColorOverride->b, baseColorOverride->a));
        REQUIRE(Arcane::SaveMaterialAsset(file, data));
        return data.id;
    }
}

// -------------------------------------------------------------- [1] MeshCache

TEST_CASE("MeshCache resolves a Guid once and keeps serving that entry", "[mesh]")
{
    const fs::path dir      = MakeTempDir("mesh_resolve_once");
    const fs::path file     = dir / "probe.arcmesh";
    const Arcane::Guid material = Arcane::Guid::Generate();
    const Arcane::Guid id       = WriteCubeMesh(file, material);

    int resolveCalls = 0;
    Arcane::MeshCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return file;
    };
    Arcane::MeshCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().size() == 1);
    REQUIRE(resolveCalls == 1);

    const Arcane::MeshEntry& entry = cache.Table().at(id);
    CHECK(entry.data.vertices.size() == Arcane::BuildCube(1.0f).vertices.size());
    CHECK(entry.bounds.min == glm::vec3(-0.5f, -0.5f, -0.5f));
    CHECK(entry.bounds.max == glm::vec3(0.5f, 0.5f, 0.5f));

    // MeshEntry::slots is what the submission sweep reads the mesh's OWN
    // default material Guid from -- copied off the loaded .arcmesh at Request
    // time so the chain never needs a second file read nor a cache pointer.
    // WriteCubeMesh above wrote `material` as ONE unnamed slot, so slots[0]
    // is where it round-trips to.
    REQUIRE(entry.slots.size() == 1u);
    CHECK(entry.slots[0].material == material);

    // Per-frame sweeps call Request for every referenced Guid every frame;
    // the whole point is that this is free after the first one. Pinned BY
    // ADDRESS, not just by resolveCalls: the same MeshData's address staying
    // put is exactly what proves no rebuild happened (the brief's own
    // instrument for this case) -- a std::unordered_map never relocates an
    // existing element's storage on insertion of OTHER keys, and Request's
    // table.contains(id) guard means THIS key is never re-inserted at all.
    const Arcane::MeshData* firstAddr = &cache.Table().at(id).data;
    for (int i = 0; i < 5; ++i)
        cache.Request(id);
    const Arcane::MeshData* secondAddr = &cache.Table().at(id).data;
    CHECK(firstAddr == secondAddr);
    CHECK(resolveCalls == 1);

    // A nil Guid is not an error and must not create an entry (MeshRenderer's
    // default `mesh` is nil -- an entity with no geometry assigned yet).
    cache.Request(Arcane::Guid::Nil());
    CHECK(cache.Table().size() == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshCache keeps an unresolvable Guid out of the table and warns once", "[mesh]")
{
    const Arcane::Guid id = Arcane::Guid::Generate();

    int resolveCalls = 0;
    Arcane::MeshCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return std::nullopt;   // not in the asset registry
    };
    Arcane::MeshCache cache(std::move(s));

    cache.Request(id);
    // Unlike SpriteCache, a failure must NOT land in the published table --
    // there is no placeholder mesh, so a broken Guid must resolve to nullptr
    // through MeshTable::Resolve so GpuSceneSync can skip the entity.
    CHECK_FALSE(cache.Table().contains(id));

    // Memoized: a per-frame sweep must not re-hit the filesystem (nor warn
    // again) for a Guid already known to be unresolvable.
    cache.Request(id);
    cache.Request(id);
    CHECK(resolveCalls == 1);
}

TEST_CASE("MeshCache keeps an .arcmesh that fails validation out of the table", "[mesh]")
{
    const fs::path dir  = MakeTempDir("mesh_invalid");
    const fs::path file = dir / "bad.arcmesh";
    const Arcane::Guid id = WriteInvalidUvSphereMesh(file);

    int resolveCalls = 0;
    Arcane::MeshCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return file;
    };
    Arcane::MeshCache cache(std::move(s));

    cache.Request(id);
    // The file loads fine (it is well-formed JSON) but BuildMeshData refuses
    // it -- nullopt exactly when ValidateMeshAsset does (MeshAsset.hpp's own
    // contract) -- so this is a DIFFERENT failure path than "not in the
    // registry" and must land the same way: out of the table, memoized.
    CHECK_FALSE(cache.Table().contains(id));

    cache.Request(id);
    CHECK(resolveCalls == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshCache::Invalidate forces the next Request to re-read the file", "[mesh]")
{
    const fs::path dir  = MakeTempDir("mesh_invalidate");
    const fs::path file = dir / "probe.arcmesh";
    const Arcane::Guid id = WritePlaneMesh(file, /*subdivisions=*/1);   // 2x2 = 4 vertices

    Arcane::MeshCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    Arcane::MeshCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().at(id).data.vertices.size() == 4);

    // The mesh editor re-saves the asset with denser topology. Without the
    // invalidation the viewport keeps drawing the PRE-edit geometry forever --
    // Request is a once-per-Guid cache, so this hook IS the mechanism that
    // makes an edit show up.
    Arcane::MeshAssetData edited;
    edited.id           = id;
    edited.name         = "probe-plane";
    edited.source       = Arcane::MeshSource::Plane;
    edited.subdivisions = 3;   // 4x4 = 16 vertices
    REQUIRE(Arcane::SaveMeshAsset(file, edited));

    cache.Request(id);
    CHECK(cache.Table().at(id).data.vertices.size() == 4);   // still stale: cached

    cache.Invalidate(id);
    CHECK_FALSE(cache.Table().contains(id));

    cache.Request(id);
    CHECK(cache.Table().at(id).data.vertices.size() == 16);

    // Clear is the project-switch path: a Guid resolves through the CURRENT
    // project's registry, so nothing may survive the switch.
    cache.Clear();
    CHECK(cache.Table().empty());

    std::error_code ec; fs::remove_all(dir, ec);
}

// -------------------------------------------------------- [2] MeshMaterialCache

TEST_CASE("MeshMaterialCache resolves a Guid once and keeps serving that entry",
          "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_resolve_once");
    const fs::path file = dir / "probe.arcmat";
    const glm::vec4 color(0.25f, 0.5f, 0.75f, 1.0f);
    const Arcane::Guid id = WriteMeshMaterial(file, color);

    int resolveCalls = 0;
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return file;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().size() == 1);
    REQUIRE(resolveCalls == 1);
    CHECK(cache.Table().at(id).baseColor == color);

    // Per-frame sweeps call Request for every referenced Guid every frame;
    // this proves the second call touches neither the filesystem nor the
    // chain-walk logic again (the resolveCalls counter is this cache's stand-
    // in for MeshCache's address pin: there is no heap allocation inside
    // ResolvedMeshMaterial to pin an address on, so "the resolver was
    // consulted exactly once" is the property that proves no re-resolve
    // happened).
    for (int i = 0; i < 5; ++i)
        cache.Request(id);
    CHECK(resolveCalls == 1);

    // A nil Guid is not an error and must not create an entry (a nil
    // materialOverride/mesh-default is the documented "fall through" case,
    // not a broken reference).
    cache.Request(Arcane::Guid::Nil());
    CHECK(cache.Table().size() == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache keeps an unresolvable Guid out of the table and warns once",
          "[mesh][material]")
{
    const Arcane::Guid id = Arcane::Guid::Generate();

    int resolveCalls = 0;
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return std::nullopt;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    CHECK_FALSE(cache.Table().contains(id));

    cache.Request(id);
    cache.Request(id);
    CHECK(resolveCalls == 1);
}

TEST_CASE("MeshMaterialCache refuses a material whose chain base is not \"mesh\"-kind",
          "[mesh][material]")
{
    // The authoring mistake with the WORST symptom: a MeshRenderer's
    // materialOverride pointed at a perfectly valid sprite or fullscreen
    // .arcmat. It used to resolve silently -- fullscreen materials declare no
    // baseColor at all, so the mesh drew plain WHITE, which is exactly what a
    // BROKEN reference looks like too. The refusal makes the two
    // distinguishable in the log.
    const fs::path dir = MakeTempDir("matl_wrong_kind");

    const auto writeKinded = [&](const char* kind, const fs::path& file)
    {
        Arcane::MaterialAssetData data;
        data.id   = Arcane::Guid::Generate();
        data.name = "probe-wrong-kind";
        data.kind = kind;
        // WITH a baseColor, deliberately: refusal must come from the KIND, not
        // from the material happening to lack the param this cache reads.
        data.params.emplace_back("baseColor",
                                 Arcane::MatParamValue::MakeColor(0.9f, 0.1f, 0.1f, 1.0f));
        REQUIRE(Arcane::SaveMaterialAsset(file, data));
        return data.id;
    };

    const fs::path fullscreenFile = dir / "fullscreen.arcmat";
    const fs::path spriteFile     = dir / "sprite.arcmat";
    const fs::path meshFile       = dir / "mesh.arcmat";
    const Arcane::Guid fullscreenId = writeKinded("fullscreen", fullscreenFile);
    const Arcane::Guid spriteId     = writeKinded("sprite", spriteFile);
    const glm::vec4 meshColor(0.2f, 0.4f, 0.6f, 1.0f);
    const Arcane::Guid meshId       = WriteMeshMaterial(meshFile, meshColor);

    // An INSTANCE of the fullscreen base: the gate reads the chain's BASE, not
    // the leaf, because an instance carries no kind of its own. This is the
    // case a leaf-only check would wave through.
    const fs::path instFile = dir / "inst.arcmat";
    const Arcane::Guid instId =
        WriteMeshMaterialInstance(instFile, fullscreenId, glm::vec4(0.1f, 0.9f, 0.1f, 1.0f));

    std::unordered_map<Arcane::Guid, fs::path> registry{
        { fullscreenId, fullscreenFile }, { spriteId, spriteFile },
        { meshId, meshFile }, { instId, instFile } };
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid& g) -> std::optional<fs::path>
    {
        auto it = registry.find(g);
        return it != registry.end() ? std::optional<fs::path>(it->second) : std::nullopt;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(fullscreenId);
    cache.Request(spriteId);
    cache.Request(instId);
    CHECK_FALSE(cache.Table().contains(fullscreenId));
    CHECK_FALSE(cache.Table().contains(spriteId));
    CHECK_FALSE(cache.Table().contains(instId));

    // The control that keeps all three above from being vacuous: the SAME
    // cache, the same call, a "mesh"-kind base -- resolves, with its colour.
    cache.Request(meshId);
    REQUIRE(cache.Table().contains(meshId));
    CHECK(cache.Table().at(meshId).baseColor == meshColor);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache keeps a malformed .arcmat out of the table", "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_malformed");
    const fs::path file = dir / "junk.arcmat";
    { std::ofstream f(file); f << "this is not json"; }
    const Arcane::Guid id = Arcane::Guid::Generate();

    int resolveCalls = 0;
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return file;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    CHECK_FALSE(cache.Table().contains(id));
    cache.Request(id);
    CHECK(resolveCalls == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache detects a parent-chain cycle and keeps it out of the table",
          "[mesh][material]")
{
    // A <-> B: neither ever reaches a base. WriteMeshMaterialInstance can't
    // express this directly (it needs the OTHER Guid before that file
    // exists), so this writes the pair by hand.
    const fs::path dir = MakeTempDir("matl_cycle");
    const fs::path fileA = dir / "a.arcmat";
    const fs::path fileB = dir / "b.arcmat";
    const Arcane::Guid idA = Arcane::Guid::Generate();
    const Arcane::Guid idB = Arcane::Guid::Generate();

    Arcane::MaterialAssetData a; a.id = idA; a.name = "a"; a.parent = idB;
    Arcane::MaterialAssetData b; b.id = idB; b.name = "b"; b.parent = idA;
    REQUIRE(Arcane::SaveMaterialAsset(fileA, a));
    REQUIRE(Arcane::SaveMaterialAsset(fileB, b));

    std::unordered_map<Arcane::Guid, fs::path> registry{ { idA, fileA }, { idB, fileB } };
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid& g) -> std::optional<fs::path>
    {
        auto it = registry.find(g);
        return it != registry.end() ? std::optional<fs::path>(it->second) : std::nullopt;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(idA);
    CHECK_FALSE(cache.Table().contains(idA));

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache resolves an instance's baseColor override over its base's",
          "[mesh][material]")
{
    const fs::path dir     = MakeTempDir("matl_instance_override");
    const fs::path baseFile = dir / "base.arcmat";
    const fs::path instFile = dir / "inst.arcmat";
    const Arcane::Guid baseId = WriteMeshMaterial(baseFile, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    const Arcane::Guid instId =
        WriteMeshMaterialInstance(instFile, baseId, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));

    std::unordered_map<Arcane::Guid, fs::path> registry{ { baseId, baseFile }, { instId, instFile } };
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid& g) -> std::optional<fs::path>
    {
        auto it = registry.find(g);
        return it != registry.end() ? std::optional<fs::path>(it->second) : std::nullopt;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(instId);
    REQUIRE(cache.Table().contains(instId));
    CHECK(cache.Table().at(instId).baseColor == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache instance inherits its base's baseColor when it overrides nothing",
          "[mesh][material]")
{
    const fs::path dir      = MakeTempDir("matl_instance_inherit");
    const fs::path baseFile = dir / "base.arcmat";
    const fs::path instFile = dir / "inst.arcmat";
    const Arcane::Guid baseId = WriteMeshMaterial(baseFile, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    const Arcane::Guid instId = WriteMeshMaterialInstance(instFile, baseId, std::nullopt);

    std::unordered_map<Arcane::Guid, fs::path> registry{ { baseId, baseFile }, { instId, instFile } };
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid& g) -> std::optional<fs::path>
    {
        auto it = registry.find(g);
        return it != registry.end() ? std::optional<fs::path>(it->second) : std::nullopt;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(instId);
    REQUIRE(cache.Table().contains(instId));
    CHECK(cache.Table().at(instId).baseColor == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache::Invalidate forces the next Request to re-read the file",
          "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_invalidate");
    const fs::path file = dir / "probe.arcmat";
    const Arcane::Guid id = WriteMeshMaterial(file, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().at(id).baseColor == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    // The material editor re-saves the asset with a new colour. Without the
    // invalidation the scene keeps drawing the PRE-edit colour forever.
    Arcane::MaterialAssetData edited;
    edited.id   = id;
    edited.name = "probe-material";
    edited.kind = "mesh";
    edited.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(0.0f, 1.0f, 0.0f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(file, edited));

    cache.Request(id);
    CHECK(cache.Table().at(id).baseColor == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));   // still stale

    cache.Invalidate(id);
    CHECK_FALSE(cache.Table().contains(id));

    cache.Request(id);
    CHECK(cache.Table().at(id).baseColor == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));

    cache.Clear();
    CHECK(cache.Table().empty());

    std::error_code ec; fs::remove_all(dir, ec);
}

// ---- F2b Task 11: albedo -> ResolvedMeshMaterial::albedo / materialSlot ----

TEST_CASE("MeshMaterialCache resolves a declared albedo Guid into ResolvedMeshMaterial::albedo",
          "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_albedo_roundtrip");
    const fs::path file = dir / "probe.arcmat";
    const Arcane::Guid albedoGuid = Arcane::Guid::Generate();
    const Arcane::Guid id = WriteMeshMaterialWithAlbedo(file, glm::vec4(1.0f), albedoGuid);

    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    // Deliberately left unset (resolveAlbedoSlot): a device-less cache --
    // every CPU test, and this class's own header-stated "CONSTANTS ONLY"
    // promise -- must still round-trip the Guid itself with no device seam
    // at all.
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().contains(id));
    CHECK(cache.Table().at(id).albedo == albedoGuid);
    // The flat baseColor path: kInvalidSlot's own numeric value
    // (BindlessTable.hpp), restated as a literal here for the same reason
    // SceneResources.hpp's own default does -- this file stays device-free
    // by design (this group's own header banner), so it does not pull in
    // <NRI.h> for one constant.
    CHECK(cache.Table().at(id).materialSlot == 0xFFFFFFFFu);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache leaves albedo nil and materialSlot at kInvalidSlot when no "
          "albedo param is declared",
          "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_albedo_nil");
    const fs::path file = dir / "probe.arcmat";
    const Arcane::Guid id = WriteMeshMaterial(file, glm::vec4(1.0f));   // no "albedo" param at all

    bool callbackInvoked = false;
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    // Installed and would happily answer -- proves the skip is keyed on "no
    // albedo declared", not "no callback installed".
    s.resolveAlbedoSlot = [&](const Arcane::Guid&) -> std::uint32_t
    {
        callbackInvoked = true;
        return 7u;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().contains(id));
    CHECK_FALSE(cache.Table().at(id).albedo.IsValid());
    CHECK(cache.Table().at(id).materialSlot == 0xFFFFFFFFu);
    CHECK_FALSE(callbackInvoked);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache resolves albedo into a bindless slot through the injected "
          "device seam",
          "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_albedo_slot");
    const fs::path file = dir / "probe.arcmat";
    const Arcane::Guid albedoGuid = Arcane::Guid::Generate();
    const Arcane::Guid id = WriteMeshMaterialWithAlbedo(file, glm::vec4(1.0f), albedoGuid);

    int slotCalls = 0;
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    s.resolveAlbedoSlot = [&](const Arcane::Guid& g) -> std::uint32_t
    {
        ++slotCalls;
        CHECK(g == albedoGuid);
        return 3u;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().contains(id));
    CHECK(cache.Table().at(id).materialSlot == 3u);
    CHECK(slotCalls == 1);

    // Per-frame sweeps call Request every frame; the memoization guard at
    // the top of Request (id already in `table`) means the seam is not
    // re-consulted either, mirroring resolveCalls' own proof above.
    cache.Request(id);
    CHECK(slotCalls == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache gives its slot reference back and re-resolves a slot the device released",
          "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_albedo_release");
    const fs::path file = dir / "probe.arcmat";
    const Arcane::Guid albedoGuid = Arcane::Guid::Generate();
    const Arcane::Guid id = WriteMeshMaterialWithAlbedo(file, glm::vec4(1.0f), albedoGuid);

    int acquired = 0;
    int released = 0;
    Arcane::Guid lastReleased{};
    std::uint32_t nextSlot = 3u;
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    s.resolveAlbedoSlot = [&](const Arcane::Guid&) -> std::uint32_t { ++acquired; return nextSlot; };
    s.releaseAlbedoSlot = [&](const Arcane::Guid& g) { ++released; lastReleased = g; };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().at(id).materialSlot == 3u);
    CHECK(acquired == 1);

    // The device released slot 3 (a texture was replaced): the material falls back to the
    // flat path at once, and the next Request re-resolves ONLY its albedo.
    cache.ResetAlbedoSlot(3u);
    CHECK(cache.Table().at(id).materialSlot == 0xFFFFFFFFu);
    nextSlot = 8u;
    cache.Request(id);
    CHECK(cache.Table().at(id).materialSlot == 8u);
    CHECK(acquired == 2);
    cache.Request(id);   // landed: no further consultation
    CHECK(acquired == 2);

    // Invalidate drops the material's reference on the slot it holds.
    cache.Invalidate(id);
    CHECK(released == 1);
    CHECK(lastReleased == albedoGuid);

    cache.Request(id);
    REQUIRE(cache.Table().at(id).materialSlot == 8u);
    cache.Clear();
    CHECK(released == 2);

    // A material whose slot the device already released holds no reference to return.
    cache.Request(id);
    cache.ResetAlbedoSlot(8u);
    cache.Clear();
    CHECK(released == 2);

    std::error_code ec; fs::remove_all(dir, ec);
}
