// Authored-transform ownership + physics sync (SPEC #1). Transform is the
// authored source of truth for a physics entity; this file exercises the engine
// Arcane::PhysicsSystem2D sync. Task 1: birth-time scale + appliedScale baseline.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Arcane;
using Catch::Approx;
namespace Phys = Manifold2D::Physics;

namespace
{
    constexpr float kDt = 1.0f / 60.0f;

    // Build reg + zero-gravity world + one Aabb-fixture body. Gravity is zeroed so
    // a Dynamic body stays put across paused/played steps (position assertions stay
    // deterministic). Returns the body entity.
    Astra::Entity BuildAabbBody(Astra::Registry& reg, glm::vec2 pos, glm::vec2 half,
                                Arcane::BodyType2D type, glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents2D(reg);
        Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
        reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Phys::PhysicsWorld>(wd)));

        Astra::Entity e = reg.CreateEntity();
        Transform lt; lt.position = glm::vec3(pos, 0.0f); lt.scale = glm::vec3(scale, 1.0f);
        reg.AddComponent<Transform>(e, lt);
        reg.AddComponent<WorldTransform>(e, WorldTransform{});
        Arcane::RigidBody2D rb; rb.type = type;
        // Manifold2D invariant: a dynamic AABB body must be fixedRotation (an
        // axis-aligned box has no meaningful orientation under the solver).
        // SetAngle still applies as an explicit teleport, so the reconcile rotate
        // path is exercised regardless.
        if (type == Arcane::BodyType2D::Dynamic) rb.fixedRotation = true;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);
        Arcane::Collider2D col; Arcane::Fixture2D fx;
        fx.kind = Arcane::ShapeKind2D::Aabb; fx.halfW = half.x; fx.halfH = half.y;
        col.fixtures.push_back(fx);
        reg.AddComponent<Arcane::Collider2D>(e, col);
        reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});
        return e;
    }

    // A FURTHER Aabb body into a registry BuildAabbBody already set up -- same
    // components, no re-registration and no world reset, so multi-body cases
    // can share one world.
    Astra::Entity AddAabbBody(Astra::Registry& reg, glm::vec2 pos, glm::vec2 half,
                              Arcane::BodyType2D type)
    {
        Astra::Entity e = reg.CreateEntity();
        Transform lt; lt.position = glm::vec3(pos, 0.0f);
        reg.AddComponent<Transform>(e, lt);
        reg.AddComponent<WorldTransform>(e, WorldTransform{});
        Arcane::RigidBody2D rb; rb.type = type;
        if (type == Arcane::BodyType2D::Dynamic) rb.fixedRotation = true;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);
        Arcane::Collider2D col; Arcane::Fixture2D fx;
        fx.kind = Arcane::ShapeKind2D::Aabb; fx.halfW = half.x; fx.halfH = half.y;
        col.fixtures.push_back(fx);
        reg.AddComponent<Arcane::Collider2D>(e, col);
        reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});
        return e;
    }

    // World-AABB half-extents of fixture[0] (single-fixture body at angle 0 -> the
    // world AABB equals the fixture box, so half-extents == the scaled half-dims).
    glm::vec2 Fixture0HalfExtents(Astra::Registry& reg)
    {
        auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
        std::vector<std::uint32_t> fx;
        std::vector<Phys::Aabb2>   boxes;
        Arcane::Detail::Physics2D::Access::Solver(*res)->LiveFixtureAabbs(fx, boxes);
        REQUIRE(!boxes.empty());
        const Phys::Aabb2& b = boxes[0];
        return glm::vec2((b.max.x - b.min.x) * 0.5f, (b.max.y - b.min.y) * 0.5f);
    }
}

TEST_CASE("create applies uniform scale to collider", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic, {2.0f,2.0f});

    Arcane::PhysicsSystem2D sys(kDt, /*stepWorld=*/false);
    sys(reg);  // CREATE pass mints the body with scaled fixtures

    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));   // 0.5 * 2
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
    CHECK(reg.GetComponent<Arcane::PhysicsBodyRef2D>(e)->appliedScale == glm::vec2(2.0f, 2.0f));
}

