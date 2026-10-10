// MeshDocument, headless halves (F2a, Task 9). The form is the Inspector's
// mesh page now (DrawFormBody, inspector filters s6a), and every case but the
// one ImGui case below leaves it undrawn -- these drive the pair the EditGesture bracket delegates to
// (ApplyMeshData, PushDataEdit's no-op guard and its doc-identity anchor),
// the preview-rebuild hook every data mutation goes through, and the
// device-less preview-vehicle lifecycle. Same split SpriteDocumentUndoTest
// uses for SpriteDocument, plus the preview half ShaderEditorDocumentTest
// pins for ShaderEditorDocument ("device-less services allocate no preview
// resources").
//
// The one ImGui case is the Inspector page at the bottom (inspector filters
// s6a): a device-less context (InputActionsDocumentUiTest's harness shape)
// that draws the document and then its page inside an "Inspector" window, to
// prove the form is submitted THERE.
//
// A real CommandStack is safe here: its resolve callback is only consulted by
// the COMPONENT snapshot paths (CommandStack.cpp:35,:49), and a generic Push
// never reaches them -- so no registry mutation, no TypeContext, and no bare
// Arcane::Runtime is needed for the undo tests.

#include <catch2/catch_test_macros.hpp>

#include "Documents/MeshDocument.hpp"
#include "Documents/PreviewStatus.hpp"
#include "Helpers/GpuCapability.hpp"
#include "Panels/AssetReferenceField.hpp"
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/OffscreenVehicle.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName / GetActiveID / ActiveIdWindow

#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>

using Arcane::Editor::MeshDocument;
namespace fs = std::filesystem;

namespace
{
    // A stack over a real (but untouched) registry -- same shape as
    // SpriteDocumentUndoTest.cpp's UndoFixture: the resolver has to return a
    // reference, and handing it a live object beats a dangling one even
    // though a generic-command test never calls it.
    struct UndoFixture
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg =
            std::make_unique<Astra::Registry>(creg);
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return *reg; } };
    };

    Arcane::MeshAssetData Fixture()
    {
        Arcane::MeshAssetData d;
        d.id     = Arcane::Guid::Generate();
        d.name   = "Prototype Cube";
        d.source = Arcane::MeshSource::Cube;
        return d;
    }

    // The document never touches disk outside Save(), so the path only has
    // to be well-formed -- same rationale as SpriteDocumentUndoTest.cpp's
    // FixturePath.
    fs::path FixturePath()
    {
        return fs::path("mesh_doc_test") / "prototype.arcmesh";
    }

    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_mesh_doc_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }
}

TEST_CASE("MeshDocument: device-less services allocate no preview resources", "[editor][mesh]")
{
    // Default-constructed Services carries no chromeGraph and no hostConfig --
    // exactly what a headless run (no EditorApp at all) hands every
    // document. Construction itself calls EnsurePreviewContext(); this pins
    // that the call is a genuine no-op rather than a crash or a lazily-built
    // vehicle on the first Tick.
    MeshDocument::Services services;
    MeshDocument doc(services, FixturePath(), Fixture());

    CHECK(doc.PreviewTextureId() == 0);

    // Tick drives RenderPreview only when a vehicle exists; with none, a Tick
    // must not conjure one or crash reaching into a null vehicle.
    doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewTextureId() == 0);
}

TEST_CASE("MeshDocument::ApplyMeshData marks the document dirty and rebuilds the preview",
          "[editor][mesh]")
{
    MeshDocument::Services services;
    Arcane::MeshAssetData before = Fixture();
    before.source = Arcane::MeshSource::Cube;
    MeshDocument doc(services, FixturePath(), before);
    CHECK_FALSE(doc.Dirty());
    REQUIRE(doc.PreviewMesh().has_value());
    const std::size_t cubeVertexCount = doc.PreviewMesh()->vertices.size();

    Arcane::MeshAssetData after = before;
    after.source = Arcane::MeshSource::UvSphere;   // rings=16/segments=32 defaults: valid
    doc.ApplyMeshData(after);

    CHECK(doc.Data().source == Arcane::MeshSource::UvSphere);
    // A source change is a data change like any other -- it must dirty the
    // document exactly the way SpriteDocument::ApplySpriteData dirties on a
    // field change (SpriteDocumentUndoTest.cpp's first test).
    CHECK(doc.Dirty());
    // ...and it must rebuild the preview from the NEW data, not silently keep
    // showing the old shape: a UV sphere's default topology has a very
    // different vertex count from a unit cube's fixed 24.
    REQUIRE(doc.PreviewMesh().has_value());
    CHECK(doc.PreviewMesh()->vertices.size() != cubeVertexCount);
}

