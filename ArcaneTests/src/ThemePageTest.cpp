// Settings arc S4 (spec s7.1): the theme page -- presets, import/export, live
// swatches and the contrast warnings. The page writes cvars; the applier (S4-2)
// re-themes, so these tests read the registry and the draw list.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/UserDataDirs.hpp"
#include "Settings/EditorThemeSettings.hpp"
#include "Settings/SettingsHost.hpp"
#include "Settings/SettingsModel.hpp"
#include "Settings/ThemePage.hpp"
#include "Settings/ThemePresets.hpp"
#include "Widgets/EditorTheme.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Project/ProjectPaths.hpp>   // kDistBuild
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace Arcane::Editor;

namespace
{
    struct PageHarness
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        int stackDrift = 0;
        PageHarness()
        {
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280, 900);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        }
        ~PageHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        template <class F> void Frame(F&& body)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(1260, 880));
            ImGui::Begin("Page");
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            const int c0 = g.ColorStack.Size, v0 = g.StyleVarStack.Size, f0 = g.FontStack.Size;
            body();
            stackDrift += (g.ColorStack.Size - c0) + (g.StyleVarStack.Size - v0) + (g.FontStack.Size - f0);
            ImGui::End();
            ImGui::Render();
        }
        bool Drew(const ImVec4& c) const
        {
            const ImU32 want = ImGui::ColorConvertFloat4ToU32(c);
            for (ImGuiWindow* w : ctx->Windows)
                for (const ImDrawVert& v : w->DrawList->VtxBuffer)
                    if (v.col == want) return true;
            return false;
        }
    };

    void RevertEditorUser()
    {
        Arcane::CVarRegistry::Get().RevertLayer(Arcane::SetBy::EditorUser);
        Arcane::CVarRegistry::Get().PublishImmediate();
    }
}

TEST_CASE("Theme page: a preset button's path writes every theme cvar and reports it", "[theme][settings-ui]")
{
    ThemePageState st;
    REQUIRE(ApplyThemePresetByName(st, "HighContrast"));
    CHECK(st.status.find("High Contrast") != std::string::npos);
    CHECK(ImGui::ColorConvertFloat4ToU32(ToPalette(Arcane::Settings<EditorThemeSettings>()).panel) == IM_COL32(0, 0, 0, 255));
    CHECK_FALSE(ApplyThemePresetByName(st, "NoSuchTheme"));
    CHECK(st.status.find("NoSuchTheme") != std::string::npos);
    RevertEditorUser();
}

TEST_CASE("Theme page: export appends .arctheme; import reads it back; a swatch edit sets one token", "[theme][settings-ui]")
{
    ThemePageState st;
    const std::filesystem::path base = std::filesystem::temp_directory_path() / "s4-export";
    REQUIRE(ExportThemeTo(st, base));
    const std::filesystem::path written = std::filesystem::temp_directory_path() / "s4-export.arctheme";
    CHECK(std::filesystem::exists(written));
    REQUIRE(SetThemeToken("error", ImVec4(0.0f, 1.0f, 0.0f, 1.0f)));
    CHECK(ImGui::ColorConvertFloat4ToU32(ToPalette(Arcane::Settings<EditorThemeSettings>()).error) == IM_COL32(0, 255, 0, 255));
    REQUIRE(ImportThemeFrom(st, written));   // the export was Dark: error comes back
    CHECK(ImGui::ColorConvertFloat4ToU32(ToPalette(Arcane::Settings<EditorThemeSettings>()).error)
          == ImGui::ColorConvertFloat4ToU32(Theme::kDarkPalette.error));
    CHECK_FALSE(ImportThemeFrom(st, std::filesystem::temp_directory_path() / "missing.arctheme"));
    CHECK(st.status.find("missing.arctheme") != std::string::npos);
    std::filesystem::remove(written);
    RevertEditorUser();
}

TEST_CASE("Theme page: the Preferences tree has Appearance/Theme with the swatch page and the theme cvars", "[theme][settings-ui]")
{
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const auto panel = reg.Describe("editor.theme.panel");
    REQUIRE(panel.has_value());
    CHECK(panel->categoryPath == "Appearance/Theme");

    SettingsModel m;
    m.SetPages({ SettingsPageRef{ Arcane::SettingScope::PreferencesMachine, "Appearance/Theme", "Theme" } });
    m.Rebuild(reg, Arcane::SettingScope::PreferencesMachine);
    const SettingsTreeNode* theme = m.Find("Appearance/Theme");
    REQUIRE(theme != nullptr);
    CHECK(theme->label == "Theme");
    CHECK(std::find(theme->cvars.begin(), theme->cvars.end(), "editor.theme.panel") != theme->cvars.end());
    REQUIRE(m.Find("Appearance") != nullptr);
    CHECK(m.Find("editor.theme") == nullptr);
}

