// Asset file operations (spec 2026-09-30 s7.1/s7.3/s7.4): the PURE planner over
// fake facts, then the executor and the undoable commands over a real TempDir.
#include <catch2/catch_test_macros.hpp>

#include "Project/AssetFileOps.hpp"
#include "Helpers/AssetFileOpsFakes.hpp"
#include "Helpers/AssetOpsFixture.hpp"
#include "Project/MeshImportWave.hpp"
#include "Helpers/TestTypeContext.hpp"
#include "Panels/AssetReferenceIndex.hpp"
#include "Project/ContentDiscovery.hpp"

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Project/MountTable.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    // The planner never touches a disk: this root does not exist; `exists` answers
    // from the set (keys are lexically_normal().generic_string()).
    struct PlanWorld
    {
        fs::path content = "C:/fake/proj/Content";
        fs::path diag    = "C:/fake/proj/Saved/Diagnostics";
        std::vector<std::pair<Arcane::Guid, std::string>> registry;
        std::set<std::string> files;
        std::vector<AssetOpFacts::Doc> docs;
        std::map<std::string, std::vector<std::string>> uris;    // .gltf -> uris
        std::map<std::string, std::vector<fs::path>> siblings;   // .arcdiag -> siblings
        Arcane::Guid openScene, bootScene;
        std::uint64_t next = 1;

        static std::string Key(const fs::path& p) { return p.lexically_normal().generic_string(); }
        fs::path FileOf(const std::string& mount) const
        {
            const auto sep = mount.find("://");
            return (mount.substr(0, sep) == "diag" ? diag : content) / fs::path(mount.substr(sep + 3));
        }
        Arcane::Guid Add(const std::string& mount, bool onDisk = true)
        {
            const Arcane::Guid g{ 0xA55E7000ull + next, next };
            ++next;
            registry.emplace_back(g, mount);
            if (onDisk)
            {
                files.insert(Key(FileOf(mount)));
                const AssetKind k = AssetKindOf(mount);
                if (k == AssetKind::Texture || k == AssetKind::Audio || k == AssetKind::Font || k == AssetKind::Model)
                    files.insert(Key(FileOf(mount)) + ".meta");
            }
            return g;
        }
        void Touch(const fs::path& p) { files.insert(Key(p)); }
        AssetOpFacts Facts() const
        {
            AssetOpFacts f;
            f.contentDir = content; f.diagDir = diag; f.registry = registry; f.docs = docs;
            f.openScene = openScene; f.bootScene = bootScene;
            f.exists = [this](const fs::path& p) { return files.count(Key(p)) != 0; };
            f.gltfUris = [this](const fs::path& p)
            { const auto it = uris.find(Key(p)); return it == uris.end() ? std::vector<std::string>{} : it->second; };
            f.diagSiblings = [this](const fs::path& p)
            { const auto it = siblings.find(Key(p)); return it == siblings.end() ? std::vector<fs::path>{} : it->second; };
            return f;
        }
    };

    AssetOpPlan Plan(const PlanWorld& w, AssetOpKind kind, std::vector<Arcane::Guid> guids,
                     std::string stem = {}, std::string dest = {})
    {
        AssetOpRequest r;
        r.kind = kind; r.guids = std::move(guids); r.newStem = std::move(stem); r.destFolder = std::move(dest);
        return PlanAssetOp(r, w.Facts());
    }
}

TEST_CASE("NextCopyName: strip a trailing number, then take the first free ' N'", "[editor][assetops]")
{
    const std::set<std::string> taken{ "C:/d/rock.arcmat", "C:/d/rock 1.arcmat", "C:/d/tree 1.png.meta" };
    const auto isTaken = [&](const fs::path& p) { return taken.count(PlanWorld::Key(p)) != 0; };
    CHECK(NextCopyName("rock", "C:/d", ".arcmat", isTaken) == "rock 2");
    CHECK(NextCopyName("rock 1", "C:/d", ".arcmat", isTaken) == "rock 2");
    CHECK(NextCopyName("rock 12", "C:/d", ".arcmat", isTaken) == "rock 2");
    CHECK(NextCopyName("tree", "C:/d", ".png", isTaken) == "tree 2");   // "tree 1" has a stray .meta
    CHECK(NextCopyName("lamp", "C:/d", ".arcmat", isTaken) == "lamp 1");
}

TEST_CASE("PlanAssetOp: file sets and labels per verb", "[editor][assetops]")
{
    PlanWorld w;
    const auto tex = w.Add("game://textures/uv_marker.png");
    const auto mat = w.Add("game://materials/f3_gold.arcmat");
    const auto rep = w.Add("diag://crash_1.arcdiag");
    w.siblings[PlanWorld::Key(w.diag / "crash_1.arcdiag")] = { w.diag / "crash_1.txt", w.diag / "crash_1.dmp" };

    SECTION("Rename: an imported binary renames its .meta; the label names both stems")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::Rename, { tex }, "uv2");
        REQUIRE(p.refusals.empty());
        REQUIRE(p.moves.size() == 1);
        const AssetMove& m = p.moves[0];
        CHECK(m.guid == tex);
        CHECK(m.kind == AssetKind::Texture);
        REQUIRE(m.files.size() == 2);
        CHECK(m.files[0].from == w.content / "textures" / "uv_marker.png");
        CHECK(m.files[0].to == w.content / "textures" / "uv2.png");
        CHECK(m.files[1].to == w.content / "textures" / "uv2.png.meta");
        CHECK(p.label == "Rename uv_marker \xE2\x86\x92 uv2");
    }
    SECTION("Rename: the same name is an empty plan; a case-only rename is not 'already exists'")
    {
        const AssetOpPlan same = Plan(w, AssetOpKind::Rename, { mat }, "f3_gold");
        CHECK(same.moves.empty());
        CHECK(same.refusals.empty());
        const AssetOpPlan p = Plan(w, AssetOpKind::Rename, { mat }, "F3_Gold");
        REQUIRE(p.refusals.empty());
        CHECK(p.moves[0].files[0].to == w.content / "materials" / "F3_Gold.arcmat");
    }
    SECTION("Move: one label for the batch; a move onto the asset's own folder is empty")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::Move, { tex, mat }, {}, "props");
        REQUIRE(p.refusals.empty());
        REQUIRE(p.moves.size() == 2);
        CHECK(p.moves[1].files[0].to == w.content / "props" / "f3_gold.arcmat");
        CHECK(p.label == "Move 2 assets to props/");
        CHECK(Plan(w, AssetOpKind::Move, { mat }, {}, "props").label == "Move f3_gold to props/");
        CHECK(Plan(w, AssetOpKind::Move, { mat }, {}, "materials").moves.empty());
    }
    SECTION("Duplicate: copy names are reserved across the batch; fresh guids are minted")
    {
        const auto rock = w.Add("game://props/rock.arcmat");
        const auto rock1 = w.Add("game://props/rock 1.arcmat");
        const AssetOpPlan p = Plan(w, AssetOpKind::Duplicate, { rock, rock1 });
        REQUIRE(p.moves.size() == 2);
        REQUIRE(p.newGuids.size() == 2);
        CHECK(p.moves[0].files[0].to == w.content / "props" / "rock 2.arcmat");
        CHECK(p.moves[1].files[0].to == w.content / "props" / "rock 3.arcmat");
        CHECK(p.newGuids[0].IsValid());
        CHECK(p.newGuids[0] != p.newGuids[1]);
        CHECK(p.label == "Duplicate 2 assets");
        const AssetOpPlan one = Plan(w, AssetOpKind::Duplicate, { tex });
        CHECK(one.moves[0].files[1].to == w.content / "textures" / "uv_marker 1.png.meta");
        CHECK(one.label == "Duplicate uv_marker");
    }
    SECTION("Delete: no destinations; a crash report brings its siblings")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::Delete, { tex });
        CHECK(p.label == "Delete uv_marker.png");
        REQUIRE(p.moves[0].files.size() == 2);
        CHECK(p.moves[0].files[1].from == w.content / "textures" / "uv_marker.png.meta");
        CHECK(p.moves[0].files[1].to.empty());
        const AssetOpPlan d = Plan(w, AssetOpKind::Delete, { rep, mat });
        CHECK(d.label == "Delete 2 assets");
        CHECK(d.moves[0].files.size() == 3);
    }
    SECTION("open and dirty documents of the touched assets")
    {
        w.docs = { { mat, true, {} }, { tex, false, {} } };
        const AssetOpPlan p = Plan(w, AssetOpKind::Delete, { mat, tex });
        CHECK(p.openDocs == std::vector<Arcane::Guid>{ mat, tex });
        CHECK(p.dirtyDocs == std::vector<Arcane::Guid>{ mat });
    }
    SECTION("New Folder")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::NewFolder, {}, "rocks", "materials");
        REQUIRE(p.moves.size() == 1);
        CHECK(p.moves[0].files[0].to == w.content / "materials" / "rocks");
        CHECK(p.label == "New Folder materials/rocks/");
        w.Touch(w.content / "materials");
        REQUIRE(Plan(w, AssetOpKind::NewFolder, {}, "materials").refusals.size() == 1);
        CHECK(Plan(w, AssetOpKind::NewFolder, {}, "materials").refusals[0].reason == "materials already exists in Content/.");
    }
}