TEST_CASE("create leaves scale-1 collider unchanged", "[transform-sync]")
{
    Astra::Registry reg;
    BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic, {1.0f,1.0f});
    Arcane::PhysicsSystem2D sys(kDt, /*stepWorld=*/false);
    sys(reg);
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(0.5f).margin(1e-4f));
    CHECK(he.y == Approx(0.5f).margin(1e-4f));
}

TEST_CASE("create applies non-uniform scale per-axis", "[transform-sync]")
{
    Astra::Registry reg;
    BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic, {2.0f,1.0f});
    Arcane::PhysicsSystem2D sys(kDt, /*stepWorld=*/false);
    sys(reg);
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));   // 0.5 * 2
    CHECK(he.y == Approx(0.5f).margin(1e-4f));   // 0.5 * 1
}

TEST_CASE("create scales multi-fixture local offsets per-axis", "[transform-sync]")
{
    Astra::Registry reg;
    RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents2D(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
    reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Phys::PhysicsWorld>(wd)));

    Astra::Entity e = reg.CreateEntity();
    Transform lt; lt.position = glm::vec3(0.0f); lt.scale = glm::vec3(2.0f, 1.0f, 1.0f);
    reg.AddComponent<Transform>(e, lt);
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Arcane::BodyType2D::Kinematic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col;
    { Arcane::Fixture2D f; f.kind = Arcane::ShapeKind2D::Aabb; f.halfW = 0.3f; f.halfH = 0.3f; col.fixtures.push_back(f); }
    { Arcane::Fixture2D f; f.kind = Arcane::ShapeKind2D::Aabb; f.halfW = 0.3f; f.halfH = 0.3f; f.localPos = {2.0f, 0.0f}; col.fixtures.push_back(f); }
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});

    Arcane::PhysicsSystem2D sys(kDt, /*stepWorld=*/false);
    sys(reg);

    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res)->FixtureCount(bh) == 2u);
    // Body at origin, angle 0: fixture[1] world pos = scaled localPos = (2*2, 0*1) = (4,0).
    const Phys::FixtureHandle fh1 = Arcane::Detail::Physics2D::Access::Solver(*res)->GetBodyFixture(bh, 1u);
    const Phys::Vec2 wp = Arcane::Detail::Physics2D::Access::Solver(*res)->GetFixtureWorldPos(fh1);
    CHECK(static_cast<float>(wp.x) == Approx(4.0f).margin(1e-4f));
    CHECK(static_cast<float>(wp.y) == Approx(0.0f).margin(1e-4f));
}

// ===========================================================================
// Task 2: paused pos/rot stateless reconcile (PASS 3.5)
// ===========================================================================

TEST_CASE("paused author move teleports body, zeroes velocity, is not stomped", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // mint at (0,0)

    // Author moves the entity while paused (as a gizmo/inspector edit would).
    reg.GetComponent<Transform>(e)->position = glm::vec3(5.0f, 5.0f, 0.0f);
    paused(reg);                                   // reconcile: body <- lt, then write-back

    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    const Phys::Vec2 bp = Arcane::Detail::Physics2D::Access::Solver(*res)->Position(bh);
    const Phys::Vec2 bv = Arcane::Detail::Physics2D::Access::Solver(*res)->Velocity(bh);
    CHECK(static_cast<float>(bp.x) == Approx(5.0f).margin(1e-4f));
    CHECK(static_cast<float>(bp.y) == Approx(5.0f).margin(1e-4f));
    CHECK(static_cast<float>(bv.x) == Approx(0.0f).margin(1e-4f));
    CHECK(static_cast<float>(bv.y) == Approx(0.0f).margin(1e-4f));
    // Not stomped back to (0,0) by the same frame's write-back.
    const glm::vec3 lp = reg.GetComponent<Transform>(e)->position;
    CHECK(lp.x == Approx(5.0f).margin(1e-4f));
    CHECK(lp.y == Approx(5.0f).margin(1e-4f));
}

