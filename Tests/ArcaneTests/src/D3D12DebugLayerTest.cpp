// Canary for the process-wide D3D12 debug layer in ArcaneTests.
//
// The per-device flag stays at its default (false). What arms the layer is
// ForceD3D12DebugLayerForProcess, called from test_main before any test --
// a case that set enableD3D12DebugLayer itself would pass on the first device
// in the process and would not prove the structural path.
//
// The invalid call is CreateCommittedResource with Width == 0. The runtime
// returns E_INVALIDARG and the device stays. The debug layer stores that as a
// CPU ERROR. Where ID3D12InfoQueue1 registers, the callback latches it during
// the call; where that interface does not, DrainD3D12DebugMessages latches the
// stored message when the NRI device is destroyed. The count is read after
// both owners are released, so either delivery moves it. With the layer off
// the HRESULT still fails and the latch does not move.
//
// [expects-render-errors] tells the suite listener that a rise in
// RenderErrorTotal across this case is deliberate. The reset destructor
// still runs, before testCaseEnded, so the next case's RenderErrorCount()
// assertion sees zero. The listener samples the total, and the reset does
// not clear it, so the tag is what keeps the run green.

#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "Helpers/GpuCapability.hpp"

#include "Helpers/RenderErrorGate.hpp"

#include <cstdint>
#include <string>

#if ARC_PLATFORM_WINDOWS
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <d3d12.h>
#    ifdef ERROR
#        undef ERROR
#    endif

namespace
{
    struct ResetRenderErrors
    {
        ~ResetRenderErrors() { Arcane::ResetRenderErrorCount(); }
    };
}

TEST_CASE("D3D12 debug layer: an invalid CreateCommittedResource increments RenderErrorCount",
          "[gpu][d3d12][validation][expects-render-errors]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);

    // Destroyed last, after the device, so a message raised on the way out
    // cannot leak into the next case.
    ResetRenderErrors reset;

    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;

    auto native = Arcane::NativeDeviceOwner::Create(desc);
    REQUIRE(native != nullptr);
    auto nri = Arcane::NriDevice::Wrap(*native);
    REQUIRE(nri != nullptr);
    REQUIRE(nri->Core().GetDeviceNativeObject != nullptr);

    void* const nativeObject = nri->Core().GetDeviceNativeObject(&nri->Device());
    REQUIRE(nativeObject != nullptr);
    auto* const device = static_cast<ID3D12Device*>(nativeObject);

    // The assertion is about the deliberate call, not about whatever device
    // creation itself reported.
    Arcane::ResetRenderErrorCount();
    REQUIRE(Arcane::RenderErrorCount() == 0);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width            = 0;
    resourceDesc.Height           = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels        = 1;
    resourceDesc.Format           = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* resource = nullptr;
    const HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    if (resource)
        resource->Release();

    CHECK(FAILED(hr));

    // ~NriDevice drains the info queue (NriDevice.cpp, before nriDestroyDevice).
    // Releasing it first covers the callback path and the storage-drain path.
    nri.reset();
    native.reset();
    CHECK(Arcane::RenderErrorCount() > 0);
    (void)reset;
}

