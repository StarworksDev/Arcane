// Settings arc S6-10: the physics debug overlay's look and toggles are the
// debug.physics.* cvars. Arcane::PhysicsDebugDrawOptions2D inherits the four settings
// blocks, so a default-constructed options block still carries the pre-sweep
// literals bit for bit; MakePhysicsDebugDrawOptions() carries the published
// values. The [physics-debug] rich/transform/capsule tests are the behavioural
// proof that the drawing did not move.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include "Helpers/PhysicsDebugDrawSolver.hpp"
#include <Manifold2D/Physics/Narrowphase/NarrowphaseTrace.hpp>

#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace Arcane;

TEST_CASE("sweep: physics debug defaults are the pre-sweep literals", "[sweep][physics-debug]")
{
    const Arcane::PhysicsDebugDrawOptions2D o{};
    CHECK(Test::SameBits(o.lineThickness, 1.0f));
    CHECK(o.contacts); CHECK_FALSE(o.aabbs); CHECK(o.velocities); CHECK(o.comMarkers); CHECK(o.orientations);
    CHECK(Test::SameBits(o.contactMarkerSize, 0.03f));
    CHECK(Test::SameBits(o.velocityScale, 0.15f));
    CHECK(Test::SameBits(o.velocityMinSpeed, 0.05f));
    CHECK(Test::SameBits(o.comMarkerSize, 0.05f));
    CHECK(Test::SameBits(o.orientationTickLen, 0.18f));
    Test::RequireDefault("debug.physics.lineThickness", CVarValue::Float32(1.0f));
    Test::RequireDefault("debug.physics.draw.aabbs", CVarValue::Bool(false));
}

TEST_CASE("sweep: MakePhysicsDebugDrawOptions carries the published values", "[sweep][physics-debug]")
{
    Test::SkipIfCompiledOut("debug.physics.draw.aabbs");
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("debug.physics.draw.aabbs"), CVarValue::Bool(true), SetBy::Code);
    reg.PublishImmediate();
    CHECK(MakePhysicsDebugDrawOptions().aabbs);
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
}

TEST_CASE("sweep: physics debug palette, trace and arrow defaults are the pre-sweep literals", "[sweep][physics-debug]")
{
    const Arcane::PhysicsDebugDrawOptions2D o{};
    CHECK(Test::SameBits(o.manifoldNormalLength, 20.0f));   // "world units" (pre-metres; converted as is)
    CHECK(Test::SameBits(o.manifoldPointPx, 3.0f));
    CHECK(Test::SameBits(o.normalLength, 28.0f));
    CHECK(Test::SameBits(o.emphasis, 1.0f));
    CHECK(Test::SameBits(o.traceLineThickness, 1.5f));
    CHECK(o.kinematic     == CVarColor{ 0.2f, 1.0f, 0.4f, 1.0f });
    CHECK(o.staticBody    == CVarColor{ 0.4f, 0.7f, 1.0f, 1.0f });   // cvar debug.physics.color.static
    CHECK(o.contact       == CVarColor{ 1.0f, 0.2f, 1.0f, 1.0f });
    CHECK(o.treeFat       == CVarColor{ 0.30f, 0.90f, 1.00f, 0.25f });
    CHECK(o.residencyGrid == CVarColor{ 1.00f, 0.70f, 0.20f, 0.35f });
    CHECK(o.subject       == CVarColor{ 1.00f, 0.95f, 0.35f, 1.0f });
    CHECK(o.island7       == CVarColor{ 0.85f, 0.85f, 0.85f, 1.0f });
    CHECK(o.narrowphase0  == CVarColor{ 0.70f, 0.70f, 0.70f, 1.0f });   // Separated / unknown kind
    CHECK(o.narrowphase6  == CVarColor{ 0.80f, 0.45f, 1.00f, 1.0f });   // Mpr
    Test::RequireDefault("debug.physics.color.static", CVarValue::Color({ 0.4f, 0.7f, 1.0f, 1.0f }));
    Test::RequireDefault("debug.physics.color.island0", CVarValue::Color({ 1.0f, 0.35f, 0.35f, 1.0f }));
    Test::RequireDefault("debug.physics.trace.lineThickness", CVarValue::Float32(1.5f));
    Test::RequireDefault("debug.physics.trace.normalLength", CVarValue::Float32(28.0f));
    Test::RequireDefault("debug.physics.manifoldNormalLength", CVarValue::Float32(20.0f));
    Test::RequireDefault("debug.physics.velocityMinSpeed", CVarValue::Float32(0.05f));
}

