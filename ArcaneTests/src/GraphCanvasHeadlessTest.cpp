// Slice 9 diagnostic + regression: drive the REAL ShaderEditorDocument::Draw()
// for a GRAPH-OWNED document through a device-less ImGui frame (null backend --
// no window, no GPU; software font atlas). This is the exact path the desk
// crash took (New Graph Material -> doc opens -> first canvas frame), which no
// other device-less test exercises: the imgui-node-editor canvas only runs inside
// a live ImGui frame.

// The R5 record case reads an input pin's bounds through the node editor's
// internal header (Detail::Pin::m_Bounds), as GraphFitTest.cpp does: the define
// and that header lead, with C4996 silenced around it alone (its vendored
// crude_json.h uses std::aligned_storage, deprecated in C++23).
#define IMGUI_DEFINE_MATH_OPERATORS
#pragma warning(push)
#pragma warning(disable : 4996)
#include <imgui_node_editor_internal.h>
#pragma warning(pop)

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Astra/Registry/Registry.hpp>

#include "Documents/DocumentHost.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Documents/ShaderNodeKey.hpp"   // FormatNodeKey: the s5.1.11 canvas cases
#include "Panels/InspectorHost.hpp"      // the harness routes the doc's page like the editor
#include "Panels/InspectorWindows.hpp"
#include "Widgets/GraphWire.hpp"      // DrawGraphWire: the wire-gradient painter guard (T3-D3)
#include "Widgets/PropertyGrid.hpp"
#include "Helpers/NodePageDocs.hpp"   // SpriteNodeDoc / ChainNodeDoc / HeadlessImGui (node page s5.1.11)
#include "Helpers/SoftRaster.hpp"     // CaptureFrameIfRequested: opt-in desk-pass evidence (T3-D6 fix round 1)

#include <imgui.h>
#include <imgui_internal.h>   // OpenPopupStack: the modal-hoist case
#include <imgui_node_editor.h>   // the s5.1.11 canvas cases ask the canvas what is selected

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "ImGuiTestKeys.hpp"   // TestKeys::AddKeyEvent: Ctrl as this platform's user presses it

using namespace Arcane;
using namespace Arcane::Editor;

TEST_CASE("Graph document survives device-less ImGui frames", "[editor][graphcanvas]")
{
    // The New-Graph-Material seed: Color wired to Output (EditorApp's shape).
    MaterialGraph g;
    GraphNode out;
    out.id = 1;
    out.type = GraphNodeType::Output;
    out.posX = 420.0f;
    out.posY = 200.0f;
    GraphNode color;
    color.id = 2;
    color.type = GraphNodeType::ConstColor;
    color.posX = 160.0f;
    color.posY = 200.0f;
    color.value[0] = 0.2f; color.value[1] = 0.8f; color.value[2] = 1.0f; color.value[3] = 1.0f;
    GraphNode custom;
    custom.id = 3;
    custom.type = GraphNodeType::Custom;
    custom.posX = 160.0f;
    custom.posY = 360.0f;
    custom.customPins = { { "x", 1 } };
    custom.customBody = "return float4(x, x, x, 1.0);";
    g.nodes = { out, color, custom };
    GraphLink l;
    l.fromNode = 2;
    l.toNode = 1;
    g.links.push_back(l);
    g.nextId = 4;

    MaterialAssetData data;
    data.id = Guid::FromString("dddd4444-4444-4444-8444-444444444444").value();
    data.name = "graphcanvas";
    data.kind = "fullscreen";
    auto gen = GenerateGraphSnippet(g);
    REQUIRE(gen.Ok());
    data.snippet = gen.snippet;
    data.graph = std::move(g);

    // Device-less ImGui: no backend; a 1x1 white font texture satisfies NewFrame.
    IMGUI_CHECKVERSION();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);   // software build, no upload

    {
        // Device-less services: the ctor skips preview resources; Rebuild
        // no-ops without a compiler/sources -- the CANVAS is what we exercise.
        DocServices services{};
        ShaderEditorDocument doc(services, std::filesystem::path("graphcanvas.arcmat"),
                                 std::move(data));
        REQUIRE(doc.IsGraphOwned());

        // Several frames: frame 1 creates the editor context + seeds node
        // positions; frame 2+ runs the readback path; all draw nodes, pins,
        // links, and the create/delete/context-menu queries.
        for (int frame = 0; frame < 4; ++frame)
        {
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            bool requestClose = false;
            doc.Draw(requestClose);
            CHECK_FALSE(requestClose);
            ImGui::Render();   // draw data discarded -- no backend
        }
        // Doc dtor runs here, inside the live context (DestroyEditor).
    }

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

namespace
{
    // One frame of the document window; the canvas draws inside it.
    void DocFrame(ShaderEditorDocument& doc)
    {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        bool requestClose = false;
        doc.Draw(requestClose);
        ImGui::Render();
    }
}

TEST_CASE("Node page mirror: a restore selects on the canvas with no event, and the canvas keeps it",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;   // before the doc: its dtor needs the context
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:3"));
    DocFrame(doc);
    DocFrame(doc);
    // The read after ed::End rebuilds the mirror from the canvas EVERY drawn
    // frame: had the Select request not landed, it would read 0 selected
    // nodes and the key would have fallen back to "material".
    CHECK(doc.SelectionKey() == "node:0:3");
    CHECK(doc.SelectionEpoch() == 1);
    REQUIRE(doc.RestoreSelection("material"));       // arms Clear
    DocFrame(doc);
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("Node page mirror: a selection the canvas makes itself is ONE event (the Problems locator path)",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    DocFrame(doc);                                   // the context exists, nothing selected
    CHECK(doc.SelectionEpoch() == 1);
    doc.RequestFocusGraphNode(3);                    // ed::SelectNode + NavigateToSelection, not a request
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "node:0:3");
    CHECK(doc.SelectionEpoch() == 2);
    DocFrame(doc);                                   // unchanged selection: no event
    CHECK(doc.SelectionEpoch() == 2);
}

TEST_CASE("Node page mirror: undo/redo of the selected node with the canvas drawing -- material with no event, then the node again",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:4"));
    DocFrame(doc);
    DocFrame(doc);
    REQUIRE(doc.SelectionKey() == "node:0:4");
    std::optional<Arcane::MaterialGraph> with = Arcane::Test::SpriteNodeDoc().graph;
    std::optional<Arcane::MaterialGraph> without = with;
    std::erase_if(without->nodes, [](const Arcane::GraphNode& n) { return n.id == 4; });
    doc.ApplyGraphState(0, without);                 // GraphEditCommand::Undo's path
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
    doc.ApplyGraphState(0, with);                    // Redo restores the same id
    DocFrame(doc);
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "node:0:4");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("Node page mirror: a pass switch is not an event, and the fresh canvas resets the mirror",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "chain.arcmat", Arcane::Test::ChainNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:2"));       // leaves the overview (EnterPass(0))
    DocFrame(doc);
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "node:0:2");
    CHECK(doc.SelectionEpoch() == 1);
    ShaderEditorDocument::PassListState s = doc.CapturePassListState();
    s.activePass = 1;
    doc.ApplyPassListState(std::move(s));            // the rebuilt context starts empty
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("Node page mirror: restoring a node in another pass switches the canvas and the selection survives the rebuild",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "chain.arcmat", Arcane::Test::ChainNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:2"));
    DocFrame(doc);
    DocFrame(doc);
    REQUIRE(doc.SelectionKey() == "node:0:2");
    REQUIRE(doc.RestoreSelection("node:1:2"));       // EnterPass(1): the canvas context is rebuilt next frame
    DocFrame(doc);
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "node:1:2");         // the Select landed AFTER the switch's ClearSelection
    CHECK(doc.SelectionEpoch() == 1);                // a restore is never an event
}

