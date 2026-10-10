// E1: the EDITOR witness for the play-mode topologies (Core-DLL split, plan 1
// Task 7). Same harness as the runtime/server witnesses (Helpers/HostWitness.hpp),
// same fresh-copy hygiene -- but [witness][gpu], not [witness][server]: this host
// builds a real graphics device even under --headless (the offscreen chrome
// vehicle), so it belongs with the other [gpu] scenarios and is INVISIBLE to a
// `~[gpu]` run by design. The unfiltered suite is where it runs.
#include "Helpers/GpuCapability.hpp"   // kNativeBackendCli: dx12 on Windows, vulkan elsewhere
#include "Helpers/HostWitness.hpp"
#include <Arcane/Platform/Platform.hpp>   // ExecutableFileName: .exe on Windows only
#include "Helpers/ReferenceProjectDir.hpp"
#include "Helpers/UserDataDirs.hpp"
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Material/MaterialAsset.hpp>   // E9: the fixture's node id
#include <Arcane/Material/MaterialGraph.hpp>
#include <Panels/DefaultLayout.hpp>   // the default layout's pixel targets (E6/E7): editor.layout.factory.* defaults
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
using namespace Arcane::Test;
namespace
{
    // ---- imgui.ini [Docking][Data] reading, for the layout witnesses (E6/E7) ----
    // One row per "DockSpace"/"DockNode" line: its id, parent, SizeRef (the
    // root carries Size), Split axis and CentralNode flag, plus the raw line.
    struct DockRow
    {
        std::uint32_t id = 0, parent = 0;
        float w = 0.0f, h = 0.0f;
        char split = 0;
        bool central = false;
        std::string line;
    };
    std::string IniField(const std::string& line, const char* key)
    {
        const std::string needle = std::string(" ") + key + "=";
        std::size_t p = line.find(needle);
        if (p == std::string::npos) return {};
        p += needle.size();
        const std::size_t e = line.find(' ', p);
        return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }
    std::vector<DockRow> ParseDockRows(const std::string& ini)
    {
        std::vector<DockRow> rows;
        std::istringstream in(ini);
        std::string line;
        bool inDocking = false;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == '[') { inDocking = line == "[Docking][Data]"; continue; }
            if (!inDocking) continue;
            const std::size_t t = line.find_first_not_of(' ');
            if (t == std::string::npos) continue;
            if (line.compare(t, 8, "DockNode") != 0 && line.compare(t, 9, "DockSpace") != 0) continue;
            DockRow r;
            r.line = line;
            r.id = static_cast<std::uint32_t>(std::stoul(IniField(line, "ID"), nullptr, 16));
            if (const std::string par = IniField(line, "Parent"); !par.empty())
                r.parent = static_cast<std::uint32_t>(std::stoul(par, nullptr, 16));
            std::string size = IniField(line, "SizeRef");
            if (size.empty()) size = IniField(line, "Size");
            if (const std::size_t comma = size.find(','); comma != std::string::npos)
            {
                r.w = std::stof(size.substr(0, comma));
                r.h = std::stof(size.substr(comma + 1));
            }
            if (const std::string sp = IniField(line, "Split"); !sp.empty()) r.split = sp[0];
            r.central = IniField(line, "CentralNode") == "1";
            rows.push_back(r);
        }
        return rows;
    }
    const DockRow* FindRow(const std::vector<DockRow>& rows, std::uint32_t id)
    {
        for (const DockRow& r : rows) if (r.id == id) return &r;
        return nullptr;
    }
    // The [Window][<name>] section's DockId (0 = none / no section).
    std::uint32_t WindowDockId(const std::string& iniText, const std::string& name)
    {
        // LINE-anchored (the committed seed's header comment names sections
        // too); the leading newline lets a section on the file's first line match.
        const std::string ini = "\n" + iniText;
        const std::size_t at = ini.find("\n[Window][" + name + "]");
        if (at == std::string::npos) return 0;
        const std::size_t end = ini.find("\n[", at + 1);
        const std::size_t d = ini.find("DockId=0x", at);
        if (d == std::string::npos || (end != std::string::npos && d > end)) return 0;
        return static_cast<std::uint32_t>(std::stoul(ini.substr(d + 9, 8), nullptr, 16));
    }
    std::string Hex8(std::uint32_t v)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X", v);
        return buf;
    }

    std::string ReadAllBytes(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::filesystem::path StagedEditorDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneEditor");
        {
            INFO("staged ArcaneEditor not found -- build Arcane.slnx first: " << p.string());
            REQUIRE(std::filesystem::exists(p / Arcane::Platform::ExecutableFileName("ArcaneEditor")));
        }

        // Every editor witness runs --headless, which pins the layout to the
        // STAGED Saved/verify-layout.ini. That copy is refreshed only when
        // ArcaneEditor's postbuild runs (or golden-gate.ps1 restages it), so a
        // seed edit made after the last editor link leaves the witnesses on
        // the OLD seed while they report green (inspector filters Task 10, fix
        // round 1: measured, the Debug witnesses ran the 2026-09-10 seed).
        // Refuse rather than test the wrong input.
        const std::filesystem::path sourceProject = Arcane::Test::FindReferenceProjectDir();
        REQUIRE_FALSE(sourceProject.empty());
        const std::filesystem::path sourceSeed = sourceProject / "Saved" / "verify-layout.ini";
        const std::filesystem::path stagedSeed = p / "ReferenceProject" / "Saved" / "verify-layout.ini";
        INFO("the staged layout seed is stale -- rebuild ArcaneEditor (its postbuild restages it) "
             "or run scripts/golden-gate.ps1: " << stagedSeed.string() << " != " << sourceSeed.string());
        REQUIRE(std::filesystem::exists(sourceSeed));
        REQUIRE(std::filesystem::exists(stagedSeed));
        const bool stagedSeedMatchesSource = ReadAllBytes(stagedSeed) == ReadAllBytes(sourceSeed);
        REQUIRE(stagedSeedMatchesSource);   // a bool, so a failure prints the message, not two whole files
        return p;
    }
}
TEST_CASE("E1: the editor stands up client + embedded server through --play-as and reports both worlds", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e1-embedded-server");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "vulkan", "--frames", "60",
                 "--report", inv.reportPath.generic_string(), "--play-as", "embedded-server" };
    inv.hardCapMs = 120000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    const auto& worlds = run.report.at("worlds");
    REQUIRE(worlds.size() == 2);
    CHECK(worlds[0].at("role") == "Client");           CHECK(worlds[0].at("hasAuthority") == false);
    CHECK(worlds[1].at("role") == "DedicatedServer");  CHECK(worlds[1].at("hasAuthority") == true);
    CHECK(worlds[0].at("entities") == worlds[1].at("entities"));   // the same scene in both
}