TEST_CASE("MeshDocument: every preview rebuild also invalidates the preview vehicle's "
          "geometry", "[editor][mesh]")
{
    // Final-review C2. The preview vehicle's NriMeshBufferCache keys residency on a
    // FIXED synthetic guid ('PRVW'), and since Plan 2 Task 4 the mesh path resolves
    // through that cache instead of re-uploading from &*m_previewMesh every record.
    // So rebuilding m_previewMesh without invalidating that guid leaves the preview
    // frozen on the first geometry the document ever built -- for every topology
    // edit, every source change, every Save and every undo/redo.
    //
    // Device-less, through the counter RebuildPreviewMesh bumps at the invalidate
    // (PreviewGeometryInvalidations): with no vehicle there is no picture to compare,
    // but WHICH PATHS ISSUE THE INVALIDATE is the whole of the bug, and the issue
    // and the m_preview->InvalidateMeshGeometry call are the same two adjacent lines.
    MeshDocument::Services services;
    Arcane::MeshAssetData before = Fixture();
    before.source   = Arcane::MeshSource::UvSphere;
    before.segments = 32;
    MeshDocument doc(services, FixturePath(), before);

    // Construction rebuilds once, so the baseline is 1, not 0.
    const std::uint64_t afterCtor = doc.PreviewGeometryInvalidations();
    CHECK(afterCtor == 1u);

    // A TOPOLOGY edit: same source, more segments -- exactly the edit the Mesh
    // document's param panel makes, and exactly the one that used to redraw the old
    // shape.
    Arcane::MeshAssetData denser = before;
    denser.segments = 48;
    doc.ApplyMeshData(denser);
    CHECK(doc.PreviewGeometryInvalidations() == afterCtor + 1);
    // ...and the geometry really did change, so a kept resident buffer would have
    // been the WRONG one rather than merely redundant.
    REQUIRE(doc.PreviewMesh().has_value());
    const std::size_t denseVerts = doc.PreviewMesh()->vertices.size();

    Arcane::MeshAssetData coarser = before;
    coarser.segments = 8;
    doc.ApplyMeshData(coarser);
    CHECK(doc.PreviewGeometryInvalidations() == afterCtor + 2);
    REQUIRE(doc.PreviewMesh().has_value());
    CHECK(doc.PreviewMesh()->vertices.size() < denseVerts);
}

TEST_CASE("MeshDocument edits round-trip through the shared CommandStack", "[editor][mesh]")
{
    UndoFixture fx;
    const Arcane::MeshAssetData before = Fixture();

    MeshDocument::Services services;
    services.undo = [p = &fx.stack]() -> Arcane::CommandStack* { return p; };
    MeshDocument doc(services, FixturePath(), before);

    // What a completed drag does: the live edit already happened, then the
    // gesture's close builds one step from the activation-time copy -- same
    // shape as SpriteDocumentUndoTest.cpp's round-trip test.
    Arcane::MeshAssetData after = before;
    after.subdivisions = 4;
    doc.ApplyMeshData(after);
    doc.PushDataEdit("Edit Subdivisions", before);

    REQUIRE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()) == "Edit Subdivisions");
    CHECK(fx.stack.SceneStateId() == 0);

    fx.stack.Undo();
    CHECK(doc.Data() == before);
    REQUIRE(fx.stack.CanRedo());

    fx.stack.Redo();
    CHECK(doc.Data() == after);
}

TEST_CASE("MeshDocument: an undo bracket that moved nothing pushes no step, "
          "and a real move pushes exactly one",
          "[editor][mesh]")
{
    UndoFixture fx;
    const Arcane::MeshAssetData data = Fixture();

    MeshDocument::Services services;
    services.undo = [p = &fx.stack]() -> Arcane::CommandStack* { return p; };
    MeshDocument doc(services, FixturePath(), data);

    // Press-and-release on a drag without moving it: before == after.
    doc.PushDataEdit("Edit Subdivisions", doc.Data());
    CHECK_FALSE(fx.stack.CanUndo());

    // Contrast, so the guard above is not vacuous: a real change does push
    // exactly one step (not e.g. two, one per some double-call defect).
    Arcane::MeshAssetData moved = data;
    moved.subdivisions = 8;
    doc.ApplyMeshData(moved);
    doc.PushDataEdit("Edit Subdivisions", data);
    REQUIRE(fx.stack.CanUndo());
    fx.stack.Undo();
    CHECK_FALSE(fx.stack.CanUndo());   // exactly one step: undoing it empties the stack
}

