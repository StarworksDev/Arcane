// Arcane::PhysicsWorld2D (input-seam spec 2026-10-02 s5.3): the game-facing physics
// commands as members of the published Arcane::PhysicsWorld2D, taking the body handle
// from entityToBody. Behaviour must equal the deleted free functions'.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    // An axis-aligned box collider. Arcane::Collider2D{} has NO fixtures, and
    // Arcane::PhysicsSystem2D skips an empty fixture list (no body is minted).
    Arcane::Collider2D BoxCollider(float halfW, float halfH)
    {
        Arcane::Fixture2D fx;
        fx.kind = Arcane::ShapeKind2D::Aabb;
        fx.halfW = halfW;
        fx.halfH = halfH;
        Arcane::Collider2D col;
        col.fixtures.push_back(fx);
        return col;
    }

    // A dynamic unit box resting on a static ground box.
    struct World
    {
        Arcane::Runtime rt{Arcane::Test::Process()};
        Astra::Entity ground, box;
        World()
        {
            auto& reg = rt.Registry();
            Arcane::RegisterSceneComponents(reg);
            ground = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(ground, Arcane::Transform{ .position = {0.0f, -0.5f, 0.0f} });
            Arcane::RigidBody2D sb; sb.type = Arcane::BodyType2D::Static;
            reg.AddComponent<Arcane::RigidBody2D>(ground, sb);
            reg.AddComponent<Arcane::Collider2D>(ground, BoxCollider(10.0f, 0.5f));
            box = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(box, Arcane::Transform{ .position = {0.0f, 0.5f, 0.0f} });
            Arcane::RigidBody2D db; db.type = Arcane::BodyType2D::Dynamic;
            db.fixedRotation = true;                        // a dynamic Aabb must be (Manifold2D asserts)
            reg.AddComponent<Arcane::RigidBody2D>(box, db);
            reg.AddComponent<Arcane::Collider2D>(box, BoxCollider(0.5f, 0.5f));
        }
        void Step(int n)
        {
            for (int i = 0; i < n; ++i) { rt.EnsurePhysics(); rt.Loop().Advance(1.0 / 60.0); }
        }
        Arcane::PhysicsWorld2D* Physics() { return rt.Registry().GetResource<Arcane::PhysicsWorld2D>(); }
        Arcane::RigidBody2D& Body(Astra::Entity e) { return *rt.Registry().GetComponent<Arcane::RigidBody2D>(e); }
    };
}

TEST_CASE("Arcane::PhysicsWorld2D is the Arcane::PhysicsWorld2D", "[physics][physics2d]")
{
    STATIC_REQUIRE(std::is_same_v<Arcane::PhysicsWorld2D, Arcane::PhysicsWorld2D>);
}

TEST_CASE("Physics2D::Motion before the body exists reads Arcane::RigidBody2D, bodyReady false", "[physics][physics2d]")
{
    World w;
    w.rt.EnsurePhysics();                                   // world minted, no step yet: no bodies
    REQUIRE(w.Physics() != nullptr);
    Arcane::RigidBody2D& rb = w.Body(w.box);
    rb.velocity = {1.5f, -2.0f};
    const Arcane::BodyMotion2D m = w.Physics()->Motion(w.box, rb);
    CHECK_FALSE(m.bodyReady);
    CHECK(m.velocityX == 1.5f);
    CHECK(m.velocityY == -2.0f);

    w.Physics()->SetVelocity(w.box, rb, 3.0f, 0.0f);       // unminted: authored mint velocity
    CHECK(rb.velocity.x == 3.0f);
}

TEST_CASE("Physics2D::SetVelocity drives the live body; a resting body reads as supported", "[physics][physics2d]")
{
    World w;
    // Landed and still AWAKE: support comes from the last step's contacts. (A
    // SLEEPING body has no active contacts and reads support through the
    // shape-cast fallback -- the asleep cases below.)
    w.Step(10);
    REQUIRE(w.Physics() != nullptr);
    const auto it = Arcane::Detail::Physics2D::Access::Entities(*w.Physics()).find(w.box);
    REQUIRE(it != Arcane::Detail::Physics2D::Access::Entities(*w.Physics()).end());
    REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*w.Physics())->IsAwake(it->second));
    Arcane::RigidBody2D& rb = w.Body(w.box);
    Arcane::BodyMotion2D m = w.Physics()->Motion(w.box, rb);
    CHECK(m.bodyReady);
    CHECK(m.supported);

    w.Physics()->SetVelocity(w.box, rb, 2.0f, 0.0f);
    m = w.Physics()->Motion(w.box, rb);
    CHECK(m.velocityX == 2.0f);
    CHECK(rb.velocity.x == 2.0f);
}

