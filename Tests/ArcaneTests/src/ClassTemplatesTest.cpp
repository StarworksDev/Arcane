// ClassTemplates: the PURE text half of Assets -> Create -> C++ Class (the
// third step of the editor<->IDE surface). Name validation and the three
// rendered templates -- Component, System, Plain class -- as strings; no
// filesystem, no ImGui. What the wizard then does with the strings (write,
// register, regenerate, open in VS) lives in EditorApp and is desk-verify.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/ModuleNames.hpp"   // fixture module file names per platform

#include <Project/ClassTemplates.hpp>

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>
#include <Arcane/Project/GameDescriptor.hpp>
#include <Arcane/Sim/Time.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "Helpers/ReferenceProjectDir.hpp"
#include "Helpers/TestTypeContext.hpp"

using namespace Arcane::Editor;

namespace
{
    bool Has(const std::string& text, std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }
}

TEST_CASE("ClassTemplates::ValidateClassName accepts C++ identifiers and refuses everything else with a reason", "[editor]")
{
    CHECK_FALSE(ClassTemplates::ValidateClassName("Health").has_value());
    CHECK_FALSE(ClassTemplates::ValidateClassName("_Private").has_value());
    CHECK_FALSE(ClassTemplates::ValidateClassName("Wave2").has_value());

    CHECK(ClassTemplates::ValidateClassName("").has_value());
    CHECK(ClassTemplates::ValidateClassName("9Lives").has_value());      // leading digit
    CHECK(ClassTemplates::ValidateClassName("Health Bar").has_value());  // whitespace
    CHECK(ClassTemplates::ValidateClassName("my-class").has_value());    // punctuation
    CHECK(ClassTemplates::ValidateClassName("Health.hpp").has_value());  // an extension is not a name
    CHECK(ClassTemplates::ValidateClassName("class").has_value());       // keyword
    CHECK(ClassTemplates::ValidateClassName("namespace").has_value());   // keyword
    CHECK(ClassTemplates::ValidateClassName("Arcane::Health").has_value()); // no qualification
}

TEST_CASE("ClassTemplates::Render Component: a reflected struct in the header, the registrar line in the source", "[editor]")
{
    const ClassTemplates::Rendered r =
        ClassTemplates::Render(ClassTemplates::Kind::Component, "Health", "Aphelyon");

    CHECK(r.headerName == "Health.hpp");
    CHECK(r.sourceName == "Health.cpp");

    // Header: the shape Components.hpp / HotReloadShared.hpp use.
    CHECK(Has(r.header, "#pragma once"));
    CHECK(Has(r.header, "#include <Arcane/Reflection.hpp>"));
    CHECK(Has(r.header, "namespace Aphelyon"));
    CHECK(Has(r.header, "struct Health"));
    CHECK(Has(r.header, "ARC_REFLECT_TYPE(Health)"));
    CHECK(Has(r.header, "ARC_END_REFLECT_TYPE()"));
    CHECK_FALSE(Has(r.header, "ASTRA_"));

    // Source: ONE registrar line, in exactly one TU (a header would register
    // once per including TU), qualified with the project namespace.
    CHECK(Has(r.source, "#include \"Health.hpp\""));
    CHECK(Has(r.source, "#include <Arcane/Plugin/GameComponents.hpp>"));
    CHECK(Has(r.source, "ARC_COMPONENT(Aphelyon::Health)"));
    CHECK(Has(r.source, "ARC_GAME_MODULE"));          // the prologue that drains it
    CHECK_FALSE(Has(r.source, "GamePlugin_Init"));

    // No template token survives, and both files end in a newline.
    CHECK_FALSE(Has(r.header, "{{"));
    CHECK_FALSE(Has(r.source, "{{"));
    CHECK(r.header.back() == '\n');
    CHECK(r.source.back() == '\n');
}