TEST_CASE("MeshDocument undo steps go inert once the document closes", "[editor][mesh]")
{
    UndoFixture fx;
    const Arcane::MeshAssetData before = Fixture();

    {
        MeshDocument::Services services;
        services.undo = [p = &fx.stack]() -> Arcane::CommandStack* { return p; };
        MeshDocument doc(services, FixturePath(), before);

        Arcane::MeshAssetData after = before;
        after.subdivisions = 6;
        doc.ApplyMeshData(after);
        doc.PushDataEdit("Edit Subdivisions", before);
        REQUIRE(fx.stack.CanUndo());
    }   // document destroyed; the step outlives it on the shared stack

    // The anchor expired, so both directions resolve to nothing and step over
    // themselves rather than dereferencing a dead document.
    CHECK_NOTHROW(fx.stack.Undo());
    CHECK_NOTHROW(fx.stack.Redo());
}

TEST_CASE("MeshDocument::Title reports the asset's name, falling back to the file stem",
          "[editor][mesh]")
{
    MeshDocument::Services services;

    Arcane::MeshAssetData named = Fixture();
    named.name = "Hero Shield";
    MeshDocument namedDoc(services, FixturePath(), named);
    CHECK(namedDoc.Title() == "Hero Shield");

    Arcane::MeshAssetData unnamed = Fixture();
    unnamed.name.clear();
    MeshDocument unnamedDoc(services, fs::path("mesh_doc_test") / "generated_mesh.arcmesh", unnamed);
    CHECK(unnamedDoc.Title() == "generated_mesh");
}

TEST_CASE("MeshDocument: an invalid param set yields no preview geometry and surfaces "
          "the validation reason instead of throwing",
          "[editor][mesh]")
{
    MeshDocument::Services services;

    Arcane::MeshAssetData invalid = Fixture();
    invalid.source = Arcane::MeshSource::Cylinder;
    invalid.segments = 1;   // floor is 3 -- refused

    // Construction itself must not throw against an already-invalid asset
    // (a hand-edited file loaded straight into the document).
    CHECK_NOTHROW([&] { MeshDocument doc(services, FixturePath(), invalid); }());

    MeshDocument doc(services, FixturePath(), invalid);
    CHECK_FALSE(doc.PreviewMesh().has_value());
    REQUIRE(doc.ValidationReason().has_value());
    CHECK(doc.ValidationReason()->find("segments") != std::string::npos);
    CHECK(doc.ValidationReason()->find("Cylinder") != std::string::npos);

    // A device-less Tick over an invalid mesh must also not throw or reach
    // into a null preview mesh.
    CHECK_NOTHROW(doc.Tick(1.0 / 60.0));

    // And a live edit that repairs the data must clear the reason and
    // publish real geometry -- the panel's "unreachable from the panel"
    // claim only means normal WIDGET interaction cannot produce this state;
    // ApplyMeshData (what a widget-driven commit calls) must still recover
    // from it once the data is valid again.
    Arcane::MeshAssetData fixed = invalid;
    fixed.segments = 8;
    doc.ApplyMeshData(fixed);
    CHECK_FALSE(doc.ValidationReason().has_value());
    REQUIRE(doc.PreviewMesh().has_value());
    CHECK_FALSE(doc.PreviewMesh()->vertices.empty());
}

