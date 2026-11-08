// mesh.hlsl -- the OPAQUE 3D pass, reading its per-instance data from the GPU
// SCENE (F3 plan 1 T7). One directional light, LAMBERT diffuse plus a constant
// ambient term, one albedo texture through the bindless table or the flat
// baseColor path -- the lighting arithmetic is UNCHANGED from the F2a pass;
// only WHERE the per-instance data comes from moved.
//
// THE ROOT BLOCK is ONE 8-byte {firstOutput, flags} (MeshNode.hpp's
// MeshRootConstants), pushed once per draw:
//   * an INDIRECT batch draw (flags == 0) reads
//         row = g_VisibleIndices[firstOutput + SV_InstanceID]
//     -- firstOutput is the batch's start in this frame slot's visible-index
//     list (GpuBatchDraw::firstOutput), and SV_InstanceID walks the rows the
//     CPU coarse test (plan 2: the cull dispatch) admitted;
//   * a DIRECT draw (flags & kMeshRootDirect) reads row = firstOutput ITSELF
//     -- the ad-hoc scratch rows (MeshSceneDesc::instances), and plan 2's
//     transparent rows drawn back-to-front one at a time.
// The 128-byte per-instance MeshConstants block this replaces is GONE: the
// model matrix, the tint, the normal matrix and the material slot are all
// fields of the row (gpu_scene.hlsli's GpuInstance).
//
// DELIBERATELY NOT PBR. The GGX/metallic-roughness material model belongs to
// the Deadlock-class renderer arc, and a half-version here would be a second
// material model to reconcile with it later. Lambert + ambient is the whole
// lighting model in this file, and that is a decision, not a placeholder.
//
// Colors are LINEAR and may exceed 1.0 -- the canvas is RGBA16F and the tonemap
// node is what turns them display-referred (same contract as sprite.hlsl).
//
// MATRIX PACKING. Every float4x4 here is COLUMN-MAJOR, which is dxc's default
// for both the DXIL and SPIR-V targets and is exactly glm::mat4's memory layout
// (m[c] is column c), so the C++ side stages a glm::mat4 in with no transpose.
// mul(M, v) is therefore the ordinary M * v.
//
// UNITS are METERS (MKS) -- MeshBuilder emits meters and the camera's nearZ/
// farZ are meters.
//
// REGISTER MAP (MeshNode.cpp's CreateBindings, THE REGISTER-SPACE RULE):
//   root (implicit space0): b0 the root block, s0 the immutable sampler;
//   space1 = { b1 MeshFrameCB, t0 g_Instances, t1 g_VisibleIndices };
//   space2 = { t0 g_BindlessTextures[kMeshBindlessCapacity] }.
// The SPIR-V register-shift table (compile-shaders.bat's SPIRV_FLAGS /
// ShaderConventions.hpp::kSpirvArgs) carries a `-fvk-t-shift 0 1` entry for
// the two space1 SRVs: t0/t1 land at set 1 bindings 0/1, b1 at binding 257.
#include "gpu_scene.hlsli"

struct MeshRoot
{
    uint firstOutput;
    uint flags;
};
#define kMeshRootDirect 1u

#if SPIRV
[[vk::push_constant]] ConstantBuffer<MeshRoot> g_PC;
#define g_firstOutput g_PC.firstOutput
#define g_flags       g_PC.flags
#else
cbuffer MeshRootCB : register(b0)
{
    MeshRoot g_PCData;
}
#define g_firstOutput g_PCData.firstOutput
#define g_flags       g_PCData.flags
#endif

