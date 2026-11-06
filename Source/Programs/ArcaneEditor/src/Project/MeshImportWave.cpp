#include "Project/MeshImportWave.hpp"

#include <Arcane/AssetPipeline/GltfSurvey.hpp>
#include <Arcane/Base/Log.hpp>   // ARC_WARN (extraction write failure)

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/IO/ReadRegularFile.hpp>

namespace Arcane::Editor
{
    namespace fs = std::filesystem;

    namespace
    {
        std::optional<std::vector<std::byte>> ReadWholeFile(const fs::path& path)
        {
            return Arcane::ReadRegularFile(path);
        }

        // A glTF image name is arbitrary UTF-8 -- ANY character that could act as a
        // path separator or reserved Windows filename character is replaced with '_'
        // (byte-wise; every offending character here is single-byte ASCII, so this
        // never splits a multi-byte UTF-8 sequence). This is what keeps
        // ImageFileStem("../../evil", ...) from ever steering a write outside the
        // folder ExtractEmbeddedTextures computed for it.
        std::string SanitizeForFilename(const std::string& raw)
        {
            std::string out;
            out.reserve(raw.size());
            for (char c : raw)
            {
                switch (c)
                {
                    case '/': case '\\': case ':': case '*': case '?':
                    case '"': case '<': case '>': case '|':
                        out.push_back('_');
                        break;
                    default:
                        out.push_back(static_cast<unsigned char>(c) < 0x20 ? '_' : c);
                        break;
                }
            }
            return out;
        }

        // A sanitized stem is "usable" as a filename when it is non-empty and is not
        // made ENTIRELY of dots. Separator replacement alone leaves a bare "."/".."
        // untouched (there is no separator IN "..", so nothing above touches it), and
        // while that is already inert against escaping the destination folder (a
        // filename with no separator can never be multiple path components), "." and
        // ".." are still not names a file can actually be created under on Windows --
        // A4's own fallback arm exists for exactly this "no usable name" case.
        bool IsUsableStem(const std::string& stem)
        {
            return stem.find_first_not_of('.') != std::string::npos;
        }

        // Where an embedded image's bytes belong on disk, decided by walking the SAME
        // candidate chain UniqueSiblingPath itself walks (`stem`+ext, `stem-1`+ext,
        // `stem-2`+ext, ...) and applying the s5.5-vs-A4 byte-compare rule at every
        // EXISTING candidate: identical bytes at some candidate means "already
        // extracted right here" -- the walk stops there with nothing to write
        // (`skip == true`). The first candidate that does not exist yet is where a
        // genuine divergence's bytes belong, whether that is the natural name itself
        // (nothing ever collided) or a later suffix (one or more EXISTING-BUT-
        // DIFFERENT candidates were skipped past first) -- there is only ever ONE
        // rule here, not a separate "natural" case and "collision" case.
        //
        // This is what keeps a REPEATED divergence from piling up a fresh numbered
        // duplicate on every re-extraction: re-extracting after `stem-1` was already
        // written for a still-diverged `stem` finds `stem-1`'s bytes identical to what
        // it would write, matches it, and stops -- rather than skipping past the
        // existing `stem-1` (because IT differs from the diverged `stem`, which it is
        // never compared against) straight to minting `stem-2`.
        struct DestinationDecision
        {
            fs::path path;
            bool     skip = false;   // an identical copy already sits somewhere in the chain
        };

        DestinationDecision ResolveDestination(const fs::path& dir, const std::string& stem,
                                                const std::string& ext,
                                                const std::vector<std::byte>& bytes)
        {
            fs::path candidate = dir / (stem + ext);
            for (int suffix = 1; ; ++suffix)
            {
                if (!fs::exists(candidate))
                    return { candidate, false };

                const std::optional<std::vector<std::byte>> existing = ReadWholeFile(candidate);
                if (existing && *existing == bytes)
                    return { candidate, true };

                candidate = dir / (stem + "-" + std::to_string(suffix) + ext);
            }
        }
    }

