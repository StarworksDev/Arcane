#pragma once

#include <Arcane/Core/Constant.hpp>
#include <Arcane/Core/Api.hpp>
#include <cstdint>

namespace Arcane
{
    ARC_CONSTANT("ABI: the game-module ABI version; the host refuses a mismatched module")
    inline constexpr std::uint32_t kGamePluginABIVersion = 62;
    ARC_CORE_API std::uint32_t PluginABIVersion();

    // The compiler-derived layout digest (Core/AbiManifest.hpp, AbiManifestMix() at
    // the default Astra defines): FoldGroups(Engine, Input, Client, Physics2D) of
    // the four group manifests. It must equal the integer in build/abi-digest.txt.
    // The three sub-digests are the Input, Client and Physics2D group mixes: a game TU that
    // cannot include a group's headers folds the committed value in its place
    // (Plugin/AbiManifestGame.hpp). `arcane-abi-digest --check` and the [abi] tests
    // enforce all four. `arcane-abi-digest --write` (after rebuilding the tool)
    // rewrites the file and these four values; then rebuild.
    ARC_CONSTANT("ABI: compiler-derived layout digest; regenerate with arcane-abi-digest --write")
    inline constexpr std::uint64_t kGamePluginABIDigest = 0xf88782b0664fc33eull;
    ARC_CONSTANT("ABI: the Input group's committed sub-digest; regenerate with arcane-abi-digest --write")
    inline constexpr std::uint64_t kGamePluginABIDigestInput = 0x63304e30bfdae997ull;
    ARC_CONSTANT("ABI: the Client group's committed sub-digest; regenerate with arcane-abi-digest --write")
    inline constexpr std::uint64_t kGamePluginABIDigestClient = 0x0e957f5c80a5bbc6ull;
    ARC_CONSTANT("ABI: the Physics2D group's committed sub-digest; regenerate with arcane-abi-digest --write")
    inline constexpr std::uint64_t kGamePluginABIDigestPhysics2D = 0x95b0119e29497d5dull;
}
