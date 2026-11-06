// See NriDiagnostics.hpp for what this file is, what it deliberately does NOT
// reimplement, and the both-topologies rule. Same include-order rule as every
// file in this directory (NriCommon.hpp): NRI headers FIRST, because
// Extensions/NRIDeviceCreation.h declares nri::Message::ERROR and <windows.h>
// (via Arcane/Base/Log.hpp -> spdlog) #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/IO/ReadRegularFile.hpp>

#include "NriDiagnostics.hpp"

#include "NriCommon.hpp"

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceRemovedObservers.hpp> // ObserveDeviceRemovedD3D12 / ...Vulkan (the narrow export)
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/GpuCrashReport.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/RenderShaderSettings.hpp>

#if !defined(ARC_BUILD_DIST)
    #include <Arcane/Render/GpuFaultInjector.hpp>   // kPassName -- ONE spelling of the breadcrumb both arms produce
    #include <Arcane/Render/ShaderConventions.hpp>  // kCsEntry -- the entry name the artifact was compiled with
    #include <Arcane/Render/ShaderPaths.hpp>        // ResolveFlavorDir -- the ONE artifact directory every shader loader resolves through
#endif

#undef ERROR

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Arcane
{
    namespace
    {
        // =============================================================
        // The graph-flavored crash backend
        // =============================================================
        // One CPU breadcrumb ring + one device identity. See the header for
        // why there is no GPU-written marker layer here yet and why that is
        // an inventory item rather than a gap: Diag::ReplayMarkerBuffer is
        // written to accept exactly this shape (a null marker region pushes
        // `breadcrumbs:off` and adds no `.gpudump` section), and the ring is
        // the half today's hang and crash reports are actually built from.
        //
        // NativeDevice() is the load-bearing member. RenderGraphExec.cpp's
        // NodeScope refuses to write a native marker unless the active
        // backend's marker buffer lives on the SAME device the command buffer
        // came from (VUID-vkCmdWriteBufferMarkerAMD-commonparent; the D3D12
        // GPU-virtual-address equivalent). Reporting the NRI device's own
        // native handle here is what makes that gate answer "same device"
        // after the one-device flip -- so the day a native NRI marker layer
        // lands, the graph starts writing markers with no edit in that file.
        class NriGraphCrashBackend final : public IGpuCrashBackend
        {
        public:
            explicit NriGraphCrashBackend(NriDevice& device)
                : m_nativeDevice(device.Core().GetDeviceNativeObject
                                     ? device.Core().GetDeviceNativeObject(&device.Device())
                                     : nullptr)
            {
            }

            // No GPU-written marker layer yet (header: it is its own arc).
            // False, not a silent true: `armed` in Diag::ReplayMarkerBuffer is
            // a RUNTIME kill switch, and claiming a marker went out when none
            // did would sell a stale timeline as a live one.
            bool WriteMarkerNative(void*, std::uint32_t, bool) override { return false; }

            [[nodiscard]] void* NativeDevice() const override { return m_nativeDevice; }

            GpuBreadcrumbs& Breadcrumbs() override { return m_breadcrumbs; }

            const char* Name() const override { return "NRI-graph"; }

            void CollectFault(Diag::Envelope& envelope) override
            {
                // The verdict, from the only evidence this backend HAS.
                //
                // The two native backends read a device-removed reason
                // (D3D12: GetDeviceRemovedReason; VK: their own device-lost
                // latch + VK_EXT_device_fault). Neither query exists here --
                // the native device is owned by the creation half, and
                // reaching for it would mean this file growing a D3D12 and a
                // Vulkan arm, i.e. becoming a third crash backend, which the
                // header refuses.
                //
                // What IS available and is not a guess: the report's own
                // KIND, which WriteReportImpl fills BEFORE calling the
                // provider, and which Diagnostics::DeriveKind resolved from
                // the reason string. A `gpu-crash` report is written by the
                // device-removed observer, and by nothing else, so the device
                // is lost by construction; a `gpu-stall` is the watchdog's
                // verdict on a device that is still alive and merely not
                // retiring. That distinction is precisely what
                // Diag::FreezeBreadcrumbsOnDeviceLoss keys off ("device-alive"
                // is the shared healthy verdict), so getting it from the kind
                // gives the ring-freeze rule the same behaviour it has on the
                // other two backends.
                envelope.fault.type =
                    (envelope.kind == "gpu-crash") ? "device-removed" : "device-alive";
            }

            // The Diagnostics GPU-section body, mirroring both native
            // backends' FillReport exactly (theirs are private too -- the
            // seam's CollectFault deliberately takes only an Envelope).
            void FillReport(Diag::Envelope& envelope,
                            std::string& humanText,
                            const std::filesystem::path& reportStem)
            {
                CollectFault(envelope);

                // Null marker region + not armed: the documented shape for a
                // backend with no marker layer at all. Pushes `breadcrumbs:off`
                // into activeLayers -- which is the INVENTORY line a reader
                // needs, not a failure -- and adds no raw section.
                Diag::GpuDumpWriter raw;
                Diag::ReplayMarkerBuffer(m_breadcrumbs, raw, envelope,
                                         /*markerMemory=*/nullptr, /*armed=*/false);

                // THE PAYLOAD: the graph's own CPU breadcrumbs, as the
                // .arcdiag queue block and the report's human-readable
                // "queue graphics" section. Every node of every frame the
                // graph executed opened one of these (RenderGraphExec.cpp,
                // NodeScope), and so did --crash-gpu's `pass:gpu-fault`.
                //
                // WHAT THAT BLOCK READS UNTIL A MARKER LAYER LANDS, exactly,
                // because a diagnostics file must not overstate its own
                // output: GpuBreadcrumbs::Capture() is MARKER-EVIDENCE-ONLY
                // (OnMarkerWritten is its sole input -- CPU-side EndScope
                // deliberately feeds it nothing, since the CPU races ahead of
                // the GPU), so with no GPU-written marker layer here the ring
                // supplies the NAMES and nothing supplies "which of them the
                // GPU reached" -- the queue block emits `<none>`/`<none>`.
                // The section is still the right one to emit, and it starts
                // naming scopes the moment WriteMarkerNative above stops
                // returning false, with no edit at this line. Pinned both
                // ways in Tests/ArcaneTests/src/NriDiagnosticsTest.cpp so the day
                // that arc lands, the change in this report is visible.
                Diag::EmitQueueSnapshot(m_breadcrumbs, "graphics", envelope, humanText);

                // A lost device freezes the ring: the frames a host keeps
                // pumping after removal must not evict the crash-time
                // timeline out from under the LATER reports of the same
                // cascade. Same call, same position as both native backends.
                Diag::FreezeBreadcrumbsOnDeviceLoss(m_breadcrumbs, envelope);

                // Written for gpu kinds ALWAYS, partial collection included --
                // the section table doubles as the capture inventory, and
                // "breadcrumbs present, markers absent" is itself the answer.
                Diag::EmitGpuDumpSibling(raw, envelope, humanText, reportStem);
            }

        private:
            void*          m_nativeDevice = nullptr;
            GpuBreadcrumbs m_breadcrumbs;
        };

        void NriGraphGpuSectionProvider(Diag::Envelope& envelope,
                                        std::string& humanText,
                                        const std::filesystem::path& reportStem,
                                        void* user)
        {
            if (!user)
                return;
            // `user` was handed to SetGpuSectionProvider as an
            // IGpuCrashBackend*, so the round trip goes back through that type
            // before down-casting -- same rule as D3D12GpuSectionProvider.
            auto* backend = static_cast<NriGraphCrashBackend*>(static_cast<IGpuCrashBackend*>(user));
            backend->FillReport(envelope, humanText, reportStem);
        }

        // =============================================================
        // The arming state
        // =============================================================
        // Process-wide because every slot it fills is: the device-removed
        // hook, the Diagnostics provider slot and the active-backend slot are
        // all one-per-process, last-writer-wins. The mutex is not for
        // contention -- Arm/Disarm run on the host's main thread at boot and
        // teardown -- it is so a Disarm racing a report already inside the
        // provider cannot free the backend under it. (The report side of that
        // race is Diagnostics::FenceReports, called in Disarm.)
        std::mutex                             g_armMutex;
        std::unique_ptr<NriGraphCrashBackend>  g_backend;
        RenderErrorLatch::DeviceRemovedHook    g_installedHook = nullptr;

        [[nodiscard]] RenderErrorLatch::DeviceRemovedHook ObserverFor(GraphicsBackend backend) noexcept
        {
            // Per-backend, because the once-only removal latch is: each device
            // creation half owns its own `g_deviceRemovedReported`
            // (DeviceRemovedObservers.hpp states why they are not collapsed
            // into one).
            switch (backend)
            {
            case GraphicsBackend::D3D12:  return &ObserveDeviceRemovedD3D12;
            case GraphicsBackend::Vulkan: return &ObserveDeviceRemovedVulkan;
            }
            return nullptr;
        }

        // The observer's other half, resolved the same way for the same reason
        // (DeviceRemovedObservers.hpp): re-arming the once-only latch is what makes
        // the observer above report the NEXT removal rather than latch out on
        // the last device's.
        void ResetRemovalLatchFor(GraphicsBackend backend) noexcept
        {
            switch (backend)
            {
            case GraphicsBackend::D3D12:  ResetDeviceRemovedLatchD3D12();  return;
            case GraphicsBackend::Vulkan: ResetDeviceRemovedLatchVulkan(); return;
            }
        }
    }

    namespace NriDiagnostics
    {
        bool Arm(NriDevice& device)
        {
            std::lock_guard lock(g_armMutex);

            if (g_backend)
                return false;   // idempotence: a second Arm touches nothing

            // THE FOREIGN-BACKEND TEST, inferred rather than configured. The
            // only way to reach this branch is a caller (or a test) installing
            // a backend of its own. The check stays because last-writer-wins
            // makes displacement trivially possible and silently wrong, and
            // refusing is the right answer for ANY owner we did not install.
            // NriDiagnosticsTest drives it with a stub. The log line below
            // names no backend VENDOR, only the fact that the slot is occupied
            // by an owner we did not install -- which is all that is knowable
            // here.
            if (ActiveGpuCrashBackend() != nullptr)
            {
                ARC_INFO("[nri] diagnostics: a GPU crash backend we did not install is already "
                         "armed -- leaving the chain to its owner");
                return false;
            }

            const RenderErrorLatch::DeviceRemovedHook hook = ObserverFor(device.Backend());

            auto backend = std::make_unique<NriGraphCrashBackend>(device);

            // BOTH latches a new device invalidates, cleared as a PAIR, in
            // this order:
            //
            //   * the observer's once-only removal latch -- it says "this
            //     process already reported ITS device loss", which was true of
            //     the device that just went away and is false of this one. Left
            //     set, the observer this Arm is about to install would swallow
            //     the next removal entirely: no report, no `.gpudump`, no
            //     NoteGpuDeviceLost for the host to quit on. (Reachable the
            //     moment a host survives a loss and rebuilds its graph
            //     context, and this is the ONLY site that clears it.)
            //   * the process-wide device-lost latch -- a stale one would quit
            //     the host the moment this healthy device started presenting.
            ResetRemovalLatchFor(device.Backend());
            ResetGpuDeviceLost();

            if (hook)
                RenderErrorLatch::Instance().SetDeviceRemovedHook(hook);
            // THE UPCAST IS EXPLICIT. The provider recovers `user` as an
            // IGpuCrashBackend* before down-casting, so handing it a
            // NriGraphCrashBackend* that decayed straight to void* would be
            // true only by the accident of a single non-virtual base sharing
            // the derived object's address. Stated in the code rather than
            // relied on; the fault injector drives this provider, so the path
            // is live rather than decorative.
            Diagnostics::SetGpuSectionProvider(&NriGraphGpuSectionProvider,
                                               static_cast<IGpuCrashBackend*>(backend.get()));
            SetActiveGpuCrashBackend(backend.get());

            g_installedHook = hook;
            g_backend       = std::move(backend);

            ARC_INFO("[nri] diagnostics armed on the {} NRI device: device-removed hook, GPU-section "
                     "provider, breadcrumb backend ({})",
                     ToString(device.Backend()),
                     g_backend->NativeDevice() ? "native markers gated on device identity"
                                               : "CPU breadcrumbs only -- no native device handle");
            return true;
        }

        void Disarm() noexcept
        {
            std::lock_guard lock(g_armMutex);

            if (!g_backend)
                return;

            // Symmetric with Arm's install, and in the order ~DeviceD3D12
            // established (that destructor is gone; the order is not): unslot the provider, FENCE any report already
            // mid-flight against this backend (clearing the slot only stops
            // the NEXT report from seeing it -- a watchdog thread can be
            // inside FillReport right now), then the conditional clears.
            Diagnostics::ClearGpuSectionProvider();
            Diagnostics::FenceReports();
            (void)ClearActiveGpuCrashBackendIfCurrent(g_backend.get());

            // CONDITIONAL, for the same stale-owner reason
            // ClearActiveGpuCrashBackendIfCurrent exists: if something else
            // installed a hook after us, unslotting it here would disconnect a
            // live, unrelated owner. RenderErrorLatch's setter has no
            // compare-and-clear of its own, so the comparison happens here.
            if (g_installedHook
                && RenderErrorLatch::Instance().CurrentDeviceRemovedHook() == g_installedHook)
            {
                RenderErrorLatch::Instance().SetDeviceRemovedHook(nullptr);
            }

            g_installedHook = nullptr;
            g_backend.reset();
        }

        bool IsArmed() noexcept
        {
            std::lock_guard lock(g_armMutex);
            return g_backend != nullptr;
        }

        IGpuCrashBackend* ArmedBackend() noexcept
        {
            std::lock_guard lock(g_armMutex);
            return g_backend.get();
        }

        void PublishHeartbeat(std::uint64_t completedFenceValue) noexcept
        {
            Diagnostics::GpuHeartbeat(completedFenceValue);
        }
    }
}

