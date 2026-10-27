// Module capture (settings spec s4.3, O1): a registration names its module --
// the innermost CVarModuleScope, else the caller's ARC_MODULE_NAME. [cvar]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarModule.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Plugin/Module.hpp>   // MappedModules: this test binary's own image span

#include "Helpers/CVarTestDesc.hpp"
#include "Helpers/SettingsProbe.hpp"

#include <cstdint>
#include <string_view>

using namespace Arcane;

namespace S1ModuleTest
{
    ARC_CVAR(cvar_moduleProbe, "tests.moduleProbe", std::int32_t, 3,
             .help = "Module-capture probe (CVarModuleTest).");
}

TEST_CASE("module capture: the scope, else the caller's ARC_MODULE_NAME; ArcaneCore names itself", "[cvar]")
{
    CHECK(std::string_view(ARC_MODULE_NAME_STRING) == "ArcaneTests");
    CHECK(CVarRegistry::ScopedModule().empty());
    CHECK(Detail::CallerModule() == "ArcaneTests");
    CHECK(CVarRegistry::CurrentModule() == "ArcaneCore");
    CHECK(CVarRegistry::Get().ModuleOf(S1ModuleTest::cvar_moduleProbe.Handle()) == "ArcaneTests");

    CVarRegistry reg;
    CVarHandle inner, outer;
    {
        const CVarModuleScope a("GameA");
        {
            const CVarModuleScope b("GameB");
            CHECK(Detail::CallerModule() == "GameB");
            inner = reg.Register(Test::Desc("cap.inner", CVarValue::Int32(0), Audience::Game, CVarFlags::None, {}));
        }
        CHECK(CVarRegistry::ScopedModule() == "GameA");
        outer = reg.Register(Test::Desc("cap.outer", CVarValue::Int32(0), Audience::Game, CVarFlags::None, {}));
    }
    const CVarHandle bare = reg.Register(Test::Desc("cap.bare", CVarValue::Int32(0), Audience::Game, CVarFlags::None, {}));
    CHECK(reg.ModuleOf(inner) == "GameB");
    CHECK(reg.ModuleOf(outer) == "GameA");
    CHECK(reg.ModuleOf(bare) == "ArcaneCore");       // Register's own fallback: the module it lives in
    CHECK(reg.ModuleOf(reg.Find("server.cheats")) == "ArcaneCore");
}

