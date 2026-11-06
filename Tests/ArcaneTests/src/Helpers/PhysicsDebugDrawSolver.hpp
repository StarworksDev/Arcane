#pragma once

// Test-only wrappers over the solver gather functions (3D-readiness Plan A,
// Task 7). Render no longer links ArcanePhysics2D: its debug overlay is painted
// from a PhysicsDebugGeometry (PaintPhysicsDebug) and Extract asks the
// solver-free proxy for that geometry. The solver-level debug-draw tests still
// hand a PhysicsWorld or a NarrowphaseTrace straight to the overlay, so these
// wrappers keep the old DrawPhysicsDebug / DrawNarrowphaseWorldOverlay calls:
// gather through ArcanePhysics2D, paint through Render.

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Scene/PhysicsDebugGeometry.hpp>

#include <optional>

namespace Arcane
{
    inline void DrawPhysicsDebug(const Manifold2D::Physics::PhysicsWorld& world,
                                 Batcher2D& batcher, const PhysicsDebugDrawOptions2D& opts = {})
    {
        PhysicsDebugGeometry geometry;
        GatherPhysicsDebug(world, opts, Settings<PhysicsDebugStyleSettings2D>(), geometry);
        PaintPhysicsDebug(batcher, geometry);
    }

    inline void DrawNarrowphaseWorldOverlay(const Manifold2D::Physics::NarrowphaseTrace& trace,
                                            int stepIndex, Batcher2D& batcher, const Affine2D& view,
                                            std::optional<float> lineThickness = std::nullopt,
                                            std::optional<float> emphasis = std::nullopt)
    {
        PhysicsDebugGeometry geometry;
        GatherNarrowphaseWorldOverlay(trace, stepIndex, view,
            lineThickness, emphasis, Settings<PhysicsDebugTraceSettings2D>(),
            Settings<PhysicsDebugColorSettings2D>(), Settings<PhysicsDebugStyleSettings2D>(), geometry);
        PaintPhysicsDebug(batcher, geometry);
    }
}
