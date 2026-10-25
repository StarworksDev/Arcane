#pragma once
#include <Arcane/Scene/PhysicsDebugTypes.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <cstdint>
#include <optional>

// The physics debug overlay's per-call options: the four debug.physics.*
// settings blocks plus the view, interpolation and one-body filter. Solver-free
// (3D-readiness Plan A, Task 7): Render builds one of these without including
// any Physics2D header, and the Physics2D gather functions consume it.
namespace Arcane
{
    struct PhysicsInterpBuffer2D;

    struct PhysicsDebugDrawOptions2D : PhysicsDebugSettings2D, PhysicsDebugDrawSettings2D,
                                       PhysicsDebugColorSettings2D, PhysicsDebugTraceSettings2D
    {
        // Camera transform applied to every emitted point + length: the
        // orthographic ViewTransform's Affine2D (F4 plan 1 T3) -- points go
        // through view.Point (per-axis scale, y NEGATIVE for +Y up on a y-down
        // canvas), lengths through view.Length. Every shape's corners are
        // computed in WORLD space and projected one by one; nothing rotates in
        // screen space by a world angle (a mirrored map would reverse it).
        // Fill from RenderContext2D::view.AsAffine2D(), skipping the overlay
        // when that is nullopt (a perspective view). Default: unit scale, zero
        // offset -- a caller that sets no camera draws at 1 px per metre, y down.
        Affine2D view{};

        // ---- Slice A broadphase + manifold overlays (default OFF) -----------
        //
        // These consume the read-only debug-visualization accessors on
        // PhysicsWorld (FixtureBroadphaseTree / StaticTree / ResidencyGrid /
        // ForEachContactConstraint).  All default off, so behavior is unchanged
        // until a caller (the Sandbox HUD) opts in.

        // Mover-broadphase DynamicTree: each live leaf's tight + fat AABB plus a
        // line between the AABB centers of every broadphase candidate pair. No-op
        // when the world's mover broadphase is not a DynamicTree (null tree).
        bool drawFixtureTree = false;

        // Static-body DynamicTree: a tinted outline of every static leaf's fat
        // box. Flag name retained (renaming ripples to the Sandbox HUD).
        bool drawStaticGrid = false;

        // Dynamic/kinematic residency SpatialGrid: occupied cells in a distinct
        // tint so static vs residency read differently.
        bool drawResidencyGrid = false;

        // Contact manifolds: for each ContactConstraint point, a disc at the
        // world contact point + a normal arrow, colored by NarrowphaseKind. This
        // is ADDITIVE to the legacy center-to-center `contacts` line; both can
        // be on at once.
        bool drawManifolds = false;

        // ---- render interpolation (Epic 04.2) -------------------------------
        // When `interp` is set (per-body previous-step poses from PhysicsSystem2D)
        // each body's outline / COM / orientation / velocity origin is drawn at
        // lerp(prev, current, alpha). Null -> current step pose (unchanged). A
        // per-body generation mismatch (recycled slot) falls back to current.
        // The per-body AABB (aabbs), contacts, and the broadphase overlays
        // (drawFixtureTree / drawStaticGrid / drawResidencyGrid / drawManifolds)
        // are NOT interpolated -- they stay at the current step by spec.
        const PhysicsInterpBuffer2D* interp = nullptr;
        float                      alpha  = 0.0f;   // RunLoop::Alpha() in [0,1)

        // ---- one-body filter (2026-09-11 physics wiring, spec s6.3) ---------
        // When set, ONLY this body's shape outline is drawn -- no contacts,
        // AABBs, velocity rays, COM crosses, orientation ticks or manifolds --
        // so the editor can outline the SELECTED entity's collider in Edit mode
        // without the whole-world overlay (the Unity collider gizmo). Null (the
        // default) is every existing caller: the whole world, every flag honoured.
        // Packed {index, generation}; the solver handle is reconstructed in the .cpp.
        std::optional<std::uint64_t> onlyBody;
    };
}
