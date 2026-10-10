// Plugin load diagnoses WHICH of the three failure causes occurred. Before this,
// LoadLibrary failure, a missing export, and an ABI mismatch all collapsed into
// one bool and one generic log line. ([plugin])
//
// The unit-level cases below exercise Plugin::Load/Module::Load directly (the
// resolve-cause plumbing). The integration cases at the bottom drive the actual
// deliverable -- PluginHost's load sites publishing/clearing through the engine
// Diagnostics seam -- via a raw capture sink, same pattern as
// DiagnosticSeamTest.cpp's Capture/CaptureSink.

#include <algorithm>
#include <cstring>
#include "Helpers/ModuleNames.hpp"   // fixture module file names per platform
#include <Arcane/Platform/Platform.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>   // IMAGE_* PE structs for the synthetic-image case
#endif

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Plugin/Plugin.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Plugin/PluginHost.hpp>

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"   // the SAME Pulse type the plugin uses

#include <Astra/Registry/Registry.hpp>

using Arcane::HotReloadTest::Pulse;
using Arcane::HotReloadTest::RoleCounters;

namespace
{
    // Records every (key, diagnostic-set) call the engine seam forwards, in
    // order -- lets a test assert the exact key PluginHost publishes under
    // ("plugin:<name>") as well as the content, which DiagnosticStore's
    // Snapshot()/Filtered() cannot: they flatten across keys and discard the
    // key string entirely.
    struct Capture
    {
        std::vector<std::pair<std::string, std::vector<Arcane::Diagnostic>>> calls;
    };

    void CaptureSink(std::string_view key, std::span<const Arcane::Diagnostic> diags, void* user)
    {
        auto* c = static_cast<Capture*>(user);
        c->calls.emplace_back(std::string(key),
                              std::vector<Arcane::Diagnostic>(diags.begin(), diags.end()));
    }

    // Detaches the sink however the case exits. A failed REQUIRE throws past
    // the case's own SetSink(nullptr, nullptr), which left `cap` (a stack
    // local) installed: the NEXT Publish from any later case then wrote
    // through a dangling pointer -- the Linux port's ArcaneTests SIGSEGV
    // (inventory 2026-10-01 L6). The explicit detach lines stay; this is the
    // backstop.
    struct ScopedCaptureSink
    {
        explicit ScopedCaptureSink(Capture& cap) { Arcane::Diagnostics::SetSink(&CaptureSink, &cap); }
        ~ScopedCaptureSink() { Arcane::Diagnostics::SetSink(nullptr, nullptr); }
        ScopedCaptureSink(const ScopedCaptureSink&) = delete;
        ScopedCaptureSink& operator=(const ScopedCaptureSink&) = delete;
    };

    // The sets published under exactly `key`, in order. A module (re)load also
    // republishes "config.cvars" (settings spec s4.8), so a test that pins
    // PluginHost's own key reads this, not the whole call list.
    std::vector<std::vector<Arcane::Diagnostic>> Under(const Capture& c, std::string_view key)
    {
        std::vector<std::vector<Arcane::Diagnostic>> out;
        for (const auto& [k, diags] : c.calls)
            if (k == key) out.push_back(diags);
        return out;
    }
}

TEST_CASE("A DLL that cannot be loaded reports the OS error", "[plugin][diagnostics]")
{
    const std::optional<Arcane::Module> m =
        Arcane::Module::Load("this-path-does-not-exist-arcane.dll");
    CHECK_FALSE(m.has_value());
    CHECK_FALSE(Arcane::Module::LastLoadError().empty());   // the OS reason, captured
}

