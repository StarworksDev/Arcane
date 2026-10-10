// See NriSwapChain.hpp for the design rationale (NRISamples attribution,
// destroy+recreate ruling, SUBOPTIMAL/OUT_OF_DATE findings, pacing vs
// GpuFrameSlot). Same include-order rule as NriDevice.cpp/NriCommon.hpp
// (nri::Message::ERROR vs wingdi.h's ERROR macro) -- NRI headers first.
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>
#include <Extensions/NRISwapChain.h>

#include "NriSwapChain.hpp"

#include "NriCommon.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // GpuDeviceLostObserved -- the device-lost teardown gate
#include <Arcane/Render/RenderDeviceSettings.hpp>   // render.allowTearing
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Platform/Platform.hpp>

#include <SDL3/SDL_timer.h>

// wingdi.h (via spdlog -> windows.h, dragged in by Arcane/Base/Log.hpp and
// Diagnostics.hpp) unconditionally #defines ERROR. Undefine it after the
// last header that could define it, before any further code in this file.
#undef ERROR

#include <chrono>
#include <cstdint>

namespace Arcane
{
    namespace
    {
        // ---------------------------------------------------------------
        // The pacing wait. See NriSwapChain.hpp's header comment for why a
        // bare Core().Wait() cannot be used here. File-local deliberately: it
        // is a leaf detail of the pacing wait, not a general-purpose fence
        // helper.
        // ---------------------------------------------------------------

        ARC_CONSTANT("OS timer floor: the poll sleep/window; below the scheduling quantum a sleep degrades into a spin")
        constexpr Uint64 kFencePollSleepNs = 1'000'000;         // 1ms, via SDL's high-resolution waitable timer -- NOT std::this_thread::sleep_for's ~15.6ms Windows quantum
        ARC_CONSTANT("OS timer floor: the poll sleep/window; below the scheduling quantum a sleep degrades into a spin")
        constexpr std::chrono::seconds kFencePollWindow{ 15 };  // comfortably above Config::gpuStallSeconds' 8s default

        void PollingWaitForTimelineFence(const nri::CoreInterface& core, nri::Fence* fence, uint64_t value)
        {
            // Fast path, before any sleep: the common case (the slot's frame
            // retired long ago) costs exactly one GetFenceValue call.
            if (core.GetFenceValue(*fence) >= value)
                return;

            const auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start < kFencePollWindow)
            {
                // Same two beats, same order, same reasoning as the deleted
                // PollingWaitForStampedQuery: Heartbeat() says the render
                // thread is alive and deliberately waiting (not wedged);
                // GpuHeartbeatRefresh() republishes freshness on the SAME
                // counter without changing it (this wait is not GPU
                // progress, just watching for it) -- exactly the state the
                // GPU-stall rule needs to stay quiet through an ordinary wait.
                Diagnostics::Heartbeat();
                Diagnostics::GpuHeartbeatRefresh();

                SDL_DelayNS(kFencePollSleepNs);

                if (core.GetFenceValue(*fence) >= value)
                    return;
            }

