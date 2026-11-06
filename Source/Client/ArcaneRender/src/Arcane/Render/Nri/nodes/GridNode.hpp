#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// GridNode -- THE 3D REFERENCE GRID as a render graph node (F4 plan 1 Task
// 10, spec s5.2).
//
// An ANALYTIC grid (data/shaders/grid.hlsl) on one large ground quad,
// drawn AFTER the mesh pass, DEPTH-TESTED against that pass's depth
// transient (LESS_OR_EQUAL; opaque and masked rows wrote it, transparent
// ones did not) WITHOUT WRITING IT, and alpha-blended (straight
// alpha) into the linear RGBA16F canvas. Minor lines every 1 m and major
// lines every 10 m from screen-space derivatives (fwidth), so a line is ~1
// px wide at every distance; the two in-plane ORIGIN AXES over the lines in
// colour; a fade with distance and a fade at grazing angles. The quad is
// centred under the camera with its origin wrapped to a major-cell multiple
// (UE's FmodFloor) and its half-extent scaled with the eye's altitude so
// flying up never reveals its edge. Plane XZ (the 3D ground, +Y up) or XY
// (the 2D authoring plane) -- a view setting, not a scene fact.
//
// WHAT IT IS DELIBERATELY NOT:
//   * NOT pickable. It is canvas content only: nothing here reaches the pick
//     pass (PickOutlineNodes), and nothing ever should -- the grid is an
//     editor affordance, not a scene object (spec s5.2).
//   * NOT a depth writer. The graph-level declaration on the depth handle is
//     DepthWrite (the only barrier state a bound depth attachment has); what
//     keeps the grid out of the depth buffer is the PSO's `depth.write =
//     false`. A later transparent pass that tested against the grid would
//     be wrong, and that is the point: the grid has no volume.
//   * NOT the 2D grid. The Ortho2D view's grid is ViewportGrid.hpp's batched
//     lines (Task 9); this node draws only when the editor's view mode is
//     Perspective.
//
// WHERE IT SITS IN THE FRAME: after `mesh`, before the post chain and the
// tonemap. DeclareGraphFrame hands it `handles.canvas` and, when the frame
// carried a mesh scene, the depth transient AddMeshNode returned; with no
// mesh pass the grid is declared with NO depth attachment (nothing to test
// against) and draws unoccluded.
//
// THE SPEC S14 DEPTH-LIFETIME DECISION, stated at the declaration site
// (NriGraphContext.cpp's grid block) and here: the depth transient is
// MeshNode's to mint, and it does NOT move. RenderGraph::Compile derives a
// transient's pool tenancy as [first node that touches it, LAST node that
// touches it] (RgCompiled::Lifetime), so AddGridNode's Write(depth,
// DepthWrite) alone extends the depth transient's life through this node --
// a declaration fact, not an ownership one. No MeshNode change was needed.
// RenderGraphTest.cpp's (T10F4) case pins the lifetime directly.
//
// WHAT THIS NODE OWNS (all persistent, created once at Create()):
//   * the pipeline layout -- ONE ordinary descriptor set (space1 = { b1, the
//     per-frame constant buffer }), NO root constants, NO root sampler, NO
//     textures. The 160-byte constant block (GridFrameConstants, GridNode.cpp)
//     exceeds Vulkan's 128-byte guaranteed push-constant minimum, so it
//     travels in a CB, not a push block. With no root descriptor or root
//     sampler in the layout THE REGISTER-SPACE RULE (Batch2DNode.hpp) is
//     moot; the set sits at space1 anyway to keep grid.hlsl's `b1, space1`
//     identical to mesh.hlsl's, which is the register the SPIR-V shift table
//     already covers;
//   * one descriptor pool and ONE descriptor set PER FRAME SLOT for b1,
//     written ONCE at Create -- MeshNode's CreateConstantArena/CreateSets
//     discipline, copied, for the reason that header gives (a set names a
//     (buffer, offset) baked at creation, so double-buffering the CB means
//     double-buffering the set);
//   * the per-frame-slot constant-buffer arena the b1 views name.
// NO vertex buffer: grid.hlsl builds the quad from SV_VertexID and Record
// issues CmdDraw(6). The PIPELINES (two: with and without a depth
// attachment) come from the vehicle's shared NriPipelineCache, keyed on the
// canvas format and the depth format.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Core/Constant.hpp>          // ARC_CONSTANT
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/FramePacing.hpp>      // kMaxFramesInFlight, FramesInFlight()
#include <Arcane/Render/GridSceneDesc.hpp>    // GridSceneDesc (the struct this node draws)
#include <Arcane/Scene/ViewTransform.hpp>     // the editor camera, whole

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;

    // THE CPU-SIDE GRID (struct GridSceneDesc) lives in
    // <Arcane/Render/GridSceneDesc.hpp> since F5 plan 1 Task 3, so the overlay
    // primitive list can carry one without including this node.

    class ARC_RENDER_API GridNode
    {
    public:
        // Loads grid_vs/grid_ps through the vehicle, registers the layout,
        // creates the pool + constant arena + per-slot sets. Null (already
        // logged) on any failure -- same posture as MeshNode::Create.
        static std::unique_ptr<GridNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- the sanctioned release is Release().
        ~GridNode();

        GridNode(const GridNode&)            = delete;
        GridNode& operator=(const GridNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Resolves the PIPELINE for the frame being declared -- at DECLARATION
        // time, never in Record, for the reason MeshNode::Prepare gives (a PSO
        // compile must not land inside the recording window). `hasDepth`
        // selects the depth-tested PSO (LESS_OR_EQUAL, no write, D32 attached)
        // or the depth-less one (no depth attachment at all). Device-less
        // [nri] cases pass a null context to AddGridNode, which skips this.
        void Prepare(nri::Format canvasFormat, bool hasDepth);

        // Records the grid into an ALREADY-OPEN raster pass whose colour
        // attachment is the canvas (and whose depth attachment, if any, is the
        // mesh pass's). Writes this frame slot's constant region from `scene`,
        // binds the set, the layout and the pipeline, and draws 6 vertices.
        // Emits NO barrier and NO clear. `frameSlot` is the vehicle's own.
        void Record(RenderGraphNodeContext& context, const GridSceneDesc& scene,
                    std::uint32_t frameSlot);

        // The b1 block's region size BEFORE alignment. grid.hlsl's GridFrameCB
        // is 160 bytes; 256 is D3D12's constant-buffer placement alignment.
        ARC_CONSTANT("layout: constant-buffer placement size; the frame struct must fit")
        static constexpr std::uint32_t kFrameCbMaxBytes = 256;

        // The arena's region stride for a device whose constant-buffer offset
        // alignment is `constantBufferAlignment` -- MeshNode::CbRegionStride's
        // arithmetic, restated so the two nodes' invariants are independently
        // pinnable.
        [[nodiscard]] static constexpr std::uint64_t CbRegionStride(
            std::uint64_t constantBufferAlignment) noexcept
        {
            return constantBufferAlignment <= 1
                 ? kFrameCbMaxBytes
                 : ((kFrameCbMaxBytes + constantBufferAlignment - 1) / constantBufferAlignment)
                       * constantBufferAlignment;
        }

        [[nodiscard]] static constexpr std::uint64_t CbRegionOffset(
            std::uint64_t regionStride, std::uint32_t frameSlot) noexcept
        {
            return (std::uint64_t)frameSlot * regionStride;
        }

        // THE DESCRIPTOR POOL'S CAPACITY: FramesInFlight() sets, one
        // CONSTANT_BUFFER descriptor each, and nothing else.
        [[nodiscard]] static nri::DescriptorPoolDesc PoolSizes() noexcept;

    private:
        GridNode() = default;

        bool Init(NriGraphContext& context);
        bool CreateBindings();
        bool CreateConstantArena();
        bool CreateSets();

        [[nodiscard]] nri::Pipeline* PipelineFor(nri::Format canvasFormat, bool hasDepth);

        [[nodiscard]] std::uint64_t ArenaOffset(std::uint32_t frameSlot) const
        {
            return CbRegionOffset(m_arenaStride, frameSlot);
        }

        // NriPipelineCache::GraphicsKey::shaderPairId -- the caller's
        // discriminator for what the key does not carry. One shared cache, so
        // the node id spaces must not overlap: Batch2DNode 0x2000..0x2002,
        // TonemapNode 0x3000, outline 0x4000..0x4002, PickNode 0x4100,
        // MeshNode 0x5000.
        ARC_CONSTANT("id scheme: the grid shader-pair id")
        static constexpr std::uint64_t kShaderPairId = 0x6000;

        NriDevice*        m_device    = nullptr;
        NriPipelineCache* m_pipelines = nullptr;

        // Bytecode is OWNED BY THE VEHICLE (its bin cache) and outlives this
        // node -- the pipeline cache's fill contract (rule 2) requires it.
        std::span<const std::uint8_t> m_vs, m_ps;

        nri::DescriptorPool* m_pool     = nullptr;
        std::uint32_t        m_layoutId = NriPipelineCache::kInvalidLayout;

        // The per-frame-slot b1 arena: ONE HOST_UPLOAD buffer, persistently
        // mapped, carved into FramesInFlight() regions.
        nri::Buffer*     m_arena       = nullptr;
        void*            m_arenaCpu    = nullptr;
        std::uint64_t    m_arenaStride = 0;
        nri::Descriptor* m_frameCbView[kMaxFramesInFlight]{};
        nri::DescriptorSet* m_sets[kMaxFramesInFlight]{};

        // Resolved by Prepare for the frame being declared; borrowed from the
        // cache. Which of the two PSOs it is (depth-tested or not) is decided
        // by Prepare's `hasDepth`, which DeclareGraphFrame derives from
        // whether the frame carried a mesh pass.
        nri::Pipeline* m_pipeline = nullptr;

        bool m_warnedNoPipeline = false;
        bool m_warnedBadCamera  = false;
    };

    // Declares the grid node into `graph`: Write(canvas, ColorWrite), and --
    // when `depth` is a valid handle -- Write(depth, DepthWrite) +
    // SetDepthAttachment(depth); SetColorAttachments(canvas) either way. An
    // INVALID `depth` declares the node with no depth attachment (the frame
    // carried no mesh pass; the grid draws unoccluded).
    //
    // `canvasFormat` is the caller's, not assumed, for the reason AddMeshNode
    // gives. `context` is a POINTER so the device-less [nri] frame-shape cases
    // can drive the real declarations with none. `scene` is BORROWED at
    // declaration time and COPIED into the exec fn (it is small: one
    // ViewTransform and a handful of floats), so the caller's object need
    // only outlive the RenderFrame call -- which FrameDesc already requires.
    ARC_RENDER_API void AddGridNode(RenderGraph& graph, NriGraphContext* context,
                                RgTexture canvas, nri::Format canvasFormat,
                                RgTexture depth, const GridSceneDesc& scene,
                                std::uint32_t width, std::uint32_t height);
}
