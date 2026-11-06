#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// RenderGraph -- Phase 2's declarative frame graph, declaration side (Task
// 3). Design informed by Filament's frame graph (filament/src/fg,
// Apache-2.0): the eager-setup/deferred-execute split and resource handles
// over raw pointers come from FrameGraph.h/PassNode/ResourceNode; this
// version is deliberately smaller (no subresources, no culling, no real
// aliasing yet -- see the plan's Deadlock chassis criteria).
//
// Scope (plan: docs/plans/2026-08-13-nri-phase2-framegraph-2d-cutover.md,
// Tasks 3+4+6): declaration (Task 3), compile (Task 4) and execution
// (Task 6). The class is split across TWO translation units on purpose:
//
//   RenderGraph.cpp      declaration + Compile() -- PURE, no nri device
//                        calls at all (desc/enum types only: nri::Format,
//                        nri::AccessLayoutStage, nri::AccessBits,
//                        nri::Layout, nri::StageBits, nri::BufferUsageBits
//                        are pure data, never dereferenced as devices/
//                        queues/etc. there). Compiling the same
//                        declarations twice yields the same RgCompiled, so
//                        the executor consumes it VERBATIM.
//   RenderGraphExec.cpp  Execute() and everything that owns GPU state --
//                        the transient pool, cached attachment views, the
//                        per-frame-slot command buffers, the submission
//                        fence, and RenderGraphNodeContext::Resolve/
//                        ColorView. This is the ONLY TU on the graph path
//                        that calls nri::CoreInterface::CmdBarrier.
//
// Keep that split: a device call creeping into Compile()'s TU would make
// the derivation impure and untestable device-less, which is what every Task-4
// test depends on.
//
// Execution order = declaration order. Phase 2 does not reorder, cull, or
// parallelize nodes: AddNode() appends to a flat vector and Task 6's
// executor walks it front to back. Revisit only if a later phase's
// profiling data demands it.
//
// Include order: NRI's Extensions/NRIDeviceCreation.h (deliberately NOT
// included by this TU) is what causes the nri::Message::ERROR / wingdi.h
// ERROR macro clash documented in NriCommon.hpp. This file only needs
// NRIDescs.h-level plain data (Format, AccessLayoutStage, BufferUsageBits,
// the opaque Texture/Buffer/CommandBuffer/Descriptor handles, CoreInterface)
// -- <NRI.h> alone provides all of it. Keep it that way; do not add an
// Extensions include here without re-reading that comment.
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>

