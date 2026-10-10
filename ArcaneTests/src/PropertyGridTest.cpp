// PropertyGrid (inspector-ownership arc, Task 1): the shared page primitives,
// driven device-less through the REAL ImGui path (EditorInspectorVectorTest's
// harness shape: software atlas, window pinned at the origin, probe centres).
#include <catch2/catch_test_macros.hpp>
#include <Widgets/PropertyGrid.hpp>
#include "Input/EditorActions.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Widgets/EditorTheme.hpp>   // Theme::kError (the refused-value look)
#include <Scene/EditGesture.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include <imgui.h>
#include <imgui_internal.h>   // ColorStack / StyleVarStack (the refused-style balance check)
#include <array>
#include <cmath>     // std::round
#include <cstdlib>   // std::abs
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "ImGuiTestKeys.hpp"   // TestKeys::AddKeyEvent: Ctrl as this platform's user presses it

namespace
{
    struct GridHarness
    {
        Arcane::Editor::PropertyGridState state;
        std::unordered_map<std::string, ImVec2> probe;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        std::string name = "Alpha";
        bool flag = false;
        int commits = 0;
        bool sectionOpen = false;
        bool drawName = true;            // false = the A "Name" row vanishes; a B row draws under another id
        int commitsB = 0;
        std::string nameB = "Beta";
        float storedSeconds = 0.40f;     // the "model": Seconds is re-derived from it EVERY frame
        int floatCommits = 0;
        float scale = 1.0f;
        int scaleCommits = 0;
        std::function<std::optional<std::string>(std::string_view)> validate;   // the Name row's rule (empty = none)
        int nameStackDrift = 0;          // |colour + style-var stack change| across the Name row, summed over every frame
        bool errorDrawn = false;         // THIS frame: the Inspector window drew a vertex in Theme::kError
        float windowWidth = 640.0f;      // the Inspector's width (the degenerate-seed case narrows it)

        GridHarness()
        {
            IMGUI_CHECKVERSION();
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            state.probe = &probe;
        }
        ~GridHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }

        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            Arcane::Editor::PropertyGrid(state).CommitOrphans();   // once per frame, BEFORE any window that draws this state Begins
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(windowWidth, 1000), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            Arcane::Editor::PropertyGrid grid(state);
            sectionOpen = grid.Section("Action");
            if (sectionOpen)
            {
                Arcane::Editor::PropertyGrid::Rows rows(grid, "##fields");
                if (rows)
                {
                    const ImGuiContext& g = *ImGui::GetCurrentContext();
                    const int colours = g.ColorStack.Size, vars = g.StyleVarStack.Size;
                    if (drawName) grid.TextRow("Name", name, [&](std::string v) { name = std::move(v); ++commits; }, false, validate);
                    else { ImGui::PushID("B"); grid.TextRow("Name", nameB, [&](std::string v) { nameB = std::move(v); ++commitsB; }); ImGui::PopID(); }
                    nameStackDrift += std::abs(g.ColorStack.Size - colours) + std::abs(g.StyleVarStack.Size - vars);
                    grid.CheckboxRow("Blocking", flag);
                    grid.ReadOnlyRow("Path", "<Keyboard>/space");
                    float seconds = storedSeconds;   // re-derived from the "model" EVERY frame, as the input pages do
                    if (grid.FloatRow("Seconds", seconds, 0.01f)) { storedSeconds = seconds; ++floatCommits; }
                    if (grid.FloatRow("Scale", scale, 0.01f)) ++scaleCommits;
                }
            }
            ImGui::End();
            ImGui::Render();
            // Draw data, not pixels: did anything in the Inspector use the refusal colour?
            const ImU32 error = ImGui::ColorConvertFloat4ToU32(Arcane::Editor::Theme::kError);
            errorDrawn = false;
            for (const ImDrawVert& v : ImGui::FindWindowByName("Inspector")->DrawList->VtxBuffer) errorDrawn = errorDrawn || v.col == error;
        }
        ImVec2 Centre(const std::string& key) { INFO(key); REQUIRE(probe.count(key) == 1); return probe.at(key); }
        void Click(ImVec2 at)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y); Frame();
            io.AddMouseButtonEvent(0, true); Frame();
            io.AddMouseButtonEvent(0, false); Frame();
        }
        void Type(const char* s) { ImGui::GetIO().AddInputCharactersUTF8(s); Frame(); }
        void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }
        void Press(ImVec2 at) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(at.x, at.y); Frame(); io.AddMouseButtonEvent(0, true); Frame(); }
    };
}

TEST_CASE("PropertyGrid T3-D6 fix 1: the label split is never seeded from a degenerate first-frame width", "[editor][inspector]")
{
    // Measured at 1920x1080 headless (--select-asset uv_marker.png): Inspector
    // 2's ##page reported avail 4.0 on frame 1, the once-per-session seed
    // took 0.4 of it, and the Import rows drew every label as "..". A region
    // under the floor (8 font heights) leaves the split unseeded; the first
    // frame with a real width seeds it.
    GridHarness h;
    h.windowWidth = 8.0f;   // clamped up to WindowMinSize: a few px of content
    h.Frame();
    CHECK(h.state.labelColWidth == 0.0f);          // nothing seeded from a sliver
    h.windowWidth = 640.0f;
    h.Frame();
    CHECK(h.state.labelColWidth >= 200.0f);        // 0.4 of the real ~620 px region
    const float seeded = h.state.labelColWidth;
    h.windowWidth = 400.0f;
    h.Frame();
    CHECK(h.state.labelColWidth == seeded);        // once seeded, the session keeps it (unchanged rule)
}

TEST_CASE("PropertyGrid: rows draw into one shared grid, TextRow commits once, CheckboxRow toggles", "[editor][inspector]")
{
    GridHarness h;
    h.Frame();
    CHECK(h.sectionOpen);                       // DefaultOpen
    CHECK(h.state.labelColWidth > 0.0f);        // the first grid seeded the shared split
    REQUIRE(h.probe.count("Name") == 1);
    REQUIRE(h.probe.count("Blocking") == 1);

    h.Click(h.Centre("Blocking"));
    CHECK(h.flag);

    h.Click(h.Centre("Name"));                  // focus the text field
    ImGui::GetIO().AddInputCharacter('X'); h.Frame();
    CHECK(h.commits == 0);                      // typing alone never commits
    h.Click(ImVec2(600.0f, 900.0f));            // click empty window space: deactivate
    CHECK(h.commits == 1);
    CHECK(h.name == "X");                       // AutoSelectAll: the click selected "Alpha", the keystroke replaced it (D06)
    CHECK(h.name.find('X') != std::string::npos);
    h.Frame();
    CHECK(h.commits == 1);                      // exactly once
}

TEST_CASE("PropertyGrid: a draft deactivated while its row is not drawn still commits to the row's own target", "[editor][inspector]")
{
    GridHarness h; h.Frame();
    h.Click(h.Centre("Name"));
    ImGui::GetIO().AddInputCharacter('X'); h.Frame();
    CHECK(h.commits == 0);
    h.drawName = false;                                  // the A row vanishes (page switched / tab hidden); the B row draws under another id
    h.Frame(); h.Frame(); h.Frame();
    CHECK(h.commits == 1);                               // flushed by CommitOrphans, through A's stored commit
    CHECK(h.name.find('X') != std::string::npos);
    CHECK(h.commitsB == 0);
    CHECK(h.nameB == "Beta");
    h.Frame();
    CHECK(h.commits == 1);
}

