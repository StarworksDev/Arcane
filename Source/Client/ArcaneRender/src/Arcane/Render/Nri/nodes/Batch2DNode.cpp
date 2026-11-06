// Batch2DNode -- see the header for what this node owns, why the CPU batching
// is reused rather than reimplemented, and THE TEXTURE GAP that bounds Task 8.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>
#include <Extensions/NRIHelper.h>

#include "Batch2DNode.hpp"

#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/GlobalParams.hpp>   // kSpriteMaterialCbSlot / kSpriteGlobalCbSlot / kSpriteMaterialTextureBase
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Render/Batcher2D.hpp>        // Batch2DDrained / Batch2DVertex / Batch2DDrawSpan
#include <Arcane/Render/RenderFramePacket.hpp>   // the bound packet's copied batch (F5 plan 1 Task 5)
#include <Arcane/Render/RenderBudgetSettings.hpp>   // RenderBatch2dSettings -- the caps the constructor latches
#include <Arcane/Render/RenderLookSettings.hpp>     // render.clearColor, render.sprite.filter
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/ShaderConventions.hpp>  // kVsEntry / kPsEntry

#undef ERROR

#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace Arcane
{
    namespace
    {
        void GraphError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        // The engine's canvas clear is render.clearColor (RenderSettings, Live):
        // DeclareClearedCanvas reads it ONCE per frame and the same value is both
        // the canvas's optimized clear value and its CLEAR colour load op, so the
        // two can never disagree (D3D12 warns on a clear that differs from the
        // resource's). DeclareClearedCanvas is the only place the canvas clear is
        // declared -- batch2d on a world frame, letterbox on a chrome frame both
        // call it -- so there is exactly one background colour in the process.
        //
        // The canvas format is GraphCanvasFormat() (NriGraphContext.hpp,
        // render.canvasFormat latched once per process), because the
        // post-chain targets must be the SAME format as the canvas.

        // data/shaders/sprite.hlsl's BatchConstants: float4x4 viewProj +
        // float2 invHalfViewport + uint worldSpace + float pad = 80 bytes (b0
        // on D3D12, [[vk::push_constant]] on SPIR-V, which is what NRI's
        // RootConstantDesc lowers to on each backend; 80 <= Vulkan's 128-byte
        // push-constant minimum). `viewProj` is the glm::mat4 memcpy'd in --
        // column-major on both sides, no transpose (sprite.hlsl's packing
        // note). `worldSpace` is PER SPAN and is why the block is re-issued
        // inside the span loop (Record) and not once per frame.
        struct BatchRootConstants
        {
            float         viewProj[16] = { 1.0f, 0.0f, 0.0f, 0.0f,
                                           0.0f, 1.0f, 0.0f, 0.0f,
                                           0.0f, 0.0f, 1.0f, 0.0f,
                                           0.0f, 0.0f, 0.0f, 1.0f };
            float         invHalfViewportX = 0.0f;
            float         invHalfViewportY = 0.0f;
            std::uint32_t worldSpace = 0u;
            float         pad = 0.0f;
        };
        static_assert(sizeof(BatchRootConstants) == 80, "must match sprite.hlsl's BatchConstants");
        static_assert(sizeof(glm::mat4) == sizeof(BatchRootConstants::viewProj),
                      "viewProj is a glm::mat4 memcpy");

        // The upload-ring alignment for the vertex stream. Batch2DVertex is 36
        // bytes since its pos became vec3 (F4 plan 1, T4), and the ring
        // ARC_ASSERTs a power-of-two alignment -- so this is CHOSEN, the way
        // ImGuiNri.cpp chooses 16 for the 20-byte ImDrawVert, not
        // sizeof(Batch2DVertex). Neither backend requires a vertex-buffer bind
        // offset to be a multiple of the stride; each attribute only has to
        // land on its natural boundary, which 16 guarantees for a stride that
        // is a multiple of 4.
        ARC_CONSTANT("hardware alignment: vertex upload alignment")
        constexpr std::uint64_t kVertexAlign = 16;
        static_assert((kVertexAlign & (kVertexAlign - 1)) == 0, "the ring wants a power of two");
        static_assert(sizeof(Batch2DVertex) % 4 == 0, "every attribute is float-aligned");

        // The built-in shader pairs, in Batcher2D::kMaterial* order -- the
        // SAME offline bins Batcher2D's material table names by string
        // (Batcher2D.cpp's InitMaterialTable). This node is their only
        // loader.
        constexpr const char* kBuiltInVs[] = { "sprite_vs", "circle_vs", "msdf_vs" };
        constexpr const char* kBuiltInPs[] = { "sprite_ps", "circle_ps", "msdf_ps" };

        // FNV-1a. Used for two different jobs below and the difference matters:
        //   * over the BYTECODE it produces NriPipelineCache::GraphicsKey::
        //     shaderPairId, which that cache's fill contract (rule 3) requires
        //     to change whenever the bytecode would -- a content hash satisfies
        //     that by construction, where a material id would not;
        //   * over the registration's identity fields it produces the stamp
        //     that decides whether a built material slot is still current.
        ARC_CONSTANT("math: FNV-1a hash parameters")
        constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
        ARC_CONSTANT("math: FNV-1a hash parameters")
        constexpr std::uint64_t kFnvPrime  = 1099511628211ull;

        std::uint64_t HashBytes(const void* data, std::size_t size, std::uint64_t seed = kFnvOffset) noexcept
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            std::uint64_t hash = seed;
            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= kFnvPrime;
            }
            return hash;
        }

        std::uint64_t HashValue(std::uint64_t value, std::uint64_t seed) noexcept
        {
            return HashBytes(&value, sizeof(value), seed);
        }
    }

    Batch2DNode::Caps Batch2DNode::CapsFrom(const RenderBatch2dSettings& settings,
                                            std::uint32_t framesInFlight) noexcept
    {
        Caps caps;
        caps.materialSlots    = settings.maxMaterialSlots;
        caps.materialTextures = settings.maxMaterialTextures;
        caps.spriteTextures   = settings.maxSpriteTextures;
        caps.materialCbBytes  = MaterialCbRegionBytes(settings.materialCbBytes);

        // THE SAMPLER CEILING (kMaxPoolSamplers). PoolSizes() spends one
        // sampler per set: per sprite-texture variant, the built-in set plus
        // one material set per (material slot, frame slot). So the variants
        // that fit are kMaxPoolSamplers / that, and spriteTextures is one less
        // (variant 0 is the nil texture). 64-bit, so no product can wrap.
        const std::uint64_t perVariant =
            1ull + (std::uint64_t)caps.materialSlots * framesInFlight;
        if ((1ull + caps.spriteTextures) * perVariant > kMaxPoolSamplers)
        {
            const std::uint64_t variants = kMaxPoolSamplers / perVariant;
            caps.spriteTextures = variants > 0 ? (std::uint32_t)(variants - 1) : 0u;
        }
        return caps;
    }

    // THE LATCH (Restart): every pool, arena and cap this node creates is
    // sized from m_caps, so a render.batch2d.* value published after this
    // point waits for the next node -- the next vehicle, i.e. a restart.
    Batch2DNode::Batch2DNode()
    {
        const RenderBatch2dSettings& settings = Settings<RenderBatch2dSettings>();
        const std::uint32_t framesInFlight = FramesInFlight();
        m_caps = CapsFrom(settings, framesInFlight);
        if (m_caps.materialCbBytes != settings.materialCbBytes)
            ARC_WARN("[nri-graph] Batch2DNode: render.batch2d.materialCbBytes {} is not a multiple of 256 -- "
                     "using {}", settings.materialCbBytes, m_caps.materialCbBytes);
        if (m_caps.spriteTextures != settings.maxSpriteTextures)
            ARC_WARN("[nri-graph] Batch2DNode: render.batch2d.maxSpriteTextures {} would need {} samplers "
                     "(render.batch2d.maxMaterialSlots {} at {} frames in flight), over the {}-descriptor "
                     "ceiling of a D3D12 shader-visible sampler heap -- clamped to {}",
                     settings.maxSpriteTextures,
                     (1ull + settings.maxSpriteTextures) * (1ull + (std::uint64_t)m_caps.materialSlots * framesInFlight),
                     m_caps.materialSlots, framesInFlight, kMaxPoolSamplers, m_caps.spriteTextures);
    }

    std::unique_ptr<Batch2DNode> Batch2DNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<Batch2DNode> node(new Batch2DNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool Batch2DNode::Init(NriGraphContext& context)
    {
        m_device       = &context.Device();
        m_pipelines    = &context.Pipelines();
        m_owner        = &context;
        m_textureCache = context.Textures();

        for (std::uint32_t i = 0; i < kBuiltInCount; ++i)
        {
            m_builtIns[i].vs = context.ShaderBytecode(kBuiltInVs[i]);
            m_builtIns[i].ps = context.ShaderBytecode(kBuiltInPs[i]);
            if (m_builtIns[i].vs.empty() || m_builtIns[i].ps.empty())
            {
                ARC_ERROR("[nri-graph] Batch2DNode: shader bin '{}'/'{}' is missing -- the batch node "
                          "cannot be built", kBuiltInVs[i], kBuiltInPs[i]);
                return false;
            }
        }

        // The vertex input, in members because GraphicsPipelineDesc::vertexInput
        // is a pointer the cache dereferences after `fill` returns. Layout is
        // Batch2DVertex, and the semantic names are sprite.hlsl's VSInput.
        m_attributes[0].d3d.semanticName = "POSITION";
        m_attributes[0].vk.location      = 0;
        m_attributes[0].offset           = offsetof(Batch2DVertex, pos);
        m_attributes[0].format           = nri::Format::RGB32_SFLOAT;   // vec3 since F4 plan 1 T4
        m_attributes[1].d3d.semanticName = "TEXCOORD";
        m_attributes[1].vk.location      = 1;
        m_attributes[1].offset           = offsetof(Batch2DVertex, uv);
        m_attributes[1].format           = nri::Format::RG32_SFLOAT;
        m_attributes[2].d3d.semanticName = "COLOR";
        m_attributes[2].vk.location      = 2;
        m_attributes[2].offset           = offsetof(Batch2DVertex, color);
        m_attributes[2].format           = nri::Format::RGBA32_SFLOAT;

        m_stream.bindingSlot = 0;
        m_stream.stepRate    = nri::VertexStreamStepRate::PER_VERTEX;
        m_stream.stride      = (std::uint16_t)sizeof(Batch2DVertex);

        m_vertexInput.attributes   = m_attributes;
        m_vertexInput.attributeNum = (std::uint8_t)std::size(m_attributes);
        m_vertexInput.streams      = &m_stream;
        m_vertexInput.streamNum    = 1;

        return CreateWhiteTexel() && CreateBindings() && CreateConstantArena();
    }

    bool Batch2DNode::CreateWhiteTexel()
    {
        const nri::CoreInterface& core = m_device->Core();

        nri::TextureDesc textureDesc = {};
        textureDesc.type      = nri::TextureType::TEXTURE_2D;
        textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
        textureDesc.format    = nri::Format::RGBA8_UNORM;
        textureDesc.width     = 1;
        textureDesc.height    = 1;
        textureDesc.depth     = 1;
        textureDesc.mipNum    = 1;
        textureDesc.layerNum  = 1;
        textureDesc.sampleNum = 1;
        if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                        0.0f, textureDesc, m_white))
            || !m_white)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the 1x1 white texel could not be created");
            return false;
        }
        core.SetDebugName(m_white, "nri-graph batch white");

        // Populated through NRI's OWN helper rather than a hand-rolled staging
        // buffer + transition pair. Deliberate: the phase's rule is that graph
        // code records no CmdBarrier of its own, and while a one-shot
        // initialisation upload is not a graph node (there is no derived
        // barrier chain for it to fight), the helper means this file contains
        // no barrier at all -- so the rule reads the same from the outside as
        // it does from the inside. UploadData submits and waits internally,
        // which is what "once, at Create()" wants anyway.
        nri::HelperInterface helper = {};
        if (!ARC_NRI_CHECK(nriGetInterface(m_device->Device(), NRI_INTERFACE(nri::HelperInterface), &helper)))
        {
            ARC_ERROR("[nri-graph] Batch2DNode: HelperInterface unavailable -- cannot upload the white texel");
            return false;
        }

        const std::uint32_t whitePixel = 0xFFFFFFFFu;
        nri::TextureSubresourceUploadDesc subresource = {};
        subresource.slices     = &whitePixel;
        subresource.sliceNum   = 1;
        subresource.rowPitch   = 4;
        subresource.slicePitch = 4;

        nri::TextureUploadDesc upload = {};
        upload.subresources = &subresource;
        upload.texture      = m_white;
        upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                nri::StageBits::FRAGMENT_SHADER };
        upload.planes       = nri::PlaneBits::ALL;
        if (!ARC_NRI_CHECK(helper.UploadData(*m_device->GraphicsQueue(), &upload, 1, nullptr, 0)))
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the white texel upload failed");
            return false;
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = m_white;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, m_whiteView)) || !m_whiteView)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the white texel's shader-resource view could not be created");
            return false;
        }
        return true;
    }

    nri::DescriptorPoolDesc Batch2DNode::PoolSizes(const Caps& caps, std::uint32_t framesInFlight) noexcept
    {
        // THREE set families since Task 2 (the middle one is new):
        //   * the ONE built-in nil-texture set (the white texel);
        //   * one built-in set per distinct SPRITE TEXTURE, capped at
        //     caps.spriteTextures -- no frame-slot dimension, because a
        //     built-in set's contents (t0 + s0) carry nothing per-frame, so it
        //     is written once and never rewritten;
        //   * per registered material: one set per (texture variant, frame
        //     slot), where variant 0 is the nil-texture one Task 9 had. Hence
        //     the (1 + caps.spriteTextures) factor.
        const std::uint32_t builtInSets  = 1 + caps.spriteTextures;
        const std::uint32_t materialSets = caps.materialSlots * framesInFlight * (1 + caps.spriteTextures);

        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum  = builtInSets + materialSets;
        // Per material set: the sprite's t0 plus its declared t1..N. Per
        // built-in set: t0 alone.
        poolDesc.textureMaxNum        = builtInSets + (1 + caps.materialTextures) * materialSets;
        poolDesc.samplerMaxNum        = builtInSets + materialSets;
        // Per material set: material CB b1 (when the template has numeric
        // params) and globals CB b2. A built-in set has neither.
        poolDesc.constantBufferMaxNum = 2 * materialSets;
        return poolDesc;
    }

    bool Batch2DNode::CreateBindings()
    {
        const nri::CoreInterface& core = m_device->Core();

        // render.sprite.filter (Restart, read here, once) + clamp: Linear
        // scales sprites smoothly, Point keeps pixel art's hard texel edges,
        // and a clamped atlas edge does not bleed.
        const nri::Filter filter = Settings<RenderSpriteSettings>().filter == SamplerFilter::Point
                                       ? nri::Filter::NEAREST : nri::Filter::LINEAR;
        nri::SamplerDesc samplerDesc = {};
        samplerDesc.filters.min   = filter;
        samplerDesc.filters.mag   = filter;
        samplerDesc.filters.mip   = filter;
        samplerDesc.addressModes  = { nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE,
                                      nri::AddressMode::CLAMP_TO_EDGE };
        samplerDesc.mipMax        = 16.0f;
        if (!ARC_NRI_CHECK(core.CreateSampler(m_device->Device(), samplerDesc, m_sampler)) || !m_sampler)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: sampler creation failed");
            return false;
        }

        // THE LAYOUT. sprite/circle/msdf all declare the same register map:
        // b0 constants, t0 texture, s0 sampler (data/shaders/sprite.hlsl).
        //
        // Root constants live in `rootRegisterSpace` (0 -> b0, space0 on
        // D3D12; a VK push-constant block, which has no space at all), and the
        // texture/sampler set is registerSpace 0 to match the shaders' implicit
        // space0. Those two CAN share space 0: NRI only refuses a collision
        // between rootRegisterSpace and a descriptor set when the layout also
        // carries root DESCRIPTORS or root SAMPLERS (Validation/DeviceVal.hpp),
        // which is exactly why the sampler here is a descriptor-set entry and
        // not a root (static) sampler -- a root sampler would claim space 0 and
        // push the texture set into a space the shaders do not name.
        //
        // On Vulkan the t0/s0 register indices are shifted by NRI itself using
        // the device's vkBindingOffsets (t=0, s=128), which NriDevice pins to
        // ShaderConventions.hpp's dxc -fvk-*-shift values with static_asserts.
        // So this desc is written in HLSL register numbers on both backends.
        nri::RootConstantDesc rootConstant = {};
        rootConstant.registerIndex = 0;
        rootConstant.size          = sizeof(BatchRootConstants);
        rootConstant.shaderStages  = nri::StageBits::VERTEX_SHADER;

        nri::DescriptorRangeDesc ranges[2] = {};
        ranges[0].baseRegisterIndex = 0;
        ranges[0].descriptorNum     = 1;
        ranges[0].descriptorType    = nri::DescriptorType::TEXTURE;
        ranges[0].shaderStages      = nri::StageBits::FRAGMENT_SHADER;
        ranges[1].baseRegisterIndex = 0;
        ranges[1].descriptorNum     = 1;
        ranges[1].descriptorType    = nri::DescriptorType::SAMPLER;
        ranges[1].shaderStages      = nri::StageBits::FRAGMENT_SHADER;

        nri::DescriptorSetDesc setDesc = {};
        setDesc.registerSpace = 0;
        setDesc.ranges        = ranges;
        setDesc.rangeNum      = 2;

        // Value-initialized then assigned field by field: NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so its padding has to
        // be zeroed).
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.rootConstants     = &rootConstant;
        layoutDesc.rootConstantNum   = 1;
        layoutDesc.descriptorSets    = &setDesc;
        layoutDesc.descriptorSetNum  = 1;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: pipeline layout registration failed");
            return false;
        }

        // ONE pool for the built-in set AND every registered material's sets.
        //
        // NOTHING IN IT IS EVER REWRITTEN WHILE THE GPU MIGHT READ IT, which is
        // what keeps ResetDescriptorPool (and its frame-slot fence discipline)
        // out of this file entirely. The built-in set binds the white texel and
        // is written once. A material's sets are per FRAME SLOT, so the set a
        // frame binds is the one whose constant-buffer region that same frame
        // owns -- and the only path that rewrites an existing set is a material
        // REBUILD (an asset re-save at the desk), which idles the device first
        // and says so. NRI's own guidance: "DescriptorSet is a tiny struct, so
        // lots of descriptor sets can be created in advance and reused without
        // calling ResetDescriptorPool" (NRI.h).
        //
        // Capacity is the hard cap m_caps names (render.batch2d.*): a pool's
        // sizes are fixed at creation and a single set cannot be freed, so the
        // numbers are decided up front rather than discovered mid-frame. The
        // arithmetic itself is PoolSizes() -- pure, public and device-lessly
        // pinned, because this device call is the only thing standing between
        // a wrong cap and a mid-frame allocation failure at the desk.
        const nri::DescriptorPoolDesc poolDesc = PoolSizes(m_caps);
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: descriptor pool creation failed");
            return false;
        }
        if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0, &m_set, 1, 0)) || !m_set)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: descriptor set allocation failed");
            return false;
        }

        const nri::Descriptor* texture = m_whiteView;
        const nri::Descriptor* sampler = m_sampler;
        nri::UpdateDescriptorRangeDesc updates[2] = {};
        updates[0].descriptorSet = m_set;
        updates[0].rangeIndex    = 0;
        updates[0].descriptors   = &texture;
        updates[0].descriptorNum = 1;
        updates[1].descriptorSet = m_set;
        updates[1].rangeIndex    = 1;
        updates[1].descriptors   = &sampler;
        updates[1].descriptorNum = 1;
        core.UpdateDescriptorRanges(updates, 2);
        return true;
    }

    void SpriteMaterialLayout::Build(std::uint32_t cbSize, std::uint32_t textureCount)
    {
        // sprite_material.hlsl's register map: root constants b0, and ONE
        // space-0 descriptor set carrying material CB b1 (only when the template
        // has numeric params -- the stitcher omits the cbuffer otherwise),
        // globals CB b2, the sprite's own texture t0, the declared textures
        // t1.., and the sampler s0.
        //
        // RANGE ORDER IS DELIBERATE. NRI's D3D12 backend merges CONSECUTIVE
        // ranges of the same D3D12 range type into one root table
        // (Source/D3D12/PipelineLayoutD3D12.hpp), so grouping the two CBVs, then
        // the SRVs, then the sampler costs three root parameters instead of
        // five -- and t0 with t1..N is ONE contiguous SRV range for the same
        // reason.
        rootConstant = {};
        rootConstant.registerIndex = 0;
        rootConstant.size          = sizeof(BatchRootConstants);   // the 80-byte b0 every 2D pipeline shares
        rootConstant.shaderStages  = nri::StageBits::VERTEX_SHADER;

        // Both stages: a material template's VERTEX_BODY may sample its declared
        // textures and read its params (sprite_material.hlsl's displace()), so
        // narrowing these to FRAGMENT would break a displacing material on
        // D3D12, where visibility is a hard root-signature property.
        constexpr nri::StageBits kBothStages =
            nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        for (nri::DescriptorRangeDesc& range : ranges)
            range = {};
        materialCb = globalsCb = textures = sampler = kNoRange;

        std::uint32_t rangeCount = 0;
        if (cbSize > 0)
        {
            materialCb = rangeCount;
            ranges[rangeCount].baseRegisterIndex = kSpriteMaterialCbSlot;   // b1
            ranges[rangeCount].descriptorNum     = 1;
            ranges[rangeCount].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
            ranges[rangeCount].shaderStages      = kBothStages;
            ++rangeCount;
        }
        globalsCb = rangeCount;
        ranges[rangeCount].baseRegisterIndex = kSpriteGlobalCbSlot;         // b2
        ranges[rangeCount].descriptorNum     = 1;
        ranges[rangeCount].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        ranges[rangeCount].shaderStages      = kBothStages;
        ++rangeCount;

        textures = rangeCount;
        // t0 is the sprite's own texture; the declared params follow at
        // kSpriteMaterialTextureBase (t1), which is why ONE range starting at 0
        // covers both.
        static_assert(kSpriteMaterialTextureBase == 1,
                      "the sprite's own texture is t0 and declared params follow it contiguously");
        ranges[rangeCount].baseRegisterIndex = 0;
        ranges[rangeCount].descriptorNum     = 1 + textureCount;
        ranges[rangeCount].descriptorType    = nri::DescriptorType::TEXTURE;
        ranges[rangeCount].shaderStages      = kBothStages;
        ++rangeCount;

        sampler = rangeCount;
        ranges[rangeCount].baseRegisterIndex = 0;                           // s0
        ranges[rangeCount].descriptorNum     = 1;
        ranges[rangeCount].descriptorType    = nri::DescriptorType::SAMPLER;
        ranges[rangeCount].shaderStages      = kBothStages;
        ++rangeCount;

        set = {};
        set.registerSpace = 0;
        set.ranges        = ranges;
        set.rangeNum      = rangeCount;

        // Value-initialized then assigned field by field: NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so its padding has to
        // be zeroed). Two materials with the same (cbSize>0, textureCount) shape
        // therefore SHARE one layout id, which is the point.
        //
        // rootDescriptorNum and rootSamplerNum stay ZERO, and that is THE
        // REGISTER-SPACE RULE in force: NRI refuses rootRegisterSpace == a set's
        // registerSpace only when one of those two is nonzero
        // (Source/Validation/DeviceVal.hpp), and the shaders leave us no choice
        // but to put both in space 0.
        desc = {};
        desc.rootRegisterSpace = 0;
        desc.rootConstants     = &rootConstant;
        desc.rootConstantNum   = 1;
        desc.descriptorSets    = &set;
        desc.descriptorSetNum  = 1;
        desc.shaderStages      = kBothStages;
    }

    bool Batch2DNode::CreateConstantArena()
    {
        const nri::CoreInterface& core = m_device->Core();
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());

        // The CALLER supplies constant-buffer alignment everywhere on this path
        // (NriUploadRing::Allocate has no default either), and this is where it
        // comes from. It also fixes the SIZE every CB view is created with:
        // NRI passes BufferViewDesc::size straight into
        // D3D12_CONSTANT_BUFFER_VIEW_DESC::SizeInBytes, which D3D12 requires to
        // be a multiple of 256 -- so the views name a whole region, not the
        // template's byte count, and the shader simply reads less than it.
        m_arenaStride = CbRegionStride(m_caps.materialCbBytes, deviceDesc.memoryAlignment.constantBufferOffset);

        // RESERVED, not merely sized: EnsureMaterial hands Prepare a
        // MaterialSlot* out of this vector and then keeps building, so a
        // reallocation mid-frame would dangle it. The cap is the reservation, so
        // the vector never grows past it.
        m_materials.reserve(m_caps.materialSlots);

        const std::uint64_t regionsPerFrame = CbRegionsPerFrame(m_caps);
        const std::uint64_t arenaBytes = regionsPerFrame * FramesInFlight() * m_arenaStride;

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = arenaBytes;
        bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                       0.0f, bufferDesc, m_arena))
            || !m_arena)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the material constant-buffer arena ({} bytes) could "
                      "not be created", arenaBytes);
            return false;
        }
        core.SetDebugName(m_arena, "nri-graph batch material CBs");

        // Persistent map, unmapped once in Release()/~Batch2DNode -- the same
        // shape NriUploadRing uses, and the same NONE-backend footgun: MapBuffer
        // returns null unconditionally there, so this node is [gpu]-only from
        // here down.
        m_arenaCpu = core.MapBuffer(*m_arena, 0, nri::WHOLE_SIZE);
        if (!m_arenaCpu)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the material constant-buffer arena could not be "
                      "mapped (the NONE backend cannot -- this node is a [gpu] path)");
            return false;
        }

        // The globals CB view per frame slot, created ONCE. Its contents change
        // every frame; its (buffer, offset) never does, which is exactly what
        // lets a descriptor set naming it be written once too.
        for (std::uint32_t slot = 0; slot < FramesInFlight(); ++slot)
        {
            nri::BufferViewDesc viewDesc = {};
            viewDesc.buffer = m_arena;
            viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
            viewDesc.offset = ArenaOffset(slot, 0);
            viewDesc.size   = m_arenaStride;
            if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, m_globalsView[slot]))
                || !m_globalsView[slot])
            {
                ARC_ERROR("[nri-graph] Batch2DNode: the globals constant-buffer view for frame slot "
                          "{} could not be created", slot);
                return false;
            }
        }
        return true;
    }

    Batch2DNode::~Batch2DNode()
    {
        if (!m_device || (!m_white && !m_whiteView && !m_sampler && !m_pool && !m_arena))
            return;

        ARC_WARN("[nri-graph] Batch2DNode destroyed with live NRI objects -- either Create() failed "
                 "part way (an ERROR above says which step) or its owner never called Release(). "
                 "Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        if (m_pool)      core.DestroyDescriptorPool(m_pool);
        for (MaterialSlot& slot : m_materials)
            for (nri::Descriptor*& view : slot.cbView)
                if (view) { core.DestroyDescriptor(view); view = nullptr; }
        for (nri::Descriptor*& view : m_globalsView)
            if (view) { core.DestroyDescriptor(view); view = nullptr; }
        if (m_arena)
        {
            if (m_arenaCpu) core.UnmapBuffer(*m_arena);
            core.DestroyBuffer(m_arena);
        }
        // The sprite-texture sets are owned by the pool destroyed above, and
        // the TEXTURES they view belong to the vehicle's shared
        // NriTextureCache, which releases its own -- this node owns neither.
        m_spriteSets.clear();
        m_materials.clear();
        m_materialSlotOf.clear();
        if (m_sampler)   core.DestroyDescriptor(m_sampler);
        if (m_whiteView) core.DestroyDescriptor(m_whiteView);
        if (m_white)     core.DestroyTexture(m_white);
        m_pool = nullptr; m_set = nullptr;
        m_arena = nullptr; m_arenaCpu = nullptr;
        m_sampler = nullptr; m_whiteView = nullptr; m_white = nullptr;
    }

    void Batch2DNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view can never outlive its texture (or, for
        // the constant buffers below, its arena).
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
            m_set  = nullptr;   // owned by the pool; not separately destroyable
        }

        // The material half: CB views, then the arena they name; texture views,
        // then the textures they view.
        for (MaterialSlot& slot : m_materials)
        {
            for (nri::Descriptor*& view : slot.cbView)
            {
                if (!view)
                    continue;
                graveyard.Bury(fence, [core, d = view] { core->DestroyDescriptor(d); });
                view = nullptr;
            }
            slot.variants.clear();   // sets are owned by the pool, buried above
        }
        m_materials.clear();
        m_materialSlotOf.clear();
        m_materialRefused.clear();
        // Owned by the pool buried above; the images behind them belong to the
        // vehicle's shared NriTextureCache, which the vehicle releases itself.
        m_spriteSets.clear();

        for (nri::Descriptor*& view : m_globalsView)
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

        if (m_sampler)
        {
            graveyard.Bury(fence, [core, d = m_sampler] { core->DestroyDescriptor(d); });
            m_sampler = nullptr;
        }
        if (m_whiteView)
        {
            graveyard.Bury(fence, [core, d = m_whiteView] { core->DestroyDescriptor(d); });
            m_whiteView = nullptr;
        }
        if (m_white)
        {
            graveyard.Bury(fence, [core, t = m_white] { core->DestroyTexture(t); });
            m_white = nullptr;
        }
    }

    nri::Pipeline* Batch2DNode::PipelineFor(std::uint16_t material, nri::Format canvasFormat)
    {
        std::uint32_t builtIn = material;
        if (builtIn >= kBuiltInCount)
        {
            // A REGISTERED material that Prepare could not build (or
            // was never offered -- a caller that skipped Prepare). Falling back
            // to the plain sprite pipeline draws the right geometry with the
            // wrong shader rather than dropping the content, which is the same
            // degradation Batcher2D::QuadMaterial already applies to an unknown
            // id. The reason it could not be built was reported there.
            if (!m_warnedRegisteredMaterial)
            {
                m_warnedRegisteredMaterial = true;
                ARC_WARN("[nri-graph] Batch2DNode: material id {} has no NRI pipeline -- the graph "
                         "path draws it through the plain sprite pipeline instead", material);
            }
            builtIn = 0;
        }

        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kShaderPairBase + builtIn;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = canvasFormat;
        key.colorCount      = 1;
        key.depthFormat     = nri::Format::UNKNOWN;
        key.topology        = nri::Topology::TRIANGLE_LIST;
        // Straight (non-premultiplied) alpha: SrcAlpha/InvSrcAlpha colour,
        // One/InvSrcAlpha alpha.
        key.blend           = NriPipelineCache::GraphicsKey::Blend::AlphaOver;
        key.depthWrite      = false;
        key.cullMode        = nri::CullMode::NONE;

        // `stages` lives in THIS frame, which encloses the GetGraphics call --
        // the fill contract's rule 2. The bytecode it points at is owned by the
        // vehicle's bin cache and outlives this node.
        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_builtIns[builtIn].vs.data();
        stages[0].size           = m_builtIns[builtIn].vs.size();
        stages[0].entryPointName = kVsEntry;   // SPIR-V matches by name; DXIL ignores it
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_builtIns[builtIn].ps.data();
        stages[1].size           = m_builtIns[builtIn].ps.size();
        stages[1].entryPointName = kPsEntry;

        return m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            desc.vertexInput = &m_vertexInput;
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            // Cull NONE + no depth test: the 2D batch is painter-ordered by its
            // own sort key, and Triangle()/Line() deliberately emit whatever
            // winding the caller's points imply (Batcher2D.hpp).
            desc.rasterization.fillMode = nri::FillMode::SOLID;
        });
    }

    // =====================================================================
    // REGISTERED MATERIALS (Task 9)
    // =====================================================================

    std::uint32_t Batch2DNode::DistinctTextureCount(std::span<const Batch2DDrawSpan> spans)
    {
        // Small by construction (a frame's distinct sprite textures), so a
        // flat scan beats a set allocation -- and this runs once per frame,
        // never per span.
        std::vector<Guid> seen;
        for (const Batch2DDrawSpan& span : spans)
        {
            if (!span.textureId.IsValid())
                continue;
            bool known = false;
            for (const Guid& id : seen)
                known = known || id == span.textureId;
            if (!known)
                seen.push_back(span.textureId);
        }
        return (std::uint32_t)seen.size();
    }

    nri::Descriptor* Batch2DNode::TextureView(const Guid& id)
    {
        // The whole of this node's image residency since Task 2: ONE shared
        // cache on the vehicle, so a sprite's own texture and a material's
        // declared param naming the same asset are one upload. The miss warn
        // lives there too -- it is where the miss happens.
        if (!m_textureCache || !m_textureCache->Resolve(id))
            return nullptr;
        return m_textureCache->View(id);
    }

    nri::DescriptorSet* Batch2DNode::EnsureSpriteSet(const Guid& id)
    {
        // A nil id is the untextured case -- Task 8's single white-texel set,
        // unchanged and still written once at Create.
        if (!id.IsValid())
            return m_set;

        const auto known = m_spriteSets.find(id);
        if (known != m_spriteSets.end())
            return known->second ? known->second : m_set;

        nri::Descriptor* view = TextureView(id);
        if (!view)
        {
            // Not resident (NriTextureCache said why, once). Remembered as a
            // null set so this does not re-ask the cache every frame.
            m_spriteSets[id] = nullptr;
            return m_set;
        }

        // Counts LIVE sets, not map entries: a memoized miss holds no
        // descriptor set, and letting eight unresolvable images spend the
        // budget would refuse a ninth that actually resolved.
        std::size_t live = 0;
        for (const auto& [key, existing] : m_spriteSets)
            if (existing)
                ++live;
        if (live >= m_caps.spriteTextures)
        {
            if (!m_warnedTextureBudget)
            {
                m_warnedTextureBudget = true;
                ARC_ERROR("[nri-graph] Batch2DNode: more than {} distinct sprite textures -- the "
                          "rest draw with the white texel. Raise render.batch2d.maxSpriteTextures (it "
                          "sizes the descriptor pool).", m_caps.spriteTextures);
            }
            m_spriteSets[id] = nullptr;
            return m_set;
        }

        const nri::CoreInterface& core = m_device->Core();
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        nri::DescriptorSet* set = nullptr;
        if (!layout
            || !ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0, &set, 1, 0))
            || !set)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: descriptor-set allocation failed for sprite "
                      "texture {} -- it draws with the white texel", id.ToString());
            m_spriteSets[id] = nullptr;
            return m_set;
        }

        // Written ONCE, here, and never again -- the same discipline every set
        // in this node obeys (see the pool comment in CreateBindings), which is
        // what keeps ResetDescriptorPool and its fence rules out of this file.
        const nri::Descriptor* texture = view;
        const nri::Descriptor* sampler = m_sampler;
        nri::UpdateDescriptorRangeDesc updates[2] = {};
        updates[0].descriptorSet = set;
        updates[0].rangeIndex    = 0;
        updates[0].descriptors   = &texture;
        updates[0].descriptorNum = 1;
        updates[1].descriptorSet = set;
        updates[1].rangeIndex    = 1;
        updates[1].descriptors   = &sampler;
        updates[1].descriptorNum = 1;
        core.UpdateDescriptorRanges(updates, 2);

        m_spriteSets[id] = set;
        return set;
    }

    void Batch2DNode::WriteMaterialSet(const MaterialSlot& slot, nri::DescriptorSet& set,
                                       std::uint32_t frameSlot, nri::Descriptor* spriteView)
    {
        const nri::CoreInterface& core = m_device->Core();

        // Rebuilt rather than carried: SpriteMaterialLayout::Build is pure and
        // cheap, and deriving the range indices from the SAME (cbSize,
        // textureCount) the set was allocated against is what makes it
        // impossible for a variant to write a different shape than the
        // original set.
        SpriteMaterialLayout layout2D;
        layout2D.Build(slot.cbSize, slot.textureCount);

        // Declaration time, written once per set: a heap vector is fine here.
        std::vector<nri::Descriptor*> textures(1 + slot.textureCount, nullptr);
        textures[0] = spriteView ? spriteView : m_whiteView;   // t0: the sprite's own
        for (std::uint32_t t = 0; t < slot.textureCount; ++t)
            textures[1 + t] = slot.paramViews[t] ? slot.paramViews[t] : m_whiteView;

        // EVERY `descriptors` SOURCE BELOW MUST OUTLIVE THE
        // UpdateDescriptorRanges CALL AT THE BOTTOM OF THIS FUNCTION.
        // UpdateDescriptorRangeDesc::descriptors is a POINTER TO AN ARRAY of
        // descriptors (NRIDescs.h:1101), dereferenced inside the call -- so a
        // single descriptor is passed as the address of a variable, and that
        // variable has to still be alive when the call runs. Hence `cb` is
        // declared HERE, in the same scope as its siblings, rather than inside
        // the `cbSize > 0` block that fills it: taking &cb from a narrower
        // scope leaves updates[] holding a pointer into dead storage, and the
        // compiler is free to give that slot to `globals` -- which would
        // silently write the GLOBALS view into range b1 and produce a
        // well-formed descriptor NRI's validation cannot fault.
        nri::UpdateDescriptorRangeDesc updates[4] = {};
        std::uint32_t updateCount = 0;
        const nri::Descriptor* cb = nullptr;
        if (slot.cbSize > 0)
        {
            cb = slot.cbView[frameSlot];
            updates[updateCount].descriptorSet = &set;
            updates[updateCount].rangeIndex    = layout2D.materialCb;
            updates[updateCount].descriptors   = &cb;
            updates[updateCount].descriptorNum = 1;
            ++updateCount;
        }
        const nri::Descriptor* globals = m_globalsView[frameSlot];
        updates[updateCount].descriptorSet = &set;
        updates[updateCount].rangeIndex    = layout2D.globalsCb;
        updates[updateCount].descriptors   = &globals;
        updates[updateCount].descriptorNum = 1;
        ++updateCount;

        const nri::Descriptor* const* textureArray = textures.data();
        updates[updateCount].descriptorSet = &set;
        updates[updateCount].rangeIndex    = layout2D.textures;
        updates[updateCount].descriptors   = textureArray;
        updates[updateCount].descriptorNum = 1 + slot.textureCount;
        ++updateCount;

        const nri::Descriptor* sampler = m_sampler;
        updates[updateCount].descriptorSet = &set;
        updates[updateCount].rangeIndex    = layout2D.sampler;
        updates[updateCount].descriptors   = &sampler;
        updates[updateCount].descriptorNum = 1;
        ++updateCount;

        core.UpdateDescriptorRanges(updates, updateCount);
    }

    Batch2DNode::MaterialSlot::TextureVariant*
    Batch2DNode::EnsureMaterialVariant(MaterialSlot& slot, const Guid& id)
    {
        // The NIL variant is always variants[0] (BuildMaterial writes it), and
        // it is the fallback for everything this function refuses.
        const auto fallback = [&slot]() -> MaterialSlot::TextureVariant*
        {
            return slot.variants.empty() ? nullptr : &slot.variants.front();
        };

        for (MaterialSlot::TextureVariant& variant : slot.variants)
            if (variant.id == id)
                return &variant;
        if (!id.IsValid())
            return fallback();

        nri::Descriptor* spriteView = TextureView(id);
        if (!spriteView)
            return fallback();

        // variants[0] is the nil one and is not a texture, hence the `>`.
        if (slot.variants.size() > m_caps.spriteTextures)
        {
            if (!m_warnedTextureBudget)
            {
                m_warnedTextureBudget = true;
                ARC_ERROR("[nri-graph] Batch2DNode: a material binds more than {} distinct sprite "
                          "textures -- the rest draw with the white texel. Raise "
                          "render.batch2d.maxSpriteTextures (it sizes the descriptor pool).",
                          m_caps.spriteTextures);
            }
            return fallback();
        }

        const nri::CoreInterface& core = m_device->Core();
        nri::PipelineLayout* layout = m_pipelines->Layout(slot.layoutId);
        if (!layout)
            return fallback();

        MaterialSlot::TextureVariant fresh;
        fresh.id = id;
        const std::uint32_t framesInFlight = FramesInFlight();
        for (std::uint32_t frameSlot = 0; frameSlot < framesInFlight; ++frameSlot)
        {
            if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0,
                                                            &fresh.set[frameSlot], 1, 0))
                || !fresh.set[frameSlot])
            {
                ARC_ERROR("[nri-graph] Batch2DNode: descriptor-set allocation failed for a "
                          "material's sprite-texture variant -- the pool holds {} material sets, "
                          "sized by render.batch2d.maxMaterialSlots x maxSpriteTextures",
                          m_caps.materialSlots * framesInFlight * (1 + m_caps.spriteTextures));
                return fallback();
            }
            WriteMaterialSet(slot, *fresh.set[frameSlot], frameSlot, spriteView);
        }

        slot.variants.push_back(fresh);
        return &slot.variants.back();
    }

    bool Batch2DNode::BuildMaterial(MaterialSlot& slot, const Material2DDesc& desc)
    {
        const nri::CoreInterface& core = m_device->Core();
        const std::uint32_t textureCount = desc.templ->TextureCount();
        const std::uint32_t cbSize       = desc.templ->CbSize();

        if (textureCount > m_caps.materialTextures)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: material '{}' declares {} textures, over this node's "
                      "cap of {} -- raise render.batch2d.maxMaterialTextures (it sizes the descriptor "
                      "pool)", desc.templ->Name(), textureCount, m_caps.materialTextures);
            return false;
        }
        if (cbSize > m_caps.materialCbBytes)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: material '{}' has a {}-byte constant buffer, over "
                      "this node's arena region of {} -- raise render.batch2d.materialCbBytes",
                      desc.templ->Name(), cbSize, m_caps.materialCbBytes);
            return false;
        }

        // THE LAYOUT -- see SpriteMaterialLayout (and THE REGISTER-SPACE RULE in
        // this file's header) for the whole of its shape and its reasoning. It
        // is a separate object so the device-less [nri] cases can assert that shape
        // without a device.
        SpriteMaterialLayout layout2D;
        layout2D.Build(cbSize, textureCount);
        static_assert(sizeof(BatchRootConstants) == 80,
                      "SpriteMaterialLayout::Build sizes its b0 block from BatchRootConstants -- "
                      "sprite_material.hlsl's BatchConstants must match");

        const std::uint32_t previousLayout = slot.layoutId;
        slot.layoutId = m_pipelines->RegisterLayout(layout2D.desc);
        nri::PipelineLayout* layout = m_pipelines->Layout(slot.layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: pipeline-layout registration failed for material '{}'",
                      desc.templ->Name());
            return false;
        }
        // A REBUILD whose SHAPE changed (the re-saved snippet gained or lost a
        // param or a texture decl). A descriptor set is allocated against one
        // layout's set-0 shape, so the existing sets no longer fit and must be
        // replaced -- and NRI cannot free the old ones, so they are abandoned
        // inside the pool until Release(). Rare (a structural edit at the desk)
        // and self-limiting: a run that does it often enough exhausts the pool
        // and gets the loud allocation failure below, which names the constant.
        if (previousLayout != NriPipelineCache::kInvalidLayout && previousLayout != slot.layoutId)
        {
            ARC_INFO("[nri-graph] Batch2DNode: material '{}' changed binding SHAPE -- its descriptor "
                     "sets are re-allocated (the previous ones are stranded in the pool until "
                     "shutdown)", desc.templ->Name());
            slot.variants.clear();   // every variant's sets were the OLD shape
        }

        // The PSO key. shaderPairId is the CONTENT hash of the blob pair, which
        // is what the cache's fill contract (rule 3) needs: a recompile of the
        // same material id produces different bytes, therefore a different key,
        // therefore a new pipeline -- where an id-derived value would have
        // served the stale one.
        slot.vs = desc.vsBytes;
        slot.ps = desc.psBytes;
        slot.shaderPairId = HashBytes(slot.ps->data(), slot.ps->size(),
                                      HashBytes(slot.vs->data(), slot.vs->size()));
        // Keeps registered materials clear of the small literal bases the
        // built-ins and TonemapNode use in this shared cache.
        slot.shaderPairId |= 0x8000000000000000ull;

        slot.cbSize = cbSize;
        slot.packed.assign(cbSize, 0);

        // ---------------------------------------------------------------
        // The per-frame-slot constant-buffer views and descriptor sets.
        // Allocated ONCE per material slot and only rewritten on a rebuild,
        // which idles first (EnsureMaterial) -- so nothing here is ever written
        // while a frame in flight reads it.
        // ---------------------------------------------------------------
        // The DECLARED param views (t1..), resolved through the vehicle's SHARED
        // NriTextureCache and kept on the slot so every later texture VARIANT
        // writes the same ones without re-resolving. An unbound, unresolvable
        // or undecodable param stays null and WriteMaterialSet substitutes
        // the white texel.
        const std::vector<Guid> textureIds = desc.instance->ResolveTextures();
        slot.textureCount = textureCount;
        slot.paramViews.assign(textureCount, nullptr);
        for (std::uint32_t t = 0; t < textureCount; ++t)
            slot.paramViews[t] = t < textureIds.size() ? TextureView(textureIds[t]) : nullptr;

        // The per-frame-slot constant-buffer views, allocated ONCE per material
        // slot and only rewritten on a rebuild, which idles first
        // (EnsureMaterial) -- so nothing here is ever written while a frame in
        // flight reads it.
        const std::uint32_t framesInFlight = FramesInFlight();
        for (std::uint32_t frameSlot = 0; frameSlot < framesInFlight; ++frameSlot)
        {
            if (cbSize > 0 && !slot.cbView[frameSlot])
            {
                nri::BufferViewDesc viewDesc = {};
                viewDesc.buffer = m_arena;
                viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
                viewDesc.offset = ArenaOffset(frameSlot, slot.cbRegion);
                viewDesc.size   = m_arenaStride;
                if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, slot.cbView[frameSlot]))
                    || !slot.cbView[frameSlot])
                {
                    ARC_ERROR("[nri-graph] Batch2DNode: the material constant-buffer view for '{}' "
                              "(frame slot {}) could not be created", desc.templ->Name(), frameSlot);
                    return false;
                }
            }
        }

        // VARIANT 0: the NIL-texture one -- what an untextured span through
        // this material binds, and exactly the single set Task 9 built. Every
        // per-sprite-texture variant beside it is allocated lazily by
        // EnsureMaterialVariant, which writes it through the same
        // WriteMaterialSet this does.
        if (slot.variants.empty())
        {
            MaterialSlot::TextureVariant nil;
            for (std::uint32_t frameSlot = 0; frameSlot < framesInFlight; ++frameSlot)
            {
                if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0,
                                                                &nil.set[frameSlot], 1, 0))
                    || !nil.set[frameSlot])
                {
                    ARC_ERROR("[nri-graph] Batch2DNode: descriptor-set allocation failed for material "
                              "'{}' -- the pool holds {} material sets, sized by "
                              "render.batch2d.maxMaterialSlots x maxSpriteTextures", desc.templ->Name(),
                              m_caps.materialSlots * framesInFlight * (1 + m_caps.spriteTextures));
                    return false;
                }
            }
            slot.variants.push_back(nil);
        }
        // EVERY variant is rewritten, not just the nil one. On a REBUILD (a
        // material re-save) the constant-buffer views above were destroyed and
        // recreated, so a texture variant's set would otherwise still name a
        // DESTROYED descriptor -- a live set pointing at freed memory. Safe to
        // rewrite here because the rebuild path idles the device first
        // (EnsureMaterial) and a fresh build has only the nil variant anyway.
        for (MaterialSlot::TextureVariant& variant : slot.variants)
        {
            nri::Descriptor* spriteView = variant.id.IsValid() ? TextureView(variant.id) : nullptr;
            for (std::uint32_t frameSlot = 0; frameSlot < framesInFlight; ++frameSlot)
                if (variant.set[frameSlot])
                    WriteMaterialSet(slot, *variant.set[frameSlot], frameSlot, spriteView);
        }

        slot.ready = true;
        return true;
    }

    Batch2DNode::MaterialSlot* Batch2DNode::EnsureMaterial(Batcher2D& batcher, std::uint16_t material)
    {
        if (m_materialRefused.contains(material))
            return nullptr;   // already reported, once

        const Material2DDesc* desc = batcher.MaterialDesc(material);
        const auto refuse = [&](const char* why) -> MaterialSlot*
        {
            m_materialRefused.insert(material);
            ARC_WARN("[nri-graph] Batch2DNode: material id {} cannot be drawn through its own "
                     "pipeline -- {}. It falls back to the plain sprite pipeline for this run.",
                     material, why);
            return nullptr;
        };
        if (!desc || !desc->templ || !desc->instance)
            return refuse("the batcher has no registration for it");
        if (!desc->vsBytes || !desc->psBytes || desc->vsBytes->empty() || desc->psBytes->empty())
            return refuse("its registration carries no stitched shader bytecode "
                          "(Material2DDesc::vsBytes)");

        // The identity of THIS registration. A material re-save replaces the
        // whole Material2DDesc (SpriteMaterialCache -> Batcher2D::
        // UpdateMaterial), so every pointer below moves with it; the sizes and
        // texture Guids are folded in so a same-address reallocation of a
        // different blob is caught too. Values are NOT in it -- PackCB runs
        // every frame, exactly as Batcher2D::End does, so a live param edit
        // needs no rebuild.
        std::uint64_t stamp = HashValue((std::uint64_t)(std::uintptr_t)desc->vsBytes.get(), kFnvOffset);
        stamp = HashValue(desc->vsBytes->size(), stamp);
        stamp = HashValue((std::uint64_t)(std::uintptr_t)desc->psBytes.get(), stamp);
        stamp = HashValue(desc->psBytes->size(), stamp);
        stamp = HashValue((std::uint64_t)(std::uintptr_t)desc->templ.get(), stamp);
        stamp = HashValue((std::uint64_t)(std::uintptr_t)desc->instance.get(), stamp);
        for (const Guid& id : desc->instance->ResolveTextures())
        {
            stamp = HashValue(id.hi, stamp);
            stamp = HashValue(id.lo, stamp);
        }

        const auto known = m_materialSlotOf.find(material);
        if (known != m_materialSlotOf.end())
        {
            MaterialSlot& slot = m_materials[known->second];
            if (slot.ready && slot.stamp == stamp)
                return &slot;

            // A REBUILD: the registration behind this id changed (an asset
            // re-save at the desk). Its descriptor sets are about to be
            // rewritten and NRI cannot free one, so they are REUSED rather than
            // re-allocated -- which means the GPU must not be reading them.
            // Idling here is honest: the edit already cost a shader compile, and
            // the alternative (a fresh set per rebuild) drains a fixed pool.
            ARC_INFO("[nri-graph] Batch2DNode: material id {} was re-registered -- rebuilding its "
                     "pipeline and bindings behind a device idle", material);
            (void)ARC_NRI_CHECK(m_device->Core().DeviceWaitIdle(&m_device->Device()));
            const nri::CoreInterface& core = m_device->Core();
            for (nri::Descriptor*& view : slot.cbView)
            {
                if (!view)
                    continue;
                core.DestroyDescriptor(view);   // safe behind the idle above
                view = nullptr;
            }
            slot.ready = false;
            slot.stamp = stamp;
            if (!BuildMaterial(slot, *desc))
                return refuse("its pipeline or bindings could not be rebuilt");
            return &slot;
        }

        if (m_materials.size() >= m_caps.materialSlots)
            return refuse("this node's material-slot table is full -- raise "
                          "render.batch2d.maxMaterialSlots (it sizes the descriptor pool and the "
                          "constant-buffer arena)");

        MaterialSlot& slot = m_materials.emplace_back();
        slot.cbRegion = (std::uint32_t)m_materials.size();   // region 0 is the globals CB
        slot.stamp    = stamp;
        if (!BuildMaterial(slot, *desc))
        {
            // Left in the table (its sets/views are already allocated out of a
            // pool that cannot free them) but never marked ready, so Record
            // draws it through the plain sprite pipeline.
            return refuse("its pipeline or bindings could not be built");
        }
        m_materialSlotOf[material] = (std::uint32_t)(m_materials.size() - 1);
        return &slot;
    }

    void Batch2DNode::Prepare(Batcher2D& batcher, const Batch2DDrained& batch,
                              nri::Format canvasFormat)
    {
        // ---------------------------------------------------------------
        // PASS 1: THE SPRITE TEXTURES (Task 2). Every span -- built-in or
        // material -- names an image asset or nil, and each distinct non-nil
        // one needs to be resident and to have a built-in descriptor set that
        // binds it at t0. Both happen HERE and only here: the upload submits
        // and waits, and a descriptor set must never be allocated or written
        // inside the frame's open command buffer.
        //
        // The result is memoized in m_spriteSets (including the misses), so a
        // scene that draws the same sprites every frame does this once.
        // ---------------------------------------------------------------
        // The budget is checked ONCE, up front, from the same pure count the
        // [nri] cases pin -- so the number in the message is the frame's real
        // distinct-texture count rather than "the ninth one I happened to
        // reach". EnsureSpriteSet still enforces per-texture (it is the
        // allocation point); both share m_warnedTextureBudget, so a frame over
        // budget says this exactly once.
        const std::uint32_t distinct = DistinctTextureCount(batch.spans);
        if (distinct > m_caps.spriteTextures && !m_warnedTextureBudget)
        {
            m_warnedTextureBudget = true;
            ARC_ERROR("[nri-graph] Batch2DNode: this frame names {} distinct sprite textures, over "
                      "this node's cap of {} -- the ones past the cap draw with the white texel. "
                      "Raise render.batch2d.maxSpriteTextures (it sizes the descriptor pool).",
                      distinct, m_caps.spriteTextures);
        }
        for (const Batch2DDrawSpan& span : batch.spans)
            (void)EnsureSpriteSet(span.textureId);

        // ---------------------------------------------------------------
        // PASS 2: THE REGISTERED MATERIALS.
        // PACK-ONCE-PER-BATCH, the same dedup Batcher2D::End applies with its
        // `packedThisBatch` flag: the sort key groups by material but a material
        // can still own several spans (a different texture between them splits
        // the run), and re-resolving a whole MaterialInstance parent chain per
        // span would be per-draw work for a per-material answer.
        for (MaterialSlot& slot : m_materials)
            slot.packedThisBatch = false;

        for (const Batch2DDrawSpan& span : batch.spans)
        {
            if (span.material < kBuiltInCount)
                continue;

            MaterialSlot* slot = EnsureMaterial(batcher, span.material);
            if (!slot || !slot->ready || slot->packedThisBatch)
                continue;   // already reported once, or already done this frame
            slot->packedThisBatch = true;

            // The PSO, built HERE so a first-frame pipeline compile does not
            // land inside the recording window either.
            NriPipelineCache::GraphicsKey key = {};
            key.shaderPairId    = slot->shaderPairId;
            key.layoutId        = slot->layoutId;
            key.colorFormats[0] = canvasFormat;
            key.colorCount      = 1;
            key.depthFormat     = nri::Format::UNKNOWN;
            key.topology        = nri::Topology::TRIANGLE_LIST;
            key.blend           = NriPipelineCache::GraphicsKey::Blend::AlphaOver;
            key.depthWrite      = false;
            key.cullMode        = nri::CullMode::NONE;

            nri::ShaderDesc stages[2] = {};
            stages[0].stage          = nri::StageBits::VERTEX_SHADER;
            stages[0].bytecode       = slot->vs->data();
            stages[0].size           = slot->vs->size();
            stages[0].entryPointName = kVsEntry;
            stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
            stages[1].bytecode       = slot->ps->data();
            stages[1].size           = slot->ps->size();
            stages[1].entryPointName = kPsEntry;

            slot->pipeline = m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
                {
                    desc.vertexInput = &m_vertexInput;
                    desc.shaders     = stages;
                    desc.shaderNum   = 2;
                    desc.rasterization.fillMode = nri::FillMode::SOLID;
                });
            if (!slot->pipeline)
            {
                // The cache already logged + latched. Refuse the material for
                // the rest of the run rather than clearing `ready`, which would
                // make EnsureMaterial take its REBUILD branch -- and therefore
                // idle the device -- on every subsequent frame.
                m_materialRefused.insert(span.material);
                ARC_WARN("[nri-graph] Batch2DNode: material id {} has no pipeline for the canvas "
                         "format -- it falls back to the plain sprite pipeline for this run",
                         span.material);
                continue;
            }

            // The values, packed on the CPU while there is no command buffer
            // open. Every frame, unconditionally: MaterialInstance resolves its
            // parent chain here and a live param edit must show up without a
            // rebuild -- which is exactly what Batcher2D::End does too.
            if (slot->cbSize > 0)
            {
                const Material2DDesc* desc = batcher.MaterialDesc(span.material);
                if (desc && desc->instance)
                    desc->instance->PackCB(slot->packed.data(), slot->packed.size());
            }
        }

        // ---------------------------------------------------------------
        // PASS 3: the (material, sprite texture) VARIANTS. Separate from pass
        // 2 because that one is deduped per MATERIAL (`packedThisBatch`) while
        // this must see EVERY span: one material drawn with three different
        // sprite textures needs three variants, and pass 2 visits it once.
        // ---------------------------------------------------------------
        for (const Batch2DDrawSpan& span : batch.spans)
        {
            if (span.material < kBuiltInCount || !span.textureId.IsValid())
                continue;
            const auto known = m_materialSlotOf.find(span.material);
            if (known == m_materialSlotOf.end() || !m_materials[known->second].ready)
                continue;   // refused already, and Record draws it built-in
            (void)EnsureMaterialVariant(m_materials[known->second], span.textureId);
        }
    }

    nri::DescriptorSet* Batch2DNode::SpriteSetFor(const Guid& id) const
    {
        // RECORD-TIME ONLY: a pure lookup over what Prepare already built. It
        // must never allocate or upload -- both would happen inside the frame's
        // open command buffer. A miss is the white texel, which is also what a
        // caller that skipped Prepare entirely gets.
        if (!id.IsValid())
            return m_set;
        const auto known = m_spriteSets.find(id);
        return (known != m_spriteSets.end() && known->second) ? known->second : m_set;
    }

    void Batch2DNode::Record(RenderGraphNodeContext& context, const Batch2DDrained& batch,
                             nri::Format canvasFormat, const CVarColor& clearColor, std::uint32_t frameSlot)
    {
        const nri::CoreInterface& core = context.core;

        // The canvas is already cleared to render.clearColor: AddBatch2DNode's
        // setup (DeclareClearedCanvas) declares a CLEAR colour load op, which the
        // executor applies when it begins rendering (F5 plan 1, Task 2).
        // `clearColor` is therefore unused here; see the header.
        if (batch.spans.empty() || batch.vertices.empty() || batch.indices.empty())
            return;   // an empty frame is a cleared canvas, not an error

        // ---------------------------------------------------------------
        // Vertex + index streams, straight into this frame's ring slot.
        // ALLOCATED HERE, at RECORD time, and that is load-bearing: the
        // vehicle calls ring.BeginFrame(slot) AFTER BuildFrame, so anything
        // allocated during graph SETUP would land in the previous frame's
        // slot and be overwritten while the GPU reads it.
        //
        // The ring's buffers are CONSTANT|VERTEX|INDEX_BUFFER upload-heap
        // allocations owned by NriUploadRing, deliberately NOT graph
        // resources: HOST_UPLOAD memory is host-coherent and the frame-pacing
        // fence the swapchain already waits on is what orders the CPU write
        // against the GPU read, so they need no barrier and no declaration.
        //
        // Alignment is the caller's to supply (NriUploadRing::Allocate has no
        // default): kVertexAlign for the VB (the 36-byte stride is not a power
        // of two -- see its definition), the index size for the IB.
        // ---------------------------------------------------------------
        const std::uint64_t vertexBytes = batch.vertices.size() * sizeof(Batch2DVertex);
        const std::uint64_t indexBytes  = batch.indices.size() * sizeof(std::uint32_t);

        const NriUploadRing::Alloc vertexAlloc = context.ring.Allocate(vertexBytes, kVertexAlign);
        const NriUploadRing::Alloc indexAlloc  = context.ring.Allocate(indexBytes, sizeof(std::uint32_t));
        if (!vertexAlloc.cpu || !indexAlloc.cpu)
        {
            GraphError("Batch2DNode: the upload ring could not fit this frame's vertex/index streams ("
                       + std::to_string(vertexBytes + indexBytes)
                       + " bytes) -- the batch is dropped this frame. Raise "
                         "render.uploadRingBytesPerFrame.");
            return;
        }
        std::memcpy(vertexAlloc.cpu, batch.vertices.data(), (std::size_t)vertexBytes);
        std::memcpy(indexAlloc.cpu, batch.indices.data(), (std::size_t)indexBytes);

        nri::PipelineLayout* builtInLayout = m_pipelines->Layout(m_layoutId);
        if (!builtInLayout)
        {
            GraphError("Batch2DNode: the pipeline layout is gone -- nothing recorded");
            return;
        }

        // ---------------------------------------------------------------
        // The constant buffers, into THIS frame slot's arena regions. Written
        // here rather than at declaration time for the same reason the ring's
        // allocations are: the frame-pacing fence this slot is safe behind is
        // waited inside Execute (the swapchain acquire), which is upstream of
        // every exec fn and downstream of every declaration. HOST_UPLOAD memory
        // is host-coherent, so a memcpy is the whole of the upload -- no
        // barrier, no flush, exactly as the ring's streams need none.
        //
        // The globals block is 16 bytes (GlobalParams) and the material blocks
        // were PACKED on the CPU in Prepare; all that is left is the
        // copy. Nothing here is written when the batch drew no registered
        // material, and the arena is untouched in that case.
        // ---------------------------------------------------------------
        auto* arena = static_cast<std::uint8_t*>(m_arenaCpu);
        if (arena && batch.globals)
            std::memcpy(arena + ArenaOffset(frameSlot, 0), batch.globals, sizeof(GlobalParams));
        if (arena)
        {
            for (MaterialSlot& slot : m_materials)
            {
                if (!slot.ready || !slot.packedThisBatch || slot.packed.empty())
                    continue;
                std::memcpy(arena + ArenaOffset(frameSlot, slot.cbRegion),
                            slot.packed.data(), slot.packed.size());
            }
        }

        // Projection: the SAME block for every pipeline here -- sprite.hlsl,
        // circle/msdf and sprite_material.hlsl declare an identical b0. Both
        // paths are filled once per frame (canvas pixels (y down) -> clip via
        // 2/viewport; world metres -> clip via the batch's view-projection);
        // which one a span takes is `push.worldSpace`, set in the loop below.
        BatchRootConstants push;
        push.invHalfViewportX = batch.viewport.x > 0.0f ? 2.0f / batch.viewport.x : 0.0f;
        push.invHalfViewportY = batch.viewport.y > 0.0f ? 2.0f / batch.viewport.y : 0.0f;
        std::memcpy(push.viewProj, &batch.viewProjection, sizeof(push.viewProj));   // column-major, no transpose

        core.CmdSetDescriptorPool(context.cmd, *m_pool);

        nri::VertexBufferDesc vertexBuffer = {};
        vertexBuffer.buffer = vertexAlloc.buffer;
        vertexBuffer.offset = vertexAlloc.offset;
        vertexBuffer.stride = sizeof(Batch2DVertex);
        core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);
        core.CmdSetIndexBuffer(context.cmd, *indexAlloc.buffer, indexAlloc.offset,
                                nri::IndexType::UINT32);

        // One draw per drained span, in the batcher's sorted order -- the same
        // loop End() ran, with the pipeline coming from the shared cache
        // instead of the batcher's own table.
        //
        // The LAYOUT and the SET are now per-span, not per-frame: a registered
        // material has its own of each (it carries two constant buffers and its
        // declared textures). CmdSetPipelineLayout invalidates the bound sets
        // and root constants on both backends, so a layout change re-binds all
        // three -- while a SET change alone (two materials sharing one layout,
        // which the dedup makes the common case for materials of the same
        // shape) re-binds only the set. Tracking BOTH is load-bearing: keying
        // the rebind on the layout alone would silently leave the previous
        // material's textures and constants bound.
        //
        // The ROOT CONSTANTS are per-span too since F4 plan 1 T4: `worldSpace`
        // selects the projection path, so the block is re-issued whenever the
        // span's flag differs from what is currently pushed OR the layout
        // changed (which invalidates the pushed block on both backends).
        // `lastWorldSpace` starts as "nothing pushed" (-1) rather than 0 so
        // the first span always pushes even under a fresh layout -- the
        // layout branch pushes anyway, but the two conditions are kept
        // independent so neither silently relies on the other.
        nri::PipelineLayout* lastLayout     = nullptr;
        nri::DescriptorSet*  lastSet        = nullptr;
        int                  lastWorldSpace = -1;
        for (const Batch2DDrawSpan& span : batch.spans)
        {
            nri::Pipeline*       pipeline = nullptr;
            nri::PipelineLayout* layout   = nullptr;
            nri::DescriptorSet*  set      = nullptr;

            // Everything a registered material needs was resolved at
            // declaration time (Prepare): `pipeline` and `set` are
            // direct field reads off the resolved MaterialSlot, and `layout`
            // is an O(1) bounds-checked index into NriPipelineCache's own
            // vector (Layout()), not a hash lookup. None of the three can
            // miss and fall back to compiling -- the recording window must
            // not contain a PSO compile.
            const auto known = span.material >= kBuiltInCount
                             ? m_materialSlotOf.find(span.material)
                             : m_materialSlotOf.end();
            if (known != m_materialSlotOf.end() && m_materials[known->second].ready)
            {
                const MaterialSlot& slot = m_materials[known->second];
                pipeline = slot.pipeline;
                layout   = m_pipelines->Layout(slot.layoutId);
                // The (material, sprite texture) VARIANT Prepare built for this
                // span, or the nil one. A LINEAR SCAN and not a map: `variants`
                // holds at most 1 + m_caps.spriteTextures entries and is almost
                // always one or two, so this is cheaper than hashing -- and it
                // allocates nothing, which is the rule inside the recording
                // window.
                for (const MaterialSlot::TextureVariant& variant : slot.variants)
                {
                    if (variant.id == span.textureId)
                    {
                        set = variant.set[frameSlot];
                        break;
                    }
                    if (!variant.id.IsValid())
                        set = variant.set[frameSlot];   // the nil fallback, kept if nothing matches
                }
            }

            if (!pipeline || !layout || !set)
            {
                // Built-in, or a registered material that could not be honoured
                // -- PipelineFor states which and warns once. The SET is the
                // one Prepare wrote for this span's texture (m_set, the white
                // texel, for a nil or non-resident id).
                pipeline = PipelineFor(span.material, canvasFormat);
                layout   = builtInLayout;
                set      = SpriteSetFor(span.textureId);
            }
            if (!pipeline)
                continue;   // already logged + latched by the cache

            bool pushRootConstants = false;
            if (layout != lastLayout)
            {
                lastLayout = layout;
                lastSet    = nullptr;   // a layout change invalidates the bound set
                lastWorldSpace = -1;    // ...and the pushed root constants
                core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);
                pushRootConstants = true;
            }
            const int spanWorldSpace = span.worldSpace ? 1 : 0;
            if (spanWorldSpace != lastWorldSpace)
            {
                lastWorldSpace  = spanWorldSpace;
                push.worldSpace = span.worldSpace ? 1u : 0u;
                pushRootConstants = true;
            }
            if (pushRootConstants)
            {
                nri::SetRootConstantsDesc rootConstants = {};
                rootConstants.rootConstantIndex = 0;
                rootConstants.data              = &push;
                rootConstants.size              = sizeof(push);
                core.CmdSetRootConstants(context.cmd, rootConstants);
            }
            if (set != lastSet)
            {
                lastSet = set;
                nri::SetDescriptorSetDesc setDesc = {};
                setDesc.setIndex      = 0;
                setDesc.descriptorSet = set;
                core.CmdSetDescriptorSet(context.cmd, setDesc);
            }

            core.CmdSetPipeline(context.cmd, *pipeline);

            nri::DrawIndexedDesc draw = {};
            draw.indexNum    = span.indexCount;
            draw.instanceNum = 1;
            draw.baseIndex   = span.firstIndex;
            core.CmdDrawIndexed(context.cmd, draw);
        }
    }

    RgTexture DeclareClearedCanvas(RenderGraph& graph, RenderGraphBuilder& builder,
                                   std::uint32_t width, std::uint32_t height)
    {
        const CVarColor clearColor = Settings<RenderSettings>().clearColor;
        nri::ClearValue clear{};
        clear.color.f = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };

        RgTextureDesc desc;
        desc.format                 = GraphCanvasFormat();
        desc.width                  = width;
        desc.height                 = height;
        desc.optimizedClearValue    = clear;
        desc.hasOptimizedClearValue = true;
        const RgTexture canvas = builder.CreateTexture("canvas", desc);
        builder.Write(canvas, RgUsage::ColorWrite);
        graph.SetColorAttachments(std::span<const RgTexture>(&canvas, 1));

        RgAttachmentOp op;
        op.load  = RgLoadOp::Clear;
        op.clear = clear;
        graph.SetColorAttachmentOps(std::span<const RgAttachmentOp>(&op, 1));
        return canvas;
    }

    RgTexture AddBatch2DNode(RenderGraph& graph, NriGraphContext* context,
                             std::uint32_t width, std::uint32_t height)
    {
        // Resolved at DECLARATION time on purpose: it is pure CPU work with no
        // ring allocation in it, and doing it here means the exec fn only
        // touches the GPU.
        //
        // WITH A BOUND PACKET (F5 plan 1 Task 5) the batch is the packet's
        // copy, which ExtractFrame drained out of the host's batcher: the
        // spans view the packet's vectors, which live until FramesInFlight()
        // later extracts recycle the slot, and nothing is drained again.
        // Prepare still resolves the REGISTERED materials on CurrentBatch(),
        // the same batcher extract submitted into (the hosts pass it as
        // FrameDesc::batch), because that is where the registrations live.
        //
        // WITHOUT ONE (thumbnail harvest, shader preview) the batcher is
        // drained here; those spans view the batcher's own storage and stay
        // valid until its next Begin(), which is a whole frame away.
        const nri::Format canvasFormat = GraphCanvasFormat();
        const CVarColor clearColor = Settings<RenderSettings>().clearColor;   // once per frame; see the top of this file
        Batch2DDrained drained;
        if (context && context->Packet())
        {
            const RenderFramePacket* packet = context->Packet();
            drained.vertices = packet->batchVertices;
            drained.indices = packet->batchIndices;
            drained.spans = packet->batchSpans;
            drained.globals = &packet->globals;
            drained.viewProjection = packet->viewProjection;
            drained.viewport = glm::vec2(float(width), float(height));
            if (Batcher2D* batcher = context->CurrentBatch())
                if (Batch2DNode* node = context->Batch2D())
                    node->Prepare(*batcher, drained, canvasFormat);
        }
        else if (context)
        {
            if (Batcher2D* batcher = context->CurrentBatch())
            {
                drained = batcher->Drain();
                // ...and the REGISTERED materials it names are resolved here
                // too, for the same reason: this is the last point before the
                // frame's command buffer opens, and making a material's texture
                // resident goes through a helper that submits and waits. See
                // Prepare.
                if (Batch2DNode* node = context->Batch2D())
                    node->Prepare(*batcher, drained, canvasFormat);
            }
        }

        // `canvas` is captured by reference ([&]) below, not shared_ptr like
        // AddTonemapNode's `backbuffer` -- safe here only because the SETUP
        // lambda is the one that mutates it, and AddNode runs setup
        // synchronously (before AddBatch2DNode returns), so `canvas` is still
        // this frame's live stack local. Nothing here shares it with the EXEC
        // lambda below, which runs later, during Execute(). If an exec fn
        // ever needed to read `canvas`, it would need AddTonemapNode's
        // shared_ptr<RgTexture> pattern instead -- a bare reference into a
        // function that has already returned by then would dangle.
        RgTexture canvas{};
        graph.AddNode("batch2d", RenderGraph::NodeKind::Raster,
            [&](RenderGraphBuilder& builder)
            {
                canvas = DeclareClearedCanvas(graph, builder, width, height);
            },
            [context, drained, canvasFormat, clearColor](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive: no device, nothing to record
                if (Batch2DNode* node = context->Batch2D())
                    node->Record(nodeContext, drained, canvasFormat, clearColor, context->FrameSlot());
            });
        return canvas;
    }
}
