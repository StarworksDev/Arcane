// Arcane::ForeignModules -- the injected-overlay detection's pure half
// (Base/ForeignModules.hpp). Device-free: the matcher is a table lookup over
// module base names, and the one OS call (EnumerateProcessModules) is
// exercised against THIS process, which needs no GPU, window or device.
//
// Why these exist: the 2026-09-22 editor close crash was an injected overlay
// (GTIII-OSD64.dll, ASUS GPU Tweak III's OSD) over-releasing our ID3D12Device.
// The armor in DeviceCreationD3D12.cpp absorbs that; THIS component is what
// tells a desk WHICH module to blacklist, and what makes a red gate lane on a
// desk with an overlay attributable from the report alone. See
// docs/research/2026-09-22-injected-overlay-modules.md for the table's
// provenance.

#include <Arcane/Base/ForeignModules.hpp>
#include <Arcane/Platform/Platform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace
{
    std::string Lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool ContainsInsensitive(const std::vector<Arcane::ForeignModules::LoadedModule>& modules, const char* wanted)
    {
        const std::string w = Lower(wanted);
        return std::any_of(modules.begin(), modules.end(),
                           [&](const Arcane::ForeignModules::LoadedModule& m) { return Lower(m.name) == w; });
    }

    std::string Join(const std::vector<std::string>& names)
    {
        std::string out;
        for (const std::string& n : names)
            out += (out.empty() ? "" : ", ") + n;
        return out.empty() ? "(none)" : out;
    }
}

TEST_CASE("foreign modules: the Tier 1 table classifies each documented overlay module, case-insensitively",
          "[foreign-modules]")
{
    // The desk's own culprit, spelled as the loader reports it.
    const auto gtiii = Arcane::ForeignModules::Classify("GTIII-OSD64.dll");
    REQUIRE(gtiii.has_value());
    CHECK(gtiii->tier == 1);
    CHECK(gtiii->product.find("GPU Tweak") != std::string::npos);
    // The name is echoed AS GIVEN -- a log line must name the module the way
    // the process shows it, not the table's canonical spelling.
    CHECK(gtiii->module == "GTIII-OSD64.dll");

    // Case never matters: module base names on Windows are case-insensitive
    // and the loader reports whatever case the injector used.
    const auto shouted = Arcane::ForeignModules::Classify("gtiii-osd64.DLL");
    REQUIRE(shouted.has_value());
    CHECK(shouted->tier == 1);
    CHECK(shouted->product == gtiii->product);
    CHECK(shouted->module == "gtiii-osd64.DLL");

    // The other Tier 1 families, one representative each.
    const auto nahimic = Arcane::ForeignModules::Classify("NAHIMICOSD.dll");
    REQUIRE(nahimic.has_value());
    CHECK(nahimic->tier == 1);
    CHECK(nahimic->product.find("Nahimic") != std::string::npos);

    const auto sonic = Arcane::ForeignModules::Classify("SS3DevProps.dll");
    REQUIRE(sonic.has_value());
    CHECK(sonic->tier == 1);
    CHECK(sonic->product.find("Sonic Studio") != std::string::npos);

    const auto rtss = Arcane::ForeignModules::Classify("RTSSHooks64.dll");
    REQUIRE(rtss.has_value());
    CHECK(rtss->tier == 1);
    CHECK(rtss->product.find("RivaTuner") != std::string::npos);

    const auto cortex = Arcane::ForeignModules::Classify("k_fps64.dll");
    REQUIRE(cortex.has_value());
    CHECK(cortex->tier == 1);
    CHECK(cortex->product.find("Razer") != std::string::npos);
}

TEST_CASE("foreign modules: Tier 2 present-path hooks classify as tier 2 with a product and no remedy",
          "[foreign-modules]")
{
    const auto shadowplay = Arcane::ForeignModules::Classify("nvspcap64.dll");
    REQUIRE(shadowplay.has_value());
    CHECK(shadowplay->tier == 2);
    CHECK(shadowplay->product.find("NVIDIA") != std::string::npos);
    // Tier 2 is attribution only -- nothing to tell the user to do.
    CHECK(shadowplay->remedy.empty());
    CHECK(shadowplay->consequence.empty());

    const auto steam = Arcane::ForeignModules::Classify("GameOverlayRenderer64.dll");
    REQUIRE(steam.has_value());
    CHECK(steam->tier == 2);
    CHECK(steam->product.find("Steam") != std::string::npos);

    const auto reshade = Arcane::ForeignModules::Classify("ReShade64.dll");
    REQUIRE(reshade.has_value());
    CHECK(reshade->tier == 2);
}