// E2: the PERSPECTIVE editor witness (F4 plan 1 T12, spec s9). The editor
// booted with --view-mode perspective on ReferenceProject renders the cube on
// the XZ grid through the Orbit3D camera, settles, and compares against its
// OWN golden slot (editor-ui-perspective.png -- --compare takes any safe name,
// so the slot needs no registry beyond the PNG). Two facts are asserted, not
// one: the report's `viewMode` is what the host actually resolved the camera
// to (a "perspective" the seed failed to apply would still exit 0 with a 2D
// picture), and the compare PASSED against the blessed slot (which is what
// makes the new golden load-bearing -- two of golden-gate.ps1's lanes (dx12
// and vulkan) name it; that table is count-derived, so this comment names no
// total). Same [witness][gpu] posture as E1: outside ~[gpu], run unfiltered.
TEST_CASE("E2: the editor boots into perspective on --view-mode, reports viewMode, and matches the perspective golden", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e2-perspective");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "60",
                 "--settle", "30", "--report", inv.reportPath.generic_string(),
                 "--view-mode", "perspective", "--compare", "editor-ui-perspective" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    CHECK(run.report.at("exitReason") == "frames-complete");
    REQUIRE(run.report.contains("viewMode"));           // schema 7: absent on a host that has no view mode
    CHECK(run.report.at("viewMode") == "perspective");
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("reference") == "editor-ui-perspective");
    CHECK(run.report["compare"].at("passed") == true);
    CHECK(run.report["compare"].at("diffCount") == 0);

    // THE VISIBILITY BLOCK (F3 spec s9.2, plan 2 T6). s9.2 asks the witness
    // lanes to assert the block for ReferenceProject's 3D scene, and until
    // this task nothing did: a host could have reported zeros, or a
    // `gpuVisible` quietly copied from the coarse count, and every lane would
    // still have been green. This is the EDITOR half -- the runtime half is
    // WitnessScenariosTest.cpp's W5, against the F3 fixture scene.
    //
    // The numbers are the BOOT SCENE's, derived from its own content and not
    // from a previous run's output: main.arcscene carries two MeshRenderers
    // (MeshCube -> reference_cube.arcmesh, one section; GoldenProp ->
    // golden_prop.arcmesh, three sections), so four (entity, section) rows,
    // all four inside the perspective view, each with its own batch key
    // (distinct mesh/section pairs, all opaque) and therefore its own indirect
    // draw. No transparent material appears in that scene at all, which is
    // exactly why the F3 fixture scene exists beside it.
    REQUIRE(run.report.contains("visibility"));
    const auto& vis = run.report["visibility"];
    CHECK(vis.at("total") == 4);
    CHECK(vis.at("coarseVisible") == 4);
    CHECK(vis.at("batches") == 4);
    CHECK(vis.at("draws") == 4);
    CHECK(vis.at("transparentRows") == 0);
    // gpuVisible is the CULL PASS's OWN count, read back asynchronously and
    // `null` until one retires (schemaVersion 9). Asserting it is a NUMBER is
    // the load-bearing half: a delayed ring that never publishes inside this
    // run's --frames 60 / --settle 30 budget would leave null here, and the
    // right answer to that is a bigger budget, never a fabricated number.
    REQUIRE(vis.at("gpuVisible").is_number_unsigned());
    // ...and it agrees with the coarse count ON THIS SCENE, where every row is
    // opaque and inside the frustum. That identity is NOT general -- a scene
    // with transparent rows has fewer gpuVisible than coarseVisible, since
    // transparent keys are never emitted to the cull (W5 pins that side).
    CHECK(vis.at("gpuVisible") == vis.at("coarseVisible"));
}

