#pragma once

// Render module: WHICH graphics API this process is running on.
//
// It lives in a header of its own, with no graphics-API dependency at all,
// so a consumer that only needs to know "which backend?" -- ShaderPaths (whose
// whole job is picking dxil-or-spirv), the host's HostConfig -- pulls in
// nothing else.
//
// Deliberately NOT an "RHI backend" enum with a NONE/Null member: the value
// answers "dxil or spirv, D3D12 or Vulkan" for shader paths, launch arguments
// and log banners. NriDevice::
// CreateNoneForTests states the other half of that rule -- it reports D3D12
// for a NONE device, because "GraphicsBackend has no NONE value and inventing
// one would leak a test-only concept into every backend switch in the tree"
// (Nri/NriDevice.hpp).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Platform/Platform.hpp>

#include <cstdint>

namespace Arcane
{
    enum class GraphicsBackend : uint8_t
    {
        D3D12,
        Vulkan,
    };

    ARC_API const char* ToString(GraphicsBackend backend);

    // The backend a host runs on when nothing asks for another one: D3D12 on
    // Windows (unchanged), Vulkan everywhere else -- the only backend a
    // non-Windows build has (Linux port, 2026-10-05). kDefaultBackendCliName
    // is the same choice spelled as the host CLI's --backend value.
#if ARC_PLATFORM_WINDOWS
    inline constexpr GraphicsBackend kDefaultGraphicsBackend = GraphicsBackend::D3D12;
    inline constexpr const char*     kDefaultBackendCliName  = "dx12";
#else
    inline constexpr GraphicsBackend kDefaultGraphicsBackend = GraphicsBackend::Vulkan;
    inline constexpr const char*     kDefaultBackendCliName  = "vulkan";
#endif
}
