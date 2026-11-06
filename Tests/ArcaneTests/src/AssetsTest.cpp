// Assets facade: the byte budget, the content root, AssetId resolution,
// bytes/json loaders, memoized failure on a missing path, and the CPU half of
// the staging repack.
//
// THE FACADE IS DEVICE-FREE. It hands out decoded pixels, bytes and json; it
// owns no GPU texture and takes no device, so everything here is device-less.
//
// COVERAGE GAP, named rather than left implicit: nothing proves a PNG reaches
// the GPU as an sRGB texture that copies back byte-true. That needs a
// texture-upload test vehicle, which does not exist yet; NriTextureCacheTest.cpp
// is the nearest neighbour and covers the cache, not the decode-to-sRGB
// contract. The decode/memo behaviour is covered through PixelsFor
// (AssetsPixelsTest.cpp).

#include <catch2/catch_test_macros.hpp>

// stb_image_write: implementation is owned by VendorSmokeTest.cpp in this
// same exe (STB_IMAGE_WRITE_IMPLEMENTATION defined there). Include header only.
#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Project/Project.hpp>

// F2b Task 8: content is artifact-only, so the anchoring test below cooks its
// real marker through the production import path (the same idiom
// AssetsPixelsTest.cpp uses -- these tests link ArcaneAssetPipeline to build
// fixtures ONLY; ArcaneClient itself never links it).
#include <Arcane/Assets/ArtifactCodec.hpp>
#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/TextureImporter.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>

// Review fix (post-Task-11): the percent-encoded-uri facade test below routes a real
// mesh cook through CookSession (the SAME orchestration arccook/the editor drive) --
// see that test's own comment for why a hand-built WriteMeshArtifact fixture cannot
// catch this class of defect.
#include <Arcane/AssetPipeline/CookSession.hpp>

// T5 s7.2: ForgetUnresolved retracts an "assets.unresolved" row, observed through
// the editor's Problems store (the AssetRegistryTest.cpp idiom).
#include <Panels/DiagnosticStore.hpp>

#include <Json.hpp>

namespace
{
    // Writes a temp JSON file; returns its path.
    std::filesystem::path WriteTestJson(const std::string& content,
                                        const char* name = "arcane-assets-test.json")
    {
        const auto path =
            std::filesystem::temp_directory_path() / name;
        std::ofstream f(path, std::ios::binary);
        REQUIRE(f.is_open());
        f << content;
        return path;
    }

    // ---- F2c Task 11: Assets::MeshArtifactFor/InvalidateMeshArtifact/CookPending --------
    // Same idiom as the texture-artifact fixtures above: link ArcaneAssetPipeline to WRITE
    // a real cooked artifact (Arcane::AssetPipeline::WriteMeshArtifact, the same production
    // writer arccook drives), then read it back purely through the ArcaneClient facade
    // under test -- Assets.cpp never links the pipeline (ArtifactReader.hpp's own banner).

    // The source hash is the shared codec's function -- the same one the importers and the
    // runtime reader call, so this fixture cannot drift from either.
    std::uint64_t FnvOfSource(std::span<const std::byte> bytes) noexcept
    {
        return Arcane::HashSourceBytes(bytes);
    }

    std::filesystem::path MeshSandboxRoot(const char* leaf)
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "arcane_assets_mesh_sandbox" / leaf;
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "Content" / "meshes", ec);
        fs::create_directories(root / "Intermediate" / "Artifacts" / "aa", ec);
        return root;
    }

    struct CookedMesh
    {
        std::filesystem::path root;
        std::filesystem::path source;    // Content/meshes/<name>.glb
        std::filesystem::path artifact;  // Intermediate/Artifacts/aa/<name>.arcart
        Arcane::Guid guid;
    };

    // Writes `sourceBytes` as a `.glb` (no external-buffer JSON parse needed -- see
    // AssetsImpl::ResolveMeshArtifact's own ".gltf only" gate) plus a matching cooked
    // artifact: a minimal-but-valid 3-vertex/1-index-triple/1-section mesh, the same
    // MeshArtifactReaderTest.cpp shape, since the SHAPE of the geometry is not the point
    // for a facade-layer resolve/memoize/refuse test.
    CookedMesh AddCookedMesh(const std::filesystem::path& root, const char* name,
                             const std::vector<std::byte>& sourceBytes)
    {
        CookedMesh cm;
        cm.root = root;
        cm.source = root / "Content" / "meshes" / (std::string(name) + ".glb");
        cm.artifact = root / "Intermediate" / "Artifacts" / "aa" / (std::string(name) + ".arcart");
        cm.guid = Arcane::Guid::Generate();

        {
            std::ofstream f(cm.source, std::ios::binary);
            REQUIRE(f.is_open());
            f.write(reinterpret_cast<const char*>(sourceBytes.data()),
                    static_cast<std::streamsize>(sourceBytes.size()));
        }

        Arcane::AssetPipeline::MeshArtifactDesc desc{};
        desc.sourceGuid = cm.guid;
        desc.sourceHash = FnvOfSource(sourceBytes);
        desc.importerVersion = Arcane::kClientMeshImporterVersionMirror;
        desc.vertexCount = 3;
        desc.indexCount = 3;
        desc.sectionCount = 1;
        desc.sections = { { "Metal", 0, 3, 0 } };
        const std::vector<Arcane::AssetPipeline::MeshArtifactVertex> vertices(3);
        const std::vector<std::uint32_t> indices = { 0, 1, 2 };
        REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(cm.artifact, desc, vertices, indices));

        return cm;
    }

    Arcane::Assets::AssetResolver MeshOneShotResolver(const CookedMesh& cm)
    {
        return [guid = cm.guid, path = cm.source](const Arcane::AssetId& a)
            -> std::optional<std::filesystem::path>
        {
            if (a.Value() == guid) return path;
            return std::nullopt;
        };
    }
}

