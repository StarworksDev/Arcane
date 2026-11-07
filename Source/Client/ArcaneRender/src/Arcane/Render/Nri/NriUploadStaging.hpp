#pragma once

// NriUploadStaging -- the pure arithmetic behind the graph's `upload` node
// (3D-readiness Plan C, Task 5): how many staging bytes a mesh or a texture
// needs, and which of the three staging classes a payload falls in.
//
// Header-only and device-free so the numbers are pinned by [nri][upload] cases
// that need no adapter. The production upload exec (NriMeshBufferCache::
// RecordUpload, NriTextureCache::RecordUpload) calls exactly these functions;
// nothing here is a test-only copy.
//
// THE THREE CLASSES (evaluated in the exec fn, AFTER ring.BeginFrame):
//   Ring      -- the payload fits what is left of this frame's ring slot: take
//                the bytes from the ring, resident object stays DEVICE memory.
//   Retry     -- it does not fit what is left but WOULD fit an empty slot: bury
//                the device-local object and try again next frame (the cursor
//                is 0 again). Never calls Allocate, so the overflow count does
//                not move.
//   Dedicated -- it is larger than the whole slot: a one-off HOST_UPLOAD
//                staging buffer, uploaded THIS frame. Not a retry, and it never
//                touches the ring's overflow count either.

#include <Arcane/Core/Constant.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>

#include <algorithm>
#include <cstdint>

namespace Arcane
{
    enum class Staging : std::uint8_t { Ring, Retry, Dedicated };

    // The ring's own rounding (ceiling division, any positive alignment).
    [[nodiscard]] constexpr std::uint64_t RingAlignUp(std::uint64_t value, std::uint64_t align) noexcept
    {
        return align <= 1 ? value : ((value + align - 1) / align) * align;
    }

    // The rounding NRI's HelperInterface uses (SharedExternal.h `Align`): a
    // bitmask, correct for the power-of-two alignments every device reports.
    [[nodiscard]] constexpr std::uint64_t NriAlign(std::uint64_t x, std::uint64_t alignment) noexcept
    {
        return (x + alignment - 1) & ~(alignment - 1);
    }

    // One mip's footprint in the staging buffer, exactly as
    // HelperDataUpload::CopyTextureContent lays it out.
    struct MipFootprint
    {
        std::uint32_t sliceRowNum      = 0;
        std::uint32_t alignedRowPitch  = 0;
        std::uint32_t alignedSlicePitch = 0;
        std::uint64_t bytes            = 0;   // alignedSlicePitch * sliceNum
    };

    [[nodiscard]] constexpr MipFootprint UploadMipFootprint(std::uint32_t rowPitch, std::uint32_t slicePitch,
                                                            std::uint32_t sliceNum, std::uint32_t rowAlign,
                                                            std::uint32_t sliceAlign) noexcept
    {
        MipFootprint f;
        if (rowPitch == 0)
            return f;
        f.sliceRowNum       = slicePitch / rowPitch;
        f.alignedRowPitch   = static_cast<std::uint32_t>(NriAlign(rowPitch, rowAlign));
        f.alignedSlicePitch = static_cast<std::uint32_t>(
            NriAlign(static_cast<std::uint64_t>(f.sliceRowNum) * f.alignedRowPitch, sliceAlign));
        f.bytes             = static_cast<std::uint64_t>(f.alignedSlicePitch) * sliceNum;
        return f;
    }

    // Source pitches are what the texture cache has always written: BC7 rows are
    // 4x4 blocks of 16 bytes (ceil(w/4) per row, ceil(h/4) rows), RGBA8 is w*4.
    [[nodiscard]] constexpr std::uint32_t StagingBc7RowPitch(std::uint32_t width) noexcept
    {
        return ((width + 3u) / 4u) * 16u;
    }
    [[nodiscard]] constexpr std::uint32_t StagingBc7SlicePitch(std::uint32_t width, std::uint32_t height) noexcept
    {
        return StagingBc7RowPitch(width) * ((height + 3u) / 4u);
    }

    [[nodiscard]] constexpr std::uint64_t FootprintRgba8(std::uint32_t width, std::uint32_t height,
                                                         std::uint32_t rowAlign, std::uint32_t sliceAlign) noexcept
    {
        return UploadMipFootprint(width * 4u, width * height * 4u, 1u, rowAlign, sliceAlign).bytes;
    }

    [[nodiscard]] constexpr std::uint64_t FootprintBc7(std::uint32_t width, std::uint32_t height,
                                                       std::uint32_t rowAlign, std::uint32_t sliceAlign) noexcept
    {
        return UploadMipFootprint(StagingBc7RowPitch(width), StagingBc7SlicePitch(width, height), 1u,
                                  rowAlign, sliceAlign).bytes;
    }

    // Every mip down to 1x1 (each level halves, never below 1): 5 -> 2 -> 1.
    [[nodiscard]] constexpr std::uint64_t FootprintRgba8Chain(std::uint32_t width, std::uint32_t height,
                                                              std::uint32_t rowAlign, std::uint32_t sliceAlign) noexcept
    {
        std::uint64_t total = 0;
        for (;;)
        {
            total += FootprintRgba8(width, height, rowAlign, sliceAlign);
            if (width == 1 && height == 1)
                break;
            width  = std::max(1u, width >> 1);
            height = std::max(1u, height >> 1);
        }
        return total;
    }

    [[nodiscard]] constexpr std::uint64_t FootprintBc7Chain(std::uint32_t width, std::uint32_t height,
                                                            std::uint32_t rowAlign, std::uint32_t sliceAlign) noexcept
    {
        std::uint64_t total = 0;
        for (;;)
        {
            total += FootprintBc7(width, height, rowAlign, sliceAlign);
            if (width == 1 && height == 1)
                break;
            width  = std::max(1u, width >> 1);
            height = std::max(1u, height >> 1);
        }
        return total;
    }

    // Mesh staging: the vertex region and the index region are each rounded up
    // to 16, then summed. The ring call, when it fits, is Allocate(bytes, 16).
    ARC_CONSTANT("staging alignment of a mesh's vertex and index regions")
    inline constexpr std::uint64_t kMeshStagingAlign = 16;

    [[nodiscard]] constexpr std::uint64_t MeshStagingBytes(std::uint64_t vertexBytes, std::uint64_t indexBytes) noexcept
    {
        return RingAlignUp(vertexBytes, kMeshStagingAlign) + RingAlignUp(indexBytes, kMeshStagingAlign);
    }

    // The class given the ring's answer to Fits: what the exec fns call, with
    // `fits` read from the live ring slot (NriUploadRing::Fits).
    [[nodiscard]] constexpr Staging ClassifyFits(bool fits, std::uint64_t capacity,
                                                 std::uint64_t alignedBytes) noexcept
    {
        if (fits)
            return Staging::Ring;
        return alignedBytes <= capacity ? Staging::Retry : Staging::Dedicated;
    }

    // The class of a payload against a ring whose cursor is already at `cursor`.
    [[nodiscard]] inline Staging ClassifyAtCursor(std::uint64_t capacity, std::uint64_t cursor,
                                                  std::uint64_t alignedBytes, std::uint64_t align) noexcept
    {
        return ClassifyFits(RingLayout::FitsAt(capacity, cursor, alignedBytes, align), capacity, alignedBytes);
    }

    // The class against an empty slot (cursor 0) -- the oversized pins.
    [[nodiscard]] inline Staging Classify(std::uint64_t capacity, std::uint64_t alignedBytes,
                                          std::uint64_t align) noexcept
    {
        return ClassifyAtCursor(capacity, 0, alignedBytes, align);
    }
}
