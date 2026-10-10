// ArcaneTests runner entry point (Server CommonTests convention).
// Add new test files under Arcane/ArcaneTests/src; premake picks them up via the glob.

#include <catch2/catch_session.hpp>

#include "Helpers/TestTypeContext.hpp"
#include "Helpers/UserDataDirs.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Platform/Platform.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Render/AgilitySdk.hpp>
#include <Astra/Core/TypeContext.hpp>

#include <cstdio>

// Agility SDK handshake: the exported version/path pair (Arcane/Render/AgilitySdk.hpp).
// The macro is empty off Windows.
ARC_AGILITY_SDK_EXPORTS();

int main(int argc, char* argv[]) {
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

    return Catch::Session().run(argc, argv);
}
