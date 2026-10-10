// Flat gameplay API (spec 2026-10-10): the ECS vocabulary is Arcane:: only.
// Each alias is the Astra type. Arcane::ECS does not exist.
//
// Absence is a compile-time probe. A missing qualified name is not a substitution
// failure in standard C++, so the probe is MSVC's __if_exists (the gate compiler).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Ecs.hpp>
#include <Arcane/Physics2D.hpp>
#include <Arcane/Scene/EngineRoster.hpp>

#include <Astra/Astra.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>

#include "Helpers/ReferenceProjectDir.hpp"

namespace
{
    struct C {};
    struct S { void operator()(Astra::Registry&) {} };

#if defined(_MSC_VER) && !defined(__clang__)
#define NSFACADE_NAME_PRESENT(qual)                                    \
    []() -> bool {                                                     \
        bool found = false;                                            \
        __if_exists(qual) { found = true; }                            \
        return found;                                                  \
    }()
#endif
}

TEST_CASE("flat Arcane ECS aliases are the Astra types", "[namespaces]")
{
    STATIC_REQUIRE(std::is_same_v<Arcane::Registry, Astra::Registry>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Entity, Astra::Entity>);
    STATIC_REQUIRE(std::is_same_v<Arcane::View<C>, Astra::View<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Res<C>, Astra::Res<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ResMut<C>, Astra::ResMut<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Commands, Astra::Commands>);
    STATIC_REQUIRE(std::is_same_v<Arcane::SystemTraits<Arcane::Before<S>>, Astra::SystemTraits<Astra::Before<S>>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::After<S>, Astra::After<S>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::AmbiguousWith<S>, Astra::AmbiguousWith<S>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Reads<C>, Astra::Reads<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Writes<C>, Astra::Writes<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Exclusive, Astra::Exclusive>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ReadsResources<C>, Astra::ReadsResources<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::WritesResources<C>, Astra::WritesResources<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Not<C>, Astra::Not<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::With<C>, Astra::With<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Changed<C>, Astra::Changed<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Added<C>, Astra::Added<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Optional<C>, Astra::Optional<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Any<C>, Astra::Any<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::OneOf<C>, Astra::OneOf<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::IncludeDisabled<C>, Astra::IncludeDisabled<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::BinaryWriter, Astra::BinaryWriter>);
    STATIC_REQUIRE(std::is_same_v<Arcane::BinaryReader, Astra::BinaryReader>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ComponentModule, Astra::ComponentModule>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ComponentRegistry, Astra::ComponentRegistry>);
    STATIC_REQUIRE(std::is_same_v<Arcane::TypeContext, Astra::TypeContext>);
    STATIC_REQUIRE(std::is_same_v<Arcane::SystemScheduler, Astra::SystemScheduler>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Tick, Astra::Tick>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Result<int, Arcane::SerializationError>,
                                  Astra::Result<int, Astra::SerializationError>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::SerializationError, Astra::SerializationError>);
    // Mosaic stays at Arcane:: until its own facade (spec s3).
    STATIC_REQUIRE(std::is_same_v<Arcane::IWorkScheduler, Mosaic::IWorkScheduler>);
}

#if defined(_MSC_VER) && !defined(__clang__)
TEST_CASE("flat Arcane:: ECS prelude names are present", "[namespaces]")
{
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Entity));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::View));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Not));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::With));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Changed));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Added));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Optional));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Any));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::OneOf));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::IncludeDisabled));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Res));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::ResMut));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Commands));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::SystemTraits));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Before));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::After));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::AmbiguousWith));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Reads));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Writes));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::ReadsResources));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::WritesResources));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Exclusive));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Tick));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Result));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::SerializationError));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::Registry));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::ComponentRegistry));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::TypeContext));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::BinaryWriter));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::BinaryReader));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::ComponentModule));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::SystemScheduler));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::IWorkScheduler));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::ECS::Entity));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::ECS::Registry));
}
#endif

TEST_CASE("Physics2D game-facing types", "[namespaces]")
{
    STATIC_REQUIRE(std::is_class_v<Arcane::PhysicsWorld2D>);
    STATIC_REQUIRE(std::is_enum_v<Arcane::BodyType2D>);
    STATIC_REQUIRE(std::is_enum_v<Arcane::ShapeKind2D>);
    STATIC_REQUIRE(std::is_same_v<decltype(Arcane::RigidBody2D::type), Arcane::BodyType2D>);
    STATIC_REQUIRE(std::is_same_v<decltype(Arcane::Fixture2D::kind), Arcane::ShapeKind2D>);
    STATIC_REQUIRE(std::is_same_v<Arcane::EngineComponentRoster,
        Arcane::TypeList<
            Arcane::Transform,
            Arcane::WorldTransform,
            Arcane::SpriteRenderer,
            Arcane::PostProcess,
            Arcane::Identity,
            Arcane::Hidden,
            Arcane::Camera,
            Arcane::MeshRenderer,
            Arcane::PhysicsSettings2D,
            Arcane::WorldBounds,
            Arcane::RigidBody2D,
            Arcane::Collider2D,
            Arcane::PhysicsBodyRef2D>>);
}

// FA2: the names that must not exist (Arcane::ECS, Arcane::Physics2D,
// Arcane::Phys, the unsuffixed physics types, and a game-side read of
// PhysicsWorld2D::world) are one translation unit each.
// The platform's namespace-compile-fail script compiles the control and
// expects each forbidden TU to fail.
TEST_CASE("forbidden namespace spellings fail to compile", "[namespaces]")
{
#if defined(_WIN32)
    const auto script = Arcane::Test::FindReferenceProjectDir().parent_path()
        / "scripts" / "namespace-compile-fail.ps1";
    REQUIRE(std::filesystem::exists(script));

    const std::string command =
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.string() + "\"";
    FILE* pipe = _popen(command.c_str(), "r");
#else
    const auto script = Arcane::Test::FindReferenceProjectDir().parent_path()
        / "scripts" / "namespace-compile-fail.sh";
    REQUIRE(std::filesystem::exists(script));

    const std::string command = "bash \"" + script.string() + "\" 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
#endif
    REQUIRE(pipe != nullptr);

    std::string output;
    char buf[4096];
    while (std::fgets(buf, static_cast<int>(sizeof(buf)), pipe) != nullptr)
        output += buf;
#if defined(_WIN32)
    const int code = _pclose(pipe);
#else
    const int code = pclose(pipe);
#endif
    INFO(output);
    CHECK(code == 0);
}