// PER-FRAME, and an ordinary descriptor-set constant buffer rather than more
// root constants: the view-projection alone is 64 bytes and root/push-constant
// budget is the scarcest thing in a pipeline layout. b1 in space1 (Task 8/10
// moved it there from the implicit space0 -- see MeshNode.cpp's
// CreateBindings, THE REGISTER-SPACE RULE: adding a root SAMPLER for the
// bindless array below meant root items and every ordinary descriptor set
// had to become distinct spaces, where before only root CONSTANTS shared
// space0 with the one set that existed).
cbuffer MeshFrameCB : register(b1, space1)
{
    float4x4 g_viewProjection;
    // xyz: a UNIT vector pointing TOWARD the light, ALREADY NORMALIZED by
    // MeshNode::Record -- do NOT normalize it again here. A zero vector is the
    // legal "no directional light" value, and normalize() on one is a division
    // by zero whose NaN would propagate through N.L into every lit pixel;
    // dotting against the zero vector instead gives 0, i.e. ambient only. The
    // CPU is the only place that case can be checked, and normalizing there
    // costs one normalize per frame rather than one per pixel here.
    float4   g_lightDirection;
    float4   g_lightColor;       // rgb: linear radiance
    float4   g_ambient;          // rgb: the constant ambient term
};

// THE GPU SCENE (F3): the ONE persistent instance buffer (every registry row
// plus each frame slot's scratch rows) and THIS frame slot's visible-index
// list, both written by GpuSceneSyncNode ahead of this pass. Same set as b1
// (space1); MeshNode::Record rewrites their descriptors for a slot when the
// scene's buffers change (growth, or a slot's visible buffer re-created).
StructuredBuffer<GpuInstance> g_Instances      : register(t0, space1);
StructuredBuffer<uint>        g_VisibleIndices : register(t1, space1);

struct VSInput
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VSOutput
{
    float4 pos    : SV_Position;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    // Per-ROW now (they were root constants): carried flat to the pixel
    // shader, uniform across one instance's triangles.
    nointerpolation float4 baseColor : COLOR0;
    nointerpolation uint   slot      : TEXCOORD1;
    // Task 3/F3: the resolved material cutoff travels in boundsMax.w. It is
    // row state, not another root constant, preserving MeshRoot's 8-byte ABI.
    nointerpolation float   alphaCutoff : TEXCOORD2;
};

VSOutput vs_main(VSInput input, uint instanceId : SV_InstanceID)
{
    const uint row = (g_flags & kMeshRootDirect) ? g_firstOutput
                                                 : g_VisibleIndices[g_firstOutput + instanceId];
    const GpuInstance inst = g_Instances[row];

    VSOutput output;
    const float4 world = mul(inst.model, float4(input.pos, 1.0));
    output.pos = mul(g_viewProjection, world);
    // THE INVERSE TRANSPOSE (Task 8/F2a), not the upper 3x3: a surface
    // tangent scales WITH the object, so for dot(normal, tangent) to stay
    // zero after a NON-UNIFORM scale the normal has to scale by the INVERSE
    // along each axis, not the same factor `model` applies to a position.
    // Arcane::NormalMatrixFor (Math/NormalMatrix.hpp) computes this once per
    // row on the CPU at staging (glm::transpose(glm::inverse(upper 3x3)),
    // with a singular-model guard that returns identity rather than feeding
    // this shader a NaN) and the row carries its three columns -- consumed as
    // an explicit weighted sum of those columns, NOT float3x3(col0,col1,col2),
    // because HLSL's matrix-from-vectors constructor fills ROWS from its
    // arguments, which would silently transpose this: mul(M,v) == v.x*col0 +
    // v.y*col1 + v.z*col2 is the definition of a column-major matrix-vector
    // product, and stating it this way is correct on BOTH the SPIR-V and DXIL
    // targets with no dependence on either compiler's matrix-packing defaults.
    // The SAME product the F2a pass computed from its root-constant columns.
    output.normal = input.normal.x * inst.normal0.xyz
                  + input.normal.y * inst.normal1.xyz
                  + input.normal.z * inst.normal2.xyz;
    output.uv        = input.uv;
    output.baseColor = inst.baseColor;
    output.slot      = inst.materialSlot;
    output.alphaCutoff = inst.boundsMax.w;
    return output;
}

// THE BINDLESS MATERIAL TABLE (Task 8/10). A FIXED-size array -- not an
// unbounded/runtime one -- because BindlessTable's capacity is decided once
// at MeshNode creation and never resized (BindlessTable.hpp's SLOT POLICY).
// kMeshBindlessCapacity comes from build/engine-constants.lua: the generated
// bindless-capacity.hlsli (and Arcane/Render/Generated/BindlessCapacity.gen.hpp,
// which MeshNode.hpp includes) are checked in, and a drift test fails if a
// rerun of scripts/generate-engine-constants would change them.
// t0 in space2, matching MeshNode.cpp's bindlessSetDesc (THE REGISTER-SPACE
// RULE, this file's MeshFrameCB comment above).
#include "bindless-capacity.hlsli"
Texture2D<float4> g_BindlessTextures[kMeshBindlessCapacity] : register(t0, space2);