TEST_CASE("Node page modal hoist: the canvas still opens the HLSL body editor", "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    DocFrame(doc);
    doc.RequestBodyEdit(0, 4);
    DocFrame(doc);
    bool open = false;
    for (const ImGuiPopupData& p : imgui.ctx->OpenPopupStack)
        open = open || (p.Window && std::string(p.Window->Name).find("Edit HLSL") != std::string::npos);
    CHECK(open);
}

// ---- Node page s5.1.11 canvas acceptance: the selection mirror (s5.1.1) seen
// from the canvas side -- what imgui-node-editor itself holds selected, a real
// mouse click, a programmatic multi-select, pass switches and the chain view. ----
namespace
{
    namespace ed = ax::NodeEditor;

    // Device-less ImGui (HeadlessImGui, FIRST member so it destructs last) + a
    // document drawn through its REAL Draw() (the canvas), one DocFrame per frame.
    struct CanvasHarness
    {
        Arcane::Test::HeadlessImGui imgui;
        // A real undo stack over an EMPTY registry (ShaderEditorDocumentTest's
        // fixture): the node page's live rows open gestures on it.
        Astra::Registry registry;
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return registry; } };
        std::unique_ptr<ShaderEditorDocument> doc;

        explicit CanvasHarness(MaterialAssetData data)
            : doc(std::make_unique<ShaderEditorDocument>(Services(&stack), std::filesystem::path("canvas.arcmat"), std::move(data)))
        {
        }
        static DocServices Services(Arcane::CommandStack* stack)
        {
            DocServices services{};
            services.undo = [stack]() -> Arcane::CommandStack* { return stack; };
            return services;
        }
        ~CanvasHarness() { doc.reset(); }   // inside the context (DestroyEditor)
        CanvasHarness(const CanvasHarness&) = delete;
        CanvasHarness& operator=(const CanvasHarness&) = delete;

        // The editor's frame order (EditorAppFrame.cpp: DrawAll, then the
        // epoch -> NotifySelected loop, then DrawInspectorWindows). PageMode:
        // None = the canvas alone; Plain = the document's CURRENT page in a
        // bare window; Host = the real InspectorHost + DrawInspectorWindows
        // over a real undo stack, the way the editor routes and draws it.
        enum class PageMode { None, Plain, Host };
        PageMode pageMode = PageMode::None;
        PropertyGridState grid;
        struct NullSource final : InspectorSource
        {
            std::string SourceName() const override { return "Scene"; }
            std::string_view Kind() const override { return "scene"; }
            InspectorPage* Page() override { return nullptr; }
            InspectorPage* PageFor(std::string_view) override { return nullptr; }
            std::string SelectionKey() const override { return {}; }
            bool RestoreSelection(std::string_view) override { return false; }
            bool Resolves(std::string_view) const override { return false; }
        } scene;
        InspectorHost host{ scene };
        InspectorWindowsState windows;
        std::uint64_t lastEpoch = 0;

        void UseHost()
        {
            pageMode = PageMode::Host;
            host.AddSource(*doc);
        }

        void Frame(int n = 1)
        {
            for (int i = 0; i < n; ++i)
            {
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                bool requestClose = false;
                doc->Draw(requestClose);
                if (pageMode == PageMode::Plain)
                {
                    PropertyGrid(grid).CommitOrphans();
                    ImGui::SetNextWindowPos(ImVec2(1060.0f, 0.0f));
                    ImGui::SetNextWindowSize(ImVec2(220.0f, 720.0f));
                    (void)ImGui::Begin("Inspector");
                    if (InspectorPage* page = doc->Page())
                    {
                        PropertyGrid g(grid);
                        page->Draw(g);
                    }
                    ImGui::End();
                }
                else if (pageMode == PageMode::Host)
                {
                    if (const std::uint64_t e = doc->SelectionEpoch(); e != lastEpoch)
                    {
                        lastEpoch = e;
                        if (!doc->SelectionKey().empty())
                            host.NotifySelected(*doc);
                    }
                    ImGui::SetNextWindowPos(ImVec2(1060.0f, 0.0f));
                    ImGui::SetNextWindowSize(ImVec2(220.0f, 720.0f));
                    (void)DrawInspectorWindows(host, windows, nullptr);
                }
                ImGui::Render();
            }
        }
        void Click(ImVec2 at)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y); Frame();
            io.AddMouseButtonEvent(0, true); Frame();
            io.AddMouseButtonEvent(0, false); Frame(2);
        }
        template <typename F> auto InCanvas(F&& f)
        {
            ed::SetCurrentEditor(doc->GraphCanvasContext());
            auto r = f();
            ed::SetCurrentEditor(nullptr);
            return r;
        }
        // A point inside the node's title band, in screen space.
        ImVec2 NodeTitle(std::uint32_t id)
        {
            return InCanvas([&]
            {
                const ImVec2 p = ed::CanvasToScreen(ed::GetNodePosition(ed::NodeId(id)));
                return ImVec2(p.x + ed::GetNodeSize(ed::NodeId(id)).x * 0.5f, p.y + 8.0f);
            });
        }
    };
    MaterialAssetData TwoNodeGraph(const char* kind, MaterialGraph g)
    {
        MaterialAssetData data;
        data.id = Guid::Generate();
        data.name = "Canvas";
        data.kind = kind;
        data.graph = std::move(g);
        return data;
    }
    // Output 1 <- Float 2; Float 3 unwired.
    MaterialGraph OutputAndFloats()
    {
        MaterialGraph g;
        GraphNode out; out.id = 1; out.type = GraphNodeType::Output; out.posX = 420.0f; out.posY = 80.0f;
        GraphNode a;   a.id = 2;   a.type = GraphNodeType::ConstFloat; a.posX = 60.0f; a.posY = 80.0f;
        GraphNode b;   b.id = 3;   b.type = GraphNodeType::ConstFloat; b.posX = 60.0f; b.posY = 220.0f;
        g.nodes = { out, a, b };
        g.links = { { 2, 0, 1, 0 } };
        g.nextId = 4;
        return g;
    }
    std::string Key(std::size_t pass, std::uint32_t id) { return FormatNodeKey({ pass, id }); }
}

TEST_CASE("Node page s5.1.11 canvas: a restore selects in the canvas with no event; 2+ selected is the material page",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(2);
    REQUIRE(h.doc->RestoreSelection(Key(0, 2)));
    const std::uint64_t e0 = h.doc->SelectionEpoch();
    h.Frame(2);
    CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(2)); }));
    CHECK(h.doc->SelectionKey() == Key(0, 2));
    CHECK(h.doc->SelectionEpoch() == e0);                      // a restore is not an event

    h.InCanvas([] { ed::SelectNode(ed::NodeId(3), /*append*/ true); return 0; });   // programmatic 2-node selection
    h.Frame(2);
    CHECK(h.doc->SelectionKey() == "material");                // s5.1.9: no multi-select page
}

TEST_CASE("Node page s5.1.11 canvas: a click on a node is exactly ONE selection event beyond the content click",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    const std::uint64_t e0 = h.doc->SelectionEpoch();
    h.Click(ImVec2(1000.0f, 650.0f));                          // empty canvas: a content click, no selection change
    const std::uint64_t e1 = h.doc->SelectionEpoch();
    CHECK(h.doc->SelectionKey() == "material");
    const ImVec2 title = h.NodeTitle(2);
    INFO("node 2 title point " << title.x << ", " << title.y);
    h.Click(title);
    CHECK(h.doc->SelectionKey() == Key(0, 2));
    CHECK(h.doc->SelectionEpoch() - e1 == (e1 - e0) + 1);      // the same content click + one selection bump
}

