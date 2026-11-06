// Mutation witness TU 'ClientPrivate' (see premake5.lua, arcane-abi-digest-mutants).
#define ABI_MUT_CLIENT_PRIVATE 1
#include <Arcane/Core/AbiManifest.hpp>

#include <cstdint>

namespace AbiMutants { namespace { constexpr std::uint64_t kMix = Arcane::AbiManifestMix(); } std::uint64_t MixClientPrivate() noexcept { return kMix; } }