// E3: the INPUT DOCUMENT witness (inspector-ownership spec s5 / input-editor
// spec s4). --open-asset puts Player.arcinput on screen, --select-in-document
// selects Player/Jump inside it, and the report's `inspector` block says the
// document -- not the scene -- owns the Inspector, with the breadcrumb a
// person would read. Compared against its own golden slot once blessed.
TEST_CASE("E3: an opened input document with a scripted selection owns the Inspector and matches the editor-input-doc golden", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e3-input-doc");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "90",
                 "--settle", "10", "--report", inv.reportPath.generic_string(),
                 "--open-asset", "97260310-8b35-4b29-b12f-1fd6f8e99071",
                 "--select-in-document", "Player/Jump", "--compare", "editor-input-doc" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"].at("source") == "Player.arcinput");
    CHECK(run.report["inspector"].at("breadcrumb") == "Player.arcinput > Player > Jump");
    // inspector.instances (schemaVersion 12, inspector filters s9): the default
    // layout's two instances -- the main one excludes only Assets, the second
    // is Assets-only (it excludes every other catalog kind, catalog order).
    REQUIRE(run.report["inspector"].contains("instances"));
    const auto& inst = run.report["inspector"]["instances"];
    REQUIRE(inst.size() == 2);
    CHECK(inst[0].at("id") == 0);
    CHECK(inst[0].at("excluded") == nlohmann::json::array({ "assets" }));
    CHECK(inst[1].at("id") == 1);
    CHECK(inst[1].at("excluded") == nlohmann::json::array({ "scene", "input-actions", "material", "sprite", "mesh" }));
    // What each instance SHOWS (final fix W): the main one the document's
    // page, the Assets-only one the Asset Browser with nothing selected.
    CHECK(inst[0].at("source") == "Player.arcinput");
    CHECK(inst[0].at("breadcrumb") == "Player.arcinput > Player > Jump");
    CHECK(inst[1].at("source") == "Assets");
    CHECK(inst[1].at("breadcrumb") == "Assets");
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("passed") == true);
}

// E4: the ASSET PAGE witness (inspector filters s6). --select-asset selects
// uv_marker.png in the Asset Browser the way a click does; the Assets-only
// Inspector shows its page while the main (All but Assets) one stays on the scene.
TEST_CASE("E4: a selected asset routes the Assets Inspector to its page and matches the editor-asset-page golden", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e4-asset-page");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    // --frames 60 --settle 30, E2's and the gate lane's values, NOT E3's 90/10:
    // the viewport is visible in this capture, and the frozen PulseBox phase
    // depends on the frame count, so the witness must render the frame the
    // shared editor-asset-page slot was blessed at (a 90-frame run converges
    // on a picture 4660 px off the golden, all of them the PulseBox).
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "60",
                 "--settle", "30", "--report", inv.reportPath.generic_string(),
                 "--select-asset", "d7f389fd-f687-407d-b9d7-9753eb6b0258",   // textures/uv_marker.png
                 "--compare", "editor-asset-page" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"].at("source") == "Assets");                  // Current(): the asset selection
    CHECK(run.report["inspector"].at("breadcrumb") == "Assets > uv_marker.png");
    REQUIRE(run.report["inspector"].contains("instances"));
    CHECK(run.report["inspector"]["instances"][0].at("source") == "Scene");   // All but Assets: untouched
    CHECK(run.report["inspector"]["instances"][1].at("source") == "Assets");
    // The asset page is what Inspector 2 SHOWS, not only where it routed (final fix W).
    CHECK(run.report["inspector"]["instances"][1].at("breadcrumb") == "Assets > uv_marker.png");
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("passed") == true);
}

