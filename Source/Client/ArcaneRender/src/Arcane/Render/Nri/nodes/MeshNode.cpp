// MeshNode -- see the header for what this node owns, what it deliberately is
// NOT, and where it sits in the frame.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>
#include <Extensions/NRIHelper.h>

#include "MeshNode.hpp"

#include <Arcane/Render/DepthConventionMap.hpp>
#include <Arcane/Render/Nri/GpuScene.hpp>                  // the instance / args / visible-index buffers Record draws from (F3 plan 1 T7)
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp>   // AddGpuSceneSyncNode -- declared ahead of the mesh node (F3 plan 1 T6)
#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderBudgetSettings.hpp>   // RenderGpuSceneSettings -- the scratch-row fallback
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/RenderLookSettings.hpp>     // render.textureAnisotropy (RenderSettings)
#include <Arcane/Render/ShaderConventions.hpp>   // kVsEntry / kPsEntry

#undef ERROR

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

namespace Arcane
{
    namespace
    {
        void GraphError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        // The offline artifacts this node loads. Their stems' _vs/_ps suffixes
        // agree with mesh.hlsl's vs_main/ps_main entry points, which is the
        // INVARIANT compile-shaders.bat states at the top of itself.
        constexpr const char* kMeshVs = "mesh_vs";
        constexpr const char* kMeshPs = "mesh_ps";
        constexpr const char* kMeshMaskedPs = "mesh_masked_ps";
        constexpr const char* kMeshTransparentPs = "mesh_transparent_ps";
        constexpr const char* kMeshMaskedPsEntry = "ps_masked_main";
        constexpr const char* kMeshTransparentPsEntry = "ps_transparent_main";

        [[nodiscard]] constexpr std::uint64_t ShaderPairId(MeshPixelShader pixel) noexcept
        {
            // One vertex artifact plus the concrete offline pixel artifact.
            // These ids are stable labels for the actual bytecode identity,
            // not a runtime blend/cull define folded into an opaque value.
            switch (pixel)
            {
                case MeshPixelShader::Opaque:      return 0x50000001ull; // mesh_vs + mesh_ps
                case MeshPixelShader::Masked:      return 0x50000002ull; // mesh_vs + mesh_masked_ps
                case MeshPixelShader::Transparent: return 0x50000003ull; // mesh_vs + mesh_transparent_ps
            }
            return 0x50000001ull;
        }

        [[nodiscard]] constexpr const char* PixelEntry(MeshPixelShader pixel) noexcept
        {
            switch (pixel)
            {
                case MeshPixelShader::Opaque:      return kPsEntry;
                case MeshPixelShader::Masked:      return kMeshMaskedPsEntry;
                case MeshPixelShader::Transparent: return kMeshTransparentPsEntry;
            }
            return kPsEntry;
        }

        // THE ROOT BLOCK is MeshRootConstants (MeshNode.hpp, 8 bytes) since
        // F3 plan 1 T7. The 128-byte per-instance MeshConstants block that
        // lived here -- model + tint + NormalMatrixFor's three columns packed
        // as float4s, with the material slot bit-cast into col0.w because the
        // block sat at EXACTLY Vulkan's guaranteed-minimum push-constant
        // budget with zero headroom -- is gone: every one of those values is
        // now a field of the GPU scene's 240-byte row (GpuInstance), staged
        // by GpuSceneSync (registry rows) or by Prepare below (ad-hoc rows),
        // and read by the vertex shader through t0/space1. The root block
        // carries only WHICH row(s): a visible-index start for an indirect
        // batch, or the row itself for a direct draw. See `git log` for the
        // old struct if the packing account is ever needed again.

        // mesh.hlsl's MeshFrameCB (b1). std140/HLSL cbuffer packing rules put
        // each float4 on its own 16-byte boundary, which is what the three
        // glm::vec4s below are for -- a glm::vec3 member would pack to 12
        // bytes here and misalign everything after it.
        struct MeshFrameConstants
        {
            glm::mat4 viewProjection{1.0f};
            // Zero here: Record writes all three from MeshSceneDesc every
            // frame (render.mesh.defaultLight.* is where the defaults live).
            glm::vec4 lightDirection{0.0f};
            glm::vec4 lightColor{0.0f};
            glm::vec4 ambient{0.0f};
        };
        static_assert(sizeof(MeshFrameConstants) == 112, "must match mesh.hlsl's MeshFrameCB");
        static_assert(sizeof(MeshFrameConstants) <= MeshNode::kFrameCbMaxBytes,
                      "the frame constants must fit one arena region");

        // Every element finite. A NaN or Inf clip position is UNDEFINED on the
        // GPU rather than a wrong picture, and SceneCamera::PerspectiveProjection
        // is an unvalidated pure function (a zero fov, aspectRatio <= 0 or
        // nearZ >= farZ all produce them silently), so the pass checks what it
        // is handed rather than trusting the caller to have used the guarded
        // path. See MeshSceneDesc's camera comment.
        [[nodiscard]] bool IsFinite(const glm::mat4& m) noexcept
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (!std::isfinite(m[c][r]))
                        return false;
            return true;
        }

