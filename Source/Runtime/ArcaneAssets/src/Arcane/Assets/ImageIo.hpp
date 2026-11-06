#pragma once
#include <Arcane/Modules/ArcaneAssetsApi.hpp>

// Pure CPU PNG I/O: device-free image codec layer. Split from
// Assets.hpp so a device-free consumer (an image comparator, a thumbnail
// writer) can decode and encode without pulling in the cached-asset
// infrastructure.

#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace Arcane
{
    // Decoded RGBA8 pixels, tight-packed (rowPitch == width*4), retained CPU-side.
    // This is Assets::PixelsFor's payload: the device-free supply that lets
    // sprite geometry and an upload path read decoded pixels without ever
    // touching a live GPU texture. Default-constructed == invalid (matches an
    // AssetCache miss/failure).
    struct PixelData
    {
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;

        // True once a real decode has landed: non-zero dims and an exactly
        // tight-packed buffer. Guards callers that index rgba by width*height*4.
        bool Valid() const
        {
            return width > 0 && height > 0 &&
                   rgba.size() == static_cast<std::size_t>(width) * height * 4;
        }
    };

    // Decode a PNG (or any stb-supported image) from disk into tight RGBA8.
    // False on missing/corrupt file (WARN-logged, never ERROR). Pure CPU.
    ARC_ASSETS_API bool LoadPngRgba(const std::filesystem::path& path,
                                std::uint32_t& width, std::uint32_t& height,
                                std::vector<unsigned char>& rgba);

    // Encode tight RGBA8 to a PNG on disk. Parent directories are created.
    // False on IO failure (WARN-logged). Pure CPU.
    ARC_ASSETS_API bool WritePngRgba(const std::filesystem::path& path,
                                 std::uint32_t width, std::uint32_t height,
                                 const unsigned char* rgba);

    // Encode tight RGBA8 to PNG bytes in memory (stb's to-func writer), so a
    // caller owns the file write: a wide-path std::ofstream, a checked close
    // and a rename (the Servitor capture job). WritePngRgba goes through
    // stbi_write_png, which neither reports a short write nor opens a
    // non-ANSI path on Windows. False (png empty) for empty input or an
    // encoder failure. Pure CPU, never logs.
    ARC_ASSETS_API bool EncodePngRgba(std::uint32_t width, std::uint32_t height,
                                      const unsigned char* rgba, std::vector<unsigned char>& png);

    // Decode PNG (or any stb-supported image) bytes already in memory into
    // tight RGBA8, so a caller owns the file read: a wide-path std::ifstream
    // (the Servitor compare job reads its reference this way; LoadPngRgba
    // narrows the path with path::string(), which throws on a non-ANSI path
    // on Windows). False (pixels invalid) for empty or undecodable input.
    // Pure CPU, never logs.
    ARC_ASSETS_API bool DecodePngRgba(const unsigned char* bytes, std::size_t size, PixelData& pixels);

    // A GPU readback's mapped rows into tight RGBA8: the row padding
    // (rowPitch past width * 4) is dropped and BGRA is swizzled to RGBA, so a
    // capture's bytes mean the same thing on every backend. The one copy of
    // that normalization: NriGraphContext::ReadCapture (the --screenshot and
    // cover path) and the Servitor capture job (ArcaneRemote) both call it.
    // `mapped` holds at least rowPitch * (height - 1) + width * 4 bytes.
    inline void NormalizeCaptureRows(const std::uint8_t* mapped,
        std::uint32_t width, std::uint32_t height,
        std::uint32_t rowPitch, bool bgra,
        std::vector<unsigned char>& rgba)
    {
        rgba.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const std::uint8_t* src = mapped + static_cast<std::size_t>(y) * rowPitch;
            unsigned char* dst = rgba.data() + static_cast<std::size_t>(y) * width * 4;
            std::memcpy(dst, src, static_cast<std::size_t>(width) * 4);
            if (bgra)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    unsigned char* p = dst + static_cast<std::size_t>(x) * 4;
                    const unsigned char b = p[0];
                    p[0] = p[2];
                    p[2] = b;
                }
            }
        }
    }
}
