#include <catch2/catch_test_macros.hpp>
#include "Helpers/ModuleNames.hpp"   // fixture module file names per platform

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Plugin/Plugin.hpp>

#include <Arcane/Plugin/PluginABI.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

TEST_CASE("ArcaneRuntime Module loads a dynamic library and resolves symbols", "[host][module]")
{
    // A RAW Module::Load has no Plugin to own the cvar, command and callback
    // the fixture's statics register inside LoadLibrary (settings spec s4.4;
    // CVarModuleLifetimeTest). Tag them here and drop them before the image
    // unmaps, or hotreload.ping would point into freed code for the rest of
    // the run. ModuleFile spells the fixture per platform.
    constexpr std::string_view kRawModule = "RuntimeModulePluginTest.raw";
    std::optional<Arcane::Module> module;
    {
        const Arcane::CVarModuleScope scope(kRawModule);
        module = Arcane::Module::Load(std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    }

    REQUIRE(module.has_value());
    CHECK(module->IsLoaded());
    CHECK(module->Path().filename() == Arcane::Test::ModuleFile("HotReloadPluginV1"));
    CHECK(module->Symbol(Arcane::PluginEntry::kABIVersion) != nullptr);
    CHECK(module->Symbol("Definitely_Not_An_Exported_Symbol") == nullptr);
    CHECK(Arcane::CVarRegistry::Get().ModuleOf(Arcane::CVarRegistry::Get().Find("hotreload.step")) == kRawModule);

    Arcane::CVarRegistry::Get().UnregisterModule(kRawModule);
    CHECK(Arcane::CVarRegistry::Get().Find("hotreload.step").IsStale());
}

TEST_CASE("ArcaneRuntime Plugin resolves the current game plugin ABI", "[host][plugin]")
{
    auto plugin = Arcane::Plugin::Load(std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));

    REQUIRE(plugin.has_value());
    CHECK(plugin->IsLoaded());
    REQUIRE(plugin->VTable().ABIVersion != nullptr);
    CHECK(plugin->VTable().ABIVersion() == Arcane::kGamePluginABIVersion);
    CHECK(plugin->VTable().Init != nullptr);
    CHECK(plugin->VTable().Shutdown != nullptr);
    CHECK(plugin->VTable().FixedUpdate != nullptr);
    CHECK(plugin->VTable().Update != nullptr);
    CHECK(plugin->VTable().SaveState != nullptr);
    CHECK(plugin->VTable().LoadState != nullptr);
}

TEST_CASE("ArcaneRuntime Plugin rejects modules that do not satisfy the game plugin ABI", "[host][plugin]")
{
    auto plugin = Arcane::Plugin::Load(std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginBad")));

    CHECK_FALSE(plugin.has_value());
}