TEST_CASE("assets: byte budget evicts least-recently-used entries and stays within budget", "[assets][budget]")
{
    // Three 40-byte files against a 100-byte budget: only two fit at a time.
    // Device-free: budget/eviction runs on the shared facade path, exercised
    // here through the bytes loader.
    const auto a = WriteTestJson(std::string(40, 'a'), "arcane-assets-budget-a.bin");
    const auto b = WriteTestJson(std::string(40, 'b'), "arcane-assets-budget-b.bin");
    const auto c = WriteTestJson(std::string(40, 'c'), "arcane-assets-budget-c.bin");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 100;
    auto assets = Arcane::Assets::Create(desc);
    REQUIRE(assets != nullptr);

    auto pa = assets->GetBytes(a);
    auto pb = assets->GetBytes(b);
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    CHECK(assets->Stats().totalBytes == 80);   // both fit

    CHECK(assets->GetBytes(a) == pa);          // touch A: B is now the LRU

    auto pc = assets->GetBytes(c);             // 120 > 100 -> evicts B (not A)
    REQUIRE(pc != nullptr);
    CHECK(assets->Stats().totalBytes == 80);
    CHECK(assets->Stats().count == 2);

    CHECK(assets->GetBytes(a) == pa);          // A survived (recently used)
    auto pb2 = assets->GetBytes(b);            // B was evicted: fresh load...
    REQUIRE(pb2 != nullptr);
    CHECK(pb2 != pb);                          // ...(pb still held, so a new alloc)
    CHECK(assets->Stats().totalBytes <= 100);  // never settles over budget

    std::remove(a.string().c_str());
    std::remove(b.string().c_str());
    std::remove(c.string().c_str());
}

TEST_CASE("assets: budget is facade-wide -- a JSON load evicts older bytes entries", "[assets][budget]")
{
    const auto a = WriteTestJson(std::string(40, 'a'), "arcane-assets-budget-d.bin");
    const auto b = WriteTestJson(std::string(40, 'b'), "arcane-assets-budget-e.bin");
    // Exactly 40 bytes of valid JSON: 7 ({"k":1}) + 33 trailing spaces
    // (legal whitespace).
    const auto j = WriteTestJson(R"({"k":1})" + std::string(33, ' '),
                                 "arcane-assets-budget-f.json");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 100;
    auto assets = Arcane::Assets::Create(desc);
    REQUIRE(assets != nullptr);

    auto pa = assets->GetBytes(a);
    auto pb = assets->GetBytes(b);
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);

    // The JSON insert pushes the FACADE total to 120: the global LRU is the
    // bytes entry A -- it lives in the OTHER cache, and is still evicted.
    auto pj = assets->GetJson(j);
    REQUIRE(pj != nullptr);
    CHECK(assets->Stats().totalBytes == 80);
    CHECK(assets->GetBytes(b) == pb);          // survived
    CHECK(assets->GetJson(j) == pj);           // survived

    auto pa2 = assets->GetBytes(a);            // A reloads: it was evicted
    REQUIRE(pa2 != nullptr);
    CHECK(pa2 != pa);
    CHECK(assets->Stats().totalBytes <= 100);

    std::remove(a.string().c_str());
    std::remove(b.string().c_str());
    std::remove(j.string().c_str());
}

TEST_CASE("assets: an asset larger than the whole budget is served but not cached", "[assets][budget]")
{
    const auto big = WriteTestJson(std::string(60, 'x'), "arcane-assets-budget-g.bin");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 50;
    auto assets = Arcane::Assets::Create(desc);
    REQUIRE(assets != nullptr);

    // The load succeeds (the caller holds the bytes) but the budget is
    // strict: the oversized entry is swept right back out, so the facade
    // never settles above budget.
    auto p1 = assets->GetBytes(big);
    REQUIRE(p1 != nullptr);
    CHECK(p1->size() == 60);
    CHECK(assets->Stats().totalBytes <= 50);

    auto p2 = assets->GetBytes(big);           // not cached: a fresh load
    REQUIRE(p2 != nullptr);
    CHECK(p2 != p1);
    CHECK(*p2 == *p1);

    std::remove(big.string().c_str());
}

TEST_CASE("assets: memoized failures are ~zero cost and survive the budget sweep", "[assets][budget]")
{
    const auto a = WriteTestJson(std::string(40, 'a'), "arcane-assets-budget-h.bin");
    const auto b = WriteTestJson(std::string(40, 'b'), "arcane-assets-budget-i.bin");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 50;
    auto assets = Arcane::Assets::Create(desc);
    REQUIRE(assets != nullptr);

    CHECK(assets->GetBytes("does/not/exist.bin") == nullptr);  // memoized, 0 bytes
    CHECK(assets->Stats().count == 1);

    auto pa = assets->GetBytes(a);
    REQUIRE(pa != nullptr);
    auto pb = assets->GetBytes(b);             // 80 > 50 -> evicts A...
    REQUIRE(pb != nullptr);

    // ...but NOT the failure memo: entries = {failure, B}, bytes = B only.
    CHECK(assets->Stats().count == 2);
    CHECK(assets->Stats().totalBytes == 40);

    std::remove(a.string().c_str());
    std::remove(b.string().c_str());
}