TEST_CASE("PlanAssetOp: one refusal per s7.1 row, and any refusal blocks the batch", "[editor][assetops]")
{
    PlanWorld w;
    const auto tex   = w.Add("game://textures/uv.png");
    const auto src   = w.Add("source://Game/Thing.cpp");
    const auto plug  = w.Add("plugin/fx://mats/glow.arcmat");
    const auto eng   = w.Add("engine://default.arcmat");
    const auto rep   = w.Add("diag://crash_1.arcdiag");
    const auto scene = w.Add("game://scenes/main.arcscene");
    const auto boot  = w.Add("game://scenes/boot.arcscene");
    const auto gone  = w.Add("game://textures/gone.png", /*onDisk*/ false);
    w.openScene = scene;
    w.bootScene = boot;
    const auto reason = [](const AssetOpPlan& p) { REQUIRE(p.refusals.size() == 1); return p.refusals[0].reason; };
    using K = AssetOpKind;

    CHECK(reason(Plan(w, K::Rename, { src }, "Other")) ==
          "C++ source: rename or move it in your IDE (a source file's identity is its path).");
    CHECK(reason(Plan(w, K::Delete, { plug })) == "Plugin and engine content is read-only here.");
    CHECK(reason(Plan(w, K::Move, { eng }, {}, "x")) == "Plugin and engine content is read-only here.");
    for (const K k : { K::Rename, K::Duplicate, K::Move })
        CHECK(reason(Plan(w, k, { rep }, "r", "x")) == "Crash reports can only be deleted.");
    CHECK(Plan(w, K::Delete, { rep }).refusals.empty());
    CHECK(reason(Plan(w, K::Move, { tex }, {}, "../outside")) == "Can't move across mounts.");
    CHECK(reason(Plan(w, K::Delete, { scene })) == "This scene is open. Open another scene first.");
    CHECK(reason(Plan(w, K::Delete, { boot })) == "This is the project's boot scene. Set another boot scene first.");
    w.Touch(w.content / "textures" / "taken.png");
    CHECK(reason(Plan(w, K::Rename, { tex }, "taken")) == "taken.png already exists in textures/.");
    w.Touch(w.content / "materials" / "uv.png.meta");
    CHECK(reason(Plan(w, K::Move, { tex }, {}, "materials")) == "uv.png.meta already exists in materials/.");
    CHECK(reason(Plan(w, K::Rename, { tex }, "a:b")) == "a name cannot contain any of  \\ / : * ? \" < > |");
    CHECK(reason(Plan(w, K::Rename, { tex }, " lead")) == "a name cannot start or end with a dot or a space");
    CHECK(reason(Plan(w, K::Delete, { Arcane::Guid::Generate() })) == "No longer exists.");
    CHECK(reason(Plan(w, K::Delete, { gone })) == "Missing on disk. Reopen the project to rescan.");
    CHECK(reason(Plan(w, K::Rename, { tex, scene }, "x")) == "Select one asset to rename");

    // s7.6: the extension is fixed (it is the kind). Typing it -- in any case -- is
    // refused, never planned as a silent uv.png.png; another inner dot stays legal.
    const AssetOpPlan typedExt = Plan(w, K::Rename, { tex }, "wall.PNG");
    CHECK(reason(typedExt).find("extension is fixed") != std::string::npos);
    CHECK(typedExt.moves.empty());
    const AssetOpPlan innerDot = Plan(w, K::Rename, { tex }, "wall.jpg");
    REQUIRE(innerDot.refusals.empty());
    CHECK(innerDot.moves[0].files[0].to == w.content / "textures" / "wall.jpg.png");

    const AssetOpPlan mixed = Plan(w, K::Delete, { tex, scene });   // one bad asset blocks all
    CHECK(mixed.refusals.size() == 1);
    CHECK(mixed.moves.size() == 1);   // the doomed rows still list (the s7.5 modal shows them)

    // Two selected assets sharing a file name, moved into one folder: the batch claims the first.
    const auto rockA = w.Add("game://a/rock.arcmat");
    const auto rockB = w.Add("game://b/rock.arcmat");
    CHECK(reason(Plan(w, K::Move, { rockA, rockB }, {}, "props")) == "rock.arcmat already exists in props/.");
}

TEST_CASE("PlanAssetOp: a .gltf moves its buffers and registered images; outside or shared files refuse", "[editor][assetops]")
{
    PlanWorld w;
    const auto prop = w.Add("game://models/prop.gltf");
    const auto tex  = w.Add("game://models/tex.png");
    w.Touch(w.content / "models" / "a.bin");
    w.uris[PlanWorld::Key(w.content / "models" / "prop.gltf")] = { "a.bin", "tex.png" };
    const auto reason = [](const AssetOpPlan& p) { REQUIRE(p.refusals.size() == 1); return p.refusals[0].reason; };

    SECTION("all three travel; the image keeps its own guid and .meta")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::Move, { prop }, {}, "props");
        REQUIRE(p.refusals.empty());
        REQUIRE(p.moves.size() == 2);
        CHECK(p.moves[0].guid == prop);
        REQUIRE(p.moves[0].files.size() == 3);   // prop.gltf, prop.gltf.meta, a.bin
        CHECK(p.moves[0].files[2].to == w.content / "props" / "a.bin");
        CHECK(p.moves[1].guid == tex);
        CHECK(p.moves[1].files[1].to == w.content / "props" / "tex.png.meta");
        CHECK(p.label == "Move 2 assets to props/");
    }
    SECTION("a ../ uri refuses")
    {
        w.uris[PlanWorld::Key(w.content / "models" / "prop.gltf")] = { "../shared/a.bin" };
        CHECK(reason(Plan(w, AssetOpKind::Move, { prop }, {}, "props")) == "References ../shared/a.bin outside its folder.");
    }
    SECTION("a buffer shared with a .gltf that is not moving refuses")
    {
        w.Add("game://models/other.gltf");
        w.uris[PlanWorld::Key(w.content / "models" / "other.gltf")] = { "a.bin" };
        CHECK(reason(Plan(w, AssetOpKind::Move, { prop }, {}, "props")) == "Shares a.bin with other.gltf.");
    }
    SECTION("two moving .gltf files sharing a buffer: no refusal, the buffer moves once")
    {
        const auto prop2 = w.Add("game://models/prop2.gltf");
        w.uris[PlanWorld::Key(w.content / "models" / "prop2.gltf")] = { "a.bin" };
        const AssetOpPlan p = Plan(w, AssetOpKind::Move, { prop, prop2 }, {}, "props");
        REQUIRE(p.refusals.empty());
        int bins = 0;
        for (const AssetMove& m : p.moves)
            for (const FileMove& fm : m.files)
                if (fm.from == (w.content / "models" / "a.bin").lexically_normal())
                {
                    ++bins;
                    CHECK(fm.to == (w.content / "props" / "a.bin").lexically_normal());
                }
        CHECK(bins == 1);
        REQUIRE(p.moves.size() == 3);   // prop (+a.bin), tex (dragged), prop2
        CHECK(p.moves[0].guid == prop);
        CHECK(p.moves[2].guid == prop2);
        CHECK(p.moves[2].files.size() == 2);   // prop2.gltf + .meta only
    }
    SECTION("a destination occupied only by a companion refuses")
    {
        w.Touch(w.content / "props" / "a.bin");
        CHECK(reason(Plan(w, AssetOpKind::Move, { prop }, {}, "props")) == "a.bin already exists in props/.");
    }
}

TEST_CASE("ReadGltfUris: buffers and images, data: skipped, percent-decoded; .glb is self-contained", "[editor][assetops]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_gltf_uris";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::ofstream(dir / "x.gltf", std::ios::binary) << R"({
        "buffers": [ { "uri": "my%20mesh.bin" }, { "uri": "data:application/octet-stream;base64,AAAA" } ],
        "images":  [ { "uri": "tex.png" }, { "bufferView": 0 } ] })";
    std::ofstream(dir / "y.glb", std::ios::binary) << "glTF";
    CHECK(ReadGltfUris(dir / "x.gltf") == std::vector<std::string>{ "my mesh.bin", "tex.png" });
    CHECK(ReadGltfUris(dir / "y.glb").empty());
    CHECK(ReadGltfUris(dir / "missing.gltf").empty());
    fs::remove_all(dir, ec);
}

TEST_CASE("PlanAssetOp: batch claims fold case as NTFS does", "[editor][assetops]")
{
    // fs::exists is case-insensitive on NTFS; the batch's own claims must agree, or
    // two approved moves collide in the executor (rock.arcmat vs Rock.arcmat).
    PlanWorld w;
    const auto rockA = w.Add("game://a/rock.arcmat");
    const auto rockC = w.Add("game://c/Rock.arcmat");
    const AssetOpPlan p = Plan(w, AssetOpKind::Move, { rockA, rockC }, {}, "props");
    REQUIRE(p.refusals.size() == 1);
    CHECK(p.refusals[0].guid == rockC);
    CHECK(p.refusals[0].reason == "Rock.arcmat already exists in props/.");
}

TEST_CASE("PlanAssetOp: an imported asset whose .meta sidecar is gone refuses every verb", "[editor][assetops]")
{
    // s7.3: the .meta always moves -- it holds the guid. Moving the binary alone would
    // fail Rebind's id check (and a rescan would mint a new guid), so the planner
    // refuses instead of approving a step the executor cannot finish.
    PlanWorld w;
    const auto bare = w.Add("game://textures/bare.png");
    w.files.erase(PlanWorld::Key(w.content / "textures" / "bare.png.meta"));
    const auto reason = [](const AssetOpPlan& p) { REQUIRE(p.refusals.size() == 1); return p.refusals[0].reason; };
    using K = AssetOpKind;
    for (const K k : { K::Rename, K::Duplicate, K::Delete, K::Move })
        CHECK(reason(Plan(w, k, { bare }, "wall", "materials")) == "bare.png.meta is missing. Reopen the project to rescan.");

    // A native asset never had a sidecar: nothing to check.
    const auto mat = w.Add("game://materials/m.arcmat");
    CHECK(Plan(w, K::Move, { mat }, {}, "props").refusals.empty());
}

TEST_CASE("PlanAssetOp: a .gltf's registered companion brings a .meta only when its kind has one", "[editor][assetops]")
{
    PlanWorld w;
    const auto prop = w.Add("game://models/prop.gltf");
    const auto tex  = w.Add("game://models/tex.png");
    const auto look = w.Add("game://models/look.arcmat");
    w.uris[PlanWorld::Key(w.content / "models" / "prop.gltf")] = { "tex.png", "look.arcmat" };

    SECTION("an imported image moves with its sidecar; a native companion moves alone")
    {
        const AssetOpPlan p = Plan(w, AssetOpKind::Move, { prop }, {}, "props");
        REQUIRE(p.refusals.empty());
        REQUIRE(p.moves.size() == 3);
        CHECK(p.moves[1].guid == tex);
        CHECK(p.moves[1].files.size() == 2);
        CHECK(p.moves[2].guid == look);
        REQUIRE(p.moves[2].files.size() == 1);
        CHECK(p.moves[2].files[0].to == w.content / "props" / "look.arcmat");
    }
    SECTION("an imported companion whose sidecar is gone refuses the .gltf")
    {
        w.files.erase(PlanWorld::Key(w.content / "models" / "tex.png.meta"));
        const AssetOpPlan p = Plan(w, AssetOpKind::Move, { prop }, {}, "props");
        REQUIRE(p.refusals.size() == 1);
        CHECK(p.refusals[0].guid == prop);
        CHECK(p.refusals[0].reason == "tex.png.meta is missing. Reopen the project to rescan.");
    }
}

