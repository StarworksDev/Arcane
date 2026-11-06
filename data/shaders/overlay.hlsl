// THE POST-TONEMAP OVERLAY PASS (F5 plan 1 Task 3, spec s3.3) -- OverlayNode.
//
// Runs AFTER the tonemap, straight-alpha source-over onto the display-referred
// target, with NO depth attachment. Scene depth is SAMPLED at t0 instead, and a
// depth-tested primitive discards itself where the scene is nearer. Two draws
// share this file and one binding layout (b0, t0, s0; space 0):
//
//   vs_main / ps_main           -- expanded lines and fills (OverlayNode::Record
//                                  builds the triangles on the CPU). Per vertex:
//                                  screen-space or world, Linear or
//                                  display-referred, depth-tested or on top.
//   grid_vs_main / grid_ps_main -- the analytic 3D reference grid, folded in from
//                                  grid.hlsl (whose own entry points stay for
//                                  GridNode's direct-drive path). Six vertices,
//                                  no vertex buffer; depth-tested when its
//                                  camera is the mesh camera; its
//                                  LINEAR colour runs the shared operator.
//
// THE COLOUR: a Linear primitive runs exactly the scene's operator
// (tonemap_operator.hlsl) and nothing else -- no scene scale, no post chain.
// A display-referred one is written as given.
//
// THE DEPTH COMPARE: SV_Position.z in the pixel shader is ALREADY the
// post-divide depth (SV_Position.w is 1/clip.w), so it is compared as is.

#include "tonemap_operator.hlsl"

struct VSIn
{
    float3 pos         : POSITION;
    float4 color       : COLOR0;
    float  screenSpace : TEXCOORD0;   // 1 = pixels, y down; 0 = world metres
    float  linearSpace : TEXCOORD1;   // 1 = run the operator; 0 = display-referred
    float  depthTest   : TEXCOORD2;   // 1 = sample-and-discard; 0 = always on top
};

struct VSOut
{
    float4 clip        : SV_Position;
    float4 color       : COLOR0;
    float  linearSpace : TEXCOORD0;
    float  depthTest   : TEXCOORD1;
};

cbuffer OverlayCB : register(b0)
{
    float4x4 g_ViewProjection;       // world-space primitives (the world camera)
    float2   g_Viewport;             // pixels
    float    g_DiscardIfGreater;     // 1 = ForwardZ (discard when frag > scene)
    float    g_HasDepth;             // 1 = sample g_Depth; 0 = draw unoccluded
    // The 3D grid's block: grid.hlsl's GridFrameCB plus a flag. Meaningful only
    // on a frame that draws the grid.
    float4x4 g_GridViewProjection;   // the grid's own camera
    float4   g_GridFlags;            // x: 1 = the grid samples depth (its camera IS the mesh camera)
    float4   g_GridEyeAndPlane;      // xyz: the eye in world space; w: plane id (0 = XZ, 1 = XY)
    float4   g_GridParams;           // x: minor spacing (m), y: major every (m), z: fade (m), w: half-extent (m)
    float4   g_GridMinorColor;       // straight alpha, linear
    float4   g_GridMajorColor;
    float4   g_GridAxisUColor;       // the world-X origin axis
    float4   g_GridAxisVColor;       // the world-Z (XZ) or world-Y (XY) origin axis
};

Texture2D<float> g_Depth : register(t0);
SamplerState     g_Point : register(s0);

VSOut vs_main(VSIn input)
{
    VSOut o;
    if (input.screenSpace > 0.5)
    {
        float2 ndc;
        ndc.x = input.pos.x * (2.0 / g_Viewport.x) - 1.0;
        ndc.y = 1.0 - input.pos.y * (2.0 / g_Viewport.y);
        o.clip = float4(ndc, input.pos.z, 1.0);
    }
    else
    {
        o.clip = mul(g_ViewProjection, float4(input.pos, 1.0));
    }
    o.color = input.color;
    o.linearSpace = input.linearSpace;
    o.depthTest = input.depthTest;
    return o;
}

float4 ps_main(VSOut input) : SV_Target0
{
    if (g_HasDepth > 0.5 && input.depthTest > 0.5)
    {
        float2 uv = input.clip.xy / g_Viewport;
        float sceneD = g_Depth.Sample(g_Point, uv);
        // SV_Position.z is the post-divide depth. SV_Position.w is 1/clip.w.
        float fragD = input.clip.z;
        bool farther = (g_DiscardIfGreater > 0.5) ? (fragD > sceneD) : (fragD < sceneD);
        if (farther) discard;
    }
    float3 rgb = input.color.rgb;
    if (input.linearSpace > 0.5)
        rgb = LinearToSrgb(ACESFilmic(rgb));
    return float4(rgb, input.color.a);
}

// ---------------------------------------------------------------------------
// THE 3D REFERENCE GRID -- grid.hlsl's vs_main / ps_main, copied. The quad is
// centred under the eye with its origin wrapped to a major-cell multiple and
// its half-extent scaled with altitude; the lines come from fwidth, so they
// stay ~1 px wide at every distance. See grid.hlsl for the full account.
// ---------------------------------------------------------------------------

