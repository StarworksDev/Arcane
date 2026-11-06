// Rich physics-debug overlay (Sandbox unify-to-outlines pivot, Item A).
//
// DrawPhysicsDebug is now the SINGLE canonical Sandbox renderer (the scenes no
// longer add filled SpriteRenderer quads), so it grew richer per-body debug
// geometry so the user can "really see what's happening":
//   * a velocity vector  -- a line from the body COM along its linear velocity
//   * a center-of-mass marker -- a tiny cross/disc at the world COM
//   * an orientation tick -- a short line along the body's local +x so rotation
//     is visible even on a circle
// Each is gated behind a Arcane::PhysicsDebugDrawOptions2D bool flag (sane defaults).
//
// CPU-only (tag [render], no graphics device): DrawPhysicsDebug takes the
// Batcher2D interface, so a recording mock captures the emitted Line/Circle
// submissions and we assert the new geometry appears (and is suppressed when
// the flag is off).

#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Manifold2D/Physics/Body.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include "Helpers/PhysicsDebugDrawSolver.hpp"
#include <Arcane/Scene/ViewTransform.hpp>   // the mirrored-affine case (F4 plan 1 T3)
#include <Arcane/Scene/RenderViewSettings.hpp>

#include <glm/glm.hpp>

using Catch::Approx;
using namespace Manifold2D::Physics;

namespace
{
    // Recording mock: captures every Line + Circle submission so a test can
    // count / inspect the emitted debug geometry.
    struct RecMock final : Arcane::Batcher2D
    {
        std::vector<std::pair<glm::vec2, glm::vec2>> lines;
        std::vector<std::pair<glm::vec2, float>>     circles;

        void Begin(uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, glm::vec2, glm::vec2,
                  glm::vec4, float) override {}
        void Glyph(glm::vec2, glm::vec2, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Line(glm::vec2 a, glm::vec2 b, float, glm::vec4) override
        {
            lines.emplace_back(a, b);
        }
        void Circle(glm::vec2 c, float r, glm::vec4) override
        {
            circles.emplace_back(c, r);
        }
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
        // World-space surface (F4 plan 1 T4): the overlay draws in pixels.
        void QuadWorld(uint16_t, const Arcane::Guid&, const std::array<glm::vec3, 4>&,
                       glm::vec2, glm::vec2, glm::vec4) override {}
        void CircleWorld(glm::vec3, glm::vec3, glm::vec3, float, glm::vec4) override {}
        void SetViewProjection(const glm::mat4&) override {}
    };

    // A world with one moving dynamic box (no gravity, no floor) so the only
    // emitted geometry is the box outline + whatever rich overlays the options
    // enable. Returns by value via the out-params: the world is built in-place.
    void OneMovingBox(PhysicsWorld& w, BodyHandle& outHandle)
    {
        BodyDef bd;
        bd.type     = BodyType::Dynamic;
        bd.position = Vec2(Real(10), Real(10));
        // Polygon box so it can carry a nonzero angle (a dynamic Aabb is
        // forced fixedRotation); 3x2 m half-extents.
        const std::vector<Vec2> verts = {
            Vec2(Real(-3), Real(-2)), Vec2(Real(3), Real(-2)),
            Vec2(Real(3), Real(2)),   Vec2(Real(-3), Real(2)),
        };
        bd.shape   = MakePolygon(verts);
        bd.density = Real(1);
        outHandle  = w.AddBody(bd);
        w.SetVelocity(outHandle, Vec2(Real(20), Real(0)));   // moving +x fast
        w.SetAngle(outHandle, Real(0.5));                    // tilted so the tick reads
    }
}

TEST_CASE("PhysicsDebug rich: velocity vector emitted only when enabled", "[render]")
{
    WorldDef wd;  // gravity 0 -- isolates kinematics from the debug-draw overlay
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);
    BodyHandle h;
    OneMovingBox(w, h);

