#include "GpuCapability.hpp"

#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>

#include <optional>

namespace Arcane::Test
{
    namespace
    {
        std::optional<bool> g_d3d12;
        std::optional<bool> g_vulkan;

        bool Probe(GraphicsBackend backend)
        {
            // A real creation attempt, because that is the only question that
            // matters -- an adapter that enumerates but cannot create a device
            // is not an available backend.
            //
            // The probe asks one question: can a device be created. It does not
            // request NRI validation or Vulkan sync validation, and it must not
            // be the call that turns the D3D12 debug layer on -- it is often
            // the process's first device, and a true flag here would hide a
            // later device that never got EnableDebugLayer. ArcaneTests arms
            // that layer earlier, process-wide, from test_main
            // (ForceD3D12DebugLayerForProcess). When that force is set, this
            // device is serviced anyway; the flag here stays false.
            RenderDeviceDesc desc;
            desc.backend               = backend;
            desc.enableValidation      = false;
            desc.enableD3D12DebugLayer = false;
            desc.enableSyncValidation  = false;
            auto owner = NativeDeviceOwner::Create(desc);
            return owner != nullptr;
        }
    }

    const char* BackendName(GraphicsBackend backend)
    {
        return ToString(backend);
    }

    bool BackendAvailable(GraphicsBackend backend)
    {
        std::optional<bool>& slot =
            (backend == GraphicsBackend::Vulkan) ? g_vulkan : g_d3d12;
        if (!slot.has_value())
            slot = Probe(backend);
        return *slot;
    }

    void ReleaseSharedTestGpuDevices()
    {
        // Probe's NativeDeviceOwner is a local and is destroyed before Probe
        // returns, so that device's stored messages are latched inside the
        // case that first asked, and EndCase samples them. MeshNodeTest's
        // parity vehicle used to be a function-local static; it is a case
        // local now because a static destructor runs after main. Nothing else
        // under Tests/ArcaneTests/src/Helpers keeps an NriDevice.
    }
}
