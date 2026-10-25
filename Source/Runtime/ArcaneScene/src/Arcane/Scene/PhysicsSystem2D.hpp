#pragma once
#include <Arcane/Scene/PhysicsSystem2DApi.hpp>

// PhysicsSystem2D's declaration. The pass body is PhysicsSystem.cpp, one
// translation unit, exported from ArcanePhysics2D. Physics2DWorld.hpp
// includes this header.

#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Ecs.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

namespace Arcane
{
    struct PhysicsSystem2D
        : Arcane::SystemTraits<Arcane::Reads<Collider2D>,
                               Arcane::Writes<Arcane::Transform, PhysicsBodyRef2D, RigidBody2D>,
                               Arcane::Before<Arcane::TransformPropagationSystem>>
    {
        static constexpr bool RequiresExclusive = true;

        explicit PhysicsSystem2D(float fixedDt, bool stepWorld = true) noexcept
            : m_fixedDt(fixedDt), m_stepWorld(stepWorld) {}

        ARC_PHYSICS_SYSTEM2D_API void operator()(Arcane::Registry& reg);

    private:
        friend struct ::Arcane::Detail::AbiManifest::Access;   // hashes these private members (Plugin/AbiManifestEngine.hpp)
        float m_fixedDt;
        bool  m_stepWorld;
    };
} // namespace Arcane
