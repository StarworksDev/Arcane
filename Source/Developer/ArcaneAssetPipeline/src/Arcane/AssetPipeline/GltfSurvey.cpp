#include "Arcane/AssetPipeline/GltfSurvey.hpp"

#include "Arcane/AssetPipeline/CgltfGuard.hpp"

#include <cgltf.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <Arcane/Core/Constant.hpp>

namespace Arcane::AssetPipeline
{
    namespace fs = std::filesystem;

    namespace
    {
        bool StartsWith(const char* s, const char* prefix) noexcept
        {
            return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
        }

        // Decoded byte length of a base64 payload with no embedded whitespace (the
        // shape glTF's own data: URI grammar requires): 3 bytes per 4 base64
        // characters, minus one byte per trailing '=' pad character. An IMAGE's
        // data: URI carries no declared byteLength the way a glTF BUFFER does
        // (cgltf_load_buffers uses buffers[i].size for exactly that reason, set from
        // the JSON's own byteLength field) -- an image has no such field, so the
        // decoded size has to be derived from the base64 text itself before calling
        // cgltf_load_buffer_base64, which trusts whatever size it is given rather
        // than stopping at the string's own end.
        std::size_t Base64DecodedSize(const char* base64, std::size_t len) noexcept
        {
            if (len < 4)
                return 0;
            std::size_t padding = 0;
            if (base64[len - 1] == '=') ++padding;
            if (len >= 2 && base64[len - 2] == '=') ++padding;
            return (len / 4) * 3 - padding;
        }

        // An image's own embedded bytes: the buffer_view slice when present (a GLB's
        // BIN chunk, or a glTF's own bufferView-backed image -- both already resolved
        // by the cgltf_load_buffers call SurveyGltf runs below), else the decoded
        // data: URI payload an inline glTF image can carry instead of a bufferView.
        // Empty for an EXTERNAL image, which contributes no bytes here by GltfImage's
        // own contract.
        std::vector<std::byte> ReadEmbeddedImageBytes(const cgltf_options& options,
                                                        const cgltf_image& image)
        {
            if (image.buffer_view != nullptr)
            {
                const std::uint8_t* data = cgltf_buffer_view_data(image.buffer_view);
                if (data == nullptr)
                    return {};
                const auto* bytes = reinterpret_cast<const std::byte*>(data);
                return std::vector<std::byte>(bytes, bytes + image.buffer_view->size);
            }

            if (image.uri != nullptr && StartsWith(image.uri, "data:"))
            {
                const char* comma = std::strchr(image.uri, ',');
                if (comma == nullptr || comma - image.uri < 7 ||
                    std::strncmp(comma - 7, ";base64", 7) != 0)
                    return {};   // a data: URI cgltf's own buffer loader would also
                                 // reject as cgltf_result_unknown_format -- no bytes,
                                 // not a crash.

                const std::size_t base64Len = std::strlen(comma + 1);
                const std::size_t decodedSize = Base64DecodedSize(comma + 1, base64Len);
                void* decoded = nullptr;
                const cgltf_result res =
                    cgltf_load_buffer_base64(&options, decodedSize, comma + 1, &decoded);
                if (res != cgltf_result_success || decoded == nullptr)
                    return {};

                const auto* bytes = reinterpret_cast<const std::byte*>(decoded);
                std::vector<std::byte> out(bytes, bytes + decodedSize);
                // cgltf_load_buffer_base64 allocates through options.memory
                // (cgltf_default_alloc when unset, i.e. CGLTF_MALLOC == malloc --
                // CgltfImpl.cpp defines neither macro, so the vendor default holds).
                // Free with the SAME fallback rule cgltf's own cgltf_default_free
                // uses, rather than a mismatched delete/free.
                if (options.memory.free_func != nullptr)
                    options.memory.free_func(options.memory.user_data, decoded);
                else
                    std::free(decoded);
                return out;
            }

            return {};
        }

        // One immutable trilinear sampler by F2b design (MeshNode.cpp's
        // RootSamplerDesc: min/mag/mip all LINEAR, REPEAT on every axis) -- a
        // texture's sampler counts as a dropped input only when it asks for
        // something that fixed sampler cannot honor. No sampler at all is NOT a
        // deviation: an unset mag/minFilter is glTF's own "renderer's choice" case,
        // which this engine's fixed choice already answers.
        bool SamplerIsNonDefault(const cgltf_sampler* sampler) noexcept
        {
            if (sampler == nullptr)
                return false;
            if (sampler->wrap_s != cgltf_wrap_mode_repeat) return true;
            if (sampler->wrap_t != cgltf_wrap_mode_repeat) return true;
            if (sampler->mag_filter != cgltf_filter_type_undefined &&
                sampler->mag_filter != cgltf_filter_type_linear) return true;
            if (sampler->min_filter != cgltf_filter_type_undefined &&
                sampler->min_filter != cgltf_filter_type_linear) return true;
            return false;
        }

        // Does any primitive using `material` carry COLOR_0 vertex colours? The fixed
        // 32-byte vertex layout (MeshImporter.cpp's own comment beside its
        // skins/animations/morph-targets paragraph) never reads this attribute even
        // when present -- s6 wants it named as a dropped input exactly when a
        // primitive actually authored it, never unconditionally.
        bool MaterialHasColor0(const cgltf_data& data, const cgltf_material& material)
        {
            for (cgltf_size m = 0; m < data.meshes_count; ++m)
            {
                const cgltf_mesh& mesh = data.meshes[m];
                for (cgltf_size p = 0; p < mesh.primitives_count; ++p)
                {
                    const cgltf_primitive& prim = mesh.primitives[p];
                    if (prim.material != &material)
                        continue;
                    if (cgltf_find_accessor(&prim, cgltf_attribute_type_color, 0) != nullptr)
                        return true;
                }
            }
            return false;
        }
    }

