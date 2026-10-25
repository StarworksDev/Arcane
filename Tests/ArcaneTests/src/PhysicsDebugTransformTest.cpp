// The physics-debug overlay must draw each body's outline reflecting its actual
// ROTATION and (rebuilt) SCALE:
//   * an Aabb (box) outline rotates with the body angle -- boxes are oriented
//     boxes in v2 (the core verts rotate), like the Capsule/Polygon cases.
//   * the outline follows the body's LIVE per-fixture shapes, so a paused
//     scale-reconcile (RebuildScaledFixtures: add a scaled fixture, drop the
//     old) is reflected -- NOT the stale single m_shape captured at AddBody.
//
// CPU-only (tag [render], no graphics device): DrawPhysicsDebug takes the
// Batcher2D interface, so a recording mock captures the emitted Line endpoints.

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Manifold2D/Physics/Fixture.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include "Helpers/PhysicsDebugDrawSolver.hpp"

#include <glm/glm.hpp>

using namespace Manifold2D::Physics;

namespace
{
    struct LineMock final : Arcane::Batcher2D
    {
        std::vector<std::pair<glm::vec2, glm::vec2>> lines;

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
        void Circle(glm::vec2, float, glm::vec4) override {}
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
        // World-space surface (F4 plan 1 T4): the overlay draws in pixels.
        void QuadWorld(uint16_t, const Arcane::Guid&, const std::array<glm::vec3, 4>&,
                       glm::vec2, glm::vec2, glm::vec4) override {}
        void CircleWorld(glm::vec3, glm::vec3, glm::vec3, float, glm::vec4) override {}
        void SetViewProjection(const glm::mat4&) override {}
    };
}

TEST_CASE("PhysicsDebug: aabb outline rotates with the body", "[render]")
{
    WorldDef wd; // gravity 0, no floor -> no contacts, only the box outline
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);

    // Kinematic: a box body CAN be rotated via SetAngle (the paused Inspector-
    // edit / script-driven path). A Dynamic Aabb is asserted fixedRotation (a
    // free-rotating dynamic box is disallowed), so Kinematic is the case that
    // rotates -- and Arcane::RigidBody2D defaults to Kinematic anyway.
    BodyDef bd;
    bd.type     = BodyType::Kinematic;
    bd.position = Vec2(Real(10), Real(10));
    bd.shape    = MakeAabb(Real(3), Real(1)); // wide box: hw 3, hh 1
    const BodyHandle h = w.AddBody(bd);
    w.SetAngle(h, Real(0.7853981633974483)); // 45 deg

    LineMock mock;
    Arcane::DrawPhysicsDebug(w, mock); // identity camera (zoom 1, offset 0)

    // At 45 deg EVERY box edge is diagonal (its endpoints differ in BOTH x and y).
    // The pre-fix Aabb case drew axis-aligned lines (each edge horizontal OR
    // vertical), so NO edge would satisfy dx>0.5 && dy>0.5.
    REQUIRE(mock.lines.size() >= 4);
    bool anyDiagonal = false;
    for (const auto& l : mock.lines)
    {
        const float dx = std::abs(l.first.x - l.second.x);
        const float dy = std::abs(l.first.y - l.second.y);
        if (dx > 0.5f && dy > 0.5f) anyDiagonal = true;
    }
    CHECK(anyDiagonal);
}

TEST_CASE("PhysicsDebug: outline follows rebuilt (scaled) fixtures, not stale m_shape",
          "[render]")
{
    WorldDef wd;
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);

    BodyDef bd;
    bd.type     = BodyType::Kinematic;
    bd.position = Vec2(Real(0), Real(0));
    bd.shape    = MakeAabb(Real(0.5), Real(0.5)); // unit box at the authored scale
    const BodyHandle h = w.AddBody(bd);

    // Simulate the paused scale reconcile (RebuildScaledFixtures): add a fixture
    // at 4x, then drop the original. m_shape stays at the 0.5 box; the live
    // fixture is the 2.0 box.
    const FixtureHandle fx0 = w.GetBodyFixture(h, 0);
    REQUIRE(w.IsValid(fx0));
    FixtureDef fd;
    fd.shape = MakeAabb(Real(2.0), Real(2.0)); // 4x half-extents
    w.AddFixture(h, fd);
    w.DropFixture(fx0);

    LineMock mock;
    Arcane::DrawPhysicsDebug(w, mock); // identity camera

    // The outline must span the SCALED box (half-extent 2 -> a corner at ~2 at
    // zoom 1, pos 0), NOT the stale m_shape (half-extent 0.5 -> corner at ~0.5).
    REQUIRE(!mock.lines.empty());
    float maxCoord = 0.0f;
    for (const auto& l : mock.lines)
        maxCoord = std::max({ maxCoord,
                              std::abs(l.first.x),  std::abs(l.first.y),
                              std::abs(l.second.x), std::abs(l.second.y) });
    CHECK(maxCoord > 1.5f); // pre-fix ~0.5 (stale m_shape); post-fix ~2.0
}
