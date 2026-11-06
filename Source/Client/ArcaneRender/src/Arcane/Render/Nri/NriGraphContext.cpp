// NriGraphContext -- see the header for what this is, the desk commands, the
// exit-code contract, and THE BORROWED WINDOW (why this object presents into
// the host's window rather than one of its own, and what the caller owes for
// that to be safe).
//
// Same include-order rule as every file in this directory (NriCommon.hpp): NRI
// headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/IO/ReadRegularFile.hpp>
#include <Extensions/NRISwapChain.h>

#include "NriGraphContext.hpp"

#include "NriCommon.hpp"

#include <Arcane/Base/Diagnostics.hpp>              // Heartbeat / GpuHeartbeatRefresh -- the offscreen pacing wait
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>       // RenderDeviceDesc (and GraphicsBackend + ToString behind it)
#include <Arcane/Render/RenderBudgetSettings.hpp>   // RenderPostSettings -- the device-less post clamp
#include <Arcane/Render/RenderDeviceSettings.hpp>   // MakeRenderDeviceDesc -- render.debug.*, render.uploadRingBytesPerFrame, the graph formats
#include <Arcane/Render/Nri/NriDiagnostics.hpp>     // the crash chain, armed by whichever device exists
#include <Arcane/Render/RenderErrorLatch.hpp>   // the tagged "nri-graph" error seam
#include <Arcane/Render/PostChainCache.hpp>         // PostChainDesc -- the frame's post-chain shape
#include <Arcane/Render/GpuInstrumentation.hpp>       // GpuDeviceLostObserved -- the device-lost teardown gate
#include <Arcane/Render/ShaderPaths.hpp>            // ShaderPaths::ResolveFlavorDir
#include <Arcane/Render/FramePacing.hpp>              // FramesInFlight(), LatchFramesInFlight()
#include <Arcane/Assets/ImageIo.hpp>                 // NormalizeCaptureRows -- the one capture normalization

#include <SDL3/SDL_timer.h>                         // SDL_DelayNS -- the offscreen pacing wait's sleep

#undef ERROR

