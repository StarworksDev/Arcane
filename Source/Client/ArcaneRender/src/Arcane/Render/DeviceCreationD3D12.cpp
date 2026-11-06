// The D3D12 CREATION HALF: DXGI factory + adapter + device + direct queue,
// the process-global debug-layer sequencing around them, DRED enablement, and
// the InfoQueue arming that gives the D3D12 debug layer a channel into our log
// and the error latch. Everything here is native D3D12.
//
// See DeviceCreationD3D12.hpp for the consumer shape and the member-order
// rule. The Vulkan twin (CreateVulkanNativeDevice, and the VkDebugCallback
// that is this file's messenger analogue) lives in DeviceCreationVulkan.cpp
// -- the tree is symmetric.

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/AgilitySdk.hpp>
#include <Arcane/Render/D3D12DebugLayer.hpp>
#include <Arcane/Render/DeviceCreationD3D12.hpp>
#include <Arcane/Render/DeviceRemovedObservers.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // NoteGpuDeviceLost -- the host's device-lost latch
#include <Arcane/Render/IGpuCrashBackend.hpp>   // EnableD3D12Dred -- the F-2 DRED tier, armed before D3D12CreateDevice
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>   // render.adapter, render.debug.*, render.d3d12.deviceArmorRefs

#include <d3d12.h>
#include <d3d12sdklayers.h>   // DXGI_DEBUG_D3D12 -- the D3D12 layer's producer GUID in the DXGI info queue
#include <dxgi1_6.h>
#include <dxgidebug.h>        // IDXGIInfoQueue / DXGIGetDebugInterface1 -- the DXGI debug layer's own channel
#include <wrl/client.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Arcane
{
    namespace
    {
        // ------------------------------------------------------------------
        // Process-global D3D12 debug-layer state.
        // ------------------------------------------------------------------
        // ID3D12Debug::EnableDebugLayer is a BEFORE-ANY-DEVICE call, and the
        // documentation is explicit about what happens otherwise: "To enable
        // the debug layers using this API, it must be called before the D3D12
        // device is created. Calling this API after creating the D3D12 device
        // will cause the D3D12 runtime to remove the device."
        // (learn.microsoft.com, ID3D12Debug::EnableDebugLayer, Remarks.)
        //
        // A process that creates a device with the layer OFF (the flag
        // defaults false -- RenderDeviceDesc.hpp, the Nahimic-OSD fail-fast
        // hazard) and only THEN runs this creation half again with
        // enableD3D12DebugLayer true lands EnableDebugLayer on a process that
        // already owns a live device: D3D12CreateDevice fails and the run
        // exits after 0 frames. That is observed behaviour, not a worry.
        //
        // These flags make the sequencing structural rather than a rule
        // somebody has to remember at each new call site.
        //
        // MONOTONE ON PURPOSE, and the conservative direction:
        // `g_d3d12DeviceCreated` is never cleared. Every owner
        // (NativeDeviceOwner) routes teardown through
        // DestroyD3D12NativeDevice, so a decrement IS expressible -- it is
        // deliberately not done, because the failure mode of being wrong here
        // is asymmetric. Skipping an enable that would have
        // been legal costs one diagnostic channel; making the call when it is
        // NOT legal removes a live device. Tests do create another device
        // after the first has been destroyed (the capability probe, then each
        // [gpu] case). The flag staying set is what drops the debug layer for
        // all of them once that first device was created with the layer off.
        // ForceD3D12DebugLayerForProcess is the opt-in that calls
        // EnableDebugLayer before that first device, so the closed window no
        // longer costs the channel. The flag is still never cleared: the
        // runtime removes a device if EnableDebugLayer runs after one has
        // existed, destroyed or not.
        std::atomic<bool> g_d3d12DeviceCreated{ false };
        std::atomic<bool> g_d3d12DebugLayerEnabled{ false };
        // Set by ForceD3D12DebugLayerForProcess. Independent of "the layer is
        // on": without this flag, a device whose enableD3D12DebugLayer is
        // false skips the callback and DXGI_CREATE_FACTORY_DEBUG even after
        // EnableDebugLayer has already run. With the flag, every later device
        // is debug-layer-active.
        std::atomic<bool> g_d3d12DebugLayerForced{ false };

        // The DXGI debug layer's info queue. PROCESS-GLOBAL like the layer
        // itself (DXGIGetDebugInterface1 hands out one queue per process, not
        // per factory or device), so it lives here beside the other
        // process-global debug-layer state and not in D3D12DeviceCreation.
        //
        // Why it has to be armed at all: the D3D12 InfoQueue disarm further
        // down covers the D3D12 layer's break-on-severity ONLY. The DXGI
        // layer keeps its own queue with its own break flags, and those
        // default to break-on-ERROR -- so any DXGI ERROR (and, on the Windows
        // 10 in-box D3D12SDKLayers.dll, which reports THROUGH DXGIDebug.dll's
        // DXGI_SDK_MESSAGE, some D3D12 ones) ends in DebugBreak(), which with
        // no debugger attached is an unhandled STATUS_BREAKPOINT (0x80000003)
        // and kills the process with the message text delivered to nobody.
        // Observed at the desk 2026-09-22: the editor's ordinary Shutdown
        // after a game-module rebuild died exactly that way inside
        // ~DeviceD3D12, with a minidump that carried no message string.
        //
        // Same policy as the D3D12 half: the host does the reporting. Break
        // off on all three severities, and the stored messages are DRAINED
        // into the log and the RenderErrorCount latch by
        // DrainDxgiDebugMessages below, so a DXGI ERROR fails the 0/0 gate
        // exactly like a D3D12 or NRI one instead of aborting the process.
        //
        // Null when the debug layer was never requested or DXGIDebug.dll is
        // not on the machine (it ships with the Graphics Tools optional
        // feature, not with Windows proper) -- DrainDxgiDebugMessages is a
        // no-op then.
        ComPtr<IDXGIInfoQueue> g_dxgiInfoQueue;
        std::atomic<bool>      g_dxgiInfoQueueProbed{ false };

        // The device reference armor (defined with its rationale further
        // down, beside the teardown that audits it).
        void ArmorD3D12Device(D3D12DeviceCreation& out);
        void ReleaseArmoredD3D12Device(D3D12DeviceCreation& creation);

        const char* DxgiDebugProducerName(const GUID& producer)
        {
            if (producer == DXGI_DEBUG_DXGI)
                return "dxgi";
            if (producer == DXGI_DEBUG_D3D12)
                return "d3d12";
            if (producer == DXGI_DEBUG_APP)
                return "app";
            return "unknown-producer";
        }

        const char* D3D12DebugSeverityName(D3D12_MESSAGE_SEVERITY severity)
        {
            switch (severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
            case D3D12_MESSAGE_SEVERITY_ERROR:      return "ERROR";
            case D3D12_MESSAGE_SEVERITY_WARNING:    return "WARNING";
            case D3D12_MESSAGE_SEVERITY_INFO:       return "INFO";
            case D3D12_MESSAGE_SEVERITY_MESSAGE:    return "MESSAGE";
            }
            return "?";
        }

        const char* DxgiDebugSeverityName(DXGI_INFO_QUEUE_MESSAGE_SEVERITY severity)
        {
            switch (severity)
            {
            case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
            case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR:      return "ERROR";
            case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING:    return "WARNING";
            case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO:       return "INFO";
            case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_MESSAGE:    return "MESSAGE";
            }
            return "?";
        }

        // Once per process, and only when the D3D12 debug layer is going on:
        // the DXGI debug layer is what DXGI_CREATE_FACTORY_DEBUG turns on, so
        // the two are armed together. Failure is a diagnostics degradation,
        // never a create failure.
        //
        // WHAT THIS DOES NOT COVER, stated because it was measured: the
        // layers' CORRUPTION-class reports -- a call on an object the layer
        // knows to be destroyed -- DebugBreak() out of DXGIDebug!
        // DXGI_SDK_MESSAGE regardless of every break switch (all read back
        // as off at the moment of such a break, desk 2026-09-22), and
        // resuming past one lands on the corrupted object as an access
        // violation. That break is protective and stays. What it guarded on
        // this desk is RepairForeignDeviceOverRelease below.
        void ArmDxgiDebugQueue(BreakSeverity breakOn)
        {
            if (g_dxgiInfoQueueProbed.exchange(true, std::memory_order_acq_rel))
                return;

            const HRESULT hr = DXGIGetDebugInterface1(0, IID_PPV_ARGS(&g_dxgiInfoQueue));
            if (FAILED(hr) || !g_dxgiInfoQueue)
            {
                g_dxgiInfoQueue.Reset();
                ARC_WARN("DXGI debug info queue unavailable (DXGIGetDebugInterface1 hr=0x{:08X}; "
                         "DXGIDebug.dll absent?); DXGI debug-layer messages will not reach the log",
                         static_cast<uint32_t>(hr));
                return;
            }

            // The DXGI twin of the ID3D12InfoQueue disarm below. DXGI_DEBUG_ALL
            // is every producer the queue knows -- DXGI itself, the D3D12
            // layer's messages that travel through it, and the app's.
            // render.debug.breakOnSeverity (default None: never break).
            g_dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION,
                                                BreaksOn(breakOn, BreakSeverity::Corruption) ? TRUE : FALSE);
            g_dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR,
                                                BreaksOn(breakOn, BreakSeverity::Error) ? TRUE : FALSE);
            g_dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING,
                                                BreaksOn(breakOn, BreakSeverity::Warning) ? TRUE : FALSE);
        }

        // F-3: the ONE device-removed observation point for this backend.
        // It is reached through RenderErrorLatch's hook slot, which
        // Render/Nri/NriDiagnostics::Arm fills with ObserveDeviceRemovedD3D12
        // below. Two producers drive that slot: the latch's "Device Removed"
        // substring scan (NoteNriError -- what NriCommon's RouteNriError
        // funnels every ARC_NRI_CHECK failure into), and its TYPED seam
        // NoteDeviceLost, which is what NriDiagnostics' `--crash-gpu`
        // removal poll calls once GetDeviceRemovedReason answers.
        //
        // Once-only per armed device: a removed device keeps reporting removal
        // on every submit, and the second report is worthless -- the marker
        // buffer and DRED state belong to the FIRST one. Reset when a new
        // backend arms (project switch recreates the device).
        std::atomic<bool> g_deviceRemovedReported{ false };

        void ObserveDeviceRemoved()
        {
            if (g_deviceRemovedReported.exchange(true, std::memory_order_acq_rel))
                return;

            // The reason string is load-bearing: Diagnostics::DeriveKind
            // classifies the .arcdiag "kind" by case-sensitive substring, and
            // only a reason containing lowercase "gpu" resolves to a gpu kind
            // (here "gpu-crash", which is what makes the .gpudump sibling get
            // written). Do not reword.
            Diagnostics::WriteReport("gpu-crash: device removed");

            // AFTER the report, deliberately: hosts poll this latch and shut
            // down on it, and "observed" must always mean "the report exists".
            NoteGpuDeviceLost();
        }

        // A failed ID3D12InfoQueue1 QueryInterface is not "System32's
        // d3d12SDKLayers.dll answered". That in-box binary (10.0.19041)
        // does not contain IID_ID3D12InfoQueue1
        // (2852dd88-b484-4c0c-b6b1-67168500e600). The vendored Agility
        // 1.619.3 layers DLL does, and both D3D12Core.dll and
        // d3d12SDKLayers.dll export D3D12SDKVersion 619, which is
        // AgilitySdk::kVersion -- the handshake this exe publishes. A
        // handshake miss fails device creation with
        // D3D12_ERROR_INVALID_REDIST before this line.
        //
        // What refuses the QI on Windows 10 is the OS build. The callback
        // interface is exposed from build 20236 up (that preview shipped as
        // Windows 11). Windows 10 is 1904x, and the Agility redist does not
        // turn the interface on there: the staged layers DLL is mapped, the
        // base ID3D12InfoQueue succeeds, InfoQueue1 still returns
        // E_NOINTERFACE. No other create path on this OS makes the QI
        // succeed. DrainD3D12DebugMessages in ~NriDevice is the delivery
        // that works, which is why the suite listener samples the latch
        // after the device is destroyed.
        //
        // The warning names the module that owns the device vtable and the
        // module that owns the info-queue vtable, plus each exported
        // D3D12SDKVersion. Those are address probes: GetModuleHandle by file
        // name returns whichever copy loaded first once two can be mapped.
        // The warning also logs that name probe for D3D12Core.dll, because
        // the vtable here is the layers DLL and a path on disk is not the
        // module the process loaded. An injected proxy shows up as a vtable
        // outside D3D12Core.dll / d3d12SDKLayers.dll.
        HMODULE ModuleContaining(const void* address)
        {
            if (!address)
                return nullptr;
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(address), &module))
                return nullptr;
            return module;
        }

        const void* ComVtable(IUnknown* object)
        {
            if (!object)
                return nullptr;
            const void* vtable = nullptr;
            std::memcpy(&vtable, object, sizeof(vtable));
            return vtable;
        }

        std::string ModulePathOr(HMODULE module, const char* fallback)
        {
            if (!module)
                return fallback;
            wchar_t wide[MAX_PATH]{};
            if (GetModuleFileNameW(module, wide, static_cast<DWORD>(std::size(wide))) == 0)
                return "path unavailable";
            char narrow[MAX_PATH * 2]{};
            size_t converted = 0;
            wcstombs_s(&converted, narrow, wide, _TRUNCATE);
            return narrow;
        }

        // -1: not loaded, or no D3D12SDKVersion export. The Agility core and
        // layers export it; the System32 loader DLL does not.
        int ExportedD3D12SdkVersion(HMODULE module)
        {
            if (!module)
                return -1;
            const auto* version =
                reinterpret_cast<const unsigned*>(GetProcAddress(module, "D3D12SDKVersion"));
            return version ? static_cast<int>(*version) : -1;
        }

        std::string SdkVersionText(int version)
        {
            return version < 0 ? std::string("absent") : std::to_string(version);
        }

        unsigned WindowsBuildNumber()
        {
            using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
            const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (!ntdll)
                return 0;
            const auto rtlGetVersion =
                reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
            if (!rtlGetVersion)
                return 0;
            OSVERSIONINFOEXW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtlGetVersion(&info) != 0)
                return 0;
            return info.dwBuildNumber;
        }

        // NRI capability contract item 12: the D3D12 debug layer's own channel
        // into our log and the RenderErrorCount latch.
        //
        // Why it has to be OURS: NRI never copies enableGraphicsAPIValidation
        // into its internal desc on the wrapper path, so its whole info-queue
        // block -- including ID3D12InfoQueue1::RegisterMessageCallback -- is
        // dead code for us, and its CallbackInterface carries NRI's own
        // messages only. Without this, D3D12 validation text reaches nothing:
        // the block below merely turns break-on-severity off. This is the one
        // channel by which a D3D12 VUID can fail the 0/0 gate, and it is the
        // exact counterpart of DeviceCreationVulkan.cpp's VkDebugCallback --
        // same sink, same severity split, so "an error happened" means one
        // thing on both backends.
        //
        // __stdcall by D3D12MessageFunc's typedef (d3d12sdklayers.h); the
        // calling convention must match exactly.
        void __stdcall D3D12DebugLayerCallback(D3D12_MESSAGE_CATEGORY /*category*/,
                                               D3D12_MESSAGE_SEVERITY severity,
                                               D3D12_MESSAGE_ID /*id*/,
                                               LPCSTR description,
                                               void* /*context*/)
        {
            const char* text = description ? description : "";
            switch (severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                // Same latch every other render-layer error producer
                // increments, so a raw D3D12 message fails the GPU tests
                // exactly like an NRI error -- but through NoteError, which
                // tags it "[d3d12]" (this text is the debug layer's, nobody
                // else's) and skips the device-removed substring hook. This
                // callback runs on whatever thread tripped the error, from
                // inside a D3D12 call; ObserveDeviceRemoved above writes a
                // report + minidump, and the producers that may fire it are
                // the latch's own two seams, not this one. See
                // RenderErrorLatch::NoteError for the full argument.
                RenderErrorLatch::Instance().NoteError("d3d12", text);
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                ARC_WARN("[d3d12] {}", text);
                break;
            case D3D12_MESSAGE_SEVERITY_INFO:
                // Reaches here only when render.debug.minSeverity is Info:
                // the default denies INFO at the storage filter.
                ARC_INFO("[d3d12] {}", text);
                break;
            default:
                // MESSAGE: the debug layer emits one per resource create and
                // destroy; it is denied at the storage filter.
                break;
            }
        }
    }

    // ----------------------------------------------------------------
    // Process-wide force. See D3D12DebugLayer.hpp for the contract.
    // ----------------------------------------------------------------
    bool ForceD3D12DebugLayerForProcess()
    {
        // Refuse before touching the layer. EnableDebugLayer after any device
        // has existed removes that device; a destroyed device does not reopen
        // the window (g_d3d12DeviceCreated is monotone).
        if (g_d3d12DeviceCreated.load(std::memory_order_acquire))
        {
            ARC_ERROR("ForceD3D12DebugLayerForProcess refused: a D3D12 device already exists in this "
                      "process. EnableDebugLayer must run before the first device; calling it later "
                      "removes that device.");
            return false;
        }

        if (!g_d3d12DebugLayerEnabled.load(std::memory_order_acquire))
        {
            // Same check-then-act as CreateD3D12NativeDevice, tightened to the
            // line above the call. Both are main-thread-only; a concurrent
            // creator would need its own serialization.
            if (g_d3d12DeviceCreated.load(std::memory_order_acquire))
            {
                ARC_ERROR("ForceD3D12DebugLayerForProcess refused: a D3D12 device already exists in this "
                          "process. EnableDebugLayer must run before the first device; calling it later "
                          "removes that device.");
                return false;
            }

            ComPtr<ID3D12Debug> debug;
            const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
            if (FAILED(hr) || !debug)
            {
                ARC_ERROR("ForceD3D12DebugLayerForProcess: D3D12 debug layer unavailable "
                          "(D3D12GetDebugInterface hr=0x{:08X})",
                          static_cast<uint32_t>(hr));
                return false;
            }
            debug->EnableDebugLayer();
            // Before the force flag, so a caller that observes the force flag
            // also observes that EnableDebugLayer has already run.
            g_d3d12DebugLayerEnabled.store(true, std::memory_order_release);
        }

        g_d3d12DebugLayerForced.store(true, std::memory_order_release);
        return true;
    }

    // ----------------------------------------------------------------
    // The CREATION HALF.
    // ----------------------------------------------------------------
    // The member ORDER inside D3D12DeviceCreation is COM release order in
    // reverse, which is what keeps teardown correct -- see the header's
    // member-order rule.
    //
    // It sits at namespace scope (outside the anonymous namespace above)
    // because DeviceCreationD3D12.hpp declares it for its one consumer,
    // Nri/NriDevice.cpp's NativeDeviceOwner.
    //
    // Failure leaves `out` holding whatever was created; the caller's teardown
    // releases it.
    bool CreateD3D12NativeDevice(const RenderDeviceDesc& desc, D3D12DeviceCreation& out)
    {
        // Recorded, not acted on: NRI's own validation layer is available in
        // wrapper mode (contract 2.1) and keys off this same switch. Pure
        // member write -- no call, no branch.
        out.enableValidation = desc.enableValidation;

        // The debug layer is PROCESS-GLOBAL state with a one-shot window (see
        // g_d3d12DeviceCreated above): it can only be turned on while this
        // process owns no D3D12 device, and turning it on later removes the
        // device that already exists. So this asks three questions in order --
        // is it already on, is it too late, otherwise turn it on -- rather than
        // enabling unconditionally. `debugLayerActive` is what the rest of this
        // function keys off, because "the caller asked for the layer" and "the
        // layer is servicing this device" stopped being the same thing here.
        // The process-wide force (ForceD3D12DebugLayerForProcess) is a third
        // answer to the same question: this device is serviced even when its
        // own enableD3D12DebugLayer is false.
        const bool wantDebugLayer = desc.enableD3D12DebugLayer
            || g_d3d12DebugLayerForced.load(std::memory_order_acquire);
        UINT factoryFlags     = 0;
        bool debugLayerActive = false;
        if (wantDebugLayer)
        {
            if (g_d3d12DebugLayerEnabled.load(std::memory_order_acquire))
            {
                // Already on process-wide: ForceD3D12DebugLayerForProcess, or an
                // earlier device's creation. Re-calling EnableDebugLayer is the
                // illegal post-device call AND would buy nothing: the layer
                // that is already loaded is the one that will service this
                // device too.
                debugLayerActive = true;
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            }
            else if (g_d3d12DeviceCreated.load(std::memory_order_acquire))
            {
                // THE SECOND-DEVICE CASE, and the tradeoff stated out loud.
                //
                // THIS BRANCH IS LIVE. ArcaneTests' capability probe
                // (Tests/ArcaneTests/src/Helpers/GpuCapability.cpp) creates the
                // process's first D3D12 device with enableD3D12DebugLayer left
                // false, which closes the EnableDebugLayer window
                // (g_d3d12DeviceCreated is monotone and is not cleared when
                // that device is destroyed). Every later device that asks for
                // the layer lands here: the suite logs "D3D12 debug layer NOT
                // enabled for this device" and D3D12DebugLayerCallback never
                // runs, so a CPU validation error cannot fail RenderErrorCount.
                // ArcaneTests closes that hole by calling
                // ForceD3D12DebugLayerForProcess before any device, which
                // takes the "already on" branch above instead. A process that
                // does not force, and whose first device left the layer off,
                // still lands here: a host with render.debug.d3d12DebugLayer
                // false, or ArcaneTests under ARCANE_TESTS_NO_D3D12_DEBUG=1.
                //
                // Such a device gets NO D3D12 debug layer: enabling it would
                // remove the live first device (and, observed at the desk,
                // makes the D3D12CreateDevice below fail outright), so the
                // choice is "one device short of a validation channel" vs "no
                // working device at all".
                //
                // WHAT IS LOST: D3D12 CPU validation messages for THIS device
                // never enter an info queue, so neither D3D12DebugLayerCallback
                // nor the stored-message drain can fail the RenderErrorCount
                // latch. NRI's own validation layer and (on Vulkan) the VK
                // validation layers are unaffected -- they are per-device, not
                // process-global. The loss is real where the Agility SDK layers
                // are loaded: ArcaneTests exports the handshake and stages
                // D3D12/D3D12SDKLayers.dll, so once the layer is actually on
                // those messages reach the latch -- through the InfoQueue1
                // callback when that interface registers, and through
                // DrainD3D12DebugMessages at device destruction when it does
                // not. On Windows 10 that callback does not register even when
                // the staged Agility layers are the ones loaded; the QI
                // failure further down says why. The drain is the path that
                // stays.
                //
                // HOW TO GET IT BACK: the layer has to be turned on before the
                // first device. ForceD3D12DebugLayerForProcess does that for
                // the whole process and then treats every later device as
                // debug-layer-active, including one whose own flag is false.
                // Setting enableD3D12DebugLayer on the first device only covers
                // later devices that ask for the layer themselves.
                ARC_WARN("D3D12 debug layer NOT enabled for this device: a D3D12 device already "
                         "exists in this process and EnableDebugLayer is documented to remove it "
                         "when called after device creation. This device's D3D12 validation "
                         "messages will not reach the log or the error latch.");
            }
            else
            {
                ComPtr<ID3D12Debug> debug;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                {
                    debug->EnableDebugLayer();
                    // Set BEFORE the create below, so the flag means "this
                    // process has called EnableDebugLayer" and not "a device
                    // came up afterwards" -- a failed create must not leave the
                    // next caller thinking the layer is still enablable.
                    g_d3d12DebugLayerEnabled.store(true, std::memory_order_release);
                    debugLayerActive = true;
                    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                }
                else
                {
                    ARC_WARN("D3D12 debug layer unavailable; continuing without it");
                }
            }
        }

        // BEFORE the factory: DXGI_CREATE_FACTORY_DEBUG is what switches the
        // DXGI debug layer on, and its queue must already have break-off set
        // when the first DXGI message can arrive.
        const RenderDebugSettings& debugSettings = Settings<RenderDebugSettings>();
        if (debugLayerActive)
            ArmDxgiDebugQueue(debugSettings.breakOnSeverity);

        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&out.factory))))
        {
            ARC_ERROR("CreateDXGIFactory2 failed");
            return false;
        }

        // render.adapter: -1 (the default) takes the high-performance
        // preference's first adapter; an index picks that DXGI adapter
        // (EnumAdapters1 order), falling back to the default when absent.
        if (const std::int32_t wanted = Settings<RenderSettings>().adapter; wanted >= 0)
        {
            ComPtr<IDXGIAdapter1> chosen;
            if (SUCCEEDED(out.factory->EnumAdapters1(static_cast<UINT>(wanted), &chosen)))
                out.adapter = chosen;
            else
                ARC_WARN("render.adapter {}: no DXGI adapter at that index; using the high-performance default", wanted);
        }
        if (!out.adapter && FAILED(out.factory->EnumAdapterByGpuPreference(
                0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&out.adapter))))
        {
            ARC_ERROR("No DXGI adapter found");
            return false;
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        out.adapter->GetDesc1(&adapterDesc);
        char name[128]{};
        size_t converted = 0;
        wcstombs_s(&converted, name, adapterDesc.Description, _TRUNCATE);
        out.adapterName = name;

        // F-2b: DRED settings are process-global and "you must configure
        // them prior to creating a Direct3D 12 Device" -- modifications
        // have no effect on devices already created. This must therefore
        // sit BEFORE D3D12CreateDevice, and it is deliberately independent
        // of enableD3D12DebugLayer (D3D12GetDebugInterface fetches the DRED
        // settings object without enabling the debug layer). Never fatal:
        // every failure inside degrades the tier and logs one WARN.
        //
        // NRI capability contract item 13: stays exactly here. NRI v180
        // contains no DRED code at all (zero matches for DRED /
        // AutoBreadcrumb / PageFault across its Source and Include), and it
        // never creates the device in wrapper mode, so it cannot clobber
        // this -- but only if the call keeps its position ahead of create.
        EnableD3D12Dred();

        // The HRESULT is in the message because this call's failure mode is
        // otherwise indistinguishable at the desk: D1 hit it three times in a
        // row with no way to tell "no 12_0 adapter" from "the runtime is in a
        // state that refuses to create one" -- SUSPECTED to be an
        // EnableDebugLayer-after-device call (see g_d3d12DeviceCreated), per
        // the MS docs cited above, but never confirmed beyond that one desk
        // repro; treat it as a working theory, not a diagnosed mechanism.
        // That theory also leans on an UNSTATED assumption: the check-then-act
        // read of g_d3d12DebugLayerEnabled/g_d3d12DeviceCreated above is two
        // independent atomic loads, not one transaction, so it is only race-
        // free if CreateD3D12NativeDevice is never entered from more than one
        // thread at a time. Nothing in the tree calls this off the main
        // thread today; a concurrent caller would need its own serialization.
        const HRESULT createHr = D3D12CreateDevice(out.adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                                   IID_PPV_ARGS(&out.device));
        if (FAILED(createHr))
        {
            ARC_ERROR("D3D12CreateDevice failed (feature level 12_0, hr=0x{:08X}) on '{}'",
                      static_cast<uint32_t>(createHr), out.adapterName);
            return false;
        }

        // From here on this process owns a device, so the EnableDebugLayer
        // window above is CLOSED for every later creation.
        g_d3d12DeviceCreated.store(true, std::memory_order_release);

        // Before anything else can hold or touch the count -- see
        // ArmorD3D12Device / ReleaseArmoredD3D12Device below.
        ArmorD3D12Device(out);

        // The device-side half of the debug layer. BOTH QueryInterface results
        // are kept, because which one fails IS the diagnosis: the base
        // ID3D12InfoQueue is implemented by the debug layer's device wrapper,
        // so failing it means the debug layer is not on this device at all,
        // while failing only ID3D12InfoQueue1 means the layer that answered is
        // too old for the callback interface. The old WARN here could tell
        // neither apart -- it discarded both HRESULTs -- and asserted a cause
        // ("pre-Agility D3D12 runtime") that the same run's "Using
        // ID3D12Device15" disproves. Each branch below now states only what it
        // actually knows.
        //
        // Keyed on `debugLayerActive`, NOT on desc.enableD3D12DebugLayer: when
        // the enable above was skipped because a device already existed, the
        // layer is genuinely absent from this device and every WARN in here
        // would be reporting our own decision back to us as a mystery.
        if (debugLayerActive)
        {
            ComPtr<ID3D12InfoQueue> infoQueue;
            const HRESULT infoQueueHr = out.device.As(&infoQueue);
            if (SUCCEEDED(infoQueueHr))
            {
                // The D3D12 debug layer defaults to break-on-error, which calls
                // __fastfail when the info queue receives a
                // D3D12_MESSAGE_SEVERITY_ERROR or CORRUPTION message. Route all
                // validation through the callback below (which logs at the
                // appropriate level and bumps the latch) rather than aborting
                // the process on first error -- unless
                // render.debug.breakOnSeverity asks for the break (default
                // None).
                const BreakSeverity breakOn = debugSettings.breakOnSeverity;
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION,
                                              BreaksOn(breakOn, BreakSeverity::Corruption) ? TRUE : FALSE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,
                                              BreaksOn(breakOn, BreakSeverity::Error) ? TRUE : FALSE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,
                                              BreaksOn(breakOn, BreakSeverity::Warning) ? TRUE : FALSE);

                // Deny what render.debug.minSeverity leaves out at the info
                // queue instead of dropping it in the callback -- by default
                // INFO and MESSAGE. The debug layer emits one of each per
                // resource create and destroy; filtering here means they are
                // never stored and never cross into D3D12DebugLayerCallback at
                // all. MESSAGE is always denied. The same setting is the
                // Vulkan messenger's mask (DeviceCreationVulkan.cpp, Error and
                // Warning by default) -- so "an error happened" and "the log
                // is quiet" mean one thing on both backends.
                const MinSeverity minSeverity = debugSettings.minSeverity;
                D3D12_MESSAGE_SEVERITY denied[3]{};
                UINT deniedCount = 0;
                if (!Reports(minSeverity, MinSeverity::Warning)) denied[deniedCount++] = D3D12_MESSAGE_SEVERITY_WARNING;
                if (!Reports(minSeverity, MinSeverity::Info))    denied[deniedCount++] = D3D12_MESSAGE_SEVERITY_INFO;
                denied[deniedCount++] = D3D12_MESSAGE_SEVERITY_MESSAGE;
                D3D12_INFO_QUEUE_FILTER filter{};
                filter.DenyList.NumSeverities = deniedCount;
                filter.DenyList.pSeverityList = denied;
                if (FAILED(infoQueue->PushStorageFilter(&filter)))
                {
                    ARC_WARN("ID3D12InfoQueue::PushStorageFilter failed; D3D12 INFO/MESSAGE "
                             "chatter will reach the debug-layer callback");
                }
                // Kept for the device's life. Where ID3D12InfoQueue1 does not
                // register (below), DrainD3D12DebugMessages reads the stored
                // messages through this.
                out.infoQueueBase = infoQueue;
            }

            // NRI capability contract item 12: turning break-off is all the
            // block above ever did -- the messages themselves went nowhere.
            // ID3D12InfoQueue1::RegisterMessageCallback is what actually
            // delivers them (see D3D12DebugLayerCallback). Missing it is a
            // diagnostics degradation, never a create failure.
            //
            // The device QI is the documented path. When it fails, ask the
            // info-queue object itself: a proxy that wraps the device and
            // forwards ID3D12InfoQueue but not InfoQueue1 still holds an
            // object whose own QI may succeed. On Windows 10 both fail.
            ComPtr<ID3D12InfoQueue1> infoQueue1;
            const HRESULT deviceQiHr = out.device.As(&infoQueue1);
            HRESULT queueQiHr = S_OK;
            if (FAILED(deviceQiHr))
            {
                infoQueue1.Reset();
                queueQiHr = infoQueue ? infoQueue.As(&infoQueue1) : E_NOINTERFACE;
            }
            const HRESULT infoQueue1Hr = SUCCEEDED(deviceQiHr) ? deviceQiHr : queueQiHr;
            if (SUCCEEDED(infoQueue1Hr))
            {
                DWORD         cookie     = 0;
                const HRESULT registerHr = infoQueue1->RegisterMessageCallback(
                    &D3D12DebugLayerCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
                if (SUCCEEDED(registerHr))
                {
                    out.infoQueue       = infoQueue1;
                    out.infoQueueCookie = cookie;
                }
                else
                {
                    ARC_WARN("ID3D12InfoQueue1::RegisterMessageCallback failed (hr=0x{:08X}); "
                             "D3D12 debug-layer messages reach the latch when the device is destroyed",
                             static_cast<uint32_t>(registerHr));
                }
            }
            else if (FAILED(infoQueueHr))
            {
                // No info queue of any generation: the debug layer is not
                // attached to this device. Either EnableDebugLayer above did
                // not take effect for the runtime that created the device,
                // or the device predates the enable.
                ARC_WARN("the D3D12 debug layer is not active on this device (ID3D12InfoQueue "
                         "QueryInterface failed, hr=0x{:08X}); D3D12 debug-layer messages will "
                         "not reach the log",
                         static_cast<uint32_t>(infoQueueHr));
            }
            else
            {
                const HMODULE deviceModule = ModuleContaining(ComVtable(out.device.Get()));
                const HMODULE queueModule  = ModuleContaining(ComVtable(infoQueue.Get()));
                const HMODULE loadedCore   = GetModuleHandleW(L"D3D12Core.dll");
                const int deviceSdk = ExportedD3D12SdkVersion(deviceModule);
                const int queueSdk  = ExportedD3D12SdkVersion(queueModule);
                const int coreSdk   = ExportedD3D12SdkVersion(loadedCore);
                const unsigned requested = AgilitySdk::kVersion;
                const unsigned osBuild   = WindowsBuildNumber();
                const bool handshake = deviceSdk == static_cast<int>(requested) &&
                                       queueSdk == static_cast<int>(requested);
                const char* why;
                if (!handshake)
                    why = "SDK handshake does not match this exe's D3D12SDKVersion";
                else if (osBuild != 0 && osBuild < 20236)
                    why = "handshake matches; ID3D12InfoQueue1 is not exposed before OS build "
                          "20236 (Windows 11). Stored messages are drained when the device is destroyed";
                else
                    why = "handshake matches and the OS build is new enough; the vtable module "
                          "above is what refused the interface";
                // The miss is identical for every device this process creates;
                // log the sentence once. Execution falls through to the queue below.
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true))
                {
                    ARC_WARN("ID3D12InfoQueue1 QueryInterface failed (device hr=0x{:08X}, info-queue hr=0x{:08X}). "
                             "Loaded D3D12Core.dll: {} (D3D12SDKVersion {}). "
                             "Device module: {} (D3D12SDKVersion {}). "
                             "InfoQueue module: {} (D3D12SDKVersion {}). "
                             "Exe requests {}. OS build {}. {}",
                             static_cast<uint32_t>(deviceQiHr), static_cast<uint32_t>(queueQiHr),
                             ModulePathOr(loadedCore, "not loaded"), SdkVersionText(coreSdk),
                             ModulePathOr(deviceModule, "none"), SdkVersionText(deviceSdk),
                             ModulePathOr(queueModule, "none"), SdkVersionText(queueSdk),
                             requested, osBuild, why);
                }
            }
        }

        // NRI capability contract item 10 (creation half): this ONE direct
        // queue is what the wrapper desc must carry in
        // QueueFamilyD3D12Desc::d3d12Queues -- leaving that null makes NRI
        // create its own, and the DXGI swapchain below is bound to THIS
        // one, so it would be presenting on a queue NRI never submits to.
        // Reachable for the wrap through GraphicsQueue() above.
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(out.device->CreateCommandQueue(&queueDesc,
                                                 IID_PPV_ARGS(&out.graphicsQueue))))
        {
            ARC_ERROR("CreateCommandQueue failed");
            return false;
        }

        return true;
    }

    // Contract item 12's teardown half, idempotent: the debug layer holds a
    // raw pointer to a function in THIS module and must not outlive it. Kept
    // separate from the release below because DestroyD3D12NativeDevice is
    // written on top of it, and idempotence makes the pair safe either way.
    void UnregisterD3D12DebugCallback(D3D12DeviceCreation& creation)
    {
        if (creation.infoQueue && creation.infoQueueCookie != 0)
        {
            creation.infoQueue->UnregisterMessageCallback(creation.infoQueueCookie);
            creation.infoQueueCookie = 0;
        }
    }

    // Owner teardown (contract item 15: the NRI device is destroyed BEFORE
    // this runs). Releases in the order D3D12DeviceCreation's member layout
    // encodes -- queue, info queue, device, adapter, factory -- which is the
    // order ~DeviceD3D12's member destruction has always produced.
    void DestroyD3D12NativeDevice(D3D12DeviceCreation& creation)
    {
        UnregisterD3D12DebugCallback(creation);
        creation.graphicsQueue.Reset();
        creation.infoQueue.Reset();
        creation.infoQueueBase.Reset();
        ReleaseArmoredD3D12Device(creation);   // the owner's reference + the armor, audited
        creation.adapter.Reset();
        creation.factory.Reset();
        // The device's final release is the last moment the layers can speak
        // about it (live-object reports land here); read them out.
        DrainDxgiDebugMessages("native device released");
    }

    // The DXGI debug queue's delivery half -- the counterpart of
    // D3D12DebugLayerCallback for a layer that has NO callback interface on
    // any Windows release: IDXGIInfoQueue only STORES, so the host has to
    // come and read. Same sink and same severity split as the D3D12 callback
    // (CORRUPTION/ERROR -> the latch through NoteError, tagged by the
    // producer; WARNING -> ARC_WARN; INFO/MESSAGE dropped), so "an error
    // happened" means one thing however it arrived.
    //
    // `moment` names the drain site in the log, because a stored message says
    // nothing about WHEN it was raised -- only that it was raised before this
    // read and after the previous one.
    void DrainDxgiDebugMessages(const char* moment)
    {
        if (!g_dxgiInfoQueue)
            return;

        // The summary says what the queue held, and only when there is
        // something to say: a quiet run stays quiet. The discarded count is
        // cumulative for the process (the limit is 1024 per producer, and a
        // 6 s windowed dx12 run fills the D3D12 producer's slot with WARNING
        // #820/#821 alone).
        const UINT64 count     = g_dxgiInfoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL);
        const UINT64 discarded = g_dxgiInfoQueue->GetNumMessagesDiscardedByMessageCountLimit(DXGI_DEBUG_ALL);
        if (count == 0 && discarded == 0)
            return;
        ARC_INFO("[dxgi] info queue at '{}': {} stored message(s), {} discarded at the {}-per-producer limit",
                 moment ? moment : "?", count, discarded, DXGI_INFO_QUEUE_DEFAULT_MESSAGE_COUNT_LIMIT);
        if (count == 0)
            return;

        std::string storage;
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T length = 0;
            if (FAILED(g_dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &length)) || length == 0)
                continue;

            storage.resize(length);
            auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(storage.data());
            if (FAILED(g_dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, message, &length)))
                continue;

            const char* producer = DxgiDebugProducerName(message->Producer);
            const std::string text = std::string(message->pDescription ? message->pDescription : "",
                                                 message->DescriptionByteLength) +
                                     " [" + producer + " " + DxgiDebugSeverityName(message->Severity) +
                                     " #" + std::to_string(message->ID) + ", drained at: " +
                                     (moment ? moment : "?") + "]";

            switch (message->Severity)
            {
            case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION:
            case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR:
                // Tagged by PRODUCER so a D3D12 message that travelled through
                // DXGIDebug (the Windows 10 in-box layer's path) reads as
                // "[d3d12]" -- the same tag D3D12DebugLayerCallback gives it
                // where ID3D12InfoQueue1 exists -- and a DXGI one as "[dxgi]".
                RenderErrorLatch::Instance().NoteError(producer, text.c_str());
                break;
            case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING:
                ARC_WARN("[{}] {}", producer, text);
                break;
            default:
                break;
            }
        }
        g_dxgiInfoQueue->ClearStoredMessages(DXGI_DEBUG_ALL);
    }

    // THE DESK CRASH OF 2026-09-22, and what it actually was.
    //
    // Symptom: the editor (windowed, dx12, debug layer on) died at CLOSE with
    // an unhandled STATUS_BREAKPOINT out of DXGIDebug!DXGI_SDK_MESSAGE, under
    // whichever device-touching Release() ran first inside ~DeviceD3D12 --
    // the zero buffer one run, a D3D12MA device interface the next, a command
    // signature the one after. Never headless, never at 90 frames, always at
    // 900, blamed on the game-module rebuild that happened to precede it.
    //
    // Cause, measured with a per-frame refcount trace and a vtable hook on
    // the device's AddRef/Release: every 31 presented frames, GTIII-OSD64.dll
    // -- the ASUS GPU Tweak III on-screen-display hook, injected into every
    // windowed D3D12 process on that desk beside NahimicOSD.dll and
    // nvspcap64.dll -- called Release() on OUR ID3D12Device three times
    // against two acquisitions. Net one reference lost per period: the count
    // read 33 at teardown headless (nriDestroyDevice then released 30 of
    // them cleanly) but 30 after 90 windowed frames and 22 after 900. Below
    // 30, nriDestroyDevice's own releases hit zero part-way, the device was
    // destroyed under NRI, D3D12MA and this owner, and the debug layer's
    // next look at it was the CORRUPTION break above (resumed once as an
    // experiment: an access violation on the freed wrapper). The rebuild was
    // a coincidence of session length.
    //
    // Why ARMOR rather than a measured repair: the leak is not Arcane's to
    // fix and cannot be prevented from inside the process, and the legal
    // count at any teardown point is NOT knowable -- on this layer every
    // live child object holds a device reference, and NRI creates
    // device-owned objects lazily (a command signature on the first indirect
    // draw, a D3D12MA pool on the first allocation) that live until
    // ~DeviceD3D12. A baseline taken after WrapD3D12 was tried and read
    // below the teardown floor for exactly that reason. What IS known: the
    // armor is ours, it is taken while nothing else can have touched the
    // count, and it is released LAST -- so at that moment the live count
    // must be armor + 1 (the owner's own reference), and any shortfall is
    // exactly the number of foreign releases. Loud, once, naming the number:
    // a desk with such an overlay sees the WARN on every close and knows
    // what to uninstall or blacklist; a clean desk never sees a line.
    //
    // 65536 references (render.d3d12.deviceArmorRefs, read once here at
    // device creation) cover ~2M presented frames at the measured rate (one
    // per 31 frames), i.e. hours; ULONG has room for far more. The cost is
    // one AddRef loop at creation and one Release loop at teardown, both
    // through the debug layer's thin wrapper -- milliseconds, once.
    namespace
    {
        void ArmorD3D12Device(D3D12DeviceCreation& out)
        {
            const ULONG refs = Settings<RenderD3d12Settings>().deviceArmorRefs;
            for (ULONG i = 0; i < refs; ++i)
                out.device->AddRef();
            out.deviceArmorRefs = refs;
        }

        // The audit + the last releases. The owner's ComPtr reference is
        // detached into this so the count read here is exactly armor + 1
        // when nobody outside this process has touched it.
        void ReleaseArmoredD3D12Device(D3D12DeviceCreation& creation)
        {
            ID3D12Device* device = creation.device.Detach();
            if (!device)
                return;
            const ULONG expected = creation.deviceArmorRefs + 1;
            creation.deviceArmorRefs = 0;

            device->AddRef();
            const ULONG live = device->Release();
            ULONG       toRelease = expected;
            if (live < expected)
            {
                // A foreign deficit: release only what is really there, so the
                // count reaches zero here and not one Release too late.
                ARC_WARN("[d3d12] ID3D12Device refcount {} at final release is below the {} this process "
                         "holds: a module outside Arcane (an injected overlay -- GPU Tweak III OSD, Nahimic, "
                         "ShadowPlay-class) released our device {} time(s) too many; the reference armor "
                         "absorbed it. Without the armor the close is an unhandled STATUS_BREAKPOINT in "
                         "D3D12SDKLayers (the device destroyed under NRI/D3D12MA).",
                         live, expected, expected - live);
                toRelease = live;
            }
            else if (live > expected)
            {
                // Somebody still holds the device after every owner released
                // theirs. Not ours to release: leave it exactly as a plain
                // ComPtr teardown would have, and say so.
                ARC_WARN("[d3d12] ID3D12Device refcount {} at final release exceeds the {} this process "
                         "holds: {} reference(s) leaked by something that outlives the render device",
                         live, expected, live - expected);
            }
            for (ULONG i = 0; i < toRelease; ++i)
                device->Release();
        }
    }

    // The D3D12 layer's stored messages, through the base ID3D12InfoQueue
    // kept in the creation half. On the Windows 10 in-box layer this storage
    // IS the D3D12 producer's slot of the DXGI queue above (clearing either
    // empties both -- observed), so the two drains are one reader with two
    // handles; where ID3D12InfoQueue1 exists the callback has already
    // delivered everything and this finds the queue empty. Same sink, same
    // severity split. ~NriDevice runs it before the DXGI drain so a D3D12
    // message is read with D3D12's own ID and severity names.
    void DrainD3D12DebugMessages(const D3D12DeviceCreation& creation, const char* moment)
    {
        ID3D12InfoQueue* queue = creation.infoQueueBase.Get();
        if (!queue)
            return;

        const UINT64 count     = queue->GetNumStoredMessages();
        const UINT64 discarded = queue->GetNumMessagesDiscardedByMessageCountLimit();
        if (count == 0 && discarded == 0)
            return;
        ARC_INFO("[d3d12] info queue at '{}': {} stored message(s), {} discarded at the {} limit",
                 moment ? moment : "?", count, discarded, queue->GetMessageCountLimit());
        if (count == 0)
            return;
        if (count == 0)
            return;

        std::string storage;
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T length = 0;
            if (FAILED(queue->GetMessage(i, nullptr, &length)) || length == 0)
                continue;
            storage.resize(length);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(queue->GetMessage(i, message, &length)))
                continue;

            const std::string text = std::string(message->pDescription ? message->pDescription : "",
                                                 message->DescriptionByteLength) +
                                     " [d3d12 " + std::string(D3D12DebugSeverityName(message->Severity)) +
                                     " #" + std::to_string(static_cast<int>(message->ID)) + ", drained at: " +
                                     (moment ? moment : "?") + "]";
            switch (message->Severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                RenderErrorLatch::Instance().NoteError("d3d12", text.c_str());
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                ARC_WARN("[d3d12] {}", text);
                break;
            case D3D12_MESSAGE_SEVERITY_INFO:
                ARC_INFO("[d3d12] {}", text);   // stored only when render.debug.minSeverity is Info
                break;
            default:
                // MESSAGE is denied at the storage filter; nothing of it is
                // ever here to drop.
                break;
            }
        }
        queue->ClearStoredMessages();
    }

    // The narrow export (DeviceRemovedObservers.hpp): the SAME observer
    // above, reachable BY ADDRESS from the Render module's installer. One
    // line, no state, no second observation point -- the once-only
    // `g_deviceRemovedReported` latch, the "gpu-crash: device removed" wording
    // and the NoteGpuDeviceLost ordering all stay in ObserveDeviceRemoved,
    // file-local.
    //
    // IT STAYS A SEPARATE FUNCTION FROM THE OBSERVER. Folding the
    // body up into this name would be a behaviour change, not a cleanup:
    // NriDiagnostics::Disarm clears the hook slot only when it still holds
    // the address Arm installed, and that address is THIS function's.
    void ObserveDeviceRemovedD3D12()
    {
        ObserveDeviceRemoved();
    }

    // Its twin (DeviceRemovedObservers.hpp): the store DeviceD3D12::Init used
    // to make one line above its own ResetGpuDeviceLost(). Since Task 8b
    // deleted that class, NriDiagnostics::Arm is the ONLY arming site left and
    // this is its only way to reach a latch that is deliberately file-local.
    void ResetDeviceRemovedLatchD3D12()
    {
        g_deviceRemovedReported.store(false, std::memory_order_release);
    }

    // The device-loss QUESTION, as opposed to the observers above which are
    // the ANSWER's delivery. See DeviceRemovedObservers.hpp for why NRI's
    // D3D12 QueueWaitIdle cannot be asked instead.
    //
    // GetDeviceRemovedReason is the one D3D12 call that is defined ON a
    // removed device. S_OK means "not removed"; every other HRESULT
    // (DXGI_ERROR_DEVICE_REMOVED / _HUNG / _RESET,
    // DXGI_ERROR_DRIVER_INTERNAL_ERROR) means it is gone. Deliberately not
    // once-only and deliberately silent: it observes nothing and reports
    // nothing, so it stays safe to call from a bail-out path.
    bool D3D12NativeDeviceRemoved(void* nativeDevice) noexcept
    {
        if (!nativeDevice)
            return false;
        return FAILED(static_cast<ID3D12Device*>(nativeDevice)->GetDeviceRemovedReason());
    }
}
