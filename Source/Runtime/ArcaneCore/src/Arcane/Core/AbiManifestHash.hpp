#pragma once

// The hashing primitives of the plugin ABI digest (3D-readiness Plan B), shared
// by its three group manifests: Engine (Plugin/AbiManifestEngine.hpp), Input
// (Input/AbiManifestInput.hpp) and Client (Client/AbiManifestClient.hpp).
// FNV-1a 64 (offset 14695981039346656037, prime 1099511628211); the stream each
// group feeds it is described in AbiManifestEngine.hpp.
//
// FoldGroups() is THE digest: "engine\n" + the Engine mix (8 LE bytes),
// "input\n" + the Input mix, "client\n" + the Client mix, "physics2d\n" + the
// Physics2D mix. The host and the tools fold four real mixes (Core/AbiManifest.hpp); a game TU folds a
// committed sub-digest for a group it cannot see (Plugin/AbiManifestGame.hpp).

#include <cstddef>
#include <cstdint>

namespace Arcane::Detail::AbiManifest
{
    struct Fnv
    {
        std::uint64_t h = 14695981039346656037ull;

        constexpr void Byte(unsigned char b) noexcept { h ^= b; h *= 1099511628211ull; }
        constexpr void Text(const char* s) noexcept { for (; *s; ++s) Byte(static_cast<unsigned char>(*s)); }
        constexpr void Line(const char* s) noexcept { Text(s); Byte('\n'); }
        constexpr void U32(std::uint64_t v) noexcept
        {
            for (int i = 0; i < 4; ++i) Byte(static_cast<unsigned char>((v >> (8 * i)) & 0xFFu));
        }
        constexpr void U64(std::uint64_t v) noexcept
        {
            for (int i = 0; i < 8; ++i) Byte(static_cast<unsigned char>((v >> (8 * i)) & 0xFFu));
        }
        constexpr void Decimal(std::uint64_t v) noexcept
        {
            char digits[24] = {};
            int n = 0;
            do { digits[n++] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v != 0);
            while (n > 0) Byte(static_cast<unsigned char>(digits[--n]));
        }
        constexpr void Define(const char* name, std::uint64_t value) noexcept
        {
            Text(name); Byte('='); Decimal(value); Byte('\n');
        }
        constexpr void Type(const char* name, std::uint64_t size, std::uint64_t align) noexcept
        {
            Line(name); U32(size); U32(align);
        }
        constexpr void Field(const char* name, std::uint64_t offset, std::uint64_t size,
                             std::uint64_t align, const char* declaredType) noexcept
        {
            Line(name); U32(offset); U32(size); U32(align); Line(declaredType);
        }
    };

    // A number that moves with the configuration or the standard library
    // (see OPAQUE FROM THE FIRST CONTAINER in AbiManifestEngine.hpp). Evaluating the argument keeps the
    // compiler checking the expression.
    constexpr std::uint64_t Opaque(std::size_t) noexcept { return 0; }

    // The digest of the three group mixes, in this order.
    constexpr std::uint64_t FoldGroups(std::uint64_t engine, std::uint64_t input, std::uint64_t client,
                                       std::uint64_t physics2d) noexcept
    {
        Fnv f;
        f.Line("engine"); f.U64(engine);
        f.Line("input");  f.U64(input);
        f.Line("client"); f.U64(client);
        f.Line("physics2d"); f.U64(physics2d);
        return f.h;
    }
}