TEST_CASE("MeshDocument: clearing slot 0 on an IMPORTED mesh keeps the slot (nil material), "
          "on a generated mesh erases it -- and an imported mesh builds no procedural preview",
          "[editor][mesh]")
{
    // Final-review fix I4 (2026-09-11). Every companion .arcmesh the import wave mints
    // opens in MeshDocument; there the material picker's clear used to erase slot 0
    // unconditionally, shifting every later slot down one and breaking the artifact's
    // slotIndex <-> slots[] correspondence (Plan 2's per-section draw resolves through
    // exactly that index). The rule is pure (ClearPrimarySlotMaterial) so it is pinned
    // here without ImGui.
    const Arcane::Guid matA = Arcane::Guid::Generate();
    const Arcane::Guid matB = Arcane::Guid::Generate();

    Arcane::MeshAssetData imported = Fixture();
    imported.source         = Arcane::MeshSource::Imported;
    imported.importedSource = Arcane::Guid::Generate();
    imported.slots          = { { "Metal", matA }, { "Paint", matB } };

    MeshDocument::ClearPrimarySlotMaterial(imported);
    REQUIRE(imported.slots.size() == 2u);           // NOT shifted
    CHECK(imported.slots[0].name == "Metal");       // the slot survives, name intact
    CHECK_FALSE(imported.slots[0].material.IsValid());
    CHECK(imported.slots[1].name == "Paint");       // slot 1 is still slot 1
    CHECK(imported.slots[1].material == matB);

    // The rule has no size threshold: a single-slot imported mesh keeps its one slot too.
    Arcane::MeshAssetData importedSingle = imported;
    importedSingle.slots = { { "SingleMat", matA } };
    MeshDocument::ClearPrimarySlotMaterial(importedSingle);
    REQUIRE(importedSingle.slots.size() == 1u);
    CHECK_FALSE(importedSingle.slots[0].material.IsValid());

    // Contrast: the F2a behaviour for a GENERATED mesh is unchanged -- erase.
    Arcane::MeshAssetData generated = Fixture();   // Cube
    generated.slots = { { "", matA } };
    MeshDocument::ClearPrimarySlotMaterial(generated);
    CHECK(generated.slots.empty());

    // And the empty case is a no-op on either kind.
    Arcane::MeshAssetData none = Fixture();
    MeshDocument::ClearPrimarySlotMaterial(none);
    CHECK(none.slots.empty());

    // The preview half: an imported mesh yields NO procedural geometry (BuildMeshData
    // is never asked, so its "wrong function" ARC_WARN never fires on an edit gesture)
    // while the validator still runs -- a valid importedSource means no reason.
    MeshDocument::Services services;
    MeshDocument doc(services, FixturePath(), imported);
    CHECK_FALSE(doc.PreviewMesh().has_value());
    CHECK_FALSE(doc.ValidationReason().has_value());
    CHECK_NOTHROW(doc.Tick(1.0 / 60.0));

    // ...and a nil importedSource IS still a reportable refusal, imported or not.
    Arcane::MeshAssetData detached = imported;
    detached.importedSource = Arcane::Guid{};
    doc.ApplyMeshData(detached);
    CHECK_FALSE(doc.PreviewMesh().has_value());
    REQUIRE(doc.ValidationReason().has_value());
    CHECK(doc.ValidationReason()->find("importedSource") != std::string::npos);
}

TEST_CASE("MeshDocument::Save writes the asset and clears dirty", "[editor][mesh]")
{
    const fs::path dir = TempDir("save");
    const fs::path file = dir / "saved.arcmesh";

    MeshDocument::Services services;
    Arcane::MeshAssetData data = Fixture();
    data.source = Arcane::MeshSource::Plane;
    data.subdivisions = 3;
    MeshDocument doc(services, file, data);

    Arcane::MeshAssetData edited = data;
    edited.subdivisions = 5;
    doc.ApplyMeshData(edited);
    REQUIRE(doc.Dirty());

    REQUIRE(doc.Save());
    CHECK_FALSE(doc.Dirty());

    const auto reloaded = Arcane::LoadMeshAsset(file);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->subdivisions == 5);
    CHECK(reloaded->source == Arcane::MeshSource::Plane);
}

