// ShaderEditorDocument, headless halves (shader-editor review follow-up): the
// ImGui panels are never drawn -- these drive the lifecycle logic the 2026-07-23
// review found untested. Save-before-bind (review M1) must keep the asset's
// saved params; ResolveParentChain must reject cycles and unresolvable parents;
// ConsumeResult must route ONLY this document's in-flight job ids. Device-less:
// the ctor skips preview resources cleanly, BindIfComplete never runs.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetReferenceField.hpp"   // AssetRefEdit: the texture row's write (T3-C6)
#include "Panels/DiagnosticStore.hpp"
#include "Documents/PreviewStatus.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Documents/ShaderNodeKey.hpp"   // FormatNodeKey: the node page harness keys (T3-B3)
#include "Widgets/IconsLucide.h"   // ICON_LC_X: the asset cell's clear button
#include "Widgets/EditorTheme.hpp"   // Theme::kError: the node page Errors colour probe
#include "Widgets/PropertyGrid.hpp"
#include "Helpers/GpuCapability.hpp"
#include "Helpers/NodePageDocs.hpp"   // SpriteNodeDoc / ChainNodeDoc (node page s5.1.11)
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarRegistry.hpp>   // editor.inspector.materialPreviewFraction
#include <Arcane/Edit/Command.hpp>   // GraphSwap: a whole-graph undo step (node page s5.1.11)
#include <Arcane/Edit/CommandStack.hpp>   // the mesh-metadata undo step rides the ONE undo history
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/OffscreenVehicle.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>   // RenderErrorCount: the mesh preview frame (T3-D6)
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <Astra/Registry/Registry.hpp>   // CommandStack's resolver target (never called here)

#include <imgui.h>   // the stale-ini case drives ImGui's settings API
#include <imgui_internal.h>   // ClearIniSettings (a windowed project switch's reset)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "ImGuiTestKeys.hpp"   // TestKeys::AddKeyEvent: Ctrl as this platform's user presses it

using Arcane::Editor::DocServices;
using Arcane::Editor::ShaderEditorDocument;
namespace fs = std::filesystem;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_shader_doc_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    constexpr const char* kSnippet =
        "//@param color Tint = (1, 0, 0, 1)\n"
        "//@param float Speed = 2.0 [0..4]\n"
        "float4 shade(Varyings v) { return Tint * Speed; }\n";

    bool AnyErrorContains(const std::vector<std::string>& errors, const char* needle)
    {
        for (const std::string& e : errors)
            if (e.find(needle) != std::string::npos)
                return true;
        return false;
    }
}

TEST_CASE("ShaderEditorDocument::Save before the first bind keeps saved params", "[editor][material]")
{
    // Review M1 regression: with no device the document can never bind, so
    // m_instance stays null -- Save must NOT wipe the params it loaded.
    const fs::path dir = TempDir("savebeforebind");
    const fs::path file = dir / "glow.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Glow";
    data.snippet = kSnippet;
    data.params.emplace_back("Tint", Arcane::MatParamValue::MakeColor(0.1f, 0.2f, 0.3f, 1.0f));
    data.params.emplace_back("Speed", Arcane::MatParamValue::MakeFloat(3.5f));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());

    ShaderEditorDocument doc(DocServices{}, file, *loaded);
    CHECK_FALSE(doc.Dirty());
    REQUIRE(doc.Save());

    const auto reloaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->params.size() == 2);   // the M1 bug left this empty
    CHECK(reloaded->snippet == kSnippet);
    CHECK(reloaded->id == data.id);
}

// Inspector filters s6a: the document is an Inspector source of kind
// "material" whose ONE page is the whole document's (preview + params).
// Opened = selected (epoch 1, so the app's per-document epoch map -- which
// starts at 0 -- sees frame 1 as an event); a history restore re-selects
// without a click and so moves no epoch.
TEST_CASE("ShaderEditorDocument is a material Inspector source selected at open", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("inspectorpage");
    const fs::path file = dir / "page.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Page";
    data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());

    ShaderEditorDocument doc(DocServices{}, file, *loaded);
    CHECK(doc.Kind() == "material");
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);                  // selected at open
    REQUIRE(doc.Page() != nullptr);
    CHECK(doc.Page()->Breadcrumb().size() == 1);
    CHECK(doc.Page()->Breadcrumb()[0].label == "Page");          // the header names the subject (s4.3)
    CHECK(doc.Page()->Breadcrumb()[0].key == std::optional<std::string>{ "material" });
    CHECK(doc.RestoreSelection("material"));
    CHECK(doc.SelectionEpoch() == 1);                  // a restore is not a click
    CHECK_FALSE(doc.Resolves("node:7"));
}

// F2b Task 13: a "mesh"-kind document binds baseColor/albedo with NO
// ShaderCompiler and NO ShaderSourceProvider at all -- Rebuild()'s mesh
// branch runs BEFORE the compiler/sources null check (its own comment), so
// the params panel works even in this bare-services shape (every other
// surface needs real services to bind ANYTHING, per the "before the first
// bind" test above). ApplyParamEdit + Save is the SAME public round trip the
// inspector's own texture picker will drive (DrawTextureParam ->
// SetParamWithUndo -> ApplyParamEdit), proving the mesh decl path
// (MeshParamTemplate) feeds the ordinary param-edit/save machinery with no
// special-casing anywhere downstream of Rebuild().
TEST_CASE("A mesh document binds baseColor/albedo with no compiler and round-trips an albedo edit",
         "[editor][material][mesh]")
{
    const fs::path dir = TempDir("mesh_doc");
    const fs::path file = dir / "hero.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Hero";
    data.kind = "mesh";
    data.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
    data.params.emplace_back("albedo", Arcane::MatParamValue::MakeTexture(Arcane::Guid{}));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());

    // Bare services: no compiler, no source provider. A fullscreen/sprite
    // document left at this shape never binds (m_instance stays null, per
    // the "before the first bind" test above) -- a mesh document does,
    // because nothing about baseColor/albedo needs either service.
    ShaderEditorDocument doc(DocServices{}, file, *loaded);
    CHECK(doc.ParseErrors().empty());

    const Arcane::Guid newAlbedo = Arcane::Guid::Generate();
    doc.ApplyParamEdit(Arcane::HashParamName("albedo"), /*hasValue=*/true,
                       Arcane::MatParamValue::MakeTexture(newAlbedo));
    REQUIRE(doc.Save());

    const auto reloaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->kind == "mesh");
    CHECK(reloaded->snippet.empty());          // still no snippet -- mesh stitches none
    CHECK_FALSE(reloaded->graph.has_value());  // still no graph
    REQUIRE(reloaded->params.size() == 2);     // baseColor AND albedo both round-trip

    bool foundBaseColor = false, foundAlbedo = false;
    for (const auto& [name, value] : reloaded->params)
    {
        if (name == "baseColor")
        {
            foundBaseColor = true;
            CHECK(value.type == Arcane::MatParamType::Color);
            CHECK(value.f[0] == 1.0f);   // untouched by the albedo edit
        }
        else if (name == "albedo")
        {
            foundAlbedo = true;
            CHECK(value.type == Arcane::MatParamType::Texture);
            CHECK(value.tex == newAlbedo);   // the edit survived Save + reload
        }
    }
    CHECK(foundBaseColor);
    CHECK(foundAlbedo);
}

TEST_CASE("ShaderEditorDocument authors bounded mesh metadata only on mesh surfaces",
          "[editor][material][mesh]")
{
    const fs::path dir = TempDir("mesh_metadata");
    const fs::path meshFile = dir / "mesh.arcmat";

    Arcane::MaterialAssetData mesh;
    mesh.id = Arcane::Guid::Generate();
    mesh.name = "Mesh metadata";
    mesh.kind = "mesh";
    REQUIRE(Arcane::SaveMaterialAsset(meshFile, mesh));
    const auto loadedMesh = Arcane::LoadMaterialAsset(meshFile);
    REQUIRE(loadedMesh.has_value());

    ShaderEditorDocument meshDoc(DocServices{}, meshFile, *loadedMesh);
    auto state = meshDoc.CaptureMeshMaterialMetadata();
    REQUIRE(state.has_value());
    CHECK_FALSE(state->blend.has_value());
    CHECK_FALSE(state->alphaCutoff.has_value());
    CHECK_FALSE(state->twoSided.has_value());

    state->blend = Arcane::MaterialBlendMode::Transparent;
    state->alphaCutoff = 2.0f;
    state->twoSided = true;
    meshDoc.ApplyMeshMaterialMetadata(*state);
    state = meshDoc.CaptureMeshMaterialMetadata();
    REQUIRE(state.has_value());
    REQUIRE(state->alphaCutoff.has_value());
    CHECK(*state->alphaCutoff == 1.0f);
    REQUIRE(meshDoc.Save());

    const auto savedMesh = Arcane::LoadMaterialAsset(meshFile);
    REQUIRE(savedMesh.has_value());
    CHECK(savedMesh->blend == Arcane::MaterialBlendMode::Transparent);
    CHECK(savedMesh->alphaCutoff == 1.0f);
    CHECK(savedMesh->twoSided == true);

    state->alphaCutoff = -3.0f;
    meshDoc.ApplyMeshMaterialMetadata(*state);
    state = meshDoc.CaptureMeshMaterialMetadata();
    REQUIRE(state.has_value());
    REQUIRE(state->alphaCutoff.has_value());
    CHECK(*state->alphaCutoff == 0.0f);

    for (const char* kind : { "fullscreen", "sprite" })
    {
        const fs::path file = dir / (std::string(kind) + ".arcmat");
        Arcane::MaterialAssetData data;
        data.id = Arcane::Guid::Generate();
        data.name = kind;
        data.kind = kind;
        data.snippet = kSnippet;
        REQUIRE(Arcane::SaveMaterialAsset(file, data));
        const auto loaded = Arcane::LoadMaterialAsset(file);
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(DocServices{}, file, *loaded);
        CHECK_FALSE(doc.CaptureMeshMaterialMetadata().has_value());
    }
}

// F3 plan 2 final review, I2: the mesh metadata rows (blend, alpha cutoff,
// two-sided) were the ONE gesture in the material panel's "Rendering" block
// that bypassed the document's undo plumbing -- Ctrl+Z after switching Blend
// to Transparent reverted the colour drag before it and left the blend alone.
// SetMeshMaterialMetadataWithUndo is the panel's write path now: one step per
// edit, Undo restores the capture, Redo re-applies what LANDED (post-clamp).
TEST_CASE("ShaderEditorDocument: a mesh metadata edit is one undo step -- undo restores blend/cutoff/twoSided, "
          "redo re-applies the clamped state",
          "[editor][material][mesh]")
{
    const fs::path dir = TempDir("mesh_metadata_undo");
    const fs::path meshFile = dir / "mesh.arcmat";

    Arcane::MaterialAssetData mesh;
    mesh.id = Arcane::Guid::Generate();
    mesh.name = "Mesh metadata undo";
    mesh.kind = "mesh";
    REQUIRE(Arcane::SaveMaterialAsset(meshFile, mesh));
    const auto loaded = Arcane::LoadMaterialAsset(meshFile);
    REQUIRE(loaded.has_value());

    // A real CommandStack over an EMPTY registry (EditGestureTest's fixture):
    // the metadata command never snapshots a component, so `resolve` is never
    // called; and no Arcane::Runtime, which would steal the TypeContext.
    Astra::Registry registry;
    Arcane::CommandStack stack{[&registry]() -> Astra::Registry& { return registry; }};
    DocServices services;
    services.undo = [p = &stack]() -> Arcane::CommandStack* { return p; };
    ShaderEditorDocument doc(services, meshFile, *loaded);

    using State = ShaderEditorDocument::MeshMaterialMetadataState;
    const auto captured = [&]() -> State
    {
        const std::optional<State> s = doc.CaptureMeshMaterialMetadata();
        REQUIRE(s.has_value());
        return *s;
    };
    const auto isUnset = [](const State& s)
    {
        return !s.blend.has_value() && !s.alphaCutoff.has_value() && !s.twoSided.has_value();
    };
    const auto isEdited = [](const State& s)
    {
        return s.blend == Arcane::MaterialBlendMode::Transparent && s.alphaCutoff == 1.0f && s.twoSided == true;
    };
    REQUIRE(isUnset(captured()));
    CHECK_FALSE(stack.CanUndo());

    // THE EDIT. The cutoff is out of range ON PURPOSE: Redo must re-apply the
    // state that LANDED (clamped to 1.0), never the request.
    State edit;
    edit.blend       = Arcane::MaterialBlendMode::Transparent;
    edit.alphaCutoff = 2.0f;
    edit.twoSided    = true;
    doc.SetMeshMaterialMetadataWithUndo(edit);
    CHECK(isEdited(captured()));
    CHECK(doc.Dirty());
    REQUIRE(stack.CanUndo());
    CHECK_FALSE(stack.CanRedo());
    CHECK(std::string(stack.UndoLabel()) == "Edit Blend");
    CHECK(stack.SceneStateId() == 0);

    // ONE undo restores all three fields to the capture...
    stack.Undo();
    CHECK(isUnset(captured()));
    CHECK(stack.CanRedo());
    CHECK_FALSE(stack.CanUndo());

    // ...and redo re-applies the landed state.
    stack.Redo();
    CHECK(isEdited(captured()));

    // A second edit is its own step: one undo reverts only it, the first stays.
    State second = captured();
    second.blend = Arcane::MaterialBlendMode::Masked;
    doc.SetMeshMaterialMetadataWithUndo(second);
    CHECK(captured().blend == Arcane::MaterialBlendMode::Masked);
    CHECK(std::string(stack.UndoLabel()) == "Edit Blend");
    stack.Undo();
    CHECK(isEdited(captured()));
    CHECK(stack.CanUndo());   // the first edit is still a step of its own
    CHECK(stack.CanRedo());

    // A write that changes nothing pushes nothing -- redo stays intact, where a
    // pushed step would have cleared it. A request the clamp collapses back
    // onto the current value is "nothing" too.
    doc.SetMeshMaterialMetadataWithUndo(captured());
    CHECK(stack.CanRedo());
    State clampedSame = captured();
    clampedSame.alphaCutoff = 7.0f;   // clamps to 1.0, which is the current value
    doc.SetMeshMaterialMetadataWithUndo(clampedSame);
    CHECK(stack.CanRedo());
    CHECK(captured().alphaCutoff == 1.0f);

    // Each field's label names the field, for the Undo menu.
    State cutoffOnly = captured();
    cutoffOnly.alphaCutoff = 0.25f;
    doc.SetMeshMaterialMetadataWithUndo(cutoffOnly);
    CHECK(std::string(stack.UndoLabel()) == "Edit Alpha Cutoff");
    State twoSidedOnly = captured();
    twoSidedOnly.twoSided = false;
    doc.SetMeshMaterialMetadataWithUndo(twoSidedOnly);
    CHECK(std::string(stack.UndoLabel()) == "Edit Two Sided");

    // Not a mesh surface: no metadata, and no step.
    {
        const fs::path spriteFile = dir / "sprite.arcmat";
        Arcane::MaterialAssetData sprite;
        sprite.id = Arcane::Guid::Generate();
        sprite.name = "sprite";
        sprite.kind = "sprite";
        sprite.snippet = kSnippet;
        REQUIRE(Arcane::SaveMaterialAsset(spriteFile, sprite));
        const auto loadedSprite = Arcane::LoadMaterialAsset(spriteFile);
        REQUIRE(loadedSprite.has_value());
        Astra::Registry spriteRegistry;
        Arcane::CommandStack spriteStack{[&spriteRegistry]() -> Astra::Registry& { return spriteRegistry; }};
        DocServices spriteServices;
        spriteServices.undo = [p = &spriteStack]() -> Arcane::CommandStack* { return p; };
        ShaderEditorDocument spriteDoc(spriteServices, spriteFile, *loadedSprite);
        spriteDoc.SetMeshMaterialMetadataWithUndo(edit);
        CHECK_FALSE(spriteStack.CanUndo());
    }
}

TEST_CASE("ShaderEditorDocument resolves, and refuses, instance parent chains", "[editor][material]")
{
    const fs::path dir = TempDir("chains");
    REQUIRE(Arcane::Project::Create(dir / "Game", "ChainTest").has_value());
    const fs::path content = dir / "Game" / "Content";

    // Base material + healthy instance + a two-node parent cycle + an orphan.
    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate();
    base.name = "Base";
    base.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", base));

    Arcane::MaterialAssetData inst;
    inst.id = Arcane::Guid::Generate();
    inst.parent = base.id;
    inst.name = "Inst";
    REQUIRE(Arcane::SaveMaterialAsset(content / "inst.arcmat", inst));

    Arcane::MaterialAssetData cycleA, cycleB;
    cycleA.id = Arcane::Guid::Generate();
    cycleB.id = Arcane::Guid::Generate();
    cycleA.parent = cycleB.id;
    cycleB.parent = cycleA.id;
    cycleA.name = "CycleA";
    cycleB.name = "CycleB";
    REQUIRE(Arcane::SaveMaterialAsset(content / "cycle_a.arcmat", cycleA));
    REQUIRE(Arcane::SaveMaterialAsset(content / "cycle_b.arcmat", cycleB));

    Arcane::MaterialAssetData orphan;
    orphan.id = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();   // never registered
    orphan.name = "Orphan";
    REQUIRE(Arcane::SaveMaterialAsset(content / "orphan.arcmat", orphan));

    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));

    DocServices services;
    services.runtime = &rt;   // no compiler/sources/device: chain logic only

    SECTION("a healthy chain resolves with no errors")
    {
        const auto data = Arcane::LoadMaterialAsset(content / "inst.arcmat");
        REQUIRE(data.has_value());
        ShaderEditorDocument doc(services, content / "inst.arcmat", *data);
        CHECK(doc.IsInstance());
        REQUIRE(doc.Page() != nullptr);
        CHECK(doc.Page()->Breadcrumb()[0].label == "Inst (Instance)");   // matches the window label (s4.3)
        CHECK(doc.ParseErrors().empty());
    }

    SECTION("a parent cycle is rejected")
    {
        const auto data = Arcane::LoadMaterialAsset(content / "cycle_a.arcmat");
        REQUIRE(data.has_value());
        ShaderEditorDocument doc(services, content / "cycle_a.arcmat", *data);
        CHECK(AnyErrorContains(doc.ParseErrors(), "cycle"));
    }

    SECTION("an unresolvable parent is reported")
    {
        const auto data = Arcane::LoadMaterialAsset(content / "orphan.arcmat");
        REQUIRE(data.has_value());
        ShaderEditorDocument doc(services, content / "orphan.arcmat", *data);
        CHECK(AnyErrorContains(doc.ParseErrors(), "not in the asset registry"));
    }

    SECTION("instances need an open project at all")
    {
        const auto data = Arcane::LoadMaterialAsset(content / "inst.arcmat");
        REQUIRE(data.has_value());
        ShaderEditorDocument doc(DocServices{}, content / "inst.arcmat", *data);
        CHECK(AnyErrorContains(doc.ParseErrors(), "open project"));
    }
}