    // The corpus's only embedded kind today is image/png (embedded_tex.glb's
    // fixture) -- anything else this engine has not been handed a fixture for falls
    // back to .png rather than growing an untested branch. PUBLIC (see this
    // function's header declaration) so Task 15's FindExtractedImagePath below can
    // re-derive the SAME extension ExtractEmbeddedTextures wrote under.
    std::string ExtensionForMime(const std::string& mimeType)
    {
        if (mimeType == "image/jpeg" || mimeType == "image/jpg")
            return ".jpg";
        return ".png";
    }

    fs::path UniqueSiblingPath(const fs::path& dir, const std::string& stem, const std::string& ext)
    {
        fs::path candidate = dir / (stem + ext);
        for (int suffix = 1; fs::exists(candidate); ++suffix)
            candidate = dir / (stem + "-" + std::to_string(suffix) + ext);
        return candidate;
    }

    std::string ImageFileStem(const std::string& imageName, const std::string& sourceStem,
                               std::size_t index)
    {
        const std::string sanitized = SanitizeForFilename(imageName);
        if (IsUsableStem(sanitized))
            return sanitized;
        return sourceStem + "-" + std::to_string(index);
    }

    std::vector<fs::path> ExtractEmbeddedTextures(const fs::path& source)
    {
        std::vector<fs::path> written;

        const std::optional<std::vector<std::byte>> bytes = ReadWholeFile(source);
        if (!bytes)
            return written;   // unreadable -- the cook will refuse it too, quietly.

        const std::optional<Arcane::AssetPipeline::GltfSurvey> survey =
            Arcane::AssetPipeline::SurveyGltf(*bytes, source);
        if (!survey)
            return written;   // parse/validate failure -- same "say nothing extra" rule.

        const fs::path dir = source.parent_path();
        const std::string sourceStem = source.stem().string();

        for (std::size_t index = 0; index < survey->images.size(); ++index)
        {
            const Arcane::AssetPipeline::GltfImage& image = survey->images[index];
            if (!image.embedded)
                continue;   // external images are not this task's concern -- they
                            // already have their own file on disk.

            const std::string stem = ImageFileStem(image.name, sourceStem, index);
            const std::string ext = ExtensionForMime(image.mimeType);

            const DestinationDecision decision = ResolveDestination(dir, stem, ext, image.bytes);
            if (decision.skip)
                continue;   // an identical copy already exists somewhere in the chain.

            // Final-review folded fix (ledger T13-28): a write failure is WARNED, naming
            // the destination, rather than skipped silently -- a Windows reserved device
            // name ("CON", "NUL", ...) or a trailing dot in a glTF image name survives
            // SanitizeForFilename and makes ofstream refuse the path; without this line
            // the image simply never appeared and nothing said why.
            std::ofstream out(decision.path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                ARC_WARN("Arcane Editor: could not extract embedded image '{}' from '{}' to '{}' "
                         "(the destination could not be opened for writing)",
                         image.name.empty() ? "(unnamed)" : image.name,
                         source.filename().string(), decision.path.generic_string());
                continue;
            }
            out.write(reinterpret_cast<const char*>(image.bytes.data()),
                       static_cast<std::streamsize>(image.bytes.size()));
            if (!out)
            {
                ARC_WARN("Arcane Editor: could not extract embedded image '{}' from '{}' to '{}' "
                         "(the write failed part-way)",
                         image.name.empty() ? "(unnamed)" : image.name,
                         source.filename().string(), decision.path.generic_string());
                continue;
            }

            written.push_back(decision.path);
        }

        return written;
    }