            // Past the window: park properly in NRI's own blocking wait
            // rather than polling forever. By the time we reach here any
            // gpu-stall report has long since been written if this is a real
            // hang; this is just how the call eventually returns either way.
            core.Wait(*fence, value);
        }
    }

    // -----------------------------------------------------------------
    // Create / Init
    // -----------------------------------------------------------------

    std::unique_ptr<NriSwapChain> NriSwapChain::Create(NriDevice& device, Window& window, bool vsync)
    {
        auto swapChain = std::unique_ptr<NriSwapChain>(new NriSwapChain());
        if (!swapChain->Init(device, window, vsync))
            return nullptr;
        return swapChain;
    }

    bool NriSwapChain::Init(NriDevice& device, Window& window, bool vsync)
    {
        m_device = &device;
        m_window = &window;
        m_vsync  = vsync;
        window.GetPixelSize(m_width, m_height);

        if (!ARC_NRI_CHECK(nriGetInterface(device.Device(), NRI_INTERFACE(nri::SwapChainInterface), &m_swapChainInterface)))
        {
            ARC_ERROR("[nri] SwapChainInterface unavailable on the wrapped {} device", ToString(device.Backend()));
            return false;
        }

        // The pacing fence: created once, survives every later Resize().
        if (!ARC_NRI_CHECK(device.Core().CreateFence(device.Device(), 0, m_frameFence)) || !m_frameFence)
        {
            ARC_ERROR("[nri] swapchain pacing-fence creation failed");
            return false;
        }

        return CreateSwapChainObjects();
    }

    // -----------------------------------------------------------------
    // Swapchain object lifetime (create/release pair; Resize == both)
    // -----------------------------------------------------------------

    bool NriSwapChain::CreateSwapChainObjects()
    {
        if (m_width == 0 || m_height == 0)
            return true;   // minimized / not yet sized; AcquireNextTexture skips until Resize() restores it

        nri::SwapChainDesc desc = {};
#if ARC_PLATFORM_WINDOWS
        desc.window.windows.hwnd = m_window->NativeHandle();
#elif ARC_PLATFORM_MACOS
        // macOS port: MoltenVK presents to the window's CAMetalLayer through
        // VK_EXT_metal_surface (NRI's VK backend is built with
        // VK_USE_PLATFORM_METAL_EXT on a Mac target).
        desc.window.metal.caMetalLayer = m_window->NativeHandle();
#else
        // Linux port: NRI's VK backend is built with the Xlib + Wayland surface
        // paths (ThirdParty/NRI/premake5.lua); fill whichever SDL is on.
        if (m_window->IsWaylandWindow())
        {
            desc.window.wayland.display = m_window->NativeDisplay();
            desc.window.wayland.surface = m_window->NativeHandle();
        }
        else
        {
            desc.window.x11.dpy    = m_window->NativeDisplay();
            desc.window.x11.window = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(m_window->NativeHandle()));
        }
