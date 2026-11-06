// M6 Physics-v2 T6 -- Arcane::PhysicsSystem2D + Arcane::PhysicsWorld2D + transform sync,
// updated for fixture-list Arcane::Collider2D schema.
//
// Tests:
//   1. Dynamic body under gravity: after N fixed steps, Transform.position.y
//      has increased (fallen), and WorldTransform.matrix[3].y agrees (propagation ran).
//   2. Kinematic body with authored velocity: after N steps, Transform.position.x
//      has increased proportionally to velocity * dt * N.
//   3. Remove: after DestroyEntity, the body row is removed from the Arcane::PhysicsWorld2D's
//      entityToBody map on the next Arcane::PhysicsSystem2D invocation.
//   4. Determinism: two identical runs of the same scene yield exactly the same
//      final Transform positions (binary ==, not Approx).
//   5. Two-fixture body: an entity with a 2-fixture Arcane::Collider2D gets both fixtures
//      attached in the PhysicsWorld (FixtureCount == 2), and Transform still
//      updates after stepping.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Reflection/MetaRegistry.hpp>   // GetMeta<ShapeKind>() -> EnumInfo (Test 4b)
#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <string_view>
#include <vector>

using Catch::Approx;

namespace
{
    // Fixed timestep used throughout: 60 Hz.
    constexpr float kDt = 1.0f / 60.0f;
    // Gravity magnitude: +Y down, matching the dynamics test convention.
    // 10.0f is the engine MKS default (WorldDef::gravityY, PhysicsWorld.hpp:188)
    // -- nothing in this file requires a non-default g, so we just take it.
    constexpr float kGravityY = 10.0f;
    // Kinematic body speed (m/s, +X direction).
    constexpr float kKinSpeed = 10.0f;

    // ---------------------------------------------------------------------------
    // Build a registry + resource for use by all tests.
    //   - SceneRoot entity (no physics)
    //   - Dynamic body entity (circle r=0.5 single-fixture, at origin)
    //   - Kinematic body entity (circle r=0.5 single-fixture, at (5,0), velocity=(kKinSpeed,0))
    // Returns {registry, root, dynEntity, kinEntity}.
    // ---------------------------------------------------------------------------
    struct SceneHandles
    {
        std::shared_ptr<Astra::ComponentRegistry> components;
        Astra::Registry*                          reg = nullptr;
        Astra::Entity                             root{};
        Astra::Entity                             dyn{};
        Astra::Entity                             kin{};
    };