TEST_CASE("sweep: debug.physics.* descriptors are Game Dev, per-project preferences, Live", "[sweep][physics-debug]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    for (const char* name : { "debug.physics.lineThickness", "debug.physics.draw.contacts",
                              "debug.physics.color.kinematic", "debug.physics.color.narrowphase3",
                              "debug.physics.trace.emphasis", "debug.physics.style.sleepingDim",
                              "debug.physics.style.axisHalfLenPx" })
    {
        INFO(name);
        if (!Test::InThisBuild(name)) continue;   // Dev: compiled out of Dist
        const auto d = reg.Explain(name);
        REQUIRE(d);
        CHECK(d->audience == Audience::Game);
        CHECK(d->scope == SettingScope::PreferencesProject);
        CHECK(d->apply == ApplyMode::Live);
        CHECK(HasFlag(d->flags, CVarFlags::Dev));
        CHECK(HasFlag(d->flags, CVarFlags::Archive));
        CHECK_FALSE(HasFlag(d->flags, CVarFlags::Deterministic));
        CHECK_FALSE(d->help.empty());
    }
}

// S6-43 fix round 1: the debug.physics.style.* row (inventory "Physics debug
// draw", SETTING; frozen names) is a settings block, not ARC_CONSTANT bases.
TEST_CASE("sweep: debug.physics.style defaults are the pre-sweep literals", "[sweep][physics-debug]")
{
    const Arcane::PhysicsDebugStyleSettings2D st{};
    CHECK(Test::SameBits(st.arrowShortLen, 12.0f));
    CHECK(Test::SameBits(st.arrowHeadLen, 6.0f));
    CHECK(Test::SameBits(st.arrowHeadSpread, 0.6f));
    CHECK(Test::SameBits(st.sleepingDim, 0.35f));
    CHECK(Test::SameBits(st.emphasisFloor, 0.15f));
    CHECK(Test::SameBits(st.subjectThicknessScale, 1.3f));
    CHECK(Test::SameBits(st.axisHiThicknessScale, 1.8f));
    CHECK(Test::SameBits(st.normalThicknessScale, 1.4f));
    CHECK(Test::SameBits(st.axisHalfLenPx, 60.0f));
    CHECK(Test::SameBits(st.anchorDiscRadius, 4.0f));
    CHECK(Test::SameBits(st.contactDiscRadius, 3.0f));
    Test::RequireDefault("debug.physics.style.arrowShortLen", CVarValue::Float32(12.0f));
    Test::RequireDefault("debug.physics.style.arrowHeadLen", CVarValue::Float32(6.0f));
    Test::RequireDefault("debug.physics.style.arrowHeadSpread", CVarValue::Float32(0.6f));
    Test::RequireDefault("debug.physics.style.sleepingDim", CVarValue::Float32(0.35f));
    Test::RequireDefault("debug.physics.style.emphasisFloor", CVarValue::Float32(0.15f));
    Test::RequireDefault("debug.physics.style.subjectThicknessScale", CVarValue::Float32(1.3f));
    Test::RequireDefault("debug.physics.style.axisHiThicknessScale", CVarValue::Float32(1.8f));
    Test::RequireDefault("debug.physics.style.normalThicknessScale", CVarValue::Float32(1.4f));
    Test::RequireDefault("debug.physics.style.axisHalfLenPx", CVarValue::Float32(60.0f));
    Test::RequireDefault("debug.physics.style.anchorDiscRadius", CVarValue::Float32(4.0f));
    Test::RequireDefault("debug.physics.style.contactDiscRadius", CVarValue::Float32(3.0f));
}

namespace
{
    // Records the radius of every Circle the overlay submits.
    struct CircleRecorder final : Batcher2D
    {
        std::vector<float> radii;
        void Begin(uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Glyph(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2, float r, glm::vec4) override { radii.push_back(r); }
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        Batch2DStats Stats() const override { return {}; }
        void QuadWorld(uint16_t, const Guid&, const std::array<glm::vec3, 4>&,
                       glm::vec2, glm::vec2, glm::vec4) override {}
        void CircleWorld(glm::vec3, glm::vec3, glm::vec3, float, glm::vec4) override {}
        void SetViewProjection(const glm::mat4&) override {}
    };
}

TEST_CASE("sweep: the narrowphase overlay draws with the published debug.physics.style", "[sweep][physics-debug]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    // A separated trace of two point shapes (no manifold points, no axes): the
    // overlay emits the two shape discs (2 px) and the contact-anchor ring.
    const Manifold2D::Physics::NarrowphaseTrace trace{};
    const Affine2D view{};

    CircleRecorder before;
    DrawNarrowphaseWorldOverlay(trace, -1, before, view);
    REQUIRE(before.radii.size() == 3);
    CHECK(Test::SameBits(before.radii.back(), 4.0f));   // the default anchorDiscRadius

    Test::SkipIfCompiledOut("debug.physics.style.anchorDiscRadius");
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("debug.physics.style.anchorDiscRadius"), CVarValue::Float32(9.0f), SetBy::Code);
    reg.PublishImmediate();
    CircleRecorder after;
    DrawNarrowphaseWorldOverlay(trace, -1, after, view);
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();

    REQUIRE(after.radii.size() == 3);
    CHECK(Test::SameBits(after.radii.back(), 9.0f));
}
