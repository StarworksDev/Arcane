// InspectorHost (inspector-ownership spec s5): the PURE routing -- which
// source's page an Inspector instance shows -- driven with fake sources, no
// ImGui. Every rule in spec s3.1/s3.3/s6 has a case here. The ImGui cases
// (the instance list's ini section, one device-less frame of the windows)
// run on bare contexts, no device.
#include <catch2/catch_test_macros.hpp>
#include <Panels/DefaultLayout.hpp>
#include <Panels/InspectorHost.hpp>
#include <Panels/InspectorKinds.hpp>     // kInspectorKinds / kInspectorAllIcon (the filter face)
#include <Panels/InspectorWindows.hpp>   // RegisterInspectorInstancesSettings
#include <Settings/InspectorSettings.hpp>  // editor.inspector.historyDepth (the ring's cap)
#include <Input/EditorActions.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Widgets/IconsLucide.h>
#include <imgui.h>
#include <imgui_internal.h>   // ClearIniSettings (the windowed switch's reset)
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ImGuiTestKeys.hpp"   // TestKeys::AddKeyEvent: Ctrl as this platform's user presses it

using namespace Arcane::Editor;

namespace
{
    struct FakePage final : InspectorPage
    {
        std::vector<InspectorCrumb> crumbs;
        int rows = 0;    // s5.7: > 0 draws that many lines (a page taller than its window)
        int draws = 0;   // Draw calls, collapsed and refused-Begin frames included
        std::vector<InspectorCrumb> Breadcrumb() const override { return crumbs; }
        void Draw(PropertyGrid&) override
        {
            ++draws;
            for (int i = 0; i < rows; ++i) ImGui::Text("row %d", i);
        }
    };
    struct FakeSource final : InspectorSource
    {
        std::string name;
        std::string kind;
        std::string key;
        bool restoreOk = true;
        bool hasPage = true;                // false: Page() is null (an asset source with nothing selected)
        std::string normalizeTo;            // non-empty: RestoreSelection lands on THIS key, not the asked one (a member died in between)
        std::vector<std::string> restored;
        FakePage page;
        explicit FakeSource(std::string n, std::string k = "scene") : name(std::move(n)), kind(std::move(k)) {}
        std::string SourceName() const override { return name; }
        std::string_view Kind() const override { return kind; }
        InspectorPage* Page() override { return hasPage ? &page : nullptr; }
        InspectorPage* PageFor(std::string_view) override { return restoreOk ? &page : nullptr; }
        std::string SelectionKey() const override { return key; }
        bool RestoreSelection(std::string_view k) override
        {
            if (!restoreOk) return false;
            key = normalizeTo.empty() ? std::string(k) : normalizeTo;
            restored.push_back(key);
            return true;
        }
        bool Resolves(std::string_view) const override { return restoreOk; }
    };
    struct World
    {
        FakeSource scene{ "Scene", "scene" }, doc{ "Player.arcinput", "input-actions" }, other{ "brick.png", "assets" }, doc2{ "Menu.arcinput", "input-actions" };
        InspectorHost host{ scene };
        World() { host.AddSource(doc); host.AddSource(other); host.AddSource(doc2); }
        void Select(FakeSource& s, std::string k) { s.key = std::move(k); host.NotifySelected(s); }
    };
}

TEST_CASE("InspectorHost: the last-selecting source wins; nothing else moves it", "[editor][inspector]")
{
    World w;
    CHECK(&w.host.Current() == &w.scene);            // fallback at rest
    w.Select(w.doc, "m/a//");
    CHECK(&w.host.Current() == &w.doc);
    w.Select(w.scene, "7");
    CHECK(&w.host.Current() == &w.scene);
    // Focus is not selection: touching the other source's page, adding an
    // instance, reading keys -- none of it is an event.
    (void)w.other.Page(); (void)w.host.AddInstance(); (void)w.host.SourceFor(0);
    CHECK(&w.host.Current() == &w.scene);
    // A deselect (empty key) is not a selection event either.
    w.Select(w.doc, "");
    CHECK(&w.host.Current() == &w.scene);
    // A re-select of the scene's UNCHANGED key is still a selection event
    // (UE: SelectActor re-notifies the Details views even when the set did
    // not change): doc selects, scene re-selects "7" -> scene wins again, and
    // history gains a scene entry so Back returns to the document.
    w.Select(w.doc, "m/a//");
    CHECK(&w.host.Current() == &w.doc);
    w.Select(w.scene, "7");
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.host.CanGoBack());
    REQUIRE(w.host.GoBack());
    CHECK(&w.host.Current() == &w.doc);
}

TEST_CASE("SelectionEdge: a selection event is a gesture (epoch moved) with a non-empty key", "[editor][inspector]")
{
    SelectionEdge edge;
    CHECK(edge.Observe(1, "7"));        // first gesture
    CHECK_FALSE(edge.Observe(1, "7"));  // nothing happened (focus, idle)
    CHECK_FALSE(edge.Observe(1, "9"));  // key moved WITHOUT a gesture (a prune re-primaried): not an event
    CHECK(edge.Observe(2, "7"));        // re-clicking the already-selected entity IS an event
    CHECK_FALSE(edge.Observe(3, ""));   // a clear moves the epoch but is never an event
    CHECK(edge.Observe(4, "7"));        // re-selecting after a clear is one
}

TEST_CASE("InspectorHost: pin holds a page while others select; a second instance follows", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "m/a//");
    w.host.SetPinned(0, true);
    const int second = w.host.AddInstance();
    w.Select(w.scene, "7");
    CHECK(w.host.SourceFor(0) == &w.doc);
    CHECK(w.host.Find(0)->pinnedKey == "m/a//");
    CHECK(w.host.SourceFor(second) == &w.scene);
    w.host.SetPinned(0, false);
    CHECK(w.host.SourceFor(0) == &w.scene);
    w.scene.restoreOk = false;                       // PageFor(key) is null: nothing to hold
    w.host.SetPinned(second, true);
    CHECK_FALSE(w.host.Find(second)->pinned);
    w.scene.restoreOk = true; w.scene.key.clear();   // an EMPTY key whose page resolves (a document's asset page) IS pinnable
    w.host.SetPinned(second, true);
    CHECK(w.host.Find(second)->pinned);
    CHECK(w.host.Find(second)->pinnedKey.empty());
    w.host.SetPinned(second, false);
    w.host.RemoveInstance(0);                        // refused: instance 0 is permanent
    CHECK(w.host.Instances().size() == 2);
    w.host.RemoveInstance(second);
    CHECK(w.host.Instances().size() == 1);
}

TEST_CASE("InspectorHost: a closed source releases the follower and flags the pin", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "m/a//");
    w.host.SetPinned(0, true);
    const int second = w.host.AddInstance();
    w.Select(w.doc, "m/b//");
    w.host.RemoveSource(w.doc);
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.host.SourceFor(second) == &w.scene);
    REQUIRE(w.host.Find(0)->pinned);
    CHECK(w.host.Find(0)->sourceClosed);
    CHECK(w.host.SourceFor(0) == nullptr);
    CHECK(w.host.Find(0)->pinnedName == "Player.arcinput");
    CHECK(w.host.History().empty());                 // every entry named the closed source
    w.host.SetPinned(0, false);
    CHECK(w.host.SourceFor(0) == &w.scene);
}

TEST_CASE("InspectorHost: history back/forward restores the selection in its source", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a");
    w.Select(w.doc, "b");
    w.Select(w.scene, "7");
    REQUIRE(w.host.History().size() == 3);
    CHECK(w.host.History()[2].label == "Scene");   // FakePage has no crumbs: the label is the source name
    CHECK(w.host.HistoryCursor() == 2);
    CHECK(w.host.CanGoBack());
    CHECK_FALSE(w.host.CanGoForward());
    REQUIRE(w.host.GoBack());
    CHECK(&w.host.Current() == &w.doc);
    CHECK(w.doc.restored.back() == "b");
    // The document re-reports the restored key next frame: the same entry
    // as the cursor, so nothing is pushed and forward history survives.
    w.host.NotifySelected(w.doc);
    CHECK(w.host.History().size() == 3);
    CHECK(w.host.CanGoForward());
    REQUIRE(w.host.GoBack());
    CHECK(w.doc.restored.back() == "a");
    REQUIRE(w.host.GoForward());
    CHECK(w.doc.restored.back() == "b");
    // A NEW selection at cursor 1 truncates the forward entries.
    w.Select(w.other, "n1");
    CHECK(w.host.History().size() == 3);
    CHECK_FALSE(w.host.CanGoForward());
}