#include <Arcane/Modules/ArcaneRenderApi.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Arcane
{
    // RenderGraphNodeContext / RgExecuteDesc only ever need REFERENCES to
    // these, which a forward declaration satisfies as long as nothing in
    // this header calls through one -- nothing does. (NriPipelineCache is
    // Task 7's; Task 6 landed the empty placeholder class in
    // Nri/NriPipelineCache.hpp so RgExecuteDesc's frozen field type names a
    // real, constructible type -- see that file.)
    class NriUploadRing;
    class NriPipelineCache;
    class NriDevice;
    class NriSwapChain;
    class Graveyard;

    class RenderGraph;

    // Sentinel for both handle structs below: an RgTexture/RgBuffer with
    // index == kInvalid never refers to a declared resource. Checked
    // verbatim (before any decode), so it stays unambiguous regardless of
    // the encoding below.
    ARC_CONSTANT("sentinel: render-graph 'none' marker")
    inline constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

    // ---------------------------------------------------------------------
    // Value handles -- a single uint32_t per the frozen contract, but NOT a
    // raw resource-vector index: `index` packs a generation into the same
    // field (top 8 bits generation, low 24 bits slot -- entt/Bevy-style
    // versioned handles). RenderGraph::EncodeHandle()/DecodeAndValidateSlot()
    // (private) are the ONLY seam that may interpret this value -- nothing
    // else, including Task 4/6 code extending this class, should treat
    // `.index` as a subscript into m_textures/m_buffers.
    //
    // RenderGraph::Reset() bumps its generation counter (Generation()), so a
    // handle minted before Reset() carries the OLD generation and fails to
    // decode against the NEW one -- caught by IsHandleValid() (queryable,
    // non-fatal) and by an ARC_ASSERT in every accessor that takes a handle
    // (Read/Write/NameOf/IsTransient/WasWritten). This catches the common,
    // steady-state case a plain bounds check cannot: a graph rebuilt every
    // frame with the SAME OR A LARGER shape, where a stale handle's raw
    // index would otherwise still be in bounds and silently alias a
    // different (fresh) resource in the same slot.
    //
    // Generation wraps mod 256 (8 bits): a handle held across exactly 256
    // (or a multiple of 256) Reset() calls is indistinguishable from a
    // current one. Accepted, documented limit on the debug safety net --
    // the same tradeoff fixed-width versioned handles always make; nothing
    // in this phase holds a graph handle across more than a handful of
    // frames.
    // ---------------------------------------------------------------------
    struct RgTexture { std::uint32_t index = kInvalid; };
    struct RgBuffer  { std::uint32_t index = kInvalid; };

    // Subset of nri::TextureDesc the graph owns. mipCount landed in Task 3
    // (Phase 4) -- the 3D slice's mip chain, the one YAGNI deferral this
    // phase actually needs. Layer count, sample count, sharing mode, and any
    // TYPE beyond TEXTURE_2D (array/3D/cube) are still not here; each arrives
    // in its own later phase with its own consumer, same as mipCount did --
    // add on demand, not speculatively.
    struct RgTextureDesc
    {
        nri::Format   format = nri::Format::UNKNOWN;
        std::uint32_t width = 0, height = 0;
        std::uint32_t mipCount = 1;           // Task 3 (Phase 4): 1 = no chain (default -- every
                                               // existing declaration is unaffected). 0 is REFUSED
                                               // at Compile (RenderGraph.cpp); realized verbatim into
                                               // nri::TextureDesc::mipNum and into the pool-slot
                                               // compatibility key (RenderGraphExec.cpp/RenderGraph.cpp).
        bool          depthStencil = false;   // chooses attachment vs shader usage bits (Task 4)
        // D3D12 bakes this value into render-target/depth resources. A node
        // that clears must declare the exact value here or the debug layer
        // reports a slow clear-value mismatch. Transients that are never
        // cleared leave hasOptimizedClearValue false and may alias either.
        nri::ClearValue optimizedClearValue{};
        bool            hasOptimizedClearValue = false;
    };

    // Per-declaration usage: what THIS Read/Write means, not a resource-wide
    // flag (the same resource can be ColorWrite in one node and ShaderRead
    // in the next). Task 4 maps each value to an (access, layout, stage)
    // triple for barrier derivation.
    //
    // F3: VISIBILITY CULLING IS AN ORDINARY COMPUTE NODE declaring these
    // usages (MeshCullNode -- ShaderWriteCs on the visible-index and
    // indirect-arg buffers, which MeshNode then reads as ShaderRead and
    // IndirectArgs), never an implicit graph property and never an
    // execution-time reordering rule. That is a DIFFERENT sense of the word
    // from this file's header ("no culling", "does not reorder, cull, or
    // parallelize nodes"), which is about culling NODES out of the graph --
    // Filament's pass culling, which this graph still does not do. Both
    // statements are true at once.
    enum class RgUsage : std::uint8_t
    {
        ColorWrite, DepthWrite, ShaderRead, ShaderWriteCs, // (UAV, compute)
        CopySrc, CopyDst, Present, ReadbackHost,
        IndirectArgs   // an indirect-draw argument buffer (buffers only) -- F3 plan 1 T6
    };

    // Handed to a node's Setup function. A thin wrapper over its owning
    // RenderGraph -- every method here forwards into the graph's own
    // storage. SetColorAttachments/SetDepthAttachment live on RenderGraph
    // itself (below), not here: reach them from within Setup by capturing
    // the RenderGraph by reference (see AddNode()'s comment).
    class ARC_RENDER_API RenderGraphBuilder
    {
    public:
        RgTexture CreateTexture(const char* name, const RgTextureDesc& desc);               // transient
        RgTexture ImportTexture(const char* name, nri::Texture* texture,
                                 nri::AccessLayoutStage entry, nri::AccessLayoutStage exit,
                                 bool persistent);                                           // history, canvas
        RgBuffer  CreateBuffer(const char* name, std::uint64_t size, nri::BufferUsageBits usage); // transient
        RgBuffer  ImportBuffer(const char* name, nri::Buffer* buffer, std::uint64_t size);

        // THE swapchain backbuffer (Task 6). Deliberately NOT ImportTexture:
        // there is no nri::Texture* to pass at declaration time, because the
        // graph -- not the caller -- owns acquire/present sequencing, and the
        // acquire happens inside Execute(), long after this call. Execute()
        // resolves every handle minted here to the texture it acquired that
        // frame (RgExecuteDesc::swapChain); Resolve() on one before an
        // Execute has acquired yields null.
        //
        // Entry/exit states are fixed rather than caller-supplied, and that
        // is the point: entry is the discarding {NONE, UNDEFINED, ALL} (a
        // freshly acquired backbuffer's contents are not ours), and exit is
        // the {NONE, PRESENT, NONE} triple nri::Layout::PRESENT's own comment
        // mandates -- which is what makes "the graph presents" structural
        // rather than a convention a caller could get wrong. Compile()'s exit
        // barriers therefore always leave the backbuffer present-ready.
        //
        // More than one call per graph is legal but pointless -- every
        // handle resolves to the SAME acquired texture. Execute() acquires
        // exactly once per call regardless.
        RgTexture ImportSwapChainTexture(const char* name);

        void Read (RgTexture texture, RgUsage usage);
        void Read (RgBuffer buffer, RgUsage usage);
        void Write(RgTexture texture, RgUsage usage);
        void Write(RgBuffer buffer, RgUsage usage);

    private:
        friend class RenderGraph;
        RenderGraphBuilder(RenderGraph& graph, std::size_t nodeIndex) noexcept
            : m_graph(graph), m_nodeIndex(nodeIndex) {}

        RenderGraph& m_graph;
        std::size_t  m_nodeIndex;
    };

    // Handed to a node's Exec function -- the executor (RenderGraphExec.cpp)
    // constructs exactly ONE of these per Execute() call and hands the same
    // object to every node in turn. Resolve/ColorView are defined in that TU,
    // never in RenderGraph.cpp.
    //
    // The command buffer is already OPEN and stays open for the whole
    // Execute() (one command buffer records every node -- the brief's
    // frozen shape). For a Raster node the executor has already emitted the
    // node's barriers AND CmdBeginRendering with its declared attachments
    // before calling the exec fn, and issues CmdEndRendering after it
    // returns: an exec fn must NOT begin/end rendering itself, and must not
    // emit barriers (the executor is the only CmdBarrier call site on the
    // graph path -- see RgCompiled's contract block below).
    // ARC_RENDER_API: Resolve/ColorView are DEFINED in RenderGraphExec.cpp inside
    // ArcaneClient.dll and CALLED from node exec fns that live wherever the
    // frame driver does -- the test exe today, a game module tomorrow. An
    // unexported struct here linked fine until a node actually resolved a
    // handle, and then failed at link time in the consumer, not here.
    struct ARC_RENDER_API RenderGraphNodeContext
    {
        nri::CommandBuffer&       cmd;
        const nri::CoreInterface& core;

        // Both Resolve overloads decode through the graph's ONE handle seam
        // and then read the executor's per-execution resolution table, so a
        // handle from a previous generation resolves to null rather than to
        // some other frame's resource. Null is also what a handle declared
        // on a DIFFERENT graph, or a swapchain import in an Execute() that
        // could not acquire, yields -- always check.
        [[nodiscard]] nri::Texture*    Resolve(RgTexture texture) const;
        [[nodiscard]] nri::Buffer*     Resolve(RgBuffer buffer) const;

        // The cached COLOR_ATTACHMENT view for `texture`, or null if no node
        // declared it as a colour attachment. Views are created up front by
        // Execute() (before the first node runs) for exactly the textures the
        // declarations attach, and are owned/destroyed by the graph -- this
        // is a lookup, never a creation point.
        [[nodiscard]] nri::Descriptor* ColorView(RgTexture texture) const;

        NriUploadRing&    ring;       // Task 5 -- the caller owes ring.BeginFrame(frameSlot) before Execute()
        NriPipelineCache& pipelines;  // Task 7

        // The graph being executed. Set by Execute(); Resolve/ColorView read
        // its per-execution tables through it.
        const RenderGraph* graph = nullptr;
    };

    // What Execute() needs that the declarations cannot carry. Frozen shape
    // (plan, Task 6).
    struct RgExecuteDesc
    {
        // Owns the queue and the CoreInterface every resource here is created
        // and destroyed through. MUST outlive the graph: the graph's resources
        // are destroyed through this device's function table, and a graph that
        // outlived its device would bury thunks into freed memory. The same
        // device must be passed to every Execute() on one graph (a second
        // device is refused, not silently adopted).
        NriDevice& device;

        // ============ THE GRAVEYARD LANE ==================================
        // WHERE EVERYTHING THIS GRAPH DESTROYS GOES -- the pool, the cached
        // attachment views, the per-execute imported views, the per-frame-slot
        // command buffers and allocators, and the submission fence. It is a
        // parameter rather than `device.Graves()` because a fence value only
        // means something inside ONE submission timeline, and this graph's
        // timeline is its own m_fence: reaping with it against a graveyard some
        // OTHER graph also buries into would run that graph's thunks before its
        // submission retired.
        //
        // ONE LANE PER CONTEXT, NOT PER DEVICE. Two NriGraphContexts on one
        // NriDevice (the editor's chrome context + its offscreen viewport
        // context) have two RenderGraphs and therefore two INDEPENDENT fence
        // timelines whose values mean nothing to each other. Before this field
        // existed both buried into NriDevice::Graves(), where the interleaving
        // tripped Graveyard::Bury's nondecreasing assert in Debug and, worse,
        // let each Execute reap the OTHER graph's burials early -- a
        // use-after-free no assert catches.
        //
        // LATCHED AT THE FIRST Execute(), beside `device`, and a second lane is
        // REFUSED for the same reason a second device is: every pending burial
        // names thunks the first lane will run. The latch is what carries the
        // lane to the burial sites no RgExecuteDesc reaches --
        // ReleaseGpuResources() and ~RenderGraph -- and to the ones INSIDE a
        // FAILING Execute (EnsureExecutionResources's all-or-nothing cleanup),
        // which is the sharpest edge the lane exists to close: it buries at
        // m_submitValue == 0 synchronously, before any destructor runs.
        //
        // MUST OUTLIVE THE GRAPH, exactly as `device` must. NriGraphContext
        // guarantees it by declaring its Graveyard BEFORE its RenderGraph and
        // by destroying the graph EXPLICITLY in its destructor body, ahead of
        // the drain -- so ~RenderGraph's own tail lands in a lane that is still
        // drained afterwards rather than in a shared structure nothing revisits.
        // ===================================================================
        Graveyard& graves;

        // Optional: null = headless/offscreen. Required as soon as any node
        // declared ImportSwapChainTexture() -- Execute() refuses that
        // combination rather than recording a frame whose backbuffer does not
        // exist. Execute() NEVER resizes it: NriSwapChain's "no Resize
        // between Acquire and Present" caller contract is retired for graph
        // users precisely because the acquire/present pair now lives entirely
        // inside one Execute() call, so a frame driver satisfies it by
        // resizing strictly between Execute() calls.
        //
        // A resize places NO FURTHER CONTRACT on the caller -- there is
        // nothing to remember. Every IMPORTED texture's attachment view is
        // rebuilt inside each Execute() and the previous frame's is buried
        // (RenderGraph::m_importedViews), so a destroyed-and-recreated
        // backbuffer can never be bound through a view of its predecessor.
        //
        // That had to be made structural rather than documented, because no
        // cache can be made correct here: views are found by texture POINTER,
        // and NRI is free to hand a recreated texture the address a destroyed
        // one just vacated. A stale nri::Descriptor still names the OLD native
        // image, so binding it is a use-after-free that a pointer comparison
        // reports as a cache hit.
        NriSwapChain* swapChain;

        // Forwarded to node exec fns untouched -- Execute() never allocates
        // from it and never calls BeginFrame() on it. Resetting the frame
        // slot's arena is the caller's call, made once per frame beside the
        // pacing wait that makes the slot safe to reuse at all.
        NriUploadRing& ring;

        NriPipelineCache& pipelines;  // Task 7 (forward-declared; NONE tests pass a stub)

        // Which of the graph's FramesInFlight() command-buffer slots
        // this frame records into. CALLER CONTRACT, same one NriUploadRing::
        // BeginFrame carries: a slot must not be reused until the submission
        // it last carried has retired, because Execute() resets that slot's
        // command allocator. NriSwapChain::AcquireNextTexture()'s pacing wait
        // establishes exactly that for a presenting frame driver; a headless
        // driver owes the equivalent discipline itself.
        std::uint32_t frameSlot;
    };

    // =====================================================================
    // Compile output (Task 4). Pure data: RenderGraph::Compile() derives all
    // of it from the declarations alone -- no nri device calls, no GPU
    // allocation, no mutation of the graph.
    //
    // WHAT TASK 6'S EXECUTOR REPLAYS, AND WHAT IT MUST MERGE: nothing needs
    // merging. Every before/after triple in here is FINAL and already
    // accounts for transient pool reuse -- when transients share a pool
    // slot, each tenant after the first has its first barrier carry the
    // IMMEDIATELY PRECEDING tenant's outgoing state in its `before` (not
    // necessarily the very first tenant's -- a 3+-tenant chain hands over
    // link by link). ALL THREE FIELDS carry, LAYOUT INCLUDED, so the handover
    // is an ordinary state-to-state transition that performs the source
    // availability operation the reused physical resource needs.
    //
    // `before.layout` used to be forced to UNDEFINED (a contents-discarding
    // transition). That was the D2 dx12 blocker: NRI's D3D12 backend pairs it
    // with the carried non-NONE access and D3D12_TEXTURE_BARRIER_FLAG_DISCARD
    // (ThirdParty/NRI/Source/D3D12/CommandBufferD3D12.hpp:1054-1056,
    // :1078-1079), an illegal enhanced-barrier combination that invalidates
    // the command list and fails Close(). See RenderGraph.cpp's POOL HANDOVER
    // comment for the full argument and the rejected alternatives.
    //
    // A HANDOVER ALWAYS PRODUCES A BARRIER, including when the two tenants
    // want the same state -- the consecutive-same-state elision does not
    // apply to a change of tenant. (While the layout was forced to UNDEFINED
    // that held by accident, since nothing ever WANTS UNDEFINED.)
    //
    // The executor's whole job is translation: emit each node's preBarriers
    // as ONE nri CmdBarrier group immediately before that node, run the
    // node, then emit exitBarriers after the last one -- textures via
    // nri::TextureBarrierDesc (full AccessLayoutStage triples) and buffers
    // via nri::BufferBarrierDesc (access + stages only; drop the layout,
    // which is always UNDEFINED for a buffer). It must not synthesize,
    // reorder, drop, or coalesce barriers, and must not derive any state of
    // its own: doing so silently diverges from the derivation this task's
    // tests pin.
    //
    // ONE amendment, and only one, is the executor's to make (Task 6 fix
    // round 1): the CROSS-FRAME half of the pool handover. The transient pool
    // survives Reset(), so two consecutive frames share one physical
    // resource -- and Compile() is pure and per-frame, so the `before` it
    // derives for a slot's first use is always {NONE, UNDEFINED, ALL}
    // regardless of what the previous frame left there. The executor patches
    // exactly that first barrier per pool slot per frame, replacing `before`
    // with the whole state IT observed the resource being left in (access,
    // layout and stages) -- the identical operation the POOL HANDOVER block in
    // RenderGraph.cpp performs for a within-frame tenant change, applied where
    // Compile() cannot see. It is not a derivation of its own: it is an
    // observation Compile() has no access to. It also never ADDS or REMOVES a
    // barrier -- the one it patches is already in Compile()'s list. See
    // RenderGraph::PoolResource::carry.
    //
    // INDEX SPACE -- READ THIS BEFORE SUBSCRIPTING ANYTHING IN HERE. Two
    // different index spaces live in this block and they are NOT the same:
    //
    //   * `resourceIndex` (RgBarrier::resourceIndex, RgTransient::
    //     resourceIndex) is a RESOURCE SLOT -- see below.
    //   * RgCompiled::transientLifetimes and ::transientPoolSlot are indexed
    //     by TRANSIENT INDEX, i.e. a position in RgCompiled::transients --
    //     NOT by a resource slot. Go slot-ward via
    //     transients[i].resourceIndex; there is no reverse mapping and
    //     subscripting the lifetime/pool vectors with a resource slot is
    //     silently wrong whenever the graph holds an imported resource.
    //
    // Every `resourceIndex` is a DECODED, plain 0-based SLOT into the
    // graph's texture vector or its buffer vector, selected by the
    // neighbouring `isTexture` flag. It is NOT an RgTexture/RgBuffer
    // `.index` value:
    // those pack a generation into their top 8 bits (see the RgTexture/
    // RgBuffer comment above) and are never valid subscripts. Compile()
    // decodes every handle it consumes -- the accesses recorded by
    // RecordAccess(), AND NodeDesc::colorAttachments/depthAttachment, which
    // the SetColorAttachments/SetDepthAttachment setters deliberately store
    // unvalidated -- through RenderGraph::DecodeAndValidateSlot(), the one
    // handle seam, and stores only decoded slots in here. So: index
    // RgCompiled with these values directly; never re-decode them, and
    // never feed an RgTexture/RgBuffer handle to one of these vectors.
    // =====================================================================

    // One derived state transition, emitted for exactly one resource.
    //
    // For TEXTURES `before`/`after` are complete nri::AccessLayoutStage
    // triples and all three fields matter. For BUFFERS the `layout` field of
    // both is ALWAYS nri::Layout::UNDEFINED and carries no meaning:
    // nri::BufferBarrierDesc has no layout member (its before/after are
    // nri::AccessStage, access+stages only), so Task 6 drops it when it
    // translates. Sharing one struct across both kinds is the frozen
    // contract's shape; forcing UNDEFINED on the buffer side keeps a
    // meaningless field from ever reading as meaningful.
    struct RgBarrier
    {
        std::uint32_t          resourceIndex = 0;   // DECODED slot -- see INDEX SPACE above
        bool                   isTexture = true;
        nri::AccessLayoutStage before{};
        nri::AccessLayoutStage after{};
    };

    struct RgCompiledNode
    {
        std::uint32_t          nodeIndex = 0;
        std::vector<RgBarrier> preBarriers;   // batched: ONE nri CmdBarrier group per node
    };

    // One transient resource, in the order Compile() enumerates them: every
    // transient TEXTURE in slot order first, then every transient BUFFER in
    // slot order. RgCompiled::transientLifetimes and ::transientPoolSlot are
    // index-parallel to RgCompiled::transients -- this struct is what makes
    // "index-parallel to transients" a defined, self-describing statement
    // instead of an ordering Task 6 would have to re-derive (and could
    // re-derive differently).
    struct RgTransient
    {
        std::uint32_t resourceIndex = 0;   // DECODED slot -- see INDEX SPACE above
        bool          isTexture = true;
    };

    // A transient no node ever reads or writes has no lifetime and gets no
    // pool slot: its Lifetime is {kRgNoNode, kRgNoNode} and its
    // transientPoolSlot entry is kRgNoPoolSlot. Task 6 must not realize it
    // (there is no pool slot to realize it into).
    ARC_CONSTANT("sentinel: render-graph 'none' marker")
    inline constexpr std::uint32_t kRgNoNode     = 0xFFFFFFFFu;
    ARC_CONSTANT("sentinel: render-graph 'none' marker")
    inline constexpr std::uint32_t kRgNoPoolSlot = 0xFFFFFFFFu;

    struct RgCompiled
    {
        // One entry per declared node, in declaration order:
        // nodes.size() == RenderGraph::NodeCount() and nodes[i].nodeIndex == i
        // ALWAYS -- a node with nothing to transition carries an EMPTY
        // preBarriers rather than being omitted, so Task 6's executor can
        // walk this vector as its node list and never has to cross-reference
        // it against the graph's own node vector.
        std::vector<RgCompiledNode> nodes;

        // Transitions that run after the LAST node, restoring every imported
        // TEXTURE to the exit state its ImportTexture() call promised (e.g.
        // nri::Layout::PRESENT for a swapchain backbuffer). Imported BUFFERS
        // never appear here: ImportBuffer() takes no entry/exit state, so the
        // graph has nothing to restore them to.
        std::vector<RgBarrier> exitBarriers;

        // Inclusive node-index range [first, last] over which a transient is
        // live -- first/last are the lowest/highest node index that reads or
        // writes it.
        struct Lifetime { std::uint32_t first = kRgNoNode, last = kRgNoNode; };

        std::vector<RgTransient>   transients;
        std::vector<Lifetime>      transientLifetimes;   // index-parallel to transients
        std::vector<std::uint32_t> transientPoolSlot;    // index-parallel to transients
        std::uint32_t              poolSlotCount = 0;    // distinct pool slots Task 6 must realize
    };

    // Declarative attachment load op (F5 plan 1, Task 2). A node that wants
    // its attachment cleared says so at declaration; the executor turns it
    // into the nri::AttachmentDesc load op at CmdBeginRendering, so a node
    // need not clear from its own exec fn. Load is the pre-F5 behaviour.
    // The store op is always STORE.
    enum class RgLoadOp : std::uint8_t { Load, Clear };

    struct RgAttachmentOp
    {
        RgLoadOp         load = RgLoadOp::Load;
        nri::ClearValue  clear{};
    };

    class ARC_RENDER_API RenderGraph
    {
    public:
        using Setup = std::function<void(RenderGraphBuilder&)>;
        using Exec  = std::function<void(RenderGraphNodeContext&)>;
        enum class NodeKind : std::uint8_t { Raster, Compute, Copy };

        RenderGraph() = default;

        // Releases everything Execute() realized -- the transient pool, the
        // cached attachment views, the per-frame-slot command buffers and the
        // submission fence -- by BURYING it in THIS GRAPH'S LANE
        // (RgExecuteDesc::graves, latched at the first Execute) at the last
        // submitted fence value. Somebody must still DRAIN that lane behind a
        // DeviceWaitIdle afterwards, which is why both the lane and the device
        // must outlive the graph; NriGraphContext does it by destroying its
        // graph explicitly, in its destructor body, right before the drain. A
        // graph that never executed latched neither and owns nothing, so this
        // is a no-op.
        ~RenderGraph();

        // Non-copyable, non-movable since Task 6: this object owns live NRI
        // resources (pool textures/buffers, descriptors, command allocators,
        // a fence). A copy would double-bury every one of them; the
        // compiler's move would leave the source holding the SAME raw
        // pointers rather than nulling them, so a moved-from graph's
        // destructor would bury a second time. Neither has a caller -- the
        // frame driver holds one graph for the process lifetime and Reset()s
        // it per frame.
        RenderGraph(const RenderGraph&)            = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;
        RenderGraph(RenderGraph&&)                 = delete;
        RenderGraph& operator=(RenderGraph&&)      = delete;

        // Declares one node in EXECUTION order (see the file-header comment
        // -- no reordering in Phase 2). Runs `setup` synchronously, right
        // here, before returning -- Filament's addPass() does the same
        // (eager setup, deferred execute). m_currentNodeIndex is this
        // call's node for the duration of `setup`, so SetColorAttachments/
        // SetDepthAttachment -- reached from within `setup` by capturing
        // this RenderGraph by reference, since they are not on
        // RenderGraphBuilder -- target the right node. `exec` is stored for
        // Task 6's executor and is never invoked here.
        //
        // Raster nodes must end this call with at least one color or depth
        // attachment declared. That is checked here, but non-fatally
        // (ARC_ENSURE) and recorded rather than refused: declaration always
        // succeeds (see NodeHasRequiredAttachments()) -- matching the
        // assert-later model RenderGraphBuilder::Read() uses for a
        // never-written transient (RenderGraph.cpp).
        void AddNode(const char* name, NodeKind kind, Setup setup, Exec exec);

        // Apply to the node the most recent AddNode() call declared. Valid
        // to call only while that node's `setup` is running (typically via
        // a `setup` that captures this RenderGraph by reference) --
        // ARC_ASSERTs otherwise.
        void SetColorAttachments(std::span<const RgTexture> attachments);
        void SetDepthAttachment(RgTexture attachment);

        // Load ops for the attachments above, same "only while setup is
        // running" rule. Colour ops are index-parallel to the colour
        // attachments; a span shorter than the attachment span leaves the
        // missing attachments at RgLoadOp::Load.
        void SetColorAttachmentOps(std::span<const RgAttachmentOp> ops);
        void SetDepthAttachmentOp(RgAttachmentOp op);

        // Derives, from the declarations above and NOTHING else, everything
        // Task 6's executor needs: the batched per-node barriers, the
        // imported-texture exit barriers, transient lifetimes, and the
        // transient pool-slot assignment. PURE -- no nri device calls, no
        // GPU allocation, no mutation of this graph; compiling the same
        // declarations twice yields the same result. See the INDEX SPACE
        // comment above RgBarrier for how to read the indices it produces.
        //
        // Returns std::nullopt -- and, when `outError` is non-null, fills it
        // with a message that NAMES the offending node and/or resource --
        // when the declarations cannot yield a correct barrier chain:
        //   1. a Raster node that declared no color or depth attachment
        //      (NodeHasRequiredAttachments() false: recorded non-fatally at
        //      declaration, refused HERE -- this is the "later" of Task 3's
        //      assert-later model);
        //   2. an attachment handle that fails to decode (invalid, stale
        //      across a Reset(), or out of range) -- the setters store
        //      attachment handles unvalidated, so Compile is their only
        //      validation point;
        //   3. a Raster node attachment the node never declared a Read()/
        //      Write() for -- without a declared access the graph derives no
        //      transition, and the attachment would be bound in whatever
        //      layout it happened to be left in;
        //   4. a Read() of a TRANSIENT at a node earlier than the first node
        //      that Write()s it (which subsumes "never written at all") --
        //      its contents are undefined. Node-granular: a node may read a
        //      transient it also writes itself, regardless of the order the
        //      two accesses were declared in, because one node resolves to
        //      exactly one state;
        //   5. a single node declaring two DIFFERENT states for the same
        //      resource (e.g. ShaderRead and ShaderWriteCs) -- the node's
        //      one batched pre-barrier group cannot transition a resource
        //      twice, and the two usages are a genuine read/write hazard;
        //   6. an access whose recorded slot is out of range -- only
        //      reachable in a build where the ARC_ASSERT in RecordAccess()
        //      is compiled out (see that function).
        [[nodiscard]] std::optional<RgCompiled> Compile(std::string* outError = nullptr) const;

        // Records `compiled` into ONE command buffer and submits it. In
        // order, per call:
        //
        //   1. reap the device graveyard up to this graph's completed
        //      submissions, and (if a node imported the swapchain) acquire
        //      the backbuffer -- a failed/skipped acquire returns false
        //      having recorded and submitted nothing;
        //   2. realize any pool slot the compile asked for that is not
        //      already realized, and any attachment view that is not already
        //      cached (both survive across Execute() calls -- a second
        //      Execute of the same compiled graph creates ZERO resources);
        //   3. reset `desc.frameSlot`'s command allocator, open its command
        //      buffer, and walk compiled.nodes front to back: each node's
        //      batched preBarriers as ONE CmdBarrier group, then (Raster
        //      only) CmdBeginRendering with the node's declared attachments,
        //      the node's exec fn, CmdEndRendering. Every node is bracketed
        //      by a `pass:<name>` breadcrumb scope + GPU marker;
        //   4. compiled.exitBarriers as one final CmdBarrier group, close,
        //      submit (waiting on / signalling the swapchain's acquire and
        //      release fences when a backbuffer was acquired, and always
        //      signalling this graph's own submission fence);
        //   5. Present(), if a backbuffer was acquired.
        //
        // Returns false -- having reported through the "nri-graph" tagged
        // error seam, which bumps the same RenderErrorCount() latch the 0/0
        // gate reads -- when the frame could not be recorded or submitted.
        // A false return with an outstanding acquire is possible only on a
        // failed submit, in which case nothing is presented (presenting a
        // frame whose release fence nothing signalled parks the present
        // engine forever).
        //
        // `compiled` MUST be the output of a Compile() on THIS graph's
        // CURRENT declarations. That is checked structurally (node count,
        // and every resource index in range) rather than assumed, because
        // the cheap caller bug -- Reset(), re-declare, Execute() with last
        // frame's RgCompiled -- would otherwise index this frame's resources
        // with last frame's slots.
        [[nodiscard]] bool Execute(const RgExecuteDesc& desc, const RgCompiled& compiled);

        // DECLARATIONS ONLY. Clears every declared node and resource (and the
        // per-frame tables derived from them) -- this graph is ready to
        // declare the next frame's build -- and bumps Generation() (see the
        // RgTexture/RgBuffer comment above for what that buys a stale
        // handle).
        //
        // IT RELEASES NO GPU RESOURCE. The transient pool, the cached
        // attachment views, the per-frame-slot command buffers and the
        // submission fence all SURVIVE, and the next Execute() reuses every
        // pool slot whose desc still matches (RealizePool's per-slot compare).
        // So the intended frame loop --
        //
        //     Reset(); BuildGraph(...); Compile(); Execute(...);
        //
        // -- costs ZERO GPU allocations in steady state, and re-creates only
        // the slots whose desc actually changed (a resize, a re-shaped frame),
        // burying exactly those.
        //
        // This is the one place the plan's own text had to be reconciled: it
        // said transients are "destroyed through the Graveyard on Reset()",
        // which collides with the same plan's pool-reuse mandate, because
        // Reset() is the ONLY way to clear declarations and a per-frame driver
        // must therefore call it every frame. Burying there would have made
        // the reuse property unreachable in the actual loop -- N committed
        // render-target creations plus N burials per frame, reaped
        // FramesInFlight() frames later. Intent governs; the pool
        // persists.
        //
        // Pool resources die through the graveyard on exactly four paths:
        // a desc mismatch at re-realization, a resize (which IS a desc
        // mismatch), an explicit ReleaseGpuResources(), and ~RenderGraph.
        void Reset();

        // Buries the transient pool and every cached attachment view in this
        // graph's LANE (RgExecuteDesc::graves), keyed to its last submitted
        // fence value -- the explicit "give the memory back" entry point, for a
        // frame driver parking a graph it will not run for a while (project
        // switch, minimised-to-tray, shutdown ahead of the device). The
        // per-frame-slot command buffers and the submission fence are NOT
        // touched: they are execution machinery, not frame resources, and
        // live until ~RenderGraph.
        //
        // Not needed in the ordinary frame loop -- Reset() does not call it,
        // and a re-declared frame with the same shape reuses the pool. Safe
        // to call at any time, including on a graph that never executed (a
        // no-op) and repeatedly (idempotent); the next Execute() simply
        // realizes the pool again.
        void ReleaseGpuResources();

        // -------------------------------------------------------------
        // Declaration-side introspection. Not part of the cross-task
        // contract frozen above -- exercised directly by this task's [nri]
        // tests, and available for Task 4's Compile() to read (same class,
        // extended in place per that task's brief).
        // -------------------------------------------------------------
        [[nodiscard]] std::uint32_t Generation() const noexcept { return m_generation; }

        [[nodiscard]] std::size_t NodeCount() const noexcept { return m_nodes.size(); }
        [[nodiscard]] const char* NodeName(std::size_t nodeIndex) const;
        [[nodiscard]] bool        NodeHasRequiredAttachments(std::size_t nodeIndex) const;
        // The declared load op, or null when that attachment's op was not
        // declared (out-of-range node/attachment, or a colour-op span that
        // stops short of `attachment`) -- an undeclared op is LOAD.
        [[nodiscard]] const RgAttachmentOp* ColorLoadOp(std::size_t nodeIndex, std::size_t attachment) const;
        [[nodiscard]] const RgAttachmentOp* DepthLoadOp(std::size_t nodeIndex) const;

        [[nodiscard]] std::size_t TextureCount() const noexcept { return m_textures.size(); }
        [[nodiscard]] std::size_t BufferCount() const noexcept { return m_buffers.size(); }
        [[nodiscard]] const char* NameOf(RgTexture texture) const;
        [[nodiscard]] const char* NameOf(RgBuffer buffer) const;
        [[nodiscard]] bool        IsTransient(RgTexture texture) const;
        [[nodiscard]] bool        IsTransient(RgBuffer buffer) const;
        [[nodiscard]] bool        WasWritten(RgTexture texture) const;
        [[nodiscard]] bool        WasWritten(RgBuffer buffer) const;
        // True when some node Reads `texture` (any non-write access). False for
        // an invalid handle. F5 plan 1 Task 3: pins that the overlay pass
        // samples the mesh depth and never the unconsumed placeholder depth.
        [[nodiscard]] bool        WasRead(RgTexture texture) const;

        // True iff `texture`/`buffer` decodes to a slot that both exists in
        // the CURRENT generation and is in range -- the non-fatal, queryable
        // proxy for "would this handle ARC_ASSERT if used right now". Exists
        // specifically so the stale-handle-across-Reset() invariant (see the
        // RgTexture/RgBuffer comment above) has a real, executed negative
        // test rather than only an untestable fatal assert.
        [[nodiscard]] bool IsHandleValid(RgTexture texture) const noexcept;
        [[nodiscard]] bool IsHandleValid(RgBuffer buffer) const noexcept;

        // -------------------------------------------------------------
        // Execution-side introspection. Test seams, not contract: they let
        // the [nri] cases prove pool REUSE rather than merely pool
        // existence, which a single count cannot distinguish from a
        // destroy-and-recreate.
        // -------------------------------------------------------------

        // Pool slots currently realized -- equals RgCompiled::poolSlotCount
        // after a successful Execute() of that compile. SURVIVES Reset();
        // 0 only before the first Execute(), after ReleaseGpuResources(), or
        // once a later compile asks for fewer slots.
        [[nodiscard]] std::size_t DebugTransientCount() const noexcept { return m_pool.size(); }

        // Pool resources RETIRED but not yet buried -- the staging area whose
        // one-frame delay makes "views are buried before the textures they
        // view" hold across a pool reshape (see m_retiredPool). Non-zero only
        // between the Execute() that reshaped the pool and the next one; a
        // device-less [nri] case reads it to pin exactly that window.
        [[nodiscard]] std::size_t DebugRetiredPoolCount() const noexcept { return m_retiredPool.size(); }

        // Physical transients ever created by this graph, monotonically.
        // Unchanged across a second Execute() of the same compiled graph AND
        // across a Reset()-then-redeclare-the-same-shape frame loop -- that is
        // what "the pool is reused" means.
        [[nodiscard]] std::uint64_t DebugTransientCreateCount() const noexcept { return m_transientCreateCount; }

        // Successful QueueSubmits so far == the value this graph last
        // signalled on its own fence, and the fence value every burial is
        // keyed to.
        [[nodiscard]] std::uint64_t DebugSubmitCount() const noexcept { return m_submitValue; }

        // THIS GRAPH'S GRAVEYARD LANE -- RgExecuteDesc::graves, latched at the
        // first Execute(). Null before it, which is exactly the state in which
        // the graph owns nothing to bury.
        //
        // FOR NODES, and it is the reason this is public: a node that caches a
        // descriptor over a pool texture must bury it when PoolEpoch() moves
        // (see the contract below), and its exec fn runs inside THIS graph's
        // Execute -- so this graph's lane, reached through
        // RenderGraphNodeContext::graph, is the one lane those burials belong
        // in. Reaching for the DEVICE's graveyard there is the bug this field
        // exists to make impossible: it would split one context's burials
        // across two graveyards whose relative drain order nothing defines,
        // and a view destroyed after the texture it views is a use-after-free.
        [[nodiscard]] Graveyard* Graves() const noexcept { return m_graves; }

        // =============================================================
        // THE POOL EPOCH -- CONTRACT, NOT A TEST SEAM. Read this before
        // caching ANYTHING that names a pool texture or buffer.
        // =============================================================
        // Bumped once per buried resource inside RealizePool (a shrink or a
        // desc change, from INSIDE Execute()) -- but bumped only ONCE, not
        // once per resource, when the whole pool is torn down and MOVED out
        // from under any cached view: an explicit ReleaseGpuResources() or
        // ~RenderGraph. A node's epoch check only needs the value to have
        // changed at all, so both granularities are correct for that
        // purpose; this note exists so nobody adds up epoch deltas expecting
        // them to count buried resources.
        //
        // It exists because a node that caches a descriptor over a pool
        // texture cannot detect that burial any other way. NRI is free to hand
        // a recreated texture the address a destroyed one just vacated, so a
        // pointer-keyed cache reports a HIT and binds a descriptor over freed
        // memory -- and the two RealizePool paths offer the owner no callback
        // and no ordering hook, because they run in the middle of Execute(),
        // after every declaration and before every exec fn. Only the node
        // itself, at record time, is in a position to notice.
        //
        // The frame's SHAPE is what makes those paths live: a frame that
        // declares a post chain needs more pool slots than one that does not,
        // and a chain can appear (its compile lands a few frames in),
        // disappear, or change its slot count (a DAG re-wire) at runtime. It
        // was unreachable while the frame's shape was constant.
        //
        // CONTRACT for such a node: compare this against the value you last
        // built views at -- an exec fn reaches the graph through
        // RenderGraphNodeContext::graph -- and when it differs, bury every
        // cached view (at DebugSubmitCount(), the submission that last used
        // them) and rebuild. FullscreenNodes.cpp's TonemapNode and
        // PostChainNode both do exactly that; it costs one comparison per node
        // per frame.
        [[nodiscard]] std::uint64_t PoolEpoch() const noexcept { return m_poolEpoch; }

        // The `before` triple actually RECORDED for the first barrier that
        // touched pool slot `slot` during the last Execute(). The only
        // observable of the cross-frame handover (PoolResource::carry): on a
        // freshly realized slot it is Compile()'s {NONE, UNDEFINED, ALL}, and
        // on a slot carried across a Reset() it is the previous frame's
        // outgoing state IN FULL -- access, layout and stages. nullopt if that
        // slot saw no barrier in the last Execute().
        [[nodiscard]] std::optional<nri::AccessLayoutStage> DebugFirstBarrierBefore(std::uint32_t slot) const;

        // Task 3 (Phase 4). The MIP COUNT pool slot `poolSlot` was actually
        // realized with -- read from the graph's own CPU-side record
        // (PoolResource::textureDesc), not a device query: the NONE backend
        // these [nri] tests run on stubs nri::CoreInterface::GetTextureDesc
        // to a fixed dummy regardless of what was created (ImplNONE.cpp), so
        // it cannot observe RealizePool's mip-chain decision. This can.
        // nullopt if the slot is out of range, was never realized, or holds
        // a buffer rather than a texture.
        [[nodiscard]] std::optional<std::uint32_t> DebugPoolTextureMipCount(std::uint32_t poolSlot) const;

    private:
        friend class RenderGraphBuilder;
        friend struct RenderGraphNodeContext;

        enum class ResourceKind : std::uint8_t { Transient, Imported };

        struct TextureResource
        {
            std::string            name;
            RgTextureDesc           desc;
            ResourceKind            kind = ResourceKind::Transient;
            nri::Texture*           imported = nullptr;
            nri::AccessLayoutStage  importEntry{};
            nri::AccessLayoutStage  importExit{};
            bool                    persistent = false;
            // ImportSwapChainTexture(): `imported` stays null at declaration
            // and Execute() resolves this resource to the texture it
            // acquired. Compile() does not care -- an imported texture's
            // barrier derivation reads importEntry/importExit only, never
            // the pointer.
            bool                    swapChain = false;
            // Imported resources always arrive with a defined incoming
            // state (the caller-supplied `entry`), so this starts true; a
            // transient starts false until its first declared Write().
            // Task 4's Compile() refuses a Read() while this is still false
            // at that point -- assert-later: recorded here, refused there.
            // See RenderGraph::RecordAccess() in the .cpp.
            bool                    everWritten = false;
        };

        struct BufferResource
        {
            std::string          name;
            std::uint64_t         size = 0;
            nri::BufferUsageBits  usage{};
            ResourceKind          kind = ResourceKind::Transient;
            nri::Buffer*          imported = nullptr;
            bool                  everWritten = false;   // see TextureResource::everWritten
        };

        struct NodeDesc
        {
            std::string             name;
            // "pass:<name>", built ONCE at declaration. The executor opens a
            // breadcrumb scope and a GPU marker under this string every
            // frame, for every node; composing it per node per frame would
            // put a heap allocation on the record path for a string that
            // never changes.
            std::string             passLabel;
            NodeKind                 kind = NodeKind::Raster;
            Exec                     exec;
            std::vector<RgTexture>   colorAttachments;
            RgTexture                depthAttachment{};
            bool                     hasRequiredAttachments = true;  // see NodeHasRequiredAttachments()
            std::vector<RgAttachmentOp> colorOps;
            RgAttachmentOp              depthOp{};
            bool                        hasDepthOp = false;
        };

        // One declared Read()/Write() -- the raw material Task 4's Compile()
        // walks to derive barriers and transient lifetimes. Recorded, never
        // rejected, at declaration time (assert-later model). resourceIndex
        // here is the DECODED, plain 0-based slot into m_textures/m_buffers
        // (RecordAccess() decodes the caller's encoded handle once, via
        // DecodeAndValidateSlot(), before storing this) -- Task 4 reading
        // m_accesses never has to deal with the handle encoding.
        struct Access
        {
            std::size_t   nodeIndex;
            std::uint32_t resourceIndex;
            bool          isTexture;
            bool          isWrite;
            RgUsage       usage;
        };

        RgTexture CreateTextureInternal(const char* name, const RgTextureDesc& desc);
        RgTexture ImportTextureInternal(const char* name, nri::Texture* texture,
                                         nri::AccessLayoutStage entry, nri::AccessLayoutStage exit,
                                         bool persistent, bool swapChain);
        RgBuffer  CreateBufferInternal(const char* name, std::uint64_t size, nri::BufferUsageBits usage);
        RgBuffer  ImportBufferInternal(const char* name, nri::Buffer* buffer, std::uint64_t size);
        void      RecordAccess(std::size_t nodeIndex, std::uint32_t encodedHandle,
                                bool isTexture, bool isWrite, RgUsage usage);

        // ---------------------------------------------------------------
        // The handle-encoding seam (see the RgTexture/RgBuffer comment
        // above). EncodeHandle() is the only place that PACKS a (slot,
        // generation) pair; DecodeAndValidateSlot() is the only place that
        // UNPACKS and validates one. Every method that mints a handle
        // (Create*/Import*Internal) or consumes one (RecordAccess, NameOf,
        // IsTransient, WasWritten, IsHandleValid) goes through exactly one
        // of these two -- never through `.index` directly. Keep it that
        // way: a future task adding a handle-consuming method (e.g. Task
        // 4/6's Resolve()) MUST route through DecodeAndValidateSlot() too.
        // ---------------------------------------------------------------
        ARC_CONSTANT("handle encoding: render-graph handle generation/index bit split")
        static constexpr std::uint32_t kIndexBits      = 24;
        ARC_CONSTANT("handle encoding: render-graph handle generation/index bit split")
        static constexpr std::uint32_t kGenerationBits = 8;
        ARC_CONSTANT("handle encoding: render-graph handle generation/index bit split")
        static constexpr std::uint32_t kIndexMask      = (1u << kIndexBits) - 1u;      // 0x00FFFFFF -- max live slots per generation
        ARC_CONSTANT("handle encoding: render-graph handle generation/index bit split")
        static constexpr std::uint32_t kGenerationMask = (1u << kGenerationBits) - 1u; // 0xFF -- generation wraps mod 256
        ARC_CONSTANT("sentinel: render-graph 'none' marker")
        static constexpr std::size_t   kNoSlot         = static_cast<std::size_t>(-1); // decode failure: invalid, stale, or out of range

        [[nodiscard]] static std::uint32_t EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        [[nodiscard]] std::size_t DecodeAndValidateSlot(std::uint32_t encoded, std::size_t resourceCount) const noexcept;

        ARC_CONSTANT("sentinel: render-graph 'none' marker")
        static constexpr std::size_t kNoCurrentNode = static_cast<std::size_t>(-1);

        // ---------------------------------------------------------------
        // Execution state (Task 6, RenderGraphExec.cpp). Everything below
        // this line is GPU-owning: it is why this class lost its copy and
        // move operations, and why ~RenderGraph is no longer trivial.
        // ---------------------------------------------------------------

        // One realized physical resource per RgCompiled pool slot. Textures
        // and buffers share one slot numbering (Compile()'s pass 4), so this
        // vector is indexed by pool slot directly and each entry says which
        // kind it holds.
        //
        // `desc` is the identity actually realized, kept so a re-Execute can
        // prove the slot still matches what the compile asks for and
        // re-realize when it does not. NRI v180 has no per-resource
        // CAN_ALIAS flag (the brief's word for it): aliasing in this NRI is a
        // MEMORY-object property -- several resources bound at overlapping
        // offsets of one nri::Memory (NRIDescs.h, "Binding resources to a
        // memory (resources can overlap, i.e. alias)") -- and Phase 2's graph
        // does no memory aliasing at all (RenderGraph.hpp header: "no real
        // aliasing yet"). What it DOES do is share one committed resource
        // between transients with disjoint lifetimes, which is a pool-slot
        // property and needs no flag.
        struct PoolResource
        {
            bool             isTexture = true;
            nri::Texture*    texture   = nullptr;
            nri::Buffer*     buffer    = nullptr;
            nri::TextureDesc textureDesc{};
            nri::BufferDesc  bufferDesc{};

            // CROSS-FRAME HANDOVER. The state the LAST Execute() left this
            // physical resource in -- the `after` of the last barrier that
            // named it. Necessary because the pool now survives Reset(): two
            // consecutive frames share one physical texture, and frame N+1's
            // first barrier for the slot carries Compile()'s
            // `before = {NONE, UNDEFINED, ALL}` (Compile is pure and
            // per-frame; it cannot know what the previous frame did). Emitting
            // that verbatim would perform no source availability operation for
            // frame N's writes -- the SAME spec-level write-after-write hazard
            // across a reused object that RenderGraph.cpp's POOL HANDOVER
            // block closes WITHIN a frame, just at the frame boundary.
            //
            // EmitBarriers() therefore patches the first barrier it emits per
            // pool slot per frame, replacing `before` with this WHOLE triple --
            // access, layout and stages -- byte-for-byte the same amendment
            // Compile() makes for an intra-frame tenant handover. Cleared
            // whenever the slot is re-realized (a fresh resource genuinely
            // starts undefined).
            //
            // The layout used to be left at UNDEFINED instead of carried, and
            // that pairing (UNDEFINED layout + a non-NONE access) is illegal on
            // D3D12 enhanced barriers -- the D2 blocker that failed Close() on
            // every dx12 `--nri-graph` run. RenderGraph.cpp's POOL HANDOVER
            // block carries the citation and the rejected alternatives.
            nri::AccessLayoutStage carry{};
            bool                   hasCarry = false;
        };

        // An attachment view, found by the texture it views. Two vectors hold
        // these, and WHICH ONE a view lands in is a lifetime decision, not a
        // filing convenience -- see m_views / m_importedViews.
        struct CachedView
        {
            nri::Texture*    texture = nullptr;
            bool             depth   = false;
            nri::Descriptor* view    = nullptr;
        };

        // One command allocator + command buffer per frame slot. Reused
        // every frame; see RgExecuteDesc::frameSlot for whose job it is to
        // make a slot safe to reset.
        struct GpuFrameSlot
        {
            nri::CommandAllocator* allocator = nullptr;
            nri::CommandBuffer*    cmd       = nullptr;
        };

        bool RealizePool(const RgCompiled& compiled);
        bool RealizeAttachmentViews();
        // Buries last frame's imported-texture views. Called once per
        // Execute(), before this frame's are created -- see m_importedViews.
        void ReleaseImportedViews();
        // Buries whatever RealizePool retired on an EARLIER Execute() and
        // staged rather than buried (see m_retiredPool). Called at the top of
        // every Execute() -- after the Reap, before RealizePool -- and again
        // from ReleaseGpuResourcesInternal so nothing can be stranded.
        // Idempotent and a no-op in the steady state (the vector is empty on
        // every frame that reshaped nothing).
        void FlushRetiredPool();
        bool EnsureExecutionResources();
        // Buries the pool + views (the public ReleaseGpuResources() and
        // ~RenderGraph); buries the command slots + fence too when `all`
        // (~RenderGraph only). NOT reached from Reset() -- see Reset().
        void ReleaseGpuResourcesInternal(bool all);
        // Translates one RgBarrier list into ONE nri CmdBarrier group. The
        // ONLY CmdBarrier call site on the graph path -- see RgCompiled's
        // contract block. Non-const because it fills the reused scratch
        // vectors below rather than allocating per call.
        void EmitBarriers(nri::CommandBuffer& cmd, std::span<const RgBarrier> barriers);

        [[nodiscard]] nri::Texture* TextureForSlot(std::size_t slot) const noexcept;
        [[nodiscard]] nri::Buffer*  BufferForSlot(std::size_t slot) const noexcept;
        // The pool slot a barrier's resource maps to this frame, or
        // kRgNoPoolSlot for an imported resource.
        [[nodiscard]] std::uint32_t PoolSlotForBarrier(const RgBarrier& barrier) const noexcept;
        [[nodiscard]] nri::Descriptor* ViewForTexture(nri::Texture* texture, bool depth) const noexcept;

        std::vector<TextureResource> m_textures;
        std::vector<BufferResource>  m_buffers;
        std::vector<NodeDesc>        m_nodes;
        std::vector<Access>          m_accesses;
        std::size_t                  m_currentNodeIndex = kNoCurrentNode;
        std::uint32_t                m_generation = 0;

        NriDevice*                m_device = nullptr;   // latched by the first Execute(); must outlive this graph
        // The graveyard lane, latched by the first Execute() BESIDE m_device
        // and never independently -- every burial site in RenderGraphExec.cpp
        // reads one and writes the other's contents, so "m_device is latched"
        // and "m_graves is latched" are the same fact. See RgExecuteDesc::
        // graves. Must outlive this graph.
        Graveyard*                m_graves = nullptr;
        std::vector<PoolResource>  m_pool;

        // ================= THE RETIREMENT STAGING AREA =====================
        // Pool resources that RealizePool has taken OUT of m_pool (a shrink or
        // a desc change) but has NOT buried yet. They are buried at the top of
        // the NEXT Execute(), and the one-frame delay is the whole point.
        //
        // WHY, in one sentence: a graveyard replays its due prefix in BURIAL
        // ORDER, so a view must be BURIED before the texture it views, and
        // burying the texture from inside RealizePool put it ahead of every
        // node view that had not been buried yet -- destroying an image and
        // then a descriptor over it, which is the use-after-free this file's
        // teardown comments and PoolEpoch's own contract call out.
        //
        // The inversion, concretely: RealizePool runs
        // mid-Execute, after the declarations and before the exec fns. It
        // buried at m_submitValue; the exec fns that follow call their node's
        // SyncPoolEpoch, which buries THAT node's cached views at
        // DebugSubmitCount() -- the same value, the same lane -- i.e. AFTER.
        // Worse, a node that is not in this frame's shape at all (the outline
        // chain with the selection toggled off; the post chain between
        // re-wires) does not run, so its views were buried FRAMES later.
        //
        // The pairing that closes both halves:
        //   1. this staging area moves the texture's burial to the next
        //      Execute(), i.e. after every burial the previous frame made; and
        //   2. NriGraphContext calls SyncPoolEpoch on EVERY node it owns at
        //      DECLARATION time -- which is before that flush -- so a node that
        //      skipped a frame still buries its views first.
        // Neither alone is sufficient; see NriGraphContext::BuildFrame.
        //
        // A staged entry is never "lost": ReleaseGpuResourcesInternal flushes
        // it too (both the public release and ~RenderGraph go through it), so
        // the only thing that ever changes is WHEN the destroy thunk is
        // recorded -- never whether.
        std::vector<PoolResource>  m_retiredPool;

        // Views over POOL textures. These persist across frames, because the
        // graph OWNS the texture each one names: a pool texture only ever goes
        // away through buryPoolResource(), which sweeps the views naming it in
        // the same breath (the VIEWS are buried immediately even though the
        // texture is staged -- that ordering is the fix, not an inconsistency).
        // Nothing can invalidate one behind the graph's back, so caching them
        // is free and correct.
        std::vector<CachedView>    m_views;

        // Views over IMPORTED textures. These are PER-EXECUTE: buried and
        // rebuilt every frame, never carried.
        //
        // Not an oversight and not symmetry-breaking for its own sake -- it is
        // the only correct treatment. The graph does not own an imported
        // texture and gets no signal when its owner destroys it; the obvious
        // one (NriSwapChain::Resize, which destroys and recreates every
        // backbuffer) happens entirely outside the graph. Caching by pointer
        // cannot survive that, because NRI may hand a recreated texture the
        // address a destroyed one just vacated -- and a stale nri::Descriptor
        // still names the OLD native image, so the pointer comparison reports
        // a hit and the frame binds freed memory as a render target.
        //
        // COST, since this is a hot path: one CreateTextureView plus one
        // deferred DestroyDescriptor per imported ATTACHMENT per frame -- ONE
        // of each in Phase 2's frame (the backbuffer). The rejected
        // alternatives were an epoch counter on NriSwapChain (covers the
        // swapchain only, leaves every plain ImportTexture() caller exposed to
        // the identical bug) and a documented "invalidate after resize" call
        // (a contract the frame driver can silently get wrong, which is
        // precisely what this replaced).
        std::vector<CachedView>    m_importedViews;

        std::vector<GpuFrameSlot>  m_frames;            // FramesInFlight() entries once realized
        nri::Fence*                m_fence = nullptr;   // this graph's own submission timeline

        // Per-execution resolution: which physical resource each declared
        // texture/buffer slot maps to THIS frame. Rebuilt at the top of
        // every Execute() (the swapchain entry changes every frame), read by
        // RenderGraphNodeContext::Resolve.
        std::vector<nri::Texture*> m_resolvedTextures;
        std::vector<nri::Buffer*>  m_resolvedBuffers;

        // Transient pool slot each declared resource maps to this frame, or
        // kRgNoPoolSlot for imported resources / untouched transients.
        std::vector<std::uint32_t> m_texturePoolSlot;
        std::vector<std::uint32_t> m_bufferPoolSlot;

        // Cross-frame handover bookkeeping, reset at the top of every
        // Execute(): which pool slots have already had their first barrier of
        // this frame emitted (and therefore patched from PoolResource::carry),
        // and what `before` that barrier actually carried.
        std::vector<char>                   m_poolCarrySeeded;
        std::vector<nri::AccessLayoutStage> m_poolFirstBefore;

        std::uint64_t m_submitValue           = 0;   // successful submits == last signalled fence value
        std::uint64_t m_transientCreateCount  = 0;   // lifetime physical-transient creations
        // Bumped on every pool BURIAL -- see PoolEpoch(). Deliberately not
        // derivable from m_transientCreateCount: a shrink destroys without
        // creating, so a create counter cannot see it.
        std::uint64_t m_poolEpoch             = 0;

        // Reused across every EmitBarriers call so translating a barrier
        // group costs no allocation on the record path once the frame shape
        // has settled (cleared, never shrunk). Same reason NodeDesc::
        // passLabel is precomputed.
        std::vector<nri::TextureBarrierDesc> m_scratchTextureBarriers;
        std::vector<nri::BufferBarrierDesc>  m_scratchBufferBarriers;
        std::vector<nri::AttachmentDesc>     m_scratchColorAttachments;
    };
}
