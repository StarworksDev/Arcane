#include "RenderGraph.hpp"
#include <Arcane/Core/Constant.hpp>

#include <Arcane/Base/Assert.hpp>

#include <cstring>
#include <string>
#include <utility>

namespace
{
    // =================================================================
    // The usage -> (access, layout, stage) mapping table (Task 4).
    //
    // Every value below is an NRI v180 enumerator from
    // ThirdParty/NRI/Include/NRIDescs.h; the bit numbers in the table are
    // that header's own NriBit() indices, quoted so a reviewer can check
    // each row against the enum definitions without leaving this file.
    // The idiom (an nri::AccessLayoutStage triple per state, transitioned
    // by mutating before/after) is Phase 1's -- the retired triangle smoke's
    // three hand-written transitions, which this table exists to replace.
    //
    //  RgUsage        access (AccessBits)              layout (Layout)             stages (StageBits)
    //  -------------  -------------------------------  --------------------------  ---------------------------------
    //  ColorWrite     COLOR_ATTACHMENT  (bits 5|6:      COLOR_ATTACHMENT            COLOR_ATTACHMENT   (bit 9)
    //                   _READ | _WRITE, the umbrella
    //                   -- ROP blending reads as well
    //                   as writes)
    //  DepthWrite     DEPTH_STENCIL_ATTACHMENT          DEPTH_STENCIL_ATTACHMENT    DEPTH_STENCIL_ATTACHMENT (bit 8)
    //                   (bits 7|8: depth test reads and
    //                   depth write writes; NRI's legacy
    //                   D3D12 translator collapses this to DEPTH_WRITE)
    //  ShaderRead     SHADER_RESOURCE   (bit 15)        SHADER_RESOURCE             VERTEX|FRAGMENT|COMPUTE_SHADER
    //                                                                                 (bits 1|7|11) -- see below
    //  ShaderWriteCs  SHADER_RESOURCE_STORAGE (bit 16)  SHADER_RESOURCE_STORAGE     COMPUTE_SHADER     (bit 11)
    //  CopySrc        COPY_SOURCE       (bit 18)        COPY_SOURCE                 COPY               (bit 20)
    //  CopyDst        COPY_DESTINATION  (bit 19)        COPY_DESTINATION            COPY               (bit 20)
    //  Present        NONE              (0)             PRESENT                     NONE               (0x7FFFFFFF)
    //  ReadbackHost   COPY_DESTINATION  (bit 19)        COPY_DESTINATION            COPY               (bit 20)
    //  IndirectArgs   ARGUMENT_BUFFER   (bit 3)         --  (buffers only)          INDIRECT           (bit 23)
    //
    // Notes a reviewer should check the table against:
    //
    // * NRIDescs.h annotates every AccessBits row with the StageBits it is
    //   compatible with; each row above uses that compatible stage rather
    //   than StageBits::ALL (which is 0, NRI's "lazy default"). The retired
    //   Phase-1 smoke left stages at that lazy default because it hand-wrote
    //   three barriers on one texture; a derivation engine can afford to be
    //   precise, and precise stages are strictly tighter synchronisation.
    //
    // * Present is the one row NRI dictates outright: Layout::PRESENT's own
    //   comment reads 'NONE (use "after.stages = StageBits::NONE")'.
    //
    // * ShaderRead's stage set is deliberately NOT StageBits::ALL_SHADERS or
    //   GRAPHICS_SHADERS. Those umbrellas pull in tessellation, task/mesh and
    //   ray-tracing stages, which on Vulkan require device features this
    //   engine does not enable -- naming an unsupported stage in a barrier is
    //   a validation error, not a no-op. VERTEX|FRAGMENT|COMPUTE is exactly
    //   the set Phase 2's 2D pipelines use.
    //   Widening it is a one-constant edit HERE when a later phase adds a
    //   geometry/mesh/RT stage -- and it must be widened then, or a shader
    //   read from that stage will race.
    //
    // * ReadbackHost derives the SAME state as CopyDst by design: the
    //   difference between them is a PLACEMENT hint (a readback buffer lives
    //   in host-readable memory) consumed by Task 5/6's allocator, not a
    //   state difference. A CopyDst -> ReadbackHost edge therefore correctly
    //   produces no barrier.
    // =================================================================
    constexpr nri::StageBits kShaderReadStages =
        nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER | nri::StageBits::COMPUTE_SHADER;

    // The state a resource is in before anything has touched it: a transient
    // has undefined contents, and an imported buffer carries no declared
    // entry state (ImportBuffer takes none). This is the brief's
    // `before = {UNKNOWN, UNDEFINED}` -- there is no special case beyond it,
    // first use is an ordinary barrier that happens to start here.
    constexpr nri::AccessLayoutStage kUnknownState{
        nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL };

    bool SameState(const nri::AccessLayoutStage& a, const nri::AccessLayoutStage& b) noexcept
    {
        return a.access == b.access && a.layout == b.layout && a.stages == b.stages;
    }

