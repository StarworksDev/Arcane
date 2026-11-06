// PickOutlineNodes -- see the header for where the entity ids come from, why
// the readback does not stall, why only the ID pass uses root constants, and
// the pool-epoch discipline the outline node owes.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp): NRI
// headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>

#include "PickOutlineNodes.hpp"

#include <Arcane/Render/DepthConventionMap.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/RenderFramePacket.hpp>   // Packet()->selectedIds (the header only forward-declares the packet)
#include <Arcane/Render/ShaderConventions.hpp>   // kVsEntry / kPsEntry
// EVERY CB LAYOUT HERE -- the seed CB's id capacity above all -- is pinned
// against the SHADER (the static_asserts below) rather than against a second
// C++ declaration, so the pin has a counterparty that cannot drift from what
// the GPU actually reads.

#undef ERROR

#include <algorithm>
#include <cstddef>   // offsetof
#include <cstring>
#include <iterator>
#include <string>

namespace Arcane
{
    namespace
    {
        // The tagged seam the whole graph path reports its OWN refusals
        // through (an NRI call's result goes through ARC_NRI_CHECK instead).
        // Both land in the RenderErrorCount() latch, which is what makes a
        // --nri-graph run's exit code mean something.
        void GraphError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        // ----------------------------------------------------------------
        // The three constant blocks, BYTE-IDENTICAL to their HLSL twins and
        // copied from SelectionOutline.cpp rather than re-derived -- the two
        // recorders feed the SAME shaders, so a layout that drifted here would
        // be a silent mis-read on one path only.
        // ----------------------------------------------------------------

        // outline_seed.hlsl's `cbuffer SeedCB`. HLSL packing: selectedCount(0),
        // int2 cursor(4..12), superSample(12), int2 dim(16..24), uint2
        // pad(24..32), then the id array (32..288). The ids are declared
        // `uint4 gSelectedIds[16]`, NOT `uint[64]`: an array of SCALARS pads
        // every element to its own 16-byte register (1024 bytes), while
        // uint4[16] packs 4 per register and mirrors a tight uint32_t[64] here.
        struct SeedCB
        {
            std::uint32_t selectedCount;
            std::int32_t  cursorX, cursorY;
            std::uint32_t superSample;
            std::int32_t  dimX, dimY;
            std::uint32_t pad0, pad1;
            std::uint32_t selectedIds[64];
        };
        static_assert(sizeof(SeedCB) == 288, "SeedCB must match outline_seed.hlsl SeedCB");
        static_assert(offsetof(SeedCB, selectedIds) == 32, "id array starts at offset 32");
        // `uint4 gSelectedIds[16]` in the shader == 64 uints == 256 bytes. This
        // is the cross-check that keeps the C++ cap and the HLSL array the same
        // size, pinned against the SHADER rather than against the deleted
        // SelectionOutline.hpp (see the include note at the top of this file).
        static_assert(OutlineNode::kMaxSelectedIds * sizeof(std::uint32_t) == 256,
                      "the seed CB's id capacity must match outline_seed.hlsl's uint4[16]");
        static_assert(sizeof(SeedCB) <= OutlineNode::kCbMaxBytes,
                      "the arena region must hold the largest of the three constant blocks");

        // outline_jfa.hlsl's `cbuffer JfaCB`: jump(0), int2 dim(4..12), pad(12).
        struct JfaCB { std::int32_t jump; std::int32_t dimX, dimY; std::int32_t pad; };
        static_assert(sizeof(JfaCB) == 16, "JfaCB must match outline_jfa.hlsl JfaCB");

        // outline_composite.hlsl's `cbuffer CompositeCB`: gSelectThick(0),
        // gHoverThick(4), gEdgeSoft(8), _pad0(12), int2 gDim(16..24), int2
        // _pad1(24..32), float4 gSelectColor(32..48), float4 gHoverColor(48..64).
        struct CompositeCB
        {
            float        selectThick, hoverThick, edgeSoft, pad0;
            std::int32_t dimX, dimY, pad1a, pad1b;
            float        selectColor[4];
            float        hoverColor[4];
        };
        static_assert(sizeof(CompositeCB) == 64, "CompositeCB must match outline_composite.hlsl");
        static_assert(offsetof(CompositeCB, selectColor) == 32, "selectColor at offset 32");

        // The authored look: amber selected, cyan hovered, an outline
        // CENTERED on the silhouette edge with an AA ramp -- the widths are
        // render.outline.* (RenderOutlineSettings; 3 px / 3 px / 1 px by
        // default). Display-referred -- the composite writes straight into the
        // tonemapped backbuffer with no sRGB conversion. Colours come from
        // OutlineNode::m_selectColor / m_hoverColor (SetColors).