TEST_CASE("assets: byteBudget 0 opts out of eviction; default is 256 MiB", "[assets][budget]")
{
    // Documentation-lock on the default so a drive-by change trips a test.
    CHECK(Arcane::AssetsDesc{}.byteBudget == 256ull * 1024 * 1024);

    const auto a = WriteTestJson(std::string(40, 'a'), "arcane-assets-budget-j.bin");
    const auto b = WriteTestJson(std::string(40, 'b'), "arcane-assets-budget-k.bin");
    const auto c = WriteTestJson(std::string(40, 'c'), "arcane-assets-budget-l.bin");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 0;                       // unbounded (the legacy contract)
    auto assets = Arcane::Assets::Create(desc);
    REQUIRE(assets != nullptr);

    auto pa = assets->GetBytes(a);
    auto pb = assets->GetBytes(b);
    auto pc = assets->GetBytes(c);
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    REQUIRE(pc != nullptr);
    CHECK(assets->Stats().totalBytes == 120);  // nothing evicted
    CHECK(assets->Stats().count == 3);

    std::remove(a.string().c_str());
    std::remove(b.string().c_str());
    std::remove(c.string().c_str());
}

TEST_CASE("Assets content-root anchors relative loads", "[assets]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_contentroot";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::ofstream(root / "probe.json", std::ios::binary) << R"({"ok":true})";

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(root);

    auto doc = assets->GetJson("probe.json");   // relative -> resolves under root
    REQUIRE(doc != nullptr);
    REQUIRE((*doc)["ok"].get<bool>() == true);

    auto abs = assets->GetJson(root / "probe.json");   // absolute bypasses the root
    REQUIRE(abs != nullptr);

    fs::remove_all(root, ec);
}

TEST_CASE("Assets resolves AssetId loads through the installed resolver", "[assets]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_guid";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::ofstream(root / "mat.json", std::ios::binary) << R"({"kind":"material"})";
    std::ofstream(root / "blob.bin", std::ios::binary) << "0123456789";

    const Arcane::Guid jsonId = Arcane::Guid::Generate();
    const Arcane::Guid binId = Arcane::Guid::Generate();

    auto assets = Arcane::Assets::Create();

    SECTION("no resolver installed -> null (one warning, no crash)")
    {
        CHECK(assets->GetJson(Arcane::AssetId::FromGuid(jsonId)) == nullptr);
        CHECK(assets->GetJson(Arcane::AssetId::FromGuid(jsonId)) == nullptr);
    }

    SECTION("resolver routes ids into the same cached loaders")
    {
        assets->SetAssetResolver(
            [&](const Arcane::AssetId& id) -> std::optional<fs::path>
            {
                if (id.Value() == jsonId) return root / "mat.json";
                if (id.Value() == binId)  return root / "blob.bin";
                return std::nullopt;
            });

        auto doc = assets->GetJson(Arcane::AssetId::FromGuid(jsonId));
        REQUIRE(doc != nullptr);
        CHECK((*doc)["kind"].get<std::string>() == "material");

        // Same file by id and by path share ONE cache entry (same shared_ptr).
        auto byPath = assets->GetJson(root / "mat.json");
        CHECK(byPath.get() == doc.get());

        auto bytes = assets->GetBytes(Arcane::AssetId::FromGuid(binId));
        REQUIRE(bytes != nullptr);
        CHECK(bytes->size() == 10);

        // Unknown id -> null, memoized warn-once. Nil id -> null.
        CHECK(assets->GetJson(Arcane::AssetId::FromGuid(Arcane::Guid::Generate())) == nullptr);
        CHECK(assets->GetJson(Arcane::AssetId{}) == nullptr);

        // A fourth CHECK here -- GetTexture(AssetId) degrading to null like the
        // path overload -- went with GetTexture at ABI v15. The id-routing it
        // shared with the three above is still covered by them.
    }

    SECTION("a real Project's ResolveAsset plugs in as the resolver")
    {
        // Scaffold a minimal project whose Content/ carries a native asset with
        // an embedded id -- the exact wiring Runtime::OpenProject installs.
        const fs::path projDir = root / "proj";
        fs::create_directories(projDir / "Content");
        std::ofstream(projDir / "Game.arcproj", std::ios::binary)
            << R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 5 } })";
        std::ofstream(projDir / "Content" / "thing.json", std::ios::binary)
            << R"({ "id": "b7e0c1de-2222-4333-8444-555566667777", "payload": 7 })";

        auto proj = Arcane::Project::Open(projDir);
        REQUIRE(proj.has_value());
        assets->SetAssetResolver(
            [p = &*proj](const Arcane::AssetId& id) { return p->ResolveAsset(id); });

        const auto g = Arcane::Guid::FromString("b7e0c1de-2222-4333-8444-555566667777");
        REQUIRE(g.has_value());
        auto doc = assets->GetJson(Arcane::AssetId::FromGuid(*g));
        REQUIRE(doc != nullptr);
        CHECK((*doc)["payload"].get<int>() == 7);
    }

    fs::remove_all(root, ec);
}

