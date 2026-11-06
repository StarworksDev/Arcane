#pragma once
#include <Arcane/Modules/ArcanePhysics2DApi.hpp>
#include <Arcane/Core/Api.hpp>
#include <Arcane/Scene/PhysicsDebugDrawOptions.hpp>
#include <Arcane/Scene/PhysicsDebugTypes.hpp>
#include <Arcane/Scene/PhysicsQuery.hpp>   // PhysicsDebugPrimitive + PhysicsDebugGeometry live there
#include <Arcane/Scene/ViewTransform.hpp>
#include <cstdint>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <optional>
#include <vector>

namespace Manifold2D::Physics { class PhysicsWorld; struct NarrowphaseTrace; }
namespace Arcane
{
    ARC_PHYSICS2D_API PhysicsDebugGeometry GatherPhysicsDebug(
        const Manifold2D::Physics::PhysicsWorld& world,
        const PhysicsDebugDrawOptions2D& opts,
        const PhysicsDebugStyleSettings2D& style);
    ARC_PHYSICS2D_API void GatherPhysicsDebug(
        const Manifold2D::Physics::PhysicsWorld& world,
        const PhysicsDebugDrawOptions2D& opts,
        const PhysicsDebugStyleSettings2D& style, PhysicsDebugGeometry& out);
    ARC_PHYSICS2D_API PhysicsDebugGeometry GatherNarrowphaseWorldOverlay(
        const Manifold2D::Physics::NarrowphaseTrace& trace,
        int stepIndex, const Affine2D& view,
        std::optional<float> lineThickness, std::optional<float> emphasis,
        const PhysicsDebugTraceSettings2D& trc,
        const PhysicsDebugColorSettings2D& pal,
        const PhysicsDebugStyleSettings2D& style);
    ARC_PHYSICS2D_API void GatherNarrowphaseWorldOverlay(
        const Manifold2D::Physics::NarrowphaseTrace& trace,
        int stepIndex, const Affine2D& view,
        std::optional<float> lineThickness, std::optional<float> emphasis,
        const PhysicsDebugTraceSettings2D& trc,
        const PhysicsDebugColorSettings2D& pal,
        const PhysicsDebugStyleSettings2D& style, PhysicsDebugGeometry& out);
}