#if !defined(ARC_BUILD_DIST)

namespace Arcane
{
    namespace
    {
        // -------------------------------------------------------------
        // The fault injector's constants -- the ONLY copy, with the reasoning
        // stated at each value that needs it.
        // -------------------------------------------------------------
        struct FaultCB
        {
            std::uint32_t iterations = 0;
            std::uint32_t oobElement = 0;
            std::uint32_t seed       = 0;
            std::uint32_t sinkMask   = 0;
        };

        // D3D12 requires a CBV's SizeInBytes to be a multiple of 256
        // (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), and the view must
        // fit inside its resource -- so BOTH the buffer and the view are sized
        // to this, not to sizeof(FaultCB) (16). Vulkan has no equivalent size
        // rule (its alignment constraint is on the OFFSET, which is 0 here),
        // which is exactly why D3b saw this fail on dx12 only: NRI's
        // ID3D12Device15::TryCreateConstantBufferView returned E_INVALIDARG
        // (0x80070057) for a 16-byte view, and the resulting ERROR-severity
        // ReportMessage aborted the process before the injector could ever
        // dispatch. The shader reads only the first 16 bytes; the rest is
        // zero-filled padding.
        ARC_CONSTANT("test-only fault injection: chosen to guarantee a TDR or out-of-bounds fault")
        constexpr std::uint32_t kFaultCBSize       = 256u;
        static_assert(sizeof(FaultCB) <= kFaultCBSize,
                      "the fault constant buffer must fit in one 256-byte CBV window");