// THE ONE IMMUTABLE SAMPLER (Task 10's slice-one sampler strategy): a ROOT/
// static sampler baked into the pipeline layout (MeshNode.cpp's
// CreateBindings, RootSamplerDesc) rather than a descriptor-set entry -- s0
// stays in the implicit space0, where this shader's OTHER root item (b0)
// already is, which is exactly what THE REGISTER-SPACE RULE requires once a
// layout carries a root sampler.
SamplerState g_Sampler : register(s0);

// BindlessTable::kInvalidSlot (BindlessTable.hpp) == kGpuInvalidMaterialSlot
// (GpuSceneTypes.hpp), restated here because HLSL cannot include a C++
// header. Keep numerically identical.
#define kMeshInvalidMaterialSlot 0xFFFFFFFFu

float4 SampledAlbedo(VSOutput input)
{
    return input.slot == kMeshInvalidMaterialSlot
        ? float4(1.0, 1.0, 1.0, 1.0)
        : g_BindlessTextures[NonUniformResourceIndex(input.slot)].Sample(g_Sampler, input.uv);
}

float4 LitAlbedo(VSOutput input)
{
    const float3 n      = normalize(input.normal);
    // NOT normalized here -- see g_lightDirection's comment. The zero vector is
    // the "no directional light" case and dot() handles it; normalize() would
    // not.
    const float  ndotl  = saturate(dot(n, g_lightDirection.xyz));

    // kMeshInvalidMaterialSlot selects F2a's ORIGINAL flat path BIT-FOR-
    // BIT: the old code sampled a 1x1 opaque-white texel and multiplied it
    // by the tint, and 1.0 * x is exact under IEEE-754, so using baseColor
    // directly is that same result with the now-redundant sample removed,
    // not an approximation of it. Any other slot must have come from THIS
    // pipeline's own MeshNode::AddMaterial. The index is wrapped in
    // NonUniformResourceIndex: it is uniform across one instance's
    // invocations but a SHADER-COMPUTED index into the array rather than a
    // compile-time constant, which is the standard bindless-indexing idiom
    // on both backends -- and, now that the slot is a per-row value read by
    // SV_InstanceID, genuinely non-uniform across one indirect draw.
    const float4 albedo = SampledAlbedo(input) * input.baseColor;

    const float3 lit    = albedo.rgb * (g_ambient.rgb + g_lightColor.rgb * ndotl);
    return float4(lit, albedo.a);
}

// Opaque stays byte-for-byte on the pre-F3 lighting path: it is its own
// offline artifact, with no runtime define selecting a material mode.
float4 ps_main(VSOutput input) : SV_Target0
{
    return LitAlbedo(input);
}

// Alpha-tested mesh artifact. The sampled albedo's alpha combines with the
// instance tint's alpha; boundsMax.w is GpuSceneSync's resolved cutoff.
float4 ps_masked_main(VSOutput input) : SV_Target0
{
    const float4 sampledAlbedo = SampledAlbedo(input);
    const float alpha = input.baseColor.a * sampledAlbedo.a;
    clip(alpha - input.alphaCutoff);

    const float3 n = normalize(input.normal);
    const float ndotl = saturate(dot(n, g_lightDirection.xyz));
    const float3 lit = (input.baseColor.rgb * sampledAlbedo.rgb)
                     * (g_ambient.rgb + g_lightColor.rgb * ndotl);
    return float4(lit, alpha);
}

// Ordered transparent mesh artifact. Straight alpha is preserved for the
// cache-owned AlphaOver blend state to consume; it deliberately writes no
// depth (that pipeline state lives in GraphicsKey, not here).
float4 ps_transparent_main(VSOutput input) : SV_Target0
{
    return LitAlbedo(input);
}