// E5: DOCUMENT PAGES (inspector filters s6a). Opening a material, a sprite or a
// mesh selects that document's page: the main Inspector routes to it, and the
// Assets-only Inspector is untouched.
TEST_CASE("E5: opening a material, a sprite or a mesh routes the main Inspector to that document's page", "[witness][gpu]")
{
    struct Doc { const char* guid; const char* title; const char* compare; const char* compile; };
    const Doc docs[] = {
        { "7e5a0010-0010-4010-8010-000000000010", "ReferenceCubeMaterial", "editor-material-page", "not-compiled-here" },   // materials/reference_mesh.arcmat
        { "87bd4fd3-c9e6-4fc8-a8e0-cf799378f049", "UvMarkerSprite",        nullptr,                nullptr },             // sprites/uv_marker.arcsprite
        { "7e5a0011-0011-4011-8011-000000000011", "ReferenceCube",         nullptr,                nullptr },             // meshes/reference_cube.arcmesh
    };
    for (const Doc& d : docs)
    {
        DYNAMIC_SECTION(d.title)
        {
            WitnessScratch scratch(StagedEditorDir(), std::string("e5-") + d.title);
            WitnessInvocation inv;
            inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
            inv.reportPath = scratch.Dir() / "witness-report.json";
            inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "90",
                         "--settle", "10", "--report", inv.reportPath.generic_string(),
                         "--open-asset", d.guid };
            if (d.compare) { inv.args.push_back("--compare"); inv.args.push_back(d.compare); }
            inv.hardCapMs = 180000;
            WitnessRun run = RunWitness(inv);
            INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
            REQUIRE_FALSE(GradeProcessFacts(run).has_value());
            REQUIRE(run.exitCode == 0);
            REQUIRE(run.report.contains("inspector"));
            REQUIRE(run.report["inspector"].contains("instances"));
            CHECK(run.report["inspector"]["instances"][0].at("source") == d.title);
            CHECK(run.report["inspector"]["instances"][1].at("source") == "Assets");   // untouched by a document open
            CHECK(run.report["inspector"]["instances"][1].at("breadcrumb") == "Assets");   // ...and still showing no selection
            // s3.2: a mesh-surface material is never compiled in the editor, and
            // the report says so instead of a permanent "compiling...".
            if (d.compile)
            {
                REQUIRE(run.report.contains("documents"));
                REQUIRE(run.report["documents"].size() == 1);
                CHECK(run.report["documents"][0].at("compile") == d.compile);
            }
            if (d.compare)
            {
                REQUIRE(run.report.contains("compare"));
                CHECK(run.report["compare"].at("passed") == true);
            }
        }
    }
}

// E9: the NODE PAGE (spec 2026-09-30 s5.1.11). --select-in-document on the
// graph-owned fixture selects its Multiply node: the main Inspector shows the
// node page, the Assets-only Inspector is untouched, and the document compiled
// and previews. No --compare: this spec adds no node-page golden slot.
TEST_CASE("E9: --select-in-document on a shader node routes the main Inspector to the node page", "[witness][gpu]")
{
    // The node id comes from the fixture itself, so the witness never hard-codes it.
    const auto fixture = Arcane::LoadMaterialAsset(FindReferenceProjectDir() / "Content" / "materials" / "node_page_graph.arcmat");
    REQUIRE(fixture.has_value());
    REQUIRE(fixture->graph.has_value());
    std::uint32_t mulId = 0;
    for (const Arcane::GraphNode& n : fixture->graph->nodes)
        if (n.type == Arcane::GraphNodeType::Mul) mulId = n.id;
    REQUIRE(mulId != 0);

    WitnessScratch scratch(StagedEditorDir(), "e9-node-page");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "90",
                 "--settle", "10", "--report", inv.reportPath.generic_string(),
                 "--open-asset", "7e5a0012-0012-4012-8012-000000000012",
                 "--select-in-document", std::to_string(mulId) };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"]["instances"][0].at("breadcrumb") == "NodePageGraph > Multiply");
    CHECK(run.report["inspector"]["instances"][1].at("source") == "Assets");
    CHECK(run.report["inspector"]["instances"][1].at("breadcrumb") == "Assets");
    REQUIRE(run.report.contains("documents"));
    bool found = false;
    for (const auto& d : run.report["documents"])
    {
        if (d.at("name") != "NodePageGraph") continue;
        found = true;
        CHECK(d.at("compile") == "ok");
        CHECK(d.at("preview") == "ready");
        CHECK(d.at("image") == true);
    }
    CHECK(found);
}