// ---- execution (s7.3/s7.4) ---------------------------------------------------

using Arcane::Test::AssetOpsWorld;
using Arcane::Test::FakeAssetOpHost;

TEST_CASE("AssetOpGateRefusal: the three s7.1 gates, in order", "[editor][assetops]")
{
    CHECK(AssetOpGateRefusal({ false, true }, false) == "No project open");
    CHECK(AssetOpGateRefusal({ true, false }, false) == "Stop Play to change asset files");
    CHECK(AssetOpGateRefusal({ true, true }, true) == "Finish the current edit first");
    CHECK_FALSE(AssetOpGateRefusal({ true, true }, false).has_value());
}

TEST_CASE("AssetFileOps: a Move runs all-or-nothing and pushes exactly one step", "[editor][assetops]")
{
    AssetOpsWorld w("exec_move");
    const auto tex = w.Write("textures/uv.png", "png-bytes");
    const auto mat = w.Write("a.arcmat", R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })");
    FakeAssetOpHost host(w);
    Arcane::CommandStack stack{ &Arcane::Test::NoSceneRegistry };
    AssetFileOpExecutor exec(host, stack, w.content);
    const auto before = w.Snapshot();
    const auto regBefore = w.registry.All();
    const AssetOpPlan plan = w.Plan(AssetOpKind::Move, { tex, mat }, {}, "materials");
    REQUIRE(plan.refusals.empty());

    SECTION("a failure at the k-th file rolls every earlier file back and pushes nothing")
    {
        int n = 0;
        exec.SetRenameForTest([&](const fs::path& from, const fs::path& to)
        {
            std::error_code ec;
            if (++n == 3) return std::make_error_code(std::errc::permission_denied);
            fs::rename(from, to, ec);
            return ec;
        });
        const ExecResult r = exec.Execute(plan, stack);
        CHECK_FALSE(r.ok);
        CHECK(w.Snapshot() == before);
        CHECK(w.registry.All() == regBefore);
        CHECK_FALSE(stack.CanUndo());
        REQUIRE(host.errors.size() == 1);
        CHECK(host.errors[0].first == "Move 2 assets to materials/ failed");
    }
    SECTION("success moves every file, rebinds, and the one step undoes and redoes")
    {
        REQUIRE(exec.Execute(plan, stack).ok);
        CHECK(w.registry.Resolve(tex) == "game://materials/uv.png");
        CHECK(fs::exists(w.content / "materials" / "uv.png.meta"));
        CHECK(std::string(stack.UndoLabel()) == "Move 2 assets to materials/");
        const auto has = [&](const char* c) { return std::count(host.calls.begin(), host.calls.end(), std::string(c)); };
        CHECK(has("NoteMoved materials/uv.png") == 1);
        CHECK(has("EvictPaths 6") == 1);
        CHECK(has("AssetsChanged -0 +0") == 1);

        stack.Undo();
        CHECK(w.Snapshot() == before);
        CHECK(w.registry.All() == regBefore);
        stack.Redo();
        CHECK(w.registry.Resolve(mat) == "game://materials/a.arcmat");
        CHECK_FALSE(stack.CanRedo());
    }
}

TEST_CASE("AssetFileOps: gates and an empty plan push nothing", "[editor][assetops]")
{
    AssetOpsWorld w("exec_gates");
    const auto mat = w.Write("a.arcmat", R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })");
    FakeAssetOpHost host(w);
    Arcane::CommandStack stack{ &Arcane::Test::NoSceneRegistry };
    AssetFileOpExecutor exec(host, stack, w.content);
    host.gates.editMode = false;
    const ExecResult r = exec.Execute(w.Plan(AssetOpKind::Rename, { mat }, "b"), stack);
    CHECK_FALSE(r.ok);
    CHECK(r.error == "Stop Play to change asset files");
    host.gates.editMode = true;
    CHECK(exec.Execute(w.Plan(AssetOpKind::Move, { mat }, {}, ""), stack).ok);   // already there
    CHECK_FALSE(stack.CanUndo());
    CHECK(fs::exists(w.content / "a.arcmat"));
}

TEST_CASE("AssetFileOps: undoing onto an occupied path refuses loudly, writes nothing, then expires", "[editor][assetops]")
{
    AssetOpsWorld w("exec_occupied");
    const auto tex = w.Write("textures/uv.png", "png-bytes");
    FakeAssetOpHost host(w);
    Arcane::CommandStack stack{ &Arcane::Test::NoSceneRegistry };
    AssetFileOpExecutor exec(host, stack, w.content);
    REQUIRE(exec.Execute(w.Plan(AssetOpKind::Rename, { tex }, "uv2"), stack).ok);

    w.WriteRaw("textures/uv.png", "someone else");
    stack.Undo();
    REQUIRE(host.errors.size() == 1);
    CHECK(host.errors[0].first == "Can't undo Rename uv \xE2\x86\x92 uv2");
    CHECK(host.errors[0].second == "textures/uv.png is occupied by another file.");
    CHECK(Arcane::Test::Slurp(w.content / "textures" / "uv.png") == "someone else");
    CHECK(fs::exists(w.content / "textures" / "uv2.png"));
    CHECK(w.registry.Resolve(tex) == "game://textures/uv2.png");
    CHECK_FALSE(stack.CanUndo());   // blocked = expired: T1's stack skips it
    CHECK_FALSE(stack.CanRedo());
}

TEST_CASE("AssetFileOps: a moved file deleted or re-identified outside the editor expires its step", "[editor][assetops]")
{
    AssetOpsWorld w("exec_expiry");
    const auto tex = w.Write("textures/uv.png", "png-bytes");
    FakeAssetOpHost host(w);
    Arcane::CommandStack stack{ &Arcane::Test::NoSceneRegistry };
    AssetFileOpExecutor exec(host, stack, w.content);
    REQUIRE(exec.Execute(w.Plan(AssetOpKind::Rename, { tex }, "uv2"), stack).ok);
    REQUIRE(stack.CanUndo());

    SECTION("deleted: the primary is gone (its .meta alone is not the asset)")
    {
        fs::remove(w.content / "textures" / "uv2.png");
        CHECK_FALSE(stack.CanUndo());
    }
    SECTION("re-identified: the .meta now names another guid")
    {
        w.WriteRaw("textures/uv2.png.meta", R"({ "guid": "dddd4444-4444-4444-8444-444444444444", "version": 1 })");
        CHECK_FALSE(stack.CanUndo());
    }
    CHECK(host.errors.empty());   // expiry is silent; only a refused side reports
}

TEST_CASE("AssetFileOps: a Move removes only the folders it created, on rollback and on undo", "[editor][assetops]")
{
    AssetOpsWorld w("exec_dirs");
    const auto tex = w.Write("textures/uv.png", "png-bytes");
    FakeAssetOpHost host(w);
    Arcane::CommandStack stack{ &Arcane::Test::NoSceneRegistry };
    AssetFileOpExecutor exec(host, stack, w.content);
    const fs::path made = w.content / "new" / "deep";   // Snapshot() lists files only: assert folders directly

    SECTION("a move into a new folder, then undo, leaves no folder; redo makes it again")
    {
        REQUIRE(exec.Execute(w.Plan(AssetOpKind::Move, { tex }, {}, "new/deep"), stack).ok);
        CHECK(fs::exists(made / "uv.png"));
        stack.Undo();
        CHECK(fs::exists(w.content / "textures" / "uv.png"));
        CHECK_FALSE(fs::exists(w.content / "new"));   // both levels it created, deepest first
        stack.Redo();
        CHECK(fs::exists(made / "uv.png.meta"));
        stack.Undo();
        CHECK_FALSE(fs::exists(w.content / "new"));
        CHECK(fs::is_directory(w.content / "textures"));   // the source folder existed before: kept throughout
    }
    SECTION("a failed move rolls the folder back with the files")
    {
        int n = 0;
        exec.SetRenameForTest([&](const fs::path& from, const fs::path& to)
        {
            std::error_code ec;
            if (++n == 2) return std::make_error_code(std::errc::permission_denied);   // the .meta
            fs::rename(from, to, ec);
            return ec;
        });
        CHECK_FALSE(exec.Execute(w.Plan(AssetOpKind::Move, { tex }, {}, "new/deep"), stack).ok);
        CHECK(fs::exists(w.content / "textures" / "uv.png"));
        CHECK_FALSE(fs::exists(w.content / "new"));
    }
    SECTION("a created folder that gained another file between apply and undo is kept")
    {
        REQUIRE(exec.Execute(w.Plan(AssetOpKind::Move, { tex }, {}, "new/deep"), stack).ok);
        w.WriteRaw("new/deep/notes.txt", "mine");
        stack.Undo();
        CHECK(fs::exists(w.content / "textures" / "uv.png"));
        CHECK(Arcane::Test::Slurp(made / "notes.txt") == "mine");
    }
    SECTION("a folder that existed before the move is never removed")
    {
        fs::create_directories(made);
        REQUIRE(exec.Execute(w.Plan(AssetOpKind::Move, { tex }, {}, "new/deep"), stack).ok);
        stack.Undo();
        CHECK(fs::is_directory(made));
    }
}

TEST_CASE("AssetFileOps: a step outliving its executor is inert", "[editor][assetops]")
{
    AssetOpsWorld w("exec_dead");
    const auto mat = w.Write("a.arcmat", R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })");
    FakeAssetOpHost host(w);
    Arcane::CommandStack stack{ &Arcane::Test::NoSceneRegistry };
    {
        AssetFileOpExecutor exec(host, stack, w.content);
        REQUIRE(exec.Execute(w.Plan(AssetOpKind::Rename, { mat }, "b"), stack).ok);
    }
    CHECK_FALSE(stack.CanUndo());   // dead anchor = expired
    stack.Undo();
    CHECK(fs::exists(w.content / "b.arcmat"));
}

