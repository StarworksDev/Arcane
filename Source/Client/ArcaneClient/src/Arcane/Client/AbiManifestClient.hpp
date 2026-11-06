#pragma once

// The CLIENT group of the plugin ABI digest (3D-readiness Plan B):
// ClientRuntime's private m_core / m_pres (game code compiles the inline
// GameInput() { return m_pres.gameInput; }) and RuntimePresentation. Hashed by
// AbiManifestClientMix(); folded into the digest by FoldGroups()
// (Core/AbiManifestHash.hpp). A game TU that can include this header hashes it
// under its own flags (Plugin/AbiManifestGame.hpp); one that cannot folds the
// committed kGamePluginABIDigestClient (Core/AbiVersion.hpp) instead. The record
// format is the Engine group's (Plugin/AbiManifestEngine.hpp).

#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Core/AbiManifestHash.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Client/RuntimePresentation.hpp>

#include <cstddef>
#include <cstdint>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

namespace Arcane
{
    namespace Detail::AbiManifest
    {
        // ClientRuntime befriends this struct
        // (friend struct ::Arcane::Detail::AbiManifest::ClientAccess;).
        struct ClientAccess
        {
        // ClientRuntime: game code compiles GameInput() { return m_pres.gameInput; }
        // inline, so m_core's and m_pres's offsets are ABI. m_pres holds a
        // LocalInputUser (containers), so its size and alignment are Opaque.
        static constexpr void R_ClientRuntime(Fnv& f) noexcept
        {
            f.Type("Arcane::ClientRuntime", Opaque(sizeof(Arcane::ClientRuntime)), Opaque(alignof(Arcane::ClientRuntime)));
            f.Field("m_core", offsetof(Arcane::ClientRuntime, m_core), sizeof(Arcane::ClientRuntime::m_core), alignof(decltype(Arcane::ClientRuntime::m_core)), "Arcane::Runtime");
            f.Field("m_pres", offsetof(Arcane::ClientRuntime, m_pres), Opaque(sizeof(Arcane::ClientRuntime::m_pres)), Opaque(alignof(decltype(Arcane::ClientRuntime::m_pres))), "Arcane::RuntimePresentation");
        }
        };
    }

    constexpr std::uint64_t AbiManifestClientMix() noexcept
    {
        using Detail::AbiManifest::Fnv;
        using Detail::AbiManifest::Opaque;
        Fnv f;
        Detail::AbiManifest::ClientAccess::R_ClientRuntime(f);
        // RuntimePresentation: input is fixed-size, so gameInput's offset (the first
        // container-holding member's own offset) is real; the sizes and offsets after
        // it hash as 0 where they depend on LocalInputUser's size.
        f.Type("Arcane::RuntimePresentation", Opaque(sizeof(Arcane::RuntimePresentation)), Opaque(alignof(Arcane::RuntimePresentation)));
        f.Field("input", offsetof(Arcane::RuntimePresentation, input), sizeof(Arcane::RuntimePresentation::input), alignof(decltype(Arcane::RuntimePresentation::input)), "InputSnapshot");
        f.Field("gameInput", offsetof(Arcane::RuntimePresentation, gameInput), Opaque(sizeof(Arcane::RuntimePresentation::gameInput)), Opaque(alignof(decltype(Arcane::RuntimePresentation::gameInput))), "LocalInputUser");
        f.Field("audioDesc", Opaque(offsetof(Arcane::RuntimePresentation, audioDesc)), sizeof(Arcane::RuntimePresentation::audioDesc), alignof(decltype(Arcane::RuntimePresentation::audioDesc)), "Audio::AudioDeviceDesc");
        f.Field("audio", Opaque(offsetof(Arcane::RuntimePresentation, audio)), sizeof(Arcane::RuntimePresentation::audio), alignof(decltype(Arcane::RuntimePresentation::audio)), "Audio::AudioDevice");
        f.Field("view", Opaque(offsetof(Arcane::RuntimePresentation, view)), sizeof(Arcane::RuntimePresentation::view), alignof(decltype(Arcane::RuntimePresentation::view)), "ViewTransform");
        f.Field("imguiContext", Opaque(offsetof(Arcane::RuntimePresentation, imguiContext)), sizeof(Arcane::RuntimePresentation::imguiContext), alignof(decltype(Arcane::RuntimePresentation::imguiContext)), "void*");
        f.Field("imguiAlloc", Opaque(offsetof(Arcane::RuntimePresentation, imguiAlloc)), sizeof(Arcane::RuntimePresentation::imguiAlloc), alignof(decltype(Arcane::RuntimePresentation::imguiAlloc)), "void*");
        f.Field("imguiFree", Opaque(offsetof(Arcane::RuntimePresentation, imguiFree)), sizeof(Arcane::RuntimePresentation::imguiFree), alignof(decltype(Arcane::RuntimePresentation::imguiFree)), "void*");
        f.Field("imguiUserData", Opaque(offsetof(Arcane::RuntimePresentation, imguiUserData)), sizeof(Arcane::RuntimePresentation::imguiUserData), alignof(decltype(Arcane::RuntimePresentation::imguiUserData)), "void*");

        return f.h;
    }
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