TEST_CASE("ClassTemplates::Render System: a registered header/source pair with safe defaults", "[editor]")
{
    const ClassTemplates::Rendered r =
        ClassTemplates::Render(ClassTemplates::Kind::System, "Movement", "Aphelyon");

    CHECK(r.headerName == "Movement.hpp");
    CHECK(r.sourceName == "Movement.cpp");

    CHECK(Has(r.header, "#pragma once"));
    CHECK(Has(r.header, "namespace Aphelyon"));
    CHECK(Has(r.header, "struct Movement"));
    CHECK(Has(r.header, "#include <Arcane/Scene/TransformSystems.hpp>"));
    CHECK(Has(r.header, "#include <Arcane/Ecs.hpp>"));
    CHECK(Has(r.header, "Arcane::SystemTraits<Arcane::Before<Arcane::TransformPropagationSystem>>"));
    CHECK(Has(r.header, "void operator()(Arcane::Res<Arcane::Time> time)"));
    CHECK_FALSE(Has(r.header, "Astra::"));
    CHECK_FALSE(Has(r.header, "Registry& reg"));

    CHECK(Has(r.source, "#include \"Movement.hpp\""));
    CHECK(Has(r.source, "#include <Arcane/Plugin/GameSystems.hpp>"));
    CHECK(Has(r.source, "ARC_SYSTEM("));
    CHECK(Has(r.source, "Aphelyon::Movement"));
    CHECK(Has(r.source, "Arcane::RoleMask::Both"));
    CHECK(Has(r.source, "Arcane::SystemPhase::FixedUpdate"));
    CHECK(Has(r.source, "ARC_GAME_MODULE discovers"));
    CHECK(Has(r.source, "scheduler order belongs in traits"));
    CHECK_FALSE(Has(r.header, "{{"));
    CHECK_FALSE(Has(r.source, "{{"));
    CHECK(r.header.back() == '\n');
    CHECK(r.source.back() == '\n');
}

TEST_CASE("ClassTemplates::Render System maps every phase and role choice", "[editor]")
{
    struct PhaseCase
    {
        int index;
        const char* spelling;
        bool transformAnchor;
    };

    for (const PhaseCase c : {
             PhaseCase{0, "Arcane::SystemPhase::FixedUpdate", true},
             PhaseCase{1, "Arcane::SystemPhase::Update", false},
         })
    {
        const auto options = ClassTemplates::SystemOptionsForChoiceIndices(c.index, 0);
        const auto rendered = ClassTemplates::Render(
            ClassTemplates::Kind::System, "Movement", "Aphelyon", options);
        CHECK(Has(rendered.source, c.spelling));
        CHECK(Has(rendered.header, "Arcane::Before<Arcane::TransformPropagationSystem>")
              == c.transformAnchor);
        CHECK(Has(rendered.header, "#include <Arcane/Scene/TransformSystems.hpp>")
              == c.transformAnchor);
        CHECK(Has(rendered.header, "scheduler order"));
        CHECK(rendered.header.back() == '\n');
        CHECK(rendered.source.back() == '\n');
        CHECK_FALSE(Has(rendered.header, "{{"));
        CHECK_FALSE(Has(rendered.source, "{{"));
    }

    struct RoleCase { int index; const char* spelling; };
    for (const RoleCase c : {
             RoleCase{0, "Arcane::RoleMask::Both"},
             RoleCase{1, "Arcane::RoleMask::Server"},
             RoleCase{2, "Arcane::RoleMask::Client"},
         })
    {
        const auto options = ClassTemplates::SystemOptionsForChoiceIndices(0, c.index);
        const auto rendered = ClassTemplates::Render(
            ClassTemplates::Kind::System, "Movement", "Aphelyon", options);
        CHECK(Has(rendered.source, c.spelling));
    }
}