TEST_CASE("AssetFileOps: New Folder creates, undoes only while empty, and expires once filled", "[editor][assetops]")
{
    AssetOpsWorld w("exec_folder");
    fs::create_directories(w.content / "materials");
    FakeAssetOpHost host(w);
    Arcane::CommandStack stack{ &Arcane::Test::NoSceneRegistry };
    AssetFileOpExecutor exec(host, stack, w.content);
    const AssetOpPlan plan = w.Plan(AssetOpKind::NewFolder, {}, "rocks", "materials");
    REQUIRE(exec.Execute(plan, stack).ok);
    const fs::path dir = w.content / "materials" / "rocks";
    CHECK(fs::is_directory(dir));
    CHECK(std::string(stack.UndoLabel()) == "New Folder materials/rocks/");

    SECTION("undo removes it; redo brings it back")
    {
        stack.Undo();
        CHECK_FALSE(fs::exists(dir));
        stack.Redo();
        CHECK(fs::is_directory(dir));
    }
    SECTION("a folder with something in it is never removed")
    {
        w.WriteRaw("materials/rocks/keep.txt", "x");
        CHECK_FALSE(stack.CanUndo());
        stack.Undo();
        CHECK(fs::exists(dir / "keep.txt"));
    }
}

namespace
{
    struct DeleteRig
    {
        AssetOpsWorld w;
        FakeAssetOpHost host{ w };
        Arcane::CommandStack stack{ &Arcane::Test::NoSceneRegistry };
        AssetFileOpExecutor exec{ host, stack, w.content };
        Arcane::Guid tex;
        explicit DeleteRig(const char* leaf) : w(leaf) { tex = w.Write("textures/uv_marker.png", "png-bytes"); }
    };
}

TEST_CASE("AssetFileOps: delete -> undo -> redo round-trips bytes, mtime and registry", "[editor][assetops]")
{
    DeleteRig r("delete_roundtrip");
    const auto before = r.w.Snapshot();
    const auto regBefore = r.w.registry.All();
    const auto mtime = fs::last_write_time(r.w.content / "textures" / "uv_marker.png");

    REQUIRE(r.exec.Execute(r.w.Plan(AssetOpKind::Delete, { r.tex }), r.stack).ok);
    CHECK(r.host.recycleCalls == 1);                          // one call for the whole batch
    CHECK_FALSE(fs::exists(r.w.content / "textures" / "uv_marker.png.meta"));
    CHECK_FALSE(r.w.registry.Resolve(r.tex).has_value());
    CHECK(std::string(r.stack.UndoLabel()) == "Delete uv_marker.png");

    r.stack.Undo();
    CHECK(r.w.Snapshot() == before);
    CHECK(r.w.registry.All() == regBefore);                   // the same guid comes back
    // Parenthesised: libc++ file_time_type has an __int128 rep, which Catch2
    // cannot stringify (no ostream operator), so the comparison is not decomposed.
    CHECK((fs::last_write_time(r.w.content / "textures" / "uv_marker.png") == mtime));

    r.w.WriteRaw("textures/uv_marker.png", "edited after undo");
    r.stack.Redo();                                           // re-captures the edited bytes
    r.stack.Undo();
    CHECK(Arcane::Test::Slurp(r.w.content / "textures" / "uv_marker.png") == "edited after undo");
}

TEST_CASE("AssetFileOps: undoing a delete onto an occupied path blocks with the bin note", "[editor][assetops]")
{
    DeleteRig r("delete_occupied");
    REQUIRE(r.exec.Execute(r.w.Plan(AssetOpKind::Delete, { r.tex }), r.stack).ok);
    r.w.WriteRaw("textures/uv_marker.png", "a new file");
    r.stack.Undo();
    REQUIRE(r.host.errors.size() == 1);
    CHECK(r.host.errors[0].first == "Can't undo Delete uv_marker.png");
    CHECK(r.host.errors[0].second ==
          "textures/uv_marker.png is occupied by another file. Your deleted file is in the Recycle Bin.");
    CHECK(Arcane::Test::Slurp(r.w.content / "textures" / "uv_marker.png") == "a new file");
    CHECK_FALSE(fs::exists(r.w.content / "textures" / "uv_marker.png.meta"));   // nothing written
    CHECK_FALSE(r.stack.CanUndo());
}

TEST_CASE("AssetFileOps: a failed capture or a recycle survivor deletes nothing", "[editor][assetops]")
{
    DeleteRig r("delete_failures");
    const auto before = r.w.Snapshot();
    SECTION("capture failure: aborts before anything is recycled")
    {
        int n = 0;
        r.exec.SetCaptureForTest([&](const fs::path& p) -> std::optional<Arcane::UndoPayload>
        {
            if (++n == 2) return std::nullopt;   // e.g. a full disk
            return r.stack.MakePayloadFromFile(p);
        });
        CHECK_FALSE(r.exec.Execute(r.w.Plan(AssetOpKind::Delete, { r.tex }), r.stack).ok);
        CHECK(r.host.recycleCalls == 0);
    }
    SECTION("survivor: already-removed files are rewritten from their payloads")
    {
        r.host.survivor = r.w.content / "textures" / "uv_marker.png.meta";
        CHECK_FALSE(r.exec.Execute(r.w.Plan(AssetOpKind::Delete, { r.tex }), r.stack).ok);
        CHECK(r.host.recycleCalls == 1);
    }
    CHECK(r.w.Snapshot() == before);
    CHECK(r.w.registry.Resolve(r.tex).has_value());
    CHECK_FALSE(r.stack.CanUndo());
}

// User ruling (2026-10-02, option A): a confirmed delete closes the doomed assets'
// documents only AFTER the files are verified gone. A failed recycle leaves every
// document open, unsaved edits included; a successful one closes them before the
// step is pushed.
TEST_CASE("AssetFileOps: a failed delete keeps the dirty document open with its edits", "[editor][assetops]")
{
    DeleteRig r("delete_keeps_dirty_doc");
    r.host.dirtyDocs.insert(r.tex);   // open in a document with unsaved edits
    r.host.survivor = r.w.content / "textures" / "uv_marker.png";   // the shell leaves the primary in place

    CHECK_FALSE(r.exec.Execute(r.w.Plan(AssetOpKind::Delete, { r.tex }), r.stack).ok);
    CHECK(r.host.recycleCalls == 1);
    CHECK(r.host.closedDocs.empty());                 // the document (and its edits) stays
    CHECK(r.host.dirtyDocs.count(r.tex) == 1);
    CHECK(r.w.registry.Resolve(r.tex).has_value());
    CHECK_FALSE(r.stack.CanUndo());

    r.host.survivor.reset();                          // the retry succeeds
    REQUIRE(r.exec.Execute(r.w.Plan(AssetOpKind::Delete, { r.tex }), r.stack).ok);
    REQUIRE(r.host.closedDocs.count(r.tex) == 1);
    CHECK(r.host.closedDocs.at(r.tex) == 2);          // closed after the second (successful) recycle
    CHECK(r.stack.CanUndo());
}

// T5-GATE desk addition (s7.13), automated: a .png the editor deleted and the user restores
// from the OS Recycle Bin mid-session comes back with its guid -- the mid-session discovery
// (EditorAppProject.cpp's PollAssetWatch: DiscoverUnknownSources over the registry's known
// sources, then RegisterCreatedAsset -> AddFile) finds it again and reads the id from its
// restored .meta instead of minting one.
TEST_CASE("Delete: a .png restored from the Recycle Bin is rediscovered with its guid", "[editor][assetops]")
{
    DeleteRig r("delete_restore_from_bin");
    const fs::path png = r.w.content / "textures" / "uv_marker.png", meta = fs::path(png) += ".meta";
    const std::string pngBytes = Arcane::Test::Slurp(png), metaBytes = Arcane::Test::Slurp(meta);
    REQUIRE(r.exec.Execute(r.w.Plan(AssetOpKind::Delete, { r.tex }), r.stack).ok);
    REQUIRE_FALSE(fs::exists(png));
    REQUIRE_FALSE(r.w.registry.Resolve(r.tex).has_value());

    std::ofstream(meta, std::ios::binary) << metaBytes;   // the shell's Restore puts both files back
    std::ofstream(png, std::ios::binary) << pngBytes;
    // PollAssetWatch's own discovery step (EditorAppProject.cpp): the editor's
    // known-set rule and extension set over the registry and a "game" mount,
    // then the registration RegisterCreatedAsset reaches (Project::RegisterAsset
    // -> AssetRegistry::AddFile under the matching mount root).
    Arcane::MountTable mounts;
    mounts.Mount("game", r.w.content);
    const std::unordered_set<std::string> known = KnownDiscoverySourcePaths(r.w.registry, mounts);
    CHECK_FALSE(known.contains(png.generic_string()));   // the delete unmapped it
    const std::vector<fs::path> found = DiscoverUnknownSources(r.w.content, kDiscoveryExtensions, known);
    REQUIRE(found.size() == 1);
    CHECK(found[0].generic_string() == png.generic_string());
    CHECK(r.w.registry.AddFile(found[0], r.w.content, "game") == r.tex);   // the id from the restored .meta, not a mint
    CHECK(KnownDiscoverySourcePaths(r.w.registry, mounts).contains(png.generic_string()));   // the next poll leaves it alone
    CHECK(r.w.registry.Resolve(r.tex) == "game://textures/uv_marker.png");
    CHECK(Arcane::Test::Slurp(meta) == metaBytes);                         // nothing re-minted or rewritten
}