TEST_CASE("ShaderEditorDocument::ConsumeResult routes only its own job ids", "[editor][material][shadercompile]")
{
    // Real compile service + real template, no device: the ctor submits both
    // stages; every drained result must route back (return true), and a foreign
    // job id must be refused -- the stale-result guard the drain site relies on.
    const fs::path dir = TempDir("routing");
    const fs::path file = dir / "routed.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Routed";
    data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    REQUIRE(sources.Get("materials/fullscreen_material.hlsl").has_value());

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());   // template found, snippet parsed

    // Both stage jobs were submitted at now=0 with zero debounce.
    std::vector<Arcane::ShaderCompileResult> results;
    for (int i = 0; i < 2000 && results.size() < 2; ++i)
    {
        compiler.Poll(/*now=*/0.0);
        auto batch = compiler.Drain();
        results.insert(results.end(), std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
        if (results.size() < 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(results.size() == 2);

    for (Arcane::ShaderCompileResult& r : results)
    {
        INFO("stage result " << r.debugName);
        CHECK(r.AllSucceeded());
        CHECK(doc.ConsumeResult(r));   // routed home
    }

    Arcane::ShaderCompileResult foreign = results[0];
    foreign.jobId = 0xDEADBEEFull;      // nobody's job
    CHECK_FALSE(doc.ConsumeResult(foreign));

    compiler.Shutdown();
}

TEST_CASE("ShaderEditorDocument compiles a pass chain per-pass and routes results",
          "[editor][material][shadercompile]")
{
    // A 3-pass chain submits BOTH stages for EVERY pass (6 jobs, distinct
    // per-pass coalesce keys) and each drained result routes home. Device-less:
    // binding is skipped, but the whole chain-source path runs for real.
    const fs::path dir = TempDir("chainrouting");
    const fs::path file = dir / "chained.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Chained";
    data.snippet = kSnippet;
    data.passes.push_back({ "swap",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv).grba; }\n" });
    data.passes.push_back({ "gain",
        "//@param float Gain = 1\n"
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv) * Gain; }\n" });
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->passes.size() == 2);
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());

    std::vector<Arcane::ShaderCompileResult> results;
    for (int i = 0; i < 2000 && results.size() < 6; ++i)
    {
        compiler.Poll(/*now=*/0.0);
        auto batch = compiler.Drain();
        results.insert(results.end(), std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
        if (results.size() < 6)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(results.size() == 6);

    for (Arcane::ShaderCompileResult& r : results)
    {
        INFO("chain stage result " << r.debugName);
        CHECK(r.AllSucceeded());
        CHECK(doc.ConsumeResult(r));
    }

    compiler.Shutdown();
}

TEST_CASE("ShaderEditorDocument T3-D6: an instance of a pass-chain material compiles its BASE's chain (one job pair per pass)",
          "[editor][material][shadercompile]")
{
    // T3-D6 desk finding: reference_post_Inst (an instance of the multi-pass
    // ReferencePost) read "errors" with no preview, in the editor and
    // headless alike. ChainMode() is the AUTHORING predicate (an instance has
    // no canvas, no pass strip), and Rebuild/BindIfComplete used it as the
    // COMPILE predicate too -- so an instance of a chain base went down the
    // single-pass path with the base snippet alone: the base's extra passes
    // were dropped and a scene-reading base snippet failed outright. An
    // instance must compile exactly what its base compiles (UE: a Material
    // Instance previews its parent's whole material), with its own params.
    const fs::path dir = TempDir("chaininstance");
    REQUIRE(Arcane::Project::Create(dir / "Game", "ChainInstance").has_value());
    const fs::path content = dir / "Game" / "Content";

    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate();
    base.name = "ChainBase";
    base.snippet = kSnippet;
    base.passes.push_back({ "swap",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv).grba; }\n" });
    base.passes.push_back({ "gain",
        "//@param float Gain = 1\n"
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv) * Gain; }\n" });
    REQUIRE(Arcane::SaveMaterialAsset(content / "chain_base.arcmat", base));

    Arcane::MaterialAssetData inst;
    inst.id = Arcane::Guid::Generate();
    inst.parent = base.id;
    inst.name = "ChainBase_Inst";
    inst.params.emplace_back("Gain", Arcane::MatParamValue::MakeFloat(0.5f));
    REQUIRE(Arcane::SaveMaterialAsset(content / "chain_base_inst.arcmat", inst));

    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    DocServices services;
    services.runtime = &rt;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(content / "chain_base_inst.arcmat");
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(services, content / "chain_base_inst.arcmat", *loaded);
    REQUIRE(doc.IsInstance());
    INFO("parse errors: " << (doc.ParseErrors().empty() ? std::string("none") : doc.ParseErrors().front()));
    CHECK(doc.ParseErrors().empty());

    // Base + 2 passes = 3 passes x 2 stages. The single-pass path submits 2.
    std::vector<Arcane::ShaderCompileResult> results;
    for (int i = 0; i < 2000 && results.size() < 6; ++i)
    {
        compiler.Poll(/*now=*/0.0);
        auto batch = compiler.Drain();
        results.insert(results.end(), std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
        if (results.size() < 6)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(results.size() == 6);
    for (Arcane::ShaderCompileResult& r : results)
    {
        INFO("chain stage result " << r.debugName);
        CHECK(r.AllSucceeded());
        CHECK(doc.ConsumeResult(r));
    }
    CHECK(doc.ComputeStatus().compile == Arcane::Editor::CompileStatus::Ok);   // all stages landed, none failed
    compiler.Shutdown();
}

TEST_CASE("ShaderEditorDocument T3-D6 fix 1: a chain INSTANCE publishes a failing pass 1+ to Problems, named by its base's pass",
          "[editor][material][shadercompile][diagnostics]")
{
    // Review finding (T3-D6 fix round 1): Rebuild/BindIfComplete compile an
    // instance of a chain base per pass (CompilesAsChain), and ConsumeResult
    // keeps each pass's diags in its PassJobs, mirroring only pass 0 into
    // m_diags. ForEachDiagnosticRow still branched on the AUTHORING ChainMode()
    // (false for every instance), so it walked m_diags alone: a failing 'gain'
    // pass read "errors" on the toolbar while Problems held nothing for it.
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const fs::path dir = TempDir("chaininstancediag");
    REQUIRE(Arcane::Project::Create(dir / "Game", "ChainInstanceDiag").has_value());
    const fs::path content = dir / "Game" / "Content";

    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate();
    base.name = "BrokenChainBase";
    base.snippet = kSnippet;
    base.passes.push_back({ "swap",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv).grba; }\n" });
    base.passes.push_back({ "gain",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv) * kNoSuchGainSymbol; }\n" });
    REQUIRE(Arcane::SaveMaterialAsset(content / "broken_chain_base.arcmat", base));

    Arcane::MaterialAssetData inst;
    inst.id = Arcane::Guid::Generate();
    inst.parent = base.id;
    inst.name = "BrokenChainBase_Inst";
    REQUIRE(Arcane::SaveMaterialAsset(content / "broken_chain_base_inst.arcmat", inst));

    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    DocServices services;
    services.runtime = &rt;
    services.compiler = &compiler;
    services.sources = &sources;

    {
        const auto loaded = Arcane::LoadMaterialAsset(content / "broken_chain_base_inst.arcmat");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(services, content / "broken_chain_base_inst.arcmat", *loaded);
        REQUIRE(doc.IsInstance());
        REQUIRE(doc.ParseErrors().empty());   // the stitch is fine; the compile is not

        std::vector<Arcane::ShaderCompileResult> results;
        for (int i = 0; i < 2000 && results.size() < 6; ++i)
        {
            compiler.Poll(/*now=*/0.0);
            auto batch = compiler.Drain();
            results.insert(results.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
            if (results.size() < 6)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(results.size() == 6);
        for (Arcane::ShaderCompileResult& r : results)
            CHECK(doc.ConsumeResult(r));
        CHECK(doc.ComputeStatus().compile == Arcane::Editor::CompileStatus::Errors);

        doc.PublishDiagnostics();
        const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
        bool gainRow = false;
        for (const Arcane::Diagnostic& d : rows)
        {
            UNSCOPED_INFO("published: " << d.message);
            if (d.severity == Arcane::DiagSeverity::Error &&
                d.message.find("gain: error") != std::string::npos &&
                d.message.find("kNoSuchGainSymbol") != std::string::npos)
                gainRow = true;
        }
        INFO("rows published: " << rows.size());
        CHECK(gainRow);   // the cause is surfaced, named by the base's pass
    }
    compiler.Shutdown();
    store.UninstallEngineSink();
}

TEST_CASE("ShaderEditorDocument T3-D6 fix 1: an instance publishes its BASE's broken vertex stage as a vertex row",
          "[editor][material][shadercompile][diagnostics]")
{
    // Review finding (T3-D6 fix round 1): Rebuild measures m_vsLineOffset
    // against the COMPILED vertex snippet (the base's, for an instance), but
    // HasErrors and ForEachDiagnosticRow filtered against m_data.vertexSnippet
    // -- empty for every instance -- so a broken base vertex stage never
    // produced its "vertex:" row (nor its vertex-body HasErrors hit).
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const fs::path dir = TempDir("instancevertexdiag");
    REQUIRE(Arcane::Project::Create(dir / "Game", "InstanceVertexDiag").has_value());
    const fs::path content = dir / "Game" / "Content";

    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate();
    base.name = "BrokenVertexBase";
    base.snippet = kSnippet;
    base.vertexSnippet =
        "Varyings displace(Varyings v)\n"
        "{\n"
        "    v.pos.x += kNoSuchVertexSymbol;\n"
        "    return v;\n"
        "}\n";
    REQUIRE(Arcane::SaveMaterialAsset(content / "broken_vertex_base.arcmat", base));

    Arcane::MaterialAssetData inst;
    inst.id = Arcane::Guid::Generate();
    inst.parent = base.id;
    inst.name = "BrokenVertexBase_Inst";
    REQUIRE(Arcane::SaveMaterialAsset(content / "broken_vertex_base_inst.arcmat", inst));

    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    DocServices services;
    services.runtime = &rt;
    services.compiler = &compiler;
    services.sources = &sources;

    {
        const auto loaded = Arcane::LoadMaterialAsset(content / "broken_vertex_base_inst.arcmat");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(services, content / "broken_vertex_base_inst.arcmat", *loaded);
        REQUIRE(doc.IsInstance());
        REQUIRE(doc.ParseErrors().empty());

        std::vector<Arcane::ShaderCompileResult> results;
        for (int i = 0; i < 2000 && results.size() < 2; ++i)
        {
            compiler.Poll(/*now=*/0.0);
            auto batch = compiler.Drain();
            results.insert(results.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
            if (results.size() < 2)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(results.size() == 2);
        for (Arcane::ShaderCompileResult& r : results)
            CHECK(doc.ConsumeResult(r));
        CHECK(doc.ComputeStatus().compile == Arcane::Editor::CompileStatus::Errors);

        doc.PublishDiagnostics();
        const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
        bool vertexRow = false;
        for (const Arcane::Diagnostic& d : rows)
        {
            UNSCOPED_INFO("published: " << d.message);
            if (d.severity == Arcane::DiagSeverity::Error &&
                d.message.find("vertex: error(3)") != std::string::npos)
                vertexRow = true;   // line 3 of the BASE's vertex body
        }
        INFO("rows published: " << rows.size());
        CHECK(vertexRow);
    }
    compiler.Shutdown();
    store.UninstallEngineSink();
}

TEST_CASE("ReloadFromDisk discards the working copy; DependsOn walks the chain",
          "[editor][material]")
{
    // The material file watcher's document hooks (external edits: git pull,
    // sibling repo, hand edits).
    const fs::path dir = TempDir("reload");
    REQUIRE(Arcane::Project::Create(dir / "Game", "ReloadTest").has_value());
    const fs::path content = dir / "Game" / "Content";

    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate();
    base.name = "Base";
    base.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", base));

    Arcane::MaterialAssetData inst;
    inst.id = Arcane::Guid::Generate();
    inst.parent = base.id;
    inst.name = "Inst";
    REQUIRE(Arcane::SaveMaterialAsset(content / "inst.arcmat", inst));

    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    DocServices services;
    services.runtime = &rt;

    SECTION("reload picks up an external rewrite")
    {
        const auto loaded = Arcane::LoadMaterialAsset(content / "base.arcmat");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(services, content / "base.arcmat", *loaded);

        Arcane::MaterialAssetData edited = base;
        edited.name = "BaseRenamed";
        edited.snippet = "float4 shade(Varyings v) { return 0.5; }\n";
        REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", edited));

        doc.ReloadFromDisk();
        CHECK_FALSE(doc.Dirty());
        CHECK(doc.Title() == "BaseRenamed");
        REQUIRE(doc.Save());   // saving right back writes the DISK version
        const auto back = Arcane::LoadMaterialAsset(content / "base.arcmat");
        REQUIRE(back.has_value());
        CHECK(back->snippet == edited.snippet);
    }
    SECTION("DependsOn is the resolved parent chain, not guesswork")
    {
        const auto loaded = Arcane::LoadMaterialAsset(content / "inst.arcmat");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(services, content / "inst.arcmat", *loaded);
        REQUIRE(doc.ParseErrors().empty());
        CHECK(doc.DependsOn(base.id));
        CHECK_FALSE(doc.DependsOn(inst.id));
        CHECK_FALSE(doc.DependsOn(Arcane::Guid::Generate()));

        // A parent edit refreshes the chain (device-less: resolve-only proof).
        Arcane::MaterialAssetData edited = base;
        edited.snippet = "float4 shade(Varyings v) { return 1.0; }\n";
        REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", edited));
        doc.RefreshParentChain();
        CHECK(doc.ParseErrors().empty());
        CHECK(doc.DependsOn(base.id));
    }
}

TEST_CASE("PatchParamRename re-keys saved params with the merge rule",
          "[editor][material]")
{
    // The open-document half of assisted rename: the base's propagation
    // rewrote this instance's FILE; the patch keeps memory in step and Save
    // must never write the orphan back.
    const fs::path dir = TempDir("patchrename");
    const fs::path file = dir / "inst.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.parent = Arcane::Guid::Generate();   // unresolvable is fine device-less
    data.name = "Inst";
    data.params.emplace_back("Speed", Arcane::MatParamValue::MakeFloat(3.5f));
    data.params.emplace_back("Tint", Arcane::MatParamValue::MakeColor(1, 0, 0, 1));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(DocServices{}, file, *loaded);

    SECTION("plain re-key")
    {
        doc.PatchParamRename("Speed", "Rate");
        REQUIRE(doc.Save());
        const auto back = Arcane::LoadMaterialAsset(file);
        REQUIRE(back.has_value());
        REQUIRE(back->params.size() == 2);
        CHECK(back->params[0].first == "Rate");
        CHECK(back->params[0].second.f[0] == 3.5f);
        CHECK(back->params[1].first == "Tint");
    }
    SECTION("merge rule: an existing new-name value wins, the orphan drops")
    {
        doc.PatchParamRename("Speed", "Tint");
        REQUIRE(doc.Save());
        const auto back = Arcane::LoadMaterialAsset(file);
        REQUIRE(back.has_value());
        REQUIRE(back->params.size() == 1);
        CHECK(back->params[0].first == "Tint");
        CHECK(back->params[0].second.f[0] == 1.0f);   // the Tint color's red
    }
    SECTION("absent old name is a no-op")
    {
        doc.PatchParamRename("NotThere", "Rate");
        REQUIRE(doc.Save());
        const auto back = Arcane::LoadMaterialAsset(file);
        REQUIRE(back.has_value());
        CHECK(back->params.size() == 2);
        CHECK(back->params[0].first == "Speed");
    }
}

TEST_CASE("A base-only scene-reading material compiles through the chain path",
          "[editor][material][shadercompile]")
{
    // The common POST material: no extra passes, the base reads the Scene.
    // ChainMode must cover it (post-mode chain build, InputTexture bound),
    // and both stage jobs must route home.
    const fs::path dir = TempDir("postbase");
    const fs::path file = dir / "grade.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Grade";
    data.snippet = "float4 shade(Varyings v)\n"
                   "{ return 1.0 - InputTexture.Sample(MaterialSampler, v.uv); }\n";
    data.baseInputs = { Arcane::kSceneInput };
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->baseInputs.size() == 1);
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());   // post-mode build accepted the scene read

    std::vector<Arcane::ShaderCompileResult> results;
    for (int i = 0; i < 2000 && results.size() < 2; ++i)
    {
        compiler.Poll(/*now=*/0.0);
        auto batch = compiler.Drain();
        results.insert(results.end(), std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
        if (results.size() < 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(results.size() == 2);
    for (Arcane::ShaderCompileResult& r : results)
    {
        INFO("post stage result " << r.debugName);
        CHECK(r.AllSucceeded());
        CHECK(doc.ConsumeResult(r));
    }
    compiler.Shutdown();
}

TEST_CASE("ApplyPassListState swaps the pass list, clamps selection, dirties",
          "[editor][material]")
{
    // The pass-canvas undo surface (PassListCommand forwards here): a stale
    // step must land safely even when its indices no longer fit the list.
    const fs::path dir = TempDir("passundo");
    const fs::path file = dir / "undoable.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Undoable";
    data.snippet = kSnippet;
    data.passes.push_back({ "swap",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv).grba; }\n" });
    data.passes.push_back({ "gain",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv); }\n" });
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(DocServices{}, file, *loaded);
    CHECK_FALSE(doc.Dirty());

    ShaderEditorDocument::PassListState s = doc.CapturePassListState();
    REQUIRE(s.passes.size() == 2);

    // Undo of an "Add Pass": one pass fewer, selection indices gone stale.
    s.passes.pop_back();
    s.activePass = 5;
    s.viewPass = 7;
    doc.ApplyPassListState(std::move(s));
    CHECK(doc.Dirty());

    const ShaderEditorDocument::PassListState now = doc.CapturePassListState();
    CHECK(now.passes.size() == 1);
    CHECK(now.passes[0].name == "swap");
    CHECK(now.activePass == 1);   // clamped to the new count
    CHECK(now.viewPass == 1);

    // Redo lands the removed pass back, rename and all.
    ShaderEditorDocument::PassListState redo = now;
    Arcane::MaterialPass gain;
    gain.name = "gain (renamed)";
    gain.snippet = "float4 shade(Varyings v) { return 1.0; }\n";
    redo.passes.push_back(std::move(gain));
    redo.activePass = 2;
    redo.viewPass = -1;
    doc.ApplyPassListState(std::move(redo));
    const ShaderEditorDocument::PassListState after = doc.CapturePassListState();
    REQUIRE(after.passes.size() == 2);
    CHECK(after.passes[1].name == "gain (renamed)");
    CHECK(after.activePass == 2);
    CHECK(after.viewPass == -1);
}

// ---------------------------------------------------------------------------
// s5.3: the material page on PropertyGrid. The preview/params split retired
// with its ini handler; a stale [ArcaneEditorLayout][MaterialPanel] section
// loads inert (no handler: ImGui skips it) and is never written back.
// ---------------------------------------------------------------------------
namespace
{
    // LogToBuffer prints a framed header as "### <label> ###": true when only
    // that decoration precedes `label`, i.e. the header is the body's first item.
    bool LogStartsWithHeader(const std::string& log, std::string_view label)
    {
        const std::size_t at = log.find(label);
        if (at == std::string::npos) return false;
        for (std::size_t i = 0; i < at; ++i)
            if (log[i] != '#' && !std::isspace(static_cast<unsigned char>(log[i]))) return false;
        return true;
    }

    // The page in a 400x600 "Inspector" window at the origin, device-less.
    struct PageUi
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        Arcane::Editor::PropertyGridState grid;
        std::unordered_map<std::string, ImVec2> probe;
        std::string logged;
        PageUi()
        {
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 900.0f);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
            grid.probe = &probe;
        }
        ~PageUi() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        void Frame(ShaderEditorDocument& doc, bool log = false)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            Arcane::Editor::PropertyGrid(grid).CommitOrphans();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(400, 600), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            if (log) ImGui::LogToBuffer();
            { Arcane::Editor::PropertyGrid g(grid); doc.Page()->Draw(g); }
            if (log) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
            ImGui::End();
            ImGui::Render();
        }
        ImVec2 At(const std::string& label) { INFO(label); REQUIRE(probe.count(label) == 1); return probe.at(label); }
        void Move(ShaderEditorDocument& d, ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); Frame(d); }
        void Button(ShaderEditorDocument& d, bool down) { ImGui::GetIO().AddMouseButtonEvent(0, down); Frame(d); }
        void Click(ShaderEditorDocument& d, ImVec2 p) { Move(d, p); Button(d, true); Button(d, false); }
    };
}

TEST_CASE("material page: a stale MaterialPanel ini section loads inert and is never written back", "[editor][material]")
{
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGui::GetIO().IniFilename = nullptr;
    CHECK(ImGui::FindSettingsHandler("ArcaneEditorLayout") == nullptr);   // no handler registers it any more
    ImGui::LoadIniSettingsFromMemory("[ArcaneEditorLayout][MaterialPanel]\nPreviewSplit=0.6900\n");
    CHECK(std::string(ImGui::SaveIniSettingsToMemory()).find("MaterialPanel") == std::string::npos);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("A material document publishes its diagnostics under its own key", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const fs::path dir = TempDir("diagpublish");
    REQUIRE(Arcane::Project::Create(dir / "Game", "DiagTest").has_value());
    const fs::path content = dir / "Game" / "Content";

    // An instance whose parent was never registered: ResolveParentChain fails,
    // which fills m_parseErrors -- diagnostic rows without a compile.
    Arcane::MaterialAssetData orphan;
    orphan.id     = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();   // never registered
    orphan.name   = "Orphan";
    REQUIRE(Arcane::SaveMaterialAsset(content / "orphan.arcmat", orphan));

    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));

    DocServices services;
    services.runtime = &rt;   // no compiler/sources/device

    const auto data = Arcane::LoadMaterialAsset(content / "orphan.arcmat");
    REQUIRE(data.has_value());

    {
        ShaderEditorDocument doc(services, content / "orphan.arcmat", *data);
        REQUIRE_FALSE(doc.ParseErrors().empty());
        doc.PublishDiagnostics();

        const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
        REQUIRE_FALSE(rows.empty());
        CHECK(rows[0].scope == Arcane::DiagScope::Material);
        CHECK(rows[0].severity == Arcane::DiagSeverity::Error);
    }
    // Destructor retracts the key: a closed document must not leave rows behind.
    CHECK(store.Snapshot().empty());

    store.UninstallEngineSink();
}

TEST_CASE("Republishing an identical diagnostic set is idempotent", "[diagnostics]")
{
    // This is what retired the FNV-1a signature gate: publication groups replace,
    // so calling PublishDiagnostics on every frame cannot accumulate rows.
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const fs::path dir = TempDir("diagidempotent");
    REQUIRE(Arcane::Project::Create(dir / "Game", "IdemTest").has_value());
    const fs::path content = dir / "Game" / "Content";

    Arcane::MaterialAssetData orphan;
    orphan.id     = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();
    orphan.name   = "Orphan";
    REQUIRE(Arcane::SaveMaterialAsset(content / "orphan.arcmat", orphan));

    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));

    DocServices services;
    services.runtime = &rt;

    const auto data = Arcane::LoadMaterialAsset(content / "orphan.arcmat");
    REQUIRE(data.has_value());
    ShaderEditorDocument doc(services, content / "orphan.arcmat", *data);

    doc.PublishDiagnostics();
    const std::size_t once = store.Snapshot().size();
    REQUIRE(once > 0);

    for (int i = 0; i < 10; ++i) doc.PublishDiagnostics();
    CHECK(store.Snapshot().size() == once);

    store.UninstallEngineSink();
}

// =====================================================================
// THE SEVERANCE
// =====================================================================
// A document RETAINS its compiled bytecode and publishes it as a device-free
// description -- exactly the shape PostChainCache publishes for a scene post
// material, and exactly what the graph's PostChainNode consumes. Nothing is
// dropped for want of a device.
//
// Same headless idiom as SeveranceTest.cpp's [render][severance] cases: a real
// dxc compile through the app-shared ShaderCompiler, no device
// anywhere. What they pin is the DATA SUPPLY -- that a compile with no device
// still produces something a second recorder could render. They cannot pin the
// render itself (that needs an NriGraphContext and therefore a device); the
// graph preview vehicle is null here, which the last CHECK in each case states
// rather than leaves implied.

namespace
{
    // Drain the compile service until `count` results have landed and route
    // every one of them home. The two existing routing cases spell this out
    // inline; the severance cases below need it three more times.
    void DrainInto(Arcane::ShaderCompiler& compiler, ShaderEditorDocument& doc,
                   std::size_t count)
    {
        std::vector<Arcane::ShaderCompileResult> results;
        for (int i = 0; i < 2000 && results.size() < count; ++i)
        {
            compiler.Poll(/*now=*/0.0);
            auto batch = compiler.Drain();
            results.insert(results.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
            if (results.size() < count)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(results.size() == count);
        for (Arcane::ShaderCompileResult& r : results)
        {
            INFO("stage result " << r.debugName);
            REQUIRE(r.AllSucceeded());
            CHECK(doc.ConsumeResult(r));
        }
    }
}

TEST_CASE("severance: a DEVICE-LESS fullscreen material publishes its preview as bytes",
          "[editor][material][shadercompile][severance]")
{
    const fs::path dir = TempDir("severance_fullscreen");
    const fs::path file = dir / "glow.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Glow";
    data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    REQUIRE(sources.Get("materials/fullscreen_material.hlsl").has_value());

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;
    // DocServices has no device member at all: services is built with only
    // compiler/sources below, which is the shape a real document gets.

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());

    // Nothing published before a full pair of stages lands.
    CHECK(doc.GraphPreviewDesc().passes.empty());

    DrainInto(compiler, doc, 2);

    const Arcane::PostChainDesc& desc = doc.GraphPreviewDesc();
    REQUIRE(desc.passes.size() == 1);
    REQUIRE(desc.passes[0].vsBytes != nullptr);
    REQUIRE(desc.passes[0].psBytes != nullptr);
    CHECK_FALSE(desc.passes[0].vsBytes->empty());
    CHECK_FALSE(desc.passes[0].psBytes->empty());
    // A non-chain fullscreen material declares NO InputTexture, so its graph
    // description is a one-pass chain with ZERO input slots. That number is
    // what PostChainNode sizes its texture range with
    // (templ->TextureCount() + chainInputSlots), i.e. the same arithmetic the
    // source generator used -- getting it wrong is a bound-resource mismatch,
    // not a compile error.
    CHECK(desc.chainInputSlots == 0);
    CHECK(desc.passes[0].inputs.empty());
    // The merged template and the LIVE instance ride with it: PackCB reads the
    // instance every frame, so a param edit reaches the graph recorder with no
    // recompile at all.
    REQUIRE(desc.templ != nullptr);
    REQUIRE(desc.instance != nullptr);
    CHECK(desc.templ->Params().size() == 2);   // Tint + Speed, from kSnippet
    // A fullscreen material is not a sprite -- the two publications are
    // mutually exclusive, so a consumer cannot be handed both.
    CHECK(doc.SpritePreviewBlobs().vs == nullptr);
    CHECK(doc.SpritePreviewBlobs().ps == nullptr);
    // ...and with no NRI device in these services there is no vehicle, so
    // nothing was rendered and there is no texture to draw.
    CHECK(doc.GraphPreviewTextureId() == 0);

    compiler.Shutdown();
}

TEST_CASE("severance: a DEVICE-LESS pass chain publishes every pass and its wiring",
          "[editor][material][shadercompile][severance]")
{
    const fs::path dir = TempDir("severance_chain");
    const fs::path file = dir / "chained.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Chained";
    data.snippet = kSnippet;
    data.passes.push_back({ "swap",
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv).grba; }\n" });
    data.passes.back().inputs = { 0u };            // reads the base pass
    data.passes.push_back({ "gain",
        "//@param float Gain = 1\n"
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv) * Gain; }\n" });
    data.passes.back().inputs = { 1u };            // reads "swap"
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());

    DrainInto(compiler, doc, 6);   // both stages x three passes

    const Arcane::PostChainDesc& desc = doc.GraphPreviewDesc();
    REQUIRE(desc.passes.size() == 3);
    for (const Arcane::PostChainPassDesc& p : desc.passes)
    {
        REQUIRE(p.vsBytes != nullptr);
        REQUIRE(p.psBytes != nullptr);
        CHECK_FALSE(p.vsBytes->empty());
        CHECK_FALSE(p.psBytes->empty());
    }
    // THE WIRING SURVIVES THE SEVERANCE, which is the half a bytes-only
    // publication could silently lose: execution order is span order, and a
    // pass's inputs name EARLIER passes, so dropping them would run a chain
    // that samples the wrong intermediate with no error anywhere.
    CHECK(desc.passes[0].inputs.empty());
    REQUIRE(desc.passes[1].inputs.size() == 1);
    CHECK(desc.passes[1].inputs[0] == 0u);
    REQUIRE(desc.passes[2].inputs.size() == 1);
    CHECK(desc.passes[2].inputs[0] == 1u);
    // Every pass's source carries the SAME InputTexture decl count -- the
    // binding layout's shape, not any one pass's wiring.
    CHECK(desc.chainInputSlots >= 1);
    REQUIRE(desc.templ != nullptr);
    REQUIRE(desc.instance != nullptr);
    CHECK(doc.GraphPreviewTextureId() == 0);

    compiler.Shutdown();
}

TEST_CASE("severance: a DEVICE-LESS sprite material publishes its blobs, not a chain",
          "[editor][material][shadercompile][severance]")
{
    const fs::path dir = TempDir("severance_sprite");
    const fs::path file = dir / "spr.arcmat";

    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Spr";
    data.kind = "sprite";      // MaterialSurfaceForKind -> Sprite
    data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(file, data));

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    REQUIRE(sources.Get("materials/sprite_material.hlsl").has_value());

    DocServices services;
    services.compiler = &compiler;
    services.sources = &sources;

    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(services, file, *loaded);
    REQUIRE(doc.ParseErrors().empty());

    DrainInto(compiler, doc, 2);

    // A sprite preview is a QUAD through a Batcher2D, not a fullscreen chain --
    // so what it publishes is Material2DDesc::vsBytes/psBytes, which is what
    // the graph's Batch2DNode builds its own pipeline from.
    const auto& blobs = doc.SpritePreviewBlobs();
    REQUIRE(blobs.vs != nullptr);
    REQUIRE(blobs.ps != nullptr);
    CHECK_FALSE(blobs.vs->empty());
    CHECK_FALSE(blobs.ps->empty());
    CHECK(doc.GraphPreviewDesc().passes.empty());   // and NOT a chain
    CHECK(doc.GraphPreviewTextureId() == 0);

    compiler.Shutdown();
}

TEST_CASE("ShaderEditorDocument: the Inspector's Ctrl+S keeps the save-with-errors guard, and the page draws the confirm",
          "[editor][material][inspector]")
{
    // Final fix S: the material's params live on the Inspector page, so its
    // Ctrl+S (InspectorWindows -> saveRequested -> the app's route) must go
    // through RequestSave's HasErrors confirm, never straight to Save().
    const fs::path dir = TempDir("inspectorsave");
    REQUIRE(Arcane::Project::Create(dir / "Game", "InspectorSave").has_value());
    const fs::path content = dir / "Game" / "Content";
    Arcane::MaterialAssetData orphan;              // parent never registered: parse errors, HasErrors()
    orphan.id     = Arcane::Guid::Generate();
    orphan.parent = Arcane::Guid::Generate();
    orphan.name   = "Orphan";
    const fs::path file = content / "orphan.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(file, orphan));
    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    DocServices services;
    services.runtime = &rt;
    const auto data = Arcane::LoadMaterialAsset(file);
    REQUIRE(data.has_value());
    ShaderEditorDocument doc(services, file, *data);
    REQUIRE_FALSE(doc.ParseErrors().empty());      // => HasErrors()

    std::error_code ec;
    fs::remove(file, ec);                          // a Save() would write it back
    REQUIRE_FALSE(fs::exists(file));
    const Arcane::Editor::InspectorSaveOutcome out = Arcane::Editor::RequestSaveFromInspector(&doc);
    CHECK(out.doc == &doc);
    CHECK(out.result == Arcane::Editor::SaveGestureResult::Deferred);   // parked behind the confirm: not a refusal
    CHECK_FALSE(fs::exists(file));                 // nothing saved yet...
    CHECK(doc.SaveWithErrorsPending());            // ...the confirm is pending

    // The confirm is reachable from the PAGE: one device-less frame that
    // draws only an Inspector-like window with the page (the document window
    // is not drawn -- a background tab) opens the modal.
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Arcane::Editor::PropertyGridState grid;
    for (int frame = 0; frame < 2; ++frame)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(400.0f, 600.0f));
        ImGui::Begin("Inspector");
        REQUIRE(doc.Page() != nullptr);
        Arcane::Editor::PropertyGrid pg(grid);
        doc.Page()->Draw(pg);
        ImGui::End();
        ImGui::Render();
    }
    CHECK_FALSE(doc.SaveWithErrorsPending());      // consumed by the page's draw
    bool modalOpen = false;
    for (const ImGuiPopupData& p : ctx->OpenPopupStack)
        if (p.Window && std::string(p.Window->Name).find("Save With Errors?") != std::string::npos) modalOpen = true;
    CHECK(modalOpen);
    CHECK_FALSE(fs::exists(file));
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    fs::remove_all(dir, ec);
}

