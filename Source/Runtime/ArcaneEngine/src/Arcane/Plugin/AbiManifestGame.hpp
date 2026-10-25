#pragma once

// The plugin ABI digest as a GAME translation unit computes it (3D-readiness
// Plan B Task 2): AbiGameMix() = FoldGroups(Engine, Input, Client, Physics2D)
// (Core/AbiManifestHash.hpp). GameModule.hpp includes this header on its last
// line; ARC_GAME_MODULE's GamePlugin_ABIDigest and the monolithic static_assert
// call AbiGameMix(), so the value is the one THIS TU's flags produce.
//
// The Engine group (Plugin/AbiManifestEngine.hpp) is always hashed here: it is
// everything an ArcaneEngine-only game can include, and it carries the Astra
// defines and Astra::Entity, so a game built with -DASTRA_ENTITY_BITS=64 still
// exports a different digest. The Input group (Input/AbiManifestInput.hpp) and
// the Client group (Client/AbiManifestClient.hpp) and the Physics2D group
// (Physics2D/AbiManifestPhysics2D.hpp) are hashed when the game can include
// their manifest headers (its module closure reaches ArcaneInput / ArcaneClient
// / ArcanePhysics2D). Otherwise the host's committed sub-digest stands in for
// the group (kGamePluginABIDigestInput / kGamePluginABIDigestClient /
// kGamePluginABIDigestPhysics2D,
// Core/AbiVersion.hpp): a game that cannot include a group's headers cannot
// depend on that group's layout, and a game project needs no include directory
// beyond its own module closure. The host compares the one folded value with
// kGamePluginABIDigest; arcane-abi-digest --check keeps the committed
// sub-digests equal to the real group mixes.
//
// Include order: AbiManifestEngine.hpp includes GameModule.hpp, whose last line
// includes this header. The group mixes are DECLARED here and defined by the
// group headers included at the bottom, so either header may come first.

#include <Arcane/Core/AbiManifestHash.hpp>
#include <Arcane/Core/AbiVersion.hpp>

#include <cstdint>

#if __has_include(<Arcane/Input/AbiManifestInput.hpp>)
#define ARC_ABI_GAME_HASHES_INPUT 1
#else
#define ARC_ABI_GAME_HASHES_INPUT 0
#endif

#if __has_include(<Arcane/Client/AbiManifestClient.hpp>)
#define ARC_ABI_GAME_HASHES_CLIENT 1
#else
#define ARC_ABI_GAME_HASHES_CLIENT 0
#endif

#if __has_include(<Arcane/Physics2D/AbiManifestPhysics2D.hpp>)
#define ARC_ABI_GAME_HASHES_PHYSICS2D 1
#else
#define ARC_ABI_GAME_HASHES_PHYSICS2D 0
#endif

namespace Arcane
{
    constexpr std::uint64_t AbiManifestEngineMix() noexcept;   // Plugin/AbiManifestEngine.hpp
#if ARC_ABI_GAME_HASHES_INPUT
    constexpr std::uint64_t AbiManifestInputMix() noexcept;    // Input/AbiManifestInput.hpp
#endif
#if ARC_ABI_GAME_HASHES_CLIENT
    constexpr std::uint64_t AbiManifestClientMix() noexcept;   // Client/AbiManifestClient.hpp
#endif
#if ARC_ABI_GAME_HASHES_PHYSICS2D
    constexpr std::uint64_t AbiManifestPhysics2DMix() noexcept;   // Physics2D/AbiManifestPhysics2D.hpp
#endif

    constexpr std::uint64_t AbiGameMix() noexcept
    {
        return Detail::AbiManifest::FoldGroups(
            AbiManifestEngineMix(),
#if ARC_ABI_GAME_HASHES_INPUT
            AbiManifestInputMix(),
#else
            kGamePluginABIDigestInput,
#endif
#if ARC_ABI_GAME_HASHES_CLIENT
            AbiManifestClientMix(),
#else
            kGamePluginABIDigestClient,
#endif
#if ARC_ABI_GAME_HASHES_PHYSICS2D
            AbiManifestPhysics2DMix()
#else
            kGamePluginABIDigestPhysics2D
#endif
        );
    }
}

#include <Arcane/Plugin/AbiManifestEngine.hpp>
#if ARC_ABI_GAME_HASHES_INPUT
#include <Arcane/Input/AbiManifestInput.hpp>
#endif
#if ARC_ABI_GAME_HASHES_CLIENT
#include <Arcane/Client/AbiManifestClient.hpp>
#endif
#if ARC_ABI_GAME_HASHES_PHYSICS2D
#include <Arcane/Physics2D/AbiManifestPhysics2D.hpp>
#endif
