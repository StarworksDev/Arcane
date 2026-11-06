#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// OverlayNode -- THE POST-TONEMAP OVERLAY PASS (F5 plan 1 Task 3, spec s3.3).
//
// One raster pass, declared right AFTER the tonemap and before the game HUD,
// that draws editor and debug affordances onto the DISPLAY-REFERRED target:
// the folded 3D reference grid (overlay.hlsl's grid_vs_main / grid_ps_main),
// then the depth-tested lines and fills (the 2D grid), then the always-on-top
// ones (physics debug, the camera rect, the gizmo), each class in submission
// order. Straight alpha,
// source-over; the post chain never grades it, because it runs after the
// chain AND after the tonemap.
//
// DEPTH IS SAMPLED, NOT ATTACHED. The pass has no depth attachment and writes
// no depth. When the frame carried a mesh pass, the node Reads that pass's
// depth transient as ShaderRead and binds a TEXTURE view of it at t0; a
// depth-tested primitive discards itself where the scene is nearer
// (NearerOrEqualOp(kSelectedDepthConvention) decides the direction). With no
// mesh pass there is nothing to test against: the node Reads NO depth -- in
// particular never the unconsumed placeholder `depth` node -- binds its own
// 1x1 R32_SFLOAT dummy at t0 (overlay.hlsl always declares t0, and an unbound
// t0 is a validation error) and sets g_HasDepth to 0.
//
// WHAT THIS NODE OWNS (created once at Create()):
//   * the pipeline layout -- ONE set in space 0: b0 (the frame constants, VS +
//     PS), t0 (scene depth, PS), s0 (a point sampler, PS), in that range order;
//   * one descriptor pool and ONE set per frame slot; the CB view and the
//     sampler are written once, t0 is rewritten when the bound texture changes;
//   * the per-frame-slot constant arena the b0 views name;
//   * the point sampler, the 1x1 dummy depth texture and its view;
//   * the TEXTURE views over the graph's scene-depth transient -- pool
//     textures, so they are buried whenever RenderGraph::PoolEpoch moves
//     (SyncPoolEpoch; FullscreenNodes.hpp, SOURCE VIEWS AND THE POOL).
// The line/fill vertices go through the vehicle's upload ring at record time
// (Batch2DNode's discipline); the grid has no vertex buffer.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <Arcane/Core/Constant.hpp>          // ARC_CONSTANT
#include <Arcane/Render/FramePacing.hpp>      // kMaxFramesInFlight, FramesInFlight()
#include <Arcane/Render/GridSceneDesc.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;

    // What one overlay frame records. `list` is BORROWED for the RenderFrame
    // call (FrameDesc's rule); `grid` is a copy of the frame's 3D grid when it
    // draws one; `worldViewProjection` projects world-space primitives.
    // `worldDepthTest` / `gridDepthTest`: whether depth-tested world-space
    // primitives / the grid may sample the scene depth -- true only when their
    // camera is the mesh camera (AddOverlayNode decides).
    struct OverlayRecordDesc
    {
        const OverlayPrimitiveList* list = nullptr;
        std::optional<GridSceneDesc> grid;
        glm::mat4 worldViewProjection{ 1.0f };
        glm::vec2 viewport{ 0.0f };
        bool worldDepthTest = false;
        bool gridDepthTest  = false;
    };

    class ARC_RENDER_API OverlayNode
    {
    public:
        // Loads overlay_vs/_ps and overlay_grid_vs/_ps through the vehicle,
        // registers the layout, creates the pool, sets, arena, sampler and the
        // dummy depth texel. Null (already logged) on any failure.
        static std::unique_ptr<OverlayNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- the sanctioned release is Release().
        ~OverlayNode();

        OverlayNode(const OverlayNode&)            = delete;
        OverlayNode& operator=(const OverlayNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Buries the scene-depth views when the graph's pool epoch moved. Called
        // by the owner at declaration time (every frame, in frame or not) and by
        // Record before it binds anything.
        void SyncPoolEpoch(const RenderGraph& graph);

        // Records the overlay into an ALREADY-OPEN raster pass whose colour
        // attachment is `target`. `depth` is the scene depth this node Read as
        // ShaderRead, or an invalid handle (the dummy is bound, g_HasDepth 0).
        // Emits NO barrier and NO clear.
        void Record(RenderGraphNodeContext& context, RgTexture target, RgTexture depth,
                    const OverlayRecordDesc& desc, std::uint32_t frameSlot);

        // The b0 block's region size BEFORE alignment; overlay.hlsl's OverlayCB
        // is 256 bytes, exactly D3D12's constant-buffer placement alignment.
        ARC_CONSTANT("layout: constant-buffer placement size; the frame struct must fit")
        static constexpr std::uint32_t kFrameCbMaxBytes = 256;

        // The segments of a filled overlay circle (a triangle fan).
        ARC_CONSTANT("geometry: overlay circle fan segment count")
        static constexpr std::uint32_t kCircleSegments = 16;

        // One overlay vertex, as overlay.hlsl's VSIn reads it.
        struct Vertex
        {
            glm::vec3 pos;
            glm::vec4 color;
            float     screenSpace;
            float     linearSpace;
            float     depthTest;
        };

        // CPU EXPANSION, exposed so it is testable without a device: appends
        // the triangles for every primitive of `list` whose depth class is
        // `depth`, IN SUBMISSION ORDER (lines and fills interleaved as given).
        // A line is 6 vertices, a Rect 6, a Triangle 3, a Circle
        // 3 * kCircleSegments. A zero-thickness or zero-length line, and a
        // non-positive-radius circle, emit nothing. A DepthTested primitive's
        // vertices sample depth when it is screen-space, or world-space with
        // `worldDepthTest` true; otherwise they draw untested.
        static void AppendTriangles(const OverlayPrimitiveList& list, OverlayDepthClass depth,
                                    bool worldDepthTest, std::vector<Vertex>& out);

        [[nodiscard]] static nri::DescriptorPoolDesc PoolSizes() noexcept;

    private:
        OverlayNode() = default;

        bool Init(NriGraphContext& context);
        bool CreateBindings();
        bool CreateConstantArena();
        bool CreateDummyDepth();
        bool CreateSets();

        void InvalidateDepthViews(Graveyard& graveyard, std::uint64_t fence);
        [[nodiscard]] nri::Descriptor* EnsureView(const nri::CoreInterface& core, nri::Texture* texture);
        [[nodiscard]] nri::Pipeline* PrimitivePipeline(nri::Format targetFormat);
        [[nodiscard]] nri::Pipeline* GridPipeline(nri::Format targetFormat);

        [[nodiscard]] std::uint64_t ArenaOffset(std::uint32_t frameSlot) const
        {
            return (std::uint64_t)frameSlot * m_arenaStride;
        }

        // NriPipelineCache::GraphicsKey::shaderPairId. One shared cache, so the
        // node id spaces must not overlap: Batch2DNode 0x2000..0x2002,
        // TonemapNode 0x3000, outline 0x4000..0x4002, PickNode 0x4100,
        // MeshNode 0x5000, GridNode 0x6000.
        ARC_CONSTANT("id scheme: the overlay primitive shader-pair id")
        static constexpr std::uint64_t kPrimitivePairId = 0x7000;
        ARC_CONSTANT("id scheme: the overlay grid shader-pair id")
        static constexpr std::uint64_t kGridPairId = 0x7001;

        NriDevice*        m_device    = nullptr;
        NriPipelineCache* m_pipelines = nullptr;

        // Bytecode is OWNED BY THE VEHICLE (its bin cache) and outlives this node.
        std::span<const std::uint8_t> m_vs, m_ps, m_gridVs, m_gridPs;

        // GraphicsPipelineDesc::vertexInput is a pointer the cache dereferences
        // after `fill` returns, so the description lives in members.
        nri::VertexAttributeDesc m_attributes[5]{};
        nri::VertexStreamDesc    m_stream{};
        nri::VertexInputDesc     m_vertexInput{};

        nri::DescriptorPool* m_pool     = nullptr;
        std::uint32_t        m_layoutId = NriPipelineCache::kInvalidLayout;
        nri::Descriptor*     m_sampler  = nullptr;

        nri::Buffer*        m_arena       = nullptr;
        void*               m_arenaCpu    = nullptr;
        std::uint64_t       m_arenaStride = 0;
        nri::Descriptor*    m_cbView[kMaxFramesInFlight]{};
        nri::DescriptorSet* m_sets[kMaxFramesInFlight]{};
        // What each slot's t0 names right now (a scene-depth view or the dummy).
        nri::Descriptor*    m_boundDepth[kMaxFramesInFlight]{};

        nri::Texture*    m_dummyDepth     = nullptr;
        nri::Descriptor* m_dummyDepthView = nullptr;

        struct DepthView
        {
            nri::Texture*    texture = nullptr;
            nri::Descriptor* view    = nullptr;
        };
        std::vector<DepthView> m_depthViews;
        std::uint64_t          m_poolEpoch = 0;

        std::vector<Vertex> m_scratch;

        bool m_warnedNoPipeline = false;
        bool m_warnedBadCamera  = false;
        bool m_warnedViewChurn  = false;
    };

    // Declares node "overlay" into `graph`: Write(target, ColorWrite) +
    // SetColorAttachments(target) with a LOAD colour op (the tonemap already
    // wrote it) and NO depth attachment; when `depth` is a valid handle,
    // Read(depth, ShaderRead) -- an invalid one Reads no depth at all.
    //
    // `grid` (the frame's 3D grid desc) and `overlay` (the primitive list) are
    // BORROWED for the RenderFrame call; either may be null. The 3D grid draws
    // when `grid` is non-null or `overlay->hasGrid3d`, through its own camera.
    //
    // THE CAMERAS (review ruling I2): world-space primitives project through
    // `worldViewProjection` -- the sprite/main view, the camera the 2D world is
    // authored in -- or, when it is null, through `meshViewProjection`, then
    // the grid's camera, then identity. `meshViewProjection` is the camera of
    // the mesh pass whose depth `depth` is (null with no mesh pass). A
    // depth-tested world primitive, and the grid, sample that depth ONLY when
    // their camera equals it; otherwise they draw untested. `context` is a
    // POINTER so device-less [nri] cases can drive the real declaration.
    ARC_RENDER_API void AddOverlayNode(RenderGraph& graph, NriGraphContext* context,
                                       RgTexture target, RgTexture depth,
                                       const GridSceneDesc* grid, const OverlayPrimitiveList* overlay,
                                       std::uint32_t width, std::uint32_t height,
                                       const glm::mat4* worldViewProjection = nullptr,
                                       const glm::mat4* meshViewProjection = nullptr);
}
