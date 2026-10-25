// 3D-readiness Plan B Task 2 fixture.
//
// Compiled with -DASTRA_ENTITY_BITS=64 (premake5.lua): this TU's AbiGameMix()
// differs from the host's committed default-32 digest while the epoch matches.
// GamePlugin_ABIDigest returns the mix evaluated HERE, the same expression the
// ARC_GAME_MODULE macro uses -- not kGamePluginABIDigest. The descriptor names
// ArcaneEngine alone, so the Input and Client groups are the committed
// sub-digests (Plugin/AbiManifestGame.hpp): the static_assert below pins that
// this fixture exercises that path, and the Engine group (Astra defines,
// Astra::Entity) is what moves.
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

// ...and Physics2D (3D-readiness Plan A Task 8 fix round 1): GameModule.hpp no
// longer reaches a Physics2D header, so an ArcaneEngine-only game cannot see it.
static_assert(!ARC_ABI_GAME_HASHES_INPUT && !ARC_ABI_GAME_HASHES_CLIENT && !ARC_ABI_GAME_HASHES_PHYSICS2D,
              "AbiDigestBits64 is ArcaneEngine-only: it must fold the committed Input/Client/Physics2D sub-digests");

extern "C"
{
    ARC_GAME_MODULE_EXPORT std::uint32_t GamePlugin_ABIVersion() { return ::Arcane::kGamePluginABIVersion; }
    ARC_GAME_MODULE_EXPORT std::uint64_t GamePlugin_ABIDigest()
    {
        static constexpr std::uint64_t digest = ::Arcane::AbiGameMix();
        return digest;
    }
    ARC_GAME_MODULE_EXPORT bool GamePlugin_Init(::Arcane::EngineContext*)
    {
        if (const char* path = std::getenv("ARC_ABI_DIGEST_SENTINEL"))
            std::ofstream(path) << "bits64 init ran\n";
        return true;
    }
    ARC_GAME_MODULE_EXPORT void GamePlugin_Shutdown() {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_FixedUpdate(double) {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_Update(double, double) {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_SaveState(::Arcane::BinaryWriter&) {}
    ARC_GAME_MODULE_EXPORT bool GamePlugin_LoadState(::Arcane::BinaryReader&) { return true; }
}