TEST_CASE("Theme page: draws balanced, and warns in the warning colour when a pair falls under its bar", "[theme][settings-ui]")
{
    ThemePageState st;
    PageHarness h;
    h.Frame([&] { DrawThemePage(&st); });
    h.Frame([&] { DrawThemePage(&st); });
    CHECK(h.stackDrift == 0);
    for (const ContrastRow& row : ContrastReport(Theme::kDarkPalette)) CHECK(row.ok);   // Dark: no warning row
    Theme::Palette bad = Theme::kDarkPalette;
    bad.text = bad.panel;                                                               // 1:1
    bad.warning = ImVec4(0.9f, 0.6f, 0.1f, 1.0f);                                       // a colour nothing else uses
    const Theme::ScopedLivePalette scope(bad);
    h.Frame([&] { DrawThemePage(&st); });
    CHECK(h.Drew(bad.warning));
    CHECK(h.stackDrift == 0);
}

TEST_CASE("Theme page: a swatch edit and a preset reach the debounced archive queue, not only the exit-time archive",
          "[theme][settings-ui]")
{
    // S4-17 carried gap: theme edits were persisted only at exit, so a crash
    // lost them. The archive folder is redirected so the flush never touches
    // the real per-user Editor/Config.
    if (Arcane::kDistBuild)
        SKIP("Dist has no EditorUserDir (spec s11.0); the no-folder case below covers it");
    const std::filesystem::path local = std::filesystem::temp_directory_path() / "s4-theme-archive";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local);
    Arcane::Test::ScopedUserDataBase scopedUserData(local);
    REQUIRE(scopedUserData.Ok());
    FlushSettingsArchives();   // whatever an earlier test left queued lands in the scratch folder
    REQUIRE_FALSE(SettingsHostArchivePending());

    REQUIRE(SetThemeToken("error", ImVec4(0.0f, 1.0f, 0.0f, 1.0f)));
    CHECK(SettingsHostArchivePending());
    FlushSettingsArchives();
    CHECK_FALSE(SettingsHostArchivePending());
    bool archived = false;
    std::error_code ec;
    Arcane::Paths::Config paths;
    paths.dist = false;
    const auto editorConfig = Arcane::Paths::Resolve(Arcane::Paths::Location::EditorUserDir, paths) / "Config";
    INFO("editor config dir " << editorConfig.string());
    REQUIRE(Arcane::Test::IsUnder(editorConfig, local));   // the archive lands in the scratch base
    for (const auto& e : std::filesystem::recursive_directory_iterator(editorConfig, ec))
        if (e.is_regular_file())
        {
            std::ifstream in(e.path());
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            archived = archived || text.find("error") != std::string::npos;
        }
    CHECK(archived);

    ThemePageState st;
    REQUIRE(ApplyThemePresetByName(st, "HighContrast"));
    CHECK(SettingsHostArchivePending());

    RevertEditorUser();
    FlushSettingsArchives();   // the reverted keys leave the scratch archive
    std::filesystem::remove_all(local);
}

// S7-DIST: Dist has no EditorUserDir (spec s11.0), so the EditorUser rung has
// no folder. A flush keeps the edit for the session; it never writes a
// relative "Config" into the working directory. Simulated in any build by a
// preset Paths dist.
TEST_CASE("Theme page: with no per-user editor folder, a flush writes nothing relative to the working directory",
          "[theme][settings-ui]")
{
    const Arcane::Paths::Config saved = Arcane::Paths::Current();
    const std::filesystem::path cwd = std::filesystem::current_path();
    struct Restore
    {
        Arcane::Paths::Config config;
        std::filesystem::path cwd;
        ~Restore()
        {
            std::error_code ec;
            std::filesystem::current_path(cwd, ec);
            Arcane::Paths::Configure(config);
        }
    } restore{ saved, cwd };
    Arcane::Paths::Config preset = saved;
    preset.dist = true;
    Arcane::Paths::Configure(preset);
    REQUIRE(Arcane::Paths::Get(Arcane::Paths::Location::EditorUserDir).empty());

    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "s7dist-theme-cwd";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::filesystem::current_path(scratch);
    FlushSettingsArchives();   // whatever an earlier test left queued
    REQUIRE_FALSE(SettingsHostArchivePending());

    REQUIRE(SetThemeToken("error", ImVec4(0.0f, 1.0f, 0.0f, 1.0f)));
    CHECK(SettingsHostArchivePending());
    FlushSettingsArchives();
    CHECK_FALSE(SettingsHostArchivePending());   // kept for this session, not retried forever
    CHECK_FALSE(std::filesystem::exists(scratch / "Config"));

    RevertEditorUser();
    FlushSettingsArchives();
    CHECK_FALSE(std::filesystem::exists(scratch / "Config"));
    std::filesystem::current_path(cwd);
    std::filesystem::remove_all(scratch);
}