        ARC_CONSTANT("test-only fault injection: chosen to guarantee a TDR or out-of-bounds fault")
        constexpr std::uint32_t kFaultSinkElements = 256u;
        constexpr std::uint32_t kFaultSinkStride   = sizeof(std::uint32_t);
        ARC_CONSTANT("test-only fault injection: chosen to guarantee a TDR or out-of-bounds fault")
        constexpr std::uint32_t kFaultThreadGroups = 256u;
        ARC_CONSTANT("test-only fault injection: chosen to guarantee a TDR or out-of-bounds fault")
        constexpr std::uint32_t kFaultIterations   = 0xFFFFFFFFu;
        ARC_CONSTANT("test-only fault injection: chosen to guarantee a TDR or out-of-bounds fault")
        constexpr std::uint32_t kFaultOobElement   = 1u << 30;

        // The artifact stem + the directory literal every shader loader in
        // the tree resolves through (Render/ShaderPaths.hpp), so
        // ARCANE_SHADER_DIR still moves this arm with everything else.
        constexpr const char* kFaultShaderStem = "gpu_fault_cs";
        constexpr const char* kFaultShaderDir  = "data/shaders";

        [[nodiscard]] std::vector<std::uint8_t> ReadShaderBlob(const std::filesystem::path& path)
        {
            const auto raw = Arcane::ReadRegularFile(path);
            if (!raw || raw->empty())
                return {};
            const auto* p = reinterpret_cast<const std::uint8_t*>(raw->data());
            return std::vector<std::uint8_t>(p, p + raw->size());
        }

