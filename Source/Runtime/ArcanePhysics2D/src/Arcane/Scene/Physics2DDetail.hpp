#pragma once

// Engine-only 2D physics helpers. Physics2D.hpp does not include this.
// Conversions, fixture builders and the private-member accessor live here.

#include <Arcane/Config/Bindings/Physics2DBinding.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>

#include <Manifold2D/Physics/Fixture.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Arcane::Detail::Physics2D
{
    namespace Phys = ::Manifold2D::Physics;

    // Author-edit detection. Metres and radians. They only reject a
    // SetAngle/GetAngle round trip, not a real gizmo edit.
    ARC_CONSTANT("math identity / tolerance: authored-position round-trip noise")
    inline constexpr float kAuthorPosEps = 1e-5f;
    ARC_CONSTANT("math identity / tolerance: authored-rotation round-trip noise")
    inline constexpr float kAuthorRotEps = 1e-5f;

    struct Access
    {
        static PhysicsWorld* Solver(PhysicsWorld2D& w) noexcept { return w.world.get(); }
        static const PhysicsWorld* Solver(const PhysicsWorld2D& w) noexcept { return w.world.get(); }
        static auto& Entities(PhysicsWorld2D& w) noexcept { return w.entityToBody; }
        static const auto& Entities(const PhysicsWorld2D& w) noexcept { return w.entityToBody; }
        static Arcane::Tick& LastReconcile(PhysicsWorld2D& w) noexcept { return w.lastReconcile; }
        static std::uint32_t& Reconciled(PhysicsWorld2D& w) noexcept { return w.reconciled; }
        static auto& Records(PhysicsWorld2D& w) noexcept { return w.bodyRecords; }
        static const auto& Records(const PhysicsWorld2D& w) noexcept { return w.bodyRecords; }
        static void SetSolver(PhysicsWorld2D& w, std::unique_ptr<PhysicsWorld> solver) noexcept
        {
            w.world = std::move(solver);
        }
    };

    inline PhysicsWorld2D Adopt(std::unique_ptr<PhysicsWorld> solver)
    {
        PhysicsWorld2D w;
        Access::SetSolver(w, std::move(solver));
        return w;
    }

    [[nodiscard]] constexpr Phys::BodyType ToVendor(BodyType2D t) noexcept
    {
        switch (t)
        {
        case BodyType2D::Static:    return Phys::BodyType::Static;
        case BodyType2D::Kinematic: return Phys::BodyType::Kinematic;
        case BodyType2D::Dynamic:   return Phys::BodyType::Dynamic;
        }
        return Phys::BodyType::Kinematic;
    }

    [[nodiscard]] constexpr Phys::ShapeKind ToVendor(ShapeKind2D k) noexcept
    {
        switch (k)
        {
        case ShapeKind2D::Circle:  return Phys::ShapeKind::Circle;
        case ShapeKind2D::Capsule: return Phys::ShapeKind::Capsule;
        case ShapeKind2D::Aabb:    return Phys::ShapeKind::Aabb;
        case ShapeKind2D::Polygon: return Phys::ShapeKind::Polygon;
        }
        return Phys::ShapeKind::Circle;
    }

    [[nodiscard]] inline Phys::BroadphaseKind ToBroadphaseKind(PhysicsBroadphase2D b) noexcept
    {
        switch (b)
        {
        case PhysicsBroadphase2D::Tree: return Phys::BroadphaseKind::Tree;
        case PhysicsBroadphase2D::Hash: return Phys::BroadphaseKind::Hash;
        case PhysicsBroadphase2D::Sap:  return Phys::BroadphaseKind::Sap;
        }
        return Phys::BroadphaseKind::Tree;
    }

    [[nodiscard]] inline Phys::WorldDef ToWorldDef(const PhysicsWorldSettings2D& s)
    {
        Phys::WorldDef wd;
        wd.broadphase             = ToBroadphaseKind(s.broadphase);
        wd.hashCellSize           = s.hashCellSize;
        wd.substepCount           = s.substepCount;
        wd.contactHertz           = s.contactHertz;
        wd.contactDampingRatio    = s.contactDampingRatio;
        wd.restitutionThreshold   = s.restitutionThreshold;
        wd.contactPushMaxVelocity = s.contactPushMaxVelocity;
        wd.maxLinearVelocity      = s.maxLinearVelocity;
        wd.sleepThreshold         = s.sleepThreshold;
        return wd;
    }

    [[nodiscard]] constexpr std::uint64_t PackBody(Phys::BodyHandle h) noexcept
    {
        return (static_cast<std::uint64_t>(h.index) << 32) | h.generation;
    }

    [[nodiscard]] constexpr Phys::BodyHandle UnpackBody(std::uint64_t packed) noexcept
    {
        Phys::BodyHandle h;
        h.index      = static_cast<std::uint32_t>(packed >> 32);
        h.generation = static_cast<std::uint32_t>(packed);
        return h;
    }

    [[nodiscard]] inline Phys::Shape MakeScaledShape(const Fixture2D& f, glm::vec2 scale)
    {
        const float sx   = std::abs(scale.x);
        const float sy   = std::abs(scale.y);
        const float sMax = std::max(sx, sy);
        switch (f.kind)
        {
        case ShapeKind2D::Circle:  return Phys::MakeCircle(f.radius * sMax);
        case ShapeKind2D::Capsule: return Phys::MakeCapsule(f.halfLen * sx, f.radius * sy);
        case ShapeKind2D::Aabb:    return Phys::MakeAabb(f.halfW * sx, f.halfH * sy);
        case ShapeKind2D::Polygon:
            assert(false && "Detail::Physics2D::MakeScaledShape: ShapeKind2D::Polygon not supported");
            return Phys::MakeCircle(f.radius * sMax);
        }
        return Phys::MakeCircle(f.radius * sMax);
    }

    [[nodiscard]] inline float AngleDelta(float a, float b)
    {
        ARC_CONSTANT("math identity / tolerance: pi")
        constexpr float kPi  = 3.14159265358979323846f;
        ARC_CONSTANT("math identity / tolerance: tau = 2 pi")
        constexpr float kTau = 6.28318530717958647692f;
        float d = a - b;
        while (d >  kPi) d -= kTau;
        while (d < -kPi) d += kTau;
        return std::abs(d);
    }

    [[nodiscard]] inline Phys::FixtureDef MakeFixtureDef(const Fixture2D& f,
                                                         glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        Phys::FixtureDef fd;
        fd.shape = MakeScaledShape(f, scale);
        fd.localPos   = Phys::Vec2(f.localPos.x * scale.x, f.localPos.y * scale.y);
        fd.localAngle = static_cast<Phys::Real>(f.localAngle);
        fd.density     = static_cast<Phys::Real>(f.density);
        fd.friction    = static_cast<Phys::Real>(f.friction);
        fd.restitution = static_cast<Phys::Real>(f.restitution);
        fd.categoryBits = f.categoryBits;
        fd.maskBits     = f.maskBits;
        fd.isSensor = f.isSensor;
        fd.contactEvents = f.contactEvents;
        fd.sensorEvents  = f.sensorEvents;
        fd.hitEvents     = f.hitEvents;
        return fd;
    }

    [[nodiscard]] inline std::vector<Phys::FixtureHandle> RebuildScaledFixtures(Phys::PhysicsWorld& world,
                                                                               Phys::BodyHandle bh,
                                                                               const Collider2D& col,
                                                                               glm::vec2 scale)
    {
        const std::uint32_t n = world.FixtureCount(bh);
        std::vector<Phys::FixtureHandle> old;
        old.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
            old.push_back(world.GetBodyFixture(bh, i));

        std::vector<Phys::FixtureHandle> neu;
        neu.reserve(col.fixtures.size());
        for (const Fixture2D& f : col.fixtures)
        {
            Phys::FixtureDef fd = MakeFixtureDef(f, scale);
            neu.push_back(world.AddFixture(bh, fd));
        }
        for (Phys::FixtureHandle fh : old)
            world.DropFixture(fh);
        return neu;
    }
}
