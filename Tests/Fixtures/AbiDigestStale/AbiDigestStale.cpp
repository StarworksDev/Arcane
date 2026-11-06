// 3D-readiness Plan B Task 2 fixture.
//
// Default entity bits and a matching epoch, but GamePlugin_ABIDigest returns a
// stale constant (1), which is not the current mix. The exports are
// hand-written: ARC_GAME_MODULE would return AbiGameMix().
//
// A real game-module DLL loaded ONLY by the AbiDigestChild process (never by
// ArcaneTests.exe): GamePlugin_Init writes a sentinel file named by the
// ARC_ABI_DIGEST_SENTINEL environment variable, so the test can prove the host
// refused the module BEFORE Init.
//
// 3D-readiness Plan A Task 8 fix round 2: the descriptor also names
// ArcanePhysics2D, an engine module AbiDigestChild does not link, so this DLL
// maps it at load and the refusal unmaps it again. The static below prints,
// from inside the load (Physics2D's statics have run by then), whether that
// dependency is mapped: the witness that the refused load really mapped it.

#include <Arcane/Core/AbiVersion.hpp>
#include <Arcane/Plugin/GameModule.hpp>
#include <Arcane/Plugin/Module.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#if defined(_WIN32)
namespace
{
    // Runs at image load, after every import of this DLL has initialised.
    // MappedImage is GetModuleHandleW: no loader reference, nothing loaded.
    const int g_physics2dWitness = [] {
        const bool mapped = ::Arcane::Module::MappedImage("ArcanePhysics2D.dll").base != nullptr;
        std::printf("fixturePhysics2dMapped=%d\n", mapped ? 1 : 0);
        std::fflush(stdout);
        return 0;
    }();
}
#endif

extern "C"
{
    ARC_GAME_MODULE_EXPORT std::uint32_t GamePlugin_ABIVersion() { return ::Arcane::kGamePluginABIVersion; }
    ARC_GAME_MODULE_EXPORT std::uint64_t GamePlugin_ABIDigest() { return 1ull; }
    ARC_GAME_MODULE_EXPORT bool GamePlugin_Init(::Arcane::EngineContext*)
    {
        if (const char* path = std::getenv("ARC_ABI_DIGEST_SENTINEL"))
            std::ofstream(path) << "stale init ran\n";
        return true;
    }
    ARC_GAME_MODULE_EXPORT void GamePlugin_Shutdown() {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_FixedUpdate(double) {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_Update(double, double) {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_SaveState(::Arcane::BinaryWriter&) {}
    ARC_GAME_MODULE_EXPORT bool GamePlugin_LoadState(::Arcane::BinaryReader&) { return true; }
}
