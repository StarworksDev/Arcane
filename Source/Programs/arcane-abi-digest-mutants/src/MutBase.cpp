// Mutation witness TU 'Base' (see premake5.lua, arcane-abi-digest-mutants).
#include <Arcane/Core/AbiManifest.hpp>

#include <cstdint>

namespace AbiMutants { namespace { constexpr std::uint64_t kMix = Arcane::AbiManifestMix(); } std::uint64_t MixBase() noexcept { return kMix; } }
