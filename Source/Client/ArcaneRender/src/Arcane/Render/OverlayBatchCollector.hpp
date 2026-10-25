#pragma once

// OverlayBatchCollector -- the bridge from Batcher2D producers to the
// post-tonemap overlay pass (F5 plan 1 Task 3). A Batcher2D whose Line,
// Circle and Rect append OverlayPrimitive entries instead of quads, so code
// that already draws through a Batcher2D (DrawGrid2D, PaintPhysicsDebug, the
// editor's camera rect) keeps its code and emits overlay primitives in the
// pixel space it already works in: screenSpace, z = 0, Linear (the colours
// were authored for the linear canvas), tagged with the collector's depth
// class. Every other entry point records nothing.
//
// Header-only and unexported: its base is the exported Batcher2D, and this
// class adds no virtual to it. Task 9 reuses it inside extract.

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Arcane
{
    class OverlayBatchCollector final : public Batcher2D
    {
    public:
        explicit OverlayBatchCollector(OverlayPrimitiveList& out,
                                       OverlayDepthClass depth = OverlayDepthClass::AlwaysOnTop)
            : m_out(out), m_depth(depth) {}

        void Begin(std::uint32_t, std::uint32_t) override {}
        void SetLayer(std::uint16_t, std::uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Glyph(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        Batch2DStats Stats() const override { return {}; }
        void QuadWorld(std::uint16_t, const Guid&, const std::array<glm::vec3, 4>&,
                       glm::vec2, glm::vec2, glm::vec4) override {}
        void CircleWorld(glm::vec3, glm::vec3, glm::vec3, float, glm::vec4) override {}
        void SetViewProjection(const glm::mat4&) override {}

        void Line(glm::vec2 a, glm::vec2 b, float thickness, glm::vec4 color) override
        {
            OverlayLine line{};
            line.a = glm::vec3(a, 0.0f);
            line.b = glm::vec3(b, 0.0f);
            line.thickness = thickness;
            line.color = color;
            line.depth = m_depth;
            line.space = OverlayColorSpace::Linear;
            line.screenSpace = true;
            m_out.AddLine(line);
        }

        void Circle(glm::vec2 center, float radius, glm::vec4 color) override
        {
            OverlayFill fill{};
            fill.kind = OverlayPrimKind::Circle;
            fill.a = glm::vec3(center, 0.0f);
            fill.radius = radius;
            fill.color = color;
            fill.depth = m_depth;
            fill.space = OverlayColorSpace::Linear;
            fill.screenSpace = true;
            m_out.AddFill(fill);
        }

        void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color, float) override
        {
            OverlayFill fill{};
            fill.kind = OverlayPrimKind::Rect;
            fill.a = glm::vec3(pos, 0.0f);
            fill.size = size;
            fill.color = color;
            fill.depth = m_depth;
            fill.space = OverlayColorSpace::Linear;
            fill.screenSpace = true;
            m_out.AddFill(fill);
        }

    private:
        OverlayPrimitiveList& m_out;
        OverlayDepthClass     m_depth;
    };
}