TEST_CASE("paused author rotate sets body angle, is not stomped", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);

    // A 2D scene turns about +Z only; RotationAboutZ/RotationZ are the
    // planar bridge the physics write-back itself uses (Components.hpp).
    reg.GetComponent<Transform>(e)->rotation = RotationAboutZ(1.0f);   // radians
    paused(reg);

    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    CHECK(static_cast<float>(Arcane::Detail::Physics2D::Access::Solver(*res)->GetAngle(bh)) == Approx(1.0f).margin(1e-4f));
    CHECK(RotationZ(reg.GetComponent<Transform>(e)->rotation) == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("a body minted by a STEPPING pass carries its authored Z rotation into the write-back", "[transform-sync]")
{
    // 2026-09-12 desk finding: rotate the capsule in Edit mode, press Play, the
    // rotation is undone. PASS 2 builds the BodyDef from Transform.position
    // and never from Transform.rotation (BodyDef carries no angle), so a fresh
    // mint starts the body at angle 0. A PAUSED pass hides that: its PASS 3.5
    // runs right after the mint in the same call, sees Changed<Transform>,
    // and SetAngles the body (the case above pins that order). A STEPPING
    // pass has no PASS 3.5 -- it mints, steps, and PASS 4 writes the body's
    // zero back over the authored quaternion. That is Play's first fixedUpdate
    // frame (Play re-mints from the authored state), ArcaneRuntime's boot, and
    // any re-mint that lands on a stepping frame.
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    reg.GetComponent<Transform>(e)->rotation = RotationAboutZ(1.0f);   // authored, pre-mint

    // Zero gravity, fixedRotation: the body cannot turn on its own, so any
    // change to the angle is the mint's doing.
    Arcane::PhysicsSystem2D stepping(kDt, /*stepWorld=*/true);
    stepping(reg);                                                        // mint + step + write-back, no reconcile
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    CHECK(static_cast<float>(Arcane::Detail::Physics2D::Access::Solver(*res)->GetAngle(bh)) == Approx(1.0f).margin(1e-4f));   // minted rotated
    CHECK(RotationZ(std::as_const(reg).GetComponent<Transform>(e)->rotation) == Approx(1.0f).margin(1e-4f));   // and written back rotated
}

TEST_CASE("author-while-paused then play resumes from authored pose", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Kinematic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->position = glm::vec3(3.0f, 0.0f, 0.0f);
    paused(reg);                                   // reconcile pushes (3,0) into the body

    Arcane::PhysicsSystem2D play(kDt, /*stepWorld=*/true);
    play(reg);                                     // stepping resumes from (3,0), no snap-back

    const glm::vec3 lp = reg.GetComponent<Transform>(e)->position;
    CHECK(lp.x == Approx(3.0f).margin(1e-3f));     // kinematic, zero velocity -> stays at 3
    CHECK(lp.y == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("play mode ignores author lt edits (body owns pose)", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Kinematic);
    Arcane::PhysicsSystem2D play(kDt, /*stepWorld=*/true);
    play(reg);                                     // mint + step; body at ~origin

    reg.GetComponent<Transform>(e)->position = glm::vec3(9.0f, 9.0f, 0.0f); // bogus author edit
    play(reg);                                     // Play: PASS 4 overwrites lt from the body

    const glm::vec3 lp = reg.GetComponent<Transform>(e)->position;
    CHECK(lp.x == Approx(0.0f).margin(1e-3f));     // body owns; edit discarded
    CHECK(lp.y == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("untouched paused body is not spuriously teleported", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {2,3}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    for (int i = 0; i < 5; ++i) paused(reg);       // no author edits between ticks

    const glm::vec3 lp = reg.GetComponent<Transform>(e)->position;
    CHECK(lp.x == Approx(2.0f).margin(1e-4f));
    CHECK(lp.y == Approx(3.0f).margin(1e-4f));
}

TEST_CASE("paused author move works on a static body", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Static);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->position = glm::vec3(4.0f, 0.0f, 0.0f);
    paused(reg);
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::Vec2 bp = Arcane::Detail::Physics2D::Access::Solver(*res)->Position(Arcane::Detail::Physics2D::Access::Entities(*res).at(e));
    CHECK(static_cast<float>(bp.x) == Approx(4.0f).margin(1e-4f));
    // If this FAILS (SetPosition no-ops on Static), the reconcile pass must
    // remove+re-add the static body at the authored pose for Static bodies.
}

// ===========================================================================
// Task 3: paused scale -> collider rebuild
// ===========================================================================

TEST_CASE("paused scale-up rebuilds fixtures at effective size, pose preserved", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {2,3}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);

    reg.GetComponent<Transform>(e)->scale = glm::vec3(2.0f, 2.0f, 1.0f);
    paused(reg);                                   // reconcile: rebuild fixtures

    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->FixtureCount(bh) == 1u);     // count preserved across rebuild
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));      // 0.5 * 2
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
    const Phys::Vec2 bp = Arcane::Detail::Physics2D::Access::Solver(*res)->Position(bh); // body did not move
    CHECK(static_cast<float>(bp.x) == Approx(2.0f).margin(1e-4f));
    CHECK(static_cast<float>(bp.y) == Approx(3.0f).margin(1e-4f));
    CHECK(reg.GetComponent<Arcane::PhysicsBodyRef2D>(e)->appliedScale == glm::vec2(2.0f, 2.0f));
}

