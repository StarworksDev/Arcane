#pragma once

// The plugin ABI digest as the HOST and the tools compute it (3D-readiness Plan
// B): AbiManifestMix() = FoldGroups(Engine, Input, Client, Physics2D)
// (Core/AbiManifestHash.hpp), every group hashed from the real headers. It
// equals build/abi-digest.txt and kGamePluginABIDigest (Core/AbiVersion.hpp).
//
// Only arcane-abi-digest, its mutation witness and ArcaneTests include this
// header: it reaches the Engine, Input, Client and Physics2D group manifests.
// A game's digest is AbiGameMix() (Plugin/AbiManifestGame.hpp, through
// GameModule.hpp), which hashes the groups the game can see and folds the
// committed sub-digests for the ones it cannot.

#include <Arcane/Plugin/AbiManifestEngine.hpp>
#include <Arcane/Input/AbiManifestInput.hpp>
#include <Arcane/Client/AbiManifestClient.hpp>
#include <Arcane/Physics2D/AbiManifestPhysics2D.hpp>

#include <cstdint>

namespace Arcane
{
    constexpr std::uint64_t AbiManifestMix() noexcept
    {
        return Detail::AbiManifest::FoldGroups(AbiManifestEngineMix(), AbiManifestInputMix(), AbiManifestClientMix(),
                                               AbiManifestPhysics2DMix());
    }
}
