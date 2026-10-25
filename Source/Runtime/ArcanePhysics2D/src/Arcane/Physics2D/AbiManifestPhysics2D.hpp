#pragma once

// The PHYSICS2D group of the plugin ABI digest (3D-readiness Plan A Task 8 fix
// round 1): PhysicsWorld2D (its private members, through Physics2DAccess), the
// contact / sensor event rows, PhysicsEvents2D, BodyMotion2D and the
// Detail::Physics2D body records. They moved here from the Engine group
// (Plugin/AbiManifestEngine.hpp) so that GameModule.hpp no longer includes a
// Physics2D (or, through it, a Manifold2D) header: the Engine group is what an
// ArcaneEngine-only game can include. Hashed by AbiManifestPhysics2DMix();
// folded into the digest by FoldGroups() (Core/AbiManifestHash.hpp). A game TU
// that can include this header (its closure names ArcanePhysics2D) hashes it
// under its own flags (Plugin/AbiManifestGame.hpp); one that cannot folds the
// committed kGamePluginABIDigestPhysics2D (Core/AbiVersion.hpp) instead. The
// record format is the Engine group's.

#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Core/AbiManifestHash.hpp>
#include <Arcane/Physics2D.hpp>
#include <Arcane/Physics2DFwd.hpp>
#include <Arcane/Scene/Physics2DHandles.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>

#include <cstddef>
#include <cstdint>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

namespace Arcane
{
    namespace Detail::AbiManifest
    {
        // PhysicsWorld2D befriends this struct
        // (friend struct ::Arcane::Detail::AbiManifest::Physics2DAccess;).
        struct Physics2DAccess
        {
        static constexpr void R_PhysicsWorld2D(Fnv& f) noexcept
        {
            f.Type("Arcane::PhysicsWorld2D", Opaque(sizeof(Arcane::PhysicsWorld2D)), Opaque(alignof(Arcane::PhysicsWorld2D)));
            f.Field("world", offsetof(Arcane::PhysicsWorld2D, world), sizeof(Arcane::PhysicsWorld2D::world), alignof(decltype(Arcane::PhysicsWorld2D::world)), "std::unique_ptr<Detail::Physics2D::PhysicsWorld>");
            f.Field("entityToBody", Opaque(offsetof(Arcane::PhysicsWorld2D, entityToBody)), Opaque(sizeof(Arcane::PhysicsWorld2D::entityToBody)), Opaque(alignof(decltype(Arcane::PhysicsWorld2D::entityToBody))), "std::unordered_map<Arcane::Entity, Detail::Physics2D::BodyHandle>");
            f.Field("lastReconcile", Opaque(offsetof(Arcane::PhysicsWorld2D, lastReconcile)), sizeof(Arcane::PhysicsWorld2D::lastReconcile), alignof(decltype(Arcane::PhysicsWorld2D::lastReconcile)), "Arcane::Tick");
            f.Field("reconciled", Opaque(offsetof(Arcane::PhysicsWorld2D, reconciled)), sizeof(Arcane::PhysicsWorld2D::reconciled), alignof(decltype(Arcane::PhysicsWorld2D::reconciled)), "std::uint32_t");
            f.Field("bodyRecords", Opaque(offsetof(Arcane::PhysicsWorld2D, bodyRecords)), Opaque(sizeof(Arcane::PhysicsWorld2D::bodyRecords)), Opaque(alignof(decltype(Arcane::PhysicsWorld2D::bodyRecords))), "std::unordered_map<std::uint64_t, Detail::Physics2D::BodyRecord>");
            f.Field("stepEvents", Opaque(offsetof(Arcane::PhysicsWorld2D, stepEvents)), Opaque(sizeof(Arcane::PhysicsWorld2D::stepEvents)), Opaque(alignof(decltype(Arcane::PhysicsWorld2D::stepEvents))), "Detail::Physics2D::EventBuffers");
            f.Field("frameEvents", Opaque(offsetof(Arcane::PhysicsWorld2D, frameEvents)), Opaque(sizeof(Arcane::PhysicsWorld2D::frameEvents)), Opaque(alignof(decltype(Arcane::PhysicsWorld2D::frameEvents))), "Detail::Physics2D::EventBuffers");
        }
        };
    }