    // Task 15's read-only re-run of the SAME chain ExtractEmbeddedTextures walks
    // above (this function's own header declaration has the full account): reuses
    // the private ResolveDestination helper -- rather than a second copy of the
    // walk -- and translates its `skip` field (an EXISTING candidate already holds
    // these exact bytes) into "found"; the candidate ResolveDestination returns when
    // `skip` is false is where a WRITE would go, meaningless to a caller that never
    // writes, so that path is discarded rather than returned.
    std::optional<fs::path> FindExtractedImagePath(const fs::path& dir, const std::string& stem,
                                                     const std::string& ext,
                                                     const std::vector<std::byte>& bytes)
    {
        const DestinationDecision decision = ResolveDestination(dir, stem, ext, bytes);
        if (decision.skip)
            return decision.path;
        return std::nullopt;
    }

    // Mirrors AssetPipeline::SlotNamesFromSections (ArtifactFormat.cpp) field for field,
    // over MeshSectionView instead of MeshArtifactSection -- see this function's own
    // header-comment banner for why the pipeline's own answer is not reachable here.
    std::vector<std::string> SlotNamesFromSections(const std::vector<Arcane::MeshSectionView>& sections)
    {
        bool any = false;
        std::uint32_t maxSlot = 0;
        for (const Arcane::MeshSectionView& s : sections)
        {
            any = true;
            if (s.slotIndex > maxSlot) maxSlot = s.slotIndex;
        }
        if (!any) return {};

        // Dedup BY NAME (A1): the first section that points at a given slot names it;
        // every later section pointing at the same slot agrees by construction.
        std::vector<std::string> names(static_cast<std::size_t>(maxSlot) + 1);
        std::vector<bool> filled(names.size(), false);
        for (const Arcane::MeshSectionView& s : sections)
        {
            if (!filled[s.slotIndex])
            {
                names[s.slotIndex] = s.name;
                filled[s.slotIndex] = true;
            }
        }
        return names;
    }

    SlotReconciliation ReconcileSlots(const std::vector<Arcane::MeshSlot>& existing,
                                       const std::vector<std::string>& authoritative)
    {
        SlotReconciliation result;
        result.slots.reserve(authoritative.size() + existing.size());

        // OURS (see this function's own declaration comment): consumed[i] tracks which
        // `existing` entries a PRIOR authoritative name has already claimed, so a later
        // authoritative name sharing that spelling cannot re-bind to the same existing
        // slot. UE's own matcher (FbxStaticMeshImport.cpp:1964-1970) never does this --
        // it breaks on the first match and leaves the candidate pool untouched. Scanning
        // `existing` in its ORIGINAL order on every lookup, unconsumed-first, is also
        // what gives the unnamed/duplicate-name case its POSITIONAL tiebreak (UE's own
        // fallback, FbxStaticMeshImport.cpp:1991-2002) for free: two same-spelled
        // candidates resolve in existing-array order, not in whatever order a hash or a
        // reverse scan would visit them.
        std::vector<bool> consumed(existing.size(), false);

        for (const std::string& name : authoritative)
        {
            std::size_t matchIndex = existing.size();
            for (std::size_t i = 0; i < existing.size(); ++i)
            {
                if (!consumed[i] && existing[i].name == name)
                {
                    matchIndex = i;
                    break;
                }
            }

            if (matchIndex < existing.size())
            {
                // Match by NAME (FbxStaticMeshImport.cpp:1964-1970) -- keeps its
                // material assignment wherever it moved to in `authoritative`.
                consumed[matchIndex] = true;
                result.slots.push_back({ name, existing[matchIndex].material });
            }
            else
            {
                // APPEND unmatched authoritative names, in authoritative order
                // (the append-if-unmatched arm at FbxStaticMeshImport.cpp:1964-1974),
                // unassigned (nil material) -- there is nothing to inherit.
                result.slots.push_back({ name, Arcane::Guid{} });
            }
        }

        // NEVER DELETE: an existing slot no authoritative name matched is KEPT (appended,
        // in its original existing order) and WARNED -- not silently dropped the way a
        // positional-only reconciliation would drop it.
        for (std::size_t i = 0; i < existing.size(); ++i)
        {
            if (consumed[i])
                continue;
            result.slots.push_back(existing[i]);
            result.warnings.push_back(
                "material slot '" + existing[i].name + "' no longer appears in the "
                "re-exported mesh -- kept, not deleted, so its material assignment is "
                "not lost");
        }

        return result;
    }