        // Everything FireFault creates, so ONE teardown path serves the happy
        // path and every bail-out. Order is reverse-creation; each destroy is
        // null-guarded because a bail can happen at any step.
        struct FaultObjects
        {
            const nri::CoreInterface* core = nullptr;

            nri::CommandBuffer*    cmd       = nullptr;
            nri::CommandAllocator* allocator = nullptr;
            nri::DescriptorPool*   pool      = nullptr;
            nri::Descriptor*       cbView    = nullptr;
            nri::Descriptor*       sinkView  = nullptr;
            nri::Buffer*           cb        = nullptr;
            nri::Buffer*           sink      = nullptr;
            nri::Pipeline*         pipeline  = nullptr;
            nri::PipelineLayout*   layout    = nullptr;

            // "THE GPU NEVER FINISHED WITH THESE -- DO NOT FREE THEM."
            //
            // FireFault's tail says it plainly: the objects below "may not be
            // freed while the GPU is still reading them", and the QueueWaitIdle
            // is "what makes the teardown legal". When that wait does not
            // complete -- which is the NORMAL outcome of the deliberate TDR
            // this whole file exists to cause -- the teardown is NOT legal, and
            // the D3D12 debug layer says so the only way it can: it fail-fasts
            // (RaiseFailFastException, code 0x87D) from inside the Release of a
            // descriptor heap the GPU is still holding. That kills the host
            // with exit 0x87D (2173) instead of the device-loss exit 1, and
            // burns the report on "crash (unhandled exception)" instead of the
            // truth. Vulkan's validation layer merely logs the same condition,
            // which is why only dx12 died of it.
            //
            // D3D12 ONLY, and the backend gate is load-bearing -- the first
            // cut of this disarm was not gated and that is what turned the
            // vulkan arm from a clean exit 1 into an abort (exit 3):
            //
            //   Assertion failed: false && "Unfreed dedicated allocations
            //   found!", ThirdParty/VMA/include/vk_mem_alloc.h:6842
            //
            // The two APIs want OPPOSITE things after a device loss, and both
            // enforce their want:
            //
            //   VULKAN -- destroying child objects is REQUIRED, not merely
            //     permitted: vkDestroyDevice's valid-usage says every object
            //     created from the device must be destroyed first, and NRI's
            //     VMA allocator asserts (above) when they are not. The
            //     "VkCommandBuffer ... is in use" messages that VVL emits on
            //     this path are ERROR-severity log lines, nothing more; they
            //     were already present in the recorded exit-1 PASS run. So on
            //     VK the fault objects are freed normally.
            //   D3D12 -- Release of an object the GPU still owns is a
            //     RaiseFailFastException (0x87D) out of D3D12SDKLayers, which
            //     is non-continuable and kills the host at exit 2173 with the
            //     report burned on "crash (unhandled exception)". So on D3D12
            //     the fault objects are NOT freed.
            //
            // The D3D12 leak is bounded and paid for at exactly one place:
            // ~NriDevice skips nriDestroyDevice on a lost D3D12 device, so
            // D3D12MA never runs its own "Unfreed committed allocations
            // found!" assert (D3D12MemAlloc.cpp:8271) over what we kept. The
            // process is already latched to quit; Windows reclaims the rest.
            void Disarm() noexcept { core = nullptr; }

