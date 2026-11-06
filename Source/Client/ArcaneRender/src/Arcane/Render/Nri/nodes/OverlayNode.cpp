// OverlayNode -- the post-tonemap overlay pass. See OverlayNode.hpp.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "OverlayNode.hpp"

#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DepthConventionMap.hpp>   // NearerOrEqualOp, kSelectedDepthConvention
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/ShaderConventions.hpp>    // kVsEntry / kPsEntry

#undef ERROR

#include <Arcane/Render/RenderFramePacket.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>

namespace Arcane
{
    namespace
    {
        void GraphError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        constexpr const char* kOverlayVs     = "overlay_vs";
        constexpr const char* kOverlayPs     = "overlay_ps";
        constexpr const char* kOverlayGridVs = "overlay_grid_vs";
        constexpr const char* kOverlayGridPs = "overlay_grid_ps";
        // SPIR-V matches the entry point by name; DXIL ignores it.
        constexpr const char* kGridVsEntry = "grid_vs_main";
        constexpr const char* kGridPsEntry = "grid_ps_main";

        // The vertex stream's placement alignment in the upload ring (the
        // 40-byte stride is not a power of two).
        ARC_CONSTANT("layout: overlay vertex placement alignment in the upload ring")
        constexpr std::uint64_t kVertexAlign = 16;

        // overlay.hlsl's OverlayCB, byte for byte.
        struct OverlayFrameConstants
        {
            glm::mat4 viewProjection{ 1.0f };       // world-space primitives
            glm::vec2 viewport{ 0.0f };
            float     discardIfGreater = 0.0f;
            float     hasDepth         = 0.0f;
            glm::mat4 gridViewProjection{ 1.0f };   // the 3D grid's own camera
            glm::vec4 gridFlags{ 0.0f };         // x: the grid samples depth (1) or not (0)
            glm::vec4 gridEyeAndPlane{ 0.0f };   // xyz eye, w plane id
            glm::vec4 gridParams{ 0.0f };        // minor, majorEvery, fade, halfExtent
            glm::vec4 gridMinorColor{ 0.0f };
            glm::vec4 gridMajorColor{ 0.0f };
            glm::vec4 gridAxisUColor{ 0.0f };
            glm::vec4 gridAxisVColor{ 0.0f };
        };
        static_assert(sizeof(OverlayFrameConstants) == 256, "must match overlay.hlsl's OverlayCB");
        static_assert(sizeof(OverlayFrameConstants) <= OverlayNode::kFrameCbMaxBytes,
                      "the frame constants must fit one arena region");
        static_assert(sizeof(OverlayNode::Vertex) == 40, "must match overlay.hlsl's VSIn stream");

        [[nodiscard]] bool IsFinite(const glm::mat4& m) noexcept
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (!std::isfinite(m[c][r]))
                        return false;
            return true;
        }

        [[nodiscard]] OverlayNode::Vertex MakeVertex(glm::vec3 pos, glm::vec4 color, bool screenSpace,
                                                     OverlayColorSpace space, bool depthTest) noexcept
        {
            OverlayNode::Vertex v{};
            v.pos         = pos;
            v.color       = color;
            v.screenSpace = screenSpace ? 1.0f : 0.0f;
            v.linearSpace = space == OverlayColorSpace::Linear ? 1.0f : 0.0f;
            v.depthTest   = depthTest ? 1.0f : 0.0f;
            return v;
        }

        // A primitive samples the scene depth when it is DepthTested AND either
        // screen-space (its z is a clip depth already) or world-space through a
        // camera that IS the mesh camera (review ruling I2): a world primitive
        // projected through any other camera has a clip.z the mesh depth buffer
        // knows nothing about, so it draws untested -- plans 1-2's picture.
        [[nodiscard]] bool SamplesDepth(OverlayDepthClass depth, bool screenSpace, bool worldDepthTest) noexcept
        {
            return depth == OverlayDepthClass::DepthTested && (screenSpace || worldDepthTest);
        }

        void AppendLine(const OverlayLine& line, bool worldDepthTest, std::vector<OverlayNode::Vertex>& out)
        {
            if (!(line.thickness > 0.0f))
                return;
            const glm::vec2 d   = glm::vec2(line.b) - glm::vec2(line.a);
            const float     len = std::sqrt(d.x * d.x + d.y * d.y);
            if (!(len > 0.0f))
                return;   // a degenerate segment has no direction to widen across
            const glm::vec2 n = glm::vec2(-d.y, d.x) / len * (line.thickness * 0.5f);
            const float     z = line.a.z;
            const glm::vec3 a0(glm::vec2(line.a) + n, z), a1(glm::vec2(line.a) - n, z);
            const glm::vec3 b0(glm::vec2(line.b) + n, z), b1(glm::vec2(line.b) - n, z);
            const bool tested = SamplesDepth(line.depth, line.screenSpace, worldDepthTest);
            const auto v = [&](glm::vec3 p) {
                out.push_back(MakeVertex(p, line.color, line.screenSpace, line.space, tested));
            };
            v(a0); v(a1); v(b1);
            v(a0); v(b1); v(b0);
        }