TEST_CASE("PropertyGrid: FloatRow accumulates a multi-frame drag against a per-frame re-seeded value and commits once", "[editor][inspector]")
{
    GridHarness h; h.Frame();
    const ImVec2 c = h.Centre("Seconds");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    for (int i = 1; i <= 3; ++i) { io.AddMousePosEvent(c.x + 15.0f * i, c.y); h.Frame(); }   // each step beats the 3 px drag threshold
    CHECK(h.floatCommits == 0);                          // in flight: nothing committed
    io.AddMouseButtonEvent(0, false); h.Frame();
    CHECK(h.floatCommits == 1);                          // exactly once, on release
    CHECK(h.storedSeconds > 0.40f + 0.25f);              // the whole gesture, not one frame's delta, not the original
    h.Frame();
    CHECK(h.floatCommits == 1);
    CHECK(h.state.numericDrafts.empty());
    h.Click(h.Centre("Seconds"));                        // press + release without moving
    CHECK(h.floatCommits == 1);                          // no MarkItemEdited: no commit
}

TEST_CASE("PropertyGrid: Escape during a numeric drag restores the seed and commits nothing", "[editor][inspector]")
{
    GridHarness h; h.Frame();
    const ImVec2 c = h.Centre("Scale");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    io.AddMousePosEvent(c.x + 40.0f, c.y); h.Frame(); h.Frame();
    CHECK(h.scale != 1.0f);                              // the drag moved it (write-through)
    io.AddKeyEvent(ImGuiKey_Escape, true); h.Frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    io.AddMouseButtonEvent(0, false); h.Frame(); h.Frame();
    CHECK(h.scale == 1.0f);
    CHECK(h.scaleCommits == 0);
}

TEST_CASE("PropertyGrid: a rebound ui.cancel cancels a numeric drag on its own chord", "[editor][inspector][shortcuts]")
{
    Arcane::Editor::EditorActions& keys = Arcane::Editor::EditorActions::Get();
    Arcane::CVarRegistry& reg = keys.Registry();
    reg.Set(keys.HandleOf("ui.cancel"), Arcane::CVarValue::String("F9"), Arcane::SetBy::EditorUser, "editor", Arcane::CVarContext::Editor);
    reg.PublishImmediate();
    keys.RefreshBindings();
    GridHarness h; h.Frame();
    const ImVec2 c = h.Centre("Scale");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    io.AddMousePosEvent(c.x + 40.0f, c.y); h.Frame(); h.Frame();
    REQUIRE(h.scale != 1.0f);
    io.AddKeyEvent(ImGuiKey_F9, true); h.Frame();
    io.AddKeyEvent(ImGuiKey_F9, false);
    io.AddMouseButtonEvent(0, false); h.Frame(); h.Frame();
    CHECK(h.scale == 1.0f);
    CHECK(h.scaleCommits == 0);
    reg.RevertLayer(Arcane::SetBy::EditorUser);
    reg.PublishImmediate();
    keys.RefreshBindings();
}

TEST_CASE("PropertyGrid: a TextRow value refused on Enter keeps the typed text and re-arms; focus loss with a refused value reverts", "[editor][inspector]")
{
    GridHarness h;
    h.validate = [](std::string_view v) -> std::optional<std::string> { return v == "X" ? std::optional<std::string>("taken") : std::nullopt; };
    h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("X");                                      // AutoSelectAll: replaces "Alpha"
    h.Key(ImGuiKey_Enter);
    h.Frame(); h.Frame();
    CHECK(h.commits == 0);
    CHECK(h.name == "Alpha");
    REQUIRE(h.state.textDrafts.size() == 1);
    CHECK(h.state.textDrafts.begin()->second.text == "X");      // kept
    CHECK(h.state.textDrafts.begin()->second.active);           // re-armed
    h.Type("Y");                                      // re-activation selected all: "Y" replaces "X"
    h.Click(ImVec2(600, 900));
    CHECK(h.commits == 1);
    CHECK(h.name == "Y");

    h.Click(h.Centre("Name"));
    h.Type("X");
    h.Click(ImVec2(600, 900));                        // focus loss with a refused value
    h.Frame();
    CHECK(h.commits == 1);
    CHECK(h.name == "Y");
    for (const auto& [id, d] : h.state.textDrafts) CHECK(d.text != "X");
}

TEST_CASE("PropertyGrid: a refused-Enter hold that expires because its row vanished never grabs focus later (final review)", "[editor][inspector]")
{
    GridHarness h;
    h.validate = [](std::string_view v) -> std::optional<std::string> { return v == "X" ? std::optional<std::string>("taken") : std::nullopt; };
    h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("X");
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Enter, true); h.Frame();         // refused: hold + a re-arm queued for the next draw
    h.drawName = false;                                      // the row vanishes before the re-arm lands
    io.AddKeyEvent(ImGuiKey_Enter, false); h.Frame(); h.Frame();
    h.drawName = true;                                       // back: the hold has expired
    for (int i = 0; i < 6; ++i) h.Frame();
    CHECK_FALSE(ImGui::IsAnyItemActive());                   // the box never took focus on its own
    CHECK(h.commits == 0);
    CHECK(h.name == "Alpha");
    bool showsModel = false;
    for (const auto& [id, d] : h.state.textDrafts)
    {
        CHECK_FALSE(d.active);
        CHECK(d.text != "X");                                // the refused text is gone
        showsModel = showsModel || d.text == "Alpha";
    }
    CHECK(showsModel);
}

TEST_CASE("PropertyGrid: a second Enter on a refused TextRow value, with no new typing, still keeps it and re-arms (D4, final review)", "[editor][inspector]")
{
    GridHarness h;
    h.validate = [](std::string_view v) -> std::optional<std::string> { return v == "X" ? std::optional<std::string>("taken") : std::nullopt; };
    h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("X");
    h.Key(ImGuiKey_Enter);                                   // refused: kept + re-armed
    h.Frame(); h.Frame();
    REQUIRE(h.state.textDrafts.size() == 1);
    REQUIRE(h.state.textDrafts.begin()->second.text == "X");
    REQUIRE(h.state.textDrafts.begin()->second.active);
    h.Key(ImGuiKey_Enter);                                   // again, no typing: this activation has no edit
    h.Frame(); h.Frame(); h.Frame();
    CHECK(h.commits == 0);
    CHECK(h.name == "Alpha");
    REQUIRE(h.state.textDrafts.size() == 1);
    CHECK(h.state.textDrafts.begin()->second.text == "X");      // still kept
    CHECK(h.state.textDrafts.begin()->second.active);           // still re-armed
}

