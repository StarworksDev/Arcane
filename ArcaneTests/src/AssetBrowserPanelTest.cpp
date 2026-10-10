// Node page + editor upgrades, T5 s7.10 (Task T5-B1): the Asset Browser's key
// routing. FoldEntityClipboardShortcuts is header-inline (EditorPanels.hpp),
// so the fold is unit-tested here without EditorPanels.cpp. The ownership
// cases drive the REAL DrawAssetBrowserPanel over a real Arcane::Project
// through device-less ImGui frames (AssetInspectorSourceTest.cpp's shape).
// BrowserHarness is shared by the later T5-B Browser tasks (rename,
// multi-select, Duplicate, Delete), which extend it in place.

#include <catch2/catch_test_macros.hpp>

#include "Documents/DocumentHost.hpp"
#include "Input/EditorActions.hpp"
#include "Panels/AssetBrowserPanel.hpp"
#include "Panels/AssetPanelModel.hpp"
#include "Panels/EditorPanels.hpp"

#include <Arcane/Project/Project.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // ActivateItemByID, FindWindowByName, GImGui

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include "ImGuiTestKeys.hpp"   // TestKeys::AddKeyEvent: Ctrl as this platform's user presses it

using namespace Arcane::Editor; namespace fs = std::filesystem;
namespace
{
    // Real project + device-less ImGui (AssetInspectorSourceTest.cpp's FrameContext shape).
    struct BrowserHarness
    {
        fs::path root; std::optional<Arcane::Project> project; AssetPanelModel model; AssetBrowserPanelState state; DocumentHost docs;
        AssetPanelServices services; ImGuiContext* prev = ImGui::GetCurrentContext(); ImGuiContext* ctx = nullptr;
        std::optional<AssetOpRequest> lastFileOp;   // the latest fileOp any Frame() raised (Click() drops its frames' actions)
        explicit BrowserHarness(const char* n, int pngs = 1) : root(fs::temp_directory_path() / n)
        {
            std::error_code ec; fs::remove_all(root, ec); REQUIRE(Arcane::Project::Create(root, "B").has_value());
            for (int i = 0; i < pngs; ++i) { char f[16]; std::snprintf(f, 16, "a%02d.png", i); std::ofstream(root / "Content" / f) << "px"; }
            project = Arcane::Project::Open(root); REQUIRE(project); (void)model.RebuildIfDirty(&project->Registry(), {});
            ctx = ImGui::CreateContext(); ImGui::SetCurrentContext(ctx); ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280, 1024); io.IniFilename = nullptr; unsigned char* px; int w, h; io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        }
        ~BrowserHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); std::error_code ec; fs::remove_all(root, ec); }
        AssetPanelActions Frame(bool focus = false, ImGuiID activate = 0)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f; ImGui::NewFrame(); if (activate) ImGui::ActivateItemByID(activate);
            ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(800, 300)); if (focus) ImGui::SetNextWindowFocus();
            AssetPanelActions a = DrawAssetBrowserPanel(state, model, &*project, docs, services);
            ImGui::Render(); (void)model.RebuildIfDirty(&project->Registry(), {}); if (a.fileOp) lastFileOp = a.fileOp; return a;
        }
        static ImGuiWindow* RowsWindow()   // the deepest child of "Asset Browser": the table's scrolling inner window
        {
            ImGuiWindow* top = ImGui::FindWindowByName("Asset Browser"); ImGuiWindow* best = nullptr; int bd = 0;
            for (ImGuiWindow* w : GImGui->Windows) { if (w == top || w->RootWindow != top) continue; int d = 0;
                for (ImGuiWindow* p = w; p != top; p = p->ParentWindow) ++d; if (d > bd) { best = w; bd = d; } }
            REQUIRE(best); return best;
        }
        ImVec2 RowCenter(int i) const   // Rows()[i] under the frozen 24 px header
        { const ImGuiWindow* w = RowsWindow(); return ImVec2(w->Pos.x + 60, w->Pos.y + TableRowHeight() * (i + 1.5f)); }
        // A left-button drag from `a` to `b` in eight mouse steps, one frame each.
        void Drag(ImVec2 a, ImVec2 b)
        {
            ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(a.x, a.y); Frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); Frame();
            for (int s = 1; s <= 8; ++s) { io.AddMousePosEvent(a.x + (b.x - a.x) * s / 8.0f, a.y + (b.y - a.y) * s / 8.0f); Frame(); }
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); Frame(); Frame();
        }
        void Mods(ImGuiKeyChord m, bool d)
        { if (m & ImGuiMod_Ctrl) Arcane::TestKeys::AddKeyEvent(ImGui::GetIO(), ImGuiMod_Ctrl, d); if (m & ImGuiMod_Shift) ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, d); }
        void Click(int row, ImGuiMouseButton b, ImGuiKeyChord m = 0)
        {
            ImGuiIO& io = ImGui::GetIO(); Mods(m, true); if (m) Frame();   // the mods get their own frame: a key change trickles the mouse move to the press frame, too late to hover an AllowOverlap row
            const ImVec2 p = RowCenter(row); io.AddMousePosEvent(p.x, p.y); Frame();
            io.AddMouseButtonEvent(b, true); Frame(); io.AddMouseButtonEvent(b, false); Frame(); Mods(m, false); Frame();
        }
        // Sweeps the mouse down the open row menu until `label`'s item is hovered (a
        // disabled item still sets HoveredId); false when no line of the menu is it.
        bool HoverMenuItem(const char* label)
        {
            REQUIRE_FALSE(GImGui->OpenPopupStack.empty());
            ImGuiWindow* menu = GImGui->OpenPopupStack.back().Window; REQUIRE(menu);
            const ImGuiID id = menu->GetID(label); const ImVec2 pos = menu->Pos, size = menu->Size;
            for (float y = pos.y + 2.0f; y < pos.y + size.y; y += 2.0f)
            { ImGui::GetIO().AddMousePosEvent(pos.x + 30.0f, y); (void)Frame(); if (GImGui->HoveredId == id) return true; }
            return false;
        }
        static bool TooltipShown() { const ImGuiWindow* t = ImGui::FindWindowByName("##Tooltip_00"); return t && t->Active; }
        AssetPanelActions Key(ImGuiKey k, ImGuiKeyChord m = 0)
        { Mods(m, true); ImGui::GetIO().AddKeyEvent(k, true); auto a = Frame(); ImGui::GetIO().AddKeyEvent(k, false); Mods(m, false); Frame(); return a; }
    };
}
TEST_CASE("FoldEntityClipboardShortcuts folds none while the Browser owns the keys, all four otherwise", "[editor][assetops]")
{
    MenuRequests owned, free;
    FoldEntityClipboardShortcuts(owned, { true, true, true, true }, true);
    FoldEntityClipboardShortcuts(free, { true, true, true, true }, false);
    CHECK_FALSE((owned.cutSelection || owned.copySelection || owned.paste || owned.duplicateSelection));
    CHECK((free.cutSelection && free.copySelection && free.paste && free.duplicateSelection));
}
TEST_CASE("Asset Browser owns the edit keys only when focused, with no popup and no text field", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_keys_test"); (void)h.Frame(true);
    CHECK(h.Frame().ownsEditKeys);
    SECTION("row context menu") { h.Click(1, ImGuiMouseButton_Right); CHECK(h.model.selected.IsValid()); CHECK_FALSE(h.Frame().ownsEditKeys); }
    SECTION("search box")
    { (void)h.Frame(false, ImGui::FindWindowByName("Asset Browser")->GetID("##assetssearch")); (void)h.Frame(); CHECK_FALSE(h.Frame().ownsEditKeys); }
}
TEST_CASE("Asset Browser F2 opens the inline rename; Enter commits a Rename; Esc reverts", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_rename_test"); (void)h.Frame(true);
    const Arcane::Guid g = h.model.Rows()[1].guid; h.model.Select(g); (void)h.Key(ImGuiKey_F2);
    REQUIRE(h.state.renameTarget == g);
    CHECK((std::string(h.state.renameBuf) == "a00" && !h.Frame().ownsEditKeys));   // stem only; the box owns the keys
    SECTION("Enter commits")
    {
        ImGui::GetIO().AddInputCharactersUTF8("wall"); (void)h.Frame();   // typed into the active box (AutoSelectAll replaces "a00")
        const AssetPanelActions a = h.Key(ImGuiKey_Enter);
        REQUIRE(a.fileOp); CHECK((a.fileOp->kind == AssetOpKind::Rename && a.fileOp->newStem == "wall" && !h.state.renameTarget.IsValid()));
    }
    SECTION("Esc reverts") { CHECK_FALSE(h.Key(ImGuiKey_Escape).fileOp); CHECK_FALSE(h.state.renameTarget.IsValid()); }
}
TEST_CASE("Asset Browser: a rebound assets.rename opens rename on its new key, F2 no longer", "[editor][assetops][shortcuts]")
{
    Arcane::Editor::EditorActions& keys = Arcane::Editor::EditorActions::Get();
    Arcane::CVarRegistry& reg = keys.Registry();
    reg.Set(keys.HandleOf("assets.rename"), Arcane::CVarValue::String("F3"), Arcane::SetBy::EditorUser, "editor", Arcane::CVarContext::Editor);
    reg.PublishImmediate();
    keys.RefreshBindings();
    BrowserHarness h("arcane_browser_rebound_rename_test"); (void)h.Frame(true);
    const Arcane::Guid g = h.model.Rows()[1].guid; h.model.Select(g);
    (void)h.Key(ImGuiKey_F2);
    CHECK_FALSE(h.state.renameTarget.IsValid());

    (void)h.Key(ImGuiKey_F3);
    CHECK(h.state.renameTarget == g);
    reg.RevertLayer(Arcane::SetBy::EditorUser);
    reg.PublishImmediate();
    keys.RefreshBindings();
}
TEST_CASE("Asset Browser Ctrl+D raises a Duplicate and keeps the keys from the entity clipboard", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_dup_test"); (void)h.Frame(true); h.model.Select(h.model.Rows()[1].guid);
    const AssetPanelActions a = h.Key(ImGuiKey_D, ImGuiMod_Ctrl);
    REQUIRE(a.fileOp); CHECK((a.fileOp->kind == AssetOpKind::Duplicate && a.fileOp->guids == std::vector<Arcane::Guid>{ h.model.selected } && a.ownsEditKeys));
}
TEST_CASE("Asset Browser Del requests a delete confirm; with a row menu open Del requests nothing", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_del_test"); (void)h.Frame(true); h.model.Select(h.model.Rows()[1].guid);
    CHECK(h.Key(ImGuiKey_Delete).requestDelete == std::vector<Arcane::Guid>{ h.model.selected });
    h.Click(1, ImGuiMouseButton_Right); CHECK(h.Key(ImGuiKey_Delete).requestDelete.empty());
}
TEST_CASE("Asset Browser multi-select: Ctrl-click then Shift-click over a clipped list; Ctrl+A never selects a group row", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_multiselect_test", 40); (void)h.Frame(true);   // 41 rows in a 300 px window: clipped
    const auto& rows = h.model.Rows(); REQUIRE(rows[0].type == AssetPanelRow::Type::Group);
    h.Click(1, ImGuiMouseButton_Left); h.Click(3, ImGuiMouseButton_Left, ImGuiMod_Ctrl);
    CHECK((h.model.SelectionCount() == 2 && h.model.selected == rows[3].guid));
    h.Click(5, ImGuiMouseButton_Left, ImGuiMod_Shift);   // range from the Ctrl-click source
    CHECK((h.model.SelectionCount() == 3 && h.model.InSelection(rows[4].guid) && !h.model.InSelection(rows[1].guid)));
    (void)h.Key(ImGuiKey_A, ImGuiMod_Ctrl); CHECK(h.model.SelectionCount() == 40);   // the adapter skips the group row
}
// s7.9 / s7.13 check 4: box selection. The scope sits inside the table's
// ScrollY inner window under the table's own ID, so a box started from the
// void must still put the scope in the nav focus route (T5-GATE fix round 1).
TEST_CASE("Asset Browser box selection: a drag from the void or from a row selects the rows it crosses", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_boxselect_test", 4); (void)h.Frame(true);   // 5 rows: void below them
    const auto& rows = h.model.Rows(); REQUIRE(rows.size() == 5);
    const ImGuiWindow* w = BrowserHarness::RowsWindow();
    const ImVec2 voidPt(w->Pos.x + 60, w->Pos.y + TableRowHeight() * 6.0f + 12.0f);   // under the last row's bottom (6 x 24 px)
    REQUIRE(w->InnerRect.Contains(voidPt));
    SECTION("from the void, upward over rows 2..4")
    {
        h.Drag(voidPt, h.RowCenter(2));
        CHECK(h.model.SelectionCount() == 3);
        CHECK((h.model.InSelection(rows[2].guid) && h.model.InSelection(rows[3].guid) && h.model.InSelection(rows[4].guid)));
        CHECK_FALSE(h.model.InSelection(rows[1].guid));
    }
    SECTION("from row 1, downward over rows 1..3")
    {
        h.Drag(h.RowCenter(1), h.RowCenter(3));
        CHECK(h.model.SelectionCount() == 3);
        CHECK((h.model.InSelection(rows[1].guid) && h.model.InSelection(rows[2].guid) && h.model.InSelection(rows[3].guid)));
        CHECK_FALSE(h.model.InSelection(rows[4].guid));
    }
    SECTION("from the void after a row click, the old selection clears first")
    {
        h.Click(1, ImGuiMouseButton_Left); REQUIRE(h.model.SelectionCount() == 1);
        h.Drag(voidPt, h.RowCenter(3));
        CHECK(h.model.SelectionCount() == 2);
        CHECK((h.model.InSelection(rows[3].guid) && h.model.InSelection(rows[4].guid) && !h.model.InSelection(rows[1].guid)));
    }
}
TEST_CASE("Asset Browser batch keys: Del and Ctrl+D carry the whole selection; F2 needs exactly one", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_batch_keys_test", 3); (void)h.Frame(true);
    const auto& rows = h.model.Rows();
    h.Click(1, ImGuiMouseButton_Left); h.Click(2, ImGuiMouseButton_Left, ImGuiMod_Ctrl);
    const std::vector<Arcane::Guid> sel = h.model.selection; REQUIRE(sel.size() == 2);
    CHECK(h.Key(ImGuiKey_Delete).requestDelete == sel);
    const AssetPanelActions d = h.Key(ImGuiKey_D, ImGuiMod_Ctrl);
    REQUIRE(d.fileOp); CHECK((d.fileOp->kind == AssetOpKind::Duplicate && d.fileOp->guids == sel));
    (void)h.Key(ImGuiKey_F2); CHECK_FALSE(h.state.renameTarget.IsValid());
    h.Click(1, ImGuiMouseButton_Right);   // inside the selection: the set stays, the primary moves
    CHECK((h.model.selection == sel && h.model.selected == rows[1].guid));
}
// T5-B8 review (owed at T5-GATE): DrawRenameBox's refusal branches and the row menu's
// Rename verb, with a host dry-run (services.fileOpRefusal) in place.
TEST_CASE("Asset Browser rename: a refused Enter keeps the box with its reason; a valid Enter then commits", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_rename_refused_test");
    h.services.fileOpRefusal = [](const AssetOpRequest& r) { return r.kind == AssetOpKind::Rename && r.newStem == "bad" ? std::string("A file named bad.png already exists") : std::string{}; };
    (void)h.Frame(true);
    const Arcane::Guid g = h.model.Rows()[1].guid; h.model.Select(g); (void)h.Key(ImGuiKey_F2);
    REQUIRE(h.state.renameTarget == g); (void)h.Frame();             // the box activates two frames after F2
    ImGui::GetIO().AddInputCharactersUTF8("bad"); (void)h.Frame();
    CHECK(BrowserHarness::TooltipShown());                        // the refusal shows while the box is active
    CHECK_FALSE(h.Key(ImGuiKey_Enter).fileOp);
    CHECK(h.state.renameTarget == g);                             // refused: the box stays, the name is kept
    CHECK(std::string(h.state.renameBuf) == "bad");
    (void)h.Frame(); (void)h.Frame();                             // the box takes focus again (select-all)
    ImGui::GetIO().AddInputCharactersUTF8("good"); (void)h.Frame();
    const AssetPanelActions a = h.Key(ImGuiKey_Enter);
    REQUIRE(a.fileOp); CHECK((a.fileOp->newStem == "good" && !h.state.renameTarget.IsValid()));
}
TEST_CASE("Asset Browser rename: a click away after an edit commits; without an edit it cancels", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_rename_clickaway_test", 2);
    h.services.fileOpRefusal = [](const AssetOpRequest&) { return std::string{}; };
    (void)h.Frame(true);
    const Arcane::Guid g = h.model.Rows()[1].guid; h.model.Select(g); (void)h.Key(ImGuiKey_F2);
    REQUIRE(h.state.renameTarget == g); (void)h.Frame();             // the box activates two frames after F2
    SECTION("edited")
    {
        ImGui::GetIO().AddInputCharactersUTF8("wall"); (void)h.Frame();
        h.Click(2, ImGuiMouseButton_Left);
        REQUIRE(h.lastFileOp);
        CHECK((h.lastFileOp->kind == AssetOpKind::Rename && h.lastFileOp->guids == std::vector<Arcane::Guid>{ g } && h.lastFileOp->newStem == "wall"));
    }
    SECTION("untouched") { h.Click(2, ImGuiMouseButton_Left); CHECK_FALSE(h.lastFileOp); }
    CHECK_FALSE(h.state.renameTarget.IsValid());
}
TEST_CASE("Asset Browser row menu: Rename opens the inline box; refused, it is disabled with its reason", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_rename_menu_test");
    std::string refusal;
    h.services.fileOpRefusal = [&](const AssetOpRequest& r) { return r.kind == AssetOpKind::Rename ? refusal : std::string{}; };
    (void)h.Frame(true);
    const Arcane::Guid g = h.model.Rows()[1].guid;
    SECTION("allowed: a click on the verb opens the box on the row")
    {
        h.Click(1, ImGuiMouseButton_Right); REQUIRE(h.state.menuRefusal.rename.empty());
        REQUIRE(h.HoverMenuItem("Rename"));
        CHECK_FALSE(GImGui->HoveredIdIsDisabled);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true); (void)h.Frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false); (void)h.Frame();
        CHECK(h.state.renameTarget == g);
        CHECK(std::string(h.state.renameBuf) == "a00");
    }
    SECTION("refused: disabled, its tooltip carries the reason, a click does nothing")
    {
        refusal = "Stop play mode first";
        h.Click(1, ImGuiMouseButton_Right); CHECK(h.state.menuRefusal.rename == refusal);
        REQUIRE(h.HoverMenuItem("Rename"));
        CHECK(GImGui->HoveredIdIsDisabled);
        (void)h.Frame(); CHECK(BrowserHarness::TooltipShown());
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true); (void)h.Frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false); (void)h.Frame();
        CHECK_FALSE(h.state.renameTarget.IsValid());
    }
}