    // F2c Task 15 (spec s6, R4 step 1): reuse-by-name. Empty name never matches (no
    // real asset has an empty stem, but the guard is what makes that a RULE rather
    // than luck -- this function's own header comment). Otherwise counts candidates
    // that are BOTH mesh-surface AND stem-equal; exactly one match reuses, zero or
    // several mint fresh (never guess among duplicates, MintOrReuseSpriteForTexture's
    // own rule).
    Arcane::Guid FindReusableMeshMaterial(std::span<const MaterialCandidate> candidates,
                                           const std::string& gltfMaterialName)
    {
        if (gltfMaterialName.empty())
            return {};

        Arcane::Guid unique{};
        int matches = 0;
        for (const MaterialCandidate& c : candidates)
        {
            if (c.meshSurface && c.stem == gltfMaterialName)
            {
                ++matches;
                unique = c.guid;
            }
        }
        return matches == 1 ? unique : Arcane::Guid{};
    }

    // F2c Plan 2 Task 9 (spec s8, R5): see this function's own header declaration for
    // the MARGIN and DEGENERATE-BOX reasoning -- both are load-bearing, not decoration.
    MeshThumbCamera FrameMeshBounds(const Arcane::MeshBounds& bounds, float fovDegrees, float framingMargin)
    {
        const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
        // A DEGENERATE (zero-extent) box is floored to a UNIT radius here -- the header
        // comment's own promised outcome ("a finite camera at a unit distance rather
        // than a division by zero"), and the same 0.5m "unit sphere" scale
        // BuildUvSphere(0.5f, ...) and MeshDocument::RenderPreview's own degenerate-
        // bounds guard both use. Flooring `radius` itself (not just clamping `distance`
        // after the fact) is load-bearing: with radius == 0, `distance` would ALSO be
        // exactly 0, putting `eye` on top of `target` -- and glm::lookAtRH normalizes
        // (target - eye), so a zero-length look vector is the NaN transform this
        // function exists to rule out, not a merely-imprecise camera.
        ARC_CONSTANT("a change would be a bug: mirrors BuildUvSphere's 0.5 m radius guard")
        constexpr float kMinRadius = 0.5f;
        const float radius = std::max(glm::length((bounds.max - bounds.min) * 0.5f), kMinRadius);

        // Breathing room past a tight fit (this function's own header comment;
        // editor.thumbnail.framingMargin, 15% by default) -- a box that exactly
        // fills the frame reads as cropped at 64px.
        const float halfFovRad = glm::radians(fovDegrees) * 0.5f;
        const float distance = radius / std::sin(halfFovRad) * (1.0f + framingMargin);

        // The mocks' own three-quarter direction (MaterialPreviewHarvester's sphere
        // camera), so a mesh thumbnail and a material thumbnail read as one family.
        const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.6f, 1.0f));

        MeshThumbCamera cam;
        cam.eye    = center + dir * distance;
        cam.target = center;

        // Framed around the bounding SPHERE (a safe over-estimate of the box for any
        // view direction), with a 1.5x radius margin on each side so the box's own
        // corners -- which sit further from the centre than the sphere's silhouette
        // radius the camera is aimed along -- are never clipped. The 0.01f floor keeps
        // this positive even at a wide FOV, where `distance` (shrinking as sin(fov/2)
        // grows) can fall below `radius * 1.5f` -- unrelated to the radius-flooring
        // above, which only rules out a degenerate BOX, not a wide-angle CALLER.
        cam.nearZ = std::max(0.01f, distance - radius * 1.5f);
        cam.farZ  = std::max(cam.nearZ + 0.01f, distance + radius * 1.5f);
        return cam;
    }
}