TEST_CASE("PropertyGrid: a refused TextRow value draws in Theme::kError while typed and while held after Enter, a valid or committed value never does, and the style stacks balance", "[editor][inspector]")
{
    GridHarness h;
    h.validate = [](std::string_view v) -> std::optional<std::string> { return v == "X" ? std::optional<std::string>("taken") : std::nullopt; };
    h.Frame();
    CHECK_FALSE(h.errorDrawn);                               // the model's own value: no red
    h.Click(h.Centre("Name"));
    CHECK_FALSE(h.errorDrawn);                               // active, unchanged: no red
    h.Type("X"); h.Frame();                                  // the look trails the keystroke by one frame
    CHECK(h.errorDrawn);                                     // typing a refused value: red
    h.Key(ImGuiKey_Enter);                                   // refused: kept + re-armed (the hold)
    h.Frame(); h.Frame();
    REQUIRE(h.state.textDrafts.size() == 1);
    REQUIRE(h.state.textDrafts.begin()->second.text == "X");
    CHECK(h.errorDrawn);                                     // held after the refused Enter: still red
    h.Type("Y"); h.Frame();                                  // re-armed select-all: "Y" replaces "X"
    CHECK_FALSE(h.errorDrawn);                               // a valid value: no red
    h.Click(ImVec2(600, 900));
    h.Frame();
    CHECK(h.commits == 1);
    CHECK(h.name == "Y");
    CHECK_FALSE(h.errorDrawn);                               // committed: no red

    h.Click(h.Centre("Name"));
    h.Type("X"); h.Frame();
    CHECK(h.errorDrawn);
    h.Click(ImVec2(600, 900));                               // focus loss with a refused value: revert
    h.Frame();
    CHECK(h.name == "Y");
    CHECK_FALSE(h.errorDrawn);                               // reverted: no red
    CHECK(h.nameStackDrift == 0);                            // every push popped inside the row, every frame
}

// ===========================================================================
// node-page spec s4.1: the row set's second half. RowHarness is GridHarness's
// shape (software atlas, window pinned at the origin, probe centres) with
// the rows supplied PER CASE through `body`. It can also wrap the window in an
// EditGesture::ScopeGuard (T2-A7's companion case).
// ===========================================================================
namespace
{
    struct RowHarness
    {
        Arcane::Editor::PropertyGridState state;
        std::unordered_map<std::string, ImVec2> probe;
        std::function<void(Arcane::Editor::PropertyGrid&)> body;
        float width = 640.0f;
        // Non-null: a ScopeGuard is the first local of the window scope, so it
        // closes last (EditGesture.hpp:263-285).
        Arcane::CommandStack* guardStack = nullptr;
        Arcane::Editor::EditGesture::GestureState* guardState = nullptr;
        Arcane::Editor::EditGesture::GestureState unguarded;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;

        RowHarness()
        {
            IMGUI_CHECKVERSION();
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            state.probe = &probe;
        }
        ~RowHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }

        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            Arcane::Editor::PropertyGrid(state).CommitOrphans();
            {
                Arcane::Editor::EditGesture::ScopeGuard guard{ guardStack, guardState ? *guardState : unguarded };
                ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(width, 1000), ImGuiCond_Always);
                ImGui::Begin("Inspector");
                Arcane::Editor::PropertyGrid grid(state);
                {
                    Arcane::Editor::PropertyGrid::Rows rows(grid, "##fields");
                    if (rows && body) body(grid);
                }
                ImGui::End();
            }
            ImGui::Render();
        }
        ImVec2 Centre(const std::string& key) { INFO(key); REQUIRE(probe.count(key) == 1); return probe.at(key); }
        // ForTooltip = Stationary + DelayShort (style.HoverFlagsForTooltipMouse): ~0.3 s still.
        void Hover(ImVec2 at, int frames = 40) { ImGui::GetIO().AddMousePosEvent(at.x, at.y); for (int i = 0; i < frames; ++i) Frame(); }
        void Press(ImVec2 at) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(at.x, at.y); Frame(); io.AddMouseButtonEvent(0, true); Frame(); }
        void MoveTo(ImVec2 at) { ImGui::GetIO().AddMousePosEvent(at.x, at.y); Frame(); }
        void Release() { ImGui::GetIO().AddMouseButtonEvent(0, false); Frame(); }
        void Click(ImVec2 at) { Press(at); Release(); }
        // Press, three equal moves (each past the 3 px drag threshold), release.
        void Drag(ImVec2 at, float dx) { Press(at); for (int i = 1; i <= 3; ++i) MoveTo(ImVec2(at.x + dx * i / 3.0f, at.y)); Release(); }
        // Escape DOWN lands on this frame; the key-up rides the next one.
        void Escape() { ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); Frame(); ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); }
        void Type(const char* s) { ImGui::GetIO().AddInputCharactersUTF8(s); Frame(); }
        void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }
        static bool TooltipShown() { ImGuiWindow* w = ImGui::FindWindowByName("##Tooltip_00"); return w && w->Active; }
    };
}

TEST_CASE("FieldLabelCell: a label wider than its column is ellipsized, reports truncated, and tooltips on hover", "[editor][inspector]")
{
    RowHarness h;
    h.width = 392.0f;   // the Inspector 2 width the asset page fits (s5.6)
    const std::string longLabel = "Angular damping applied while the body is resting underwater";
    REQUIRE(longLabel.size() == 60);
    bool longCut = false, shortCut = true;
    ImVec2 lo{}, hi{}, shortLo{}, shortHi{};
    h.body = [&](Arcane::Editor::PropertyGrid&)
    {
        (void)Arcane::Editor::FieldLabelCell(longLabel, false, &longCut);
        lo = ImGui::GetItemRectMin(); hi = ImGui::GetItemRectMax();   // still the label: the cell adds no item after it
        ImGui::TextUnformatted("value");
        (void)Arcane::Editor::FieldLabelCell("Mass", false, &shortCut);
        shortLo = ImGui::GetItemRectMin(); shortHi = ImGui::GetItemRectMax();
        ImGui::TextUnformatted("1.0");
    };
    h.Frame();
    CHECK(longCut);
    CHECK_FALSE(shortCut);
    CHECK(hi.x - lo.x < 392.0f * 0.5f);   // the full label is ~400 px; the cut one fits the 40% column
    h.Hover(ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f));
    CHECK(RowHarness::TooltipShown());
    h.Hover(ImVec2((shortLo.x + shortHi.x) * 0.5f, (shortLo.y + shortHi.y) * 0.5f));
    CHECK_FALSE(RowHarness::TooltipShown());   // an uncut label has nothing to reveal
}

TEST_CASE("PropertyGrid: ReadOnlyRow ellipsizes a long value and tooltips the full text on hover", "[editor][inspector]")
{
    RowHarness h;
    h.width = 392.0f;
    const std::string path = "/Game/Content/Textures/Environment/Foliage/pine_bark_albedo.png";
    h.body = [&](Arcane::Editor::PropertyGrid& g) { g.ReadOnlyRow("Path", path); g.ReadOnlyRow("Kind", "Texture"); };
    h.Frame();
    h.Hover(h.Centre("Path"));
    CHECK(RowHarness::TooltipShown());
    h.Hover(h.Centre("Kind"));
    CHECK_FALSE(RowHarness::TooltipShown());
}