// E3b: an unresolvable --select-in-document is loud (ERROR on stderr) but not
// fatal; the Inspector keeps the document's opening selection (first map +
// first action of Player.arcinput). No --compare: no golden slot for this state.
TEST_CASE("E3b: an unresolvable --select-in-document is a loud ERROR, the run completes, and the Inspector keeps the document's opening selection", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e3b-input-doc-unresolved");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "60",
                 "--report", inv.reportPath.generic_string(),
                 "--open-asset", "97260310-8b35-4b29-b12f-1fd6f8e99071",
                 "--select-in-document", "Player/NoSuchAction" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    CHECK(run.report.at("exitReason") == "frames-complete");
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"].at("source") == "Player.arcinput");
    CHECK(run.report["inspector"].at("breadcrumb") == "Player.arcinput > Player > Move");
    CHECK_FALSE(run.report.contains("compare"));
    std::ifstream err(run.stderrPath);
    const std::string all((std::istreambuf_iterator<char>(err)), std::istreambuf_iterator<char>());
    CHECK(all.find("--select-in-document 'Player/NoSuchAction': the opened document has no such path") != std::string::npos);
}

// E6: THE DEFAULT LAYOUT (USER DECISION 2026-09-30; final fix W). With the
// scratch copy's seed removed, a --headless run falls back to
// BuildDefaultLayout (the seed-less branch of RetargetLayoutIni): the dumped
// dock tree must be the user's ReferenceProject layout at the headless
// 1280x720 -- the main Inspector a full-height right column, the Outliner
// top-left beside the central Viewport (pixel targets), and the asset/console
// band under the Outliner up to the Inspector with Inspector 2 at its right
// end (the user's 1144 : 392 proportion, the band's split being ratio-shared) --
// and the picture must match editor-ui, whose seed IS that default.
TEST_CASE("E6: with no layout seed the editor builds the default layout -- the user's ReferenceProject layout -- and matches editor-ui", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e6-default-layout");
    std::filesystem::remove(scratch.Dir() / "ReferenceProject" / "Saved" / "verify-layout.ini");   // THIS copy only
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    const std::filesystem::path dump = scratch.Dir() / "dumped-layout.ini";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "60",
                 "--settle", "30", "--report", inv.reportPath.generic_string(),
                 "--dump-layout", dump.generic_string(), "--compare", "editor-ui" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("inspector"));
    const auto& inst = run.report["inspector"]["instances"];
    REQUIRE(inst.size() == 2);
    CHECK(inst[0].at("excluded") == nlohmann::json::array({ "assets" }));
    CHECK(inst[1].at("excluded") == nlohmann::json::array({ "scene", "input-actions", "material", "sprite", "mesh" }));

    REQUIRE(std::filesystem::exists(dump));
    const std::string ini = ReadAllBytes(dump);
    INFO("dumped layout:\n" << ini);
    const std::vector<DockRow> rows = ParseDockRows(ini);
    REQUIRE_FALSE(rows.empty());
    const DockRow& root = rows.front();
    CHECK(root.split == 'X');
    constexpr float kTol = 3.0f;
    // The main Inspector: a direct child of the root (full height), ~380 px.
    const DockRow* insp = FindRow(rows, WindowDockId(ini, "Inspector"));
    REQUIRE(insp != nullptr);
    CHECK(insp->parent == root.id);
    CHECK(std::abs(insp->w - Arcane::Editor::LayoutFactorySettings{}.inspectorWidth) <= kTol);
    // Left of it: the block split top/bottom.
    const DockRow* leftBlock = nullptr;
    for (const DockRow& r : rows) if (r.parent == root.id && r.id != insp->id) leftBlock = &r;
    REQUIRE(leftBlock != nullptr);
    CHECK(leftBlock->split == 'Y');
    // Top: the Outliner (~270 px) beside the central node, which holds the Viewport.
    const DockRow* outliner = FindRow(rows, WindowDockId(ini, "Outliner"));
    const DockRow* viewport = FindRow(rows, WindowDockId(ini, "Viewport"));
    REQUIRE(outliner != nullptr);
    REQUIRE(viewport != nullptr);
    CHECK(std::abs(outliner->w - Arcane::Editor::LayoutFactorySettings{}.outlinerWidth) <= kTol);
    CHECK(viewport->central);
    CHECK(outliner->parent == viewport->parent);
    const DockRow* top = FindRow(rows, outliner->parent);
    REQUIRE(top != nullptr);
    CHECK(top->parent == leftBlock->id);
    // Bottom: ONE tab node for the five asset/console panels, Inspector 2 on its right.
    const std::uint32_t browserId = WindowDockId(ini, "Asset Browser");
    for (const char* tab : { "Asset Graph", "Asset Status", "Console", "Problems" })
    {
        INFO(tab);
        CHECK(WindowDockId(ini, tab) == browserId);
    }
    const DockRow* browser = FindRow(rows, browserId);
    const DockRow* assetsInsp = FindRow(rows, WindowDockId(ini, "inspector_1"));
    REQUIRE(browser != nullptr);
    REQUIRE(assetsInsp != nullptr);
    CHECK(browser->parent == assetsInsp->parent);
    const DockRow* band = FindRow(rows, browser->parent);
    REQUIRE(band != nullptr);
    CHECK(band->parent == leftBlock->id);                       // under the Outliner, left of the Inspector
    CHECK(std::abs(band->h - Arcane::Editor::LayoutFactorySettings{}.bottomBand) <= kTol);
    // The band's browser | Inspector 2 split has no central node, so ImGui
    // re-divides it by the children's SizeRef RATIO on resize: the SizeRefs
    // must carry the user's 1920-scale proportion (1144 : 392), not a pixel
    // target taken at this 1280x720 build (integration residual 2a).
    const float bandShare = assetsInsp->w / (browser->w + assetsInsp->w);
    INFO("band SizeRef browser " << browser->w << " : Inspector 2 " << assetsInsp->w);
    CHECK(std::abs(bandShare - Arcane::Editor::DefaultAssetsInspectorBandFraction(Arcane::Editor::LayoutFactorySettings{})) <= 0.005f);
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("passed") == true);          // the seed IS this default
}

