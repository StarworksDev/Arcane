// RenderGraph, EXECUTION half. The declaration and
// compile halves live in RenderGraph.cpp and are PURE -- see RenderGraph.hpp's
// header for why the class is split across two TUs and why it must stay that
// way.
//
// What this TU owns: every GPU resource the graph holds (the transient pool,
// the cached attachment views, one command allocator + command buffer per
// frame slot, and the graph's own submission fence), the translation of
// RgCompiled into recorded NRI commands, and present sequencing.
//
// THE ONE CmdBarrier CALL SITE. Every barrier on the graph path is emitted by
// EmitBarriers() below, from an RgBarrier list Compile() derived. Nothing in
// this file synthesizes, reorders, drops or coalesces a barrier, and no other
// file on the graph path may call CmdBarrier at all (spec ratification 1;
// the one hand-written exemption in the tree is NriDiagnostics' fault
// dispatch, which states its own terms).
// The `before` triples Compile() hands over already account for pool-slot
// handover between transients, so replaying them verbatim is not laziness --
// re-deriving anything here would silently diverge from the derivation Task
// 4's tests pin.
//
// Include order: NRI headers first, ALWAYS (see NriCommon.hpp). NriDevice.hpp
// pulls in Extensions/NRIDeviceCreation.h, which declares nri::Message::ERROR;
// <windows.h> reaches this TU through Arcane/Base/Log.hpp -> spdlog and
// #defines ERROR via wingdi.h, corrupting every later textual "ERROR" in the
// TU if it lands first.
#include "RenderGraph.hpp"

#include "NriCommon.hpp"
#include "NriDevice.hpp"
#include "NriSwapChain.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/FramePacing.hpp>   // FramesInFlight()