TEST_CASE("Node page s5.1.11 canvas: pass switches and the chain view move no epoch; leaving the chain view restores the node",
          "[editor][graphcanvas][nodepage]")
{
    MaterialAssetData data = TwoNodeGraph("fullscreen", OutputAndFloats());
    MaterialPass blur; blur.name = "blur"; blur.inputs = { 0 }; blur.graph = OutputAndFloats();
    data.passes.push_back(std::move(blur));
    CanvasHarness h(std::move(data));
    h.Frame(2);                                                 // opens on the chain overview
    CHECK(h.doc->SelectionKey() == "material");
    const std::uint64_t e0 = h.doc->SelectionEpoch();
    REQUIRE(h.doc->RestoreSelection(Key(1, 2)));                // enters pass 1 (history: chain, pass 1)
    h.Frame(3);
    CHECK(h.doc->SelectionKey() == Key(1, 2));

    SECTION("pass switch")
    {
        REQUIRE(h.doc->RestoreSelection(Key(0, 2)));            // rebuilds the canvas context for the base
        h.Frame(3);
        CHECK(h.doc->SelectionKey() == Key(0, 2));
        CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(2)); }));
        CHECK(h.doc->SelectionEpoch() == e0);
    }
    SECTION("chain view")
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(500.0f, 400.0f); h.Frame();
        io.AddMouseButtonEvent(3, true); h.Frame(); io.AddMouseButtonEvent(3, false); h.Frame(2);   // back: the overview
        CHECK(h.doc->SelectionKey() == "material");             // pass-canvas nodes keep the material page
        io.AddMouseButtonEvent(4, true); h.Frame(); io.AddMouseButtonEvent(4, false); h.Frame(2);   // forward: pass 1
        CHECK(h.doc->SelectionKey() == Key(1, 2));              // the mirror survived the overview
        CHECK(h.doc->SelectionEpoch() == e0);
    }
}

// ---- R5 (spec s5.1.11 desk list, risk R5): does a click on a node's INLINE
// widget select the node? The question is recorded, not fixed, in T3. The probe:
// Sine's unwired `x` input carries the pin-literal DragFloat ("##lit", drawn
// right of the pin, ShaderEditorDocument.cpp:5555-5582). A drag on that point
// must change the literal -- that proves the point IS the widget -- and then the
// page key says whether imgui-node-editor also selected the node. ----
namespace
{
    // Output 1 <- Sin 2 (x unwired, so it shows the literal); Float 3 unwired.
    MaterialGraph OutputSineFloat()
    {
        MaterialGraph g;
        GraphNode out; out.id = 1; out.type = GraphNodeType::Output; out.posX = 420.0f; out.posY = 80.0f;
        GraphNode s;   s.id = 2;   s.type = GraphNodeType::Sin;      s.posX = 60.0f;  s.posY = 80.0f;
        GraphNode f;   f.id = 3;   f.type = GraphNodeType::ConstFloat; f.posX = 60.0f; f.posY = 260.0f;
        g.nodes = { out, s, f };
        g.links = { { 2, 0, 1, 0 } };
        g.nextId = 4;
        return g;
    }
    const GraphPinLiteral* SineLiteral(const ShaderEditorDocument& doc)
    {
        const MaterialGraph* g = doc.PassGraph(0);
        if (!g) return nullptr;
        for (const GraphNode& n : g->nodes)
            if (n.id == 2) return n.FindPinLiteral(0);
        return nullptr;
    }
}

// R5 VERDICT (recorded 2026-10-01, T3-GATE fix round 2): NO. A press on an
// inline widget makes that ImGui item active, and imgui-node-editor's
// BuildControl returns an empty Control while any non-editor item is active
// (ThirdParty/imgui-node-editor/imgui_node_editor.cpp:2576-2577), so no node is
// "clicked" and the canvas selection is untouched: the page stays on its
// previous key, whether that is the material or another node. This case PINS
// that behaviour as the record; a later fix (select the owning node on an
// inline-widget press) flips these CHECKs on purpose.
TEST_CASE("Node page R5 record: a click on a node's inline widget (the Sine x literal) does NOT select the node",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputSineFloat()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // empty canvas: the material page
    REQUIRE(h.doc->SelectionKey() == "material");
    REQUIRE(SineLiteral(*h.doc) == nullptr);                   // absent until touched

    // The literal's centre: right of the x pin's bounds (pin, SameLine, a 64 px
    // drag), on the pin's row, in canvas space -> screen.
    const ImVec2 lit = h.InCanvas([]
    {
        auto* editor = reinterpret_cast<ed::Detail::EditorContext*>(ed::GetCurrentEditor());
        const ed::Detail::Pin* pin = editor->FindPin(ed::PinId(2 * 1000ull + 1 + 0));   // InPin(2, 0)
        REQUIRE(pin != nullptr);
        const ImRect b = pin->m_Bounds;
        const ImVec2 c(b.Max.x + ImGui::GetStyle().ItemSpacing.x + 32.0f, (b.Min.y + b.Max.y) * 0.5f);
        return ed::CanvasToScreen(c);
    });
    INFO("Sine x literal point " << lit.x << ", " << lit.y);
    const auto sineSelected = [&] { return h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(2)); }); };

    ImGuiIO& io = ImGui::GetIO();
    SECTION("press-drag-release on the literal: the value moves, the page stays on the material")
    {
        io.AddMousePosEvent(lit.x, lit.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame();
        io.AddMousePosEvent(lit.x + 30.0f, lit.y); h.Frame(2);
        io.AddMouseButtonEvent(0, false); h.Frame(2);
        const GraphPinLiteral* l = SineLiteral(*h.doc);
        REQUIRE(l != nullptr);                                  // the point IS the widget: the drag wrote x
        CHECK(l->v[0] != 0.0f);
        CHECK_FALSE(sineSelected());
        CHECK(h.doc->SelectionKey() == "material");
    }
    SECTION("plain click on the literal: the page stays on the material")
    {
        h.Click(lit);
        CHECK_FALSE(sineSelected());
        CHECK(h.doc->SelectionKey() == "material");
    }
    SECTION("plain click on the literal with another node selected: the page stays on that node")
    {
        h.Click(h.NodeTitle(3));
        REQUIRE(h.doc->SelectionKey() == Key(0, 3));
        h.Click(lit);
        CHECK_FALSE(sineSelected());
        CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(3)); }));
        CHECK(h.doc->SelectionKey() == Key(0, 3));
    }
}

TEST_CASE("Node page s5.1.11 canvas: a click on node A then DIRECTLY on node B lands on B, page drawn or not",
          "[editor][graphcanvas][nodepage]")
{
    const int mode = GENERATE(0, 1, 2);
    INFO("page mode (0 none, 1 plain window, 2 real InspectorHost): " << mode);
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    if (mode == 1) h.pageMode = CanvasHarness::PageMode::Plain;
    if (mode == 2) h.UseHost();
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // settles the open fit: NodeTitle reads the landed view
    h.Click(h.NodeTitle(2));
    REQUIRE(h.doc->SelectionKey() == Key(0, 2));
    const std::uint64_t e1 = h.doc->SelectionEpoch();
    h.Click(h.NodeTitle(3));                                    // no deselect in between
    CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(3)); }));
    CHECK(h.doc->SelectionKey() == Key(0, 3));
    CHECK(h.doc->SelectionEpoch() > e1);
    h.Click(h.NodeTitle(2));                                    // and straight back
    CHECK(h.doc->SelectionKey() == Key(0, 2));
}