using Arcane::Editor::PropertyGrid;

TEST_CASE("PropertyGrid: VecRow follows a 3-frame drag every frame and commits once with the final value", "[editor][inspector]")
{
    RowHarness h;
    float v[3]{ 1.0f, 2.0f, 3.0f };
    int commits = 0;
    h.body = [&](PropertyGrid& g) { if (g.VecRow("Offset", v, 3)) ++commits; };
    h.Frame();
    const ImVec2 c = h.Centre("Offset");   // a 3-wide group's centre sits inside component 1 (y)
    h.Press(c);
    float last = v[1];
    for (int i = 1; i <= 3; ++i)
    {
        h.MoveTo(ImVec2(c.x + 15.0f * i, c.y));
        CHECK(v[1] > last);                 // write-through: `v` follows the gesture every frame
        last = v[1];
    }
    CHECK(commits == 0);
    h.Release();
    CHECK(commits == 1);
    CHECK(v[1] > 2.0f + 0.25f);             // the whole gesture, not one frame's delta
    CHECK(v[0] == 1.0f);
    CHECK(v[2] == 3.0f);
    h.Frame();
    CHECK(commits == 1);
    CHECK(h.state.numericDrafts.empty());
}

TEST_CASE("PropertyGrid: VecRow released on its seed commits nothing", "[editor][inspector]")
{
    RowHarness h;
    float v[3]{ 1.0f, 2.0f, 3.0f };
    int commits = 0;
    h.body = [&](PropertyGrid& g) { if (g.VecRow("Offset", v, 3)) ++commits; };
    h.Frame();
    const ImVec2 c = h.Centre("Offset");
    h.Press(c);
    h.MoveTo(ImVec2(c.x + 30.0f, c.y));
    REQUIRE(v[1] != 2.0f);
    h.MoveTo(c);                             // back: "%.3f" rounding lands exactly on 2.000
    h.Release();
    CHECK(v[1] == 2.0f);
    CHECK(commits == 0);
    h.Click(c);                              // a pure click: no edit, no commit
    CHECK(commits == 0);
}

TEST_CASE("PropertyGrid: Escape mid-drag on a VecRow restores every component, returns false and reports cancelled", "[editor][inspector]")
{
    RowHarness h;
    float v[3]{ 1.0f, 2.0f, 3.0f };
    int commits = 0, cancels = 0;
    bool returnedOnCancel = true;
    h.body = [&](PropertyGrid& g)
    {
        const bool committed = g.VecRow("Offset", v, 3);
        if (committed) ++commits;
        if (g.LastRowEvents().cancelled) { ++cancels; returnedOnCancel = committed; }
    };
    h.Frame();
    const ImVec2 c = h.Centre("Offset");
    h.Press(c);
    h.MoveTo(ImVec2(c.x + 40.0f, c.y));
    h.Frame();
    REQUIRE(v[1] != 2.0f);
    h.Escape();
    h.Release();
    h.Frame();
    CHECK(v[0] == 1.0f);
    CHECK(v[1] == 2.0f);
    CHECK(v[2] == 3.0f);
    CHECK(commits == 0);
    CHECK(cancels == 1);                     // exactly the Escape frame
    CHECK_FALSE(returnedOnCancel);
    CHECK(h.state.numericDrafts.empty());
}

TEST_CASE("PropertyGrid: a ranged VecRow clamps a Ctrl+click typed component and leaves the others alone", "[editor][inspector]")
{
    RowHarness h;
    float v[3]{ 0.2f, 0.3f, 0.4f };
    int commits = 0;
    h.body = [&](PropertyGrid& g) { if (g.VecRow("Offset", v, 3, 0.01f, Astra::Range(0.0, 1.0))) ++commits; };
    h.Frame();
    const ImVec2 c = h.Centre("Offset");                  // the 3-wide group's centre: component 1 (y)
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(c.x, c.y); h.Frame();
    Arcane::TestKeys::AddKeyEvent(io, ImGuiMod_Ctrl, true); h.Frame();
    io.AddMouseButtonEvent(0, true); h.Frame();           // Ctrl+click: that component's temp text input
    io.AddMouseButtonEvent(0, false); h.Frame();
    io.AddKeyEvent(ImGuiKey_A, true); h.Frame();
    io.AddKeyEvent(ImGuiKey_A, false); Arcane::TestKeys::AddKeyEvent(io, ImGuiMod_Ctrl, false); h.Frame();
    h.Type("5");
    h.Key(ImGuiKey_Enter);
    CHECK(commits == 1);
    CHECK(v[1] == 1.0f);                                   // ClampOnInput, per component
    CHECK(v[0] == 0.2f);
    CHECK(v[2] == 0.4f);
}

TEST_CASE("PropertyGrid: a ranged FloatRow clamps a Ctrl+click typed 5 to 1 and commits it", "[editor][inspector]")
{
    RowHarness h;
    float v = 0.5f;
    int commits = 0;
    h.body = [&](PropertyGrid& g) { if (g.FloatRow("Metallic", v, 0.01f, Astra::Range(0.0, 1.0))) ++commits; };
    h.Frame();
    const ImVec2 c = h.Centre("Metallic");
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(c.x, c.y); h.Frame();
    Arcane::TestKeys::AddKeyEvent(io, ImGuiMod_Ctrl, true); h.Frame();
    io.AddMouseButtonEvent(0, true); h.Frame();      // Ctrl+click: DragScalar enters its temp text input
    io.AddMouseButtonEvent(0, false); h.Frame();
    io.AddKeyEvent(ImGuiKey_A, true); h.Frame();     // Ctrl+A on top of TempInputScalar's AutoSelectAll (imgui_widgets.cpp:3828)
    io.AddKeyEvent(ImGuiKey_A, false); Arcane::TestKeys::AddKeyEvent(io, ImGuiMod_Ctrl, false); h.Frame();
    h.Type("5");
    h.Key(ImGuiKey_Enter);
    CHECK(commits == 1);
    CHECK(v == 1.0f);                                // ClampOnInput: typed input obeys the range
}

TEST_CASE("PropertyGrid: FloatRow with \"%.0f\" commits an integral value", "[editor][inspector]")
{
    RowHarness h;
    float v = 3.0f;
    int commits = 0;
    h.body = [&](PropertyGrid& g) { if (g.FloatRow("Pixels", v, 1.0f, std::nullopt, "%.0f")) ++commits; };
    h.Frame();
    h.Drag(h.Centre("Pixels"), 45.0f);
    CHECK(commits == 1);
    CHECK(v > 3.0f);
    CHECK(v == std::round(v));                       // DragBehavior rounds to the format's precision
}