// E7: THE ONE-TIME LEGACY UPGRADE (inspector filters s6; final fix W). The
// scratch copy's seed is turned into a PRE-FEATURE layout -- no Filters= line,
// no extra instance, no [Window][inspector_1], the band's browser|Inspector 2
// split folded back into one browser node -- and the run must upgrade it once:
// instance 0 All but Assets, an Assets-only instance at slot 1, docked in a
// split right of the Asset Browser's node at the default band's proportion
// (1144 : 392 -- the split is ratio-shared, integration residual 2a).
TEST_CASE("E7: a pre-feature layout seed (no Filters=) is upgraded once -- Inspector 2 splits right of the Asset Browser at the default proportion", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e7-legacy-upgrade");
    const std::filesystem::path seedPath = scratch.Dir() / "ReferenceProject" / "Saved" / "verify-layout.ini";
    std::string seed = ReadAllBytes(seedPath);
    // A text=auto checkout under core.autocrlf stages the seed CRLF, and every
    // edit below searches LF-anchored patterns ("\nIds=1\n", "\n[Window]..."):
    // normalise THIS copy to LF first (ImGui's ini reader takes either).
    std::erase(seed, '\r');
    {
        // Fold the band's split: drop the browser and Inspector 2 leaves, make
        // their parent the browser's node, retarget the tabs' DockIds to it.
        const std::vector<DockRow> rows = ParseDockRows(seed);
        const std::uint32_t a = WindowDockId(seed, "inspector_1");
        const std::uint32_t b = WindowDockId(seed, "Asset Browser");
        const DockRow* ra = FindRow(rows, a);
        const DockRow* rb = FindRow(rows, b);
        REQUIRE(ra != nullptr);
        REQUIRE(rb != nullptr);
        REQUIRE(ra->parent == rb->parent);
        const DockRow* rp = FindRow(rows, ra->parent);
        REQUIRE(rp != nullptr);
        auto eraseLine = [&](const std::string& line)
        {
            const std::size_t at = seed.find(line);
            REQUIRE(at != std::string::npos);
            std::size_t end = seed.find('\n', at);
            seed.erase(at, end == std::string::npos ? std::string::npos : end - at + 1);
        };
        eraseLine(ra->line);
        eraseLine(rb->line);
        std::string parentLine = rp->line;
        const std::size_t split = parentLine.find(" Split=X");
        REQUIRE(split != std::string::npos);
        parentLine.erase(split, 8);
        seed.replace(seed.find(rp->line), rp->line.size(), parentLine);
        const std::string from = "DockId=0x" + Hex8(b) + ",", to = "DockId=0x" + Hex8(rp->id) + ",";
        for (std::size_t at = seed.find(from); at != std::string::npos; at = seed.find(from, at + to.size()))
            seed.replace(at, from.size(), to);
        // No [Window][inspector_1], no extra instance, no Filters= line.
        const std::size_t w = seed.find("\n[Window][inspector_1]");   // line-anchored: the header comment names it too
        REQUIRE(w != std::string::npos);
        seed.erase(w + 1, seed.find("\n[", w + 1) - w);
        const std::size_t ids = seed.find("\nIds=1\n");
        REQUIRE(ids != std::string::npos);
        seed.replace(ids, 7, "\nIds=\n");
        const std::size_t f = seed.find("\nFilters=");
        REQUIRE(f != std::string::npos);
        seed.erase(f + 1, seed.find('\n', f + 1) - f);
        std::ofstream(seedPath, std::ios::binary | std::ios::trunc) << seed;   // THIS copy only
    }
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    const std::filesystem::path dump = scratch.Dir() / "dumped-layout.ini";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "60",
                 "--report", inv.reportPath.generic_string(), "--dump-layout", dump.generic_string() };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    INFO("edited seed:\n" << seed);
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("inspector"));
    const auto& inst = run.report["inspector"]["instances"];
    REQUIRE(inst.size() == 2);
    CHECK(inst[0].at("id") == 0);
    CHECK(inst[0].at("excluded") == nlohmann::json::array({ "assets" }));
    CHECK(inst[1].at("id") == 1);
    CHECK(inst[1].at("excluded") == nlohmann::json::array({ "scene", "input-actions", "material", "sprite", "mesh" }));

    REQUIRE(std::filesystem::exists(dump));
    const std::string ini = ReadAllBytes(dump);
    INFO("dumped layout:\n" << ini);
    CHECK(ini.find("\nFilters=0:assets,1:scene+input-actions+material+sprite+mesh") != std::string::npos);   // upgraded once, now persisted
    const std::vector<DockRow> rows = ParseDockRows(ini);
    const DockRow* browser = FindRow(rows, WindowDockId(ini, "Asset Browser"));
    const DockRow* assetsInsp = FindRow(rows, WindowDockId(ini, "inspector_1"));
    REQUIRE(browser != nullptr);
    REQUIRE(assetsInsp != nullptr);
    CHECK(browser->parent == assetsInsp->parent);               // a split OF the browser's node
    const DockRow* parent = FindRow(rows, browser->parent);
    REQUIRE(parent != nullptr);
    CHECK(parent->split == 'X');
    // A ratio-shared split (no central node on either side), like the
    // default's band: the default's proportion, not a pixel target.
    const float share = assetsInsp->w / (browser->w + assetsInsp->w);
    INFO("SizeRef browser " << browser->w << " : Inspector 2 " << assetsInsp->w);
    CHECK(std::abs(share - Arcane::Editor::DefaultAssetsInspectorBandFraction(Arcane::Editor::LayoutFactorySettings{})) <= 0.005f);
}