TEST_CASE("AssetFileOps: spill, nuked items and a dirty document on redo", "[editor][assetops]")
{
    DeleteRig r("delete_spill");
    const fs::path spill = r.w.root / "Saved" / "UndoCache";
    r.stack.SetSpillDirectory(spill);
    r.stack.SetLimits(Arcane::UndoLimits{ 100, 1ull << 30, 4 });   // 4-byte threshold: everything spills
    r.host.permanently = true;
    REQUIRE(r.exec.Execute(r.w.Plan(AssetOpKind::Delete, { r.tex }), r.stack).ok);
    CHECK(r.exec.LastRecycle().permanentlyDeleted.size() == 2);   // s7.12 words the activity row
    CHECK_FALSE(fs::is_empty(spill));
    r.stack.Undo();
    CHECK(Arcane::Test::Slurp(r.w.content / "textures" / "uv_marker.png") == "png-bytes");

    r.host.dirtyDocs.insert(r.tex);   // reopened and edited since
    r.stack.Redo();
    REQUIRE(r.host.errors.size() == 1);
    CHECK(r.host.errors[0].second == "Close or save textures/uv_marker.png first.");
    CHECK(fs::exists(r.w.content / "textures" / "uv_marker.png"));
}

TEST_CASE("AssetFileOps: a delete whose spilled undo copy is gone expires silently", "[editor][assetops]")
{
    DeleteRig r("delete_spill_lost");
    const fs::path spill = r.w.root / "Saved" / "UndoCache";
    r.stack.SetSpillDirectory(spill);
    r.stack.SetLimits(Arcane::UndoLimits{ 100, 1ull << 30, 4 });   // 4-byte threshold: everything spills
    REQUIRE(r.exec.Execute(r.w.Plan(AssetOpKind::Delete, { r.tex }), r.stack).ok);
    std::error_code ec;
    fs::remove_all(spill, ec);                                      // e.g. a cleaner wiped Saved/
    REQUIRE_FALSE(ec);
    CHECK_FALSE(r.stack.CanUndo());                                 // s7.4: payload unreadable = expired
    r.stack.Undo();
    CHECK(r.host.errors.empty());                                   // expiry is silent
    CHECK_FALSE(fs::exists(r.w.content / "textures" / "uv_marker.png"));
    CHECK_FALSE(fs::exists(r.w.content / "textures" / "uv_marker.png.meta"));
}

TEST_CASE("AssetFileOps: a duplicate's step recycles the copy on undo and restores it with its guid on redo", "[editor][assetops]")
{
    AssetOpsWorld w("dup_cmd");
    const auto rock = w.Write("props/rock.arcmat", R"({ "id": "aaaa1111-1111-4111-8111-111111111111", "name": "rock" })");
    const auto copy = w.Write("props/rock 1.arcmat", R"({ "id": "bbbb2222-2222-4222-8222-222222222222", "name": "rock 1" })");   // what s7.7 writes
    FakeAssetOpHost host(w);
    Arcane::CommandStack stack{ &Arcane::Test::NoSceneRegistry };
    AssetFileOpExecutor exec(host, stack, w.content);
    stack.Push(std::make_unique<AssetDuplicateCommand>(exec.Anchor(), "Duplicate rock",
        std::vector<AssetFiles>{ { copy, { w.content / "props" / "rock 1.arcmat" } } }));
    const auto withCopy = w.Snapshot();

    stack.Undo();
    CHECK_FALSE(fs::exists(w.content / "props" / "rock 1.arcmat"));
    CHECK_FALSE(w.registry.Resolve(copy).has_value());
    CHECK(w.registry.Resolve(rock).has_value());
    stack.Redo();
    CHECK(w.Snapshot() == withCopy);
    CHECK(w.registry.Resolve(copy) == "game://props/rock 1.arcmat");
}

TEST_CASE("AssetFileOps: no file step affects the scene; a dead anchor reads expired", "[editor][assetops]")
{
    const std::weak_ptr<AssetFileOpExecutor*> dead;
    CHECK_FALSE(AssetMoveCommand(dead, "m", {}).AffectsScene());
    CHECK_FALSE(NewFolderCommand(dead, "n", {}).AffectsScene());
    CHECK_FALSE(AssetDeleteCommand(dead, "d", {}, {}).AffectsScene());
    CHECK_FALSE(AssetDuplicateCommand(dead, "u", {}).AffectsScene());
    CHECK(AssetDuplicateCommand(dead, "u", {}).IsExpired());
}

// Carry ruling (T5-A13 minors fixed in T5-A14): a failed mtime restore is a failure
// (s7.4: the watcher must see no change), never a silent pass; and a restore that fails
// part-way removes what it wrote AND the folders it created (T5-A10's rule).
TEST_CASE("AssetFileOps: a restore whose modified time cannot be set fails and keeps nothing it made", "[editor][assetops]")
{
    DeleteRig r("restore_mtime_fail");
    const fs::path dir = r.w.content / "textures";
    const fs::path png = dir / "uv_marker.png";
    std::vector<AssetPayloads> payloads;
    REQUIRE_FALSE(r.exec.RemoveAssets(std::vector<AssetFiles>{ { r.tex, { png, fs::path(png) += ".meta" } } },
                                      payloads, /*discardDirty*/ false));
    REQUIRE(payloads.size() == 1);
    REQUIRE(payloads[0].files.size() == 2);
    payloads[0].files[0].mtime = fs::file_time_type::min();   // the primary, written after its .meta; no filesystem records it

    SECTION("into its folder: the .meta and the primary go; the folder, there before, stays")
    {
        const auto failure = r.exec.RestoreAssets(payloads);
        REQUIRE(failure.has_value());
        CHECK(failure->starts_with("textures/uv_marker.png could not keep its modified time ("));
        CHECK(fs::is_directory(dir));
        CHECK(fs::is_empty(dir));
    }
    SECTION("into a folder deleted since: the folder the restore created goes too")
    {
        REQUIRE(fs::remove(dir));   // emptied by the recycle
        REQUIRE(r.exec.RestoreAssets(payloads).has_value());
        CHECK_FALSE(fs::exists(dir));
    }
    CHECK_FALSE(r.w.registry.Resolve(r.tex).has_value());
    CHECK(r.host.errors.empty());   // the primitive reports; the step that called it words the error
}

// ---- T5-B6 (s7.6): rename end to end, the case-only primitive ---------------