TEST_CASE("PropertyGrid: an unranged IntRow keeps its step buttons; a ranged one drags and clamps", "[editor][inspector]")
{
    RowHarness h;
    int priority = 5, px = 10, pxCommits = 0;
    ImVec2 lo{}, hi{};
    float frameH = 0.0f;
    h.body = [&](PropertyGrid& g)
    {
        (void)g.IntRow("Priority", priority);
        lo = ImGui::GetItemRectMin(); hi = ImGui::GetItemRectMax();   // InputScalar's group: text + "-" + "+"
        frameH = ImGui::GetFrameHeight();
        if (g.IntRow("Pixels", px, Astra::Range(0.0, 64.0))) ++pxCommits;
    };
    h.Frame();
    h.Click(ImVec2(hi.x - frameH * 0.5f, (lo.y + hi.y) * 0.5f));   // the right-most square button is "+"
    CHECK(priority == 6);
    h.Drag(h.Centre("Pixels"), 300.0f);
    CHECK(px == 64);                                 // DragInt clamps at the range max
    CHECK(pxCommits == 1);
}

TEST_CASE("PropertyGrid: SliderRow commits once per gesture", "[editor][inspector]")
{
    RowHarness h;
    float rough = 0.5f;
    int commits = 0;
    h.body = [&](PropertyGrid& g) { if (g.SliderRow("Roughness", rough, 0.0f, 1.0f)) ++commits; };
    h.Frame();
    h.Drag(h.Centre("Roughness"), 45.0f);            // pressing ON the grab does not jump (SliderGrabClickOffset)
    CHECK(rough > 0.5f);
    CHECK(commits == 1);
    h.Frame();
    CHECK(commits == 1);
}

TEST_CASE("PropertyGrid: ColorRow box drag commits once; one popup session commits once on close, original latched at open", "[editor][inspector]")
{
    RowHarness h;
    float tint[4]{ 0.25f, 0.25f, 0.25f, 1.0f };
    int commits = 0;
    ImGuiID popupId = 0;
    h.body = [&](PropertyGrid& g) { if (g.ColorRow("Tint", tint, &popupId)) ++commits; };
    h.Frame();

    // The four linear boxes are the probed (last) item. Four equal boxes put the
    // group centre on the 1|2 gap; 12 px left is inside box 1 (G).
    const ImVec2 boxes = h.Centre("Tint");
    h.Drag(ImVec2(boxes.x - 12.0f, boxes.y), 45.0f);
    CHECK(commits == 1);
    CHECK(tint[1] > 0.25f + 0.1f);                     // ColorEdit4's float speed: 1/255 per px
    CHECK(tint[0] == 0.25f);
    CHECK(tint[2] == 0.25f);
    CHECK(tint[3] == 1.0f);
    h.Frame();
    CHECK(commits == 1);

    const float atOpen[4]{ tint[0], tint[1], tint[2], tint[3] };
    CHECK(h.Centre("Tint#swatch").x < boxes.x);        // the swatch sits LEFT of the boxes
    h.Click(h.Centre("Tint#swatch"));
    REQUIRE(popupId != 0);
    REQUIRE(ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None));
    for (int i = 0; i < 4; ++i) CHECK(h.state.colorOriginal[i] == atOpen[i]);
    // The popup body writes through into the row's storage every frame; a write
    // to that storage while it is open stands in for a picker drag.
    tint[0] = 0.8f;
    h.Frame(); h.Frame();
    CHECK(commits == 1);                                // open: nothing committed yet
    h.Click(ImVec2(1200.0f, 1000.0f));                  // outside every window: the popup closes
    CHECK_FALSE(ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None));
    CHECK(commits == 2);                                // one commit for the whole session
    CHECK(tint[0] == 0.8f);
    h.Frame();
    CHECK(commits == 2);
}

TEST_CASE("PropertyGrid: an hdr ColorRow's box drag keeps a component above 1; a plain one clamps it", "[editor][inspector]")
{
    // T2-A4 carry ruling (s4.1(c) step 3 clarification): `hdr` also ORs
    // ImGuiColorEditFlags_HDR into the linear boxes, so a leftward drag on a
    // component above 1 does not snap it into 0..1.
    for (const bool hdr : { true, false })
    {
        INFO("hdr = " << hdr);
        RowHarness h;
        float glow[4]{ 0.5f, 2.0f, 0.5f, 1.0f };
        int commits = 0;
        h.body = [&](PropertyGrid& g) { if (g.ColorRow("Glow", glow, nullptr, hdr)) ++commits; };
        h.Frame();
        const ImVec2 boxes = h.Centre("Glow");
        h.Drag(ImVec2(boxes.x - 12.0f, boxes.y), -45.0f);   // box 1 (G), leftward
        CHECK(commits == 1);
        if (hdr) { CHECK(glow[1] > 1.0f); CHECK(glow[1] < 2.0f); }
        else     CHECK(glow[1] <= 1.0f);
        CHECK(glow[0] == 0.5f);
        CHECK(glow[2] == 0.5f);
    }
}

TEST_CASE("PropertyGrid: a ColorRow too narrow for \"0.000\" shows its boxes at two decimals -- same ids, full precision, one commit", "[editor][inspector]")
{
    // T3 gate (finding 3): ColorEdit4 prints "%0.3f" whatever the box width, so
    // the 1080p material page clipped "0.350" to "0.35C". A row whose boxes
    // cannot hold "0.000" draws the same boxes at the decimals that fit.
    struct Seen { std::string text; ImGuiID pressedId = 0; int commits = 0; float tint[4]{}; };
    const auto run = [](float width)
    {
        Seen s;
        float tint[4]{ 0.3456f, 0.25f, 0.6544f, 1.0f };
        RowHarness h;
        h.width = width;
        h.body = [&](PropertyGrid& g)
        {
            ImGui::LogToBuffer();
            if (g.ColorRow("Tint", tint)) ++s.commits;
            s.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
        };
        h.Frame(); h.Frame();
        const std::string shown = s.text;
        const ImVec2 boxes = h.Centre("Tint");
        h.Press(ImVec2(boxes.x - 6.0f, boxes.y));             // box 1 (G): 6 px left of the 1|2 gap
        s.pressedId = ImGui::GetCurrentContext()->ActiveId;
        for (int i = 1; i <= 3; ++i) h.MoveTo(ImVec2(boxes.x - 6.0f + 15.0f * i, boxes.y));
        h.Release();
        s.text = shown;
        for (int i = 0; i < 4; ++i) s.tint[i] = tint[i];
        return s;
    };
    const Seen wide = run(640.0f);
    const Seen narrow = run(350.0f);                       // boxes fit "0.00", not "0.000" (ProggyClean 13 px)
    INFO("wide: " << wide.text << " | narrow: " << narrow.text);
    CHECK(wide.text.find("0.346") != std::string::npos);      // the stock ColorEdit4: three decimals
    CHECK(narrow.text.find("0.35") != std::string::npos);     // narrow: two...
    CHECK(narrow.text.find("0.346") == std::string::npos);    // ...never the clipped three
    CHECK(narrow.text.find("0.65") != std::string::npos);
    REQUIRE(wide.pressedId != 0);
    CHECK(narrow.pressedId == wide.pressedId);                // the same box id as ColorEdit4's "##Y"
    for (const Seen* s : { &wide, &narrow })
    {
        CHECK(s->commits == 1);                               // one gesture, one commit
        CHECK(s->tint[1] > 0.25f + 0.1f);                     // 1/255 per px, as ColorEdit4
        CHECK(s->tint[0] == 0.3456f);                         // display-only rounding: storage untouched
        CHECK(s->tint[2] == 0.6544f);
        CHECK(s->tint[3] == 1.0f);
    }
}