TEST_CASE("material page T3-D6: a mesh BASE material's page keeps its preview square -- the lit-sphere box, the not-compiled-here note as its caption",
          "[editor][material][mesh][inspector]")
{
    // T3-D6 (spec s5.3 amendment, 2026-10-02): a mesh surface is never
    // compiled here, but it IS previewed -- the thumbnail's lit sphere in the
    // material's CURRENT params. The page keeps the square every base gets;
    // the old one-line note becomes the caption under it. Device-less here,
    // so the box names why it has no image instead of reading "compiling...".
    const fs::path dir = TempDir("mesh_page");
    const fs::path file = dir / "hero.arcmat";
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Hero";
    data.kind = "mesh";
    data.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));
    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(DocServices{}, file, *loaded);

    PageUi h;
    h.Frame(doc); h.Frame(doc, true);
    INFO(h.logged);
    CHECK(LogStartsWithHeader(h.logged, "Preview"));                       // the body opens on the square
    CHECK(h.logged.find("compiling...") == std::string::npos);
    CHECK(h.logged.find("No preview -- no GPU device") != std::string::npos);   // the box's honest reason
    CHECK(h.logged.find("Mesh material: not compiled here") != std::string::npos);   // the caption
    ImGuiWindow* box = nullptr;
    for (ImGuiWindow* win : h.ctx->Windows)
        if (std::string(win->Name).find("##preview") != std::string::npos) box = win;
    REQUIRE(box != nullptr);
    CHECK(box->Size.x == Catch::Approx(box->Size.y));                     // the square, like every base
    CHECK(box->Size.x > 100.0f);
}

