// GpuContext: the ordered engine boot extracted from main.cpp. See the header
// for the teardown contract. Create() below is the ONE factory.

#include <Arcane/Host/GpuContext.hpp>

#include <Arcane/Base/ForeignModules.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Host/HostSettings.hpp>   // render.window.* (Restart: read at window creation)
#include <Arcane/Render/RenderDeviceSettings.hpp>   // render.vulkan.foreignModuleFallback

#include <string>
#include <vector>

namespace Arcane
{
    namespace
    {
        // The window shape: hidden until the first presented frame (the
        // caller reveals it via Window::Show once the render vehicle that
        // owns this window's only swapchain exists). The 1280x720 default is
        // load-bearing for anything that compares captured frames: change it
        // and two captures differ in DIMENSION rather than in pixels, which
        // reads as a renderer change and is not one.
        WindowDesc HostWindowDesc(const HostConfig& cfg)
        {
            WindowDesc wd;
            wd.title  = kHostWindowTitle;
            wd.vulkan = (cfg.backend == GraphicsBackend::Vulkan);
            wd.hidden = true;
            // render.window.* (settings arc S6-24): the configured extent; its
            // default is the load-bearing 1280x720 above.
            const RenderWindowSettings& window = Settings<RenderWindowSettings>();
            wd.width     = window.width;
            wd.height    = window.height;
            wd.resizable = window.resizable;
            // --window-size WxH (automation only, T3-D6 fix round 1): the
            // explicit extent, over the setting.
            if (cfg.windowWidth != 0 && cfg.windowHeight != 0)
            {
                wd.width  = cfg.windowWidth;
                wd.height = cfg.windowHeight;
            }
            return wd;
        }
    }

    std::unique_ptr<GpuContext> GpuContext::Create(HostConfig& cfg)
    {
        // Private ctor -> can't use make_unique; the partial unwinds via RAII on any
        // early return (members destruct in reverse declaration order, the shutdown
        // order).
        auto ctx = std::unique_ptr<GpuContext>(new GpuContext());

        // BEFORE the window, so SDL_WINDOW_VULKAN is never set on a session
        // that must not call the hooked loader. The scan is the same
        // enumeration Report() runs after the device exists; doing it here
        // is what makes the refusal earlier than the fast-fail. Headless
        // stays on the requested backend: its device is offscreen and the
        // measured crash is the windowed swapchain. render.vulkan.
        // foreignModuleFallback off (a developer reproducing the crash) skips
        // the fallback and keeps Vulkan.
        if (!cfg.headless && cfg.backend == GraphicsBackend::Vulkan &&
            Settings<RenderVulkanSettings>().foreignModuleFallback)
        {
            const std::vector<ForeignModules::LoadedModule> loaded = ForeignModules::EnumerateProcessModules();
            std::vector<std::string> names;
            names.reserve(loaded.size());
            for (const ForeignModules::LoadedModule& module : loaded)
                names.push_back(module.name);
            if (const std::optional<ForeignModules::Match> blocker = ForeignModules::WindowedVulkanBlocker(names))
            {
                ARC_WARN("[foreign-module] {} is injected. A windowed Vulkan swapchain fast-fails "
                         "inside vulkan-1.dll (0xC0000409 STATUS_STACK_BUFFER_OVERRUN) under that "
                         "hook, which is not a refcount the D3D12 reference armor can absorb. This "
                         "session continues on D3D12. {}",
                         blocker->module, blocker->remedy);
                cfg.backend = GraphicsBackend::D3D12;
            }
        }

        // THE window of the process, created first for the reason stated in
        // the header: it destructs LAST, and the NRI swapchain the caller
        // builds over it must be gone before it is.
        if (!ctx->m_window.Create(HostWindowDesc(cfg)))
        {
            ARC_ERROR("GpuContext: window create failed");
            return nullptr;
        }

        // A CPU BATCHER, and there is no other kind: it takes no device and
        // creates no GPU object. Begin/SetLayer/Quad*/Drain/RegisterMaterial/
        // SetGlobals/MaterialDesc/Stats are the whole data-supply side of the
        // frame; the
        // caller's Batch2DNode DRAINS this instance and issues the draws
        // through NRI. One batcher, one batching algorithm.
        ctx->m_batcher = Batcher2D::Create();
        if (!ctx->m_batcher) { ARC_ERROR("GpuContext: batcher create failed"); return nullptr; }

        // ImGuiLayer has ONE flavor: context + SDL3 platform backend + event
        // tap, and no renderer -- a graph node is the renderer.
        const HostOverlayHooks overlayHooks = GetHostOverlayHooks();
        if (!overlayHooks.create)
        {
            static bool warned = false;
            if (!warned) { warned = true; ARC_WARN("[gpu-context] host overlay requested without a hook"); }
        }
        ctx->m_imgui = overlayHooks.create ? overlayHooks.create(ctx->m_window)
                                           : CreateNullHostOverlay();
        if (!ctx->m_imgui) { ARC_ERROR("GpuContext: imgui create failed"); return nullptr; }

        ctx->m_inputDevices = InputDevices::Create();
        if (!ctx->m_inputDevices) { ARC_ERROR("GpuContext: input devices create failed"); return nullptr; }
        ctx->m_input = InputActions::Create();
        if (!ctx->m_input) { ARC_ERROR("GpuContext: input actions create failed"); return nullptr; }
        // The input-actions CONFIG is loaded by the host AFTER it opens a project, so the
        // config can resolve through the project's game:// mount (or data/ when no project).
        // GpuContext only creates the empty action system here. See HostBoot::LoadInputConfig.

        return ctx;
    }
}