    SceneHandles BuildScene(Astra::Registry& reg)
    {
        Arcane::RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents2D(reg);

        // Physics world resource: gravity +Y down. gravityX and the sleep/
        // restitution/push/hash-grid knobs are all left at the MKS engine
        // defaults (PhysicsWorld.hpp: gravityX=0, sleepThreshold=0.05,
        // restitutionThreshold=1, contactPushMaxVelocity=3, hashCellSize=1).
        Manifold2D::Physics::WorldDef wd;
        wd.gravityY = kGravityY;
        reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));

        // SceneRoot (no physics, just anchors the hierarchy).
        Astra::Entity root = reg.CreateEntity();
        Arcane::Transform rootLT;
        rootLT.position = glm::vec3(0.0f);
        reg.AddComponent<Arcane::Transform>(root, rootLT);
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

        // Dynamic body entity: free-falls under gravity (single-fixture circle).
        Astra::Entity dyn = reg.CreateEntity();
        {
            Arcane::Transform lt; lt.position = glm::vec3(0.0f);
            reg.AddComponent<Arcane::Transform>(dyn, lt);
            reg.AddComponent<Arcane::WorldTransform>(dyn, Arcane::WorldTransform{});

            Arcane::RigidBody2D rb;
            rb.type = Arcane::BodyType2D::Dynamic;
            reg.AddComponent<Arcane::RigidBody2D>(dyn, rb);

            // Single-fixture Arcane::Collider2D (circle r=0.5).
            Arcane::Collider2D col;
            {
                Arcane::Fixture2D fx;
                fx.kind   = Arcane::ShapeKind2D::Circle;
                fx.radius = 0.5f;
                col.fixtures.push_back(fx);
            }
            reg.AddComponent<Arcane::Collider2D>(dyn, col);

            reg.AddComponent<Arcane::PhysicsBodyRef2D>(dyn, Arcane::PhysicsBodyRef2D{});
            reg.SetParent(dyn, root);
        }

        // Kinematic body entity: constant velocity in +X (single-fixture circle).
        Astra::Entity kin = reg.CreateEntity();
        {
            Arcane::Transform lt; lt.position = glm::vec3(5.0f, 0.0f, 0.0f);
            reg.AddComponent<Arcane::Transform>(kin, lt);
            reg.AddComponent<Arcane::WorldTransform>(kin, Arcane::WorldTransform{});

            Arcane::RigidBody2D rb;
            rb.type     = Arcane::BodyType2D::Kinematic;
            rb.velocity = glm::vec2(kKinSpeed, 0.0f);
            reg.AddComponent<Arcane::RigidBody2D>(kin, rb);

            // Single-fixture Arcane::Collider2D (circle r=0.5).
            Arcane::Collider2D col;
            {
                Arcane::Fixture2D fx;
                fx.kind   = Arcane::ShapeKind2D::Circle;
                fx.radius = 0.5f;
                col.fixtures.push_back(fx);
            }
            reg.AddComponent<Arcane::Collider2D>(kin, col);

            reg.AddComponent<Arcane::PhysicsBodyRef2D>(kin, Arcane::PhysicsBodyRef2D{});
            reg.SetParent(kin, root);
        }

        SceneHandles h;
        h.reg  = &reg;
        h.root = root;
        h.dyn  = dyn;
        h.kin  = kin;
        return h;
    }

    // Run N fixed steps via Arcane::PhysicsSystem2D (physics only, not via RunLoop, so we
    // can test in isolation), then run TransformPropagationSystem once.
    void RunNSteps(Astra::Registry& reg, int n)
    {
        Arcane::PhysicsSystem2D   physics(kDt);
        Arcane::TransformPropagationSystem propagate;
        for (int i = 0; i < n; ++i)
        {
            physics(reg);
        }
        propagate(reg);
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// TEST 1 -- Dynamic body falls under gravity
// ---------------------------------------------------------------------------
TEST_CASE("Arcane::PhysicsSystem2D: dynamic body falls under gravity (Transform updated)", "[physics]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    auto h = BuildScene(reg);

    constexpr int kSteps = 10;
    RunNSteps(reg, kSteps);

    const auto* lt = reg.GetComponent<Arcane::Transform>(h.dyn);
    REQUIRE(lt != nullptr);

    // After 10 steps at 60 Hz with gravityY = 10 (+Y down), y must be well positive.
    // Forward-Euler lower bound: 0.5 * g * (dt*N)^2 ~= 0.5*10*(10/60)^2 ~= 0.139 m.
    // A bound of 0.1f is safely below the true displacement but well above 0, so
    // a near-zero or sign-flipped gravity is caught.
    CHECK(lt->position.y > 0.1f);

    // WorldTransform must match: TransformPropagationSystem ran; root is at origin,
    // so the child's world matrix column 2 = its local position.
    const auto* wt = reg.GetComponent<Arcane::WorldTransform>(h.dyn);
    REQUIRE(wt != nullptr);
    CHECK(wt->matrix[3].y == Approx(lt->position.y).margin(1e-4f));   // mat4 translation column
}

// ---------------------------------------------------------------------------
// TEST 2 -- Kinematic body moves at authored velocity
// ---------------------------------------------------------------------------
TEST_CASE("Arcane::PhysicsSystem2D: kinematic body moves by authored velocity", "[physics]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    auto h = BuildScene(reg);

    constexpr int   kSteps = 30;
    const float startX = 5.0f;

    RunNSteps(reg, kSteps);

    const auto* lt = reg.GetComponent<Arcane::Transform>(h.kin);
    REQUIRE(lt != nullptr);

    // Kinematic body at (5, 0), velocity (kKinSpeed, 0):
    // expected X = 5 + kKinSpeed * kDt * kSteps = 5 + 10*(1/60)*30 = 10.0 m
    // (exact forward-Euler, one Step per invocation, constant dt). The
    // margin(0.01f) is an absolute float-accumulation tolerance, not a
    // percentage of the displacement -- 30 additions of a ~0.1667 m/step
    // increment accumulate at most a few float32 ULPs (<< 0.01 m regardless
    // of the target magnitude), so the MKS recompute (100 -> 10) does not
    // demand widening this margin; it stays 0.01f.
    const float expectedX = startX + kKinSpeed * kDt * static_cast<float>(kSteps);
    CHECK(lt->position.x == Approx(expectedX).margin(0.01f));

    // Y should stay near zero (no gravity on kinematics, no vertical velocity).
    CHECK(std::abs(lt->position.y) < 1.0f);
}

// ---------------------------------------------------------------------------
// TEST 3 -- Body row removed when entity is destroyed
// ---------------------------------------------------------------------------
TEST_CASE("Arcane::PhysicsSystem2D: body row removed after entity is destroyed", "[physics]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    auto h = BuildScene(reg);

    // Run one step to register both bodies.
    {
        Arcane::PhysicsSystem2D physics(kDt);
        physics(reg);
    }

    // Confirm both entities are tracked in the resource.
    {
        const auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
        REQUIRE(res != nullptr);
        CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).count(h.dyn) == 1);
        CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).count(h.kin) == 1);
    }

    // Destroy the dynamic entity.
    reg.DestroyEntity(h.dyn);

    // Run another step: Arcane::PhysicsSystem2D should detect the dead entity and remove its row.
    {
        Arcane::PhysicsSystem2D physics(kDt);
        physics(reg);
    }

    {
        const auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
        REQUIRE(res != nullptr);
        CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).count(h.dyn) == 0);  // removed
        CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).count(h.kin) == 1);  // still alive
    }
}

