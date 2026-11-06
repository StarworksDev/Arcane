// NRI substrate: result-check discipline (ARC_NRI_CHECK/NriCheckImpl), the
// callback-to-latch wiring (MakeNriCallbacks), and the one-line identity log
// (LogNriIdentity). Device-less -- [nri], inside the ~[gpu] baseline set.
//
// Include order matters in this file: NRI's Extensions/NRIDeviceCreation.h
// declares nri::Message with an enumerator literally named ERROR, and
// <windows.h> #defines ERROR via wingdi.h. Once that macro is live, every
// later textual "ERROR" in this translation unit -- including a qualified
// nri::Message::ERROR -- gets corrupted by preprocessor substitution. Keep the
// NRI includes first.
//
// The route into spdlog is Arcane/Render/RenderErrorLatch.hpp -> Base/Log.hpp.
// This file names RenderDeviceDesc.hpp (for the desc) and the latch (for
// RenderErrorCount) directly.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>   // RenderDebugSettings -- the per-configuration validation default
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "Helpers/GpuCapability.hpp"

TEST_CASE("nri: ARC_NRI_CHECK on SUCCESS returns true and does not bump RenderErrorCount", "[nri]")
{
    const uint64_t before = Arcane::RenderErrorCount();

    const bool ok = ARC_NRI_CHECK(nri::Result::SUCCESS);

    CHECK(ok);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("nri: ARC_NRI_CHECK on a failure result returns false and bumps RenderErrorCount by exactly 1",
          "[nri][expects-render-errors]")
{
    // This is the one [nri] case that deliberately trips the REAL, shared
    // 0/0 gate latch (proving ARC_NRI_CHECK reaches it, not a fake local
    // counter) -- reset around it so this doesn't leak a permanent +1 into
    // every other test case's RenderErrorCount()==0 assertion for the rest
    // of the process (same "other cases must not leak into this one"
    // reasoning as GpuCrashReportTest.cpp's device-lost latch reset).
    Arcane::ResetRenderErrorCount();
    REQUIRE(Arcane::RenderErrorCount() == 0);

    const bool ok = ARC_NRI_CHECK(nri::Result::FAILURE);

    CHECK_FALSE(ok);
    CHECK(Arcane::RenderErrorCount() == 1);

    Arcane::ResetRenderErrorCount();
}

namespace
{
    // File scope, not a lambda: DeviceRemovedHook is a RAW function pointer
    // with no context parameter, so nothing capturing can convert to it.
    bool g_deviceRemovedHookFired = false;

    void RecordDeviceRemovedHook() { g_deviceRemovedHookFired = true; }
}

TEST_CASE("nri: NoteError bumps RenderErrorCount by exactly 1 and never fires the device-removed hook",
          "[nri][expects-render-errors]")
{
    // Same bracket as the ARC_NRI_CHECK failure case above: this case
    // deliberately trips the REAL shared 0/0 latch (that is the point --
    // NoteError must reach the counter RenderErrorCount() reads, not a local
    // copy), so it resets around itself rather than leaking a permanent +1.
    Arcane::ResetRenderErrorCount();
    REQUIRE(Arcane::RenderErrorCount() == 0);

    g_deviceRemovedHookFired = false;
    Arcane::SetRenderDeviceRemovedHookForTest(&RecordDeviceRemovedHook);

    // The text carries the exact substring RenderErrorLatch matches on
    // (NotifyIfDeviceRemoved looks for "Device Removed"). Pushed through
    // NoteNriError -- which is what NriCommon's RouteNriError and the Vulkan
    // debug messenger both call -- this WOULD fire the hook, and
    // firing it from an InfoQueue1 callback thread, mid-D3D12-call, is the
    // re-entrancy hazard NoteError exists to kill. Through NoteError it must
    // stay silent.
    Arcane::NoteRenderErrorForTest("d3d12", "Device Removed! (synthetic, from a unit test)");

    CHECK(Arcane::RenderErrorCount() == 1);
    CHECK_FALSE(g_deviceRemovedHookFired);

    // Clear before RecordDeviceRemovedHook could be reached by anything else,
    // and restore the latch for every other case's RenderErrorCount()==0.
    Arcane::SetRenderDeviceRemovedHookForTest(nullptr);
    Arcane::ResetRenderErrorCount();
}

TEST_CASE("nri: NONE-backend device lifecycle via MakeNriCallbacks/LogNriIdentity leaves RenderErrorCount untouched", "[nri]")
{
    const uint64_t before = Arcane::RenderErrorCount();

    nri::DeviceCreationDesc desc{};
    desc.graphicsAPI = nri::GraphicsAPI::NONE;
    desc.callbackInterface = Arcane::MakeNriCallbacks();

    nri::Device* device = nullptr;
    // NONE-backend carve-out (task-4 brief): nriCreateDevice is the standard
    // creation path for every NRI backend, but the wrapper-path-only rule
    // forbids it for REAL backends (D3D12/VK), which must go through the
    // native-device wrapper extensions (nriCreateDeviceFrom*Device) instead.
    // NONE has no native device to wrap -- there is nothing to wrap it FROM
    // -- so nriCreateDevice is the only creation path that exists for it,
    // and is acceptable here, and only here.
    const nri::Result result = nriCreateDevice(desc, device);
    REQUIRE(result == nri::Result::SUCCESS);
    REQUIRE(device != nullptr);

    Arcane::LogNriIdentity(*device);

    nriDestroyDevice(device);

    CHECK(Arcane::RenderErrorCount() == before);
}

// ---------------------------------------------------------------------------
// Task 7: the wrap smoke -- [gpu], outside the ~[gpu] baseline set.
//
// SCAFFOLDING with a planned deletion point: Phase 1's own hand-rolled
// triangle scaffold was retired at Task 13, once the frame-graph vehicle
// (--nri-graph) rendered real content through NRI. This pair of cases is a
// DELIBERATE carry-forward past that point -- it is the only device-less-
// adjacent [gpu] proof that the wrap itself (native device -> NRI, both
// backends) still works, which nothing else in the tree currently covers --
// and goes away at Phase 5, once the wrapper path is covered by something
// that draws.
//
// Device-less coverage of the wrap is impossible by construction -- wrapping
// needs a REAL native device, which is precisely what [gpu] means here. What
// these prove at the desk: our creation half produces a device NRI accepts
// through the WRAPPER entry point (never nriCreateDevice), the post-wrap
// asserts hold (CoreInterface resolves, GetQueue(GRAPHICS, 0) succeeds --
// the clamp-to-zero canary), and the teardown order of contract item 15
// (NRI device first, native device second) runs clean with no growth in the
// shared 0/0 error latch.
//
// Everything goes through NriDevice's own methods on purpose: this exe links
// its own static copy of NRI, so calling nri* functions directly on a device
// created inside ArcaneClient.dll would cross function tables.
// ---------------------------------------------------------------------------

namespace
{
    void CheckNriWrapSmoke(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);

        // before/after rather than ResetRenderErrorCount: this case is not
        // meant to trip the shared 0/0 latch, so it must not touch it either.
        const uint64_t before = Arcane::RenderErrorCount();

        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        desc.enableValidation = Arcane::RenderDebugSettings{}.validation;   // the per-configuration default (on in Debug)

        // The creation half -- the same function the engine's own boot runs
        // (Render/DeviceCreation{D3D12,Vulkan}.cpp), so what gets wrapped here
        // is what the engine boots with.
        auto native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(native != nullptr);
        REQUIRE(native->Backend() == backend);

        auto nri = Arcane::NriDevice::Wrap(*native);
        REQUIRE(nri != nullptr);
        CHECK(nri->Backend() == backend);
        // Post-wrap asserts already ran inside Wrap (a miss returns null);
        // re-read the queue here so the case states what it depends on.
        CHECK(nri->GraphicsQueue() != nullptr);
        CHECK(nri->Graves().Pending() == 0);

        // Contract item 15, made explicit rather than left to declaration
        // order: the NRI device dies BEFORE the native device it wraps.
        nri.reset();
        native.reset();

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("nri wrap smoke: d3d12 native device wraps through nriCreateDeviceFromD3D12Device", "[gpu][nri][d3d12]")
{
    CheckNriWrapSmoke(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("nri wrap smoke: vulkan native device wraps through nriCreateDeviceFromVKDevice", "[gpu][nri][vulkan]")
{
    CheckNriWrapSmoke(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// Task 5: Graveyard -- fence-tagged deferred destruction. Pure-core cases
// first (no NRI device involved at all), then one NONE-device integration
// case proving Bury/Reap drives a real NRI CoreInterface function-table
// destroy call. Added here rather than a sibling file per the task-5 brief:
// this file stayed small (< 100 lines before this section) and Graveyard is
// the NRI substrate's next load-bearing piece, same as Task 4's cases.
// ---------------------------------------------------------------------------

TEST_CASE("graveyard: burial order is preserved for thunks buried at the same fence value", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<int> ran;

    graveyard.Bury(1, [&ran] { ran.push_back(1); });
    graveyard.Bury(1, [&ran] { ran.push_back(2); });
    graveyard.Bury(1, [&ran] { ran.push_back(3); });

    REQUIRE(graveyard.Pending() == 3);

    graveyard.Reap(1);

    CHECK(ran == std::vector<int>{1, 2, 3});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: Reap(completed) runs every thunk with fenceValue <= completed and leaves the rest", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<std::uint64_t> ran;

    graveyard.Bury(3, [&ran] { ran.push_back(3); });
    graveyard.Bury(5, [&ran] { ran.push_back(5); });
    graveyard.Bury(6, [&ran] { ran.push_back(6); });

    graveyard.Reap(5);

    // Not 6: fenceValue 6 has not completed yet at completedValue == 5.
    CHECK(ran == std::vector<std::uint64_t>{3, 5});
    CHECK(graveyard.Pending() == 1);

    graveyard.Reap(6);

    CHECK(ran == std::vector<std::uint64_t>{3, 5, 6});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: interleaved Bury/Reap calls run each thunk exactly once, in burial order", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<int> ran;

    graveyard.Bury(1, [&ran] { ran.push_back(1); });
    graveyard.Reap(1);
    CHECK(ran == std::vector<int>{1});

    graveyard.Bury(2, [&ran] { ran.push_back(2); });
    graveyard.Bury(2, [&ran] { ran.push_back(20); });

    // Reap at a value below both pending fence values: neither is due yet.
    graveyard.Reap(1);
    CHECK(ran == std::vector<int>{1});
    CHECK(graveyard.Pending() == 2);

    graveyard.Bury(4, [&ran] { ran.push_back(4); });

    // Reap between fence 2 and fence 4: the fence-2 pair is due, fence 4 is not.
    graveyard.Reap(3);
    CHECK(ran == std::vector<int>{1, 2, 20});
    CHECK(graveyard.Pending() == 1);

    graveyard.Reap(4);
    CHECK(ran == std::vector<int>{1, 2, 20, 4});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: Drain runs every pending thunk regardless of fence value", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<int> ran;

    graveyard.Bury(10, [&ran] { ran.push_back(10); });
    graveyard.Bury(20, [&ran] { ran.push_back(20); });
    graveyard.Bury(30, [&ran] { ran.push_back(30); });

    REQUIRE(graveyard.Pending() == 3);

    // None of these fence values have "completed" from any queue's
    // perspective -- Drain runs them anyway, which is exactly its contract
    // (teardown / device-loss path; caller has already idled the GPU).
    graveyard.Drain();

    CHECK(ran == std::vector<int>{10, 20, 30});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: Pending reflects the exact count of unreaped burials", "[nri]")
{
    Arcane::Graveyard graveyard;
    CHECK(graveyard.Pending() == 0);

    graveyard.Bury(1, [] {});
    CHECK(graveyard.Pending() == 1);

    graveyard.Bury(1, [] {});
    graveyard.Bury(2, [] {});
    CHECK(graveyard.Pending() == 3);

    graveyard.Reap(1);
    CHECK(graveyard.Pending() == 1);

    graveyard.Reap(2);
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: an explicit Drain leaves it empty, so destruction takes the safe (nothing-to-do) path", "[nri]")
{
    // Exercises the same "must be empty/drained before destruction" contract
    // the destructor enforces (fatal ARC_ASSERT in debug; a WARN + Drain in
    // release -- see Graveyard.cpp) without needing to observe either of
    // those config-specific behaviors directly: Drain() is the exact
    // operation release-mode destruction performs, called here explicitly.
    std::vector<int> ran;
    {
        Arcane::Graveyard graveyard;
        graveyard.Bury(1, [&ran] { ran.push_back(1); });
        graveyard.Bury(2, [&ran] { ran.push_back(2); });

        graveyard.Drain();

        CHECK(ran == std::vector<int>{1, 2});
        CHECK(graveyard.Pending() == 0);

        // graveyard goes out of scope here with Pending() == 0: the
        // destructor's debug-mode ARC_ASSERT(m_graves.empty()) passes
        // silently, and its release-mode Drain() is a no-op.
    }
}

// ---------------------------------------------------------------------------
// Post-review fixes (code review on the initial Task-5 commit): reentrancy
// and exception safety. Reentrant Bury()/Reap()/Drain() from within a
// running destroy thunk is a documented-forbidden, debug-asserted contract
// violation -- like the destructor's debug-mode assert (see the case above),
// this is NOT exercised by a test: the assert is fatal (std::abort(), via
// Mosaic's FailFatal), so a Catch2 case cannot trigger it and survive to
// report a result. See the class comment in Graveyard.hpp for the contract
// and the reasoning for choosing "forbid and assert" over "make it safe."
//
// What IS testable in release semantics is the exception-safety half: a
// throwing thunk's entry (and everything buried before it) must never
// re-run on a subsequent Reap()/Drain() call. Both cases below use
// CHECK_THROWS_AS, which catches the exception internally -- the test itself
// stays exception-clean.
// ---------------------------------------------------------------------------

TEST_CASE("graveyard: Reap erases a throwing thunk's entry (and everything before it) before the exception propagates, so neither re-runs", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<int> ran;

    graveyard.Bury(1, [&ran] { ran.push_back(1); });
    graveyard.Bury(1, [&ran] { ran.push_back(2); throw std::runtime_error("thunk 2 throws"); });
    graveyard.Bury(1, [&ran] { ran.push_back(3); });

    REQUIRE(graveyard.Pending() == 3);

    // All three are due at Reap(1); the second throws mid-sweep.
    CHECK_THROWS_AS(graveyard.Reap(1), std::runtime_error);

    // Thunks 1 and 2 both ran (2's push_back happened before its throw).
    // Thunk 3 never got a chance -- the sweep stopped at the exception.
    CHECK(ran == std::vector<int>{1, 2});
    // 1 and 2 are erased (the throwing entry counts as "executed" and is
    // never retried); 3 is still pending.
    CHECK(graveyard.Pending() == 1);

    // A second Reap must NOT re-run thunks 1 or 2 -- only thunk 3, which was
    // never attempted, runs.
    graveyard.Reap(1);
    CHECK(ran == std::vector<int>{1, 2, 3});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: Drain erases a throwing thunk's entry (and everything before it) before the exception propagates, so neither re-runs", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<int> ran;

    graveyard.Bury(10, [&ran] { ran.push_back(1); });
    graveyard.Bury(20, [&ran] { ran.push_back(2); throw std::runtime_error("thunk 2 throws"); });
    graveyard.Bury(30, [&ran] { ran.push_back(3); });

    REQUIRE(graveyard.Pending() == 3);

    CHECK_THROWS_AS(graveyard.Drain(), std::runtime_error);

    CHECK(ran == std::vector<int>{1, 2});
    CHECK(graveyard.Pending() == 1);

    // A second Drain must not re-run 1 or 2 -- only the untouched 3 runs.
    graveyard.Drain();
    CHECK(ran == std::vector<int>{1, 2, 3});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("nri: Graveyard defers a CoreInterface DestroyBuffer through a NONE device to Reap, no latch growth", "[nri]")
{
    const uint64_t before = Arcane::RenderErrorCount();

    nri::DeviceCreationDesc desc{};
    desc.graphicsAPI = nri::GraphicsAPI::NONE;
    desc.callbackInterface = Arcane::MakeNriCallbacks();

    nri::Device* device = nullptr;
    // NONE-backend carve-out -- see the lifecycle test above for the full
    // rationale (nriCreateDevice is the only creation path NONE has).
    REQUIRE(ARC_NRI_CHECK(nriCreateDevice(desc, device)));
    REQUIRE(device != nullptr);

    nri::CoreInterface core{};
    REQUIRE(ARC_NRI_CHECK(nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core)));

    // NONE's CreateBuffer (ImplNONE.cpp) hands back a non-null sentinel
    // object rather than a real allocation, and DestroyBuffer is a no-op --
    // there is nothing to leak-check on this backend. What this case proves
    // is that Bury/Reap correctly threads a REAL NRI function-table call
    // (core.DestroyBuffer, resolved through nriGetInterface, same as
    // production code would call it) through to execution at the right
    // fence value, not a fake/local stand-in for "destroy".
    nri::BufferDesc bufferDesc{};
    bufferDesc.size = 256;

    nri::Buffer* buffer = nullptr;
    REQUIRE(ARC_NRI_CHECK(core.CreateBuffer(*device, bufferDesc, buffer)));
    REQUIRE(buffer != nullptr);

    Arcane::Graveyard graveyard;
    graveyard.Bury(42, [&core, buffer] { core.DestroyBuffer(buffer); });
    CHECK(graveyard.Pending() == 1);

    // NONE-backend footgun (brief): GetFenceValue is hard-wired to 0
    // (ImplNONE.cpp) -- there is nothing meaningful to read back, so this
    // feeds Reap the releasing value by hand rather than querying the fence.
    graveyard.Reap(42);
    CHECK(graveyard.Pending() == 0);

    nriDestroyDevice(device);

    CHECK(Arcane::RenderErrorCount() == before);
}
