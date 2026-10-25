#pragma once
#include <Arcane/Config/Settings.hpp>
namespace Arcane
{
    struct PhysicsDebugSettings2D
    {
        float lineThickness        = 1.0f;    // canvas px, Line primitives
        float contactMarkerSize    = 0.03f;   // m, contact-midpoint disc radius
        float velocityScale        = 0.15f;   // s of look-ahead for the velocity ray
        float velocityMinSpeed     = 0.05f;   // m/s; the MKS sleep threshold (was velocityRayMinSpeed)
        float comMarkerSize        = 0.05f;   // m, COM cross half-arm
        float orientationTickLen   = 0.18f;   // m
        float manifoldNormalLength = 20.0f;   // "world units": a pre-metres leftover, converted as is
        float manifoldPointPx      = 3.0f;    // canvas px
    };

    struct PhysicsDebugDrawSettings2D
    {
        bool contacts     = true;
        bool aabbs        = false;
        bool velocities   = true;
        bool comMarkers   = true;
        bool orientations = true;
    };

    struct PhysicsDebugColorSettings2D
    {
        CVarColor kinematic     { 0.2f, 1.0f, 0.4f, 1.0f };
        CVarColor staticBody    { 0.4f, 0.7f, 1.0f, 1.0f };      // cvar debug.physics.color.static
        CVarColor sensor        { 1.0f, 0.9f, 0.2f, 0.9f };
        CVarColor contact       { 1.0f, 0.2f, 1.0f, 1.0f };
        CVarColor aabb          { 1.0f, 1.0f, 1.0f, 0.4f };
        CVarColor velocity      { 0.20f, 1.00f, 0.55f, 1.0f };
        CVarColor com           { 1.00f, 1.00f, 1.00f, 1.0f };
        CVarColor orient        { 1.00f, 0.55f, 0.15f, 1.0f };
        CVarColor treeTight     { 0.30f, 0.90f, 1.00f, 0.85f };
        CVarColor treeFat       { 0.30f, 0.90f, 1.00f, 0.25f };
        CVarColor treePair      { 0.20f, 1.00f, 0.90f, 0.80f };
        CVarColor staticGrid    { 0.35f, 0.55f, 1.00f, 0.35f };
        CVarColor residencyGrid { 1.00f, 0.70f, 0.20f, 0.35f };
        CVarColor traceShapeB   { 0.40f, 0.70f, 1.00f, 1.0f };
        CVarColor traceAxis     { 0.55f, 0.55f, 0.60f, 0.6f };
        CVarColor traceAxisHi   { 1.00f, 0.85f, 0.20f, 1.0f };
        CVarColor traceNormal   { 1.00f, 0.25f, 1.00f, 1.0f };
        CVarColor tracePoint    { 1.00f, 1.00f, 1.00f, 1.0f };
        CVarColor subject       { 1.00f, 0.95f, 0.35f, 1.0f };
        // Dynamic bodies are tinted by island: islandRoot % 8 picks one.
        CVarColor island0       { 1.0f, 0.35f, 0.35f, 1.0f };
        CVarColor island1       { 1.0f, 0.65f, 0.15f, 1.0f };
        CVarColor island2       { 1.0f, 1.00f, 0.20f, 1.0f };
        CVarColor island3       { 0.2f, 0.95f, 0.35f, 1.0f };
        CVarColor island4       { 0.2f, 0.80f, 1.00f, 1.0f };
        CVarColor island5       { 0.5f, 0.35f, 1.00f, 1.0f };
        CVarColor island6       { 1.0f, 0.30f, 0.90f, 1.0f };
        CVarColor island7       { 0.85f, 0.85f, 0.85f, 1.0f };
        // Manifold points are tinted by NarrowphaseKind ordinal (0 Separated, 1
        // CircleCircle, 2 CircleVsPolygon, 3 Capsule, 4 SatPolygon, 5 Epa, 6 Mpr);
        // an unknown kind takes narrowphase0.
        CVarColor narrowphase0  { 0.70f, 0.70f, 0.70f, 1.0f };
        CVarColor narrowphase1  { 1.00f, 0.30f, 0.30f, 1.0f };
        CVarColor narrowphase2  { 1.00f, 0.65f, 0.15f, 1.0f };
        CVarColor narrowphase3  { 1.00f, 1.00f, 0.25f, 1.0f };
        CVarColor narrowphase4  { 0.30f, 1.00f, 0.45f, 1.0f };
        CVarColor narrowphase5  { 0.40f, 0.70f, 1.00f, 1.0f };
        CVarColor narrowphase6  { 0.80f, 0.45f, 1.00f, 1.0f };
    };

    // The narrowphase-inspector world overlay (DrawNarrowphaseWorldOverlay).
    struct PhysicsDebugTraceSettings2D
    {
        float traceLineThickness = 1.5f;   // canvas px; cvar debug.physics.trace.lineThickness (the
                                           // member name keeps it apart from the inherited overlay lineThickness)
        float emphasis           = 1.0f;    // alpha scale when the caller passes none (1 = the selected contact)
        float normalLength       = 28.0f;   // "world units": a pre-metres leftover, converted as is
    };

    // Fine styling shared by both overlays (DrawPhysicsDebug and
    // DrawNarrowphaseWorldOverlay). Px values are canvas px.
    struct PhysicsDebugStyleSettings2D
    {
        float arrowShortLen         = 12.0f;  // px; a shorter shaft gets a head of half its length
        float arrowHeadLen          = 6.0f;   // px, arrow head length along the shaft
        float arrowHeadSpread       = 0.6f;   // head half-width as a fraction of its length
        float sleepingDim           = 0.35f;  // RGB scale of sleeping dynamic bodies (Lua dim = 0.35)
        float emphasisFloor         = 0.15f;  // lowest trace emphasis (alpha scale) a dimmed contact keeps
        float subjectThicknessScale = 1.3f;   // inspected shape's outline at full emphasis, x line thickness
        float axisHiThicknessScale  = 1.8f;   // chosen / stepped SAT axis, x line thickness
        float normalThicknessScale  = 1.4f;   // contact normal arrow, x line thickness
        float axisHalfLenPx         = 60.0f;  // px, half-length of each drawn SAT axis segment
        float anchorDiscRadius      = 4.0f;   // px, the contact-anchor ring
        float contactDiscRadius     = 3.0f;   // px, each manifold contact-point disc
    };

}