// THE JOIN the two tests above never made: a registry resolver AND a content
// root, installed together exactly as Runtime::OpenProject installs them,
// against a project opened by a RELATIVE path -- i.e. `ArcaneRuntime --project
// ReferenceProject`. That combination is the hole the suite had: every other
// resolver test hands back an ABSOLUTE temp path, which ResolveAssetPath passes
// through untouched, so a second anchoring step on the id route was
// unobservable. With a relative project root it is anything but: contentRoot /
// <resolver result> yields "Proj/Content/Proj/Content/textures/marker.png" --
// the desk-observed LoadPngRgba failure that left the reference scene
// untextured on both backends.
//
// A DECOY is planted at exactly that doubled location so a regression names the
// MECHANISM rather than reporting a missing file: against the pre-fix facade
// every id-shaped load below reads the decoy (4x4, payload 13) in place of the
// real asset (2x2, payload 7).
TEST_CASE("assets: registry-resolved ids are load-ready -- the content root must not anchor them twice",
          "[assets][project]")
{
    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path sandbox = fs::temp_directory_path() / "arcane_assets_anchoring";
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);

    // A relative project root only means something against a CWD, so pin one --
    // and restore it on EVERY exit path, including a throwing REQUIRE.
    struct CwdGuard
    {
        fs::path prev;
        ~CwdGuard() { std::error_code e; fs::current_path(prev, e); }
    } const cwdGuard{ fs::current_path(ec) };
    fs::current_path(sandbox, ec);
    REQUIRE(!ec);

    const auto writePng = [](const fs::path& p, int w, int h)
    {
        std::vector<unsigned char> px((std::size_t)w * h * 4, 255);
        REQUIRE(stbi_write_png(p.string().c_str(), w, h, 4, px.data(), w * 4) != 0);
    };
    const auto writeText = [](const fs::path& p, const std::string& s)
    {
        std::ofstream f(p, std::ios::binary);
        REQUIRE(f.is_open());
        f << s;
    };

    constexpr const char* kPngGuid  = "11111111-2222-4333-8444-555555555555";
    constexpr const char* kJsonGuid = "66666666-7777-4888-8999-aaaaaaaaaaaa";

    fs::create_directories("Proj/Content/textures");
    fs::create_directories("Proj/Content/data");
    writeText("Proj/Game.arcproj",
              R"({ "formatVersion": 1, "name": "Anchoring", "engine": { "abi": 5 } })");
    writePng("Proj/Content/textures/marker.png", 2, 2);
    writeText("Proj/Content/textures/marker.png.meta",
              std::string(R"({"guid":")") + kPngGuid + R"(","version":1})");

    // Cook the REAL marker (Task 8: no artifact, no pixels). The decoy is
    // deliberately NOT cooked: if double-anchoring regressed, the id route
    // resolves the decoy's path, its bytes hash-mismatch this artifact's
    // sourceHash, and PixelsFor refuses -- the REQUIRE below still catches
    // the mechanism, now by refusal instead of by decoy dims.
    {
        const auto srcBytes = [&]() {
            std::ifstream f("Proj/Content/textures/marker.png", std::ios::binary);
            REQUIRE(f.good());
            f.seekg(0, std::ios::end);
            std::vector<std::byte> out(static_cast<std::size_t>(f.tellg()));
            f.seekg(0, std::ios::beg);
            f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
            REQUIRE(f.good());
            return out;
        }();
        const auto markerGuid = Arcane::Guid::FromString(kPngGuid);
        REQUIRE(markerGuid.has_value());
        Arcane::AssetPipeline::TextureMetaSettings settings;
        settings.format = Arcane::AssetPipeline::TextureMetaSettings::Format::Rgba8;
        auto imported = Arcane::AssetPipeline::ImportTexture(srcBytes, *markerGuid, settings);
        REQUIRE(imported.has_value());
        fs::create_directories("Proj/Intermediate/Artifacts/aa");
        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
            fs::path("Proj/Intermediate/Artifacts/aa/marker.arcart"),
            imported->desc, imported->payload, imported->thumbRgba));
    }
    const std::string realJson = std::string(R"({"id":")") + kJsonGuid + R"(","payload":7})";
    writeText("Proj/Content/data/thing.json", realJson);

    // THE DECOY, sitting at contentRoot/contentRoot/... -- precisely the path the
    // pre-fix id route built. Different dimensions and a different payload so a
    // wrong read is loud instead of merely absent.
    fs::create_directories("Proj/Content/Proj/Content/textures");
    fs::create_directories("Proj/Content/Proj/Content/data");
    writePng("Proj/Content/Proj/Content/textures/marker.png", 4, 4);
    writeText("Proj/Content/Proj/Content/textures/marker.png.meta",
              R"({"guid":"deadbeef-1111-4222-8333-444444444444","version":1})");
    writeText("Proj/Content/Proj/Content/data/thing.json",
              R"({"id":"deadbeef-5555-4666-8777-888888888888","payload":13,"decoy":true})");

    auto proj = Arcane::Project::Open(fs::path("Proj"));   // RELATIVE, like --project
    REQUIRE(proj.has_value());
    REQUIRE(proj->Root() == fs::path("Proj"));

    const auto pngId  = Arcane::Guid::FromString(kPngGuid);
    const auto jsonId = Arcane::Guid::FromString(kJsonGuid);
    REQUIRE(pngId.has_value());
    REQUIRE(jsonId.has_value());

    // The PRECONDITION the bug needs: the registry hands back a RELATIVE path
    // that is ALREADY anchored at its mount root. Asserted, not assumed -- if
    // this ever becomes absolute, double-anchoring stops being reachable and
    // everything below would pass for the wrong reason.
    const auto resolvedPng = proj->ResolveAsset(Arcane::AssetId::FromGuid(*pngId));
    REQUIRE(resolvedPng.has_value());
    CHECK_FALSE(resolvedPng->is_absolute());
    CHECK(resolvedPng->generic_string() == "Proj/Content/textures/marker.png");

    auto assets = Arcane::Assets::Create();
    REQUIRE(assets != nullptr);
    // Exactly Runtime::OpenProject's two calls, in its order.
    assets->SetContentRoot(proj->Root() / "Content");
    assets->SetAssetResolver(
        [p = &*proj](const Arcane::AssetId& id) { return p->ResolveAsset(id); });

    // PixelsFor: preview pixels through the artifact route (Task 8). The 2x2
    // marker's thumbnail is 2x2; a double-anchored resolve reads the uncooked
    // decoy's bytes, hash-mismatches the artifact, and REFUSES -- either way
    // the doubled path can no longer pass silently.
    const Arcane::PixelData* pixels = assets->PixelsFor(*pngId);
    REQUIRE(pixels != nullptr);
    CHECK(pixels->width  == 2u);
    CHECK(pixels->height == 2u);

    // GetJson(AssetId) / GetBytes(AssetId): the same ResolveId seam, device-free.
    auto doc = assets->GetJson(Arcane::AssetId::FromGuid(*jsonId));
    REQUIRE(doc != nullptr);
    CHECK((*doc)["payload"].get<int>() == 7);   // 13 == the decoy
    auto bytes = assets->GetBytes(Arcane::AssetId::FromGuid(*jsonId));
    REQUIRE(bytes != nullptr);
    CHECK(bytes->size() == realJson.size());

    // The OTHER half of the convention is untouched: a caller-supplied LOOSE
    // relative path still anchors under the content root -- and lands on the very
    // cache entry the id route just filled, one file, one entry.
    auto byPath = assets->GetJson(fs::path("data/thing.json"));
    REQUIRE(byPath != nullptr);
    CHECK(byPath.get() == doc.get());

    // THE ID ROUTE RETAINS WHAT IT SERVED, on a FRESH facade: the first call
    // resolves through the project and serves the artifact thumbnail, and a
    // second call for the same id hits the retained pixel cache rather than
    // disk. Overwrite the file in between -- a fresh miss would now
    // hash-mismatch (8x8 bytes vs the 2x2 artifact) and refuse, so an answer
    // that is still 2x2 can only be the retained entry.
    //
    // The first call was GetTexture(AssetId) until ABI v15, which proved the
    // same thing one layer up (it decoded through this same supply before it
    // ever reached its device gate). GetTexture is gone; PixelsFor is the
    // route that remains, and it is the one that was doing the work.
    auto texAssets = Arcane::Assets::Create();
    texAssets->SetContentRoot(proj->Root() / "Content");
    texAssets->SetAssetResolver(
        [p = &*proj](const Arcane::AssetId& id) { return p->ResolveAsset(id); });
    REQUIRE(texAssets->PixelsFor(*pngId) != nullptr);   // first call: resolve + decode
    writePng("Proj/Content/textures/marker.png", 8, 8);
    const Arcane::PixelData* cached = texAssets->PixelsFor(*pngId);
    REQUIRE(cached != nullptr);
    CHECK(cached->width  == 2u);   // 4 == it read the decoy; 8 == it re-read disk
    CHECK(cached->height == 2u);

    fs::current_path(cwdGuard.prev, ec);   // step out of the tree before removing it
    fs::remove_all(sandbox, ec);
}

