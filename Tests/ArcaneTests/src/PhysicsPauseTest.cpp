// Pausing must SKIP the solve, not run Step(0): a no-step Arcane::PhysicsSystem2D mints +
// writes back but generates zero contacts (the expensive narrowphase/solve is
// skipped). Guards the interactive "pause to inspect" path + the perf claim.
#include <catch2/catch_test_macros.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Astra/Registry/Registry.hpp>

using namespace Arcane;

namespace
{
    // Spawn one dynamic box resting on a wider static box so a real step would
    // generate >=1 contact constraint.
    void BuildOverlap(Astra::Registry& reg)
    {
        RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents2D(reg);

        Manifold2D::Physics::WorldDef wd;
        reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));
        auto add = [&](glm::vec2 pos, glm::vec2 half, Arcane::BodyType2D t) {
            Astra::Entity e = reg.CreateEntity();
            Transform lt; lt.position = glm::vec3(pos, 0.0f);
            reg.AddComponent<Transform>(e, lt);
            reg.AddComponent<WorldTransform>(e, WorldTransform{});
            Arcane::RigidBody2D rb; rb.type = t; rb.fixedRotation = true;
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);
            Arcane::Collider2D col; Arcane::Fixture2D fx;
            fx.kind = Arcane::ShapeKind2D::Aabb; fx.halfW = half.x; fx.halfH = half.y;
            col.fixtures.push_back(fx);
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});
        };
        add({0.0f, 10.0f}, {20.0f, 2.0f}, Arcane::BodyType2D::Static);
        add({0.0f, 7.9f}, { 2.0f, 2.0f}, Arcane::BodyType2D::Dynamic); // resting/overlapping
    }
}

TEST_CASE("Arcane::PhysicsSystem2D no-step skips contact generation", "[physics][pause]")
{
    Astra::Registry reg;
    BuildOverlap(reg);

    // No-step: mint + write-back, but DO NOT solve -> zero contacts generated.
    Arcane::PhysicsSystem2D noStep(1.0f / 60.0f, /*stepWorld=*/false);
    noStep(reg);
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res)->ActiveContactCount() == 0);
    // The CREATE/SYNC pass must still have minted both bodies even while paused.
    REQUIRE(Arcane::Detail::Physics2D::Access::Entities(*res).size() == 2);

    // A real step on the same overlap DOES generate at least one contact.
    Arcane::PhysicsSystem2D real(1.0f / 60.0f, /*stepWorld=*/true);
    real(reg);
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->ActiveContactCount() >= 1);
}