TEST_CASE("Node page s5.1.11 canvas: a click on a visible node lands while another node is CULLED off-screen",
          "[editor][graphcanvas][nodepage]")
{
    // The desk reselect report. A culled node (NodeCulled) is submitted as a
    // stand-in whose pins are ImGui::Dummy(0,0), so their bounds are zero-size.
    // Upstream imgui-node-editor's invisibleButtonEx returned `false` (= 0, i.e.
    // "clicked with button 0") for a zero-size area, so every culled pin
    // reported a left click on every frame. BuildControl walks m_Nodes back to
    // front and OVERWRITES clickedObject on each hit, so a real node click lost
    // to whichever culled pin was walked after it: the Control carried a
    // ClickedPin and no ClickedNode, and SelectAction selected nothing.
    //
    // Order matters, so it is arranged: m_Nodes is creation order (the doc
    // draws g.nodes in order: 1, 2, 3) with the pressed node rotated to the
    // back, and the walk is in REVERSE. Culling node 1 (the FIRST created)
    // puts its pins LAST in the walk, after whichever visible node is clicked.
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // settles the open fit (and measures every node)

    // Move node 1 far outside the cull rect (viewport + its 0.25 guard band).
    // It was drawn already, so it has a measured size and NodeCulled applies.
    h.InCanvas([] { ed::SetNodePosition(ed::NodeId(1), ImVec2(40000.0f, 40000.0f)); return 0; });
    h.Frame(3);
    const ImVec2 parked = h.InCanvas([] { return ed::CanvasToScreen(ed::GetNodePosition(ed::NodeId(1))); });
    INFO("node 1 screen pos " << parked.x << ", " << parked.y);
    REQUIRE((parked.x > 1280.0f * 1.25f || parked.y > 720.0f * 1.25f));   // beyond the guard band: culled

    const std::uint64_t e0 = h.doc->SelectionEpoch();
    h.Click(h.NodeTitle(2));
    CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(2)); }));
    CHECK(h.doc->SelectionKey() == Key(0, 2));
    CHECK(h.doc->SelectionEpoch() > e0);

    const std::uint64_t e1 = h.doc->SelectionEpoch();
    h.Click(h.NodeTitle(3));                                    // A then B, no deselect in between
    CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(3)); }));
    CHECK(h.doc->SelectionKey() == Key(0, 3));
    CHECK(h.doc->SelectionEpoch() > e1);
}

TEST_CASE("Canvas pin paint: a Mul fed by a float4 Param resolves to 4 -- its pins paint PinColorForWidth(4) with the adapts ring; fixed and unresolved pins do not",
          "[editor][graphcanvas]")
{
    // Output 1 <- Mul 3 (a <- Param 2 'tint', float4); Add 4 unwired.
    MaterialGraph g;
    GraphNode out; out.id = 1; out.type = GraphNodeType::Output; out.posX = 520.0f; out.posY = 80.0f;
    GraphNode param; param.id = 2; param.type = GraphNodeType::Param; param.posX = 40.0f; param.posY = 80.0f;
    param.paramName = "tint";
    param.paramType = MatParamType::Float4;
    param.paramDefault = MatParamValue::MakeFloat4(1.0f, 1.0f, 1.0f, 1.0f);
    GraphNode mul; mul.id = 3; mul.type = GraphNodeType::Mul; mul.posX = 280.0f; mul.posY = 80.0f;
    GraphNode add; add.id = 4; add.type = GraphNodeType::Add; add.posX = 280.0f; add.posY = 260.0f;
    g.nodes = { out, param, mul, add };
    g.links = { { 2, 0, 3, 0 }, { 3, 0, 1, 0 } };
    g.nextId = 5;
    CanvasHarness h(TwoNodeGraph("sprite", g));
    h.Frame(3);

    const auto same = [](const ImVec4& a, const ImVec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; };
    const GraphPinPaint mulA = h.doc->CanvasPinPaint(3, 0, /*input*/ true);
    CHECK(same(mulA.color, PinColorForWidth(4)));
    CHECK(mulA.adapts);
    const GraphPinPaint mulB = h.doc->CanvasPinPaint(3, 1, true);    // unwired, but its node resolved
    CHECK(same(mulB.color, PinColorForWidth(4)));
    CHECK(mulB.adapts);
    const GraphPinPaint mulOut = h.doc->CanvasPinPaint(3, 0, false);
    CHECK(same(mulOut.color, PinColorForWidth(4)));
    CHECK(mulOut.adapts);
    const GraphPinPaint paramOut = h.doc->CanvasPinPaint(2, 0, false);   // Param's dynamic out: its type's lanes
    CHECK(same(paramOut.color, PinColorForWidth(4)));
    CHECK(paramOut.adapts);
    const GraphPinPaint outColor = h.doc->CanvasPinPaint(1, 0, true);    // a FIXED float4 pin: no ring
    CHECK(same(outColor.color, PinColorForWidth(4)));
    CHECK_FALSE(outColor.adapts);
    const GraphPinPaint addA = h.doc->CanvasPinPaint(4, 0, true);         // unresolved: plain grey
    CHECK(same(addA.color, PinColorForWidth(0)));
    CHECK_FALSE(addA.adapts);

    // The resolution is re-taken every frame: unwire the Param and the Mul goes grey.
    MaterialGraph unwired = *h.doc->PassGraph(0);
    unwired.links = { { 3, 0, 1, 0 } };
    h.doc->ApplyGraphState(0, unwired);
    h.Frame(2);
    const GraphPinPaint after = h.doc->CanvasPinPaint(3, 0, true);
    CHECK(same(after.color, PinColorForWidth(0)));
    CHECK_FALSE(after.adapts);
}

TEST_CASE("Canvas pin tooltip (T3-D6 desk item 18): HOVERING a Mul input fed by a float4 Param shows the pin's name, 'dynamic (now float4)' and what it is wired from",
          "[editor][graphcanvas]")
{
    // The pure PinTooltipText case pins the wording; this drives the REAL
    // canvas with a stationary mouse over the pin, the way the desk does, and
    // reads the tooltip the frame actually drew.
    MaterialGraph g;
    GraphNode out; out.id = 1; out.type = GraphNodeType::Output; out.posX = 520.0f; out.posY = 80.0f;
    GraphNode param; param.id = 2; param.type = GraphNodeType::Param; param.posX = 40.0f; param.posY = 80.0f;
    param.paramName = "tint";
    param.paramType = MatParamType::Float4;
    param.paramDefault = MatParamValue::MakeFloat4(1.0f, 1.0f, 1.0f, 1.0f);
    GraphNode mul; mul.id = 3; mul.type = GraphNodeType::Mul; mul.posX = 280.0f; mul.posY = 80.0f;
    g.nodes = { out, param, mul };
    g.links = { { 2, 0, 3, 0 }, { 3, 0, 1, 0 } };
    g.nextId = 4;
    CanvasHarness h(TwoNodeGraph("sprite", g));
    h.Frame(90);   // past the fit-on-open's animated settle: the view no longer moves under the scan
    const ImRect node = h.InCanvas([]
    {
        const ImVec2 p = ed::CanvasToScreen(ed::GetNodePosition(ed::NodeId(3)));
        const ImVec2 sz = ed::GetNodeSize(ed::NodeId(3));
        return ImRect(p, ImVec2(p.x + sz.x, p.y + sz.y));
    });
    REQUIRE(node.GetWidth() > 0.0f);

    // Walk the node's left (input) edge top-down; at each point hold the mouse
    // still past the tooltip delay, logging the last frame's text.
    ImGuiIO& io = ImGui::GetIO();
    std::string tip;
    for (float y = node.Min.y + 2.0f; y < node.Max.y && tip.empty(); y += 3.0f)
        for (float x = node.Min.x + 2.0f; x < node.Min.x + 26.0f && tip.empty(); x += 4.0f)
        {
            io.AddMousePosEvent(x, y);
            h.Frame(24);   // 0.4 s stationary: past HoverDelayShort/HoverStationaryDelay
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::LogToBuffer();
            bool requestClose = false;
            h.doc->Draw(requestClose);
            const std::string logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::Render();
            bool tooltipShown = false;
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                if ((w->Flags & ImGuiWindowFlags_Tooltip) && w->Active)
                    tooltipShown = true;
            if (tooltipShown && logged.find("dynamic (now") != std::string::npos)
            {
                tip = logged;
                Arcane::Test::CaptureFrameIfRequested("T3-D6-B-18-pin-tooltip");   // the frame that drew it
            }
        }
    INFO("tooltip log: " << tip);
    REQUIRE_FALSE(tip.empty());
    CHECK(tip.find("dynamic (now float4)") != std::string::npos);   // the type word, resolved
    const std::size_t from = tip.find("wired from");                 // what it is wired to...
    REQUIRE(from != std::string::npos);
    CHECK(tip.substr(from, 48).find("tint") != std::string::npos);  // ...named by its source
}