TEST_CASE("assets: RepackStagingToRgba swizzles BGRA, skips row padding, forces opaque alpha",
          "[assets]")
{
    // A 2x2 BGRA source with a PADDED row pitch (12 bytes for 8 of pixels) --
    // the shape a mapped staging texture actually has. Per-texel byte order
    // is [B,G,R,A]. This is the CPU half of SaveTexturePng, exported exactly
    // so the contract is testable without a device.
    //
    // THIS CASE IS THE ONLY PIN ON THAT BYTE ORDER. Nothing asserts it
    // GPU-side: the graph path's own BGRA handling (NriTextureCache's Display
    // format) exercises the order end-to-end but implicitly, never by
    // assertion. Re-pinning the swizzle GPU-side, explicitly, is still owed.
    const unsigned char src[2 * 12] = {
        // row 0: pure blue, pure green | 4 padding bytes of garbage
        255, 0, 0, 7,    0, 255, 0, 7,    0xAA, 0xBB, 0xCC, 0xDD,
        // row 1: pure red, mid grey | padding
        0, 0, 255, 7,    64, 64, 64, 7,   0xAA, 0xBB, 0xCC, 0xDD,
    };
    std::vector<unsigned char> out;
    Arcane::RepackStagingToRgba(src, 12, 2, 2, /*bgraSource=*/true, out);
    REQUIRE(out.size() == 2 * 2 * 4);
    // (0,0) B=255 -> RGBA (0,0,255); (1,0) G=255 -> (0,255,0).
    CHECK(out[0] == 0);   CHECK(out[1] == 0);   CHECK(out[2] == 255); CHECK(out[3] == 255);
    CHECK(out[4] == 0);   CHECK(out[5] == 255); CHECK(out[6] == 0);   CHECK(out[7] == 255);
    // (0,1) R=255 -> (255,0,0): row 1 starts at pitch 12, so the padding
    // garbage must NOT have leaked into it.
    CHECK(out[8] == 255); CHECK(out[9] == 0);   CHECK(out[10] == 0);  CHECK(out[11] == 255);
    // (1,1): grey survives the swizzle unchanged; source alpha 7 is
    // overwritten OPAQUE -- coverage math is not a statement about the picture.
    CHECK(out[12] == 64); CHECK(out[13] == 64); CHECK(out[14] == 64); CHECK(out[15] == 255);
}