    // Baseline: velocity vector OFF -> outline lines only (4 for the box).
    {
        RecMock off;
        Arcane::PhysicsDebugDrawOptions2D opts;
        opts.velocities  = false;
        opts.comMarkers  = false;
        opts.orientations = false;
        opts.contacts    = false;
        Arcane::DrawPhysicsDebug(w, off, opts);
        CHECK(off.lines.size() == 4);   // just the 4 polygon edges
    }

    // Velocity vector ON -> at least one MORE line (the velocity ray).
    {
        RecMock on;
        Arcane::PhysicsDebugDrawOptions2D opts;
        opts.velocities   = true;
        opts.comMarkers   = false;
        opts.orientations = false;
        opts.contacts     = false;
        Arcane::DrawPhysicsDebug(w, on, opts);
        CHECK(on.lines.size() > 4);   // outline + velocity ray
    }
}

TEST_CASE("PhysicsDebug rich: orientation tick + COM marker gated by flags", "[render]")
{
    WorldDef wd;  // gravity 0 -- isolates kinematics from the debug-draw overlay
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);
    BodyHandle h;
    OneMovingBox(w, h);

    // Orientation tick ON (everything else off) -> at least one extra line.
    {
        RecMock on;
        Arcane::PhysicsDebugDrawOptions2D opts;
        opts.velocities   = false;
        opts.comMarkers   = false;
        opts.orientations = true;
        opts.contacts     = false;
        Arcane::DrawPhysicsDebug(w, on, opts);
        CHECK(on.lines.size() > 4);   // outline + orientation tick
    }

    // COM marker ON -> at least one extra primitive (line cross or disc).
    {
        RecMock on;
        Arcane::PhysicsDebugDrawOptions2D opts;
        opts.velocities   = false;
        opts.comMarkers   = true;
        opts.orientations = false;
        opts.contacts     = false;
        Arcane::DrawPhysicsDebug(w, on, opts);
        CHECK((on.lines.size() > 4 || on.circles.size() > 0));
    }
}

TEST_CASE("PhysicsDebug rich: a resting body draws no velocity ray", "[render]")
{
    WorldDef wd;  // gravity 0 -- isolates kinematics from the debug-draw overlay
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);

    // A static body never moves -> with velocities on it must still emit
    // ONLY its outline (no zero-length velocity ray clutter).
    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = Vec2(Real(5), Real(5));
    bd.shape    = MakeAabb(Real(2), Real(1));
    w.AddBody(bd);

    RecMock m;
    Arcane::PhysicsDebugDrawOptions2D opts;
    opts.velocities   = true;
    opts.comMarkers   = false;
    opts.orientations = false;
    opts.contacts     = false;
    Arcane::DrawPhysicsDebug(w, m, opts);

    // 4 outline lines for the AABB, and NO velocity ray (static -> v == 0).
    CHECK(m.lines.size() == 4);
}