TEST_CASE("material document T3-D6: a mesh INSTANCE's tab is its preview -- a full-tab box with the caption under it",
          "[editor][material][mesh]")
{
    // The user's desk finding (2026-10-02): meshes/Metal showed a dim line in
    // the tab and nothing in the Inspector -- no visual preview anywhere. The
    // instance tab owns the preview (T3-D5, user decision A), so a mesh
    // instance's tab now draws the box, filling the tab above its caption.
    const fs::path dir = TempDir("mesh_instance_tab");
    REQUIRE(Arcane::Project::Create(dir / "Game", "MeshInstanceTab").has_value());
    const fs::path content = dir / "Game" / "Content";
    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate(); base.name = "MeshBase"; base.kind = "mesh";
    base.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(0.2f, 0.4f, 0.6f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(content / "mesh_base.arcmat", base));
    Arcane::MaterialAssetData child;
    child.id = Arcane::Guid::Generate(); child.parent = base.id; child.name = "Metal";
    child.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(0.1f, 0.45f, 0.95f, 1.0f));
    const fs::path file = content / "metal.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(file, child));
    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    DocServices services;
    services.runtime = &rt;
    ShaderEditorDocument doc(services, file, *Arcane::LoadMaterialAsset(file));
    REQUIRE(doc.IsInstance());

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* px = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    std::string logged;
    for (int f = 0; f < 2; ++f)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_Always);
        if (f == 1) ImGui::LogToBuffer();
        bool close = false;
        doc.Draw(close);
        if (f == 1) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::Render();
    }
    INFO(logged);
    ImGuiWindow* docWin = nullptr;
    ImGuiWindow* box = nullptr;
    for (ImGuiWindow* win : ctx->Windows)
    {
        const std::string name = win->Name;
        if (name.find("###matdoc_") != std::string::npos && !(win->Flags & ImGuiWindowFlags_ChildWindow)) docWin = win;
        if (name.find("##preview") != std::string::npos) box = win;
    }
    REQUIRE(docWin != nullptr);
    REQUIRE(box != nullptr);                                              // the box, not a dim line
    CHECK(box->Size.x > 0.9f * docWin->Size.x - 2.0f * ImGui::GetStyle().WindowPadding.x);   // full width
    CHECK(box->Size.y > 0.7f * docWin->Size.y);                         // most of the tab's height
    CHECK(box->Pos.y + box->Size.y <= docWin->Pos.y + docWin->Size.y);  // the caption fits under it
    CHECK(logged.find("Mesh material: not compiled here") != std::string::npos);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("material document T3-D6: the mesh preview draws the LIVE params -- an inherited value, an override, an unsaved edit",
          "[editor][material][mesh]")
{
    // "It must reflect the CURRENT parameter values": the sphere reads the
    // bound instance (parent chain under this document's overrides), so an
    // edit shows before any save -- unlike the 64px thumbnail, which is
    // harvested from the saved file.
    const fs::path dir = TempDir("mesh_preview_live");
    REQUIRE(Arcane::Project::Create(dir / "Game", "MeshPreviewLive").has_value());
    const fs::path content = dir / "Game" / "Content";
    const Arcane::Guid albedo = Arcane::Guid::Generate();
    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate(); base.name = "MeshBase"; base.kind = "mesh";
    base.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(0.2f, 0.4f, 0.6f, 1.0f));
    base.params.emplace_back("albedo", Arcane::MatParamValue::MakeTexture(albedo));
    REQUIRE(Arcane::SaveMaterialAsset(content / "mesh_base.arcmat", base));
    Arcane::MaterialAssetData child;
    child.id = Arcane::Guid::Generate(); child.parent = base.id; child.name = "Paint";
    const fs::path file = content / "paint.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(file, child));
    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    DocServices services;
    services.runtime = &rt;
    ShaderEditorDocument doc(services, file, *Arcane::LoadMaterialAsset(file));
    REQUIRE(doc.IsInstance());

    // Inherited: no override of its own, the base's saved values.
    auto in = doc.MeshPreviewInputs();
    CHECK(in.baseColor[0] == Catch::Approx(0.2f));
    CHECK(in.baseColor[2] == Catch::Approx(0.6f));
    CHECK(in.albedo == albedo);

    // An unsaved override edit shows at once.
    doc.ApplyParamEdit(Arcane::HashParamName("baseColor"), /*hasValue=*/true,
                       Arcane::MatParamValue::MakeColor(0.95f, 0.15f, 0.1f, 1.0f));
    REQUIRE(doc.Dirty());
    in = doc.MeshPreviewInputs();
    CHECK(in.baseColor[0] == Catch::Approx(0.95f));
    CHECK(in.baseColor[1] == Catch::Approx(0.15f));
    CHECK(in.albedo == albedo);   // still inherited

    // Clearing the override falls back to the parent again.
    doc.ApplyParamEdit(Arcane::HashParamName("baseColor"), /*hasValue=*/false, Arcane::MatParamValue{});
    CHECK(doc.MeshPreviewInputs().baseColor[0] == Catch::Approx(0.2f));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- Node page + editor upgrades s3.2: the late-bound seam + PreviewStatus ----
namespace
{
    std::optional<Arcane::MaterialAssetData> WriteAndLoad(const fs::path& file, const char* kind)
    {
        Arcane::MaterialAssetData data;
        data.id   = Arcane::Guid::Generate();
        data.name = "Status";
        data.kind = kind;
        if (std::string(kind) != "mesh")
            data.snippet = kSnippet;
        else
        {
            data.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
            data.params.emplace_back("albedo", Arcane::MatParamValue::MakeTexture(Arcane::Guid{}));
        }
        if (!Arcane::SaveMaterialAsset(file, data))
            return std::nullopt;
        return Arcane::LoadMaterialAsset(file);
    }
}

TEST_CASE("ShaderEditorDocument status: no compiler reads compiler-unavailable, a mesh surface not-compiled-here, no seam no-device without latching", "[editor][material][preview]")
{
    using Arcane::Editor::CompileStatus;
    using Arcane::Editor::PreviewAvailability;
    const fs::path dir = TempDir("status_bare");
    {
        const auto loaded = WriteAndLoad(dir / "glow.arcmat", "fullscreen");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(DocServices{}, dir / "glow.arcmat", *loaded);
        for (int i = 0; i < 3; ++i) doc.Tick(1.0 / 60.0);
        const auto st = doc.ComputeStatus();
        CHECK(st.compile == CompileStatus::CompilerUnavailable);
        CHECK(st.preview == PreviewAvailability::NoDevice);
        CHECK_FALSE(st.image);
        CHECK(doc.PreviewVehicleAttempts() == 0);
    }
    {
        // Wired but not up (the boot window): still no-device, never a latch.
        const auto loaded = WriteAndLoad(dir / "glow2.arcmat", "fullscreen");
        REQUIRE(loaded.has_value());
        Arcane::HostConfig cfg;
        DocServices services;
        services.hostConfig  = &cfg;
        services.chromeGraph = [] { return static_cast<Arcane::NriGraphContext*>(nullptr); };
        ShaderEditorDocument doc(services, dir / "glow2.arcmat", *loaded);
        for (int i = 0; i < 3; ++i) doc.Tick(1.0 / 60.0);
        CHECK(doc.ComputeStatus().preview == PreviewAvailability::NoDevice);
        CHECK(doc.PreviewVehicleAttempts() == 0);
    }
    {
        const auto loaded = WriteAndLoad(dir / "hero.arcmat", "mesh");
        REQUIRE(loaded.has_value());
        ShaderEditorDocument doc(DocServices{}, dir / "hero.arcmat", *loaded);
        CHECK(doc.ComputeStatus().compile == CompileStatus::NotCompiledHere);
    }
}

TEST_CASE("ShaderEditorDocument status: compiling while jobs are in flight, ok once both stages land -- never compiling with no seam", "[editor][material][shadercompile][preview]")
{
    using Arcane::Editor::CompileStatus;
    using Arcane::Editor::PreviewAvailability;
    const fs::path dir = TempDir("status_compile");
    const auto loaded = WriteAndLoad(dir / "glow.arcmat", "fullscreen");
    REQUIRE(loaded.has_value());

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    DocServices services;
    services.compiler = &compiler;
    services.sources  = &sources;
    ShaderEditorDocument doc(services, dir / "glow.arcmat", *loaded);
    CHECK(doc.ComputeStatus().compile == CompileStatus::Compiling);   // two stages submitted

    DrainInto(compiler, doc, 2);
    const auto st = doc.ComputeStatus();
    CHECK(st.compile == CompileStatus::Ok);                // the old toolbar read "compiling..." forever here
    CHECK(st.preview == PreviewAvailability::NoDevice);
    CHECK_FALSE(st.image);
}



TEST_CASE("ShaderEditorDocument status: a recompile before the first lands (reload mid-compile) still ends ok, never stuck compiling", "[editor][material][shadercompile][preview]")
{
    using Arcane::Editor::CompileStatus;
    const fs::path dir = TempDir("status_recompile");
    const auto loaded = WriteAndLoad(dir / "glow.arcmat", "fullscreen");
    REQUIRE(loaded.has_value());

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    DocServices services;
    services.compiler = &compiler;
    services.sources  = &sources;
    ShaderEditorDocument doc(services, dir / "glow.arcmat", *loaded);
    REQUIRE(doc.ComputeStatus().compile == CompileStatus::Compiling);

    // RegenerateFromGraph -> Rebuild: both live ids invalidated, the counter
    // zeroed, a fresh pair submitted under the same coalesce keys.
    doc.ReloadFromDisk();
    CHECK(doc.ComputeStatus().compile == CompileStatus::Compiling);

    DrainInto(compiler, doc, 2);   // the superseded pair never runs; only the live pair lands
    CHECK(doc.ComputeStatus().compile == CompileStatus::Ok);
}

TEST_CASE("ShaderEditorDocument: the first non-null chromeGraph makes Tick build the preview vehicle exactly once", "[editor][material][preview][gpu]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    using Arcane::Editor::PreviewAvailability;
    Arcane::HostConfig cfg;
    cfg.backend  = Arcane::Test::kNativeBackend;
    cfg.headless = true;
    auto chrome = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(chrome != nullptr);

    const fs::path dir = TempDir("status_gpu");
    const auto loaded = WriteAndLoad(dir / "glow.arcmat", "fullscreen");
    REQUIRE(loaded.has_value());
    Arcane::NriGraphContext* live = nullptr;
    DocServices services;
    services.hostConfig  = &cfg;
    services.chromeGraph = [&] { return live; };
    ShaderEditorDocument doc(services, dir / "glow.arcmat", *loaded);
    for (int i = 0; i < 3; ++i) doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 0);

    live = &chrome->Graph();
    doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 1);
    for (int i = 0; i < 5; ++i) doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 1);
    CHECK(doc.ComputeStatus().preview == PreviewAvailability::Ready);
    CHECK(doc.GraphPreviewTextureId() != 0);
}

TEST_CASE("ShaderEditorDocument T3-D6: a MESH-surface material builds its preview vehicle and presents its sphere", "[editor][material][mesh][preview][gpu]")
{
    // Before T3-D6 Tick refused a vehicle to every mesh surface ("an image
    // nothing ever draws"); now the tab/page draws it, so the vehicle is built
    // once and the status reports a bound image.
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    using Arcane::Editor::PreviewAvailability;
    Arcane::HostConfig cfg;
    cfg.backend  = Arcane::Test::kNativeBackend;
    cfg.headless = true;
    auto chrome = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(chrome != nullptr);

    const fs::path dir = TempDir("mesh_preview_gpu");
    const auto loaded = WriteAndLoad(dir / "metal.arcmat", "mesh");
    REQUIRE(loaded.has_value());
    DocServices services;
    services.hostConfig  = &cfg;
    services.chromeGraph = [&] { return &chrome->Graph(); };
    ShaderEditorDocument doc(services, dir / "metal.arcmat", *loaded);
    const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
    for (int i = 0; i < 4; ++i) doc.Tick(1.0 / 60.0);
    CHECK(doc.PreviewVehicleAttempts() == 1);
    const auto status = doc.ComputeStatus();
    CHECK(status.compile == Arcane::Editor::CompileStatus::NotCompiledHere);   // the toolbar still says so
    CHECK(status.preview == PreviewAvailability::Ready);
    CHECK(status.image);
    CHECK(doc.GraphPreviewTextureId() != 0);
    CHECK(Arcane::RenderErrorCount() == errorsBefore);
}

TEST_CASE("ShaderEditorDocument: the material page body never repeats the title the header crumb carries", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("page_no_title");
    const fs::path file = dir / "probe.arcmat";
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "TitleProbe";
    data.kind = "mesh";                                   // no compiler needed: the body is the square + caption + params
    data.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(file, data));
    const auto loaded = Arcane::LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    ShaderEditorDocument doc(DocServices{}, file, *loaded);

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Arcane::Editor::PropertyGridState grid;
    std::string logged;
    for (int frame = 0; frame < 2; ++frame)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(400.0f, 600.0f));
        ImGui::Begin("Inspector");
        if (frame == 1) ImGui::LogToBuffer();
        Arcane::Editor::PropertyGrid pg(grid);
        doc.Page()->Draw(pg);
        if (frame == 1) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::End();
        ImGui::Render();
    }
    INFO(logged);
    CHECK(logged.find("not compiled here") != std::string::npos);   // the control: the body drew
    CHECK(logged.find("TitleProbe") == std::string::npos);          // no title line
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("ShaderEditorDocument: the toolbar names the real state -- no compiler, mesh surface (s5.2)", "[editor][material][preview]")
{
    const fs::path dir = TempDir("toolbar_status");
    Arcane::MaterialAssetData full;
    full.id = Arcane::Guid::Generate(); full.name = "Full"; full.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(dir / "full.arcmat", full));
    Arcane::MaterialAssetData mesh;
    mesh.id = Arcane::Guid::Generate(); mesh.name = "Mesh"; mesh.kind = "mesh";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "mesh.arcmat", mesh));
    ShaderEditorDocument noCompiler(DocServices{}, dir / "full.arcmat", *Arcane::LoadMaterialAsset(dir / "full.arcmat"));
    CHECK(Arcane::Editor::ToolbarStatusText(noCompiler.ComputeStatus())
          == "not compiled -- shader compiler unavailable (see the log)");
    ShaderEditorDocument meshDoc(DocServices{}, dir / "mesh.arcmat", *Arcane::LoadMaterialAsset(dir / "mesh.arcmat"));
    CHECK(Arcane::Editor::ToolbarStatusText(meshDoc.ComputeStatus()) == "not compiled here");
}

TEST_CASE("ShaderEditorDocument: \"Output preview\" replaces Thumbs and is disabled with no image", "[editor][material][preview]")
{
    Arcane::MaterialGraph g;
    Arcane::GraphNode out; out.id = 1; out.type = Arcane::GraphNodeType::Output; out.posX = 420.0f; out.posY = 200.0f;
    Arcane::GraphNode color; color.id = 2; color.type = Arcane::GraphNodeType::ConstColor; color.posX = 160.0f; color.posY = 200.0f;
    g.nodes = { out, color };
    Arcane::GraphLink l; l.fromNode = 2; l.toNode = 1;
    g.links.push_back(l);
    g.nextId = 3;
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate(); data.name = "thumbs"; data.kind = "fullscreen";
    const auto gen = Arcane::GenerateGraphSnippet(g);
    REQUIRE(gen.Ok());
    data.snippet = gen.snippet;
    data.graph = std::move(g);

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    {
        ShaderEditorDocument doc(DocServices{}, fs::path("thumbs.arcmat"), std::move(data));
        std::string logged;
        ImGuiID toggle = 0;
        const auto frame = [&](bool log)
        {
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            if (toggle) ImGui::ActivateItemByID(toggle);
            if (log) ImGui::LogToBuffer();
            bool close = false;
            doc.Draw(close);
            if (log) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
            ImGui::Render();
        };
        frame(false); frame(true);
        INFO(logged);
        CHECK(logged.find("Output preview") != std::string::npos);
        CHECK(logged.find("Thumbs") == std::string::npos);
        CHECK(logged.find("not compiled -- shader compiler unavailable (see the log)") != std::string::npos);
        ImGuiWindow* docWindow = nullptr;
        for (ImGuiWindow* win : ctx->Windows)
            if (std::string(win->Name).find("###matdoc_") != std::string::npos && !(win->Flags & ImGuiWindowFlags_ChildWindow)) docWindow = win;
        REQUIRE(docWindow != nullptr);
        const ImGuiID toggleId = docWindow->GetID("Output preview");
        toggle = toggleId;
        frame(false);
        toggle = 0;
        frame(false);
        CHECK(doc.ShowNodePreviews());      // a disabled checkbox refuses nav activation (imgui_widgets.cpp:693)

        // ...and that refusal is the DISABLED flag, not a missed id: sweep the
        // toolbar row until ImGui reports the toggle hovered. A disabled item
        // still claims HoveredId but raises HoveredIdIsDisabled (ItemHoverable).
        const float rowY = docWindow->ContentRegionRect.Min.y + ImGui::GetFrameHeight() * 0.5f;
        bool hovered = false;
        for (float x = docWindow->ContentRegionRect.Min.x; x < docWindow->ContentRegionRect.Max.x && !hovered; x += 4.0f)
        {
            io.AddMousePosEvent(x, rowY);
            frame(false);
            hovered = ctx->HoveredId == toggleId;
        }
        REQUIRE(hovered);
        CHECK(ctx->HoveredIdIsDisabled);
        // Held still past the tooltip delay, the box says why it is unavailable
        // (NoPreviewReason: the device-less preview outranks the compile).
        for (int i = 0; i < 30; ++i)
            frame(false);
        frame(true);
        INFO(logged);
        CHECK(logged.find("Shows the material preview on the Output node. Unavailable: no GPU device") != std::string::npos);
    }
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("material page: the body opens on Preview; the box is a square of min(width, 0.45 x page height)", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("page_square");
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate(); data.name = "Square"; data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(dir / "square.arcmat", data));
    ShaderEditorDocument doc(DocServices{}, dir / "square.arcmat", *Arcane::LoadMaterialAsset(dir / "square.arcmat"));
    PageUi h;
    h.Frame(doc); h.Frame(doc, true);
    INFO(h.logged);
    CHECK(LogStartsWithHeader(h.logged, "Preview"));
    ImGuiWindow* box = nullptr;
    for (ImGuiWindow* win : h.ctx->Windows)
        if (std::string(win->Name).find("##preview") != std::string::npos) box = win;
    REQUIRE(box != nullptr);
    const float side = (std::min)(400.0f - 16.0f, 0.45f * 600.0f);   // availX vs f x pageHeight
    CHECK(std::abs(box->Size.x - side) < 1.0f);
    CHECK(std::abs(box->Size.y - side) < 1.0f);
    CHECK(h.logged.find("Not compiled -- shader compiler unavailable (see the log)") != std::string::npos);
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const auto f = reg.Get(reg.Find("editor.inspector.materialPreviewFraction"));
    REQUIRE(f.has_value());
    CHECK(f->AsFloat32() == 0.45f);
}