// E8: THE LATE-BOUND PREVIEW SEAM (node page + editor upgrades s3.2). A mesh
// opened by --open-asset is built inside StageFinalize, BEFORE
// CreateGraphVehicles makes the chrome context; its Tick retry must still
// build the preview vehicle once the seam is up, so documents[] reads
// "ready" -- not the old copied-null-seam "no GPU device".
TEST_CASE("E8: a mesh opened by --open-asset during boot reaches a ready preview", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e8-mesh-preview");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "90",
                 "--settle", "10", "--report", inv.reportPath.generic_string(),
                 "--open-asset", "7e5a0011-0011-4011-8011-000000000011" };   // meshes/reference_cube.arcmesh (generated)
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("documents"));
    const auto& docs = run.report["documents"];
    REQUIRE(docs.size() == 1);
    CHECK(docs[0].at("guid") == "7e5a0011-0011-4011-8011-000000000011");
    CHECK(docs[0].at("kind") == "mesh");
    CHECK(docs[0].at("name") == "ReferenceCube");
    CHECK(docs[0].at("compile") == "ok");
    CHECK(docs[0].at("preview") == "ready");
}

// E10 (node-page phase s8.4, R10): a SCRIPTED bare launch still refuses before any
// window. The start page derives from project state, never CLI state, so this
// refusal must stay ahead of EditorApp construction (main.cpp:559-566) or a
// scripted run would open a window and hang CI. Not [gpu]: it returns before any
// device exists, so `~[gpu]` runs it.
TEST_CASE("E10: a scripted launch with no project exits 2 and names the reason on stderr", "[witness][editor]")
{
    WitnessScratch scratch(StagedEditorDir(), "e10-no-project");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor"); inv.workingDir = scratch.Dir();
    inv.args = { "--headless", "--frames", "1" };
    inv.hardCapMs = 30000;
    WitnessRun run = RunWitness(inv);
    INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(run.timedOut);
    CHECK(run.exitCode == 2);
    CHECK_FALSE(run.reportFound);
    CHECK(ReadAllBytes(run.stderrPath).find("no project selected") != std::string::npos);
}