        std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) noexcept
        {
            return alignment <= 1 ? value : ((value + alignment - 1) / alignment) * alignment;
        }
    }

    std::uint32_t OutlineJfaStepCount(std::uint32_t maxThicknessPx) noexcept
    {
        // The halving schedule's length, plus one for the trailing jump==1
        // refinement pass. RenderGraphTest's "outline jfa" case pins this
        // output directly against the literal schedule.
        std::uint32_t passes = 1;
        if (maxThicknessPx > 1u)
        {
            std::uint32_t n = 0, v = maxThicknessPx - 1u;
            while (v) { v >>= 1; ++n; }   // ceil(log2(maxThicknessPx))
            passes = n + 1u;
        }
        return passes + 1u;
    }

    std::int32_t OutlineJfaJump(std::uint32_t step, std::uint32_t steps) noexcept
    {
        // `steps` counts the trailing refinement, so the HALVING schedule is one
        // shorter: jumps 2^(passes-1) .. 1, then 1 once more. Same expression as
        // SelectionOutline::Render's `jc.jump`, with `i` and `passes` renamed.
        const std::uint32_t passes = steps > 0u ? steps - 1u : 0u;
        return step < passes ? (std::int32_t)(1u << (passes - 1u - step)) : 1;
    }

    // =====================================================================
    // PickNode
    // =====================================================================

    std::unique_ptr<PickNode> PickNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<PickNode> node(new PickNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool PickNode::Init(NriGraphContext& context)
    {
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();

        m_vs = context.ShaderBytecode("entity_id_vs");
        m_ps = context.ShaderBytecode("entity_id_ps");
        if (m_vs.empty() || m_ps.empty())
        {
            ARC_ERROR("[nri-graph] PickNode: the entity_id_vs/entity_id_ps bins are missing -- the "
                      "graph cannot render an entity-id pass");
            return false;
        }
        m_meshVs = context.ShaderBytecode("entity_id_mesh_vs");
        m_meshPs = context.ShaderBytecode("entity_id_mesh_ps");
        if (m_meshVs.empty() || m_meshPs.empty())
        {
            ARC_ERROR("[nri-graph] PickNode: the entity_id_mesh_vs/entity_id_mesh_ps bins are missing "
                      "-- the graph cannot render an entity-id pass");
            return false;
        }

        // The 2D vertex input. Attribute ORDER must match entity_id.hlsl's VSInput
        // member order: Vulkan locations are assigned by declaration order
        // there, and D3D matches the custom semantic name at SemanticIndex 0.
        // SHAPEPARAM covers (radius, halfLen) and KINDID covers (kind, id) --
        // two scalars each, packed as one two-component attribute, which is why
        // the offsets below name the FIRST member of each pair. POSITION is a
        // WORLD-space vec3 (F4 spec s7.1).
        m_attributes[0].d3d.semanticName = "POSITION";
        m_attributes[0].vk.location      = 0;
        m_attributes[0].offset           = offsetof(PickIdVertex, pos);
        m_attributes[0].format           = nri::Format::RGB32_SFLOAT;
        m_attributes[1].d3d.semanticName = "LOCAL";
        m_attributes[1].vk.location      = 1;
        m_attributes[1].offset           = offsetof(PickIdVertex, local);
        m_attributes[1].format           = nri::Format::RG32_SFLOAT;
        m_attributes[2].d3d.semanticName = "SHAPEPARAM";
        m_attributes[2].vk.location      = 2;
        m_attributes[2].offset           = offsetof(PickIdVertex, radius);
        m_attributes[2].format           = nri::Format::RG32_SFLOAT;
        m_attributes[3].d3d.semanticName = "KINDID";
        m_attributes[3].vk.location      = 3;
        m_attributes[3].offset           = offsetof(PickIdVertex, kind);
        m_attributes[3].format           = nri::Format::RG32_UINT;

        m_stream.bindingSlot = 0;
        m_stream.stepRate    = nri::VertexStreamStepRate::PER_VERTEX;
        m_stream.stride      = (std::uint16_t)sizeof(PickIdVertex);

        m_vertexInput.attributes   = m_attributes;
        m_vertexInput.attributeNum = (std::uint8_t)std::size(m_attributes);
        m_vertexInput.streams      = &m_stream;
        m_vertexInput.streamNum    = 1;

        // The MESH half's vertex input: MeshVertex (MeshBuilder.hpp), attribute
        // for attribute what MeshNode.cpp declares, because the buffers bound
        // are the SAME resident buffers MeshNode draws. entity_id_mesh.hlsl
        // declares NORMAL and TEXCOORD it never reads, so the layout matches.
        m_meshAttributes[0].d3d.semanticName = "POSITION";
        m_meshAttributes[0].vk.location      = 0;
        m_meshAttributes[0].offset           = offsetof(MeshVertex, position);
        m_meshAttributes[0].format           = nri::Format::RGB32_SFLOAT;
        m_meshAttributes[1].d3d.semanticName = "NORMAL";
        m_meshAttributes[1].vk.location      = 1;
        m_meshAttributes[1].offset           = offsetof(MeshVertex, normal);
        m_meshAttributes[1].format           = nri::Format::RGB32_SFLOAT;
        m_meshAttributes[2].d3d.semanticName = "TEXCOORD";
        m_meshAttributes[2].vk.location      = 2;
        m_meshAttributes[2].offset           = offsetof(MeshVertex, uv);
        m_meshAttributes[2].format           = nri::Format::RG32_SFLOAT;

        m_meshStream.bindingSlot = 0;
        m_meshStream.stepRate    = nri::VertexStreamStepRate::PER_VERTEX;
        m_meshStream.stride      = (std::uint16_t)sizeof(MeshVertex);

        m_meshVertexInput.attributes   = m_meshAttributes;
        m_meshVertexInput.attributeNum = (std::uint8_t)std::size(m_meshAttributes);
        m_meshVertexInput.streams      = &m_meshStream;
        m_meshVertexInput.streamNum    = 1;

        // THE LAYOUT: root constants b0 and NOTHING else. Neither entity_id.hlsl
        // nor entity_id_mesh.hlsl binds a texture, a sampler or a descriptor-set
        // constant buffer, so there is no descriptor set here at all -- and with
        // rootDescriptorNum and rootSamplerNum both zero, NRI's
        // rootRegisterSpace-vs-set-space guard (Source/Validation/DeviceVal.hpp)
        // has nothing to collide. ONE layout for BOTH pipelines: the two root
        // blocks are the same 80 bytes (the static_assert in the header), and
        // the stages are VERTEX + FRAGMENT because the mesh PS reads the id.
        //
        // Root constants lower to b0/space0 on D3D12 and to a VK push-constant
        // block, which is exactly what the shaders' `#if SPIRV
        // [[vk::push_constant]]` half declares.
        nri::RootConstantDesc rootConstant = {};
        rootConstant.registerIndex = 0;
        rootConstant.size          = sizeof(IdRootConstants);
        rootConstant.shaderStages  = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        // Value-initialized then assigned field by field -- NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so its padding has to
        // be zeroed).
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.rootConstants     = &rootConstant;
        layoutDesc.rootConstantNum   = 1;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        if (!m_pipelines->Layout(m_layoutId))
        {
            ARC_ERROR("[nri-graph] PickNode: pipeline layout registration failed");
            return false;
        }

        return CreateReadback();
    }

    bool PickNode::CreateReadback()
    {
        const nri::CoreInterface& core = m_device->Core();
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());

        // ONE REGION PER FRAME SLOT, each a legal CmdReadbackTextureToBuffer
        // destination on its own: TextureDataLayoutDesc documents rowPitch as a
        // multiple of uploadBufferTextureRow and BOTH slicePitch and the buffer
        // OFFSET as multiples of uploadBufferTextureSlice. A 1x1 R32_UINT copy
        // is 4 bytes before either alignment; rounding the row up and then the
        // region up to the slice alignment makes every region offset legal by
        // construction.
        m_readbackRow    = (std::uint32_t)AlignUp(4, deviceDesc.memoryAlignment.uploadBufferTextureRow);
        m_readbackStride = ReadbackRegionStride(m_readbackRow,
                                                deviceDesc.memoryAlignment.uploadBufferTextureSlice);
        m_readbackBytes  = m_readbackStride * FramesInFlight();

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = m_readbackBytes;
        bufferDesc.usage = nri::BufferUsageBits::NONE;   // copy destination only, like Readback.cpp's
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(),
                                                       nri::MemoryLocation::HOST_READBACK,
                                                       0.0f, bufferDesc, m_readback))
            || !m_readback)
        {
            ARC_ERROR("[nri-graph] PickNode: the {}-byte pick readback buffer could not be created",
                      m_readbackBytes);
            m_readback = nullptr;
            return false;
        }
        core.SetDebugName(m_readback, "nri-graph pick readback");

        // Persistently mapped, like the upload ring and Batch2DNode's arena --
        // and the same NONE-backend footgun: MapBuffer returns null there
        // unconditionally, so this node is a [gpu] path from here down.
        m_readbackCpu = core.MapBuffer(*m_readback, 0, nri::WHOLE_SIZE);
        if (!m_readbackCpu)
        {
            ARC_ERROR("[nri-graph] PickNode: the pick readback buffer could not be mapped (the NONE "
                      "backend cannot -- this node is a [gpu] path)");
            return false;
        }
        return true;
    }

    PickNode::~PickNode()
    {
        if (!m_device || !m_readback)
            return;

        ARC_WARN("[nri-graph] PickNode destroyed with live NRI objects -- either Create() failed part "
                 "way (an ERROR above says which step) or its owner never called Release(). "
                 "Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        if (m_readbackCpu) core.UnmapBuffer(*m_readback);
        core.DestroyBuffer(m_readback);
        m_readback    = nullptr;
        m_readbackCpu = nullptr;
    }

    void PickNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device || !m_readback)
            return;
        const nri::CoreInterface* core = &m_device->Core();
        if (m_readbackCpu)
        {
            core->UnmapBuffer(*m_readback);
            m_readbackCpu = nullptr;
        }
        graveyard.Bury(fence, [core, b = m_readback] { core->DestroyBuffer(b); });
        m_readback = nullptr;
    }

    void PickNode::PrepareDrawables(std::span<const PickDrawable> drawables, const ViewTransform& view,
                                    NriMeshBufferCache* meshBuffers, std::uint64_t frameCounter)
    {
        // NO VIEW, NOTHING RASTERISES -- FrameDesc::pickView's contract. A
        // default-constructed ViewTransform (zero viewport) is a frame that
        // armed the chain without saying where the camera is; its identity
        // matrices would otherwise put whatever happens to lie in the [-1,1]
        // metre cube on screen, which is not an answer to any click.
        m_meshDraws.clear();
        if (view.viewport.x == 0u || view.viewport.y == 0u)
        {
            m_vertices.clear();
            m_indices.clear();
            return;
        }

        // The SHARED emitter -- see the header's ONE EMITTER, TWO RECORDERS.
        BuildPickIdGeometry(drawables, m_vertices, m_indices);
        m_viewProj = view.ViewProjection();

        // The mesh half: resolve every Mesh drawable's resident buffers NOW, at
        // declaration time (Resolve may upload -- MeshNode::Prepare's rule).
        // The id is the drawable's index + 1, the SAME numbering the 2D half's
        // vertices carry, so both pipelines write into one id space.
        if (!meshBuffers)
            return;   // a vehicle with no mesh cache picks no meshes -- not an error
        for (std::size_t i = 0; i < drawables.size(); ++i)
        {
            const PickDrawable& d = drawables[i];
            if (d.kind != PickDrawable::Kind::Mesh)
                continue;
            const NriMeshBufferCache::Resident* resident = meshBuffers->Resolve(d.mesh, frameCounter);
            if (!resident || !resident->ready || !resident->vertexBuffer || !resident->indexBuffer
                || resident->indexCount == 0)
                continue;   // not resident -- skip, never a stale bind
            m_meshDraws.push_back({ resident, m_viewProj * d.world, (std::uint32_t)(i + 1) });
        }
    }

    void PickNode::Record(RenderGraphNodeContext& context, std::uint32_t frameSlot)
    {
        const nri::CoreInterface& core = context.core;
        (void)frameSlot;   // no per-frame-slot state here: the streams are ring allocations

        // THE CLEAR, both planes. This node still clears from its own exec fn
        // rather than through a declared load op (RenderGraph::
        // SetColorAttachmentOps / SetDepthAttachmentOp, F5 plan 1 T2): moving
        // it was out of that task's scope, and it is the one remaining
        // in-pass clear (a source scan in RenderGraphTest.cpp pins it, so its
        // removal is a deliberate act). 0 IS the background id, so the colour clear is not
        // cosmetic: every pixel no silhouette covers must read back as
        // "nothing here". The depth plane is the pass's OWN transient (F4 spec
        // s7.1), a fresh pool slot with undefined contents, cleared to the far
        // value so the mesh half's LESS test starts from "nothing drawn".
        //
        // The UINT half of the colour union, not the float one -- the target is
        // R32_UINT and a float clear would write a reinterpreted bit pattern.
        nri::ClearAttachmentDesc clears[2] = {};
        clears[0].planes                    = nri::PlaneBits::COLOR;
        clears[0].colorAttachmentIndex      = 0;
        clears[0].value.color.ui            = { 0u, 0u, 0u, 0u };
        clears[1].planes                    = nri::PlaneBits::DEPTH;
        clears[1].value.depthStencil.depth  = 1.0f;
        clears[1].value.depthStencil.stencil = 0;
        core.CmdClearAttachments(context.cmd, clears, 2, nullptr, 0);

        // A scene with nothing pickable is a cleared id target, not an error.
        // The two halves are INDEPENDENT: a mesh-only scene still draws its
        // meshes, a sprite-only scene still draws its quads.
        if (!m_vertices.empty() && !m_indices.empty())
            Record2D(context);
        if (!m_meshDraws.empty())
            RecordMeshes(context);
    }

    void PickNode::Record2D(RenderGraphNodeContext& context)
    {
        const nri::CoreInterface& core = context.core;

        // Vertex + index streams into THIS frame's ring slot, allocated HERE at
        // record time -- the vehicle calls ring.BeginFrame(slot) AFTER the frame
        // is declared, so a setup-time allocation would land in the previous
        // frame's slot and be overwritten while the GPU reads it.
        const std::uint64_t vertexBytes = m_vertices.size() * sizeof(PickIdVertex);
        const std::uint64_t indexBytes  = m_indices.size() * sizeof(std::uint32_t);

        // The VB's alignment is CHOSEN (16), not sizeof(PickIdVertex): the ring
        // asserts a power of two and the 36-byte stride is not one -- Batch2DNode's
        // kVertexAlign reasoning, verbatim (no backend wants a bind offset that is
        // a multiple of the stride; every attribute lands on its natural boundary).
        ARC_CONSTANT("hardware alignment: vertex upload alignment")
        constexpr std::uint64_t kVertexAlign = 16;
        const NriUploadRing::Alloc vertexAlloc = context.ring.Allocate(vertexBytes, kVertexAlign);
        const NriUploadRing::Alloc indexAlloc  = context.ring.Allocate(indexBytes, sizeof(std::uint32_t));
        if (!vertexAlloc.cpu || !indexAlloc.cpu)
        {
            if (!m_warnedRing)
            {
                m_warnedRing = true;
                GraphError("PickNode: the upload ring could not fit this frame's id-pass geometry ("
                           + std::to_string(vertexBytes + indexBytes)
                           + " bytes) -- the id pass is dropped. Raise "
                             "render.uploadRingBytesPerFrame.");
            }
            return;
        }
        std::memcpy(vertexAlloc.cpu, m_vertices.data(), (std::size_t)vertexBytes);
        std::memcpy(indexAlloc.cpu, m_indices.data(), (std::size_t)indexBytes);

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("PickNode: the pipeline layout is gone -- no id pass recorded");
            return;
        }

        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kShaderPairId;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = kGraphPickIdFormat;
        key.colorCount      = 1;
        // The depth attachment is BOUND for the whole pass, so this pipeline
        // must name its format; the 2D silhouettes neither test nor write it
        // (the fill below) -- later-wins by submission order, as before.
        key.depthFormat     = GraphDepthFormat();
        key.topology        = nri::Topology::TRIANGLE_LIST;
        // No blend: an R32_UINT target is integer and therefore unblendable.
        // Front-most wins by SUBMISSION ORDER -- the output merger is
        // primitive-ordered and the drawables are emitted back to front. The
        // meshes that follow (RecordMeshes) are the depth-tested half.
        key.blend           = NriPipelineCache::GraphicsKey::Blend::Opaque;
        key.depthWrite      = false;
        key.cullMode        = nri::CullMode::NONE;

        // `stages` lives in THIS frame, which encloses GetGraphics -- the fill
        // contract's rule 2.
        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_vs.data();
        stages[0].size           = m_vs.size();
        stages[0].entryPointName = kVsEntry;
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_ps.data();
        stages[1].size           = m_ps.size();
        stages[1].entryPointName = kPsEntry;

        nri::Pipeline* pipeline = m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            desc.vertexInput = &m_vertexInput;
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            desc.rasterization.fillMode = nri::FillMode::SOLID;
            // Depth-OFF: neither tested nor written. The attachment is bound
            // (the key names its format) so the mesh half can use it after.
            desc.outputMerger.depth.compareOp = nri::CompareOp::NONE;
        });
        if (!pipeline)
            return;   // already logged + latched by the cache

        // World -> clip through the frame's view-projection. No target-size
        // dependency anywhere: the supersampled extent only grows the
        // viewport, which the executor sets from the attachment.
        IdRootConstants push;
        push.viewProj = m_viewProj;

        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetRootConstantsDesc rootConstants = {};
        rootConstants.rootConstantIndex = 0;
        rootConstants.data              = &push;
        rootConstants.size              = sizeof(push);
        core.CmdSetRootConstants(context.cmd, rootConstants);

        nri::VertexBufferDesc vertexBuffer = {};
        vertexBuffer.buffer = vertexAlloc.buffer;
        vertexBuffer.offset = vertexAlloc.offset;
        vertexBuffer.stride = sizeof(PickIdVertex);
        core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);
        core.CmdSetIndexBuffer(context.cmd, *indexAlloc.buffer, indexAlloc.offset,
                                nri::IndexType::UINT32);

        core.CmdSetPipeline(context.cmd, *pipeline);

        // ONE draw for every silhouette in the scene -- the id is per-vertex, so
        // there is nothing per-drawable to rebind.
        nri::DrawIndexedDesc draw = {};
        draw.indexNum    = (std::uint32_t)m_indices.size();
        draw.instanceNum = 1;
        core.CmdDrawIndexed(context.cmd, draw);
    }

    void PickNode::RecordMeshes(RenderGraphNodeContext& context)
    {
        const nri::CoreInterface& core = context.core;

        // m_meshDraws holds BORROWED pointers into NriMeshBufferCache's map and
        // this function is their last reader, so the list is dropped on EVERY
        // exit path -- MeshNode::m_residents' discipline, for the same reason.
        struct DrawScope
        {
            std::vector<MeshDraw>* draws;
            ~DrawScope() { draws->clear(); }
        } drawScope{ &m_meshDraws };

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("PickNode: the pipeline layout is gone -- no mesh id pass recorded");
            return;
        }

        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kMeshShaderPairId;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = kGraphPickIdFormat;
        key.colorCount      = 1;
        key.depthFormat     = GraphDepthFormat();
        key.topology        = nri::Topology::TRIANGLE_LIST;
        key.blend           = NriPipelineCache::GraphicsKey::Blend::Opaque;
        key.depthWrite      = true;
        key.cullMode        = nri::CullMode::BACK;

        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_meshVs.data();
        stages[0].size           = m_meshVs.size();
        stages[0].entryPointName = kVsEntry;
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_meshPs.data();
        stages[1].size           = m_meshPs.size();
        stages[1].entryPointName = kPsEntry;

        nri::Pipeline* pipeline = m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            desc.vertexInput = &m_meshVertexInput;
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            // What MeshNode culls -- pick what is DRAWN: BACK faces of a
            // CCW-from-outside mesh, under the same front-face rule (MeshNode.cpp's
            // THE WINDING block; get the pair backwards and every closed convex
            // mesh becomes unpickable rather than subtly wrong).
            desc.rasterization.fillMode              = nri::FillMode::SOLID;
            desc.rasterization.frontCounterClockwise = true;
            // Depth-tested AND written, forward-Z [0,1] -- LESS is "nearer wins"
            // against the 1.0 the pass cleared to. This is what makes two
            // overlapping meshes resolve by depth, not by emission order.
            desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);
        });
        if (!pipeline)
            return;   // already logged + latched by the cache

        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);
        core.CmdSetPipeline(context.cmd, *pipeline);

        // One bind + one draw per drawable: the id rides in the root block, so
        // even two entities sharing a mesh are two draws (their ids differ).
        for (const MeshDraw& d : m_meshDraws)
        {
            MeshIdRootConstants push;
            push.mvp = d.mvp;
            push.id  = d.id;
            nri::SetRootConstantsDesc rootConstants = {};
            rootConstants.rootConstantIndex = 0;
            rootConstants.data              = &push;
            rootConstants.size              = sizeof(push);
            core.CmdSetRootConstants(context.cmd, rootConstants);

            nri::VertexBufferDesc vertexBuffer = {};
            vertexBuffer.buffer = d.resident->vertexBuffer;
            vertexBuffer.offset = 0;
            vertexBuffer.stride = sizeof(MeshVertex);
            core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);
            core.CmdSetIndexBuffer(context.cmd, *d.resident->indexBuffer, 0, nri::IndexType::UINT32);

            nri::DrawIndexedDesc draw = {};
            draw.indexNum    = d.resident->indexCount;
            draw.instanceNum = 1;
            core.CmdDrawIndexed(context.cmd, draw);
        }
    }

    void PickNode::RecordReadback(RenderGraphNodeContext& context, RgTexture ids, RgBuffer readback,
                                  std::int32_t probeX, std::int32_t probeY,
                                  std::uint32_t width, std::uint32_t height,
                                  std::uint32_t frameSlot, std::uint64_t ticket)
    {
        if (frameSlot >= FramesInFlight())
        {
            GraphError("PickNode: the readback node was handed a frame slot outside the ring");
            return;
        }

        // ---------------------------------------------------------------
        // DRAIN FIRST, RECORD SECOND -- the whole synchronisation argument,
        // stated in full at the top of the header. The executor acquired the
        // backbuffer before it reset this frame's command allocator, and the
        // pacing wait inside that acquire is what makes THIS slot safe to
        // reuse: whatever was copied into it FramesInFlight() frames
        // ago has retired. No fence query, no idle.
        // ---------------------------------------------------------------
        if (m_pending[frameSlot] && m_readbackCpu)
        {
            std::uint32_t id = 0;
            std::memcpy(&id,
                        static_cast<const std::uint8_t*>(m_readbackCpu) + frameSlot * m_readbackStride,
                        sizeof(id));
            m_probeId  = id;
            // Published TOGETHER with the id, from the same slot, so the pair a
            // host reads is always the pair one copy produced.
            m_probeTicket = m_ticket[frameSlot];
            m_hasProbe = true;
            m_pending[frameSlot] = false;
        }

        const nri::CoreInterface& core = context.core;
        nri::Texture* source = context.Resolve(ids);
        nri::Buffer*  dest   = context.Resolve(readback);
        if (!source || !dest)
        {
            GraphError("PickNode: the readback node could not resolve its id target or its staging "
                       "buffer");
            return;
        }

        // A zero extent has no texel to copy, and PickSampleTexel would clamp
        // into an empty range. Unreachable through the vehicle (RenderFrame
        // skips a zero-sized surface before it declares anything), which is
        // exactly why it is checked rather than assumed: this is the one place
        // that would turn that guard's removal into undefined behaviour instead
        // of a visible failure.
        if (width == 0 || height == 0)
            return;

        // The centre subsample of the probed 1x pixel, clamped to the target --
        // the SAME mapping PickBuffer::Pick uses, so a probe and an editor click
        // at the same pixel read the same texel.
        const glm::ivec2 texel = PickSampleTexel(glm::vec2((float)probeX, (float)probeY),
                                                  SuperSample(),
                                                  width * SuperSample(), height * SuperSample());

        nri::TextureDataLayoutDesc layout = {};
        layout.offset     = frameSlot * m_readbackStride;
        layout.rowPitch   = m_readbackRow;
        layout.slicePitch = (std::uint32_t)m_readbackStride;

        nri::TextureRegionDesc region = {};
        region.x      = (nri::Dim_t)texel.x;
        region.y      = (nri::Dim_t)texel.y;
        region.width  = 1;
        region.height = 1;
        region.depth  = 1;

        core.CmdReadbackTextureToBuffer(context.cmd, *dest, layout, *source, region);
        m_pending[frameSlot]  = true;
        // Written with the pending flag and never without it, so a drain can
        // only ever read a ticket the matching copy set.
        m_ticket[frameSlot]   = ticket;
    }

    // =====================================================================
    // OutlineNode
    // =====================================================================

    std::unique_ptr<OutlineNode> OutlineNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<OutlineNode> node(new OutlineNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool OutlineNode::Init(NriGraphContext& context)
    {
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();

        struct Bin { const char* name; std::span<const std::uint8_t>* out; };
        const Bin bins[] = {
            { "outline_seed_vs",      &m_seedVs },      { "outline_seed_ps",      &m_seedPs },
            { "outline_jfa_vs",       &m_jfaVs },       { "outline_jfa_ps",       &m_jfaPs },
            { "outline_composite_vs", &m_compositeVs }, { "outline_composite_ps", &m_compositePs },
        };
        for (const Bin& bin : bins)
        {
            *bin.out = context.ShaderBytecode(bin.name);
            if (bin.out->empty())
            {
                ARC_ERROR("[nri-graph] OutlineNode: shader bin '{}' is missing -- the outline chain "
                          "cannot be built", bin.name);
                return false;
            }
        }

        // Reserved, not merely sized: PrepareSelection runs every frame of a
        // probe run and a steady-state frame must not allocate.
        m_selectedIds.reserve(kMaxSelectedIds);

        return CreateBindings() && CreateConstantArena();
    }

    bool OutlineNode::CreateBindings()
    {
        const nri::CoreInterface& core = m_device->Core();

        // ONE LAYOUT FOR ALL THREE PASSES. outline_seed/jfa/composite each
        // declare exactly `cbuffer ... : register(b0)` + one
        // `Texture2D<...> : register(t0)`, read from the PIXEL stage, and none
        // of them declares a sampler (every fetch is a `.Load`). So the shape is
        // identical and the cache's byte-wise dedup collapses the three
        // registrations into one -- which is also why RegisterLayout is called
        // once here rather than three times.
        //
        // RANGE ORDER IS DELIBERATE: NRI's D3D12 backend merges CONSECUTIVE
        // ranges of the same range type into one root table, so the CBV before
        // the SRV costs two root parameters and never more. On Vulkan the b0/t0
        // register indices are shifted by NRI itself using the device's
        // vkBindingOffsets (b=256, t=0), which NriDevice pins to
        // ShaderConventions.hpp's dxc -fvk-*-shift values with static_asserts --
        // so this desc is written in HLSL register numbers on both backends.
        //
        // No root constants at ALL here, and that is the source-verified
        // correction the header states: these three shaders have no
        // push-constant variant, so a root constant would leave their uniform
        // block unwritten on Vulkan.
        nri::DescriptorRangeDesc ranges[2] = {};
        ranges[0].baseRegisterIndex = 0;   // b0
        ranges[0].descriptorNum     = 1;
        ranges[0].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        ranges[0].shaderStages      = nri::StageBits::FRAGMENT_SHADER;
        ranges[1].baseRegisterIndex = 0;   // t0
        ranges[1].descriptorNum     = 1;
        ranges[1].descriptorType    = nri::DescriptorType::TEXTURE;
        ranges[1].shaderStages      = nri::StageBits::FRAGMENT_SHADER;

        nri::DescriptorSetDesc setDesc = {};
        setDesc.registerSpace = 0;
        setDesc.ranges        = ranges;
        setDesc.rangeNum      = 2;

        // Value-initialized then assigned field by field -- the DEDUP CONTRACT.
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.descriptorSets    = &setDesc;
        layoutDesc.descriptorSetNum  = 1;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] OutlineNode: pipeline layout registration failed");
            return false;
        }

        // ONE SET PER (region, frame slot). Both halves are load-bearing: the
        // REGION because each pass reads its own constants and its own source
        // texture, and the FRAME SLOT because a set's contents must not be
        // rewritten while an earlier submission may still be reading it --
        // exactly the mechanism FullscreenNodes documents.
        const std::uint32_t setCount = FramesInFlight() * kCbRegionsPerFrame;
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum  = setCount;
        poolDesc.textureMaxNum        = setCount;
        poolDesc.constantBufferMaxNum = setCount;
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] OutlineNode: descriptor pool creation failed");
            return false;
        }
        for (std::uint32_t i = 0; i < setCount; ++i)
        {
            if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0, &m_sets[i], 1, 0))
                || !m_sets[i])
            {
                ARC_ERROR("[nri-graph] OutlineNode: descriptor set allocation failed at {} of {}",
                          i, setCount);
                return false;
            }
        }
        return true;
    }

    bool OutlineNode::CreateConstantArena()
    {
        const nri::CoreInterface& core = m_device->Core();
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());

        // The alignment fixes both the stride AND the size every CB view is
        // created with: NRI passes BufferViewDesc::size straight into
        // D3D12_CONSTANT_BUFFER_VIEW_DESC::SizeInBytes, which D3D12 requires to
        // be a multiple of 256 -- so the views name a whole region and the
        // shader simply reads less than it.
        m_arenaStride = CbRegionStride(deviceDesc.memoryAlignment.constantBufferOffset);

        const std::uint32_t regionCount = FramesInFlight() * kCbRegionsPerFrame;
        const std::uint64_t arenaBytes = (std::uint64_t)regionCount * m_arenaStride;

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = arenaBytes;
        bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                       0.0f, bufferDesc, m_arena))
            || !m_arena)
        {
            ARC_ERROR("[nri-graph] OutlineNode: the outline constant-buffer arena ({} bytes) could "
                      "not be created", arenaBytes);
            return false;
        }
        core.SetDebugName(m_arena, "nri-graph outline CBs");

        // Persistent map, unmapped once in Release()/~OutlineNode -- same shape,
        // and same NONE-backend footgun, as Batch2DNode's arena.
        m_arenaCpu = core.MapBuffer(*m_arena, 0, nri::WHOLE_SIZE);
        if (!m_arenaCpu)
        {
            ARC_ERROR("[nri-graph] OutlineNode: the outline constant-buffer arena could not be "
                      "mapped (the NONE backend cannot -- this node is a [gpu] path)");
            return false;
        }

        // One CB view per region, created ONCE and written into its set once:
        // the CONTENTS change every frame, the (buffer, offset) never does,
        // which is exactly what lets the set naming it be written once too.
        for (std::uint32_t slot = 0; slot < FramesInFlight(); ++slot)
        {
            for (std::uint32_t region = 0; region < kCbRegionsPerFrame; ++region)
            {
                const std::uint32_t index = SetIndex(slot, region);

                nri::BufferViewDesc viewDesc = {};
                viewDesc.buffer = m_arena;
                viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
                viewDesc.offset = ArenaOffset(slot, region);
                viewDesc.size   = m_arenaStride;
                if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, m_cbView[index])) || !m_cbView[index])
                {
                    ARC_ERROR("[nri-graph] OutlineNode: the constant-buffer view for frame slot {} "
                              "region {} could not be created", slot, region);
                    return false;
                }

                const nri::Descriptor* cb = m_cbView[index];
                nri::UpdateDescriptorRangeDesc update = {};
                update.descriptorSet = m_sets[index];
                update.rangeIndex    = 0;
                update.descriptors   = &cb;
                update.descriptorNum = 1;
                core.UpdateDescriptorRanges(&update, 1);
            }
        }
        return true;
    }

    OutlineNode::~OutlineNode()
    {
        if (!m_device || (!m_pool && !m_arena && m_views.empty()))
            return;

        ARC_WARN("[nri-graph] OutlineNode destroyed with live NRI objects -- either Create() failed "
                 "part way (an ERROR above says which step) or its owner never called Release(). "
                 "Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (const SourceView& cached : m_views)
            if (cached.view) core.DestroyDescriptor(cached.view);
        m_views.clear();
        if (m_pool) core.DestroyDescriptorPool(m_pool);
        for (nri::Descriptor*& view : m_cbView)
            if (view) { core.DestroyDescriptor(view); view = nullptr; }
        if (m_arena)
        {
            if (m_arenaCpu) core.UnmapBuffer(*m_arena);
            core.DestroyBuffer(m_arena);
        }
        m_pool = nullptr; m_arena = nullptr; m_arenaCpu = nullptr;
        for (nri::DescriptorSet*& set : m_sets) set = nullptr;
        for (nri::Texture*& bound : m_bound)    bound = nullptr;
    }

    void OutlineNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        InvalidateSources(graveyard, fence);

        const nri::CoreInterface* core = &m_device->Core();

        // The POOL first (its sets are what name the views below and are not
        // separately destroyable), then descriptors before the resource they
        // view -- the graveyard runs burials in order, so a CB view can never
        // outlive the arena it names. Byte-for-byte Batch2DNode::Release's
        // ordering, deliberately: this is the third node on the path with an
        // arena and a pool, and three different teardown orders would be three
        // different things to verify.
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
            for (nri::DescriptorSet*& set : m_sets)
                set = nullptr;   // owned by the pool, buried above
        }
        for (nri::Descriptor*& view : m_cbView)
        {
            if (!view)
                continue;
            graveyard.Bury(fence, [core, d = view] { core->DestroyDescriptor(d); });
            view = nullptr;
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
    }

    void OutlineNode::InvalidateSources(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();
        for (const SourceView& cached : m_views)
            if (cached.view)
                graveyard.Bury(fence, [core, view = cached.view] { core->DestroyDescriptor(view); });
        m_views.clear();
        // The sets still NAME the buried views, so nothing may bind one until a
        // Record rewrites the texture range -- which clearing this forces.
        for (nri::Texture*& bound : m_bound)
            bound = nullptr;
    }

    void OutlineNode::SyncPoolEpoch(const RenderGraphNodeContext& context)
    {
        // Mechanism 1 (FullscreenNodes.hpp, SOURCE VIEWS AND THE POOL).
        // RealizePool may have buried a pool texture THIS Execute() -- a shrink
        // or a desc change -- and it runs between the declarations and this exec
        // fn, so there was no earlier point at which the owner could have told
        // us. The epoch is the only observable; a pointer comparison cannot see
        // it, because NRI may hand the recreated texture the vacated address.
        //
        // REACHABLE HERE, not theoretical: every texture this chain samples --
        // the id target and both field ping-pong slots -- is declared at an
        // extent derived from the canvas, so a resize genuinely re-shapes the
        // pool and recreates the physicals these views name. (The STEP COUNT no
        // longer moves with the extent as of D3c; the extents still do, which is
        // what this guards.)
        if (!context.graph)
            return;
        SyncPoolEpoch(*context.graph);
    }

    void OutlineNode::SyncPoolEpoch(const RenderGraph& graph)
    {
        if (!m_device)
            return;
        // The GRAPH's lane rather than the device's: an exec fn runs inside
        // that graph's Execute, so its lane is where these views belong -- the
        // same lane the pool retirement that moved the epoch stages into. The
        // owner's declaration-time call names the same graph and the same
        // lane, one step earlier, which is what covers the frames this node is
        // NOT in.
        Graveyard* graves = graph.Graves();
        if (!graves)
            return;   // from an exec fn: unreachable (Execute latches the lane before recording).
                      // From the owner at declaration time: the graph has never Executed, so it
                      // owns no pool and this node can hold no view over one.
        const std::uint64_t epoch = graph.PoolEpoch();
        if (epoch == m_poolEpoch)
            return;
        m_poolEpoch = epoch;
        // Buried, not destroyed: an earlier submitted frame may still be reading
        // them. DebugSubmitCount() is the submission that last used them -- the
        // same value the graph keys its OWN burials to, which is what keeps
        // Graveyard's nondecreasing rule satisfied. Views before resources: this
        // buries ONLY views, and the arena/pool burials in Release() happen
        // after this call there too.
        InvalidateSources(*graves, graph.DebugSubmitCount());
    }

    nri::Descriptor* OutlineNode::EnsureView(const nri::CoreInterface& core, nri::Texture* texture)
    {
        for (const SourceView& cached : m_views)
            if (cached.texture == texture)
                return cached.view;

        // The chain samples the id target plus at most two ping-pong physicals,
        // so a cache that keeps growing means pool textures are being destroyed
        // without the epoch moving -- a bug in RenderGraph rather than here.
        // Say so once instead of growing silently.
        if (m_views.size() >= 6 && !m_warnedViewChurn)
        {
            m_warnedViewChurn = true;
            GraphError("OutlineNode: more distinct source textures than one frame can have -- pool "
                       "textures are being replaced without RenderGraph::PoolEpoch moving");
        }

        nri::Descriptor* view = nullptr;
        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        // The transient's ACTUAL format, read back from NRI -- never assumed.
        // It differs between passes here (R32_UINT for the id target,
        // RGBA16_SNORM for the field), which is exactly why it is read.
        viewDesc.format   = core.GetTextureDesc(*texture).format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, view)) || !view)
        {
            GraphError("OutlineNode: a source shader-resource view could not be created");
            return nullptr;
        }
        m_views.push_back(SourceView{ texture, view });
        return view;
    }

    bool OutlineNode::RecordPass(RenderGraphNodeContext& context, std::uint32_t region,
                                 std::uint64_t shaderPairId,
                                 std::span<const std::uint8_t> vs, std::span<const std::uint8_t> ps,
                                 RgTexture source, RgTexture target,
                                 NriPipelineCache::GraphicsKey::Blend blend,
                                 const void* cb, std::size_t cbBytes, std::uint32_t frameSlot)
    {
        const nri::CoreInterface& core = context.core;

        if (frameSlot >= FramesInFlight() || region >= kCbRegionsPerFrame)
        {
            GraphError("OutlineNode: a pass was handed a frame slot or region outside the arena");
            return false;
        }
        const std::uint32_t index = SetIndex(frameSlot, region);
        if (!m_sets[index] || !m_arenaCpu)
        {
            GraphError("OutlineNode: no descriptor set or constant arena for this pass");
            return false;
        }

        nri::Texture* sourceTexture = context.Resolve(source);
        nri::Texture* targetTexture = context.Resolve(target);
        if (!sourceTexture || !targetTexture)
        {
            GraphError("OutlineNode: a pass could not resolve its source or its target");
            return false;
        }

        // Bind the source at t0, creating the view on first sight. Only THIS
        // frame slot's set is touched, and its previous submission has already
        // retired (the pacing wait inside AcquireNextTexture) -- which is what
        // makes a changed source ordinary rather than a hazard needing an idle.
        if (m_bound[index] != sourceTexture)
        {
            nri::Descriptor* view = EnsureView(core, sourceTexture);
            if (!view)
                return false;   // already reported

            const nri::Descriptor* bound = view;
            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = m_sets[index];
            update.rangeIndex    = 1;
            update.descriptors   = &bound;
            update.descriptorNum = 1;
            core.UpdateDescriptorRanges(&update, 1);
            m_bound[index] = sourceTexture;
        }

        // The constants, into THIS frame slot's region. HOST_UPLOAD memory is
        // host-coherent, so the memcpy is the whole of the upload -- no barrier,
        // no flush, exactly as the ring's streams need none.
        std::memcpy(static_cast<std::uint8_t*>(m_arenaCpu) + ArenaOffset(frameSlot, region),
                    cb, cbBytes);

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("OutlineNode: the pipeline layout is gone -- nothing recorded");
            return false;
        }

        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = shaderPairId;
        key.layoutId        = m_layoutId;
        // Read back from the resolved texture rather than assumed: the seed and
        // JFA targets are the graph's own RGBA16_SNORM transients, but the
        // composite's target is the BACKBUFFER, whose channel order NRI resolves
        // instead of letting anyone pin it (NriSwapChain::Format).
        key.colorFormats[0] = core.GetTextureDesc(*targetTexture).format;
        key.colorCount      = 1;
        key.depthFormat     = nri::Format::UNKNOWN;
        key.topology        = nri::Topology::TRIANGLE_LIST;
        key.blend           = blend;
        key.depthWrite      = false;
        key.cullMode        = nri::CullMode::NONE;

        // `stages` lives in THIS frame, which encloses GetGraphics -- rule 2.
        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = vs.data();
        stages[0].size           = vs.size();
        stages[0].entryPointName = kVsEntry;
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = ps.data();
        stages[1].size           = ps.size();
        stages[1].entryPointName = kPsEntry;

        nri::Pipeline* pipeline = m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            // No vertex input at all: every one of these VS entry points builds
            // the fullscreen triangle from SV_VertexID.
            desc.vertexInput = nullptr;
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            desc.rasterization.fillMode = nri::FillMode::SOLID;
        });
        if (!pipeline)
            return false;   // already logged + latched by the cache

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetDescriptorSetDesc setDesc = {};
        setDesc.setIndex      = 0;
        setDesc.descriptorSet = m_sets[index];
        core.CmdSetDescriptorSet(context.cmd, setDesc);

        core.CmdSetPipeline(context.cmd, *pipeline);

        nri::DrawDesc draw = {};
        draw.vertexNum   = 3;
        draw.instanceNum = 1;
        core.CmdDraw(context.cmd, draw);
        return true;
    }

    void OutlineNode::PrepareSelection(std::span<const std::uint32_t> selectedIds)
    {
        // COPIED, not borrowed -- the driver's span is published for the span of
        // one BuildFrame and RecordSeed runs later. Capped here rather than at
        // record time so the WARN fires once per overflow rather than once per
        // frame of a large selection.
        const std::size_t count = std::min<std::size_t>(selectedIds.size(), kMaxSelectedIds);
        if (selectedIds.size() > kMaxSelectedIds && !m_warnedIdOverflow)
        {
            m_warnedIdOverflow = true;
            ARC_WARN("[nri-graph] OutlineNode: {} selected ids exceeds the {} the seed CB holds -- "
                     "outlining the first {}", selectedIds.size(), kMaxSelectedIds, kMaxSelectedIds);
        }
        m_selectedIds.assign(selectedIds.begin(), selectedIds.begin() + count);
    }

    void OutlineNode::RecordSeed(RenderGraphNodeContext& context, RgTexture ids, RgTexture target,
                                 std::int32_t cursorX, std::int32_t cursorY,
                                 std::uint32_t superSample, std::uint32_t width,
                                 std::uint32_t height, std::uint32_t frameSlot)
    {
        // BEFORE anything reads m_views: this Execute() may already have buried
        // a pool texture one of them names.
        SyncPoolEpoch(context);

        SeedCB cb{};
        const std::size_t idCount = std::min<std::size_t>(m_selectedIds.size(), kMaxSelectedIds);
        cb.selectedCount = (std::uint32_t)idCount;
        for (std::size_t i = 0; i < idCount; ++i)
            cb.selectedIds[i] = m_selectedIds[i];
        cb.cursorX     = cursorX;
        cb.cursorY     = cursorY;
        cb.superSample = superSample == 0 ? 1u : superSample;
        cb.dimX        = (std::int32_t)width;
        cb.dimY        = (std::int32_t)height;

        (void)RecordPass(context, kSeedRegion, kSeedPairId, m_seedVs, m_seedPs, ids, target,
                         NriPipelineCache::GraphicsKey::Blend::Opaque, &cb, sizeof(cb), frameSlot);
    }

    void OutlineNode::RecordJfa(RenderGraphNodeContext& context, std::uint32_t step, std::int32_t jump,
                                RgTexture source, RgTexture target, std::uint32_t width,
                                std::uint32_t height, std::uint32_t frameSlot)
    {
        SyncPoolEpoch(context);

        if (step >= kMaxJfaSteps)
        {
            GraphError("OutlineNode: a jump-flood step past kMaxJfaSteps reached the recorder -- the "
                       "declarator should have clamped it");
            return;
        }

        JfaCB cb{};
        cb.jump = jump;
        cb.dimX = (std::int32_t)width;
        cb.dimY = (std::int32_t)height;

        (void)RecordPass(context, kJfaRegionBase + step, kJfaPairId, m_jfaVs, m_jfaPs, source, target,
                         NriPipelineCache::GraphicsKey::Blend::Opaque, &cb, sizeof(cb), frameSlot);
    }

    void OutlineNode::RecordComposite(RenderGraphNodeContext& context, RgTexture field,
                                      RgTexture target, std::uint32_t width, std::uint32_t height,
                                      std::uint32_t frameSlot)
    {
        SyncPoolEpoch(context);

        // A width past the latched ceiling would read the field where it is
        // deliberately empty, so the ceiling caps both.
        const RenderOutlineSettings& o = Settings<RenderOutlineSettings>();
        const float ceiling = static_cast<float>(OutlineMaxThicknessPx());
        CompositeCB cb{};
        cb.selectThick = std::min(o.selectWidthPx, ceiling);
        cb.hoverThick  = std::min(o.hoverWidthPx,  ceiling);
        cb.edgeSoft    = o.edgeSoftnessPx;
        cb.dimX        = (std::int32_t)width;
        cb.dimY        = (std::int32_t)height;
        std::memcpy(cb.selectColor, m_selectColor, sizeof(m_selectColor));
        std::memcpy(cb.hoverColor,  m_hoverColor,  sizeof(m_hoverColor));

        // AlphaOver, matching SelectionOutline's composite blend state
        // (SrcAlpha / InvSrcAlpha for colour, One / InvSrcAlpha for alpha):
        // outline texels return a possibly translucent colour and everything
        // else discards, so the AA ring composites over the target.
        (void)RecordPass(context, kCompositeRegion, kCompositePairId, m_compositeVs, m_compositePs,
                         field, target, NriPipelineCache::GraphicsKey::Blend::AlphaOver,
                         &cb, sizeof(cb), frameSlot);
    }

    // =====================================================================
    // The declarators
    // =====================================================================

    RgPickHandles AddPickNodes(RenderGraph& graph, NriGraphContext* context,
                               std::uint32_t width, std::uint32_t height)
    {
        // Built at DECLARATION time on purpose, exactly like
        // AddBatch2DNode's Drain: the 2D half is pure CPU work (a quad
        // expansion over the scene's pickables) with no ring allocation in it,
        // and the mesh half's residency resolve may UPLOAD (MeshNode::Prepare's
        // rule) -- so both happen here and the exec fn only records.
        PickNode* node = context ? context->Pick() : nullptr;
        if (node && context)
            node->PrepareDrawables(context->CurrentPickables(), context->CurrentPickView(),
                                   context->MeshBuffers(), context->PresentedFrames());

        // Shared rather than captured by value: all three handles are minted
        // INSIDE a setup fn, which AddNode runs after both lambdas are constructed.
        auto ids      = std::make_shared<RgTexture>();
        auto depth    = std::make_shared<RgTexture>();
        auto readback = std::make_shared<RgBuffer>();

        graph.AddNode("pick", RenderGraph::NodeKind::Raster,
            [&graph, ids, depth, width, height](RenderGraphBuilder& builder)
            {
                RgTextureDesc desc;
                desc.format = kGraphPickIdFormat;
                // SUPERSAMPLED: the extent is the only thing that grows (the
                // root constants stay logical and the executor takes the
                // viewport from the attachment), so the same silhouettes
                // rasterise at ss x density and outline_seed.hlsl gets the
                // sub-pixel coverage its centroid needs. See PickNode::SuperSample().
                desc.width  = width  * PickNode::SuperSample();
                desc.height = height * PickNode::SuperSample();
                desc.hasOptimizedClearValue = true; // PickNode clears ids to uint zero.
                *ids = builder.CreateTexture("pickids", desc);
                builder.Write(*ids, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(ids.get(), 1));

                // THE PASS'S OWN DEPTH (spec s7.1), at the id target's extent:
                // the mesh half resolves overlapping meshes by depth INSIDE the
                // pick pass, never against the mesh node's depth (that one
                // belongs to the canvas, at 1x). Written and attached here,
                // read by nothing else -- its whole lifetime is this node.
                RgTextureDesc depthDesc;
                depthDesc.format       = GraphDepthFormat();
                depthDesc.width        = width  * PickNode::SuperSample();
                depthDesc.height       = height * PickNode::SuperSample();
                depthDesc.depthStencil = true;
                depthDesc.optimizedClearValue.depthStencil.depth = 1.0f;
                depthDesc.hasOptimizedClearValue = true;
                *depth = builder.CreateTexture("pickdepth", depthDesc);
                builder.Write(*depth, RgUsage::DepthWrite);
                graph.SetDepthAttachment(*depth);
            },
            [context](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                if (PickNode* pick = context->Pick())
                    pick->Record(nodeContext, context->FrameSlot());
            });

        // A COPY node, so the executor opens no rendering for it: the id
        // target's COLOR_ATTACHMENT -> COPY_SOURCE transition and the staging
        // buffer's -> COPY_DESTINATION are both DERIVED from these
        // declarations. No node on this path records a barrier.
        //
        // The staging buffer is IMPORTED rather than created as a transient for
        // the same reason the capture buffer is: the graph realizes transients
        // in MemoryLocation::DEVICE (RealizePool), which can never be mapped.
        // RgUsage::ReadbackHost is the declaration that says "this buffer is the
        // host's to read".
        graph.AddNode("pickreadback", RenderGraph::NodeKind::Copy,
            [context, ids, readback](RenderGraphBuilder& builder)
            {
                PickNode* pick = context ? context->Pick() : nullptr;
                *readback = builder.ImportBuffer("pickreadback",
                                                  pick ? pick->ReadbackBuffer() : nullptr,
                                                  pick ? pick->ReadbackBytes() : 0);
                builder.Read(*ids, RgUsage::CopySrc);
                builder.Write(*readback, RgUsage::ReadbackHost);
            },
            [context, ids, readback, width, height](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                if (PickNode* pick = context->Pick())
                    pick->RecordReadback(nodeContext, *ids, *readback,
                                          // THIS FRAME's probe pixel, which
                                          // defaults to the --pick-probe one
                                          // when the driver named none -- so
                                          // the Phase-2 probe path reads
                                          // exactly as it did.
                                          context->PickPixelX(), context->PickPixelY(),
                                          width, height, context->FrameSlot(),
                                          context->CurrentPickTicket());
            });

        return RgPickHandles{ *ids, *depth, *readback };
    }

    RgTexture AddOutlineNodes(RenderGraph& graph, NriGraphContext* context, RgTexture ids,
                              std::uint32_t width, std::uint32_t height)
    {
        // The selection is COPIED here, at DECLARATION time, for the same
        // reason AddPickNodes prepares its geometry here: the driver's span is
        // published for the span of one BuildFrame and every Record runs later.
        if (OutlineNode* node = context ? context->Outline() : nullptr)
        {
            if (context->Packet())
                node->PrepareSelection(context->Packet()->selectedIds);
            else
                node->PrepareSelection(context->CurrentSelectedIds());
        }

        // Shared rather than captured by value: a later step reads an EARLIER
        // step's target, and every handle is minted inside its own setup fn.
        auto seed = std::make_shared<RgTexture>();

        graph.AddNode("outlineseed", RenderGraph::NodeKind::Raster,
            [&graph, seed, ids, width, height](RenderGraphBuilder& builder)
            {
                RgTextureDesc desc;
                desc.format = kGraphOutlineFieldFormat;
                desc.width  = width;
                desc.height = height;
                *seed = builder.CreateTexture("outlineseed", desc);
                builder.Read(ids, RgUsage::ShaderRead);
                builder.Write(*seed, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(seed.get(), 1));
            },
            [context, seed, ids, width, height](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                if (OutlineNode* node = context->Outline())
                    node->RecordSeed(nodeContext, ids, *seed,
                                      // THE HOVER CURSOR, which is a DIFFERENT
                                      // per-frame value from the probe pixel
                                      // above wherever a host has both (the
                                      // editor hovers continuously and probes
                                      // only on a click). Both fall back to the
                                      // --pick-probe pixel, which is what makes
                                      // that run still show cyan under its own
                                      // probe.
                                      context->HoverX(), context->HoverY(), PickNode::SuperSample(),
                                      width, height, context->FrameSlot());
            });

        // EXTENT-INDEPENDENT: the schedule is derived from the outline's max
        // thickness, so it is identical on every surface size. See
        // OutlineJfaStepCount.
        const std::uint32_t steps =
            std::min(OutlineJfaStepCount(OutlineMaxThicknessPx()), OutlineNode::kMaxJfaSteps);

        // Each step CREATES its own transient and the pool allocator collapses
        // them onto two physical textures -- step k overlaps step k-1 (both live
        // at node k) so they never share, while step k and step k-2 are disjoint
        // and do. Declaring two ping-pong textures by hand would have produced
        // the same picture with three pool slots. See FullscreenNodes.hpp, THE
        // PING-PONG IS DERIVED.
        auto targets = std::make_shared<std::vector<RgTexture>>(steps);
        for (std::uint32_t step = 0; step < steps; ++step)
        {
            const RgTexture source = step == 0 ? *seed : (*targets)[step - 1];
            const std::string name = "outlinejfa" + std::to_string(step);
            // Jumps halve from 2^(steps-2) down to 1 and then repeat 1 once --
            // SelectionOutline::Render's schedule, expression included.
            const std::int32_t jump = OutlineJfaJump(step, steps);

            graph.AddNode(name.c_str(), RenderGraph::NodeKind::Raster,
                [&graph, targets, source, step, width, height](RenderGraphBuilder& builder)
                {
                    RgTextureDesc desc;
                    desc.format = kGraphOutlineFieldFormat;
                    desc.width  = width;
                    desc.height = height;
                    (*targets)[step] = builder.CreateTexture(
                        ("outlinejfa" + std::to_string(step)).c_str(), desc);
                    builder.Read(source, RgUsage::ShaderRead);
                    builder.Write((*targets)[step], RgUsage::ColorWrite);
                    graph.SetColorAttachments(std::span<const RgTexture>(&(*targets)[step], 1));
                },
                [context, targets, source, step, jump, width, height](RenderGraphNodeContext& nodeContext)
                {
                    if (!context)
                        return;   // device-less declaration-shape drive
                    if (OutlineNode* node = context->Outline())
                        node->RecordJfa(nodeContext, step, jump, source, (*targets)[step],
                                         width, height, context->FrameSlot());
                });
        }

        // NO CLEAR anywhere above -- no clear load op is declared and no exec fn
        // clears -- deliberately (see DeclareGraphFrame's THE CLEAR IS
        // DECLARATIVE): every pass here is an OPAQUE fullscreen triangle
        // that writes every pixel of its target, so a LOADed undefined pool slot
        // is fully overwritten before anything can read it. The ID target is the
        // exception and clears itself -- 0 is a meaningful value there.
        return steps > 0 ? (*targets)[steps - 1] : *seed;
    }

    void AddOutlineCompositeNode(RenderGraph& graph, NriGraphContext* context,
                                 RgTexture field, RgTexture target,
                                 std::uint32_t width, std::uint32_t height)
    {
        graph.AddNode("outlinecomposite", RenderGraph::NodeKind::Raster,
            [&graph, field, target](RenderGraphBuilder& builder)
            {
                builder.Read(field, RgUsage::ShaderRead);
                // The target is the imported backbuffer the tonemap has already
                // written, and this declares the SAME ColorWrite state -- so the
                // compile derives no transition between the two nodes, which
                // is correct: a barrier between equal non-UAV states is a
                // no-op. The blend
                // reads the destination through the ROP, which
                // AccessBits::COLOR_ATTACHMENT already covers (it is the
                // _READ|_WRITE umbrella -- RenderGraph.cpp's usage table).
                builder.Write(target, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(&target, 1));
            },
            [context, field, target, width, height](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                if (OutlineNode* node = context->Outline())
                    node->RecordComposite(nodeContext, field, target, width, height,
                                           context->FrameSlot());
            });
    }
}