TEST_CASE("material page T3-D5: an instance's page draws no Preview section -- the document tab owns it; it opens on Parameters",
          "[editor][material][inspector]")
{
    // User decision A (2026-10-02, spec s5.3 amendment): UE's Material
    // Instance editor shape. The instance's document tab is its full-tab
    // preview, so the page omits Section("Preview") rather than show a second one.
    const fs::path dir = TempDir("page_instance_nopreview");
    REQUIRE(Arcane::Project::Create(dir / "Game", "PageInstanceNoPreview").has_value());
    const fs::path content = dir / "Game" / "Content";
    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate(); base.name = "Base"; base.kind = "sprite"; base.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", base));
    Arcane::MaterialAssetData child;
    child.id = Arcane::Guid::Generate(); child.parent = base.id; child.name = "Child"; child.kind = "sprite";
    const fs::path file = content / "child.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(file, child));
    // A mesh instance too: its page carries authored rows with no compiler
    // (the base's baseColor), so "the parameter rows still draw" is observable.
    Arcane::MaterialAssetData meshBase;
    meshBase.id = Arcane::Guid::Generate(); meshBase.name = "MeshBase"; meshBase.kind = "mesh";
    meshBase.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(0.2f, 0.4f, 0.6f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(content / "mesh_base.arcmat", meshBase));
    Arcane::MaterialAssetData meshChild;
    meshChild.id = Arcane::Guid::Generate(); meshChild.parent = meshBase.id; meshChild.name = "MeshChild"; meshChild.kind = "mesh";
    const fs::path meshFile = content / "mesh_child.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(meshFile, meshChild));
    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    DocServices services;
    services.runtime = &rt;
    ShaderEditorDocument doc(services, file, *Arcane::LoadMaterialAsset(file));
    REQUIRE(doc.IsInstance());
    REQUIRE(doc.ParseErrors().empty());
    PageUi h;
    h.Frame(doc); h.Frame(doc, true);
    INFO(h.logged);
    CHECK(h.logged.find("Preview") == std::string::npos);            // no section header, no box text
    CHECK(LogStartsWithHeader(h.logged, "Parameters"));              // the body opens on the params
    CHECK(h.logged.find("Only overridden") != std::string::npos);
    for (ImGuiWindow* win : h.ctx->Windows)
        CHECK(std::string(win->Name).find("##preview") == std::string::npos);

    ShaderEditorDocument meshDoc(services, meshFile, *Arcane::LoadMaterialAsset(meshFile));
    REQUIRE(meshDoc.IsInstance());
    PageUi m;
    m.Frame(meshDoc); m.Frame(meshDoc, true);
    INFO(m.logged);
    CHECK(m.logged.find("Preview") == std::string::npos);
    CHECK(m.logged.find("not compiled here") == std::string::npos);  // the base's dim Preview line is gone too
    CHECK(LogStartsWithHeader(m.logged, "Rendering"));               // a mesh page opens on Rendering
    CHECK(m.logged.find("Parameters") != std::string::npos);
    CHECK(m.probe.count("baseColor") == 1);                           // the inherited row still draws
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("material toolbar: the surface combo carries a Surface label; a re-kind pushes no undo step", "[editor][material]")
{
    const fs::path dir = TempDir("surface_label");
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate(); data.name = "Rekind"; data.snippet = kSnippet;
    REQUIRE(Arcane::SaveMaterialAsset(dir / "rekind.arcmat", data));
    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.undo = [&stack] { return &stack; };
    ShaderEditorDocument doc(services, dir / "rekind.arcmat", *Arcane::LoadMaterialAsset(dir / "rekind.arcmat"));
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* px = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    std::string logged;
    ImGuiID activate = 0;
    const auto frame = [&](bool log = false)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        if (activate) ImGui::ActivateItemByID(activate);
        if (log) ImGui::LogToBuffer();
        bool close = false;
        doc.Draw(close);
        if (log) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::Render();
    };
    frame(); frame(true);
    CHECK(logged.find("Surface") != std::string::npos);
    ImGuiWindow* dw = nullptr;
    for (ImGuiWindow* win : ctx->Windows)
        if (std::string(win->Name).find("###matdoc_") != std::string::npos && !(win->Flags & ImGuiWindowFlags_ChildWindow)) dw = win;
    REQUIRE(dw != nullptr);
    activate = dw->GetID("##surface"); frame(); activate = 0; frame();      // the combo opens
    REQUIRE(ctx->OpenPopupStack.Size == 1);
    ImGuiWindow* popup = ctx->OpenPopupStack.back().Window;
    REQUIRE(popup != nullptr);
    activate = ImGui::GetIDWithSeed("Sprite", nullptr, ImGui::GetIDWithSeed(1, popup->ID));   // Combo pushes ID(i)
    frame(); activate = 0; frame();
    CHECK(doc.Dirty());                // re-kinded...
    CHECK_FALSE(stack.CanUndo());      // ...without a step (cpp :2208-2219; the combo did not move)
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("material page: params are PropertyGrid rows; Esc on a slider drag restores and pushes nothing",
          "[editor][material][inspector][shadercompile]")
{
    const fs::path dir = TempDir("page_rows");
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate(); data.name = "Rows";
    data.snippet = "//@param float Speed = 2.0 [0..4]\n"
                   "//@param float2 Scale = (2, 3)\n"
                   "//@param float4 Rect = (0, 1, 2, 3)\n"
                   "//@param color Tint = (1, 0, 0, 1)\n"
                   "//@param texture Noise\n"
                   "float4 shade(Varyings v) { return Tint * Speed; }\n";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "rows.arcmat", data));
    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(/*debounceSeconds=*/0.0));
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");
    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.compiler = &compiler; services.sources = &sources;
    services.undo = [&stack] { return &stack; };
    ShaderEditorDocument doc(services, dir / "rows.arcmat", *Arcane::LoadMaterialAsset(dir / "rows.arcmat"));
    DrainInto(compiler, doc, 2);
    PageUi h;
    h.Frame(doc); h.Frame(doc);
    for (const char* p : { "Speed", "Scale", "Rect", "Tint", "Noise" }) { INFO(p); CHECK(h.probe.count(p) == 1); }
    const std::uint32_t speed = Arcane::HashParamName("Speed");
    const auto overridden = [&] { return doc.GraphPreviewDesc().instance->HasOverride(speed); };
    REQUIRE(doc.GraphPreviewDesc().instance != nullptr);
    REQUIRE_FALSE(overridden());
    const ImVec2 at = h.At("Speed");
    h.Move(doc, at); h.Button(doc, true);
    h.Move(doc, ImVec2(at.x + 60.0f, at.y));
    CHECK(overridden());                                  // live write-through
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); h.Frame(doc);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); h.Button(doc, false);
    CHECK_FALSE(overridden());                            // Esc restored the activation state, override flag included (R2)
    CHECK_FALSE(stack.CanUndo());
    compiler.Shutdown();
}

namespace
{
    // A mesh base (baseColor + albedo) on a real stack: binds with no compiler (Rebuild's mesh branch).
    struct MeshMaterialFixture
    {
        fs::path dir, file;
        Astra::Registry registry;
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return registry; } };
        std::optional<ShaderEditorDocument> doc;
        explicit MeshMaterialFixture(const char* leaf) : dir(TempDir(leaf)), file(dir / "mesh.arcmat")
        {
            Arcane::MaterialAssetData d;
            d.id = Arcane::Guid::Generate(); d.name = "Mesh"; d.kind = "mesh";
            REQUIRE(Arcane::SaveMaterialAsset(file, d));
            DocServices s;
            s.undo = [this] { return &stack; };
            doc.emplace(s, file, *Arcane::LoadMaterialAsset(file));
        }
        std::string UndoLabel() const { return stack.CanUndo() ? std::string(stack.UndoLabel()) : std::string(); }
    };
}

TEST_CASE("material page: a base's reset decoration pushes Reset <name>", "[editor][material][inspector]")
{
    MeshMaterialFixture fx("page_reset");
    fx.doc->ApplyParamEdit(Arcane::HashParamName("baseColor"), true, Arcane::MatParamValue::MakeColor(0.5f, 0.5f, 0.5f, 1.0f));
    PageUi h;
    h.Frame(*fx.doc); h.Frame(*fx.doc);
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
    const ImVec2 row = h.At("baseColor");
    h.Click(*fx.doc, ImVec2(iw->ContentRegionRect.Max.x - 8.0f, row.y));   // the reset slot ends the value cell
    CHECK(fx.UndoLabel() == "Reset baseColor");
}

TEST_CASE("material page: a texture param's Set and Clear are one step each", "[editor][material][inspector]")
{
    MeshMaterialFixture fx("page_texture");
    const std::uint32_t albedo = Arcane::HashParamName("albedo");
    const Arcane::Guid tex = Arcane::Guid::Generate();
    Arcane::Editor::AssetRefEdit set;
    set.op = Arcane::Editor::AssetRefEdit::Op::Set; set.guid = tex;
    REQUIRE(fx.doc->ApplyParamRefEdit(albedo, set));
    CHECK(fx.UndoLabel() == "Edit albedo");
    Arcane::Editor::AssetRefEdit clear;
    clear.op = Arcane::Editor::AssetRefEdit::Op::Clear;
    REQUIRE(fx.doc->ApplyParamRefEdit(albedo, clear));
    CHECK_FALSE(fx.doc->ApplyParamRefEdit(Arcane::HashParamName("baseColor"), set));   // not a texture param
    fx.stack.Undo();                                                                 // back to the Set
    REQUIRE(fx.doc->Save());
    const auto saved = Arcane::LoadMaterialAsset(fx.file);
    REQUIRE(saved.has_value());
    bool found = false;
    for (const auto& [name, value] : saved->params)
        if (name == "albedo") { found = true; CHECK(value.tex == tex); }
    CHECK(found);
    fx.stack.Undo();
    CHECK_FALSE(fx.stack.CanUndo());                                                 // exactly two steps
}

TEST_CASE("material page: the Alpha cutoff drag applies live and lands as one step", "[editor][material][inspector][mesh]")
{
    MeshMaterialFixture fx("page_cutoff");
    PageUi h;
    h.Frame(*fx.doc); h.Frame(*fx.doc);
    const ImVec2 at = h.At("Alpha cutoff");
    h.Move(*fx.doc, at); h.Button(*fx.doc, true);
    h.Move(*fx.doc, ImVec2(at.x + 50.0f, at.y));
    const auto live = fx.doc->CaptureMeshMaterialMetadata();
    REQUIRE(live.has_value());
    CHECK(live->alphaCutoff.has_value());                // applied mid-drag
    CHECK_FALSE(fx.stack.CanUndo());                     // ...with no step yet
    h.Button(*fx.doc, false);
    h.Frame(*fx.doc);
    CHECK(fx.UndoLabel() == "Edit Alpha Cutoff");
    fx.stack.Undo();
    CHECK_FALSE(fx.stack.CanUndo());
    CHECK_FALSE(fx.doc->CaptureMeshMaterialMetadata()->alphaCutoff.has_value());
}



TEST_CASE("material page: Esc on the Alpha cutoff drag restores nullopt and pushes nothing", "[editor][material][inspector][mesh]")
{
    MeshMaterialFixture fx("page_cutoff_esc");
    PageUi h;
    h.Frame(*fx.doc); h.Frame(*fx.doc);
    const ImVec2 at = h.At("Alpha cutoff");
    h.Move(*fx.doc, at); h.Button(*fx.doc, true);
    h.Move(*fx.doc, ImVec2(at.x + 50.0f, at.y));
    REQUIRE(fx.doc->CaptureMeshMaterialMetadata()->alphaCutoff.has_value());       // live mid-drag
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); h.Frame(*fx.doc);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); h.Button(*fx.doc, false);
    h.Frame(*fx.doc);
    CHECK_FALSE(fx.doc->CaptureMeshMaterialMetadata()->alphaCutoff.has_value());   // R2: the activation state, nullopt included
    CHECK_FALSE(fx.stack.CanUndo());
}

