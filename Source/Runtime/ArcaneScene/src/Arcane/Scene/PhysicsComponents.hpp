#pragma once

// Physics ECS components: authored data for Arcane's 2D physics. RigidBody2D,
// Collider2D (a fixture list) and PhysicsBodyRef2D (the runtime handle). PhysicsSettings2D,
// the per-scene gravity override, lives in Components.hpp.

#include <Arcane/Core/Constant.hpp>
#include <Arcane/Ecs.hpp>
#include <Arcane/Reflection.hpp>
#include <Arcane/Scene/PhysicsBodyRef2DDisk.hpp>
#include <Arcane/Scene/TypeList.hpp>

#include <Astra/Serialization/BinaryArchive.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <glm/vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Arcane
{
    // Same enumerator names and values as the solver. Polygon is part of the
    // value set so a hand-built kind round-trips, and it is not reflected:
    // Fixture2D carries no vertex array, so an Inspector pick would assert.
    enum class BodyType2D : std::uint8_t
    {
        Static    = 0,
        Kinematic = 1,
        Dynamic   = 2,
    };

    enum class ShapeKind2D : std::uint8_t
    {
        Circle  = 0,
        Capsule = 1,
        Aabb    = 2,
        Polygon = 3,
    };

    ARC_REFLECT_ENUM(BodyType2D)
        ARC_REFLECT_ENUM_VALUE(BodyType2D, Static)
        ARC_REFLECT_ENUM_VALUE(BodyType2D, Kinematic)
        ARC_REFLECT_ENUM_VALUE(BodyType2D, Dynamic)
    ARC_END_REFLECT_ENUM()

    ARC_REFLECT_ENUM(ShapeKind2D)
        ARC_REFLECT_ENUM_VALUE(ShapeKind2D, Circle)
        ARC_REFLECT_ENUM_VALUE(ShapeKind2D, Capsule)
        ARC_REFLECT_ENUM_VALUE(ShapeKind2D, Aabb)
    ARC_END_REFLECT_ENUM()

    struct RigidBody2D
    {
        ARC_CHANGE_TRACKED

        BodyType2D type          = BodyType2D::Kinematic;
        glm::vec2 velocity     {0.0f, 0.0f};
        float     mass          = 0.0f;
        float     linearDamping = 0.0f;
        bool      fixedRotation = false;
        bool      bullet        = false;
    };

    struct Fixture2D
    {
        ShapeKind2D kind      = ShapeKind2D::Circle;
        float     radius    = 0.5f;
        float     halfLen   = 0.0f;
        float     halfW     = 0.5f;
        float     halfH     = 0.5f;

        glm::vec2 localPos   {0.0f, 0.0f};
        float     localAngle = 0.0f;

        float density     = 1.0f;
        float friction    = 0.3f;
        float restitution = 0.0f;

        uint32_t categoryBits = 0x00000001u;
        uint32_t maskBits     = 0xFFFFFFFFu;

        bool isSensor = false;

        bool contactEvents = true;
        bool sensorEvents  = true;
        bool hitEvents     = false;
    };

    struct Collider2D
    {
        ARC_CHANGE_TRACKED

        std::vector<Fixture2D> fixtures;

        template<typename Archive>
        void Serialize(Archive& ar)
        {
            ar(fixtures);
        }
    };

    // Runtime handle. Hidden, not serialized. Re-established by System on load.
    // handle is the solver's body handle packed as (index << 32) | generation
    // (Detail::Physics2D::PackBody); 0 is no body. This header names no solver
    // type, so the packing lives in the physics module.
    struct PhysicsBodyRef2D
    {
        std::uint64_t handle = 0;
        glm::vec2     appliedScale{1.0f, 1.0f};
    };

    ARC_REFLECT_TYPE(RigidBody2D)
        ARC_REFLECT_FIELD(RigidBody2D, type)
        ARC_REFLECT_FIELD(RigidBody2D, velocity)
        ARC_REFLECT_FIELD(RigidBody2D, mass)
        ARC_REFLECT_FIELD(RigidBody2D, linearDamping)
        ARC_REFLECT_FIELD(RigidBody2D, fixedRotation)
        ARC_REFLECT_FIELD(RigidBody2D, bullet)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(Fixture2D)
        ARC_REFLECT_FIELD(Fixture2D, kind)
        ARC_REFLECT_FIELD(Fixture2D, radius)
        ARC_REFLECT_FIELD(Fixture2D, halfLen)
        ARC_REFLECT_FIELD(Fixture2D, halfW)
        ARC_REFLECT_FIELD(Fixture2D, halfH)
        ARC_REFLECT_FIELD(Fixture2D, localPos)
        ARC_REFLECT_FIELD(Fixture2D, localAngle)
        ARC_REFLECT_FIELD(Fixture2D, density)
        ARC_REFLECT_FIELD(Fixture2D, friction)
        ARC_REFLECT_FIELD(Fixture2D, restitution)
        ARC_REFLECT_FIELD(Fixture2D, categoryBits)
        ARC_REFLECT_FIELD(Fixture2D, maskBits)
        ARC_REFLECT_FIELD(Fixture2D, isSensor)
        ARC_REFLECT_FIELD(Fixture2D, contactEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report contact begin/end for this fixture. Both fixtures of a pair must allow it.")
        ARC_REFLECT_FIELD(Fixture2D, sensorEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report sensor enter/exit, as the sensor or as the visitor. Both must allow it.")
        ARC_REFLECT_FIELD(Fixture2D, hitEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report impacts faster than physics.events.hitThreshold. Either fixture suffices.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(Collider2D)
        ARC_REFLECT_FIELD(Collider2D, fixtures)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(PhysicsBodyRef2D)
        ARC_REFLECT_FIELD(PhysicsBodyRef2D, handle)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
        ARC_REFLECT_FIELD(PhysicsBodyRef2D, appliedScale)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
    ARC_END_REFLECT_TYPE()

    // EXACTLY the order RegisterPhysicsComponents2D registers in. Appended after the
    // scene roster (EngineRoster.hpp); ProjectHost's VerifySharedTypeContext walks both.
    using PhysicsComponentRoster2D = TypeList<RigidBody2D, Collider2D, PhysicsBodyRef2D>;

    inline void RegisterPhysicsComponents2D(Arcane::ComponentRegistry& creg)
    {
        creg.RegisterComponent<RigidBody2D>();
        creg.RegisterComponent<Collider2D>();
        creg.RegisterComponent<PhysicsBodyRef2D>();
    }

    inline void RegisterPhysicsComponents2D(Arcane::Registry& reg)
    {
        RegisterPhysicsComponents2D(*reg.GetComponentRegistry());
    }
}

// Binary form of PhysicsBodyRef2D. Version 2 writes appliedScale only (two
// floats); the handle is runtime state and loads as 0. Version 1 was the whole
// component written as bytes (an 8-byte BodyHandle {index, generation}, then
// the scale), and Migrate reads that explicit layout. JSON is unaffected: the
// reflected Serializable(false) attributes govern that walk, not this one.
namespace Astra
{
    template<>
    struct SerializationTraits<Arcane::PhysicsBodyRef2D>
    {
        static constexpr bool HasCustomSerializer = true;
        static constexpr bool IsTrivial = false;
        ARC_CONSTANT("file format: the PhysicsBodyRef2D binary component version")
        static constexpr uint32_t Version = 2;
        ARC_CONSTANT("file format: the oldest PhysicsBodyRef2D binary version Migrate reads")
        static constexpr uint32_t MinVersion = 1;

        template<typename Archive>
        static void Serialize(Archive& ar, Arcane::PhysicsBodyRef2D& value)
        {
            if (ar.IsLoading())
                value.handle = 0;
            float x = value.appliedScale.x;
            float y = value.appliedScale.y;
            ar(x);
            ar(y);
            if (ar.IsLoading())
                value.appliedScale = glm::vec2{ x, y };
        }

        // Reached for every stored version >= MinVersion that is not Version.
        static void Migrate(BinaryReader& ar, Arcane::PhysicsBodyRef2D& value, uint32_t storedVersion)
        {
            if (storedVersion == 1)
            {
                Arcane::Detail::PhysicsBodyRef2DDiskV1 old{};
                ar.ReadBytes(&old, sizeof old);
                if (ar.HasError())
                    return;
                value.handle = 0;
                value.appliedScale = glm::vec2{ old.scaleX, old.scaleY };
                return;
            }
            // A version this build does not know: the reader has no error
            // setter, so ask for one byte more than remains. The size check
            // sets CorruptedData and returns before any copy.
            std::byte sink{};
            ar.ReadBytes(&sink, ar.GetRemaining() + 1);
        }
    };
}