TEST_CASE("module capture: a module's callbacks leave with it; built-in commands survive any unload", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("cap.engineKnob", CVarValue::Int32(0), Audience::Game, CVarFlags::None, "ArcaneClient"));
    int fires = 0;
    {
        const CVarModuleScope scope("GameA");
        reg.AddCallback(knob, [](CVarHandle, void* u) { ++*static_cast<int*>(u); }, &fires);
    }
    reg.AddCallback(knob, [](CVarHandle, void* u) { *static_cast<int*>(u) += 100; }, &fires);   // unscoped: the host's own
    reg.UnregisterModule("GameA");
    REQUIRE(reg.Set(knob, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(fires == 100);                              // GameA's callback is gone, the host's stays

    reg.UnregisterModule("ArcaneCore");
    CHECK(reg.Execute("cvarlist", CVarContext::Editor).ok);
    CHECK(reg.Execute("cvar_explain cap.engineKnob", CVarContext::Editor).ok);
}

TEST_CASE("an untagged AddCallback whose fn lies in a published module image is attributed to that module", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("img.knob", CVarValue::Int32(0)));
    int fires = 0;
    CVarRegistry::ChangeFn fn = [](CVarHandle, void* u) { ++*static_cast<int*>(u); };
    reg.RegisterModuleImage("img-mod", reinterpret_cast<const void*>(fn), 16);
    reg.AddCallback(knob, fn, &fires);   // no CVarModuleScope
    REQUIRE(reg.Set(knob, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(fires == 1);
    reg.UnregisterModule("img-mod");
    REQUIRE(reg.Set(knob, CVarValue::Int32(2), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(fires == 1);
}

TEST_CASE("UnregisterModuleRange drops callbacks by image address", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("img.rangeKnob", CVarValue::Int32(0)));
    int fires = 0;
    CVarRegistry::ChangeFn fn = [](CVarHandle, void* u) { ++*static_cast<int*>(u); };
    reg.AddCallback(knob, fn, &fires);   // unscoped, unpublished image
    CHECK(reg.UnregisterModuleRange(reinterpret_cast<const void*>(fn), 16) >= 1);
    REQUIRE(reg.Set(knob, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(fires == 0);
}

TEST_CASE("UnregisterModuleRange does not drop another image's same-named registrations", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("img.sharedKnob", CVarValue::Int32(0)));
    int firesA = 0;
    int firesB = 0;
    CVarRegistry::ChangeFn fnA = [](CVarHandle, void* u) { ++*static_cast<int*>(u); };
    CVarRegistry::ChangeFn fnB = [](CVarHandle, void* u) { *static_cast<int*>(u) += 10; };
    REQUIRE(fnA != fnB);
    // Size 1 so the two fake images cannot overlap even if the thunks sit next
    // to each other in this test binary.
    reg.RegisterModuleImage("shared-stem", reinterpret_cast<const void*>(fnA), 1);
    reg.RegisterModuleImage("shared-stem", reinterpret_cast<const void*>(fnB), 1);
    reg.AddCallback(knob, fnA, &firesA);
    reg.AddCallback(knob, fnB, &firesB);
    CHECK(reg.UnregisterModuleRange(reinterpret_cast<const void*>(fnA), 1) >= 1);
    REQUIRE(reg.Set(knob, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(firesA == 0);
    CHECK(firesB == 10);
}

namespace
{
    // The mapped image holding this test's code: the stand-in for a dependency
    // image whose statics ran inside a game load's CVarModuleScope.
    Module::ImageSpan ThisImage()
    {
        const auto* self = reinterpret_cast<const unsigned char*>(&ThisImage);
        for (const Module::MappedModule& m : Module::MappedModules())
        {
            const auto* base = static_cast<const unsigned char*>(m.image.base);
            if (base && self >= base && self < base + m.image.size)
                return m.image;
        }
        return {};
    }
}

TEST_CASE("ReattributeModuleRange leaves a registration outside the range under its old module", "[cvar]")
{
    // Issue #21 Task 2 review (c): modelled on the UnregisterModuleRange case
    // above. Two GameA callbacks; a one-byte span around one of them moves
    // only that one to "Dep".
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("reattr.rangeKnob", CVarValue::Int32(0)));
    int firesA = 0;
    int firesB = 0;
    CVarRegistry::ChangeFn fnA = [](CVarHandle, void* u) { ++*static_cast<int*>(u); };
    CVarRegistry::ChangeFn fnB = [](CVarHandle, void* u) { *static_cast<int*>(u) += 10; };
    REQUIRE(fnA != fnB);
    // Distinct bodies so the two functions cannot be folded into one address.
    static const auto cmdA = +[](std::string_view, void*) { return CommandResult{ true, {} }; };
    static const auto cmdB = +[](std::string_view, void*) { return CommandResult{ true, "b" }; };
    REQUIRE(cmdA != cmdB);
    {
        const CVarModuleScope scope("GameA");
        reg.AddCallback(knob, fnA, &firesA);
        reg.AddCallback(knob, fnB, &firesB);
        // Two commands too: the command loop is separate from the callback loop.
        REQUIRE(reg.RegisterCommand("reattr.cmdA", CVarFlags::None, "Range probe A (CVarModuleTest).", "GameA",
                                    cmdA, nullptr));
        REQUIRE(reg.RegisterCommand("reattr.cmdB", CVarFlags::None, "Range probe B (CVarModuleTest).", "GameA",
                                    cmdB, nullptr));
    }
    CHECK(reg.ReattributeModuleRange(reinterpret_cast<const void*>(fnA), 1, "GameA", "Dep") == 1);
    CHECK(reg.ReattributeModuleRange(reinterpret_cast<const void*>(cmdA), 1, "GameA", "Dep") == 1);

    // fnB lies outside the span: it is still GameA's, so dropping "Dep" leaves
    // it and dropping "GameA" then takes it.
    reg.UnregisterModule("Dep");
    REQUIRE(reg.Set(knob, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(firesA == 0);
    CHECK(firesB == 10);
    CHECK_FALSE(reg.Execute("reattr.cmdA", CVarContext::Editor).ok);   // moved to "Dep", dropped with it
    CHECK(reg.Execute("reattr.cmdB", CVarContext::Editor).ok);         // outside the span: still GameA's

    reg.UnregisterModule("GameA");
    REQUIRE(reg.Set(knob, CVarValue::Int32(2), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(firesB == 10);
    CHECK_FALSE(reg.Execute("reattr.cmdB", CVarContext::Editor).ok);
}

TEST_CASE("ReattributeModuleRange re-names what an image registered under another module's scope", "[cvar]")
{
    // Issue #21 M-P1/M-P2: a dependency a game load maps runs its statics
    // inside the game's CVarModuleScope. Plugin::Load re-attributes them to the
    // dependency, so the game's name drop (a failed load) leaves them and the
    // dependency's own name layers them.
    const Module::ImageSpan self = ThisImage();
    REQUIRE(self.base != nullptr);

    CVarRegistry reg;
    int fires = 0;
    CVarHandle dep, other;
    {
        const CVarModuleScope scope("GameA");
        dep = reg.Register(Test::Desc("reattr.dep", CVarValue::Int32(0), Audience::Game, CVarFlags::None, {}));
        REQUIRE(RegisterSettings<SettingsProbe::ProbeSettings>(reg));   // CallerModule(): the scope, "GameA"
        reg.AddCallback(dep, [](CVarHandle, void* u) { ++*static_cast<int*>(u); }, &fires);
        REQUIRE(reg.RegisterCommand("reattr.cmd", CVarFlags::None, "Reattribute probe (CVarModuleTest).", "GameA",
                                    [](std::string_view, void*) { return CommandResult{ true, {} }; }, nullptr));
    }
    {
        const CVarModuleScope scope("GameB");
        other = reg.Register(Test::Desc("reattr.other", CVarValue::Int32(0), Audience::Game, CVarFlags::None, {}));
    }
    const CVarHandle field = reg.Find("tests.settingsProbe.count");
    REQUIRE(reg.ModuleOf(dep) == "GameA");
    REQUIRE(reg.ModuleOf(field) == "GameA");

    // Outside the range, or another name: nothing moves.
    const std::uint64_t revBefore = reg.Revision();
    CHECK(reg.ReattributeModuleRange(self.base, 0, "GameA", "Dep") == 0);
    CHECK(reg.ReattributeModuleRange(self.base, self.size, "GameA", "GameA") == 0);
    CHECK(reg.ReattributeModuleRange(self.base, self.size, "NoSuchModule", "Dep") == 0);
    CHECK(reg.ModuleOf(dep) == "GameA");
    CHECK(reg.Revision() == revBefore);   // a no-op rename leaves the Settings window's cache alone

    // The cvar, the settings struct AND its field cvars (Register'ed from
    // ArcaneCore, so they follow the struct's factory), the callback, the command.
    CHECK(reg.ReattributeModuleRange(self.base, self.size, "GameA", "Dep") >= 5);
    CHECK(reg.ModuleOf(dep) == "Dep");
    CHECK(reg.ModuleOf(field) == "Dep");
    CHECK(reg.ModuleOf(other) == "GameB");
    CHECK(reg.Revision() > revBefore);    // a real rename must invalidate the editor's by-module filing

    // The game's name drop leaves every one of them.
    reg.UnregisterModule("GameA");
    CHECK_FALSE(reg.Find("reattr.dep").IsStale());
    CHECK_FALSE(reg.Find("tests.settingsProbe.count").IsStale());
    reg.Publish();
    CHECK(reg.SettingsBlock(Astra::TypeID<SettingsProbe::ProbeSettings>::Hash()) != nullptr);
    CHECK(reg.Execute("reattr.cmd", CVarContext::Editor).ok);
    REQUIRE(reg.Set(dep, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(fires == 1);

    // ...and the dependency's own name takes them.
    reg.UnregisterModule("Dep");
    CHECK(reg.Find("reattr.dep").IsStale());
    CHECK(reg.Find("tests.settingsProbe.count").IsStale());
    CHECK_FALSE(reg.Execute("reattr.cmd", CVarContext::Editor).ok);
    CHECK_FALSE(reg.Find("reattr.other").IsStale());
}
