// Non-Windows twin of the D3D12-only device-removed hooks that live in
// DeviceCreationD3D12.cpp (a Windows-target TU, excluded from a Linux build by
// premake5.lua). NriDiagnostics.cpp still names them in its per-backend
// switch, so a D3D12-less build must still DEFINE them: a D3D12 device can
// never exist there, so "never removed" and empty observers are exact, not
// placeholders. Compiles to nothing on Windows (the real definitions win).
#include <Arcane/Platform/Platform.hpp>

#if !ARC_PLATFORM_WINDOWS

#include <Arcane/Render/DeviceRemovedObservers.hpp>

namespace Arcane
{
    void ObserveDeviceRemovedD3D12() {}
    void ResetDeviceRemovedLatchD3D12() {}
    bool D3D12NativeDeviceRemoved(void*) noexcept { return false; }
}

#endif