#undef ERROR

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace Arcane
{
    namespace
    {
        // Task 1's tagged error seam. "nri-graph" rather than "nri": these
        // are the GRAPH's own refusals (a caller contract broken, a compile
        // that does not match the declarations), not an NRI call's result --
        // those go through ARC_NRI_CHECK, which tags "[nri]" and routes a
        // typed DEVICE_LOST into the device-removed chain (NriCommon.cpp).
        // Both land in the same RenderErrorCount() latch the 0/0 gate reads.
        void GraphError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        // ---------------------------------------------------------------
        // NodeScope -- one graph node, both marker channels.
        // ---------------------------------------------------------------
        // The NRI-side twin of Render/GpuInstrumentation.hpp's GpuPassScope,
        // deliberately the same SHAPE (F-2c-bis is binding here too:
        // markers-only DRED with no annotation markers yields an EMPTY
        // breadcrumb list, strictly worse than no DRED at all), over TWO
        // channels:
        //
        //   CmdBeginAnnotation / CmdEndAnnotation  (PIX/RenderDoc/DRED)
        //   backend->WriteMarkerNative(native)     (the marker buffer), with
        //                                          the native list from
        //                                          GetCommandBufferNativeObject
        //
        // It is not a GpuPassScope subclass or a template over the two: the
        // classes share no member and no branch once the two calls differ.
        //
        // The crash backend comes from the process-wide slot
        // NriDiagnostics::Arm installs, and there is no other writer.
        // Every step degrades independently: no backend, an unarmed backend, a
        // backend that hands back no native command list, or a backend whose
        // marker buffer lives on ANOTHER DEVICE each disable exactly their own
        // channel and nothing else.
        //
        // THE CROSS-DEVICE RULE. Writing a native marker means recording,
        // into THIS command buffer, a write to the backend's marker buffer --
        // and both APIs require the two to share ONE device. A backend built
        // over a different device than the one recording produces, on Vulkan:
        //
        //   vkCmdWriteBufferMarkerAMD(): dstBuffer (VkBuffer ...) was created
        //   ... from VkDevice A, but command is using ... VkDevice B
        //   VUID-vkCmdWriteBufferMarkerAMD-commonparent
        //
        // and on dx12 the identical bug, mutely (WriteBufferImmediate against
        // a GPU virtual address minted by another device).
        //
        // So the marker is GATED ON DEVICE IDENTITY rather than assumed:
        // `nativeDevice` is this graph device's own native handle, and the
        // write happens only when the backend's marker buffer lives on that
        // same device. The gate cannot be forgotten and cannot be wrong in the
        // unsafe direction.
        //
        // WHAT IS NOT LOST if the gate ever closes: the CPU-side breadcrumb
        // ring (Breadcrumbs().BeginScope/EndScope) and the PIX/RenderDoc/DRED
        // annotation are both device-agnostic and both still emitted -- and the
        // ring is the half today's hang and crash reports are actually built
        // from. What would be lost is the GPU-written marker VALUE, i.e. how
        // far into the frame the GPU got.
        std::atomic<bool> g_crossDeviceMarkersNoted{ false };

        class NodeScope
        {
        public:
            // `nativeDevice` is GetDeviceNativeObject for the device `cmd` was
            // allocated from -- resolved once per Execute by the caller, not
            // per node.
            NodeScope(const nri::CoreInterface& core, nri::CommandBuffer& cmd, const std::string& label,
                      void* nativeDevice) noexcept
                : m_core(&core), m_cmd(&cmd)
            {
                // The annotation goes out even with no crash backend
                // installed: it is what a PIX/RenderDoc capture and D3D12
                // DRED's markers-only tier read, and DRED enablement is
                // process-global and independent of whether a backend object
                // exists.
                m_core->CmdBeginAnnotation(*m_cmd, label.c_str(), nri::BGRA_UNUSED);
                m_annotated = true;

                // Latched, not re-read in the destructor: a teardown that
                // cleared the slot mid-scope must not leave a BeginScope
                // without its EndScope, nor hand the end marker to a
                // different backend than the begin marker. Same rule as
                // GpuPassScope's.
                m_backend = ActiveGpuCrashBackend();
                if (!m_backend)
                    return;

                m_token  = m_backend->Breadcrumbs().BeginScope(label);
                m_scoped = true;

                // Null on the NONE backend by construction (ImplNONE returns
                // nullptr) and on any backend that has no native list to
                // give -- both are ordinary, not errors.
                void* const native = m_core->GetCommandBufferNativeObject(m_cmd);
                if (!native)
                    return;

                // The cross-device rule (see this class's header comment). A
                // mismatch leaves m_native null, so the destructor's mirror
                // check keeps the two halves paired without a second flag.
                if (!nativeDevice || m_backend->NativeDevice() != nativeDevice)
                {
                    if (!g_crossDeviceMarkersNoted.exchange(true, std::memory_order_acq_rel))
                    {
                        ARC_WARN("[nri-graph] native GPU markers are OFF on the graph path: the "
                                 "active {} crash backend's marker buffer belongs to another "
                                 "device (Phase 2 holds two). CPU breadcrumbs and pass annotations "
                                 "are unaffected; Phase 3's one-device flip restores them.",
                                 m_backend->Name());
                    }
                    return;
                }

                m_native = native;
                (void)m_backend->WriteMarkerNative(m_native, m_token, true);
            }

            ~NodeScope()
            {
                // Exact mirror of the constructor's order so the scopes nest.
                if (m_scoped && m_backend)
                {
                    if (m_native)
                        (void)m_backend->WriteMarkerNative(m_native, m_token, false);
                    m_backend->Breadcrumbs().EndScope(m_token);
                }
                if (m_annotated)
                    m_core->CmdEndAnnotation(*m_cmd);
            }

            NodeScope(const NodeScope&)            = delete;
            NodeScope& operator=(const NodeScope&) = delete;

        private:
            const nri::CoreInterface* m_core      = nullptr;
            nri::CommandBuffer*       m_cmd       = nullptr;
            IGpuCrashBackend*         m_backend   = nullptr;
            void*                     m_native    = nullptr;
            std::uint32_t             m_token     = 0;
            bool                      m_annotated = false;
            bool                      m_scoped    = false;
        };

        // The usage bits ONE declared access implies for the texture it
        // touches. This is the other half of Task 4's usage -> (access,
        // layout, stage) table: NRI's validation layer checks every barrier's
        // access AND layout against the texture's usage mask
        // (Source/Validation/CommandBufferVal.hpp, IsAccessMaskSupported /
        // IsTextureLayoutSupported), so a pool texture realized without the
        // bit some tenant's barrier needs has EVERY barrier on it rejected.
        //
        // Copy usages (CopySrc/CopyDst/ReadbackHost) and Present contribute
        // NOTHING on purpose: NRI constrains neither COPY_SOURCE/
        // COPY_DESTINATION nor Layout::PRESENT against the usage mask, and
        // both backends add the transfer bits unconditionally at creation
        // (VK: DeviceVK.hpp always ORs VK_IMAGE_USAGE_TRANSFER_SRC|DST). A
        // copy-only transient therefore legitimately realizes with usage
        // NONE.
        nri::TextureUsageBits TextureUsageFor(RgUsage usage) noexcept
        {
            switch (usage)
            {
            case RgUsage::ColorWrite:    return nri::TextureUsageBits::COLOR_ATTACHMENT;
            case RgUsage::DepthWrite:    return nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
            case RgUsage::ShaderRead:    return nri::TextureUsageBits::SHADER_RESOURCE;
            case RgUsage::ShaderWriteCs: return nri::TextureUsageBits::SHADER_RESOURCE_STORAGE;
            case RgUsage::CopySrc:
            case RgUsage::CopyDst:
            case RgUsage::ReadbackHost:
            case RgUsage::Present:
            case RgUsage::IndirectArgs:  break;   // buffers only: no texture usage bit exists for it
            }
            return nri::TextureUsageBits::NONE;
        }

        bool SameTextureDesc(const nri::TextureDesc& a, const nri::TextureDesc& b) noexcept
        {
            return a.type == b.type && a.usage == b.usage && a.format == b.format
                && a.width == b.width && a.height == b.height && a.depth == b.depth
                && a.mipNum == b.mipNum && a.layerNum == b.layerNum && a.sampleNum == b.sampleNum
                && std::memcmp(&a.optimizedClearValue, &b.optimizedClearValue,
                               sizeof(nri::ClearValue)) == 0;
        }

        bool SameBufferDesc(const nri::BufferDesc& a, const nri::BufferDesc& b) noexcept
        {
            return a.size == b.size && a.structureStride == b.structureStride && a.usage == b.usage;
        }
    }

    // ==================================================================
    // RenderGraphNodeContext -- resolution, from within a node's exec fn.
    // ==================================================================

    nri::Texture* RenderGraphNodeContext::Resolve(RgTexture texture) const
    {
        if (!graph)
            return nullptr;
        // Through the ONE handle seam, exactly as the header's private
        // section requires of every handle-consuming method -- so a handle
        // minted before a Reset() fails to decode here rather than indexing
        // this frame's table with last frame's slot.
        const std::size_t slot = graph->DecodeAndValidateSlot(texture.index, graph->m_textures.size());
        if (slot == RenderGraph::kNoSlot || slot >= graph->m_resolvedTextures.size())
            return nullptr;
        return graph->m_resolvedTextures[slot];
    }

    nri::Buffer* RenderGraphNodeContext::Resolve(RgBuffer buffer) const
    {
        if (!graph)
            return nullptr;
        const std::size_t slot = graph->DecodeAndValidateSlot(buffer.index, graph->m_buffers.size());
        if (slot == RenderGraph::kNoSlot || slot >= graph->m_resolvedBuffers.size())
            return nullptr;
        return graph->m_resolvedBuffers[slot];
    }

    nri::Descriptor* RenderGraphNodeContext::ColorView(RgTexture texture) const
    {
        nri::Texture* resolved = Resolve(texture);
        if (!resolved || !graph)
            return nullptr;
        return graph->ViewForTexture(resolved, /*depth=*/false);
    }

    // ==================================================================
    // Lifetime
    // ==================================================================

    RenderGraph::~RenderGraph()
    {
        // Buried, not destroyed: the last submission may still be reading
        // them. They go into THIS GRAPH'S LANE (RgExecuteDesc::graves), and
        // the lane's owner is what drains it behind a DeviceWaitIdle -- the
        // one operation that can honor Graveyard's "the caller has already
        // made the GPU idle" contract on the way out, and the reason
        // RgExecuteDesc documents that BOTH the device and the lane must
        // outlive the graph.
        //
        // NriGraphContext therefore destroys its graph EXPLICITLY, in its
        // destructor body, immediately before that drain. Leaving it to the
        // member destructor would put this tail in a lane whose only remaining
        // drain is ~Graveyard's -- fatal in Debug, un-fenced in Release. That
        // is header window (a), and this is the half of its closure that lives
        // here: the tail is buried in a lane somebody still drains.
        ReleaseGpuResourcesInternal(/*all=*/true);
    }

    void RenderGraph::ReleaseGpuResources()
    {
        // The public, frame-driver-facing half: pool + views only. The
        // command slots and the fence are execution machinery and stay --
        // a graph released this way is immediately usable again, it just
        // realizes its pool from scratch on the next Execute().
        ReleaseGpuResourcesInternal(/*all=*/false);
    }

    void RenderGraph::ReleaseImportedViews()
    {
        if (!m_device || !m_graves || m_importedViews.empty())
            return;

        Graveyard&                graves = *m_graves;
        const nri::CoreInterface* core   = &m_device->Core();

        // m_submitValue is the submission that last USED these views, so
        // burial at it says exactly "free once that frame has retired" -- and
        // it keeps every burial in this file keyed off the same monotonically
        // growing counter, which is what makes Graveyard's nondecreasing
        // invariant hold no matter how release, re-realization and this
        // interleave.
        for (const CachedView& cached : m_importedViews)
        {
            if (cached.view)
                graves.Bury(m_submitValue, [core, view = cached.view] { core->DestroyDescriptor(view); });
        }
        m_importedViews.clear();
    }

    void RenderGraph::FlushRetiredPool()
    {
        if (m_retiredPool.empty() || !m_device || !m_graves)
            return;

        Graveyard&                graves = *m_graves;
        const nri::CoreInterface* core   = &m_device->Core();

        // m_submitValue, like every other burial in this file. At the top of an
        // Execute() this is the value the LAST submission signalled -- the same
        // value the node views buried during (and at the declaration of) that
        // frame carry -- so Graveyard's nondecreasing rule holds and the due
        // prefix replays views-then-textures, which is the entire point of the
        // staging area. From ReleaseGpuResourcesInternal it is the same value
        // that call's own burials use, appended after them.
        for (const PoolResource& retired : m_retiredPool)
        {
            if (retired.texture)
                graves.Bury(m_submitValue, [core, t = retired.texture] { core->DestroyTexture(t); });
            if (retired.buffer)
                graves.Bury(m_submitValue, [core, b = retired.buffer] { core->DestroyBuffer(b); });
        }
        m_retiredPool.clear();
    }

    void RenderGraph::ReleaseGpuResourcesInternal(bool all)
    {
        if (!m_device || !m_graves)
        {
            // Nothing was ever realized (no Execute), so there is nothing to
            // bury -- and neither a device to destroy it through nor a lane to
            // bury it into. The two are latched together at Execute's entry, so
            // this one test covers both; the second half is spelled out anyway
            // because it is what makes the never-entered-Execute window (header
            // window (b1)) a NON-event rather than a fence-0 burial.
            return;
        }

        Graveyard&                graves = *m_graves;
        const nri::CoreInterface* core   = &m_device->Core();

        // Every burial in this call uses the SAME fence value -- the last
        // value this graph signalled. m_submitValue only ever grows (one per
        // successful submit) and every burial path in this file keys off its
        // CURRENT value, so Graveyard's nondecreasing invariant holds across
        // any interleaving of release, re-realization and destruction. A
        // graph that never submitted buries at 0, which is correct: nothing
        // it created was ever seen by the GPU.
        const std::uint64_t fence = m_submitValue;

        for (const CachedView& cached : m_views)
        {
            if (cached.view)
                graves.Bury(fence, [core, view = cached.view] { core->DestroyDescriptor(view); });
        }
        m_views.clear();

        // The imported half normally turns over inside Execute(); this catches
        // the frame that never came (a release, or teardown).
        ReleaseImportedViews();

        // Anything an earlier Execute() retired and staged, buried HERE rather
        // than stranded: the next Execute() that would have flushed it may
        // never come (this call IS the teardown on the ~RenderGraph path).
        // After the view burials above and before the pool's below, which keeps
        // this call's own burial order views-then-resources exactly like every
        // other path's.
        FlushRetiredPool();

        // The POOL EPOCH bumps here too (RenderGraph.hpp): this path is the
        // one the owner DOES get to sequence -- NriGraphContext invalidates
        // its nodes before calling in -- but a node's epoch check must still
        // agree with reality afterwards, and a release that left the epoch
        // unchanged would leave a node believing its (now buried) views were
        // still current.
        if (!m_pool.empty())
            ++m_poolEpoch;
        for (const PoolResource& slot : m_pool)
        {
            if (slot.texture)
                graves.Bury(fence, [core, texture = slot.texture] { core->DestroyTexture(texture); });
            if (slot.buffer)
                graves.Bury(fence, [core, buffer = slot.buffer] { core->DestroyBuffer(buffer); });
        }
        m_pool.clear();

        m_resolvedTextures.clear();
        m_resolvedBuffers.clear();
        m_texturePoolSlot.clear();
        m_bufferPoolSlot.clear();

        // The pool is gone, so every carry state it held went with it -- the
        // next Execute() realizes fresh resources that genuinely start
        // undefined, and must not be handed a state from a buried one.
        m_poolCarrySeeded.clear();
        m_poolFirstBefore.clear();

        if (!all)
            return;

        // Command buffer before its allocator (the buffer is allocated OUT of
        // it).
        for (const GpuFrameSlot& slot : m_frames)
        {
            if (slot.cmd)
                graves.Bury(fence, [core, cmd = slot.cmd] { core->DestroyCommandBuffer(cmd); });
            if (slot.allocator)
                graves.Bury(fence, [core, allocator = slot.allocator] { core->DestroyCommandAllocator(allocator); });
        }
        m_frames.clear();

        if (m_fence)
        {
            graves.Bury(fence, [core, f = m_fence] { core->DestroyFence(f); });
            m_fence = nullptr;
        }
    }

    // ==================================================================
    // Realization
    // ==================================================================

    bool RenderGraph::EnsureExecutionResources()
    {
        if (!m_frames.empty() && m_fence)
            return true;

        const nri::CoreInterface& core = m_device->Core();

        if (!m_fence)
        {
            // Starts at 0 and is signalled with m_submitValue + 1 per
            // successful submit, so GetFenceValue() reads "submissions this
            // graph has COMPLETED" -- which is exactly what the graveyard
            // needs to reap against, and the only completion signal a
            // headless (swapchain-less) graph has at all.
            if (!ARC_NRI_CHECK(core.CreateFence(m_device->Device(), 0, m_fence)) || !m_fence)
            {
                m_fence = nullptr;
                return false;
            }
            core.SetDebugName(m_fence, "RenderGraph submission fence");
        }

        if (m_frames.empty())
        {
            m_frames.resize(FramesInFlight());
            for (GpuFrameSlot& slot : m_frames)
            {
                // Short-circuit order matters: a failed CreateCommandAllocator
                // leaves slot.allocator null, and CreateCommandBuffer takes it
                // by reference.
                if (!ARC_NRI_CHECK(core.CreateCommandAllocator(*m_device->GraphicsQueue(), slot.allocator))
                    || !slot.allocator
                    || !ARC_NRI_CHECK(core.CreateCommandBuffer(*slot.allocator, slot.cmd))
                    || !slot.cmd)
                {
                    // ALL-OR-NOTHING: bury whatever was created and leave
                    // m_frames EMPTY. A half-filled vector would satisfy this
                    // function's "already realized" early-out above on the
                    // next call, and Execute() would then reset a null
                    // allocator -- turning a create failure into a crash one
                    // frame later.
                    //
                    // THE LANE MATTERS MOST HERE (header window (b2), the
                    // sharpest edge): this runs SYNCHRONOUSLY inside a FAILING
                    // Execute, at m_submitValue == 0, before any destructor.
                    // Into a graveyard some other context had already driven to
                    // N that is a nondecreasing-assert in Debug and a foreign
                    // reap in Release; into this graph's own lane it is simply
                    // the first burial that lane ever saw.
                    Graveyard&                graves    = *m_graves;
                    const nri::CoreInterface* graveCore = &core;
                    for (const GpuFrameSlot& partial : m_frames)
                    {
                        if (partial.cmd)
                            graves.Bury(m_submitValue, [graveCore, c = partial.cmd]
                                        { graveCore->DestroyCommandBuffer(c); });
                        if (partial.allocator)
                            graves.Bury(m_submitValue, [graveCore, a = partial.allocator]
                                        { graveCore->DestroyCommandAllocator(a); });
                    }
                    m_frames.clear();

                    GraphError("RenderGraph::Execute: per-frame command buffers could not be created");
                    return false;
                }
            }
        }
        return true;
    }

    bool RenderGraph::RealizePool(const RgCompiled& compiled)
    {
        const nri::CoreInterface& core = m_device->Core();

        // ------------------------------------------------------------
        // Step 1: what the compile ASKS for, per pool slot.
        //
        // Textures need a usage mask, and RgTextureDesc has none: the graph
        // models usage per DECLARED ACCESS, not per resource. So each slot's
        // mask is the UNION over every tenant's every access (plus the
        // attachment roles below) -- because all those tenants share ONE
        // physical texture, and NRI's validation layer checks each barrier's
        // access and layout against that one texture's mask. Miss a bit and
        // every barrier on the slot is rejected, including the handover
        // barrier whose `before` carries the PREVIOUS tenant's access.
        //
        // Buffers need no union: Compile()'s descsMatch compares usage
        // exactly, so every tenant of a buffer slot declared the same usage
        // and the representative's is the slot's.
        // ------------------------------------------------------------
        struct Desired
        {
            bool             isTexture = true;
            bool             seen      = false;
            nri::TextureDesc texture{};
            nri::BufferDesc  buffer{};
            const std::string* name = nullptr;
        };
        std::vector<Desired> desired(compiled.poolSlotCount);

        if (compiled.transientPoolSlot.size() != compiled.transients.size())
        {
            GraphError("RenderGraph::Execute: the compiled graph's transient vectors disagree in size -- "
                       "it was not produced by a Compile() of these declarations");
            return false;
        }

        for (std::size_t t = 0; t < compiled.transients.size(); ++t)
        {
            const std::uint32_t poolSlot = compiled.transientPoolSlot[t];
            if (poolSlot == kRgNoPoolSlot)
                continue;   // no node touches it -- nothing to realize (RenderGraph.hpp, RgTransient)
            if (poolSlot >= compiled.poolSlotCount)
            {
                GraphError("RenderGraph::Execute: a transient's pool slot is out of range");
                return false;
            }

            const RgTransient& transient = compiled.transients[t];
            const std::size_t  count     = transient.isTexture ? m_textures.size() : m_buffers.size();
            if (transient.resourceIndex >= count)
            {
                GraphError("RenderGraph::Execute: a transient's resource index is out of range -- the compiled "
                           "graph does not describe these declarations");
                return false;
            }

            Desired& slot = desired[poolSlot];
            if (!slot.seen)
            {
                slot.seen      = true;
                slot.isTexture = transient.isTexture;
                if (transient.isTexture)
                {
                    const TextureResource& resource = m_textures[transient.resourceIndex];
                    // nri::Dim_t is uint16_t. A declared extent that does not
                    // round-trip through it would silently become a DIFFERENT
                    // (possibly zero) texture that creates successfully --
                    // refuse instead of realizing the wrong thing.
                    if (resource.desc.width  > 0xFFFFu || resource.desc.height > 0xFFFFu
                        || resource.desc.width == 0 || resource.desc.height == 0)
                    {
                        GraphError("RenderGraph::Execute: transient texture '" + resource.name
                                    + "' has an extent NRI cannot express (nri::Dim_t is 16-bit, and 0 is "
                                      "not a valid dimension)");
                        return false;
                    }
                    slot.name                = &resource.name;
                    slot.texture.type        = nri::TextureType::TEXTURE_2D;
                    slot.texture.format      = resource.desc.format;
                    slot.texture.width       = static_cast<nri::Dim_t>(resource.desc.width);
                    slot.texture.height      = static_cast<nri::Dim_t>(resource.desc.height);
                    slot.texture.depth       = 1;
                    // Task 3 (Phase 4): the declared mip chain. Compile()'s
                    // Pass 0 already refused mipCount == 0 and
                    // mipCount > 0xFFFFu (fix round 1 -- see that pass for
                    // why both halves of the range live there rather than
                    // here beside width/height), so this cast always
                    // round-trips: never truncates, never produces 0.
                    slot.texture.mipNum      = static_cast<nri::Dim_t>(resource.desc.mipCount);
                    slot.texture.layerNum    = 1;
                    slot.texture.sampleNum   = 1;
                    slot.texture.usage       = nri::TextureUsageBits::NONE;
                }
                else
                {
                    const BufferResource& resource = m_buffers[transient.resourceIndex];
                    slot.name          = &resource.name;
                    slot.buffer.size   = resource.size;
                    slot.buffer.usage  = resource.usage;
                }
            }
            else if (slot.isTexture != transient.isTexture)
            {
                // Compile() never assigns a texture and a buffer to one slot
                // (its slot ids are one numbering across both kinds); refuse
                // rather than realize whichever kind was seen first.
                GraphError("RenderGraph::Execute: a pool slot is claimed by both a texture and a buffer");
                return false;
            }

            if (!transient.isTexture)
                continue;

            // A no-clear tenant is compatible with either optimized value.
            // Compile() prevents two tenants with DIFFERENT declared clears
            // from sharing, so the last declared clear here is unambiguous.
            const TextureResource& resource = m_textures[transient.resourceIndex];
            if (resource.desc.hasOptimizedClearValue)
                slot.texture.optimizedClearValue = resource.desc.optimizedClearValue;

            for (const Access& access : m_accesses)
            {
                if (!access.isTexture || access.resourceIndex != transient.resourceIndex)
                    continue;
                slot.texture.usage = slot.texture.usage | TextureUsageFor(access.usage);
            }
        }

        // Attachment roles, ORed in on top of the declared accesses. Compile()
        // requires an attachment to carry SOME declared access but not that
        // the access matches its attachment role (a known, deferred Task-4
        // gap), and creating a COLOR_ATTACHMENT view on a texture without
        // that usage bit is a hard failure rather than a wrong-looking pixel.
        // Cheap insurance, and it never widens a mask the declarations
        // already justify.
        const auto addAttachmentUsage = [&](RgTexture attachment, nri::TextureUsageBits bits)
        {
            const std::size_t slot = DecodeAndValidateSlot(attachment.index, m_textures.size());
            if (slot == kNoSlot || slot >= m_texturePoolSlot.size())
                return;
            const std::uint32_t poolSlot = m_texturePoolSlot[slot];
            if (poolSlot == kRgNoPoolSlot || poolSlot >= desired.size() || !desired[poolSlot].isTexture)
                return;
            desired[poolSlot].texture.usage = desired[poolSlot].texture.usage | bits;
        };
        for (const NodeDesc& node : m_nodes)
        {
            if (node.kind != NodeKind::Raster)
                continue;
            for (const RgTexture& color : node.colorAttachments)
                addAttachmentUsage(color, nri::TextureUsageBits::COLOR_ATTACHMENT);
            if (node.depthAttachment.index != kInvalid)
                addAttachmentUsage(node.depthAttachment, nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT);
        }

        // ------------------------------------------------------------
        // Step 2: realize what is not already realized. A slot whose
        // realized desc still matches is KEPT untouched -- that is what
        // makes a second Execute() of the same compiled graph create nothing
        // (DebugTransientCreateCount()).
        // ------------------------------------------------------------
        Graveyard&                graves    = *m_graves;
        const nri::CoreInterface* graveCore = &core;

        // RETIRES a pool entry: buries every cached view of the graph's OWN
        // that names its texture, then STAGES the resource itself for burial
        // at the top of the next Execute().
        //
        // THE TWO HALVES ARE DELIBERATELY ASYMMETRIC, and that asymmetry IS
        // the ordering rule (see m_retiredPool for the full statement). A
        // Graveyard replays its due prefix in BURIAL ORDER, so "a view must
        // never be destroyed after the texture it views" means "a view must
        // never be buried after it". The graph's own views go in RIGHT HERE,
        // ahead of everything; the texture goes into the staging area, i.e.
        // behind every burial any node makes for the rest of this frame AND at
        // this frame's declaration-time sync. Burying the texture here would
        // put it AHEAD of the node views that follow inside the very same
        // Execute.
        //
        // It also bumps the POOL EPOCH (RenderGraph.hpp), which is the ONLY
        // way a NODE caching its own views over pool textures can learn that
        // this happened: both callers below run in the middle of Execute(),
        // after every declaration and before every exec fn, so there is no
        // ordering hook and no callback the owner could be given. The bump is
        // unconditional -- a slot has been invalidated whether or not it held
        // anything.
        const auto buryPoolResource = [&](PoolResource& entry)
        {
            ++m_poolEpoch;
            if (entry.texture)
            {
                for (std::size_t v = 0; v < m_views.size();)
                {
                    if (m_views[v].texture == entry.texture)
                    {
                        if (m_views[v].view)
                        {
                            graves.Bury(m_submitValue, [graveCore, view = m_views[v].view]
                                        { graveCore->DestroyDescriptor(view); });
                        }
                        m_views.erase(m_views.begin() + static_cast<std::ptrdiff_t>(v));
                        continue;
                    }
                    ++v;
                }
            }
            if (entry.texture || entry.buffer)
            {
                PoolResource retired;
                retired.isTexture = entry.isTexture;
                retired.texture   = entry.texture;
                retired.buffer    = entry.buffer;
                m_retiredPool.push_back(retired);
            }
            entry = PoolResource{};
        };

        for (std::size_t i = compiled.poolSlotCount; i < m_pool.size(); ++i)
            buryPoolResource(m_pool[i]);
        m_pool.resize(compiled.poolSlotCount);

        for (std::uint32_t p = 0; p < compiled.poolSlotCount; ++p)
        {
            const Desired&  want    = desired[p];
            PoolResource&   slot    = m_pool[p];
            const bool      realized = slot.texture != nullptr || slot.buffer != nullptr;

            if (!want.seen)
            {
                // A pool slot Compile() counted but assigned nothing to
                // cannot happen today (poolSlotCount IS the assignment
                // count); leaving it unrealized is the safe reading either
                // way, since nothing can reference it.
                continue;
            }

            if (realized && slot.isTexture == want.isTexture
                && (want.isTexture ? SameTextureDesc(slot.textureDesc, want.texture)
                                    : SameBufferDesc(slot.bufferDesc, want.buffer)))
            {
                continue;   // reuse -- the whole point of DebugTransientCreateCount()
            }

            if (realized)
            {
                // Shape changed (a resize, or a differently-declared frame
                // without a Reset()): bury the old one before replacing it.
                buryPoolResource(slot);
            }

            slot.isTexture = want.isTexture;
            if (want.isTexture)
            {
                // CreateCommittedTexture rather than CreateTexture +
                // GetTextureMemoryDesc + AllocateMemory + BindTextureMemory:
                // one dedicated allocation per pool slot, which is what
                // Phase 2's "no real aliasing yet" scope calls for. Slot
                // SHARING (two transients with disjoint lifetimes on one
                // resource) is what buys the memory back here, and it needs
                // no memory-object machinery at all.
                if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                                0.0f, want.texture, slot.texture))
                    || !slot.texture)
                {
                    slot = PoolResource{};
                    GraphError("RenderGraph::Execute: transient texture '"
                                + (want.name ? *want.name : std::string("?")) + "' could not be created");
                    return false;
                }
                slot.textureDesc = want.texture;
                if (want.name)
                    core.SetDebugName(slot.texture, want.name->c_str());
            }
            else
            {
                if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                               0.0f, want.buffer, slot.buffer))
                    || !slot.buffer)
                {
                    slot = PoolResource{};
                    GraphError("RenderGraph::Execute: transient buffer '"
                                + (want.name ? *want.name : std::string("?")) + "' could not be created");
                    return false;
                }
                slot.bufferDesc = want.buffer;
                if (want.name)
                    core.SetDebugName(slot.buffer, want.name->c_str());
            }
            ++m_transientCreateCount;
        }

        return true;
    }

    nri::Descriptor* RenderGraph::ViewForTexture(nri::Texture* texture, bool depth) const noexcept
    {
        if (!texture)
            return nullptr;
        // Both halves, pool first. A pool texture and an imported one cannot
        // share an address while both are alive, so which vector answers is
        // never ambiguous at runtime -- the split is about LIFETIME (see
        // m_importedViews), not about lookup.
        for (const CachedView& cached : m_views)
        {
            if (cached.texture == texture && cached.depth == depth)
                return cached.view;
        }
        for (const CachedView& cached : m_importedViews)
        {
            if (cached.texture == texture && cached.depth == depth)
                return cached.view;
        }
        return nullptr;
    }

    bool RenderGraph::RealizeAttachmentViews()
    {
        const nri::CoreInterface& core = m_device->Core();

        // LAST frame's imported views die here, before this frame's are made.
        // They cannot be carried: the graph does not own an imported texture
        // and is never told when its owner destroys one (NriSwapChain::Resize
        // destroys and recreates every backbuffer, entirely outside the
        // graph), and a pointer-keyed cache cannot detect a recreated texture
        // landing on a recycled address. See m_importedViews.
        ReleaseImportedViews();

        // Created up front, for exactly the textures the declarations attach
        // -- so RenderGraphNodeContext::ColorView is a lookup and never a
        // creation point, and so a view failure refuses the frame before
        // anything is recorded rather than half way through it.
        const auto ensure = [&](RgTexture attachment, bool depth) -> bool
        {
            const std::size_t slot = DecodeAndValidateSlot(attachment.index, m_textures.size());
            if (slot == kNoSlot)
                return true;   // Compile() already refused genuinely bad attachment handles
            nri::Texture* texture = slot < m_resolvedTextures.size() ? m_resolvedTextures[slot] : nullptr;
            if (!texture)
            {
                GraphError("RenderGraph::Execute: attachment '" + m_textures[slot].name
                            + "' resolved to no texture");
                return false;
            }
            if (ViewForTexture(texture, depth))
                return true;

            nri::TextureViewDesc viewDesc = {};
            viewDesc.texture   = texture;
            viewDesc.type      = depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT
                                        : nri::TextureView::COLOR_ATTACHMENT;
            // Task 3 (Phase 4): an attachment view always targets exactly
            // mip 0, explicitly -- zero-init's default (mipOffset = 0,
            // mipNum = 0 = REMAINING) used to coincide with "the only mip"
            // because every realized texture had exactly one; now that a
            // pool texture can carry a chain, REMAINING would ask for every
            // mip, which neither COLOR_ATTACHMENT nor DEPTH_STENCIL_ATTACHMENT
            // views can be.
            viewDesc.mipOffset = 0;
            viewDesc.mipNum    = 1;
            // A transient's format is the one it was realized with; an
            // imported texture's is whatever it actually is, read back from
            // NRI rather than assumed (an imported backbuffer's channel
            // order is resolved by NRI, not by us -- NriSwapChain::Format()).
            const std::uint32_t poolSlot = slot < m_texturePoolSlot.size() ? m_texturePoolSlot[slot]
                                                                            : kRgNoPoolSlot;
            viewDesc.format = (poolSlot != kRgNoPoolSlot && poolSlot < m_pool.size())
                            ? m_pool[poolSlot].textureDesc.format
                            : core.GetTextureDesc(*texture).format;

            nri::Descriptor* view = nullptr;
            if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, view)) || !view)
            {
                GraphError("RenderGraph::Execute: attachment view for '" + m_textures[slot].name
                            + "' could not be created");
                return false;
            }
            // Which vector decides the view's LIFETIME, so it follows the
            // texture's ownership, not the call site: a pool texture's view is
            // cached until the graph itself retires that texture, an imported
            // texture's lives exactly this frame.
            (m_textures[slot].kind == ResourceKind::Imported ? m_importedViews : m_views)
                .push_back(CachedView{ texture, depth, view });
            return true;
        };

        for (const NodeDesc& node : m_nodes)
        {
            if (node.kind != NodeKind::Raster)
                continue;
            for (const RgTexture& color : node.colorAttachments)
            {
                if (!ensure(color, /*depth=*/false))
                    return false;
            }
            if (node.depthAttachment.index != kInvalid)
            {
                if (!ensure(node.depthAttachment, /*depth=*/true))
                    return false;
            }
        }
        return true;
    }

    nri::Texture* RenderGraph::TextureForSlot(std::size_t slot) const noexcept
    {
        return slot < m_resolvedTextures.size() ? m_resolvedTextures[slot] : nullptr;
    }

    nri::Buffer* RenderGraph::BufferForSlot(std::size_t slot) const noexcept
    {
        return slot < m_resolvedBuffers.size() ? m_resolvedBuffers[slot] : nullptr;
    }

    std::uint32_t RenderGraph::PoolSlotForBarrier(const RgBarrier& barrier) const noexcept
    {
        if (barrier.isTexture)
        {
            return barrier.resourceIndex < m_texturePoolSlot.size()
                 ? m_texturePoolSlot[barrier.resourceIndex] : kRgNoPoolSlot;
        }
        return barrier.resourceIndex < m_bufferPoolSlot.size()
             ? m_bufferPoolSlot[barrier.resourceIndex] : kRgNoPoolSlot;
    }

    std::optional<nri::AccessLayoutStage> RenderGraph::DebugFirstBarrierBefore(std::uint32_t slot) const
    {
        if (slot >= m_poolCarrySeeded.size() || m_poolCarrySeeded[slot] == 0
            || slot >= m_poolFirstBefore.size())
        {
            return std::nullopt;
        }
        return m_poolFirstBefore[slot];
    }

    std::optional<std::uint32_t> RenderGraph::DebugPoolTextureMipCount(std::uint32_t poolSlot) const
    {
        if (poolSlot >= m_pool.size() || !m_pool[poolSlot].isTexture || !m_pool[poolSlot].texture)
            return std::nullopt;
        return m_pool[poolSlot].textureDesc.mipNum;
    }

    // ==================================================================
    // Barriers -- the ONE CmdBarrier call site (see the file header).
    // ==================================================================

    void RenderGraph::EmitBarriers(nri::CommandBuffer& cmd, std::span<const RgBarrier> barriers)
    {
        if (barriers.empty())
            return;   // never issue an empty group

        m_scratchTextureBarriers.clear();
        m_scratchBufferBarriers.clear();

        for (const RgBarrier& barrier : barriers)
        {
            // CROSS-FRAME POOL HANDOVER -- the executor's one amendment to a
            // verbatim replay (RgCompiled's contract block says so, and why).
            // The pool survives Reset(), so this physical resource may still
            // be carrying the previous frame's writes; Compile(), being pure
            // and per-frame, always hands the slot's first use a
            // `before = {NONE, UNDEFINED, ALL}`, which performs no source
            // availability operation at all. Patch the WHOLE triple -- access,
            // stages AND LAYOUT -- from what the last frame actually left
            // here, byte-for-byte the amendment Compile() makes for a
            // within-frame tenant handover.
            //
            // The layout used to be left at UNDEFINED here too, and that was
            // the D2 dx12 blocker: NRI's D3D12 backend turns {non-NONE access,
            // UNDEFINED layout} into a D3D12_TEXTURE_BARRIER with
            // LayoutBefore = UNDEFINED, AccessBefore != NO_ACCESS and
            // D3D12_TEXTURE_BARRIER_FLAG_DISCARD (CommandBufferD3D12.hpp
            // :1054-1056, :1078-1079), which enhanced-barrier validation
            // rejects -- invalidating the list, so EndCommandBuffer ->
            // Close() fails. RenderGraph.cpp's POOL HANDOVER block carries the
            // full argument, including why zeroing the access instead would
            // reopen the write-after-write hazard this patch exists to close.
            //
            // No force-emit is needed on this side: Compile() derived this
            // barrier against kUnknownState, so it is already IN the list --
            // this only reshapes its `before`. The "a handover always
            // barriers" rule is upheld structurally here.
            const std::uint32_t poolSlot = PoolSlotForBarrier(barrier);
            nri::AccessLayoutStage before = barrier.before;
            if (poolSlot != kRgNoPoolSlot && poolSlot < m_pool.size()
                && poolSlot < m_poolCarrySeeded.size() && m_poolCarrySeeded[poolSlot] == 0)
            {
                m_poolCarrySeeded[poolSlot] = 1;
                // Guarded on the state Compile() actually emits for a first
                // use rather than applied blind: anything else means this is
                // NOT the slot's first-use barrier and must not be touched.
                if (m_pool[poolSlot].hasCarry
                    && before.access == nri::AccessBits::NONE
                    && before.layout == nri::Layout::UNDEFINED)
                {
                    before = m_pool[poolSlot].carry;
                }
                if (poolSlot < m_poolFirstBefore.size())
                    m_poolFirstBefore[poolSlot] = before;
            }

            if (barrier.isTexture)
            {
                nri::Texture* texture = TextureForSlot(barrier.resourceIndex);
                // Execute() bounds-checks and null-checks every barrier
                // before recording starts, so reaching this is a bug in that
                // check rather than bad input.
                ARC_ASSERT(texture != nullptr, "RenderGraph::EmitBarriers: unresolved texture barrier");
                if (!texture)
                    continue;

                nri::TextureBarrierDesc desc = {};
                desc.texture = texture;
                desc.before  = before;
                desc.after   = barrier.after;
                // mipNum/layerNum left at REMAINING (0): the graph transitions
                // whole resources, and REMAINING stays correct for an imported
                // texture that happens to carry more than one mip or layer.
                m_scratchTextureBarriers.push_back(desc);
            }
            else
            {
                nri::Buffer* buffer = BufferForSlot(barrier.resourceIndex);
                ARC_ASSERT(buffer != nullptr, "RenderGraph::EmitBarriers: unresolved buffer barrier");
                if (!buffer)
                    continue;

                // nri::BufferBarrierDesc's before/after are AccessStage:
                // access + stages, no layout. RgBarrier's layout is always
                // UNDEFINED for a buffer and is DROPPED here, exactly as
                // RgBarrier's contract says.
                nri::BufferBarrierDesc desc = {};
                desc.buffer = buffer;
                desc.before = { before.access, before.stages };
                desc.after  = { barrier.after.access, barrier.after.stages };
                m_scratchBufferBarriers.push_back(desc);
            }

            // The slot now holds whatever this barrier moved it to. The LAST
            // barrier of the frame therefore leaves the carry state the next
            // frame patches from -- and that is exact, because Compile()
            // emits a barrier for every state change and the physical
            // resource cannot change state any other way.
            if (poolSlot != kRgNoPoolSlot && poolSlot < m_pool.size())
            {
                m_pool[poolSlot].carry    = barrier.after;
                m_pool[poolSlot].hasCarry = true;
            }
        }

        nri::BarrierDesc group = {};
        group.buffers    = m_scratchBufferBarriers.empty() ? nullptr : m_scratchBufferBarriers.data();
        group.bufferNum  = static_cast<std::uint32_t>(m_scratchBufferBarriers.size());
        group.textures   = m_scratchTextureBarriers.empty() ? nullptr : m_scratchTextureBarriers.data();
        group.textureNum = static_cast<std::uint32_t>(m_scratchTextureBarriers.size());
        if (group.bufferNum == 0 && group.textureNum == 0)
            return;

        m_device->Core().CmdBarrier(cmd, group);
    }

    // ==================================================================
    // Execute
    // ==================================================================

    bool RenderGraph::Execute(const RgExecuteDesc& desc, const RgCompiled& compiled)
    {
        if (m_device == nullptr)
        {
            // The device and the LANE latch together, atomically, before
            // anything fallible -- which is what puts every burial a FAILING
            // Execute makes (EnsureExecutionResources's all-or-nothing cleanup
            // below) in this context's own lane rather than in a shared one at
            // fence 0. See RgExecuteDesc::graves.
            m_device = &desc.device;
            m_graves = &desc.graves;
        }
        else if (m_device != &desc.device)
        {
            // Every resource this graph holds belongs to the first device;
            // adopting a second one would bury this device's textures through
            // that device's function table.
            GraphError("RenderGraph::Execute: a second NriDevice was passed to a graph that already "
                       "realized resources on another -- one graph belongs to one device");
            return false;
        }
        else if (m_graves != &desc.graves)
        {
            // Same argument one level down: every burial already pending for
            // this graph sits in the FIRST lane, keyed to this graph's fence
            // timeline. Splitting the rest across a second lane would leave two
            // graveyards holding halves of one ordered sequence -- and the
            // ordering IS the contract (a view must be destroyed before the
            // texture it views), which nothing can enforce across two of them.
            GraphError("RenderGraph::Execute: a second Graveyard lane was passed to a graph that "
                       "already buried into another -- one graph belongs to one lane");
            return false;
        }

        const nri::CoreInterface& core = m_device->Core();

        // ------------------------------------------------------------
        // The compiled graph must describe THESE declarations. The cheap
        // caller bug is Reset() -> re-declare -> Execute() with last frame's
        // RgCompiled, which would index this frame's resources with last
        // frame's slots; RgCompiled carries no generation stamp, so this is
        // structural.
        // ------------------------------------------------------------
        if (compiled.nodes.size() != m_nodes.size())
        {
            GraphError("RenderGraph::Execute: the compiled graph has " + std::to_string(compiled.nodes.size())
                        + " node(s) but this graph declares " + std::to_string(m_nodes.size())
                        + " -- Compile() again after re-declaring");
            return false;
        }

        if (!EnsureExecutionResources())
            return false;

        if (desc.frameSlot >= m_frames.size())
        {
            GraphError("RenderGraph::Execute: frameSlot " + std::to_string(desc.frameSlot)
                        + " is out of range (FramesInFlight() = "
                        + std::to_string(m_frames.size()) + ")");
            return false;
        }

        // Whatever this graph's own timeline has retired is safe to free. On
        // the NONE backend GetFenceValue is hard-wired to 0, so this reaps
        // only value-0 burials -- device-less tests feed the graveyard their own
        // values by hand.
        //
        // OWN FENCE, OWN LANE. This is the pairing the whole lane exists for:
        // reaping THIS graph's completed value against a graveyard a SECOND
        // graph also buries into would run that graph's thunks while its
        // submission was still in flight. m_fence is private to this graph and
        // m_graves is private to its context, so the two can only ever describe
        // each other.
        m_graves->Reap(core.GetFenceValue(*m_fence));

        // Whatever a PREVIOUS Execute() retired out of the pool, buried now
        // (see m_retiredPool). It has to be here,
        // at the top of a LATER Execute: by this point every view any node
        // cached over those textures has already been buried -- by the exec
        // fns of the frame that retired them, and, for a node that skipped
        // that frame entirely, by the declaration-time SyncPoolEpoch sweep
        // NriGraphContext runs before calling in. A graveyard replays in
        // burial order, so "later" is exactly what makes the texture die after
        // its views.
        FlushRetiredPool();

        // Transient slot mapping for this frame, built before anything reads
        // it (RealizePool's attachment-role pass does).
        m_texturePoolSlot.assign(m_textures.size(), kRgNoPoolSlot);
        m_bufferPoolSlot.assign(m_buffers.size(), kRgNoPoolSlot);
        for (std::size_t t = 0; t < compiled.transients.size() && t < compiled.transientPoolSlot.size(); ++t)
        {
            const RgTransient& transient = compiled.transients[t];
            if (transient.isTexture && transient.resourceIndex < m_texturePoolSlot.size())
                m_texturePoolSlot[transient.resourceIndex] = compiled.transientPoolSlot[t];
            else if (!transient.isTexture && transient.resourceIndex < m_bufferPoolSlot.size())
                m_bufferPoolSlot[transient.resourceIndex] = compiled.transientPoolSlot[t];
        }

        if (!RealizePool(compiled))
            return false;

        // Cross-frame handover bookkeeping for THIS frame: no slot has had
        // its first barrier emitted yet. PoolResource::carry itself is NOT
        // cleared -- it is precisely what survives from the last Execute().
        m_poolCarrySeeded.assign(m_pool.size(), 0);
        m_poolFirstBefore.assign(m_pool.size(), nri::AccessLayoutStage{});

        // ------------------------------------------------------------
        // Acquire -- as LATE as possible. Everything fallible above this
        // line fails without an outstanding acquire; a failure BELOW it
        // leaves one for ~NriSwapChain's QueueWaitIdle to clean up, which is
        // untidy but strictly better than presenting a frame whose release
        // fence nothing signalled (that parks the present engine forever).
        // ------------------------------------------------------------
        bool wantsSwapChain = false;
        for (const TextureResource& resource : m_textures)
            wantsSwapChain = wantsSwapChain || resource.swapChain;

        nri::Texture* backbuffer = nullptr;
        if (wantsSwapChain)
        {
            if (!desc.swapChain)
            {
                GraphError("RenderGraph::Execute: a node imported the swapchain backbuffer but "
                           "RgExecuteDesc::swapChain is null");
                return false;
            }
            backbuffer = desc.swapChain->AcquireNextTexture();
            if (!backbuffer)
            {
                // Routine, already logged at the right severity by
                // NriSwapChain (a zero-sized surface, or an OUT_OF_DATE
                // awaiting the frame driver's Resize()). Deliberately NOT a
                // GraphError: skipping a frame is not a latch-worthy event.
                return false;
            }
        }

        // ------------------------------------------------------------
        // Resolution table for this frame.
        // ------------------------------------------------------------
        m_resolvedTextures.assign(m_textures.size(), nullptr);
        m_resolvedBuffers.assign(m_buffers.size(), nullptr);
        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            const TextureResource& resource = m_textures[i];
            if (resource.swapChain)
                m_resolvedTextures[i] = backbuffer;
            else if (resource.kind == ResourceKind::Imported)
                m_resolvedTextures[i] = resource.imported;
            else if (m_texturePoolSlot[i] != kRgNoPoolSlot && m_texturePoolSlot[i] < m_pool.size())
                m_resolvedTextures[i] = m_pool[m_texturePoolSlot[i]].texture;
        }
        for (std::size_t i = 0; i < m_buffers.size(); ++i)
        {
            const BufferResource& resource = m_buffers[i];
            if (resource.kind == ResourceKind::Imported)
                m_resolvedBuffers[i] = resource.imported;
            else if (m_bufferPoolSlot[i] != kRgNoPoolSlot && m_bufferPoolSlot[i] < m_pool.size())
                m_resolvedBuffers[i] = m_pool[m_bufferPoolSlot[i]].buffer;
        }

        if (!RealizeAttachmentViews())
            return false;

        // Every barrier's resource must exist BEFORE recording opens: a
        // barrier that named an out-of-range slot, or a resource nothing
        // realized, would otherwise be silently skipped mid-record and leave
        // the resource in the wrong state for the node that follows it.
        const auto barriersResolve = [&](std::span<const RgBarrier> barriers, const char* which) -> bool
        {
            for (const RgBarrier& barrier : barriers)
            {
                const bool ok = barrier.isTexture
                              ? (barrier.resourceIndex < m_resolvedTextures.size()
                                 && m_resolvedTextures[barrier.resourceIndex] != nullptr)
                              : (barrier.resourceIndex < m_resolvedBuffers.size()
                                 && m_resolvedBuffers[barrier.resourceIndex] != nullptr);
                if (!ok)
                {
                    GraphError(std::string("RenderGraph::Execute: a ") + which + " barrier names a "
                                + (barrier.isTexture ? "texture" : "buffer")
                                + " this frame did not realize -- the compiled graph does not describe "
                                  "these declarations");
                    return false;
                }
            }
            return true;
        };
        for (const RgCompiledNode& node : compiled.nodes)
        {
            if (!barriersResolve(node.preBarriers, "pre"))
                return false;
        }
        if (!barriersResolve(compiled.exitBarriers, "exit"))
            return false;

        // ------------------------------------------------------------
        // Record. ONE command buffer for every node (the frozen shape).
        // ------------------------------------------------------------
        // Named `frame`, not `slot`: the lambdas below take a texture SLOT,
        // and two different slot spaces sharing one name in one scope is
        // exactly the kind of confusion RgCompiled's INDEX SPACE comment
        // exists to prevent.
        GpuFrameSlot& frame = m_frames[desc.frameSlot];
        core.ResetCommandAllocator(*frame.allocator);

        nri::CommandBuffer& cmd = *frame.cmd;
        if (!ARC_NRI_CHECK(core.BeginCommandBuffer(cmd, nullptr)))
            return false;

        RenderGraphNodeContext context{ cmd, core, desc.ring, desc.pipelines, this };

        // THIS graph's native device -- the identity every node's GPU marker is
        // checked against (NodeScope's cross-device rule). Resolved once here
        // rather than per node: it cannot change inside one Execute, and the
        // NONE backend answers null, which closes the gate exactly as a
        // mismatch would.
        void* const nativeDevice = core.GetDeviceNativeObject(&m_device->Device());

        // The cached view for an attachment handle -- RealizeAttachmentViews
        // above already created every one of them, so a null here means the
        // handle did not decode (which Compile() has already refused).
        const auto attachmentView = [&](RgTexture attachment, bool depth) -> nri::Descriptor*
        {
            const std::size_t slot = DecodeAndValidateSlot(attachment.index, m_textures.size());
            if (slot == kNoSlot)
                return nullptr;
            return ViewForTexture(TextureForSlot(slot), depth);
        };

        // The pass's render area: the first colour attachment's extent, or
        // the depth attachment's for a depth-only pass. A transient's is the
        // desc it was realized with; an imported texture's is read back from
        // NRI rather than assumed.
        const auto attachmentExtent = [&](const NodeDesc& node, std::uint32_t& width,
                                          std::uint32_t& height) -> bool
        {
            const RgTexture reference = node.colorAttachments.empty() ? node.depthAttachment
                                                                      : node.colorAttachments.front();
            const std::size_t slot = DecodeAndValidateSlot(reference.index, m_textures.size());
            if (slot == kNoSlot)
                return false;

            const std::uint32_t poolSlot = m_texturePoolSlot[slot];
            if (poolSlot != kRgNoPoolSlot && poolSlot < m_pool.size() && m_pool[poolSlot].texture)
            {
                width  = m_pool[poolSlot].textureDesc.width;
                height = m_pool[poolSlot].textureDesc.height;
                return true;
            }

            nri::Texture* texture = TextureForSlot(slot);
            if (!texture)
                return false;
            const nri::TextureDesc& textureDesc = core.GetTextureDesc(*texture);
            width  = textureDesc.width;
            height = textureDesc.height;
            return true;
        };

        for (std::size_t i = 0; i < compiled.nodes.size(); ++i)
        {
            const RgCompiledNode& compiledNode = compiled.nodes[i];
            const NodeDesc&       node         = m_nodes[i];

            // Opened BEFORE the node's barriers so a capture attributes the
            // transitions to the pass that needed them.
            NodeScope scope(core, cmd, node.passLabel, nativeDevice);

            EmitBarriers(cmd, compiledNode.preBarriers);

            bool rendering = false;
            if (node.kind == NodeKind::Raster)
            {
                // Load ops are declarative (RenderGraph::SetColorAttachmentOps /
                // SetDepthAttachmentOp): an attachment with no declared op, or a
                // colour-op span shorter than the attachment span, LOADs. The
                // zero-initialized storeOp is STORE.
                m_scratchColorAttachments.clear();
                std::size_t colorIndex = 0;
                for (const RgTexture& color : node.colorAttachments)
                {
                    nri::AttachmentDesc attachment = {};
                    attachment.descriptor = attachmentView(color, /*depth=*/false);
                    if (colorIndex < node.colorOps.size() && node.colorOps[colorIndex].load == RgLoadOp::Clear)
                    {
                        attachment.loadOp     = nri::LoadOp::CLEAR;
                        attachment.clearValue = node.colorOps[colorIndex].clear;
                    }
                    m_scratchColorAttachments.push_back(attachment);
                    ++colorIndex;
                }

                nri::RenderingDesc renderingDesc = {};
                renderingDesc.colors   = m_scratchColorAttachments.empty() ? nullptr
                                                                            : m_scratchColorAttachments.data();
                renderingDesc.colorNum = static_cast<std::uint32_t>(m_scratchColorAttachments.size());
                if (node.depthAttachment.index != kInvalid)
                    renderingDesc.depth.descriptor = attachmentView(node.depthAttachment, /*depth=*/true);
                if (node.hasDepthOp && node.depthOp.load == RgLoadOp::Clear)
                {
                    renderingDesc.depth.loadOp     = nri::LoadOp::CLEAR;
                    renderingDesc.depth.clearValue = node.depthOp.clear;
                }

                core.CmdBeginRendering(cmd, renderingDesc);
                rendering = true;

                // "Initial state (mandatory)" per NRI.h -- a pipeline is not
                // allowed to inherit either of these, so the graph sets them
                // to the full attachment once per pass. A node that wants
                // something else overrides them from its exec fn.
                std::uint32_t width = 0, height = 0;
                if (attachmentExtent(node, width, height) && width != 0 && height != 0)
                {
                    const nri::Viewport viewport = { 0.0f, 0.0f, static_cast<float>(width),
                                                     static_cast<float>(height), 0.0f, 1.0f };
                    core.CmdSetViewports(cmd, &viewport, 1);
                    const nri::Rect scissor = { 0, 0, static_cast<nri::Dim_t>(width),
                                                static_cast<nri::Dim_t>(height) };
                    core.CmdSetScissors(cmd, &scissor, 1);
                }
            }

            if (node.exec)
                node.exec(context);

            if (rendering)
                core.CmdEndRendering(cmd);
        }

        EmitBarriers(cmd, compiled.exitBarriers);

        if (!ARC_NRI_CHECK(core.EndCommandBuffer(cmd)))
            return false;

        // ------------------------------------------------------------
        // Submit. The graph's own fence is ALWAYS signalled (it is the
        // graveyard's clock and a headless graph's only completion signal);
        // the swapchain's acquire/release fences join it only for a frame
        // that acquired. NriSwapChain owns both but submits no real work
        // itself, so wiring them into a submission is the caller's job --
        // and the caller is now the graph.
        // ------------------------------------------------------------
        nri::FenceSubmitDesc waitFence = {};
        waitFence.fence  = backbuffer ? desc.swapChain->CurrentAcquireFence() : nullptr;
        // StageBits::ALL (NRI's lazy default, 0) rather than a narrower
        // single stage: a graph does not know which stage first touches
        // the backbuffer -- the first node might copy into it rather than
        // render to it -- and a too-narrow wait stage is a race, not a
        // slowdown.
        waitFence.stages = nri::StageBits::ALL;

        nri::FenceSubmitDesc signalFences[2] = {};
        signalFences[0].fence = m_fence;
        signalFences[0].value = m_submitValue + 1;
        std::uint32_t signalNum = 1;
        if (backbuffer)
        {
            // A SWAPCHAIN_SEMAPHORE fence carries no value, so it is left at 0.
            signalFences[1].fence = desc.swapChain->CurrentReleaseFence();
            signalNum = 2;
        }

        nri::CommandBuffer* commandBuffers[] = { &cmd };

        nri::QueueSubmitDesc submit = {};
        submit.waitFences       = waitFence.fence ? &waitFence : nullptr;
        submit.waitFenceNum     = waitFence.fence ? 1u : 0u;
        submit.commandBuffers   = commandBuffers;
        submit.commandBufferNum = 1;
        submit.signalFences     = signalFences;
        submit.signalFenceNum   = signalNum;

        if (!ARC_NRI_CHECK(core.QueueSubmit(*m_device->GraphicsQueue(), submit)))
        {
            // No Present: a failed submit signalled no release fence, so
            // presenting would park the present engine on a fence nothing
            // will ever signal. ARC_NRI_CHECK has already routed a typed
            // DEVICE_LOST into the device-removed chain.
            return false;
        }
        ++m_submitValue;

        if (backbuffer)
            desc.swapChain->Present();

        return true;
    }
}