        void AppendFill(const OverlayFill& fill, bool worldDepthTest, std::vector<OverlayNode::Vertex>& out)
        {
            const bool tested = SamplesDepth(fill.depth, fill.screenSpace, worldDepthTest);
            const auto v = [&](glm::vec3 p) {
                out.push_back(MakeVertex(p, fill.color, fill.screenSpace, fill.space, tested));
            };
            const float z = fill.a.z;
            switch (fill.kind)
            {
            case OverlayPrimKind::Triangle:
                v(fill.a); v(fill.b); v(fill.c);
                break;
            case OverlayPrimKind::Rect:
            {
                const glm::vec3 p0(fill.a.x,               fill.a.y,               z);
                const glm::vec3 p1(fill.a.x + fill.size.x, fill.a.y,               z);
                const glm::vec3 p2(fill.a.x + fill.size.x, fill.a.y + fill.size.y, z);
                const glm::vec3 p3(fill.a.x,               fill.a.y + fill.size.y, z);
                v(p0); v(p1); v(p2);
                v(p0); v(p2); v(p3);
                break;
            }
            case OverlayPrimKind::Circle:
            {
                if (!(fill.radius > 0.0f))
                    break;
                ARC_CONSTANT("math: a full turn in radians")
                constexpr float kTwoPi = 6.28318530717958647692f;
                const glm::vec3 centre(fill.a.x, fill.a.y, z);
                const std::uint32_t segments = OverlayNode::kCircleSegments;
                for (std::uint32_t i = 0; i < segments; ++i)
                {
                    const float t0 = kTwoPi * (float)i       / (float)segments;
                    const float t1 = kTwoPi * (float)(i + 1) / (float)segments;
                    v(centre);
                    v(glm::vec3(fill.a.x + std::cos(t0) * fill.radius, fill.a.y + std::sin(t0) * fill.radius, z));
                    v(glm::vec3(fill.a.x + std::cos(t1) * fill.radius, fill.a.y + std::sin(t1) * fill.radius, z));
                }
                break;
            }
            case OverlayPrimKind::Line:
                break;   // a line is a Tag::Line primitive; a Line-kind fill draws nothing
            }
        }
    }

    // =====================================================================
    // CPU expansion -- ONE pass over the list per depth class, so lines and
    // fills keep the order they were submitted in (review ruling I1).
    // =====================================================================
    void OverlayNode::AppendTriangles(const OverlayPrimitiveList& list, OverlayDepthClass depth,
                                      bool worldDepthTest, std::vector<Vertex>& out)
    {
        for (const OverlayPrimitive& prim : list.prims)
        {
            if (prim.tag == OverlayPrimitive::Tag::Line)
            {
                if (prim.line.depth == depth)
                    AppendLine(prim.line, worldDepthTest, out);
            }
            else if (prim.fill.depth == depth)
            {
                AppendFill(prim.fill, worldDepthTest, out);
            }
        }
    }

    // =====================================================================
    // Create / bindings
    // =====================================================================
    std::unique_ptr<OverlayNode> OverlayNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<OverlayNode> node(new OverlayNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool OverlayNode::Init(NriGraphContext& context)
    {
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();

        m_vs     = context.ShaderBytecode(kOverlayVs);
        m_ps     = context.ShaderBytecode(kOverlayPs);
        m_gridVs = context.ShaderBytecode(kOverlayGridVs);
        m_gridPs = context.ShaderBytecode(kOverlayGridPs);
        if (m_vs.empty() || m_ps.empty() || m_gridVs.empty() || m_gridPs.empty())
        {
            ARC_ERROR("[nri-graph] OverlayNode: shader bin '{}'/'{}'/'{}'/'{}' is missing -- the "
                      "overlay pass cannot be built", kOverlayVs, kOverlayPs, kOverlayGridVs, kOverlayGridPs);
            return false;
        }

        // overlay.hlsl's VSIn. TEXCOORD0..2 are three semantic indices of one name.
        m_attributes[0].d3d.semanticName  = "POSITION";
        m_attributes[0].vk.location       = 0;
        m_attributes[0].offset            = offsetof(Vertex, pos);
        m_attributes[0].format            = nri::Format::RGB32_SFLOAT;
        m_attributes[1].d3d.semanticName  = "COLOR";
        m_attributes[1].vk.location       = 1;
        m_attributes[1].offset            = offsetof(Vertex, color);
        m_attributes[1].format            = nri::Format::RGBA32_SFLOAT;
        m_attributes[2].d3d.semanticName  = "TEXCOORD";
        m_attributes[2].d3d.semanticIndex = 0;
        m_attributes[2].vk.location       = 2;
        m_attributes[2].offset            = offsetof(Vertex, screenSpace);
        m_attributes[2].format            = nri::Format::R32_SFLOAT;
        m_attributes[3].d3d.semanticName  = "TEXCOORD";
        m_attributes[3].d3d.semanticIndex = 1;
        m_attributes[3].vk.location       = 3;
        m_attributes[3].offset            = offsetof(Vertex, linearSpace);
        m_attributes[3].format            = nri::Format::R32_SFLOAT;
        m_attributes[4].d3d.semanticName  = "TEXCOORD";
        m_attributes[4].d3d.semanticIndex = 2;
        m_attributes[4].vk.location       = 4;
        m_attributes[4].offset            = offsetof(Vertex, depthTest);
        m_attributes[4].format            = nri::Format::R32_SFLOAT;

        m_stream.bindingSlot = 0;
        m_stream.stepRate    = nri::VertexStreamStepRate::PER_VERTEX;
        m_stream.stride      = (std::uint16_t)sizeof(Vertex);

        m_vertexInput.attributes   = m_attributes;
        m_vertexInput.attributeNum = (std::uint8_t)std::size(m_attributes);
        m_vertexInput.streams      = &m_stream;
        m_vertexInput.streamNum    = 1;

        return CreateBindings() && CreateConstantArena() && CreateDummyDepth() && CreateSets();
    }

    nri::DescriptorPoolDesc OverlayNode::PoolSizes() noexcept
    {
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum  = FramesInFlight();
        poolDesc.constantBufferMaxNum = FramesInFlight();
        poolDesc.textureMaxNum        = FramesInFlight();
        poolDesc.samplerMaxNum        = FramesInFlight();
        return poolDesc;
    }

    bool OverlayNode::CreateBindings()
    {
        const nri::CoreInterface& core = m_device->Core();

        // overlay.hlsl: b0 (VS reads the matrix + viewport, PS reads the depth
        // flags and the grid block), t0 (scene depth), s0 (point), all space 0.
        // CBV, SRV, sampler: three different range types, so D3D12 merges none
        // of them (OutlineNode::CreateBindings, PickOutlineNodes.cpp). On Vulkan
        // NRI shifts these HLSL register numbers by vkBindingOffsets itself.
        // Value-initialised, then assigned field by field -- the dedup contract.
        nri::DescriptorRangeDesc ranges[3] = {};
        ranges[0].baseRegisterIndex = 0;   // b0
        ranges[0].descriptorNum     = 1;
        ranges[0].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        ranges[0].shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        ranges[1].baseRegisterIndex = 0;   // t0
        ranges[1].descriptorNum     = 1;
        ranges[1].descriptorType    = nri::DescriptorType::TEXTURE;
        ranges[1].shaderStages      = nri::StageBits::FRAGMENT_SHADER;
        ranges[2].baseRegisterIndex = 0;   // s0
        ranges[2].descriptorNum     = 1;
        ranges[2].descriptorType    = nri::DescriptorType::SAMPLER;
        ranges[2].shaderStages      = nri::StageBits::FRAGMENT_SHADER;

        nri::DescriptorSetDesc setDesc = {};
        setDesc.registerSpace = 0;
        setDesc.ranges        = ranges;
        setDesc.rangeNum      = 3;

        // No root constants, root descriptors or root samplers, so the
        // register-space rule (FullscreenNodes.cpp) puts nothing at the root;
        // rootRegisterSpace is 0 because the set's registerSpace is 0.
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.descriptorSets    = &setDesc;
        layoutDesc.descriptorSetNum  = 1;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        if (!m_pipelines->Layout(m_layoutId))
        {
            ARC_ERROR("[nri-graph] OverlayNode: pipeline layout registration failed");
            return false;
        }

        const nri::DescriptorPoolDesc poolDesc = PoolSizes();
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] OverlayNode: descriptor pool creation failed");
            return false;
        }

        // POINT + clamp, TonemapNode's sampler: the depth maps 1:1 onto the target.
        nri::SamplerDesc samplerDesc = {};
        samplerDesc.filters.min  = nri::Filter::NEAREST;
        samplerDesc.filters.mag  = nri::Filter::NEAREST;
        samplerDesc.filters.mip  = nri::Filter::NEAREST;
        samplerDesc.addressModes = { nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE,
                                     nri::AddressMode::CLAMP_TO_EDGE };
        samplerDesc.mipMax       = 16.0f;
        if (!ARC_NRI_CHECK(core.CreateSampler(m_device->Device(), samplerDesc, m_sampler)) || !m_sampler)
        {
            ARC_ERROR("[nri-graph] OverlayNode: sampler creation failed");
            return false;
        }
        return true;
    }

    bool OverlayNode::CreateConstantArena()
    {
        const nri::CoreInterface& core = m_device->Core();
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());

        const std::uint64_t align = deviceDesc.memoryAlignment.constantBufferOffset;
        m_arenaStride = align <= 1 ? kFrameCbMaxBytes
                                   : ((kFrameCbMaxBytes + align - 1) / align) * align;
        const std::uint64_t arenaBytes = m_arenaStride * FramesInFlight();

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = arenaBytes;
        bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                       0.0f, bufferDesc, m_arena))
            || !m_arena)
        {
            ARC_ERROR("[nri-graph] OverlayNode: the frame constant-buffer arena ({} bytes) could not "
                      "be created", arenaBytes);
            return false;
        }
        core.SetDebugName(m_arena, "nri-graph overlay frame CBs");

        m_arenaCpu = core.MapBuffer(*m_arena, 0, nri::WHOLE_SIZE);
        if (!m_arenaCpu)
        {
            ARC_ERROR("[nri-graph] OverlayNode: the frame constant-buffer arena could not be mapped "
                      "(the NONE backend cannot -- this node is a [gpu] path)");
            return false;
        }

        // One CB view per slot, created once: the CONTENTS change every frame,
        // the (buffer, offset) never does.
        for (std::uint32_t slot = 0; slot < FramesInFlight(); ++slot)
        {
            nri::BufferViewDesc viewDesc = {};
            viewDesc.buffer = m_arena;
            viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
            viewDesc.offset = ArenaOffset(slot);
            viewDesc.size   = m_arenaStride;
            if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, m_cbView[slot])) || !m_cbView[slot])
            {
                ARC_ERROR("[nri-graph] OverlayNode: the constant-buffer view for frame slot {} could "
                          "not be created", slot);
                return false;
            }
        }
        return true;
    }

    bool OverlayNode::CreateDummyDepth()
    {
        const nri::CoreInterface& core = m_device->Core();

        // overlay.hlsl always declares t0, so a frame with no scene depth still
        // binds something there: a 1x1 R32_SFLOAT texel holding 0. The shader
        // never samples it (g_HasDepth is 0 on such a frame).
        nri::TextureDesc textureDesc = {};
        textureDesc.type      = nri::TextureType::TEXTURE_2D;
        textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
        textureDesc.format    = nri::Format::R32_SFLOAT;
        textureDesc.width     = 1;
        textureDesc.height    = 1;
        textureDesc.depth     = 1;
        textureDesc.mipNum    = 1;
        textureDesc.layerNum  = 1;
        textureDesc.sampleNum = 1;
        if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                        0.0f, textureDesc, m_dummyDepth))
            || !m_dummyDepth)
        {
            ARC_ERROR("[nri-graph] OverlayNode: the 1x1 dummy depth texel could not be created");
            return false;
        }
        core.SetDebugName(m_dummyDepth, "nri-graph overlay dummy depth");

        // Through NRI's own helper, as PostChainNode's fallback texels are:
        // UploadData submits and waits internally.
        nri::HelperInterface helper = {};
        if (!ARC_NRI_CHECK(nriGetInterface(m_device->Device(), NRI_INTERFACE(nri::HelperInterface), &helper)))
        {
            ARC_ERROR("[nri-graph] OverlayNode: HelperInterface unavailable -- cannot upload the "
                      "dummy depth texel");
            return false;
        }
        const float zero = 0.0f;
        nri::TextureSubresourceUploadDesc subresource = {};
        subresource.slices     = &zero;
        subresource.sliceNum   = 1;
        subresource.rowPitch   = 4;
        subresource.slicePitch = 4;

        nri::TextureUploadDesc upload = {};
        upload.subresources = &subresource;
        upload.texture      = m_dummyDepth;
        upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                nri::StageBits::FRAGMENT_SHADER };
        upload.planes       = nri::PlaneBits::ALL;
        if (!ARC_NRI_CHECK(helper.UploadData(*m_device->GraphicsQueue(), &upload, 1, nullptr, 0)))
        {
            ARC_ERROR("[nri-graph] OverlayNode: the dummy depth texel upload failed");
            return false;
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = m_dummyDepth;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, m_dummyDepthView)) || !m_dummyDepthView)
        {
            ARC_ERROR("[nri-graph] OverlayNode: the dummy depth texel's view could not be created");
            return false;
        }
        return true;
    }

    bool OverlayNode::CreateSets()
    {
        const nri::CoreInterface& core = m_device->Core();
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] OverlayNode: no layout to allocate descriptor sets from");
            return false;
        }

        for (std::uint32_t slot = 0; slot < FramesInFlight(); ++slot)
        {
            if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0, &m_sets[slot], 1, 0))
                || !m_sets[slot])
            {
                ARC_ERROR("[nri-graph] OverlayNode: descriptor-set allocation failed for frame slot {} "
                          "-- the pool holds {} sets (PoolSizes)", slot, PoolSizes().descriptorSetMaxNum);
                return false;
            }

            // b0 and s0 once; t0 starts as the dummy so the set is never
            // incomplete, and Record rewrites it when the bound texture changes.
            const nri::Descriptor* cb      = m_cbView[slot];
            const nri::Descriptor* dummy   = m_dummyDepthView;
            const nri::Descriptor* sampler = m_sampler;
            nri::UpdateDescriptorRangeDesc updates[3] = {};
            updates[0].descriptorSet = m_sets[slot];
            updates[0].rangeIndex    = 0;
            updates[0].descriptors   = &cb;
            updates[0].descriptorNum = 1;
            updates[1].descriptorSet = m_sets[slot];
            updates[1].rangeIndex    = 1;
            updates[1].descriptors   = &dummy;
            updates[1].descriptorNum = 1;
            updates[2].descriptorSet = m_sets[slot];
            updates[2].rangeIndex    = 2;
            updates[2].descriptors   = &sampler;
            updates[2].descriptorNum = 1;
            core.UpdateDescriptorRanges(updates, 3);
            m_boundDepth[slot] = m_dummyDepthView;
        }
        return true;
    }

    // =====================================================================
    // Lifetime
    // =====================================================================
    OverlayNode::~OverlayNode()
    {
        if (m_device && (m_pool || m_arena || m_sampler || m_dummyDepth || !m_depthViews.empty()))
        {
            ARC_WARN("[nri-graph] OverlayNode destroyed with live NRI objects -- either Create() "
                     "failed part way (an ERROR above says which step) or its owner never called "
                     "Release(). Destroying directly behind a DeviceWaitIdle.");
            const nri::CoreInterface& core = m_device->Core();
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
            for (const DepthView& dv : m_depthViews)
                if (dv.view) core.DestroyDescriptor(dv.view);
            m_depthViews.clear();
            if (m_pool) core.DestroyDescriptorPool(m_pool);
            for (nri::Descriptor*& view : m_cbView)
                if (view) { core.DestroyDescriptor(view); view = nullptr; }
            if (m_sampler) core.DestroyDescriptor(m_sampler);
            if (m_dummyDepthView) core.DestroyDescriptor(m_dummyDepthView);
            if (m_dummyDepth) core.DestroyTexture(m_dummyDepth);
            if (m_arena)
            {
                if (m_arenaCpu) core.UnmapBuffer(*m_arena);
                core.DestroyBuffer(m_arena);
            }
            for (nri::DescriptorSet*& set : m_sets)
                set = nullptr;
            m_pool = nullptr; m_sampler = nullptr;
            m_dummyDepthView = nullptr; m_dummyDepth = nullptr;
            m_arena = nullptr; m_arenaCpu = nullptr;
        }
    }

    void OverlayNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // Views before the resources they view.
        InvalidateDepthViews(graveyard, fence);
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
        }
        for (nri::DescriptorSet*& set : m_sets)
            set = nullptr;   // owned by the pool buried above
        for (nri::Descriptor*& view : m_cbView)
        {
            if (!view)
                continue;
            graveyard.Bury(fence, [core, d = view] { core->DestroyDescriptor(d); });
            view = nullptr;
        }
        if (m_sampler)
        {
            graveyard.Bury(fence, [core, d = m_sampler] { core->DestroyDescriptor(d); });
            m_sampler = nullptr;
        }
        if (m_dummyDepthView)
        {
            graveyard.Bury(fence, [core, d = m_dummyDepthView] { core->DestroyDescriptor(d); });
            m_dummyDepthView = nullptr;
        }
        if (m_dummyDepth)
        {
            graveyard.Bury(fence, [core, t = m_dummyDepth] { core->DestroyTexture(t); });
            m_dummyDepth = nullptr;
        }
        if (m_arena)
        {
            if (m_arenaCpu)
            {
                core->UnmapBuffer(*m_arena);
                m_arenaCpu = nullptr;
            }
            graveyard.Bury(fence, [core, b = m_arena] { core->DestroyBuffer(b); });
            m_arena = nullptr;
        }
        for (nri::Descriptor*& bound : m_boundDepth)
            bound = nullptr;
    }

    void OverlayNode::InvalidateDepthViews(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();
        for (const DepthView& dv : m_depthViews)
            if (dv.view)
                graveyard.Bury(fence, [core, view = dv.view] { core->DestroyDescriptor(view); });
        m_depthViews.clear();
        // The sets may still NAME a buried view, so force Record to rewrite t0.
        for (nri::Descriptor*& bound : m_boundDepth)
            bound = nullptr;
    }

    void OverlayNode::SyncPoolEpoch(const RenderGraph& graph)
    {
        // OutlineNode::SyncPoolEpoch's mechanism: the graph's lane, and the
        // submission that last used the views as the burial fence.
        if (!m_device)
            return;
        Graveyard* graves = graph.Graves();
        if (!graves)
            return;   // the graph has never Executed: it owns no pool to view
        const std::uint64_t epoch = graph.PoolEpoch();
        if (epoch == m_poolEpoch)
            return;
        m_poolEpoch = epoch;
        InvalidateDepthViews(*graves, graph.DebugSubmitCount());
    }

    nri::Descriptor* OverlayNode::EnsureView(const nri::CoreInterface& core, nri::Texture* texture)
    {
        for (const DepthView& cached : m_depthViews)
            if (cached.texture == texture)
                return cached.view;

        // One scene depth per frame; a cache that keeps growing means pool
        // textures are replaced without RenderGraph::PoolEpoch moving.
        if (m_depthViews.size() >= 4 && !m_warnedViewChurn)
        {
            m_warnedViewChurn = true;
            GraphError("OverlayNode: more distinct scene-depth textures than one frame can have -- "
                       "pool textures are being replaced without RenderGraph::PoolEpoch moving");
        }

        // A TEXTURE view in the depth texture's own format. NRI's D3D12 backend
        // patches D32_FLOAT to R32_FLOAT for a shader-resource view, which is
        // what overlay.hlsl's Texture2D<float> reads.
        nri::Descriptor* view = nullptr;
        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = core.GetTextureDesc(*texture).format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, view)) || !view)
        {
            GraphError("OverlayNode: a scene-depth shader-resource view could not be created");
            return nullptr;
        }
        m_depthViews.push_back(DepthView{ texture, view });
        return view;
    }

    // =====================================================================
    // Pipelines (resolved at record time against the TARGET's actual format,
    // OutlineNode's composite posture: the backbuffer's channel order is
    // NRI's to resolve; the shared cache compiles once per format)
    // =====================================================================
    nri::Pipeline* OverlayNode::PrimitivePipeline(nri::Format targetFormat)
    {
        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kPrimitivePairId;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = targetFormat;
        key.colorCount      = 1;
        key.depthFormat     = nri::Format::UNKNOWN;
        key.topology        = nri::Topology::TRIANGLE_LIST;
        key.blend           = NriPipelineCache::GraphicsKey::Blend::AlphaOver;
        key.depthWrite      = false;
        key.cullMode        = nri::CullMode::NONE;

        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_vs.data();
        stages[0].size           = m_vs.size();
        stages[0].entryPointName = kVsEntry;
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_ps.data();
        stages[1].size           = m_ps.size();
        stages[1].entryPointName = kPsEntry;

        return m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            desc.vertexInput = &m_vertexInput;
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            desc.rasterization.fillMode = nri::FillMode::SOLID;
        });
    }

    nri::Pipeline* OverlayNode::GridPipeline(nri::Format targetFormat)
    {
        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kGridPairId;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = targetFormat;
        key.colorCount      = 1;
        key.depthFormat     = nri::Format::UNKNOWN;
        key.topology        = nri::Topology::TRIANGLE_LIST;
        key.blend           = NriPipelineCache::GraphicsKey::Blend::AlphaOver;
        key.depthWrite      = false;
        key.cullMode        = nri::CullMode::NONE;

        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_gridVs.data();
        stages[0].size           = m_gridVs.size();
        stages[0].entryPointName = kGridVsEntry;
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_gridPs.data();
        stages[1].size           = m_gridPs.size();
        stages[1].entryPointName = kGridPsEntry;

        return m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            desc.vertexInput = nullptr;   // the quad is built from SV_VertexID
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            desc.rasterization.fillMode = nri::FillMode::SOLID;
        });
    }

    // =====================================================================
    // Record
    // =====================================================================
    void OverlayNode::Record(RenderGraphNodeContext& context, RgTexture target, RgTexture depth,
                             const OverlayRecordDesc& desc, std::uint32_t frameSlot)
    {
        const nri::CoreInterface& core = context.core;

        // Mechanism 1 (FullscreenNodes.hpp): RealizePool may have buried a pool
        // texture this Execute(), between the declarations and this exec fn.
        if (context.graph)
            SyncPoolEpoch(*context.graph);

        if (frameSlot >= FramesInFlight() || !m_sets[frameSlot] || !m_arenaCpu)
        {
            GraphError("OverlayNode: no descriptor set or constant arena for this frame slot -- "
                       "nothing recorded");
            return;
        }
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("OverlayNode: the pipeline layout is gone -- nothing recorded");
            return;
        }
        nri::Texture* targetTexture = context.Resolve(target);
        if (!targetTexture)
        {
            GraphError("OverlayNode: the node could not resolve its target");
            return;
        }
        const nri::Format targetFormat = core.GetTextureDesc(*targetTexture).format;

        // ---- t0: the scene depth this node Read, or the dummy -------------
        nri::Descriptor* depthView = m_dummyDepthView;
        bool hasDepth = false;
        if (context.graph && context.graph->IsHandleValid(depth))
        {
            nri::Texture* depthTexture = context.Resolve(depth);
            if (!depthTexture)
            {
                GraphError("OverlayNode: the scene depth could not be resolved -- nothing recorded");
                return;
            }
            depthView = EnsureView(core, depthTexture);
            if (!depthView)
                return;   // already reported
            hasDepth = true;
        }
        // Only THIS frame slot's set is touched, and its previous submission
        // has retired (the pacing wait inside Execute).
        if (m_boundDepth[frameSlot] != depthView)
        {
            const nri::Descriptor* bound = depthView;
            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = m_sets[frameSlot];
            update.rangeIndex    = 1;
            update.descriptors   = &bound;
            update.descriptorNum = 1;
            core.UpdateDescriptorRanges(&update, 1);
            m_boundDepth[frameSlot] = depthView;
        }

        // ---- the 3D grid's constants, when it draws -----------------------
        OverlayFrameConstants constants;
        constants.viewProjection   = desc.worldViewProjection;
        constants.viewport         = desc.viewport;
        constants.discardIfGreater =
            NearerOrEqualOp(kSelectedDepthConvention) == nri::CompareOp::LESS_EQUAL ? 1.0f : 0.0f;
        constants.hasDepth         = hasDepth ? 1.0f : 0.0f;

        bool drawGrid = false;
        if (desc.grid)
        {
            const GridSceneDesc& scene = *desc.grid;
            if (!IsFinite(scene.view.view) || !IsFinite(scene.view.projection))
            {
                if (!m_warnedBadCamera)
                {
                    m_warnedBadCamera = true;
                    GraphError("OverlayNode: the 3D grid's view or projection matrix is not finite -- "
                               "the grid is dropped rather than recording a draw with an undefined "
                               "clip position");
                }
            }
            else
            {
                // GridNode::Record's arithmetic, unchanged.
                const glm::mat4 inverseView = glm::inverse(scene.view.view);
                const glm::vec3 eye         = glm::vec3(inverseView[3]);
                const bool      xy          = scene.plane == GridSceneDesc::Plane::XY;
                const float     altitude    = std::abs(xy ? eye.z : eye.y);
                const float     halfExtent  = std::max(scene.minHalfExtent, scene.extentPerAltitude * altitude);
                const float     minor       = std::max(scene.minorSpacing, 1e-3f);
                const float     major       = std::max(scene.majorEvery,   minor);
                const float     fade        = std::max(scene.fadeDistance, 1e-3f);
                constants.gridViewProjection = scene.view.projection * scene.view.view;
                constants.gridFlags       = glm::vec4(desc.gridDepthTest ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
                constants.gridEyeAndPlane = glm::vec4(eye, xy ? 1.0f : 0.0f);
                constants.gridParams      = glm::vec4(minor, major, fade, halfExtent);
                constants.gridMinorColor  = scene.minorColor;
                constants.gridMajorColor  = scene.majorColor;
                constants.gridAxisUColor  = scene.axisUColor;
                constants.gridAxisVColor  = scene.axisVColor;
                drawGrid = true;
            }
        }
        std::memcpy(static_cast<std::uint8_t*>(m_arenaCpu) + ArenaOffset(frameSlot),
                    &constants, sizeof(constants));

        // ---- the primitives, depth-tested first, then always-on-top ------
        m_scratch.clear();
        if (desc.list)
        {
            AppendTriangles(*desc.list, OverlayDepthClass::DepthTested, desc.worldDepthTest, m_scratch);
            AppendTriangles(*desc.list, OverlayDepthClass::AlwaysOnTop, desc.worldDepthTest, m_scratch);
        }

        nri::Pipeline* gridPipeline = drawGrid ? GridPipeline(targetFormat) : nullptr;
        nri::Pipeline* primPipeline = !m_scratch.empty() ? PrimitivePipeline(targetFormat) : nullptr;
        if ((drawGrid && !gridPipeline) || (!m_scratch.empty() && !primPipeline))
        {
            if (!m_warnedNoPipeline)
            {
                m_warnedNoPipeline = true;
                ARC_WARN("[nri-graph] OverlayNode: no pipeline for the target format -- part of the "
                         "overlay draws nothing this run (the cache refused it and said why)");
            }
        }

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);
        nri::SetDescriptorSetDesc setDesc = {};
        setDesc.setIndex      = 0;
        setDesc.descriptorSet = m_sets[frameSlot];
        core.CmdSetDescriptorSet(context.cmd, setDesc);

        // 1. The 3D grid (no vertex buffer).
        if (gridPipeline)
        {
            core.CmdSetPipeline(context.cmd, *gridPipeline);
            nri::DrawDesc draw = {};
            draw.vertexNum   = 6;
            draw.instanceNum = 1;
            core.CmdDraw(context.cmd, draw);
        }

        // 2 + 3. Depth-tested, then always-on-top: one draw, in that order --
        // rasterisation keeps primitive order for blending.
        if (primPipeline)
        {
            // ALLOCATED HERE, at RECORD time, for the reason Batch2DNode gives:
            // the vehicle begins this slot's ring after the declarations.
            const std::uint64_t bytes = (std::uint64_t)m_scratch.size() * sizeof(Vertex);
            const NriUploadRing::Alloc alloc = context.ring.Allocate(bytes, kVertexAlign);
            if (!alloc.cpu)
            {
                GraphError("OverlayNode: the upload ring could not fit this frame's overlay vertices ("
                           + std::to_string(bytes) + " bytes) -- the primitives are dropped this frame. "
                             "Raise render.uploadRingBytesPerFrame.");
                return;
            }
            std::memcpy(alloc.cpu, m_scratch.data(), (std::size_t)bytes);

            core.CmdSetPipeline(context.cmd, *primPipeline);
            nri::VertexBufferDesc vertexBuffer = {};
            vertexBuffer.buffer = alloc.buffer;
            vertexBuffer.offset = alloc.offset;
            vertexBuffer.stride = sizeof(Vertex);
            core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);

            nri::DrawDesc draw = {};
            draw.vertexNum   = (std::uint32_t)m_scratch.size();
            draw.instanceNum = 1;
            core.CmdDraw(context.cmd, draw);
        }
    }

    // =====================================================================
    // Declaration
    // =====================================================================
    void AddOverlayNode(RenderGraph& graph, NriGraphContext* context,
                        RgTexture target, RgTexture depth,
                        const GridSceneDesc* grid, const OverlayPrimitiveList* overlay,
                        std::uint32_t width, std::uint32_t height,
                        const glm::mat4* worldViewProjection,
                        const glm::mat4* meshViewProjection)
    {
        const bool hasDepth = graph.IsHandleValid(depth);

        // The record desc: the 3D grid copied (small), the list borrowed.
        OverlayRecordDesc desc;
        desc.list     = overlay;
        desc.viewport = glm::vec2((float)width, (float)height);
        if (grid)
            desc.grid = *grid;
        else if (overlay && overlay->hasGrid3d)
            desc.grid = overlay->grid3d;
        // THE CAMERAS (review ruling I2). World-space primitives project
        // through the caller's world camera (the sprite/main view); with none
        // given, through the mesh camera, then the grid's, then identity. The
        // 3D grid always projects through its own camera. Either one samples
        // the scene depth ONLY when its camera IS the mesh camera that wrote
        // that depth (exact equality: every caller builds these products the
        // same way, projection * view, from the same values) -- a different
        // camera's clip.z means nothing against that buffer, so it draws
        // untested. No mesh pass, no depth: both draw unoccluded.
        if (worldViewProjection)
            desc.worldViewProjection = *worldViewProjection;
        else if (meshViewProjection)
            desc.worldViewProjection = *meshViewProjection;
        else if (desc.grid)
            desc.worldViewProjection = desc.grid->view.projection * desc.grid->view.view;
        desc.worldDepthTest = meshViewProjection && desc.worldViewProjection == *meshViewProjection;
        desc.gridDepthTest  = meshViewProjection && desc.grid
                           && desc.grid->view.projection * desc.grid->view.view == *meshViewProjection;

        graph.AddNode("overlay", RenderGraph::NodeKind::Raster,
            [&](RenderGraphBuilder& builder)
            {
                builder.Write(target, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(&target, 1));
                // LOAD: the tonemap already wrote every pixel of the target.
                const RgAttachmentOp load{};
                graph.SetColorAttachmentOps(std::span<const RgAttachmentOp>(&load, 1));
                // Sampled, not attached: no SetDepthAttachment, no depth op.
                if (hasDepth)
                    builder.Read(depth, RgUsage::ShaderRead);
            },
            [context, target, depth, desc](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                OverlayRecordDesc draw = desc;
                // A bound packet is the list. The matrices and depth-test
                // flags were fixed at declaration from that same packet.
                if (const RenderFramePacket* packet = context->Packet())
                {
                    draw.list = &packet->overlay;
                    if (packet->overlay.hasGrid3d)
                        draw.grid = packet->overlay.grid3d;
                    else
                        draw.grid.reset();
                }
                if (OverlayNode* node = context->Overlay())
                    node->Record(nodeContext, target, depth, draw, context->FrameSlot());
            });
    }
}