            ~FaultObjects()
            {
                if (!core)
                    return;
                if (cmd)       core->DestroyCommandBuffer(cmd);
                if (allocator) core->DestroyCommandAllocator(allocator);
                if (pool)      core->DestroyDescriptorPool(pool);
                if (cbView)    core->DestroyDescriptor(cbView);
                if (sinkView)  core->DestroyDescriptor(sinkView);
                if (cb)        core->DestroyBuffer(cb);
                if (sink)      core->DestroyBuffer(sink);
                if (pipeline)  core->DestroyPipeline(pipeline);
                if (layout)    core->DestroyPipelineLayout(layout);
            }
        };
    }

    namespace NriDiagnostics
    {
        bool FireFault(NriDevice& device, nri::Queue& queue)
        {
            const nri::CoreInterface& core = device.Core();

            const std::filesystem::path dir =
                ShaderPaths::ResolveFlavorDir(device.Backend(), kFaultShaderDir);
            if (dir.empty())
            {
                ARC_ERROR("[nri] --crash-gpu: no shader directory -- nothing dispatched");
                return false;
            }

            const std::vector<std::uint8_t> blob =
                ReadShaderBlob(dir / (std::string(kFaultShaderStem) + ".bin"));
            if (blob.empty())
            {
                ARC_ERROR("[nri] --crash-gpu: {} missing or unreadable -- run "
                          "data/shaders/compile-shaders.bat",
                          (dir / (std::string(kFaultShaderStem) + ".bin")).string());
                return false;
            }

            FaultObjects objects;
            objects.core = &core;

            // b0 = FaultCB, u0 = the sink. Written in HLSL register numbers on
            // BOTH backends: NRI shifts them by the device's vkBindingOffsets,
            // which NriDevice pins to ShaderConventions.hpp's dxc -fvk-*-shift
            // values with static_asserts.
            nri::DescriptorRangeDesc ranges[2] = {};
            ranges[0].baseRegisterIndex = 0;
            ranges[0].descriptorNum     = 1;
            ranges[0].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
            ranges[0].shaderStages      = nri::StageBits::COMPUTE_SHADER;
            ranges[1].baseRegisterIndex = 0;
            ranges[1].descriptorNum     = 1;
            ranges[1].descriptorType    = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
            ranges[1].shaderStages      = nri::StageBits::COMPUTE_SHADER;

            nri::DescriptorSetDesc setDesc = {};
            setDesc.registerSpace = 0;
            setDesc.ranges        = ranges;
            setDesc.rangeNum      = 2;

            nri::PipelineLayoutDesc layoutDesc = {};
            layoutDesc.rootRegisterSpace = 0;
            layoutDesc.descriptorSets    = &setDesc;
            layoutDesc.descriptorSetNum  = 1;
            layoutDesc.shaderStages      = nri::StageBits::COMPUTE_SHADER;

            // Created locally rather than through NriPipelineCache: that cache
            // has a GRAPHICS path only (GetGraphics), and a one-shot pipeline
            // whose caller intends to lose the device has no business
            // occupying a cache entry that Clear() then has to bury.
            if (!ARC_NRI_CHECK(core.CreatePipelineLayout(device.Device(), layoutDesc, objects.layout))
                || !objects.layout)
            {
                ARC_ERROR("[nri] --crash-gpu: pipeline layout creation failed");
                return false;
            }

            nri::ComputePipelineDesc pipelineDesc = {};
            pipelineDesc.pipelineLayout   = objects.layout;
            pipelineDesc.shader.stage     = nri::StageBits::COMPUTE_SHADER;
            pipelineDesc.shader.bytecode  = blob.data();
            pipelineDesc.shader.size      = blob.size();
            pipelineDesc.shader.entryPointName = kCsEntry;
            if (!ARC_NRI_CHECK(core.CreateComputePipeline(device.Device(), pipelineDesc, objects.pipeline))
                || !objects.pipeline)
            {
                ARC_ERROR("[nri] --crash-gpu: compute pipeline creation failed");
                return false;
            }

            nri::BufferDesc cbDesc = {};
            cbDesc.size  = kFaultCBSize;   // 256-aligned; see kFaultCBSize
            cbDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
            if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(device.Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                          0.0f, cbDesc, objects.cb))
                || !objects.cb)
            {
                ARC_ERROR("[nri] --crash-gpu: parameter constant buffer creation failed");
                return false;
            }