TEST_CASE("ClassTemplates::Render System normalizes stale choices and invalid typed options", "[editor]")
{
    STATIC_REQUIRE(ClassTemplates::kSystemPhaseChoiceCount == 2);   // Render retired (F5 plan 1 Task 5)
    STATIC_REQUIRE(ClassTemplates::kSystemRoleChoiceCount == 3);
    CHECK(std::string_view(ClassTemplates::SystemPhaseChoiceLabel(0)) == "Fixed Update");
    CHECK(std::string_view(ClassTemplates::SystemPhaseChoiceLabel(1)) == "Update");
    CHECK(std::string_view(ClassTemplates::SystemPhaseChoiceLabel(2)) == "Fixed Update");   // the retired Render slot
    CHECK(std::string_view(ClassTemplates::SystemPhaseChoiceLabel(99)) == "Fixed Update");
    CHECK(std::string_view(ClassTemplates::SystemRoleChoiceLabel(0)) == "Both");
    CHECK(std::string_view(ClassTemplates::SystemRoleChoiceLabel(1)) == "Server");
    CHECK(std::string_view(ClassTemplates::SystemRoleChoiceLabel(2)) == "Client");
    CHECK(std::string_view(ClassTemplates::SystemRoleChoiceLabel(-1)) == "Both");

    const auto choices = ClassTemplates::SystemOptionsForChoiceIndices(-1, 99);
    CHECK(choices.phase == Arcane::SystemPhase::FixedUpdate);
    CHECK(choices.role == Arcane::RoleMask::Both);
    // A stale index 2 (the retired Render choice) is a Fixed Update system.
    CHECK(ClassTemplates::SystemOptionsForChoiceIndices(2, 0).phase == Arcane::SystemPhase::FixedUpdate);

    ClassTemplates::SystemOptions invalid;
    invalid.phase = static_cast<Arcane::SystemPhase>(255);
    invalid.role  = static_cast<Arcane::RoleMask>(0);
    const auto normalized = ClassTemplates::Render(
        ClassTemplates::Kind::System, "Movement", "Aphelyon", invalid);
    CHECK(Has(normalized.source, "Arcane::SystemPhase::FixedUpdate"));
    CHECK(Has(normalized.source, "Arcane::RoleMask::Both"));

    // The retired Render phase is never written, even when typed in directly:
    // a Render-phase system would register and never run.
    ClassTemplates::SystemOptions render;
    render.phase = Arcane::SystemPhase::Render;
    const auto retired = ClassTemplates::Render(
        ClassTemplates::Kind::System, "Movement", "Aphelyon", render);
    CHECK(Has(retired.source, "Arcane::SystemPhase::FixedUpdate"));
    CHECK_FALSE(Has(retired.source, "Arcane::SystemPhase::Render"));
}

TEST_CASE("ClassTemplates::Render PlainClass: a class in the project namespace with its own .cpp", "[editor]")
{
    const ClassTemplates::Rendered r =
        ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "Inventory", "Aphelyon");

    CHECK(r.headerName == "Inventory.hpp");
    CHECK(r.sourceName == "Inventory.cpp");
    CHECK(Has(r.header, "#pragma once"));
    CHECK(Has(r.header, "namespace Aphelyon"));
    CHECK(Has(r.header, "class Inventory"));
    CHECK(Has(r.source, "#include \"Inventory.hpp\""));
    CHECK(Has(r.source, "namespace Aphelyon"));
    CHECK_FALSE(Has(r.header, "{{"));
    CHECK_FALSE(Has(r.source, "{{"));
}

TEST_CASE("ClassTemplates::Render derives a legal namespace from any project name", "[editor]")
{
    // A manifest name is free text ("My Game", "2D Test"); the namespace it
    // becomes must be an identifier.
    const ClassTemplates::Rendered spaced =
        ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "Thing", "My Game");
    CHECK(Has(spaced.header, "namespace My_Game"));

    const ClassTemplates::Rendered digit =
        ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "Thing", "2D Test");
    CHECK(Has(digit.header, "namespace _2D_Test"));

    const ClassTemplates::Rendered empty =
        ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "Thing", "");
    CHECK(Has(empty.header, "namespace Game"));   // the fallback when the name yields nothing
}

TEST_CASE("ClassTemplates::KindLabel names every kind", "[editor]")
{
    CHECK(std::string(ClassTemplates::KindLabel(ClassTemplates::Kind::Component)) == "Component");
    CHECK(std::string(ClassTemplates::KindLabel(ClassTemplates::Kind::System)) == "System");
    CHECK(std::string(ClassTemplates::KindLabel(ClassTemplates::Kind::PlainClass)) == "Plain class");
    CHECK(static_cast<int>(ClassTemplates::Kind::Count) == 3);
}

// ---- the game descriptor (engine modularization s4) ------------------------------

TEST_CASE("ClassTemplates renders the default game descriptor for the project's module", "[editor][templates]")
{
    const ClassTemplates::DescriptorFile d = ClassTemplates::RenderGameDescriptor("ReferenceGame");
    CHECK(d.fileName == "ReferenceGame.arcmodule");
    CHECK(d.text == Arcane::RenderDefaultGameDescriptor("ReferenceGame"));
    CHECK(Has(d.text, "\"name\": \"ReferenceGame\""));   // name == file stem (build/modules.lua's rule)
    CHECK(Has(d.text, "\"type\": \"Game\""));
    for (const std::string_view module : Arcane::kDefaultGameModules)
        CHECK(Has(d.text, "\"" + std::string(module) + "\""));
    CHECK_FALSE(Has(d.text, "ArcaneEdit"));      // no Editor module reaches a game
    CHECK_FALSE(Has(d.text, "ArcaneVerify"));    // no Developer module either
    CHECK(d.text.back() == '\n');
}