TEST_CASE("assets: RepackStagingToRgba passes RGBA rows through, still forcing opaque alpha",
          "[assets]")
{
    const unsigned char src[8] = {10, 20, 30, 0, 40, 50, 60, 128};
    std::vector<unsigned char> out;
    Arcane::RepackStagingToRgba(src, 8, 2, 1, /*bgraSource=*/false, out);
    REQUIRE(out.size() == 8);
    CHECK(out[0] == 10);  CHECK(out[1] == 20); CHECK(out[2] == 30); CHECK(out[3] == 255);
    CHECK(out[4] == 40);  CHECK(out[5] == 50); CHECK(out[6] == 60); CHECK(out[7] == 255);
}

TEST_CASE("assets: png rgba round-trip preserves every byte", "[assets][golden]")
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "arcane_png_roundtrip";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "roundtrip.png";

    // 3x2 with distinct channel values incl. alpha (alpha must survive).
    const std::uint32_t w = 3, h = 2;
    std::vector<unsigned char> src(w * h * 4);
    for (std::size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<unsigned char>((i * 37) & 0xFF);

    REQUIRE(Arcane::WritePngRgba(file, w, h, src.data()));

    std::uint32_t rw = 0, rh = 0;
    std::vector<unsigned char> back;
    REQUIRE(Arcane::LoadPngRgba(file, rw, rh, back));
    CHECK(rw == w);
    CHECK(rh == h);
    CHECK(back == src);

    std::filesystem::remove_all(dir);
}

TEST_CASE("assets: png load of a missing or corrupt file fails without throwing", "[assets][golden]")
{
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> px;
    CHECK_FALSE(Arcane::LoadPngRgba("this-file-does-not-exist-arcane.png", w, h, px));

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "arcane_png_corrupt";
    std::filesystem::create_directories(dir);
    const std::filesystem::path junk = dir / "junk.png";
    { std::ofstream f(junk, std::ios::binary); f << "not a png at all"; }
    CHECK_FALSE(Arcane::LoadPngRgba(junk, w, h, px));
    std::filesystem::remove_all(dir);
}

// ---- F2c Task 11: Assets::MeshArtifactFor / InvalidateMeshArtifact / CookPending --------
//
// ONE test case walking the whole lifecycle sequentially -- resolve, memoize, invalidate,
// a SECOND (independent) cooked mesh proving the HashMismatch refusal latches, then
// CookPending's own forward/default -- the same "one continuous narrative" shape
// AssetsPixelsTest.cpp's own "InvalidateArtifact clears a memoized ArtifactMissing
// refusal..." case already uses for the texture half of this exact lifecycle.

TEST_CASE("assets: MeshArtifactFor resolves and memoizes a cooked mesh artifact; "
          "InvalidateMeshArtifact drops the memo; a HashMismatch artifact refuses "
          "loudly and latches; CookPending forwards its probe", "[assets][mesh]")
{
    Arcane::ResetContentArtifactRefusal();   // [assets] cases' standing discipline under random order
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    const auto root = MeshSandboxRoot("resolve_memoize_invalidate_hashmismatch");

    // ---- resolve, memoize, invalidate -------------------------------------------------
    const std::vector<std::byte> sourceBytes = { std::byte{0x67}, std::byte{0x6C}, std::byte{0x54}, std::byte{0x46} };
    const auto cm = AddCookedMesh(root, "cube", sourceBytes);

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(root / "Content");
    assets->SetAssetResolver(MeshOneShotResolver(cm));

    const Arcane::LoadedClientMesh* first = assets->MeshArtifactFor(cm.guid);
    REQUIRE(first != nullptr);
    CHECK(first->vertices.size() == 3u * 8u);
    CHECK(first->indices == std::vector<std::uint32_t>{ 0, 1, 2 });
    REQUIRE(first->sections.size() == 1u);
    CHECK(first->sections[0].name == "Metal");

    // Memoized: deleting the artifact FILE between calls proves the second call never
    // re-scans disk -- if it did, the cache would find nothing and answer null instead
    // of the same pointer.
    std::filesystem::remove(cm.artifact);
    const Arcane::LoadedClientMesh* second = assets->MeshArtifactFor(cm.guid);
    CHECK(second == first);

    // InvalidateMeshArtifact drops the memo -- the NEXT call re-scans and, with the
    // artifact genuinely gone now, answers null (a fresh ArtifactMissing refusal).
    assets->InvalidateMeshArtifact(cm.guid);
    CHECK(assets->MeshArtifactFor(cm.guid) == nullptr);
    Arcane::ResetContentArtifactRefusal();   // that fresh Missing latched; clear before the next phase

    // ---- HashMismatch refuses loudly and latches --------------------------------------
    const std::vector<std::byte> originalBytes = { std::byte{0x01}, std::byte{0x02}, std::byte{0x03} };
    const auto mismatch = AddCookedMesh(root, "mismatch", originalBytes);

    // Edit the STAGED source AFTER cooking, without recooking -- the scratch scenario
    // F2b Task 6's own HashMismatch case exercises for textures, reproduced here for mesh.
    {
        std::ofstream f(mismatch.source, std::ios::binary | std::ios::trunc);
        REQUIRE(f.is_open());
        f << "edited, no longer matches the cooked sourceHash";
    }

    auto mismatchAssets = Arcane::Assets::Create();
    mismatchAssets->SetContentRoot(root / "Content");
    mismatchAssets->SetAssetResolver(MeshOneShotResolver(mismatch));

    CHECK(mismatchAssets->MeshArtifactFor(mismatch.guid) == nullptr);
    CHECK(mismatchAssets->MeshArtifactFor(mismatch.guid) == nullptr);   // memoized: no retry storm

    CHECK(Arcane::ContentArtifactRefusalObserved());
    const std::string detail = Arcane::ContentArtifactRefusalDetail();
    CHECK(detail.find("HashMismatch") != std::string::npos);
    CHECK(detail.find(mismatch.guid.ToString()) != std::string::npos);
    Arcane::ResetContentArtifactRefusal();   // leave the latch clean for later cases

    // ---- CookPending: forwards the installed probe; false with none installed --------
    const Arcane::Guid watched = Arcane::Guid::Generate();
    const Arcane::Guid other   = Arcane::Guid::Generate();
    CHECK_FALSE(mismatchAssets->CookPending(watched));   // no probe installed yet

    mismatchAssets->SetCookPendingProbe([&](const Arcane::Guid& g) { return g == watched; });
    CHECK(mismatchAssets->CookPending(watched));
    CHECK_FALSE(mismatchAssets->CookPending(other));

    // Clearing the probe (an empty std::function) restores "none installed" exactly.
    mismatchAssets->SetCookPendingProbe({});
    CHECK_FALSE(mismatchAssets->CookPending(watched));

    std::filesystem::remove_all(root);
}