// Same Width=0 call. The private gate below is fed as an untagged case and
// must report the rise. This Catch2 case is tagged: the monotonic total
// survives ResetRenderErrors, so an untagged real case would fail the run.
TEST_CASE("D3D12 debug layer: an untagged validation error fails the run after device teardown",
          "[gpu][d3d12][validation][expects-render-errors]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);

    ResetRenderErrors reset;

    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;

    auto native = Arcane::NativeDeviceOwner::Create(desc);
    REQUIRE(native != nullptr);
    auto nri = Arcane::NriDevice::Wrap(*native);
    REQUIRE(nri != nullptr);

    void* const nativeObject = nri->Core().GetDeviceNativeObject(&nri->Device());
    REQUIRE(nativeObject != nullptr);
    auto* const device = static_cast<ID3D12Device*>(nativeObject);

    Arcane::ResetRenderErrorCount();
    REQUIRE(Arcane::RenderErrorCount() == 0);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width            = 0;
    resourceDesc.Height           = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels        = 1;
    resourceDesc.Format           = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* resource = nullptr;
    const HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    if (resource)
        resource->Release();
    CHECK(FAILED(hr));

    nri.reset();
    native.reset();
    const std::uint64_t risen = Arcane::RenderErrorCount();
    CHECK(risen > 0);

    {
        Arcane::Test::RenderErrorGate gate;
        gate.BeginCase("untagged invalid CreateCommittedResource", false, 0);
        gate.EndCase(risen);
        REQUIRE(gate.ExitStatus() != 0);
        REQUIRE(gate.Offenders().size() == 1);
        CHECK(gate.Offenders()[0].name == "untagged invalid CreateCommittedResource");
        CHECK(gate.Offenders()[0].before == 0);
        CHECK(gate.Offenders()[0].after == risen);
        CHECK(gate.Summary().find("untagged invalid CreateCommittedResource") != std::string::npos);
        CHECK(gate.Summary().find("(+" + std::to_string(risen) + ")") != std::string::npos);
    }
    {
        Arcane::Test::RenderErrorGate gate;
        gate.BeginCase("tagged invalid CreateCommittedResource", true, 0);
        gate.EndCase(risen);
        CHECK(gate.ExitStatus() == 0);
        CHECK(gate.Summary().empty());
    }
    {
        Arcane::Test::RenderErrorGate gate;
        gate.BeginRun(0);
        gate.BeginCase("quiet", false, 0);
        gate.EndCase(0);
        gate.EndRun(risen);
        REQUIRE(gate.ExitStatus() != 0);
        REQUIRE(gate.Offenders().size() == 1);
        CHECK(gate.Offenders()[0].name == Arcane::Test::RenderErrorGate::kAfterLastCase);
        CHECK(gate.Offenders()[0].before == 0);
        CHECK(gate.Offenders()[0].after == risen);
        CHECK(gate.Summary().find(Arcane::Test::RenderErrorGate::kAfterLastCase) != std::string::npos);
    }
    {
        Arcane::Test::RenderErrorGate gate;
        gate.BeginRun(0);
        gate.BeginCase("first", false, 0);
        gate.EndCase(0);
        gate.BeginCase("second", false, risen);
        gate.EndCase(risen);
        REQUIRE(gate.Offenders().size() == 1);
        CHECK(gate.Offenders()[0].before == 0);
        CHECK(gate.Offenders()[0].after == risen);
        CHECK(gate.Offenders()[0].name.find("second") != std::string::npos);
        CHECK(gate.ExitStatus() != 0);
    }
    (void)reset;
}
#endif

// Decision logic only. The Catch2 case is tagged because NoteRenderErrorForTest
// moves RenderErrorTotal and the listener samples that. The private gate is
// the untagged case: its body resets the resettable count, and a gate fed the
// total still reports the rise. A gate fed the resettable count does not.
TEST_CASE("render error gate: resetting RenderErrorCount cannot hide an untagged rise",
          "[validation][expects-render-errors]")
{
    struct Reset
    {
        ~Reset() { Arcane::ResetRenderErrorCount(); }
    } reset;

    const std::uint64_t totalBefore = Arcane::RenderErrorTotal();
    const std::uint64_t countBefore = Arcane::RenderErrorCount();
    Arcane::NoteRenderErrorForTest("gate", "deliberate untagged rise");
    CHECK(Arcane::RenderErrorCount() == countBefore + 1);
    const std::uint64_t totalAfter = Arcane::RenderErrorTotal();
    CHECK(totalAfter == totalBefore + 1);

    Arcane::ResetRenderErrorCount();
    CHECK(Arcane::RenderErrorCount() == 0);
    CHECK(Arcane::RenderErrorTotal() == totalAfter);

    {
        // BeginRun at the total this case started from. Earlier cases in the
        // same process have already moved the monotonic total; that history
        // is not this case's rise.
        Arcane::Test::RenderErrorGate gate;
        gate.BeginRun(totalBefore);
        gate.BeginCase("untagged case resets RenderErrorCount", false, totalBefore);
        gate.EndCase(totalAfter);
        REQUIRE(gate.ExitStatus() != 0);
        REQUIRE(gate.Offenders().size() == 1);
        CHECK(gate.Offenders()[0].name == "untagged case resets RenderErrorCount");
        CHECK(gate.Offenders()[0].before == totalBefore);
        CHECK(gate.Offenders()[0].after == totalAfter);
    }
    {
        Arcane::Test::RenderErrorGate hidden;
        hidden.BeginCase("untagged case resets RenderErrorCount", false, 0);
        hidden.EndCase(Arcane::RenderErrorCount());
        CHECK(hidden.ExitStatus() == 0);
        CHECK(hidden.Offenders().empty());
    }
    (void)reset;
}

