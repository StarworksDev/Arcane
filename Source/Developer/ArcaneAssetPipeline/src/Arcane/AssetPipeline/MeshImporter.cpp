#include "Arcane/AssetPipeline/MeshImporter.hpp"

#include "Arcane/AssetPipeline/CgltfGuard.hpp"
#include "Arcane/AssetPipeline/CookKey.hpp"
#include <Arcane/Assets/ArtifactCodec.hpp>
#include <Arcane/IO/PathConfine.hpp>
#include <Arcane/IO/ReadRegularFile.hpp>

#include <cgltf.h>
#include <meshoptimizer.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace Arcane::AssetPipeline
{
    namespace fs = std::filesystem;

    namespace
    {
        // cgltf_data* RAII guard -- CgltfGuard.hpp (F2c Task 13: extracted out of this file
        // so GltfSurvey.cpp, which runs the same parse/load/validate front half, shares the
        // ONE definition rather than carrying a second near-copy). ImportMesh and
        // ReadExternalBuffers below each own exactly one, as a local.

        // Whole-file read, used both by ReadExternalBuffers (for a referenced .bin) and
        // nowhere else in this TU -- ImportMesh never reads files itself, it is handed
        // sourceBytes/externalBuffers already read (see MeshImporter.hpp's header comment
        // for why: the cook key and the importer must see the SAME bytes, by construction).
        std::optional<std::vector<std::byte>> ReadWholeFileBytes(const fs::path& path)
        {
            return Arcane::ReadRegularFile(path);
        }

        bool StartsWith(const char* s, const char* prefix) noexcept
        {
            return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
        }

        // cgltf_decode_uri(char*) decodes percent-escapes IN PLACE and null-terminates at
        // the (never longer) decoded length -- it must never run on cgltf's own buffer.uri,
        // which cgltf_load_buffers still needs to resolve buffers itself later. Work on a
        // copy: std::string::data() is a mutable char* since C++17, and the decoded bytes
        // land inside it null-terminated at the shorter length, so reading it back via
        // c_str() (which stops at that new null, ignoring the string's stale size()) is
        // exactly the value fs::path needs.
        fs::path DecodeUriToPath(const char* uri)
        {
            std::string copy(uri);
            cgltf_decode_uri(copy.data());
            return fs::path(copy.c_str());
        }

        std::string CgltfResultName(cgltf_result r)
        {
            switch (r)
            {
                case cgltf_result_success:          return "success";
                case cgltf_result_data_too_short:    return "data_too_short";
                case cgltf_result_unknown_format:    return "unknown_format";
                case cgltf_result_invalid_json:      return "invalid_json";
                case cgltf_result_invalid_gltf:      return "invalid_gltf";
                case cgltf_result_invalid_options:   return "invalid_options";
                case cgltf_result_file_not_found:    return "file_not_found";
                case cgltf_result_io_error:          return "io_error";
                case cgltf_result_out_of_memory:     return "out_of_memory";
                case cgltf_result_legacy_gltf:       return "legacy_gltf";
                default:                             return "unknown(" + std::to_string(static_cast<int>(r)) + ")";
            }
        }

        // Names the primitive a degenerate-triangle warning (or a refusal, if this task
        // ever needed to name one at primitive granularity) points at: its material's own
        // name when present and non-empty, else "primitive N" where N is a flat index over
        // every primitive in the file (mesh-major, primitive-minor) -- stable regardless of
        // how many meshes a file declares, and readable without cross-referencing a mesh
        // index too.
        std::string PrimitiveLabel(const cgltf_primitive& prim, std::size_t flatIndex)
        {
            if (prim.material != nullptr && prim.material->name != nullptr && prim.material->name[0] != '\0')
                return prim.material->name;
            return "primitive " + std::to_string(flatIndex);
        }

        // A triangle whose three indices are not all distinct is degenerate (zero area) --
        // shared by the rung 5 diagnostic pass below (counts and WARNS) and Task 7's bake pass
        // (actually DROPS it from the baked buffers), so the two can never disagree about which
        // triangles survive.
        bool IsDegenerateTriangleIndices(cgltf_size i0, cgltf_size i1, cgltf_size i2) noexcept
        {
            return i0 == i1 || i1 == i2 || i0 == i2;
        }

        // Final-review fix I2 (2026-09-11): NON-INDEXED primitives are legal glTF and common
        // (a primitive with no `indices` accessor draws its POSITION accessor's vertices in
        // order, three per triangle). Through Task 7 both the rung 5 gate and the bake
        // `continue`d on `prim.indices == nullptr`, so a mixed file lost that geometry
        // silently and a pure non-indexed file refused with the WRONG reason ("every
        // triangle is degenerate"). The two helpers below are the ONE definition of "how
        // many indices does this primitive have" and "what is index i" for BOTH passes, so
        // they can never disagree about which triangles exist:
        //   IndexCountOf -- the index accessor's count, or the POSITION accessor's count when
        //                   there is no index accessor (the implicit identity index buffer);
        //   ReadIndex    -- cgltf_accessor_read_index, or `i` itself when non-indexed.
        // `posAcc` is the primitive's POSITION accessor (cgltf_validate, rung 4, requires one
        // on every primitive; a null here is a defensive zero, never a crash).
        cgltf_size IndexCountOf(const cgltf_primitive& prim, const cgltf_accessor* posAcc) noexcept
        {
            if (prim.indices != nullptr)
                return prim.indices->count;
            return posAcc != nullptr ? posAcc->count : 0;
        }

        cgltf_size ReadIndex(const cgltf_primitive& prim, cgltf_size i) noexcept
        {
            if (prim.indices != nullptr)
                return cgltf_accessor_read_index(prim.indices, i);
            return i;
        }

        // The glTF `mode` NUMBER for a cgltf primitive type (cgltf's own enum is offset by
        // one and starts at `invalid`, so the enumerator's integer value is NOT the glTF
        // mode) plus its spec name -- for the skipped-primitive warning below, which names
        // the mode the way the file's author wrote it.
        std::string PrimitiveModeLabel(cgltf_primitive_type type)
        {
            switch (type)
            {
                case cgltf_primitive_type_points:         return "0 (points)";
                case cgltf_primitive_type_lines:          return "1 (lines)";
                case cgltf_primitive_type_line_loop:      return "2 (line_loop)";
                case cgltf_primitive_type_line_strip:     return "3 (line_strip)";
                case cgltf_primitive_type_triangles:      return "4 (triangles)";
                case cgltf_primitive_type_triangle_strip: return "5 (triangle_strip)";
                case cgltf_primitive_type_triangle_fan:   return "6 (triangle_fan)";
                default:                                  return "invalid";
            }
        }

        // Task 7 -- geometry bake helpers ------------------------------------------------------

        // Section name (A1): the primitive's material name, or EMPTY when absent. Distinct from
        // PrimitiveLabel above (which falls back to "primitive N" for a human-readable warning):
        // an empty section name here is the signal the slot-dedup pass (pipeline step 4) uses to
        // route the primitive into the shared unnamed slot, so it must never fall back to a
        // synthesized label the way the warning text does.
        std::string PrimitiveMaterialName(const cgltf_primitive& prim)
        {
            if (prim.material != nullptr && prim.material->name != nullptr)
                return prim.material->name;
            return {};
        }

        glm::vec3 ReadVec3(const cgltf_accessor* accessor, cgltf_size index)
        {
            cgltf_float v[3] = { 0.0f, 0.0f, 0.0f };
            cgltf_accessor_read_float(accessor, index, v, 3);
            return glm::vec3(v[0], v[1], v[2]);
        }

        glm::vec2 ReadVec2(const cgltf_accessor* accessor, cgltf_size index)
        {
            cgltf_float v[2] = { 0.0f, 0.0f };
            cgltf_accessor_read_float(accessor, index, v, 2);
            return glm::vec2(v[0], v[1]);
        }

        glm::mat4 LocalTransform(const cgltf_node& node)
        {
            float m[16];
            cgltf_node_transform_local(&node, m);
            // cgltf's out_matrix is 16 floats, column-major (columns 0..3 at m[0..3], m[4..7],
            // m[8..11], m[12..15], translation last) -- exactly glm::mat4's own memory layout,
            // so glm::make_mat4 reads it directly with no transpose.
            return glm::make_mat4(m);
        }

        // Pipeline step 1: flatten the node tree -- depth-first, accumulating
        // `parentWorld * cgltf_node_transform_local(node)` per node (MeshImporter.hpp's ImportMesh
        // comment names why the explicit walk is used over cgltf_node_transform_world: it is what
        // makes the parent-child product OBSERVABLE in the nested.gltf test rather than merely
        // trusted). A node with no mesh of its own still contributes its transform to its
        // children (nested.gltf's parent node). `out` collects one (mesh*, worldTransform) pair
        // per node that DOES carry a mesh, in walk order.
        void WalkNodeTree(const cgltf_node& root, const glm::mat4& parentWorld,
                           std::vector<std::pair<const cgltf_mesh*, glm::mat4>>& out)
        {
            // Explicit stack: a hostile glTF can nest nodes as deep as its file allows, and
            // the recursive walk this replaces overflowed the native stack on one. The
            // visited set (insert is the membership test, so the walk stays linear in the
            // node count) also ends a malformed child cycle instead of looping.
            struct Frame { const cgltf_node* node; glm::mat4 parent; };
            std::vector<Frame> stack;
            stack.push_back({ &root, parentWorld });
            std::unordered_set<const cgltf_node*> seen;
            while (!stack.empty())
            {
                const Frame frame = stack.back();
                stack.pop_back();
                if (!seen.insert(frame.node).second)
                    continue;
                const glm::mat4 world = frame.parent * LocalTransform(*frame.node);
                if (frame.node->mesh != nullptr)
                    out.emplace_back(frame.node->mesh, world);
                // Right to left, so the left-most child pops first: the order the
                // recursive walk produced.
                for (cgltf_size i = frame.node->children_count; i > 0; --i)
                    stack.push_back({ frame.node->children[i - 1], world });
            }
        }

        // One drawable range before slot dedup runs -- indexOffset/indexCount are already final
        // (they describe the RAW, pre-remap index buffer, and remap never reorders index slots,
        // only rewrites the values living in them -- pipeline step 5), only `slotIndex` is filled
        // in afterward once every section's name has been seen.
        struct RawSection
        {
            std::string   name;
            std::uint32_t indexOffset;
            std::uint32_t indexCount;
        };
    }

    MeshImportResult ImportMesh(std::span<const std::byte> sourceBytes,
                                 std::span<const std::span<const std::byte>> externalBuffers,
                                 const fs::path& sourcePath,
                                 const Guid& sourceGuid,
                                 const MeshMetaSettings& /*settings*/)
    {
        MeshImportResult result;
        const std::string fileName = sourcePath.filename().string();

        // ---- Rung 1: parse ------------------------------------------------------------
        cgltf_options options{};
        CgltfDataGuard guard;
        const cgltf_result parseResult =
            cgltf_parse(&options, sourceBytes.data(), sourceBytes.size(), &guard.data);
        if (parseResult != cgltf_result_success)
        {
            result.refusal = "mesh import refused: '" + fileName + "' failed to parse as glTF/GLB (cgltf_parse: "
                + CgltfResultName(parseResult) + ")";
            return result;
        }

        // ---- Rung 2: extensionsRequired -- this importer implements NONE --------------
        // Walk every entry rather than checking against a hand-maintained list of known
        // extensions: that is what makes this rung cover KHR_texture_basisu,
        // EXT_meshopt_compression and every future extension by construction, and what A2
        // part 1's test asserts ("the GENERAL rule, not a hand-list").
        if (guard.data->extensions_required_count > 0)
        {
            std::string names;
            for (cgltf_size i = 0; i < guard.data->extensions_required_count; ++i)
            {
                if (i > 0) names += ", ";
                names += guard.data->extensions_required[i];
            }
            result.refusal = "mesh import refused: '" + fileName
                + "' declares extensionsRequired this importer does not implement: [" + names + "]";
            return result;
        }

        // ---- Rung 3: load buffers ------------------------------------------------------
        // Geometry below is decoded from CGLTF'S OWN buffer load here, while Task 7's Step 7
        // hashes the caller's `externalBuffers` snapshot instead of re-reading through cgltf --
        // two INDEPENDENT reads of what is, today, the same bytes on disk (ReadExternalBuffers
        // is what produced `externalBuffers`, and this call resolves the identical referenced
        // files). If a referenced .bin changes on disk between the two reads, the artifact's
        // sourceHash disagrees with whichever bytes the NEXT read of "current source bytes"
        // sees, the client refuses with HashMismatch, and the next cook heals it -- ACCEPTED,
        // not a bug: the alternative would be threading one already-read buffer through both
        // cgltf's own buffer resolution and the hash, which cgltf's API gives no seam for.
        // Buffer uris resolve through the confining reader (CgltfGuard.hpp): "../" and
        // absolute uris are refused here, not only in ReadExternalBuffers.
        CgltfConfinedReads confinedReads(sourcePath);
        confinedReads.Apply(options);
        const cgltf_result loadResult = cgltf_load_buffers(&options, guard.data, sourcePath.string().c_str());
        if (loadResult != cgltf_result_success)
        {
            // cgltf_load_buffers loads data->buffers[0..N) IN ORDER and returns at the
            // FIRST failure -- every buffer before it already has .data set, so the first
            // one still null (with a non-null uri; the embedded/GLB-BIN case always has
            // .data set before this loop even runs) is the one that actually failed. Named
            // by its own uri when it has one, else its index, so the refusal points at the
            // specific missing file rather than "a buffer, somewhere".
            std::string missingBuffer = "<unknown>";
            for (cgltf_size i = 0; i < guard.data->buffers_count; ++i)
            {
                if (guard.data->buffers[i].data == nullptr)
                {
                    missingBuffer = (guard.data->buffers[i].uri != nullptr)
                        ? guard.data->buffers[i].uri
                        : ("buffer[" + std::to_string(i) + "]");
                    break;
                }
            }
            result.refusal = "mesh import refused: '" + fileName + "' failed to load buffer '" + missingBuffer
                + "' (cgltf_load_buffers: " + CgltfResultName(loadResult) + ")";
            return result;
        }

        // ---- Rung 4: validate -- MANDATORY, never behind a settings flag ---------------
        // The CVE-2026-32845-hardened path (ThirdParty/cgltf/PATCHES.md) this vendor pin
        // exists for (spec s4.5/s10) -- bad_sparse.glb parses fine (cgltf_parse does no
        // bounds checking) and is caught only here.
        const cgltf_result validateResult = cgltf_validate(guard.data);
        if (validateResult != cgltf_result_success)
        {
            result.refusal = "mesh import refused: '" + fileName
                + "' failed cgltf_validate (cgltf_validate: " + CgltfResultName(validateResult) + ")";
            return result;
        }

        // ---- Rung 5: drawability gate + per-primitive degenerate drop ------------------
        if (guard.data->meshes_count == 0)
        {
            result.refusal = "mesh import refused: '" + fileName + "' has nothing drawable (no meshes)";
            return result;
        }

        std::vector<std::string> warnings;
        std::uint64_t survivingTrianglesTotal = 0;
        std::size_t flatPrimitiveIndex = 0;
        std::size_t admittedPrimitives = 0;   // triangle-mode primitives this gate examined
        std::size_t skippedPrimitives = 0;    // non-triangle-mode primitives, WARNED and skipped

        for (cgltf_size meshIdx = 0; meshIdx < guard.data->meshes_count; ++meshIdx)
        {
            const cgltf_mesh& mesh = guard.data->meshes[meshIdx];
            for (cgltf_size primIdx = 0; primIdx < mesh.primitives_count; ++primIdx, ++flatPrimitiveIndex)
            {
                const cgltf_primitive& prim = mesh.primitives[primIdx];

                // Only triangle-LIST primitives (mode 4) carry the "triangle" notion this
                // gate and the degenerate-drop rule both operate on. Final-review fix I2: a
                // strip/fan/points/lines primitive is SKIPPED WITH ONE WARNING naming it and
                // its mode -- never silently (a mixed file used to lose that geometry with
                // no diagnostic at all). Converting strips/fans to lists is a future
                // extension, not this importer's; the warning is what makes the gap visible.
                if (prim.type != cgltf_primitive_type_triangles)
                {
                    ++skippedPrimitives;
                    warnings.push_back(PrimitiveLabel(prim, flatPrimitiveIndex) + ": skipped -- primitive mode "
                        + PrimitiveModeLabel(prim.type) + " unsupported (only mode 4, triangles, imports)");
                    continue;
                }
                ++admittedPrimitives;

                // I2: a NON-INDEXED triangle primitive (no `indices` accessor) is legal and
                // imports -- IndexCountOf/ReadIndex (above) make the implicit identity index
                // buffer look exactly like an authored one to this pass and to the bake.
                const cgltf_accessor* posAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
                const cgltf_size indexCount = IndexCountOf(prim, posAcc);
                const cgltf_size triangleCount = indexCount / 3;

                // Ledger T6-13: an index count that is not a multiple of 3 has a trailing
                // partial triangle (one or two dangling indices). Dropped WITH a warning --
                // the `/ 3` above already never reads it; this names it so the drop is not
                // silent.
                if (indexCount % 3 != 0)
                {
                    warnings.push_back(PrimitiveLabel(prim, flatPrimitiveIndex) + ": index count "
                        + std::to_string(indexCount) + " is not a multiple of 3 -- dropped the trailing "
                        + std::to_string(indexCount % 3) + " dangling index(es) (partial triangle)");
                }

                cgltf_size droppedInPrimitive = 0;
                cgltf_size survivingInPrimitive = 0;
                for (cgltf_size t = 0; t < triangleCount; ++t)
                {
                    const cgltf_size i0 = ReadIndex(prim, t * 3 + 0);
                    const cgltf_size i1 = ReadIndex(prim, t * 3 + 1);
                    const cgltf_size i2 = ReadIndex(prim, t * 3 + 2);

                    // Degenerate (zero area) -- dropped, never refused, per primitive.
                    if (IsDegenerateTriangleIndices(i0, i1, i2))
                        ++droppedInPrimitive;
                    else
                        ++survivingInPrimitive;
                }

                survivingTrianglesTotal += survivingInPrimitive;

                if (droppedInPrimitive > 0)
                {
                    warnings.push_back(PrimitiveLabel(prim, flatPrimitiveIndex) + ": dropped "
                        + std::to_string(droppedInPrimitive) + " degenerate triangle(s) ("
                        + std::to_string(survivingInPrimitive) + " surviving)");
                }
            }
        }

        // The count is per FILE, not per primitive: one all-degenerate primitive beside a
        // healthy one is a warning (already pushed above), not a refusal. I2: the refusal
        // names the ACTUAL reason -- a file whose primitives were ALL skipped for their mode
        // says so, rather than being blamed for degenerate triangles it never had.
        if (survivingTrianglesTotal == 0)
        {
            if (admittedPrimitives == 0 && skippedPrimitives > 0)
            {
                result.refusal = "mesh import refused: '" + fileName
                    + "' has nothing drawable (every primitive uses an unsupported mode -- only mode 4, "
                      "triangles, imports; " + std::to_string(skippedPrimitives) + " skipped)";
            }
            else if (admittedPrimitives == 0)
            {
                result.refusal = "mesh import refused: '" + fileName
                    + "' has nothing drawable (meshes exist but declare no primitives)";
            }
            else
            {
                result.refusal = "mesh import refused: '" + fileName
                    + "' has nothing drawable (every triangle is degenerate)";
            }
            return result;
        }

        // ---- Task 7: flatten, bake, flip, section/slot, remap/optimize, AABB, hash ------
        // Every file that reaches here already passed rungs 1-5 above (parses, no unsupported
        // required extension, buffers loaded, validated, and at least one triangle survives
        // file-wide) -- this builds the actual `ImportedMesh` those rungs cleared.

        // Step 1: flatten the node tree. Walk every root of the default scene (falling back to
        // every root of every scene when there is no single default one) depth-first via
        // WalkNodeTree above, collecting one (mesh*, worldTransform) pair per mesh-bearing node.
        std::vector<std::pair<const cgltf_mesh*, glm::mat4>> nodeMeshRefs;
        if (guard.data->scene != nullptr)
        {
            for (cgltf_size i = 0; i < guard.data->scene->nodes_count; ++i)
                WalkNodeTree(*guard.data->scene->nodes[i], glm::mat4(1.0f), nodeMeshRefs);
        }
        else
        {
            for (cgltf_size s = 0; s < guard.data->scenes_count; ++s)
                for (cgltf_size i = 0; i < guard.data->scenes[s].nodes_count; ++i)
                    WalkNodeTree(*guard.data->scenes[s].nodes[i], glm::mat4(1.0f), nodeMeshRefs);
        }

        std::vector<MeshArtifactVertex> rawVertices;
        std::vector<std::uint32_t>      rawIndices;
        std::vector<RawSection>         rawSections;

        for (const auto& [meshPtr, world] : nodeMeshRefs)
        {
            const glm::mat3 world3(world);
            // Step 3's trigger: a negative determinant is a genuine mirror (an odd number of
            // negative-scale axes), not merely "some component is negative".
            const bool mirrored = glm::determinant(world3) < 0.0f;

            // Step 2's normal matrix: the inverse transpose, the same two lines UE spells at
            // GLTFMeshFactory.cpp:456-457 -- computed ONCE per node rather than once per vertex.
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(world3));

            for (cgltf_size primIdx = 0; primIdx < meshPtr->primitives_count; ++primIdx)
            {
                const cgltf_primitive& prim = meshPtr->primitives[primIdx];
                // Same admission rule as the rung 5 gate above: only triangle-LIST primitives
                // are bakeable (a non-triangle mode was already WARNED there -- I2 -- so this
                // skip is silent by design, not a second diagnostic). Indexed OR non-indexed:
                // IndexCountOf/ReadIndex below make the two look identical to the bake.
                if (prim.type != cgltf_primitive_type_triangles)
                    continue;

                const cgltf_accessor* posAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
                if (posAcc == nullptr)
                    continue;   // cgltf_validate (rung 4) requires POSITION on a drawable
                                // primitive; a null here would mean validate let through
                                // something it should not have -- skip defensively, never crash.
                const cgltf_accessor* nrmAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_normal, 0);
                const cgltf_accessor* uvAcc  = cgltf_find_accessor(&prim, cgltf_attribute_type_texcoord, 0);

                // Parsed-and-ignored, deliberately (S2, trigger: a consumer exists): this
                // importer never reads a parsed file's `skins`/`animations`, or a primitive's
                // morph `targets` -- no renderer or scene support exists for any of the three,
                // so importing them would be stored-but-unread, which S6's own discipline
                // forbids. JOINTS_0/WEIGHTS_0 attributes are skipped by the same rule and for
                // the same reason the fixed 32-byte vertex layout already skips COLOR_0.

                const std::string sectionName = PrimitiveMaterialName(prim);   // empty when absent
                const std::uint32_t sectionIndexOffset = static_cast<std::uint32_t>(rawIndices.size());
                // `/ 3` drops a trailing partial triangle here exactly as rung 5 did (and
                // WARNED about -- ledger T6-13), so the two passes agree on the triangle set.
                const cgltf_size triangleCount = IndexCountOf(prim, posAcc) / 3;

                if (nrmAcc != nullptr)
                {
                    // Authored NORMAL: shared, indexed layout -- one raw vertex per POSITION
                    // accessor entry (never per triangle-corner), so a primitive's own raw
                    // vertex count matches its accessor's count exactly. multi.glb's fixture
                    // contract depends on this: 4 raw vertices per quad primitive,
                    // kMultiGlbRawVertexCount = 12 total before the remap below dedupes the
                    // bit-identical vertices at the two shared edges.
                    const std::uint32_t vertexBase = static_cast<std::uint32_t>(rawVertices.size());
                    for (cgltf_size v = 0; v < posAcc->count; ++v)
                    {
                        MeshArtifactVertex mv{};
                        const glm::vec3 worldPos = glm::vec3(world * glm::vec4(ReadVec3(posAcc, v), 1.0f));
                        mv.px = worldPos.x; mv.py = worldPos.y; mv.pz = worldPos.z;

                        const glm::vec3 worldN = glm::normalize(normalMatrix * ReadVec3(nrmAcc, v));
                        mv.nx = worldN.x; mv.ny = worldN.y; mv.nz = worldN.z;

                        // Missing TEXCOORD_0 -> (0,0), the honest "no UV" -- the fixed 32-byte
                        // stride requires writing SOMETHING for every vertex.
                        const glm::vec2 uv = (uvAcc != nullptr) ? ReadVec2(uvAcc, v) : glm::vec2(0.0f, 0.0f);
                        mv.u = uv.x; mv.v = uv.y;

                        rawVertices.push_back(mv);
                    }

                    for (cgltf_size t = 0; t < triangleCount; ++t)
                    {
                        const cgltf_size a0 = ReadIndex(prim, t * 3 + 0);
                        const cgltf_size a1 = ReadIndex(prim, t * 3 + 1);
                        const cgltf_size a2 = ReadIndex(prim, t * 3 + 2);
                        if (IsDegenerateTriangleIndices(a0, a1, a2))
                            continue;   // already WARNED by the rung 5 pass above -- dropped
                                        // here, never refused (A2 part 2).

                        // Step 3: winding flip. A negative-determinant node reverses this
                        // triangle's corner order. A3, WINDING HALF ONLY: F2c ships the winding
                        // half of the mirror rule; the reserved Tangents tag inherits the
                        // tangent-basis HANDEDNESS half, and mirrored.glb's test grows a
                        // handedness assertion when that tag is first written -- recorded here
                        // because the existing test stays green while a mirrored asset renders
                        // with inverted normal-map lighting, which is the silent regression A3
                        // disarms.
                        const cgltf_size c0 = a0;
                        const cgltf_size c1 = mirrored ? a2 : a1;
                        const cgltf_size c2 = mirrored ? a1 : a2;

                        rawIndices.push_back(vertexBase + static_cast<std::uint32_t>(c0));
                        rawIndices.push_back(vertexBase + static_cast<std::uint32_t>(c1));
                        rawIndices.push_back(vertexBase + static_cast<std::uint32_t>(c2));
                    }
                }
                else
                {
                    // No authored NORMAL: generate one per TRIANGLE (flat/faceted shading).
                    // Vertices are duplicated per triangle-corner here rather than shared through
                    // the primitive's own index buffer: sharing would let one corner's normal be
                    // silently overwritten by whichever adjacent triangle bakes last -- blending
                    // two distinct face normals into a wrong value at a vertex meant to carry
                    // exactly ONE flat normal. Duplicating is the textbook-correct faceted-
                    // shading convention (hard edges everywhere a normal must be derived rather
                    // than authored), and it costs nothing downstream: meshopt_generateVertexRemap
                    // (step 5) re-dedupes any bit-identical vertices this produces, the same way
                    // it dedupes shared edges ACROSS primitives.
                    for (cgltf_size t = 0; t < triangleCount; ++t)
                    {
                        const cgltf_size a0 = ReadIndex(prim, t * 3 + 0);
                        const cgltf_size a1 = ReadIndex(prim, t * 3 + 1);
                        const cgltf_size a2 = ReadIndex(prim, t * 3 + 2);
                        if (IsDegenerateTriangleIndices(a0, a1, a2))
                            continue;

                        // Step 3's flip, same rule as the authored-NORMAL branch above.
                        const cgltf_size corners[3] = { a0, mirrored ? a2 : a1, mirrored ? a1 : a2 };

                        glm::vec3 bakedPos[3];
                        for (int k = 0; k < 3; ++k)
                            bakedPos[k] = glm::vec3(world * glm::vec4(ReadVec3(posAcc, corners[k]), 1.0f));

                        // Step 2's UE-divergence guard: the flat normal is computed from the
                        // BAKED triangle IN ITS POST-FLIP CORNER ORDER (`corners` above is
                        // already reversed when `mirrored`) -- never from a fabricated up-vector
                        // (S7.1's never-fabricate rule, read as geometry: derive it from what is
                        // actually there). cross(v1-v0,v2-v0) reverses sign when the corner order
                        // reverses, so a flat normal taken from the PRE-flip order points INTO a
                        // mirrored surface. UE has exactly this bug: GenerateFlatNormals runs at
                        // GLTFMeshFactory.cpp:515, BEFORE the mirrored-corner loop at :597-605, so
                        // a mirrored no-NORMAL primitive imports there with inverted normals. Do
                        // not inherit it -- computing after the flip (as here) and computing
                        // before the flip then negating by `mirrored` are equivalent; this is the
                        // harder-to-get-wrong of the two.
                        const glm::vec3 flat = glm::normalize(
                            glm::cross(bakedPos[1] - bakedPos[0], bakedPos[2] - bakedPos[0]));

                        for (int k = 0; k < 3; ++k)
                        {
                            MeshArtifactVertex mv{};
                            mv.px = bakedPos[k].x; mv.py = bakedPos[k].y; mv.pz = bakedPos[k].z;
                            mv.nx = flat.x; mv.ny = flat.y; mv.nz = flat.z;
                            // Missing TEXCOORD_0 -> (0,0), same honest-no-UV rule as above.
                            const glm::vec2 uv = (uvAcc != nullptr) ? ReadVec2(uvAcc, corners[k])
                                                                     : glm::vec2(0.0f, 0.0f);
                            mv.u = uv.x; mv.v = uv.y;

                            rawIndices.push_back(static_cast<std::uint32_t>(rawVertices.size()));
                            rawVertices.push_back(mv);
                        }
                    }
                }

                const std::uint32_t sectionIndexCount =
                    static_cast<std::uint32_t>(rawIndices.size()) - sectionIndexOffset;
                rawSections.push_back(RawSection{ sectionName, sectionIndexOffset, sectionIndexCount });
            }
        }

        // The bake's own closing gate. Rung 5 above counts triangles over data->meshes[]
        // FILE-WIDE, regardless of whether any node's scene graph actually reaches a given
        // mesh; the bake above is NODE-major (step 1's walk from the scene's roots), so a mesh
        // nobody's node references still contributes its triangles to rung 5's "something's
        // drawable" verdict while the bake itself produces NOTHING for it. This is what
        // reconciles the two gates' different traversal orders: a file that passed rung 5 but
        // baked to empty vertices/indices is exactly spec S4.5's "parses but yields nothing
        // drawable" case ("a file that parses but yields nothing drawable refuses loudly at
        // cook -- never a silent empty artifact") -- refuse loudly here rather than hand back a
        // "successful" ImportedMesh with empty vertices/indices/sections.
        if (rawVertices.empty() || rawIndices.empty())
        {
            result.refusal = "mesh import refused: '" + fileName
                + "' has nothing drawable (meshes exist but no scene node references them -- "
                  "nothing drawable through the scene)";
            return result;
        }

        // Step 4: sections + slot dedup BY NAME (A1). `slotNames` collects one entry per
        // distinct non-empty material name, in FIRST-SEEN order; primitives with no material
        // (empty name) share ONE unnamed slot APPENDED LAST -- its index is always
        // `slotNames.size()`, never "wherever the first unnamed primitive was seen" the way the
        // named slots are. Sections themselves are already in walk order (rawSections was built
        // in that order above) and their ranges tile the (pre-remap) index buffer with no gaps
        // by construction: each section's offset is the running `rawIndices.size()` at the time
        // it started, and its count is exactly what that primitive appended.
        std::vector<std::string> slotNames;
        for (const RawSection& s : rawSections)
        {
            if (s.name.empty())
                continue;
            if (std::find(slotNames.begin(), slotNames.end(), s.name) == slotNames.end())
                slotNames.push_back(s.name);
        }
        const auto unnamedSlotIndex = static_cast<std::uint32_t>(slotNames.size());

        std::vector<MeshArtifactSection> sections;
        sections.reserve(rawSections.size());
        for (const RawSection& s : rawSections)
        {
            MeshArtifactSection section{};
            section.name = s.name;
            section.indexOffset = s.indexOffset;
            section.indexCount = s.indexCount;
            section.slotIndex = s.name.empty()
                ? unnamedSlotIndex
                : static_cast<std::uint32_t>(
                      std::find(slotNames.begin(), slotNames.end(), s.name) - slotNames.begin());
            sections.push_back(std::move(section));
        }

        // Step 5: remap. meshopt_generateVertexRemap dedupes bit-identical vertices across the
        // WHOLE concatenated buffer (every primitive, every node), then
        // meshopt_remapVertexBuffer/meshopt_remapIndexBuffer apply it. Section ranges survive
        // unchanged: remap rewrites index VALUES (which vertex a slot in the index buffer
        // names), never their ORDER (which slot a triangle occupies) -- `sections`'
        // indexOffset/indexCount above were computed against `rawIndices`' own order and are
        // still correct against the remapped `indices` below, because that order never moved.
        // This is the property that makes step 6's per-section optimize safe: it can address
        // `indices` by the SAME ranges `sections` already declares.
        std::vector<std::uint32_t> remapTable(rawVertices.size());
        const std::size_t uniqueVertexCount = meshopt_generateVertexRemap(
            remapTable.data(), rawIndices.data(), rawIndices.size(),
            rawVertices.data(), rawVertices.size(), sizeof(MeshArtifactVertex));

        std::vector<MeshArtifactVertex> vertices(uniqueVertexCount);
        meshopt_remapVertexBuffer(vertices.data(), rawVertices.data(), rawVertices.size(),
                                   sizeof(MeshArtifactVertex), remapTable.data());

        std::vector<std::uint32_t> indices(rawIndices.size());
        meshopt_remapIndexBuffer(indices.data(), rawIndices.data(), rawIndices.size(), remapTable.data());

        // Step 6: optimize, per section, NEVER across sections. meshopt_optimizeVertexCache runs
        // on each section's own index range in place -- cache-optimizing across a section
        // boundary would scramble which triangles belong to which range, the exact hazard A1
        // names when it explains why sections are never merged.
        for (const MeshArtifactSection& s : sections)
        {
            // vertex_count is the WHOLE buffer's -- meshopt's scratch tables are indexed by
            // INDEX VALUE, and a section's indices address the shared vertex buffer. Passing
            // s.indexCount (or a section-local vertex tally) here is an out-of-bounds write,
            // not a weaker optimisation.
            meshopt_optimizeVertexCache(indices.data() + s.indexOffset,
                                        indices.data() + s.indexOffset,
                                        s.indexCount,
                                        vertices.size());
        }

        // Then meshopt_optimizeVertexFetch ONCE over the whole buffer: it permutes VERTICES and
        // rewrites `indices` in place, so it is range-agnostic (safe to run once across every
        // section, unlike optimizeVertexCache above). It can return fewer vertices than it was
        // given when some are unreferenced after the remap/degenerate-drop above.
        std::vector<MeshArtifactVertex> fetchOptimized(vertices.size());
        const std::size_t vertexCountAfterFetch = meshopt_optimizeVertexFetch(
            fetchOptimized.data(), indices.data(), indices.size(),
            vertices.data(), vertices.size(), sizeof(MeshArtifactVertex));
        fetchOptimized.resize(vertexCountAfterFetch);
        vertices = std::move(fetchOptimized);

        // Step 7: AABB -- min/max over the baked (and now remapped/optimized) positions, the
        // artifact's stored bounds (S5.2/S7.1) that resolution reads instead of recomputing.
        // Taken AFTER optimizeVertexFetch's permutation on purpose: the bounds are permutation-
        // invariant, so computing them here proves they survive the whole pipeline rather than
        // merely the bake.
        float aabbMin[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::max() };
        float aabbMax[3] = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest() };
        for (const MeshArtifactVertex& v : vertices)
        {
            aabbMin[0] = std::min(aabbMin[0], v.px); aabbMax[0] = std::max(aabbMax[0], v.px);
            aabbMin[1] = std::min(aabbMin[1], v.py); aabbMax[1] = std::max(aabbMax[1], v.py);
            aabbMin[2] = std::min(aabbMin[2], v.pz); aabbMax[2] = std::max(aabbMax[2], v.pz);
        }

        // Step 7 (SourceHash): the artifact's sourceHash covers the source bytes FOLLOWED BY
        // every passed-in external buffer's bytes, concatenated in glTF declaration order --
        // the exact concatenation Source/Runtime/ArcaneAssets/src/Arcane/Assets/ArtifactReader.hpp's
        // `currentSourceBytes` contract names, so the client's re-hash of "the current source
        // bytes" agrees with this header field by construction rather than by two functions
        // happening to match. This is what makes `externalBuffers` a USED parameter: through
        // Task 6 it was accepted but never read.
        std::size_t hashInputSize = sourceBytes.size();
        for (const std::span<const std::byte>& buffer : externalBuffers)
            hashInputSize += buffer.size();
        std::vector<std::byte> hashInput;
        hashInput.reserve(hashInputSize);
        hashInput.insert(hashInput.end(), sourceBytes.begin(), sourceBytes.end());
        for (const std::span<const std::byte>& buffer : externalBuffers)
            hashInput.insert(hashInput.end(), buffer.begin(), buffer.end());

        MeshArtifactDesc desc{};
        desc.contentKind = ContentKind::Mesh;
        desc.sourceGuid = sourceGuid;
        desc.sourceHash = HashSourceBytes(hashInput);
        desc.importerVersion = kMeshImporterVersion;
        desc.vertexCount = static_cast<std::uint32_t>(vertices.size());
        desc.indexCount = static_cast<std::uint32_t>(indices.size());
        desc.sectionCount = static_cast<std::uint32_t>(sections.size());
        desc.indexWidth = 4;
        desc.aabbMin[0] = aabbMin[0]; desc.aabbMin[1] = aabbMin[1]; desc.aabbMin[2] = aabbMin[2];
        desc.aabbMax[0] = aabbMax[0]; desc.aabbMax[1] = aabbMax[1]; desc.aabbMax[2] = aabbMax[2];
        desc.sections = std::move(sections);

        ImportedMesh mesh{};
        mesh.desc = std::move(desc);
        mesh.vertices = std::move(vertices);
        mesh.indices = std::move(indices);

        result.warnings = std::move(warnings);
        result.mesh = std::move(mesh);
        return result;
    }

    std::optional<std::vector<std::vector<std::byte>>> ReadExternalBuffers(
        std::span<const std::byte> sourceBytes, const fs::path& sourcePath)
    {
        cgltf_options options{};
        CgltfDataGuard guard;
        const cgltf_result parseResult =
            cgltf_parse(&options, sourceBytes.data(), sourceBytes.size(), &guard.data);
        if (parseResult != cgltf_result_success)
            return std::nullopt;

        std::vector<std::vector<std::byte>> buffers;
        buffers.reserve(guard.data->buffers_count);

        const fs::path baseDir = sourcePath.parent_path();

        for (cgltf_size i = 0; i < guard.data->buffers_count; ++i)
        {
            const cgltf_buffer& buffer = guard.data->buffers[i];

            // Embedded buffers contribute NOTHING -- their bytes already live inside
            // `sourceBytes`. A GLB's BIN-chunk buffer has uri == nullptr; a glTF's inline
            // buffer is a "data:" URI (cgltf_load_buffers resolves it into .data, but we
            // never call that here -- .uri itself already carries the "data:" prefix at
            // this point, which is all the skip test needs).
            if (buffer.uri == nullptr || StartsWith(buffer.uri, "data:"))
                continue;

            // operator/ replaces its left side for an absolute right side, so an absolute
            // decoded uri is refused BEFORE the join; the joined path must then stay
            // strictly inside the asset directory (no "../", no symlink out).
            const fs::path decoded = DecodeUriToPath(buffer.uri);
            if (decoded.is_absolute())
                return std::nullopt;
            const fs::path bufferPath = baseDir / decoded;
            if (!Arcane::PathIsInsideDirectory(baseDir, bufferPath))
                return std::nullopt;
            const std::optional<std::vector<std::byte>> bytes = ReadWholeFileBytes(bufferPath);
            if (!bytes)
                return std::nullopt;   // referenced buffer unreadable -- CookSession turns
                                        // this into a refusal naming it.

            buffers.push_back(*bytes);
        }

        return buffers;
    }
}