        // glm::normalize divides by the length WITHOUT checking it, so a
        // zero-length light direction yields NaN in all three components --
        // which then propagates through N.L into every lit pixel, on a value
        // MeshSceneDesc explicitly allows a caller to leave at its default and
        // then zero out. Returning the zero vector instead makes that case mean
        // "no directional light": saturate(dot(n, 0)) is 0, so the surface
        // falls back to the ambient term, which is the only sensible reading.
        //
        // ON THE CPU, ONCE PER FRAME, rather than in the pixel shader: mesh.hlsl
        // consumes this value directly, so the guard cannot be bypassed by a
        // caller and costs one normalize per frame instead of one per pixel.
        // The epsilon is on the SQUARED length, so it is 1e-12 in length terms
        // -- far below any direction anyone would author and far above the
        // denormal range where the division itself misbehaves.
        [[nodiscard]] glm::vec3 SafeNormalize(const glm::vec3& v) noexcept
        {
            const float lengthSquared = glm::dot(v, v);
            if (!std::isfinite(lengthSquared) || lengthSquared < 1e-12f)
                return glm::vec3(0.0f);
            return v / std::sqrt(lengthSquared);
        }
    }

    std::unique_ptr<MeshNode> MeshNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<MeshNode> node(new MeshNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    MeshPipelineState MeshNode::PipelineStateFor(MaterialBlendMode blend, bool twoSided) noexcept
    {
        MeshPipelineState state;
        state.cullMode = twoSided ? nri::CullMode::NONE : nri::CullMode::BACK;
        switch (blend)
        {
            case MaterialBlendMode::Opaque:
                state.pixel      = MeshPixelShader::Opaque;
                state.blend      = NriPipelineCache::GraphicsKey::Blend::Opaque;
                state.depthWrite = true;
                break;
            case MaterialBlendMode::Masked:
                state.pixel      = MeshPixelShader::Masked;
                state.blend      = NriPipelineCache::GraphicsKey::Blend::Opaque;
                state.depthWrite = true;
                break;
            case MaterialBlendMode::Transparent:
                state.pixel      = MeshPixelShader::Transparent;
                state.blend      = NriPipelineCache::GraphicsKey::Blend::AlphaOver;
                state.depthWrite = false;
                break;
        }
        return state;
    }

    std::size_t MeshNode::PipelineIndex(MaterialBlendMode blend, bool twoSided) noexcept
    {
        switch (blend)
        {
            case MaterialBlendMode::Opaque:      return twoSided ? 1u : 0u;
            case MaterialBlendMode::Masked:      return twoSided ? 3u : 2u;
            case MaterialBlendMode::Transparent: return twoSided ? 5u : 4u;
        }
        return 0;
    }

    bool MeshNode::Init(NriGraphContext& context)
    {
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();
        // The ad-hoc cap is the GPU scene's scratch region, latched by
        // GpuScene::Create (built before this node, in InitCommon).
        m_scratchRows = context.Scene() ? context.Scene()->ScratchRows()
                                        : Settings<RenderGpuSceneSettings>().scratchRowsPerFrame;

        // THE GATE (Task 8/10 Step 1), FIRST -- before shader loads, before
        // any NRI object exists. This node's whole material model is a fixed-
        // capacity descriptor-indexed table, so both the bindless tier and
        // the sampled-image descriptor limit must cover it.
        // The house refuse-loudly posture: producing wrong pixels (or
        // silently degrading to some other path) on tier-0 hardware is
        // worse than refusing to build the node at all, so this returns
        // false -- Create() turns that into a logged, latched null -- rather
        // than limping on with an unbuilt or half-built table.
        if (!m_device->Caps().SupportsBindlessTextures(kBindlessCapacity))
        {
            ARC_ERROR("[nri-graph] MeshNode: refused -- this device cannot provide the "
                      "required {} bindless sampled-image descriptors "
                      "(tier={}, setLimit={}, stageLimit={}). "
                      "The mesh pass's "
                      "material table is a descriptor-indexed bindless array; there is no "
                      "correct way to build it on hardware that cannot dynamically index a "
                      "descriptor array, so the node is refused here rather than rendering "
                      "wrong pixels or silently degrading.", kBindlessCapacity,
                      static_cast<unsigned>(m_device->Caps().bindlessTier),
                      m_device->Caps().maxDescriptorSetTextures,
                      m_device->Caps().maxPerStageTextures);
            return false;
        }

        m_bindlessUpdateAfterSet =
            m_device->Caps().SupportsTextureUpdateAfterSet(kBindlessCapacity);
        if (!m_bindlessUpdateAfterSet)
        {
            ARC_WARN("[nri-graph] MeshNode: sampled-image update-after-set is unavailable "
                     "for the {}-descriptor material table; new material residency will "
                     "wait for device idle (setLimit={}, stageLimit={})", kBindlessCapacity,
                     m_device->Caps().maxDescriptorSetUpdateAfterSetTextures,
                     m_device->Caps().maxPerStageUpdateAfterSetTextures);
        }

        m_vs = context.ShaderBytecode(kMeshVs);
        m_ps[static_cast<std::size_t>(MeshPixelShader::Opaque)] = context.ShaderBytecode(kMeshPs);
        m_ps[static_cast<std::size_t>(MeshPixelShader::Masked)] = context.ShaderBytecode(kMeshMaskedPs);
        m_ps[static_cast<std::size_t>(MeshPixelShader::Transparent)] = context.ShaderBytecode(kMeshTransparentPs);
        if (m_vs.empty()
            || m_ps[static_cast<std::size_t>(MeshPixelShader::Opaque)].empty()
            || m_ps[static_cast<std::size_t>(MeshPixelShader::Masked)].empty()
            || m_ps[static_cast<std::size_t>(MeshPixelShader::Transparent)].empty())
        {
            ARC_ERROR("[nri-graph] MeshNode: required shader artifact is missing -- expected '{}', '{}', '{}', and '{}'",
                      kMeshVs, kMeshPs, kMeshMaskedPs, kMeshTransparentPs);
            return false;
        }

        // The vertex input, in members because GraphicsPipelineDesc::vertexInput
        // is a pointer the cache dereferences after `fill` returns. Layout is
        // MeshVertex (MeshBuilder.hpp) and the semantic names are mesh.hlsl's
        // VSInput.
        m_attributes[0].d3d.semanticName = "POSITION";
        m_attributes[0].vk.location      = 0;
        m_attributes[0].offset           = offsetof(MeshVertex, position);
        m_attributes[0].format           = nri::Format::RGB32_SFLOAT;
        m_attributes[1].d3d.semanticName = "NORMAL";
        m_attributes[1].vk.location      = 1;
        m_attributes[1].offset           = offsetof(MeshVertex, normal);
        m_attributes[1].format           = nri::Format::RGB32_SFLOAT;
        m_attributes[2].d3d.semanticName = "TEXCOORD";
        m_attributes[2].vk.location      = 2;
        m_attributes[2].offset           = offsetof(MeshVertex, uv);
        m_attributes[2].format           = nri::Format::RG32_SFLOAT;

        m_stream.bindingSlot = 0;
        m_stream.stepRate    = nri::VertexStreamStepRate::PER_VERTEX;
        m_stream.stride      = (std::uint16_t)sizeof(MeshVertex);

        m_vertexInput.attributes   = m_attributes;
        m_vertexInput.attributeNum = (std::uint8_t)std::size(m_attributes);
        m_vertexInput.streams      = &m_stream;
        m_vertexInput.streamNum    = 1;

        // Reserved once so Prepare()'s per-frame residency table keeps the STEADY
        // STATE free of heap traffic inside the declaration window. It is not an
        // absolute: a frame carrying more than kInitialResidentSlots distinct meshes
        // grows the vector once, on the high-water mark.
        m_residents.reserve(kInitialResidentSlots);

        return CreateBindless() && CreateBindings() && CreateConstantArena() && CreateSets();
    }

    bool MeshNode::CreateBindless()
    {
        // Gated already, in Init(), before this ever runs -- a device that
        // reached here has bindless tier > 0.
        m_bindless = BindlessTable::Create(*m_device, kBindlessCapacity);
        if (!m_bindless)
        {
            ARC_ERROR("[nri-graph] MeshNode: BindlessTable::Create failed -- the material table "
                      "could not be built (already logged: capacity {})", kBindlessCapacity);
            return false;
        }
        return true;
    }

    nri::DescriptorPoolDesc MeshNode::PoolSizes(bool bindlessUpdateAfterSet) noexcept
    {
        // TWO dimensions now (Task 8/10): FramesInFlight() per-frame
        // sets, each carrying exactly ONE CONSTANT_BUFFER descriptor (b1);
        // and ONE bindless set carrying up to kBindlessCapacity TEXTURE
        // descriptors. The root sampler (CreateBindings) consumes NO pool
        // budget at all -- static/immutable samplers are not allocated from
        // a descriptor pool on either backend (NRIDescs.h:1077's own words).
        const std::uint32_t frameSets = FramesInFlight();

        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum      = frameSets + 1;       // +1: the one bindless set
        poolDesc.constantBufferMaxNum     = frameSets;           // b1, one per frame slot
        poolDesc.structuredBufferMaxNum   = 2 * frameSets;       // t0 instances + t1 visible indices, per frame slot (F3 plan 1 T7)
        poolDesc.textureMaxNum            = kBindlessCapacity;   // the bindless array's own budget
        // The pool-level update-after-set permission is conditional. Only the
        // bindless texture set needs it, and Vulkan may expose descriptor
        // indexing without the granular sampled-image update-after-bind bit.
        poolDesc.flags = bindlessUpdateAfterSet
                       ? nri::DescriptorPoolBits::ALLOW_UPDATE_AFTER_SET
                       : nri::DescriptorPoolBits::NONE;
        return poolDesc;
    }

    bool MeshNode::CreateBindings()
    {
        const nri::CoreInterface& core = m_device->Core();

        // THE LAYOUT (Task 8/10 rewrote this in full; F3 plan 1 T7 shrank
        // the root block and grew the frame set). mesh.hlsl's register map:
        // b0 root constants (the 8-byte MeshRootConstants {firstOutput,
        // flags} -- MeshNode.hpp) plus ONE immutable ROOT SAMPLER at s0,
        // both at the implicit rootRegisterSpace; and TWO ordinary
        // descriptor sets -- space1 = { b1 frame CB, t0 the GPU scene's
        // instance rows, t1 this slot's visible indices }, space2 = { t0
        // bindless material array }.
        //
        // THE REGISTER-SPACE RULE (Batch2DNode.hpp's header states it in
        // full, verified against Source/Validation/DeviceVal.hpp's
        // `rootDescriptorNum || rootSamplerNum` guard): NRI refuses
        // rootRegisterSpace == a set's registerSpace ONLY when the layout
        // carries a root DESCRIPTOR or root SAMPLER -- root CONSTANTS alone
        // are exempt, because VK lowers them to push constants, outside the
        // set-space numbering entirely. Before this task the layout carried
        // only root constants, so keeping the one descriptor set AND
        // rootRegisterSpace both at space0 was legal and is what
        // mesh.hlsl's then-implicit space0 registers required. Adding the
        // root sampler below trips that guard, so root items stay at
        // space0 (rootRegisterSpace unchanged -- b0 and s0 keep their
        // existing implicit-space0 HLSL registers, UNCHANGED text) and the
        // two ordinary sets move to space1/space2 instead (mesh.hlsl's b1/
        // t0 gain explicit `spaceN` annotations to match). This also has to
        // agree with the SPIR-V register-shift table dxc is invoked with
        // (compile-shaders.bat's SPIRV_FLAGS / ShaderConventions.hpp::
        // kSpirvArgs): NRI adds the SAME per-resource-type binding offset
        // to a range regardless of which space it is in
        // (Source/VK/PipelineLayoutVK.hpp's `bindingOffsets` array), so
        // dxc must shift b-registers in space1 and t-registers in space1/
        // space2 by the same amounts it already shifts space0's -- both
        // files carry `-fvk-b-shift 256 1` / `-fvk-t-shift 0 2` (Task 8/10)
        // and `-fvk-t-shift 0 1` (F3 plan 1 T7) for exactly that reason:
        // in SPIR-V, set 1 = { b1 at binding 257, t0 at 0, t1 at 1 }.
        nri::RootConstantDesc rootConstant = {};
        rootConstant.registerIndex = 0;                       // b0
        rootConstant.size          = sizeof(MeshRootConstants);   // 8 (the header static_asserts it)
        // BOTH stages, unchanged from Task 8/10 -- though only vs_main reads
        // the root block now (the tint and the material slot travel per row
        // and reach ps_main as varyings; spirv-dis of mesh_ps.bin shows no
        // push-constant block at all). A stage set that is a SUPERSET of the
        // readers is legal on both backends (a VK push-constant range may
        // name a stage that declares none; a D3D12 root parameter visible
        // to more stages than read it is merely unoptimised), and two
        // dwords of root budget is not worth narrowing something that has
        // been right on both backends since F2a.
        rootConstant.shaderStages  = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        // THE ONE IMMUTABLE SAMPLER (Task 10's slice-one sampler strategy,
        // spec section 6): trilinear -- LINEAR min/mag/mip, unchanged from
        // the sampler this replaces -- and REPEAT (a mesh's UVs tile),
        // anisotropy as the ONE tunable knob (render.textureAnisotropy, read
        // here, once, when the layout is built: Restart). A ROOT/static sampler
        // (NRIDescs.h:1022's own words), baked into the pipeline layout
        // itself rather than a descriptor-set entry: it consumes no pool
        // budget and needs no per-frame write -- CmdSetPipelineLayout
        // pushes it automatically on both backends (VK:
        // CommandBufferVK::SetPipelineLayout's "Push immutable samplers"
        // block; D3D12: a D3D12_STATIC_SAMPLER_DESC baked into the root
        // signature at creation) -- and Record()'s existing
        // CmdSetPipelineLayout call is already what does that; nothing new
        // to call here.
        nri::RootSamplerDesc rootSampler = {};
        rootSampler.registerIndex     = 0;                    // s0
        rootSampler.desc.filters.min  = nri::Filter::LINEAR;
        rootSampler.desc.filters.mag  = nri::Filter::LINEAR;
        rootSampler.desc.filters.mip  = nri::Filter::LINEAR;
        rootSampler.desc.addressModes = { nri::AddressMode::REPEAT, nri::AddressMode::REPEAT,
                                          nri::AddressMode::REPEAT };
        rootSampler.desc.anisotropy   = static_cast<std::uint8_t>(Settings<RenderSettings>().textureAnisotropy);   // THE ONE KNOB -- see the comment above
        rootSampler.desc.mipMax       = 16.0f;
        rootSampler.shaderStages      = nri::StageBits::FRAGMENT_SHADER;   // only ps_main samples

        // set (array index 0, space1), THREE ranges: b1, the per-frame-slot
        // frame CB -- read by both stages (viewProjection in the VS, the
        // light in the PS); then (F3 plan 1 T7) t0, the GPU scene's ONE
        // instance buffer (a STRUCTURED_BUFFER view, stride 240 --
        // GpuScene::InstancesView), and t1, THIS slot's visible-index buffer
        // (stride 4 -- GpuScene::VisibleIndicesView(slot)), both vertex-
        // only. Range indices 0/1/2 are what Record's UpdateDescriptorRanges
        // names. They do not need update-after-set: Record rewrites only the
        // current frame slot after BeginFrame retired that slot's prior use.
        nri::DescriptorRangeDesc frameRanges[3] = {};
        frameRanges[0].baseRegisterIndex = 1;                 // b1
        frameRanges[0].descriptorNum     = 1;
        frameRanges[0].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        frameRanges[0].shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        frameRanges[1].baseRegisterIndex = 0;                 // t0 -- the instance rows
        frameRanges[1].descriptorNum     = 1;
        frameRanges[1].descriptorType    = nri::DescriptorType::STRUCTURED_BUFFER;
        frameRanges[1].shaderStages      = nri::StageBits::VERTEX_SHADER;
        frameRanges[2].baseRegisterIndex = 1;                 // t1 -- this slot's visible indices
        frameRanges[2].descriptorNum     = 1;
        frameRanges[2].descriptorType    = nri::DescriptorType::STRUCTURED_BUFFER;
        frameRanges[2].shaderStages      = nri::StageBits::VERTEX_SHADER;
        nri::DescriptorSetDesc frameSetDesc = {};
        frameSetDesc.registerSpace = 1;
        frameSetDesc.ranges        = frameRanges;
        frameSetDesc.rangeNum      = 3;

        // set (array index 1, space2): the bindless material array at t0 --
        // a FIXED size (kBindlessCapacity, matched by mesh.hlsl's own
        // literal), not VARIABLE_SIZED_ARRAY: BindlessTable's capacity is
        // decided once at MeshNode creation and never resized
        // (BindlessTable.hpp's SLOT POLICY), so there is nothing for
        // AllocateDescriptorSets' variableDescriptorNum argument to do
        // here (CreateSets passes 0). PARTIALLY_BOUND: a table that has
        // not yet Added every slot leaves the unused tail genuinely
        // unwritten, which is legal by construction -- mesh.hlsl only ever
        // indexes a slot AddMaterial actually wrote -- rather than a
        // validation violation.
        // ALLOW_UPDATE_AFTER_SET is added only when the granular sampled-image
        // limit covers this entire range. Otherwise AddMaterial waits for
        // device idle before appending a descriptor.
        nri::DescriptorRangeDesc bindlessRange = {};
        bindlessRange.baseRegisterIndex = 0;                  // t0
        bindlessRange.descriptorNum     = kBindlessCapacity;
        bindlessRange.descriptorType    = nri::DescriptorType::TEXTURE;
        bindlessRange.shaderStages      = nri::StageBits::FRAGMENT_SHADER;
        bindlessRange.flags             = nri::DescriptorRangeBits::ARRAY
                                         | nri::DescriptorRangeBits::PARTIALLY_BOUND;
        if (m_bindlessUpdateAfterSet)
            bindlessRange.flags |= nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

        nri::DescriptorSetDesc bindlessSetDesc = {};
        bindlessSetDesc.registerSpace = 2;
        bindlessSetDesc.ranges        = &bindlessRange;
        bindlessSetDesc.rangeNum      = 1;
        // The SET-level counterpart is present exactly when the range opted in.
        bindlessSetDesc.flags = m_bindlessUpdateAfterSet
                              ? nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET
                              : nri::DescriptorSetBits::NONE;

        nri::DescriptorSetDesc setDescs[2] = { frameSetDesc, bindlessSetDesc };

        // Value-initialized then assigned field by field: NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so its padding has to
        // be zeroed).
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.rootConstants     = &rootConstant;
        layoutDesc.rootConstantNum   = 1;
        layoutDesc.rootSamplers      = &rootSampler;
        layoutDesc.rootSamplerNum    = 1;
        layoutDesc.descriptorSets    = setDescs;
        layoutDesc.descriptorSetNum  = 2;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        if (!m_pipelines->Layout(m_layoutId))
        {
            ARC_ERROR("[nri-graph] MeshNode: pipeline layout registration failed");
            return false;
        }

        // The pool. NOTHING IN IT IS EVER REWRITTEN WHILE THE GPU MIGHT READ
        // IT: a per-frame set's b1 is written once, at Create; its t0/t1 are
        // rewritten only for the CURRENT slot, whose previous frame retired
        // at BeginFrame (the header's WHAT THIS NODE OWNS block); and the
        // bindless set only ever gains slots nothing has read yet (see
        // AddMaterial's own synchronization comment). Nothing is ever FREED,
        // which is what keeps ResetDescriptorPool and its fence discipline
        // out of this file, exactly as in Batch2DNode.
        const nri::DescriptorPoolDesc poolDesc = PoolSizes(m_bindlessUpdateAfterSet);
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] MeshNode: descriptor pool creation failed");
            return false;
        }
        return true;
    }

    bool MeshNode::CreateConstantArena()
    {
        const nri::CoreInterface& core = m_device->Core();
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());

        // The CALLER supplies constant-buffer alignment everywhere on this
        // path, and this is where it comes from. It also fixes the SIZE every
        // CB view is created with: NRI passes BufferViewDesc::size straight
        // into D3D12_CONSTANT_BUFFER_VIEW_DESC::SizeInBytes, which D3D12
        // requires to be a multiple of 256 -- so the views name a whole region
        // and the shader simply reads less than it.
        m_arenaStride = CbRegionStride(deviceDesc.memoryAlignment.constantBufferOffset);
        const std::uint64_t arenaBytes = m_arenaStride * FramesInFlight();

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = arenaBytes;
        bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                       0.0f, bufferDesc, m_arena))
            || !m_arena)
        {
            ARC_ERROR("[nri-graph] MeshNode: the frame constant-buffer arena ({} bytes) could not "
                      "be created", arenaBytes);
            return false;
        }
        core.SetDebugName(m_arena, "nri-graph mesh frame CBs");

        // Persistent map, unmapped once in Release()/~MeshNode -- the same
        // shape NriUploadRing and Batch2DNode use, and the same NONE-backend
        // footgun: MapBuffer returns null unconditionally there, so this node
        // is [gpu]-only from here down.
        m_arenaCpu = core.MapBuffer(*m_arena, 0, nri::WHOLE_SIZE);
        if (!m_arenaCpu)
        {
            ARC_ERROR("[nri-graph] MeshNode: the frame constant-buffer arena could not be mapped "
                      "(the NONE backend cannot -- this node is a [gpu] path)");
            return false;
        }

        // The b1 view per frame slot, created ONCE. Its contents change every
        // frame; its (buffer, offset) never does, which is exactly what lets a
        // descriptor set naming it be written once too.
        for (std::uint32_t slot = 0; slot < FramesInFlight(); ++slot)
        {
            nri::BufferViewDesc viewDesc = {};
            viewDesc.buffer = m_arena;
            viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
            viewDesc.offset = ArenaOffset(slot);
            viewDesc.size   = m_arenaStride;
            if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, m_frameCbView[slot]))
                || !m_frameCbView[slot])
            {
                ARC_ERROR("[nri-graph] MeshNode: the frame constant-buffer view for frame slot {} "
                          "could not be created", slot);
                return false;
            }
        }

        return true;
    }

    bool MeshNode::CreateSets()
    {
        const nri::CoreInterface& core = m_device->Core();
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] MeshNode: no layout to allocate descriptor sets from");
            return false;
        }

        for (std::uint32_t slot = 0; slot < FramesInFlight(); ++slot)
        {
            // setIndex 0: the ARRAY position of frameSetDesc in
            // CreateBindings' setDescs[2] -- an ARRAY INDEX, not a register
            // space (Source/VK/PipelineLayoutVK.hpp's `m_BindingInfo.sets`
            // is a 1:1 copy of `descriptorSets[]` in array order, and
            // `SetDescriptorSet` indexes it by that same position).
            if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0,
                                                           &m_sets[slot], 1, 0))
                || !m_sets[slot])
            {
                ARC_ERROR("[nri-graph] MeshNode: descriptor-set allocation failed for frame slot "
                          "{} -- the pool holds {} sets (PoolSizes)", slot,
                          PoolSizes(m_bindlessUpdateAfterSet).descriptorSetMaxNum);
                return false;
            }

            // `cb`'s ADDRESS must outlive the UpdateDescriptorRanges call
            // below: UpdateDescriptorRangeDesc::descriptors is a POINTER TO
            // AN ARRAY dereferenced inside the call, so a single descriptor
            // is passed as the address of a variable that has to still be
            // alive when the call runs.
            const nri::Descriptor* cb = m_frameCbView[slot];

            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = m_sets[slot];
            update.rangeIndex    = 0;   // b1 -- ranges 1-2 (t0/t1, the GPU scene) are Record's, per slot
            update.descriptors   = &cb;
            update.descriptorNum = 1;
            core.UpdateDescriptorRanges(&update, 1);
        }

        // THE BINDLESS SET -- setIndex 1 (bindlessSetDesc's array position),
        // allocated ONCE for the node's whole lifetime, not per frame slot:
        // unlike b1 there is nothing in it that differs frame to frame.
        // Its range is left UNWRITTEN here -- AddMaterial writes each slot
        // as it is Added, and CreateBindings' PARTIALLY_BOUND flag is what
        // makes allocating it with a still-empty range legal.
        if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 1, &m_bindlessSet, 1, 0))
            || !m_bindlessSet)
        {
            ARC_ERROR("[nri-graph] MeshNode: the bindless material descriptor set could not be "
                      "allocated -- the pool holds {} sets (PoolSizes)",
                      PoolSizes(m_bindlessUpdateAfterSet).descriptorSetMaxNum);
            return false;
        }
        return true;
    }

    std::uint32_t MeshNode::AddMaterial(nri::Descriptor* srv)
    {
        // See this method's own doc comment in MeshNode.hpp for the full
        // ownership and synchronization contract.
        if (!m_bindless || !m_bindlessSet || !srv)
            return BindlessTable::kInvalidSlot;

        const nri::CoreInterface& core = m_device->Core();
        if (!m_bindlessUpdateAfterSet
            && !ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device())))
        {
            ARC_ERROR("[nri-graph] MeshNode: could not retire in-flight work before "
                      "updating a bindless material descriptor");
            return BindlessTable::kInvalidSlot;
        }

        const std::uint32_t slot = m_bindless->Add(srv);
        if (slot == BindlessTable::kInvalidSlot)
            return BindlessTable::kInvalidSlot;   // null srv, or the table is full -- BindlessTable
                                                   // already warned (once) or refused silently, per
                                                   // its own Add() contract

        // `view`'s ADDRESS must outlive UpdateDescriptorRanges -- same rule
        // CreateSets' `cb` local follows, and for the same reason.
        const nri::Descriptor* view = srv;

        nri::UpdateDescriptorRangeDesc update = {};
        update.descriptorSet  = m_bindlessSet;
        update.rangeIndex     = 0;      // the bindless array is the set's only range
        update.baseDescriptor = slot;   // WHERE in the array -- not an append
        update.descriptors    = &view;
        update.descriptorNum  = 1;
        core.UpdateDescriptorRanges(&update, 1);
        return slot;
    }

    MeshNode::~MeshNode()
    {
        // m_bindless resets UNCONDITIONALLY below, regardless of this guard
        // (and regardless of whether m_device is even non-null): BindlessTable's
        // own destructor is a no-op once Release() has emptied it (its guard is
        // `!m_device || m_slots.empty()`), and a genuine leak -- Release() never
        // called -- is exactly the case that destructor's own WARN + DeviceWaitIdle
        // + destroy exists to catch, so this function does not need to duplicate
        // that check, and a default-constructed MeshNode (m_bindless still null)
        // resets a null unique_ptr harmlessly.
        if (m_device && (m_pool || m_arena))
        {
            ARC_WARN("[nri-graph] MeshNode destroyed with live NRI objects -- either Create() failed "
                     "part way (an ERROR above says which step) or its owner never called Release(). "
                     "Destroying directly behind a DeviceWaitIdle.");
            const nri::CoreInterface& core = m_device->Core();
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
            if (m_pool) core.DestroyDescriptorPool(m_pool);
            for (nri::Descriptor*& view : m_frameCbView)
                if (view) { core.DestroyDescriptor(view); view = nullptr; }
            if (m_arena)
            {
                if (m_arenaCpu) core.UnmapBuffer(*m_arena);
                core.DestroyBuffer(m_arena);
            }
            // The sets (including the bindless one) are owned by the pool
            // destroyed above -- NRI has no per-set destroy, so forgetting
            // the pointers is the whole of it.
            for (nri::DescriptorSet*& set : m_sets)
                set = nullptr;
            m_bindlessSet = nullptr;
            m_pool = nullptr;
            m_arena = nullptr; m_arenaCpu = nullptr;
        }
        m_bindless.reset();
    }

    void MeshNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // THE BINDLESS TABLE FIRST: BindlessTable::Release buries every
        // occupied slot's descriptor at `fence` (its own comment) --
        // discharging views this node's table owns per AddMaterial's
        // OWNERSHIP contract, NOT the textures they view (a caller's own,
        // per that same contract). The unique_ptr itself is NOT reset here
        // -- see ~MeshNode's comment for why an idempotent Release() leaving
        // an empty-but-alive BindlessTable behind is the right shape.
        if (m_bindless)
            m_bindless->Release(graveyard, fence);
        m_bindlessSet = nullptr;   // owned by the pool buried below

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view can never outlive its texture (or, for
        // the constant buffer below, its arena).
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
        }
        // Sets are owned by the pool buried above; NRI has no per-set destroy,
        // so forgetting the pointers is the whole of it.
        for (nri::DescriptorSet*& set : m_sets)
            set = nullptr;

        for (nri::Descriptor*& view : m_frameCbView)
        {
            if (!view)
                continue;
            graveyard.Bury(fence, [core, d = view] { core->DestroyDescriptor(d); });
            view = nullptr;
        }
        if (m_arena)
        {
            // Unmapped HERE rather than in the burial: the map is a CPU-side
            // fact with no GPU lifetime, and leaving it live inside a deferred
            // thunk would mean a mapped pointer this object still advertises
            // outlives the object.
            if (m_arenaCpu)
            {
                core->UnmapBuffer(*m_arena);
                m_arenaCpu = nullptr;
            }
            graveyard.Bury(fence, [core, b = m_arena] { core->DestroyBuffer(b); });
            m_arena = nullptr;
        }

        for (nri::Pipeline*& pipeline : m_pipeline)
            pipeline = nullptr;   // owned by the shared cache; the vehicle clears it
    }

    nri::Pipeline* MeshNode::PipelineFor(nri::Format canvasFormat, MaterialBlendMode blend, bool twoSided)
    {
        const MeshPipelineState state = PipelineStateFor(blend, twoSided);
        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = ShaderPairId(state.pixel);
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = canvasFormat;
        key.colorCount      = 1;
        // THE DEPTH FORMAT IS PART OF THE PSO's IDENTITY -- NRI bakes the
        // depth-stencil format into a graphics pipeline exactly as it does the
        // colour formats, and binding one inside a CmdBeginRendering whose
        // depth attachment carries a different format is undefined on both
        // backends.
        key.depthFormat     = GraphDepthFormat();
        key.topology        = nri::Topology::TRIANGLE_LIST;
        key.blend           = state.blend;
        key.depthWrite      = state.depthWrite;
        key.cullMode        = state.cullMode;

        // `stages` lives in THIS frame, which encloses the GetGraphics call --
        // the fill contract's rule 2. The bytecode it points at is owned by the
        // vehicle's bin cache and outlives this node, which the same rule needs
        // (CreateGraphicsPipeline runs after the callback returns).
        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_vs.data();
        stages[0].size           = m_vs.size();
        stages[0].entryPointName = kVsEntry;   // SPIR-V matches by name; DXIL ignores it
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        const std::span<const std::uint8_t> ps = m_ps[static_cast<std::size_t>(state.pixel)];
        stages[1].bytecode       = ps.data();
        stages[1].size           = ps.size();
        stages[1].entryPointName = PixelEntry(state.pixel);

        return m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            desc.vertexInput = &m_vertexInput;
            desc.shaders     = stages;
            desc.shaderNum   = 2;

            // ============ THE WINDING, and why it is THIS pair ============
            // MeshBuilder emits triangles whose (v0,v1,v2) order is
            // COUNTER-CLOCKWISE as seen from OUTSIDE the surface
            // (MeshBuilder.hpp's WINDING block, algebraically verified there
            // for all six cube faces and for the sphere).
            //
            // The camera is right-handed and looks down -Z with +Y up
            // (SceneCamera.hpp, Task 5), and the executor sets a viewport with
            // originBottomLeft = false -- which on Vulkan is NRI's negative
            // height flip, so BOTH backends put NDC +Y at the TOP of the
            // target. An outward-facing triangle therefore lands on screen in
            // the order a viewer reads as counter-clockwise.
            //
            // `frontCounterClockwise = true` is what names that orientation
            // front-facing, and BACK culling then drops the far side of a
            // closed mesh. NRI maps this flag 1:1 to
            // D3D12_RASTERIZER_DESC::FrontCounterClockwise and to
            // VK_FRONT_FACE_COUNTER_CLOCKWISE (Source/D3D12/PipelineD3D12.hpp,
            // Source/VK/PipelineVK.hpp), so one value covers both -- and NRI's
            // own samples, which draw glTF content (CCW front faces, by that
            // spec), set the same `true`.
            //
            // THE DESK CHECK THAT FALSIFIES THIS IN ONE LOOK: get the pair
            // backwards and a closed CONVEX mesh (the cube, the sphere)
            // disappears entirely rather than rendering subtly wrong. There is
            // no plausible-looking failure mode here.
            desc.rasterization.fillMode              = nri::FillMode::SOLID;
            desc.rasterization.frontCounterClockwise = true;

            // ============ THE DEPTH TEST ============
            // FORWARD-Z, [0,1]: near maps to 0 and far to 1
            // (SceneCamera.hpp's DEPTH CONVENTION -- reverse-Z is a
            // separately-decided-against choice), so LESS is "nearer wins" and
            // the clear value is DepthClearValue(kSelectedDepthConvention), the
            // depth load op AddMeshNode declares. All six variants
            // TEST depth; whether one also WRITES it is the blend mode's
            // (PipelineStateFor: opaque and masked write, transparent tests
            // only), carried in `key.depthWrite` above and stamped by the
            // cache -- so this callback sets only the shared compare op.
            desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);
        });
    }

    void MeshNode::Prepare(nri::Format canvasFormat, const MeshSceneDesc& scene,
                           NriMeshBufferCache* meshBuffers, std::uint64_t frameCounter)
    {
        // The PSO, built HERE so a first-frame pipeline compile does not land
        // inside the recording window. Re-resolved every frame because the
        // canvas format is the CALLER's (AddMeshNode takes it as a parameter,
        // and a differently-formatted canvas must be a cache MISS rather than
        // a silent attachment mismatch) and a cache HIT is a linear scan over
        // a handful of entries.
        for (MaterialBlendMode blend : { MaterialBlendMode::Opaque, MaterialBlendMode::Masked,
                                         MaterialBlendMode::Transparent })
            for (const bool twoSided : { false, true })
                m_pipeline[PipelineIndex(blend, twoSided)] = PipelineFor(canvasFormat, blend, twoSided);

        // Residency at DECLARATION time -- UploadData submits and waits
        // internally. Record only looks this table up.
        //
        // GATED SEPARATELY FROM THE PIPELINE ABOVE, deliberately: a cache-less
        // context still gets its pipeline and still reports the real reason it drew
        // nothing, instead of a "missing pipeline" warning naming the wrong cause.
        m_residents.clear();
        m_adHocRows.clear();
        m_adHocDraws.clear();
        const auto resolveOnce = [&](const Guid& id)
        {
            if (id.IsNil() || !meshBuffers)
                return;
            for (const auto& e : m_residents)
                if (e.first == id)
                    return;
            m_residents.push_back({ id, meshBuffers->Resolve(id, frameCounter) });
        };

        // 1. The registry-backed batches: one resolve per distinct mesh.
        if (scene.scene)
            for (const GpuBatchDraw& batch : scene.scene->batches)
                resolveOnce(batch.mesh);
        if (scene.scene)
            for (const TransparentDraw& draw : scene.scene->transparentDraws)
                resolveOnce(draw.mesh);

        // 2. The ad-hoc instances (F3 plan 1 T7): resolved the same way, AND
        //    staged as GpuInstance rows for the sync node's scratch region --
        //    built even with a null cache, since they are the sync node's
        //    input regardless of whether anything can be drawn. CAPPED at the
        //    scratch capacity HERE, and the overflow WARNED HERE, once: the
        //    sync node only ever sees the capped span, so GpuScene::Reserve's
        //    own `adHocCount > ScratchRows()` guard can never fire on this
        //    path (it stays as a second line of defence for any other
        //    caller). Row i here IS scratch row i on the device -- Apply
        //    copies the span contiguously and Record's pushRoot depends on it.
        std::size_t dropped = 0;
        for (const MeshInstance& instance : scene.instances)
        {
            if (instance.mesh.IsNil())
                continue;   // an empty slot is not an error -- see MeshInstance::mesh
            if (m_adHocRows.size() >= m_scratchRows)
            {
                ++dropped;
                continue;   // counted, not staged -- the WARN below names the total
            }
            resolveOnce(instance.mesh);

            GpuInstance row;
            row.model     = instance.model;
            row.prevModel = instance.model;   // an ad-hoc row has no history: prev == model (the G2 contract's "new row")
            // NormalMatrixFor's singular guard means a degenerate instance
            // gets identity here rather than NaN reaching the GPU -- the same
            // product the old root block carried, now three row columns.
            const glm::mat3 n = NormalMatrixFor(instance.model);
            row.normal0      = glm::vec4(n[0], 0.0f);
            row.normal1      = glm::vec4(n[1], 0.0f);
            row.normal2      = glm::vec4(n[2], 0.0f);
            row.baseColor    = instance.baseColor;
            row.materialSlot = instance.materialSlot;
            m_adHocRows.push_back(row);
            m_adHocDraws.push_back(AdHocDraw{ instance.mesh, instance.indexOffset, instance.indexCount });
        }
        if (dropped > 0 && !m_warnedScratchOverflow)
        {
            m_warnedScratchOverflow = true;
            ARC_WARN("[nri-graph] MeshNode: {} ad-hoc instance(s) exceed the {} scratch rows per frame slot "
                     "(render.gpuScene.scratchRowsPerFrame) and are DROPPED this frame -- the first {} are drawn; "
                     "further occurrences are silent",
                     dropped, m_scratchRows, m_scratchRows);
        }
    }

    void MeshNode::Record(RenderGraphNodeContext& context, const MeshSceneDesc& scene,
                          std::uint32_t frameSlot, GpuScene* gpuScene,
                          const GpuSceneFrameReadiness& readiness)
    {
        const nri::CoreInterface& core = context.core;

        // m_residents holds BORROWED pointers into NriMeshBufferCache's map and this
        // function is their last reader, so the table is dropped on EVERY exit path
        // -- not just the tail. Between Record(N) and Prepare(N+1) an
        // InvalidateMeshGeometry erases map nodes; leaving stale pointers in a live
        // member for that window is a trap even though nothing walks it today. See
        // the member's own declaration comment.
        struct ResidentScope
        {
            std::vector<std::pair<Guid, const NriMeshBufferCache::Resident*>>* table;
            ~ResidentScope() { table->clear(); }
        } residentScope{ &m_residents };

        // The depth target was cleared when rendering began: AddMeshNode
        // declares a CLEAR depth load op to DepthClearValue(
        // kSelectedDepthConvention). The colour attachment LOADs -- batch2d
        // already cleared the canvas and drew this frame's 2D content into it.

        // NOTHING TO DRAW is a cleared depth buffer, not an error: no emitted
        // batch and no ad-hoc row. That includes ruling R-D's frame -- rows
        // STAGED but everything culled (MeshSceneDesc::Empty explains why
        // such a frame is declared at all): the sync node ahead of this one
        // uploaded them, and this pass has no draw to record.
        const MeshDrawSelection draws = SelectMeshDrawPaths(
            scene.scene && scene.scene->HasDraws(), !m_adHocDraws.empty(), readiness);
        if (!draws.registry && !draws.adHoc)
            return;

        // ALL SIX VARIANTS, not just the opaque one: Prepare resolves them
        // together, so in practice they are all present or all missing -- but
        // the gate says what is actually true either way. With every variant
        // missing there is nothing to bind and the pass records only its clear;
        // with some missing, bindPipeline below skips the draws in those
        // variants (the cache already said why they failed) and the rest draw.
        std::uint32_t missingPipelines = 0;
        for (MaterialBlendMode blend : { MaterialBlendMode::Opaque, MaterialBlendMode::Masked,
                                         MaterialBlendMode::Transparent })
            for (const bool twoSided : { false, true })
                if (!m_pipeline[PipelineIndex(blend, twoSided)])
                    ++missingPipelines;
        if (missingPipelines != 0)
        {
            if (!m_warnedNoPipeline)
            {
                m_warnedNoPipeline = true;
                ARC_WARN("[nri-graph] MeshNode: {} of the six mesh pipelines (opaque/masked/transparent x "
                         "one-/two-sided) are missing for the canvas format -- {} (Prepare was skipped, "
                         "or the cache refused them and said why)",
                         missingPipelines,
                         missingPipelines == 6 ? "the mesh pass draws nothing this run"
                                               : "draws in the missing variants are skipped");
            }
            if (missingPipelines == 6)
                return;
        }

        // THE CAMERA. Checked rather than trusted -- see IsFinite's comment.
        if (!IsFinite(scene.view) || !IsFinite(scene.projection))
        {
            if (!m_warnedBadCamera)
            {
                m_warnedBadCamera = true;
                GraphError("MeshNode: the scene's view or projection matrix is not finite (a zero "
                           "fov, a non-positive aspect ratio or nearZ >= farZ all produce NaN "
                           "silently) -- the whole mesh pass (opaque, masked and transparent) is "
                           "dropped rather than recording a draw with an undefined clip position");
            }
            return;
        }

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("MeshNode: the pipeline layout is gone -- nothing recorded");
            return;
        }
        if (!gpuScene)   // AddMeshNode passes context->Scene(); null means the vehicle has no GPU scene
        {
            GraphError("MeshNode: no GpuScene on the context -- nothing recorded");
            return;
        }

        // ---------------------------------------------------------------
        // THE FRAME CONSTANTS, into THIS frame slot's arena region. Written
        // here rather than at declaration time because the frame-pacing fence
        // this slot is safe behind is waited inside Execute (the swapchain
        // acquire), which is upstream of every exec fn and downstream of every
        // declaration. HOST_UPLOAD memory is host-coherent, so a memcpy is the
        // whole of the upload -- no barrier, no flush.
        // ---------------------------------------------------------------
        MeshFrameConstants frameConstants;
        frameConstants.viewProjection = scene.projection * scene.view;
        frameConstants.lightDirection = glm::vec4(SafeNormalize(scene.lightDirection), 0.0f);
        frameConstants.lightColor     = glm::vec4(scene.lightColor, 0.0f);
        frameConstants.ambient        = glm::vec4(scene.ambient, 0.0f);
        if (auto* arena = static_cast<std::uint8_t*>(m_arenaCpu))
            std::memcpy(arena + ArenaOffset(frameSlot), &frameConstants, sizeof(frameConstants));

        // ---------------------------------------------------------------
        // Resident vertex + index buffers, looked up from Prepare's table.
        // NEVER resolved here -- UploadData submits and waits internally.
        // DEDUPED BY GUID: a scene of twenty cubes is one bind and twenty
        // draws. Offset is always 0: these are dedicated buffers, not a ring.
        // ---------------------------------------------------------------
        const auto residentFor = [&](const Guid& id) -> const NriMeshBufferCache::Resident*
        {
            for (const auto& e : m_residents)
                if (e.first == id)
                    return e.second;
            return nullptr;
        };

        // THE TWO SETS this pass binds. set0 (frame slot) carries only b1
        // now (Task 8/10 moved the white texel/sampler out -- s0 is a root/
        // immutable sampler CmdSetPipelineLayout pushes automatically,
        // needing no CmdSetDescriptorSet of its own); every instance reads
        // the same b1, so there is nothing per-instance in it to rebind.
        // set1 is the bindless material array -- ONE set for the node's
        // whole lifetime, not per frame slot, bound here once per pass
        // exactly like set0 (its CONTENTS may still be growing via
        // AddMaterial between passes; what does not change mid-pass is
        // WHICH set is bound). A null here means Create() failed part way
        // and already said so.
        nri::DescriptorSet* set = frameSlot < FramesInFlight() ? m_sets[frameSlot] : nullptr;
        if (!set || !m_bindlessSet)
        {
            GraphError("MeshNode: no descriptor set for this frame slot -- nothing recorded");
            return;
        }

        // ---------------------------------------------------------------
        // THE FRAME SET's t0/t1 VIEWS (F3 plan 1 T7): rewritten for THIS slot
        // when the GPU scene's buffers changed -- EITHER the instance buffer
        // grew (its generation moved; the view object was replaced with it)
        // OR this slot's visible-index buffer was re-created (its view
        // pointer moved; GpuScene keeps no generation for those). Both
        // halves of the compare are load-bearing. Safe: this slot's previous
        // frame retired at BeginFrame, so nothing in flight reads the set;
        // no update-after-set feature is needed. The first Record for a slot
        // always writes (the members start at 0 / null).
        // ---------------------------------------------------------------
        if (m_setInstanceGen[frameSlot] != gpuScene->InstanceBufferGeneration()
            || m_setVisibleView[frameSlot] != gpuScene->VisibleIndicesView(frameSlot))
        {
            // `views` must outlive the UpdateDescriptorRanges call -- the same
            // rule CreateSets' `cb` follows: `descriptors` is a pointer to an
            // array dereferenced inside the call.
            const nri::Descriptor* views[2] = { gpuScene->InstancesView(), gpuScene->VisibleIndicesView(frameSlot) };
            if (!views[0] || !views[1])
            {
                GraphError("MeshNode: the GPU scene has no instance / visible-index view for this frame slot -- nothing recorded");
                return;
            }
            nri::UpdateDescriptorRangeDesc updates[2] = {};
            for (std::uint32_t i = 0; i < 2; ++i)
            {
                updates[i].descriptorSet = set;
                updates[i].rangeIndex    = 1 + i;   // CreateBindings' frameRanges[1] (t0) / [2] (t1)
                updates[i].descriptors   = &views[i];
                updates[i].descriptorNum = 1;
            }
            core.UpdateDescriptorRanges(updates, 2);
            m_setInstanceGen[frameSlot] = gpuScene->InstanceBufferGeneration();
            m_setVisibleView[frameSlot] = gpuScene->VisibleIndicesView(frameSlot);
        }

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        // ONE layout for the whole pass, so this is bound once. CmdSetPipeline
        // Layout invalidates the bound sets and root constants on both
        // backends, which is exactly why it must not be re-issued per draw --
        // and why the sets below are bound AFTER it. It is also what pushes
        // the root/immutable s0 sampler automatically on both backends
        // (CommandBufferVK::SetPipelineLayout's "Push immutable samplers"
        // block; a D3D12 static sampler needs no runtime bind call at all)
        // -- nothing else in this function touches s0.
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetDescriptorSetDesc frameSetDesc = {};
        frameSetDesc.setIndex      = 0;   // frameSetDesc's array position, CreateBindings
        frameSetDesc.descriptorSet = set;
        core.CmdSetDescriptorSet(context.cmd, frameSetDesc);

        nri::SetDescriptorSetDesc bindlessSetDesc = {};
        bindlessSetDesc.setIndex      = 1;   // bindlessSetDesc's array position, CreateBindings
        bindlessSetDesc.descriptorSet = m_bindlessSet;
        core.CmdSetDescriptorSet(context.cmd, bindlessSetDesc);

        nri::Pipeline* boundPipeline = nullptr;
        const auto bindPipeline = [&](MaterialBlendMode blend, bool twoSided) -> bool
        {
            nri::Pipeline* pipeline = m_pipeline[PipelineIndex(blend, twoSided)];
            if (!pipeline)
                return false;   // Prepare/cache already reported the failed variant.
            if (pipeline != boundPipeline)
            {
                core.CmdSetPipeline(context.cmd, *pipeline);
                boundPipeline = pipeline;
            }
            return true;
        };

        // Binds a mesh's resident vertex + index buffers if it is resident and
        // not already bound; null means SKIP THE DRAW -- never a stale bind
        // from the previous one. Offset is always 0: dedicated buffers, not a
        // ring.
        Guid lastMesh{};
        bool lastBound = false;
        const auto bindMesh = [&](const Guid& id) -> const NriMeshBufferCache::Resident*
        {
            const NriMeshBufferCache::Resident* resident = residentFor(id);
            if (!resident || !resident->ready || !resident->vertexBuffer || !resident->indexBuffer)
                return nullptr;
            if (!lastBound || id != lastMesh)
            {
                lastMesh  = id;
                lastBound = true;
                nri::VertexBufferDesc vertexBuffer = {};
                vertexBuffer.buffer = resident->vertexBuffer;
                vertexBuffer.offset = 0;
                vertexBuffer.stride = sizeof(MeshVertex);
                core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);
                core.CmdSetIndexBuffer(context.cmd, *resident->indexBuffer, 0,
                                        nri::IndexType::UINT32);
            }
            return resident;
        };
        // THE ROOT BLOCK, once per draw: {firstOutput, flags} -- the whole of
        // what a draw carries now (MeshRootConstants, MeshNode.hpp).
        const auto pushRoot = [&](std::uint32_t firstOutput, std::uint32_t flags)
        {
            const MeshRootConstants push{ firstOutput, flags };
            nri::SetRootConstantsDesc rootConstants = {};
            rootConstants.rootConstantIndex = 0;
            rootConstants.data              = &push;
            rootConstants.size              = sizeof(push);
            core.CmdSetRootConstants(context.cmd, rootConstants);
        };

        // ---------------------------------------------------------------
        // 1. THE REGISTRY-BACKED BATCHES, INDIRECT, in the frame's order
        //    (BuildGpuSceneFrame: opaque before masked, nearest first). One
        //    CmdDrawIndexedIndirect per batch, its nri::DrawIndexedDesc at
        //    argIndex in this slot's args buffer (GpuSceneSyncNode copied
        //    the frame's DrawIndexedArgs there -- GpuScene.cpp static_asserts
        //    the two layouts agree). instanceNum ARRIVES ZERO and is filled
        //    IN by the mesh-cull compute dispatch, which is its sole
        //    incrementer -- the CPU never authors a nonzero count, so a
        //    stale one cannot survive a frame -- and the vertex shader maps
        //    SV_InstanceID through the visible-index list from
        //    batch.firstOutput. An indirect draw whose instanceNum the cull
        //    left at 0 (every row of an emitted batch failed the GPU frustum
        //    test) is legal and draws nothing.
        // ---------------------------------------------------------------
        if (draws.registry)
        {
            nri::Buffer* args = gpuScene->Args(frameSlot);
            if (!args)
            {
                GraphError("MeshNode: the GPU scene has no indirect-args buffer for this frame slot -- the batches are dropped");
            }
            else
            {
                for (const GpuBatchDraw& batch : scene.scene->batches)
                {
                    if (!bindPipeline(batch.blend, batch.twoSided))
                        continue;
                    if (!bindMesh(batch.mesh))
                        continue;   // not resident: skip, never a stale bind
                    pushRoot(batch.firstOutput, 0);
                    core.CmdDrawIndexedIndirect(context.cmd, *args,
                                                std::uint64_t(batch.argIndex) * sizeof(nri::DrawIndexedDesc),
                                                1, sizeof(nri::DrawIndexedDesc), nullptr, 0);
                }
            }
        }

        // ---------------------------------------------------------------
        // 2. THE AD-HOC ROWS, DIRECT, in submission order, from this slot's
        //    scratch region: Prepare staged row i as m_adHocRows[i], the sync
        //    node copied it to scratch row ScratchFirstRow(slot) + i, and the
        //    root block names that row outright (kMeshRootDirect). One
        //    CmdDrawIndexed each -- the F2a shape, minus the 128-byte push.
        // ---------------------------------------------------------------
        const std::uint32_t scratchFirst = gpuScene->ScratchFirstRow(frameSlot);
        for (std::size_t i = 0; draws.adHoc && i < m_adHocDraws.size(); ++i)
        {
            if (!bindPipeline(MaterialBlendMode::Opaque, false))
                break;
            const AdHocDraw& d = m_adHocDraws[i];
            const NriMeshBufferCache::Resident* resident = bindMesh(d.mesh);
            if (!resident)
                continue;
            pushRoot(scratchFirst + static_cast<std::uint32_t>(i), kMeshRootDirect);

            nri::DrawIndexedDesc draw = {};
            draw.baseIndex   = d.indexOffset;
            draw.indexNum    = d.indexCount ? d.indexCount : resident->indexCount;
            draw.instanceNum = 1;
            core.CmdDrawIndexed(context.cmd, draw);
        }

        // 3. Ordered transparent rows. They never enter indirect batches:
        // Task 2 already sorted these direct records back-to-front, so retain
        // that exact order while switching only the independent sidedness PSO.
        if (draws.registry && scene.scene)
            for (const TransparentDraw& draw : scene.scene->transparentDraws)
            {
                if (!bindPipeline(draw.blend, draw.twoSided) || !bindMesh(draw.mesh))
                    continue;
                pushRoot(draw.row, kMeshRootDirect);
                nri::DrawIndexedDesc direct = {};
                direct.baseIndex   = draw.indexOffset;
                direct.indexNum    = draw.indexCount;
                direct.instanceNum = 1;
                core.CmdDrawIndexed(context.cmd, direct);
            }
    }

    RgTexture AddMeshNode(RenderGraph& graph, NriGraphContext* context,
                          RgTexture canvas, nri::Format canvasFormat,
                          const MeshSceneDesc& scene,
                          std::uint32_t width, std::uint32_t height)
    {
        // Resolved at DECLARATION time on purpose: a PSO compile must not land
        // inside the recording window. See MeshNode::Prepare.
        //
        // `canvasFormat` is the CALLER's, not GraphCanvasFormat() assumed --
        // see AddMeshNode's header comment for why this function cannot derive
        // it and what a wrong one costs.
        std::span<const GpuInstance> adHoc;
        if (context)
        {
            // UNCONDITIONAL once a node exists: MeshBuffers() may be null, and
            // Prepare's own residency half is what skips then -- the PIPELINE half
            // must still run (see Prepare's declaration comment). Prepare ALSO
            // stages the ad-hoc rows (F3 plan 1 T7), which the sync node below
            // needs BEFORE the mesh node is declared -- so it runs first.
            if (MeshNode* node = context->Mesh())
            {
                node->Prepare(canvasFormat, scene, context->MeshBuffers(),
                              context->PresentedFrames());
                adHoc = node->AdHocRows();
            }
        }

        // THE GPU SCENE'S WRITER, FIRST (F3 plan 1 T6, ruling R-A): the sync
        // node imports the persistent instance / args / visible-index buffers
        // as CopyDst and copies this frame's rows into them -- the registry
        // scene's staged rows and (T7) the ad-hoc rows into this slot's
        // scratch region; the mesh node below Reads the same handles, so the
        // graph derives the copy -> read barriers.
        const GpuSceneNodeInputs gpuScene = AddGpuSceneSyncNode(graph, context, scene.scene, adHoc);
        AddMeshCullNode(graph, context, gpuScene, scene.scene);

        // `depth` is captured by reference ([&]) below, not shared_ptr -- safe
        // here only because the SETUP lambda is the one that mutates it and
        // AddNode runs setup synchronously (before this function returns), so
        // `depth` is still this frame's live stack local. The EXEC lambda below
        // captures nothing of it.
        RgTexture depth{};
        graph.AddNode("mesh", RenderGraph::NodeKind::Raster,
            [&](RenderGraphBuilder& builder)
            {
                RgTextureDesc desc;
                desc.format       = GraphDepthFormat();
                desc.width        = width;
                desc.height       = height;
                desc.depthStencil = true;
                desc.optimizedClearValue.depthStencil.depth = DepthClearValue(kSelectedDepthConvention);
                desc.hasOptimizedClearValue = true;
                depth = builder.CreateTexture("depth", desc);

                // BOTH targets, both written, both attached. The colour one
                // was minted by batch2d and already carries this frame's 2D
                // content -- this pass draws on top of it, which is why it is
                // a Write and not a Read.
                builder.Write(canvas, RgUsage::ColorWrite);
                builder.Write(depth, RgUsage::DepthWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(&canvas, 1));
                graph.SetDepthAttachment(depth);

                // Declarative load ops (F5 plan 1, Task 2): colour LOADs (it
                // carries batch2d's content), depth CLEARs to the one
                // depth-convention value. The depth target is a fresh transient
                // pool slot whose contents are undefined, so its clear is
                // mandatory rather than tidy.
                Arcane::RgAttachmentOp colorOp;   // load defaults to Load
                graph.SetColorAttachmentOps(std::span<const Arcane::RgAttachmentOp>(&colorOp, 1));
                Arcane::RgAttachmentOp depthOp;
                depthOp.load = Arcane::RgLoadOp::Clear;
                depthOp.clear.depthStencil.depth = Arcane::DepthClearValue(Arcane::kSelectedDepthConvention);
                depthOp.clear.depthStencil.stencil = 0;
                graph.SetDepthAttachmentOp(depthOp);

                // The GPU scene's buffers, as this pass consumes them: the
                // instance rows and the visible indices from the vertex
                // shader, the args by CmdDrawIndexedIndirect.
                builder.Read(gpuScene.instances, RgUsage::ShaderRead);
                builder.Read(gpuScene.visibleIndices, RgUsage::ShaderRead);
                builder.Read(gpuScene.args, RgUsage::IndirectArgs);
            },
            [context, scene, readiness = gpuScene.readiness](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive: no device, nothing to record
                if (MeshNode* node = context->Mesh(); node && readiness)
                    node->Record(nodeContext, scene, context->FrameSlot(), context->Scene(), *readiness);
            });

        // WHAT THE CULL PASS WROTE, read back WITHOUT A WAIT (F3 plan 2 T5),
        // and declared only when a host or a test armed the ring
        // (GpuScene::EnableVisibilityReadback): this slot's indirect args and
        // visible indices into a HOST_READBACK buffer, published once this
        // frame's fence has retired. Declared HERE, after the mesh pass, so the
        // copy reads the buffers in the state the draw left them -- the graph
        // derives the IndirectArgs/ShaderRead -> CopySrc transitions. A no-op on
        // every unarmed frame.
        AddGpuSceneVisibilityReadbackNode(graph, context, gpuScene, scene.scene);

        // TEST-ONLY, and declared only when a test armed it (GpuScene::
        // EnableDebugReadback): the instance buffer read back after this pass
        // consumed it. A no-op on every production frame.
        AddGpuSceneDebugReadbackNode(graph, context, gpuScene);
        return depth;
    }
}
