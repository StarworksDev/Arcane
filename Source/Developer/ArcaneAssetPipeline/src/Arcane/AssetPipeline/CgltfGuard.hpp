#pragma once
#include <Arcane/Modules/ArcaneAssetPipelineApi.hpp>

// Arcane::AssetPipeline::CgltfDataGuard -- the cgltf_data* RAII guard, shared by
// MeshImporter.cpp and GltfSurvey.cpp (F2c Task 13). Both TUs run the SAME
// cgltf_parse -> (extensionsRequired check) -> cgltf_load_buffers -> cgltf_validate
// front half (MeshImporter.hpp's own comment names the refusal ladder rungs 1-4;
// SurveyGltf reuses them verbatim, minus rung 5's drawability gate, which is
// mesh-geometry-specific and has no bearing on images/materials). Extracted here
// rather than left as MeshImporter.cpp's own private copy, so the guard has exactly
// ONE definition instead of two near-copies drifting apart -- the failure mode this
// task's own brief calls out by name.
//
// FILE-LOCAL: not part of ArcaneAssetPipeline's public surface (no header outside
// this directory's two importer TUs should reach for it). Written FIRST, before any
// early-return refusal in a caller: a function with several early-return refusals and
// a hand-written cgltf_free on each is exactly where a leak hides. Never copied or
// moved (each caller owns exactly one, as a local) -- copy is deleted so an accidental
// copy can't double-free; the destructor is the only cgltf_free call site either TU
// needs.

#include <Arcane/IO/PathConfine.hpp>
#include <Arcane/IO/ReadRegularFile.hpp>

#include <cgltf.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace Arcane::AssetPipeline
{
    struct ARC_ASSET_PIPELINE_API CgltfDataGuard
    {
        cgltf_data* data = nullptr;
        ~CgltfDataGuard() { if (data) cgltf_free(data); }
        CgltfDataGuard() = default;
        CgltfDataGuard(const CgltfDataGuard&) = delete;
        CgltfDataGuard& operator=(const CgltfDataGuard&) = delete;
    };

    // cgltf's default file reader concatenates "<gltf dir>/<uri>" and fopen()s it, so a
    // buffer uri of "../secret" reads outside the asset folder. Every cgltf load in the
    // pipeline (cgltf_load_buffers in ImportMesh and SurveyGltf) goes through this
    // reader instead: it refuses any path that is not a regular file strictly inside
    // the glTF's own directory (Arcane::PathIsInsideDirectory), and reads through
    // Arcane::ReadRegularFile (size-capped). `baseDir` lives in the struct, so the
    // struct must outlive the cgltf_load_buffers call that uses it.
    struct CgltfConfinedReads
    {
        std::filesystem::path baseDir;

        explicit CgltfConfinedReads(const std::filesystem::path& sourcePath)
            : baseDir(sourcePath.parent_path()) {}
        CgltfConfinedReads(const CgltfConfinedReads&) = delete;
        CgltfConfinedReads& operator=(const CgltfConfinedReads&) = delete;

        void Apply(cgltf_options& options) noexcept
        {
            options.file.read = &Read;
            options.file.user_data = this;
        }

    private:
        static cgltf_result Read(const cgltf_memory_options* memory, const cgltf_file_options* fileOptions,
                                 const char* path, cgltf_size* size, void** data)
        {
            const auto* self = static_cast<const CgltfConfinedReads*>(fileOptions->user_data);
            const std::filesystem::path p(path);
            if (self == nullptr || !Arcane::PathIsInsideDirectory(self->baseDir, p))
                return cgltf_result_file_not_found;
            const auto bytes = Arcane::ReadRegularFile(p);
            if (!bytes)
                return cgltf_result_io_error;
            // Like cgltf's default reader: a declared size (buffer.byteLength) is the
            // number of bytes taken, and a shorter file is an io error.
            const cgltf_size want = (size != nullptr && *size != 0) ? *size : bytes->size();
            if (bytes->size() < want)
                return cgltf_result_io_error;
            void* out = memory->alloc_func ? memory->alloc_func(memory->user_data, want)
                                           : std::malloc(want == 0 ? 1 : want);
            if (out == nullptr)
                return cgltf_result_out_of_memory;
            if (want != 0)
                std::memcpy(out, bytes->data(), want);
            if (size != nullptr)
                *size = want;
            *data = out;
            return cgltf_result_success;
        }
    };
}
