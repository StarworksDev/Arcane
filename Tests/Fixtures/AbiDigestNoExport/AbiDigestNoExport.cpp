// 3D-readiness Plan B Task 2 fixture: resolve-order step 3.
//
// The CURRENT epoch and NO GamePlugin_ABIDigest export. Once the epoch
// matches, Plugin.cpp requires the digest export, so this module is refused
// as an unresolved symbol (MissingExport, symbol GamePlugin_ABIDigest), not as
// AbiMismatch. The exports are hand-written: ARC_GAME_MODULE would emit the
// digest.
//
// A real game-module DLL loaded ONLY by the AbiDigestChild process (never by
// ArcaneTests.exe): GamePlugin_Init writes a sentinel file named by the
// ARC_ABI_DIGEST_SENTINEL environment variable, so the test can prove the host
// refused the module BEFORE Init.

#include <Arcane/Core/AbiVersion.hpp>
#include <Arcane/Plugin/GameModule.hpp>

#include <cstdint>
#include <cstdlib>
#include <fstream>

extern "C"
{
    ARC_GAME_MODULE_EXPORT std::uint32_t GamePlugin_ABIVersion() { return ::Arcane::kGamePluginABIVersion; }
    ARC_GAME_MODULE_EXPORT bool GamePlugin_Init(::Arcane::EngineContext*)
    {
        if (const char* path = std::getenv("ARC_ABI_DIGEST_SENTINEL"))
            std::ofstream(path) << "no-export init ran\n";
        return true;
    }
    ARC_GAME_MODULE_EXPORT void GamePlugin_Shutdown() {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_FixedUpdate(double) {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_Update(double, double) {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_SaveState(::Arcane::BinaryWriter&) {}
    ARC_GAME_MODULE_EXPORT bool GamePlugin_LoadState(::Arcane::BinaryReader&) { return true; }
}
