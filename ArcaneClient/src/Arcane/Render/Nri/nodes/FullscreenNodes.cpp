// FullscreenNodes -- see the header for the file's scope (the post chain and
// the tonemap), THE PING-PONG IS DERIVED, and the two lifetime rules (THE
// SOURCE VIEW for the tonemap, SOURCE VIEWS AND THE POOL for the chain).
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp).
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>
#include <Extensions/NRIHelper.h>

#include "FullscreenNodes.hpp"

#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>    // kSceneInput / kMaxPassInputs
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Render/RenderBudgetSettings.hpp>   // RenderPostSettings -- the caps the constructor latches
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/PostChainCache.hpp>      // PostChainDesc
#include <Arcane/Render/ShaderConventions.hpp>   // kVsEntry / kPsEntry

#undef ERROR

#include <algorithm>
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

        // FNV-1a over the stitched bytecode, for the same two jobs it does in
        // Batch2DNode: NriPipelineCache::GraphicsKey::shaderPairId (whose fill
        // contract, rule 3, requires the key to change whenever the bytecode
        // would -- a CONTENT hash satisfies that by construction) and the stamp
        // that decides whether the built chain is still current.
        ARC_CONSTANT("math: FNV-1a hash parameters")
        constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
        ARC_CONSTANT("math: FNV-1a hash parameters")
        constexpr std::uint64_t kFnvPrime  = 1099511628211ull;

        std::uint64_t HashBytes(const void* data, std::size_t size,
                                std::uint64_t seed = kFnvOffset) noexcept
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

    // =====================================================================
    // FullscreenMaterialLayout -- see the header for the whole of its shape
    // and how it differs from the sprite twin.
    // =====================================================================

    void FullscreenMaterialLayout::Build(std::uint32_t cbSize, std::uint32_t textureCount,
                                         std::uint32_t chainInputs)
    {
        // Both stages: a merged template's %{VERTEX_BODY} may read its
        // params and sample its
        // textures, and on D3D12 visibility is a hard root-signature property,
        // so narrowing to FRAGMENT would break a displacing post material.
        constexpr nri::StageBits kBothStages =
            nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        for (nri::DescriptorRangeDesc& range : ranges)
            range = {};
        materialCb = globalsCb = textures = sampler = kNoRange;

        std::uint32_t rangeCount = 0;
        // The stitcher omits the cbuffer entirely when no pass declares a
        // numeric param, so a b0 range would name a register the shader does
        // not have.
        if (cbSize > 0)
        {
            materialCb = rangeCount;
            ranges[rangeCount].baseRegisterIndex = kMaterialCbSlot;   // b0
            ranges[rangeCount].descriptorNum     = 1;
            ranges[rangeCount].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
            ranges[rangeCount].shaderStages      = kBothStages;
            ++rangeCount;
        }
        globalsCb = rangeCount;
        ranges[rangeCount].baseRegisterIndex = kGlobalCbSlot;         // b1
        ranges[rangeCount].descriptorNum     = 1;
        ranges[rangeCount].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        ranges[rangeCount].shaderStages      = kBothStages;
        ++rangeCount;

        // ONE contiguous SRV range: GenerateMaterialBindings emits the
        // declared textures at t0.. and the reserved InputTexture(N) slots
        // immediately after them (MaterialSource.cpp), so they are contiguous
        // registers of the same type -- which is also what lets NRI's D3D12
        // backend merge them into a single root table.
        if (textureCount + chainInputs > 0)
        {
            textures = rangeCount;
            ranges[rangeCount].baseRegisterIndex = 0;
            ranges[rangeCount].descriptorNum     = textureCount + chainInputs;
            ranges[rangeCount].descriptorType    = nri::DescriptorType::TEXTURE;
            ranges[rangeCount].shaderStages      = kBothStages;
            ++rangeCount;

            // The generator emits MaterialSampler only when there is something
            // to sample; declaring s0 here for a material that has neither
            // would name a register the shader does not have.
            sampler = rangeCount;
            ranges[rangeCount].baseRegisterIndex = 0;                 // s0
            ranges[rangeCount].descriptorNum     = 1;
            ranges[rangeCount].descriptorType    = nri::DescriptorType::SAMPLER;
            ranges[rangeCount].shaderStages      = kBothStages;
            ++rangeCount;
        }

        set = {};
        set.registerSpace = 0;
        set.ranges        = ranges;
        set.rangeNum      = rangeCount;

        // Value-initialized then assigned field by field: NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so its padding has to
        // be zeroed).
        //
        // rootConstantNum, rootDescriptorNum and rootSamplerNum all stay ZERO.
        // The first because fullscreen_material.hlsl declares no push
        // constants at all; the other two because THE REGISTER-SPACE RULE
        // (Batch2DNode.hpp) refuses rootRegisterSpace == a set's registerSpace
        // whenever either is nonzero -- and every register in the fullscreen
        // template is in the implicit space0, so both must be 0 here.
        desc = {};
        desc.rootRegisterSpace = 0;
        desc.descriptorSets    = &set;
        desc.descriptorSetNum  = 1;
        desc.shaderStages      = kBothStages;
    }

    // =====================================================================
    // PostChainNode (Task 10)
    // =====================================================================

    // The chain's reserved input count is a MaterialSource fact; this node's
    // per-pass binding table is sized from its own constant, so the two must
    // not drift.
    static_assert(PostChainNode::kMaxInputs == kMaxPassInputs,
                  "PostChainNode::kMaxInputs must match MaterialSource's kMaxPassInputs");

    PostChainNode::Caps PostChainNode::CapsFrom(const RenderPostSettings& settings) noexcept
    {
        Caps caps;
        caps.maxPasses       = settings.maxPasses;
        caps.maxTextures     = settings.maxTextures;
        caps.materialCbBytes = MaterialCbRegionBytes(settings.materialCbBytes);
        return caps;
    }

    // THE LATCH (Restart) -- Batch2DNode's shape: the pool, the arena and the
    // record scratch are sized from m_caps, so a render.post.* value published
    // after this point waits for the next node.
    PostChainNode::PostChainNode()
    {
        const RenderPostSettings& settings = Settings<RenderPostSettings>();
        m_caps = CapsFrom(settings);
        if (m_caps.materialCbBytes != settings.materialCbBytes)
            ARC_WARN("[nri-graph] PostChainNode: render.post.materialCbBytes {} is not a multiple of 256 -- "
                     "using {}", settings.materialCbBytes, m_caps.materialCbBytes);
        m_wantedScratch.assign(m_caps.maxTextures + kMaxInputs, nullptr);
        m_viewScratch.assign(m_caps.maxTextures + kMaxInputs, nullptr);
    }

    std::unique_ptr<PostChainNode> PostChainNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<PostChainNode> node(new PostChainNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool PostChainNode::Init(NriGraphContext& context)
    {
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();
        m_textures  = context.Textures();
        return CreateFallbackTexels() && CreateSampler() && CreatePool() && CreateConstantArena();
    }

    bool PostChainNode::CreateFallbackTexels()
    {
        const nri::CoreInterface& core = m_device->Core();

        // WHITE for an unbound/unresolvable declared texture param, BLACK
        // (transparent) for a chain input slot nothing is wired into -- the
        // two fallbacks FullscreenMaterialPass::Init creates, with the same
        // meanings, so a chain renders the same picture on both recorders.
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
            || !m_white
            || !ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                           0.0f, textureDesc, m_black))
            || !m_black)
        {
            ARC_ERROR("[nri-graph] PostChainNode: the 1x1 fallback texels could not be created");
            return false;
        }
        core.SetDebugName(m_white, "nri-graph post white");
        core.SetDebugName(m_black, "nri-graph post black");

        // Through NRI's OWN helper, exactly as Batch2DNode's white texel is:
        // graph code records no CmdBarrier of its own, and UploadData submits
        // and waits internally -- which is what "once, at Create()" wants.
        nri::HelperInterface helper = {};
        if (!ARC_NRI_CHECK(nriGetInterface(m_device->Device(), NRI_INTERFACE(nri::HelperInterface), &helper)))
        {
            ARC_ERROR("[nri-graph] PostChainNode: HelperInterface unavailable -- cannot upload the "
                      "fallback texels");
            return false;
        }

        const std::uint32_t whitePixel = 0xFFFFFFFFu;
        const std::uint32_t blackPixel = 0x00000000u;
        nri::TextureSubresourceUploadDesc subresources[2] = {};
        subresources[0].slices     = &whitePixel;
        subresources[0].sliceNum   = 1;
        subresources[0].rowPitch   = 4;
        subresources[0].slicePitch = 4;
        subresources[1].slices     = &blackPixel;
        subresources[1].sliceNum   = 1;
        subresources[1].rowPitch   = 4;
        subresources[1].slicePitch = 4;

        const nri::AccessLayoutStage shaderResource = { nri::AccessBits::SHADER_RESOURCE,
                                                        nri::Layout::SHADER_RESOURCE,
                                                        nri::StageBits::FRAGMENT_SHADER };
        nri::TextureUploadDesc uploads[2] = {};
        uploads[0].subresources = &subresources[0];
        uploads[0].texture      = m_white;
        uploads[0].after        = shaderResource;
        uploads[0].planes       = nri::PlaneBits::ALL;
        uploads[1].subresources = &subresources[1];
        uploads[1].texture      = m_black;
        uploads[1].after        = shaderResource;
        uploads[1].planes       = nri::PlaneBits::ALL;
        if (!ARC_NRI_CHECK(helper.UploadData(*m_device->GraphicsQueue(), uploads, 2, nullptr, 0)))
        {
            ARC_ERROR("[nri-graph] PostChainNode: the fallback texel upload failed");
            return false;
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        viewDesc.texture  = m_white;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, m_whiteView)) || !m_whiteView)
        {
            ARC_ERROR("[nri-graph] PostChainNode: the white texel's view could not be created");
            return false;
        }
        viewDesc.texture = m_black;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, m_blackView)) || !m_blackView)
        {
            ARC_ERROR("[nri-graph] PostChainNode: the black texel's view could not be created");
            return false;
        }
        return true;
    }

    bool PostChainNode::CreateSampler()
    {
        // LINEAR + WRAP, exactly FullscreenMaterialPass::Init's sampler
        // ("material textures tile and scale, unlike TonemapPass's point 1:1
        // blit sampler"). The chain's own inputs are 1:1 with the target, so
        // filtering is a no-op for them and the choice is about the declared
        // texture params.
        nri::SamplerDesc samplerDesc = {};
        samplerDesc.filters.min  = nri::Filter::LINEAR;
        samplerDesc.filters.mag  = nri::Filter::LINEAR;
        samplerDesc.filters.mip  = nri::Filter::LINEAR;
        samplerDesc.addressModes = { nri::AddressMode::REPEAT, nri::AddressMode::REPEAT,
                                     nri::AddressMode::REPEAT };
        samplerDesc.mipMax       = 16.0f;
        if (!ARC_NRI_CHECK(m_device->Core().CreateSampler(m_device->Device(), samplerDesc, m_sampler))
            || !m_sampler)
        {
            ARC_ERROR("[nri-graph] PostChainNode: sampler creation failed");
            return false;
        }
        return true;
    }

    bool PostChainNode::CreatePool()
    {
        // Sized from the caps, not discovered: a pool's capacity is fixed at
        // creation and NRI cannot free ONE descriptor set. Sets are per (pass,
        // frame slot) so the set a frame binds is the one whose arena region
        // that same frame owns -- which is what keeps ResetDescriptorPool (and
        // its fence discipline) out of this file entirely.
        const std::uint32_t sets = m_caps.maxPasses * FramesInFlight();
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum  = sets;
        poolDesc.constantBufferMaxNum = 2 * sets;                        // b0 + b1
        poolDesc.textureMaxNum        = (m_caps.maxTextures + kMaxInputs) * sets;
        poolDesc.samplerMaxNum        = sets;
        if (!ARC_NRI_CHECK(m_device->Core().CreateDescriptorPool(m_device->Device(), poolDesc, m_pool))
            || !m_pool)
        {
            ARC_ERROR("[nri-graph] PostChainNode: descriptor pool creation failed");
            return false;
        }
        return true;
    }

    bool PostChainNode::CreateConstantArena()
    {
        const nri::CoreInterface& core = m_device->Core();
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());

        // The device's own constant-buffer alignment, which also fixes the
        // SIZE every CB view is created with: NRI passes BufferViewDesc::size
        // straight into D3D12_CONSTANT_BUFFER_VIEW_DESC::SizeInBytes, which
        // D3D12 requires to be a multiple of 256 -- so the views name a whole
        // region and the shader simply reads less than it.
        m_arenaStride = CbRegionStride(m_caps.materialCbBytes, deviceDesc.memoryAlignment.constantBufferOffset);

        const std::uint64_t arenaBytes =
            (std::uint64_t)kCbRegionsPerFrame * FramesInFlight() * m_arenaStride;

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = arenaBytes;
        bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                       0.0f, bufferDesc, m_arena))
            || !m_arena)
        {
            ARC_ERROR("[nri-graph] PostChainNode: the post constant-buffer arena ({} bytes) could "
                      "not be created", arenaBytes);
            return false;
        }
        core.SetDebugName(m_arena, "nri-graph post chain CBs");

        // Persistent map, unmapped once in Release()/~PostChainNode -- the same
        // shape NriUploadRing and Batch2DNode's arena use, and the same NONE-
        // backend footgun: MapBuffer returns null unconditionally there, so
        // this node is [gpu]-only from here down.
        m_arenaCpu = core.MapBuffer(*m_arena, 0, nri::WHOLE_SIZE);
        if (!m_arenaCpu)
        {
            ARC_ERROR("[nri-graph] PostChainNode: the post constant-buffer arena could not be "
                      "mapped (the NONE backend cannot -- this node is a [gpu] path)");
            return false;
        }

        // Both CB views per frame slot, created ONCE. Their contents change
        // every frame; their (buffer, offset) never does, which is what lets a
        // descriptor set naming them be written once too.
        for (std::uint32_t slot = 0; slot < FramesInFlight(); ++slot)
        {
            nri::BufferViewDesc viewDesc = {};
            viewDesc.buffer = m_arena;
            viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
            viewDesc.size   = m_arenaStride;

            viewDesc.offset = ArenaOffset(slot, kGlobalsRegion);
            if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, m_globalsView[slot]))
                || !m_globalsView[slot])
            {
                ARC_ERROR("[nri-graph] PostChainNode: the globals constant-buffer view for frame "
                          "slot {} could not be created", slot);
                return false;
            }
            viewDesc.offset = ArenaOffset(slot, kMaterialRegion);
            if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, m_materialView[slot]))
                || !m_materialView[slot])
            {
                ARC_ERROR("[nri-graph] PostChainNode: the material constant-buffer view for frame "
                          "slot {} could not be created", slot);
                return false;
            }
        }
        return true;
    }

    PostChainNode::~PostChainNode()
    {
        if (!m_device || (!m_white && !m_black && !m_sampler && !m_pool && !m_arena))
            return;

        ARC_WARN("[nri-graph] PostChainNode destroyed with live NRI objects -- either Create() "
                 "failed part way (an ERROR above says which step) or its owner never called "
                 "Release(). Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        if (m_pool) core.DestroyDescriptorPool(m_pool);
        for (const FullscreenSourceView& cached : m_views)
            if (cached.view) core.DestroyDescriptor(cached.view);
        m_views.clear();
        for (nri::Descriptor*& view : m_globalsView)
            if (view) { core.DestroyDescriptor(view); view = nullptr; }
        for (nri::Descriptor*& view : m_materialView)
            if (view) { core.DestroyDescriptor(view); view = nullptr; }
        if (m_arena)
        {
            if (m_arenaCpu) core.UnmapBuffer(*m_arena);
            core.DestroyBuffer(m_arena);
        }
        if (m_sampler)   core.DestroyDescriptor(m_sampler);
        if (m_whiteView) core.DestroyDescriptor(m_whiteView);
        if (m_blackView) core.DestroyDescriptor(m_blackView);
        if (m_white)     core.DestroyTexture(m_white);
        if (m_black)     core.DestroyTexture(m_black);
        m_passes.clear();
        m_pool = nullptr; m_arena = nullptr; m_arenaCpu = nullptr;
        m_sampler = nullptr; m_whiteView = nullptr; m_blackView = nullptr;
        m_white = nullptr; m_black = nullptr;
        m_ready = false;
    }

    void PostChainNode::InvalidateSources(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();
        for (const FullscreenSourceView& cached : m_views)
            if (cached.view)
                graveyard.Bury(fence, [core, d = cached.view] { core->DestroyDescriptor(d); });
        m_views.clear();

        // The sets still NAME the buried views, so nothing may bind one until
        // Record rewrites the texture range -- which `written = false` is
        // exactly what forces.
        for (Pass& pass : m_passes)
        {
            for (bool& written : pass.written)
                written = false;
            for (auto& slot : pass.bound)
                for (nri::Texture*& texture : slot)
                    texture = nullptr;
        }
    }

    void PostChainNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        InvalidateSources(graveyard, fence);

        const nri::CoreInterface* core = &m_device->Core();

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view can never outlive its arena or texture.
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
        }
        m_passes.clear();   // the sets were owned by the pool, buried above
        m_ready    = false;
        m_stamp    = 0;
        m_layoutId = NriPipelineCache::kInvalidLayout;

        for (nri::Descriptor*& view : m_globalsView)
        {
            if (!view) continue;
            graveyard.Bury(fence, [core, d = view] { core->DestroyDescriptor(d); });
            view = nullptr;
        }
        for (nri::Descriptor*& view : m_materialView)
        {
            if (!view) continue;
            graveyard.Bury(fence, [core, d = view] { core->DestroyDescriptor(d); });
            view = nullptr;
        }
        if (m_arena)
        {
            // Unmapped HERE rather than inside the burial: the map is a CPU-side
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
        if (m_blackView)
        {
            graveyard.Bury(fence, [core, d = m_blackView] { core->DestroyDescriptor(d); });
            m_blackView = nullptr;
        }
        if (m_white)
        {
            graveyard.Bury(fence, [core, t = m_white] { core->DestroyTexture(t); });
            m_white = nullptr;
        }
        if (m_black)
        {
            graveyard.Bury(fence, [core, t = m_black] { core->DestroyTexture(t); });
            m_black = nullptr;
        }
    }

    void PostChainNode::SyncPoolEpoch(const RenderGraphNodeContext& context)
    {
        // Mechanism 1 (see the header). Identical to TonemapNode::
        // SyncPoolEpoch, and identical for the same reason: RealizePool may
        // have buried a pool texture THIS Execute(), between the declarations
        // and this exec fn, and the epoch is the only observable.
        if (!context.graph)
            return;
        SyncPoolEpoch(*context.graph);
    }

    void PostChainNode::SyncPoolEpoch(const RenderGraph& graph)
    {
        if (!m_device)
            return;
        // THE GRAPH'S LANE, not the device's: an exec fn runs inside that
        // graph's Execute, so its lane is where the views it is about to bury
        // belong -- and it is the same lane the pool retirement that moved the
        // epoch stages into, which is what keeps the two in one ordered
        // sequence. The owner's declaration-time call names the same graph and
        // therefore the same lane, one step EARLIER in that sequence.
        Graveyard* graves = graph.Graves();
        if (!graves)
            return;   // from an exec fn: unreachable (Execute latches the lane before recording).
                      // From the owner at declaration time: the graph has never Executed, so it
                      // owns no pool and this node can hold no view over one.
        const std::uint64_t epoch = graph.PoolEpoch();
        if (epoch == m_poolEpoch)
            return;
        m_poolEpoch = epoch;
        InvalidateSources(*graves, graph.DebugSubmitCount());
    }

    nri::Descriptor* PostChainNode::EnsureView(const nri::CoreInterface& core, nri::Texture* texture)
    {
        if (!texture)
            return nullptr;
        for (const FullscreenSourceView& cached : m_views)
            if (cached.texture == texture)
                return cached.view;

        // A CHAIN sees at most (canvas + one target per pass) distinct
        // textures, and the pool collapses those onto two physical ones -- so
        // a cache that keeps growing past that means pool textures are being
        // replaced without RenderGraph::PoolEpoch moving, which would be a bug
        // in the graph rather than here. Say so, once, rather than letting it
        // grow silently.
        if (m_views.size() >= (std::size_t)m_caps.maxPasses + 2 && !m_warnedViewChurn)
        {
            m_warnedViewChurn = true;
            GraphError("PostChainNode: more distinct source textures than a chain can have -- pool "
                       "textures are being replaced without RenderGraph::PoolEpoch moving");
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        // The transient's ACTUAL format, read back from NRI -- never assumed.
        viewDesc.format   = core.GetTextureDesc(*texture).format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        nri::Descriptor* view = nullptr;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, view)) || !view)
        {
            GraphError("PostChainNode: a source shader-resource view could not be created");
            return nullptr;
        }
        m_views.push_back(FullscreenSourceView{ texture, view });
        return view;
    }

    bool PostChainNode::BuildChain(const PostChainDesc& desc, nri::Format targetFormat)
    {
        const nri::CoreInterface& core = m_device->Core();
        const std::uint32_t passCount    = (std::uint32_t)desc.passes.size();
        const std::uint32_t cbSize       = desc.templ->CbSize();
        const std::uint32_t textureCount = desc.templ->TextureCount();
        const std::uint32_t chainInputs  = desc.chainInputSlots;

        const auto refuse = [&](const std::string& why)
        {
            ARC_ERROR("[nri-graph] PostChainNode: the scene post chain '{}' cannot be recorded -- "
                      "{}. The frame renders canvas -> tonemap instead.",
                      desc.templ->Name(), why);
            return false;
        };
        if (passCount > m_caps.maxPasses)
            return refuse("it has " + std::to_string(passCount) + " passes, over this node's cap of "
                          + std::to_string(m_caps.maxPasses) + " -- raise render.post.maxPasses (it "
                          "sizes the descriptor pool)");
        if (textureCount > m_caps.maxTextures)
            return refuse("it declares " + std::to_string(textureCount) + " texture params, over "
                          "this node's cap of " + std::to_string(m_caps.maxTextures)
                          + " -- raise render.post.maxTextures");
        if (chainInputs > kMaxInputs)
            return refuse("it reserves " + std::to_string(chainInputs) + " input slots, over "
                          "MaterialSource's own kMaxPassInputs");
        if (cbSize > m_caps.materialCbBytes)
            return refuse("its merged constant buffer is " + std::to_string(cbSize)
                          + " bytes, over this node's arena region of "
                          + std::to_string(m_caps.materialCbBytes) + " -- raise render.post.materialCbBytes");

        // THE LAYOUT -- see FullscreenMaterialLayout for the whole of its shape
        // and its reasoning. A separate object so the device-less [nri] cases can
        // assert that shape without a device.
        FullscreenMaterialLayout layout;
        layout.Build(cbSize, textureCount, chainInputs);

        const std::uint32_t previousLayout = m_layoutId;
        m_layoutId = m_pipelines->RegisterLayout(layout.desc);
        nri::PipelineLayout* pipelineLayout = m_pipelines->Layout(m_layoutId);
        if (!pipelineLayout)
            return refuse("its pipeline layout could not be registered");

        // A rebuild whose binding SHAPE changed (a re-saved snippet that gained
        // or lost a param, a texture decl or an input wire). A descriptor set is
        // allocated against one layout's set-0 shape, so the existing sets no
        // longer fit and must be replaced -- and NRI cannot free the old ones,
        // so they are abandoned inside the pool until Release(). Rare (a
        // structural edit at the desk) and self-limiting: a run that does it
        // often enough exhausts the pool and gets the loud allocation failure
        // below, which names the constant.
        const bool shapeChanged = previousLayout != NriPipelineCache::kInvalidLayout
                               && previousLayout != m_layoutId;
        if (shapeChanged)
        {
            ARC_INFO("[nri-graph] PostChainNode: the post chain changed binding SHAPE -- its "
                     "descriptor sets are re-allocated (the previous ones are stranded in the pool "
                     "until shutdown)");
        }
        if (shapeChanged)
            m_passes.assign(passCount, Pass{});   // the old sets no longer fit the layout
        else
            // Same layout: every surviving pass KEEPS its descriptor sets, which
            // is the common re-save (edited shader body, edited values). Only a
            // pass-count change costs sets -- a shrink strands the tail's in the
            // pool, a growth allocates fresh ones.
            m_passes.resize(passCount);

        m_cbSize       = cbSize;
        m_textureCount = textureCount;
        m_chainInputs  = chainInputs;
        m_textureRange = layout.textures;
        m_targetFormat = targetFormat;
        m_packed.assign(cbSize, 0);

        // A POST MATERIAL'S DECLARED TEXTURE PARAMS are resolved HERE -- at
        // declaration time, because the upload submits and waits -- through
        // the vehicle's SHARED NriTextureCache, the same one Batch2DNode
        // uses. An unbound, unresolvable or undecodable param falls back to
        // the white texel (the cache says why, once). Chain INPUTS are
        // unaffected: those are graph transients, always bound for real.
        m_paramTextures.assign(textureCount, nullptr);
        m_paramViews.assign(textureCount, nullptr);
        if (textureCount > 0 && desc.instance)
        {
            const std::vector<Guid> paramIds = desc.instance->ResolveTextures();
            for (std::uint32_t t = 0; t < textureCount; ++t)
            {
                if (!m_textures || t >= paramIds.size())
                    continue;
                nri::Texture* texture = m_textures->Resolve(paramIds[t]);
                if (!texture)
                    continue;   // the cache reported the miss, once
                m_paramTextures[t] = texture;
                m_paramViews[t]    = m_textures->View(paramIds[t]);
            }
        }

        const std::uint32_t framesInFlight = FramesInFlight();
        for (std::uint32_t p = 0; p < passCount; ++p)
        {
            Pass& pass = m_passes[p];
            const PostChainPassDesc& source = desc.passes[p];
            if (!source.vsBytes || !source.psBytes || source.vsBytes->empty() || source.psBytes->empty())
                return refuse("pass " + std::to_string(p) + " carries no stitched shader bytecode");

            pass.vs = source.vsBytes;
            pass.ps = source.psBytes;
            // A CONTENT hash rather than an id: the pipeline cache's fill
            // contract (rule 3) requires the key to change whenever the
            // bytecode would, and a chain RE-compile replaces the bytes under
            // the same scene assignment.
            pass.shaderPairId = HashBytes(pass.ps->data(), pass.ps->size(),
                                          HashBytes(pass.vs->data(), pass.vs->size()))
                              | kShaderPairMark;

            for (std::uint32_t slot = 0; slot < framesInFlight; ++slot)
            {
                if (!pass.set[slot]
                    && (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *pipelineLayout, 0,
                                                                    &pass.set[slot], 1, 0))
                        || !pass.set[slot]))
                {
                    return refuse("a descriptor set could not be allocated -- the pool holds "
                                  + std::to_string(m_caps.maxPasses * framesInFlight)
                                  + " sets, sized by render.post.maxPasses");
                }

                // EVERY `descriptors` SOURCE BELOW MUST OUTLIVE THE
                // UpdateDescriptorRanges CALL. UpdateDescriptorRangeDesc::
                // descriptors is a POINTER TO AN ARRAY (NRIDescs.h),
                // dereferenced inside the call -- so a single descriptor is
                // passed as the address of a variable, and that variable has to
                // still be alive when the call runs. Hence every one of them is
                // declared in THIS scope, including `materialCb`, which is only
                // assigned inside the `cbSize > 0` branch (Task 9's fix round 1
                // found exactly that dangle in the sprite twin).
                nri::UpdateDescriptorRangeDesc updates[2] = {};
                std::uint32_t updateCount = 0;
                const nri::Descriptor* materialCb = nullptr;
                if (cbSize > 0)
                {
                    materialCb = m_materialView[slot];
                    updates[updateCount].descriptorSet = pass.set[slot];
                    updates[updateCount].rangeIndex    = layout.materialCb;
                    updates[updateCount].descriptors   = &materialCb;
                    updates[updateCount].descriptorNum = 1;
                    ++updateCount;
                }
                const nri::Descriptor* globalsCb = m_globalsView[slot];
                updates[updateCount].descriptorSet = pass.set[slot];
                updates[updateCount].rangeIndex    = layout.globalsCb;
                updates[updateCount].descriptors   = &globalsCb;
                updates[updateCount].descriptorNum = 1;
                ++updateCount;
                core.UpdateDescriptorRanges(updates, updateCount);

                if (layout.sampler != FullscreenMaterialLayout::kNoRange)
                {
                    const nri::Descriptor* sampler = m_sampler;
                    nri::UpdateDescriptorRangeDesc samplerUpdate = {};
                    samplerUpdate.descriptorSet = pass.set[slot];
                    samplerUpdate.rangeIndex    = layout.sampler;
                    samplerUpdate.descriptors   = &sampler;
                    samplerUpdate.descriptorNum = 1;
                    core.UpdateDescriptorRanges(&samplerUpdate, 1);
                }

                // The TEXTURE range is deliberately NOT written here: its
                // contents are graph transients that do not exist until
                // Execute() has realized the pool, so Record writes it on
                // first sight and whenever a resolved pointer changes.
                pass.written[slot] = false;
                pass.bound[slot].assign(m_caps.maxTextures + kMaxInputs, nullptr);
            }

            // The PSO, built HERE so a first-frame pipeline compile does not
            // land inside the recording window either.
            NriPipelineCache::GraphicsKey key = {};
            key.shaderPairId    = pass.shaderPairId;
            key.layoutId        = m_layoutId;
            key.colorFormats[0] = targetFormat;
            key.colorCount      = 1;
            key.depthFormat     = nri::Format::UNKNOWN;
            key.topology        = nri::Topology::TRIANGLE_LIST;
            // OPAQUE: a fullscreen pass overwrites every pixel of its target
            // (which is also why no pass clears one).
            key.blend           = NriPipelineCache::GraphicsKey::Blend::Opaque;
            key.depthWrite      = false;
            key.cullMode        = nri::CullMode::NONE;

            // `stages` lives in THIS frame, which encloses GetGraphics -- the
            // cache's fill contract, rule 2.
            nri::ShaderDesc stages[2] = {};
            stages[0].stage          = nri::StageBits::VERTEX_SHADER;
            stages[0].bytecode       = pass.vs->data();
            stages[0].size           = pass.vs->size();
            stages[0].entryPointName = kVsEntry;
            stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
            stages[1].bytecode       = pass.ps->data();
            stages[1].size           = pass.ps->size();
            stages[1].entryPointName = kPsEntry;

            pass.pipeline = m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& pipelineDesc)
            {
                // No vertex input at all: fullscreen_material.hlsl's vs_main
                // builds the triangle from SV_VertexID (the tonemap's shape).
                pipelineDesc.vertexInput = nullptr;
                pipelineDesc.shaders     = stages;
                pipelineDesc.shaderNum   = 2;
                pipelineDesc.rasterization.fillMode = nri::FillMode::SOLID;
            });
            if (!pass.pipeline)
                return refuse("pass " + std::to_string(p) + " has no pipeline for the canvas format");
        }
        return true;
    }

    std::uint32_t PostChainNode::PrepareChain(const PostChainDesc& desc, const GlobalParams& globals,
                                              nri::Format targetFormat)
    {
        if (desc.passes.empty() || !desc.templ || !desc.instance)
            return 0;

        m_globals = globals;

        // The identity of THIS bind. A chain re-save replaces the whole
        // PostChainDesc (PostChainCache::Bind), so every pointer below moves
        // with it; the sizes are folded in so a same-address reallocation of
        // different blobs is caught too. VALUES are deliberately NOT in it --
        // PackCB runs every frame, so a live param edit needs no rebuild.
        std::uint64_t stamp = HashValue((std::uint64_t)(std::uintptr_t)desc.templ.get(), kFnvOffset);
        stamp = HashValue((std::uint64_t)(std::uintptr_t)desc.instance.get(), stamp);
        stamp = HashValue(desc.chainInputSlots, stamp);
        stamp = HashValue(desc.passes.size(), stamp);
        stamp = HashValue((std::uint64_t)targetFormat, stamp);
        for (const PostChainPassDesc& pass : desc.passes)
        {
            stamp = HashValue((std::uint64_t)(std::uintptr_t)pass.vsBytes.get(), stamp);
            stamp = HashValue(pass.vsBytes ? pass.vsBytes->size() : 0, stamp);
            stamp = HashValue((std::uint64_t)(std::uintptr_t)pass.psBytes.get(), stamp);
            stamp = HashValue(pass.psBytes ? pass.psBytes->size() : 0, stamp);
            for (std::uint32_t input : pass.inputs)
                stamp = HashValue(input, stamp);
        }

        if (stamp != m_stamp)
        {
            // A REBUILD rewrites descriptor sets NRI cannot free and re-allocate
            // per edit, so the GPU must not be reading them. Idling is honest
            // here for the same reason it is in Batch2DNode::EnsureMaterial: the
            // edit already cost a shader compile, and it only happens on an
            // asset re-save at the desk. A FIRST build has nothing in flight.
            if (m_ready)
            {
                ARC_INFO("[nri-graph] PostChainNode: the scene post chain was re-bound -- "
                         "rebuilding its pipelines and bindings behind a device idle");
                (void)ARC_NRI_CHECK(m_device->Core().DeviceWaitIdle(&m_device->Device()));
            }
            m_ready   = false;
            m_refused = false;
            m_stamp   = stamp;
        }
        // A chain this node has already refused stays refused until it is
        // re-bound; without the latch its ERROR would repeat every frame for
        // the rest of the run.
        //
        // EXIT VISIBILITY, decided deliberately: refuse() below logs through
        // ARC_ERROR, which does NOT grow RenderErrorCount and therefore does
        // NOT fail a `--nri-graph` run's exit code. That is the same treatment
        // Batch2DNode gives a material it cannot honour, and it is the right
        // one: an over-cap or bytecode-less chain is a DEGRADATION (the frame
        // renders canvas -> tonemap, correctly) rather than a rendering
        // ERROR. The tagged GraphError seam -- which does latch -- is reserved
        // for the things that mean this frame is wrong, and Record uses it.
        if (m_refused)
            return 0;
        if (!m_ready)
        {
            if (!BuildChain(desc, targetFormat))
            {
                m_refused = true;
                return 0;   // already reported, once
            }
            m_ready = true;
        }

        // The values, packed on the CPU while no command buffer is open. Every
        // frame, unconditionally: MaterialInstance resolves its parent chain
        // here and a live param edit must show up without a rebuild -- which is
        // exactly what FullscreenMaterialPass::Render does too.
        if (m_cbSize > 0)
            desc.instance->PackCB(m_packed.data(), m_packed.size());

        return (std::uint32_t)m_passes.size();
    }

    void PostChainNode::Record(RenderGraphNodeContext& context, std::uint32_t pass,
                               std::span<const RgTexture> sources, RgTexture target,
                               std::uint32_t frameSlot)
    {
        const nri::CoreInterface& core = context.core;

        if (!m_ready || pass >= m_passes.size() || frameSlot >= FramesInFlight())
        {
            GraphError("PostChainNode: asked to record a pass of a chain that is not prepared");
            return;
        }

        // BEFORE anything reads m_views: this Execute() may already have
        // buried a pool texture one of them names.
        SyncPoolEpoch(context);

        // ---------------------------------------------------------------
        // The constant buffers, into THIS frame slot's arena regions, ONCE per
        // frame (every pass reads the same two -- the chain shares one merged
        // instance). FIRST, ahead of every failure path below: a pass 0 that
        // cannot record for any reason must not leave passes 1..N-1 reading
        // the PREVIOUS frame's constants out of this slot, which would be a
        // silently wrong picture layered on an already-reported problem.
        //
        // Written at record time for the same reason the ring's allocations
        // are: the frame-pacing fence this slot is safe behind is waited
        // inside Execute, upstream of every exec fn. HOST_UPLOAD memory is
        // host-coherent, so a memcpy is the whole of the upload -- and every
        // CPU write here precedes the single submit that makes any of it
        // readable by the GPU.
        // ---------------------------------------------------------------
        if (pass == 0 && m_arenaCpu)
        {
            auto* arena = static_cast<std::uint8_t*>(m_arenaCpu);
            std::memcpy(arena + ArenaOffset(frameSlot, kGlobalsRegion), &m_globals,
                        sizeof(GlobalParams));
            if (m_cbSize > 0 && !m_packed.empty())
                std::memcpy(arena + ArenaOffset(frameSlot, kMaterialRegion), m_packed.data(),
                            m_packed.size());
        }

        Pass& current = m_passes[pass];

        nri::Texture* targetTexture = context.Resolve(target);
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!targetTexture || !layout || !current.pipeline || !current.set[frameSlot])
        {
            GraphError("PostChainNode: pass " + std::to_string(pass)
                       + " could not resolve its target, layout, pipeline or descriptor set");
            return;
        }
        // The PSO baked ONE attachment format at PrepareChain time. The
        // declarator creates the transients with that same constant, so a
        // mismatch is structurally impossible -- and therefore worth refusing
        // loudly rather than handing the driver a pipeline for another format.
        if (core.GetTextureDesc(*targetTexture).format != m_targetFormat)
        {
            GraphError("PostChainNode: pass " + std::to_string(pass) + "'s target is not the format "
                       "its pipeline was built for");
            return;
        }

        // ---------------------------------------------------------------
        // The texture range: the declared params first, then this pass's
        // chain inputs. Rewritten only when a resolved pointer actually
        // changes (first frame, and after a resize re-realizes the pool) --
        // and safe to rewrite because this is frame slot `frameSlot`'s OWN
        // set, whose previous submission has retired before this frame
        // records (the pacing wait inside NriSwapChain::AcquireNextTexture).
        // ---------------------------------------------------------------
        const std::uint32_t textureCount = m_textureCount + m_chainInputs;
        // m_wantedScratch holds Caps::maxTextures + kMaxInputs entries and
        // BuildChain refused anything wider, so textureCount always fits.
        std::vector<nri::Texture*>& wanted = m_wantedScratch;
        for (std::uint32_t t = 0; t < m_textureCount; ++t)
            wanted[t] = m_paramTextures[t] ? m_paramTextures[t] : m_white;
        for (std::uint32_t i = 0; i < m_chainInputs; ++i)
        {
            nri::Texture* resolved = i < sources.size() ? context.Resolve(sources[i]) : nullptr;
            // A slot this pass wired nothing into reads the black texel, the
            // same fallback FullscreenMaterialPass::GetBindingSet substitutes.
            wanted[m_textureCount + i] = resolved ? resolved : m_black;
        }

        bool changed = !current.written[frameSlot];
        for (std::uint32_t t = 0; t < textureCount && !changed; ++t)
            changed = current.bound[frameSlot][t] != wanted[t];

        if (changed && textureCount > 0 && m_textureRange != FullscreenMaterialLayout::kNoRange)
        {
            std::vector<const nri::Descriptor*>& views = m_viewScratch;
            for (std::uint32_t t = 0; t < textureCount; ++t)
            {
                // THE INDEX IS THE DISCRIMINATOR, and it has to be: a DECLARED
                // param (t < m_textureCount) is an asset texture owned by the
                // shared cache, whose view is created once and lives as long as
                // the cache; a CHAIN INPUT is a graph POOL texture, whose view
                // this node caches and BURIES on every pool-epoch move. Routing
                // a cache texture through EnsureView would put a view over a
                // non-pool texture into that epoch-buried cache -- destroyed
                // out from under a live descriptor set the next time the pool
                // moved.
                nri::Descriptor* view =
                    t < m_textureCount ? (m_paramViews[t] ? m_paramViews[t] : m_whiteView)
                  : wanted[t] == m_white ? m_whiteView
                  : wanted[t] == m_black ? m_blackView
                  : EnsureView(core, wanted[t]);
                // A view NRI refused still has to bind something -- an empty
                // range entry is a validation error, not a black pixel.
                views[t] = view ? view : m_blackView;
            }

            // `views` is a local ARRAY and UpdateDescriptorRangeDesc::
            // descriptors is a pointer to one, dereferenced inside the call --
            // so it must (and does) outlive the call below. See BuildChain's
            // matching note.
            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = current.set[frameSlot];
            update.rangeIndex    = m_textureRange;
            update.descriptors   = views.data();
            update.descriptorNum = textureCount;
            core.UpdateDescriptorRanges(&update, 1);

            for (std::uint32_t t = 0; t < textureCount; ++t)
                current.bound[frameSlot][t] = wanted[t];
            current.written[frameSlot] = true;
        }

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetDescriptorSetDesc setDesc = {};
        setDesc.setIndex      = 0;
        setDesc.descriptorSet = current.set[frameSlot];
        core.CmdSetDescriptorSet(context.cmd, setDesc);

        core.CmdSetPipeline(context.cmd, *current.pipeline);

        nri::DrawDesc draw = {};
        draw.vertexNum   = 3;
        draw.instanceNum = 1;
        core.CmdDraw(context.cmd, draw);
    }

    RgTexture AddPostChainNodes(RenderGraph& graph, NriGraphContext* context,
                                RgTexture scene, const PostChainDesc& desc,
                                std::uint32_t passCount,
                                std::uint32_t width, std::uint32_t height)
    {
        // Defensive, not expected: PrepareChain returns m_passes.size(), which
        // IS desc.passes.size(), and the device-less drive clamps to
        // render.post.maxPasses.
        passCount = (std::uint32_t)std::min<std::size_t>(passCount, desc.passes.size());
        if (passCount == 0)
            return scene;

        // Shared rather than captured by value: every pass's target is minted
        // INSIDE its own setup fn, and a LATER pass may read an EARLIER one, so
        // the handles have to be readable after the AddNode that produced them.
        auto targets = std::make_shared<std::vector<RgTexture>>(passCount);

        const std::uint32_t inputSlots =
            (std::uint32_t)std::min<std::size_t>(desc.chainInputSlots, PostChainNode::kMaxInputs);

        for (std::uint32_t p = 0; p < passCount; ++p)
        {
            // This pass's wiring, resolved to handles BEFORE the node is
            // declared: everything it can legally reference (the scene, and any
            // EARLIER pass -- BuildMaterialChainSource validates that) already
            // exists at this point. An entry with nothing behind it stays
            // invalid and binds the black texel at record time.
            std::vector<RgTexture> sources((std::size_t)inputSlots, RgTexture{});
            const std::vector<std::uint32_t>& wiring = desc.passes[p].inputs;
            for (std::size_t i = 0; i < wiring.size() && i < sources.size(); ++i)
            {
                if (wiring[i] == kSceneInput)
                    sources[i] = scene;
                else if (wiring[i] < p)
                    sources[i] = (*targets)[wiring[i]];
            }

            const std::string name = "post" + std::to_string(p);
            graph.AddNode(name.c_str(), RenderGraph::NodeKind::Raster,
                [&graph, targets, sources, p, width, height](RenderGraphBuilder& builder)
                {
                    RgTextureDesc textureDesc;
                    textureDesc.format = GraphCanvasFormat();
                    textureDesc.width  = width;
                    textureDesc.height = height;
                    // Its OWN transient. The pool allocator is what makes two
                    // physical textures serve all of them -- see THE PING-PONG
                    // IS DERIVED in the header.
                    (*targets)[p] = builder.CreateTexture(("post" + std::to_string(p)).c_str(),
                                                           textureDesc);
                    // The declared reads ARE the barrier derivation: pass k
                    // reading pass k-1's target is what makes the executor
                    // emit COLOR_ATTACHMENT -> SHADER_RESOURCE. No node here
                    // records a CmdBarrier.
                    for (const RgTexture& source : sources)
                        if (graph.IsHandleValid(source))
                            builder.Read(source, RgUsage::ShaderRead);
                    builder.Write((*targets)[p], RgUsage::ColorWrite);
                    graph.SetColorAttachments(std::span<const RgTexture>(&(*targets)[p], 1));
                },
                [context, targets, sources, p](RenderGraphNodeContext& nodeContext)
                {
                    if (!context)
                        return;   // device-less declaration-shape drive
                    if (PostChainNode* node = context->PostChain())
                        node->Record(nodeContext, p, sources, (*targets)[p], context->FrameSlot());
                });
        }
        // NO CLEAR anywhere above, deliberately (the clear seam, stated in
        // NriGraphContext's DeclareGraphFrame): a fullscreen pass writes every
        // pixel of its target with an OPAQUE pipeline, so a LOADed undefined
        // pool slot is fully overwritten before it can be read.
        return passCount > 0 ? (*targets)[passCount - 1] : scene;
    }

    std::unique_ptr<TonemapNode> TonemapNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<TonemapNode> node(new TonemapNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool TonemapNode::Init(NriGraphContext& context)
    {
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();

        m_vs = context.ShaderBytecode("tonemap_vs");
        m_ps = context.ShaderBytecode("tonemap_ps");
        if (m_vs.empty() || m_ps.empty())
        {
            ARC_ERROR("[nri-graph] TonemapNode: the tonemap_vs/tonemap_ps bins are missing -- the graph "
                      "cannot produce display-referred pixels");
            return false;
        }

        const nri::CoreInterface& core = m_device->Core();

        // POINT + clamp, exactly TonemapPass::Init's sampler: the canvas maps
        // 1:1 onto the target, so any filtering would only soften it.
        nri::SamplerDesc samplerDesc = {};
        samplerDesc.filters.min  = nri::Filter::NEAREST;
        samplerDesc.filters.mag  = nri::Filter::NEAREST;
        samplerDesc.filters.mip  = nri::Filter::NEAREST;
        samplerDesc.addressModes = { nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE,
                                     nri::AddressMode::CLAMP_TO_EDGE };
        samplerDesc.mipMax       = 16.0f;
        if (!ARC_NRI_CHECK(core.CreateSampler(m_device->Device(), samplerDesc, m_sampler)) || !m_sampler)
        {
            ARC_ERROR("[nri-graph] TonemapNode: sampler creation failed");
            return false;
        }

        // data/shaders/tonemap.hlsl declares t0 + s0 and NO constant buffer, so
        // this layout carries no root constants at all. Register space 0 to
        // match the shaders' implicit space; on Vulkan NRI applies the device's
        // vkBindingOffsets (t=0, s=128) to these indices itself.
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

        // Value-initialized then assigned field by field -- NriPipelineCache's
        // DEDUP CONTRACT.
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.descriptorSets    = &setDesc;
        layoutDesc.descriptorSetNum  = 1;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] TonemapNode: pipeline layout registration failed");
            return false;
        }

        // ONE SET PER FRAME SLOT -- see SOURCE VIEWS AND THE POOL, mechanism 2.
        const std::uint32_t framesInFlight = FramesInFlight();
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum = framesInFlight;
        poolDesc.textureMaxNum       = framesInFlight;
        poolDesc.samplerMaxNum       = framesInFlight;
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] TonemapNode: descriptor pool creation failed");
            return false;
        }
        for (std::uint32_t slot = 0; slot < framesInFlight; ++slot)
        {
            if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0, &m_set[slot], 1, 0))
                || !m_set[slot])
            {
                ARC_ERROR("[nri-graph] TonemapNode: descriptor set allocation failed for frame slot {}",
                          slot);
                return false;
            }

            // The sampler half of each set never changes; the texture half is
            // written by EnsureSource on the first frame that uses the slot and
            // whenever the source that frame resolves to changes.
            const nri::Descriptor* sampler = m_sampler;
            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = m_set[slot];
            update.rangeIndex    = 1;
            update.descriptors   = &sampler;
            update.descriptorNum = 1;
            core.UpdateDescriptorRanges(&update, 1);
        }
        return true;
    }

    TonemapNode::~TonemapNode()
    {
        if (!m_device || (!m_sampler && !m_pool && m_views.empty()))
            return;

        ARC_WARN("[nri-graph] TonemapNode destroyed with live NRI objects -- either Create() failed "
                 "part way (an ERROR above says which step) or its owner never called Release(). "
                 "Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (const FullscreenSourceView& cached : m_views)
            if (cached.view) core.DestroyDescriptor(cached.view);
        m_views.clear();
        if (m_pool)    core.DestroyDescriptorPool(m_pool);
        if (m_sampler) core.DestroyDescriptor(m_sampler);
        m_pool = nullptr; m_sampler = nullptr;
        for (nri::DescriptorSet*& set : m_set)
            set = nullptr;
        for (nri::Texture*& bound : m_bound)
            bound = nullptr;
    }

    void TonemapNode::InvalidateSource(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();
        for (const FullscreenSourceView& cached : m_views)
            if (cached.view)
                graveyard.Bury(fence, [core, view = cached.view] { core->DestroyDescriptor(view); });
        m_views.clear();
        // The sets still NAME the buried views, so nothing may bind one until
        // EnsureSource rewrites the texture range -- which clearing this is
        // exactly what forces.
        for (nri::Texture*& bound : m_bound)
            bound = nullptr;
    }

    void TonemapNode::SyncPoolEpoch(const RenderGraphNodeContext& context)
    {
        // Mechanism 1 (see the header). RealizePool may have buried a pool
        // texture THIS Execute() -- a shrink or a desc change -- and it runs
        // between the declarations and this exec fn, so there was no earlier
        // point at which the owner could have told us. The epoch is the only
        // observable; a pointer comparison cannot see it, because NRI may hand
        // the recreated texture the vacated address.
        if (!context.graph)
            return;
        SyncPoolEpoch(*context.graph);
    }

    void TonemapNode::SyncPoolEpoch(const RenderGraph& graph)
    {
        if (!m_device)
            return;
        // The GRAPH's lane rather than the device's -- see
        // PostChainNode::SyncPoolEpoch above for why.
        Graveyard* graves = graph.Graves();
        if (!graves)
            return;   // from an exec fn: unreachable (Execute latches the lane before recording).
                      // From the owner at declaration time: the graph has never Executed, so it
                      // owns no pool and this node can hold no view over one.
        const std::uint64_t epoch = graph.PoolEpoch();
        if (epoch == m_poolEpoch)
            return;
        m_poolEpoch = epoch;
        // Buried, not destroyed: an earlier submitted frame may still be
        // reading them. DebugSubmitCount() is the submission that last used
        // them -- the same value the graph keys its OWN burials to, which is
        // what keeps Graveyard's nondecreasing rule satisfied.
        InvalidateSource(*graves, graph.DebugSubmitCount());
    }

    void TonemapNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        InvalidateSource(graveyard, fence);

        const nri::CoreInterface* core = &m_device->Core();
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
            for (nri::DescriptorSet*& set : m_set)
                set = nullptr;   // owned by the pool
        }
        if (m_sampler)
        {
            graveyard.Bury(fence, [core, d = m_sampler] { core->DestroyDescriptor(d); });
            m_sampler = nullptr;
        }
    }

    bool TonemapNode::EnsureSource(const nri::CoreInterface& core, nri::Texture* texture,
                                   std::uint32_t frameSlot)
    {
        if (frameSlot >= FramesInFlight() || !m_set[frameSlot])
        {
            GraphError("TonemapNode: no descriptor set for this frame slot");
            return false;
        }
        if (m_bound[frameSlot] == texture)
            return true;   // this slot's set already names it

        nri::Descriptor* view = nullptr;
        for (const FullscreenSourceView& cached : m_views)
            if (cached.texture == texture)
                view = cached.view;

        if (!view)
        {
            // The tonemap samples the canvas OR the post chain's last target,
            // and the pool collapses those onto two physical textures -- so a
            // cache that keeps growing means pool textures are being destroyed
            // without the epoch moving, which would be a bug in RenderGraph
            // rather than here. Say so once instead of growing silently.
            if (m_views.size() >= 4 && !m_warnedViewChurn)
            {
                m_warnedViewChurn = true;
                GraphError("TonemapNode: more distinct source textures than this frame can have -- "
                           "pool textures are being replaced without RenderGraph::PoolEpoch moving");
            }

            nri::TextureViewDesc viewDesc = {};
            viewDesc.texture  = texture;
            viewDesc.type     = nri::TextureView::TEXTURE;
            // The transient's ACTUAL format, read back from NRI -- never assumed.
            viewDesc.format   = core.GetTextureDesc(*texture).format;
            viewDesc.mipNum   = 1;
            viewDesc.layerNum = 1;
            if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, view)) || !view)
            {
                GraphError("TonemapNode: the source shader-resource view could not be created");
                return false;
            }
            m_views.push_back(FullscreenSourceView{ texture, view });
        }

        // ONLY this frame slot's set is touched, and its previous submission
        // has already retired (the pacing wait inside AcquireNextTexture) --
        // which is what makes a mid-run source change ordinary rather than a
        // hazard needing a device idle.
        const nri::Descriptor* bound = view;
        nri::UpdateDescriptorRangeDesc update = {};
        update.descriptorSet = m_set[frameSlot];
        update.rangeIndex    = 0;
        update.descriptors   = &bound;
        update.descriptorNum = 1;
        core.UpdateDescriptorRanges(&update, 1);
        m_bound[frameSlot] = texture;
        return true;
    }

    void TonemapNode::Record(RenderGraphNodeContext& context, RgTexture source, RgTexture target,
                             std::uint32_t frameSlot)
    {
        const nri::CoreInterface& core = context.core;

        // BEFORE anything reads m_views: this Execute() may already have
        // buried a pool texture one of them names.
        SyncPoolEpoch(context);

        nri::Texture* sourceTexture = context.Resolve(source);
        nri::Texture* targetTexture = context.Resolve(target);
        if (!sourceTexture || !targetTexture)
        {
            GraphError("TonemapNode: the node could not resolve its source or its target");
            return;
        }
        if (!EnsureSource(core, sourceTexture, frameSlot))
            return;   // already reported

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("TonemapNode: the pipeline layout is gone -- nothing recorded");
            return;
        }

        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kShaderPairId;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = core.GetTextureDesc(*targetTexture).format;
        key.colorCount      = 1;
        key.depthFormat     = nri::Format::UNKNOWN;
        key.topology        = nri::Topology::TRIANGLE_LIST;
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
            // No vertex input at all: tonemap.hlsl's vs_main builds the
            // fullscreen triangle from SV_VertexID.
            desc.vertexInput = nullptr;
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            desc.rasterization.fillMode = nri::FillMode::SOLID;
        });
        if (!pipeline)
            return;   // already logged + latched by the cache

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetDescriptorSetDesc setDesc = {};
        setDesc.setIndex      = 0;
        setDesc.descriptorSet = m_set[frameSlot];
        core.CmdSetDescriptorSet(context.cmd, setDesc);

        core.CmdSetPipeline(context.cmd, *pipeline);

        nri::DrawDesc draw = {};
        draw.vertexNum   = 3;
        draw.instanceNum = 1;
        core.CmdDraw(context.cmd, draw);
    }

    RgTexture AddTonemapNode(RenderGraph& graph, NriGraphContext* context, RgTexture source,
                             nri::Texture* offscreenOutput)
    {
        // The offscreen import's entry/exit pair. Stated
        // here, beside the one call that uses them, rather than on
        // NriGraphContext: they describe how THE TONEMAP treats its target, and
        // the swapchain's equivalents live in RenderGraphBuilder::
        // ImportSwapChainTexture for exactly the same reason.
        //
        // ENTRY is contents-discarding -- byte-for-byte the triple a freshly
        // acquired backbuffer carries. That is not indifference about the
        // previous frame's contents: the tonemap covers the whole target with
        // an opaque fullscreen triangle, so nothing is lost, and it makes frame
        // 1 (a just-created texture, genuinely undefined) and frame N (left in
        // SHADER_RESOURCE by the last frame's exit barrier) ONE case rather
        // than two. It is also the only entry unconditionally legal under D3D12
        // enhanced barriers, whose UNDEFINED layout admits no access bits
        // (RenderGraphTest.cpp's CheckBeforeIsD3D12Legal carries the citation).
        //
        // EXIT is SHADER_RESOURCE, and that is the whole feature: the frame
        // hands its output back in a state a sampler can read, without anybody
        // writing a barrier for it. The stages are the same three
        // RgUsage::ShaderRead derives, so the exit matches what a sampler in
        // any of them expects.
        // static: the tonemap lambda below binds these to const& (an odr-use),
        // which a non-static local would need captured -- Clang enforces it.
        static constexpr nri::AccessLayoutStage kOffscreenEntry{
            nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL };
        static constexpr nri::AccessLayoutStage kOffscreenExit{
            nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
            nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER
                | nri::StageBits::COMPUTE_SHADER };

        // Shared rather than captured by value: the target handle is minted
        // INSIDE this node's own setup fn, which AddNode runs after both
        // lambdas have already been constructed. A by-value capture would
        // freeze the default (invalid) handle.
        auto backbuffer = std::make_shared<RgTexture>();
        graph.AddNode("tonemap", RenderGraph::NodeKind::Raster,
            [&graph, source, backbuffer, offscreenOutput](RenderGraphBuilder& builder)
            {
                if (offscreenOutput)
                {
                    // An ORDINARY import: unlike the swapchain there IS a
                    // texture at declaration time, this vehicle owns it, and it
                    // outlives the frame -- so `persistent` is true, and the
                    // graph restores it to a sampler-readable state on the way
                    // out instead of to PRESENT.
                    *backbuffer = builder.ImportTexture("offscreen", offscreenOutput,
                                                        kOffscreenEntry, kOffscreenExit,
                                                        /*persistent=*/true);
                }
                else
                {
                    // NOT ImportTexture: the graph owns acquire/present
                    // sequencing and there is no nri::Texture* to hand over at
                    // declaration time -- Execute() resolves this to the
                    // texture it acquires.
                    *backbuffer = builder.ImportSwapChainTexture("backbuffer");
                }
                builder.Read(source, RgUsage::ShaderRead);
                builder.Write(*backbuffer, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(backbuffer.get(), 1));
            },
            [context, source, backbuffer](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                if (TonemapNode* node = context->Tonemap())
                    node->Record(nodeContext, source, *backbuffer, context->FrameSlot());
            });
        return *backbuffer;
    }
}