// ---------------------------------------------------------------------------
// TEST 4 -- Determinism: two identical runs produce exactly the same positions
// ---------------------------------------------------------------------------
TEST_CASE("Arcane::PhysicsSystem2D: two identical runs yield bit-exact Transform positions", "[physics]")
{
    constexpr int kSteps = 20;

    // ---- Run A ----
    glm::vec2 posA_dyn{}, posA_kin{};
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry regA(components);
        auto hA = BuildScene(regA);
        RunNSteps(regA, kSteps);
        posA_dyn = regA.GetComponent<Arcane::Transform>(hA.dyn)->position;
        posA_kin = regA.GetComponent<Arcane::Transform>(hA.kin)->position;
    }

    // ---- Run B (fresh registry, identical setup) ----
    glm::vec2 posB_dyn{}, posB_kin{};
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry regB(components);
        auto hB = BuildScene(regB);
        RunNSteps(regB, kSteps);
        posB_dyn = regB.GetComponent<Arcane::Transform>(hB.dyn)->position;
        posB_kin = regB.GetComponent<Arcane::Transform>(hB.kin)->position;
    }

    // Exact binary equality: same code path, same inputs, same f32 ops.
    CHECK(posA_dyn.x == posB_dyn.x);
    CHECK(posA_dyn.y == posB_dyn.y);
    CHECK(posA_kin.x == posB_kin.x);
    CHECK(posA_kin.y == posB_kin.y);
}

// ---------------------------------------------------------------------------
// TEST 4b -- The reflected ShapeKind offers only what MakeScaledShape builds
// ---------------------------------------------------------------------------
TEST_CASE("Arcane::PhysicsSystem2D: every reflected ShapeKind value is one MakeScaledShape can build", "[physics]")
{
    // 2026-09-12 review finding 2. The Inspector's enum combo (FieldKind::Enum,
    // one level down inside the Arcane::Collider2D fixture list since the Vector
    // editor) offers EVERY reflected value, and a paused pass re-mints the
    // body on the resulting Changed<Arcane::Collider2D> -- so a reflected value
    // MakeScaledShape cannot build (Polygon: Arcane::Fixture2D carries no vertex array;
    // MakeScaledShape asserts on it) was a Debug-editor abort one menu click
    // away. The reflected set is therefore exactly the buildable set; a
    // Polygon comes back when Arcane::Fixture2D can carry its verts.
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::ShapeKind2D>();
    REQUIRE(meta != nullptr);
    const Astra::EnumInfo* info = meta->GetEnumInfo();
    REQUIRE(info != nullptr);

    std::vector<std::string_view> names;
    for (const Astra::EnumValue& v : info->values)
        names.push_back(v.name);
    CHECK(names == std::vector<std::string_view>{ "Circle", "Capsule", "Aabb" });

    // And each of them builds -- the shape kind round-trips through the
    // scaled build, no fallback, no assert.
    for (const Astra::EnumValue& v : info->values)
    {
        Arcane::Fixture2D fx;
        fx.kind = static_cast<Arcane::ShapeKind2D>(v.value);
        const Manifold2D::Physics::Shape s = Arcane::Detail::Physics2D::MakeScaledShape(fx, glm::vec2(1.0f, 1.0f));
        INFO("kind: " << v.name);
        CHECK(s.kind == Arcane::Detail::Physics2D::ToVendor(fx.kind));
    }
}