// =============================================================================
// THE EDITOR'S OWN WIRING, END TO END
// =============================================================================
// Everything above drives MeshDocument in isolation, and SceneRenderResolver
// Test drives InvalidateMesh in isolation -- and BOTH passed while a desk
// session showed a saved .arcmesh never reaching the viewport. That gap is
// the point of this case: EditorApp.cpp (which builds Services::
// invalidateMesh) is NOT compiled into ArcaneTests, so nothing joined the two
// halves. This case joins them the only way a headless test can -- by
// standing up the exact chain EditorApp does (a real Project + Runtime, a
// registered .arcmesh, a MeshRenderer referencing it, a real
// SceneRenderResolver, and a document whose invalidateMesh routes into it) --
// and then asserting on the PUBLISHED MeshTable, which is what
// GpuSceneSync (and therefore the viewport) actually reads.
TEST_CASE("MeshDocument::Save reaches the scene's MeshTable through the editor's own "
          "invalidateMesh wiring",
          "[editor][mesh][host]")
{
    const fs::path dir = TempDir("save_invalidates_scene");
    REQUIRE(Arcane::Project::Create(dir / "Game", "G").has_value());
    const fs::path file = dir / "Game" / "Content" / "plane.arcmesh";

    // A Plane, for SceneRenderResolverTest's reason: (subdivisions+1)^2 makes
    // an edit observable through vertex count alone -- 1 -> 4, 3 -> 16.
    Arcane::MeshAssetData authored;
    authored.id           = Arcane::Guid::Generate();
    authored.name         = "probe-plane";
    authored.source       = Arcane::MeshSource::Plane;
    authored.subdivisions = 1;
    REQUIRE(Arcane::SaveMeshAsset(file, authored));

    Arcane::ClientRuntime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game") == true);
    REQUIRE(rt.RegisterCreatedAsset(file).has_value());

    const Astra::Entity e = rt.Registry().CreateEntity();
    Arcane::MeshRenderer mr; mr.mesh = authored.id;
    rt.Registry().AddComponent<Arcane::MeshRenderer>(e, mr);

    Arcane::SceneRenderResolver::Services rs;
    rs.runtime = &rt;
    Arcane::SceneRenderResolver resolver(std::move(rs));

    Arcane::SceneRenderResolver::FrameInfo frame;
    frame.dt = 1.0 / 60.0;
    resolver.Refresh(frame);

    // Re-read the resource every time: every Refresh re-publishes it.
    const auto vertexCount = [&]() -> std::size_t
    {
        const Arcane::MeshTable* t = rt.Registry().GetResource<Arcane::MeshTable>();
        REQUIRE(t != nullptr);
        const Arcane::MeshEntry* entry = t->Resolve(authored.id);
        REQUIRE(entry != nullptr);
        return entry->data.vertices.size();
    };
    REQUIRE(vertexCount() == 4);

    // THE FACTORY'S LAMBDA, VERBATIM (EditorApp.cpp:698-702) -- including the
    // null-resolver guard, so this test fails the same way the editor would if
    // the guard ever swallowed the call.
    MeshDocument::Services services;
    Arcane::SceneRenderResolver* resolverPtr = &resolver;
    services.invalidateMesh = [&resolverPtr](const Arcane::Guid& g)
    {
        if (resolverPtr)
            resolverPtr->InvalidateMesh(g);
    };

    // The document opens the file the way the factory does -- LoadMeshAsset,
    // not the authored struct -- so an id that does not survive the round trip
    // shows up here rather than being papered over.
    const auto opened = Arcane::LoadMeshAsset(file);
    REQUIRE(opened.has_value());
    MeshDocument doc(services, file, *opened);

    Arcane::MeshAssetData edited = *opened;
    edited.subdivisions = 3;
    doc.ApplyMeshData(edited);
    REQUIRE(doc.Dirty());

    REQUIRE(doc.Save());

    // NO Refresh IN BETWEEN, deliberately: the save has to be visible to the
    // very next frame's GpuSceneSync, and a host may render before its
    // next sweep.
    CHECK(vertexCount() == 16);

    // ...and the host's OWN next sweep must not undo it. A host calls Refresh
    // every frame (EditorAppFrame.cpp:1206, RuntimeFrame.cpp), so a re-resolve
    // that only survived until the next sweep would still read as "the
    // viewport never updated" at a desk.
    resolver.Refresh(frame);
    CHECK(vertexCount() == 16);

    std::error_code ec; fs::remove_all(dir, ec);
}

// Inspector filters s6a: the document is an Inspector source of kind "mesh"
// whose ONE page is the mesh form (the preview stays in the document window).
// Opened = selected (epoch 1, so the app's per-document epoch map -- which
// starts at 0 -- sees frame 1 as an event); a history restore re-selects
// without a click and so moves no epoch.
TEST_CASE("MeshDocument is a mesh Inspector source selected at open", "[editor][mesh][inspector]")
{
    MeshDocument doc(MeshDocument::Services{}, FixturePath(), Fixture());
    CHECK(doc.Kind() == "mesh");
    CHECK(doc.SelectionKey() == "mesh");
    CHECK(doc.SelectionEpoch() == 1);                  // selected at open
    REQUIRE(doc.Page() != nullptr);
    CHECK(doc.Page()->Breadcrumb().size() == 1);
    CHECK(doc.Page()->Breadcrumb()[0].label == doc.Title());
    CHECK(doc.Page()->Breadcrumb()[0].key == std::optional<std::string>{ "mesh" });
    CHECK(doc.PageFor("mesh") == doc.Page());
    CHECK(doc.RestoreSelection("mesh"));
    CHECK(doc.SelectionEpoch() == 1);                  // a restore is not a click
    CHECK_FALSE(doc.Resolves("sprite"));
}

