// Output pass: linear HDR canvas -> ACES filmic -> TRUE sRGB encode ->
// display-referred backbuffer. Fullscreen triangle from SV_VertexID -- no
// vertex buffer.
//
// The encode is the IEC 61966-2-1 piecewise curve, NOT pow(1/2.2). That is the
// same transfer nvrhi::Format::SRGBA8_UNORM applies in HARDWARE when a texture
// is sampled (Assets.cpp:176), so the pipeline is symmetric: true sRGB in, true
// sRGB out. It is also what UE uses on output
// (GammaCorrectionCommon.ush LinearToSrgbBranchless).
//
// It previously used pow(1/2.2) to byte-match the RETIRED LOVE client's
// post_process.glsl. That oracle is gone and byte-identity is explicitly not a
// goal, so input and output no longer disagree about the transfer function.
//
// THE OPERATOR LIVES IN tonemap_operator.hlsl (F5 plan 1 Task 3), included
// here and by overlay.hlsl, so the scene and a Linear overlay colour are
// encoded by the same two functions.
//
// CURVE IS MIRRORED in two other places -- keep all three in step:
//   Tests/src/TonemapTest.cpp   (CPU golden reference, same branchless form)
//   Source/Programs/ArcaneEditor/src/EditorWidgets.cpp (LinearToSrgb, branching form)

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput vs_main(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.pos = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}

Texture2D    g_Scene   : register(t0);
SamplerState g_Sampler : register(s0);

// ACESFilmic + LinearToSrgb: the ONE operator, shared with overlay.hlsl.
#include "tonemap_operator.hlsl"

float4 ps_main(VSOutput input) : SV_Target0
{
    float3 linearColor = g_Scene.Sample(g_Sampler, input.uv).rgb;
    float3 display = LinearToSrgb(ACESFilmic(linearColor));
    return float4(display, 1.0);
}
