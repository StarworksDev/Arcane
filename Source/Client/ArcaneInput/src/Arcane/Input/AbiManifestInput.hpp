#pragma once

// The INPUT group of the plugin ABI digest (3D-readiness Plan B): the layout of
// InputSnapshot, which game code reads by value. Hashed by
// AbiManifestInputMix(); folded into the digest by FoldGroups()
// (Core/AbiManifestHash.hpp). A game TU that can include this header hashes it
// under its own flags (Plugin/AbiManifestGame.hpp); one that cannot folds the
// committed kGamePluginABIDigestInput (Core/AbiVersion.hpp) instead. The record
// format is the Engine group's (Plugin/AbiManifestEngine.hpp).

#include <Arcane/Core/AbiManifestHash.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

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
    constexpr std::uint64_t AbiManifestInputMix() noexcept
    {
        using Detail::AbiManifest::Fnv;
        Fnv f;
        f.Type("Arcane::InputSnapshot", sizeof(Arcane::InputSnapshot), alignof(Arcane::InputSnapshot));
        f.Field("scancodes", offsetof(Arcane::InputSnapshot, scancodes), sizeof(Arcane::InputSnapshot::scancodes), alignof(decltype(Arcane::InputSnapshot::scancodes)), "uint64_t[8]");
        f.Field("keycodesDown", offsetof(Arcane::InputSnapshot, keycodesDown), sizeof(Arcane::InputSnapshot::keycodesDown), alignof(decltype(Arcane::InputSnapshot::keycodesDown)), "uint32_t[16]");
        f.Field("keycodeCount", offsetof(Arcane::InputSnapshot, keycodeCount), sizeof(Arcane::InputSnapshot::keycodeCount), alignof(decltype(Arcane::InputSnapshot::keycodeCount)), "uint32_t");
        f.Field("mouseButtons", offsetof(Arcane::InputSnapshot, mouseButtons), sizeof(Arcane::InputSnapshot::mouseButtons), alignof(decltype(Arcane::InputSnapshot::mouseButtons)), "uint8_t");
        f.Field("mouseX", offsetof(Arcane::InputSnapshot, mouseX), sizeof(Arcane::InputSnapshot::mouseX), alignof(decltype(Arcane::InputSnapshot::mouseX)), "float");
        f.Field("mouseY", offsetof(Arcane::InputSnapshot, mouseY), sizeof(Arcane::InputSnapshot::mouseY), alignof(decltype(Arcane::InputSnapshot::mouseY)), "float");
        f.Field("wheelY", offsetof(Arcane::InputSnapshot, wheelY), sizeof(Arcane::InputSnapshot::wheelY), alignof(decltype(Arcane::InputSnapshot::wheelY)), "float");
        f.Field("gamepadConnected", offsetof(Arcane::InputSnapshot, gamepadConnected), sizeof(Arcane::InputSnapshot::gamepadConnected), alignof(decltype(Arcane::InputSnapshot::gamepadConnected)), "bool");
        f.Field("gamepadButtons", offsetof(Arcane::InputSnapshot, gamepadButtons), sizeof(Arcane::InputSnapshot::gamepadButtons), alignof(decltype(Arcane::InputSnapshot::gamepadButtons)), "uint16_t");
        f.Field("gamepadAxes", offsetof(Arcane::InputSnapshot, gamepadAxes), sizeof(Arcane::InputSnapshot::gamepadAxes), alignof(decltype(Arcane::InputSnapshot::gamepadAxes)), "float[6]");
        f.Field("wantCaptureKeyboard", offsetof(Arcane::InputSnapshot, wantCaptureKeyboard), sizeof(Arcane::InputSnapshot::wantCaptureKeyboard), alignof(decltype(Arcane::InputSnapshot::wantCaptureKeyboard)), "bool");
        f.Field("wantCaptureMouse", offsetof(Arcane::InputSnapshot, wantCaptureMouse), sizeof(Arcane::InputSnapshot::wantCaptureMouse), alignof(decltype(Arcane::InputSnapshot::wantCaptureMouse)), "bool");

        return f.h;
    }
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