TEST_CASE("InspectorHost: GoBack skips and prunes entries that no longer resolve", "[editor][inspector]")
{
    World w;
    w.Select(w.scene, "1");
    w.Select(w.doc, "a");
    w.Select(w.doc, "b");
    w.Select(w.scene, "7");
    w.doc.restoreOk = false;                         // the bindings were deleted
    REQUIRE(w.host.GoBack());
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.scene.restored.back() == "1");
    CHECK(w.host.History().size() == 2);             // both doc entries pruned
    w.scene.restoreOk = false;
    CHECK_FALSE(w.host.GoForward());                 // "7" unresolvable: pruned, nothing to land on
    CHECK(w.host.History().size() == 1);
}

TEST_CASE("InspectorHost: history is capped at 32 and never grows past it", "[editor][inspector]")
{
    World w;
    for (int i = 0; i < 40; ++i) w.Select(w.doc, "k" + std::to_string(i));
    const auto depth = static_cast<std::size_t>(Arcane::Settings<InspectorSettings>().historyDepth);   // 32
    CHECK(w.host.History().size() == depth);
    CHECK(w.host.History().front().key == "k8");
    CHECK(w.host.HistoryCursor() == depth - 1);
}

TEST_CASE("InspectorHost: ReleaseAll drops every non-fallback source, the history and the pins", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a");
    w.host.SetPinned(0, true);
    const int ids[] = { 2, 3, 3, 0, 99 }; w.host.SetInstanceIds(ids);   // {0,2,3}: 0 implicit, dup + out-of-range dropped
    CHECK(w.host.Instances().size() == 3);
    w.host.ReleaseAll();
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.host.History().empty());
    for (const auto& inst : w.host.Instances()) CHECK_FALSE(inst.pinned);
    CHECK(w.host.Instances().size() == 3);           // instances are layout, not project state
    w.host.NotifySelected(w.doc);                    // an unregistered source is ignored
    CHECK(&w.host.Current() == &w.scene);
}

TEST_CASE("InspectorHost: InvalidateSource prunes history and releases pins but keeps the source registered", "[editor][inspector]")
{
    World w;
    w.Select(w.scene, "1");
    w.host.SetPinned(0, true);
    w.Select(w.doc, "a");
    REQUIRE(w.host.History().size() == 2);
    w.host.InvalidateSource(w.scene);                // the registry was replaced
    CHECK(&w.host.Current() == &w.doc);              // current is untouched
    REQUIRE(w.host.History().size() == 1);
    CHECK(w.host.History()[0].key == "a");
    CHECK(w.host.HistoryCursor() == 0);
    REQUIRE(w.host.Find(0)->pinned);
    CHECK(w.host.SourceFor(0) == nullptr);           // "Pinned selection is gone"
    CHECK_FALSE(w.host.Find(0)->sourceClosed);
    w.host.NotifySelected(w.scene);                  // still registered: the next scene selection lands
    CHECK(&w.host.Current() == &w.scene);
    w.host.SetPinned(0, false);
    CHECK(w.host.SourceFor(0) == &w.scene);
}

TEST_CASE("InspectorHost: PruneStale drops entries that no longer resolve and CanGoBack turns false without a click", "[editor][inspector]")
{
    World w;
    w.Select(w.scene, "1"); w.Select(w.doc, "a"); w.Select(w.doc, "b"); w.Select(w.scene, "7");
    w.scene.restoreOk = false; w.doc.restoreOk = false;
    w.host.PruneStale();
    CHECK(w.host.History().empty());
    CHECK_FALSE(w.host.CanGoBack());
    CHECK_FALSE(w.host.CanGoForward());
    CHECK(w.scene.restored.empty());                 // nothing was selected by the prune
}

TEST_CASE("InspectorHost: pruning an earlier duplicate keeps the cursor on the same entry", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a"); w.Select(w.other, "x"); w.Select(w.doc, "b"); w.Select(w.doc, "a");
    REQUIRE(w.host.History().size() == 4);
    w.host.RemoveSource(w.other);
    REQUIRE(w.host.History().size() == 3);
    CHECK(w.host.HistoryCursor() == 2);
    CHECK(w.host.History()[2].key == "a");
    REQUIRE(w.host.GoBack());
    CHECK(w.doc.restored.back() == "b");             // a key search would have landed on index 0 and Back done nothing
}

TEST_CASE("InspectorHost: a restore that lands on a normalized key keeps forward history", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a,b,c"); w.Select(w.scene, "7");
    w.doc.normalizeTo = "a,c";                       // b died between push and restore
    REQUIRE(w.host.GoBack());
    CHECK(w.host.History()[0].key == "a,c");         // re-snapshotted
    w.host.NotifySelected(w.doc);                    // the next-frame re-report
    CHECK(w.host.History().size() == 2);             // an echo, not a new entry
    CHECK(w.host.CanGoForward());
    REQUIRE(w.host.GoForward());
    CHECK(w.scene.restored.back() == "7");
}

TEST_CASE("InspectorHost: JumpTo lands on the chosen entry or prunes only it; entries carry labels", "[editor][inspector]")
{
    World w;
    w.doc.page.crumbs = { { "Player.arcinput", {}, {} }, { "Player", {}, {} }, { "Jump", {}, {} } };
    w.Select(w.scene, "1"); w.Select(w.doc, "a"); w.Select(w.doc, "b"); w.Select(w.scene, "7");
    CHECK(w.host.History()[1].label == "Player.arcinput > Player > Jump");
    REQUIRE(w.host.JumpTo(1));
    CHECK(w.doc.restored.back() == "a");
    CHECK(w.host.HistoryCursor() == 1);
    CHECK(w.host.CanGoForward());
    CHECK(w.host.BackEntry() != nullptr); CHECK(w.host.BackEntry()->key == "1");
    w.doc.restoreOk = false;
    CHECK_FALSE(w.host.JumpTo(2));                   // that one entry is pruned, nothing else moves
    CHECK(w.host.History().size() == 3);
    CHECK(w.host.HistoryCursor() == 1);
    CHECK_FALSE(w.host.JumpTo(1));                   // index == cursor: no-op
    CHECK_FALSE(w.host.JumpTo(99));
}

TEST_CASE("InspectorHost: instance ids are a reusable pool of 8 slots, never minted", "[editor][inspector]")
{
    World w;
    CHECK(w.host.AddInstance() == 1);
    CHECK(w.host.AddInstance() == 2);
    w.host.RemoveInstance(1);
    CHECK(w.host.AddInstance() == 1);                // the freed slot, not 3
    for (int i = 0; i < 8; ++i) (void)w.host.AddInstance();
    CHECK(w.host.Instances().size() == static_cast<std::size_t>(w.host.MaxInstances()));
    CHECK(w.host.AddInstance() == -1);
    w.host.RemoveInstance(0);                        // still refused
    CHECK(w.host.Instances().size() == static_cast<std::size_t>(w.host.MaxInstances()));
    const int ids[] = { 0, 3, 3, 42, -1 };
    w.host.SetInstanceIds(ids);
    REQUIRE(w.host.Instances().size() == 2);
    CHECK(w.host.Instances()[1].id == 3);
}

TEST_CASE("InspectorHost: RepinKey moves only the pinned instance's key", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "m/a//");
    w.host.SetPinned(0, true);
    const auto historyBefore = w.host.History().size();
    w.host.RepinKey(0, "m///");
    CHECK(w.host.Find(0)->pinnedKey == "m///");
    CHECK(w.host.SourceFor(0) == &w.doc);
    CHECK(w.doc.key == "m/a//");                       // the source's selection did not move
    CHECK(w.host.History().size() == historyBefore);   // no history push
    w.host.SetPinned(0, false);
    w.host.RepinKey(0, "x");                           // unpinned: ignored
    CHECK(w.host.Find(0)->pinnedKey.empty());
}