// A message that shows up at the next BeginCase, at EndRun, or at the sample
// main takes after Session::run, belongs to the case that last ended. A tag
// on that case exempts it. A later untagged case does not inherit the exemption.
TEST_CASE("render error gate: a late rise belongs to the tagged case that last ended", "[validation]")
{
    constexpr std::uint64_t risen = 4;

    {
        Arcane::Test::RenderErrorGate gate;
        gate.BeginRun(0);
        gate.BeginCase("expected", true, 0);
        gate.EndCase(0);
        gate.BeginCase("next", false, risen);
        gate.EndCase(risen);
        CHECK(gate.ExitStatus() == 0);
        CHECK(gate.Offenders().empty());
        REQUIRE(gate.LateAttributions().size() == 1);
        CHECK(gate.LateAttributions()[0].caseName == "expected");
        CHECK(gate.LateAttributions()[0].before == 0);
        CHECK(gate.LateAttributions()[0].after == risen);
    }
    {
        Arcane::Test::RenderErrorGate gate;
        gate.BeginRun(0);
        gate.BeginCase("expected", true, 0);
        gate.EndCase(0);
        gate.EndRun(risen);
        CHECK(gate.ExitStatus() == 0);
        REQUIRE(gate.LateAttributions().size() == 1);
        CHECK(gate.LateAttributions()[0].caseName == "expected");
        CHECK(gate.LateAttributions()[0].before == 0);
        CHECK(gate.LateAttributions()[0].after == risen);
    }
    {
        // testRunEnded sampled nothing; the device reported after run() returned.
        Arcane::Test::RenderErrorGate gate;
        gate.BeginRun(0);
        gate.BeginCase("expected", true, 0);
        gate.EndCase(0);
        gate.EndRun(0);
        gate.EndRun(risen);
        CHECK(gate.ExitStatus() == 0);
        REQUIRE(gate.LateAttributions().size() == 1);
        CHECK(gate.LateAttributions()[0].caseName == "expected");
        CHECK(gate.LateAttributions()[0].after == risen);
    }
    {
        Arcane::Test::RenderErrorGate gate;
        gate.BeginRun(0);
        gate.BeginCase("plain", false, 0);
        gate.EndCase(0);
        gate.EndRun(0);
        gate.EndRun(risen);
        REQUIRE(gate.ExitStatus() != 0);
        REQUIRE(gate.Offenders().size() == 1);
        CHECK(gate.Offenders()[0].name == Arcane::Test::RenderErrorGate::kAfterLastCase);
        CHECK(gate.Offenders()[0].before == 0);
        CHECK(gate.Offenders()[0].after == risen);
        CHECK(gate.LateAttributions().empty());
    }
    {
        Arcane::Test::RenderErrorGate gate;
        gate.BeginRun(0);
        gate.BeginCase("expected", true, 0);
        gate.EndCase(0);
        gate.BeginCase("quiet", false, 0);
        gate.EndCase(0);
        gate.EndRun(risen);
        REQUIRE(gate.ExitStatus() != 0);
        REQUIRE(gate.Offenders().size() == 1);
        CHECK(gate.Offenders()[0].name == Arcane::Test::RenderErrorGate::kAfterLastCase);
        CHECK(gate.LateAttributions().empty());
    }
}
