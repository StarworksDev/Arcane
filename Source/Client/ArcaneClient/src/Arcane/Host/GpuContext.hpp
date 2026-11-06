#pragma once
#include <Arcane/Modules/ArcaneClientApi.hpp>

// GpuContext: the host-owned platform/render/input stack, extracted out of
// main.cpp so the boot sequence + the (load-bearing) teardown order live in one
// place. Create() runs the ordered boot (window -> device-less batcher ->
// imgui -> inputDevices -> input); on any fallible step it logs ARC_ERROR +
// returns nullptr, and the partially-built unique_ptr<GpuContext> unwinds
// (RAII) in reverse member declaration order -- the same order the destructor
// would use at shutdown.
//
// MEMBER DECLARATION ORDER IS THE TEARDOWN CONTRACT -- window first (destructs
// LAST: imgui/input hold SDL window refs); the render/input stack outlives
// runtime/plugin (which a host owns + declares after a GpuContext). Do NOT
// reorder.
//
// =====================================================================
// ONE FLAVOR, AND NO FLAVOR PREDICATE
// =====================================================================
// Create() builds the window (which the caller's NRI render vehicle then
// binds: ONE window per process, DXGI permits one flip-model swapchain per
// HWND), a DEVICE-LESS Batcher2D, ImGuiLayer, and the input stack. NOTHING
// ELSE -- there is no render device, swapchain, shader library, canvas or
// command list anywhere in this object, so there is nothing for a flavor
// predicate to select between.

#include <Arcane/Host/HostOverlay.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputDevices.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Batcher2D.hpp>

#include <Arcane/Host/HostConfig.hpp>

#include <memory>

namespace Arcane
{
    // The host window's title as Create() makes it. The editor replaces it
    // with its own; the runtime keeps it, plus " [held]" while a --remote
    // tick gate holds its sim (Servitor P0 Task 29, Remote::HeldWindowTitle).
    inline constexpr const char* kHostWindowTitle = "Arcane Runtime";

    class ARC_CLIENT_API GpuContext
    {
    public:
        // Runs the ordered boot into the members. Returns null (with ARC_ERROR on
        // the failing step) when any stage fails -- the partial unwinds via RAII in
        // reverse member order, the same order ~GpuContext uses at shutdown.
        //
        // The window is created HIDDEN; the caller reveals it (Window::Show)
        // once the render vehicle that owns this window's only swapchain
        // exists.
        //
        // May rewrite cfg.backend from Vulkan to D3D12 before the window
        // exists (unless render.vulkan.foreignModuleFallback is off).
        // Windowed Vulkan plus GTIII-OSD64-VK.dll fast-fails inside
        // vulkan-1.dll at swapchain creation; D3D12 is the path whose device
        // reference armor survives that injector family. Headless is left on
        // Vulkan: it never builds the swapchain the hook kills.
        static std::unique_ptr<GpuContext> Create(HostConfig& cfg);

        Window&        Win()       { return m_window; }
        Batcher2D&     Batch()     { return *m_batcher; }
        HostOverlay&  Imgui()     { return *m_imgui; }
        InputDevices&  InDevices() { return *m_inputDevices; }
        InputActions&  Input()     { return *m_input; }

    private:
        GpuContext() = default;

        // --- TEARDOWN CONTRACT: declaration order == reverse destruction order ---
        // Do NOT reorder. See the file-header comment for the why of each position.

        // Window first: it destructs LAST (imgui + inputDevices tap the SDL window).
        // It also outlives the borrowing render vehicle, which the host guarantees
        // by declaring that object AFTER its GpuContext.
        Window m_window;

        std::unique_ptr<Batcher2D>     m_batcher;
        std::unique_ptr<HostOverlay>    m_imgui;
        std::unique_ptr<InputDevices>  m_inputDevices;
        std::unique_ptr<InputActions>  m_input;
    };
}