    nri::AccessLayoutStage StateFor(Arcane::RgUsage usage, bool isTexture) noexcept
    {
        nri::AccessLayoutStage state = kUnknownState;
        switch (usage)
        {
        case Arcane::RgUsage::ColorWrite:
            state = { nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT,
                      nri::StageBits::COLOR_ATTACHMENT };
            break;
        case Arcane::RgUsage::DepthWrite:
            state = { nri::AccessBits::DEPTH_STENCIL_ATTACHMENT, nri::Layout::DEPTH_STENCIL_ATTACHMENT,
                      nri::StageBits::DEPTH_STENCIL_ATTACHMENT };
            break;
        case Arcane::RgUsage::ShaderRead:
            state = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, kShaderReadStages };
            break;
        case Arcane::RgUsage::ShaderWriteCs:
            state = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE,
                      nri::StageBits::COMPUTE_SHADER };
            break;
        case Arcane::RgUsage::CopySrc:
            state = { nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY };
            break;
        case Arcane::RgUsage::CopyDst:
        case Arcane::RgUsage::ReadbackHost:   // same state by design -- see the table
            state = { nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY };
            break;
        case Arcane::RgUsage::Present:
            state = { nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE };
            break;
        case Arcane::RgUsage::IndirectArgs:
            // Buffers only (an indirect-draw argument buffer has no texture
            // reading); ARGUMENT_BUFFER's one compatible stage is INDIRECT
            // (NRIDescs.h's AccessBits table). The layout is forced UNDEFINED
            // below like every other buffer state.
            state = { nri::AccessBits::ARGUMENT_BUFFER, nri::Layout::UNDEFINED, nri::StageBits::INDIRECT };
            break;
        }

        // nri::BufferBarrierDesc's before/after are nri::AccessStage --
        // access + stages, no layout at all. Forcing UNDEFINED keeps a field
        // Task 6 will drop from ever reading as meaningful (RenderGraph.hpp,
        // RgBarrier).
        if (!isTexture)
            state.layout = nri::Layout::UNDEFINED;

        // NO DECLARED USAGE MAY DERIVE kUnknownState. Two rules downstream
        // rest on this and would both fail SILENTLY if a new RgUsage
        // enumerator were added without a case here and fell through to the
        // `state = kUnknownState` initialiser above:
        //
        //  * a transient's FIRST USE would emit no barrier at all (Compile's
        //    `!SameState(current, want)` sees no edge out of kUnknownState);
        //  * the executor's cross-frame handover patch is GUARDED on
        //    `before == {NONE, UNDEFINED, ...}` (RenderGraphExec.cpp), so it
        //    would silently claim a real first-use barrier as a slot's
        //    unpatched one -- and, being already unable to tell the two
        //    apart, would leave the reused resource with no availability
        //    operation for the previous frame's writes.
        //
        // Compile-time coverage would be better, but the switch above is over
        // a plain enum with no MAX_NUM sentinel, so there is nothing for
        // -Wswitch to bite on. The gate's mapping-table test walks every
        // enumerator against this same property.
        ARC_ASSERT(!SameState(state, kUnknownState),
                   "RenderGraph: an RgUsage derived the unknown state -- a missing StateFor case");
        return state;
    }
}

namespace Arcane
{
    void RenderGraph::AddNode(const char* name, NodeKind kind, Setup setup, Exec exec)
    {
        NodeDesc node;
        node.name = name != nullptr ? name : "";
        // Composed once, here, and never again: the executor opens a
        // breadcrumb scope + GPU marker under this exact string for this node
        // on EVERY frame (see NodeDesc::passLabel).
        node.passLabel = "pass:" + node.name;
        node.kind = kind;
        node.exec = std::move(exec);

        const std::size_t nodeIndex = m_nodes.size();
        m_nodes.push_back(std::move(node));

        // `setup` runs synchronously, right here -- eager setup, deferred
        // execute (see the header comment). m_currentNodeIndex is this
        // call's node for the duration, so SetColorAttachments/
        // SetDepthAttachment (reached from within `setup` via a captured
        // RenderGraph reference) target the right node.
        m_currentNodeIndex = nodeIndex;
        if (setup)
        {
            RenderGraphBuilder builder(*this, nodeIndex);
            setup(builder);
        }

        // Re-index fresh rather than keeping a reference across `setup`:
        // nothing in this task calls AddNode() reentrantly from within a
        // `setup`, but indexing fresh costs nothing and matches the
        // discipline the rest of the NRI substrate uses around callables
        // that could reallocate a vector out from under a cached reference
        // (Graveyard::ExecutePrefix).
        if (kind == NodeKind::Raster)
        {
            NodeDesc& declared = m_nodes[nodeIndex];
            const bool hasAttachment = !declared.colorAttachments.empty()
                                        || declared.depthAttachment.index != kInvalid;
            declared.hasRequiredAttachments = hasAttachment;

            // Non-fatal, recorded rather than refused -- the same
            // assert-later shape RecordAccess() uses below for a
            // never-written transient: a Raster node with no attachment is
            // still declared (NodeHasRequiredAttachments() reports the
            // miss), it just is not valid input for Task 4's Compile().
            ARC_ENSURE(hasAttachment,
                       "RenderGraph::AddNode: a Raster node was declared with no color or "
                       "depth attachment -- call SetColorAttachments/SetDepthAttachment "
                       "during Setup");
        }

        m_currentNodeIndex = kNoCurrentNode;
    }

    void RenderGraph::SetColorAttachments(std::span<const RgTexture> attachments)
    {
        ARC_ASSERT(m_currentNodeIndex < m_nodes.size(),
                   "RenderGraph::SetColorAttachments: no node's Setup is currently running -- "
                   "call this only from within the Setup passed to AddNode()");
        m_nodes[m_currentNodeIndex].colorAttachments.assign(attachments.begin(), attachments.end());
    }

    void RenderGraph::SetDepthAttachment(RgTexture attachment)
    {
        ARC_ASSERT(m_currentNodeIndex < m_nodes.size(),
                   "RenderGraph::SetDepthAttachment: no node's Setup is currently running -- "
                   "call this only from within the Setup passed to AddNode()");
        m_nodes[m_currentNodeIndex].depthAttachment = attachment;
    }

    void RenderGraph::SetColorAttachmentOps(std::span<const RgAttachmentOp> ops)
    {
        ARC_ASSERT(m_currentNodeIndex < m_nodes.size(),
                   "RenderGraph::SetColorAttachmentOps: no node's Setup is currently running -- "
                   "call this only from within the Setup passed to AddNode()");
        m_nodes[m_currentNodeIndex].colorOps.assign(ops.begin(), ops.end());
    }

    void RenderGraph::SetDepthAttachmentOp(RgAttachmentOp op)
    {
        ARC_ASSERT(m_currentNodeIndex < m_nodes.size(),
                   "RenderGraph::SetDepthAttachmentOp: no node's Setup is currently running -- "
                   "call this only from within the Setup passed to AddNode()");
        NodeDesc& node = m_nodes[m_currentNodeIndex];
        node.depthOp    = op;
        node.hasDepthOp = true;
    }

