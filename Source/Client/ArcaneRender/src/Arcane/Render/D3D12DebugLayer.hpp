#pragma once

// Process-wide D3D12 debug layer. The per-device switch
// (RenderDeviceDesc::enableD3D12DebugLayer) is too late once any device in
// the process has been created with the layer off: EnableDebugLayer is
// documented to remove a device that already exists. This is the one call
// that turns the layer on before that window closes, and then requires every
// later device to be treated as debug-layer-active.

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Platform/Platform.hpp>

namespace Arcane
{
#if ARC_PLATFORM_WINDOWS
    // EnableDebugLayer, then record that every subsequent device in this
    // process is debug-layer-active: DXGI_CREATE_FACTORY_DEBUG, the InfoQueue1
    // callback, and the RenderErrorCount latch, whatever that device's
    // enableD3D12DebugLayer says.
    //
    // Legal only while this process owns no D3D12 device. A later call returns
    // false and logs ARC_ERROR without calling EnableDebugLayer. Production
    // hosts do not call this; they follow render.debug.d3d12DebugLayer.
    // Returns false when the layer itself is unavailable.
    [[nodiscard]] ARC_RENDER_API bool ForceD3D12DebugLayerForProcess();
#endif
}