// ---- T3-D3 desk findings (2026-10-02): the marquee and its modifiers, undo
// keeping the view, Esc reverting an inline canvas drag. ----
namespace
{
    // The screen rect just covering nodes 2 and 3 of OutputAndFloats (the two
    // Floats in the left column), padded so the press lands on empty canvas.
    ImRect FloatsBox(CanvasHarness& h)
    {
        return h.InCanvas([]
        {
            ImRect r(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
            for (const std::uint32_t id : { 2u, 3u })
            {
                const ImVec2 p = ed::GetNodePosition(ed::NodeId(id));
                const ImVec2 s = ed::GetNodeSize(ed::NodeId(id));
                r.Add(ed::CanvasToScreen(p));
                r.Add(ed::CanvasToScreen(ImVec2(p.x + s.x, p.y + s.y)));
            }
            r.Expand(12.0f);
            return r;
        });
    }
    // Press on `from`, drag through the midpoint to `to`, release -- with
    // `mod` (ImGuiMod_Shift / ImGuiMod_Ctrl) held throughout when given.
    // `capture` (opt-in evidence, SoftRaster.hpp): the frame with the box
    // drawn mid-drag, and the frame after the release, as <capture>-drag /
    // <capture>-released.
    void Marquee(CanvasHarness& h, ImVec2 from, ImVec2 to, ImGuiKey mod = ImGuiKey_None,
                 const char* capture = nullptr)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (mod != ImGuiKey_None) Arcane::TestKeys::AddKeyEvent(io, mod, true);
        io.AddMousePosEvent(from.x, from.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame();
        // The press is the canvas's: a point off the canvas (the toolbar, the
        // window padding) grabs the WINDOW instead and no marquee can start.
        REQUIRE(ImGui::GetCurrentContext()->MovingWindow == nullptr);
        io.AddMousePosEvent((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f); h.Frame();
        io.AddMousePosEvent(to.x, to.y); h.Frame(2);
        if (capture) Arcane::Test::CaptureFrameIfRequested((std::string(capture) + "-drag").c_str());
        io.AddMouseButtonEvent(0, false); h.Frame(2);
        if (mod != ImGuiKey_None) { Arcane::TestKeys::AddKeyEvent(io, mod, false); h.Frame(); }
        if (capture) Arcane::Test::CaptureFrameIfRequested((std::string(capture) + "-released").c_str());
    }
    void ClickWith(CanvasHarness& h, ImVec2 at, ImGuiKey mod)
    {
        ImGuiIO& io = ImGui::GetIO();
        Arcane::TestKeys::AddKeyEvent(io, mod, true);
        h.Click(at);
        Arcane::TestKeys::AddKeyEvent(io, mod, false); h.Frame();
    }
    bool Selected(CanvasHarness& h, std::uint32_t id)
    {
        return h.InCanvas([id] { return ed::IsNodeSelected(ed::NodeId(id)); });
    }
}

TEST_CASE("Canvas marquee (T3-D3): a left-drag from empty canvas selects the nodes inside the box -- 2+ selected is the material page",
          "[editor][graphcanvas][nodepage]")
{
    const int mode = GENERATE(0, 1, 2);
    const bool cull = GENERATE(false, true);
    INFO("page mode (0 none, 1 plain window, 2 real InspectorHost): " << mode << "; node 1 culled off-screen: " << cull);
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    if (mode == 1) h.pageMode = CanvasHarness::PageMode::Plain;
    if (mode == 2) h.UseHost();
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // settles the open fit: the box reads the landed view
    if (cull)
    {
        h.InCanvas([] { ed::SetNodePosition(ed::NodeId(1), ImVec2(40000.0f, 40000.0f)); return 0; });
        h.Frame(3);
    }
    const ImRect box = FloatsBox(h);
    INFO("box " << box.Min.x << ", " << box.Min.y << " -> " << box.Max.x << ", " << box.Max.y);
    Marquee(h, box.Min, box.Max);
    CHECK(Selected(h, 2));
    CHECK(Selected(h, 3));
    CHECK_FALSE(Selected(h, 1));                                // the Output lies outside the box
    CHECK(h.doc->SelectionKey() == "material");                 // s5.1.9: 2+ selected is the material page
}

// UE's Material Editor: Shift ADDS (Shift+drag = FMarqueeOperation::Add,
// Shift+click = "Shift always adds to selection", SNodePanel.cpp:194-212 /
// MarqueeOperation.h:50-68). Ctrl+drag keeps the selection too (the library's
// own rule; UE inverts, which differs only on boxed nodes already selected).
TEST_CASE("Canvas marquee (T3-D3): Shift or Ctrl held, the box ADDS to the selection; Shift+click adds a node",
          "[editor][graphcanvas][nodepage]")
{
    const ImGuiKey mod = GENERATE(ImGuiMod_Shift, ImGuiMod_Ctrl);
    INFO("modifier: " << (mod == ImGuiMod_Shift ? "Shift" : "Ctrl"));
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));
    h.Click(h.NodeTitle(1));
    REQUIRE(h.doc->SelectionKey() == Key(0, 1));
    const ImRect box = FloatsBox(h);
    Marquee(h, box.Min, box.Max, mod, mod == ImGuiMod_Shift ? "T3-D6-B-21-shift-marquee" : nullptr);
    CHECK(Selected(h, 1));                                      // kept
    CHECK(Selected(h, 2));
    CHECK(Selected(h, 3));
    CHECK(h.doc->SelectionKey() == "material");
}

TEST_CASE("Canvas click (T3-D3): Shift+click on a node adds it to the selection, as in UE",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));
    h.Click(h.NodeTitle(2));
    REQUIRE(h.doc->SelectionKey() == Key(0, 2));
    ClickWith(h, h.NodeTitle(3), ImGuiMod_Shift);
    CHECK(Selected(h, 2));
    CHECK(Selected(h, 3));
    CHECK(h.doc->SelectionKey() == "material");
    ClickWith(h, h.NodeTitle(3), ImGuiMod_Shift);               // adds, never toggles (Ctrl toggles)
    CHECK(Selected(h, 3));
}

namespace
{
    // The Sine x literal's centre in screen space (the R5 probe point): right
    // of the x pin's bounds -- pin, SameLine, a 64 px drag -- on the pin's row.
    ImVec2 SineLiteralPoint(CanvasHarness& h)
    {
        return h.InCanvas([]
        {
            auto* editor = reinterpret_cast<ed::Detail::EditorContext*>(ed::GetCurrentEditor());
            const ed::Detail::Pin* pin = editor->FindPin(ed::PinId(2 * 1000ull + 1 + 0));   // InPin(2, 0)
            REQUIRE(pin != nullptr);
            const ImRect b = pin->m_Bounds;
            return ed::CanvasToScreen(ImVec2(b.Max.x + ImGui::GetStyle().ItemSpacing.x + 32.0f, (b.Min.y + b.Max.y) * 0.5f));
        });
    }
    struct CanvasView
    {
        float zoom = 0.0f;
        ImVec2 origin;   // canvas (0,0) on screen
    };
    CanvasView ViewOf(CanvasHarness& h)
    {
        return h.InCanvas([] { return CanvasView{ ed::GetCurrentZoom(), ed::CanvasToScreen(ImVec2(0.0f, 0.0f)) }; });
    }
    bool SameView(const CanvasView& a, const CanvasView& b)
    {
        constexpr float kEps = 1e-3f;
        return std::abs(a.zoom - b.zoom) <= kEps && std::abs(a.origin.x - b.origin.x) <= kEps &&
               std::abs(a.origin.y - b.origin.y) <= kEps;
    }
}