// ---------------------------------------------------------------------------
// TEST 5 -- Two-fixture body: PhysicsWorld has 2 fixtures + transform updates
// ---------------------------------------------------------------------------
TEST_CASE("Arcane::PhysicsSystem2D: two-fixture Arcane::Collider2D registers both fixtures in PhysicsWorld", "[physics]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);

    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents2D(reg);

    // Physics world (no gravity needed; we only test fixture count + write-back).
    // BOTH gravity axes are explicitly zeroed as a deliberate scene statement
    // (the engine default is gravityY=+10); the sleep/restitution/push/hash-grid
    // knobs are left at the MKS engine defaults (see BuildScene above).
    Manifold2D::Physics::WorldDef wd;
    wd.gravityY = 0.0f;
    wd.gravityX = 0.0f;
    reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));

    // Minimal SceneRoot.
    Astra::Entity root = reg.CreateEntity();
    {
        Arcane::Transform lt; lt.position = glm::vec3(0.0f);
        reg.AddComponent<Arcane::Transform>(root, lt);
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    }

    // Entity with a 2-fixture Arcane::Collider2D.
    // Arcane::Fixture2D 0: circle r=0.5 @ local(0,0).
    // Arcane::Fixture2D 1: aabb(0.3,0.3) @ local(2,0), sensor.
    Astra::Entity e = reg.CreateEntity();
    {
        Arcane::Transform lt; lt.position = glm::vec3(10.0f, 5.0f, 0.0f);
        reg.AddComponent<Arcane::Transform>(e, lt);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});

        Arcane::RigidBody2D rb;
        rb.type = Arcane::BodyType2D::Kinematic;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);

        Arcane::Collider2D col;
        // fixture 0
        {
            Arcane::Fixture2D fx;
            fx.kind     = Arcane::ShapeKind2D::Circle;
            fx.radius   = 0.5f;
            fx.localPos = glm::vec2(0.0f, 0.0f);
            fx.density  = 1.0f;
            fx.friction = 0.3f;
            col.fixtures.push_back(fx);
        }
        // fixture 1
        {
            Arcane::Fixture2D fx;
            fx.kind      = Arcane::ShapeKind2D::Aabb;
            fx.halfW     = 0.3f;
            fx.halfH     = 0.3f;
            fx.localPos  = glm::vec2(2.0f, 0.0f);
            fx.isSensor  = true;
            fx.density   = 1.0f;
            fx.friction  = 0.3f;
            col.fixtures.push_back(fx);
        }
        reg.AddComponent<Arcane::Collider2D>(e, col);
        reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});
        reg.SetParent(e, root);
    }

    // Run one physics step to trigger the CREATE pass.
    {
        Arcane::PhysicsSystem2D physics(kDt);
        physics(reg);
    }

    // The body must be live in the resource.
    const auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(res != nullptr);
    REQUIRE(Arcane::Detail::Physics2D::Access::Entities(*res).count(e) == 1);

    // Check that exactly 2 fixtures were registered on this body.
    const Manifold2D::Physics::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res)->IsValid(bh));
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->FixtureCount(bh) == 2u);

    // Transform still gets written back after Step.
    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    const auto* lt = reg.GetComponent<Arcane::Transform>(e);
    REQUIRE(lt != nullptr);
    // Kinematic body with no velocity, gravity 0 -> position stays at (10, 5).
    CHECK(lt->position.x == Approx(10.0f).margin(0.01f));
    CHECK(lt->position.y == Approx(5.0f).margin(0.01f));
}