// The [EditorInspector][Instances] ini section (arc-1 debt F): the handler
// moved beside InspectorWindows so it can be driven on a bare ImGui context
// (the ShaderEditorDocumentTest "material panel layout round-trips through
// imgui.ini" pattern). The clear half is what a WINDOWED project switch does
// (EditorApp::RetargetLayoutIni: ClearIniSettings, then the incoming file):
// a file without the section must leave exactly {0}, never the outgoing
// project's extra instances.
TEST_CASE("InspectorHost: the [EditorInspector][Instances] section round-trips, and a clear + reload without it resets to {0}", "[editor][inspector]")
{
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    // Restores the previous context even when a REQUIRE below throws.
    struct ContextGuard
    {
        ImGuiContext* ctx; ImGuiContext* prev;
        ~ContextGuard() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
    } guard{ ctx, prev };
    ImGui::SetCurrentContext(ctx);
    ImGui::GetIO().IniFilename = nullptr;   // never let a test touch a real ini
    FakeSource scene{ "Scene" };            // InspectorHost has no default ctor: it needs its fallback source
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    RegisterInspectorInstancesSettings(host);                           // idempotent
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=2,3\n");
    REQUIRE(host.Instances().size() == 3);
    const std::string saved = ImGui::SaveIniSettingsToMemory();
    CHECK(saved.find("[EditorInspector][Instances]\nIds=2,3") != std::string::npos);
    CHECK(saved.find("[EditorInspector]") == saved.rfind("[EditorInspector]"));   // one section
    ImGui::ClearIniSettings();                                          // the switch's reset
    ImGui::LoadIniSettingsFromMemory("[EditorPanels][Visibility]\nConsole=1\n");
    CHECK(host.Instances().size() == 1);
    CHECK(host.Instances()[0].id == 0);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=0,3,3,42,-1\n");
    CHECK(host.Instances().size() == 2);                                // {0,3}
}

// ---- Filters= persistence, the legacy upgrade, the default configuration (spec s6/s7) ----
namespace
{
    // One bare ImGui context per case (the existing ini case's shape).
    struct IniContext
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        IniContext() { ImGui::SetCurrentContext(ctx); ImGui::GetIO().IniFilename = nullptr; }
        ~IniContext() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
    };
}

TEST_CASE("InspectorHost ini: Filters= round-trips after Ids=; a Filters= line means no upgrade", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1,3\nFilters=0:assets,1:scene+input-actions+material+sprite+mesh\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(host.Find(1)->filter == InspectorFilter::Only("assets"));
    CHECK(host.Find(3)->filter.IsAll());
    const std::string saved = ImGui::SaveIniSettingsToMemory();
    CHECK(saved.find("Ids=1,3\nFilters=0:assets,1:scene+input-actions+material+sprite+mesh\n") != std::string::npos);
}

TEST_CASE("InspectorHost ini: garbage Filters= entries are sanitized, never thrown on", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory(
        "[EditorInspector][Instances]\nIds=2\nFilters=7:scene,x:assets,2,0:bogus+assets,,2:scene+assets+input-actions+material+sprite+mesh,\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());           // the line was present: not legacy
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));   // bogus dropped
    CHECK(host.Find(2)->filter.IsAll());                    // all-excluded -> All
    CHECK(host.Instances().size() == 2);                    // id 7 never created
}

TEST_CASE("InspectorHost ini: a layout without Filters= is flagged once for the legacy upgrade", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=\n");
    CHECK(host.TakeLegacyLayoutUpgrade());
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());            // consumed
    ImGui::LoadIniSettingsFromMemory("[EditorPanels][Visibility]\nConsole=1\n");   // no section at all: also legacy
    CHECK(host.TakeLegacyLayoutUpgrade());
    // A second load WITHOUT ClearIniSettings between: instances absent from
    // the Filters= line read as All (spec s7), survivors of Ids= included.
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1\nFilters=0:assets,1:scene\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    REQUIRE(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    REQUIRE(host.Find(1)->filter == InspectorFilter::AllBut("scene"));
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1\nFilters=\n");   // empty line: a real answer
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    CHECK(host.Find(0)->filter.IsAll());
    CHECK(host.Find(1)->filter.IsAll());
}

TEST_CASE("InspectorHost: the default and the legacy-upgrade configurations", "[editor][inspector]")
{
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    (void)host.AddInstance(); (void)host.AddInstance();     // {0,1,2}
    host.ApplyDefaultInspectorLayout();
    REQUIRE(host.Instances().size() == 2);
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(host.Find(InspectorHost::kAssetsInstanceId)->filter == InspectorFilter::Only("assets"));

    InspectorHost legacy{ scene };
    const int ids[] = { 1 };
    legacy.SetInstanceIds(ids);                             // the user already had "Inspector 2"
    const int assetsId = legacy.UpgradeLegacyInspectorLayout();
    CHECK(assetsId == 2);                                   // lowest FREE id; the user's 1 is untouched
    CHECK(legacy.Find(1)->filter.IsAll());
    CHECK(legacy.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(legacy.Find(2)->filter == InspectorFilter::Only("assets"));
}

TEST_CASE("InspectorWindowTitle: the filter label rides the title; the ### id never changes", "[editor][inspector]")
{
    InspectorHost::Instance main;                        // id 0, All
    CHECK(InspectorWindowTitle(main) == "Inspector###Inspector");
    // The stable id: every title of instance 0 hashes to the same window id,
    // and ImHashStr skips "###" (imgui.cpp:2539-2544), so that id IS the legacy
    // bare "Inspector" one -- true by construction; these CHECKs pin it.
    CHECK(ImHashStr("Inspector###Inspector") == ImHashStr("Inspector - Scene###Inspector"));
    CHECK(ImHashStr("Inspector###Inspector") == ImHashStr(kPrimaryInspectorWindowId));
    CHECK(ImHashStr(kPrimaryInspectorWindowId) == ImHashStr("Inspector"));   // [Window][Inspector] carries over
    main.filter = InspectorFilter::AllBut("assets");
    CHECK(InspectorWindowTitle(main) == "Inspector - All but Assets###Inspector");
    InspectorHost::Instance second; second.id = 1;
    CHECK(InspectorWindowTitle(second) == "Inspector 2###inspector_1");
    second.filter = InspectorFilter::Only("assets");
    CHECK(InspectorWindowTitle(second) == "Inspector 2 - Assets###inspector_1");
}

// The windows themselves, one device-less frame: the filtered title reaches
// the ImGui window, and an instance whose filter admits nothing registered
// draws its one-line note. The note text is probed through ImGui's own
// LogToBuffer (every RenderText is logged while it is on), read BEFORE
// Render: the log ends with the implicit Debug window at EndFrame.
TEST_CASE("DrawInspectorWindows: filtered titles and the no-source / no-selection lines", "[editor][inspector]")
{
    IniContext ic;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);   // the software atlas (InputActionsDocumentUiTest's shape)
    FakeSource scene{ "Scene", "scene" };
    scene.hasPage = false;                              // a routed source with nothing to show
    InspectorHost host{ scene };
    const int input = host.AddInstance();               // 1
    REQUIRE(input == 1);
    REQUIRE(host.SetFilter(input, InspectorFilter::Only("input-actions")));   // no input-actions source registered
    const int docs = host.AddInstance();                // 2: several kinds admitted, none registered
    InspectorFilter noSceneNoAssets;
    noSceneNoAssets.excluded = { "scene", "assets" };
    REQUIRE(host.SetFilter(docs, noSceneNoAssets));
    InspectorWindowsState state;

    ImGui::NewFrame();
    ImGui::LogToBuffer();
    (void)DrawInspectorWindows(host, state, nullptr);
    const std::string logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::Render();

    ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr("###inspector_1"));
    REQUIRE(w != nullptr);
    CHECK(std::string(w->Name) == "Inspector 2 - Input Actions###inspector_1");
    ImGuiWindow* primary = ImGui::FindWindowByName(kPrimaryInspectorWindowId);
    REQUIRE(primary != nullptr);
    CHECK(std::string(primary->Name) == "Inspector###Inspector");
    INFO(logged);
    CHECK(logged.find("No Input Actions document open") != std::string::npos);
    CHECK(logged.find("Nothing to show for this filter") != std::string::npos);
    CHECK(logged.find("No selection") != std::string::npos);   // instance 0: the scene is routed but has no page
}

// The filter dropdown's face and list (user request 2026-09-30): the face
// shows the ticked kinds' ICONS (no text; All = one glyph), its tooltip
// carries the text label, and every list row reads checkbox, icon, name.
// Probed through ImGui's LogToBuffer like the case above; the combo is found
// by its id (##filter under the window id) with a mouse sweep along the
// header row, then hovered (tooltip) and clicked (popup).
namespace
{
    std::string DrawLoggedFrame(InspectorHost& host, InspectorWindowsState& state)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(100.0f, 100.0f));    // the primary Inspector: the next Begin
        ImGui::SetNextWindowSize(ImVec2(480.0f, 320.0f));
        ImGui::LogToBuffer();
        (void)DrawInspectorWindows(host, state, nullptr);
        std::string logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        return logged;
    }
    std::size_t CountOf(const std::string& haystack, std::string_view needle)
    {
        std::size_t n = 0;
        for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) ++n;
        return n;
    }
}