TEST_CASE("Canvas view (T3-D3): undo and redo of a graph edit keep the user's zoom and scroll -- the fit-on-open fires once per open, not per reseed",
          "[editor][graphcanvas]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputSineFloat()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // the open fit has landed
    const CanvasView fitted = ViewOf(h);

    // The user's own view: two wheel notches in, off-centre (zoom AND scroll move).
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(300.0f, 250.0f); h.Frame();
    io.AddMouseWheelEvent(0.0f, 1.0f); h.Frame();
    io.AddMouseWheelEvent(0.0f, 1.0f); h.Frame(20);
    const CanvasView user = ViewOf(h);
    INFO("fitted zoom " << fitted.zoom << " user zoom " << user.zoom);
    REQUIRE_FALSE(SameView(fitted, user));

    // A real edit: drag the Sine x literal (one undo step on release).
    const ImVec2 lit = SineLiteralPoint(h);
    io.AddMousePosEvent(lit.x, lit.y); h.Frame();
    io.AddMouseButtonEvent(0, true); h.Frame();
    io.AddMousePosEvent(lit.x + 30.0f, lit.y); h.Frame(2);
    io.AddMouseButtonEvent(0, false); h.Frame(2);
    REQUIRE(SineLiteral(*h.doc) != nullptr);
    REQUIRE(h.stack.CanUndo());
    const CanvasView edited = ViewOf(h);
    CHECK(SameView(user, edited));

    h.stack.Undo();
    h.Frame(10);
    CHECK(SineLiteral(*h.doc) == nullptr);                      // the edit is gone ...
    const CanvasView afterUndo = ViewOf(h);
    INFO("after undo zoom " << afterUndo.zoom << " origin " << afterUndo.origin.x << ", " << afterUndo.origin.y
         << "; user origin " << user.origin.x << ", " << user.origin.y);
    CHECK(SameView(user, afterUndo));                           // ... the view is not

    h.stack.Redo();
    h.Frame(10);
    CHECK(SineLiteral(*h.doc) != nullptr);
    CHECK(SameView(user, ViewOf(h)));
}

// The node page's numeric rows restore their seed on Esc mid-drag
// (PropertyGrid.cpp NumericRow); a canvas inline drag does the same: the value
// from the drag's start, the gesture ended, NO undo step, ClearActiveID.
TEST_CASE("Canvas inline drag (T3-D3): Esc while the button is held reverts the Sine x literal and leaves the undo stack alone",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputSineFloat()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 lit = SineLiteralPoint(h);
    INFO("Sine x literal point " << lit.x << ", " << lit.y);
    auto dragThenEsc = [&](float dx)
    {
        io.AddMousePosEvent(lit.x, lit.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame();
        io.AddMousePosEvent(lit.x + dx, lit.y); h.Frame(2);
        REQUIRE(ImGui::GetActiveID() != 0);                      // mid-drag
        io.AddKeyEvent(ImGuiKey_Escape, true); h.Frame();
        CHECK(ImGui::GetActiveID() == 0);                        // the gesture ended at the Esc
        io.AddKeyEvent(ImGuiKey_Escape, false); h.Frame();
        io.AddMouseButtonEvent(0, false); h.Frame(2);
    };

    SECTION("an untouched literal: it is absent again, and no step was pushed")
    {
        REQUIRE(SineLiteral(*h.doc) == nullptr);
        dragThenEsc(30.0f);
        CHECK(SineLiteral(*h.doc) == nullptr);
        CHECK_FALSE(h.stack.CanUndo());
    }
    SECTION("an existing literal: back to its value at the drag's start, and the stack still holds only the first edit")
    {
        io.AddMousePosEvent(lit.x, lit.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame();
        io.AddMousePosEvent(lit.x + 20.0f, lit.y); h.Frame(2);
        io.AddMouseButtonEvent(0, false); h.Frame(2);
        const GraphPinLiteral* first = SineLiteral(*h.doc);
        REQUIRE(first != nullptr);
        const float start = first->v[0];
        REQUIRE(start != 0.0f);
        REQUIRE(h.stack.CanUndo());

        dragThenEsc(40.0f);
        const GraphPinLiteral* after = SineLiteral(*h.doc);
        REQUIRE(after != nullptr);
        CHECK(after->v[0] == start);
        h.stack.Undo();                                          // undoes the FIRST drag ...
        h.Frame(2);
        CHECK(SineLiteral(*h.doc) == nullptr);
        CHECK_FALSE(h.stack.CanUndo());                          // ... and there was nothing else
    }
}

// ---- T3-D3 wire-gradient guard (user ruling 2026-10-02): a wire is a
// gradient from its source pin's colour to its destination pin's, so it is a
// gradient exactly where the two ends paint differently and one tone where they
// match. Two halves: the canvas hands the painter BOTH end colours, and the
// painter strokes both. ----
TEST_CASE("Canvas wire gradient (T3-D3): a wire whose ends paint differently hands DrawGradientWire two distinct tints -- float into a float4-resolved Mul, float2 into the fixed float4 Output; a float4 -> float4 wire stays one tone",
          "[editor][graphcanvas]")
{
    // Mul 3 (a <- Param 2 'tint' float4, b <- Float 4) resolves to float4;
    // Output 1 <- Float2 5. Links in this order: 0 = Param -> Mul.a (4 -> 4),
    // 1 = Float -> Mul.b (1 -> resolved 4), 2 = Float2 -> Output (2 -> fixed 4).
    MaterialGraph g;
    GraphNode out;   out.id = 1;   out.type = GraphNodeType::Output;      out.posX = 520.0f; out.posY = 80.0f;
    GraphNode param; param.id = 2; param.type = GraphNodeType::Param;     param.posX = 40.0f; param.posY = 80.0f;
    param.paramName = "tint";
    param.paramType = MatParamType::Float4;
    param.paramDefault = MatParamValue::MakeFloat4(1.0f, 1.0f, 1.0f, 1.0f);
    GraphNode mul;   mul.id = 3;   mul.type = GraphNodeType::Mul;         mul.posX = 280.0f; mul.posY = 80.0f;
    GraphNode f1;    f1.id = 4;    f1.type = GraphNodeType::ConstFloat;   f1.posX = 40.0f;   f1.posY = 260.0f;
    GraphNode f2;    f2.id = 5;    f2.type = GraphNodeType::ConstFloat2;  f2.posX = 280.0f;  f2.posY = 300.0f;
    g.nodes = { out, param, mul, f1, f2 };
    g.links = { { 2, 0, 3, 0 }, { 4, 0, 3, 1 }, { 5, 0, 1, 0 } };
    g.nextId = 6;
    CanvasHarness h(TwoNodeGraph("sprite", g));
    h.Frame(3);

    const auto same = [](const ImVec4& a, const ImVec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; };

    // Matched ends: one tone (the painter's equal-colour path).
    const auto matched = h.doc->CanvasWireTintOf(0);
    REQUIRE(matched.has_value());
    CHECK(same(matched->from, PinColorForWidth(4)));
    CHECK(same(matched->to, PinColorForWidth(4)));

    // float -> a dynamic input resolved to float4: BOTH tints, and they differ.
    const auto scalarIn = h.doc->CanvasWireTintOf(1);
    REQUIRE(scalarIn.has_value());
    CHECK(same(scalarIn->from, PinColorForWidth(1)));
    CHECK(same(scalarIn->to, PinColorForWidth(4)));
    CHECK_FALSE(same(scalarIn->from, scalarIn->to));
    // ... and each end is exactly its pin dot's colour.
    CHECK(same(scalarIn->from, h.doc->CanvasPinPaint(4, 0, /*input*/ false).color));
    CHECK(same(scalarIn->to, h.doc->CanvasPinPaint(3, 1, /*input*/ true).color));

    // float2 -> the fixed float4 Output pin: two tints, distinct.
    const auto vec2In = h.doc->CanvasWireTintOf(2);
    REQUIRE(vec2In.has_value());
    CHECK(same(vec2In->from, PinColorForWidth(2)));
    CHECK(same(vec2In->to, PinColorForWidth(4)));
    CHECK_FALSE(same(vec2In->from, vec2In->to));
    CHECK(same(vec2In->from, h.doc->CanvasPinPaint(5, 0, false).color));
    CHECK(same(vec2In->to, h.doc->CanvasPinPaint(1, 0, true).color));

    CHECK_FALSE(h.doc->CanvasWireTintOf(3).has_value());          // only the links that draw walked
}

TEST_CASE("Graph wire paint (T3-D3): DrawGraphWire strokes two different end colours as a gradient -- the tail vertices carry the source hue, the head vertices the destination hue; equal ends stay one tone",
          "[editor][graphcanvas]")
{
    // The painter half of the guard, on a canvas of its own: the stroke's
    // vertices are exactly the ones DrawGraphWire appends to the shared vertex
    // buffer (channels split index and command lists, never vertices).
    Arcane::Test::HeadlessImGui imgui;
    ed::Config cfg;
    cfg.SettingsFile = nullptr;
    ed::EditorContext* ctx = ed::CreateEditor(&cfg);

    // The RGB of every vertex the stroke appended (alpha dropped: the AA
    // fringe repeats each colour at zero alpha), in emission order.
    const auto stroke = [&](const ImVec4& from, const ImVec4& to)
    {
        std::vector<ImU32> rgb;
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f));
        (void)ImGui::Begin("WireCanvas");
        ed::SetCurrentEditor(ctx);
        ed::Begin("wire");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        REQUIRE(dl->_Splitter._Count > kGraphLinkChannel);       // else the painter draws nothing
        const int first = dl->VtxBuffer.Size;
        (void)DrawGraphWire(ImVec2(100.0f, 100.0f), ImVec2(500.0f, 260.0f), from, to, 2.0f, 1.0f);
        for (int i = first; i < dl->VtxBuffer.Size; ++i)
            rgb.push_back(dl->VtxBuffer[i].col & ~IM_COL32_A_MASK);
        ed::End();
        ed::SetCurrentEditor(nullptr);
        ImGui::End();
        ImGui::Render();
        return rgb;
    };
    const auto rgbOf = [](const ImVec4& c) { return ImGui::GetColorU32(c) & ~IM_COL32_A_MASK; };
    // Per-channel distance, 0..255.
    const auto dist = [](ImU32 a, ImU32 b)
    {
        int d = 0;
        for (const int shift : { IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT })
            d = (std::max)(d, std::abs(static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF)));
        return d;
    };

    const ImVec4 src = PinColorForWidth(1);
    const ImVec4 dst = PinColorForWidth(4);
    const std::vector<ImU32> gradient = stroke(src, dst);
    REQUIRE(gradient.size() >= 4);
    // The tail sits on the source hue, the head on the destination hue, each
    // nearer its own end than the other (sampled at segment midpoints, so not
    // bit-exact), and the run between them is not one tone.
    const ImU32 tail = gradient.front();
    const ImU32 head = gradient.back();
    INFO("tail " << std::hex << tail << " head " << head << " src " << rgbOf(src) << " dst " << rgbOf(dst));
    CHECK(dist(tail, rgbOf(src)) < dist(tail, rgbOf(dst)));
    CHECK(dist(head, rgbOf(dst)) < dist(head, rgbOf(src)));
    std::vector<ImU32> tones = gradient;
    std::sort(tones.begin(), tones.end());
    tones.erase(std::unique(tones.begin(), tones.end()), tones.end());
    CHECK(tones.size() > 2);

    // Equal ends: every vertex is the one tone.
    const std::vector<ImU32> flat = stroke(dst, dst);
    REQUIRE_FALSE(flat.empty());
    for (const ImU32 c : flat)
        CHECK(c == rgbOf(dst));

    ed::DestroyEditor(ctx);
}

