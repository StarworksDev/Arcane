#pragma once

// OverlayPrimitive -- what the post-tonemap overlay pass draws (F5 plan 1
// Task 3, spec s3.3). Header-only plain data: no NRI, no Astra.
//
// The overlay pass (Nri/nodes/OverlayNode.hpp) runs AFTER the tonemap, on the
// display-referred target, and samples the mesh pass's scene depth instead of
// attaching it. Every primitive states three things about itself:
//
//   * its DEPTH CLASS -- DepthTested primitives are discarded where the scene
//     depth is nearer; AlwaysOnTop primitives never are;
//   * its COLOUR SPACE -- Linear colours run the tonemap operator in the
//     overlay shader (the same ACES + sRGB encode the scene got, with no
//     exposure term); DisplayReferred colours are written as given;
//   * its POSITION SPACE -- screenSpace primitives are canvas pixels (y down,
//     origin top-left) with z the clip depth; world primitives are metres
//     projected through the view the frame carries.
//
// Lines are expanded into quads of `thickness` (pixels in screen space,
// metres in world space) by OverlayNode::Record.

#include <Arcane/Render/GridSceneDesc.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Arcane
{
    enum class OverlayDepthClass : std::uint8_t { DepthTested, AlwaysOnTop };
    enum class OverlayColorSpace : std::uint8_t { Linear, DisplayReferred };
    enum class OverlayPrimKind : std::uint8_t { Line, Triangle, Rect, Circle };

    struct OverlayLine
    {
        glm::vec3 a;
        glm::vec3 b;
        glm::vec4 color;
        float thickness;
        OverlayDepthClass depth;
        OverlayColorSpace space;
        bool screenSpace;
    };

    // Triangle: a, b, c. Rect: min corner a.xy, extent `size`, at a.z.
    // Circle: centre a.xy, `radius`, at a.z. Line is not used as a fill kind.
    struct OverlayFill
    {
        OverlayPrimKind kind;
        glm::vec3 a, b, c;
        glm::vec2 size;
        float radius;
        glm::vec4 color;
        OverlayDepthClass depth;
        OverlayColorSpace space;
        bool screenSpace;
    };

    // One overlay primitive: a line OR a fill, tagged. `line` is meaningful
    // when tag == Line, `fill` when tag == Fill.
    struct OverlayPrimitive
    {
        enum class Tag : std::uint8_t { Line, Fill };
        Tag         tag = Tag::Line;
        OverlayLine line{};
        OverlayFill fill{};
    };

    // One frame's overlay. BORROWED by FrameDesc::overlay for the duration of
    // the RenderFrame call. `hasGrid3d` adds the analytic 3D grid described by
    // `grid3d` as the pass's first draw.
    //
    // ONE LIST, IN SUBMISSION ORDER (F5 plan 1 Task 3, review ruling I1):
    // lines and fills interleave exactly as the producers submitted them, and
    // the pass keeps that order within each depth class -- so a gizmo's plane
    // fill stays UNDER its outline bars, and a primitive appended later paints
    // over one appended earlier, whatever their kinds.
    struct OverlayPrimitiveList
    {
        std::vector<OverlayPrimitive> prims;
        bool hasGrid3d = false;
        GridSceneDesc grid3d{};

        void AddLine(const OverlayLine& line)
        {
            OverlayPrimitive p{};
            p.tag  = OverlayPrimitive::Tag::Line;
            p.line = line;
            prims.push_back(p);
        }
        void AddFill(const OverlayFill& fill)
        {
            OverlayPrimitive p{};
            p.tag  = OverlayPrimitive::Tag::Fill;
            p.fill = fill;
            prims.push_back(p);
        }
        // Appends `other`'s primitives after this list's, keeping both orders.
        void Append(const OverlayPrimitiveList& other)
        {
            prims.insert(prims.end(), other.prims.begin(), other.prims.end());
        }
        // Drops every primitive and the 3D grid.
        void Clear()
        {
            prims.clear();
            hasGrid3d = false;
        }
    };
}
