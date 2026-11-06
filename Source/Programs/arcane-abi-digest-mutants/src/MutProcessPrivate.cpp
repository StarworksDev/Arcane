// Mutation witness TU 'ProcessPrivate' (see premake5.lua, arcane-abi-digest-mutants).
#define ABI_MUT_PROCESS_PRIVATE 1
#include <Arcane/Core/AbiManifest.hpp>

#include <cstdint>

namespace AbiMutants { namespace { constexpr std::uint64_t kMix = Arcane::AbiManifestMix(); } std::uint64_t MixProcessPrivate() noexcept { return kMix; } }