TEST_CASE("foreign modules: only GTIII-OSD64-VK.dll blocks windowed Vulkan", "[foreign-modules]")
{
    // The D3D12 and GL siblings are the over-release the reference armor
    // absorbs. Nahimic is a present hook, not the loader fast-fail.
    const std::vector<std::string> tolerated = {
        "GTIII-OSD64.dll", "GTIII-OSD64-GL.dll", "NahimicOSD.dll", "AudioDevProps2.dll",
        "vulkan-1.dll", "GTIII-OSD64-VK.dll.bak",
    };
    const std::vector<std::string> none;
    const std::vector<std::string> hooked = { "ntdll.dll", "gtiii-osd64-vk.dll" };
    CHECK_FALSE(Arcane::ForeignModules::WindowedVulkanBlocker(none).has_value());
    CHECK_FALSE(Arcane::ForeignModules::WindowedVulkanBlocker(tolerated).has_value());

    const auto blocked = Arcane::ForeignModules::WindowedVulkanBlocker(hooked);
    REQUIRE(blocked.has_value());
    CHECK(blocked->module == "gtiii-osd64-vk.dll");
    CHECK(blocked->tier == 1);
    CHECK(blocked->remedy.find("Blacklist") != std::string::npos);
}

TEST_CASE("foreign modules: a normal module list yields no match -- exact base names, never substrings",
          "[foreign-modules]")
{
    const std::vector<std::string> ordinary = {
        "ArcaneTests.exe", "ntdll.dll", "KERNEL32.DLL", "KERNELBASE.dll", "d3d12.dll",
        "D3D12Core.dll", "d3d12SDKLayers.dll", "dxgi.dll", "DXGIDebug.dll", "nvwgf2umx.dll",
        "ArcaneCore.dll", "ArcaneClient.dll", "dxcompiler.dll", "vulkan-1.dll",
        // Near misses: a substring of a table name is NOT the table name.
        "MyNahimicOSD.dll", "GTIII-OSD64.exe", "RTSSHooks64.dll.bak",
    };

    for (const std::string& name : ordinary)
    {
        INFO("module " << name);
        CHECK_FALSE(Arcane::ForeignModules::Classify(name).has_value());
    }
    CHECK(Arcane::ForeignModules::MatchAll(ordinary).empty());
}

TEST_CASE("foreign modules: MatchAll keeps the process's order and reports a module once however often it is listed",
          "[foreign-modules]")
{
    const std::vector<std::string> loaded = {
        "ntdll.dll", "GameOverlayRenderer64.dll", "d3d12.dll", "GTIII-OSD64.dll",
        "gtiii-osd64.dll",   // the same module under another case: one match, not two
        "NahimicOSD.dll",
    };

    const std::vector<Arcane::ForeignModules::Match> matches = Arcane::ForeignModules::MatchAll(loaded);
    REQUIRE(matches.size() == 3);
    CHECK(matches[0].module == "GameOverlayRenderer64.dll");
    CHECK(matches[0].tier == 2);
    CHECK(matches[1].module == "GTIII-OSD64.dll");
    CHECK(matches[1].tier == 1);
    CHECK(matches[2].module == "NahimicOSD.dll");
    CHECK(matches[2].tier == 1);
}

TEST_CASE("foreign modules: Tier1Names joins only the Tier 1 modules, in order, and is empty on a clean list",
          "[foreign-modules]")
{
    const std::vector<std::string> loaded = { "nvspcap64.dll", "GTIII-OSD64.dll", "NahimicOSD.dll" };
    const std::vector<Arcane::ForeignModules::Match> matches = Arcane::ForeignModules::MatchAll(loaded);
    CHECK(Arcane::ForeignModules::Tier1Names(matches) == "GTIII-OSD64.dll, NahimicOSD.dll");

    const std::vector<std::string> onlyTier2 = { "nvspcap64.dll", "DiscordHook64.dll" };
    CHECK(Arcane::ForeignModules::Tier1Names(Arcane::ForeignModules::MatchAll(onlyTier2)).empty());
    CHECK(Arcane::ForeignModules::Tier1Names({}).empty());
}