TEST_CASE("DrawInspectorWindows: the filter face is icons, its tooltip the label, and list rows lead with the icon", "[editor][inspector]")
{
    IniContext ic;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    InspectorWindowsState state;

    // All: ONE "all" glyph, never the text "All" and never six icons.
    std::string logged = DrawLoggedFrame(host, state);
    INFO(logged);
    CHECK(CountOf(logged, kInspectorAllIcon) == 1);
    CHECK(logged.find("All") == std::string::npos);
    CHECK(logged.find(ICON_LC_CLAPPERBOARD) == std::string::npos);

    // All but Assets: the five other icons, catalog order; the label rides
    // only the window title (the face draws no text).
    REQUIRE(host.SetFilter(0, InspectorFilter::AllBut("assets")));
    logged = DrawLoggedFrame(host, state);
    CHECK(CountOf(logged, "All but Assets") == 1);   // the title
    CHECK(logged.find(ICON_LC_PACKAGE) == std::string::npos);
    std::size_t prev = 0;
    for (const char* icon : { ICON_LC_CLAPPERBOARD, ICON_LC_GAMEPAD_2, ICON_LC_PALETTE, ICON_LC_STICKER, ICON_LC_BOX })
    {
        const std::size_t at = logged.find(icon);
        REQUIRE(at != std::string::npos);
        CHECK(at > prev);    // strictly after the previous icon (the first sits after the title)
        prev = at;
    }

    // Find the face: sweep the mouse along the header row until the combo is hovered.
    ImGuiWindow* primary = ImGui::FindWindowByName(kPrimaryInspectorWindowId);
    REQUIRE(primary != nullptr);
    const ImGuiID comboId = ImHashStr("##filter", 0, primary->ID);
    const float rowY = primary->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f;
    bool found = false;
    for (float x = primary->DC.CursorStartPos.x; x < primary->Pos.x + primary->Size.x && !found; x += 4.0f)
    {
        io.AddMousePosEvent(x, rowY);
        (void)DrawLoggedFrame(host, state);
        found = ImGui::GetCurrentContext()->HoveredId == comboId;
    }
    REQUIRE(found);

    // Hover (stationary): the tooltip carries the full text label.
    bool tooltip = false;
    for (int frame = 0; frame < 120 && !tooltip; ++frame)
        tooltip = CountOf(DrawLoggedFrame(host, state), "All but Assets") == 2;   // the title + the tooltip
    CHECK(tooltip);

    // Click: the list opens, and each row is checkbox, icon, name.
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    std::string list = DrawLoggedFrame(host, state);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    list += DrawLoggedFrame(host, state);
    list += DrawLoggedFrame(host, state);
    INFO(list);
    for (const InspectorKind& k : kInspectorKinds)
    {
        const std::string row = std::string(k.icon) + " " + std::string(k.displayName);
        CHECK(list.find(row) != std::string::npos);
    }
    CHECK(list.find(std::string("[x] ") + ICON_LC_CLAPPERBOARD + " Scene") != std::string::npos);
    CHECK(list.find(std::string("[ ] ") + ICON_LC_PACKAGE + " Assets") != std::string::npos);
}

TEST_CASE("InspectorHost ini: ClearAllFn resets filters with the list", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1\nFilters=0:assets\n");
    ImGui::ClearIniSettings();
    CHECK(host.Instances().size() == 1);
    CHECK(host.Find(0)->filter.IsAll());
}

TEST_CASE("InspectorHost filters: a Scene instance keeps the scene while a document selects", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();                        // 1
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.scene, "A");
    w.Select(w.scene, "B");
    w.Select(w.doc, "Player/Jump");
    CHECK(w.host.SourceFor(0) == &w.doc);                          // All follows the binding
    CHECK(w.host.SourceFor(scene) == &w.scene);                    // Scene keeps B
    CHECK(w.scene.key == "B");
}

TEST_CASE("InspectorHost filters: a clear in one source never moves an instance routed elsewhere", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc, "Player/Jump");
    w.Select(w.scene, "7");
    w.Select(w.scene, "");                                         // empty-space click: a clear
    CHECK(w.host.SourceFor(input) == &w.doc);
    CHECK(w.doc.key == "Player/Jump");
    // An All instance stays on the scene (a clear is not an event) and a
    // document clear does not move it either.
    CHECK(w.host.SourceFor(0) == &w.scene);
    w.Select(w.doc, "");
    CHECK(w.host.SourceFor(0) == &w.scene);
}

TEST_CASE("InspectorHost filters: Current wins when admitted, else the latest admitted stamp", "[editor][inspector]")
{
    World w;
    const int noAssets = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(noAssets, InspectorFilter::AllBut("assets")));
    w.Select(w.doc, "a");
    w.Select(w.doc2, "b");
    w.Select(w.other, "brick");                                    // an asset click
    CHECK(w.host.SourceFor(0) == &w.other);
    CHECK(w.host.SourceFor(noAssets) == &w.doc2);                  // the latest admitted, never the asset
    w.host.RemoveSource(w.doc2);                                   // closing it falls back to the other
    CHECK(w.host.SourceFor(noAssets) == &w.doc);
    w.Select(w.scene, "7");
    CHECK(w.host.SourceFor(noAssets) == &w.scene);                 // Current() admitted: agrees with All
}

TEST_CASE("InspectorHost filters: closing the last admitted source leaves null, never a dangling pointer", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc, "a");
    w.host.RemoveSource(w.doc);
    w.host.RemoveSource(w.doc2);
    CHECK(w.host.SourceFor(input) == nullptr);
}

TEST_CASE("InspectorHost filters: nothing stamped falls back to the scene, else the newest admitted source", "[editor][inspector]")
{
    World w;
    const int noAssets = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(noAssets, InspectorFilter::AllBut("assets")));
    CHECK(w.host.SourceFor(noAssets) == &w.scene);
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    CHECK(w.host.SourceFor(input) == &w.doc2);                     // most recently added
}

TEST_CASE("InspectorHost filters: an empty-kind source is admitted only by All", "[editor][inspector]")
{
    World w;
    FakeSource mesh{ "rock.arcmesh", "" };
    w.host.AddSource(mesh);
    const int noAssets = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(noAssets, InspectorFilter::AllBut("assets")));
    w.Select(mesh, "m");                                           // (a real mesh doc never selects; the rule still holds)
    CHECK(w.host.SourceFor(0) == &mesh);
    CHECK(w.host.SourceFor(noAssets) == &w.scene);
}

TEST_CASE("InspectorHost filters: SetFilter refuses an all-excluded set and an unknown id", "[editor][inspector]")
{
    World w;
    InspectorFilter none;
    none.excluded = { "scene", "assets", "input-actions", "material", "sprite", "mesh" };
    CHECK_FALSE(w.host.SetFilter(0, none));
    CHECK(w.host.Find(0)->filter.IsAll());
    CHECK_FALSE(w.host.SetFilter(5, InspectorFilter::Only("scene")));
}

TEST_CASE("InspectorHost filters: a pin captures THIS instance's page; the pin wins over a filter change", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.scene, "B");
    w.Select(w.doc, "Player/Jump");                                // Current() is the document
    REQUIRE(w.host.CanPin(scene));
    w.host.SetPinned(scene, true);
    CHECK(w.host.Find(scene)->pinnedSource == &w.scene);           // not the binding
    CHECK(w.host.Find(scene)->pinnedKey == "B");
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("input-actions")));
    CHECK(w.host.SourceFor(scene) == &w.scene);                    // pinned: unchanged
    w.host.SetPinned(scene, false);
    CHECK(w.host.SourceFor(scene) == &w.doc);                      // follows the new filter
}

TEST_CASE("InspectorHost filters: ReleaseAll keeps a permanent source and drops its history and pins", "[editor][inspector]")
{
    FakeSource scene{ "Scene", "scene" }, assets{ "Assets", "assets" }, doc{ "P.arcinput", "input-actions" };
    InspectorHost host{ scene };
    host.AddSource(assets, /*permanent*/ true);
    host.AddSource(doc);
    const int onlyAssets = host.AddInstance();
    REQUIRE(host.SetFilter(onlyAssets, InspectorFilter::Only("assets")));
    assets.key = "g1"; host.NotifySelected(assets);
    host.SetPinned(onlyAssets, true);
    host.ReleaseAll();
    CHECK(host.SourceFor(onlyAssets) == &assets);                  // still registered, pin released
    CHECK_FALSE(host.Find(onlyAssets)->pinned);
    CHECK(host.History().empty());
    CHECK(host.Find(onlyAssets)->filter == InspectorFilter::Only("assets"));   // filters are layout
    doc.key = "x"; host.NotifySelected(doc);                       // doc was dropped: ignored
    CHECK(&host.Current() == &scene);
    assets.key = "g2"; host.NotifySelected(assets);                // assets still live
    CHECK(&host.Current() == &assets);
}