using Arcane::Editor::RowDecor;

namespace
{
    // The value rows the EditGesture-after-row contract covers (s4.1(f)).
    enum class RowKind { Int, Float, Slider, Vec, Color };
    constexpr RowKind kRowKinds[]{ RowKind::Int, RowKind::Float, RowKind::Slider, RowKind::Vec, RowKind::Color };
    const char* KindName(RowKind k)
    {
        switch (k) { case RowKind::Int: return "IntRow"; case RowKind::Float: return "FloatRow";
                     case RowKind::Slider: return "SliderRow"; case RowKind::Vec: return "VecRow"; default: return "ColorRow"; }
    }
    // One row of `kind`, labelled "Row", over `v`: Int reads v[0] in HUNDREDTHS
    // (0.5 <-> 50, ranged 0..1000 -> DragInt) so the non-integral seed survives
    // the int round trip exactly, Slider is 0..1, Vec uses 3 components, Color all 4.
    bool DrawKind(PropertyGrid& g, RowKind k, std::array<float, 4>& v)
    {
        switch (k)
        {
            case RowKind::Int:
            {
                int i = static_cast<int>(std::lround(v[0] * 100.0f));   // <cmath>, added in T2-A3
                const bool c = g.IntRow("Row", i, Astra::Range(0.0, 1000.0));
                v[0] = static_cast<float>(i) / 100.0f;
                return c;
            }
            case RowKind::Float:  return g.FloatRow("Row", v[0]);
            case RowKind::Slider: return g.SliderRow("Row", v[0], 0.0f, 1.0f);
            case RowKind::Vec:    return g.VecRow("Row", v.data(), 3);
            default:              return g.ColorRow("Row", v.data());
        }
    }
    // Where to press: the probed value centre, except ColorRow's 4-box group,
    // whose centre is the 1|2 gap (12 px left = box 1).
    ImVec2 ValueTarget(RowKind k, ImVec2 c) { return k == RowKind::Color ? ImVec2(c.x - 12.0f, c.y) : c; }
}

