// ProcessContext (spec docs/specs/2026-09-15-core-dll-split-design.md s3): the
// process-wide state Arcane pays for exactly once. N Runtimes share it; a second
// construction is a REFUSAL. The test exe's own instance (Helpers/TestTypeContext.hpp,
// created in test_main before Catch2 runs) is the live one every case here sees --
// which is the honest shape: the refusal is only observable against a live instance,
// and this process has one for its whole life, like every host.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>      // Arcane::PhysicsWorld2D
#include <Arcane/Scene/SceneResources.hpp>     // SceneRoot / Arcane::PhysicsInterpBuffer2D / the four tables
#include <Arcane/Scene/TransformSystems.hpp>   // TransformOrder

#include <Astra/Component/Component.hpp>   // INVALID_COMPONENT
#include <Astra/Core/TypeID.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <type_traits>

TEST_CASE("ProcessContext: exactly one per process -- a second Create is refused", "[process]")
{
    Arcane::ProcessContext& live = Arcane::Test::Process();
    REQUIRE(Arcane::ProcessContext::Current() == &live);
    // a refused instance frees its never-installed context (plan 1 Task 3 review)
    CHECK(Arcane::ProcessContext::Create({}) == nullptr);                       // owned-context flavour
    Arcane::ProcessContextDesc adopt; adopt.externalTypeContext = &Arcane::Test::SharedTypeContext();
    CHECK(Arcane::ProcessContext::Create(adopt) == nullptr);                    // adopting flavour, same refusal
    CHECK(Arcane::ProcessContext::Current() == &live);                          // the refusal did not disturb the slot
}

TEST_CASE("ProcessContext: the test process is not a dedicated-server process and owns the shared TypeContext", "[process]")
{
    Arcane::ProcessContext& live = Arcane::Test::Process();
    CHECK_FALSE(live.IsDedicatedServerProcess());
    CHECK(&live.TypeContext() == &Arcane::Test::SharedTypeContext());
}

// ProcessContext::Create pre-warms the engine's RESOURCE types (Core-owned, no
// registrar to order them) so ArcaneCore.dll is their first registrar by
// construction -- see Arcane/Scene/EngineResourceTypes.hpp.
//
// WHAT THIS PROVES IS PRESENCE, NOT REGISTRAR IDENTITY: from inside the test exe
// there is no way to ask "which module minted this id" -- TypeID<T>::Value() answers
// the same for every module on the shared context, which is the point of the shared
// context. So this pins that all eight resolve to a real id once the process's
// ProcessContext exists, and that the id is stable across calls (a second Value()
// re-reads the same magic static rather than minting a second id).
TEST_CASE("ProcessContext::Create leaves every engine resource type resolved and stable", "[process]")
{
    (void)Arcane::Test::Process();   // forces Create if test_main somehow had not

    auto pin = [](auto tag)
    {
        using T = typename decltype(tag)::type;
        const Astra::ComponentID first = Astra::TypeID<T>::Value();
        CHECK(first != Astra::INVALID_COMPONENT);
        CHECK(Astra::TypeID<T>::Value() == first);
    };
    // The list tracks EngineResourceTypes.cpp's, plus PhysicsWorld2D: the 2D physics
    // hook's Prewarm touches it (ProcessContext::Create), so it is no longer in
    // EngineResourceTypes.cpp but stays pinned here. Presence and stability only;
    // the id order of resource types is not asserted.
    pin(std::type_identity<Arcane::SceneRoot>{});
    pin(std::type_identity<Arcane::PhysicsInterpBuffer2D>{});
    pin(std::type_identity<Arcane::SpriteTable>{});
    pin(std::type_identity<Arcane::SpriteMaterialTable>{});
    pin(std::type_identity<Arcane::MeshTable>{});
    pin(std::type_identity<Arcane::MeshMaterialTable>{});
    pin(std::type_identity<Arcane::PhysicsWorld2D>{});
    pin(std::type_identity<Arcane::TransformOrder>{});
}

TEST_CASE("Runtime: every Runtime is built on the ProcessContext and reports its TypeContext", "[process][runtime]")
{
    Arcane::Runtime a(Arcane::Test::Process());
    Arcane::Runtime b(Arcane::Test::Process());
    CHECK(a.TypeContext() == &Arcane::Test::Process().TypeContext());
    CHECK(b.TypeContext() == a.TypeContext());
}