TEST_CASE("InspectorHost filters: Back in a Scene instance skips document entries and re-selects in the scene", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.scene, "A");        // 0
    w.Select(w.doc, "d1");         // 1
    w.Select(w.scene, "B");        // 2
    w.Select(w.doc, "d2");         // 3  <- cursor
    CHECK(w.host.PositionFor(scene) == std::optional<std::size_t>{ 2 });   // the scene's live "B"
    REQUIRE(w.host.CanGoBack(scene));
    CHECK(w.host.BackEntry(scene)->key == "A");                     // position = 2 (B); back = 0 (A)
    CHECK(w.host.BackIndices(scene) == std::vector<std::size_t>{ 0 });
    CHECK_FALSE(w.host.CanGoForward(scene));
    REQUIRE(w.host.GoBack(scene));
    CHECK(w.scene.key == "A");
    CHECK(&w.host.Current() == &w.scene);                           // a landing IS a selection: All follows
    CHECK(w.host.HistoryCursor() == 0);
    CHECK(w.host.ForwardEntry(scene)->key == "B");                  // skips d1
    CHECK(w.host.ForwardIndices(scene) == std::vector<std::size_t>{ 2 });
}

TEST_CASE("InspectorHost filters: a PINNED instance anchors on the page it shows (pinnedSource + pinnedKey), not the source's live key", "[editor][inspector]")
{
    // Integration residual 2c: PositionFor anchored every instance on its
    // routed source's LIVE SelectionKey(). A pinned instance shows its
    // pinnedKey's page, so its arrows measured from the wrong entry.
    World w;
    const int scene = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.scene, "A");        // 0
    w.Select(w.scene, "B");        // 1
    w.host.SetPinned(scene, true); // holds B
    w.Select(w.scene, "C");        // 2
    w.Select(w.scene, "D");        // 3  <- cursor; the scene's live key is D
    REQUIRE(w.host.Find(scene)->pinnedKey == "B");
    CHECK(w.host.PositionFor(scene) == std::optional<std::size_t>{ 1 });   // the pinned B, not the live D
    REQUIRE(w.host.BackEntry(scene) != nullptr);
    CHECK(w.host.BackEntry(scene)->key == "A");
    REQUIRE(w.host.ForwardEntry(scene) != nullptr);
    CHECK(w.host.ForwardEntry(scene)->key == "C");
    // A breadcrumb click inside the pin (RepinKey) moves the anchor with the page.
    w.host.RepinKey(scene, "C");
    CHECK(w.host.PositionFor(scene) == std::optional<std::size_t>{ 2 });
    // The unpinned main instance still anchors on the live key.
    CHECK(w.host.PositionFor(0) == std::optional<std::size_t>{ 3 });
}

TEST_CASE("InspectorHost filters: an All instance matches the unfiltered history exactly", "[editor][inspector]")
{
    World w;
    constexpr int all = 0;
    w.Select(w.scene, "A");
    w.Select(w.doc, "d1");
    w.Select(w.scene, "B");
    CHECK(w.host.BackIndex(all) == std::optional<std::size_t>{ 1 });
    CHECK(w.host.BackEntry(all) == w.host.BackEntry());
    REQUIRE(w.host.GoBack(all));
    CHECK(w.host.ForwardIndex(all) == std::optional<std::size_t>{ 2 });
}

TEST_CASE("InspectorHost filters: a filtered GoBack prunes a stale admitted entry and keeps walking", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc2, "old");       // 0 -- will not restore
    w.Select(w.doc, "d1");         // 1
    w.Select(w.scene, "S");        // 2
    w.Select(w.doc, "d2");         // 3
    w.doc2.restoreOk = false;
    REQUIRE(w.host.GoBack(input)); // position 3 -> back 1 (d1) restores fine
    CHECK(w.doc.key == "d1");
    CHECK_FALSE(w.host.GoBack(input));   // only the stale doc2 entry is left: pruned, no landing
    CHECK(w.host.History().size() == 3);
}

namespace
{
    // The default two-Inspector layout over one shared history: instance 0 =
    // All but Assets, instance 1 = Assets only (spec s6).
    struct DefaultLayoutWorld
    {
        FakeSource scene{ "Scene", "scene" }, assets{ "Assets", "assets" };
        InspectorHost host{ scene };
        DefaultLayoutWorld() { host.AddSource(assets, /*permanent*/ true); host.ApplyDefaultInspectorLayout(); }
        void Select(FakeSource& s, std::string k) { s.key = std::move(k); host.NotifySelected(s); }
    };
}

TEST_CASE("InspectorHost history: Back in the main Inspector leaves the Assets instance's arrows on the asset it shows", "[editor][inspector]")
{
    // Final review I2/F1: [S1,A1,S2,A2], Back in the main Inspector lands S1;
    // Inspector 2 still SHOWS A2, so its Back is A1 and it has no Forward.
    DefaultLayoutWorld w;
    constexpr int kMain = 0, kAssets = InspectorHost::kAssetsInstanceId;
    w.Select(w.scene, "S1"); w.Select(w.assets, "A1"); w.Select(w.scene, "S2"); w.Select(w.assets, "A2");
    REQUIRE(w.host.GoBack(kMain));
    CHECK(w.scene.key == "S1");
    CHECK(w.assets.key == "A2");                                    // untouched: the asset model did not move
    REQUIRE(w.host.SourceFor(kAssets) == &w.assets);
    REQUIRE(w.host.BackEntry(kAssets) != nullptr);
    CHECK(w.host.BackEntry(kAssets)->key == "A1");
    CHECK(w.host.BackIndices(kAssets) == std::vector<std::size_t>{ 1 });
    CHECK_FALSE(w.host.CanGoForward(kAssets));
    CHECK(w.host.ForwardEntry(kAssets) == nullptr);
    CHECK(w.host.ForwardIndices(kAssets).empty());
    // The main Inspector is truthful too: it shows S1, Forward is S2.
    CHECK_FALSE(w.host.CanGoBack(kMain));
    REQUIRE(w.host.ForwardEntry(kMain) != nullptr);
    CHECK(w.host.ForwardEntry(kMain)->key == "S2");
}

TEST_CASE("InspectorHost history: Back in the Assets Inspector leaves the main Inspector's arrows on the entity it shows", "[editor][inspector]")
{
    // Final review I4: [a1,A,a2,B], Back in Inspector 2 lands a1; the main
    // Inspector still SHOWS B, so its Back is A and it has no Forward.
    DefaultLayoutWorld w;
    constexpr int kMain = 0, kAssets = InspectorHost::kAssetsInstanceId;
    w.Select(w.assets, "a1"); w.Select(w.scene, "A"); w.Select(w.assets, "a2"); w.Select(w.scene, "B");
    REQUIRE(w.host.GoBack(kAssets));
    CHECK(w.assets.key == "a1");
    CHECK(w.scene.key == "B");
    REQUIRE(w.host.SourceFor(kMain) == &w.scene);
    REQUIRE(w.host.BackEntry(kMain) != nullptr);
    CHECK(w.host.BackEntry(kMain)->key == "A");
    CHECK_FALSE(w.host.CanGoForward(kMain));
    CHECK(w.host.ForwardEntry(kMain) == nullptr);
    CHECK_FALSE(w.host.CanGoBack(kAssets));
    REQUIRE(w.host.ForwardEntry(kAssets) != nullptr);
    CHECK(w.host.ForwardEntry(kAssets)->key == "a2");
    // Back in the main Inspector now walks from B, not from the cursor.
    REQUIRE(w.host.GoBack(kMain));
    CHECK(w.scene.key == "A");
}

TEST_CASE("InspectorHost history: a filtered GoForward prunes a stale admitted entry and keeps walking", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc, "d1");         // 0
    w.Select(w.scene, "S");        // 1
    w.Select(w.doc2, "gone");      // 2 -- will not restore
    w.Select(w.doc, "d3");         // 3
    REQUIRE(w.host.GoBack(input)); // d3 -> doc2 (still restorable here)
    CHECK(w.doc2.key == "gone");
    REQUIRE(w.host.GoBack(input)); // -> d1
    CHECK(w.doc.key == "d1");
    w.doc2.restoreOk = false;
    REQUIRE(w.host.GoForward(input));   // the stale doc2 entry is pruned on the way; lands d3
    CHECK(w.doc.key == "d3");
    CHECK(w.host.History().size() == 3);
    CHECK_FALSE(w.host.CanGoForward(input));
}

