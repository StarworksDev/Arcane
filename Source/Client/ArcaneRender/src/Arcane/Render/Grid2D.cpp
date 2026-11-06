// The 2D reference grid. The LOD rule is ViewportGrid.hpp's; this file is
// the arithmetic, shared by extract and the editor's screen-pixel draw.

#include <Arcane/Render/Grid2D.hpp>

#include <Arcane/Core/Constant.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace Arcane
{
    namespace
    {
        // The decade range PlanGrid2D searches. Nothing an editor viewport can
        // show falls outside 1e-9 .. 1e12 m; the bounds just keep the scan
        // finite for an absurd ppm (count < 3 only at those extremes).
        ARC_CONSTANT("math tolerance: the float-precision bounds of the decade LOD")
        constexpr int kMinDecade = -9;
        ARC_CONSTANT("math tolerance: the float-precision bounds of the decade LOD")
        constexpr int kMaxDecade = 12;

        // 10^k as the nearest float, from EXACT integer powers: a positive
        // decade is an exact integer, a negative one is a single correctly
        // rounded division of two exact values. std::pow(10, k) can land a
        // ulp under 0.1 and put "0.1 m at 80 ppm" a hair below fade-in.
        [[nodiscard]] float Decade(int k) noexcept
        {
            float p = 1.0f;
            for (int i = 0; i < std::abs(k); ++i) p *= 10.0f;
            return k < 0 ? 1.0f / p : p;
        }

        // The crossfade: 0 at fade-in, 1 at full, clamped (both in screen px).
        [[nodiscard]] float Ramp(float screenSpacingPx, float fadeInPx, float fadeFullPx) noexcept
        {
            return std::clamp((screenSpacingPx - fadeInPx) / (fadeFullPx - fadeInPx), 0.0f, 1.0f);
        }

        // The inclusive index range [i0, i1] of the multiples of `spacing`
        // inside [lo, hi]. A multiple that sits ON the edge counts (the edge
        // rule Ruling G pins); the epsilon absorbs the inverse-matrix noise
        // that would otherwise put an exact edge at 1.9999998 and drop it.
        struct IndexRange
        {
            std::int64_t first = 0, last = -1;   // empty when last < first
        };

        [[nodiscard]] IndexRange MultiplesInside(float lo, float hi, float spacing) noexcept
        {
            const float eps = spacing * 1e-4f;
            IndexRange r;
            r.first = static_cast<std::int64_t>(std::ceil((lo - eps) / spacing));
            r.last  = static_cast<std::int64_t>(std::floor((hi + eps) / spacing));
            return r;
        }

        [[nodiscard]] bool Finite2(glm::vec3 p) noexcept
        {
            return std::isfinite(p.x) && std::isfinite(p.y);
        }
    }

    float GridPixelsPerMetre(const ViewTransform& view) noexcept
    {
        const std::optional<Affine2D> a = view.AsAffine2D();
        if (!a) return 0.0f;
        const float ppm = std::abs(a->scale.x);
        return std::isfinite(ppm) ? ppm : 0.0f;
    }

    Grid2DPlan PlanGrid2D(float pixelsPerMetre, float minorAlpha, float majorAlpha,
                          float fadeInPx, float fadeFullPx) noexcept
    {
        Grid2DPlan plan;
        if (!(pixelsPerMetre > 0.0f) || !std::isfinite(pixelsPerMetre))
            return plan;

        // The window is at least 1 px wide, so the ramp never divides by zero.
        const float fullPx = std::max(fadeFullPx, fadeInPx + 1.0f);

        int finest = kMaxDecade + 1;
        for (int k = kMinDecade; k <= kMaxDecade; ++k)
        {
            const float spacing = Decade(k);
            if (spacing * pixelsPerMetre >= fadeInPx)
            {
                finest = k;
                break;
            }
        }
        if (finest > kMaxDecade)
            return plan;

        // The finest level's ramp is the crossfade: the finest fades in with
        // it, the decade above (the major, every ten minors) is promoted from
        // minorAlpha to majorAlpha with the same t, and the decade above that
        // stays at majorAlpha.
        const float tFinest = Ramp(Decade(finest) * pixelsPerMetre, fadeInPx, fullPx);

        for (int i = 0; i < 3 && finest + i <= kMaxDecade; ++i)
        {
            GridLevel& level    = plan.levels[static_cast<std::size_t>(i)];
            level.spacingMetres = Decade(finest + i);
            switch (i)
            {
                case 0:  level.alpha = tFinest * minorAlpha; break;
                case 1:  level.alpha = minorAlpha + tFinest * (majorAlpha - minorAlpha); break;
                default: level.alpha = majorAlpha; break;
            }
            plan.count = i + 1;
        }
        return plan;
    }

    void EmitGrid2D(const ViewTransform& view, const Grid2DPlan& plan,
                    const Grid2DStyle& style, OverlayPrimitiveList& out)
    {
        if (!view.IsOrthographic()) return;
        if (view.viewport.x == 0u || view.viewport.y == 0u) return;
        if (plan.count <= 0) return;

        const float ppm = GridPixelsPerMetre(view);
        if (!(ppm > 0.0f)) return;
        // World thickness: the overlay shader expands metres, and
        // lineThicknessPx / ppm metres is lineThicknessPx screen pixels.
        const float thickness = style.lineThicknessPx / ppm;
        if (!(thickness > 0.0f) || !std::isfinite(thickness)) return;

        const float W = static_cast<float>(view.viewport.x);
        const float H = static_cast<float>(view.viewport.y);
        const glm::vec2 corners[4] = { { 0.0f, 0.0f }, { W, 0.0f }, { 0.0f, H }, { W, H } };
        float minX = std::numeric_limits<float>::infinity(), maxX = -minX;
        float minY = minX, maxY = maxX;
        for (const glm::vec2 c : corners)
        {
            const glm::vec3 o = view.ScreenToRay(c).origin;
            if (!Finite2(o)) return;
            minX = std::min(minX, o.x); maxX = std::max(maxX, o.x);
            minY = std::min(minY, o.y); maxY = std::max(maxY, o.y);
        }
        if (!(maxX > minX) || !(maxY > minY)) return;

        const auto emit = [&](glm::vec2 a, glm::vec2 b, glm::vec4 colour)
        {
            OverlayLine line{};
            line.a = glm::vec3(a, 0.0f);
            line.b = glm::vec3(b, 0.0f);
            line.thickness = thickness;
            line.color = colour;
            line.depth = OverlayDepthClass::DepthTested;
            line.space = OverlayColorSpace::Linear;
            line.screenSpace = false;
            out.AddLine(line);
        };

        const int count = std::min(plan.count, static_cast<int>(plan.levels.size()));
        for (int li = 0; li < count; ++li)
        {
            const GridLevel& level = plan.levels[static_cast<std::size_t>(li)];
            const float spacing    = level.spacingMetres;
            if (!(spacing > 0.0f) || !std::isfinite(spacing)) continue;
            if (!(level.alpha > 0.0f)) continue;

            const bool hasCoarser     = li + 1 < count;
            const glm::vec4 colour    = glm::vec4(style.lineRgb, level.alpha);
            const glm::vec4 heldMajor = glm::vec4(style.lineRgb, std::max(level.alpha, style.majorAlpha));

            const IndexRange xs = MultiplesInside(minX, maxX, spacing);
            const IndexRange ys = MultiplesInside(minY, maxY, spacing);
            if (xs.last - xs.first > style.maxLinesPerAxis || ys.last - ys.first > style.maxLinesPerAxis) continue;

            for (std::int64_t i = xs.first; i <= xs.last; ++i)
            {
                if (i == 0) continue;
                const bool tenth = i % 10 == 0;
                if (hasCoarser && tenth) continue;
                const float x = static_cast<float>(i) * spacing;
                emit({ x, minY }, { x, maxY }, tenth ? heldMajor : colour);
            }
            for (std::int64_t j = ys.first; j <= ys.last; ++j)
            {
                if (j == 0) continue;
                const bool tenth = j % 10 == 0;
                if (hasCoarser && tenth) continue;
                const float y = static_cast<float>(j) * spacing;
                emit({ minX, y }, { maxX, y }, tenth ? heldMajor : colour);
            }
        }

        const float axisEps = 1e-6f * std::max(maxX - minX, maxY - minY);
        if (minY - axisEps <= 0.0f && 0.0f <= maxY + axisEps)
            emit({ minX, 0.0f }, { maxX, 0.0f }, style.axisX);
        if (minX - axisEps <= 0.0f && 0.0f <= maxX + axisEps)
            emit({ 0.0f, minY }, { 0.0f, maxY }, style.axisY);
    }
}