TEST_CASE("material page: an instance's override cell materialises and clears, one step each, undoable", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("page_instance");
    REQUIRE(Arcane::Project::Create(dir / "Game", "PageInstance").has_value());
    const fs::path content = dir / "Game" / "Content";
    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate(); base.name = "Base"; base.kind = "mesh";
    base.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(0.2f, 0.4f, 0.6f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", base));
    Arcane::MaterialAssetData child;
    child.id = Arcane::Guid::Generate(); child.parent = base.id; child.name = "Child"; child.kind = "mesh";
    const fs::path file = content / "child.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(file, child));
    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.runtime = &rt;
    services.undo = [&stack] { return &stack; };
    ShaderEditorDocument doc(services, file, *Arcane::LoadMaterialAsset(file));
    REQUIRE(doc.IsInstance());
    REQUIRE(doc.ParseErrors().empty());
    PageUi h;
    h.Frame(doc); h.Frame(doc, true);
    CHECK(h.logged.find("Only overridden") != std::string::npos);   // on the Parameters band (T3-C4)
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
    const ImVec2 row = h.At("baseColor");
    const ImVec2 cell(iw->ContentRegionRect.Min.x + ImGui::GetStyle().CellPadding.x + ImGui::GetFrameHeight() * 0.5f, row.y);
    h.Click(doc, cell);                                              // tick: materialise the inherited value
    REQUIRE(stack.CanUndo());
    CHECK(std::string(stack.UndoLabel()) == "Edit baseColor");
    h.Click(doc, cell);                                              // untick: inherit again
    CHECK(std::string(stack.UndoLabel()) == "Reset baseColor");
    stack.Undo();
    CHECK(std::string(stack.UndoLabel()) == "Edit baseColor");
    REQUIRE(doc.Save());
    const auto saved = Arcane::LoadMaterialAsset(file);
    REQUIRE(saved.has_value());
    REQUIRE(saved->params.size() == 1);                              // the override is back
    CHECK(saved->params[0].first == "baseColor");
    std::error_code ec;
    fs::remove_all(dir, ec);
}



TEST_CASE("material page: an instance's texture row has its own override cell; a neighbour's toggle never leaks onto it", "[editor][material][inspector]")
{
    const fs::path dir = TempDir("page_instance_tex");
    REQUIRE(Arcane::Project::Create(dir / "Game", "PageInstanceTex").has_value());
    const fs::path content = dir / "Game" / "Content";
    Arcane::MaterialAssetData base;
    base.id = Arcane::Guid::Generate(); base.name = "Base"; base.kind = "mesh";
    REQUIRE(Arcane::SaveMaterialAsset(content / "base.arcmat", base));
    Arcane::MaterialAssetData child;
    child.id = Arcane::Guid::Generate(); child.parent = base.id; child.name = "Child"; child.kind = "mesh";
    const fs::path file = content / "child.arcmat";
    REQUIRE(Arcane::SaveMaterialAsset(file, child));
    Arcane::Runtime rt(Arcane::Test::Process());
    REQUIRE(rt.OpenProject(dir / "Game"));
    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.runtime = &rt;
    services.undo = [&stack] { return &stack; };
    ShaderEditorDocument doc(services, file, *Arcane::LoadMaterialAsset(file));
    REQUIRE(doc.IsInstance());
    Arcane::Editor::AssetRefEdit set;
    set.op = Arcane::Editor::AssetRefEdit::Op::Set; set.guid = Arcane::Guid::Generate();
    REQUIRE(doc.ApplyParamRefEdit(Arcane::HashParamName("albedo"), set));   // albedo overridden on the instance
    PageUi h;
    h.Frame(doc); h.Frame(doc);
    REQUIRE(h.probe.count("albedo#override") == 1);                        // the texture row carries the cell
    h.Click(doc, h.At("baseColor#override"));                              // tick the row ABOVE albedo (MeshParamTemplate order)
    REQUIRE(stack.CanUndo());
    CHECK(std::string(stack.UndoLabel()) == "Edit baseColor");             // no spurious "Edit albedo" rode along
    stack.Undo();
    CHECK(std::string(stack.UndoLabel()) == "Edit albedo");                // only the Set is left
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("material page: a base texture row's reset slot never covers the asset cell's clear button", "[editor][material][inspector]")
{
    // Carry ruling (T3-C6): the asset cell honours BeginValueCell's reserved
    // reset strip, so the trailing picker/clear buttons end before it. The
    // reset is submitted first and would otherwise take the clear's hover.
    MeshMaterialFixture fx("page_texture_reset");
    const std::uint32_t albedo = Arcane::HashParamName("albedo");
    Arcane::Editor::AssetRefEdit set;
    set.op = Arcane::Editor::AssetRefEdit::Op::Set; set.guid = Arcane::Guid::Generate();
    REQUIRE(fx.doc->ApplyParamRefEdit(albedo, set));   // an override with a valid guid: clear + reset both drawn
    PageUi h;
    h.Frame(*fx.doc); h.Frame(*fx.doc);
    const ImVec2 resetC = h.At("albedo#reset");
    // The row's probe is the cell's LAST item -- the clear button (no popup open,
    // no drag active), a SmallButton: FramePadding.y = 0.
    const ImVec2 clearC = h.At("albedo");
    const ImGuiStyle& st = ImGui::GetStyle();
    const float frameH = ImGui::GetFrameHeight();
    const ImVec2 clearHalf(ImGui::CalcTextSize(ICON_LC_X).x * 0.5f + st.FramePadding.x, ImGui::GetFontSize() * 0.5f);
    const ImRect resetR(ImVec2(resetC.x - frameH * 0.5f, resetC.y - frameH * 0.5f),
                        ImVec2(resetC.x + frameH * 0.5f, resetC.y + frameH * 0.5f));
    const ImRect clearR(ImVec2(clearC.x - clearHalf.x, clearC.y - clearHalf.y),
                        ImVec2(clearC.x + clearHalf.x, clearC.y + clearHalf.y));
    INFO("reset [" << resetR.Min.x << ", " << resetR.Max.x << "] clear [" << clearR.Min.x << ", " << clearR.Max.x << "]");
    CHECK_FALSE(resetR.Overlaps(clearR));

    h.Click(*fx.doc, clearC);                          // Op::Clear -> the nil guid, "Edit albedo"
    CHECK(fx.UndoLabel() == "Edit albedo");            // not "Reset albedo": the reset never took the click
    REQUIRE(fx.doc->Save());
    const auto cleared = Arcane::LoadMaterialAsset(fx.file);
    REQUIRE(cleared.has_value());
    bool found = false;
    for (const auto& [name, value] : cleared->params)
        if (name == "albedo") { found = true; CHECK_FALSE(value.tex.IsValid()); }   // still overridden, now nil
    CHECK(found);

    h.Frame(*fx.doc);
    h.Click(*fx.doc, h.At("albedo#reset"));
    CHECK(fx.UndoLabel() == "Reset albedo");
}

// Node page s5.1.1: node keys resolve purely against the data -- the pass is
// range-checked BEFORE the graph (GraphOptAt would silently fall back to the base).
TEST_CASE("ShaderEditorDocument node keys: Resolves is pure and refuses an out-of-range pass",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    CHECK(doc.Resolves("material"));
    CHECK(doc.Resolves("node:0:3"));
    CHECK_FALSE(doc.Resolves("node:0:99"));          // no such id
    CHECK_FALSE(doc.Resolves("node:1:3"));           // pass 1 does not exist: NOT the base fallback
    CHECK_FALSE(doc.Resolves("node:00:3"));          // non-canonical
    CHECK_FALSE(doc.Resolves("node:7"));             // malformed (the :121 case)
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);                // Resolves never selects

    // A graphless (text-owned) base resolves only "material".
    Arcane::MaterialAssetData text;
    text.id = Arcane::Guid::Generate();
    text.name = "Text";
    text.snippet = kSnippet;
    ShaderEditorDocument textDoc(DocServices{}, "text.arcmat", text);
    CHECK(textDoc.Resolves("material"));
    CHECK_FALSE(textDoc.Resolves("node:0:1"));
}

TEST_CASE("ShaderEditorDocument RestoreSelection: the key is live at once, no epoch, no pass entry on a sprite base",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    CHECK_FALSE(doc.ChainViewShowing());             // sprite surface: there is no overview
    REQUIRE(doc.RestoreSelection("node:0:3"));
    CHECK(doc.SelectionKey() == "node:0:3");         // TryLand re-reads it straight after
    CHECK(doc.SelectionEpoch() == 1);                // a restore is not a selection event
    CHECK(doc.NavHistoryDepth() == 1);               // no EnterPass on the active pass
    CHECK_FALSE(doc.RestoreSelection("node:0:99"));
    CHECK(doc.SelectionKey() == "node:0:3");         // a failed restore changes nothing
    REQUIRE(doc.RestoreSelection("material"));
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("ShaderEditorDocument RestoreSelection: a node key leaves the chain overview through EnterPass",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "chain.arcmat", Arcane::Test::ChainNodeDoc());
    CHECK(doc.ChainViewShowing());                   // seeded true, fullscreen surface
    REQUIRE(doc.RestoreSelection("node:1:2"));
    CHECK_FALSE(doc.ChainViewShowing());
    CHECK(doc.NavHistoryDepth() == 2);               // EnterPass recorded the navigation
    CHECK(doc.SelectionKey() == "node:1:2");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("ShaderEditorDocument SelectByPath: a scripted select bumps the epoch exactly once",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    REQUIRE(doc.SelectByPath("3"));                  // <id> = the active pass
    CHECK(doc.SelectionKey() == "node:0:3");
    CHECK(doc.SelectionEpoch() == 2);
    REQUIRE(doc.SelectByPath("0/2"));
    CHECK(doc.SelectionKey() == "node:0:2");
    CHECK(doc.SelectionEpoch() == 3);
    CHECK_FALSE(doc.SelectByPath("9"));              // no such node
    CHECK_FALSE(doc.SelectByPath("1/3"));            // no such pass
    CHECK_FALSE(doc.SelectByPath("Player/Jump"));
    CHECK(doc.SelectionEpoch() == 3);
}

TEST_CASE("ShaderEditorDocument: undoing the selected node's creation falls back to material with no event; redo restores it",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:4"));
    std::optional<Arcane::MaterialGraph> with = Arcane::Test::SpriteNodeDoc().graph;
    std::optional<Arcane::MaterialGraph> without = with;
    std::erase_if(without->nodes, [](const Arcane::GraphNode& n) { return n.id == 4; });
    doc.ApplyGraphState(0, without);                 // GraphEditCommand::Undo's path
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
    doc.ApplyGraphState(0, with);                    // Redo restores the same id
    CHECK(doc.SelectionKey() == "node:0:4");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("ShaderEditorDocument PageFor: material page, node page, and null for a missing id or pass",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    Arcane::Editor::InspectorPage* material = doc.PageFor("material");
    REQUIRE(material != nullptr);
    CHECK(doc.Page() == material);                   // nothing selected: the material page
    Arcane::Editor::InspectorPage* node = doc.PageFor("node:0:3");
    REQUIRE(node != nullptr);
    CHECK(node != material);
    CHECK(doc.PageFor("node:0:99") == nullptr);      // missing id
    CHECK(doc.PageFor("node:1:3") == nullptr);       // out-of-range pass, NOT the base fallback
    CHECK(doc.PageFor("bogus") == nullptr);
    REQUIRE(doc.RestoreSelection("node:0:3"));
    CHECK(doc.Page() == node);                       // Page() = PageFor(SelectionKey())
    CHECK(doc.Kind() == "material");                 // the kind never changes (filters untouched)
}

TEST_CASE("ShaderEditorDocument node page crumbs: [material, node], or [material, pass, node] in a chain",
          "[editor][material][inspector][nodepage]")
{
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:3"));
    std::vector<Arcane::Editor::InspectorCrumb> crumbs = doc.Page()->Breadcrumb();
    REQUIRE(crumbs.size() == 2);
    CHECK(crumbs[0].label == "Nodes");
    CHECK(crumbs[0].key == std::optional<std::string>{ "material" });
    CHECK(crumbs[1].label == "Multiply");
    CHECK(crumbs[1].key == std::optional<std::string>{ "node:0:3" });
    CHECK(Arcane::Editor::InspectorCrumbText(doc, doc.Page()) == "Nodes > Multiply");
    crumbs[0].select();                              // a crumb click is a selection
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 2);

    ShaderEditorDocument chain(DocServices{}, "chain.arcmat", Arcane::Test::ChainNodeDoc());
    REQUIRE(chain.RestoreSelection("node:1:2"));
    crumbs = chain.Page()->Breadcrumb();
    REQUIRE(crumbs.size() == 3);
    CHECK(crumbs[0].label == "Chain");
    CHECK(crumbs[1].label == "blur");
    CHECK_FALSE(crumbs[1].key.has_value());          // inert while pinned
    CHECK(crumbs[2].label == "Custom (HLSL)");
    CHECK(crumbs[2].key == std::optional<std::string>{ "node:1:2" });
    crumbs[1].select();                              // EnterPass(1), then the material crumb's select
    CHECK(chain.SelectionKey() == "material");
    CHECK(chain.SelectionEpoch() == 2);
}

TEST_CASE("ShaderEditorDocument node page: a node deleted under a held page draws one read-only line",
          "[editor][material][inspector][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    Arcane::Editor::InspectorPage* page = doc.PageFor("node:0:4");
    REQUIRE(page != nullptr);
    std::optional<Arcane::MaterialGraph> without = Arcane::Test::SpriteNodeDoc().graph;
    std::erase_if(without->nodes, [](const Arcane::GraphNode& n) { return n.id == 4; });
    doc.ApplyGraphState(0, without);                 // the transient frame after an undo
    Arcane::Editor::PropertyGridState grid;
    std::string logged;
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(400.0f, 600.0f));
    ImGui::Begin("Inspector");
    ImGui::LogToBuffer();
    Arcane::Editor::PropertyGrid pg(grid);
    page->Draw(pg);
    logged = imgui.ctx->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
    INFO(logged);
    CHECK(logged.find("This node no longer exists") != std::string::npos);
}

TEST_CASE("ShaderEditorDocument node page: Edit HLSL opens from the page with no canvas, and Apply writes the PINNED pass",
          "[editor][material][inspector][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.undo = [&stack]() -> Arcane::CommandStack* { return &stack; };   // T1's resolver
    ShaderEditorDocument doc(services, "chain.arcmat", Arcane::Test::ChainNodeDoc());
    // Active pass 0 (the base); the page targets pass 1's Custom node, as a
    // pinned page would. The document window is never drawn (canvas hidden).
    Arcane::Editor::InspectorPage* page = doc.PageFor("node:1:2");
    REQUIRE(page != nullptr);
    doc.RequestBodyEdit(1, 2);

    Arcane::Editor::PropertyGridState grid;
    auto frame = [&](const char* activate)
    {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        if (activate)
            if (ImGuiWindow* modal = ImGui::FindWindowByName("Edit HLSL##graphbody"))
                ImGui::ActivateItemByID(modal->GetID(activate));   // lands on the NEXT frame
        ImGui::SetNextWindowSize(ImVec2(400.0f, 600.0f));
        ImGui::Begin("Inspector");
        Arcane::Editor::PropertyGrid pg(grid);
        doc.PageFor("node:1:2")->Draw(pg);
        ImGui::End();
        ImGui::Render();
    };
    frame(nullptr);                                  // the page consumes the request and opens the modal
    bool open = false;
    for (const ImGuiPopupData& p : imgui.ctx->OpenPopupStack)
        open = open || (p.Window && std::string(p.Window->Name).find("Edit HLSL") != std::string::npos);
    REQUIRE(open);

    frame("##bodyedit");                             // queue focus on the body
    frame(nullptr);                                  // the text box is active
    ImGui::GetIO().AddInputCharactersUTF8("x");
    frame(nullptr);                                  // the typed text lands in the buffer
    frame("Apply");
    frame(nullptr);                                  // Apply lands

    const ShaderEditorDocument::PassListState after = doc.CapturePassListState();
    REQUIRE(after.passes.size() == 1);
    REQUIRE(after.passes[0].graph.has_value());
    CHECK(after.passes[0].graph->FindNode(2)->customBody != "return p1;");   // pass 1 written
    REQUIRE(stack.CanUndo());
    CHECK(std::string(stack.UndoLabel()) == "Edit HLSL Body");
    stack.Undo();
    CHECK(doc.CapturePassListState().passes[0].graph->FindNode(2)->customBody == "return p1;");
}

// ---- Node page plumbing (spec 2026-09-30 s5.1.4): the pin add/remove both
// the canvas and the node page call. One step each; links AND literals
// re-index (they address pins by bare index). ----
TEST_CASE("ShaderEditorDocument: RemoveCustomPin drops the pin's link and literal and re-indexes later pins; AddCustomPin takes the next free p<k>",
          "[editor][material][nodepage]")
{
    Arcane::MaterialGraph g;
    Arcane::GraphNode out;  out.id = 1;  out.type = Arcane::GraphNodeType::Output;
    Arcane::GraphNode c;    c.id = 2;    c.type = Arcane::GraphNodeType::Custom;
    c.customPins = { { "p1", 1 }, { "p2", 1 }, { "p3", 1 } };
    c.customBody = "return float4(p1, p2, p3, 1.0);";
    Arcane::GraphPinLiteral l1; l1.pin = 1; l1.v[0] = 0.5f;
    Arcane::GraphPinLiteral l2; l2.pin = 2; l2.v[0] = 0.7f;
    c.pinLiterals = { l1, l2 };
    Arcane::GraphNode f3;   f3.id = 3;   f3.type = Arcane::GraphNodeType::ConstFloat;
    Arcane::GraphNode f4;   f4.id = 4;   f4.type = Arcane::GraphNodeType::ConstFloat;
    g.nodes = { out, c, f3, f4 };
    g.links = { { 3, 0, 2, 0 }, { 4, 0, 2, 2 }, { 2, 0, 1, 0 } };
    g.nextId = 5;
    Arcane::MaterialAssetData data;
    data.id = Arcane::Guid::Generate();
    data.name = "Pins";
    data.kind = "sprite";
    data.graph = g;

    Astra::Registry registry;
    Arcane::CommandStack stack{ [&registry]() -> Astra::Registry& { return registry; } };
    DocServices services;
    services.undo = [&stack]() -> Arcane::CommandStack* { return &stack; };
    ShaderEditorDocument doc(services, fs::path("pins.arcmat"), data);
    const std::string before = Arcane::GraphToJson(*doc.PassGraph(0)).dump();

    REQUIRE(doc.RemoveCustomPin(0, 2, 0));
    const Arcane::GraphNode* n = doc.PassGraph(0)->FindNode(2);
    REQUIRE(n->customPins.size() == 2);
    CHECK(n->customPins[0].name == "p2");
    CHECK(n->FindPinLiteral(0) != nullptr);  CHECK(n->FindPinLiteral(0)->v[0] == 0.5f);   // was pin 1
    CHECK(n->FindPinLiteral(1) != nullptr);  CHECK(n->FindPinLiteral(1)->v[0] == 0.7f);   // was pin 2
    CHECK(n->FindPinLiteral(2) == nullptr);
    bool fromF3 = false, f4ToPin1 = false;
    for (const Arcane::GraphLink& l : doc.PassGraph(0)->links)
    {
        fromF3 = fromF3 || l.fromNode == 3;
        f4ToPin1 = f4ToPin1 || (l.fromNode == 4 && l.toNode == 2 && l.toPin == 1);
    }
    CHECK_FALSE(fromF3);                     // the removed pin's wire is gone
    CHECK(f4ToPin1);                         // the later wire slid down one index
    REQUIRE(stack.CanUndo());
    CHECK(std::string(stack.UndoLabel()) == "Remove Pin");

    REQUIRE(doc.AddCustomPin(0, 2));
    CHECK(doc.PassGraph(0)->FindNode(2)->customPins.back().name == "p1");   // the first free p<k>
    CHECK(std::string(stack.UndoLabel()) == "Add Pin");

    stack.Undo();
    stack.Undo();
    CHECK_FALSE(stack.CanUndo());            // one step each
    CHECK(Arcane::GraphToJson(*doc.PassGraph(0)).dump() == before);
    CHECK_FALSE(doc.RemoveCustomPin(0, 2, 9));   // out of range: no edit, no step
    CHECK_FALSE(doc.RemoveCustomPin(4, 2, 0));   // no such pass: never the base fallback
    CHECK_FALSE(stack.CanUndo());
}

// ==== The NODE PAGE (spec 2026-09-30 s5.1.4/5.1.5/5.1.9/5.1.11) ====
namespace
{
    using T = Arcane::GraphNodeType;

    // The s5.1.10 fixture's SHAPE, built in code with fixed ids so no test depends on
    // the ReferenceProject file: 1 Output, 2 Sprite Texture, 3 Param 'tint' (color),
    // 4 Multiply (rgba -> a, tint -> b, -> Output.color), 5 Power (b literal 2, a
    // unwired), 6 Swizzle "xy", 7 Custom (p1 float4, "return p1;", out float4),
    // 8 Panner (Fractional), 9 Comment.
    Arcane::GraphNode& AddNode(Arcane::MaterialGraph& g, std::uint32_t id, T type)
    {
        Arcane::GraphNode n;
        n.id = id; n.type = type;
        n.posX = 40.0f + 200.0f * static_cast<float>((id - 1) % 3);
        n.posY = 40.0f + 150.0f * static_cast<float>((id - 1) / 3);
        g.nodes.push_back(std::move(n));
        return g.nodes.back();   // use immediately: the next AddNode may reallocate
    }
    Arcane::MaterialGraph NodePageGraph()
    {
        Arcane::MaterialGraph g;
        AddNode(g, 1, T::Output);
        AddNode(g, 2, T::SpriteTexture);
        { Arcane::GraphNode& p = AddNode(g, 3, T::Param); p.paramName = "tint"; p.paramType = Arcane::MatParamType::Color;
          p.paramDefault = Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f); }
        AddNode(g, 4, T::Mul);
        { Arcane::GraphNode& p = AddNode(g, 5, T::Power); Arcane::GraphPinLiteral l; l.pin = 1; l.v[0] = 2.0f; p.pinLiterals.push_back(l); }
        { Arcane::GraphNode& s = AddNode(g, 6, T::Swizzle); s.swizzleMask = "xy"; }
        { Arcane::GraphNode& c = AddNode(g, 7, T::Custom); c.customPins = { { "p1", 4 } }; c.customBody = "return p1;"; c.customOutWidth = 4; }
        { Arcane::GraphNode& p = AddNode(g, 8, T::Panner); p.pannerFractional = true; }
        { Arcane::GraphNode& c = AddNode(g, 9, T::Comment); c.paramName = "Node page fixture"; c.value[0] = 240.0f; c.value[1] = 120.0f; }
        g.links = { { 2, 0, 4, 0 }, { 3, 0, 4, 1 }, { 4, 0, 1, 0 } };
        g.nextId = 10;
        return g;
    }
    Arcane::MaterialAssetData GraphDoc(Arcane::MaterialGraph g, const char* kind = "sprite")
    {
        Arcane::MaterialAssetData data;
        data.id = Arcane::Guid::Generate();
        data.name = "NodePageGraph";
        data.kind = kind;
        data.graph = std::move(g);   // the ctor's RegenerateFromGraph writes the snippet
        return data;
    }
    // A fullscreen base graph + one graph-owned pass "blur", each holding the SAME
    // ids: 1 Output, 2 Float (-> Output.color), 3 Custom (pin x, float).
    Arcane::MaterialAssetData ChainDoc()
    {
        const auto graph = [](float v)
        {
            Arcane::MaterialGraph g;
            AddNode(g, 1, T::Output);
            { Arcane::GraphNode& f = AddNode(g, 2, T::ConstFloat); f.value[0] = v; }
            { Arcane::GraphNode& c = AddNode(g, 3, T::Custom); c.customPins = { { "x", 1 } }; c.customBody = "return float4(x, x, x, 1.0);"; }
            g.links = { { 2, 0, 1, 0 } };
            g.nextId = 4;
            return g;
        };
        Arcane::MaterialAssetData data = GraphDoc(graph(0.25f), "fullscreen");
        Arcane::MaterialPass blur;
        blur.name = "blur";
        blur.inputs = { 0 };
        blur.graph = graph(0.75f);
        data.passes.push_back(std::move(blur));
        return data;
    }
    std::string NodeKeyOf(std::size_t pass, std::uint32_t id) { return Arcane::Editor::FormatNodeKey({ pass, id }); }
    // A CollapsingHeader logs as "### <label> ###" (imgui_widgets.cpp:7116).
    bool HasSection(const std::string& log, const char* label) { return log.find(std::string("### ") + label) != std::string::npos; }

    // Device-less ImGui (HeadlessImGui, FIRST member so it destructs last) + a real
    // CommandStack + the document; each Frame draws an Inspector-like 392 px window
    // (the 1080p Inspector width) with PageFor(key) -- or a fixed page pointer --
    // logging its text and recording PropertyGrid probes.
    struct NodePageHarness
    {
        Arcane::Test::HeadlessImGui imgui;
        ImGuiContext* ctx = imgui.ctx;
        Astra::Registry registry;
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return registry; } };
        Arcane::Editor::PropertyGridState state;
        std::unordered_map<std::string, ImVec2> probe;
        std::unique_ptr<ShaderEditorDocument> doc;
        std::string key, log;
        bool pageDrawn = false, errorDrawn = false;
        float width = 392.0f;   // the 1080p Inspector width; a content-only case may widen it

        explicit NodePageHarness(Arcane::MaterialAssetData data)
        {
            ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 1024.0f);   // room for the 1000 px window
            state.probe = &probe;
            DocServices services;
            services.undo = [this]() -> Arcane::CommandStack* { return &stack; };
            doc = std::make_unique<ShaderEditorDocument>(services, fs::path("nodepage.arcmat"), std::move(data));
        }
        ~NodePageHarness() { doc.reset(); }   // inside the context (the document's dtor may touch ImGui)
        NodePageHarness(const NodePageHarness&) = delete;
        NodePageHarness& operator=(const NodePageHarness&) = delete;

        void Frame(Arcane::Editor::InspectorPage* fixed = nullptr)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            Arcane::Editor::PropertyGrid(state).CommitOrphans();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(width, 1000), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            ImGui::LogToBuffer();
            pageDrawn = false;
            Arcane::Editor::InspectorPage* page = fixed ? fixed : (doc ? doc->PageFor(key) : nullptr);
            if (page) { Arcane::Editor::PropertyGrid grid(state); page->Draw(grid); pageDrawn = true; }
            log = ctx->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::End();
            ImGui::Render();
            const ImU32 error = ImGui::ColorConvertFloat4ToU32(Arcane::Editor::Theme::kError);
            errorDrawn = false;
            for (const ImDrawVert& v : ImGui::FindWindowByName("Inspector")->DrawList->VtxBuffer) errorDrawn = errorDrawn || v.col == error;
        }
        ImVec2 Centre(const std::string& label) { INFO(label); REQUIRE(probe.count(label) == 1); return probe.at(label); }
        void Press(ImVec2 at) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(at.x, at.y); Frame(); io.AddMouseButtonEvent(0, true); Frame(); }
        void Click(ImVec2 at) { Press(at); ImGui::GetIO().AddMouseButtonEvent(0, false); Frame(); }
        // ForTooltip = Stationary + DelayShort (style.HoverFlagsForTooltipMouse): ~0.3 s still.
        void Hover(ImVec2 at, int frames = 40) { ImGui::GetIO().AddMousePosEvent(at.x, at.y); for (int i = 0; i < frames; ++i) Frame(); }
        static bool TooltipShown() { ImGuiWindow* w = ImGui::FindWindowByName("##Tooltip_00"); return w && w->Active; }
        void Type(const char* s) { ImGui::GetIO().AddInputCharactersUTF8(s); Frame(); }
        void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }
        const Arcane::GraphNode* Node(std::size_t pass, std::uint32_t id) const
        {
            const Arcane::MaterialGraph* g = doc->PassGraph(pass);
            return g ? g->FindNode(id) : nullptr;
        }
    };
    std::string GraphJson(const NodePageHarness& h, std::size_t pass) { return Arcane::GraphToJson(*h.doc->PassGraph(pass)).dump(); }
}