TEST_CASE("foreign modules: every table row carries a product; every Tier 1 row carries a consequence and a remedy",
          "[foreign-modules]")
{
    // The WARN line is product + consequence + remedy. A row missing any of
    // the three would warn a desk without telling it what to do -- this is
    // the schema check on the table itself, so a future row cannot ship
    // half-filled.
    const auto table = Arcane::ForeignModules::Table();
    REQUIRE_FALSE(table.empty());
    for (const Arcane::ForeignModules::Entry& row : table)
    {
        INFO("table row " << row.module);
        CHECK_FALSE(row.module.empty());
        CHECK_FALSE(row.product.empty());
        CHECK((row.tier == 1 || row.tier == 2));
        if (row.tier == 1)
        {
            CHECK_FALSE(row.consequence.empty());
            CHECK_FALSE(row.remedy.empty());
        }
        else
        {
            CHECK(row.consequence.empty());
            CHECK(row.remedy.empty());
        }
        // Every row must be reachable through the matcher under its own
        // spelling -- a row nothing can match is dead weight.
        const auto self = Arcane::ForeignModules::Classify(row.module);
        REQUIRE(self.has_value());
        CHECK(self->tier == row.tier);
        CHECK(self->product == row.product);
    }
}

TEST_CASE("foreign modules: OriginOf tells this host's own tree, Windows' tree and everything else apart, "
          "case- and slash-insensitively",
          "[foreign-modules]")
{
    using Arcane::ForeignModules::Origin;
    using Arcane::ForeignModules::OriginOf;
    const std::vector<std::string> owned = { "D:\\dev\\starworks\\Arcane\\bin\\Debug-windows-x86_64-md\\ArcaneEditor" };
    const std::string_view          windows = "C:\\Windows";

    // Ours: the exe's own tree, subdirectories included (the vendored Agility
    // runtime lives in <exedir>/D3D12/), whatever case and slashes the loader
    // reports.
    CHECK(OriginOf("D:\\dev\\starworks\\Arcane\\bin\\Debug-windows-x86_64-md\\ArcaneEditor\\ArcaneClient.dll", owned, windows) == Origin::Owned);
    CHECK(OriginOf("D:\\dev\\starworks\\Arcane\\bin\\Debug-windows-x86_64-md\\ArcaneEditor\\D3D12\\D3D12Core.dll", owned, windows) == Origin::Owned);
    CHECK(OriginOf("d:/DEV/starworks/arcane/bin/debug-windows-x86_64-md/arcaneeditor/ArcaneCore.dll", owned, windows) == Origin::Owned);

    // The OS: everything under the Windows directory, the driver store included.
    CHECK(OriginOf("C:\\Windows\\System32\\ntdll.dll", owned, windows) == Origin::System);
    CHECK(OriginOf("c:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_x\\nvwgf2umx.dll", owned, windows) == Origin::System);

    // Everything else got into the process from outside: foreign by
    // construction, catalogued or not.
    CHECK(OriginOf("C:\\Program Files\\ASUS\\GPU TweakIII\\GTIII-OSD64.dll", owned, windows) == Origin::Foreign);
    CHECK(OriginOf("C:\\ProgramData\\A-Volute\\Modules\\NahimicOSD.dll", owned, windows) == Origin::Foreign);

    // A prefix is not a tree: a sibling directory whose name merely starts
    // the same way is outside both.
    CHECK(OriginOf("C:\\Windows2\\x.dll", owned, windows) == Origin::Foreign);
    CHECK(OriginOf("D:\\dev\\starworks\\Arcane\\bin\\Debug-windows-x86_64-md\\ArcaneEditorOld\\x.dll", owned, windows) == Origin::Foreign);

    // No path at all cannot be placed: it is not called foreign on no evidence.
    CHECK(OriginOf("", owned, windows) == Origin::Unknown);
}