TEST_CASE("An ABI-mismatched plugin reports BOTH version numbers", "[plugin][diagnostics]")
{
    // HotReloadPluginBad exports ABI = kGamePluginABIVersion + 999 on purpose
    // (ArcaneTests/plugins/HotReloadPlugin.cpp:4,48), so this is unambiguously the
    // AbiMismatch cause -- not a missing export and not a load failure.
    Arcane::PluginResolveError error;
    auto plugin = Arcane::Plugin::Load(std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginBad")), &error);

    CHECK_FALSE(plugin.has_value());
    CHECK(error.kind == Arcane::PluginResolveError::Kind::AbiMismatch);
    CHECK(error.pluginAbi == Arcane::kGamePluginABIVersion + 999);
    CHECK(error.engineAbi == Arcane::kGamePluginABIVersion);
}

TEST_CASE("A missing required export is named", "[plugin][diagnostics]")
{
    // ArcaneClient.dll itself loads fine as a module but exports none of the
    // plugin entry points -- the cheapest real MissingExport case in-tree.
    // (libArcaneClient.so on ELF: SharedLibraryFileName spells it.)
    Arcane::PluginResolveError error;
    auto plugin = Arcane::Plugin::Load(std::filesystem::path(Arcane::Platform::SharedLibraryFileName("ArcaneClient")), &error);

    CHECK_FALSE(plugin.has_value());
    CHECK(error.kind == Arcane::PluginResolveError::Kind::MissingExport);
    CHECK(error.symbol == std::string(Arcane::PluginEntry::kABIVersion));   // the first checked

    // The refusal must not touch the LIVE ArcaneClient.dll's registrations:
    // that image was already mapped, so this Load ran none of its statics and
    // has nothing of its own to drop. (The S1 gate caught the by-name
    // UnregisterModule("ArcaneClient") here wiping render.meshCull, which made
    // CVarRegistryTest's meshCull case vacuous under one random order.)
#if !defined(ARC_BUILD_DIST)   // the Dev cvar is compiled out of a Dist registry
    const Arcane::CVarHandle meshCull = Arcane::CVarRegistry::Get().Find("render.meshCull");
    CHECK_FALSE(meshCull.IsStale());
    CHECK(Arcane::CVarRegistry::Get().ModuleOf(meshCull) == "ArcaneClient");
#endif
}

TEST_CASE("Module::IsMapped tells an already-mapped image from one this process has not loaded", "[plugin][diagnostics]")
{
    // The query Plugin::Load guards its refused-image UnregisterModule with:
    // ArcaneClient is mapped (this exe links it), the fixture is not until
    // someone loads it, and a missing file is never mapped. SharedLibraryFileName
    // / ModuleFileName spell the running platform's image names.
    const std::filesystem::path client = Arcane::Platform::SharedLibraryFileName("ArcaneClient");
    const std::filesystem::path bad = Arcane::Platform::ModuleFileName("HotReloadPluginBad");
    const std::filesystem::path missing = "this-path-does-not-exist-arcane";
    CHECK(Arcane::Module::IsMapped(client));
    CHECK(Arcane::Module::MappedImage(client).base != nullptr);
    CHECK(Arcane::Module::MappedImage(client).size != 0);
    CHECK_FALSE(Arcane::Module::IsMapped(missing));
    CHECK(Arcane::Module::MappedImage(missing).base == nullptr);

    auto plugin = Arcane::Plugin::Load(bad);
    REQUIRE_FALSE(plugin.has_value());   // ABI-refused, then unmapped by Plugin::Load itself
    CHECK_FALSE(Arcane::Module::IsMapped(bad));
}

#if defined(_WIN32)
TEST_CASE("Module::MappedModules lists mapped images and PinMapped takes a reference by base without loading", "[plugin][diagnostics]")
{
    // The two primitives PluginHost's dependency rule stands on (S2 gate): a
    // host diffs MappedModules() around each load and pins every newly mapped
    // image by its BASE. ArcaneClient.dll stands in for that dependency here --
    // this exe links it, so the pin is a second reference and dropping it must
    // leave the image mapped. The end-to-end case (a Core-only host whose game
    // DLL maps ArcaneClient, then unloads it) is ServerWitnessTest's
    // [witness][server] S1/S3: ArcaneTests always has Client mapped, so no
    // in-process host here can make Client a NEWLY mapped dependency.
    const Arcane::Module::ImageSpan client = Arcane::Module::MappedImage(std::filesystem::path("ArcaneClient.dll"));
    REQUIRE(client.base != nullptr);

    const std::vector<Arcane::Module::MappedModule> mapped = Arcane::Module::MappedModules();
    const auto it = std::find_if(mapped.begin(), mapped.end(), [&](const Arcane::Module::MappedModule& m) {
        return m.image.base == client.base;
    });
    REQUIRE(it != mapped.end());
    CHECK(it->image.size == client.size);
    CHECK(it->path.filename() == std::filesystem::path("ArcaneClient.dll"));

    {
        std::optional<Arcane::Module> pin = Arcane::Module::PinMapped(*it);
        REQUIRE(pin.has_value());
        CHECK(pin->Image().base == client.base);
        CHECK(pin->Image().size == client.size);
    }   // the pin's FreeLibrary drops only its own reference
    CHECK(Arcane::Module::IsMapped(std::filesystem::path("ArcaneClient.dll")));
    CHECK(Arcane::Module::MappedImage(std::filesystem::path("ArcaneClient.dll")).base == client.base);

    // An address inside an image but not at its base names no image to pin:
    // the guard against a base whose range a different module now occupies.
    Arcane::Module::MappedModule inside = *it;
    inside.image.base = static_cast<const unsigned char*>(client.base) + 0x1000;
    CHECK_FALSE(Arcane::Module::PinMapped(inside).has_value());
    CHECK_FALSE(Arcane::Module::PinMapped(Arcane::Module::MappedModule{}).has_value());
}
#endif

TEST_CASE("CRT-flavor scan classifies in-tree binaries without loading them", "[plugin][diagnostics]")
{
    // HotReloadPluginBad.dll and ArcaneClient.dll are built by THIS build, so
    // their CRT family is the test's own -- the scan must agree with the
    // compile-time truth in both configurations. (A genuine cross-flavor DLL
    // cannot exist in-tree; the synthetic-image case below covers the other
    // family's detection path.)
#if ARC_PLATFORM_WINDOWS
#if defined(_DEBUG)
    constexpr auto expected = Arcane::CrtFlavor::Debug;
#else
    constexpr auto expected = Arcane::CrtFlavor::Release;
#endif
    std::string matched;
    CHECK(Arcane::Module::ScanFileCrtFlavor(Arcane::Test::ModuleFile("HotReloadPluginBad"), &matched) == expected);
    CHECK_FALSE(matched.empty());
    CHECK(Arcane::Module::ScanFileCrtFlavor("ArcaneClient.dll") == expected);
#else
    // The CRT-flavor split is a PE import-table fact (ucrtbased vs ucrtbase);
    // an ELF image links the one libstdc++/glibc. Module.hpp's contract is
    // "Windows-only; always Unknown elsewhere" -- the fail-OPEN verdict
    // Plugin::Load ignores -- so that is what the in-tree binaries must read.
    std::string matched;
    CHECK(Arcane::Module::ScanFileCrtFlavor(Arcane::Test::ModuleFile("HotReloadPluginBad"), &matched) == Arcane::CrtFlavor::Unknown);
    CHECK(matched.empty());
    CHECK(Arcane::Module::ScanFileCrtFlavor(Arcane::Platform::SharedLibraryFileName("ArcaneClient")) == Arcane::CrtFlavor::Unknown);
#endif
}

TEST_CASE("CRT-flavor scan yields Unknown for missing, empty, and non-PE input",
          "[plugin][diagnostics]")
{
    CHECK(Arcane::Module::ScanFileCrtFlavor("this-path-does-not-exist-arcane.dll") ==
          Arcane::CrtFlavor::Unknown);
    CHECK(Arcane::Module::DetectCrtFlavorFromImage(nullptr, 0) == Arcane::CrtFlavor::Unknown);

    const unsigned char junk[] = { 'n', 'o', 't', ' ', 'a', ' ', 'P', 'E' };
    CHECK(Arcane::Module::DetectCrtFlavorFromImage(junk, sizeof junk) == Arcane::CrtFlavor::Unknown);

    // A truthful DOS magic followed by garbage must be Unknown, not a fault.
    unsigned char truncated[64] = { 'M', 'Z' };
    CHECK(Arcane::Module::DetectCrtFlavorFromImage(truncated, sizeof truncated) ==
          Arcane::CrtFlavor::Unknown);
}

#if defined(_WIN32)
namespace
{
    // The smallest well-formed PE32+ image whose import table names exactly
    // `importName` -- lets the classifier's Debug path run in a Release test
    // build (and vice versa), which no real in-tree DLL can.
    std::vector<unsigned char> SyntheticPeImporting(std::string_view importName)
    {
        std::vector<unsigned char> img(0x600, 0);
        const auto put = [&](std::size_t off, const auto& v)
        { std::memcpy(img.data() + off, &v, sizeof v); };

        IMAGE_DOS_HEADER dos{};
        dos.e_magic  = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x40;
        put(0, dos);

        const DWORD sig = IMAGE_NT_SIGNATURE;
        put(0x40, sig);

        IMAGE_FILE_HEADER fh{};
        fh.Machine              = IMAGE_FILE_MACHINE_AMD64;
        fh.NumberOfSections     = 1;
        fh.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        put(0x44, fh);

        IMAGE_OPTIONAL_HEADER64 oh{};
        oh.Magic               = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        oh.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = 0x1000;
        oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size =
            2 * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        const std::size_t ohOff = 0x44 + sizeof(IMAGE_FILE_HEADER);
        put(ohOff, oh);

        IMAGE_SECTION_HEADER sh{};
        sh.VirtualAddress   = 0x1000;
        sh.Misc.VirtualSize = 0x200;
        sh.SizeOfRawData    = 0x200;
        sh.PointerToRawData = 0x400;
        put(ohOff + sizeof(IMAGE_OPTIONAL_HEADER64), sh);

        // Section raw data at 0x400 == RVA 0x1000: descriptor[0], the all-zero
        // terminator, then the import-name string at RVA 0x1030.
        IMAGE_IMPORT_DESCRIPTOR desc{};
        desc.Name = 0x1030;
        put(0x400, desc);
        std::memcpy(img.data() + 0x430, importName.data(), importName.size());
        return img;
    }
}

TEST_CASE("CRT-flavor detection classifies both families, case-insensitively",
          "[plugin][diagnostics]")
{
    std::string matched;

    const auto dbg = SyntheticPeImporting("ucrtbased.dll");
    CHECK(Arcane::Module::DetectCrtFlavorFromImage(dbg.data(), dbg.size(), &matched) ==
          Arcane::CrtFlavor::Debug);
    CHECK(matched == "ucrtbased.dll");

    const auto rel = SyntheticPeImporting("VCRUNTIME140.dll");   // mixed case on purpose
    CHECK(Arcane::Module::DetectCrtFlavorFromImage(rel.data(), rel.size(), &matched) ==
          Arcane::CrtFlavor::Release);
    CHECK(matched == "VCRUNTIME140.dll");

    const auto ucrt = SyntheticPeImporting("api-ms-win-crt-runtime-l1-1-0.dll");
    CHECK(Arcane::Module::DetectCrtFlavorFromImage(ucrt.data(), ucrt.size()) ==
          Arcane::CrtFlavor::Release);

    const auto other = SyntheticPeImporting("KERNEL32.dll");   // imports, but no CRT evidence
    CHECK(Arcane::Module::DetectCrtFlavorFromImage(other.data(), other.size()) ==
          Arcane::CrtFlavor::Unknown);
}
#endif

TEST_CASE("PluginHost::Load publishes the ABI-mismatch diagnostic under plugin:<name>",
          "[plugin][diagnostics][hotreload]")
{
    // The actual deliverable: PluginHost's primary-load-failure site, not just
    // the resolve plumbing the cases above cover in isolation.
    Arcane::Runtime rt(Arcane::Test::Process());

    Capture cap;
    const ScopedCaptureSink sinkGuard(cap);

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginBad")));
    host.AttachRuntime(rt);
    CHECK_FALSE(host.Load());

    REQUIRE(cap.calls.size() == 1);
    CHECK(cap.calls[0].first == "plugin:HotReloadPluginBad");   // stem of the SOURCE path, not the temp copy
    REQUIRE(cap.calls[0].second.size() == 1);

    const Arcane::Diagnostic& d = cap.calls[0].second[0];
    CHECK(d.severity == Arcane::DiagSeverity::Error);
    CHECK(d.scope == Arcane::DiagScope::Plugin);
    CHECK(d.code == "plugin.abi.mismatch");
    CHECK(d.locator.kind == Arcane::DiagLocator::Kind::File);
    CHECK(d.locator.file == Arcane::Test::ModuleFile("HotReloadPluginBad"));
    // BOTH ABI numbers must be in the message, not just latched in the struct.
    CHECK(d.message.find(std::to_string(Arcane::kGamePluginABIVersion + 999)) != std::string::npos);
    CHECK(d.message.find(std::to_string(Arcane::kGamePluginABIVersion)) != std::string::npos);

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
    host.Unload();
}

TEST_CASE("A failed reload publishes the cause; the next successful reload retracts it",
          "[plugin][diagnostics][hotreload]")
{
    // Guard against ordering contamination: ensure we start with genuine V1 content
    // (same idiom as PluginHostTest.cpp's "ABI mismatch rolls back to last-good").
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();   // engine sees the type, mirrors PluginHostTest.cpp
    rt.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());   // good load first -- its Diagnostics::Clear happens BEFORE the sink below

    Capture cap;
    const ScopedCaptureSink sinkGuard(cap);

    // Swap in the ABI-mismatched image and force a reload: fails, rolls back to
    // last-good, and (this task's deliverable) publishes the real cause under
    // plugin:HotReloadPluginV1 even though the session survives on the old image.
    std::filesystem::copy_file(Arcane::Test::ModuleFile("HotReloadPluginBad"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);
    CHECK_FALSE(host.ForceReload());
    CHECK(host.IsLoaded());   // still on last-good

    std::vector<std::vector<Arcane::Diagnostic>> plugin = Under(cap, "plugin:HotReloadPluginV1");
    REQUIRE(plugin.size() == 1);
    REQUIRE(plugin[0].size() == 1);
    CHECK(plugin[0][0].code == "plugin.abi.mismatch");

    // Swap the good image back and reload again: succeeds, and retracts the row
    // via Diagnostics::Clear -- a Publish with an empty set for the SAME key.
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);
    CHECK(host.ForceReload());

    plugin = Under(cap, "plugin:HotReloadPluginV1");
    REQUIRE(plugin.size() == 2);
    CHECK(plugin[1].empty());

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
    host.Unload();

    // restore the fixture for re-runs
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);
}