    constexpr std::uint64_t AbiManifestPhysics2DMix() noexcept
    {
        using Detail::AbiManifest::Fnv;
        using Detail::AbiManifest::Opaque;
        Fnv f;
        f.Type("Arcane::ContactSide2D", sizeof(Arcane::ContactSide2D), alignof(Arcane::ContactSide2D));
        f.Field("entity", offsetof(Arcane::ContactSide2D, entity), sizeof(Arcane::ContactSide2D::entity), alignof(decltype(Arcane::ContactSide2D::entity)), "Arcane::Entity");
        f.Field("guid", offsetof(Arcane::ContactSide2D, guid), sizeof(Arcane::ContactSide2D::guid), alignof(decltype(Arcane::ContactSide2D::guid)), "Guid");
        f.Field("fixture", offsetof(Arcane::ContactSide2D, fixture), sizeof(Arcane::ContactSide2D::fixture), alignof(decltype(Arcane::ContactSide2D::fixture)), "std::uint32_t");
        f.Type("Arcane::ContactBegin2D", sizeof(Arcane::ContactBegin2D), alignof(Arcane::ContactBegin2D));
        f.Field("a", offsetof(Arcane::ContactBegin2D, a), sizeof(Arcane::ContactBegin2D::a), alignof(decltype(Arcane::ContactBegin2D::a)), "ContactSide2D");
        f.Field("b", offsetof(Arcane::ContactBegin2D, b), sizeof(Arcane::ContactBegin2D::b), alignof(decltype(Arcane::ContactBegin2D::b)), "ContactSide2D");
        f.Type("Arcane::ContactEnd2D", sizeof(Arcane::ContactEnd2D), alignof(Arcane::ContactEnd2D));
        f.Field("a", offsetof(Arcane::ContactEnd2D, a), sizeof(Arcane::ContactEnd2D::a), alignof(decltype(Arcane::ContactEnd2D::a)), "ContactSide2D");
        f.Field("b", offsetof(Arcane::ContactEnd2D, b), sizeof(Arcane::ContactEnd2D::b), alignof(decltype(Arcane::ContactEnd2D::b)), "ContactSide2D");
        f.Type("Arcane::ContactHit2D", sizeof(Arcane::ContactHit2D), alignof(Arcane::ContactHit2D));
        f.Field("a", offsetof(Arcane::ContactHit2D, a), sizeof(Arcane::ContactHit2D::a), alignof(decltype(Arcane::ContactHit2D::a)), "ContactSide2D");
        f.Field("b", offsetof(Arcane::ContactHit2D, b), sizeof(Arcane::ContactHit2D::b), alignof(decltype(Arcane::ContactHit2D::b)), "ContactSide2D");
        f.Field("point", offsetof(Arcane::ContactHit2D, point), sizeof(Arcane::ContactHit2D::point), alignof(decltype(Arcane::ContactHit2D::point)), "glm::vec2");
        f.Field("normal", offsetof(Arcane::ContactHit2D, normal), sizeof(Arcane::ContactHit2D::normal), alignof(decltype(Arcane::ContactHit2D::normal)), "glm::vec2");
        f.Field("approachSpeed", offsetof(Arcane::ContactHit2D, approachSpeed), sizeof(Arcane::ContactHit2D::approachSpeed), alignof(decltype(Arcane::ContactHit2D::approachSpeed)), "float");
        f.Type("Arcane::SensorBegin2D", sizeof(Arcane::SensorBegin2D), alignof(Arcane::SensorBegin2D));
        f.Field("sensor", offsetof(Arcane::SensorBegin2D, sensor), sizeof(Arcane::SensorBegin2D::sensor), alignof(decltype(Arcane::SensorBegin2D::sensor)), "ContactSide2D");
        f.Field("visitor", offsetof(Arcane::SensorBegin2D, visitor), sizeof(Arcane::SensorBegin2D::visitor), alignof(decltype(Arcane::SensorBegin2D::visitor)), "ContactSide2D");
        f.Type("Arcane::SensorEnd2D", sizeof(Arcane::SensorEnd2D), alignof(Arcane::SensorEnd2D));
        f.Field("sensor", offsetof(Arcane::SensorEnd2D, sensor), sizeof(Arcane::SensorEnd2D::sensor), alignof(decltype(Arcane::SensorEnd2D::sensor)), "ContactSide2D");
        f.Field("visitor", offsetof(Arcane::SensorEnd2D, visitor), sizeof(Arcane::SensorEnd2D::visitor), alignof(decltype(Arcane::SensorEnd2D::visitor)), "ContactSide2D");
        f.Type("Arcane::ContactPoint2D", sizeof(Arcane::ContactPoint2D), alignof(Arcane::ContactPoint2D));
        f.Field("self", offsetof(Arcane::ContactPoint2D, self), sizeof(Arcane::ContactPoint2D::self), alignof(decltype(Arcane::ContactPoint2D::self)), "ContactSide2D");
        f.Field("other", offsetof(Arcane::ContactPoint2D, other), sizeof(Arcane::ContactPoint2D::other), alignof(decltype(Arcane::ContactPoint2D::other)), "ContactSide2D");
        f.Field("normal", offsetof(Arcane::ContactPoint2D, normal), sizeof(Arcane::ContactPoint2D::normal), alignof(decltype(Arcane::ContactPoint2D::normal)), "glm::vec2");
        f.Field("pointCount", offsetof(Arcane::ContactPoint2D, pointCount), sizeof(Arcane::ContactPoint2D::pointCount), alignof(decltype(Arcane::ContactPoint2D::pointCount)), "std::uint32_t");
        f.Type("Arcane::PhysicsEvents2D", sizeof(Arcane::PhysicsEvents2D), alignof(Arcane::PhysicsEvents2D));
        f.Field("contactBegin", offsetof(Arcane::PhysicsEvents2D, contactBegin), sizeof(Arcane::PhysicsEvents2D::contactBegin), alignof(decltype(Arcane::PhysicsEvents2D::contactBegin)), "std::span<const ContactBegin2D>");
        f.Field("contactEnd", offsetof(Arcane::PhysicsEvents2D, contactEnd), sizeof(Arcane::PhysicsEvents2D::contactEnd), alignof(decltype(Arcane::PhysicsEvents2D::contactEnd)), "std::span<const ContactEnd2D>");
        f.Field("contactHit", offsetof(Arcane::PhysicsEvents2D, contactHit), sizeof(Arcane::PhysicsEvents2D::contactHit), alignof(decltype(Arcane::PhysicsEvents2D::contactHit)), "std::span<const ContactHit2D>");
        f.Field("sensorBegin", offsetof(Arcane::PhysicsEvents2D, sensorBegin), sizeof(Arcane::PhysicsEvents2D::sensorBegin), alignof(decltype(Arcane::PhysicsEvents2D::sensorBegin)), "std::span<const SensorBegin2D>");
        f.Field("sensorEnd", offsetof(Arcane::PhysicsEvents2D, sensorEnd), sizeof(Arcane::PhysicsEvents2D::sensorEnd), alignof(decltype(Arcane::PhysicsEvents2D::sensorEnd)), "std::span<const SensorEnd2D>");
        f.Type("Arcane::BodyMotion2D", sizeof(Arcane::BodyMotion2D), alignof(Arcane::BodyMotion2D));
        f.Field("velocityX", offsetof(Arcane::BodyMotion2D, velocityX), sizeof(Arcane::BodyMotion2D::velocityX), alignof(decltype(Arcane::BodyMotion2D::velocityX)), "float");
        f.Field("velocityY", offsetof(Arcane::BodyMotion2D, velocityY), sizeof(Arcane::BodyMotion2D::velocityY), alignof(decltype(Arcane::BodyMotion2D::velocityY)), "float");
        f.Field("bodyReady", offsetof(Arcane::BodyMotion2D, bodyReady), sizeof(Arcane::BodyMotion2D::bodyReady), alignof(decltype(Arcane::BodyMotion2D::bodyReady)), "bool");
        f.Field("supported", offsetof(Arcane::BodyMotion2D, supported), sizeof(Arcane::BodyMotion2D::supported), alignof(decltype(Arcane::BodyMotion2D::supported)), "bool");
        f.Type("Arcane::Detail::Physics2D::RetiredFixture", sizeof(Arcane::Detail::Physics2D::RetiredFixture), alignof(Arcane::Detail::Physics2D::RetiredFixture));
        f.Field("handle", offsetof(Arcane::Detail::Physics2D::RetiredFixture, handle), sizeof(Arcane::Detail::Physics2D::RetiredFixture::handle), alignof(decltype(Arcane::Detail::Physics2D::RetiredFixture::handle)), "FixtureHandle");
        f.Field("index", offsetof(Arcane::Detail::Physics2D::RetiredFixture, index), sizeof(Arcane::Detail::Physics2D::RetiredFixture::index), alignof(decltype(Arcane::Detail::Physics2D::RetiredFixture::index)), "std::uint32_t");
        f.Type("Arcane::Detail::Physics2D::BodyRecord", Opaque(sizeof(Arcane::Detail::Physics2D::BodyRecord)), Opaque(alignof(Arcane::Detail::Physics2D::BodyRecord)));
        f.Field("entity", offsetof(Arcane::Detail::Physics2D::BodyRecord, entity), sizeof(Arcane::Detail::Physics2D::BodyRecord::entity), alignof(decltype(Arcane::Detail::Physics2D::BodyRecord::entity)), "Arcane::Entity");
        f.Field("guid", offsetof(Arcane::Detail::Physics2D::BodyRecord, guid), sizeof(Arcane::Detail::Physics2D::BodyRecord::guid), alignof(decltype(Arcane::Detail::Physics2D::BodyRecord::guid)), "Arcane::Guid");
        f.Field("fixtures", Opaque(offsetof(Arcane::Detail::Physics2D::BodyRecord, fixtures)), Opaque(sizeof(Arcane::Detail::Physics2D::BodyRecord::fixtures)), Opaque(alignof(decltype(Arcane::Detail::Physics2D::BodyRecord::fixtures))), "std::vector<FixtureHandle>");
        f.Field("retired", Opaque(offsetof(Arcane::Detail::Physics2D::BodyRecord, retired)), sizeof(Arcane::Detail::Physics2D::BodyRecord::retired), alignof(decltype(Arcane::Detail::Physics2D::BodyRecord::retired)), "bool");
        f.Field("retiredFixtures", Opaque(offsetof(Arcane::Detail::Physics2D::BodyRecord, retiredFixtures)), Opaque(sizeof(Arcane::Detail::Physics2D::BodyRecord::retiredFixtures)), Opaque(alignof(decltype(Arcane::Detail::Physics2D::BodyRecord::retiredFixtures))), "std::vector<RetiredFixture>");
        f.Type("Arcane::Detail::Physics2D::EventBuffers", Opaque(sizeof(Arcane::Detail::Physics2D::EventBuffers)), Opaque(alignof(Arcane::Detail::Physics2D::EventBuffers)));
        f.Field("contactBegin", Opaque(offsetof(Arcane::Detail::Physics2D::EventBuffers, contactBegin)), Opaque(sizeof(Arcane::Detail::Physics2D::EventBuffers::contactBegin)), Opaque(alignof(decltype(Arcane::Detail::Physics2D::EventBuffers::contactBegin))), "std::vector<Arcane::ContactBegin2D>");
        f.Field("contactEnd", Opaque(offsetof(Arcane::Detail::Physics2D::EventBuffers, contactEnd)), Opaque(sizeof(Arcane::Detail::Physics2D::EventBuffers::contactEnd)), Opaque(alignof(decltype(Arcane::Detail::Physics2D::EventBuffers::contactEnd))), "std::vector<Arcane::ContactEnd2D>");
        f.Field("contactHit", Opaque(offsetof(Arcane::Detail::Physics2D::EventBuffers, contactHit)), Opaque(sizeof(Arcane::Detail::Physics2D::EventBuffers::contactHit)), Opaque(alignof(decltype(Arcane::Detail::Physics2D::EventBuffers::contactHit))), "std::vector<Arcane::ContactHit2D>");
        f.Field("sensorBegin", Opaque(offsetof(Arcane::Detail::Physics2D::EventBuffers, sensorBegin)), Opaque(sizeof(Arcane::Detail::Physics2D::EventBuffers::sensorBegin)), Opaque(alignof(decltype(Arcane::Detail::Physics2D::EventBuffers::sensorBegin))), "std::vector<Arcane::SensorBegin2D>");
        f.Field("sensorEnd", Opaque(offsetof(Arcane::Detail::Physics2D::EventBuffers, sensorEnd)), Opaque(sizeof(Arcane::Detail::Physics2D::EventBuffers::sensorEnd)), Opaque(alignof(decltype(Arcane::Detail::Physics2D::EventBuffers::sensorEnd))), "std::vector<Arcane::SensorEnd2D>");
        Detail::AbiManifest::Physics2DAccess::R_PhysicsWorld2D(f);

        return f.h;
    }
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