namespace
{
    // InputActionsDocumentUiTest.cpp's DocUi shape: own context, software font
    // atlas, the document drawn FIRST (as DocumentHost::DrawAll does), then a
    // pinned "Inspector" window drawing its page (as DrawInspectorWindows does).
    // Device-less services: the preview child shows its "No preview -- no GPU
    // device" line, so the document window still has content.
    struct MeshPageUi
    {
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        Arcane::Editor::PropertyGridState grid;
        std::unordered_map<std::string, ImVec2> probe;     // value-widget centres by row label
        std::function<void()> beforePage;                  // runs inside "Inspector" before the page (ActivateItemByID)
        bool log = false;
        std::string docLog, pageLog;                       // each window's LogToBuffer text, when `log`
        MeshDocument doc;                                  // default Fixture(): a Cube, the form's first widget is the Source combo
        explicit MeshPageUi(MeshDocument::Services s = {}, Arcane::MeshAssetData d = Fixture())
            : doc(std::move(s), FixturePath(), std::move(d))
        {
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600.0f, 900.0f);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
            grid.probe = &probe;
        }
        ~MeshPageUi() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always);
            bool close = false;
            if (log) ImGui::LogToBuffer();
            doc.Draw(close);
            if (log) { docLog = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
            ImGui::SetNextWindowPos(ImVec2(820, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(700, 880), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            if (beforePage) beforePage();
            if (log) ImGui::LogToBuffer();
            { Arcane::Editor::PropertyGrid g(grid); if (auto* page = doc.Page()) page->Draw(g); }
            if (log) { pageLog = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
            ImGui::End();
            ImGui::Render();
        }
        void Move(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); Frame(); }
        void Button(int b, bool down) { ImGui::GetIO().AddMouseButtonEvent(b, down); Frame(); }
        ImVec2 At(const std::string& label) { INFO(label); REQUIRE(probe.count(label) == 1); return probe.at(label); }
    };
}

// The form moved OUT of the document window: its first widget (a generated
// mesh's "Source" ComboRow, s5.5) is submitted in the Inspector window, keyed
// "##value" under PushID("Source") inside the "##mesh" Rows table. Proven
// through the ACTIVE id after a press on the row's probed value widget --
// ImGuiWindow::GetID only hashes a label, and LastItemData
// is restored to the parent's at End() (imgui.cpp:8849), so neither alone
// shows a submission.
TEST_CASE("MeshDocument's form draws in the Inspector window, not the document's", "[editor][mesh][inspector]")
{
    MeshPageUi h;
    h.Frame();                                         // warm-up: both windows exist
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
    REQUIRE(iw != nullptr);
    h.Frame();
    const ImVec2 at = h.At("Source");
    h.Move(at);
    h.Button(ImGuiMouseButton_Left, true);
    CHECK(ImGui::GetActiveID() == ImGui::GetIDWithSeed("##value", nullptr,
                                  ImGui::GetIDWithSeed("Source", nullptr, iw->GetID("##mesh"))));
    CHECK(ImGui::GetCurrentContext()->ActiveIdWindow == iw);   // in the Inspector, not the document window
    h.Button(ImGuiMouseButton_Left, false);
}

// ---- Node page + editor upgrades s3.2: the late-bound seam + PreviewStatus ----
TEST_CASE("MeshDocument status: no seam reads no-device and never latches; imported reads not-compiled-here; a validation reason reads errors", "[editor][mesh][preview]")
{
    using Arcane::Editor::CompileStatus;
    using Arcane::Editor::PreviewAvailability;
    {
        MeshDocument doc(MeshDocument::Services{}, FixturePath(), Fixture());   // chromeGraph unset
        for (int i = 0; i < 3; ++i) doc.Tick(1.0 / 60.0);
        const auto st = doc.ComputeStatus();
        CHECK(st.compile == CompileStatus::Ok);
        CHECK(st.preview == PreviewAvailability::NoDevice);
        CHECK_FALSE(st.image);
        CHECK(doc.PreviewVehicleAttempts() == 0);
    }
    {
        // The boot window: the seam is WIRED but the chrome context is not up yet.
        Arcane::HostConfig cfg;
        MeshDocument::Services services;
        services.hostConfig  = &cfg;
        services.chromeGraph = [] { return static_cast<Arcane::NriGraphContext*>(nullptr); };
        MeshDocument doc(services, FixturePath(), Fixture());
        for (int i = 0; i < 3; ++i) doc.Tick(1.0 / 60.0);
        CHECK(doc.ComputeStatus().preview == PreviewAvailability::NoDevice);   // never vehicle-failed
        CHECK(doc.PreviewVehicleAttempts() == 0);
    }
    {
        Arcane::MeshAssetData imported = Fixture();
        imported.source         = Arcane::MeshSource::Imported;
        imported.importedSource = Arcane::Guid::Generate();
        MeshDocument doc(MeshDocument::Services{}, FixturePath(), imported);
        CHECK(doc.ComputeStatus().compile == CompileStatus::NotCompiledHere);
    }
    {
        Arcane::MeshAssetData invalid = Fixture();
        invalid.source   = Arcane::MeshSource::Cylinder;
        invalid.segments = 1;   // floor is 3
        MeshDocument doc(MeshDocument::Services{}, FixturePath(), invalid);
        CHECK(doc.ComputeStatus().compile == CompileStatus::Errors);
    }
}