TEST_CASE("Node page: the header shows category, type and description; Outputs list widths and targets; no preview",
          "[editor][material][nodepage]")
{
    NodePageHarness h(GraphDoc(NodePageGraph()));
    h.key = NodeKeyOf(0, 4);   // Multiply
    h.Frame(); h.Frame();
    REQUIRE(h.pageDrawn);
    INFO(h.log);
    const Arcane::GraphNodeTypeInfo& mul = Arcane::GraphNodeInfo(T::Mul);
    CHECK(h.log.find("Math") != std::string::npos);            // the category chip
    CHECK(h.log.find("Multiply") != std::string::npos);        // the type display
    CHECK(h.log.find(mul.description) != std::string::npos);   // wrapped, dim
    CHECK(HasSection(h.log, "Outputs"));
    CHECK(h.log.find("| out | -> Output.color") != std::string::npos);   // T3-D2: at 392 px the type word is the dot's tooltip
    CHECK_FALSE(HasSection(h.log, "Errors"));
    for (ImGuiWindow* w : h.ctx->Windows)                       // s5.1.9: no per-node preview, no copy of the material's
        CHECK(std::string(w->Name).find("##preview") == std::string::npos);

    h.key = NodeKeyOf(0, 2); h.Frame();                         // Sprite Texture: one wired output, one not
    CHECK(h.log.find("| rgba | -> Multiply.a") != std::string::npos);
    CHECK(h.log.find("| a | (unused)") != std::string::npos);
    h.key = NodeKeyOf(0, 1); h.Frame();                         // Output has no Outputs section
    CHECK_FALSE(HasSection(h.log, "Outputs"));
}

TEST_CASE("Node page T3-D1: Inputs and Outputs rows carry the pin's type word -- dynamic pins say what they resolved to",
          "[editor][material][nodepage]")
{
    // A WIDE Inspector (640 px): the value cell fits the dot, the widest type
    // word and editor.inspector.nodePageMinTextRun characters after it, so the
    // chip shows its word (T3-D2; at 392 px the word folds into the tooltip --
    // the next case). The wiring / default text after a long "dynamic (...)"
    // may still be cut (whole text on hover, s4.1(e)): read up to the arrow.
    NodePageHarness h(GraphDoc(NodePageGraph()));
    h.width = 640.0f;
    const auto at = [&](std::uint32_t id) { h.key = NodeKeyOf(0, id); h.Frame(); h.Frame(); return h.log; };
    {
        const std::string log = at(4);                          // Multiply: a <- Sprite Texture.rgba (float4), b <- Param 'tint' (color)
        INFO(log);
        CHECK(log.find("| a | dynamic (now float4) <- ") != std::string::npos);
        CHECK(log.find("| b | dynamic (now float4) <- ") != std::string::npos);
        CHECK(log.find("| out | dynamic (now float4) -> ") != std::string::npos);
    }
    {
        const std::string log = at(2);                          // Sprite Texture: fixed pins keep their plain words
        INFO(log);
        CHECK(log.find("| uv | float2 default: v.uv") != std::string::npos);
        CHECK(log.find("| rgba | float4 -> Multiply.a") != std::string::npos);
        CHECK(log.find("| a | float (unused)") != std::string::npos);
    }
    {
        const std::string log = at(5);                          // Power: nothing wired -- unresolved on both sides
        INFO(log);
        CHECK(log.find("| a | dynamic (unresolved) ") != std::string::npos);
        CHECK(log.find("| out | dynamic (unresolved) ") != std::string::npos);
    }
    {
        const std::string log = at(6);                          // Swizzle "xy": its output is the mask's width
        INFO(log);
        CHECK(log.find("| out | dynamic (now float2) ") != std::string::npos);
    }
    {
        const std::string log = at(1);                          // Output.color is a FIXED float4
        INFO(log);
        CHECK(log.find("| color | float4 <- Multiply.out") != std::string::npos);
    }
}

TEST_CASE("Node page T3-D2: at the 392 px Inspector a pin row shows only its dot -- the type word leads the row's hover tooltip",
          "[editor][material][nodepage]")
{
    // The 1080p Inspector's value cell is narrower than the dot + the widest
    // type word + editor.inspector.nodePageMinTextRun characters, so EVERY row
    // folds its word (the rule reads the widest word, so a page's rows agree).
    NodePageHarness h(GraphDoc(NodePageGraph()));
    const auto at = [&](std::uint32_t id) { h.key = NodeKeyOf(0, id); h.Frame(); h.Frame(); return h.log; };
    // `head`, then only whitespace, then `tail`: the two-line tooltip as the log
    // records it (the tooltip's text is logged too; its lines split on '\n').
    const auto leads = [](const std::string& log, const std::string& head, const std::string& tail)
    {
        const std::size_t pos = log.find(head);
        if (pos == std::string::npos) return false;
        const std::size_t next = log.find_first_not_of(" \r\n", pos + head.size());
        return next != std::string::npos && log.compare(next, tail.size(), tail) == 0;
    };
    {
        const std::string log = at(4);                          // Multiply
        INFO(log);
        CHECK(log.find("| a | <- Sprite Texture.rgba") != std::string::npos);   // the whole wiring, no word ahead of it
        CHECK(log.find("| b | <- Param 'tint'.out") != std::string::npos);
        CHECK(log.find("| out | -> Output.color") != std::string::npos);
        CHECK(log.find("dynamic (") == std::string::npos);
    }
    h.Hover(h.Centre("a"));                                     // the wired row's text
    CHECK(NodePageHarness::TooltipShown());
    {
        INFO(h.log);
        CHECK(leads(h.log, "dynamic (now float4)", "<- Sprite Texture.rgba"));   // the type first, then the full row text
    }
    h.Hover(ImVec2(0.0f, 0.0f), 1);                             // off the rows: the next page reads tooltip-free
    {
        const std::string log = at(2);                          // Sprite Texture: fixed words fold too
        INFO(log);
        CHECK(log.find("| uv | default: v.uv") != std::string::npos);
        CHECK(log.find("| rgba | -> Multiply.a") != std::string::npos);
        CHECK(log.find("float4") == std::string::npos);
    }
    {
        const std::string log = at(5);                          // Power: live literal rows (a neutral, b = 2)
        INFO(log);
        CHECK(log.find("2.000") != std::string::npos);
        CHECK(log.find("dynamic (") == std::string::npos);
    }
    h.Hover(h.Centre("b"));                                     // the literal's value widget
    CHECK(NodePageHarness::TooltipShown());
    {
        INFO(h.log);
        CHECK(h.log.find("dynamic (unresolved)") != std::string::npos);
    }

    // The threshold is the cvar's run, not a pixel literal: with no run to keep
    // readable, the dot + widest word fit 392 px and the word comes back.
    struct RunRestore
    {
        ~RunRestore() { Arcane::CVarRegistry::Get().UnregisterModule("nodepage-test"); Arcane::CVarRegistry::Get().Publish(); }
    } runRestore;
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    REQUIRE(reg.Set(reg.Find("editor.inspector.nodePageMinTextRun"), Arcane::CVarValue::Int32(0),
                    Arcane::SetBy::Code, "nodepage-test") == Arcane::SetResult::Applied);
    reg.Publish();
    h.Hover(ImVec2(0.0f, 0.0f), 1);
    {
        const std::string log = at(4);
        INFO(log);
        CHECK(log.find("| a | dynamic (now float4) <- ") != std::string::npos);
    }
}

TEST_CASE("Node page: Errors (N) carries this node's codegen errors only, in kError", "[editor][material][nodepage]")
{
    Arcane::MaterialGraph g = NodePageGraph();
    g.FindNode(3)->paramName = "1bad";            // codegen refuses the name (MaterialGraph.cpp:503-507)
    NodePageHarness h(GraphDoc(std::move(g)));
    h.key = NodeKeyOf(0, 3); h.Frame(); h.Frame();
    INFO(h.log);
    CHECK(HasSection(h.log, "Errors (1)"));
    CHECK(h.log.find("not a valid identifier") != std::string::npos);
    CHECK(h.errorDrawn);
    h.key = NodeKeyOf(0, 9); h.Frame();           // the Comment: codegen never names it
    CHECK_FALSE(HasSection(h.log, "Errors"));
}

TEST_CASE("Node page: a page drawn after its node died says so and draws nothing else", "[editor][material][nodepage]")
{
    NodePageHarness h(GraphDoc(NodePageGraph()));
    h.key = NodeKeyOf(0, 6); h.Frame();
    Arcane::Editor::InspectorPage* page = h.doc->PageFor(h.key);   // resolved BEFORE the delete: the transient frame
    REQUIRE(page != nullptr);
    Arcane::MaterialGraph g = *h.doc->PassGraph(0);
    std::erase_if(g.nodes, [](const Arcane::GraphNode& n) { return n.id == 6; });
    h.doc->ApplyGraphState(0, g);
    h.Frame(page);
    CHECK(h.log.find("This node no longer exists") != std::string::npos);
    CHECK_FALSE(HasSection(h.log, "Outputs"));
}

TEST_CASE("Node page Inputs: wired, literal, expression-neutral and refusing pins draw the s5.1.4 pin rows",
          "[editor][material][nodepage]")
{
    Arcane::MaterialGraph g = NodePageGraph();
    AddNode(g, 10, T::Remap);          // ranges read their default directly
    AddNode(g, 11, T::VertexOutput);   // passthrough pins
    g.nextId = 12;
    NodePageHarness h(GraphDoc(std::move(g)));
    // CONTENT, not fit: wide enough that no row's text is cut. A type chip
    // leads each value cell (T3-D1): at 640 px it shows its word, at 392 px
    // only its dot (T3-D2) -- the two T3-D1/T3-D2 cases read those layouts.
    h.width = 640.0f;
    const auto at = [&](std::uint32_t id) { h.key = NodeKeyOf(0, id); h.Frame(); h.Frame(); return h.log; };
    std::string log = at(4);
    CHECK(log.find("<- Sprite Texture.rgba") != std::string::npos);
    CHECK(log.find("<- Param 'tint'.out") != std::string::npos);
    CHECK(at(1).find("<- Multiply.out") != std::string::npos);
    CHECK(at(6).find("default: 0") != std::string::npos);                                   // Swizzle source
    CHECK(at(10).find("default: (0, 1)") != std::string::npos);                             // Remap ranges
    // The full "default: unchanged (only a wire contributes)" outruns the 392 px
    // value cell, so ReadOnlyRow ellipsizes it (s4.1(e); the whole text is the
    // hover tooltip): the log carries its head.
    CHECK(at(11).find("default: unchanged") != std::string::npos);
    log = at(8);                                                                             // Panner: uv's neutral is v.uv
    CHECK(h.probe.count("uv") == 1);
    CHECK(h.probe.count("speed") == 1);
    CHECK(log.find("v.uv") != std::string::npos);
    log = at(5);                                                                             // Power: a = neutral 0, b = literal 2
    CHECK(h.probe.count("a") == 1);
    CHECK(h.probe.count("b") == 1);
    CHECK(log.find("2.000") != std::string::npos);
    CHECK(log.find("default:") == std::string::npos);
    CHECK(at(2).find("default: v.uv") != std::string::npos);                                // Sprite Texture uv refuses literals
    CHECK(at(9).find("### Inputs") == std::string::npos);                                    // Comment: no Inputs
}

TEST_CASE("Node page Inputs: a width-1 neutral splats across a 2-lane pin (Tiling & Offset tiling reads (1, 1))",
          "[editor][material][nodepage]")
{
    Arcane::MaterialGraph g = NodePageGraph();
    AddNode(g, 10, T::TilingOffset);
    g.nextId = 11;
    NodePageHarness h(GraphDoc(std::move(g)));
    h.key = NodeKeyOf(0, 10); h.Frame(); h.Frame();
    INFO(h.log);
    std::size_t ones = 0;   // uv reads "default: v.uv", offset (0.000, 0.000), tiling must be (1.000, 1.000)
    for (std::size_t at = h.log.find("1.000"); at != std::string::npos; at = h.log.find("1.000", at + 1))
        ++ones;
    CHECK(ones == 2);
    CHECK(h.Node(0, 10)->pinLiterals.empty());   // showing the neutral wrote nothing
}

TEST_CASE("Node page Inputs: a literal drag writes live and is ONE 'Pin Value' step; Reset is ONE 'Reset Pin Value' step",
          "[editor][material][nodepage]")
{
    NodePageHarness h(GraphDoc(NodePageGraph()));
    const std::string before = GraphJson(h, 0);
    h.key = NodeKeyOf(0, 5); h.Frame(); h.Frame();
    const ImVec2 c = h.Centre("a");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    io.AddMousePosEvent(c.x + 30.0f, c.y); h.Frame();
    REQUIRE(h.Node(0, 5)->FindPinLiteral(0) != nullptr);     // live while dragging...
    CHECK_FALSE(h.stack.CanUndo());                         // ...one step only at release
    io.AddMousePosEvent(c.x + 60.0f, c.y); h.Frame();
    io.AddMouseButtonEvent(0, false); h.Frame(); h.Frame();
    REQUIRE(h.stack.CanUndo());
    CHECK(std::string(h.stack.UndoLabel()) == "Pin Value");
    CHECK(h.Node(0, 5)->FindPinLiteral(0)->v[0] > 0.0f);
    h.stack.Undo();
    CHECK_FALSE(h.stack.CanUndo());
    CHECK(GraphJson(h, 0) == before);

    h.Frame(); h.Frame();
    h.Click(h.Centre("b#reset"));                            // T2's reset slot (s4.1(d))
    CHECK(h.Node(0, 5)->FindPinLiteral(1) == nullptr);       // codegen reads the neutral again
    REQUIRE(h.stack.CanUndo());
    CHECK(std::string(h.stack.UndoLabel()) == "Reset Pin Value");
    h.stack.Undo();
    REQUIRE(h.Node(0, 5)->FindPinLiteral(1) != nullptr);
    CHECK(h.Node(0, 5)->FindPinLiteral(1)->v[0] == 2.0f);
    CHECK_FALSE(h.stack.CanUndo());
}

TEST_CASE("Node page Inputs: Esc mid-drag restores the pin, pushes nothing and leaves no new literal",
          "[editor][material][nodepage]")
{
    NodePageHarness h(GraphDoc(NodePageGraph()));
    const std::string before = GraphJson(h, 0);
    h.key = NodeKeyOf(0, 5); h.Frame(); h.Frame();
    const ImVec2 c = h.Centre("a");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    io.AddMousePosEvent(c.x + 40.0f, c.y); h.Frame(); h.Frame();
    REQUIRE(h.Node(0, 5)->FindPinLiteral(0) != nullptr);
    io.AddKeyEvent(ImGuiKey_Escape, true); h.Frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    io.AddMouseButtonEvent(0, false); h.Frame(); h.Frame();
    CHECK(h.Node(0, 5)->FindPinLiteral(0) == nullptr);
    CHECK_FALSE(h.stack.CanUndo());
    CHECK(GraphJson(h, 0) == before);
}

TEST_CASE("Node page Inputs: Esc mid-drag on an EXISTING literal restores it in place, keeps it, and pushes nothing",
          "[editor][material][nodepage]")
{
    NodePageHarness h(GraphDoc(NodePageGraph()));
    const std::string before = GraphJson(h, 0);
    h.key = NodeKeyOf(0, 5); h.Frame(); h.Frame();
    const ImVec2 c = h.Centre("b");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    io.AddMousePosEvent(c.x + 40.0f, c.y); h.Frame(); h.Frame();
    REQUIRE(h.Node(0, 5)->FindPinLiteral(1) != nullptr);
    CHECK(h.Node(0, 5)->FindPinLiteral(1)->v[0] != 2.0f);      // live while dragging
    io.AddKeyEvent(ImGuiKey_Escape, true); h.Frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    io.AddMouseButtonEvent(0, false); h.Frame(); h.Frame();
    REQUIRE(h.Node(0, 5)->FindPinLiteral(1) != nullptr);       // the user's literal survives the cancel
    CHECK(h.Node(0, 5)->FindPinLiteral(1)->v[0] == 2.0f);
    CHECK_FALSE(h.stack.CanUndo());
    CHECK(GraphJson(h, 0) == before);
}

