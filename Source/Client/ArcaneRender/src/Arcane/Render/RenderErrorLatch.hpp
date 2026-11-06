#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// THE RENDER-LAYER ERROR LATCH -- the 0/0 gate's one counter, and the one
// device-removed hook slot.
//
// It is the instrument every host, every GPU test and every desk battery
// reads (`RenderErrorCount B -> N`), and every render-side producer --
// ARC_NRI_CHECK, the executor, the nodes, the pipeline cache, the ImGui
// layer -- bumps it.
//
// A process-wide singleton owned by ArcaneRender.dll (Instance() is exported;
// other modules' producers reach the same latch), with the counters below
// and the public seams -- which is why the exported *ForTest shims exist.

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Base/Log.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace Arcane
{
    class RenderErrorLatch final
    {
    public:
        // F-3b: a device loss is reported through THIS sink, on submit, on
        // both backends -- which makes it the earliest cross-backend
        // observation point (it fires before the next Present), so a GPU crash
        // backend installs a hook here rather than inventing a second,
        // backend-shaped detection path.
        //
        // THE PRODUCERS are NRI's own: ARC_NRI_CHECK's typed DEVICE_LOST
        // branch (NoteDeviceLost, which does not depend on message text at
        // all), and the "Device Removed" substring scan below, fed by
        // RouteNriError.
        //
        // Raw function pointer + last-writer-wins, mirroring the diagnostics
        // Sink slot. The hook must be cleared by whoever installed it, before
        // the object it captures dies.
        using DeviceRemovedHook = void (*)();

        // Out of line (RenderErrorLatch.cpp) and exported: ONE latch per process.
        // A header-defined function-local static would give every module image
        // that bumps the latch (ArcaneImGui's ImGuiNri, ...) its own copy, and
        // its errors would never reach the gate's count (P2 header-statics sweep).
        static ARC_RENDER_API RenderErrorLatch& Instance();

        uint64_t ErrorCount() const { return m_errorCount.load(); }

        // Every Note* call bumps this with the resettable count. Nothing
        // stores zero back into it. The ArcaneTests listener samples it so a
        // case cannot hide a rise by calling ResetForTest before the sample.
        uint64_t ErrorTotal() const { return m_errorTotal.load(); }

        // Test support ONLY. Clears the resettable count tests assert on.
        // Does not clear ErrorTotal. Production code must never call this.
        // It exists so a test that deliberately drives an Error/Fatal through
        // this sink (proving a discipline macro or callback wiring actually
        // reaches the shared 0/0 gate latch, e.g. NriCommon's ARC_NRI_CHECK)
        // can restore the count afterward instead of permanently failing
        // every OTHER test case's RenderErrorCount()==0 assertion. Same
        // idiom as GpuInstrumentation's ResetGpuDeviceLost(), reused by
        // GpuCrashReportTest.cpp. A case that does this on purpose is tagged
        // [expects-render-errors]: the suite gate reads ErrorTotal, which
        // this reset leaves alone.
        void ResetForTest() noexcept { m_errorCount.store(0); }

        void SetDeviceRemovedHook(DeviceRemovedHook hook) noexcept
        {
            m_deviceRemovedHook.store(hook, std::memory_order_release);
        }

        // What the slot currently holds, or null. Read-only and side-effect
        // free -- it never invokes the hook. Exists because the slot is
        // last-writer-wins and Render/Nri/NriDiagnostics is its one writer:
        // "did arming actually install one, and did a second arm leave it
        // alone" is a property worth pinning, and it is unobservable without
        // a reader.
        // (Named Current* because a member called `DeviceRemovedHook` would
        // shadow the type alias above inside this class.)
        [[nodiscard]] DeviceRemovedHook CurrentDeviceRemovedHook() const noexcept
        {
            return m_deviceRemovedHook.load(std::memory_order_acquire);
        }

        // THE TAGGED ERROR SEAM, for every producer that is not the
        // substring path below -- the D3D12 debug layer's InfoQueue1
        // callback, and the frame graph's `NoteError("nri-graph", ...)`. It
        // gives such a producer the two things it needs and withholds the one
        // it must not have:
        //
        //   - It bumps the SAME atomic RenderErrorCount() reads, so a D3D12
        //     VUID or a graph refusal fails the 0/0 gate exactly like any
        //     other render-layer error. That is the whole point of the latch.
        //   - It logs under the producer's OWN tag, rather than borrowing
        //     whatever tag the seam it reached the latch through happens to
        //     print -- a tag that names the wrong origin is the first thing
        //     to mislead whoever reads the log. (See NoteNriError's
        //     tag-honesty caveat for what that costs a producer which cannot
        //     have its own.)
        //   - It deliberately does NOT run NotifyIfDeviceRemoved. That hook
        //     writes a diagnostic report and a minidump; the D3D12 debug
        //     layer invokes its message callback on whatever thread tripped
        //     the error, from INSIDE a D3D12 call, so letting a "Device
        //     Removed" substring there re-enter removal handling is a
        //     re-entrancy hazard for no gain. The submit-time feed (see
        //     NoteNriError and NoteDeviceLost) stays the ONE device-removed
        //     observation point -- F-3b, and the reason the hook lives there.
        void NoteError(const char* tag, const char* text) noexcept
        {
            Bump();
            ARC_ERROR("[{}] {}", tag ? tag : "?", text ? text : "");
        }

        // THE TYPED DEVICE-LOST SEAM. NoteError above
        // deliberately withholds the removal hook because its producer (the
        // D3D12 debug layer) reports from inside a D3D12 call on whatever
        // thread tripped the error. This one is for the opposite case: a
        // producer that holds a TYPED result -- nri::Result::DEVICE_LOST off
        // its own QueueSubmit/AcquireNextTexture/QueuePresent call, on its
        // own render thread, outside any driver callback -- where firing the
        // hook is both safe and the whole point.
        //
        // It exists because the substring path cannot serve that producer:
        // NotifyIfDeviceRemoved matches on "Device Removed" and NRI's failure
        // text says "DEVICE_LOST", so without this seam an NRI device loss
        // would reach the latch but NEVER ObserveDeviceRemoved -- no
        // .arcdiag, no .gpudump, no host shutdown latch. Same counter, same
        // hook, one honest tag.
        void NoteDeviceLost(const char* tag, const char* text) noexcept
        {
            Bump();
            ARC_ERROR("[{}] {}", tag ? tag : "?", text ? text : "");
            if (const DeviceRemovedHook hook = m_deviceRemovedHook.load(std::memory_order_acquire))
                hook();
        }

        // THE SUBSTRING SEAM -- the message-TEXT path into the latch, for a
        // producer that reports an error as a string rather than as a typed
        // result.
        //
        // TWO producers route here: NriCommon's RouteNriError, which every
        // ARC_NRI_CHECK failure funnels into, and DeviceCreationVulkan's
        // VkDebugCallback. Both share this one counter and this one removal
        // scan.
        //
        // TAG-HONESTY CAVEAT, stated rather than hidden: of the two producers
        // that route here, RouteNriError IS NRI, but DeviceCreationVulkan's
        // VkDebugCallback is the VULKAN VALIDATION LAYER reporting on a device
        // NRI created. "[nri]" names the render path rather than the exact
        // producer. It also splits that one callback's output: its warning branch already
        // logs ARC_WARN("[vk] ...") (DeviceCreationVulkan.cpp:183) while its
        // errors leave here tagged "[nri]".
        //
        // DO NOT "FIX" THAT BY REPOINTING THE VULKAN PRODUCER AT NoteError.
        // NoteError(tag, text) does take a per-producer tag, but it
        // DELIBERATELY does not run NotifyIfDeviceRemoved (see its own comment
        // above), and this substring scan is one of the TWO feeds driving that
        // backend's device-removed hook -- DeviceCreationVulkan.cpp:119-127
        // names both. Swapping the call would buy a "[vk]" tag and silently
        // disarm the .arcdiag/.gpudump/shutdown-latch path for Vulkan device
        // loss. Giving THIS seam its own tag parameter is the shape that works.
        void NoteNriError(const char* messageText) noexcept
        {
            Bump();
            ARC_ERROR("[nri] {}", messageText);
            NotifyIfDeviceRemoved(messageText);
        }

        // THERE IS NO FATAL TWIN of the seam above, and NRI's own vocabulary
        // is why none is owed: nri::Message has exactly INFO, WARNING and
        // ERROR (Extensions/NRIDeviceCreation.h) -- no fatal severity at all
        // -- and NriMessageCallback routes only ERROR here, logging the other
        // two straight to ARC_INFO/ARC_WARN (NriCommon.cpp:84-101). No
        // producer could reach a Fatal twin, so adding one means inventing a
        // caller, not just declaring the function.

    private:
        void Bump() noexcept
        {
            ++m_errorTotal;
            ++m_errorCount;
        }

        void NotifyIfDeviceRemoved(const char* messageText)
        {
            // Substring, not equality: NRI's callback interface, the D3D12
            // debug layer and the Vulkan validation messenger each wrap the
            // phrase in their own prefixes and context, so an exact-equality
            // match would miss every one of them.
            if (!messageText || std::strstr(messageText, "Device Removed") == nullptr)
                return;
            if (const DeviceRemovedHook hook = m_deviceRemovedHook.load(std::memory_order_acquire))
                hook();
        }

        std::atomic<uint64_t>          m_errorCount{0};
        std::atomic<uint64_t>          m_errorTotal{0};
        std::atomic<DeviceRemovedHook> m_deviceRemovedHook{nullptr};
    };

    // Resettable render-layer Error/Fatal count. Tests assert this stays
    // zero. ResetRenderErrorCount clears it after a deliberate bump.
    // Producers are the Vulkan debug messenger, the D3D12 debug layer, NRI's
    // callback interface and ARC_NRI_CHECK, and anything else reporting
    // through RenderErrorLatch::NoteError. The suite gate does not sample
    // this: a reset before the listener runs would hide the bump. It samples
    // RenderErrorTotal.
    ARC_RENDER_API uint64_t RenderErrorCount();

    // Monotonic total of the same errors since process start. Nothing resets
    // it, including ResetRenderErrorCount. The ArcaneTests listener samples
    // this around each case, at run end, and once more after Session::run
    // returns.
    ARC_RENDER_API uint64_t RenderErrorTotal();

    // Test support ONLY -- production code must never call this. Clears
    // RenderErrorCount and leaves RenderErrorTotal untouched, so a deliberate
    // bump can be hidden from later RenderErrorCount()==0 assertions and
    // cannot be hidden from the suite gate. Same idiom as
    // ResetGpuDeviceLost() (GpuInstrumentation.hpp).
    ARC_RENDER_API void ResetRenderErrorCount();

    // Test support ONLY -- the two seams a [nri] case needs to prove that
    // RenderErrorLatch::NoteError reaches THIS latch (and that it does
    // NOT fire the device-removed hook). They exist because
    // RenderErrorLatch is a header-only singleton -- a function-local
    // static in RenderErrorLatch.hpp -- so a test exe that included that
    // header would drive its OWN instance while RenderErrorCount(), exported
    // from ArcaneClient.dll, kept reading the DLL's. Production code inside
    // the DLL calls RenderErrorLatch::Instance().NoteError directly and
    // must never reach for these.
    ARC_RENDER_API void NoteRenderErrorForTest(const char* tag, const char* text) noexcept;

    // Installs (or, with nullptr, clears) the device-removed hook on the
    // DLL-side RenderErrorLatch. Last-writer-wins, exactly like the
    // class's own setter -- so a test MUST clear it before the function it
    // names goes out of scope, and must not run while a real device holds
    // the slot (no device exists in the ~[gpu] gate, which is where the one
    // caller lives).
    ARC_RENDER_API void SetRenderDeviceRemovedHookForTest(void (*hook)()) noexcept;

    // Reads the same DLL-side slot back, without ever invoking the hook.
    // Test support ONLY, and for the same header-only-singleton reason as the
    // setter above. The slot is last-writer-wins and
    // Render/Nri/NriDiagnostics::Arm/Disarm is its one writer, so "arming
    // installed one" and "a second arm left it alone" are properties a
    // device-less case can only state if it can read the slot
    // (NriDiagnosticsTest pins both). Production code has no business reading
    // it: the hook exists to be CALLED by RenderErrorLatch, by nobody
    // else.
    [[nodiscard]] ARC_RENDER_API void (*RenderDeviceRemovedHookForTest() noexcept)();
}
