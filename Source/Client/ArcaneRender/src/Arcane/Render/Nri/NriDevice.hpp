#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// NRI substrate (Phase 1, Task 7): the WRAPPER PATH.
//
// We create the D3D12/VK device ourselves -- keeping DRED configuration, the
// VK device-fault sweep, our own debug-message routing, and every capability
// the contract requires -- and hand the finished thing to NRI through
// `nriCreateDeviceFromVKDevice` / `nriCreateDeviceFromD3D12Device`. NRI then
// derives its capability model from the physical device plus the extension
// name lists in the desc, and does NOT take ownership of our native objects.
//
// A call to `nriCreateDevice` for a REAL backend (D3D12/VK) is a review
// defect (spec ratification 2) -- the only sanctioned use of that entry point
// in this tree is the NONE backend in the [nri] tests, which has no native
// device to wrap. Nothing in this header or its .cpp calls it.
//
// Include order: NRI's Extensions/NRIDeviceCreation.h declares nri::Message
// with an enumerator literally named ERROR, and <windows.h> (reachable
// through spdlog) #defines ERROR via wingdi.h. Keep the NRI includes first,
// here and in NriDevice.cpp -- same rule as NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/Nri/DeviceResourceLifetime.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriDeviceCaps.hpp>

#include <memory>

namespace Arcane
{
    // The creation halves. Defined in Render/DeviceCreationVulkan.hpp and
    // Render/DeviceCreationD3D12.hpp, which carry vk:: / D3D12 types and are
    // therefore Render-internal -- forward-declared here so this header stays
    // consumable by code outside ArcaneClient.dll (ArcaneTests has no Vulkan
    // headers and no Vulkan-Hpp dispatcher storage of its own).
    struct VulkanDeviceCreation;
    struct D3D12DeviceCreation;
    class BindlessTable;

    // -------------------------------------------------------------------
    // NativeDeviceOwner -- the caller-owns half of the ownership contract.
    // -------------------------------------------------------------------
    // NriDevice WRAPS a creation half; it never owns one. Something else has
    // to, and outlive it (contract item 15: the NRI device is destroyed
    // BEFORE the native device, because ~DeviceVK/~DeviceD3D12 release NRI's
    // own objects -- VMA allocator, queues, render-pass caches -- against it,
    // while our VkDevice/ID3D12Device is left untouched).
    //
    // NativeDeviceOwner (this class) owns the creation half for EVERY device
    // in the process -- there is no other owner.
    class ARC_RENDER_API NativeDeviceOwner
    {
    public:
        // Runs the creation half (DeviceCreation{D3D12,Vulkan}) and wraps
        // what it produced. Returns null on failure (reason already logged).
        //
        // ONE LIVE VULKAN DEVICE PER PROCESS: the Vulkan-Hpp default
        // dispatcher binds ONE VkDevice (see VulkanDispatchStorage.cpp), so
        // do not hold one of these alongside another live Vulkan device
        // wrapper. Nothing in the tree does today -- this is the only such
        // wrapper -- but the underlying constraint is the dispatcher's, not
        // ours.
        static std::unique_ptr<NativeDeviceOwner> Create(const RenderDeviceDesc& desc);

        ~NativeDeviceOwner();

        NativeDeviceOwner(const NativeDeviceOwner&)            = delete;
        NativeDeviceOwner& operator=(const NativeDeviceOwner&) = delete;

        [[nodiscard]] GraphicsBackend Backend() const;

        // Exactly one of these is non-null, per Backend(). Both types are
        // incomplete outside the Render module -- in-module callers include
        // the matching DeviceCreation*.hpp; everyone else just passes the
        // owner to NriDevice::Wrap.
        [[nodiscard]] const VulkanDeviceCreation* Vulkan() const;
        [[nodiscard]] const D3D12DeviceCreation*  D3D12() const;

    private:
        NativeDeviceOwner();

        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // -------------------------------------------------------------------
    // NriDevice -- the wrapped device.
    // -------------------------------------------------------------------
    class ARC_RENDER_API NriDevice
    {
    public:
        // Wrapper path only. Each returns null (loudly, naming what failed)
        // if the desc could not be filled, if NRI refused the wrap, or if a
        // post-wrap assert did not hold. The creation half must outlive the
        // returned object.
        static std::unique_ptr<NriDevice> WrapVulkan(const VulkanDeviceCreation& creation);
        static std::unique_ptr<NriDevice> WrapD3D12(const D3D12DeviceCreation& creation);
        // Backend-dispatching convenience over the two above.
        static std::unique_ptr<NriDevice> Wrap(const NativeDeviceOwner& native);

        // The NONE backend, for DEVICE-LESS TESTS ONLY (Task 6's [nri] graph
        // executor integration cases). It is the one carve-out from the
        // wrapper-path rule stated at the top of this file: nriCreateDevice
        // is a review defect for a REAL backend, but NONE has no native
        // device to wrap -- there is nothing to wrap it FROM -- so that is
        // the only creation path it has. NriSubstrateTest.cpp's NONE cases
        // already call nriCreateDevice directly for the same reason.
        //
        // Why it must live HERE rather than in the test exe: ArcaneTests
        // links its OWN static copy of NRI (premake5.lua's ArcaneTests
        // links{} comment), so a device created by the exe's copy and driven
        // through ArcaneClient.dll's CoreInterface would cross function
        // tables. RenderGraph::Execute() runs inside the DLL, so its device
        // has to be created inside the DLL too. Returns null on failure
        // (already logged); Backend() reports D3D12 for a NONE device --
        // GraphicsBackend has no NONE value and inventing one would leak a
        // test-only concept into every backend switch in the tree.
        static std::unique_ptr<NriDevice> CreateNoneForTests();

