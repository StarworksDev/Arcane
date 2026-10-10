#pragma once

// Render module: WHAT KIND OF DEVICE to create -- backend plus the three
// validation switches. Creation itself is headless by design: a swapchain is
// created separately against a Window, so tools and tests can run
// compute/offscreen work without any window.
//
// It lives in a header of its own, with no graphics-API dependency, because
// it is on the render path's critical route: Nri/NriDevice's
// NativeDeviceOwner::Create takes one, and so do both creation halves
// (DeviceCreationD3D12.hpp / DeviceCreationVulkan.hpp).

#include <Arcane/Render/GraphicsBackend.hpp>

namespace Arcane
{
    // The members are a PLAIN request: all three validation switches default
    // off here. An engine host's device comes from MakeRenderDeviceDesc()
    // (RenderDeviceSettings.hpp), which fills them from render.debug.* --
    // on in Debug builds, off otherwise, and overridable without a rebuild.
    // A hand-built desc (the [gpu] test vehicles, the capability probe) sets
    // what it needs.
    struct RenderDeviceDesc
    {
        GraphicsBackend backend = kDefaultGraphicsBackend;   // D3D12 on Windows, Vulkan elsewhere

        // NRI validation layer + Vulkan validation (render.debug.validation).
        bool enableValidation = false;

        // D3D12 CPU debug layer (EnableDebugLayer; render.debug.d3d12DebugLayer).
        // D3D12SDKLayers.dll raises RaiseFailFastException (code 0x87D) when
        // third-party window hooks (e.g. Nahimic OSD) are loaded, and it can
        // only be armed before the process's first device. NRI's own
        // validation layer covers command-level errors.
        bool enableD3D12DebugLayer = false;

        // Vulkan SYNCHRONIZATION validation (render.debug.vkSyncValidation),
        // on top of the ordinary VK_LAYER_KHRONOS_validation core checks
        // `enableValidation` turns on. Vulkan-only -- D3D12's debug layer has
        // no separate sync-validation switch (its closest analogue,
        // GPU-Based Validation, is a different and far costlier thing), so
        // `enableD3D12DebugLayer` above is the whole D3D12 story. It catches
        // hazards in hand- or graph-derived barrier placement, which core
        // validation does not; it is expensive and false-positive-prone on a
        // full engine frame, which is why it is its own switch rather than
        // folded into `enableValidation`.
        //
        // Requires `enableValidation` (it configures the validation layer; with
        // no layer loaded there is nothing to configure) and the
        // VK_EXT_validation_features instance extension. Missing either is a
        // WARN and a degrade, never a create failure -- see
        // DeviceCreationVulkan.cpp's CreateVulkanNativeDevice.
        bool enableSyncValidation = false;
    };
}