    // ------------------------------------------------------------------
    // Compile -- the whole barrier derivation. PURE: reads m_nodes/
    // m_textures/m_buffers/m_accesses, writes nothing, calls no nri device
    // entry point. See the header's Compile() comment for the refusal list
    // and the INDEX SPACE note above RgBarrier for what the indices mean.
    // ------------------------------------------------------------------
    std::optional<RgCompiled> RenderGraph::Compile(std::string* outError) const
    {
        const auto fail = [outError](std::string message) -> std::optional<RgCompiled>
        {
            if (outError != nullptr)
                *outError = std::move(message);
            return std::nullopt;
        };

        // Every refusal message names its resource through this, so the
        // caller reads a declaration-site name instead of a slot number.
        const auto resourceName = [this](std::uint32_t slot, bool isTexture) -> const std::string&
        {
            return isTexture ? m_textures[slot].name : m_buffers[slot].name;
        };

        // Does node `nodeIndex` declare any access at all to this resource?
        // Used only by the attachment check below -- an attachment with no
        // declared access gets no derived transition, so binding it would
        // bind whatever layout the resource was last left in.
        const auto nodeAccessesResource = [this](std::size_t nodeIndex, std::uint32_t slot, bool isTexture)
        {
            for (const Access& access : m_accesses)
            {
                if (access.nodeIndex == nodeIndex && access.isTexture == isTexture && access.resourceIndex == slot)
                    return true;
            }
            return false;
        };

        // --------------------------------------------------------------
        // Pass 0 (Task 3, Phase 4): RgTextureDesc.mipCount validation. A
        // texture always has at least one mip level -- 1 (the default)
        // already means "no chain" -- so 0 has no realizable meaning and is
        // refused here, up front, rather than reaching RealizePool and
        // becoming either a silent 1-mip texture or an nri::TextureDesc the
        // backend rejects for a reason far from this declaration.
        //
        // Fix round 1: the upper bound. nri::Dim_t is 16-bit
        // (NRIDescs.h:61), and RealizePool's static_cast<nri::Dim_t> does
        // not saturate -- it wraps mod 65536, exactly like the width/height
        // guard three lines below this one in RenderGraphExec.cpp (same
        // Dim_t, same failure mode, same "refuse instead of realizing the
        // wrong thing" reasoning). Left unguarded, a mipCount that is an
        // exact multiple of 65536 would sail past the == 0 check above
        // (checked pre-cast) and truncate to EXACTLY 0 at the cast --
        // silently producing the one value this whole pass exists to
        // prevent, through a different door. Both mipCount rules belong in
        // this one Compile()-time pass rather than split across Compile()
        // and RealizePool: mipCount, unlike a transient's width/height, is
        // never resolved late (no swapchain-extent-style deferral applies
        // to it), so there is no functional reason to defer either half of
        // its validity range to realize time.
        // --------------------------------------------------------------
        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            if (m_textures[i].kind != ResourceKind::Transient)
                continue;   // ImportTexture carries no RgTextureDesc to validate
            if (m_textures[i].desc.mipCount == 0)
            {
                return fail("RenderGraph::Compile: transient texture '" + m_textures[i].name
                            + "' declares mipCount = 0 -- 1 means no chain, not 0");
            }
            if (m_textures[i].desc.mipCount > 0xFFFFu)
            {
                return fail("RenderGraph::Compile: transient texture '" + m_textures[i].name
                            + "' declares a mipCount NRI cannot express (nri::Dim_t is 16-bit)");
            }
        }

        // --------------------------------------------------------------
        // Pass 1: structural validation of the Raster nodes' attachments.
        // This is where the attachment handles get decoded -- the setters
        // (SetColorAttachments/SetDepthAttachment) deliberately store them
        // unvalidated, so this is their one and only validation point, and
        // it goes through DecodeAndValidateSlot() like every other handle
        // consumer in the class.
        // --------------------------------------------------------------
        for (std::size_t nodeIndex = 0; nodeIndex < m_nodes.size(); ++nodeIndex)
        {
            const NodeDesc& node = m_nodes[nodeIndex];
            if (node.kind != NodeKind::Raster)
                continue;

            if (!node.hasRequiredAttachments)
            {
                return fail("RenderGraph::Compile: Raster node '" + node.name + "' declares no colour or depth "
                            "attachment -- call SetColorAttachments/SetDepthAttachment during its Setup");
            }

            const auto checkAttachment = [&](RgTexture attachment, const char* which) -> std::optional<std::string>
            {
                const std::size_t slot = DecodeAndValidateSlot(attachment.index, m_textures.size());
                if (slot == kNoSlot)
                {
                    return "RenderGraph::Compile: Raster node '" + node.name + "' has a " + which
                         + " attachment whose handle is invalid, stale, or out of range (held across a Reset()?)";
                }
                if (!nodeAccessesResource(nodeIndex, static_cast<std::uint32_t>(slot), /*isTexture=*/true))
                {
                    return "RenderGraph::Compile: Raster node '" + node.name + "' attaches texture '"
                         + m_textures[slot].name + "' as a " + which + " attachment but declares no Read()/Write() "
                           "for it -- without a declared access the graph derives no layout transition for it";
                }
                return std::nullopt;
            };

            for (const RgTexture& color : node.colorAttachments)
            {
                if (std::optional<std::string> problem = checkAttachment(color, "colour"))
                    return fail(std::move(*problem));
            }
            // kInvalid is "no depth attachment", not a bad handle -- checked
            // verbatim, exactly as DecodeAndValidateSlot does.
            if (node.depthAttachment.index != kInvalid)
            {
                if (std::optional<std::string> problem = checkAttachment(node.depthAttachment, "depth"))
                    return fail(std::move(*problem));
            }
        }