namespace
{
    // The midpoint of a wire's curve, in screen space: a press here lands ON
    // the wire (FindLinkAt's hit radius is the wire's thickness plus
    // c_LinkSelectThickness, imgui_node_editor.cpp:984-998, 2240-2246).
    ImVec2 WireMidpoint(CanvasHarness& h, std::uint32_t linkIndex)
    {
        return h.InCanvas([linkIndex]
        {
            auto* editor = reinterpret_cast<ed::Detail::EditorContext*>(ed::GetCurrentEditor());
            const ed::Detail::Link* link = editor->FindLink(ed::LinkId(linkIndex + 1));   // ed::LinkId(i + 1)
            REQUIRE(link != nullptr);
            const ImCubicBezierPoints c = link->GetCurve();
            const ImVec2 mid = ImCubicBezier(c.P0, c.P1, c.P2, c.P3, 0.5f);
            REQUIRE(editor->FindLinkAt(mid) == link);                // the press really is on the wire
            return ed::CanvasToScreen(mid);
        });
    }
    // A real mouse: the press, then 1 px a frame along the drag for 12 px --
    // so the 1 px drag-lock threshold (SelectAction::Accept's
    // IsMouseDragging(.., 1)) is crossed right beside the press point, not
    // at a far-away first sample the way Marquee's two-step drag crosses it.
    void SlowMarquee(CanvasHarness& h, ImVec2 from, ImVec2 to)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(from.x, from.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame();
        const ImVec2 d(to.x - from.x, to.y - from.y);
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        for (int px = 1; px <= 12; ++px)
        {
            io.AddMousePosEvent(from.x + d.x / len * float(px), from.y + d.y / len * float(px));
            h.Frame();
        }
        io.AddMousePosEvent((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f); h.Frame();
        io.AddMousePosEvent(to.x, to.y); h.Frame(2);
        io.AddMouseButtonEvent(0, false); h.Frame(2);
    }
}

// UE parity (SGraphPanel.cpp:1034-1123): a plain left press on a hovered wire
// falls through to SNodePanel::OnMouseButtonDown -- the marquee -- unless the
// schema allows relinking. A dense graph (the desk's logo_showcase) is crossed
// by wires everywhere, so "empty" canvas is often within a wire's hit radius.
TEST_CASE("Canvas marquee (T3-D3): a left-drag that starts ON a wire still draws the box and selects the nodes inside it",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));
    const ImVec2 wire = WireMidpoint(h, 0);                      // Float 2 -> Output 1
    const ImRect box = FloatsBox(h);
    INFO("wire point " << wire.x << ", " << wire.y << "; box " << box.Min.x << ", " << box.Min.y << " -> " << box.Max.x << ", " << box.Max.y);
    REQUIRE(wire.x > box.Max.x);                                 // right of both Floats, left of the Output
    const ImVec2 to(box.Min.x, box.Max.y);
    const bool slow = GENERATE(false, true);
    INFO("slow (a real mouse: 1 px a frame off the press point first): " << slow);
    if (slow)
        SlowMarquee(h, wire, to);
    else
        Marquee(h, wire, to);
    CHECK(Selected(h, 2));
    CHECK(Selected(h, 3));
    CHECK_FALSE(Selected(h, 1));
    CHECK(h.doc->SelectionKey() == "material");
}

TEST_CASE("Canvas marquee (T3-D3): a SLOW left-drag from empty canvas (1 px a frame, as a real mouse) selects the nodes inside the box",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));
    const ImRect box = FloatsBox(h);
    SlowMarquee(h, box.Min, box.Max);
    CHECK(Selected(h, 2));
    CHECK(Selected(h, 3));
    CHECK_FALSE(Selected(h, 1));
    CHECK(h.doc->SelectionKey() == "material");
}

