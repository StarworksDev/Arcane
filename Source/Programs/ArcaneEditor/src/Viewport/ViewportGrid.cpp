// The editor viewport's 2D reference grid (F4 plan 1 T9, spec s5.1).
// The LOD and the world lines live in Arcane::PlanGrid2D / EmitGrid2D.
// DrawGrid2D projects those lines back to screen pixels.

#include "Viewport/ViewportGrid.hpp"

#include "Settings/EditorGridSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Render/Batcher2D.hpp>

#include <cstdint>
#include <cmath>

namespace Arcane::Editor
{
    namespace
    {
        [[nodiscard]] bool Finite2(glm::vec3 p) noexcept
        {
            return std::isfinite(p.x) && std::isfinite(p.y);
        }
    }

    float PixelsPerMetre(const Arcane::ViewTransform& view) noexcept
    {
        return Arcane::GridPixelsPerMetre(view);
    }

    Grid2DPlan PlanGrid2D(float pixelsPerMetre) noexcept
    {
        const EditorGridSettings& s = Arcane::Settings<EditorGridSettings>();
        return Arcane::PlanGrid2D(pixelsPerMetre, s.minorAlpha, s.majorAlpha, s.fadeInPx, s.fadeFullPx);
    }

    void DrawGrid2D(Arcane::Batcher2D& b, const Arcane::ViewTransform& view, const Grid2DPlan& plan, const Grid2DAxisColors& axes)
    {
        const EditorGridSettings& s = Arcane::Settings<EditorGridSettings>();
        Arcane::Grid2DStyle style;
        style.minorAlpha = s.minorAlpha;
        style.majorAlpha = s.majorAlpha;
        style.lineRgb = glm::vec3(s.lineColor.r, s.lineColor.g, s.lineColor.b);
        style.lineThicknessPx = s.lineThickness;
        style.maxLinesPerAxis = static_cast<std::int64_t>(s.maxLinesPerAxis);
        style.axisX = axes.x;
        style.axisY = axes.y;

        Arcane::OverlayPrimitiveList prims;
        Arcane::EmitGrid2D(view, plan, style, prims);
        if (prims.prims.empty()) return;

        const float ppm = PixelsPerMetre(view);
        b.SetLayer(0, 0);
        for (const Arcane::OverlayPrimitive& prim : prims.prims)
        {
            if (prim.tag != Arcane::OverlayPrimitive::Tag::Line)
                continue;
            const Arcane::OverlayLine& line = prim.line;
            const glm::vec3 sa = view.WorldToScreen(line.a);
            const glm::vec3 sb = view.WorldToScreen(line.b);
            if (!Finite2(sa) || !Finite2(sb)) continue;
            b.Line(glm::vec2(sa), glm::vec2(sb), line.thickness * ppm, line.color);
        }
    }
}