TEST_CASE("paused non-uniform scale rebuilds per-axis", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->scale = glm::vec3(3.0f, 1.0f, 1.0f);
    paused(reg);
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.5f).margin(1e-4f));      // 0.5 * 3
    CHECK(he.y == Approx(0.5f).margin(1e-4f));      // 0.5 * 1
}

TEST_CASE("unchanged scale does not rebuild fixtures (no compounding)", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->scale = glm::vec3(2.0f, 2.0f, 1.0f);
    paused(reg);                                   // rebuild once

    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    const Phys::FixtureHandle before = Arcane::Detail::Physics2D::Access::Solver(*res)->GetBodyFixture(bh, 0u);
    for (int i = 0; i < 3; ++i) paused(reg);       // scale unchanged -> no rebuild
    const Phys::FixtureHandle after = Arcane::Detail::Physics2D::Access::Solver(*res)->GetBodyFixture(bh, 0u);

    // appliedScale suppresses re-rebuild: the fixture identity is unchanged.
    // (FixtureHandle is a {index, generation} slot handle, like BodyHandle.)
    CHECK(before.index == after.index);
    CHECK(before.generation == after.generation);
    // Dims did not compound.
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("mint at scale then reconcile does not double-apply", "[transform-sync]")
{
    Astra::Registry reg;
    BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic, {2.0f,2.0f});
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // CREATE bakes scale 2 (he == 1.0)
    paused(reg);                                   // reconcile: scale unchanged -> no rebuild
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));      // NOT 2.0 (no double-scale)
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("body still steps after a scale rebuild", "[transform-sync]")
{
    Astra::Registry reg;
    // Gravity on for this one: prove the rebuilt body is a live dynamic body.
    // NEGATIVE y since the +Y flip (F4 plan 1 T2) -- this case authors its own
    // WorldDef rather than going through the engine default, so the sign is
    // the test's own statement of "down".
    RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents2D(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = -10.0f;
    reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Phys::PhysicsWorld>(wd)));
    Astra::Entity e = reg.CreateEntity();
    Transform lt; lt.position = glm::vec3(0.0f);
    reg.AddComponent<Transform>(e, lt);
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Arcane::BodyType2D::Dynamic; rb.fixedRotation = true;  // dynamic AABB invariant
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Aabb; fx.halfW = 0.5f; fx.halfH = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});

    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->scale = glm::vec3(2.0f, 2.0f, 1.0f);
    paused(reg);                                   // rebuild fixtures

    Arcane::PhysicsSystem2D play(kDt, /*stepWorld=*/true);
    for (int i = 0; i < 10; ++i) play(reg);        // must fall under gravity
    CHECK(reg.GetComponent<Transform>(e)->position.y < -0.1f);
}

// ---- review-coverage follow-ups -------------------------------------------

TEST_CASE("paused author move zeroes a moving body's velocity", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    // Author a non-zero velocity so the reconcile's zeroing is actually observable
    // (without this, a default (0,0) velocity would pass even if SetVelocity(0) were gone).
    reg.GetComponent<Arcane::RigidBody2D>(e)->velocity = glm::vec2(3.0f, -2.0f);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // mint applies the authored velocity

    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    REQUIRE(static_cast<float>(Arcane::Detail::Physics2D::Access::Solver(*res)->Velocity(bh).x) == Approx(3.0f).margin(1e-4f));  // present pre-move

    reg.GetComponent<Transform>(e)->position = glm::vec3(5.0f, 0.0f, 0.0f);  // author move
    paused(reg);                                   // reconcile teleports + zeroes velocity

    const Phys::Vec2 bv = Arcane::Detail::Physics2D::Access::Solver(*res)->Velocity(bh);
    CHECK(static_cast<float>(bv.x) == Approx(0.0f).margin(1e-4f));   // "don't fling on resume"
    CHECK(static_cast<float>(bv.y) == Approx(0.0f).margin(1e-4f));
}

