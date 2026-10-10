#pragma once

// Where a golden reference image lives, and how an intentional visual change is
// accepted.
//
// HOST-TIER, not Assets-tier, deliberately: this knows about projects and
// backends, which ImageCompare does not and must not -- the comparator answers
// "are these two images the same", and nothing about where images come from.
//
// The hierarchy follows Unity's ColorSpace/Platform/GraphicsAPI shape, reduced
// to the one axis that actually varies for us: an image sits at the MOST
// GENERAL level that is still correct, and resolution walks up from most
// specific. This matters because D3D12 and Vulkan legitimately differ for some
// content and legitimately must not for other content -- mesh.hlsl carries a
// `#if SPIRV` split, and Plan A's desk pass measured the editor's full-UI
// capture differing by 121 ImGui text pixels across backends while the scene
// itself was identical. A flat directory forces one wrong answer or the other.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Arcane
{
    // Whether `name` is a safe reference NAME rather than something that
    // could walk out of the project when turned into a file path: non-empty,
    // no '/' or '\', no ".." substring, no leading '.'. ResolveReference and
    // DiffArtifactPath both apply this to `name` AND `backend` below.
    //
    // EXPORTED (Task 8) so HostConfig's own --compare parse-time refusal can
    // share this exact rule rather than duplicating it -- a name HostConfig
    // accepted that this file went on to refuse would misreport a rejected
    // name as "no reference on disk" (ResolveReference's refusal and "the
    // file genuinely does not exist" both resolve to ReferenceLevel::None),
    // and a second, independently-drifting copy of this predicate is exactly
    // how the two validators would end up disagreeing about what "unsafe"
    // means.
    [[nodiscard]] ARC_API bool ReferenceNameIsSafe(const std::string& name) noexcept;

    enum class ReferenceLevel : std::uint8_t
    {
        None,      // nothing on disk for this name
        Shared,    // Verify/References/<name>.png
        Backend,   // Verify/References/<backend>/<name>.png
        Adapter,   // Verify/References/<adapter set>/<name>.png (see ReferenceAdapterSet)
    };

    // The ADAPTER level above the backend one: the name of a reference set for
    // an adapter whose rasterisation legitimately differs from the hardware the
    // backend-level images were blessed on, or "" when the adapter needs none.
    //
    // Only SOFTWARE adapters get a set. A hardware GPU renders the same images
    // the backend level already holds (the shared/backend split was measured on
    // discrete Windows GPUs of two vendors), while a CPU rasteriser such as
    // Mesa's lavapipe differs in its own consistent way -- its AA coverage, its
    // texture filtering, its transcendental precision -- and it is what a
    // GPU-less CI runner has. Keying every hardware adapter would fragment the
    // references for no gain.
    //
    //   Vulkan on llvmpipe (Mesa lavapipe)            -> "<backend>-lavapipe"
    //   D3D12 on the Microsoft Basic Render Driver    -> "<backend>-warp"
    //   any other software adapter                    -> "<backend>-software"
    //
    // `backend` is the CLI spelling ("dx12"/"vulkan"), the same string the
    // backend level's directory is named with.
    [[nodiscard]] ARC_API std::string ReferenceAdapterSet(const std::string& backend,
                                                             const std::string& adapterName,
                                                             bool softwareAdapter);

    struct ReferenceResolution
    {
        ReferenceLevel        level = ReferenceLevel::None;
        std::filesystem::path path;          // empty iff level == None
        // Where --bless writes. The level the image RESOLVED FROM, or the
        // shared level when nothing resolved. EMPTY means the name itself was
        // refused, and no write of any kind may happen.
        std::filesystem::path blessTarget;
        // The candidate paths ResolveReference probed, in the exact order it
        // probed them -- including the one that resolved, which is always
        // last. Empty on a REFUSED name: refusal is not a search, and an
        // empty list distinguishes "never looked" from "looked everywhere"
        // (UE logs this list and then discards it; the report carries ours).
        std::vector<std::filesystem::path> triedPaths;
    };

    // `name` is a bare name -- no extension, no directory. A name containing a
    // separator or a parent-directory component is REFUSED (level None,
    // blessTarget empty) rather than resolved: it arrives from a command line,
    // and blessing writes files. `backend` is guarded the same way -- it is
    // also command-line-sourced (Task 8 reads it from the host), not a literal.
    [[nodiscard]] ARC_API ReferenceResolution ResolveReference(
        const std::filesystem::path& projectRoot,
        const std::string& name, const std::string& backend);

    // As above, with the adapter level probed FIRST when `adapterSet` is not
    // empty: <adapterSet>/<name>.png, then <backend>/, then shared.
    //
    // An adapter set NEVER blesses into a level below it: blessTarget is the
    // adapter-level path whether or not that image exists yet, so a bless on a
    // software adapter creates (or rewrites) its own image and cannot touch the
    // backend or shared images the hardware lanes own. Resolution still falls
    // through for COMPARING, so a set that has no image for a name yet is
    // judged against the hardware reference rather than reported missing.
    // `adapterSet` is guarded like `name` and `backend`.
    [[nodiscard]] ARC_API ReferenceResolution ResolveReference(
        const std::filesystem::path& projectRoot,
        const std::string& name, const std::string& backend, const std::string& adapterSet);

    // The same resolution under an explicit references directory rather than
    // <projectRoot>/Verify/References -- for reference families kept in a
    // subdirectory (the mesh-thumbnail goldens under References/thumbs/).
    [[nodiscard]] ARC_API ReferenceResolution ResolveReferenceIn(
        const std::filesystem::path& referencesDir,
        const std::string& name, const std::string& backend, const std::string& adapterSet);

    // Write `rgba` (tight RGBA8) to resolution.blessTarget, creating parents.
    // False on a refused name (blessTarget empty) or any IO failure.
    [[nodiscard]] ARC_API bool BlessReference(
        const ReferenceResolution& resolution,
        std::uint32_t width, std::uint32_t height, const unsigned char* rgba);

    // Where a failing comparison's diff image goes. Under Saved/, which the
    // project's .gitignore already excludes, so a failed run never leaves a
    // staged artifact behind -- Plan A's desk pass verified that ignore form
    // holds after a full editor session.
    //
    // `name` and `backend` are guarded exactly as ResolveReference guards
    // them: this path is also built from command-line-sourced strings, and
    // Task 8 WRITES to the result on a comparison failure. An EMPTY return
    // means the name or backend was refused -- the caller must not write
    // anything in that case.
    [[nodiscard]] ARC_API std::filesystem::path DiffArtifactPath(
        const std::filesystem::path& projectRoot,
        const std::string& name, const std::string& backend);
}
