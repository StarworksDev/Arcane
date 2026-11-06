#pragma once

// GridSceneDesc -- THE CPU-SIDE 3D REFERENCE GRID (F4 plan 1 Task 10, spec
// s5.2), header-only so the overlay primitive list (OverlayPrimitive.hpp)
// can carry one without pulling NRI or the node in. Moved here verbatim from
// GridNode.hpp at F5 plan 1 Task 3; GridNode.hpp includes this header.
//
// Read by OverlayNode (overlay.hlsl's grid_vs_main / grid_ps_main) on the
// frame, and by GridNode on its direct-drive path.

#include <Arcane/Core/Constant.hpp>          // ARC_CONSTANT
#include <Arcane/Scene/ViewTransform.hpp>     // the editor camera, whole

#include <glm/glm.hpp>

#include <cstdint>

namespace Arcane
{
    // =====================================================================
    // THE CPU-SIDE GRID, and the whole of what a host has to build. The same
    // borrowing rule MeshSceneDesc carries: FrameDesc::grid points at one of
    // these for the duration of the RenderFrame call; Record reads it then.
    // =====================================================================
    struct GridSceneDesc
    {
        // The editor camera -- view AND projection, whole, because the shader
        // needs the product for the quad and the EYE (inverse(view)[3]) for
        // the wrap, the extent and the fades.
        ViewTransform view;

        // Which world plane the grid lies on. XZ is the 3D ground (+Y up, spec
        // s2); XY is the 2D authoring plane the Ortho2D view looks down -Z at.
        enum class Plane : std::uint8_t { XZ = 0, XY = 1 };
        Plane plane = Plane::XZ;

        // THE TUNABLES ARE SETTINGS (settings S6-21): editor.viewport.grid.*
        // and editor.viewport.grid3D.*, which the editor turns into this desc
        // through Arcane::Editor::MakeGridScene every frame. They are
        // zero-initialised here on purpose -- a desc nobody filled is a bug the
        // node's clamps make visible, not a second copy of the defaults.
        float minorSpacing = 0.0f;     // metres between minor lines
        float majorEvery   = 0.0f;     // metres between major lines (and the wrap cell)
        float fadeDistance = 0.0f;     // metres: alpha reaches 0 here (ours; UE relies on extent)

        // The quad's MINIMUM half-extent (metres), and how it grows with the
        // eye's altitude above the plane: UE scales its grid radii with the
        // camera's height so the grid never ends inside the view when you fly
        // up. The defaults (2000 m, x100) put the far edge ten times the
        // default fade distance away, so it is never visible at ground level.
        float minHalfExtent     = 0.0f;
        float extentPerAltitude = 0.0f;

        // Straight alpha, LINEAR (the canvas is RGBA16F; the tonemap is what
        // makes these display-referred).
        glm::vec4 minorColor{ 0.0f };
        glm::vec4 majorColor{ 0.0f };

        // The two in-plane ORIGIN AXES, drawn in colour over the grid (UE's
        // UAxisColor/VAxisColor in FGridWidget::DrawNewGrid,
        // EditorComponents.cpp): the U axis is world X on both planes; the V
        // axis is world Z on XZ and world Y on XY. Spec s5.2: X red, Z blue,
        // Y green. The defaults are the XZ pair; a caller
        // switching to XY assigns kAxisYColor to axisVColor -- SetPlane()
        // below does exactly that, and is what the editor wiring calls. The
        // shader never branches on the plane for COLOUR (grid.hlsl's header),
        // so an explicit override of either field after SetPlane stands.
        // pending axis unification re-bless (S5-2 A): painted 3D-grid values.
        ARC_CONSTANT("pending axis unification re-bless (S5-2)")
        static constexpr glm::vec4 kAxisXColor{ 0.85f, 0.25f, 0.25f, 0.9f };
        ARC_CONSTANT("pending axis unification re-bless (S5-2)")
        static constexpr glm::vec4 kAxisYColor{ 0.30f, 0.80f, 0.35f, 0.9f };
        ARC_CONSTANT("pending axis unification re-bless (S5-2)")
        static constexpr glm::vec4 kAxisZColor{ 0.30f, 0.40f, 0.90f, 0.9f };
        glm::vec4 axisUColor = kAxisXColor;
        glm::vec4 axisVColor = kAxisZColor;

        // Sets `plane` AND the V axis colour that plane's spec pairing calls
        // for (Z blue for XZ, Y green for XY). The U axis is X red on both.
        void SetPlane(Plane p) noexcept
        {
            plane      = p;
            axisVColor = (p == Plane::XY) ? kAxisYColor : kAxisZColor;
        }
    };
}