#endif
        desc.queue               = m_device->GraphicsQueue();
        desc.width               = (nri::Dim_t)m_width;
        desc.height              = (nri::Dim_t)m_height;
        // FramesInFlight() + 1: the textureNum shape NRISamples'
        // NRIFramework recommends
        // (GetOptimalSwapChainTextureNum() == GetQueuedFrameNum() + 1) --
        // texture count strictly greater than frames-in-flight is also what
        // makes the recycled-by-frame-index acquire-fence indexing below
        // valid (NRISwapChain.h's usage comment: "valid if the number of
        // swap chain images >= queued frames").
        desc.textureNum = FramesInFlight() + 1;
        // BT709_G22_8BIT: 8 bits/channel, gamma ~2.2, LDR, no HDR
        // reinterpretation -- an sRGB-nonlinear presentation class. See
        // Format()'s comment: NRI does not let a wrapper pin exact channel
        // order, only this abstract class -- the concrete nri::Format is
        // queried after creation, not assumed.
        desc.format = nri::SwapChainFormat::BT709_G22_8BIT;
        // ALLOW_TEARING only when render.allowTearing is on AND vsync is off
        // (default off). Without it: on D3D12, FLIP_DISCARD with
        // DXGI_PRESENT_ALLOW_TEARING never passed; on Vulkan, NRI's own
        // present-mode search (SwapChainVK.hpp) tries MAILBOX first when not
        // syncing, falling back to FIFO_LATEST_READY or FIFO if Mailbox is
        // unavailable. Present-mode selection past what these two bits
        // control is deliberately not chased.
        desc.flags = m_vsync ? nri::SwapChainBits::VSYNC : nri::SwapChainBits::NONE;
        if (!m_vsync && Settings<RenderSettings>().allowTearing)
            desc.flags |= nri::SwapChainBits::ALLOW_TEARING;
        // aka "frames in flight" per NRISwapChain.h -- keep DXGI's own
        // frame-latency machinery (SetMaximumFrameLatency, D3D12 non-WAITABLE
        // path) aligned with the depth our OWN timeline-fence pacing already
        // enforces, rather than leaving it at NRI's default.
        desc.queuedFrameNum = (uint8_t)FramesInFlight();

        if (!ARC_NRI_CHECK(m_swapChainInterface.CreateSwapChain(m_device->Device(), desc, m_swapChain)) || !m_swapChain)
        {
            ARC_ERROR("[nri] CreateSwapChain failed ({}x{}, backend={})", m_width, m_height, ToString(m_device->Backend()));
            return false;
        }

        uint32_t textureNum = 0;
        nri::Texture* const* textures = m_swapChainInterface.GetSwapChainTextures(*m_swapChain, textureNum);
        if (!textures || textureNum == 0)
        {
            ARC_ERROR("[nri] GetSwapChainTextures returned no textures");
            return false;
        }

        // Query the ACTUAL resolved format rather than assume it -- see
        // Format()'s comment on why NRI's abstract SwapChainFormat cannot be
        // trusted to pin BGRA vs RGBA channel order. Same idiom the samples
        // use (Source/Triangle.cpp, Source/Resize.cpp).
        m_format = m_device->Core().GetTextureDesc(*textures[0]).format;

        m_textures.resize(textureNum);
        for (uint32_t i = 0; i < textureNum; ++i)
        {
            m_textures[i].texture = textures[i];
            const bool acquireOk = ARC_NRI_CHECK(m_device->Core().CreateFence(m_device->Device(), nri::SWAPCHAIN_SEMAPHORE, m_textures[i].acquireFence));
            const bool releaseOk = ARC_NRI_CHECK(m_device->Core().CreateFence(m_device->Device(), nri::SWAPCHAIN_SEMAPHORE, m_textures[i].releaseFence));
            if (!acquireOk || !releaseOk)
            {
                ARC_ERROR("[nri] swapchain per-image fence creation failed for image {}", i);
                return false;
            }
        }

        return true;
    }

    void NriSwapChain::ReleaseSwapChainObjects()
    {
        // Idle the queue before touching anything the GPU might still be
        // reading -- the swapchain's textures and per-image fences included.
        // Mirrors NRISamples' ResizeSwapChain() (Source/Resize.cpp), which
        // calls waitForIdle() for the identical reason.
        //
        // SKIPPED ON A LOST DEVICE. Same rule as
        // NriGraphContext's and ~NriDevice's: after an observed loss the idle
        // cannot make anything idle that is not already stopped -- it returns
        // DEVICE_LOST on VK and burns NRI_TIMEOUT_FENCE (5 s, SharedExternal.h
        // :53) on D3D12 before reporting SUCCESS regardless. NOTE this
        // function is also the RESIZE path, and resize runs on a healthy
        // device, where GpuDeviceLostObserved() is false and the idle happens
        // exactly as before -- the resize path is untouched.
        if (m_device && m_device->GraphicsQueue() && !GpuDeviceLostObserved())
            (void)ARC_NRI_CHECK(m_device->Core().QueueWaitIdle(m_device->GraphicsQueue()));

        for (TextureSlot& slot : m_textures)
        {
            // slot.texture is NOT ours to destroy: NRI's SwapChain owns
            // swapchain-image Texture objects and frees them itself inside
            // DestroySwapChain (SwapChainVK::~SwapChainVK / SwapChainD3D12's
            // destructor both destroy their own m_Textures). Only the fences
            // we created are ours.
            if (m_device)
            {
                if (slot.acquireFence) m_device->Core().DestroyFence(slot.acquireFence);
                if (slot.releaseFence) m_device->Core().DestroyFence(slot.releaseFence);
            }
        }
        m_textures.clear();

        if (m_swapChain)
        {
            m_swapChainInterface.DestroySwapChain(m_swapChain);
            m_swapChain = nullptr;
        }

        m_format               = nri::Format::UNKNOWN;
        m_acquired              = false;
        m_currentAcquireFence   = nullptr;
        m_currentTextureIndex   = 0;
    }

    void NriSwapChain::Resize(uint32_t width, uint32_t height)
    {
        if (width == m_width && height == m_height)
            return;

        // Caller-contract enforcement (see the .hpp's doc-comment on this
        // method): a resize landing between a successful AcquireNextTexture()
        // and its Present() would free the nri::Texture* already handed to
        // the caller inside DestroySwapChain below, with no API signal --
        // a silent dangling pointer. No cheap structurally-safe fix exists,
        // so this is a loud
        // contract violation rather than a handled case: fatal in debug
        // (ARC_ASSERT, compiled out in release -- Mosaic/Assert.hpp), and an
        // unconditional ARC_WARN on the release path so the violation is
        // never silent in ANY config, matching Graveyard's own
        // debug-fatal/release-warn idiom (Graveyard.cpp's ~Graveyard()).
#if defined(ARC_BUILD_DEBUG)
        ARC_ASSERT(!m_acquired,
                    "NriSwapChain::Resize: called with an outstanding un-Presented "
                    "AcquireNextTexture() -- sequence Resize() at frame boundaries only");
#else
        if (m_acquired)
        {
            ARC_WARN("[nri] NriSwapChain::Resize: called with an outstanding un-Presented "
                     "AcquireNextTexture() -- the acquired texture is about to dangle; "
                     "sequence Resize() at frame boundaries only");
        }
#endif

        m_width  = width;
        m_height = height;

        // No swapchain-resize API exists on nri::SwapChainInterface (create/
        // destroy only) -- destroy+recreate is the only path (spec ruling;
        // see the .hpp's header comment; NRISamples' ResizeSwapChain() does
        // the same). The pacing fence and frame counter deliberately survive
        // this call: ReleaseSwapChainObjects() only clears swapchain-local
        // state, never m_frameFence/m_frameCounter, matching NRISamples'
        // ResizeSwapChain() (Source/Resize.cpp), which leaves m_FrameFence
        // untouched across the rebuild.
        ReleaseSwapChainObjects();
        if (!CreateSwapChainObjects())
        {
            // Return to the same safe "no swapchain" state a failed Create()
            // leaves it in, rather than a half-built one: CreateSwapChainObjects
            // can fail AFTER a successful CreateSwapChain (a per-image
            // CreateFence failing partway through its loop, above), which
            // would otherwise leave m_swapChain non-null with some
            // m_textures[i] fences still null -- and AcquireNextTexture()
            // dereferences the recycled slot's acquire fence unconditionally.
            // ReleaseSwapChainObjects() is safe to call again here: it tears
            // down whatever WAS partially built (null fences are skipped,
            // see above) and clears m_swapChain, so AcquireNextTexture()'s
            // `if (!m_swapChain || m_textures.empty())` guard skips frames
            // instead of crashing. The pacing fence and frame counter are
            // untouched by either call.
            ARC_ERROR("[nri] NriSwapChain::Resize: CreateSwapChainObjects failed at {}x{} "
                      "(backend={}); swapchain left null, frames will be skipped until "
                      "the next successful Resize()", m_width, m_height, ToString(m_device->Backend()));
            ReleaseSwapChainObjects();
        }
    }

    // -----------------------------------------------------------------
    // Per-frame: AcquireNextTexture / Present
    // -----------------------------------------------------------------

    nri::Texture* NriSwapChain::AcquireNextTexture()
    {
        if (!m_swapChain || m_textures.empty())
            return nullptr;   // zero-sized surface; caller skips the frame

        // Double-acquire without an intervening Present(): hand back the
        // texture already acquired rather than acquiring again. Both NRI
        // backends' AcquireNextTexture has a real side effect (consuming/
        // re-signalling the acquire fence, advancing an internal present id),
        // so a second call before Present() would clobber the first
        // acquire's un-consumed fence -- the same VUID-class hazard
        // SwapchainVulkan::BeginFrame used to guard against (in the deleted
        // DeviceVulkan.cpp: "re-acquiring would reuse the already-signaled
        // binary semaphore").
        if (m_acquired)
            return m_textures[m_currentTextureIndex].texture;

        // Pacing wait: skip entirely (no call at all, not even a trivially-
        // true one) for the first FramesInFlight() frames, rather than
        // NRISamples' literal shape of always calling Wait() with a value of 0
        // for early frames.
        const uint32_t framesInFlight = FramesInFlight();
        if (m_frameCounter >= framesInFlight)
        {
            const uint64_t waitValue = m_frameCounter - framesInFlight + 1;
            PollingWaitForTimelineFence(m_device->Core(), m_frameFence, waitValue);
        }

        // Recycled by FRAME index, not by the image index Acquire returns --
        // the acquire fence handed to AcquireNextTexture must not still be in
        // flight, and NRISwapChain.h's own usage example indexes it exactly
        // this way ("recycledSemaphoreIndex = frameIndex % textureNum"),
        // valid here because textureNum (FramesInFlight() + 1) is
        // strictly greater than FramesInFlight().
        const uint32_t recycled = (uint32_t)(m_frameCounter % m_textures.size());
        nri::Fence* acquireFence = m_textures[recycled].acquireFence;

        uint32_t textureIndex = 0;
        const nri::Result result = m_swapChainInterface.AcquireNextTexture(*m_swapChain, *acquireFence, textureIndex);
        if (result == nri::Result::OUT_OF_DATE)
        {
            // VK only -- a negative VK_ERROR_OUT_OF_DATE_KHR (ConversionVK.h).
            // Expected and self-healing: the window's resize EVENT (not this
            // return code) is what drives Resize(), so by the time the
            // caller's NEXT frame acquires, Resize() should already have run.
            // Routine, not latch-worthy -- exactly how SwapchainVulkan's own
            // OutOfDateKHRError catch treated it (in the deleted
            // DeviceVulkan.cpp: no RenderErrorCount bump). Deliberately NOT routed through
            // ARC_NRI_CHECK for that reason -- see the .hpp's header comment
            // for the full SUBOPTIMAL/OUT_OF_DATE reasoning.
            ARC_WARN("[nri] AcquireNextTexture: swapchain out of date at {}x{}, "
                     "skipping frame (awaiting Resize())", m_width, m_height);
            return nullptr;
        }
        if (!ARC_NRI_CHECK(result))
            return nullptr;   // genuine failure; ARC_NRI_CHECK already logged the result name

        m_currentTextureIndex = textureIndex;
        m_currentAcquireFence = acquireFence;
        m_acquired             = true;
        return m_textures[textureIndex].texture;
    }

    nri::Fence* NriSwapChain::CurrentReleaseFence() const
    {
        return m_acquired ? m_textures[m_currentTextureIndex].releaseFence : nullptr;
    }

    void NriSwapChain::Present()
    {
        if (!m_acquired || !m_swapChain)
            return;
        m_acquired = false;

        nri::Fence* releaseFence = m_textures[m_currentTextureIndex].releaseFence;
        const nri::Result result = m_swapChainInterface.QueuePresent(*m_swapChain, *releaseFence);
        if (result == nri::Result::OUT_OF_DATE)
        {
            // Same routine, non-latching treatment as the acquire-side check
            // above -- present-time OUT_OF_DATE is the same VK-only, resize-
            // driven condition (SwapChainVK::Present funnels through the
            // identical NRI_RETURN_ON_BAD_VKRESULT macro as AcquireNextTexture).
            ARC_WARN("[nri] QueuePresent: swapchain out of date at {}x{} (awaiting Resize())", m_width, m_height);
        }
        else
        {
            (void)ARC_NRI_CHECK(result);   // genuine failure already logged; nothing else to do here
        }

        // Deliberately UNCONDITIONAL past this point: the pacing signal and
        // frameCounter advance even on a genuine QueuePresent failure. The
        // slot this frame's pacing value represents was already consumed by
        // AcquireNextTexture() (its acquire fence was handed out), so
        // skipping the signal here would leave that slot's pacing wait
        // permanently unsatisfiable on a future frame -- worse than the
        // pacing counter briefly describing a frame whose present failed.
        // Frame-pacing signal: a small QueueSubmit carrying only the pacing
        // fence's signal -- "signal at submit". Mirrors NRISamples' trailing
        // signal-only submit ("Signaling after Present improves D3D11
        // performance a bit", Source/Triangle.cpp/Resize.cpp) and
        // SwapchainVulkan::Present's own empty-submit semaphore flush, back
        // when that class existed (in the deleted DeviceVulkan.cpp) -- same
        // idiom, NRI's fence model instead of a raw VkSemaphore.
        nri::FenceSubmitDesc signal = {};
        signal.fence = m_frameFence;
        signal.value = m_frameCounter + 1;   // 1-based signal values, matching NRISamples ("1 + frameIndex")

        nri::QueueSubmitDesc submit = {};
        submit.signalFences   = &signal;
        submit.signalFenceNum = 1;
        (void)ARC_NRI_CHECK(m_device->Core().QueueSubmit(*m_device->GraphicsQueue(), submit));

        ++m_frameCounter;
    }

    std::uint64_t NriSwapChain::CompletedFrameValue() const
    {
        // Survives Resize() along with the fence itself, so the counter this
        // publishes is monotone across a drag-storm -- which is exactly what
        // Diagnostics::GpuHeartbeat's rule requires of it (a counter that
        // reset on resize would look like GPU progress that never happened,
        // and then like a stall that never ended).
        if (!m_device || !m_frameFence)
            return 0;
        return m_device->Core().GetFenceValue(*m_frameFence);
    }

    // -----------------------------------------------------------------
    // Teardown
    // -----------------------------------------------------------------

    NriSwapChain::~NriSwapChain()
    {
        if (!m_device)
            return;

        ReleaseSwapChainObjects();

        if (m_frameFence)
        {
            m_device->Core().DestroyFence(m_frameFence);
            m_frameFence = nullptr;
        }
    }
}