TEST_CASE("InspectorHost history: nothing admitted before the cursor means no Back (the sentinel case)", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.doc, "d1");
    w.Select(w.doc, "d2");
    CHECK(w.host.SourceFor(scene) == &w.scene);    // the fallback, nothing selected in it
    CHECK_FALSE(w.host.CanGoBack(scene));
    CHECK(w.host.BackEntry(scene) == nullptr);
    CHECK(w.host.BackIndices(scene).empty());
    CHECK_FALSE(w.host.CanGoForward(scene));
    CHECK_FALSE(w.host.GoBack(scene));
    CHECK(w.host.HistoryCursor() == 1);
}

TEST_CASE("Inspector header layout: one row when it fits; the breadcrumb wraps, then the icon face gives way, the pin never clips", "[editor][inspector]")
{
    // The combo's face is always icons (user request 2026-09-30): comboFull
    // = every ticked kind's icon, comboMin = one icon + "+N". Final fix H's
    // icon-only collapse below 200 px is superseded -- no threshold, the face
    // just drops icons into "+N" as row 1 narrows.
    InspectorHeaderMetrics m;
    m.arrows = 56.0f; m.comboFull = 122.0f; m.comboMin = 42.0f; m.pin = 24.0f; m.spacing = 8.0f;

    m.avail = 800.0f;                       // wide: arrows | combo | crumbs | pin
    InspectorHeaderLayout l = LayoutInspectorHeader(m);
    CHECK_FALSE(l.crumbsOwnRow);
    CHECK_FALSE(l.pinOnCrumbRow);
    CHECK(l.comboWidth == 122.0f);

    m.avail = 250.0f;                       // no room for 120 px of crumbs: own row, every icon
    l = LayoutInspectorHeader(m);
    CHECK(l.crumbsOwnRow);
    CHECK_FALSE(l.pinOnCrumbRow);
    CHECK(l.comboWidth == 122.0f);

    m.avail = 200.0f;                       // the face gives way (fewer icons + "+N") so the pin still ends row 1
    l = LayoutInspectorHeader(m);
    CHECK(l.crumbsOwnRow);
    CHECK_FALSE(l.pinOnCrumbRow);
    CHECK(l.comboWidth == 200.0f - 56.0f - 24.0f - 2.0f * 8.0f);
    CHECK(l.comboWidth >= 42.0f);

    m.avail = 150.0f;                       // narrower still: never below one icon + "+N"
    l = LayoutInspectorHeader(m);
    CHECK(l.crumbsOwnRow);
    CHECK_FALSE(l.pinOnCrumbRow);
    CHECK(l.comboWidth == 54.0f);

    m.avail = 120.0f;                       // even arrows + one icon + pin overflow: the pin leads the crumb row
    l = LayoutInspectorHeader(m);
    CHECK(l.crumbsOwnRow);
    CHECK(l.pinOnCrumbRow);
    CHECK(l.comboWidth == 42.0f);

    m.comboFull = m.comboMin = 38.0f;       // All: one glyph, full = min
    m.avail = 180.0f;
    l = LayoutInspectorHeader(m);
    CHECK(l.comboWidth == 38.0f);
}

TEST_CASE("Inspector header layout: row 1 reserves the crumbs' NATURAL width (spec 2026-09-30 s4.3)", "[editor][inspector]")
{
    // 1080p main-Inspector metrics (s4.3): avail 380, arrows 56, combo 130,
    // pin 24, stock ItemSpacing 8 -> row 1 holds at most 380 - 234 = 146 px
    // of crumbs. (s4.3 names "~145" for "Scene > MeshCube"; at these exact
    // metrics the boundary is 146, so the boundary itself is pinned.)
    InspectorHeaderMetrics m;
    m.avail = 380.0f; m.arrows = 56.0f; m.comboFull = 130.0f; m.comboMin = 42.0f; m.pin = 24.0f; m.spacing = 8.0f;
    m.crumbsNatural = 60.0f;
    CHECK_FALSE(LayoutInspectorHeader(m).crumbsOwnRow);
    m.crumbsNatural = 146.0f;                             // exactly fits
    CHECK_FALSE(LayoutInspectorHeader(m).crumbsOwnRow);
    m.crumbsNatural = 147.0f;                             // one px over: its own row
    CHECK(LayoutInspectorHeader(m).crumbsOwnRow);
    m.crumbsNatural = 160.0f;
    CHECK(LayoutInspectorHeader(m).crumbsOwnRow);
    m.crumbsNatural = 0.0f;                               // unknown: the old 120 px reservation
    CHECK_FALSE(LayoutInspectorHeader(m).crumbsOwnRow);
}

TEST_CASE("FitCrumbs: fits, one head hidden, two hidden, leaf ellipsized; a single crumb never overflows", "[editor][inspector]")
{
    const float chevron = 20.0f, more = 24.0f;
    CrumbFit f = FitCrumbs(std::vector<float>{ 50.0f, 60.0f }, chevron, more, 200.0f);   // 130 <= 200
    CHECK(f.firstShown == 0);
    CHECK_FALSE(f.overflow);
    CHECK(f.leafMax == 60.0f);
    f = FitCrumbs(std::vector<float>{ 50.0f, 60.0f, 70.0f }, chevron, more, 200.0f);       // 220 > 200; 24+20+60+20+70 = 194
    CHECK(f.firstShown == 1);
    CHECK(f.overflow);
    CHECK(f.leafMax == 70.0f);
    f = FitCrumbs(std::vector<float>{ 50.0f, 60.0f, 70.0f }, chevron, more, 150.0f);       // 194 > 150; 24+20+70 = 114
    CHECK(f.firstShown == 2);
    CHECK(f.overflow);
    CHECK(f.leafMax == 70.0f);
    f = FitCrumbs(std::vector<float>{ 50.0f, 300.0f }, chevron, more, 200.0f);             // the leaf alone: 344 > 200
    CHECK(f.firstShown == 1);                             // the leaf is never hidden
    CHECK(f.overflow);
    CHECK(f.leafMax == 200.0f - more - chevron);
    f = FitCrumbs(std::vector<float>{ 300.0f }, chevron, more, 200.0f);
    CHECK(f.firstShown == 0);
    CHECK_FALSE(f.overflow);                              // nothing to hide behind a button
    CHECK(f.leafMax == 200.0f);
    f = FitCrumbs(std::vector<float>{ 100.0f }, chevron, more, 200.0f);
    CHECK_FALSE(f.overflow);
    CHECK(f.leafMax == 100.0f);
}

namespace
{
    // One device-less Inspector frame loop at a forced window width; returns
    // the primary window (after `frames` frames) for geometry probes.
    ImGuiWindow* DrawPrimaryAtWidth(InspectorHost& host, InspectorWindowsState& state, float width, int frames = 3)
    {
        for (int f = 0; f < frames; ++f)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            (void)DrawInspectorWindows(host, state, nullptr);
            if (ImGuiWindow* w = ImGui::FindWindowByName(kPrimaryInspectorWindowId))
                ImGui::SetWindowSize(w, ImVec2(width, 400.0f), ImGuiCond_Always);
            ImGui::Render();
        }
        return ImGui::FindWindowByName(kPrimaryInspectorWindowId);
    }
    ImGuiWindow* CrumbsChildOf(ImGuiWindow* parent)
    {
        for (ImGuiWindow* c : ImGui::GetCurrentContext()->Windows)
            if (c->ParentWindow == parent && std::string(c->Name).find("##crumbs") != std::string::npos) return c;
        return nullptr;
    }
}