// ---- T3-D7 user desk bug (2026-10-02): "with a node selected [the Vertex
// Output] and panned so it is off screen, trying to move another node draws a
// white line connector leading from the selected node" -- a press-drag on a
// visible node started a LINK drag from an off-screen node's pin. ----
namespace
{
    ed::Detail::EditorContext& EditorOf(CanvasHarness& h)
    {
        return *reinterpret_cast<ed::Detail::EditorContext*>(h.doc->GraphCanvasContext());
    }
    ImRect NodeScreenRect(CanvasHarness& h, std::uint32_t id)
    {
        return h.InCanvas([id]
        {
            const ImVec2 p = ed::GetNodePosition(ed::NodeId(id));
            const ImVec2 s = ed::GetNodeSize(ed::NodeId(id));
            return ImRect(ed::CanvasToScreen(p), ed::CanvasToScreen(ImVec2(p.x + s.x, p.y + s.y)));
        });
    }
    // The off-screen stand-in's tell (ShaderEditorDocument::DrawGraphNode,
    // NodeCulled): every pin of a culled node is submitted as a 0x0 Dummy.
    bool PinsCollapsed(CanvasHarness& h, std::uint32_t id)
    {
        const ed::Detail::Node* node = EditorOf(h).FindNode(ed::NodeId(id));
        REQUIRE(node != nullptr);
        REQUIRE(node->m_LastPin != nullptr);
        for (const ed::Detail::Pin* pin = node->m_LastPin; pin; pin = pin->m_PreviousPin)
            if (pin->m_Bounds.GetWidth() > 0.0f || pin->m_Bounds.GetHeight() > 0.0f)
                return false;
        return true;
    }
    bool FullyOnCanvas(CanvasHarness& h, std::uint32_t id)
    {
        const ImRect canvas = EditorOf(h).GetRect();
        const ImRect r = NodeScreenRect(h, id);
        return canvas.Contains(r);
    }
    // Wheel-zoom in with the cursor on `at` (the zoom keeps that point fixed).
    void ZoomIn(CanvasHarness& h, ImVec2 at, int notches)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y); h.Frame();
        for (int i = 0; i < notches; ++i) { io.AddMouseWheelEvent(0.0f, 1.0f); h.Frame(); }
        h.Frame(20);                                             // the zoom animation lands
    }
    // A right-drag pan (Config::NavigateButtonIndex) from `from` by `delta`.
    void Pan(CanvasHarness& h, ImVec2 from, ImVec2 delta)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(from.x, from.y); h.Frame();
        io.AddMouseButtonEvent(1, true); h.Frame();
        for (int i = 1; i <= 4; ++i)
        {
            io.AddMousePosEvent(from.x + delta.x * float(i) / 4.0f, from.y + delta.y * float(i) / 4.0f);
            h.Frame();
        }
        io.AddMouseButtonEvent(1, false); h.Frame(2);
    }
    // Press on `from`, move 1 px a frame for 8 px then on to from + delta,
    // release; reports which editor actions ran while the button was held.
    struct DragSeen { bool linkDrag = false; bool nodeDrag = false; std::uint64_t linkFromNode = 0; };
    DragSeen PressDrag(CanvasHarness& h, ImVec2 from, ImVec2 delta)
    {
        ImGuiIO& io = ImGui::GetIO();
        DragSeen seen;
        auto observe = [&]
        {
            if (ed::Detail::EditorAction* a = EditorOf(h).GetCurrentAction())
            {
                if (ed::Detail::CreateItemAction* create = a->AsCreateItem())
                {
                    seen.linkDrag = true;
                    if (create->m_DraggedPin && create->m_DraggedPin->m_Node)
                        seen.linkFromNode = create->m_DraggedPin->m_Node->m_ID.Get();
                }
                seen.nodeDrag = seen.nodeDrag || a->AsDrag() != nullptr;
            }
        };
        io.AddMousePosEvent(from.x, from.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame(); observe();
        const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        for (int px = 1; px <= 8; ++px)
        {
            io.AddMousePosEvent(from.x + delta.x / len * float(px), from.y + delta.y / len * float(px));
            h.Frame(); observe();
        }
        io.AddMousePosEvent(from.x + delta.x, from.y + delta.y); h.Frame(); observe();
        h.Frame(); observe();
        io.AddMouseButtonEvent(0, false); h.Frame(2);
        return seen;
    }
}

TEST_CASE("Canvas drag (T3-D7 user desk): a press-drag on a visible node MOVES it while another node is panned off-screen -- no link drag from the off-screen node's pins",
          "[editor][graphcanvas][nodepage]")
{
    // The mechanism (imgui_node_editor.cpp BuildControl): a culled node's
    // pins are 0x0 stand-ins that submit NO ImGui item, so the walk's
    // IsItemActive() on them answered for the PREVIOUS item. The walk is
    // m_Nodes back to front and a press rotates the pressed node to the back
    // (End(), "Bring active node to front"), so the node walked right after it
    // is the one just before it in m_Nodes: the node pressed before -- the
    // SELECTED one -- or, with nothing pressed yet, the one created before it.
    // When that node was off-screen its pins took activeObject; DragAction
    // refuses a pin and CreateItemAction dragged a link from it. Pin kind
    // never mattered.
    //
    // OutputAndFloats: the Output (1, input pins ONLY -- the user's Vertex
    // Output) in the right column, Floats 2 and 3 (output pins only) left.
    struct Variant { const char* name; std::uint32_t select; std::uint32_t offscreen; std::uint32_t drag; float panX; };
    const Variant v = GENERATE(
        Variant{ "the desk: input-only Output SELECTED off-screen, drag Float 2", 1, 1, 2, +60.0f },
        Variant{ "a node WITH outputs (Float 3) selected off-screen, drag the Output", 3, 3, 1, -60.0f },
        Variant{ "the Output off-screen, NOTHING selected, drag Float 2", 0, 1, 2, +60.0f });
    INFO("variant: " << v.name);
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // settles the open fit
    if (v.select != 0)
    {
        h.Click(h.NodeTitle(v.select));
        REQUIRE(Selected(h, v.select));
    }

    // The user's gesture: the view moves (wheel zoom on the node to be dragged,
    // then right-drag pans) until the selected node is off-screen and culled.
    ZoomIn(h, h.NodeTitle(v.drag), 5);
    const ImRect canvas = EditorOf(h).GetRect();
    const ImVec2 panFrom(canvas.GetCenter().x - v.panX * 0.5f, canvas.Max.y - 16.0f);   // empty canvas under the nodes
    for (int i = 0; i < 40 && !PinsCollapsed(h, v.offscreen); ++i)
        Pan(h, panFrom, ImVec2(v.panX, 0.0f));
    INFO("drag node rect " << NodeScreenRect(h, v.drag).Min.x << ", " << NodeScreenRect(h, v.drag).Min.y
         << "; off-screen node rect " << NodeScreenRect(h, v.offscreen).Min.x << ", " << NodeScreenRect(h, v.offscreen).Min.y
         << "; canvas " << canvas.Min.x << ", " << canvas.Min.y << " -> " << canvas.Max.x << ", " << canvas.Max.y);
    REQUIRE(PinsCollapsed(h, v.offscreen));                     // culled: its pins are the 0x0 stand-ins
    REQUIRE(FullyOnCanvas(h, v.drag));
    if (v.select != 0)
        REQUIRE(Selected(h, v.select));                          // the pan kept the selection

    // The node follows the mouse: the canvas offset is the drag in canvas
    // units (GetCurrentZoom is canvas units per pixel), floored to the
    // editor's 16-unit grid by DragAction::Process's pivot alignment
    // (AlignPointToGrid: p - fmod(p, 16)) -- so within one grid cell under it.
    const ImVec2 drag(60.0f, 40.0f);
    auto canvasPos = [&] { return h.InCanvas([&] { return ed::GetNodePosition(ed::NodeId(v.drag)); }); };
    const float unitsPerPixel = h.InCanvas([] { return ed::GetCurrentZoom(); });
    const ImVec2 before = canvasPos();
    const DragSeen seen = PressDrag(h, h.NodeTitle(v.drag), drag);
    const ImVec2 moved(canvasPos().x - before.x, canvasPos().y - before.y);
    const ImVec2 want(drag.x * unitsPerPixel, drag.y * unitsPerPixel);
    INFO("moved " << moved.x << ", " << moved.y << " canvas units; the drag is " << want.x << ", " << want.y
         << "; link drag from node " << seen.linkFromNode);
    CHECK_FALSE(seen.linkDrag);                                 // no white connector
    CHECK(seen.nodeDrag);
    CHECK(moved.x > want.x - 16.5f);                            // the node followed the mouse
    CHECK(moved.x < want.x + 0.5f);
    CHECK(moved.y > want.y - 16.5f);
    CHECK(moved.y < want.y + 0.5f);
}