TEST_CASE("ClassTemplates names the game module from the manifest, else the project", "[editor][templates]")
{
    CHECK(ClassTemplates::GameModuleNameFor("ReferenceGame.dll", "ReferenceProject") == "ReferenceGame");
    CHECK(ClassTemplates::GameModuleNameFor("Aphelyon.so", "Aphelyon") == "Aphelyon");
    CHECK(ClassTemplates::GameModuleNameFor("Binaries/Mod.dylib", "X") == "Mod");
    CHECK(ClassTemplates::GameModuleNameFor("", "MyGame") == "MyGame");   // a plugins-only project
}

// ---- the compile proof (input-seam spec s8 T8) --------------------------------
// This file never compiles generated code, so the renders for (Component
// SmokeComponent, System SmokeSystem [FixedUpdate header], System
// SmokeUpdateSystem [Update -- the unanchored Update/Render header], project
// TemplateSmoke) are checked in under Tests/Fixtures/TemplateSmokePlugin and
// built as TemplateSmokePlugin.dll
// (premake5.lua). The first case pins those files to Render(...) byte for
// byte; the build pins that they compile. A template drift fails one or the
// other.

namespace
{
    std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf(); return ss.str();
    }
    std::filesystem::path SmokeDir()
    {
        return Arcane::Test::FindReferenceProjectDir().parent_path() / "Tests" / "Fixtures" / "TemplateSmokePlugin";
    }
}

TEST_CASE("ClassTemplates renders equal the compiled TemplateSmoke sources byte for byte", "[editor][templates]")
{
    const auto component = ClassTemplates::Render(ClassTemplates::Kind::Component, "SmokeComponent", "TemplateSmoke");
    const auto system    = ClassTemplates::Render(ClassTemplates::Kind::System, "SmokeSystem", "TemplateSmoke");
    CHECK(Slurp(SmokeDir() / "SmokeComponent.hpp") == component.header);
    CHECK(Slurp(SmokeDir() / "SmokeComponent.cpp") == component.source);
    CHECK(Slurp(SmokeDir() / "SmokeSystem.hpp")    == system.header);
    CHECK(Slurp(SmokeDir() / "SmokeSystem.cpp")    == system.source);

    ClassTemplates::SystemOptions update;
    update.phase = Arcane::SystemPhase::Update;
    const auto updateSystem = ClassTemplates::Render(ClassTemplates::Kind::System, "SmokeUpdateSystem", "TemplateSmoke", update);
    CHECK(Slurp(SmokeDir() / "SmokeUpdateSystem.hpp") == updateSystem.header);
    CHECK(Slurp(SmokeDir() / "SmokeUpdateSystem.cpp") == updateSystem.source);
    // The two system renders really are the two different header templates.
    CHECK(updateSystem.header.find("TransformPropagationSystem") == std::string::npos);
    CHECK(system.header.find("TransformPropagationSystem") != std::string::npos);
}

TEST_CASE("the rendered component and parameter-style system load as a module and the system runs", "[editor][templates][hotreload]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("TemplateSmokePlugin")));
    REQUIRE(host.AttachRuntime(rt));
    REQUIRE(host.Load());
    // The rendered system registered through ARC_SYSTEM's PARAMETER path...
    bool registered = false, updateRegistered = false;
    for (const Arcane::SystemFactoryEntry& e : Arcane::Test::Process().SystemFactories().Entries())
    {
        if (e.name.find("SmokeSystem") != std::string::npos) registered = true;
        if (e.name.find("SmokeUpdateSystem") != std::string::npos && e.phase == Arcane::SystemPhase::Update) updateRegistered = true;
    }
    CHECK(registered);
    CHECK(updateRegistered);
    // ...and runs: its only parameter is Res<Time>, which RunLoop publishes, so
    // a skip would log "param-system skipped" and a crash would end the test.
    for (int i = 0; i < 3; ++i)
        rt.Loop().Advance(1.0 / 60.0, [&](double dt) { host.FixedUpdateAll(dt); }, [&](double dt, double a) { host.UpdateAll(dt, a); });
    CHECK(rt.Registry().GetResource<Arcane::Time>() != nullptr);
    CHECK(host.IsLoaded());
    host.Unload();
}
