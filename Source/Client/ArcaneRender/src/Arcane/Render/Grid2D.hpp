#pragma once

// The 2D reference grid (F4 plan 1 T9, spec s5.1; F5 plan 1 Task 9).
// PlanGrid2D chooses the decade levels. EmitGrid2D writes them as
// depth-tested, linear, world-space overlay lines (metres; the overlay
// shader projects them). The editor's DrawGrid2D projects those lines
// back to screen pixels for the batcher.

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Arcane
{
    struct GridLevel
    {
        float spacingMetres = 0.0f;
        float alpha         = 0.0f;   // in [0, 1]; 0 only at the exact fade-in edge
    };

    // Finest first; levels[i + 1] is ten times levels[i]. Only the first
    // `count` entries are meaningful.
    struct Grid2DPlan
    {
        std::array<GridLevel, 3> levels{};
        int count = 0;
    };

    // The line look EmitGrid2D reads. The fade window is PlanGrid2D's.
    struct Grid2DStyle
    {
        float        minorAlpha = 0.35f;
        float        majorAlpha = 0.55f;
        glm::vec3    lineRgb{ 0.5f, 0.5f, 0.5f };
        float        lineThicknessPx = 1.0f;
        std::int64_t maxLinesPerAxis = 4096;
        glm::vec4    axisX{ 0.85f, 0.25f, 0.25f, 0.9f };
        glm::vec4    axisY{ 0.3f, 0.8f, 0.3f, 0.9f };
    };

    // |scale.x| of an orthographic XY view's Affine2D; 0 for a perspective
    // view, a tilted orthographic one, or a view with no viewport.
    // Named apart from Editor::PixelsPerMetre: an identical signature in
    // Arcane is an ADL candidate for every unqualified call on a ViewTransform.
    [[nodiscard]] ARC_RENDER_API float GridPixelsPerMetre(const ViewTransform& view) noexcept;

    // Decade LOD. A non-positive or non-finite ppm yields count 0. The fade
    // window is widened to at least one pixel (fadeFull = max(fadeFull, fadeIn + 1)).
    [[nodiscard]] ARC_RENDER_API Grid2DPlan PlanGrid2D(float pixelsPerMetre, float minorAlpha, float majorAlpha,
                                                       float fadeInPx = 8.0f, float fadeFullPx = 24.0f) noexcept;

    // World-space lines into `out`, in submission order (levels, then the
    // axes). No lines for a non-orthographic view, an empty viewport, an
    // empty plan, or a non-positive pixels-per-metre.
    ARC_RENDER_API void EmitGrid2D(const ViewTransform& view, const Grid2DPlan& plan,
                                   const Grid2DStyle& style, OverlayPrimitiveList& out);
}
