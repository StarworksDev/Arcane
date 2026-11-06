// ArcaneTests runner entry point (Server CommonTests convention).
// Add new test files under Arcane/Tests/ArcaneTests/src; premake picks them up via the glob.

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "Helpers/RenderErrorGate.hpp"
#include "Helpers/TestTypeContext.hpp"
#include "Helpers/UserDataDirs.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Platform.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Host/VerifyHooks.hpp>
#include <Arcane/ImGui/ImGuiHostHooks.hpp>
#include <Arcane/Render/AgilitySdk.hpp>
#include <Arcane/Render/D3D12DebugLayer.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Astra/Core/TypeContext.hpp>

#include "Helpers/GpuCapability.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Agility SDK handshake: the exported version/path pair (Arcane/Render/AgilitySdk.hpp).
// The macro is empty off Windows.
ARC_AGILITY_SDK_EXPORTS();

namespace
{
    Arcane::Test::RenderErrorGate& SuiteRenderErrorGate()
    {
        static Arcane::Test::RenderErrorGate gate;
        return gate;
    }

    bool ExpectsRenderErrors(Catch::TestCaseInfo const& info)
    {
        for (const Catch::Tag& tag : info.tags)
        {
            if (tag.original == Catch::StringRef("expects-render-errors"))
                return true;
        }
        return false;
    }

    // Samples RenderErrorTotal around each case. The total is monotonic:
    // ResetRenderErrorCount does not clear it, so a scope guard that puts
    // the resettable count back cannot hide a rise. testCaseEnded runs after
    // the case function returns, so the case's device is already destroyed
    // and a stored D3D12 message has had its chance to reach the latch. The
    // rise is recorded, not asserted: a listener cannot fail the case. main
    // reads the gate after session.run.
    class RenderErrorListener final : public Catch::EventListenerBase
    {
    public:
        using Catch::EventListenerBase::EventListenerBase;

        void testRunStarting(Catch::TestRunInfo const&) override
        {
            SuiteRenderErrorGate().BeginRun(Arcane::RenderErrorTotal());
        }

        void testCaseStarting(Catch::TestCaseInfo const& info) override
        {
            SuiteRenderErrorGate().BeginCase(info.name, ExpectsRenderErrors(info),
                                             Arcane::RenderErrorTotal());
        }

        void testCaseEnded(Catch::TestCaseStats const&) override
        {
            SuiteRenderErrorGate().EndCase(Arcane::RenderErrorTotal());
        }

        // ~RunContext calls this before Session::run returns. main samples
        // once more after that, once any helper-held device has been released.
        void testRunEnded(Catch::TestRunStats const&) override
        {
            SuiteRenderErrorGate().EndRun(Arcane::RenderErrorTotal());
        }
    };
}

CATCH_REGISTER_LISTENER(RenderErrorListener)

int main(int argc, char* argv[]) {
    Arcane::InstallVerifyHooks();
    Arcane::InstallImGuiHostHooks();
    // A Dist build resolves game user data under the OS per-user dir (settings
    // S7). Point it at a private folder under TEMP before anything resolves a
    // path -- the pin below included -- so no case writes the real profile.
    const Arcane::Test::PrivateUserDataRoot userData;
    if (!userData.Ok())
    {
        std::fprintf(stderr, "ArcaneTests: no private per-user root, refusing to run: %s\n", userData.Error().c_str());
        return 2;
    }
    // Install the shared context in the TEST module BEFORE any test computes a
    // component TypeID, so engine/plugin/test agree (TypeID caches per-module).
    Astra::SetTypeContext(&Arcane::Test::SharedTypeContext());
    // Same for the ENGINE DLLs' own module slots, and BEFORE any test runs. A
    // throwaway ClientRuntime installs BOTH -- ArcaneClient.dll's from its own
    // ctor, and ArcaneCore.dll's through the Runtime it owns -- and each slot
    // persists after the object dies. This must happen up front because per-type
    // IDs are cached in per-module magic statics and never re-resolve -- pinning
    // later cannot repair an id a DLL already cached.
    // Scoped so it really is throwaway: otherwise it would hold an enkiTS
    // worker pool, an Assets facade and a loaded EngineConfig alive for the
    // whole session.
    {
        Arcane::ClientRuntime pin(Arcane::Test::Process());
    }
    // Route this module's Mosaic guard failures through the engine logger, the
    // same as a host does. This does NOT change whether a FATAL guard aborts:
    // both Mosaic::detail::DefaultAssertHandler (Mosaic/Assert.hpp:96-119) and
    // the installed MosaicAssertHandlerImpl (Assert.cpp:12-26) unconditionally
    // return AssertAction::Break, so FailFatal's abort() fires either way --
    // this call installs neither more nor less of a Break than the default.
    // What it actually buys is WHERE the failure is reported: through the
    // engine logger (the same sink/format an unattended CI run already
    // captures) instead of Mosaic's raw stderr fallback, so a guard failure
    // -- fatal or not -- is diagnosable from the run's normal log output.
    Arcane::Assert::InstallMosaicHandler();

#if ARC_PLATFORM_WINDOWS
    // Before any test creates a device. The capability probe leaves
    // enableD3D12DebugLayer false and would otherwise close the
    // EnableDebugLayer window for the rest of the process. Opt out on a
    // machine whose OSD injection makes the layer fail-fast.
    if (const char* optOut = std::getenv("ARCANE_TESTS_NO_D3D12_DEBUG");
        optOut != nullptr && std::strcmp(optOut, "1") == 0)
    {
        ARC_INFO("ArcaneTests: D3D12 debug layer OFF (ARCANE_TESTS_NO_D3D12_DEBUG=1)");
    }
    else if (Arcane::ForceD3D12DebugLayerForProcess())
    {
        ARC_INFO("ArcaneTests: D3D12 debug layer ON for this process");
    }
    else
    {
        ARC_INFO("ArcaneTests: D3D12 debug layer OFF (ForceD3D12DebugLayerForProcess refused)");
    }
#endif

    // The listener records offenders; it cannot fail a case. A non-zero status
    // from the gate is the suite failing, including when Catch itself passed.
    // testRunEnded samples from inside run(), before this line. Release any
    // device the helpers still hold, then sample the monotonic total again.
    const int catchResult = Catch::Session().run(argc, argv);
    Arcane::Test::ReleaseSharedTestGpuDevices();
    SuiteRenderErrorGate().EndRun(Arcane::RenderErrorTotal());
    if (const std::string summary = SuiteRenderErrorGate().Summary(); !summary.empty())
    {
        std::fputs(summary.c_str(), stderr);
        std::fflush(stderr);
        return catchResult == 0 ? SuiteRenderErrorGate().ExitStatus() : catchResult;
    }
    return catchResult;
}