TEST_CASE("E11: editor.settings.openAtBoot=both opens Editor Preferences and Project Settings through the real editor", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e11-settings-windows");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe";
    inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    const std::filesystem::path dump = scratch.Dir() / "dumped-layout.ini";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "30",
                 "--report", inv.reportPath.generic_string(), "--dump-layout", dump.generic_string(),
                 "--set", "editor.settings.openAtBoot=both", "--set", "editor.settings.openCategory=Engine" };
    inv.hardCapMs = 120000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    CHECK(run.report.at("exitReason") == "frames-complete");
    REQUIRE(std::filesystem::exists(dump));
    const std::string ini = ReadAllBytes(dump);
    INFO("dumped layout:\n" << ini);
    CHECK(ini.find("[Window][Editor Preferences]") != std::string::npos);   // drawn: the menu's window exists
    CHECK(ini.find("[Window][Project Settings]") != std::string::npos);
}

TEST_CASE("E12: a saved shortcut and a --set editor.keys.* reach a declared cvar at boot -- the editor declares its shortcut cvars before the early config rungs", "[witness][gpu]")
{
    // S4-GATE: HostBoot::ApplyEarlyConfigRungs layers the EditorUser rung (the
    // Keyboard page's saved shortcuts) and the --set list before EditorApp
    // exists. The registry layers a rung only onto cvars that already exist,
    // so main() declares the editor.keys.* table first. Declared later, the
    // --set below was refused as "unknown" (the RED run of this gate).
    WitnessScratch scratch(StagedEditorDir(), "e12-shortcut-boot");
    const std::filesystem::path userData = scratch.Dir() / "user-data";
    std::filesystem::create_directories(userData);
    ScopedUserDataBase scopedUserData(userData);
    REQUIRE(scopedUserData.Ok());
    Arcane::Paths::Config paths;
    paths.dist = false;
    const std::filesystem::path editorConfig = Arcane::Paths::Resolve(Arcane::Paths::Location::EditorUserDir, paths) / "Config";
    INFO("editor config dir " << editorConfig.string());
    REQUIRE(IsUnder(editorConfig, userData));   // the child inherits the redirected base, never the real profile
    std::filesystem::create_directories(editorConfig);
    {
        std::ofstream out(editorConfig / "editor.json", std::ios::binary);
        out << R"({ "keys": { "edit.copy": "F" } })";
    }

    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneEditor");
    inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", kNativeBackendCli, "--frames", "5",
                 "--report", inv.reportPath.generic_string(), "--set", "editor.keys.edit.cut=Ctrl+Shift+X" };
    inv.hardCapMs = 120000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    const std::string out = ReadAllBytes(run.stdoutPath) + ReadAllBytes(run.stderrPath);
    CHECK(out.find("unknown 'editor.keys.") == std::string::npos);    // the --set landed on a declared cvar
    CHECK(out.find("unknown-key 'keys.") == std::string::npos);       // so did the saved editor.json key
    // The positive marker (S6-GATE): the report echoes the --set with the
    // value the registry PUBLISHED, so a reworded warning cannot turn this
    // test vacuous.
    REQUIRE(run.report.contains("cvarSets"));
    REQUIRE(run.report["cvarSets"].size() == 1);
    CHECK(run.report["cvarSets"][0].at("name") == "editor.keys.edit.cut");
    CHECK(run.report["cvarSets"][0].at("value") == "Ctrl+Shift+X");
}