TEST_CASE("create scales a circle by the representative (max) axis", "[transform-sync]")
{
    Astra::Registry reg;
    RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents2D(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
    reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Phys::PhysicsWorld>(wd)));

    Astra::Entity e = reg.CreateEntity();
    Transform lt; lt.position = glm::vec3(0.0f); lt.scale = glm::vec3(3.0f, 1.0f, 1.0f);  // non-uniform
    reg.AddComponent<Transform>(e, lt);
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Arcane::BodyType2D::Kinematic;   // circle: no dynamic-AABB constraint
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Circle; fx.radius = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});

    Arcane::PhysicsSystem2D sys(kDt, /*stepWorld=*/false);
    sys(reg);

    // Circle uses radius * max(|sx|,|sy|) = 0.5 * 3 -> a circle's world AABB is square,
    // so both half-extents are 1.5 (NOT 1.5 x 0.5 -- a circle has no distinguished axis).
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.5f).margin(1e-4f));
    CHECK(he.y == Approx(1.5f).margin(1e-4f));
}

// ---- Astra adoption Task 7: the Changed<Transform> gate on PASS 3.5 ---------

TEST_CASE("untouched paused bodies are not visited by the reconcile", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {2,3}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // first pass: since == never, the body is seen once
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(Arcane::Detail::Physics2D::Access::Reconciled(*res) == 1u);

    for (int i = 0; i < 5; ++i) paused(reg);       // nothing written between passes
    CHECK(Arcane::Detail::Physics2D::Access::Reconciled(*res) == 1u);                  // PASS 4's own write-back did not re-trigger it

    reg.GetComponent<Transform>(e)->position = glm::vec3(5.0f, 3.0f, 0.0f);   // an author edit
    paused(reg);
    CHECK(Arcane::Detail::Physics2D::Access::Reconciled(*res) == 2u);
    CHECK(static_cast<float>(Arcane::Detail::Physics2D::Access::Solver(*res)->Position(Arcane::Detail::Physics2D::Access::Entities(*res).at(e)).x) == Approx(5.0f).margin(1e-4f));
}

TEST_CASE("a position-only paused edit does not rebuild fixtures", "[transform-sync]")
{
    // The exact appliedScale compare STAYS inside the gate: a position edit
    // changes Transform (so the gate admits the body) but must not cost a
    // RebuildScaledFixtures.
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle bh = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    const Phys::FixtureHandle before = Arcane::Detail::Physics2D::Access::Solver(*res)->GetBodyFixture(bh, 0u);

    reg.GetComponent<Transform>(e)->position = glm::vec3(4.0f, 0.0f, 0.0f);
    paused(reg);
    const Phys::FixtureHandle after = Arcane::Detail::Physics2D::Access::Solver(*res)->GetBodyFixture(bh, 0u);
    CHECK(before.index == after.index);
    CHECK(before.generation == after.generation);
    CHECK(static_cast<float>(Arcane::Detail::Physics2D::Access::Solver(*res)->Position(bh).x) == Approx(4.0f).margin(1e-4f));
}

// ---- 2D physics wiring Plan 1 Task 5: the paused-pass fixes ----------------