TEST_CASE("PropertyGrid: an inherited row disables its value; ticking the override raises overrideToggled once", "[editor][inspector]")
{
    RowHarness h;
    bool overridden = false;
    float rough = 0.5f;
    int commits = 0, toggles = 0;
    bool disabled = false;
    h.body = [&](PropertyGrid& g)
    {
        g.SetNextRowDecor(RowDecor{ &overridden });
        if (g.FloatRow("Roughness", rough)) ++commits;
        disabled = (ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0;
        if (g.LastRowEvents().overrideToggled) ++toggles;
    };
    h.Frame();
    CHECK(disabled);                                   // inherited: dimmed and read-only (Inspector #9)
    h.Drag(h.Centre("Roughness"), 45.0f);
    CHECK(rough == 0.5f);
    CHECK(commits == 0);
    h.Click(h.Centre("Roughness#override"));
    CHECK(overridden);
    CHECK(toggles == 1);
    h.Frame();
    CHECK(toggles == 1);                               // one-shot: raised on the toggle frame only
    CHECK_FALSE(disabled);
    h.Drag(h.Centre("Roughness"), 45.0f);
    CHECK(rough > 0.5f);
    CHECK(commits == 1);
}

TEST_CASE("PropertyGrid: the reset slot is reserved at default and drawn when changed; a click raises resetClicked", "[editor][inspector]")
{
    RowHarness h;
    float a = 0.7f, b = 0.5f;
    int resets = 0;
    float wChanged = 0.0f, wDefault = 0.0f;
    h.body = [&](PropertyGrid& g)
    {
        g.SetNextRowDecor(RowDecor{ nullptr, true, true });
        (void)g.FloatRow("Changed", a);
        wChanged = ImGui::GetItemRectSize().x;
        if (g.LastRowEvents().resetClicked) ++resets;
        g.SetNextRowDecor(RowDecor{ nullptr, true, false });
        (void)g.FloatRow("Default", b);
        wDefault = ImGui::GetItemRectSize().x;
    };
    h.Frame();
    CHECK(h.probe.count("Changed#reset") == 1);
    CHECK(h.probe.count("Default#reset") == 0);        // at default the slot is empty...
    CHECK(wChanged > 0.0f);
    CHECK(wChanged == wDefault);                       // ...but reserved: values stay aligned (9.28)
    h.Click(h.Centre("Changed#reset"));
    CHECK(resets == 1);
    h.Frame();
    CHECK(resets == 1);
}

TEST_CASE("PropertyGrid: an override row's long label is cut after the checkbox and tooltips the full name", "[editor][inspector]")
{
    RowHarness h;
    h.width = 392.0f;
    const std::string longLabel = "Angular damping applied while the body is resting underwater";
    bool overridden = true;
    float v = 0.5f;
    float fh = 0.0f;
    h.body = [&](PropertyGrid& g)
    {
        g.SetNextRowDecor(RowDecor{ &overridden });
        (void)g.FloatRow(longLabel.c_str(), v);
        fh = ImGui::GetFrameHeight();
    };
    h.Frame();
    const ImVec2 box = h.Centre(longLabel + "#override");
    const ImVec2 onLabel(box.x + fh + 20.0f, box.y);    // past the square checkbox, inside the cut name
    CHECK(onLabel.x < h.Centre(longLabel).x);
    h.Hover(onLabel);
    CHECK(RowHarness::TooltipShown());                  // FieldLabelText: the cut name is one hover away
    CHECK(overridden);                                  // hovering the name never toggles the override
}

TEST_CASE("PropertyGrid: after a decorated row the VALUE widget is LastItemData, for every row type", "[editor][inspector]")
{
    for (const RowKind kind : kRowKinds)
        for (const bool viaOverride : { false, true })
        {
            INFO(KindName(kind) << (viaOverride ? " + override" : " + reset"));
            RowHarness h;
            std::array<float, 4> v{ 0.5f, 0.5f, 0.5f, 1.0f };
            bool overridden = true;
            bool activated = false;
            ImGuiID idAfterRow = 0, activeAfterRow = 0;
            h.body = [&](PropertyGrid& g)
            {
                g.SetNextRowDecor(viaOverride ? RowDecor{ &overridden } : RowDecor{ nullptr, true, true });
                (void)DrawKind(g, kind, v);
                if (ImGui::IsItemActivated())          // exactly what EditGesture::BeginOnActivate asks
                {
                    activated = true;
                    idAfterRow = ImGui::GetItemID();
                    activeAfterRow = ImGui::GetActiveID();
                }
            };
            h.Frame();
            h.Press(ValueTarget(kind, h.Centre("Row")));
            CHECK(activated);                          // a trailing decoration would have stolen LastItemData
            CHECK(idAfterRow == activeAfterRow);
            h.Release();
        }
}

TEST_CASE("PropertyGrid: a RowDecor lead draws first in the value cell; the value keeps LastItemData and the rest of the cell; ReadOnlyRow takes a lead too",
          "[editor][inspector]")
{
    RowHarness h;
    h.width = 392.0f;
    float a = 0.5f, b = 0.5f, c = 0.5f;
    float plainW = 0.0f, leadW = 0.0f, leadResetW = 0.0f, leadRight = 0.0f, valueLeft = 0.0f;
    bool activated = false;
    std::string log;
    h.body = [&](PropertyGrid& g)
    {
        ImGui::LogToBuffer();
        (void)g.FloatRow("Plain", a);
        plainW = ImGui::GetItemRectSize().x;
        RowDecor lead;
        lead.lead = [&] { ImGui::TextUnformatted("float2"); leadRight = ImGui::GetItemRectMax().x; return std::string(); };
        g.SetNextRowDecor(lead);
        (void)g.FloatRow("Lead", b);
        leadW = ImGui::GetItemRectSize().x;
        valueLeft = ImGui::GetItemRectMin().x;
        if (ImGui::IsItemActivated())
            activated = true;
        RowDecor leadReset;
        leadReset.lead = [] { ImGui::TextUnformatted("float2"); return std::string(); };
        leadReset.reset = true;
        leadReset.resetActive = true;
        g.SetNextRowDecor(leadReset);
        (void)g.FloatRow("LeadReset", c);
        leadResetW = ImGui::GetItemRectSize().x;
        RowDecor readOnly;
        readOnly.lead = [] { ImGui::TextUnformatted("float4"); return std::string(); };
        g.SetNextRowDecor(readOnly);
        g.ReadOnlyRow("Out", "-> Output.color");
        log = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    h.Frame();
    CHECK(valueLeft > leadRight);                      // the lead first, then the value
    CHECK(leadW > 0.0f);
    CHECK(leadW < plainW);                             // the value takes the cell left after the lead
    CHECK(leadResetW < leadW);                         // ...and the reset slot still reserves its own
    CHECK(h.probe.count("LeadReset#reset") == 1);
    INFO(log);
    CHECK(log.find("| Lead | float2 { 0.50 }") != std::string::npos);
    CHECK(log.find("| Out | float4 -> Output.color") != std::string::npos);   // lead, then the value, in one cell
    h.Press(h.Centre("Lead"));                         // the probe is the VALUE: LastItemData survived the lead
    CHECK(activated);
    h.Release();
}

namespace
{
    // `head`, then only whitespace, then `tail`: a two-line tooltip as the log
    // records it (a tooltip's text is logged too; its lines split on '\n').
    bool LeadsWith(const std::string& log, const std::string& head, const std::string& tail)
    {
        const std::size_t at = log.find(head);
        if (at == std::string::npos) return false;
        const std::size_t next = log.find_first_not_of(" \r\n", at + head.size());
        return next != std::string::npos && log.compare(next, tail.size(), tail) == 0;
    }
}

TEST_CASE("PropertyGrid: a lead that folds its text away hands it to the cell's hover tooltip -- a ReadOnlyRow's tooltip leads with it even when uncut, a value row tooltips it",
          "[editor][inspector]")
{
    // T3-D2: the node page's narrow-cell type dot. The lead returns what it did
    // NOT draw; the cell's hover tooltip carries it (first line), so nothing
    // the row means is lost.
    RowHarness h;
    float v = 0.5f;
    std::string log;
    h.body = [&](PropertyGrid& g)
    {
        ImGui::LogToBuffer();
        RowDecor folded;
        folded.lead = [] { ImGui::Dummy(ImVec2(8.0f, 8.0f)); return std::string("float4"); };
        g.SetNextRowDecor(folded);
        g.ReadOnlyRow("Out", "-> Output.color");   // short: never cut at 640 px
        RowDecor value;
        value.lead = [] { ImGui::Dummy(ImVec2(8.0f, 8.0f)); return std::string("float2"); };
        g.SetNextRowDecor(value);
        (void)g.FloatRow("In", v);
        RowDecor shown;
        shown.lead = [] { ImGui::TextUnformatted("float"); return std::string(); };
        g.SetNextRowDecor(shown);
        g.ReadOnlyRow("Whole", "-> Add.a");        // folded nothing, cut nothing: no tooltip
        log = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    h.Frame();
    {
        INFO(log);
        CHECK(log.find("| Out | -> Output.color") != std::string::npos);   // the folded word is not in the row
        CHECK(log.find("float4") == std::string::npos);
        CHECK(log.find("float2") == std::string::npos);
    }
    h.Hover(h.Centre("Out"));
    CHECK(RowHarness::TooltipShown());
    {
        INFO(log);
        CHECK(LeadsWith(log, "float4", "-> Output.color"));   // the type first, then the whole row text
    }
    h.Hover(h.Centre("In"));
    CHECK(RowHarness::TooltipShown());
    {
        INFO(log);
        CHECK(log.find("float2") != std::string::npos);
    }
    h.Hover(h.Centre("Whole"));
    CHECK_FALSE(RowHarness::TooltipShown());
}

TEST_CASE("PropertyGrid: a decorated scalar row's GetItemID() is its own ##value id", "[editor][inspector]")
{
    RowHarness h;
    float f = 0.7f, s = 0.5f;
    int i = 3;
    bool b = false;
    const char* items[]{ "A", "B" };
    std::vector<std::pair<ImGuiID, ImGuiID>> got;      // (GetItemID after the row, the value widget's id)
    const auto valueId = [](const char* label) { ImGui::PushID(label); const ImGuiID id = ImGui::GetID("##value"); ImGui::PopID(); return id; };
    h.body = [&](PropertyGrid& g)
    {
        got.clear();
        const RowDecor reset{ nullptr, true, true };
        g.SetNextRowDecor(reset); (void)g.FloatRow("F", f);                        { const ImGuiID id = ImGui::GetItemID(); got.emplace_back(id, valueId("F")); }
        g.SetNextRowDecor(reset); (void)g.IntRow("I", i, Astra::Range(0.0, 10.0)); { const ImGuiID id = ImGui::GetItemID(); got.emplace_back(id, valueId("I")); }
        g.SetNextRowDecor(reset); (void)g.SliderRow("S", s, 0.0f, 1.0f);           { const ImGuiID id = ImGui::GetItemID(); got.emplace_back(id, valueId("S")); }
        g.SetNextRowDecor(reset); (void)g.CheckboxRow("B", b);                     { const ImGuiID id = ImGui::GetItemID(); got.emplace_back(id, valueId("B")); }
        g.SetNextRowDecor(reset); (void)g.ComboRow("C", items, 2, 0);              { const ImGuiID id = ImGui::GetItemID(); got.emplace_back(id, valueId("C")); }
    };
    h.Frame();
    REQUIRE(got.size() == 5);
    for (const auto& [after, want] : got)
    {
        CHECK(after != 0);
        CHECK(after == want);
    }
}

namespace
{
    namespace EG = Arcane::Editor::EditGesture;

    // The "counting builder"'s product: one step over a 4-float document.
    struct SetModelCommand final : Arcane::ICommand
    {
        std::array<float, 4>* model;
        std::array<float, 4> before, after;
        SetModelCommand(std::array<float, 4>* m, std::array<float, 4> b, std::array<float, 4> a)
            : model(m), before(b), after(a) {}
        void Undo() override { *model = before; }
        void Redo() override { *model = after; }
        const char* Label() const override { return "Edit Row"; }
    };

    enum class CloseMode { EndAfterRow, EndOnDeactivateOnly };

    // A live-preview page over ONE row (s4.1(f)): `model` is the document (it
    // changes only through a step), `live` is the preview target (it follows the
    // row every frame, the Escape seed included). The builder is parked at
    // activation and runs once at close, with the page's before == after guard.
    struct ContractRig
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return reg; } };
        EG::GestureState gesture;
        const std::array<float, 4> initial{ 0.5f, 0.5f, 0.5f, 1.0f };
        std::array<float, 4> model = initial;
        std::array<float, 4> live = initial;
        int built = 0;
        bool sawCancel = false, slotsEmptyAtCancel = false, parkedAtCancel = false;

        void Body(PropertyGrid& g, RowKind kind, CloseMode mode)
        {
            std::array<float, 4> v = model;                 // re-derived every frame, as pages do
            (void)DrawKind(g, kind, v);                     // the gesture, not the row's bool, owns the step
            if (live != v) live = v;                        // (2) live write-through
            EG::BeginOnActivate(&stack, gesture, [] { return std::string("Edit Row"); },
                [this]() -> std::function<void()>
                {
                    const std::array<float, 4> before = model;
                    return [this, before]
                    {
                        ++built;
                        if (live == before) return;         // before == after: no step (R2)
                        stack.Push(std::make_unique<SetModelCommand>(&model, before, live));
                        model = live;
                    };
                });
            const bool cancelled = g.LastRowEvents().cancelled;
            if (mode == CloseMode::EndAfterRow) EG::EndAfterRow(&stack, gesture, cancelled);   // (3)
            else                                EG::EndOnDeactivate(&stack, gesture);
            if (cancelled)
            {
                sawCancel = true;
                slotsEmptyAtCancel = gesture.slots.item == 0 && gesture.slots.txn == Arcane::TransactionId::None;
                parkedAtCancel = gesture.slots.item != 0;
            }
        }
    };
}