            nri::BufferDesc sinkDesc = {};
            sinkDesc.size            = (std::uint64_t)kFaultSinkElements * kFaultSinkStride;
            sinkDesc.structureStride = kFaultSinkStride;
            sinkDesc.usage           = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
            if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(device.Device(), nri::MemoryLocation::DEVICE,
                                                          0.0f, sinkDesc, objects.sink))
                || !objects.sink)
            {
                ARC_ERROR("[nri] --crash-gpu: sink buffer creation failed");
                return false;
            }

            // The parameters. Written through a MAP rather than a staged copy
            // because a HOST_UPLOAD constant buffer is exactly the case NRI's
            // map exists for, and one fewer command in a command buffer whose
            // whole job is to die is one fewer thing to get wrong.
            // NONE-backend footgun: MapBuffer returns null unconditionally
            // there, which is one of the reasons this whole function is a
            // [gpu] path.
            if (void* cpu = core.MapBuffer(*objects.cb, 0, nri::WHOLE_SIZE))
            {
                FaultCB params;
                params.iterations = kFaultIterations;
                params.oobElement = kFaultOobElement;
                params.seed       = 0x9E3779B9u;   // any non-zero constant; keeps the chain from folding
                params.sinkMask   = kFaultSinkElements - 1u;
                // Zero the whole 256-byte window before the payload: the CBV
                // spans kFaultCBSize, so the padding past sizeof(FaultCB) is
                // readable by the shader and a HOST_UPLOAD allocation arrives
                // uninitialised.
                std::memset(cpu, 0, kFaultCBSize);
                std::memcpy(cpu, &params, sizeof(params));
                core.UnmapBuffer(*objects.cb);
            }
            else
            {
                ARC_ERROR("[nri] --crash-gpu: the parameter buffer could not be mapped -- nothing "
                          "dispatched (the NONE backend cannot map; this is a [gpu] path)");
                return false;
            }

            nri::BufferViewDesc cbViewDesc = {};
            cbViewDesc.buffer = objects.cb;
            cbViewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
            cbViewDesc.offset = 0;
            cbViewDesc.size   = kFaultCBSize;   // 256-aligned; see kFaultCBSize

            nri::BufferViewDesc sinkViewDesc = {};
            sinkViewDesc.buffer          = objects.sink;
            sinkViewDesc.type            = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
            sinkViewDesc.offset          = 0;
            sinkViewDesc.size            = sinkDesc.size;
            sinkViewDesc.structureStride = kFaultSinkStride;

            if (!ARC_NRI_CHECK(core.CreateBufferView(cbViewDesc, objects.cbView)) || !objects.cbView
                || !ARC_NRI_CHECK(core.CreateBufferView(sinkViewDesc, objects.sinkView)) || !objects.sinkView)
            {
                ARC_ERROR("[nri] --crash-gpu: buffer view creation failed");
                return false;
            }

            nri::DescriptorPoolDesc poolDesc = {};
            poolDesc.descriptorSetMaxNum           = 1;
            poolDesc.constantBufferMaxNum          = 1;
            poolDesc.storageStructuredBufferMaxNum = 1;
            if (!ARC_NRI_CHECK(core.CreateDescriptorPool(device.Device(), poolDesc, objects.pool))
                || !objects.pool)
            {
                ARC_ERROR("[nri] --crash-gpu: descriptor pool creation failed");
                return false;
            }

            nri::DescriptorSet* set = nullptr;
            if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*objects.pool, *objects.layout, 0, &set, 1, 0))
                || !set)
            {
                ARC_ERROR("[nri] --crash-gpu: descriptor set allocation failed");
                return false;
            }

            const nri::Descriptor* cbDescriptor   = objects.cbView;
            const nri::Descriptor* sinkDescriptor = objects.sinkView;
            nri::UpdateDescriptorRangeDesc updates[2] = {};
            updates[0].descriptorSet = set;
            updates[0].rangeIndex    = 0;
            updates[0].descriptors   = &cbDescriptor;
            updates[0].descriptorNum = 1;
            updates[1].descriptorSet = set;
            updates[1].rangeIndex    = 1;
            updates[1].descriptors   = &sinkDescriptor;
            updates[1].descriptorNum = 1;
            core.UpdateDescriptorRanges(updates, 2);

            if (!ARC_NRI_CHECK(core.CreateCommandAllocator(queue, objects.allocator)) || !objects.allocator
                || !ARC_NRI_CHECK(core.CreateCommandBuffer(*objects.allocator, objects.cmd)) || !objects.cmd)
            {
                ARC_ERROR("[nri] --crash-gpu: command buffer creation failed");
                return false;
            }

            // Loud and unconditional: this is the one log line that will be
            // in the console when the session dies, and the difference between
            // "the diagnostics arc works" and "the runtime crashed" is whether
            // a reader can find it.
            ARC_WARN("[nri] --crash-gpu: dispatching a DELIBERATE GPU fault "
                     "({} groups x 64 threads, {} iterations, OOB element {}). "
                     "The device is expected to be lost.",
                     kFaultThreadGroups, kFaultIterations, kFaultOobElement);

            if (!ARC_NRI_CHECK(core.BeginCommandBuffer(*objects.cmd, objects.pool)))
            {
                ARC_ERROR("[nri] --crash-gpu: BeginCommandBuffer failed -- nothing dispatched");
                return false;
            }

            // =========================================================
            // THE HAND-WRITTEN BARRIER, AND WHY IT IS ALLOWED HERE
            // =========================================================
            // The phase rule is absolute for RENDER work: all barriers are
            // derived by RenderGraph::Compile from declared accesses, and a
            // node never records CmdBarrier. This is a DELIBERATE,
            // diagnostics-only exemption -- the same one Phase 1's smoke
            // carried -- and it is exempt for a structural reason rather than
            // convenience: this command buffer is NOT graph work. It is
            // allocated here, carries exactly one dispatch, is submitted
            // directly to the queue, and names a resource that no graph node
            // has ever declared or ever will. There is no declaration for
            // Compile to derive a barrier FROM, and routing a fault injector
            // through the frame graph would put a deliberate TDR inside the
            // very machinery the crash report has to survive to describe.
            //
            // Scope of the exemption: this function, this buffer, this one
            // barrier. Nothing in Render/Nri/ outside this block records a
            // CmdBarrier by hand, and nothing may start to.
            nri::BufferBarrierDesc sinkBarrier = {};
            sinkBarrier.buffer = objects.sink;
            // `before` is RenderGraph.cpp's kUnknownState minus the layout a
            // buffer does not have -- {NONE, StageBits::ALL}, where ALL is 0,
            // NRI's documented lazy default for "nothing has touched this".
            sinkBarrier.before = { nri::AccessBits::NONE, nri::StageBits::ALL };
            sinkBarrier.after  = { nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                   nri::StageBits::COMPUTE_SHADER };

            nri::BarrierDesc barriers = {};
            barriers.buffers   = &sinkBarrier;
            barriers.bufferNum = 1;
            core.CmdBarrier(*objects.cmd, barriers);

            // THE breadcrumb this whole command exists to produce. The scope
            // is opened HERE rather than by the caller (see
            // GpuFaultInjector.hpp, which is now just the name): the scope name
            // IS the payload -- reading "pass:gpu-fault" back out of a
            // `.arcdiag` is the desk battery item -- so it must not be
            // forgettable at a call site.
            //
            // A compact NodeScope (RenderGraphExec.cpp): the annotation always,
            // the CPU ring when a backend is armed, and the native marker only
            // when that backend's marker buffer lives on THIS device.
            core.CmdBeginAnnotation(*objects.cmd, GpuFaultInjector::kPassName, nri::BGRA_UNUSED);

            IGpuCrashBackend* backend = ActiveGpuCrashBackend();
            std::uint32_t     token   = 0;
            void*             native  = nullptr;
            if (backend)
            {
                token = backend->Breadcrumbs().BeginScope(GpuFaultInjector::kPassName);
                void* const nativeCmd = core.GetCommandBufferNativeObject(objects.cmd);
                void* const nativeDev = core.GetDeviceNativeObject
                                            ? core.GetDeviceNativeObject(&device.Device())
                                            : nullptr;
                if (nativeCmd && nativeDev && backend->NativeDevice() == nativeDev)
                {
                    native = nativeCmd;
                    (void)backend->WriteMarkerNative(native, token, true);
                }
            }

            core.CmdSetPipelineLayout(*objects.cmd, nri::BindPoint::COMPUTE, *objects.layout);
            core.CmdSetPipeline(*objects.cmd, *objects.pipeline);

            nri::SetDescriptorSetDesc setBind = {};
            setBind.setIndex      = 0;
            setBind.descriptorSet = set;
            setBind.bindPoint     = nri::BindPoint::COMPUTE;
            core.CmdSetDescriptorSet(*objects.cmd, setBind);

            nri::DispatchDesc dispatch = {};
            dispatch.x = kFaultThreadGroups;
            dispatch.y = 1;
            dispatch.z = 1;
            core.CmdDispatch(*objects.cmd, dispatch);

            if (backend)
            {
                if (native)
                    (void)backend->WriteMarkerNative(native, token, false);
                backend->Breadcrumbs().EndScope(token);
            }
            core.CmdEndAnnotation(*objects.cmd);

            if (!ARC_NRI_CHECK(core.EndCommandBuffer(*objects.cmd)))
            {
                ARC_ERROR("[nri] --crash-gpu: EndCommandBuffer failed -- nothing dispatched");
                return false;
            }

            nri::CommandBuffer* const submitted[1] = { objects.cmd };
            nri::QueueSubmitDesc submit = {};
            submit.commandBuffers   = submitted;
            submit.commandBufferNum = 1;
            if (!ARC_NRI_CHECK(core.QueueSubmit(queue, submit)))
            {
                ARC_ERROR("[nri] --crash-gpu: QueueSubmit failed -- nothing dispatched");
                return false;
            }

            // Best effort, and the ONLY correct kind here: the objects above
            // are freed by ~FaultObjects the moment this returns, and they may
            // not be freed while the GPU is still reading them. On a healthy
            // device this idle returns normally; on the device this call is
            // trying to kill it returns a DEVICE_LOST that ARC_NRI_CHECK
            // routes straight into NriCommon's typed device-loss path, i.e.
            // into the very observation chain Arm() installed. Either way the
            // wait is what makes the teardown legal.
            (void)ARC_NRI_CHECK(core.QueueWaitIdle(&queue));

            // ...on VULKAN, where vkQueueWaitIdle genuinely blocks until the
            // driver has finished losing the device and then returns
            // VK_ERROR_DEVICE_LOST, which ARC_NRI_CHECK routes into
            // NriCheckImpl's typed branch.
            //
            // On D3D12 that sentence is a wish TWICE OVER, and the second half
            // is what D3b's first attempt at this missed:
            //
            //  (i)  QueueD3D12::WaitIdle returns SUCCESS even when its fence
            //       wait failed, because FenceD3D12::Wait is `void`
            //       (DeviceRemovedObservers.hpp carries the full citation). So
            //       ARC_NRI_CHECK above can never see DEVICE_LOST here.
            //  (ii) That fence wait does not even wait for the TDR. It is
            //       WaitForSingleObjectEx(m_Event, NRI_TIMEOUT_FENCE, TRUE)
            //       with NRI_TIMEOUT_FENCE == 5000u (SharedExternal.h:53), and
            //       on this path it comes back WAIT_TIMEOUT with the GPU still
            //       hung and the device still PRESENT. The desk transcript is
            //       unambiguous: fault dispatched 02:14:16.157, "nri::
            //       FenceD3D12::Wait: WaitForSingleObjectEx() failed!" at
            //       02:14:21.158 -- 5.001 s, i.e. exactly the timeout -- while
            //       the SAME fault on the SAME GPU took vkQueueWaitIdle
            //       15.9 s (02:14:59.626 -> 02:15:15.528) to come back
            //       DEVICE_LOST.
            //
            // (ii) is why asking GetDeviceRemovedReason() immediately after
            // the wait answered S_OK and observed nothing: at T+5 s the device
            // is HUNG, not yet REMOVED. So do the waiting NRI's fence gave up
            // on, then hand the answer to the SAME NoteDeviceLost the typed
            // branch uses -- dx12 then walks the identical chain vulkan walks:
            // device-removed hook -> ObserveDeviceRemoved -> "gpu-crash:
            // device removed" report + `.gpudump` -> NoteGpuDeviceLost -> the
            // host's exit 1.
            //
            // The budget is generous on purpose (observed 15.9 s here; TdrDelay
            // is machine-configurable) and bounded on purpose (a `--crash-gpu`
            // that somehow did NOT kill the device must not wedge the host).
            // This whole function is `#if !defined(ARC_BUILD_DIST)` and only runs
            // under an explicit `--crash-gpu`, so a multi-second poll costs
            // nothing anyone ships. The hang watchdog WILL file a report while
            // we sit here -- vulkan's arm already does exactly that at ~12 s
            // and the battery already expects it.
            if (device.Backend() == GraphicsBackend::D3D12 && !GpuDeviceLostObserved())
            {
                const DiagnosticsGpuFaultSettings& fault = Settings<DiagnosticsGpuFaultSettings>();
                const auto kRemovalBudget = std::chrono::seconds(fault.removalBudgetSeconds);
                const auto kRemovalPoll   = std::chrono::milliseconds(fault.removalPollMs);

                void* const nativeDev = core.GetDeviceNativeObject
                                            ? core.GetDeviceNativeObject(&device.Device())
                                            : nullptr;

                const auto deadline = std::chrono::steady_clock::now() + kRemovalBudget;
                bool       removed  = D3D12NativeDeviceRemoved(nativeDev);
                while (!removed && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(kRemovalPoll);
                    removed = D3D12NativeDeviceRemoved(nativeDev);
                }

                if (removed)
                {
                    RenderErrorLatch::Instance().NoteDeviceLost(
                        "nri",
                        "[nri] --crash-gpu: ID3D12Device::GetDeviceRemovedReason says the device "
                        "is gone -- DEVICE_LOST (QueueD3D12::WaitIdle could not report it: it "
                        "returns the result of CREATING its scratch fence, FenceD3D12::Wait is "
                        "void, and that wait only ever waits NRI_TIMEOUT_FENCE = 5s)");
                }
                else
                {
                    ARC_ERROR("[nri] --crash-gpu: the D3D12 device was still present after "
                              "waiting {}s for the removal -- no device-loss observation was "
                              "made; the report will describe whatever happens next",
                              kRemovalBudget.count());
                }
            }

            // The wait never made the teardown legal -- see FaultObjects::
            // Disarm for why that is unconditional on D3D12 and must NOT be
            // done on Vulkan. Not gated on GpuDeviceLostObserved(): the whole
            // purpose of this function is to hang the GPU, so there is no
            // D3D12 outcome in which the fault objects are provably free to
            // release -- including the branch above where we could not even
            // confirm the removal, which is the LEAST safe case of all.
            if (device.Backend() == GraphicsBackend::D3D12)
                objects.Disarm();

            return true;
        }
    }
}

#endif   // !ARC_BUILD_DIST