TEST_CASE("a paused pass writes back nothing: Transform and Arcane::RigidBody2D stay unstamped", "[transform-sync]")
{
    // Spec s4.1: PASS 4 is gated on stepWorld. Before, every paused pass wrote
    // Transform + velocity for every body, stamping every physics entity
    // changed each Edit frame (propagation recomposed them all) and flattening
    // an authored out-of-plane rotation to its Z turn.
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {1,2}, {0.5f,0.5f}, Arcane::BodyType2D::Kinematic);
    // An authored tilt OUT of the XY plane: the 2D solver has no state for it,
    // and a paused pass must leave it alone.
    const glm::quat tilt = glm::angleAxis(0.3f, glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f)));
    reg.GetComponent<Transform>(e)->rotation = tilt;

    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // mints the body
    const Astra::Tick afterMint = reg.CurrentTick();
    paused(reg);                                   // a second paused pass: nothing authored changed
    paused(reg);

    // Nothing stamped since the mint pass: a Changed<Transform> view since
    // afterMint is empty, and so is one over Arcane::RigidBody2D.
    int changedT = 0, changedRb = 0;
    reg.CreateView<const Transform, Astra::Changed<Transform>>().Since(afterMint)
        .ForEach([&](Astra::Entity, const Transform&) { ++changedT; });
    reg.CreateView<const Arcane::RigidBody2D, Astra::Changed<Arcane::RigidBody2D>>().Since(afterMint)
        .ForEach([&](Astra::Entity, const Arcane::RigidBody2D&) { ++changedRb; });
    CHECK(changedT == 0);
    CHECK(changedRb == 0);
    // The tilt survived (PASS 4 used to overwrite rotation with RotationAboutZ).
    const glm::quat& r = reg.GetComponent<Transform>(e)->rotation;
    CHECK(std::abs(glm::dot(r, tilt)) == Approx(1.0f).margin(1e-5f));
}

TEST_CASE("a paused Arcane::Collider2D edit re-mints the body with the new shape", "[transform-sync]")
{
    // Spec s4.1a: Arcane::Collider2D is tracked; a paused PASS 1 destroys a body whose
    // Arcane::Collider2D changed since lastReconcile and PASS 2 re-mints it the same
    // pass. Observable as the fixture's world half-extents.
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Kinematic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle before = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    CHECK(Fixture0HalfExtents(reg).x == Approx(0.5f).margin(1e-4f));

    for (int i = 0; i < 3; ++i) paused(reg);       // untouched: the body is NOT re-minted
    CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).at(e) == before);

    reg.GetComponent<Arcane::Collider2D>(e)->fixtures[0].halfW = 1.5f;   // the Inspector's edit, stamped by Mut
    paused(reg);
    REQUIRE(Arcane::Detail::Physics2D::Access::Entities(*res).count(e) == 1);
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->IsValid(Arcane::Detail::Physics2D::Access::Entities(*res).at(e)));
    CHECK_FALSE(Arcane::Detail::Physics2D::Access::Solver(*res)->IsValid(before));       // the old body is gone
    CHECK(Fixture0HalfExtents(reg).x == Approx(1.5f).margin(1e-4f));
}

TEST_CASE("a paused Arcane::RigidBody2D type edit re-mints; removing Arcane::Collider2D destroys", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Kinematic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle first = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->TypeSlot(first.index) == Phys::BodyType::Kinematic);

    reg.GetComponent<Arcane::RigidBody2D>(e)->type = Arcane::BodyType2D::Static;
    paused(reg);
    const Phys::BodyHandle second = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    CHECK_FALSE(Arcane::Detail::Physics2D::Access::Solver(*res)->IsValid(first));
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->TypeSlot(second.index) == Phys::BodyType::Static);

    reg.RemoveComponent<Arcane::Collider2D>(e);
    paused(reg);
    CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).count(e) == 0);
    CHECK_FALSE(Arcane::Detail::Physics2D::Access::Solver(*res)->IsValid(second));
}