TEST_CASE("DrawInspectorWindows: a narrow Inspector wraps the breadcrumb to its own row and never overflows; a wide one keeps one row", "[editor][inspector]")
{
    IniContext ic;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    FakeSource scene{ "Scene", "scene" };
    scene.page.crumbs = { { "Scene", {}, {} }, { "Player", {}, {} }, { "Camera", {}, {} } };
    scene.key = "7";
    InspectorHost host{ scene };
    REQUIRE(host.SetFilter(0, InspectorFilter::AllBut("assets")));   // the longest default label
    host.NotifySelected(scene);
    InspectorWindowsState state;

    SECTION("narrow (180 px)")
    {
        ImGuiWindow* w = DrawPrimaryAtWidth(host, state, 180.0f);
        REQUIRE(w != nullptr);
        ImGuiWindow* crumbs = CrumbsChildOf(w);
        REQUIRE(crumbs != nullptr);
        INFO("maxX " << w->DC.CursorMaxPos.x << " workMaxX " << w->WorkRect.Max.x);
        CHECK(w->DC.CursorMaxPos.x <= w->WorkRect.Max.x + 0.5f);          // nothing (the pin included) past the edge
        CHECK(crumbs->Pos.y > w->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f);   // row 2
        CHECK(crumbs->Size.x >= w->WorkRect.GetWidth() - ImGui::GetStyle().ItemSpacing.x - 1.0f);   // full width
    }
    SECTION("very narrow (140 px): one icon + \"+4\", the pin leads the crumb row")
    {
        ImGuiWindow* w = DrawPrimaryAtWidth(host, state, 140.0f);
        REQUIRE(w != nullptr);
        INFO("maxX " << w->DC.CursorMaxPos.x << " workMaxX " << w->WorkRect.Max.x);
        CHECK(w->DC.CursorMaxPos.x <= w->WorkRect.Max.x + 0.5f);
        // One more frame, logged: the face is ONE icon and the "+4" overflow
        // (All but Assets ticks five kinds), never a text label.
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        (void)DrawInspectorWindows(host, state, nullptr);
        const std::string logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        INFO(logged);
        CHECK(logged.find("+4") != std::string::npos);
        CHECK(logged.find(ICON_LC_CLAPPERBOARD) != std::string::npos);
        CHECK(logged.find(ICON_LC_GAMEPAD_2) == std::string::npos);
    }
    SECTION("wide (800 px)")
    {
        ImGuiWindow* w = DrawPrimaryAtWidth(host, state, 800.0f);
        REQUIRE(w != nullptr);
        ImGuiWindow* crumbs = CrumbsChildOf(w);
        REQUIRE(crumbs != nullptr);
        CHECK(w->DC.CursorMaxPos.x <= w->WorkRect.Max.x + 0.5f);
        CHECK(crumbs->Pos.y < w->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f);   // row 1
    }
}

namespace
{
    // One logged frame with the primary Inspector forced to `size` at (100,100);
    // `activate` (optional) is pressed through nav on the NEXT frame.
    std::string DrawLoggedAt(InspectorHost& host, InspectorWindowsState& state, ImVec2 size, ImGuiID activate = 0)
    {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        if (activate) ImGui::ActivateItemByID(activate);
        ImGui::SetNextWindowPos(ImVec2(100.0f, 100.0f));
        ImGui::SetNextWindowSize(size);
        ImGui::LogToBuffer();
        (void)DrawInspectorWindows(host, state, nullptr);
        std::string logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        return logged;
    }
}

TEST_CASE("DrawInspectorWindows: an overflowing breadcrumb hides its head behind \"...\", ellipsizes the leaf, never clips (s4.3)", "[editor][inspector]")
{
    IniContext ic;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    const std::string name = "ReferenceCubeMaterialWithALongName";
    bool headSelected = false;
    FakeSource scene{ "Scene", "scene" };
    scene.key = "7";
    InspectorHost host{ scene };
    host.NotifySelected(scene);
    InspectorWindowsState state;
    const ImVec2 size(392.0f, 330.0f);                    // the 1080p Assets-only Inspector (spec 9.3)

    SECTION("the spec's pair fits its row whole at the test atlas: no overflow button")
    {
        scene.page.crumbs = { { "Scene", {}, {} }, { name, {}, {} } };
        std::string logged;
        for (int f = 0; f < 3; ++f) logged = DrawLoggedAt(host, state, size);
        INFO(logged);
        CHECK(logged.find(ICON_LC_ELLIPSIS) == std::string::npos);
        CHECK(logged.find("Scene") != std::string::npos);
        CHECK(logged.find(name) != std::string::npos);
    }
    SECTION("a leaf wider than the row: head behind \"...\", leaf cut, tooltip carries it")
    {
        // The spec's leaf, doubled: ~490 px at the test atlas's 7 px advance,
        // wider than the 376 px row on any face >= 5.5 px (the single name
        // fits whole here -- the section above).
        const std::string leaf = name + name;
        scene.page.crumbs = { { "Scene", [&] { headSelected = true; }, {} }, { leaf, {}, {} } };
        std::string logged;
        for (int f = 0; f < 3; ++f) logged = DrawLoggedAt(host, state, size);
        ImGuiWindow* w = ImGui::FindWindowByName(kPrimaryInspectorWindowId);
        REQUIRE(w != nullptr);
        ImGuiWindow* crumbs = CrumbsChildOf(w);
        REQUIRE(crumbs != nullptr);
        INFO(logged);
        CHECK(logged.find(ICON_LC_ELLIPSIS) != std::string::npos);    // the overflow button
        CHECK(logged.find("Scene") == std::string::npos);             // the head hides behind it
        CHECK(logged.find(leaf) == std::string::npos);                // the leaf is cut...
        CHECK(logged.find(name.substr(0, 12)) != std::string::npos);  // ...its head shows
        CHECK(crumbs->DC.CursorMaxPos.x <= crumbs->Pos.x + crumbs->Size.x + 0.5f);   // every crumb inside the row
        CHECK(crumbs->Pos.x + crumbs->Size.x <= w->WorkRect.Max.x + 0.5f);
        CHECK(w->DC.CursorMaxPos.x <= w->WorkRect.Max.x + 0.5f);

        // Click "...": the popup lists the hidden head; its row performs the crumb's select.
        const ImVec2 more(crumbs->Pos.x + 4.0f, crumbs->Pos.y + ImGui::GetTextLineHeight() * 0.5f);
        io.AddMousePosEvent(more.x, more.y); (void)DrawLoggedAt(host, state, size);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); (void)DrawLoggedAt(host, state, size);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); (void)DrawLoggedAt(host, state, size);
        const std::string listed = DrawLoggedAt(host, state, size);
        CHECK(listed.find("Scene") != std::string::npos);
        char popupName[32];
        std::snprintf(popupName, sizeof(popupName), "##Popup_%08x", ImHashStr("##crumbmore", 0, crumbs->ID));
        ImGuiWindow* popup = ImGui::FindWindowByName(popupName);
        REQUIRE(popup != nullptr);
        REQUIRE(popup->Active);
        CHECK(popup->Pos.y >= crumbs->Pos.y + ImGui::GetTextLineHeight() - 0.5f);   // under the button
        (void)DrawLoggedAt(host, state, size, ImHashStr("Scene##crumbhidden0", 0, popup->ID));
        (void)DrawLoggedAt(host, state, size);
        CHECK(headSelected);

        // Hover the leaf (stationary): its tooltip is the full label.
        io.AddMousePosEvent(crumbs->Pos.x + crumbs->Size.x * 0.6f, more.y);
        bool tooltip = false;
        for (int frame = 0; frame < 120 && !tooltip; ++frame)
            tooltip = DrawLoggedAt(host, state, size).find(leaf) != std::string::npos;
        CHECK(tooltip);
    }
}

TEST_CASE("DrawInspectorWindows: a single-kind empty state names the kind in the singular", "[editor][inspector]")
{
    IniContext ic;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    REQUIRE(host.SetFilter(0, InspectorFilter::Only("material")));
    InspectorWindowsState state;
    ImGui::NewFrame();
    ImGui::LogToBuffer();
    (void)DrawInspectorWindows(host, state, nullptr);
    const std::string logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::Render();
    INFO(logged);
    CHECK(logged.find("No Material document open") != std::string::npos);
    CHECK(logged.find("Materials document") == std::string::npos);
}

TEST_CASE("Default layout geometry: the user's ReferenceProject layout in pixels, clamped so the central node keeps 40%", "[editor][inspector]")
{
    // USER DECISION 2026-09-30: Inspector 380 w (full height, right), Outliner
    // 270 w, the bottom band 350 h (under the Outliner, left of the
    // Inspector). These three splits each have the central node on one side,
    // so ImGui keeps the OTHER side at its pixel size on resize.
    const DefaultLayoutPixels full = ComputeDefaultLayoutPixels(1920.0f, 954.0f, LayoutFactorySettings{});   // 1920x1080 maximized
    CHECK(full.inspector == 380.0f);
    CHECK(full.outliner == 270.0f);
    CHECK(full.bottomBand == 350.0f);

    const DefaultLayoutPixels boot = ComputeDefaultLayoutPixels(1280.0f, 647.0f, LayoutFactorySettings{});   // the hidden 1280x720 boot window
    CHECK(boot.inspector == 380.0f);                                  // 650 of 1280: the central node keeps 49%
    CHECK(boot.outliner == 270.0f);
    CHECK(boot.bottomBand == 350.0f);                                 // 297 of 647 left: 46%

    const DefaultLayoutPixels small = ComputeDefaultLayoutPixels(800.0f, 500.0f, LayoutFactorySettings{});
    CHECK(small.inspector + small.outliner <= 800.0f * 0.6f + 0.01f);   // central >= 40% of the width
    CHECK(std::abs(small.inspector / small.outliner - 380.0f / 270.0f) < 1e-5f);   // both shrink by one factor
    CHECK(small.bottomBand == 500.0f * 0.6f);                           // central >= 40% of the height
}