        // Destroys the NRI device ONLY. The native device it wrapped is the
        // caller's and is untouched. Drains the graveyard first, after an
        // NRI DeviceWaitIdle -- Graveyard's contract is "the caller has
        // already made the GPU idle", and this is the one place that can
        // honor it on the way out.
        ~NriDevice();

        NriDevice(const NriDevice&)            = delete;
        NriDevice& operator=(const NriDevice&) = delete;

        [[nodiscard]] nri::Device& Device() const { return *m_device; }
        [[nodiscard]] const nri::CoreInterface& Core() const { return m_core; }
        // nriGetInterface through THIS module's NRI (ArcaneRender links the only
        // NRI copy the engine modules share): a module that links no NRI of its
        // own -- ArcaneImGui's HelperInterface -- fetches extension tables here,
        // so every table it drives belongs to the image that created the device
        // (the cross-copy hazard CreateNoneForTests describes). Pass
        // NRI_INTERFACE(nri::XInterface), &table.
        [[nodiscard]] nri::Result QueryInterface(const char* interfaceName, size_t interfaceSize,
                                                 void* interfacePtr) const;
        [[nodiscard]] GraphicsBackend Backend() const { return m_backend; }

        // Tiers/features snapshot, queried once at wrap time (FinishWrap,
        // immediately after GetQueue succeeds) so later gates -- bindless
        // material indexing, ray-tracing bakers -- read a value that was
        // ALWAYS there, rather than a query someone forgot to add at the
        // point of first use.
        [[nodiscard]] const NriDeviceCaps& Caps() const noexcept { return m_caps; }

        // The D3D12 creation half this device wrapped; null on other backends.
        [[nodiscard]] const D3D12DeviceCreation* D3D12Creation() const noexcept { return m_d3d12Creation; }

        // The GRAPHICS queue proven reachable at wrap time (contract item 7 /
        // §1.6.3: NRI clamps a declared queueNum against PHYSICAL family
        // capacity, and a failed throwaway-instance probe clamps it to ZERO,
        // which would otherwise surface as an UNSUPPORTED far from the cause).
        [[nodiscard]] nri::Queue* GraphicsQueue() const { return m_graphicsQueue; }

        // Fence-tagged deferred destruction for objects owned by the DEVICE
        // ITSELF -- i.e. by an owner with no submission timeline of its own.
        // (Named Graves() because a member function called Graveyard() would
        // shadow the type inside this class.)
        //
        // ===== IT IS DRAIN-ONLY, AND IT IS NOT A DEFAULT (Task 8-pre) =====
        // Phase 1 made this per-device because there was one queue timeline and
        // one owner. There are now N owners -- one per NriGraphContext -- and a
        // fence value only means something inside ONE submission timeline, so
        // "per device" is the wrong granularity for anything a context owns.
        // EVERY graph-context-owned object is buried in that context's OWN lane
        // (NriGraphContext::Graves(), threaded into RenderGraph through
        // RgExecuteDesc::graves). Nothing in the tree buries here, and nothing
        // REAPS here: this graveyard has no clock. ~NriDevice drains it exactly
        // once, behind a DeviceWaitIdle.
        //
        // SO: burying here means "destroy at device teardown", full stop --
        // fence values are burial-ORDER hints and nothing more. If what you own
        // needs fence-paced reclamation, own a Graveyard, do not borrow this
        // one. Two owners burying here on their own fence clocks is the exact
        // hazard NriGraphContext.hpp's TWO CONTEXTS, TWO LANES block describes:
        // a Debug nondecreasing assert, or a reap against a foreign fence.
        //
        // The device-less [nri] cases DO drive it by hand (Bury/Reap/Drain with
        // their own values) -- as a plain, conveniently-owned Graveyard, which
        // is all it is.
        [[nodiscard]] Graveyard& Graves() { return m_graveyard; }

        // THE DEVICE-WIDE LAST-USE RECORD (3D-readiness arc spec 3.5): which
        // contexts sampled which bindless slot, and the fence of that context's
        // own submit. One per device, because the bindless index space is one
        // per device.
        [[nodiscard]] DeviceResourceLifetime& ResourceLifetime() noexcept { return m_life; }

        // THE DEVICE'S BINDLESS TABLE: one per NriDevice, created on first use at
        // Arcane::kBindlessCapacity over ResourceLifetime(). Every
        // NriGraphContext on this device shares it; null only if creation was
        // refused (already logged).
        [[nodiscard]] BindlessTable* Bindless();

    private:
        NriDevice();

        // Shared tail of both Wrap* entry points: CoreInterface, the queue
        // assert, and the identity log (contract item 14).
        static std::unique_ptr<NriDevice> FinishWrap(nri::Device* device,
                                                     GraphicsBackend backend);

        Graveyard          m_graveyard;
        DeviceResourceLifetime m_life;
        std::unique_ptr<BindlessTable> m_bindless;
        nri::CoreInterface m_core{};
        nri::Device*       m_device        = nullptr;
        nri::Queue*        m_graphicsQueue = nullptr;
        GraphicsBackend    m_backend       = GraphicsBackend::D3D12;
        NriDeviceCaps      m_caps{};
        // The creation half this device wrapped (WrapD3D12), which outlives
        // it by contract; null on every other backend. ~NriDevice reads the
        // D3D12 debug layer's stored messages through it.
        const D3D12DeviceCreation* m_d3d12Creation = nullptr;
    };
}