TEST_CASE("MeshDocument: the first non-null chromeGraph makes Tick build the preview vehicle exactly once", "[editor][mesh][preview][gpu]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    using Arcane::Editor::PreviewAvailability;
    Arcane::HostConfig cfg;
    cfg.backend  = Arcane::Test::kNativeBackend;
    cfg.headless = true;
    auto chrome = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(chrome != nullptr);

    Arcane::NriGraphContext* live = nullptr;     // ChromeGraph() before CreateGraphVehicles
    MeshDocument::Services services;
    services.hostConfig  = &cfg;
    services.chromeGraph = [&] { return live; };
    MeshDocument doc(services, FixturePath(), Fixture());   // constructed during "boot"
    for (int i = 0; i < 3; ++i) doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 0);
    CHECK(doc.ComputeStatus().preview == PreviewAvailability::NoDevice);

    live = &chrome->Graph();                     // CreateGraphVehicles ran
    doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 1);
    for (int i = 0; i < 5; ++i) doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 1);    // built: Tick stops retrying
    const auto st = doc.ComputeStatus();
    CHECK(st.preview == PreviewAvailability::Ready);
    CHECK(st.image);                             // a Presented frame landed
    CHECK(doc.PreviewTextureId() != 0);
}   // no retire sink: ~MeshDocument invalidates against the hud recorded at creation, then chrome dies

TEST_CASE("MeshDocument: a device-less document's preview box says why -- no GPU device (s5.2)", "[editor][mesh][preview]")
{
    MeshPageUi h;
    h.log = true;
    h.Frame(); h.Frame();
    INFO(h.docLog);
    CHECK(h.docLog.find("No preview -- no GPU device") != std::string::npos);
    CHECK(Arcane::Editor::PreviewBoxText(h.doc.ComputeStatus()) == "No preview -- no GPU device");
}

// ---- Node page + editor upgrades s5.5: the mesh page on PropertyGrid ----
TEST_CASE("MeshDocument::ClearSlotMaterial: imported nils slot k and keeps it; generated erases; out of range is a no-op", "[editor][mesh]")
{
    const Arcane::Guid matA = Arcane::Guid::Generate(), matB = Arcane::Guid::Generate();
    Arcane::MeshAssetData imported = Fixture();
    imported.source = Arcane::MeshSource::Imported;
    imported.importedSource = Arcane::Guid::Generate();
    imported.slots = { { "Metal", matA }, { "Paint", matB } };
    MeshDocument::ClearSlotMaterial(imported, 1);
    REQUIRE(imported.slots.size() == 2u);
    CHECK(imported.slots[1].name == "Paint");
    CHECK_FALSE(imported.slots[1].material.IsValid());
    CHECK(imported.slots[0].material == matA);
    MeshDocument::ClearSlotMaterial(imported, 7);
    CHECK(imported.slots.size() == 2u);
    Arcane::MeshAssetData generated = Fixture();
    generated.slots = { { "", matA } };
    MeshDocument::ClearSlotMaterial(generated, 0);
    CHECK(generated.slots.empty());
}