#include <Arcane/Render/RenderFramePacket.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace Arcane
{
    // render.canvasFormat / render.depthFormat, latched on first use (the
    // header's contract: one format per process, Restart). The latch itself,
    // and its too-early diagnostic, live in RenderDeviceSettings.cpp.
    nri::Format GraphCanvasFormat() noexcept
    {
        return ToNriFormat(LatchedCanvasFormat());
    }

    nri::Format GraphDepthFormat() noexcept
    {
        return ToNriFormat(LatchedDepthFormat());
    }

    namespace
    {
        // The tagged seam RenderGraphExec.cpp reports through -- the VEHICLE's
        // own refusals, not an NRI call's result (those go through
        // ARC_NRI_CHECK). Both land in the RenderErrorCount() latch, which is
        // what makes a `--nri-graph` desk run's exit code meaningful.
        void GraphError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        // BindPacket's pointer lives for exactly one RenderFrame /
        // RenderFrameOffscreen call: this resets it on every return path of
        // the function that declares it, so a later frame -- one whose host
        // bound nothing -- can never draw a stale packet.
        struct PacketBindingReset
        {
            const RenderFramePacket*& packet;
            ~PacketBindingReset() { packet = nullptr; }
        };

        // Where the offline shader artifacts live, relative to the
        // executable. Resolved through ShaderPaths::ResolveFlavorDir, so a
        // desk user pointing ARCANE_SHADER_DIR at a live recompile moves
        // every shader consumer together.
        constexpr const char* kShaderDir = "data/shaders";

        // WritePngRgba wants RGBA; NRI resolves the swapchain's channel order
        // rather than pinning it (NriSwapChain::Format()), so a BGRA
        // backbuffer has to be swizzled on the way out. Exactly the check
        // NRISamples' Readback.cpp performs on its mapped pixel.
        bool IsBgraFormat(nri::Format format) noexcept
        {
            return format == nri::Format::BGRA8_UNORM || format == nri::Format::BGRA8_SRGB;
        }

        std::uint64_t AlignUp(std::uint64_t value, std::uint32_t alignment) noexcept
        {
            return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
        }

        // ---------------------------------------------------------------
        // THE OFFSCREEN PACING WAIT.
        //
        // A DELIBERATE TWIN of NriSwapChain.cpp's PollingWaitForTimelineFence:
        // same constants, same two heartbeat beats in the same order, same
        // blocking fallback past the window, and the same reasoning -- that
        // file's header states it in full, and it is the reason a bare
        // Core().Wait() cannot be used (both backends' nri::Fence::Wait blocks
        // internally for up to NRI_TIMEOUT_FENCE = 5s publishing nothing, so a
        // hung GPU would look like a hung process).
        //
        // DUPLICATED RATHER THAN EXTRACTED, and the extraction is still
        // owed: a shared version would have to be called from
        // NriSwapChain.cpp too, which is the host-window present path.
        // ---------------------------------------------------------------
        ARC_CONSTANT("OS timer floor: the poll sleep/window; below the scheduling quantum a sleep degrades into a spin")
        constexpr Uint64 kFencePollSleepNs = 1'000'000;         // 1ms, matching NriSwapChain.cpp
        ARC_CONSTANT("OS timer floor: the poll sleep/window; below the scheduling quantum a sleep degrades into a spin")
        constexpr std::chrono::seconds kFencePollWindow{ 15 };  // ...and its 15s window

        void PollingWaitForTimelineFence(const nri::CoreInterface& core, nri::Fence* fence,
                                         std::uint64_t value)
        {
            if (!fence)
                return;
            if (core.GetFenceValue(*fence) >= value)
                return;   // fast path: the slot's frame retired long ago

            const auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start < kFencePollWindow)
            {
                Diagnostics::Heartbeat();
                Diagnostics::GpuHeartbeatRefresh();

                SDL_DelayNS(kFencePollSleepNs);

                if (core.GetFenceValue(*fence) >= value)
                    return;
            }

            core.Wait(*fence, value);
        }
    }

    // FIRST, before any per-frame resource: render.framesInFlight (Restart)
    // becomes FramesInFlight() for the rest of the process, so the ring,
    // the command-buffer slots, the node pools and the swapchain all agree.
    //
    // render.uploadRingBytesPerFrame (Restart) is this context's per-frame-
    // slot upload arena -- the batch node's vertex + index streams (Task 8),
    // the HUD's geometry and the pick pass's ids. Init()'d at Create so a
    // mapping failure is found at boot rather than inside the first draw. A
    // starting point, not a measurement: NriUploadRing::HighWater() is the
    // number to size it from, and it is logged at shutdown. The 4 MiB default
    // is ~27k quads of batch geometry (128 B of vertices + 24 B of indices
    // each).
    NriGraphContext::NriGraphContext()
        : m_uploadRingBytes(Settings<RenderSettings>().uploadRingBytesPerFrame)
    {
        LatchFramesInFlight();
    }

    std::unique_ptr<NriGraphContext> NriGraphContext::Create(const RenderGraphConfig& config, Window& window)
    {
        std::unique_ptr<NriGraphContext> context(new NriGraphContext());
        if (!context->Init(config, window))
            return nullptr;
        return context;
    }

    bool NriGraphContext::Init(const RenderGraphConfig& config, Window& window)
    {
        m_mode           = Mode::HostWindow;
        m_vsync          = config.vsync;
        m_borrowedWindow = &window;

        ARC_INFO("[nri-graph] starting the graph render half: backend={} vsync={} frames={}",
                 ToString(config.backend), config.vsync ? "on" : "off", config.maxFrames);

        // NO WINDOW IS CREATED HERE. The swapchain below binds the HOST's
        // window -- the one GpuContext::Create built, with 1280x720 resizable
        // defaults and (on Vulkan) SDL_WINDOW_VULKAN set. See the header's
        // THE BORROWED WINDOW for why it is borrowed rather than owned.
        if (!window.NativeHandle())
        {
            ARC_ERROR("[nri-graph] the borrowed host window has no native handle -- nothing to "
                      "bind a swapchain to");
            return false;
        }

        // -------------------------------------------------------------
        // The creation half, its validation from render.debug.* (settings
        // arc S6-16; all three on by default in Debug builds, off otherwise)
        // -- every channel a validation message can arrive through ends at
        // RenderErrorCount, which is what makes this run's exit code mean
        // something (VK core + sync validation -> DeviceCreationVulkan.cpp's
        // VkDebugCallback; the D3D12 debug layer -> DeviceCreationD3D12.cpp's
        // ID3D12InfoQueue1 callback; NRI's own validation layer ->
        // MakeNriCallbacks).
        //
        // ALL THREE ARE LIVE SINCE TASK 6, and that is the change: this is now
        // the FIRST graphics device the process creates, so
        // ID3D12Debug::EnableDebugLayer -- a before-any-device call -- is
        // actually made rather than declined (DeviceCreationD3D12.cpp's
        // g_d3d12DeviceCreated is still false at this point). dx12 Debug runs
        // therefore route D3D12 CPU validation into D3D12DebugLayerCallback and
        // into the error latch for the first time on this path; findings from
        // it are NEW SIGNAL, not regressions. Whether ID3D12InfoQueue1 is
        // implemented by the servicing d3d12SDKLayers.dll is a separate
        // question that same file logs the answer to.
        // -------------------------------------------------------------
        // config.backend, not render.backend: the host adopted the setting
        // at boot, and GpuContext may have fallen Vulkan back to D3D12.
        RenderDeviceDesc dd = MakeRenderDeviceDesc();
        dd.backend = config.backend;

        // NO two-VkDevice WARN here any more, and its absence is the point:
        // NriDevice.hpp's "one live Vulkan device per process" rule held only
        // by convention while GpuContext also created one. This is the only
        // VkDevice now, so the Vulkan-Hpp default dispatcher
        // (Render/VulkanDispatchStorage.cpp) binds the device that is actually
        // rendering, and the dispatcher-rebind hazard the Phase-2 vehicle
        // carried is gone by construction rather than by care.

        m_native = NativeDeviceOwner::Create(dd);
        if (!m_native)
        {
            ARC_ERROR("[nri-graph] native {} device creation failed", ToString(config.backend));
            return false;
        }

        m_ownedDevice = NriDevice::Wrap(*m_native);
        if (!m_ownedDevice)
        {
            ARC_ERROR("[nri-graph] wrapping the native device failed");
            return false;
        }
        // The reading pointer, for every line below and for the offscreen
        // flavor's borrowed case -- see the member declarations.
        m_device = m_ownedDevice.get();

        // THE CRASH CHAIN, armed on the one device this process has. This is
        // the ONLY arming call in the process, so Arm() takes its full-arm
        // path: the device-removed hook, the graph GPU-section provider, the
        // active-backend slot and both latch resets. (It infers the topology
        // from the slot rather than being told, so a second context on the
        // same device works through the same call.)
        //
        // Immediately after the wrap and BEFORE the swapchain, so a failure
        // anywhere below is already covered by a live device-removed
        // observation point and a live GPU-section provider. Disarmed in
        // ~NriGraphContext, before the device it names goes away.
        //
        // THE RETURN VALUE IS KEPT, and it is what gates the Disarm in the
        // destructor. That matters because a SECOND context on this same
        // device must not be able to disarm the chain THIS one installed, and
        // NriDiagnostics keeps one process-wide slot with no owner identity --
        // so the ownership has to be remembered here.
        m_armedDiagnostics = NriDiagnostics::Arm(*m_device);

        m_swap = NriSwapChain::Create(*m_device, window, m_vsync);
        if (!m_swap)
        {
            // On dx12 the one cause worth naming: DXGI allows only one
            // flip-model swap chain per HWND at a time. Nothing else in the
            // process owns one on this window, so if this fires it is a real
            // defect -- a second graph context over the same window -- see
            // the header's THE BORROWED WINDOW.
            ARC_ERROR("[nri-graph] swapchain create failed over the host window (dx12: is another "
                      "flip-model swapchain already associated with this HWND?)");
            return false;
        }
        m_format = m_swap->Format();
        if (m_format == nri::Format::UNKNOWN)
        {
            ARC_ERROR("[nri-graph] the swapchain resolved no format (zero-sized window at create?)");
            return false;
        }

        // A HOST-WINDOW CONTEXT PRESENTS CHROME BY DEFINITION, so it always
        // builds the host HUD node -- this is the line that keeps the runtime's
        // (and, from Task 10, the editor chrome frame's) HUD backend alive now
        // that the node is gated (Task 9 fix round 1). Nothing else is asked
        // for: this context's frame is the batch, the tonemap and its own HUD,
        // and --pick-probe is the only thing that has ever added to that.
        NodeSet chromeNodes;
        chromeNodes.hostHud = true;
        if (!InitCommon(config, chromeNodes))
            return false;   // already logged

        ARC_INFO("[nri-graph] ready: {}x{} format={} textures={} ring={}KiB/slot",
                 m_swap->Width(), m_swap->Height(), (int)m_format, m_swap->TextureCount(),
                 m_uploadRingBytes / 1024);
        return true;
    }

    // =====================================================================
    // THE OFFSCREEN FLAVOR -- see the header's OFFSCREEN
    // MODE for the contract, and CALLER CONTRACT: TWO CONTEXTS, ONE GRAVEYARD
    // for what a second live context on one device owes.
    // =====================================================================

    std::unique_ptr<NriGraphContext> NriGraphContext::CreateOffscreen(const RenderGraphConfig& config,
                                                                      NriDevice& shared,
                                                                      std::uint32_t width,
                                                                      std::uint32_t height,
                                                                      const NodeSet& nodes)
    {
        std::unique_ptr<NriGraphContext> context(new NriGraphContext());
        if (!context->InitOffscreen(config, shared, width, height, nodes))
            return nullptr;
        return context;
    }

    bool NriGraphContext::InitOffscreen(const RenderGraphConfig& config, NriDevice& shared,
                                        std::uint32_t width, std::uint32_t height,
                                        const NodeSet& nodes)
    {
        m_mode = Mode::Offscreen;
        // NO WINDOW, NO SWAPCHAIN, NO VSYNC. m_vsync stays at its default and
        // is never read on this path: there is no present to pace against a
        // refresh, and the frame's own pacing fence is what bounds it instead.

        ARC_INFO("[nri-graph] starting the graph render half OFFSCREEN: {}x{} format={} on the "
                 "shared {} device", width, height, (int)kGraphOffscreenFormat,
                 ToString(shared.Backend()));

        // A zero extent is a CALLER BUG here, unlike the minimised-window case
        // the present path treats as routine: nothing outside this call chose
        // the size, so there is nobody to wait for.
        if (width == 0 || height == 0)
        {
            ARC_ERROR("[nri-graph] offscreen create refused: a {}x{} output is not a target",
                      width, height);
            return false;
        }

        // BORROWED. No NativeDeviceOwner, no Wrap, and deliberately no
        // NriDiagnostics::Arm -- the crash chain is already installed by
        // whoever created this device, and Arm/Disarm name ONE process-wide
        // slot with no owner identity, so an offscreen context that armed would
        // be harmless while one that DISARMED would unplug the host-window
        // context's chain. m_armedDiagnostics therefore stays false and the
        // destructor's Disarm is skipped.
        m_device = &shared;
        m_format = kGraphOffscreenFormat;

        if (!CreateOffscreenTarget(width, height))
            return false;   // already logged

        // The pacing timeline this frame loop has instead of a swapchain's.
        // Created ONCE and never recreated -- ResizeOffscreen leaves it alone
        // for the same reason NriSwapChain::Resize leaves its frame fence
        // alone: the frame counter and the pacing depth survive a resize.
        if (!ARC_NRI_CHECK(m_device->Core().CreateFence(m_device->Device(), 0, m_offscreenFence))
            || !m_offscreenFence)
        {
            ARC_ERROR("[nri-graph] offscreen pacing-fence creation failed");
            m_offscreenFence = nullptr;
            return false;
        }
        m_device->Core().SetDebugName(m_offscreenFence, "nri-graph offscreen pacing fence");

        if (!InitCommon(config, nodes))
            return false;   // already logged

        ARC_INFO("[nri-graph] offscreen ready: {}x{} format={} ring={}KiB/slot, pacing {} frames deep",
                 m_offscreenWidth, m_offscreenHeight, (int)m_format,
                 m_uploadRingBytes / 1024, FramesInFlight());
        return true;
    }

    bool NriGraphContext::CreateOffscreenTarget(std::uint32_t width, std::uint32_t height)
    {
        // nri::Dim_t is 16-bit; an extent that does not round-trip through it
        // would create successfully as a DIFFERENT texture. Refused here rather
        // than discovered as a wrong-sized viewport.
        if (width == 0 || height == 0 || width > 0xFFFFu || height > 0xFFFFu)
        {
            ARC_ERROR("[nri-graph] offscreen extent {}x{} is not expressible (nri::Dim_t is 16-bit, "
                      "and 0 is not a valid dimension)", width, height);
            return false;
        }

        const nri::CoreInterface& core = m_device->Core();

        nri::TextureDesc desc = {};
        desc.type      = nri::TextureType::TEXTURE_2D;
        // BOTH bits, and both are load-bearing: the tonemap renders into it
        // (COLOR_ATTACHMENT) and the consumer samples it (SHADER_RESOURCE),
        // which is the exit state the graph restores. NRI's validation layer
        // checks every barrier's access and layout against this mask, so a
        // missing bit rejects the frame's exit barrier rather than merely
        // looking wrong.
        desc.usage     = nri::TextureUsageBits::COLOR_ATTACHMENT | nri::TextureUsageBits::SHADER_RESOURCE;
        desc.format    = kGraphOffscreenFormat;
        desc.width     = (nri::Dim_t)width;
        desc.height    = (nri::Dim_t)height;
        desc.depth     = 1;
        desc.mipNum    = 1;
        desc.layerNum  = 1;
        desc.sampleNum = 1;
        if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                        0.0f, desc, m_offscreen))
            || !m_offscreen)
        {
            ARC_ERROR("[nri-graph] offscreen output creation failed at {}x{}", width, height);
            m_offscreen = nullptr;
            return false;
        }
        core.SetDebugName(m_offscreen, "nri-graph offscreen output");

        m_offscreenWidth  = width;
        m_offscreenHeight = height;
        return true;
    }

    bool NriGraphContext::InitCommon(const RenderGraphConfig& config, const NodeSet& nodes)
    {
        // Everything from here down is MODE-AGNOSTIC and is the same object
        // graph in the same order in both flavors -- which is what makes the
        // teardown contract in the header one contract rather than two.

        // The upload ring is [gpu]-only by construction (NONE's MapBuffer
        // returns null), so this is its first real Init in the tree.
        if (!m_ring.Init(*m_device, m_uploadRingBytes))
        {
            ARC_ERROR("[nri-graph] upload-ring init failed ({} bytes x {} slots; render.uploadRingBytesPerFrame)",
                      m_uploadRingBytes, FramesInFlight());
            return false;
        }

        m_pipelines.Bind(*m_device);

        // THE SHARED IMAGE CACHE, before the nodes -- they take a pointer to
        // it at Create. Its pixel SUPPLY is installed later by the frame
        // driver (SetPixelSupply); until then every Resolve misses, once and
        // loudly, which is the correct behaviour for a vehicle nobody wired an
        // Assets facade into.
        m_textures = NriTextureCache::Create(*m_device);
        if (!m_textures)
            return false;   // already logged

        m_meshBuffers = NriMeshBufferCache::Create(*m_device);
        if (!m_meshBuffers)
            return false;   // already logged

        // The GPU scene's device half (F3 plan 1 T6), eagerly like the cache
        // above -- see Scene().
        m_scene = GpuScene::Create(*m_device);
        if (!m_scene)
            return false;   // already logged

        m_graph = std::make_unique<RenderGraph>();

        // The offline artifacts the nodes below need as RAW BYTECODE (NRI's
        // ShaderDesc takes a blob, not a compiled shader object). Resolved
        // once, here, so a missing shader directory is one loud line at boot
        // instead of a per-node mystery.
        m_shaderDir = ShaderPaths::ResolveFlavorDir(config.backend, kShaderDir);
        if (m_shaderDir.empty())
        {
            ARC_ERROR("[nri-graph] no shader directory -- the graph path cannot build its pipelines");
            return false;
        }

        // Built EAGERLY, not on the first frame: a node that cannot be created
        // must fail the vehicle at boot rather than render a frame that
        // silently draws nothing.
        m_batch2D = Batch2DNode::Create(*this);
        if (!m_batch2D)
            return false;   // already logged
        m_meshCull = MeshCullNode::Create(*this);
        if (!m_meshCull)
            return false;
        // The mesh pass (Task 7; opaque + masked + ordered transparent since F3
        // plan 2), eagerly like the rest -- see Mesh().
        m_mesh = MeshNode::Create(*this);
        if (!m_mesh)
            return false;   // already logged
        // The 3D reference grid (F4 plan 1 T10), eagerly like the rest -- see Grid().
        m_grid = GridNode::Create(*this);
        if (!m_grid)
            return false;   // already logged
        // The post-tonemap overlay pass (F5 plan 1 T3), eagerly -- see Overlay().
        m_overlay = OverlayNode::Create(*this);
        if (!m_overlay)
            return false;   // already logged
        m_post = PostChainNode::Create(*this);
        if (!m_post)
            return false;   // already logged
        m_tonemap = TonemapNode::Create(*this);
        if (!m_tonemap)
            return false;   // already logged
        // ---------------------------------------------------------------
        // THE TWO HUD NODES -- the host chrome's and the game's -- each built
        // ONLY when the caller asks for it. Either way the node declares
        // nothing unless the driver hands that frame draw data, so two runs
        // differ only in what the frame ASKS for: not asking for a HUD leaves
        // the rest of the frame byte for byte identical.
        //
        // ASKING IS NOT BOOKKEEPING (Task 9 fix round 1). Each node ADOPTS an
        // ImGui context, and its Release walks THAT context's platform texture
        // list disowning every RefCount==1 ImTextureData. Two of them over ONE
        // context is one backend disowning the other's LIVE atlas.
        // `hostHud` was unconditional until fix round 1, which is precisely how
        // a second backend landed on the editor context: the editor's OFFSCREEN
        // viewport context built a host-HUD node it can never draw through,
        // and -- created while the editor context was current -- adopted it.
        // See NodeSet's invariant block.
        // ---------------------------------------------------------------
        const RenderOverlayHooks overlay = GetRenderOverlayHooks();
        if ((nodes.hostHud || nodes.gameUi) && !overlay.create)
        {
            static bool warned = false;
            if (!warned) { warned = true; ARC_WARN("[nri-graph] UI node requested without a render overlay hook"); }
        }
        if (nodes.hostHud && overlay.create)
        {
            m_imguiHud = overlay.create(*this);
            if (!m_imguiHud)
                return false;   // already logged
        }
        // The GAME node's caller owes it one AdoptImGuiContext call. The host
        // HUD's needs none only because a host builds its vehicle with its own
        // ImGui context current, which is what ImGuiNri::Init records.
        if (nodes.gameUi && overlay.create)
        {
            m_imguiGame = overlay.create(*this);
            if (!m_imguiGame)
                return false;   // already logged
        }

        // ---------------------------------------------------------------
        // The pick + outline pair is built ONLY when something asks for it, and
        // that gate is not laziness -- it is what makes "not asking costs
        // nothing" structural. A run that does not ask creates NO readback
        // buffer, NO descriptor pool, NO pipeline layout and NO arena for this
        // chain, so nothing about it can perturb the frame. Asked for, it is
        // built EAGERLY like the other three: a node
        // that cannot be created must fail the vehicle at boot rather than
        // render a probe frame that silently draws nothing.
        //
        // TWO THINGS CAN ASK, and they ask different questions (Task 9).
        // `--pick-probe` arms a FIXED probe pixel and is a dev flag;
        // NodeSet::pickOutline says only "this host will drive the chain per
        // frame", which is what the editor's viewport does -- its probe pixel
        // is a click that has not happened at Create time and its hover cursor
        // moves every frame. m_pickArmed therefore keeps its NARROWER meaning
        // (the FLAG), because the out-of-range latch, ProbeX/ProbeY and the log
        // line below all describe the flag rather than the nodes.
        // ---------------------------------------------------------------
#if !defined(ARC_BUILD_DIST)
        // ...AND ONLY A HOST-WINDOW CONTEXT MAY INHERIT THE FLAG (whole-branch
        // review, I4's ledgered half). --pick-probe is a RUNTIME desk item: one
        // fixed canvas pixel, reported once as an exit code. The editor's
        // OFFSCREEN viewport context is the other consumer of these nodes, and
        // its probe pixel is a CLICK -- driven per frame through
        // FrameDesc::pickPixel/pickTicket, which needs no arming at all. Left
        // inherited, an `ArcaneEditor --nri-graph --pick-probe x,y --frames N`
        // run latched the flag on the viewport context too, and
        // RenderFrameOffscreen's out-of-range check then measured the RUNTIME
        // coordinate against the PANEL's extent -- a coordinate nobody chose
        // for that surface. m_probeOutOfRange is a PERMANENT latch that
        // ProbeId()/ProbeResult() refuse on, so one trip silently killed editor
        // click-pick for the rest of the session while everything else kept
        // working.
        m_pickArmed = config.pickProbe && !IsOffscreen();
        m_probeX    = config.pickProbeX;
        m_probeY    = config.pickProbeY;
#endif
        // The frame's default probe/hover, before any frame has named its own.
        // Set unconditionally so a Dist build (where the flag does not exist)
        // still has defined values rather than a coordinate nobody wrote.
        m_currentPick  = glm::ivec2(m_probeX, m_probeY);
        m_currentHover = m_currentPick;

        if (m_pickArmed || nodes.pickOutline)
        {
            // The Restart pair (render.outline.maxThicknessPx / .supersample)
            // latches HERE, at node creation, for the process: the JFA
            // schedule and the id target's extent never change under a graph.
            (void)OutlineMaxThicknessPx();
            (void)PickSupersample();
            m_pick = PickNode::Create(*this);
            if (!m_pick)
                return false;   // already logged
            m_outline = OutlineNode::Create(*this);
            if (!m_outline)
                return false;   // already logged
        }
        if (m_pickArmed)
        {
            ARC_INFO("[nri-graph] --pick-probe armed at canvas pixel ({}, {}): the frame carries the "
                     "entity-id pass, its readback and a {}-step JFA outline; the id lands after "
                     "{} presented frames",
                     m_probeX, m_probeY,
                     // The CLAMPED count, i.e. what the declarator will actually
                     // declare -- a log line that disagreed with the frame would
                     // be worse than no log line. Thickness-derived and
                     // therefore surface-independent (D3c), which is exactly why
                     // it can be stated once here at boot.
                     std::min(OutlineJfaStepCount(OutlineMaxThicknessPx()),
                              OutlineNode::kMaxJfaSteps),
                     FramesInFlight());
        }
        else if (nodes.pickOutline)
        {
            ARC_INFO("[nri-graph] pick + outline nodes built for a per-frame driver: the frame "
                     "carries the entity-id pass, its readback and up to a {}-step JFA outline "
                     "whenever it asks; a readback lands {} rendered frames after the frame that "
                     "armed it",
                     std::min(OutlineJfaStepCount(OutlineMaxThicknessPx()),
                              OutlineNode::kMaxJfaSteps),
                     FramesInFlight());
        }

        return true;
    }

    std::uint32_t NriGraphContext::SurfaceWidth() const noexcept
    {
        return m_mode == Mode::Offscreen ? m_offscreenWidth : (m_swap ? m_swap->Width() : 0);
    }

    std::uint32_t NriGraphContext::SurfaceHeight() const noexcept
    {
        return m_mode == Mode::Offscreen ? m_offscreenHeight : (m_swap ? m_swap->Height() : 0);
    }

    std::uint64_t NriGraphContext::OffscreenTextureId() const noexcept
    {
        // THE ImGuiNri CONVENTION (that header's §7.3): the RAW texture handle
        // through intptr_t -- not a descriptor. 0 is ImTextureID_Invalid, which
        // is what a host-window context correctly reports.
        return (std::uint64_t)(std::intptr_t)m_offscreen;
    }

    std::span<const std::uint8_t> NriGraphContext::ShaderBytecode(const char* name)
    {
        if (!name || m_shaderDir.empty())
            return {};

        const auto cached = m_shaderBins.find(name);
        if (cached != m_shaderBins.end())
            return std::span<const std::uint8_t>(cached->second);

        const std::filesystem::path path = m_shaderDir / (std::string(name) + ".bin");
        const auto raw = Arcane::ReadRegularFile(path);
        if (!raw)
        {
            ARC_ERROR("[nri-graph] shader artifact missing/unreadable: {}", path.string());
            return {};
        }
        const auto* first = reinterpret_cast<const std::uint8_t*>(raw->data());
        std::vector<std::uint8_t> bytes(first, first + raw->size());
        if (bytes.empty())
        {
            ARC_ERROR("[nri-graph] shader artifact could not be read: {}", path.string());
            return {};
        }

        // Cached by VALUE in a node-based container: the node authors hold
        // spans into these vectors for their whole lifetime, and
        // NriPipelineCache dereferences the blob after the fill callback
        // returns (its fill contract, rule 2).
        const auto inserted = m_shaderBins.emplace(name, std::move(bytes));
        return std::span<const std::uint8_t>(inserted.first->second);
    }

    std::uint32_t NriGraphContext::ResolveMeshAlbedoSlot(const Guid& id)
    {
        // See this method's own doc comment (NriGraphContext.hpp) for the
        // full contract -- what each refusal means and why, the memoization
        // key, and the PendingCook gap left for Task 12.
        if (!id.IsValid() || !m_mesh || !m_textures || !m_device)
            return BindlessTable::kInvalidSlot;

        const auto cached = m_meshAlbedoSlots.find(id);
        if (cached != m_meshAlbedoSlots.end())
            return cached->second;

        // Srgb, the scene default (NriTextureCache::ColorSpace) -- a mesh
        // albedo is scene content like a sprite's own texture, never chrome.
        nri::Texture* texture = m_textures->Resolve(id);
        if (!texture)
            return BindlessTable::kInvalidSlot;   // not resident -- NriTextureCache already
                                                   // warned, once (its own one-shot latch)

        // A SEPARATE, INDEPENDENTLY-OWNED view over the SAME texture --
        // deliberately NOT m_textures->View(id). AddMaterial's own
        // OWNERSHIP contract (MeshNode.hpp) takes the descriptor it is
        // handed and destroys it at BindlessTable::Release; NriTextureCache
        // ALREADY owns and destroys ITS OWN view of this texture (any
        // sprite/material param naming the same asset reads that one). Two
        // owners destroying the SAME nri::Descriptor* is a double-destroy
        // -- caught the hard way once already (a D3D12-only SIGSEGV at this
        // vehicle's teardown, desk-verified before this comment was
        // written) -- so this mints its own SRV instead: multiple views
        // over one resource is ordinary and legal on both backends, and it
        // is the only shape that keeps BOTH caches' "I own what I created"
        // promise true at once.
        const nri::CoreInterface& core = m_device->Core();
        const nri::TextureDesc& textureDesc = core.GetTextureDesc(*texture);

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = textureDesc.mipNum;
        viewDesc.layerNum = textureDesc.layerNum;
        nri::Descriptor* bindlessView = nullptr;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, bindlessView)) || !bindlessView)
            return BindlessTable::kInvalidSlot;   // already logged (ARC_NRI_CHECK)

        const std::uint32_t slot = m_mesh->AddMaterial(bindlessView);
        if (slot == BindlessTable::kInvalidSlot)
        {
            // AddMaterial/BindlessTable refused (a null srv cannot happen
            // here -- CreateTextureView succeeded above -- so this is "the
            // table is already full", already warned once by BindlessTable
            // itself). The view just minted is now ownerless: nothing will
            // ever bury it via a graph teardown, because it was never
            // handed to anything that tracks it. Safe to destroy directly,
            // with no fence/graveyard involved: Add() refused BEFORE ever
            // writing it into the bindless descriptor set (BindlessTable::
            // Add's own contract -- a refusal consumes no slot), so no
            // command buffer can ever have read it.
            core.DestroyDescriptor(bindlessView);
            return BindlessTable::kInvalidSlot;
        }

        // ONLY a successful resolve is memoized -- a miss (not yet resident,
        // refused, or a full table) is NOT cached here, so a later frame's
        // ask for the same Guid tries again for free (NriTextureCache's own
        // memoization already makes a repeat Resolve() on a known-failed or
        // still-PendingCook key an O(1) map lookup, not a re-decode).
        m_meshAlbedoSlots.emplace(id, slot);
        return slot;
    }

    NriGraphContext::~NriGraphContext()
    {
        // BEFORE the early-out below and before ANY member is destroyed: the
        // arming installs process-wide slots that point at a backend built
        // over m_device, and a slot outliving what it names is the dangling-
        // registration hazard every other owner in this tree guards against.
        // Disarm() is conditional-and-idempotent (it clears only what IT
        // installed, and no-ops when Arm no-oped -- which since Task 6 is only
        // the case when some other owner armed first). It stays FIRST in this
        // body, before the borrowed window's owner can possibly run its own
        // teardown, for the same reason.
        //
        // GATED ON m_armedDiagnostics SINCE TASK 7, and the gate is what makes
        // a second context on one device safe: NriDiagnostics holds ONE
        // process-wide slot with no owner identity, so an unconditional Disarm
        // from an offscreen context would clear the chain the HOST-WINDOW
        // context armed and still owns. It changes nothing for a host-window
        // context in any existing topology -- the flag is exactly Arm()'s own
        // return value, and Disarm() already no-oped whenever that was false.
        if (m_armedDiagnostics)
            NriDiagnostics::Disarm();

        if (!m_device)
            return;   // Init() failed before the device existed; nothing was created on one

        const nri::CoreInterface& core = m_device->Core();

        // The last submit may still be in flight. Idle before releasing
        // anything it referenced -- ~NriDevice idles again before draining,
        // which is harmless.
        //
        // SKIPPED ON A LOST DEVICE. Once the loss
        // has been observed this call cannot idle anything that is not already
        // stopped; all it can do is fail. On VK it returns DEVICE_LOST
        // immediately; on D3D12 it burns NRI_TIMEOUT_FENCE (5 s, SharedExternal
        // .h:53) inside its scratch fence and then reports SUCCESS anyway. Both
        // outcomes cost the crash teardown time and one more entry on the error
        // latch and buy nothing. The healthy path is unchanged -- every
        // ordinary shutdown has GpuDeviceLostObserved() == false.
        if (core.DeviceWaitIdle && !GpuDeviceLostObserved())
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        // ONE fence value for every burial below, and it is the graph's own
        // last submitted value -- the same one RenderGraph::
        // ReleaseGpuResources and ~RenderGraph bury at. That is what keeps
        // Graveyard's NONDECREASING rule satisfied: burying everything at a
        // fixed low sentinel (fence 0, after an idle) would violate it here,
        // because the graph has been burying at m_submitValue all run.
        const std::uint64_t fence = m_graph ? m_graph->DebugSubmitCount() : 0;

        // ==============================================================
        // WHICH GRAVEYARD -- THIS CONTEXT'S OWN, ALWAYS AND ONLY.
        // ==============================================================
        // ONE MECHANISM, NO SPECIAL CASES. m_graves is a member of this
        // object, it starts empty, only this context's graph/nodes/caches bury
        // into it, and only this context's graph reaps it -- with the one
        // fence that describes those burials. That alone closes every window:
        //
        //   (b1) EXECUTE NEVER ENTERED -- burials key at fence 0, and 0 is
        //        nondecreasing against an EMPTY lane. (RenderGraph in fact
        //        buries nothing at all: it gates on the device it latches at
        //        Execute's entry.) No branch needed.
        //   (b2) EXECUTE ENTERED, NEVER SUCCEEDED -- RenderGraph latches the
        //        LANE beside the device, at that same entry and before anything
        //        fallible, so EnsureExecutionResources's all-or-nothing cleanup
        //        (which buries SYNCHRONOUSLY inside the failing Execute, at
        //        m_submitValue == 0, before any destructor runs) lands here too.
        //   (a)  ~RenderGraph's TAIL -- the command buffers, the allocators and
        //        the submission fence. A member destructor would run AFTER this
        //        body, i.e. after the drain below, leaving them pending in a
        //        lane nothing revisits. m_graph is therefore destroyed
        //        EXPLICITLY below, inside this body, ahead of that drain.
        //
        // HOST-WINDOW MODE IS NOT AN EXCEPTION, deliberately: both modes
        // bury the same objects, at the same fence values, in the same order,
        // into this same lane.
        Graveyard& graves = m_graves;

        if (m_capture)
        {
            const nri::CoreInterface* graveCore = &core;
            graves.Bury(fence, [graveCore, b = m_capture] { graveCore->DestroyBuffer(b); });
            m_capture = nullptr;
        }
#if !defined(ARC_BUILD_DIST)
        // The Servitor capture ring's staging buffer, at the same fence. No
        // retire: the device may be lost here, and a capture still waiting on
        // this context is answered capture.no-frame by its host hook.
        ReleaseTicketRing(graves, fence, false);
#endif

        // The nodes' own objects go out at the same fence, and BEFORE the
        // graph releases its transient pool below -- the tonemap's source view
        // names a POOL texture, so a view buried after the texture it views
        // would reach DestroyDescriptor over freed memory (the same class of
        // teardown bug as fix round 1's imported-view ordering). The outline
        // node holds the same class of view (over the id target and the JFA
        // ping-pong) and goes out for the same reason.
        if (m_imguiGame)
            m_imguiGame->Release(graves, fence);
        if (m_imguiHud)
            m_imguiHud->Release(graves, fence);
        if (m_outline)
            m_outline->Release(graves, fence);
        if (m_pick)
            m_pick->Release(graves, fence);
        if (m_tonemap)
            m_tonemap->Release(graves, fence);
        if (m_post)
            m_post->Release(graves, fence);
        if (m_overlay)
            m_overlay->Release(graves, fence);
        if (m_grid)
            m_grid->Release(graves, fence);
        if (m_meshCull)
            m_meshCull->Release(graves, fence);
        if (m_mesh)
            m_mesh->Release(graves, fence);
        if (m_batch2D)
            m_batch2D->Release(graves, fence);

        // AFTER every node: their descriptor sets name this cache's views, and
        // burying a view before the set that reads it is destroyed is the same
        // ordering hazard the node releases above exist to avoid.
        if (m_textures)
            m_textures->Release(graves, fence);
        if (m_meshBuffers)
            m_meshBuffers->Release(graves, fence);
        if (m_scene)
            m_scene->Release(graves, fence);

        // The sanctioned cache release (see NriPipelineCache.hpp): explicit,
        // at a fence the caller knows, rather than the destructor's direct-
        // destroy safety net.
        m_pipelines.Clear(graves, fence);

        // DESTROYED HERE, EXPLICITLY, not left to the member destructor --
        // which is the closure of header window (a).
        //
        // ~RenderGraph buries EVERYTHING it owns at its own last submitted
        // fence value: the transient pool and every cached attachment view
        // (including the IMPORTED ones, which name this frame's swapchain
        // backbuffers -- or, offscreen, the output texture below), AND the
        // per-frame-slot command buffers, their allocators and the submission
        // fence. A MEMBER destructor runs after this BODY, i.e. after the drain
        // below, so that tail would sit pending in a lane nothing revisits: in
        // Debug ~Graveyard makes that fatal, and in Release it runs those
        // thunks from a destructor with no fence-completion guarantee.
        //
        // Doing it here puts the whole graph in ONE lane and ONE sweep,
        // behind the DeviceWaitIdle above and with the swapchain still
        // alive.
        m_graph.reset();

        // ---- offscreen mode's own two objects (Task 7) -------------------
        // STRICTLY AFTER the release above, because Graveyard::Drain runs
        // thunks in BURIAL ORDER: the graph's imported COLOR_ATTACHMENT view
        // names this exact texture, so burying the texture first would destroy
        // it and then destroy a view over freed memory -- precisely the
        // teardown bug fix round 1 closed for the swapchain's backbuffers.
        //
        // The swapchain half gets this ordering from OUTER ownership (the host
        // window, and NRI's swapchain textures with it, outlive this object).
        // An offscreen context has no outer owner, so the order is explicit
        // here instead.
        //
        // The pacing fence goes out beside it: nothing else names it, and the
        // idle at the top of this body means nothing is still waiting on it.
        if (m_offscreen)
        {
            const nri::CoreInterface* graveCore = &core;
            graves.Bury(fence, [graveCore, t = m_offscreen] { graveCore->DestroyTexture(t); });
            m_offscreen = nullptr;
        }
        if (m_offscreenFence)
        {
            const nri::CoreInterface* graveCore = &core;
            graves.Bury(fence, [graveCore, f = m_offscreenFence] { graveCore->DestroyFence(f); });
            m_offscreenFence = nullptr;
        }

        // ...and RUNS every burial above HERE rather than leaving it pending.
        // Load-bearing, and Vulkan-only in its consequences (fix round 1,
        // finding 1):
        //
        // A pending view must not outlive the image it views, and this class's
        // member order destroys the SWAPCHAIN -- backbuffer IMAGES included --
        // a few lines after this body returns. A view left buried would then
        // reach DestroyDescriptor with its VkImage already gone: "a VkImageView
        // outliving its VkSwapchainKHR is a validation error". On a vehicle
        // whose entire exit-code contract is "the latch did not grow", that is
        // not an ordering nit -- it is a guaranteed nonzero exit on vulkan.
        //
        // Draining here fixes it structurally: a destructor BODY runs before
        // any member is destroyed, so the swapchain and its images are still
        // alive at this line. Graveyard::Drain's precondition -- the caller has
        // already made the GPU idle -- is satisfied by the DeviceWaitIdle at
        // the top of this function.
        //
        // THIS SWEEP IS NOW COMPLETE, which it was not before Task 8-pre.
        // Everything this context ever created goes out in it: the capture
        // buffer, the nodes' objects, the texture cache, the pipeline cache,
        // the graph's pool and views AND -- because m_graph was reset above
        // rather than left to its member destructor -- the graph's command
        // buffers, allocators and submission fence, plus the offscreen output
        // and pacing fence just buried. Nothing of this context's is pending
        // anywhere when this line returns, which is what ~Graveyard asserts a
        // few members later.
        //
        // OFFSCREEN MODE NEEDED THAT COMPLETENESS MOST, because it has no
        // ~NriDevice to fall back on: the device is BORROWED and outlives this
        // object. Before the lane, an offscreen context's leftovers sat in a
        // LIVE device's shared graveyard naming objects nothing else
        // remembered; now there is no shared structure for them to sit in.
        graves.Drain();

        // The number to size render.uploadRingBytesPerFrame from once a real frame
        // has run -- the peak across every slot, not slot 0's.
        std::uint64_t ringPeak = 0;
        for (std::uint32_t slot = 0; slot < FramesInFlight(); ++slot)
            ringPeak = ringPeak < m_ring.HighWater(slot) ? m_ring.HighWater(slot) : ringPeak;
        // Two lines rather than one composed one, because the host-window
        // wording is what a desk log is read against (D3b) and must not move.
        if (m_mode == Mode::Offscreen)
        {
            ARC_INFO("[nri-graph] offscreen graph render half shut down after {} rendered frame(s); "
                     "upload-ring peak {} of {} bytes per slot", m_frameIndex, ringPeak,
                     m_uploadRingBytes);
        }
        else
        {
            ARC_INFO("[nri-graph] graph render half shut down after {} presented frame(s); upload-ring "
                     "peak {} of {} bytes per slot", m_frameIndex, ringPeak, m_uploadRingBytes);
        }
        // NOTHING releases the window here, and nothing may start to: it is the
        // host's (THE BORROWED WINDOW). Every NRI object that named its HWND or
        // its VkSurfaceKHR -- the swapchain above all -- is destroyed by the
        // member destructors that run right after this body, which is strictly
        // before the host destroys the window it borrowed.
        //
        // NOTHING RELEASES THE DEVICE ON THE OFFSCREEN PATH EITHER, and for the
        // same shape of reason: m_ownedDevice is empty there, so the member
        // destructors leave the shared NriDevice (and the NativeDeviceOwner
        // behind it) entirely to the context that created them.
    }

    void NriGraphContext::Resize(std::uint32_t width, std::uint32_t height)
    {
        // Host-window only. An offscreen context has no swapchain to resize and
        // its twin is ResizeOffscreen; falling through here would silently do
        // nothing and leave a viewport rendering at the old extent, so the
        // no-swapchain early-out below covers it -- loudly, once, rather than
        // by accident.
        if (!m_swap)
        {
            if (m_mode == Mode::Offscreen)
                ARC_ERROR("[nri-graph] Resize() on an OFFSCREEN context -- call ResizeOffscreen()");
            return;
        }

        // ==============================================================
        // EVERYTHING THAT NAMES A BACKBUFFER DIES BEFORE THE BACKBUFFERS DO.
        // ==============================================================
        // NriSwapChain::Resize destroys and recreates every swapchain texture
        // (its header: destroy+recreate is the only path NRI offers). The
        // graph holds a COLOR_ATTACHMENT view over the currently-acquired
        // backbuffer from the last Execute -- it turns those over per frame
        // (RenderGraph::ReleaseImportedViews) but by BURYING them, so at this
        // instant they are still live descriptors naming images that are about
        // to be freed. Letting the resize proceed would leave them to be
        // destroyed later against a dead VkImage -- the same validation error
        // as the teardown case in ~NriGraphContext, and the one D2's vulkan
        // drag-storm would hit on the FIRST drag (fix round 1, finding 1).
        //
        // Order below is the whole point, and each step needs the one above it:
        //   1. idle -- Graveyard::Drain's stated precondition, and NriSwapChain
        //      does its own QueueWaitIdle only LATER, inside Resize();
        //   2. drop the capture staging buffer, whose size is the old extent;
        //   3. release the pool + every view (imported ones included);
        //   4. DRAIN, so 2 and 3 actually execute while the images still exist;
        //   5. only now, resize.
        // Releasing the transient pool here is not collateral damage: a pool
        // slot's desc is extent-derived, so RealizePool would re-create it on
        // the next frame anyway (and Phase 2's frame has no transients at all).
        // A resize is a rare event; a dangling descriptor is not a rare bug.
        const nri::CoreInterface& core = m_device->Core();
        if (core.DeviceWaitIdle)
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        // THIS CONTEXT'S LANE, never the device's (Task 8-pre): everything
        // buried below belongs to this context and is keyed to its graph's
        // fence, and the Drain a few lines on must not sweep another context's
        // pending burials.
        Graveyard& graves = m_graves;
        if (m_capture)
        {
            const nri::CoreInterface* graveCore = &core;
            graves.Bury(m_graph ? m_graph->DebugSubmitCount() : 0,
                        [graveCore, b = m_capture] { graveCore->DestroyBuffer(b); });
            m_capture = nullptr;
        }
        m_captureRecorded = false;
#if !defined(ARC_BUILD_DIST)
        // The capture ring's buffer is extent-sized too. The idle above means
        // a pending slot's copy has landed, so it is retired (kept for its
        // ticket) before the buffer goes: a resize mid-capture still answers.
        ReleaseTicketRing(graves, m_graph ? m_graph->DebugSubmitCount() : 0, true);
#endif

        // Both fullscreen nodes hold SHADER_RESOURCE views over pool textures
        // -- the tonemap over whatever it samples, the chain over the canvas
        // and its own targets -- and ReleaseGpuResources below is about to
        // bury every one of them, at addresses NRI is free to hand to their
        // replacements. Drop them here, in the same order and for the same
        // reason the graph turns its own imported views over: a descriptor
        // that outlives the resource it views is a use-after-free a pointer
        // comparison reports as a cache hit.
        //
        // NOT the only path that destroys pool textures -- RealizePool does
        // too, on a shrink or a desc change, from inside Execute(). That one
        // the nodes catch themselves through RenderGraph::PoolEpoch, because
        // it happens where no owner can sequence it. THIS path is invalidated
        // explicitly instead because it is followed by a graveyard DRAIN and
        // possibly by teardown: there may be no later Record() to notice the
        // epoch at all. (FullscreenNodes.hpp, SOURCE VIEWS AND THE POOL.)
        const std::uint64_t viewFence = m_graph ? m_graph->DebugSubmitCount() : 0;
        if (m_tonemap)
            m_tonemap->InvalidateSource(graves, viewFence);
        if (m_post)
            m_post->InvalidateSources(graves, viewFence);
        // Same reasoning, third holder of pool views: the outline node samples
        // the id target and the JFA ping-pong, all of them pool transients.
        if (m_outline)
            m_outline->InvalidateSources(graves, viewFence);

        if (m_graph)
            m_graph->ReleaseGpuResources();
        graves.Drain();

        // NriSwapChain's "no Resize between Acquire and Present" caller
        // contract is satisfied structurally here: the graph's acquire/present
        // pair lives entirely inside one Execute(), and this is only ever
        // called between RenderFrame() calls (header: Resize's contract).
        m_swap->Resize(width, height);

        const nri::Format resolved = m_swap->Format();
        if (resolved != nri::Format::UNKNOWN && resolved != m_format)
        {
            // Nothing to DO here, and that is by construction rather than by
            // luck: the attachment format is part of NriPipelineCache's
            // GraphicsKey, so the tonemap's next GetGraphics is a cache MISS
            // and creates a pipeline for the new format -- a stale PSO can
            // never be served. The old entries are released with the rest of
            // the cache at teardown. Still WARNed, because a swapchain that
            // changes channel order mid-run is worth knowing about when a
            // capture suddenly looks wrong-hued.
            ARC_WARN("[nri-graph] swapchain format changed across a resize (was {}, now {}) -- "
                     "format-keyed pipelines will be rebuilt on the next frame", (int)m_format,
                     (int)resolved);
            m_format = resolved;
        }
    }

    void NriGraphContext::ResizeOffscreen(std::uint32_t width, std::uint32_t height)
    {
        if (m_mode != Mode::Offscreen)
        {
            ARC_ERROR("[nri-graph] ResizeOffscreen() on a HOST-WINDOW context -- call Resize()");
            return;
        }
        if (width == 0 || height == 0)
        {
            // A panel collapsed to nothing. Unlike a minimised window there is
            // no state to fall into and nothing to acquire, so the existing
            // output is simply KEPT: the caller stops drawing the panel, and
            // the next non-zero size resizes for real. Recreating at 0x0 would
            // mean refusing a texture (nri::Dim_t cannot express it) and losing
            // the one the ImTextureID still names.
            ARC_WARN("[nri-graph] ResizeOffscreen({}, {}) ignored -- the current {}x{} output is "
                     "kept until a non-zero size arrives", width, height,
                     m_offscreenWidth, m_offscreenHeight);
            return;
        }
        if (width == m_offscreenWidth && height == m_offscreenHeight)
            return;   // no-op, matching NriSwapChain::Resize's unchanged-size contract

        // ==============================================================
        // THE SAME FIVE STEPS Resize() PERFORMS, and each still needs the one
        // above it -- see that function for the full argument. What is being
        // protected here is the OUTPUT TEXTURE rather than the swapchain's
        // backbuffers, but the hazard is identical: the graph holds a
        // COLOR_ATTACHMENT view over it from the last Execute and turns those
        // over by BURYING them, so at this instant they are live descriptors
        // naming an image about to be freed.
        //
        //   1. idle -- Graveyard::Drain's stated precondition;
        //   2. drop the capture staging buffer, whose size is the old extent;
        //   3. invalidate the nodes' cached pool views (PoolEpoch discipline
        //      unchanged: this path is invalidated EXPLICITLY because it is
        //      followed by a drain and there may be no later Record to notice
        //      the epoch), then release the pool + every view;
        //   4. DRAIN, so 2 and 3 execute while the output still exists;
        //   5. only now destroy the output and create its replacement.
        // ==============================================================
        const nri::CoreInterface& core = m_device->Core();
        if (core.DeviceWaitIdle)
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        // THIS CONTEXT'S LANE (Task 8-pre) -- see Resize() above. It matters
        // more here than there: an offscreen context is by construction the
        // SECOND one on a shared device, so the device's graveyard is exactly
        // where another context's un-reaped burials would be sitting.
        Graveyard& graves = m_graves;
        const std::uint64_t fence = m_graph ? m_graph->DebugSubmitCount() : 0;
        const nri::CoreInterface* graveCore = &core;

        if (m_capture)
        {
            graves.Bury(fence, [graveCore, b = m_capture] { graveCore->DestroyBuffer(b); });
            m_capture = nullptr;
        }
        m_captureRecorded = false;
#if !defined(ARC_BUILD_DIST)
        ReleaseTicketRing(graves, fence, true);   // see Resize(): idled above, so pending copies retire
#endif

        if (m_tonemap)
            m_tonemap->InvalidateSource(graves, fence);
        if (m_post)
            m_post->InvalidateSources(graves, fence);
        if (m_outline)
            m_outline->InvalidateSources(graves, fence);

        if (m_graph)
            m_graph->ReleaseGpuResources();

        // AFTER the release, because Drain runs thunks in BURIAL ORDER and the
        // release is what buries the imported view naming this texture -- the
        // identical ordering ~NriGraphContext uses, and the reason neither
        // destroys the output directly.
        //
        // m_meshBuffers is deliberately NOT released here -- resident meshes
        // are persistent, not pool tenants (the same reason m_textures is
        // kept across Resize). Re-uploading every mesh on a viewport drag
        // would reintroduce the ring cliff through a different door.
        if (m_offscreen)
        {
            graves.Bury(fence, [graveCore, t = m_offscreen] { graveCore->DestroyTexture(t); });
            m_offscreen = nullptr;
        }

        graves.Drain();

        const std::uint32_t oldWidth = m_offscreenWidth, oldHeight = m_offscreenHeight;
        if (!CreateOffscreenTarget(width, height))
        {
            // Already logged + latched. m_offscreen stays null and every later
            // RenderFrameOffscreen refuses rather than declaring a frame whose
            // final target does not exist -- and OffscreenTextureId() reports
            // ImTextureID_Invalid, so a viewport draws nothing instead of
            // sampling a destroyed texture.
            m_offscreenWidth = m_offscreenHeight = 0;
            ARC_ERROR("[nri-graph] offscreen resize {}x{} -> {}x{} failed; this context can no "
                      "longer render", oldWidth, oldHeight, width, height);
            return;
        }

        // NOTHING to do about the format: it is a constant here, not something
        // a driver resolves (kGraphOffscreenFormat), so the tonemap's
        // format-keyed pipeline stays a cache HIT across every resize.
    }

    bool NriGraphContext::EnsureCaptureBuffer()
    {
        const std::uint32_t width  = SurfaceWidth();
        const std::uint32_t height = SurfaceHeight();
        if (width == 0 || height == 0)
        {
            ARC_WARN("[nri-graph] capture requested on a zero-sized surface");
            return false;
        }
        if (m_capture && m_captureWidth == width && m_captureHeight == height)
            return true;

        const nri::CoreInterface& core = m_device->Core();

        // Unreachable today -- Resize() is the only thing that can change the
        // swapchain extent and it already drops the staging buffer -- but a
        // silent leak is the wrong failure mode for "unreachable", and the
        // burial costs one line.
        if (m_capture)
        {
            const nri::CoreInterface* graveCore = &core;
            m_graves.Bury(m_graph ? m_graph->DebugSubmitCount() : 0,
                          [graveCore, b = m_capture] { graveCore->DestroyBuffer(b); });
            m_capture         = nullptr;
            m_captureRecorded = false;
        }

        // Both pitches carry NRI's documented alignment rules
        // (TextureDataLayoutDesc: rowPitch a multiple of
        // uploadBufferTextureRow, slicePitch of uploadBufferTextureSlice), and
        // the buffer is sized to the ALIGNED slice so the aligned values are
        // legal to hand to CmdReadbackTextureToBuffer as well.
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());
        m_captureWidth      = width;
        m_captureHeight     = height;
        m_captureRowPitch   = AlignUp(std::uint64_t(width) * 4,
                                       deviceDesc.memoryAlignment.uploadBufferTextureRow);
        m_captureSlicePitch = AlignUp(m_captureRowPitch * height,
                                       deviceDesc.memoryAlignment.uploadBufferTextureSlice);

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = m_captureSlicePitch;
        bufferDesc.usage = nri::BufferUsageBits::NONE;   // copy destination only, like Readback.cpp's
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_READBACK,
                                                       0.0f, bufferDesc, m_capture))
            || !m_capture)
        {
            ARC_ERROR("[nri-graph] capture staging buffer creation failed -- no capture this run");
            m_capture = nullptr;
            return false;
        }
        core.SetDebugName(m_capture, "nri-graph capture");
        return true;
    }

    RgFrameHandles DeclareGraphFrame(RenderGraph& graph, const RgFrameShape& shape,
                                      NriGraphContext* context)
    {
        // ---------------------------------------------------------------
        // THE CLEAR IS DECLARATIVE (F5 plan 1, Task 2; it replaced Task 7's
        // clear-in-the-exec-fn seam). A node that wants a cleared attachment
        // declares a load op beside the attachment
        // (RenderGraph::SetColorAttachmentOps / SetDepthAttachmentOp) and the
        // executor lowers it to nri::LoadOp::CLEAR at CmdBeginRendering. An
        // attachment with no declared op LOADs; the store op is always STORE.
        //   - batch2d (and, on a chrome frame, letterbox) CLEARs the canvas to
        //     render.clearColor. The canvas is a TRANSIENT whose pool slot the
        //     graph hands over with a contents-discarding barrier, so its
        //     LOADed contents would be undefined.
        //   - mesh LOADs the canvas (it carries batch2d's content) and CLEARs
        //     its fresh depth transient to DepthClearValue(
        //     kSelectedDepthConvention).
        //   - The BACKBUFFER needs no clear: the tonemap covers it with a
        //     fullscreen triangle, and neither does a POST-CHAIN target, for
        //     the same reason.
        //   - Pick keeps its own exec-fn clear (PickOutlineNodes.cpp).
        //
        // WORLD vs CHROME. A world frame (shape.world, the default) declares
        // the empty D1 slots "gbuffer" and "lighting" ahead of batch2d and
        // "taa" just before the tonemap: compute nodes with no access and no
        // attachment, so Compile gives them no barrier and no pool slot. They
        // are not literally free on the command buffer: the executor wraps
        // every node in its NodeScope, so each records one empty
        // begin/end annotation pair (and a breadcrumb scope while a crash
        // backend is armed) -- three empty scopes in a PIX/RenderDoc capture,
        // no draw, dispatch or barrier. A chrome frame (world == false) declares none of the world nodes --
        // only "letterbox", which clears the canvas the tonemap samples.
        //
        // RING FOOTGUN, for whoever adds the next node here: the vehicle calls
        // ring.BeginFrame(slot) AFTER this function returns, so an allocation
        // made from a SETUP fn lands in the PREVIOUS frame's slot and is
        // overwritten while the GPU still reads it. Allocate from the ring only
        // inside exec fns (record time). Batch2DNode does.
        // ---------------------------------------------------------------
        RgFrameHandles handles;

        const auto addEmpty = [&](const char* name) {
            graph.AddNode(name, RenderGraph::NodeKind::Compute,
                [](RenderGraphBuilder&) {},
                [](RenderGraphNodeContext&) {});
        };

        if (shape.world)
        {
            addEmpty("gbuffer");
            addEmpty("lighting");
            handles.canvas = AddBatch2DNode(graph, context, shape.canvasWidth, shape.canvasHeight);
        }
        else
        {
            // THE CHROME LETTERBOX: AddBatch2DNode's setup minus the batch --
            // the SAME DeclareClearedCanvas call, so chrome and world can never
            // disagree on the canvas. The canvas is still minted (the tonemap
            // samples it) and cleared by its load op; the exec records nothing.
            // The attachment is required anyway: Compile refuses a Raster node
            // with no attachment.
            graph.AddNode("letterbox", RenderGraph::NodeKind::Raster,
                [&](RenderGraphBuilder& builder)
                {
                    handles.canvas = DeclareClearedCanvas(graph, builder, shape.canvasWidth, shape.canvasHeight);
                },
                [](RenderGraphNodeContext&) {});
        }

        // ---------------------------------------------------------------
        // THE MESH PASS (Task 4 declared its depth target; Task 7 is the pass
        // itself; F3 plan 2 widened it to opaque + masked + ordered
        // transparent). MeshNode CREATES the depth transient, Writes the canvas
        // as ColorWrite and the depth as DepthWrite, and attaches both -- all
        // in its own Setup, which is the same create-then-write-then-attach
        // shape AddBatch2DNode uses for the canvas one line above.
        //
        // POSITIONED AFTER batch2d, BEFORE the post chain and the tonemap.
        // After batch2d because the canvas is MINTED AND CLEARED there and this
        // pass draws on top of it (its colour load op LOADs; only its depth
        // load op CLEARs); before the tonemap because the
        // canvas is linear and the mesh pass writes linear colour.
        //
        // GATED ON A NON-EMPTY SCENE, and an empty span reads exactly like a
        // null pointer -- the same "slice the frame by nulling" rule
        // FrameDesc's own banner states. A frame that asks for no mesh pass is
        // byte for byte the frame this function built before Task 7 existed.
        // ---------------------------------------------------------------
        // Since F3 plan 1 T6 "non-empty" is MeshSceneDesc::Empty(): ad-hoc
        // instances OR a registry-backed scene with draws OR (T7, ruling R-D)
        // a registry-backed scene with STAGED rows and nothing emitted --
        // the upload still has to happen; the mesh node's pass then only
        // clears depth through its load op.
        const bool wantsMesh = shape.world && shape.mesh != nullptr && !shape.mesh->Empty();
        if (wantsMesh)
        {
            // GraphCanvasFormat() is passed EXPLICITLY because it is the format
            // AddBatch2DNode minted `handles.canvas` with, a few lines up.
            // AddMeshNode cannot read a handle's format back and must not
            // assume one -- see its header for what a wrong one costs.
            handles.depth = AddMeshNode(graph, context, handles.canvas, GraphCanvasFormat(),
                                         *shape.mesh, shape.canvasWidth, shape.canvasHeight);
        }

        // ---------------------------------------------------------------
        // THE DEPTH TRANSIENT ON ITS OWN (Task 4, Phase 4). Closes gap F:
        // RgUsage::DepthWrite, RenderGraph::SetDepthAttachment and the barrier
        // mapping (RenderGraph.cpp's StateFor) all landed in an earlier phase
        // with no production node ever calling any of them. This is that
        // resource -- NOT a consumer of it.
        //
        // SINCE TASK 7 IT IS THE `else` OF THE MESH BLOCK ABOVE, and reachable
        // only for `shape.depth` asked for with NO mesh scene -- the exact case
        // Task 4 shipped and pinned. Kept rather than deleted because that case
        // is a real, tested contract (a frame may want the resource declared
        // without a pass consuming it) and because the two branches being
        // mutually exclusive is what guarantees no graph ever carries two
        // CreateTexture("depth") calls. A frame carrying a mesh scene gets its
        // depth target from MeshNode and never reaches here.
        //
        // GraphDepthFormat(), canvas-sized, so it can pair with `handles.canvas`
        // as a Raster node's colour + depth attachments the moment something
        // wants to. Nothing on THIS branch is that something: no Read(), no
        // Write(), no SetDepthAttachment().
        //
        // CreateTexture() is only callable from inside a node's Setup (the
        // RenderGraphBuilder that exposes it is constructed by AddNode() and
        // by nothing else), so declaring the resource at all -- even
        // unconsumed -- needs a node. NodeKind::Compute, not Raster: this node
        // declares no attachment, and a Raster node with none would fail
        // NodeHasRequiredAttachments() at Compile().
        //
        // GATED LIKE EVERY OTHER OPTIONAL PIECE OF THIS FRAME: shape.depth ==
        // false adds NO node and creates NO resource, so "not asking costs
        // nothing" holds by construction -- a frame that never sets `depth`
        // is byte-for-byte the frame this function built before this block
        // existed (same node count, same node names, same pool slot count).
        // A frame that DOES ask for it still costs nothing at Execute() time:
        // an untouched transient gets no lifetime and no pool slot ("a
        // transient no node touches has no lifetime and no pool slot",
        // RenderGraph.hpp), so RealizePool never allocates it.
        // ---------------------------------------------------------------
        else if (shape.world && shape.depth)
        {
            graph.AddNode("depth", RenderGraph::NodeKind::Compute,
                [&handles, &shape](RenderGraphBuilder& builder)
                {
                    RgTextureDesc desc;
                    desc.format       = GraphDepthFormat();
                    desc.width        = shape.canvasWidth;
                    desc.height       = shape.canvasHeight;
                    desc.depthStencil = true;
                    handles.depth = builder.CreateTexture("depth", desc);
                },
                [](RenderGraphNodeContext&) {});
        }

        // THE 3D REFERENCE GRID is no longer a canvas node here (F5 plan 1
        // Task 3): it is drawn after the tonemap by the overlay pass below,
        // which SAMPLES the mesh depth instead of attaching it.

        // ---------------------------------------------------------------
        // THE POST CHAIN (Task 10). One node per chain pass, between the
        // canvas and the tonemap; the tonemap then samples the LAST pass's
        // target instead of the canvas. Everything about the ping-pong -- two
        // physical textures for N logical targets -- is the transient pool
        // allocator's doing, not a declaration here (FullscreenNodes.hpp, THE
        // PING-PONG IS DERIVED).
        //
        // THE CHAIN IS DROPPED when the caller passes no post desc, even if
        // the scene binds one -- "batcher + tonemap and nothing else" is
        // asked for by nulling `post`, not by a mode flag. The HUD node below
        // works the same way, off FrameDesc::imgui.
        // ---------------------------------------------------------------
        RgTexture sceneColor = handles.canvas;
        if (shape.world && shape.post && !shape.post->passes.empty())
        {
            // With a context the NODE decides: PrepareChain builds the
            // pipelines, the descriptor sets and the packed constants, and
            // returns 0 for a chain it cannot honour -- which this then
            // declares nothing for, rather than declaring passes whose exec fn
            // would leave their targets holding undefined pool contents.
            // Device-lessly it is the wiring alone, clamped to the cap a node
            // would latch (render.post.maxPasses).
            std::uint32_t passCount = (std::uint32_t)std::min<std::size_t>(
                shape.post->passes.size(), Settings<RenderPostSettings>().maxPasses);
            if (context)
            {
                PostChainNode* node = context->PostChain();
                passCount = node ? node->PrepareChain(*shape.post, context->CurrentGlobals(),
                                                       GraphCanvasFormat())
                                 : 0;
            }
            if (passCount > 0)
            {
                handles.post = AddPostChainNodes(graph, context, handles.canvas, *shape.post,
                                                  passCount, shape.canvasWidth, shape.canvasHeight);
                handles.postPassCount = passCount;
                sceneColor            = handles.post;
            }
        }

        // The empty TAA slot (D1), last of the world nodes, just ahead of the
        // tonemap. A chrome frame has no world and no TAA.
        if (shape.world)
            addEmpty("taa");

        // The frame's FINAL TARGET: the acquired backbuffer, or -- when the
        // shape carries one -- the vehicle's persistent offscreen output
        // (Task 7). Everything below writes `handles.backbuffer` and neither
        // knows nor cares which it got, which is what keeps the two modes one
        // frame shape rather than two.
        handles.backbuffer = AddTonemapNode(graph, context, sceneColor, shape.offscreenOutput);

        // ---------------------------------------------------------------
        // THE POST-TONEMAP OVERLAY PASS (F5 plan 1 Task 3, spec s3.3),
        // declared right AFTER the tonemap and BEFORE the game HUD: the 3D
        // grid, then depth-tested lines and fills (the 2D grid), then
        // always-on-top ones (physics debug, the camera rect, the gizmo),
        // each class in submission order, blended
        // straight-alpha onto the DISPLAY-REFERRED target -- so the post
        // chain never grades them. Linear colours run the tonemap operator in
        // overlay.hlsl; display-referred ones are written as given.
        //
        // DEPTH IS SAMPLED, NOT ATTACHED: with a mesh pass the node Reads that
        // pass's depth transient as ShaderRead (its lifetime then runs mesh ->
        // overlay); with none it Reads no depth at all -- in particular NOT the
        // placeholder `depth` node above, which stays declared-and-unconsumed.
        //
        // GATED ON CONTENT: a frame with no grid and an empty (or null) list
        // declares nothing, so the runtime's frame is unchanged.
        // ---------------------------------------------------------------
        // A bound packet owns the overlay. FrameDesc's list, grid and world
        // camera are the packet-less path (tests and any caller that does
        // not extract).
        const RenderFramePacket* packet = context ? context->Packet() : nullptr;
        bool wantOverlay = false;
        const OverlayPrimitiveList* overlayList = nullptr;
        const GridSceneDesc* overlayGrid = nullptr;
        const glm::mat4* worldVp = nullptr;
        const glm::mat4* meshVp = nullptr;
        glm::mat4 packetWorldVp{ 1.0f };
        glm::mat4 packetMeshVp{ 1.0f };
        glm::mat4 shapeMeshVp{ 1.0f };
        if (packet)
        {
            wantOverlay = !packet->overlay.prims.empty() || packet->overlay.hasGrid3d;
            overlayList = &packet->overlay;
            overlayGrid = packet->overlay.hasGrid3d ? &packet->overlay.grid3d : nullptr;
            packetWorldVp = packet->spriteView
                ? packet->spriteView->view.ViewProjection()
                : packet->viewProjection;
            worldVp = &packetWorldVp;
            if (packet->meshView)
            {
                packetMeshVp = packet->meshView->ViewProjection();
                meshVp = &packetMeshVp;
            }
            else
            {
                // No mesh view: a null mesh VP cannot compare equal, so
                // world-space primitives are not depth-tested.
                meshVp = nullptr;
            }
        }
        else
        {
            wantOverlay = (shape.grid != nullptr) ||
                (shape.overlay != nullptr &&
                 (!shape.overlay->prims.empty() || shape.overlay->hasGrid3d));
            overlayList = shape.overlay;
            overlayGrid = shape.grid;
            worldVp = shape.overlayWorldViewProjection;
            if (wantsMesh)
            {
                shapeMeshVp = shape.mesh->projection * shape.mesh->view;
                meshVp = &shapeMeshVp;
            }
        }
        if (shape.world && wantOverlay)
        {
            AddOverlayNode(graph, context, handles.backbuffer,
                           wantsMesh ? handles.depth : RgTexture{},
                           overlayGrid, overlayList, shape.canvasWidth, shape.canvasHeight,
                           worldVp, meshVp);
        }

        // ---------------------------------------------------------------
        // THE GAME HUD (Task 9), declared BETWEEN the tonemap and the
        // pick/outline chain -- which is the editor's own compositing order
        // expressed against this recorder: the editor's phase 11 (the game HUD)
        // runs after the scene render and BEFORE phase 12 (the selection
        // outline). Neither phase has a function of its own any more -- both are
        // armed into THIS frame by EditorApp::ArmGraphViewportFrame, so this
        // declaration order is now the only place it is stated. They are mutually
        // exclusive by MODE there (Play draws the HUD, Edit draws the outline),
        // so no frame exercises the order today -- which is exactly why it is
        // pinned by declaration rather than left to be discovered by whoever
        // first makes both true.
        //
        // AFTER the tonemap for the same reason the outline composite is: a HUD
        // is display-referred and must not be graded.
        //
        // GATED LIKE THE HOST HUD IMMEDIATELY BELOW, and for the same reason:
        // the game HUD is OVERLAY content. An overlay drawn on top of the scene
        // masks exactly the pixels a frame-to-frame comparison needs, so a
        // caller that asked for the scene without host chrome does not want the
        // plugin's chrome either.
        //
        // BEHAVIOUR-INERT ON THE RUNTIME: RuntimeFrame.cpp never sets
        // FrameDesc::gameUi (only EditorApp::ArmGraphViewportFrame does),
        // so this gate is reachable only through the editor. See
        // RgFrameShape::gameUi and FrameDesc::gameUi for the full account.
        // ---------------------------------------------------------------
        if (shape.gameUi)
            if (auto addNode = GetRenderOverlayHooks().addNode)
                addNode(graph, context, handles.backbuffer, true);

        // ---------------------------------------------------------------
        // THE PICK + OUTLINE CHAIN, declared BETWEEN the tonemap and
        // the capture, which is both halves of the ordering contract:
        //
        //   * AFTER the tonemap, because the composite blends over the
        //     DISPLAY-REFERRED backbuffer -- which is also where the editor's
        //     phase 12 composites: after the scene render, before the ImGui
        //     pass;
        //   * BEFORE the capture, because the capture node copies the
        //     backbuffer and must see the outline. Declaration order IS
        //     execution order on this graph, so this placement is the whole
        //     mechanism -- there is nothing else to get right.
        //
        // The pick pass itself does not depend on the tonemap at all and
        // could sit anywhere; keeping the chain contiguous here is what makes
        // "the flag off leaves the frame byte for byte" obvious by reading.
        // ---------------------------------------------------------------
        if (shape.pickOutline)
        {
            const RgPickHandles pick = AddPickNodes(graph, context,
                                                     shape.canvasWidth, shape.canvasHeight);
            handles.pickIds      = pick.ids;
            handles.pickDepth    = pick.depth;
            handles.pickReadback = pick.readback;
            handles.outlineField = AddOutlineNodes(graph, context, pick.ids,
                                                    shape.canvasWidth, shape.canvasHeight);
            handles.jfaStepCount = std::min(OutlineJfaStepCount(OutlineMaxThicknessPx()),
                                             OutlineNode::kMaxJfaSteps);
            AddOutlineCompositeNode(graph, context, handles.outlineField, handles.backbuffer,
                                     shape.canvasWidth, shape.canvasHeight);
        }

        // ---------------------------------------------------------------
        // THE HUD, the LAST visual writer of the frame -- after the tonemap
        // (host chrome is display-referred and must not be graded), after the
        // outline composite when a probe run armed one, and BEFORE the
        // capture, so a capture carries the chrome that was actually shown.
        // Declaration order is execution order here, so this placement is the
        // whole mechanism: pass:tone, then pass:imgui, then the readback.
        //
        // A CALLER THAT WANTS NO CHROME passes no draw data at all, and this
        // node is simply not declared -- there is no separate mode to keep in
        // step with the pointer.
        // ---------------------------------------------------------------
        if (shape.imgui)
            if (auto addNode = GetRenderOverlayHooks().addNode)
                addNode(graph, context, handles.backbuffer, false);

#if !defined(ARC_BUILD_DIST)
        // The Servitor capture ring's copy node (Task 27): the backbuffer, after
        // every node that writes it, into this frame's slot of the ring's
        // HOST_READBACK buffer. Declared beside (before) the --screenshot
        // `capture` node; both only READ the backbuffer, so their order is
        // free. The exec fn drains first and records second
        // (NriGraphContext::NoteRenderSubmission), the PickNode rule.
        if (shape.ticketCapture && shape.ticketCaptureBuffer)
        {
            auto ringHandle = std::make_shared<RgBuffer>();
            const RgTexture ringSource = handles.backbuffer;
            graph.AddNode("ticket-capture", RenderGraph::NodeKind::Copy,
                [shape, ringSource, ringHandle](RenderGraphBuilder& builder)
                {
                    *ringHandle = builder.ImportBuffer("ticket-capture", shape.ticketCaptureBuffer,
                                                        shape.ticketCaptureBytes);
                    builder.Read(ringSource, RgUsage::CopySrc);
                    builder.Write(*ringHandle, RgUsage::ReadbackHost);
                },
                [context, ringSource, ringHandle](RenderGraphNodeContext& ctx)
                {
                    if (!context)
                        return;   // device-less declaration-shape drive
                    nri::Texture* source = ctx.Resolve(ringSource);
                    nri::Buffer*  dest   = ctx.Resolve(*ringHandle);
                    if (!source || !dest)
                    {
                        GraphError("NriGraphContext: the ticket-capture node could not resolve its "
                                   "backbuffer or the capture ring's buffer");
                        return;
                    }
                    context->NoteRenderSubmission(ctx.core, ctx.cmd, *source, *dest);
                });
        }
#endif

        if (!shape.capture)
            return handles;

        // The capture node. A Copy node, so the executor opens no rendering
        // for it; the backbuffer's COLOR_ATTACHMENT -> COPY_SOURCE transition
        // and the final COPY_SOURCE -> PRESENT exit barrier are both DERIVED
        // from these declarations -- the graph path records no hand-written
        // barrier anywhere, which is the whole point of the port.
        //
        // The staging buffer is IMPORTED rather than created as a transient:
        // the graph realizes transients in MemoryLocation::DEVICE
        // (RealizePool), which can never be mapped. RgUsage::ReadbackHost is
        // the declaration that says "this buffer is the host's to read"; it
        // derives the same COPY_DESTINATION state CopyDst does, by design.
        //
        // Shared rather than captured by value for the same reason
        // AddTonemapNode's backbuffer is: the handle is minted inside this
        // node's own setup, which runs after both lambdas are constructed.
        auto captureHandle = std::make_shared<RgBuffer>();
        const RgTexture backbuffer = handles.backbuffer;
        graph.AddNode("capture", RenderGraph::NodeKind::Copy,
            [shape, backbuffer, captureHandle](RenderGraphBuilder& builder)
            {
                *captureHandle = builder.ImportBuffer("capture", shape.captureBuffer,
                                                       shape.captureBytes);
                builder.Read(backbuffer, RgUsage::CopySrc);
                builder.Write(*captureHandle, RgUsage::ReadbackHost);
            },
            [context, backbuffer, captureHandle](RenderGraphNodeContext& ctx)
            {
                if (!context)
                    return;   // device-less declaration-shape drive

                nri::Texture* source = ctx.Resolve(backbuffer);
                nri::Buffer*  dest   = ctx.Resolve(*captureHandle);
                if (!source || !dest)
                {
                    GraphError("NriGraphContext: the capture node could not resolve its backbuffer "
                               "or staging buffer");
                    return;
                }

                nri::TextureDataLayoutDesc layout = {};
                layout.rowPitch   = (std::uint32_t)context->CaptureRowPitch();
                layout.slicePitch = (std::uint32_t)context->CaptureSlicePitch();

                nri::TextureRegionDesc region = {};
                region.width  = (nri::Dim_t)context->CaptureWidth();
                region.height = (nri::Dim_t)context->CaptureHeight();
                region.depth  = 1;

                ctx.core.CmdReadbackTextureToBuffer(ctx.cmd, *dest, layout, *source, region);
                context->NoteCaptureRecorded();
            });

        handles.capture = *captureHandle;
        return handles;
    }

    void NriGraphContext::PublishFrameCoordinates(const FrameDesc& frame)
    {
        // ONE function, called from both RenderFrame and RenderFrameOffscreen,
        // because the two paths publishing DIFFERENT defaults is precisely the
        // drift this task must not introduce: the whole promise of the
        // per-frame probe is that a driver which names nothing gets exactly the
        // --pick-probe behaviour, on either flavor.
        m_currentPick       = frame.pickPixel.value_or(glm::ivec2(m_probeX, m_probeY));
        m_currentHover      = frame.hoverPixel.value_or(glm::ivec2(m_probeX, m_probeY));
        m_currentPickTicket = frame.pickTicket;
        // Published for the whole call, like m_currentImGui and for the same
        // reason (the node copies the geometry at RECORD time). Re-set every
        // frame including to null, so a later frame can never reach an earlier
        // frame's lists.
        m_currentGameUi     = frame.gameUi;
    }

    void NriGraphContext::BuildFrame(const FrameDesc& frame)
    {
        // ===== EVERY OWNED NODE SYNCS ITS POOL EPOCH, IN FRAME OR NOT =======
        // The three nodes below cache descriptors
        // over RenderGraph POOL textures, and the epoch is the only signal that
        // one of those textures has been retired (RenderGraph::PoolEpoch's
        // contract). Each already syncs from its own exec fn -- but an exec fn
        // only runs when the node is IN the frame, and the reachable case is
        // precisely the opposite one: the outline chain is declared only while
        // something wants an outline, so toggling the selection off shrinks the
        // pool on a frame OutlineNode does not record; the post chain does the
        // same across a re-wire. Such a node would hold stale descriptors until
        // it next happened to record, and bury them then -- after the texture
        // they name had already been destroyed, which is a use-after-free and
        // not merely an untidy order.
        //
        // HERE, at declaration time, is the one place that covers it: it runs
        // before Execute(), and Execute() opens by flushing the graph's
        // retirement staging area (RenderGraph::m_retiredPool, whose one-frame
        // delay is this fix's other half). So every node's views are already in
        // the graveyard, ahead of the textures they view, whatever the frame's
        // shape was. Cheap: one uint64 compare per node per frame, and a no-op
        // in the steady state.
        if (m_post)     m_post->SyncPoolEpoch(*m_graph);
        if (m_tonemap)  m_tonemap->SyncPoolEpoch(*m_graph);
        if (m_outline)  m_outline->SyncPoolEpoch(*m_graph);
        if (m_overlay)  m_overlay->SyncPoolEpoch(*m_graph);

        // Thin on purpose: the frame's SHAPE lives in DeclareGraphFrame so the
        // device-less [nri] frame-shape cases drive the real declarations instead
        // of transcribing them (see that function's comment block). The handles
        // it returns belong to the nodes that use them -- each exec fn already
        // captured what it needs -- so nothing here holds a per-frame handle
        // that would go stale on the next Reset().
        RgFrameShape shape;
        shape.capture       = frame.capture;
        // World or host-chrome frame (F5 plan 1, Task 2). Default true, so every
        // caller stays a world frame until it opts out.
        shape.world         = frame.world;
        // A chrome frame declares no world node, so world inputs on it are
        // dropped -- and a `batch` is never drained. Nothing sets them today
        // (PresentChromeFrame passes only `imgui`); say so loudly, once, if a
        // future caller does.
        if (!frame.world && (frame.batch != nullptr || frame.mesh != nullptr
                             || frame.grid != nullptr || frame.post != nullptr
                             || frame.overlay != nullptr))
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                ARC_WARN("[nri-graph] a world=false (chrome) frame carries world inputs "
                         "(batch/mesh/grid/post/overlay); they are ignored and the batch is not drained");
            }
        }
        shape.canvasWidth   = SurfaceWidth();
        shape.canvasHeight  = SurfaceHeight();
        shape.captureBuffer = m_capture;
        shape.captureBytes  = m_captureSlicePitch;
