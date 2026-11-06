// Engine-owned scene systems (spec docs/specs/2026-09-13-game-module-boilerplate-
// design.md s4): InstallEngineSystems owns the standard set, it survives
// ClearSystems (module unload / hot reload), a module that still registers those
// systems is harmlessly refused, and a game system PLACES itself with Astra's
// Before/After against the engine's types -- across the DLL boundary, because
// Astra keys systems by a hash of the type NAME.
//
// The standard set SPLIT at the Core-DLL split (spec 2026-09-15 s2, plan 1
// Task 4): the fixedUpdate pair is Runtime's (Core, headless) and
// RenderSubmissionSystem is ClientRuntime's (presentation). The split itself --
// no render system on a bare Runtime, one on a ClientRuntime, surviving
// ClearSystems through the hooks -- is pinned by ClientRuntimeTest.cpp; what
// stays here is everything that is about the SCHEDULING contract.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Render/RenderSystems.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <string>
#include <vector>

// NAMESPACED probes, not anonymous: Astra keys a system by TypeID<T>::Hash(),
// a hash of the type name, and an anonymous-namespace name is per-TU. The
// Before/After anchors below name engine types by the same rule -- which is
// exactly what a game module compiled into another DLL does.
namespace Arcane::Test::EngineSystems
{
    inline std::vector<std::string> g_order;

    struct BeforePropagation : Astra::SystemTraits<Astra::Before<Arcane::TransformPropagationSystem>>
    {
        void operator()(Astra::Registry&) { g_order.push_back("before"); }
    };
    struct AfterPropagation : Astra::SystemTraits<Astra::After<Arcane::TransformPropagationSystem>>
    {
        void operator()(Astra::Registry&) { g_order.push_back("after"); }
    };
    struct Untraited
    {
        void operator()(Astra::Registry&) { g_order.push_back("untraited"); }
    };
    // Complete, never registered: an anchor the scheduler cannot resolve.
    struct NeverRegistered { void operator()(Astra::Registry&) {} };
    struct NamesDangling : Astra::SystemTraits<Astra::Before<NeverRegistered>>
    {
        void operator()(Astra::Registry&) { g_order.push_back("dangling"); }
    };

    inline void OneFixedStep(Arcane::Runtime& rt)
    {
        // 1/60 s at the default 60 Hz = exactly one fixed step (PluginHostTest's StepK shape).
        rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
    }
}

using namespace Arcane::Test::EngineSystems;

TEST_CASE("Runtime installs the engine's standard systems and reinstalls them after ClearSystems", "[runtime]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    auto& sch = rt.Schedulers();

    CHECK(sch.fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>());
    CHECK(sch.fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    CHECK_FALSE(sch.update.HasSystem<Arcane::TransformPropagationSystem>());

    rt.ClearSystems();   // what PluginHost does around a module unload / reload
    CHECK(sch.fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>());
    CHECK(sch.fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
}

TEST_CASE("A module built against ABI 28 that still registers the pair is refused harmlessly", "[runtime]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    auto& sch = rt.Schedulers();
    // AlreadyRegistered -- the only failure AddSystem<T> has for a known T; the
    // old modules std::ignore it, so an unconverted DLL keeps working.
    CHECK(sch.fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>().IsErr());

    // RenderSubmissionSystem is installed by NO runtime since F5 plan 1 Task 5
    // (ExtractFrame runs the sprite sweep), so an unconverted module's
    // registration into a ClientRuntime's render scheduler is now ACCEPTED --
    // and inert: no host runs that scheduler (RunLoop::SubmitRender has no
    // caller), so the sweep still runs exactly once per frame.
    // ClientRuntimeTest pins that the client installs nothing.
    Arcane::ClientRuntime crt(Arcane::Test::Process());
    CHECK(crt.Schedulers().render.AddSystem<Arcane::RenderSubmissionSystem>().IsOk());
}

TEST_CASE("A game system places itself with Before/After against the engine's systems", "[runtime]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    auto& fixed = rt.Schedulers().fixedUpdate;
    g_order.clear();

    // Registered AFTER the engine's install and in this insertion order:
    // untraited first, so insertion order alone would run it FIRST.
    REQUIRE(fixed.AddSystem<Untraited>().IsOk());
    REQUIRE(fixed.AddSystem<AfterPropagation>().IsOk());
    REQUIRE(fixed.AddSystem<BeforePropagation>().IsOk());

    OneFixedStep(rt);

    REQUIRE(g_order.size() == 3);
    // Before<Propagation> runs before After<Propagation> regardless of insertion;
    // the untraited one keeps its insertion slot relative to the engine's
    // systems (it was added after them) and is unconstrained against the probes.
    const auto pos = [&](const char* s) {
        for (std::size_t i = 0; i < g_order.size(); ++i) if (g_order[i] == s) return i;
        return g_order.size();
    };
    CHECK(pos("before") < pos("after"));
    CHECK(pos("untraited") < g_order.size());
}

TEST_CASE("An ordering anchor that is not registered adds no constraint and no error", "[runtime]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    auto& fixed = rt.Schedulers().fixedUpdate;
    g_order.clear();

    // Before<NeverRegistered>: the headless-host case (a module naming
    // RenderSubmissionSystem in a host that never installed it).
    REQUIRE(fixed.AddSystem<NamesDangling>().IsOk());
    OneFixedStep(rt);
    REQUIRE(g_order.size() == 1);
    CHECK(g_order[0] == "dangling");
}