TEST_CASE("a fixture-less entity's Arcane::PhysicsBodyRef2D is cleared, never left to alias another body after a world re-mint",
          "[transform-sync]")
{
    // 2026-09-12 review finding 1. PASS 1's erase and PASS 2's empty-fixtures
    // return used to leave ref.handle at its old {index, gen}. A FRESH world
    // (gravity re-mint, Play->Stop restore, structural undo) hands out
    // {index, gen} from the same sequence again, so whichever entity mints
    // into that slot next holds the SAME handle -- and PASS 3.5 / PASS 4
    // trust world.IsValid(handle) alone, so an author move of the
    // fixture-less entity teleported SOMEONE ELSE's body.
    Astra::Registry reg;
    Astra::Entity a = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Kinematic);
    Astra::Entity b = AddAabbBody  (reg, {2,0}, {0.5f,0.5f}, Arcane::BodyType2D::Kinematic);
    Astra::Entity c = AddAabbBody  (reg, {4,0}, {0.5f,0.5f}, Arcane::BodyType2D::Kinematic);
    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(Arcane::Detail::Physics2D::Access::Entities(*res).size() == 3);

    // The Inspector removes B's only fixture (the Vector editor's [-]): the
    // paused PASS 1 destroys B's body on Changed<Arcane::Collider2D>.
    reg.GetComponent<Arcane::Collider2D>(b)->fixtures.clear();
    paused(reg);
    CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).count(b) == 0);
    CHECK(reg.GetComponent<Arcane::PhysicsBodyRef2D>(b)->handle == 0u);   // cleared, not dangling

    // Any world replacement. A and C re-mint into the fresh world; B has
    // nothing to mint and must not keep a handle the fresh world will reuse.
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
    reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Phys::PhysicsWorld>(wd)));
    res = reg.GetResource<Arcane::PhysicsWorld2D>();
    paused(reg);
    REQUIRE(Arcane::Detail::Physics2D::Access::Entities(*res).count(a) == 1);
    REQUIRE(Arcane::Detail::Physics2D::Access::Entities(*res).count(c) == 1);
    CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).count(b) == 0);
    CHECK(reg.GetComponent<Arcane::PhysicsBodyRef2D>(b)->handle == 0u);
    CHECK_FALSE(reg.GetComponent<Arcane::PhysicsBodyRef2D>(b)->handle == reg.GetComponent<Arcane::PhysicsBodyRef2D>(c)->handle);

    // The consequence that made this a defect: moving B must not move C.
    const Phys::Vec2 cBefore = Arcane::Detail::Physics2D::Access::Solver(*res)->Position(Arcane::Detail::Physics2D::Access::Entities(*res).at(c));
    reg.GetComponent<Transform>(b)->position.x = 9.0f;
    paused(reg);
    const Phys::Vec2 cAfter = Arcane::Detail::Physics2D::Access::Solver(*res)->Position(Arcane::Detail::Physics2D::Access::Entities(*res).at(c));
    CHECK(static_cast<float>(cAfter.x) == Approx(static_cast<float>(cBefore.x)));
    CHECK(static_cast<float>(cAfter.y) == Approx(static_cast<float>(cBefore.y)));
}

TEST_CASE("a stepping pass never re-mints on its own velocity write-back", "[transform-sync]")
{
    // The re-mint criteria are PAUSED-ONLY: PASS 4 writes Arcane::RigidBody2D::velocity
    // every step, which would otherwise read as an author edit next pass.
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Arcane::BodyType2D::Dynamic);
    Arcane::PhysicsSystem2D stepping(kDt, /*stepWorld=*/true);
    stepping(reg);
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    const Phys::BodyHandle h = Arcane::Detail::Physics2D::Access::Entities(*res).at(e);
    for (int i = 0; i < 10; ++i) stepping(reg);
    CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).at(e) == h);            // same body across ten steps
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->IsValid(h));
}

TEST_CASE("an entity authored without Arcane::PhysicsBodyRef2D is minted anyway", "[transform-sync]")
{
    // The Inspector can never add Arcane::PhysicsBodyRef2D (it is structure-locked), so
    // PASS 1.5 adds it for any Arcane::RigidBody2D + Arcane::Collider2D entity that lacks it.
    Astra::Registry reg;
    RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents2D(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
    reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Phys::PhysicsWorld>(wd)));
    Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<Transform>(e, Transform{});
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Arcane::BodyType2D::Kinematic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Circle; fx.radius = 0.5f; col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);
    REQUIRE_FALSE(reg.HasComponent<Arcane::PhysicsBodyRef2D>(e));

    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld=*/false);
    paused(reg);
    REQUIRE(reg.HasComponent<Arcane::PhysicsBodyRef2D>(e));
    auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(Arcane::Detail::Physics2D::Access::Entities(*res).count(e) == 1);
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->IsValid(Arcane::Detail::Physics2D::Access::Entities(*res).at(e)));
}

TEST_CASE("Arcane::PhysicsSystem2D declares the scheduler contract: exclusive, before propagation", "[transform-sync]")
{
    STATIC_REQUIRE(Arcane::PhysicsSystem2D::RequiresExclusive);
    STATIC_REQUIRE(std::tuple_size_v<Arcane::PhysicsSystem2D::BeforeTypes> == 1);
    STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<0, Arcane::PhysicsSystem2D::BeforeTypes>, TransformPropagationSystem>);
}