        // --------------------------------------------------------------
        // Pass 2: the first node that WRITES each resource. A Read() before
        // that node reads undefined content (declaration order IS execution
        // order in Phase 2). Node-granular deliberately: a node may read a
        // transient it also writes itself -- both accesses resolve to the
        // one state that node runs in -- regardless of the order the two
        // were declared in. Imported resources arrive with a defined entry
        // state, so they count as written before node 0.
        // --------------------------------------------------------------
        std::vector<std::uint32_t> textureFirstWrite(m_textures.size(), kRgNoNode);
        std::vector<std::uint32_t> bufferFirstWrite(m_buffers.size(), kRgNoNode);
        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            if (m_textures[i].kind == ResourceKind::Imported)
                textureFirstWrite[i] = 0;
        }
        for (std::size_t i = 0; i < m_buffers.size(); ++i)
        {
            if (m_buffers[i].kind == ResourceKind::Imported)
                bufferFirstWrite[i] = 0;
        }
        for (const Access& access : m_accesses)
        {
            if (!access.isWrite)
                continue;
            const std::size_t count = access.isTexture ? m_textures.size() : m_buffers.size();
            if (access.resourceIndex >= count)
                continue;   // reported by the in-range guard in pass 3
            std::uint32_t& first = access.isTexture ? textureFirstWrite[access.resourceIndex]
                                                    : bufferFirstWrite[access.resourceIndex];
            const std::uint32_t node = static_cast<std::uint32_t>(access.nodeIndex);
            if (first == kRgNoNode || node < first)
                first = node;
        }

        RgCompiled compiled;
        compiled.nodes.resize(m_nodes.size());

        // --------------------------------------------------------------
        // Pass 3: transient enumeration + lifetimes. Textures in slot order
        // first, then buffers -- the order RgCompiled::transients documents
        // and everything index-parallel to it depends on.
        //
        // This and pass 4 run BEFORE the node walk on purpose (fix round 1):
        // the walk needs each pool slot's tenancy to seed a handover
        // correctly, and both are derivable from the declared accesses alone
        // -- neither reads a single barrier.
        // --------------------------------------------------------------
        ARC_CONSTANT("sentinel: no transient resource")
        constexpr std::size_t kNoTransient = static_cast<std::size_t>(-1);
        std::vector<std::size_t> textureTransient(m_textures.size(), kNoTransient);
        std::vector<std::size_t> bufferTransient(m_buffers.size(), kNoTransient);

        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            if (m_textures[i].kind != ResourceKind::Transient)
                continue;
            textureTransient[i] = compiled.transients.size();
            compiled.transients.push_back(RgTransient{ static_cast<std::uint32_t>(i), /*isTexture=*/true });
        }
        for (std::size_t i = 0; i < m_buffers.size(); ++i)
        {
            if (m_buffers[i].kind != ResourceKind::Transient)
                continue;
            bufferTransient[i] = compiled.transients.size();
            compiled.transients.push_back(RgTransient{ static_cast<std::uint32_t>(i), /*isTexture=*/false });
        }

        compiled.transientLifetimes.assign(compiled.transients.size(), RgCompiled::Lifetime{});
        for (std::size_t t = 0; t < compiled.transients.size(); ++t)
        {
            const RgTransient& transient = compiled.transients[t];
            RgCompiled::Lifetime& lifetime = compiled.transientLifetimes[t];
            for (const Access& access : m_accesses)
            {
                if (access.isTexture != transient.isTexture || access.resourceIndex != transient.resourceIndex)
                    continue;
                const std::uint32_t node = static_cast<std::uint32_t>(access.nodeIndex);
                if (lifetime.first == kRgNoNode || node < lifetime.first)
                    lifetime.first = node;
                if (lifetime.last == kRgNoNode || node > lifetime.last)
                    lifetime.last = node;
            }
        }

        // --------------------------------------------------------------
        // Pass 4: pool-slot assignment. Greedy first fit over the
        // transients in enumeration order: a transient joins the first pool
        // slot whose desc matches EXACTLY and whose already-assigned
        // lifetimes it does not overlap. Lifetimes are INCLUSIVE node-index
        // spans, so two transients that are both live at the same node
        // overlap and cannot share.
        //
        // Slot ids are one numbering across textures and buffers: a texture
        // and a buffer never share an id, so Task 6 can key its pool on the
        // slot id alone and read each slot's kind/desc off the transient
        // that maps to it.
        //
        // Greedy in ENUMERATION order, not sorted by first use. Interval
        // colouring is only provably minimal when intervals are visited in
        // start order, so a graph that declares its transients out of
        // first-use order can end up with more pool slots than strictly
        // necessary. That is a packing inefficiency, never a correctness
        // bug -- the overlap test below is against real lifetimes, so a
        // shared slot is always safe. Sorting by lifetime.first (carrying a
        // permutation so the index-parallel output stays in enumeration
        // order) is the upgrade if a profile ever shows the transient pool
        // is the thing costing memory.
        // --------------------------------------------------------------
        const auto descsMatch = [this](const RgTransient& a, const RgTransient& b)
        {
            if (a.isTexture != b.isTexture)
                return false;
            if (a.isTexture)
            {
                const RgTextureDesc& lhs = m_textures[a.resourceIndex].desc;
                const RgTextureDesc& rhs = m_textures[b.resourceIndex].desc;
                return lhs.format == rhs.format && lhs.width == rhs.width && lhs.height == rhs.height
                    // Task 3 (Phase 4): a mismatch here would hand a pass a
                    // physical texture with fewer mips than it declared.
                    && lhs.mipCount == rhs.mipCount
                    && lhs.depthStencil == rhs.depthStencil;
            }
            const BufferResource& lhs = m_buffers[a.resourceIndex];
            const BufferResource& rhs = m_buffers[b.resourceIndex];
            return lhs.size == rhs.size && lhs.usage == rhs.usage;
        };

        struct PoolSlot
        {
            std::size_t representative;
            std::vector<RgCompiled::Lifetime> occupied;
            bool hasOptimizedClearValue = false;
            nri::ClearValue optimizedClearValue{};
        };
        std::vector<PoolSlot> pool;
        compiled.transientPoolSlot.assign(compiled.transients.size(), kRgNoPoolSlot);

        for (std::size_t t = 0; t < compiled.transients.size(); ++t)
        {
            const RgCompiled::Lifetime& lifetime = compiled.transientLifetimes[t];
            if (lifetime.first == kRgNoNode)
                continue;   // no node touches it -- nothing to realize, no slot

            bool assigned = false;
            for (std::size_t p = 0; p < pool.size() && !assigned; ++p)
            {
                if (!descsMatch(compiled.transients[pool[p].representative], compiled.transients[t]))
                    continue;

                const RgTransient& candidate = compiled.transients[t];
                if (candidate.isTexture)
                {
                    const RgTextureDesc& desc = m_textures[candidate.resourceIndex].desc;
                    if (desc.hasOptimizedClearValue && pool[p].hasOptimizedClearValue
                        && std::memcmp(&desc.optimizedClearValue, &pool[p].optimizedClearValue,
                                       sizeof(nri::ClearValue)) != 0)
                    {
                        continue;
                    }
                }

                bool overlaps = false;
                for (const RgCompiled::Lifetime& occupied : pool[p].occupied)
                {
                    if (lifetime.first <= occupied.last && occupied.first <= lifetime.last)
                    {
                        overlaps = true;
                        break;
                    }
                }
                if (overlaps)
                    continue;

                pool[p].occupied.push_back(lifetime);
                if (candidate.isTexture)
                {
                    const RgTextureDesc& desc = m_textures[candidate.resourceIndex].desc;
                    if (desc.hasOptimizedClearValue && !pool[p].hasOptimizedClearValue)
                    {
                        pool[p].hasOptimizedClearValue = true;
                        pool[p].optimizedClearValue = desc.optimizedClearValue;
                    }
                }
                compiled.transientPoolSlot[t] = static_cast<std::uint32_t>(p);
                assigned = true;
            }

            if (!assigned)
            {
                compiled.transientPoolSlot[t] = static_cast<std::uint32_t>(pool.size());
                PoolSlot slot{ t, { lifetime } };
                const RgTransient& candidate = compiled.transients[t];
                if (candidate.isTexture)
                {
                    const RgTextureDesc& desc = m_textures[candidate.resourceIndex].desc;
                    if (desc.hasOptimizedClearValue)
                    {
                        slot.hasOptimizedClearValue = true;
                        slot.optimizedClearValue = desc.optimizedClearValue;
                    }
                }
                pool.push_back(slot);
            }
        }
        compiled.poolSlotCount = static_cast<std::uint32_t>(pool.size());

        // --------------------------------------------------------------
        // Pass 5: walk the nodes in declaration order, tracking each
        // resource's current state and emitting one barrier per real state
        // change. Imported textures seed from their declared entry state;
        // everything else seeds from kUnknownState -- EXCEPT a transient
        // taking over a pool slot that already had a tenant. See the
        // handover block below for why that case is different.
        // --------------------------------------------------------------
        std::vector<nri::AccessLayoutStage> textureState(m_textures.size(), kUnknownState);
        std::vector<nri::AccessLayoutStage> bufferState(m_buffers.size(), kUnknownState);
        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            if (m_textures[i].kind == ResourceKind::Imported)
                textureState[i] = m_textures[i].importEntry;
        }
        // Imported BUFFERS keep kUnknownState: ImportBuffer() takes no entry
        // state, so the graph cannot assume one. A buffer barrier out of
        // {NONE, ALL} is the conservative, always-correct start.

        // POOL HANDOVER (fix round 1; `before` reshaped by the D2 dx12 fix).
        // Barrier state is tracked per LOGICAL resource, but two transients
        // that share a pool slot are ONE physical nri::Texture/nri::Buffer at
        // execution time. Seeding the second tenant's first use from
        // kUnknownState would emit `before.access = NONE`, which performs no
        // source availability operation for the FIRST tenant's writes -- a
        // spec-level write-after-write hazard across the reused object (what
        // Vulkan sync-validation reports as SYNC-HAZARD-WRITE-AFTER-WRITE, and
        // intermittent corruption with sync-val off).
        //
        // So each pool slot carries the state its current occupant left the
        // physical resource in, and a new tenant's first-use `before` takes
        // that slot's WHOLE triple -- access, stages AND LAYOUT. The handover
        // is therefore an ordinary state-to-state transition, not a
        // contents-discarding one.
        //
        // WHY THE LAYOUT IS CARRIED AND NOT FORCED TO UNDEFINED. It used to be
        // forced ("discard, never inherit contents"): legal on Vulkan, ILLEGAL
        // on D3D12, and it was the D2 blocker -- every `--nri-graph --dx12`
        // run died with EndCommandBuffer -> ID3D12GraphicsCommandList::Close()
        // failing. NRI's D3D12 backend derives LayoutBefore and AccessBefore
        // INDEPENDENTLY -- GetBarrierLayout is HANDED the access mask, but its
        // only access-sensitive branch is Layout::INPUT_ATTACHMENT, which
        // StateFor above never produces, so for our states the two really are
        // uncoupled -- and then sets D3D12_TEXTURE_BARRIER_FLAG_DISCARD from
        // the layout alone (ThirdParty/NRI/Source/D3D12/CommandBufferD3D12.hpp
        // :1054-1056, :170-175 and :1078-1079 -- the last carries NRI's own
        // `// TODO: verify that it works`). So {previous access, UNDEFINED}
        // became a D3D12_TEXTURE_BARRIER pairing LayoutBefore = UNDEFINED with
        // AccessBefore != NO_ACCESS *and* FLAG_DISCARD; D3D12 enhanced
        // barriers reject that -- an UNDEFINED-layout side is the "no previous
        // access" side, and FLAG_DISCARD is only valid on such a side. NRI
        // states the same contract in its own public enum: Layout::UNDEFINED's
        // "Compatible AccessBits" column is EMPTY (Include/NRIDescs.h:544-547).
        // The rejection is a deferred, list-invalidating parameter error, so it
        // surfaces at Close() rather than at the CmdBarrier call.
        //
        // The alternative -- keep UNDEFINED and zero the access instead -- is
        // D3D12-legal but reopens the exact hazard this block exists to close:
        // the sync/stage half alone is an EXECUTION dependency, and a WAW needs
        // a MEMORY dependency (a source availability operation over the
        // previous writes' access scope) on both backends. Zeroing the access
        // is what produced the sync-val WAW report in the first place.
        //
        // Losing the discard is a lost driver HINT, nothing more: the pool's
        // new tenant either clears its target or fully overwrites it, so
        // inheriting the old contents is unobservable.
        //
        // AND THE HANDOVER BARRIER IS ALWAYS EMITTED. Carrying the real layout
        // means a handover can now be state-IDENTICAL (X leaves the slot in
        // COLOR_ATTACHMENT, Y wants COLOR_ATTACHMENT), which the
        // consecutive-same-state elision below would swallow -- and eliding a
        // handover is exactly the availability operation this block exists to
        // emit. `handover` forces it; see the emission site.
        //
        // A slot with no previous tenant (first tenant, or a slot nothing
        // else shares) is untouched by this and still seeds from
        // kUnknownState exactly as before.
        std::vector<nri::AccessLayoutStage> poolState(compiled.poolSlotCount, kUnknownState);
        std::vector<char> poolHasTenant(compiled.poolSlotCount, 0);
        // `char`, not bool: these are read through a reference below, and
        // std::vector<bool>'s proxy reference cannot bind to one.
        std::vector<char> textureSeeded(m_textures.size(), 0);
        std::vector<char> bufferSeeded(m_buffers.size(), 0);

        // The resources this node has already transitioned, so a second
        // access to the same resource within one node collapses (same state)
        // or refuses (different state) rather than emitting a second entry
        // into a batch that can only transition each resource once.
        struct NodeTouch { std::uint32_t resourceIndex; bool isTexture; nri::AccessLayoutStage state; };
        std::vector<NodeTouch> touched;

        for (std::size_t nodeIndex = 0; nodeIndex < m_nodes.size(); ++nodeIndex)
        {
            RgCompiledNode& compiledNode = compiled.nodes[nodeIndex];
            compiledNode.nodeIndex = static_cast<std::uint32_t>(nodeIndex);
            touched.clear();

            for (const Access& access : m_accesses)
            {
                if (access.nodeIndex != nodeIndex)
                    continue;

                const bool          isTexture = access.isTexture;
                const std::uint32_t slot      = access.resourceIndex;
                const std::size_t   count     = isTexture ? m_textures.size() : m_buffers.size();

                // Only reachable in a build where RecordAccess()'s
                // ARC_ASSERT is compiled out and a bad handle got recorded
                // as kNoSlot -- refuse rather than subscript out of range.
                if (slot >= count)
                {
                    return fail("RenderGraph::Compile: node '" + m_nodes[nodeIndex].name + "' declares an access to a "
                                + std::string(isTexture ? "texture" : "buffer")
                                + " whose handle was invalid, stale, or out of range at declaration time");
                }

                if (!access.isWrite)
                {
                    const std::uint32_t firstWrite = isTexture ? textureFirstWrite[slot] : bufferFirstWrite[slot];
                    if (firstWrite == kRgNoNode || static_cast<std::uint32_t>(nodeIndex) < firstWrite)
                    {
                        return fail("RenderGraph::Compile: node '" + m_nodes[nodeIndex].name + "' reads "
                                    + std::string(isTexture ? "texture" : "buffer") + " '" + resourceName(slot, isTexture)
                                    + "' before any node writes it -- its contents are undefined");
                    }
                }

                // Pool handover: the first time this resource is touched,
                // pick up the physical object's outgoing state if a previous
                // tenant left one in the same pool slot. See the block above
                // pool the state vectors for the full reasoning.
                const std::size_t transientIndex = isTexture ? textureTransient[slot] : bufferTransient[slot];
                const std::uint32_t poolSlot     = transientIndex != kNoTransient
                                                 ? compiled.transientPoolSlot[transientIndex]
                                                 : kRgNoPoolSlot;

                nri::AccessLayoutStage& current = isTexture ? textureState[slot] : bufferState[slot];
                char& seeded                    = isTexture ? textureSeeded[slot] : bufferSeeded[slot];
                bool handover                   = false;
                if (seeded == 0)
                {
                    if (poolSlot != kRgNoPoolSlot && poolHasTenant[poolSlot] != 0)
                    {
                        // The WHOLE triple, layout included -- an ordinary
                        // state-to-state transition. See POOL HANDOVER above.
                        current  = poolState[poolSlot];
                        handover = true;
                    }
                    seeded = 1;
                }

                const nri::AccessLayoutStage want = StateFor(access.usage, isTexture);

                bool alreadyTouched = false;
                for (const NodeTouch& touch : touched)
                {
                    if (touch.resourceIndex != slot || touch.isTexture != isTexture)
                        continue;
                    if (!SameState(touch.state, want))
                    {
                        return fail("RenderGraph::Compile: node '" + m_nodes[nodeIndex].name
                                    + "' declares two different states for " + std::string(isTexture ? "texture" : "buffer")
                                    + " '" + resourceName(slot, isTexture)
                                    + "' -- one node runs in exactly one state per resource, and the two usages "
                                      "are a read/write hazard against each other");
                    }
                    alreadyTouched = true;
                    break;
                }
                if (alreadyTouched)
                    continue;

                touched.push_back(NodeTouch{ slot, isTexture, want });

                // Consecutive same-state declarations produce NO barrier --
                // EXCEPT a pool handover, which is emitted unconditionally.
                // Two tenants of one slot are two different LOGICAL resources
                // on one physical object, so "same state" there does not mean
                // "nothing happened between them": the previous tenant's
                // writes still need the source availability operation only a
                // barrier performs. Before the D2 dx12 fix the forced
                // UNDEFINED layout guaranteed this by construction (nothing
                // ever WANTS UNDEFINED, so `before != after` always held);
                // carrying the real layout removes that accident, so the rule
                // is stated outright. See POOL HANDOVER above.
                if (handover || !SameState(current, want))
                {
                    compiledNode.preBarriers.push_back(RgBarrier{ slot, isTexture, current, want });
                    current = want;
                }

                // Whether or not a barrier was emitted, the physical object
                // behind this pool slot is now in `current` -- that is what
                // the slot's NEXT tenant seeds its handover from.
                if (poolSlot != kRgNoPoolSlot)
                {
                    poolState[poolSlot]     = current;
                    poolHasTenant[poolSlot] = 1;
                }
            }
        }

        // --------------------------------------------------------------
        // Pass 6: exit barriers. Every imported TEXTURE's ImportTexture()
        // call promised the caller an exit state; restore it if the graph
        // left the texture somewhere else. An imported texture no node
        // touched still gets one (entry -> exit) when the two differ -- the
        // promise does not depend on the graph having used the resource.
        // Imported BUFFERS have no declared exit state, and TRANSIENTS have no
        // exit CONTRACT to honour either -- unlike an imported texture's
        // importExit, nobody promised a caller what state a transient's pool
        // slot is left in. (The physical resource itself is not scoped to
        // this frame: the pool survives Reset() and can carry a slot across
        // frames -- see PoolResource::carry -- it is just that nothing here
        // needs restoring TO.) So neither appears here.
        // --------------------------------------------------------------
        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            if (m_textures[i].kind != ResourceKind::Imported)
                continue;
            const nri::AccessLayoutStage& exit = m_textures[i].importExit;
            if (SameState(textureState[i], exit))
                continue;
            compiled.exitBarriers.push_back(
                RgBarrier{ static_cast<std::uint32_t>(i), /*isTexture=*/true, textureState[i], exit });
        }

        return compiled;
    }

    void RenderGraph::Reset()
    {
        // DECLARATIONS ONLY -- see the header's Reset() comment. Nothing here
        // touches a GPU resource: the transient pool and the cached
        // attachment views survive, so the next frame's Execute() reuses
        // every pool slot whose desc still matches. Burying here would make
        // the pool-reuse property unreachable in the one loop shape that
        // exists, because a per-frame driver has to Reset() to clear
        // declarations at all.
        m_nodes.clear();
        m_textures.clear();
        m_buffers.clear();
        m_accesses.clear();
        m_currentNodeIndex = kNoCurrentNode;

        // Derived from the declarations just cleared, so they go with them.
        // Execute() rebuilds all four from the next compile before anything
        // reads them; leaving last frame's contents behind would make this
        // graph briefly describe resources it no longer declares.
        m_resolvedTextures.clear();
        m_resolvedBuffers.clear();
        m_texturePoolSlot.clear();
        m_bufferPoolSlot.clear();

        ++m_generation;
    }

    // ------------------------------------------------------------------
    // The handle-encoding seam -- see the RgTexture/RgBuffer comment in
    // RenderGraph.hpp and the private-section comment above these
    // declarations. Nothing outside this pair may pack or unpack `.index`.
    // ------------------------------------------------------------------

    std::uint32_t RenderGraph::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept
    {
        return ((generation & kGenerationMask) << kIndexBits) | (slot & kIndexMask);
    }

    std::size_t RenderGraph::DecodeAndValidateSlot(std::uint32_t encoded, std::size_t resourceCount) const noexcept
    {
        // kInvalid is checked verbatim, before any decode -- it stays
        // unambiguous no matter the (slot, generation) split.
        if (encoded == kInvalid)
            return kNoSlot;

        const std::uint32_t slot       = encoded & kIndexMask;
        const std::uint32_t generation = encoded >> kIndexBits;

        // Compare mod 256 against the graph's CURRENT generation, not a
        // per-slot stamp: this is what catches the common case (a stale
        // handle used after Reset() on a graph re-declared with the same
        // or a larger shape) -- a stale handle's generation never matches
        // the post-Reset generation regardless of whether its slot is
        // still in bounds.
        if (generation != (m_generation & kGenerationMask))
            return kNoSlot;
        if (slot >= resourceCount)
            return kNoSlot;
        return slot;
    }

    const char* RenderGraph::NodeName(std::size_t nodeIndex) const
    {
        ARC_ASSERT(nodeIndex < m_nodes.size(), "RenderGraph::NodeName: index out of range");
        return m_nodes[nodeIndex].name.c_str();
    }

    bool RenderGraph::NodeHasRequiredAttachments(std::size_t nodeIndex) const
    {
        ARC_ASSERT(nodeIndex < m_nodes.size(), "RenderGraph::NodeHasRequiredAttachments: index out of range");
        return m_nodes[nodeIndex].hasRequiredAttachments;
    }

    const RgAttachmentOp* RenderGraph::ColorLoadOp(std::size_t nodeIndex, std::size_t attachment) const
    {
        if (nodeIndex >= m_nodes.size())
            return nullptr;
        const NodeDesc& node = m_nodes[nodeIndex];
        if (attachment >= node.colorAttachments.size() || attachment >= node.colorOps.size())
            return nullptr;
        return &node.colorOps[attachment];
    }

    const RgAttachmentOp* RenderGraph::DepthLoadOp(std::size_t nodeIndex) const
    {
        if (nodeIndex >= m_nodes.size())
            return nullptr;
        const NodeDesc& node = m_nodes[nodeIndex];
        return node.hasDepthOp ? &node.depthOp : nullptr;
    }

    const char* RenderGraph::NameOf(RgTexture texture) const
    {
        const std::size_t slot = DecodeAndValidateSlot(texture.index, m_textures.size());
        ARC_ASSERT(slot != kNoSlot, "RenderGraph::NameOf(RgTexture): handle is invalid, stale, or out of range");
        return m_textures[slot].name.c_str();
    }

    const char* RenderGraph::NameOf(RgBuffer buffer) const
    {
        const std::size_t slot = DecodeAndValidateSlot(buffer.index, m_buffers.size());
        ARC_ASSERT(slot != kNoSlot, "RenderGraph::NameOf(RgBuffer): handle is invalid, stale, or out of range");
        return m_buffers[slot].name.c_str();
    }

    bool RenderGraph::IsTransient(RgTexture texture) const
    {
        const std::size_t slot = DecodeAndValidateSlot(texture.index, m_textures.size());
        ARC_ASSERT(slot != kNoSlot, "RenderGraph::IsTransient(RgTexture): handle is invalid, stale, or out of range");
        return m_textures[slot].kind == ResourceKind::Transient;
    }

    bool RenderGraph::IsTransient(RgBuffer buffer) const
    {
        const std::size_t slot = DecodeAndValidateSlot(buffer.index, m_buffers.size());
        ARC_ASSERT(slot != kNoSlot, "RenderGraph::IsTransient(RgBuffer): handle is invalid, stale, or out of range");
        return m_buffers[slot].kind == ResourceKind::Transient;
    }

    bool RenderGraph::WasWritten(RgTexture texture) const
    {
        const std::size_t slot = DecodeAndValidateSlot(texture.index, m_textures.size());
        ARC_ASSERT(slot != kNoSlot, "RenderGraph::WasWritten(RgTexture): handle is invalid, stale, or out of range");
        return m_textures[slot].everWritten;
    }

    bool RenderGraph::WasRead(RgTexture texture) const
    {
        const std::size_t slot = DecodeAndValidateSlot(texture.index, m_textures.size());
        if (slot == kNoSlot)
            return false;
        for (const Access& access : m_accesses)
            if (access.isTexture && access.resourceIndex == slot && !access.isWrite)
                return true;
        return false;
    }

    bool RenderGraph::WasWritten(RgBuffer buffer) const
    {
        const std::size_t slot = DecodeAndValidateSlot(buffer.index, m_buffers.size());
        ARC_ASSERT(slot != kNoSlot, "RenderGraph::WasWritten(RgBuffer): handle is invalid, stale, or out of range");
        return m_buffers[slot].everWritten;
    }

    bool RenderGraph::IsHandleValid(RgTexture texture) const noexcept
    {
        return DecodeAndValidateSlot(texture.index, m_textures.size()) != kNoSlot;
    }

    bool RenderGraph::IsHandleValid(RgBuffer buffer) const noexcept
    {
        return DecodeAndValidateSlot(buffer.index, m_buffers.size()) != kNoSlot;
    }

    RgTexture RenderGraph::CreateTextureInternal(const char* name, const RgTextureDesc& desc)
    {
        TextureResource resource;
        resource.name = name != nullptr ? name : "";
        resource.desc = desc;
        resource.kind = ResourceKind::Transient;
        resource.everWritten = false; // undefined content until the first declared Write()

        ARC_ASSERT(m_textures.size() < kIndexMask,
                   "RenderGraph: texture slot count exceeds the encoded handle's 24-bit capacity");
        const std::uint32_t slot = static_cast<std::uint32_t>(m_textures.size());
        m_textures.push_back(std::move(resource));
        return RgTexture{ EncodeHandle(slot, m_generation) };
    }

    RgTexture RenderGraph::ImportTextureInternal(const char* name, nri::Texture* texture,
                                                  nri::AccessLayoutStage entry, nri::AccessLayoutStage exit,
                                                  bool persistent, bool swapChain)
    {
        TextureResource resource;
        resource.name = name != nullptr ? name : "";
        resource.kind = ResourceKind::Imported;
        resource.imported = texture;
        resource.importEntry = entry;
        resource.importExit = exit;
        resource.persistent = persistent;
        resource.swapChain = swapChain;
        resource.everWritten = true; // see TextureResource::everWritten in the header

        ARC_ASSERT(m_textures.size() < kIndexMask,
                   "RenderGraph: texture slot count exceeds the encoded handle's 24-bit capacity");
        const std::uint32_t slot = static_cast<std::uint32_t>(m_textures.size());
        m_textures.push_back(std::move(resource));
        return RgTexture{ EncodeHandle(slot, m_generation) };
    }

    RgBuffer RenderGraph::CreateBufferInternal(const char* name, std::uint64_t size, nri::BufferUsageBits usage)
    {
        BufferResource resource;
        resource.name = name != nullptr ? name : "";
        resource.size = size;
        resource.usage = usage;
        resource.kind = ResourceKind::Transient;
        resource.everWritten = false;

        ARC_ASSERT(m_buffers.size() < kIndexMask,
                   "RenderGraph: buffer slot count exceeds the encoded handle's 24-bit capacity");
        const std::uint32_t slot = static_cast<std::uint32_t>(m_buffers.size());
        m_buffers.push_back(std::move(resource));
        return RgBuffer{ EncodeHandle(slot, m_generation) };
    }

    RgBuffer RenderGraph::ImportBufferInternal(const char* name, nri::Buffer* buffer, std::uint64_t size)
    {
        BufferResource resource;
        resource.name = name != nullptr ? name : "";
        resource.size = size;
        resource.kind = ResourceKind::Imported;
        resource.imported = buffer;
        resource.everWritten = true; // see TextureResource::everWritten in the header

        ARC_ASSERT(m_buffers.size() < kIndexMask,
                   "RenderGraph: buffer slot count exceeds the encoded handle's 24-bit capacity");
        const std::uint32_t slot = static_cast<std::uint32_t>(m_buffers.size());
        m_buffers.push_back(std::move(resource));
        return RgBuffer{ EncodeHandle(slot, m_generation) };
    }

    void RenderGraph::RecordAccess(std::size_t nodeIndex, std::uint32_t encodedHandle,
                                    bool isTexture, bool isWrite, RgUsage usage)
    {
        // Decode once, here -- the single seam (see the class comment):
        // everything downstream (everWritten flip, the stored Access) works
        // in plain decoded slots, never the encoded handle value.
        const std::size_t resourceCount = isTexture ? m_textures.size() : m_buffers.size();
        const std::size_t slot = DecodeAndValidateSlot(encodedHandle, resourceCount);

        if (isTexture)
        {
            ARC_ASSERT(slot != kNoSlot,
                       "RenderGraph: RgTexture handle is invalid, stale, or out of range "
                       "(held across a Reset()?)");
            // slot != kNoSlot guards the subscript itself, not just the assert:
            // ARC_ASSERT compiles out in Release, and kNoSlot is size_t(-1), so
            // without this a bad handle would be an OOB write here, before
            // Compile()'s own guarded check (this function's "Only reachable
            // in a build where..." sibling below) ever gets a chance to refuse.
            if (isWrite && slot != kNoSlot)
                m_textures[slot].everWritten = true;
        }
        else
        {
            ARC_ASSERT(slot != kNoSlot,
                       "RenderGraph: RgBuffer handle is invalid, stale, or out of range "
                       "(held across a Reset()?)");
            if (isWrite && slot != kNoSlot)
                m_buffers[slot].everWritten = true;
        }

        // A Read() of a never-written transient is accepted here -- the
        // assert-later model (plan brief, Task 3): declaration never fails
        // for this, it only records the access (this Access entry) and
        // leaves everWritten as-is (still false for that case). Task 4's
        // Compile() walks these and refuses, naming the resource.
        m_accesses.push_back(Access{ nodeIndex, static_cast<std::uint32_t>(slot), isTexture, isWrite, usage });
    }

    RgTexture RenderGraphBuilder::CreateTexture(const char* name, const RgTextureDesc& desc)
    {
        return m_graph.CreateTextureInternal(name, desc);
    }

    RgTexture RenderGraphBuilder::ImportTexture(const char* name, nri::Texture* texture,
                                                 nri::AccessLayoutStage entry, nri::AccessLayoutStage exit,
                                                 bool persistent)
    {
        return m_graph.ImportTextureInternal(name, texture, entry, exit, persistent, /*swapChain=*/false);
    }

    RgTexture RenderGraphBuilder::ImportSwapChainTexture(const char* name)
    {
        // Fixed entry/exit -- see the header for why they are not the
        // caller's to choose. The exit triple is the one nri::Layout::PRESENT
        // mandates ('NONE (use "after.stages = StageBits::NONE")'), which is
        // what StateFor(RgUsage::Present) derives.
        constexpr nri::AccessLayoutStage kAcquiredState{
            nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL };
        constexpr nri::AccessLayoutStage kPresentState{
            nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE };

        // No nri::Texture* yet: Execute() acquires it and fills the
        // resolution table. Nothing between here and there dereferences it
        // (Compile() reads importEntry/importExit only).
        return m_graph.ImportTextureInternal(name, /*texture=*/nullptr, kAcquiredState, kPresentState,
                                              /*persistent=*/false, /*swapChain=*/true);
    }

    RgBuffer RenderGraphBuilder::CreateBuffer(const char* name, std::uint64_t size, nri::BufferUsageBits usage)
    {
        return m_graph.CreateBufferInternal(name, size, usage);
    }

    RgBuffer RenderGraphBuilder::ImportBuffer(const char* name, nri::Buffer* buffer, std::uint64_t size)
    {
        return m_graph.ImportBufferInternal(name, buffer, size);
    }

    void RenderGraphBuilder::Read(RgTexture texture, RgUsage usage)
    {
        m_graph.RecordAccess(m_nodeIndex, texture.index, /*isTexture=*/true, /*isWrite=*/false, usage);
    }

    void RenderGraphBuilder::Read(RgBuffer buffer, RgUsage usage)
    {
        m_graph.RecordAccess(m_nodeIndex, buffer.index, /*isTexture=*/false, /*isWrite=*/false, usage);
    }

    void RenderGraphBuilder::Write(RgTexture texture, RgUsage usage)
    {
        m_graph.RecordAccess(m_nodeIndex, texture.index, /*isTexture=*/true, /*isWrite=*/true, usage);
    }

    void RenderGraphBuilder::Write(RgBuffer buffer, RgUsage usage)
    {
        m_graph.RecordAccess(m_nodeIndex, buffer.index, /*isTexture=*/false, /*isWrite=*/true, usage);
    }
}