    std::optional<GltfSurvey> SurveyGltf(std::span<const std::byte> sourceBytes,
                                          const fs::path& sourcePath)
    {
        // Rungs 1-4 of MeshImporter.hpp's refusal ladder, verbatim (CgltfGuard.hpp's
        // shared RAII guard -- see that header for why this is not a second copy).
        // Rung 5 (drawability) is deliberately absent: it is a MESH-geometry gate
        // with no bearing on images/materials, and this survey never inspects
        // triangles.
        cgltf_options options{};
        CgltfDataGuard guard;

        if (cgltf_parse(&options, sourceBytes.data(), sourceBytes.size(), &guard.data)
            != cgltf_result_success)
            return std::nullopt;

        if (guard.data->extensions_required_count > 0)
            return std::nullopt;   // the mesh cook refuses this file too -- nothing
                                    // extra to report here (this header's own comment).

        // Buffer uris resolve through the confining reader: "../" and absolute uris are refused.
        CgltfConfinedReads confinedReads(sourcePath);
        confinedReads.Apply(options);
        if (cgltf_load_buffers(&options, guard.data, sourcePath.string().c_str())
            != cgltf_result_success)
            return std::nullopt;

        if (cgltf_validate(guard.data) != cgltf_result_success)
            return std::nullopt;

        GltfSurvey survey;

        survey.images.reserve(guard.data->images_count);
        for (cgltf_size i = 0; i < guard.data->images_count; ++i)
        {
            const cgltf_image& image = guard.data->images[i];
            GltfImage out;
            out.name = image.name != nullptr ? image.name : "";
            out.mimeType = image.mime_type != nullptr ? image.mime_type : "";

            const bool hasDataUri = image.uri != nullptr && StartsWith(image.uri, "data:");
            if (image.buffer_view != nullptr || hasDataUri)
            {
                out.embedded = true;
                out.bytes = ReadEmbeddedImageBytes(options, image);
            }
            else
            {
                out.embedded = false;
                out.uri = image.uri != nullptr ? image.uri : "";
            }

            survey.images.push_back(std::move(out));
        }

        survey.materials.reserve(guard.data->materials_count);
        for (cgltf_size i = 0; i < guard.data->materials_count; ++i)
        {
            const cgltf_material& material = guard.data->materials[i];
            GltfMaterial out;
            out.name = material.name != nullptr ? material.name : "";

            const cgltf_texture* baseColorTexture = nullptr;

            if (material.has_pbr_metallic_roughness)
            {
                const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
                out.baseColorFactor[0] = pbr.base_color_factor[0];
                out.baseColorFactor[1] = pbr.base_color_factor[1];
                out.baseColorFactor[2] = pbr.base_color_factor[2];
                out.baseColorFactor[3] = pbr.base_color_factor[3];

                baseColorTexture = pbr.base_color_texture.texture;
                if (baseColorTexture != nullptr && baseColorTexture->image != nullptr)
                    out.baseColorImage =
                        static_cast<int>(cgltf_image_index(guard.data, baseColorTexture->image));

                // s6, in order -- each entry added ONLY when the material actually
                // SETS it, never unconditionally (a list naming inputs the file never
                // used would train users to ignore the warning it feeds, Task 13's
                // own brief).
                ARC_CONSTANT("file format: glTF 2.0's default metallic/roughness factor (1.0)")
                constexpr float kDefaultFactor = 1.0f;
                const bool metallicRoughnessSet =
                    pbr.metallic_factor != kDefaultFactor ||
                    pbr.roughness_factor != kDefaultFactor ||
                    pbr.metallic_roughness_texture.texture != nullptr;
                if (metallicRoughnessSet)
                    out.droppedInputs.emplace_back("metallic/roughness factors and texture");
            }

            if (material.normal_texture.texture != nullptr)
                out.droppedInputs.emplace_back("normal texture");

            if (material.occlusion_texture.texture != nullptr)
                out.droppedInputs.emplace_back("occlusion texture");

            const bool emissiveSet =
                material.emissive_factor[0] != 0.0f ||
                material.emissive_factor[1] != 0.0f ||
                material.emissive_factor[2] != 0.0f ||
                material.emissive_texture.texture != nullptr;
            if (emissiveSet)
                out.droppedInputs.emplace_back("emissive factor and texture");

            if (MaterialHasColor0(*guard.data, material))
                out.droppedInputs.emplace_back("COLOR_0 vertex colors");

            if (material.double_sided)
                out.droppedInputs.emplace_back("double_sided");

            if (material.alpha_mode != cgltf_alpha_mode_opaque)
                out.droppedInputs.emplace_back("alpha_mode");

            // Sampler settings, last in s6's list: only the base-color texture is a
            // destination this engine actually has, so only ITS sampler can name a
            // real information loss -- the other texture kinds above are already
            // flagged as dropped wholesale, regardless of how they are sampled.
            if (baseColorTexture != nullptr && SamplerIsNonDefault(baseColorTexture->sampler))
                out.droppedInputs.emplace_back("glTF sampler settings");

            survey.materials.push_back(std::move(out));
        }

        return survey;
    }
}