namespace
{
    std::string OnDiskName(const std::filesystem::path& p)   // the entry's real case
    {
        const auto low = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
        for (const auto& e : std::filesystem::directory_iterator(p.parent_path())) if (low(e.path().filename().string()) == low(p.filename().string())) return e.path().filename().string();
        return {};
    }
    AssetOpRequest Rename(const Arcane::Guid& g, std::string s) { return { .kind = AssetOpKind::Rename, .guids = { g }, .newStem = std::move(s) }; }
}
TEST_CASE("Rename: a .png moves its .meta, keeps its guid, and a case-only rename round-trips undo/redo", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_rename"); t.Write("t/brick.png", "px"); t.Write("a.png", "px"); t.Scan();
    const Arcane::Guid g = t.GuidOf("t/brick.png"), a = t.GuidOf("a.png");
    AssetOpsTest::Host h(t); AssetFileOpExecutor exec(h, t.stack, t.content);
    REQUIRE(exec.Execute(PlanAssetOp(Rename(g, "wall"), t.Facts()), t.stack).ok);
    CHECK((std::filesystem::exists(t.content / "t/wall.png.meta") && !std::filesystem::exists(t.content / "t/brick.png.meta")));
    CHECK(t.registry.Resolve(g) == std::optional<std::string>("game://t/wall.png"));
    const AssetOpPlan cs = PlanAssetOp(Rename(a, "A"), t.Facts());
    REQUIRE(cs.refusals.empty());                                   // the equivalent destination is free
    REQUIRE(exec.Execute(cs, t.stack).ok); CHECK(OnDiskName(t.content / "A.png") == "A.png");
    t.stack.Undo(); CHECK(OnDiskName(t.content / "a.png") == "a.png");
    t.stack.Redo(); CHECK((OnDiskName(t.content / "A.png") == "A.png" && Arcane::AssetRegistry::PeekId(t.content / "A.png") == a));
}
TEST_CASE("Rename: a .gltf keeps its .bin; same stem is a no-op; a bad name refuses with the rule's message", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_rename_gltf"); t.Write("m/ship.gltf", R"({"buffers":[{"uri":"ship.bin"}]})"); t.Write("m/ship.bin", "b"); t.Scan();
    AssetOpsTest::Host h(t); AssetFileOpExecutor exec(h, t.stack, t.content);
    REQUIRE(exec.Execute(PlanAssetOp(Rename(t.GuidOf("m/ship.gltf"), "hull"), t.Facts()), t.stack).ok);
    CHECK((std::filesystem::exists(t.content / "m/hull.gltf") && std::filesystem::exists(t.content / "m/ship.bin")));
    const Arcane::Guid hull = *Arcane::AssetRegistry::PeekId(t.content / "m/hull.gltf");
    const AssetOpPlan same = PlanAssetOp(Rename(hull, "hull"), t.Facts()), bad = PlanAssetOp(Rename(hull, "a|b"), t.Facts());
    CHECK((same.refusals.empty() && same.moves.empty() && bad.refusals.size() == 1 && bad.refusals[0].reason.find("cannot contain") != std::string::npos));
    t.Write("c.png", "px");                                          // the case-only primitive itself (fails first: undeclared)
    CHECK((OsShell::RenameCaseOnly(t.content / "c.png", t.content / "C.png") && OnDiskName(t.content / "C.png") == "C.png"));
}
TEST_CASE("NextCopyName: rock -> rock 1 -> rock 2; rock 1 -> rock 2; reservations and orphan sidecars count", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_copyname"); const auto c = t.content;   // T5-A6's predicate form over a real disk (coverage)
    const auto disk = [](const std::filesystem::path& p) { std::error_code e; return std::filesystem::exists(p, e); };
    t.Write("rock.arcmat", "{}"); CHECK(NextCopyName("rock", c, ".arcmat", disk) == "rock 1");
    t.Write("rock 1.arcmat", "{}"); CHECK((NextCopyName("rock", c, ".arcmat", disk) == "rock 2" && NextCopyName("rock 1", c, ".arcmat", disk) == "rock 2"));
    CHECK(NextCopyName("rock", c, ".arcmat", [&](const std::filesystem::path& p) { return disk(p) || p == c / "rock 2.arcmat"; }) == "rock 3");
    t.Write("img 1.png.meta", "{}"); CHECK(NextCopyName("img", c, ".png", disk) == "img 2");
}
TEST_CASE("Duplicate: every kind's copy carries a fresh on-disk id; the registry holds both; undo/redo", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_dup_kinds");
    for (const char* d : { "m", "s", "x" }) std::filesystem::create_directories(t.content / d);   // Save*Asset leave parent dirs to the caller
    Arcane::MaterialAssetData mat; mat.id = Arcane::Guid::Generate(); mat.name = "Gold"; REQUIRE(Arcane::SaveMaterialAsset(t.content / "m/gold.arcmat", mat));
    Arcane::SpriteAssetData spr; spr.id = Arcane::Guid::Generate(); spr.texture = Arcane::Guid::Generate(); REQUIRE(Arcane::SaveSpriteAsset(t.content / "s/ui.arcsprite", spr));
    const Arcane::Guid model = Arcane::Guid::Generate();
    Arcane::MeshAssetData mesh; mesh.id = Arcane::Guid::Generate(); mesh.source = Arcane::MeshSource::Imported; mesh.importedSource = model;
    REQUIRE(Arcane::SaveMeshAsset(t.content / "x/prop.arcmesh", mesh));
    t.Write("in/p.arcinput", R"({"id":"7e5a7777-0001-4001-8001-000000000001","maps":[]})");
    t.Write("tex/a.png", "px"); t.Write("tex/a.png.meta", R"({"guid":"7e5a7777-0002-4002-8002-000000000002","version":1,"srgb":false})"); t.Scan();
    const std::vector<Arcane::Guid> src = { t.GuidOf("m/gold.arcmat"), t.GuidOf("s/ui.arcsprite"), t.GuidOf("x/prop.arcmesh"), t.GuidOf("in/p.arcinput"), t.GuidOf("tex/a.png") };
    AssetOpsTest::Host h(t); AssetFileOpExecutor exec(h, t.stack, t.content);
    const AssetOpPlan plan = PlanAssetOp({ .kind = AssetOpKind::Duplicate, .guids = src }, t.Facts());
    REQUIRE(exec.Execute(plan, t.stack).ok); REQUIRE(plan.newGuids.size() == src.size());
    for (std::size_t i = 0; i < src.size(); ++i) CHECK((plan.newGuids[i] != src[i] && t.registry.Resolve(src[i]) && t.registry.Resolve(plan.newGuids[i])));
    CHECK(Arcane::LoadMaterialAsset(t.content / "m/gold 1.arcmat")->name == "gold 1");
    CHECK(Arcane::LoadSpriteAsset(t.content / "s/ui 1.arcsprite")->texture == spr.texture);
    const auto prop1 = Arcane::LoadMeshAsset(t.content / "x/prop 1.arcmesh"); REQUIRE(prop1);
    CHECK_FALSE(prop1->importedSource.IsValid());              // companion strip (drafting pick 9.28)
    const std::vector<std::pair<Arcane::Guid, Arcane::Guid>> pairs = { { mesh.id, model }, { prop1->id, prop1->importedSource } };
    CHECK(UniqueImportedCompanion(model, pairs) == mesh.id);
    const auto meta = nlohmann::json::parse(std::ifstream(t.content / "tex/a 1.png.meta"));
    CHECK((meta["srgb"] == false && meta["guid"] == plan.newGuids[4].ToString()));
    t.stack.Undo(); CHECK_FALSE(std::filesystem::exists(t.content / "m/gold 1.arcmat"));
    t.stack.Redo(); CHECK(Arcane::AssetRegistry::PeekId(t.content / "m/gold 1.arcmat") == plan.newGuids[0]);
}
// T5-B9 review (owed at T5-GATE): the atomic write behind every duplicate leaves no
// "<to>.arctmp" behind on ANY failure after the temp opens -- a failed write, a failed
// rename -- and never a half-written target.
TEST_CASE("WriteTextFileAtomic: a failed write or rename leaves no temp and no target", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_atomic_write");
    const fs::path to = t.content / "m" / "gold 1.arcmat";
    const fs::path tmp = fs::path(to).concat(".arctmp");
    fs::create_directories(to.parent_path());
    SECTION("the stream write fails part-way")
    {
        const auto failing = [](std::ostream& o, const std::string& text) { o.write(text.data(), 3); return false; };
        CHECK_FALSE(WriteTextFileAtomic(to, R"({ "id": "x" })", failing));
        CHECK_FALSE(fs::exists(tmp));
        CHECK_FALSE(fs::exists(to));
    }
    SECTION("the rename over the target fails")
    {
        fs::create_directories(to);   // a folder squats on the copy's name
        CHECK_FALSE(WriteTextFileAtomic(to, R"({ "id": "x" })"));
        CHECK_FALSE(fs::exists(tmp));
        CHECK(fs::is_directory(to));
    }
    SECTION("success: the target holds every byte, no temp")
    {
        REQUIRE(WriteTextFileAtomic(to, R"({ "id": "x" })"));
        CHECK(Arcane::Test::Slurp(to) == R"({ "id": "x" })");
        CHECK_FALSE(fs::exists(tmp));
    }
}

// T5-B9 review (owed at T5-GATE): the kinds the first every-kind case leaves out.
TEST_CASE("Duplicate: Data, Audio, Font and Model copies carry fresh ids and undo away", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_dup_more_kinds");
    t.Write("d/cfg.json", R"({"id":"7e5a8888-0001-4001-8001-000000000001","rate":3})");
    t.Write("a/hit.wav", "RIFF"); t.Write("a/hit.wav.meta", R"({"guid":"7e5a8888-0002-4002-8002-000000000002","version":1})");
    t.Write("f/ui.ttf", "ttf");   t.Write("f/ui.ttf.meta", R"({"guid":"7e5a8888-0003-4003-8003-000000000003","version":1})");
    t.Write("g/prop.gltf", R"({"asset":{"version":"2.0"}})");
    t.Write("g/prop.gltf.meta", R"({"guid":"7e5a8888-0004-4004-8004-000000000004","version":1,"scale":2})");
    t.Scan();
    const std::vector<std::string> rel = { "d/cfg.json", "a/hit.wav", "f/ui.ttf", "g/prop.gltf" };
    std::vector<Arcane::Guid> src;
    for (const std::string& r : rel) { src.push_back(t.GuidOf(r)); REQUIRE(src.back().IsValid()); }
    const auto before = t.Snapshot();
    AssetOpsTest::Host h(t); AssetFileOpExecutor exec(h, t.stack, t.content);
    const AssetOpPlan plan = PlanAssetOp({ .kind = AssetOpKind::Duplicate, .guids = src }, t.Facts());
    REQUIRE(plan.refusals.empty());
    REQUIRE(exec.Execute(plan, t.stack).ok);
    REQUIRE(plan.newGuids.size() == src.size());
    const std::vector<std::string> copies = { "d/cfg 1.json", "a/hit 1.wav", "f/ui 1.ttf", "g/prop 1.gltf" };
    for (std::size_t i = 0; i < src.size(); ++i)
    {
        INFO(copies[i]);
        CHECK(plan.newGuids[i] != src[i]);
        CHECK(Arcane::AssetRegistry::PeekId(t.content / copies[i]) == plan.newGuids[i]);   // the on-disk id
        CHECK(t.registry.Resolve(plan.newGuids[i]) == "game://" + copies[i]);
        CHECK(t.registry.Resolve(src[i]) == "game://" + rel[i]);
    }
    CHECK(nlohmann::json::parse(std::ifstream(t.content / "d/cfg 1.json"))["rate"] == 3);         // every other field stays
    CHECK(nlohmann::json::parse(std::ifstream(t.content / "g/prop 1.gltf.meta"))["scale"] == 2);
    CHECK(Arcane::Test::Slurp(t.content / "a/hit 1.wav") == "RIFF");
    t.stack.Undo();
    CHECK(t.Snapshot() == before);
    for (const Arcane::Guid& g : plan.newGuids) CHECK_FALSE(t.registry.Resolve(g).has_value());
}