// ---------------------------------------------------------------------------
// TEST 6 -- fixture[0] authored filter + local transform flow through AddBody
//
// This is the T6 review regression gate. Before the fix, AddBody's auto-fixture
// used hardcoded categoryBits=1 / maskBits=0xFFFFFFFF / localPos=(0,0) /
// localAngle=0, silently discarding authored values on the primary fixture.
// After the fix, BodyDef carries those fields and AddBody's auto-fixture uses
// them. The test authors fixture[0] with non-default filter + local offset and
// asserts the world's primary fixture carries the authored values.
// ---------------------------------------------------------------------------
TEST_CASE("Arcane::PhysicsSystem2D: fixture[0] authored filter and local-xf flow through AddBody", "[physics]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);

    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents2D(reg);

    // Physics world (zero gravity; we're testing fixture metadata, not dynamics).
    // Zero-g is a deliberate scene statement now that the engine default is
    // gravityY=+10; gravityX and the sleep/restitution/push/hash-grid knobs
    // are left at the MKS engine defaults (see BuildScene above).
    Manifold2D::Physics::WorldDef wd;
    wd.gravityY = 0.0f;
    wd.gravityX = 0.0f;
    reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));

    // Minimal SceneRoot.
    Astra::Entity root = reg.CreateEntity();
    {
        Arcane::Transform lt; lt.position = glm::vec3(0.0f);
        reg.AddComponent<Arcane::Transform>(root, lt);
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    }

    // Authored fixture[0] params: non-default filter + local offset.
    //   categoryBits = 0x02, maskBits = 0x05, localPos = (0.3, 0), localAngle = 0.
    constexpr uint32_t kCat   = 0x02u;
    constexpr uint32_t kMask  = 0x05u;
    constexpr float    kLx    = 0.3f;
    constexpr float    kLy    = 0.0f;

    // Entity at world origin (body angle 0) with a single kinematic circle
    // so GetFixtureWorldPos(fixture0) == bodyPos + R(0)*localPos == (0.3,0).
    Astra::Entity e = reg.CreateEntity();
    {
        Arcane::Transform lt; lt.position = glm::vec3(0.0f);
        reg.AddComponent<Arcane::Transform>(e, lt);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});

        Arcane::RigidBody2D rb;
        rb.type = Arcane::BodyType2D::Kinematic;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);

        Arcane::Collider2D col;
        {
            Arcane::Fixture2D fx;
            fx.kind         = Arcane::ShapeKind2D::Circle;
            fx.radius       = 0.1f;
            fx.localPos     = glm::vec2(kLx, kLy);
            fx.localAngle   = 0.0f;
            fx.categoryBits = kCat;
            fx.maskBits     = kMask;
            col.fixtures.push_back(fx);
        }
        reg.AddComponent<Arcane::Collider2D>(e, col);
        reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});
        reg.SetParent(e, root);
    }

    // Run one step to trigger the CREATE pass (registers the body).
    {
        Arcane::PhysicsSystem2D physics(kDt);
        physics(reg);
    }

    const auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(res != nullptr);
    REQUIRE(Arcane::Detail::Physics2D::Access::Entities(*res).count(e) == 1);

    const Manifold2D::Physics::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res)->IsValid(bh));

    // Exactly 1 fixture (single-fixture Arcane::Collider2D).
    REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res)->FixtureCount(bh) == 1u);

    // Get the primary fixture handle via index 0.
    const Manifold2D::Physics::FixtureHandle fh0 = Arcane::Detail::Physics2D::Access::Solver(*res)->GetBodyFixture(bh, 0u);
    REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res)->IsValid(fh0));

    // Assert the authored filter flowed through.
    // BEFORE the fix these would observe 1 / 0xFFFFFFFF (the old hardcoded defaults).
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->GetFixtureCategory(fh0) == kCat);
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->GetFixtureMask(fh0)     == kMask);

    // Assert the authored local-xf flowed through:
    //   body at origin (0,0), angle 0 -> worldPos = (0,0) + R(0)*(0.3,0) = (0.3,0).
    // BEFORE the fix this would return (0,0) because localPos was hardcoded to (0,0).
    const Manifold2D::Physics::Vec2 worldPos = Arcane::Detail::Physics2D::Access::Solver(*res)->GetFixtureWorldPos(fh0);
    CHECK(static_cast<double>(worldPos.x) == Approx(static_cast<double>(kLx)).margin(1e-4));
    CHECK(static_cast<double>(worldPos.y) == Approx(static_cast<double>(kLy)).margin(1e-4));
}