TEST_CASE("Arcane::PhysicsWorld2D ignores non-finite input and non-dynamic bodies", "[physics][physics2d]")
{
    World w;
    w.Step(2);
    REQUIRE(w.Physics() != nullptr);
    Arcane::RigidBody2D& rb = w.Body(w.box);
    const glm::vec2 before = rb.velocity;
    w.Physics()->SetVelocity(w.box, rb, std::numeric_limits<float>::quiet_NaN(), 0.0f);
    w.Physics()->SetVelocity(w.box, rb, 0.0f, std::numeric_limits<float>::infinity());
    CHECK(rb.velocity == before);

    Arcane::RigidBody2D& sb = w.Body(w.ground);
    w.Physics()->SetVelocity(w.ground, sb, 5.0f, 5.0f);
    CHECK(sb.velocity == glm::vec2(0.0f, 0.0f));
    const Arcane::BodyMotion2D gm = w.Physics()->Motion(w.ground, sb);
    CHECK_FALSE(gm.bodyReady);
    CHECK_FALSE(gm.supported);
}

// ---- floor support for a SLEEPING body (IN-12 ruling, from IN-9's concern) ----
// A sleeping body has no active contacts, so Motion() falls back to a short
// downward shape cast. The cast must not start inside the solver's resting slop
// overlap (an initial overlap answers t=0 with a ZERO normal, Box2D-v3 parity).
namespace
{
    bool IsAsleep(World& w, Astra::Entity entity)
    {
        const Arcane::PhysicsWorld2D* physics = w.Physics();
        if (!physics || !Arcane::Detail::Physics2D::Access::Solver(*physics)) return false;
        const auto it = Arcane::Detail::Physics2D::Access::Entities(*physics).find(entity);
        return it != Arcane::Detail::Physics2D::Access::Entities(*physics).end() && !Arcane::Detail::Physics2D::Access::Solver(*physics)->IsAwake(it->second);
    }

    // Steps until `entity`'s body sleeps (false if still awake after 10 s), then
    // a few more steps: the step that puts a body to sleep still lists its
    // contacts, and the defect only shows once the active solver has none.
    bool StepUntilAsleep(World& w, Astra::Entity entity)
    {
        for (int i = 0; i < 600 && !IsAsleep(w, entity); ++i)
            w.Step(1);
        if (!IsAsleep(w, entity))
            return false;
        w.Step(5);
        return IsAsleep(w, entity);
    }
}

TEST_CASE("Physics2D::Motion: a box asleep on static ground reads as supported", "[physics][physics2d]")
{
    World w;
    REQUIRE(StepUntilAsleep(w, w.box));
    const Arcane::BodyMotion2D m = w.Physics()->Motion(w.box, w.Body(w.box));
    CHECK(m.bodyReady);
    CHECK(m.supported);
}

TEST_CASE("Physics2D::Motion: a box 0.2 m above the ground reads as unsupported", "[physics][physics2d]")
{
    World w;
    w.rt.Registry().GetComponent<Arcane::Transform>(w.box)->position.y = 0.7f; // feet at y = 0.2
    w.Step(1);
    const Arcane::BodyMotion2D m = w.Physics()->Motion(w.box, w.Body(w.box));
    REQUIRE(m.bodyReady);
    REQUIRE(m.velocityY <= 0.0f);                                              // the support check runs
    CHECK_FALSE(m.supported);
}

TEST_CASE("Physics2D::Motion: a box asleep on a sleeping dynamic crate reads as supported", "[physics][physics2d]")
{
    World w;
    auto& reg = w.rt.Registry();
    const Astra::Entity crate = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(crate, Arcane::Transform{ .position = {0.0f, 0.5f, 0.0f} });
    Arcane::RigidBody2D cb; cb.type = Arcane::BodyType2D::Dynamic;
    cb.fixedRotation = true;
    reg.AddComponent<Arcane::RigidBody2D>(crate, cb);
    reg.AddComponent<Arcane::Collider2D>(crate, BoxCollider(0.5f, 0.5f));
    reg.GetComponent<Arcane::Transform>(w.box)->position.y = 1.5f;             // stacked on the crate

    REQUIRE(StepUntilAsleep(w, w.box));
    REQUIRE(IsAsleep(w, crate));
    const Arcane::BodyMotion2D m = w.Physics()->Motion(w.box, w.Body(w.box));
    CHECK(m.bodyReady);
    CHECK(m.supported);
}