// T5-B9 review (owed at T5-GATE): CopyForward's rollback -- a later copy failing undoes
// every earlier copy of the batch (files and registrations); nothing is pushed.
TEST_CASE("Duplicate: a copy failing mid-batch rolls back the earlier copies", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_dup_rollback");
    t.Write("m/gold.arcmat", R"({"id":"7e5a9999-0001-4001-8001-000000000001","name":"gold"})");
    t.Write("m/iron.arcmat", R"({"id":"7e5a9999-0002-4002-8002-000000000002","name":"iron"})");
    t.Scan();
    const std::vector<Arcane::Guid> src = { t.GuidOf("m/gold.arcmat"), t.GuidOf("m/iron.arcmat") };
    AssetOpsTest::Host h(t); AssetFileOpExecutor exec(h, t.stack, t.content);
    const AssetOpPlan plan = PlanAssetOp({ .kind = AssetOpKind::Duplicate, .guids = src }, t.Facts());
    REQUIRE((plan.refusals.empty() && plan.newGuids.size() == 2));
    SECTION("the second copy's write fails (its source went unreadable)")
    {
        t.Write("m/iron.arcmat", "not json");
    }
    SECTION("the second copy registers under another id")
    {
        h.registerOverride = [&](const fs::path& p) -> std::optional<Arcane::Guid>
        {
            if (p.filename() == "iron 1.arcmat") return Arcane::Guid::Generate();
            return t.registry.AddFile(p, t.content, "game");
        };
    }
    const auto before = t.Snapshot();
    CHECK_FALSE(exec.Execute(plan, t.stack).ok);
    CHECK(t.Snapshot() == before);                                  // gold 1 and iron 1 are gone
    CHECK_FALSE(t.registry.Resolve(plan.newGuids[0]).has_value());  // gold 1 unregistered
    CHECK_FALSE(t.registry.Resolve(plan.newGuids[1]).has_value());
    CHECK(t.registry.Resolve(src[0]) == "game://m/gold.arcmat");
    CHECK_FALSE(t.stack.CanUndo());
    CHECK(std::count(h.calls.begin(), h.calls.end(), "Error Duplicate 2 assets failed") == 1);
}
TEST_CASE("Duplicate: a scene copy re-mints every Identity id, leaves the original alone, and loads", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_dup_scene"); Arcane::Runtime rt{ Arcane::Test::Process() }; Arcane::RegisterSceneComponents(rt.Registry());
    Astra::Registry& reg = rt.Registry(); const Astra::Entity root = reg.CreateEntity();
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });   // SaveJson walks only the SceneRoot subtree (SceneJsonTest.cpp's pattern)
    for (const char* n : { "A", "B" })
    { const Astra::Entity e = reg.CreateEntity(); reg.AddComponent<Arcane::Identity>(e, Arcane::Identity{ Arcane::Guid::Generate(), n }); reg.SetParent(e, root); }
    nlohmann::json doc = Arcane::Scene::SaveJson(rt.Registry()); doc["id"] = Arcane::Guid::Generate().ToString(); t.Write("sc/lv.arcscene", doc.dump(2)); t.Scan();
    AssetOpsTest::Host h(t); AssetFileOpExecutor exec(h, t.stack, t.content);
    REQUIRE(exec.Execute(PlanAssetOp({ .kind = AssetOpKind::Duplicate, .guids = { t.GuidOf("sc/lv.arcscene") } }, t.Facts()), t.stack).ok);
    const auto his = [](const nlohmann::json& j) { std::vector<std::uint64_t> o; for (const auto& e : j["entities"])
        if (e["components"].contains("Arcane::Identity")) o.push_back(e["components"]["Arcane::Identity"]["id"]["hi"]); return o; };
    const auto orig = nlohmann::json::parse(std::ifstream(t.content / "sc/lv.arcscene")), copy = nlohmann::json::parse(std::ifstream(t.content / "sc/lv 1.arcscene"));
    CHECK(orig == doc); REQUIRE(his(copy).size() == 2);
    const std::vector<std::uint64_t> origHis = his(orig);   // ONE vector: begin()/end() of two temporaries is UB
    for (const std::uint64_t hi : his(copy)) CHECK(std::count(origHis.begin(), origHis.end(), hi) == 0);
    Arcane::Runtime rt2{ Arcane::Test::Process() }; Arcane::RegisterSceneComponents(rt2.Registry()); CHECK(Arcane::Scene::LoadJson(rt2.Registry(), copy));
}
namespace
{
    // png <-DerivesFrom- sprite <-References- main, lv; sliced References the png; inst DerivesFrom base.
    struct DeleteWorld
    {
        AssetOpsTest::Tree t{ "arcane_ops_delete_plan" }; AssetReferenceIndex index; Arcane::Guid tex, sprite, sliced, mainScene, lv, base, inst;
        DeleteWorld()
        {
            t.Write("t/uv_marker.png", "px"); int n = 1;
            for (const char* r : { "s/uv_marker.arcsprite", "s/sliced.arcsprite", "sc/main.arcscene", "sc/lv.arcscene", "m/base.arcmat", "m/inst.arcmat" })
                t.Write(r, "{\"id\":\"7e5a8000-0000-4000-8000-00000000000" + std::to_string(n++) + "\"}");
            t.Scan();
            tex = t.GuidOf("t/uv_marker.png"); sprite = t.GuidOf("s/uv_marker.arcsprite"); sliced = t.GuidOf("s/sliced.arcsprite");
            mainScene = t.GuidOf("sc/main.arcscene"); lv = t.GuidOf("sc/lv.arcscene"); base = t.GuidOf("m/base.arcmat"); inst = t.GuidOf("m/inst.arcmat");
            using K = Arcane::AssetRefKind; const auto e = [&](const Arcane::Guid& g, std::vector<Arcane::AssetRef> r) { index.Update(g, true, r); };
            e(tex, {}); e(base, {}); e(sprite, { { tex, K::DerivesFrom } }); e(sliced, { { tex, K::References } });
            e(mainScene, { { sprite, K::References } }); e(lv, { { sprite, K::References } }); e(inst, { { base, K::DerivesFrom } }); t.refs = &index;
        }
        static const AssetReferencer* Row(const DeleteAnalysis& a, const Arcane::Guid& g) { for (const auto& r : a.referencers) if (r.referencer == g) return &r; return nullptr; }
    };
}
TEST_CASE("Delete plan: a texture's plain sprite cascades; scenes one hop away; the sliced sprite is a referencer", "[editor][assetops]")
{
    DeleteWorld w; const std::vector<Arcane::Guid> req{ w.tex };
    const DeleteAnalysis on = AnalyzeDelete(req, true, w.t.Facts());
    CHECK(on.doomed == std::vector<Arcane::Guid>{ w.tex, w.sprite });
    REQUIRE(on.derived.size() == 1); CHECK((on.derived[0].child == w.sprite && on.derived[0].cascades));
    CHECK((DeleteWorld::Row(on, w.mainScene) && DeleteWorld::Row(on, w.lv) && DeleteWorld::Row(on, w.sliced) && !DeleteWorld::Row(on, w.sprite)));
    const DeleteAnalysis off = AnalyzeDelete(req, false, w.t.Facts());
    CHECK((off.doomed == std::vector<Arcane::Guid>{ w.tex } && DeleteWorld::Row(off, w.sprite) && DeleteWorld::Row(off, w.mainScene)));
    const AssetOpPlan offPlan = PlanAssetOp({ .kind = AssetOpKind::Delete, .guids = req, .cascadeDerived = false }, w.t.Facts());
    CHECK((offPlan.moves.size() == 1 && offPlan.derived.size() == 1 && !offPlan.derived[0].cascades));   // unticked: the child stays out of moves

    const std::vector<Arcane::Guid> both{ w.tex, w.sprite };   // the multi-select case: requested AND cascaded
    const DeleteAnalysis twice = AnalyzeDelete(both, true, w.t.Facts());
    CHECK(std::count(twice.doomed.begin(), twice.doomed.end(), w.sprite) == 1);
    const AssetOpPlan bp = PlanAssetOp({ .kind = AssetOpKind::Delete, .guids = both }, w.t.Facts());
    CHECK((bp.refusals.empty() && bp.moves.size() == 2));
}
TEST_CASE("Delete plan: live manifest, dirty documents, project manifest; instances list, never cascade; refusals; diag siblings", "[editor][assetops]")
{
    DeleteWorld w; w.t.openScene = w.mainScene; w.t.sceneAssets = { w.base };   // live only, absent from the saved scene
    w.t.docs = { { w.lv, true, { w.base }, "Level One" }, { w.sliced, false, { w.base }, "Sliced" } }; w.t.inputActions = w.base;
    const std::vector<Arcane::Guid> req{ w.base }; const DeleteAnalysis a = AnalyzeDelete(req, true, w.t.Facts());
    CHECK((a.doomed == std::vector<Arcane::Guid>{ w.base } && DeleteWorld::Row(a, w.inst) && !DeleteWorld::Row(a, w.sliced)));   // a CLEAN doc does not count
    CHECK(ReferencerTags(*DeleteWorld::Row(a, w.mainScene)) == "(open scene, unsaved)");
    CHECK(ReferencerTags(*DeleteWorld::Row(a, w.lv)) == "(unsaved in Level One)");   // spec s7.5: the document's title
    CHECK(ReferencerTags(*DeleteWorld::Row(a, Arcane::Guid{})) == "(project: input actions)");
    w.t.bootScene = w.lv;
    const std::vector<Arcane::Guid> bootReq{ w.lv }; const DeleteAnalysis boot = AnalyzeDelete(bootReq, true, w.t.Facts());
    REQUIRE(DeleteWorld::Row(boot, Arcane::Guid{})); CHECK(ReferencerTags(*DeleteWorld::Row(boot, Arcane::Guid{})) == "(project: boot scene)");
    CHECK_FALSE(PlanAssetOp({ .kind = AssetOpKind::Delete, .guids = { w.mainScene } }, w.t.Facts()).refusals.empty());   // open scene
    CHECK_FALSE(PlanAssetOp({ .kind = AssetOpKind::Delete, .guids = { w.lv } }, w.t.Facts()).refusals.empty());          // boot scene
    const auto dd = w.t.root / "Saved" / "Diagnostics"; std::filesystem::create_directories(dd);
    Arcane::Diag::Envelope env; env.guid = Arcane::Guid::Generate(); env.siblingTxt = "crash-1.txt"; REQUIRE(Arcane::Diag::WriteFile(env, dd / "crash-1.arcdiag"));
    std::ofstream(dd / "crash-1.txt") << "t"; std::ofstream(dd / "crash-1.symbolized.txt") << "s";
    CHECK(DiagSiblingFiles(dd / "crash-1.arcdiag").size() == 2);
}
// ---- T5-B13: the s7.12 follow-up table and tombstone names ----------------
namespace
{
    AssetOpPlan One(AssetOpKind k, AssetKind kind, std::filesystem::path from, std::filesystem::path to = {})
    { AssetOpPlan p; p.kind = k; p.moves.push_back({ Arcane::Guid::Generate(), kind, { { std::move(from), std::move(to) } } }); return p; }
    using Calls = std::vector<std::string>;
}
TEST_CASE("Follow-up: the s7.12 per-operation table as exact host call sets", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_followup"); AssetOpsTest::Host h(t); const auto c = t.content;
    AssetOpPlan del = One(AssetOpKind::Delete, AssetKind::Texture, c / "a.png");
    del.moves[0].files.push_back({ c / "a.png.meta", {} }); del.derived.push_back({ del.moves[0].guid, Arcane::Guid::Generate(), false, {} });
    RunAssetOpFollowUp(h, del, AssetOpSide::Forward, c);
    CHECK(h.calls == Calls{ "Invalidate 1", "Invalidate 6", "Evict 2", "AssetsChanged -1 +0", "Activity 4 restore from Recycle Bin" });
    h.calls.clear(); RunAssetOpFollowUp(h, One(AssetOpKind::Delete, AssetKind::Material, c / "m.arcmat"), AssetOpSide::Undo, c);
    CHECK(h.calls == Calls{ "Invalidate 0", "Evict 1", "AssetsChanged -0 +1", "Activity 3 restored (undo)" });
    h.calls.clear(); RunAssetOpFollowUp(h, One(AssetOpKind::Move, AssetKind::Material, c / "a.arcmat", c / "m/a.arcmat"), AssetOpSide::Forward, c);
    CHECK(h.calls == Calls{ "NoteMoved a.arcmat->a.arcmat", "Evict 2", "AssetsChanged -0 +0", "Activity 5 from game://a.arcmat" });
    h.calls.clear(); RunAssetOpFollowUp(h, One(AssetOpKind::Rename, AssetKind::Material, c / "a.arcmat", c / "b.arcmat"), AssetOpSide::Undo, c);
    CHECK(h.calls.front() == "NoteMoved b.arcmat->a.arcmat");   // undo runs to -> from
    AssetOpPlan dup = One(AssetOpKind::Duplicate, AssetKind::Sprite, c / "s.arcsprite", c / "s 1.arcsprite"); dup.newGuids = { Arcane::Guid::Generate() };
    h.calls.clear(); RunAssetOpFollowUp(h, dup, AssetOpSide::Forward, c);
    CHECK(h.calls == Calls{ "Evict 1", "AssetsChanged -0 +1", "Activity 3 duplicate of s.arcsprite" });
    h.calls.clear(); RunAssetOpFollowUp(h, dup, AssetOpSide::Undo, c);
    CHECK(h.calls == Calls{ "Invalidate 6", "Evict 1", "AssetsChanged -1 +0", "Activity 4 restore from Recycle Bin" });
    const OsShell::RecycleResult nuked{ true, {}, { c / "b.png" }, {} };
    h.calls.clear(); RunAssetOpFollowUp(h, One(AssetOpKind::Delete, AssetKind::Texture, c / "b.png"), AssetOpSide::Forward, c, &nuked);
    CHECK(h.calls.back() == "Activity 4 permanently; not in the Recycle Bin");
    AssetOpPlan nf; nf.kind = AssetOpKind::NewFolder; h.calls.clear(); RunAssetOpFollowUp(h, nf, AssetOpSide::Forward, c); CHECK(h.calls.empty());
}
TEST_CASE("TombstoneName: the snapshot after a delete, nullopt after its undo", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_tombstone"); AssetOpsTest::Host h(t); AssetActivityLog log;
    const AssetOpPlan p = One(AssetOpKind::Delete, AssetKind::Texture, t.content / "uv_marker.png");
    RunAssetOpFollowUp(h, p, AssetOpSide::Forward, t.content); for (const auto& e : h.activity) log.Push(e);
    CHECK(TombstoneName(log, p.moves[0].guid) == std::optional<std::string>("uv_marker.png"));
    h.activity.clear(); RunAssetOpFollowUp(h, p, AssetOpSide::Undo, t.content); for (const auto& e : h.activity) log.Push(e);
    CHECK_FALSE(TombstoneName(log, p.moves[0].guid));
}
TEST_CASE("DescribeDeleteModal: title, confirm wording and the unsaved line follow the plan", "[editor][assetops]")
{
    AssetOpPlan p; p.kind = AssetOpKind::Delete; p.moves.push_back({ Arcane::Guid::Generate(), AssetKind::Texture, { { "C:/p/Content/t/uv_marker.png", {} } } });
    const std::vector<Arcane::Guid> one{ p.moves[0].guid };
    DeleteModalText t = DescribeDeleteModal(p, one, {});
    CHECK((t.title == "Delete uv_marker.png?" && t.confirm == "Delete" && t.unsaved.empty()));
    CHECK(t.footer == "Files go to the Recycle Bin. Ctrl+Z restores them while this session's undo history lasts.");
    p.referencers.push_back({ p.moves[0].guid, Arcane::Guid::Generate(), { RefSource::AssetOnDisk }, "main.arcscene" });
    CHECK(DescribeDeleteModal(p, one, {}).confirm == "Delete anyway");
    const std::vector<std::string> dirty{ "UvMarkerSprite" }; t = DescribeDeleteModal(p, one, dirty);
    CHECK((t.confirm == "Discard changes and delete" && t.unsaved == "Unsaved changes in UvMarkerSprite will be discarded."));
    p.moves.resize(3, p.moves[0]);
    const std::vector<Arcane::Guid> three{ one[0], Arcane::Guid::Generate(), Arcane::Guid::Generate() };
    CHECK(DescribeDeleteModal(p, three, {}).title == "Delete 3 assets?");
}
TEST_CASE("DescribeDeleteModal titles the REQUESTED assets; cascaded children only add doomed rows", "[editor][assetops]")
{
    const Arcane::Guid tex = Arcane::Guid::Generate(), sprite = Arcane::Guid::Generate();
    AssetOpPlan p; p.kind = AssetOpKind::Delete;
    p.moves.push_back({ tex, AssetKind::Texture, { { "C:/p/Content/t/uv_marker.png", {} } } });
    p.moves.push_back({ sprite, AssetKind::Sprite, { { "C:/p/Content/t/uv_marker.arcsprite", {} } } });
    p.derived = { { tex, sprite, true, {} } };
    const std::vector<Arcane::Guid> texOnly{ tex }, both{ tex, sprite }, missing{ Arcane::Guid::Generate() };
    CHECK(DescribeDeleteModal(p, texOnly, {}).title == "Delete uv_marker.png?");
    CHECK(DescribeDeleteModal(p, both, {}).title == "Delete 2 assets?");
    CHECK(DescribeDeleteModal(p, missing, {}).title == "Delete uv_marker.png?");   // a refused/absent guid falls back to the first doomed file
    const AssetOpPlan empty;
    CHECK(DescribeDeleteModal(empty, missing, {}).title == "Delete " + missing[0].ToString() + "?");
}
TEST_CASE("Move: a .gltf takes its .bin and its registered image (own guid, .meta); undo returns all", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_move_gltf");
    t.Write("m/ship.gltf", R"({"buffers":[{"uri":"ship%20a.bin"}],"images":[{"uri":"tex.png"},{"uri":"data:image/png;base64,AA=="}]})");
    t.Write("m/ship a.bin", "b"); t.Write("m/tex.png", "px"); t.Scan(); const Arcane::Guid tex = t.GuidOf("m/tex.png");
    AssetOpsTest::Host h(t); AssetFileOpExecutor exec(h, t.stack, t.content);
    const AssetOpPlan plan = PlanAssetOp({ .kind = AssetOpKind::Move, .guids = { t.GuidOf("m/ship.gltf") }, .destFolder = "hulls" }, t.Facts());
    REQUIRE(plan.refusals.empty()); REQUIRE(exec.Execute(plan, t.stack).ok);
    CHECK((std::filesystem::exists(t.content / "hulls/ship a.bin") && std::filesystem::exists(t.content / "hulls/tex.png.meta")));
    CHECK(t.registry.Resolve(tex) == std::optional<std::string>("game://hulls/tex.png"));
    t.stack.Undo(); CHECK((std::filesystem::exists(t.content / "m/ship a.bin") && t.registry.Resolve(tex) == std::optional<std::string>("game://m/tex.png")));
}
TEST_CASE("Move refusals: a ../ URI, a buffer shared with an unmoved .gltf, a destination taken only by a .meta", "[editor][assetops]")
{
    AssetOpsTest::Tree t("arcane_ops_move_refuse");
    t.Write("a/out.gltf", R"({"buffers":[{"uri":"../shared.bin"}]})");
    t.Write("b/one.gltf", R"({"buffers":[{"uri":"s.bin"}]})"); t.Write("b/two.gltf", R"({"buffers":[{"uri":"s.bin"}]})"); t.Write("b/s.bin", "b");
    t.Write("c/x.png", "px"); t.Write("d/x.png.meta", R"({"guid":"7e5a9999-0001-4001-8001-000000000001","version":1})"); t.Scan();
    const auto ref = [&](const char* rel, const char* dest) { return PlanAssetOp({ .kind = AssetOpKind::Move, .guids = { t.GuidOf(rel) }, .destFolder = dest }, t.Facts()).refusals; };
    const auto r1 = ref("a/out.gltf", "z"); REQUIRE(r1.size() == 1); CHECK(r1[0].reason == "References ../shared.bin outside its folder.");
    const auto r2 = ref("b/one.gltf", "z"); REQUIRE(r2.size() == 1); CHECK(r2[0].reason == "Shares s.bin with two.gltf.");
    CHECK_FALSE(ref("c/x.png", "d").empty());   // s7.1's "already exists" row (.meta only)
}
TEST_CASE("Move: two co-moving .gltf files sharing a buffer carry it once; undo returns it once", "[editor][assetops]")
{
    // T5-A8's batch de-dup (carry ruling): s7.1 refuses only a buffer shared with a NON-moving .gltf.
    AssetOpsTest::Tree t("arcane_ops_move_gltf_shared");
    t.Write("b/one.gltf", R"({"buffers":[{"uri":"s.bin"}]})"); t.Write("b/two.gltf", R"({"buffers":[{"uri":"s.bin"}]})"); t.Write("b/s.bin", "b"); t.Scan();
    AssetOpsTest::Host h(t); AssetFileOpExecutor exec(h, t.stack, t.content);
    const AssetOpPlan plan = PlanAssetOp({ .kind = AssetOpKind::Move, .guids = { t.GuidOf("b/one.gltf"), t.GuidOf("b/two.gltf") }, .destFolder = "z" }, t.Facts());
    REQUIRE(plan.refusals.empty());
    std::size_t binMoves = 0;
    for (const auto& m : plan.moves) for (const auto& f : m.files) if (f.from.filename() == "s.bin") ++binMoves;
    CHECK(binMoves == 1);
    REQUIRE(exec.Execute(plan, t.stack).ok);
    CHECK((std::filesystem::exists(t.content / "z/one.gltf") && std::filesystem::exists(t.content / "z/two.gltf") && std::filesystem::exists(t.content / "z/s.bin")));
    CHECK_FALSE(std::filesystem::exists(t.content / "b/s.bin"));
    t.stack.Undo();
    CHECK((std::filesystem::exists(t.content / "b/one.gltf") && std::filesystem::exists(t.content / "b/two.gltf") && std::filesystem::exists(t.content / "b/s.bin")));
    CHECK_FALSE(std::filesystem::exists(t.content / "z/s.bin"));
}