// F4 plan 1 T3 (fix round 1): the overlay under a MIRRORED affine -- the
// orthographic +Y-up view on a y-down canvas (Affine2D scale (60,-60), offset
// (400,300)). An oriented box is rotated in WORLD space and each corner is
// projected; it must never be rotated about the projected centre in screen
// space, which a mirrored map would spin the wrong way.
TEST_CASE("PhysicsDebug projects an oriented box's WORLD corners through a mirrored affine",
          "[render][physics][debug]")
{
    WorldDef wd;
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);

    // A 1 x 0.5 m box (polygon, so it can carry an angle) centred at world (0, +1),
    // turned +0.3 rad. No step, so the pose stays exactly as authored.
    const float     angle = 0.3f;
    const glm::vec2 centre(0.0f, 1.0f);
    const glm::vec2 local[4] = { {-0.5f, -0.25f}, {0.5f, -0.25f}, {0.5f, 0.25f}, {-0.5f, 0.25f} };
    std::vector<Vec2> verts;
    for (const glm::vec2& l : local) verts.emplace_back(Real(l.x), Real(l.y));
    BodyDef bd;
    bd.type     = BodyType::Dynamic;
    bd.position = Vec2(Real(centre.x), Real(centre.y));
    bd.shape    = MakePolygon(verts);
    bd.density  = Real(1);
    const BodyHandle h = w.AddBody(bd);
    w.SetAngle(h, Real(angle));

    const auto affine = Arcane::Ortho2DView({0.0f, 0.0f}, 5.0f, {800u, 600u}).AsAffine2D();
    REQUIRE(affine.has_value());
    REQUIRE(affine->scale.y < 0.0f);   // the mirror is what this case is about

    RecMock rec;
    Arcane::PhysicsDebugDrawOptions2D opts;
    opts.view = *affine;
    opts.velocities = opts.comMarkers = opts.orientations = opts.contacts = false;
    Arcane::DrawPhysicsDebug(w, rec, opts);
    REQUIRE(rec.lines.size() == 4);   // the four edges, nothing else

    // Expected: rotate each local corner in WORLD, translate, THEN project.
    const float c = std::cos(angle), s = std::sin(angle);
    glm::vec2 worldCorner[4];
    glm::vec2 expected[4];
    for (int i = 0; i < 4; ++i)
    {
        worldCorner[i] = centre + glm::vec2(c * local[i].x - s * local[i].y,
                                            s * local[i].x + c * local[i].y);
        expected[i]    = affine->Point(worldCorner[i]);
    }
    const auto same = [](glm::vec2 a, glm::vec2 b) {
        return std::abs(a.x - b.x) < 1e-2f && std::abs(a.y - b.y) < 1e-2f;
    };

    // Every emitted endpoint is one of the projected WORLD corners, and every
    // projected corner is emitted (as a line endpoint) at least once.
    for (const auto& ln : rec.lines)
    {
        bool firstOk = false, secondOk = false;
        for (const glm::vec2& e : expected)
        {
            firstOk  = firstOk  || same(ln.first,  e);
            secondOk = secondOk || same(ln.second, e);
        }
        CHECK(firstOk);
        CHECK(secondOk);
        // ...and all of them sit ABOVE the viewport centre: world +1 m is UP.
        CHECK(ln.first.y  < 300.0f);
        CHECK(ln.second.y < 300.0f);
    }
    for (const glm::vec2& e : expected)
    {
        int hits = 0;
        for (const auto& ln : rec.lines)
            hits += (same(ln.first, e) ? 1 : 0) + (same(ln.second, e) ? 1 : 0);
        CHECK(hits >= 1);
    }

    // The mirror itself: the corner HIGHEST in world (largest world y) has the
    // SMALLEST canvas y. Under the old y-down map it would have had the largest.
    int topWorld = 0;
    for (int i = 1; i < 4; ++i)
        if (worldCorner[i].y > worldCorner[topWorld].y) topWorld = i;
    for (int i = 0; i < 4; ++i)
        if (i != topWorld) CHECK(expected[topWorld].y < expected[i].y);
}

TEST_CASE("onlyBody draws exactly one outline and no other overlay", "[physics][debug]")
{
    // Two circles; filter to the second: one Circle call, zero Lines (no
    // contacts, velocity rays, COM crosses or orientation ticks).
    WorldDef wd;
    PhysicsWorld w(wd);
    BodyDef a; a.type = BodyType::Static;  a.position = Vec2(0, 0); a.shape = MakeCircle(Real(0.5)); a.density = Real(1);
    BodyDef b; b.type = BodyType::Dynamic; b.position = Vec2(3, 0); b.shape = MakeCircle(Real(0.5)); b.density = Real(1);
    w.AddBody(a);
    const BodyHandle hb = w.AddBody(b);
    RecMock rec;
    Arcane::PhysicsDebugDrawOptions2D opts;   // defaults: contacts/velocity/COM/orientation ON
    opts.onlyBody = (static_cast<std::uint64_t>(hb.index) << 32) | hb.generation;
    Arcane::DrawPhysicsDebug(w, rec, opts);
    CHECK(rec.circles.size() == 1);
    CHECK(rec.lines.size() == 0);
    CHECK(rec.circles[0].first.x == Approx(3.0f));
}