TEST_CASE("EditGesture-after-row contract: one step per drag and none for a pure click, for every row type", "[editor][inspector]")
{
    for (const RowKind kind : kRowKinds)
    {
        INFO(KindName(kind));
        {
            ContractRig rig;
            RowHarness h;
            h.body = [&](PropertyGrid& g) { rig.Body(g, kind, CloseMode::EndAfterRow); };
            h.Frame();
            h.Drag(ValueTarget(kind, h.Centre("Row")), 45.0f);
            h.Frame();
            CHECK(rig.model != rig.initial);
            CHECK(rig.built == 1);
            REQUIRE(rig.stack.CanUndo());
            rig.stack.Undo();
            CHECK(rig.model == rig.initial);
            CHECK_FALSE(rig.stack.CanUndo());               // exactly one step
            CHECK_FALSE(rig.stack.InTransaction());
        }
        {
            ContractRig rig;
            RowHarness h;
            h.body = [&](PropertyGrid& g) { rig.Body(g, kind, CloseMode::EndAfterRow); };
            h.Frame();
            h.Click(ValueTarget(kind, h.Centre("Row")));    // Slider: pressing ON the grab does not move it
            h.Frame();
            CHECK(rig.model == rig.initial);
            CHECK_FALSE(rig.stack.CanUndo());
            CHECK_FALSE(rig.stack.InTransaction());         // a pure click cancels, nothing stranded
        }
    }
}

TEST_CASE("EditGesture-after-row contract: Escape with a live target restores the seed, adds no step, and EndAfterRow empties the slots on the Escape frame", "[editor][inspector]")
{
    for (const RowKind kind : kRowKinds)
    {
        INFO(KindName(kind));
        ContractRig rig;
        RowHarness h;
        h.body = [&](PropertyGrid& g) { rig.Body(g, kind, CloseMode::EndAfterRow); };
        h.Frame();
        const ImVec2 at = ValueTarget(kind, h.Centre("Row"));
        h.Press(at);
        h.MoveTo(ImVec2(at.x + 40.0f, at.y));
        h.Frame();
        REQUIRE(rig.live != rig.model);                     // previewing the in-flight drag
        h.Escape();
        CHECK(rig.sawCancel);
        CHECK(rig.slotsEmptyAtCancel);                      // closed AT the row, before any later row
        CHECK(rig.live == rig.model);                       // the restored seed re-applied to the preview
        h.Release();
        h.Frame();
        CHECK_FALSE(rig.stack.CanUndo());
        CHECK_FALSE(rig.stack.InTransaction());
    }
}

TEST_CASE("EditGesture-after-row contract: on a grouped row EndOnDeactivate alone leaves an Escape parked until the ScopeGuard", "[editor][inspector]")
{
    for (const RowKind kind : { RowKind::Vec, RowKind::Color })
    {
        INFO(KindName(kind));
        ContractRig rig;
        RowHarness h;
        h.guardStack = &rig.stack;
        h.guardState = &rig.gesture;
        h.body = [&](PropertyGrid& g) { rig.Body(g, kind, CloseMode::EndOnDeactivateOnly); };
        h.Frame();
        const ImVec2 at = ValueTarget(kind, h.Centre("Row"));
        h.Press(at);
        h.MoveTo(ImVec2(at.x + 40.0f, at.y));
        h.Frame();
        h.Escape();
        CHECK(rig.sawCancel);
        CHECK(rig.parkedAtCancel);                          // EndGroup set HasDeactivated before the clear (imgui.cpp:12494-12497)
        CHECK(rig.gesture.slots.item == 0);                 // the ScopeGuard closed it at scope end, same frame
        CHECK(rig.live == rig.model);
        h.Release();
        h.Frame();
        CHECK_FALSE(rig.stack.CanUndo());                   // ClosePending's commit saw before == after
        CHECK_FALSE(rig.stack.InTransaction());
    }
}

TEST_CASE("PropertyGrid::Section with a trailing control: right-aligned on the band; clicks reach it, not the header", "[editor][inspector]")
{
    Arcane::Editor::PropertyGridState state;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* px = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    bool only = false, open = false;
    ImVec2 lo{}, hi{};
    float workMaxX = 0.0f;
    const auto frame = [&]
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
        ImGui::Begin("Inspector");
        Arcane::Editor::PropertyGrid grid(state);
        open = grid.Section("Parameters", true, [&]
        {
            ImGui::Checkbox("Only overridden", &only);
            lo = ImGui::GetItemRectMin(); hi = ImGui::GetItemRectMax();
        });
        workMaxX = ImGui::GetCurrentWindow()->WorkRect.Max.x;
        ImGui::End();
        ImGui::Render();
    };
    frame(); frame();                                           // frame 2 places it by frame 1's measured width
    CHECK(open);
    CHECK(hi.x <= workMaxX + 0.5f);
    CHECK(hi.x >= workMaxX - ImGui::GetStyle().FramePadding.x - 1.0f);
    const ImVec2 at((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    io.AddMousePosEvent(at.x, at.y); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false); frame();
    CHECK(only);
    CHECK(open);                                                // AllowOverlap: the header did not toggle
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}