TEST_CASE("MeshDocument::ApplySlotMaterialEdit: one step per Set and Clear, generated and imported", "[editor][mesh]")
{
    UndoFixture fx;
    MeshDocument::Services s;
    s.undo = [&fx] { return &fx.stack; };
    const Arcane::Guid matA = Arcane::Guid::Generate(), matB = Arcane::Guid::Generate();
    Arcane::Editor::AssetRefEdit set, clear;
    set.op = Arcane::Editor::AssetRefEdit::Op::Set;
    clear.op = Arcane::Editor::AssetRefEdit::Op::Clear;
    {
        MeshDocument doc(s, FixturePath(), Fixture());               // a Cube with no slot
        set.guid = matA;
        doc.ApplySlotMaterialEdit(0, set);
        REQUIRE(doc.Data().slots.size() == 1u);
        CHECK(doc.Data().slots[0].material == matA);
        CHECK(std::string(fx.stack.UndoLabel()) == "Assign Material");
        doc.ApplySlotMaterialEdit(0, clear);
        CHECK(doc.Data().slots.empty());                             // generated: erase
        CHECK(std::string(fx.stack.UndoLabel()) == "Clear Material");
        fx.stack.Undo(); CHECK(doc.Data().slots.size() == 1u);
        fx.stack.Undo(); CHECK(doc.Data().slots.empty());
        CHECK_FALSE(fx.stack.CanUndo());
    }
    {
        Arcane::MeshAssetData d = Fixture();
        d.source = Arcane::MeshSource::Imported;
        d.importedSource = Arcane::Guid::Generate();
        d.slots = { { "Metal", matA }, { "Paint", Arcane::Guid{} } };
        MeshDocument doc(s, FixturePath(), d);
        set.guid = matB;
        doc.ApplySlotMaterialEdit(1, set);
        CHECK(doc.Data().slots[1].material == matB);
        doc.ApplySlotMaterialEdit(1, clear);
        REQUIRE(doc.Data().slots.size() == 2u);                      // imported: nil, keep the slot
        CHECK_FALSE(doc.Data().slots[1].material.IsValid());
        CHECK(doc.Data().slots[1].name == "Paint");
        fx.stack.Undo(); CHECK(doc.Data().slots[1].material == matB);
        fx.stack.Undo(); CHECK_FALSE(doc.Data().slots[1].material.IsValid());
        CHECK_FALSE(fx.stack.CanUndo());
    }
}

TEST_CASE("MeshDocument page: a Source pick is one step and swaps the topology rows", "[editor][mesh][inspector]")
{
    UndoFixture fx;
    MeshDocument::Services s;
    s.undo = [&fx] { return &fx.stack; };
    MeshPageUi h(s);
    h.Frame(); h.Frame();
    CHECK(h.probe.count("Rings") == 0);                              // a Cube has no topology rows
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
    REQUIRE(iw != nullptr);
    const ImGuiID combo = ImGui::GetIDWithSeed("##value", nullptr, ImGui::GetIDWithSeed("Source", nullptr, iw->GetID("##mesh")));
    h.beforePage = [&] { ImGui::ActivateItemByID(combo); };
    h.Frame(); h.beforePage = nullptr; h.Frame();                    // the press lands: the popup opens
    REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    ImGuiWindow* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
    REQUIRE(popup != nullptr);
    const ImGuiID sphere = popup->GetID("UV Sphere");                // ComboRow's Selectables push no index id
    h.beforePage = [&] { ImGui::ActivateItemByID(sphere); };
    h.Frame(); h.beforePage = nullptr; h.Frame();
    CHECK(h.doc.Data().source == Arcane::MeshSource::UvSphere);
    REQUIRE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()) == "Change Source");
    CHECK(h.probe.count("Rings") == 1);
    CHECK(h.probe.count("Segments") == 1);
    fx.stack.Undo();
    CHECK(h.doc.Data().source == Arcane::MeshSource::Cube);
    CHECK_FALSE(fx.stack.CanUndo());
}

TEST_CASE("MeshDocument page: Info reads BuildMeshData's counts and the bounds", "[editor][mesh][inspector]")
{
    MeshPageUi h;
    h.log = true;
    h.Frame(); h.Frame();
    const auto mesh = Arcane::BuildMeshData(Fixture());
    REQUIRE(mesh.has_value());
    const Arcane::MeshBounds b = Arcane::ComputeMeshBounds(*mesh);
    char bounds[96];
    std::snprintf(bounds, sizeof(bounds), "%.2f \xC3\x97 %.2f \xC3\x97 %.2f m", b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z);
    INFO(h.pageLog);
    for (const char* row : { "Vertices", "Triangles", "Bounds" }) { INFO(row); CHECK(h.probe.count(row) == 1); }
    CHECK(h.pageLog.find(std::to_string(mesh->vertices.size())) != std::string::npos);
    CHECK(h.pageLog.find(std::to_string(mesh->indices.size() / 3)) != std::string::npos);
    CHECK(h.pageLog.find(bounds) != std::string::npos);
}