// Review fix (post-Task-11): the .gltf external-buffer path, ROUTED END-TO-END through the
// REAL production cook (CookSession -- the same orchestration arccook and the editor's cook
// queue both drive) and then through THIS reader's own client-side re-derivation
// (Assets::MeshArtifactFor -> AssetsImpl::ResolveMeshArtifact -> ReadClientExternalBuffers).
// Every mesh-facade fixture above this point hand-builds its artifact via WriteMeshArtifact
// with a plain (never-percent-encoded) source path, which can NEVER catch a decode mismatch
// between the two independent readers -- ArtifactReader.cpp's ReadClientExternalBuffers and
// MeshImporter.cpp's ReadExternalBuffers (via cgltf_decode_uri) -- because nothing on the
// client side ever had a percent-escape to get wrong. This case does: the fixture's buffer
// uri needs a literal space, the shape a real DCC export commonly produces.
TEST_CASE("assets: MeshArtifactFor resolves a .gltf whose external buffer uri is "
          "percent-encoded (a space, the common artist-export shape)", "[assets][mesh]")
{
    namespace fs = std::filesystem;
    const fs::path project = fs::temp_directory_path() / "arcane_assets_mesh_percent_uri";
    std::error_code ec;
    fs::remove_all(project, ec);
    fs::create_directories(project / "Content" / "meshes", ec);

    // nested.gltf's own fixture text declares `"buffers":[{"uri":"nested.bin",...}]` --
    // rename the referenced .bin to a name that NEEDS percent-encoding (a literal space)
    // and rewrite the .gltf's own uri to the percent-encoded form, exactly what a real
    // export would carry.
    const fs::path gltfFixture = fs::path("data") / "gltf" / "nested.gltf";
    const fs::path binFixture  = fs::path("data") / "gltf" / "nested.bin";
    REQUIRE(fs::exists(gltfFixture));
    REQUIRE(fs::exists(binFixture));

    std::string gltfText;
    {
        std::ifstream in(gltfFixture, std::ios::binary);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        gltfText = ss.str();
    }
    const std::string kOriginalUri = "\"nested.bin\"";
    const std::string kEncodedUri  = "\"nested%20mesh.bin\"";   // decodes to "nested mesh.bin"
    const auto pos = gltfText.find(kOriginalUri);
    REQUIRE(pos != std::string::npos);
    gltfText.replace(pos, kOriginalUri.size(), kEncodedUri);

    const fs::path gltfPath = project / "Content" / "meshes" / "nested.gltf";
    {
        std::ofstream out(gltfPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << gltfText;
    }
    fs::copy_file(binFixture, project / "Content" / "meshes" / "nested mesh.bin", ec);
    REQUIRE_FALSE(ec);

    const Arcane::Guid meshGuid = Arcane::Guid::Generate();
    {
        nlohmann::json meta;
        meta["guid"] = meshGuid.ToString();
        meta["version"] = 1;
        fs::path metaPath = gltfPath;
        metaPath += ".meta";
        std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << meta.dump(2);
    }

    // THE PIPELINE SIDE: a real cook through CookSession, which percent-DECODES the buffer
    // uri via cgltf (MeshImporter.cpp's DecodeUriToPath -> cgltf_decode_uri) before it ever
    // opens "nested mesh.bin" -- if this refuses, the FIXTURE is wrong, not the client
    // reader under test.
    Arcane::AssetPipeline::CookSession session;
    const Arcane::AssetPipeline::CookResult result = session.CookProject(project);
    REQUIRE(result.failed == 0u);
    REQUIRE(result.cooked == 1u);

    // THE CLIENT SIDE, under test: Assets::MeshArtifactFor must independently re-derive the
    // SAME currentSourceBytes -- which means percent-decoding "nested%20mesh.bin" to
    // "nested mesh.bin" BEFORE opening it -- to hash-match what CookSession just cooked.
    // Before the fix this returns null (the literal file "nested%20mesh.bin" does not
    // exist, so the external-buffer read fails and the hash cannot match); after the fix
    // it resolves.
    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(project / "Content");
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value() == meshGuid) return gltfPath;
        return std::nullopt;
    });

    const Arcane::LoadedClientMesh* mesh = assets->MeshArtifactFor(meshGuid);
    REQUIRE(mesh != nullptr);
    CHECK_FALSE(mesh->vertices.empty());

    fs::remove_all(project, ec);
}

