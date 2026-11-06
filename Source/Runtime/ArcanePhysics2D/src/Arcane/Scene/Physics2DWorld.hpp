#pragma once
#include <Arcane/Modules/ArcanePhysics2DApi.hpp>

// PhysicsWorld2D. PhysicsSystem2D's declaration is PhysicsSystem2D.hpp
// (included below); its pass body is PhysicsSystem.cpp, so a game that
// includes Physics2D.hpp does not compile the solver passes.
// PhysicsWorld2D's destructor is out of line: the solver object stays an
// incomplete type here.

#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Core/Api.hpp>
#include <Arcane/Ecs.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/Physics2DHandles.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>
#include <Arcane/Scene/PhysicsSystem2D.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Arcane::Detail::Physics2D
{
    struct Access;

    struct RetiredFixture
    {
        FixtureHandle handle{};
        std::uint32_t index = 0;
    };

    struct BodyRecord
    {
        Arcane::Entity               entity = Arcane::Entity::Invalid();
        Arcane::Guid                 guid{};
        std::vector<FixtureHandle>   fixtures;
        bool                         retired = false;
        std::vector<RetiredFixture>  retiredFixtures;
    };

    struct EventBuffers
    {
        std::vector<Arcane::ContactBegin2D> contactBegin;
        std::vector<Arcane::ContactEnd2D>   contactEnd;
        std::vector<Arcane::ContactHit2D>   contactHit;
        std::vector<Arcane::SensorBegin2D>  sensorBegin;
        std::vector<Arcane::SensorEnd2D>    sensorEnd;

        void Clear() noexcept
        {
            contactBegin.clear(); contactEnd.clear(); contactHit.clear();
            sensorBegin.clear(); sensorEnd.clear();
        }

        [[nodiscard]] Arcane::PhysicsEvents2D View() const noexcept
        {
            return { contactBegin, contactEnd, contactHit, sensorBegin, sensorEnd };
        }
    };
}

namespace Arcane
{
    // Transient registry resource. The solver, the entity map and the event
    // records are private. Games use the methods below.
    struct PhysicsWorld2D
    {
        PhysicsWorld2D() = default;
        ARC_PHYSICS2D_API ~PhysicsWorld2D();
        ARC_PHYSICS2D_API PhysicsWorld2D(PhysicsWorld2D&&) noexcept;
        ARC_PHYSICS2D_API PhysicsWorld2D& operator=(PhysicsWorld2D&&) noexcept;
        PhysicsWorld2D(const PhysicsWorld2D&) = delete;
        PhysicsWorld2D& operator=(const PhysicsWorld2D&) = delete;

        ARC_PHYSICS2D_API PhysicsEvents2D StepEvents() const;
        ARC_PHYSICS2D_API PhysicsEvents2D FrameEvents() const;
        ARC_PHYSICS2D_API void BeginFrame();
        ARC_PHYSICS2D_API void ContactsOf(Arcane::Entity entity, std::vector<ContactPoint2D>& out) const;
        ARC_PHYSICS2D_API BodyMotion2D Motion(Arcane::Entity entity, const RigidBody2D& body) const;
        ARC_PHYSICS2D_API void SetVelocity(Arcane::Entity entity, RigidBody2D& body, float velocityX, float velocityY);

        static constexpr bool AstraTransientResource = true;

        template<typename Archive>
        void Serialize(Archive& /*ar*/) {}

    private:
        friend struct PhysicsSystem2D;
        friend struct ::Arcane::Detail::AbiManifest::Physics2DAccess;   // hashes these private members (Physics2D/AbiManifestPhysics2D.hpp)
        friend struct ::Arcane::Detail::Physics2D::Access;

        ARC_PHYSICS2D_API void RecordBody(Arcane::Entity entity, Guid guid, Detail::Physics2D::BodyHandle handle,
                                     std::vector<Detail::Physics2D::FixtureHandle> fixtures);
        ARC_PHYSICS2D_API void RetireBody(Detail::Physics2D::BodyHandle handle);
        ARC_PHYSICS2D_API void CaptureStep();

        std::unique_ptr<Detail::Physics2D::PhysicsWorld>                  world;
        std::unordered_map<Arcane::Entity, Detail::Physics2D::BodyHandle> entityToBody;
        Arcane::Tick  lastReconcile = 0;
        std::uint32_t reconciled = 0;
        std::unordered_map<std::uint64_t, Detail::Physics2D::BodyRecord> bodyRecords;
        Detail::Physics2D::EventBuffers stepEvents;
        Detail::Physics2D::EventBuffers frameEvents;
    };
} // namespace Arcane
