// The comment-stability witness: this TU includes a COPY of the Engine group
// manifest (Plugin/AbiManifestEngine.hpp) with a comment line appended (the
// project's prebuild step makes the copy in Intermediate/AbiDigest) BEFORE the
// real headers, so the copy's macro guard hides the real Engine body and the
// fold below evaluates the copy. Its mix is baked at compile time;
// arcane-abi-digest --print-commented prints it and the [abi] test requires it
// to equal the plain mix.

#include <AbiManifestEngine.commented.hpp>
#include <Arcane/Core/AbiManifest.hpp>

#include <cstdint>

namespace AbiDigestTool
{
    namespace { constexpr std::uint64_t kCommentedMix = Arcane::AbiManifestMix(); }
    std::uint64_t CommentedMix() noexcept { return kCommentedMix; }
}