TEST_CASE("Default layout geometry: Inspector 2 splits the band by the user's 1920-scale PROPORTION, not a pixel target", "[editor][inspector]")
{
    // Integration residual 2a: the band's browser | Inspector 2 split has NO
    // central node on either side, so ImGui re-divides it by the children's
    // SizeRef RATIO on every resize (imgui.cpp DockNodeTreeUpdatePosSize,
    // rule 4). A 390 px target built at the 1280x720 boot size (506:390)
    // grew Inspector 2 to ~668 px maximized. The split is the user's own
    // saved proportion instead -- browser 1144 : Inspector 2 392 -- the same
    // at every build size, and exactly 392 px on a 1536 px band (1920x1080).
    const LayoutFactorySettings factory{};   // editor.layout.factory.* defaults (settings S6-32)
    const float fraction = DefaultAssetsInspectorBandFraction(factory);
    CHECK(factory.browserRefPx == 1144.0f);
    CHECK(factory.assetsInspectorRefPx == 392.0f);
    CHECK(fraction == 392.0f / 1536.0f);
    CHECK(std::abs(1536.0f * fraction - 392.0f) < 1e-3f);
    // Well inside the split clamps at any size (it used to need a 45% cap).
    CHECK(fraction > 0.05f);
    CHECK(fraction < 0.45f);
}

TEST_CASE("InspectorHost: a closed slot's filter comes back when Window > New Inspector reuses the slot", "[editor][inspector]")
{
    // Final review m2: closing "Inspector 2 - Assets" and opening a New
    // Inspector reused slot 1 UNFILTERED -- in the Assets dock slot its
    // [Window][inspector_1] entry still names -- so it followed every scene
    // and document selection. The host remembers each slot's last filter for
    // the session.
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    host.ApplyDefaultInspectorLayout();
    REQUIRE(host.Find(InspectorHost::kAssetsInstanceId)->filter == InspectorFilter::Only("assets"));
    host.RemoveInstance(InspectorHost::kAssetsInstanceId);
    const int reopened = host.AddInstance();
    REQUIRE(reopened == InspectorHost::kAssetsInstanceId);
    CHECK(host.Find(reopened)->filter == InspectorFilter::Only("assets"));
    // A slot never used this session opens All, as before.
    const int fresh = host.AddInstance();
    CHECK(host.Find(fresh)->filter.IsAll());
    // A filter change made before the close is what comes back.
    REQUIRE(host.SetFilter(fresh, InspectorFilter::Only("scene")));
    host.RemoveInstance(fresh);
    CHECK(host.Find(host.AddInstance())->filter == InspectorFilter::Only("scene"));
}

// s5.7: the header (crumbs, filter, pin) stays put while a tall page scrolls in
// "##page"; the page still draws on collapsed frames (its ScopeGuard must run);
// Ctrl+S still routes from inside it (the child is in the parent's focus route).
TEST_CASE("DrawInspectorWindows: the page scrolls in ##page under a pinned header", "[editor][inspector]")
{
    IniContext ic;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    FakeSource scene{ "Scene", "scene" };
    scene.page.crumbs = { { "Scene", {}, {} }, { "Player", {}, {} } };
    scene.page.rows = 60;
    scene.key = "7";
    InspectorHost host{ scene };
    host.NotifySelected(scene);
    InspectorWindowsState state;
    const auto frame = [&](bool collapsed = false)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(100.0f, 100.0f));   // the primary Inspector: the next Begin
        ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
        ImGui::SetNextWindowCollapsed(collapsed);
        InspectorWindowsResult r = DrawInspectorWindows(host, state, nullptr);
        ImGui::Render();
        return r;
    };
    const auto pageOf = [](ImGuiWindow* parent) -> ImGuiWindow*
    {
        for (ImGuiWindow* c : ImGui::GetCurrentContext()->Windows)
            if (c->ParentWindow == parent && std::string(c->Name).find("##page") != std::string::npos) return c;
        return nullptr;
    };
    frame(); frame();
    ImGuiWindow* w = ImGui::FindWindowByName(kPrimaryInspectorWindowId);
    REQUIRE(w != nullptr);
    ImGuiWindow* page = pageOf(w);
    REQUIRE(page != nullptr);

    SECTION("scrolled to the end, the window never scrolls and the crumbs stay above the page")
    {
        REQUIRE(page->ScrollMax.y > 0.0f);                 // 60 rows overflow 300 px
        ImGui::SetScrollY(page, page->ScrollMax.y);        // imgui_internal overload; lands next frame
        frame(); frame();
        CHECK(page->Scroll.y > 0.0f);
        CHECK(w->ScrollMax.y == 0.0f);                     // the header's window has nothing to scroll
        ImGuiWindow* crumbs = CrumbsChildOf(w);
        REQUIRE(crumbs != nullptr);
        CHECK(crumbs->Pos.y >= w->InnerRect.Min.y - 0.5f);
        CHECK(crumbs->Pos.y + crumbs->Size.y <= page->Pos.y + 0.5f);
    }
    SECTION("a collapsed window still draws the page once per frame")
    {
        const int before = scene.page.draws;
        frame(true); frame(true); frame(true);
        CHECK(scene.page.draws == before + 3);
    }
    SECTION("Ctrl+S with focus inside ##page reports the source")
    {
        ImGui::FocusWindow(page);
        frame(); frame();                                  // the focus route settles
        Arcane::TestKeys::AddKeyEvent(io, ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_S, true);
        const InspectorWindowsResult r = frame();
        io.AddKeyEvent(ImGuiKey_S, false);
        Arcane::TestKeys::AddKeyEvent(io, ImGuiMod_Ctrl, false);
        frame();
        REQUIRE(r.saveRequested.size() == 1);
        CHECK(r.saveRequested[0] == &scene);
        CHECK(r.focusedSource == &scene);
    }
    SECTION("a rebound document.save fires on its new chord, and Ctrl+S no longer does")
    {
        Arcane::Editor::EditorActions& keys = Arcane::Editor::EditorActions::Get();
        Arcane::CVarRegistry& reg = keys.Registry();
        reg.Set(keys.HandleOf("document.save"), Arcane::CVarValue::String("Ctrl+K"), Arcane::SetBy::EditorUser, "editor", Arcane::CVarContext::Editor);
        reg.PublishImmediate();
        keys.RefreshBindings();
        ImGui::FocusWindow(page);
        frame(); frame();
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_S, true);
        const InspectorWindowsResult viaS = frame();
        io.AddKeyEvent(ImGuiKey_S, false); io.AddKeyEvent(ImGuiKey_K, true);
        const InspectorWindowsResult viaK = frame();
        io.AddKeyEvent(ImGuiKey_K, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
        CHECK(viaS.saveRequested.empty());
        REQUIRE(viaK.saveRequested.size() == 1);
        CHECK(viaK.saveRequested[0] == &scene);
        reg.RevertLayer(Arcane::SetBy::EditorUser);
        reg.PublishImmediate();
        keys.RefreshBindings();
    }
}

TEST_CASE("InspectorHost::RefreshLabels updates history labels and pin names after a rename", "[editor][inspector][assetops]")
{
    World w;
    w.other.page.crumbs = { InspectorCrumb{ "Assets", [] {}, std::nullopt }, InspectorCrumb{ "old.png", [] {}, std::string{ "k" } } };
    w.Select(w.other, "k"); const int pin = w.host.AddInstance(); w.host.SetPinned(pin, true);
    REQUIRE(w.host.History().back().label == "Assets > old.png");
    w.other.page.crumbs[1].label = "new.png"; w.other.name = "Assets (renamed)";
    w.host.RefreshLabels();
    CHECK((w.host.History().back().label == "Assets > new.png" && w.host.Find(pin)->pinnedName == "Assets (renamed)" && w.host.SourceFor(pin) == &w.other));
}