TEST_CASE("foreign modules: MatchAll over loaded modules reports table hits by tier, every other outsider as tier 3, "
          "and nothing for our own or Windows' trees",
          "[foreign-modules]")
{
    const std::vector<std::string> owned   = { "D:\\host", "D:\\project\\Binaries" };
    const std::string_view          windows = "C:\\Windows";
    const std::vector<Arcane::ForeignModules::LoadedModule> loaded = {
        { "ArcaneEditor.exe",     "D:\\host\\ArcaneEditor.exe" },
        { "ntdll.dll",            "C:\\Windows\\System32\\ntdll.dll" },
        // The game module: loaded by us, from the project's tree (NoteOwned).
        { "Aphelyon.dll",         "D:\\project\\Binaries\\Aphelyon.dll" },
        { "GTIII-OSD64.dll",      "C:\\Program Files\\ASUS\\GPU TweakIII\\GTIII-OSD64.dll" },
        // The table outranks the path: a Tier 1 module is Tier 1 wherever it sits.
        { "AudioDevProps2.dll",   "C:\\Windows\\System32\\AudioDevProps2.dll" },
        { "nvspcap64.dll",        "C:\\Program Files\\NVIDIA Corporation\\NVIDIA app\\nvspcap64.dll" },
        // Not catalogued: STILL reported -- the list decorates, it does not gate.
        { "SomeNewOverlay64.dll", "C:\\Program Files\\Vendor\\Overlay\\SomeNewOverlay64.dll" },
        // A shell extension a file dialog pulled in: foreign too, and said so.
        { "7-zip.dll",            "C:\\Program Files\\7-Zip\\7-zip.dll" },
        // A module the enumerator could not path: skipped, not accused.
        { "mystery.dll",          "" },
    };

    const std::vector<Arcane::ForeignModules::Match> m = Arcane::ForeignModules::MatchAll(loaded, owned, windows);
    REQUIRE(m.size() == 5);
    CHECK(m[0].module == "GTIII-OSD64.dll");
    CHECK(m[0].tier == 1);
    CHECK(m[0].path == "C:\\Program Files\\ASUS\\GPU TweakIII\\GTIII-OSD64.dll");
    CHECK(m[1].module == "AudioDevProps2.dll");
    CHECK(m[1].tier == 1);
    CHECK(m[2].module == "nvspcap64.dll");
    CHECK(m[2].tier == 2);
    CHECK(m[3].module == "SomeNewOverlay64.dll");
    CHECK(m[3].tier == Arcane::ForeignModules::kTierUncatalogued);
    CHECK(m[3].product.empty());
    CHECK(m[3].consequence.empty());
    CHECK(m[3].remedy.empty());
    CHECK(m[3].path == "C:\\Program Files\\Vendor\\Overlay\\SomeNewOverlay64.dll");
    CHECK(m[4].module == "7-zip.dll");
    CHECK(m[4].tier == Arcane::ForeignModules::kTierUncatalogued);

    // Tier1Names still names only the catalogued Tier 1 rows.
    CHECK(Arcane::ForeignModules::Tier1Names(m) == "GTIII-OSD64.dll, AudioDevProps2.dll");
}

TEST_CASE("foreign modules: NoteOwned makes a loaded module's directory one of ours; the exe's own directory always is",
          "[foreign-modules]")
{
    const std::vector<std::string> before = Arcane::ForeignModules::OwnedRoots();
    REQUIRE_FALSE(before.empty());

    // The exe's own directory is owned without anybody saying so: the test
    // binary resolves as ours against the roots as they stand.
    const std::vector<Arcane::ForeignModules::LoadedModule> live = Arcane::ForeignModules::EnumerateProcessModules();
    const auto exe = std::find_if(live.begin(), live.end(), [](const Arcane::ForeignModules::LoadedModule& m) {
        return Lower(m.name) == Arcane::Platform::ExecutableFileName("arcanetests");   // .exe on Windows only
    });
    REQUIRE(exe != live.end());
    REQUIRE_FALSE(exe->path.empty());
    CHECK(Arcane::ForeignModules::OriginOf(exe->path, before, Arcane::ForeignModules::SystemRoot())
          == Arcane::ForeignModules::Origin::Owned);

    // Module::Load's hook: the DIRECTORY of what we loaded joins the roots,
    // so a game module under <project>/Binaries is never accused.
    Arcane::ForeignModules::NoteOwned("Q:\\some\\project\\Binaries\\Game.dll");
    const std::vector<std::string> after = Arcane::ForeignModules::OwnedRoots();
    CHECK(after.size() == before.size() + 1);
    CHECK(Arcane::ForeignModules::OriginOf("Q:\\some\\project\\Binaries\\Other.dll", after, "C:\\Windows")
          == Arcane::ForeignModules::Origin::Owned);

    // The same directory under another spelling adds nothing.
    Arcane::ForeignModules::NoteOwned("q:/some/project/binaries/Game2.dll");
    CHECK(Arcane::ForeignModules::OwnedRoots().size() == after.size());
}

