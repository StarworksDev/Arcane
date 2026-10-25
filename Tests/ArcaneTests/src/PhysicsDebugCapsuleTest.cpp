// The physics-debug overlay must draw a CAPSULE's outline (end circles + side
// lines) ROTATED by the body angle -- capsules rotate freely in v2, so the old
// "capsule axis is always horizontal" assumption in DrawPhysicsDebug is wrong.
//
// CPU-only (tag [render], no graphics device): DrawPhysicsDebug takes the
// Batcher2D interface, so a recording mock captures the emitted Line endpoints.

#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("PhysicsDebug: capsule outline rotates with the body", "[render]")
{
    WorldDef wd; // gravity 0, no floor -> no contacts, only the capsule outline
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);

    BodyDef bd;
    bd.type          = BodyType::Dynamic;
    bd.position      = Vec2(Real(10), Real(10));
    bd.shape         = MakeCapsule(Real(3), Real(1)); // halfLen 3, radius 1
    bd.density       = Real(1);
    bd.fixedRotation = false;
    const BodyHandle h = w.AddBody(bd);
    w.SetAngle(h, Real(0.7853981633974483)); // 45 deg -> a horizontal capsule would be flat

    LineMock mock;
    Arcane::DrawPhysicsDebug(w, mock); // identity camera (zoom 1, offset 0)

    // The two side lines of the capsule outline. At 45 deg they must be TILTED
    // (their endpoints differ in BOTH x and y); the pre-fix code drew them
    // axis-aligned horizontal (a.y == b.y) regardless of the body angle.
    REQUIRE(mock.lines.size() >= 2);
    // The 1.0f tilt threshold is coupled to the /10 content scale: side-line
    // |dy| ~ 4.24 at halfLen 3 / 45 deg, but a /100 re-divide would land it
    // at ~0.42 and fail spuriously -- re-derive alongside any future rescale.
    bool anyTilted = false;
    for (const auto& l : mock.lines)
        if (std::abs(l.first.y - l.second.y) > 1.0f) anyTilted = true;
    CHECK(anyTilted);
}