#if !defined(ARC_BUILD_DIST)
        // The Servitor capture ring (Task 27): declared only while a copy is
        // wanted or pending, so a frame with no remote capture keeps its shape.
        shape.ticketCapture       = PrepareTicketCapture();
        shape.ticketCaptureBuffer = m_ringBuffer;
        shape.ticketCaptureBytes  = m_ringSlicePitch * kMaxFramesInFlight;
#endif
        shape.post          = frame.post;
        // The mesh pass (Task 7). Belt-and-braces the same way
        // `pickOutline` and the two HUDs are: a scene for THIS frame and a node
        // that was actually built, so a driver handing a mesh scene to a
        // vehicle whose MeshNode failed to build declares nothing rather than
        // declaring a node that does not exist.
        shape.mesh          = m_mesh ? frame.mesh : nullptr;
        // The 3D reference grid (F4 plan 1 T10), belt-and-braces the same way.
        shape.grid          = m_overlay ? frame.grid : nullptr;
        // The post-tonemap overlay (F5 plan 1 T3), against ITS node the same way.
        shape.overlay       = m_overlay ? frame.overlay : nullptr;
        shape.overlayWorldViewProjection =
            frame.overlayWorldViewProjection ? &*frame.overlayWorldViewProjection : nullptr;
        // THE ONE LINE THAT MAKES A FRAME OFFSCREEN (Task 7). Null in
        // host-window mode, so the tonemap imports the swapchain exactly as it
        // always has; the vehicle's output texture otherwise. Nothing else in
        // this function -- or anywhere downstream of it -- is mode-aware.
        shape.offscreenOutput = m_offscreen;
        // Armed at Create AND asked for by this frame. Both, so a driver that
        // forgets the flag cannot declare a chain whose nodes were never built.
        shape.pickOutline   = frame.pickOutline && m_pick && m_outline;
        // Same belt-and-braces for the HUD: draw data for THIS frame and a
        // node that was actually built. The stage gate lives inside
        // DeclareGraphFrame, beside the post chain's, so the device-less
        // frame-shape cases exercise the real rule.
        shape.imgui         = frame.imgui != nullptr && m_imguiHud != nullptr;
        // ...and for the game HUD, against ITS node (Task 9). A driver that
        // hands gameUi draw data to a vehicle built without NodeSet::gameUi
        // declares nothing rather than declaring a node that does not exist.
        shape.gameUi        = frame.gameUi != nullptr && m_imguiGame != nullptr;

        // NOTHING follows the declaration, and that is worth stating because
        // an earlier draft of this task needed something here.
        //
        // Two things change when a post chain appears, disappears or re-wires:
        // the TONEMAP'S SOURCE texture (the canvas without a chain, the last
        // pass's target with one), and the TRANSIENT POOL -- RealizePool
        // buries a slot on a shrink or a desc change, from inside Execute().
        // Both are now handled where they are actually observable, inside the
        // nodes at record time: one descriptor set PER FRAME SLOT makes a
        // changed source ordinary rather than a hazard, and
        // RenderGraph::PoolEpoch is how a node learns its cached views were
        // buried. See FullscreenNodes.hpp, SOURCE VIEWS AND THE POOL.
        //
        // What that replaced was a DeviceWaitIdle here whenever the declared
        // pass count moved -- a stall, and INCOMPLETE: the pass count is a
        // proxy, and a chain re-wired from a DAG to a pipe keeps its count
        // while changing which physical texture the tonemap ends up sampling.
        (void)DeclareGraphFrame(*m_graph, shape, this);
    }

    NriGraphContext::FrameOutcome NriGraphContext::RenderFrame(const FrameDesc& frame)
    {
        const PacketBindingReset packetReset{ m_packet };

        // HOST-WINDOW ONLY. An offscreen context has no swapchain, so every
        // line below would dereference null; the twin is RenderFrameOffscreen.
        if (m_mode == Mode::Offscreen)
        {
            GraphError("NriGraphContext: RenderFrame() on an OFFSCREEN context -- call "
                       "RenderFrameOffscreen()");
            return FrameOutcome::Failed;
        }

        // ==============================================================
        // A ZERO-SIZED SURFACE IS ROUTINE, AND IT HAS TO BE CAUGHT HERE.
        // ==============================================================
        // FrameOutcome's own contract names "a zero-sized surface, a minimized
        // window" as SKIPPED -- routine, does not advance the frame counter,
        // does not end a --frames N run. NriSwapChain::AcquireNextTexture
        // implements exactly that skip, and until Task 8 it was reachable:
        // Task 7's frame declared no transients, so the first thing Execute()
        // did that could fail was the acquire.
        //
        // Task 8's frame declares the CANVAS, a transient sized to this
        // swapchain -- and NriSwapChain::Resize stores a 0x0 extent verbatim,
        // so a minimised window makes that a 0x0 transient. Execute() realizes
        // the pool BEFORE it acquires, and RealizePool REFUSES a zero extent
        // through the latched error seam (nri::Dim_t cannot express it and a
        // zero-dimension texture would create successfully as something else).
        // A latched refusal is FrameOutcome::Failed, which stops the run with a
        // nonzero exit code -- the opposite of routine, and it would have made
        // the acquire's own skip unreachable.
        //
        // So the guard belongs in front of the DECLARATION, not inside it: at
        // 0x0 there is no frame worth declaring at all.
        if (m_swap->Width() == 0 || m_swap->Height() == 0)
            return FrameOutcome::Skipped;

        // A capture frame needs its staging buffer sized to the CURRENT extent
        // before the graph is declared (the node imports the buffer, so it has
        // to exist first). A failure here degrades to an uncaptured frame
        // rather than a failed one -- the run's pixels are still valid and
        // only the artifact is lost.
        FrameDesc effective = frame;
        if (effective.capture && !EnsureCaptureBuffer())
            effective.capture = false;

        // Cheap and expected per frame: clears DECLARATIONS only. The
        // transient pool, the cached attachment views, the command slots and
        // the submission fence all survive, so a steady-state frame costs zero
        // GPU allocations (RenderGraph::Reset's contract).
        m_graph->Reset();

        // Published for exactly the span of this frame's declaration: the batch
        // node drains it from its declarator (AddBatch2DNode -> CurrentBatch),
        // and nothing may reach a stale batcher afterwards.
        //
        // The GLOBALS are copied rather than published-and-cleared, because
        // unlike the batcher they are read at RECORD time (the post chain
        // writes b1 inside its exec fn) -- see CurrentGlobals().
        m_currentGlobals = effective.globals ? *effective.globals : GlobalParams{};
        m_currentBatch = effective.batch;

        // Published for exactly the span of this frame's declaration, like the
        // batcher: the pick node builds its geometry from the drawables inside
        // AddPickNodes, and the seed node copies the selection into this
        // frame's arena region at record time from the node's own storage.
        m_currentPickables   = effective.pickables;
        m_currentSelectedIds = effective.selectedIds;
        m_currentPickView    = effective.pickView;

        // The HUD's draw data, published for the whole of THIS call rather
        // than only its declaration half -- the node's exec fn is what copies
        // the geometry into the ring (FrameDesc::imgui states why). Re-set
        // every frame, including to null, so a later frame can never reach a
        // previous frame's lists.
        m_currentImGui = effective.imgui;

        // ...and the frame's COORDINATES + the game HUD's draw data, both
        // paths through one function -- see PublishFrameCoordinates.
        PublishFrameCoordinates(effective);

        // A probe pixel outside the surface would still be CLAMPED into the id
        // target by the readback node (PickSampleTexel), because the frame's
        // shape must not depend on a coordinate -- but reporting that clamped
        // texel's id as "the id at (x, y)" would be a confident answer about a
        // pixel nobody asked for. Latch it here instead; ProbeId() then reports
        // nothing and the run exits as a miss.
        if (m_pickArmed && !m_probeOutOfRange
            && (m_probeX >= (std::int32_t)m_swap->Width() || m_probeY >= (std::int32_t)m_swap->Height()))
        {
            m_probeOutOfRange = true;
            ARC_ERROR("[nri-graph] --pick-probe ({}, {}) is outside the {}x{} surface -- no id will "
                      "be reported", m_probeX, m_probeY, m_swap->Width(), m_swap->Height());
        }

        BuildFrame(effective);
        m_currentBatch       = nullptr;
        m_currentPickables   = {};
        m_currentSelectedIds = {};

        std::string error;
        const std::optional<RgCompiled> compiled = m_graph->Compile(&error);
        if (!compiled)
        {
            GraphError("NriGraphContext: the frame did not compile -- " + error);
            return FrameOutcome::Failed;
        }

        // Which command-buffer / ring slot this frame records into. Advanced
        // only on a PRESENTED frame, so it stays in lockstep with
        // NriSwapChain's own frame counter -- which is what makes the pacing
        // wait inside AcquireNextTexture the proof that this slot is safe to
        // reset (RgExecuteDesc::frameSlot's caller contract).
        const std::uint32_t slot = (std::uint32_t)(m_frameIndex % FramesInFlight());
        m_ring.BeginFrame(slot);   // the caller owes this before Execute()

        const RgExecuteDesc desc{ *m_device, m_graves, m_swap.get(), m_ring, m_pipelines, slot };

        // Execute() returns false for TWO different things and the frame
        // driver must treat them differently: a skipped acquire (routine --
        // zero-sized surface or an OUT_OF_DATE awaiting a Resize; deliberately
        // NOT latched, see RenderGraphExec) and a real failure (always
        // latched, through ARC_NRI_CHECK or the "nri-graph" seam). The latch
        // is therefore the discriminator -- there is no other observable, and
        // inventing a second return channel on a frozen signature would have
        // been worse.
        const std::uint64_t errorsBefore = RenderErrorCount();
        if (!m_graph->Execute(desc, *compiled))
            return RenderErrorCount() > errorsBefore ? FrameOutcome::Failed : FrameOutcome::Skipped;

        // BEFORE the advance, deliberately: this frame's Resolve calls stamped
        // lastDrawnFrame with m_frameIndex's CURRENT value, and SelectEvictions
        // protects entries carrying exactly it. Evicting with the advanced counter
        // protects nothing and can drop geometry the frame just recorded still names.
        //
        // AND ONLY AFTER A SUCCESSFUL Execute, also deliberately: the two early
        // returns above (Failed / Skipped) leave the budget unchecked for that frame.
        // A vehicle that skips every frame therefore never evicts -- accepted,
        // because a skipping vehicle is not growing its resident set either (nothing
        // resolved, nothing uploaded), and evicting behind a half-built command
        // buffer is the one thing the frame-boundary rule above exists to prevent.
        if (m_meshBuffers)
            m_meshBuffers->EvictToBudget(m_frameIndex, m_graves,
                                         m_graph ? m_graph->DebugSubmitCount() : 0);
        // The GPU scene's retirements (a grown instance buffer, a grown slot
        // buffer), stamped at declaration with the value this submit just
        // signalled -- see NriGraphContext::CurrentFence for why they wait
        // until here.
        if (m_scene)
            m_scene->FlushGraves(m_graves);
#if !defined(ARC_BUILD_DIST)
        ConfirmCaptureRingSubmission(m_captureRing, m_frameIndex);   // this frame's ring copy was submitted
#endif
        ++m_frameIndex;

        // THE GPU-PROGRESS HEARTBEAT, after the frame's present and
        // therefore after its last submit: a stamp placed before the frame's
        // work would retire early and report progress the GPU had not made.
        // Presented frames only -- a skipped frame submitted nothing, so
        // republishing here would say "the GPU is fine" about a frame the GPU
        // never saw.
        //
        // The value is the pacing fence's COMPLETED value, not the frame
        // index: m_frameIndex is what the CPU has SUBMITTED and advances
        // happily while the GPU is wedged, which is precisely the state this
        // heartbeat exists to make visible.
        NriDiagnostics::PublishHeartbeat(m_swap->CompletedFrameValue());

        return FrameOutcome::Presented;
    }

    NriGraphContext::FrameOutcome NriGraphContext::RenderFrameOffscreen(const FrameDesc& frame)
    {
        const PacketBindingReset packetReset{ m_packet };

        if (m_mode != Mode::Offscreen)
        {
            GraphError("NriGraphContext: RenderFrameOffscreen() on a HOST-WINDOW context -- call "
                       "RenderFrame()");
            return FrameOutcome::Failed;
        }

        // A failed ResizeOffscreen leaves no output. Refuse rather than declare
        // a frame whose final target does not exist -- the resize already
        // latched the reason, and repeating it every frame would bury it.
        if (!m_offscreen)
            return FrameOutcome::Skipped;

        // The same guard, in the same place and for the same reason, as
        // RenderFrame's: at 0x0 the canvas transient is a texture NRI cannot
        // express, and RealizePool's refusal is LATCHED -- Failed, not the
        // routine Skipped this actually is. Only reachable through a resize
        // that could not create its replacement.
        if (SurfaceWidth() == 0 || SurfaceHeight() == 0)
            return FrameOutcome::Skipped;

        // ==============================================================
        // PACING, WITHOUT A SWAPCHAIN TO DO IT.
        // ==============================================================
        // On the present path NriSwapChain::AcquireNextTexture holds the wait
        // that makes this frame's slot safe to reuse -- and it is not
        // decoration: the frame is about to reset frame slot
        // `m_frameIndex % FramesInFlight()`'s command allocator
        // (RgExecuteDesc::frameSlot's caller contract), reset that slot's
        // upload-ring arena, and rewrite that slot's descriptor sets in the
        // tonemap, the post chain and the HUD. Every one of those is a write to
        // memory the GPU may still be reading FramesInFlight() frames
        // back. Nothing acquires here, so the wait has to be ours.
        //
        // Same shape as the swapchain's, deliberately: ONE timeline fence,
        // 1-based signal values, wait for `m_frameIndex - depth + 1`, and no
        // call at all for the first `depth` frames (nothing has been submitted
        // to wait on, and value 0 is what an unsignalled fence already reads).
        const std::uint32_t framesInFlight = FramesInFlight();
        if (m_frameIndex >= framesInFlight)
        {
            PollingWaitForTimelineFence(m_device->Core(), m_offscreenFence,
                                        m_frameIndex - framesInFlight + 1);
        }

        FrameDesc effective = frame;
        if (effective.capture && !EnsureCaptureBuffer())
            effective.capture = false;

        m_graph->Reset();

        // Published exactly as RenderFrame publishes them -- same lifetimes,
        // same clearing points. See that function for what each one owes.
        m_currentGlobals     = effective.globals ? *effective.globals : GlobalParams{};
        m_currentBatch       = effective.batch;
        m_currentPickables   = effective.pickables;
        m_currentSelectedIds = effective.selectedIds;
        m_currentPickView    = effective.pickView;
        m_currentImGui       = effective.imgui;
        PublishFrameCoordinates(effective);

        if (m_pickArmed && !m_probeOutOfRange
            && (m_probeX >= (std::int32_t)SurfaceWidth() || m_probeY >= (std::int32_t)SurfaceHeight()))
        {
            m_probeOutOfRange = true;
            ARC_ERROR("[nri-graph] --pick-probe ({}, {}) is outside the {}x{} offscreen target -- no "
                      "id will be reported", m_probeX, m_probeY, SurfaceWidth(), SurfaceHeight());
        }

        BuildFrame(effective);
        m_currentBatch       = nullptr;
        m_currentPickables   = {};
        m_currentSelectedIds = {};

        std::string error;
        const std::optional<RgCompiled> compiled = m_graph->Compile(&error);
        if (!compiled)
        {
            GraphError("NriGraphContext: the offscreen frame did not compile -- " + error);
            return FrameOutcome::Failed;
        }

        const std::uint32_t slot = (std::uint32_t)(m_frameIndex % FramesInFlight());
        m_ring.BeginFrame(slot);   // the caller owes this before Execute()

        // THE ONE LINE THAT DIFFERS FROM THE PRESENT PATH'S EXECUTE: no
        // swapchain. RgExecuteDesc documents null as "headless/offscreen" and
        // the executor already honours it end to end -- it acquires nothing,
        // waits on no acquire fence, signals no release fence and presents
        // nothing, while still signalling the graph's OWN fence (the
        // graveyard's clock, and this frame's only completion signal). No
        // executor change was needed for this task; the [nri] NONE cases pin
        // both halves of that path.
        // The lane beside it is the OTHER half of what makes an offscreen
        // context safe on a shared device (Task 8-pre): the fence this Execute
        // reaps with is THIS graph's, and m_graves is the only graveyard it
        // describes. The host-window context's lane is untouched by it.
        const RgExecuteDesc desc{ *m_device, m_graves, /*swapChain=*/nullptr, m_ring, m_pipelines, slot };

        const std::uint64_t errorsBefore = RenderErrorCount();
        if (!m_graph->Execute(desc, *compiled))
            return RenderErrorCount() > errorsBefore ? FrameOutcome::Failed : FrameOutcome::Skipped;

        // ...and the pacing stamp the present path gets from
        // NriSwapChain::Present: a submission carrying no command buffers and
        // ONE signal, which the queue's in-order semantics make "signal once
        // everything submitted before this has retired". Exactly the shape (and
        // the 1-based value) NriSwapChain uses, and the reason that class
        // stamps AFTER the present rather than inside the frame's own submit.
        //
        // A FAILED STAMP IS NOT A FAILED FRAME -- the frame's pixels landed.
        // But it IS latched (ARC_NRI_CHECK) and it does break pacing, so
        // m_frameIndex is deliberately still advanced: leaving the counter
        // behind the fence's values would make every later wait ask for a value
        // that was already signalled, which is silently worse than a loud one-
        // off error.
        nri::FenceSubmitDesc signalFence = {};
        signalFence.fence = m_offscreenFence;
        signalFence.value = m_frameIndex + 1;

        nri::QueueSubmitDesc stamp = {};
        stamp.signalFences   = &signalFence;
        stamp.signalFenceNum = 1;
        (void)ARC_NRI_CHECK(m_device->Core().QueueSubmit(*m_device->GraphicsQueue(), stamp));

        // BEFORE the advance -- same contract as RenderFrame() above.
        if (m_meshBuffers)
            m_meshBuffers->EvictToBudget(m_frameIndex, m_graves,
                                         m_graph ? m_graph->DebugSubmitCount() : 0);
        if (m_scene)
            m_scene->FlushGraves(m_graves);   // same contract as RenderFrame() above
#if !defined(ARC_BUILD_DIST)
        ConfirmCaptureRingSubmission(m_captureRing, m_frameIndex);   // this frame's ring copy was submitted
#endif
        ++m_frameIndex;

        // THE GPU-PROGRESS HEARTBEAT, published ONLY when the caller has said
        // this context is its process's sole graph context
        // (SetGpuHeartbeatPublisher -- the header carries the full contract).
        //
        // The DEFAULT is still silence, and for the original reason:
        // Diagnostics::GpuHeartbeat holds ONE process-wide counter whose stall
        // rule fires on "the value stopped changing", so two publishers with
        // unrelated timelines would alternate values and read as progress
        // forever -- disarming the rule on exactly the case it exists to
        // catch. In the editor's topology the PRESENTING context is that one
        // publisher, and its pacing fence only advances when the queue
        // retires, this context's work included (they share the queue).
        //
        // What changed is that the engine now has a host that RENDERS AND
        // NEVER PRESENTS (ArcaneRuntime --headless). "The presenting context
        // publishes" covered every case while every device-ful host presented;
        // it covers nothing when there is no presenting context, and the
        // result was not a false positive but NO GPU-stall coverage at all --
        // g_gpuBeatSeen never armed, so Diagnostics.cpp's rule returned before
        // it looked at anything, and a hang during an offscreen run wrote no
        // capture. The opt-in closes that without touching the two-context
        // case, which never calls it.
        //
        // The VALUE is the pacing fence's COMPLETED value, deliberately, and
        // for the same reason the present path publishes
        // NriSwapChain::CompletedFrameValue() rather than its frame index:
        // m_frameIndex is what the CPU has SUBMITTED and keeps climbing while
        // the GPU is wedged. GetFenceValue advances only when the trailing
        // signal submit above has actually retired.
        //
        // PLACED AFTER THAT STAMP, mirroring the present path's "after the
        // frame's last submit": a beat published before this frame's work was
        // signalled would report progress the GPU had not yet made. The
        // FRESHNESS half of the rule is already wired on this path --
        // PollingWaitForTimelineFence (top of this file) calls
        // Diagnostics::GpuHeartbeatRefresh() while it polls, which is what
        // keeps a wedged GPU reading as "publishing, but frozen" instead of as
        // silence.
        if (m_publishGpuBeat && m_offscreenFence)
            NriDiagnostics::PublishHeartbeat(m_device->Core().GetFenceValue(*m_offscreenFence));

        return FrameOutcome::Presented;
    }

    void NriGraphContext::SetGpuHeartbeatPublisher(bool enable) noexcept
    {
        // REFUSED, not silently ignored, on a host-window context: that
        // context already publishes unconditionally from RenderFrame, so a
        // caller reaching here has misread the topology and the flag would do
        // nothing -- which is the failure mode that hides a misreading rather
        // than surfacing it. Not latched: it is a caller mistake, not a render
        // error, and it must not fail an otherwise clean run's exit code.
        if (m_mode != Mode::Offscreen)
        {
            ARC_WARN("[nri-graph] SetGpuHeartbeatPublisher on a HOST-WINDOW context -- ignored; "
                     "the present path is already this process's GPU-progress publisher");
            return;
        }
        m_publishGpuBeat = enable;
        if (enable)
            ARC_INFO("[nri-graph] offscreen context is this process's GPU-progress publisher -- "
                     "the gpu-stall watchdog is armed for a run that never presents");
    }

    std::optional<std::uint32_t> NriGraphContext::ProbeId() const noexcept
    {
        // No node, an out-of-range coordinate, or no readback landed yet (the
        // first FramesInFlight() frames of a probe run) all report
        // NOTHING rather than 0 -- 0 is a real answer (background) and the
        // caller must be able to tell the two apart.
        if (!m_pick || m_probeOutOfRange)
            return std::nullopt;
        return m_pick->LastProbeId();
    }

    std::optional<NriGraphContext::PickReadback> NriGraphContext::ProbeResult() const noexcept
    {
        // ProbeId()'s refusals, verbatim -- one predicate, so the two accessors
        // cannot disagree about whether a value exists.
        const std::optional<std::uint32_t> id = ProbeId();
        if (!id)
            return std::nullopt;
        return PickReadback{ *id, m_pick->LastProbeTicket() };
    }

    bool NriGraphContext::ReadCapture(std::uint32_t& width, std::uint32_t& height,
                                       std::vector<unsigned char>& rgba)
    {
        width = height = 0;
        rgba.clear();

        if (!m_captureRecorded || !m_capture)
        {
            ARC_ERROR("[nri-graph] capture requested but no frame recorded one "
                      "(a capture is taken on the LAST frame -- it needs --frames N, and the run "
                      "must reach that frame)");
            return false;
        }

        const nri::CoreInterface& core = m_device->Core();

        // The copy was recorded into the frame that was just submitted and
        // presented; it has to have LANDED before the mapped bytes mean
        // anything. A stall paid on a frame the process is about to exit
        // after.
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        const auto* mapped = static_cast<const std::uint8_t*>(
            core.MapBuffer(*m_capture, 0, nri::WHOLE_SIZE));
        if (!mapped)
        {
            ARC_ERROR("[nri-graph] MapBuffer on the capture buffer returned null");
            return false;
        }

        // Tight RGBA8 out, whatever came in: the row pitch is dropped and
        // BGRA is swizzled. NRI cannot be told which channel order to
        // resolve, so the normalization has to happen HERE -- and then a
        // capture's bytes mean the same thing on every backend. The one copy
        // of that loop is NormalizeCaptureRows (ImageIo.hpp), which the
        // Servitor capture job shares.
        NormalizeCaptureRows(mapped, m_captureWidth, m_captureHeight,
                             static_cast<std::uint32_t>(m_captureRowPitch),
                             IsBgraFormat(m_format), rgba);
        core.UnmapBuffer(*m_capture);

        width  = m_captureWidth;
        height = m_captureHeight;
        return true;
    }