TEST_CASE("foreign modules: the live enumeration sees this process's own modules with their paths, and a scan is remembered",
          "[foreign-modules]")
{
    // No device, no window: EnumProcessModulesEx on the test process itself.
    // ArcaneCore.dll is where the enumerator lives, so it is necessarily
    // loaded; the exe is module zero on every Windows process.
    const std::vector<Arcane::ForeignModules::LoadedModule> modules = Arcane::ForeignModules::EnumerateProcessModules();
    REQUIRE_FALSE(modules.empty());
    CHECK(ContainsInsensitive(modules, Arcane::Platform::SharedLibraryFileName("ArcaneCore").c_str()));   // libArcaneCore.so on ELF
    CHECK(ContainsInsensitive(modules, Arcane::Platform::ExecutableFileName("ArcaneTests").c_str()));
    // ONE assertion however many modules the process holds: how many are
    // loaded depends on which cases the random order ran first, and a
    // per-module CHECK made the committed assertion baseline seed-sensitive.
    std::vector<std::string> unpathed;
    for (const Arcane::ForeignModules::LoadedModule& m : modules)
        if (m.path.empty())
            unpathed.push_back(m.name);
    INFO("modules with no path: " << Join(unpathed));
    CHECK(unpathed.empty());

    // The Windows directory is a real, non-empty root, and the OS's own
    // loader DLL resolves under it.
    const std::string systemRoot = Arcane::ForeignModules::SystemRoot();
    REQUIRE_FALSE(systemRoot.empty());
    CHECK(Arcane::ForeignModules::OriginOf(systemRoot + "\\System32\\ntdll.dll", {}, systemRoot)
          == Arcane::ForeignModules::Origin::System);

    // A scan is the enumeration matched, and it is REMEMBERED: LastScan hands
    // back exactly what the most recent Scan found, without enumerating again
    // -- a crash-report writer reads it from inside an exception filter,
    // where taking the loader lock is not an option. Every row it carries is
    // tiered and pathed, whatever this desk has injected.
    const std::vector<Arcane::ForeignModules::Match> scanned = Arcane::ForeignModules::Scan();
    // Same rule as above: what a scan finds depends on what this desk has
    // injected and on what the random order loaded, so the rows collapse
    // into one assertion each.
    std::vector<std::string> badScanRows;
    for (const Arcane::ForeignModules::Match& m : scanned)
        if (!(m.tier == 1 || m.tier == 2 || m.tier == Arcane::ForeignModules::kTierUncatalogued) || m.path.empty())
            badScanRows.push_back(m.module);
    INFO("scanned rows untiered or unpathed: " << Join(badScanRows));
    CHECK(badScanRows.empty());
    const auto remembered = Arcane::ForeignModules::LastScan();
    REQUIRE(remembered.has_value());
    REQUIRE(remembered->size() == scanned.size());
    std::vector<std::string> mismatched;
    for (std::size_t i = 0; i < scanned.size(); ++i)
        if ((*remembered)[i].module != scanned[i].module || (*remembered)[i].tier != scanned[i].tier)
            mismatched.push_back(scanned[i].module);
    INFO("remembered rows differing from the scan: " << Join(mismatched));
    CHECK(mismatched.empty());
}

TEST_CASE("foreign modules: Report says each module once per process -- a second call has nothing new",
          "[foreign-modules]")
{
    // Whatever this desk has injected (possibly nothing in a console test
    // process) is reported by the first call in the process; every later
    // call finds it already said. The ONE line per module is the contract
    // the log relies on -- never per device, never per frame.
    (void)Arcane::ForeignModules::Report();
    const std::vector<Arcane::ForeignModules::Match> again = Arcane::ForeignModules::Report();
    CHECK(again.empty());
}