// Final-review fix I1 (2026-09-11): a .glb whose buffers[1] carries a `uri`. The GLB
// container only requires buffer 0 to be the BIN chunk -- any further buffer may reference
// an external file exactly like a .gltf's, and the pipeline (CookSession ->
// ReadExternalBuffers -> cgltf, which sniffs the container) hashes such a file as
// .glb ++ .bin. Before the fix the client side (a) never even called
// ReadClientExternalBuffers for a .glb (an extension gate in AssetsImpl::ResolveMeshArtifact,
// built on the false premise "a .glb carries no external buffers") and (b) could not have
// parsed one anyway (it JSON-parsed the RAW file bytes, and a GLB is not JSON) -- so it
// hashed the .glb alone and this legal file refused HashMismatch permanently. Same
// end-to-end shape as the percent-encoding case above: a REAL cook, then the facade.
TEST_CASE("assets: MeshArtifactFor resolves a .glb whose second buffer references an "
          "external .bin (the GLB container allows it; the pipeline hashes it)", "[assets][mesh]")
{
    namespace fs = std::filesystem;
    const fs::path project = fs::temp_directory_path() / "arcane_assets_mesh_glb_external_bin";
    std::error_code ec;
    fs::remove_all(project, ec);
    fs::create_directories(project / "Content" / "meshes", ec);

    const fs::path glbFixture = fs::path("data") / "gltf" / "external_bin.glb";
    const fs::path binFixture = fs::path("data") / "gltf" / "external_bin.bin";
    REQUIRE(fs::exists(glbFixture));
    REQUIRE(fs::exists(binFixture));

    const fs::path glbPath = project / "Content" / "meshes" / "external_bin.glb";
    fs::copy_file(glbFixture, glbPath, ec);
    REQUIRE_FALSE(ec);
    fs::copy_file(binFixture, project / "Content" / "meshes" / "external_bin.bin", ec);
    REQUIRE_FALSE(ec);

    const Arcane::Guid meshGuid = Arcane::Guid::Generate();
    {
        nlohmann::json meta;
        meta["guid"] = meshGuid.ToString();
        meta["version"] = 1;
        fs::path metaPath = glbPath;
        metaPath += ".meta";
        std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << meta.dump(2);
    }

    // THE PIPELINE SIDE: the real cook. If this refuses, the FIXTURE is wrong (cgltf
    // resolves buffers[1] through cgltf_load_buffers relative to the .glb's own dir).
    Arcane::AssetPipeline::CookSession session;
    const Arcane::AssetPipeline::CookResult result = session.CookProject(project);
    REQUIRE(result.failed == 0u);
    REQUIRE(result.cooked == 1u);

    // THE CLIENT SIDE, under test: the facade must locate the JSON chunk INSIDE the GLB,
    // read `buffers[1].uri`, append external_bin.bin's bytes, and hash-match the cook.
    // Before the fix this returns null (HashMismatch: the .glb hashed alone).
    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(project / "Content");
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value() == meshGuid) return glbPath;
        return std::nullopt;
    });

    const Arcane::LoadedClientMesh* mesh = assets->MeshArtifactFor(meshGuid);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->indices.size() == 6u);   // the two triangles the EXTERNAL buffer carried
    REQUIRE(mesh->sections.size() == 1u);
    CHECK(mesh->sections[0].name == "ExternalBinMat");

    fs::remove_all(project, ec);
}

// T5 s7.2 hazard #12: path-keyed memos remember contents AND failures; a rename
// A->B then a new A would be served A's old bytes without EvictPath.
TEST_CASE("assets: EvictPath serves an in-place replace fresh and retries a remembered failure", "[assets][assetops]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_assets_evict";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path a = dir / "a.json", b = dir / "b.json";
    std::ofstream(a, std::ios::binary) << R"({ "v": 1 })";

    auto assets = Arcane::Assets::Create();
    REQUIRE(assets->GetJson(a) != nullptr);
    CHECK(assets->GetJson(a)->at("v") == 1);
    std::ofstream(a, std::ios::binary | std::ios::trunc) << R"({ "v": 2 })";
    CHECK(assets->GetJson(a)->at("v") == 1);   // memoized: stale
    assets->EvictPath(a);
    CHECK(assets->GetJson(a)->at("v") == 2);

    CHECK(assets->GetJson(b) == nullptr);      // a remembered failure...
    std::ofstream(b, std::ios::binary) << R"({ "v": 3 })";
    CHECK(assets->GetJson(b) == nullptr);      // ...sticks
    assets->EvictPath(b);
    REQUIRE(assets->GetJson(b) != nullptr);
    CHECK(assets->GetJson(b)->at("v") == 3);

    fs::remove_all(dir, ec);
}

TEST_CASE("assets: ForgetUnresolved retracts exactly one assets.unresolved row", "[assets][assetops][diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();
    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([](const Arcane::AssetId&) { return std::optional<std::filesystem::path>{}; });
    const Arcane::Guid a = Arcane::Guid::Generate(), b = Arcane::Guid::Generate();
    CHECK(assets->GetJson(Arcane::AssetId::FromGuid(a)) == nullptr);
    CHECK(assets->GetJson(Arcane::AssetId::FromGuid(b)) == nullptr);
    const auto unresolved = [&]
    {
        std::vector<Arcane::Guid> ids;
        for (const auto& d : store.Snapshot())
            if (d.code == "assets.unresolved") ids.push_back(d.locator.asset);
        return ids;
    };
    REQUIRE(unresolved().size() == 2);

    assets->ForgetUnresolved(a);
    CHECK(unresolved() == std::vector<Arcane::Guid>{ b });

    store.UninstallEngineSink();
}