#if !defined(ARC_BUILD_DIST)
    // =====================================================================
    // THE SERVITOR CAPTURE RING (Task 27). See CaptureRing in the header.
    // Main thread throughout: Arm/Drain run inside the host's remote Drain,
    // PrepareTicketCapture inside BuildFrame, NoteRenderSubmission inside
    // Execute's record pass.
    // =====================================================================
    bool NriGraphContext::ArmTicketCapture(std::uint64_t ticket)
    {
        if (ticket == 0 || !m_device)
            return false;
        CaptureRing& ring = m_captureRing;
        ring.framesInFlight = FramesInFlight();
        ring.wanted         = ticket;
        // One capture in flight per host (CaptureInFlight in ArcaneRemote), so
        // the previous capture's bytes have been consumed by now.
        ring.haveRetired   = false;
        ring.retiredTicket = 0;
        ring.retiredBytes.clear();
        ring.retiredBytes.shrink_to_fit();
        return true;
    }

    void NriGraphContext::CancelTicketCapture(std::uint64_t ticket)
    {
        CancelCaptureRingTicket(m_captureRing, ticket);
    }

    bool NriGraphContext::DrainTicketCapture(std::uint64_t ticket, TicketCaptureBytes& out) const
    {
        out = {};
        if (ticket == 0)
            return false;
        if (m_lostTicket == ticket)
            return true;   // the copy never happened or was dropped: a true drain with a null map
        const CaptureRing& ring = m_captureRing;
        if (!ring.haveRetired || ring.retiredTicket != ticket)
            return false;
        out.width       = ring.retiredW;
        out.height      = ring.retiredH;
        out.rowPitch    = ring.retiredPitch;
        out.bgra        = ring.retiredBgra;
        out.mapped      = ring.retiredBytes.empty() ? nullptr : ring.retiredBytes.data();
        out.mappedBytes = static_cast<std::uint32_t>(ring.retiredBytes.size());
        return true;
    }

    bool NriGraphContext::PrepareTicketCapture()
    {
        CaptureRing& ring = m_captureRing;
        bool anyPending = false;
        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
            anyPending = anyPending || ring.pending[i];
        if (ring.wanted == 0)
            return anyPending && m_ringBuffer != nullptr;

        const std::uint32_t width  = SurfaceWidth();
        const std::uint32_t height = SurfaceHeight();
        const bool sized = m_ringBuffer && m_ringWidth == width && m_ringHeight == height;
        if (sized || anyPending)
            return m_ringBuffer != nullptr;   // a pending slot keeps the old buffer; the wanted copy waits

        const nri::CoreInterface& core = m_device->Core();
        ReleaseTicketRing(m_graves, m_graph ? m_graph->DebugSubmitCount() : 0, false);

        // The --screenshot buffer's alignment rules (EnsureCaptureBuffer),
        // one aligned slice per frame slot.
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());
        const std::uint64_t rowPitch   = AlignUp(std::uint64_t(width) * 4,
                                                 deviceDesc.memoryAlignment.uploadBufferTextureRow);
        const std::uint64_t slicePitch = AlignUp(rowPitch * height,
                                                 deviceDesc.memoryAlignment.uploadBufferTextureSlice);
        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = slicePitch * kMaxFramesInFlight;
        bufferDesc.usage = nri::BufferUsageBits::NONE;   // copy destination only
        nri::Buffer* buffer = nullptr;
        const void* cpu = nullptr;
        if (width != 0 && height != 0
            && ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_READBACK,
                                                         0.0f, bufferDesc, buffer))
            && buffer)
        {
            core.SetDebugName(buffer, "nri-graph ticket capture");
            cpu = core.MapBuffer(*buffer, 0, nri::WHOLE_SIZE);
        }
        if (!buffer || !cpu)
        {
            ARC_ERROR("[nri-graph] the capture ring's staging buffer ({}x{}) could not be created or mapped -- "
                      "the remote capture for ticket {} is lost", width, height, ring.wanted);
            if (buffer)
                core.DestroyBuffer(buffer);   // never recorded into: nothing in flight names it
            m_lostTicket = ring.wanted;
            ring.wanted  = 0;
            return false;
        }
        m_ringBuffer     = buffer;
        m_ringCpu        = static_cast<const std::uint8_t*>(cpu);
        m_ringRowPitch   = rowPitch;
        m_ringSlicePitch = slicePitch;
        m_ringWidth      = width;
        m_ringHeight     = height;
        return true;
    }

    void NriGraphContext::NoteRenderSubmission(const nri::CoreInterface& core, nri::CommandBuffer& cmd,
                                               nri::Texture& source, nri::Buffer& dest)
    {
        CaptureRing& ring = m_captureRing;
        // The ring's slot IS this frame's slot (FrameSlot(), m_frameIndex %
        // FramesInFlight()): `submission` follows the context's own clock, so
        // the pacing wait this frame already paid for its slot covers the ring's.
        ring.submission = static_cast<std::uint32_t>(m_frameIndex);
        if (ring.framesInFlight == 0)
            ring.framesInFlight = 1;
        const std::uint32_t slot = ring.submission % ring.framesInFlight;
        if (slot >= kMaxFramesInFlight || &dest != m_ringBuffer || !m_ringCpu)
        {
            GraphError("NriGraphContext: the ticket-capture node ran with a slot outside the ring or "
                       "without its buffer");
            return;
        }

        const CaptureRingStep step = PlanCaptureRingSlot(
            ring, slot, m_frameIndex, m_ringWidth == SurfaceWidth() && m_ringHeight == SurfaceHeight());

        // DRAIN FIRST: the copy in this slot was SUBMITTED FramesInFlight() or
        // more frames ago, so the pacing wait this frame paid has retired it
        // (PickNode::RecordReadback's argument). No fence query, no idle.
        if (step.drain)
        {
            const std::size_t bytes = static_cast<std::size_t>(m_ringRowPitch) * m_ringHeight;
            const std::uint8_t* src = m_ringCpu + static_cast<std::size_t>(slot) * m_ringSlicePitch;
            ring.retiredBytes.assign(src, src + bytes);
            ring.retiredW      = m_ringWidth;
            ring.retiredH      = m_ringHeight;
            ring.retiredPitch  = static_cast<std::uint32_t>(m_ringRowPitch);
            ring.retiredBgra   = IsBgraFormat(m_format);
            RetireCaptureRingSlot(ring, slot);
        }

        // RECORD SECOND: the wanted copy into this slot, only at the buffer's
        // extent (PrepareTicketCapture resizes it only while nothing is pending).
        if (step.record)
        {
            nri::TextureDataLayoutDesc layout = {};
            layout.offset     = static_cast<std::uint64_t>(slot) * m_ringSlicePitch;
            layout.rowPitch   = static_cast<std::uint32_t>(m_ringRowPitch);
            layout.slicePitch = static_cast<std::uint32_t>(m_ringSlicePitch);

            nri::TextureRegionDesc region = {};
            region.width  = static_cast<nri::Dim_t>(m_ringWidth);
            region.height = static_cast<nri::Dim_t>(m_ringHeight);
            region.depth  = 1;

            core.CmdReadbackTextureToBuffer(cmd, dest, layout, source, region);
            RecordCaptureRingSlot(ring, slot, m_frameIndex);
        }
        ++ring.submission;
    }

    void NriGraphContext::ReleaseTicketRing(Graveyard& graves, std::uint64_t fence, bool retire)
    {
        CaptureRing& ring = m_captureRing;
        const bool canRetire = retire && m_ringCpu != nullptr;
        for (std::uint32_t slot = 0; slot < kMaxFramesInFlight; ++slot)
        {
            if (!ring.pending[slot])
                continue;
            if (ring.abandoned[slot])
            {
                // Its job gave up (CancelCaptureRingTicket): drop it, never retire it.
                ring.pending[slot]   = false;
                ring.abandoned[slot] = false;
                continue;
            }
            if (canRetire && ring.submitted[slot])
            {
                const std::size_t bytes = static_cast<std::size_t>(m_ringRowPitch) * m_ringHeight;
                const std::uint8_t* src = m_ringCpu + static_cast<std::size_t>(slot) * m_ringSlicePitch;
                ring.retiredBytes.assign(src, src + bytes);
                ring.retiredW      = m_ringWidth;
                ring.retiredH      = m_ringHeight;
                ring.retiredPitch  = static_cast<std::uint32_t>(m_ringRowPitch);
                ring.retiredBgra   = IsBgraFormat(m_format);
                ring.retiredTicket = ring.ticket[slot];
                ring.haveRetired   = true;
            }
            else if (ring.submitted[slot])
            {
                m_lostTicket = ring.ticket[slot];   // its drain answers with a null map: capture.no-frame
            }
            else if (ring.wanted == 0)
            {
                ring.wanted = ring.ticket[slot];    // never submitted: copy it again from the next frame
            }
            ring.pending[slot] = false;
        }
        if (m_ringBuffer)
        {
            const nri::CoreInterface* graveCore = &m_device->Core();
            graves.Bury(fence, [graveCore, b = m_ringBuffer] {
                graveCore->UnmapBuffer(*b);
                graveCore->DestroyBuffer(b);
            });
        }
        m_ringBuffer     = nullptr;
        m_ringCpu        = nullptr;
        m_ringRowPitch   = 0;
        m_ringSlicePitch = 0;
        m_ringWidth      = 0;
        m_ringHeight     = 0;
    }
#endif
}