TEST_CASE("Node page: a page on a NON-active pass edits that pass", "[editor][material][nodepage]")
{
    NodePageHarness h(ChainDoc());                          // opens on the base (active pass 0)
    const std::string base = GraphJson(h, 0);
    h.key = NodeKeyOf(1, 3); h.Frame(); h.Frame();          // pass 1's Custom, pin x
    const ImVec2 c = h.Centre("x");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    io.AddMousePosEvent(c.x + 40.0f, c.y); h.Frame();
    io.AddMouseButtonEvent(0, false); h.Frame(); h.Frame();
    REQUIRE(h.Node(1, 3)->FindPinLiteral(0) != nullptr);
    CHECK(GraphJson(h, 0) == base);                         // the base never moved
    REQUIRE(h.stack.CanUndo());
    CHECK(std::string(h.stack.UndoLabel()) == "Pin Value");
    h.stack.Undo();
    CHECK(h.Node(1, 3)->FindPinLiteral(0) == nullptr);      // undo targets pass 1 too
}

TEST_CASE("Node page Settings: the row set and sections per node type follow the s5.1.4 tables", "[editor][material][nodepage]")
{
    Arcane::MaterialGraph g;
    AddNode(g, 1, T::Output);
    AddNode(g, 2, T::ConstFloat);
    AddNode(g, 3, T::ConstFloat2);
    AddNode(g, 4, T::ConstColor);
    { Arcane::GraphNode& p = AddNode(g, 5, T::Param); p.paramName = "k"; p.hasRange = true; }
    { Arcane::GraphNode& t = AddNode(g, 6, T::TextureSample); t.paramName = "tex"; }
    AddNode(g, 7, T::Swizzle);
    AddNode(g, 8, T::PassInput);
    AddNode(g, 9, T::Panner);
    { Arcane::GraphNode& c = AddNode(g, 10, T::Custom); c.customPins = { { "p1", 1 } }; c.customBody = "return p1;\nreturn 2;"; }
    { Arcane::GraphNode& c = AddNode(g, 11, T::Comment); c.paramName = "note"; }
    AddNode(g, 12, T::Mul);
    AddNode(g, 13, T::UV);
    g.nextId = 14;
    NodePageHarness h(GraphDoc(std::move(g)));
    struct Expect { std::uint32_t id; std::vector<std::string> rows; bool inputs, settings, outputs; };
    const Expect table[] = {
        { 1,  { "color" },                                              true,  false, false },
        { 2,  { "Value", "out" },                                       false, true,  true  },
        { 3,  { "Value" },                                              false, true,  true  },
        { 4,  { "Color" },                                              false, true,  true  },
        { 5,  { "Name", "Type", "Default", "Range", "Min", "Max" },     false, true,  true  },
        { 6,  { "uv", "Texture Param", "rgba", "a" },                   true,  true,  true  },
        { 7,  { "x", "Mask", "out" },                                   true,  true,  true  },
        { 8,  { "uv", "Slot" },                                         true,  true,  true  },
        { 9,  { "uv", "speed", "Fractional" },                          true,  true,  true  },
        { 10, { "p1", "Name", "Width", "#Remove", "#Add Pin", "Output", "Body", "#Edit HLSL..." }, true, true, true },
        { 11, { "Text" },                                               false, true,  false },
        { 12, { "a", "b", "out" },                                      true,  false, true  },
        { 13, { "out" },                                                false, false, true  },
    };
    for (const Expect& e : table)
    {
        h.key = NodeKeyOf(0, e.id); h.Frame(); h.Frame();
        INFO("node " << e.id << "\n" << h.log);
        REQUIRE(h.pageDrawn);
        for (const std::string& r : e.rows) { INFO(r); CHECK(h.probe.count(r) == 1); }
        CHECK(HasSection(h.log, "Inputs") == e.inputs);
        CHECK(HasSection(h.log, "Settings") == e.settings);
        CHECK(HasSection(h.log, "Outputs") == e.outputs);
    }
    h.key = NodeKeyOf(0, 11); h.Frame();                     // s5.1.9: no Comment size row
    CHECK(h.probe.size() == 1);                              // "Text" only
    h.key = NodeKeyOf(0, 10); h.Frame();
    CHECK(h.log.find("return p1;") != std::string::npos);    // Body = the FIRST line...
    CHECK(h.log.find("return 2;") == std::string::npos);     // ...the rest is the tooltip
}

TEST_CASE("Node page Settings: a Texture-typed Param draws a read-only Default row, never a 0-lane VecRow", "[editor][material][nodepage]")
{
    // MatParamType::Texture is outside the Type combo but loads (codegen
    // diagnoses it); ComponentCount(Texture) == 0 must not reach VecRow's
    // n >= 2 assert, and nothing may write paramDefault.f on it.
    Arcane::MaterialGraph g = NodePageGraph();
    {
        Arcane::GraphNode& p = *g.FindNode(3);
        p.paramType = Arcane::MatParamType::Texture;
        p.paramDefault.type = Arcane::MatParamType::Texture;
        p.paramDefault.f[0] = 0.5f;
    }
    NodePageHarness h(GraphDoc(std::move(g)));
    h.key = NodeKeyOf(0, 3); h.Frame(); h.Frame();           // Debug: an abort here before the fix
    INFO(h.log);
    REQUIRE(h.pageDrawn);
    CHECK(HasSection(h.log, "Settings"));
    for (const char* r : { "Name", "Type", "Default", "Range" }) { INFO(r); CHECK(h.probe.count(r) == 1); }
    CHECK(h.log.find("n/a") != std::string::npos);          // the read-only text (ellipsized past the cell)
    h.Press(h.Centre("Default"));
    const ImGuiID moveId = ImGui::FindWindowByName("Inspector")->MoveId;   // a press on plain text grabs the window
    CHECK((h.ctx->ActiveId == 0 || h.ctx->ActiveId == moveId));            // no drag/box under the row: read-only
    ImGui::GetIO().AddMouseButtonEvent(0, false); h.Frame();
    CHECK_FALSE(h.stack.CanUndo());
    CHECK(h.Node(0, 3)->paramType == Arcane::MatParamType::Texture);
    CHECK(h.Node(0, 3)->paramDefault.f[0] == 0.5f);
}

TEST_CASE("Node page Settings: a Param rename is ONE 'Rename Param' step and starts the assisted rename", "[editor][material][nodepage]")
{
    NodePageHarness h(GraphDoc(NodePageGraph()));
    h.key = NodeKeyOf(0, 3); h.Frame(); h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("glow");
    h.Click(ImVec2(380.0f, 980.0f));                         // empty window space: deactivate = commit
    CHECK(h.Node(0, 3)->paramName == "glow");
    REQUIRE(h.stack.CanUndo());
    CHECK(std::string(h.stack.UndoLabel()) == "Rename Param");
    REQUIRE(h.doc->PendingParamRenames().size() == 1);
    CHECK(h.doc->PendingParamRenames()[0] == std::pair<std::string, std::string>{ "tint", "glow" });
    h.stack.Undo();
    CHECK(h.Node(0, 3)->paramName == "tint");
    CHECK_FALSE(h.stack.CanUndo());
}

TEST_CASE("Node page Settings: no page-side validation (codegen's verdict shows in Errors) and a pin rename never rewrites the body",
          "[editor][material][nodepage]")
{
    NodePageHarness h(GraphDoc(NodePageGraph()));
    h.key = NodeKeyOf(0, 3); h.Frame(); h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("1bad");
    h.Click(ImVec2(380.0f, 980.0f));
    h.Frame();
    CHECK(h.Node(0, 3)->paramName == "1bad");                // the page refused nothing
    CHECK(HasSection(h.log, "Errors (1)"));
    CHECK(h.log.find("not a valid identifier") != std::string::npos);

    h.key = NodeKeyOf(0, 7); h.Frame(); h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("q1");
    h.Click(ImVec2(380.0f, 980.0f));
    CHECK(h.Node(0, 7)->customPins[0].name == "q1");
    CHECK(h.Node(0, 7)->customBody == "return p1;");         // untouched: the compile will say why
    CHECK(std::string(h.stack.UndoLabel()) == "Rename Pin");
}

TEST_CASE("Node page Settings: Remove Pin routes through RemoveCustomPin as ONE step", "[editor][material][nodepage]")
{
    // Re-indexing an EARLIER pin is pinned on the shared member (T3-B2's test);
    // the probe resolves "#Remove" to the LAST drawn pin, so this removes p2.
    Arcane::MaterialGraph g = NodePageGraph();
    g.FindNode(7)->customPins = { { "p1", 4 }, { "p2", 1 } };
    AddNode(g, 10, T::ConstFloat);
    g.links.push_back({ 10, 0, 7, 1 });
    g.nextId = 11;
    NodePageHarness h(GraphDoc(std::move(g)));
    const std::string before = GraphJson(h, 0);
    h.key = NodeKeyOf(0, 7); h.Frame(); h.Frame();
    h.Click(h.Centre("#Remove"));
    REQUIRE(h.Node(0, 7)->customPins.size() == 1);
    CHECK(h.Node(0, 7)->customPins[0].name == "p1");
    for (const Arcane::GraphLink& l : h.doc->PassGraph(0)->links) CHECK(l.fromNode != 10);
    CHECK(std::string(h.stack.UndoLabel()) == "Remove Pin");
    h.stack.Undo();
    CHECK_FALSE(h.stack.CanUndo());
    CHECK(GraphJson(h, 0) == before);
}

TEST_CASE("Node page Settings: a stored text commit is inert once the document or the node is gone", "[editor][material][nodepage]")
{
    SECTION("document destroyed with the box active")
    {
        NodePageHarness h(GraphDoc(NodePageGraph()));
        h.key = NodeKeyOf(0, 6); h.Frame(); h.Frame();
        h.Click(h.Centre("Mask"));
        h.Type("zw");
        h.doc.reset();
        h.Frame(); h.Frame(); h.Frame();                     // CommitOrphans flushes through the stored commit
        CHECK_FALSE(h.stack.CanUndo());                      // the anchor is dead: nothing ran, nothing crashed
    }
    SECTION("node deleted with the box active")
    {
        NodePageHarness h(GraphDoc(NodePageGraph()));
        h.key = NodeKeyOf(0, 6); h.Frame(); h.Frame();
        h.Click(h.Centre("Mask"));
        h.Type("zw");
        Arcane::MaterialGraph g = *h.doc->PassGraph(0);
        std::erase_if(g.nodes, [](const Arcane::GraphNode& n) { return n.id == 6; });
        h.doc->ApplyGraphState(0, g);                        // the row vanishes; its draft is orphaned
        h.Frame(); h.Frame(); h.Frame();
        CHECK_FALSE(h.stack.CanUndo());
        CHECK(h.doc->PassGraph(0)->FindNode(6) == nullptr);
    }
}

// ---- Node page s5.1.11 / s5.1.9 page-side acceptance. The body-modal case
// extends "Edit HLSL opens from the page with no canvas" above through the
// page's own button route (a real click on "Edit HLSL..."), not RequestBodyEdit. ----
TEST_CASE("Node page s5.1.11: Edit HLSL... opens the body modal with the canvas NOT drawn, and Apply writes the page's pass",
          "[editor][material][nodepage]")
{
    NodePageHarness h(ChainDoc());
    const std::string base = GraphJson(h, 0);
    h.key = NodeKeyOf(1, 3); h.Frame(); h.Frame();              // the document window never draws in this test
    h.Click(h.Centre("#Edit HLSL..."));
    h.Frame(); h.Frame();                                       // the modal opens, then settles at its centred position
    ImGuiWindow* modal = nullptr;
    for (const ImGuiPopupData& p : h.ctx->OpenPopupStack)
        if (p.Window && std::string(p.Window->Name).find("Edit HLSL") != std::string::npos) modal = p.Window;
    REQUIRE(modal != nullptr);
    ImGuiWindow* box = nullptr;                                 // the multiline's child window
    for (ImGuiWindow* w : h.ctx->Windows)
        if (std::string(w->Name).find("bodyedit") != std::string::npos) box = w;
    REQUIRE(box != nullptr);
    h.Click(ImVec2((box->Rect().Min.x + box->Rect().Max.x) * 0.5f, (box->Rect().Min.y + box->Rect().Max.y) * 0.5f));
    h.Type("//");
    ImGui::ActivateItemByID(modal->GetID("Apply")); h.Frame(); h.Frame();
    CHECK(h.Node(1, 3)->customBody != "return float4(x, x, x, 1.0);");   // pass 1 took the edit...
    CHECK(GraphJson(h, 0) == base);                                     // ...never the active base
    REQUIRE(h.stack.CanUndo());
    CHECK(std::string(h.stack.UndoLabel()) == "Edit HLSL Body");
}

TEST_CASE("Node page s5.1.9: the page never creates, deletes, copies or pastes nodes", "[editor][material][nodepage]")
{
    NodePageHarness h(GraphDoc(NodePageGraph()));
    for (std::uint32_t id = 1; id <= 9; ++id)
    {
        h.key = NodeKeyOf(0, id); h.Frame(); h.Frame();
        INFO("node " << id);
        for (const auto& [label, at] : h.probe)
            for (const char* verb : { "Delete", "Duplicate", "Copy", "Paste", "Create" })
                CHECK(label.find(verb) == std::string::npos);
    }
    const std::size_t count = h.doc->PassGraph(0)->nodes.size();
    h.key = NodeKeyOf(0, 4); h.Frame();
    h.Click(ImVec2(380.0f, 980.0f));                            // focus the page's window
    h.Key(ImGuiKey_Delete);
    ImGuiIO& io = ImGui::GetIO();
    Arcane::TestKeys::AddKeyEvent(io, ImGuiMod_Ctrl, true);
    h.Key(ImGuiKey_C); h.Key(ImGuiKey_V); h.Key(ImGuiKey_D);
    Arcane::TestKeys::AddKeyEvent(io, ImGuiMod_Ctrl, false); h.Frame();
    CHECK(h.doc->PassGraph(0)->nodes.size() == count);
    CHECK_FALSE(h.stack.CanUndo());
}

// ==== Node page s5.1.11 document acceptance (T3-B7) ====
namespace
{
    // A whole-graph swap as an undo step, so a test can undo a CREATE (= a
    // delete of the selected node) without the canvas.
    struct GraphSwap final : Arcane::ICommand
    {
        ShaderEditorDocument& doc;
        Arcane::MaterialGraph before, after;
        GraphSwap(ShaderEditorDocument& d, Arcane::MaterialGraph b, Arcane::MaterialGraph a) : doc(d), before(std::move(b)), after(std::move(a)) {}
        void Undo() override { doc.ApplyGraphState(0, before); }
        void Redo() override { doc.ApplyGraphState(0, after); }
        const char* Label() const override { return "Create Node"; }
    };
}

TEST_CASE("Node page s5.1.11 document: PageFor, RestoreSelection, SelectByPath, crumbs, and undo of the selected node",
          "[editor][material][nodepage][inspector]")
{
    NodePageHarness h(GraphDoc(NodePageGraph()));
    ShaderEditorDocument& doc = *h.doc;
    CHECK(doc.PageFor("material") == doc.Page());              // the material page (closes an untested gap)
    CHECK(doc.PageFor(NodeKeyOf(0, 4)) != nullptr);
    CHECK(doc.PageFor(NodeKeyOf(0, 4)) != doc.PageFor("material"));
    CHECK(doc.PageFor(NodeKeyOf(0, 99)) == nullptr);           // missing id
    CHECK(doc.PageFor(NodeKeyOf(5, 4)) == nullptr);            // out-of-range pass: NOT the base fallback

    const std::uint64_t e0 = doc.SelectionEpoch();
    REQUIRE(doc.RestoreSelection(NodeKeyOf(0, 4)));
    CHECK(doc.SelectionKey() == NodeKeyOf(0, 4));              // immediately, before any draw
    CHECK(doc.SelectionEpoch() == e0);
    doc.SelectByPath("5");
    CHECK(doc.SelectionKey() == NodeKeyOf(0, 5));
    CHECK(doc.SelectionEpoch() == e0 + 1);                     // a scripted select is ONE event

    const auto crumbs = doc.PageFor(NodeKeyOf(0, 4))->Breadcrumb();
    REQUIRE(crumbs.size() == 2);
    CHECK(crumbs[0].label == "NodePageGraph");
    CHECK(crumbs[0].key == std::optional<std::string>{ "material" });
    CHECK(crumbs[1].label == "Multiply");
    CHECK(crumbs[1].key == std::optional<std::string>{ NodeKeyOf(0, 4) });

    Arcane::MaterialGraph without = *doc.PassGraph(0);
    Arcane::MaterialGraph with = without;
    Arcane::GraphNode fresh; fresh.id = 10; fresh.type = T::ConstFloat;
    with.nodes.push_back(fresh); with.nextId = 11;
    doc.ApplyGraphState(0, with);
    h.stack.Push(std::make_unique<GraphSwap>(doc, without, with));
    REQUIRE(doc.RestoreSelection(NodeKeyOf(0, 10)));
    const std::uint64_t e1 = doc.SelectionEpoch();
    h.stack.Undo();                                             // deletes the selected node
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == e1);
    h.stack.Redo();
    CHECK(doc.SelectionKey() == NodeKeyOf(0, 10));
}

TEST_CASE("Node page s5.1.11 document: chain crumbs are material > pass > node, with the pass crumb inert while pinned",
          "[editor][material][nodepage][inspector]")
{
    NodePageHarness h(ChainDoc());
    const auto crumbs = h.doc->PageFor(NodeKeyOf(1, 2))->Breadcrumb();
    REQUIRE(crumbs.size() == 3);
    CHECK(crumbs[0].key == std::optional<std::string>{ "material" });
    CHECK(crumbs[1].label == "blur");
    CHECK_FALSE(crumbs[1].key.has_value());
    CHECK(crumbs[2].label == "Float");
    CHECK(crumbs[2].key == std::optional<std::string>{ NodeKeyOf(1, 2) });
}
