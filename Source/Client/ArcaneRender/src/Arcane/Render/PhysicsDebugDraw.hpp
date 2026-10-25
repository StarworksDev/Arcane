#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// Render module: physics debug-draw overlay (M6, Task P3.6).
//
// ENGINE-INTERNAL, NOT GAME API: the engine's hosts (editor, runtime) draw
// the physics debug overlay; game code never includes this header.
//
// SOLVER-FREE (3D-readiness Plan A, Task 7): Render names no solver type and
// links no physics module. Extract asks the physics query proxy
// (Scene/PhysicsQuery.hpp, FillPhysicsDebug2D) for a PhysicsDebugGeometry and
// PaintPhysicsDebug submits its primitives to the Batcher2D. The gather
// functions that build the geometry from a solver live in ArcanePhysics2D.
//
// MODERNIZATION over the Lua: dynamic bodies are colored BY ISLAND rather
// than a single kinematic color.  IslandRootOf(i) keys into a small hash-to-
// hue palette so every body in the same dynamic island shares a color; bodies
// in different islands get distinct colors.  Sleeping dynamics are drawn dim
// (x 0.35 multiplier, matching the Lua).  Static/kinematic/sensor bodies keep
// the Lua's type-based tints.
//
// PRESENTATION BOUNDARY: this file lives in Arcane.dll (Render/).  Core never
// includes Render -- the boundary is one-way.  No SDL3 / graphics headers in
// the options struct itself; its settings blocks bring Settings.hpp
// (reflection) with them.

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Render/PhysicsDebugSettings.hpp>   // the debug.physics.* blocks PhysicsDebugDrawOptions inherits
#include <Arcane/Scene/ViewTransform.hpp>   // Affine2D (PhysicsDebugDrawOptions::view)

#include <Arcane/Scene/PhysicsDebugDrawOptions.hpp>
#include <Arcane/Scene/PhysicsQuery.hpp>   // PhysicsDebugGeometry

namespace Arcane
{
    class Batcher2D;

    // Options for the physics debug gather (PhysicsDebugDrawOptions2D, Scene).
    //
    // Settings arc S6-10: the overlay's look, toggles and palette are the
    // debug.physics.* cvars (PhysicsDebugSettings.hpp). The options block
    // inherits the four settings blocks, so a default-constructed block draws
    // with their defaults (the pre-sweep literals) and
    // MakePhysicsDebugDrawOptions() draws with the published values; a caller
    // may still override any inherited member for one call. Inherited:
    //   PhysicsDebugSettings2D    lineThickness (canvas px), contactMarkerSize
    //                             (m, through view.Length), velocityScale (s of
    //                             look-ahead), velocityMinSpeed (m/s; slower
    //                             bodies draw no ray), comMarkerSize (m),
    //                             orientationTickLen (m), manifoldNormalLength,
    //                             manifoldPointPx;
    //   PhysicsDebugDrawSettings2D contacts (centre-to-centre line + midpoint
    //                             disc per touching pool contact), aabbs (each body's tight
    //                             SlotAabb), velocities (awake dynamic bodies),
    //                             comMarkers (dynamic bodies), orientations
    //                             (local +x tick, so circles show rotation);
    //   PhysicsDebugColorSettings2D the palette (per body type, island, overlay
    //                             and NarrowphaseKind);
    //   PhysicsDebugTraceSettings2D GatherNarrowphaseWorldOverlay (Physics2D)'s defaults.
    // NOT inherited: PhysicsDebugStyleSettings2D (debug.physics.style.*: arrow
    // heads, sleeping dim, emphasis floor, thickness scales, px radii); both
    // overlays read its published values directly.
    // A fresh options block holding the PUBLISHED debug.physics.* values (the
    // per-call members keep their defaults). Read it once per frame.
    [[nodiscard]] ARC_RENDER_API PhysicsDebugDrawOptions2D MakePhysicsDebugDrawOptions();

    // Submit physics debug geometry to `batcher`: each Line / Circle primitive
    // in order. The caller brackets batcher.Begin() .. batcher.Drain() around
    // this call; it opens and closes no bracket itself (the batcher's
    // Line / Circle submission methods only).
    //
    // The geometry comes from FillPhysicsDebug2D (Scene/PhysicsQuery.hpp) for
    // the live world, or from the Physics2D gather functions in solver-level
    // tests. What the primitives mean (body outlines colored by type / island,
    // contact lines, AABBs, velocity rays, COM crosses, orientation ticks,
    // manifold overlays, the narrowphase inspector's world overlay) is decided
    // there; this only paints them.
    ARC_RENDER_API void PaintPhysicsDebug(Batcher2D& batcher, const PhysicsDebugGeometry& geometry);

} // namespace Arcane