struct GridVSOut
{
    float4 clip      : SV_Position;
    float3 world     : TEXCOORD0;   // world-space position on the plane
    float  depthTest : TEXCOORD1;   // 1 when the grid's camera is the mesh camera
};

float3 GridPlaneToWorld(float2 uv, float planeId)
{
    return planeId < 0.5 ? float3(uv.x, 0.0, uv.y) : float3(uv.x, uv.y, 0.0);
}

float2 GridWorldToPlane(float3 world, float planeId)
{
    return planeId < 0.5 ? world.xz : world.xy;
}

float3 GridPlaneNormal(float planeId)
{
    return planeId < 0.5 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
}

float GridLineCoverage(float2 coord, float2 dcoord)
{
    const float2 g = abs(frac(coord - 0.5) - 0.5) / dcoord;
    return 1.0 - min(min(g.x, g.y), 1.0);
}

GridVSOut grid_vs_main(uint vertexId : SV_VertexID)
{
    const float2 corners[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2( 1.0,  1.0),
        float2(-1.0, -1.0), float2( 1.0,  1.0), float2(-1.0,  1.0)
    };
    const float  planeId    = g_GridEyeAndPlane.w;
    const float  majorEvery = g_GridParams.y;
    const float  halfExtent = g_GridParams.w;

    const float2 eyeUV  = GridWorldToPlane(g_GridEyeAndPlane.xyz, planeId);
    const float2 centre = floor(eyeUV / majorEvery) * majorEvery;

    const float2 uv = centre + corners[vertexId] * halfExtent;

    GridVSOut o;
    o.world     = GridPlaneToWorld(uv, planeId);
    o.clip      = mul(g_GridViewProjection, float4(o.world, 1.0));
    o.depthTest = g_GridFlags.x;
    return o;
}

float4 grid_ps_main(GridVSOut input) : SV_Target0
{
    if (g_HasDepth > 0.5 && input.depthTest > 0.5)
    {
        float2 uv = input.clip.xy / g_Viewport;
        float sceneD = g_Depth.Sample(g_Point, uv);
        // SV_Position.z is the post-divide depth. SV_Position.w is 1/clip.w.
        float fragD = input.clip.z;
        bool farther = (g_DiscardIfGreater > 0.5) ? (fragD > sceneD) : (fragD < sceneD);
        if (farther) discard;
    }

    const float  planeId      = g_GridEyeAndPlane.w;
    const float  minorSpacing = g_GridParams.x;
    const float  majorEvery   = g_GridParams.y;
    const float  fadeDistance = g_GridParams.z;

    const float2 planePos = GridWorldToPlane(input.world, planeId);

    const float2 dPlane    = max(fwidth(planePos), 1e-6);
    float        minorLine = GridLineCoverage(planePos / minorSpacing, dPlane / minorSpacing);
    float        majorLine = GridLineCoverage(planePos / majorEvery,   dPlane / majorEvery);

    const float  kFadeCellPx = 8.0;
    const float2 minorCellPx = minorSpacing / dPlane;
    const float2 majorCellPx = majorEvery   / dPlane;
    minorLine *= saturate(min(minorCellPx.x, minorCellPx.y) / kFadeCellPx * 2.0 - 1.0);
    majorLine *= saturate(min(majorCellPx.x, majorCellPx.y) / kFadeCellPx * 2.0 - 1.0);

    const float2 axisPx = abs(planePos) / dPlane;
    const float  vAxis  = 1.0 - saturate(axisPx.x - 0.5);   // |u| within ~1.5 px of 0
    const float  uAxis  = 1.0 - saturate(axisPx.y - 0.5);   // |v| within ~1.5 px of 0

    const float3 toEye   = g_GridEyeAndPlane.xyz - input.world;
    const float  dist    = length(toEye);
    const float  fade    = saturate(1.0 - dist / fadeDistance);
    const float  grazing = saturate(abs(dot(toEye / max(dist, 1e-4), GridPlaneNormal(planeId))) * 4.0);

    float4 color = lerp(g_GridMinorColor, g_GridMajorColor, majorLine);
    color.a     *= max(minorLine, majorLine);

    float4 axisColor = g_GridAxisUColor;
    axisColor.a     *= uAxis;
    float4 vAxisColor = g_GridAxisVColor;
    vAxisColor.a     *= vAxis;
    axisColor = lerp(axisColor, vAxisColor, vAxisColor.a > axisColor.a ? 1.0 : 0.0);

    // The grid's LINEAR colour (grid.hlsl's return value), then the operator.
    float4 gridLinear = axisColor.a > color.a ? axisColor : color;
    gridLinear.a *= fade * grazing;
    return float4(LinearToSrgb(ACESFilmic(gridLinear.rgb)), gridLinear.a);
}
