#include "Arcane/AssetPipeline/TextureImporter.hpp"

#include "Arcane/AssetPipeline/CookKey.hpp"
#include <Arcane/Assets/ArtifactCodec.hpp>

#include <stb_image.h>

#include <bc7enc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace Arcane::AssetPipeline
{
    namespace
    {
        // ---- sRGB <-> linear (IEC 61966-2-1) -----------------------------------------------
        // Alpha is NEVER gamma-corrected (standard convention: alpha is a linear coverage
        // value even in an sRGB-encoded color texture) -- only R/G/B ride these curves.

        float SrgbToLinear(float c) noexcept
        {
            return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        float LinearToSrgb(float c) noexcept
        {
            return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
        }

        std::byte EncodeChannel(float v) noexcept
        {
            v = std::clamp(v, 0.0f, 1.0f);
            const auto rounded = static_cast<std::uint8_t>(std::lround(v * 255.0f));
            return static_cast<std::byte>(rounded);
        }

        // ---- Linear-float image working buffer ---------------------------------------------

        struct RgbaF { float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f; };

        std::vector<RgbaF> ToLinear(std::span<const std::byte> rgba8, std::uint32_t w, std::uint32_t h, bool srgb)
        {
            std::vector<RgbaF> out(static_cast<std::size_t>(w) * h);
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                const auto* px = reinterpret_cast<const std::uint8_t*>(rgba8.data()) + i * 4;
                float r = px[0] / 255.0f;
                float g = px[1] / 255.0f;
                float b = px[2] / 255.0f;
                const float a = px[3] / 255.0f;   // alpha stays linear
                if (srgb)
                {
                    r = SrgbToLinear(r);
                    g = SrgbToLinear(g);
                    b = SrgbToLinear(b);
                }
                out[i] = RgbaF{ r, g, b, a };
            }
            return out;
        }

        std::vector<std::byte> FromLinear(const std::vector<RgbaF>& img, bool srgb)
        {
            std::vector<std::byte> out(img.size() * 4);
            for (std::size_t i = 0; i < img.size(); ++i)
            {
                float r = img[i].r, g = img[i].g, b = img[i].b;
                if (srgb)
                {
                    r = LinearToSrgb(r);
                    g = LinearToSrgb(g);
                    b = LinearToSrgb(b);
                }
                out[i * 4 + 0] = EncodeChannel(r);
                out[i * 4 + 1] = EncodeChannel(g);
                out[i * 4 + 2] = EncodeChannel(b);
                out[i * 4 + 3] = EncodeChannel(img[i].a);   // alpha stays linear
            }
            return out;
        }

        // Plain 2x2 box average, FLOOR-halved output dims: max(1, dim >> 1). Sample indices are
        // CLAMPED to the input's last row/column rather than wrapped -- this is what makes the
        // same routine correct both for an odd input dimension (the box for the last output
        // texel simply samples the even-aligned pair and the final odd row/column is dropped,
        // per the spec's "keep it simple" ruling -- no polyphase filter) and for an input
        // dimension already pinned at 1 (both sample indices clamp to 0, so the "average" is
        // just that one value, correctly leaving the pinned axis untouched).
        std::vector<RgbaF> BoxDownsampleOnce(const std::vector<RgbaF>& src, std::uint32_t w, std::uint32_t h,
                                              std::uint32_t& outW, std::uint32_t& outH)
        {
            outW = std::max<std::uint32_t>(1, w >> 1);
            outH = std::max<std::uint32_t>(1, h >> 1);

            std::vector<RgbaF> dst(static_cast<std::size_t>(outW) * outH);
            for (std::uint32_t oy = 0; oy < outH; ++oy)
            {
                const std::uint32_t iy0 = std::min(2 * oy, h - 1);
                const std::uint32_t iy1 = std::min(2 * oy + 1, h - 1);
                for (std::uint32_t ox = 0; ox < outW; ++ox)
                {
                    const std::uint32_t ix0 = std::min(2 * ox, w - 1);
                    const std::uint32_t ix1 = std::min(2 * ox + 1, w - 1);

                    const RgbaF& p00 = src[static_cast<std::size_t>(iy0) * w + ix0];
                    const RgbaF& p10 = src[static_cast<std::size_t>(iy0) * w + ix1];
                    const RgbaF& p01 = src[static_cast<std::size_t>(iy1) * w + ix0];
                    const RgbaF& p11 = src[static_cast<std::size_t>(iy1) * w + ix1];

                    dst[static_cast<std::size_t>(oy) * outW + ox] = RgbaF{
                        (p00.r + p10.r + p01.r + p11.r) * 0.25f,
                        (p00.g + p10.g + p01.g + p11.g) * 0.25f,
                        (p00.b + p10.b + p01.b + p11.b) * 0.25f,
                        (p00.a + p10.a + p01.a + p11.a) * 0.25f,
                    };
                }
            }
            return dst;
        }

        // The pure-dimensions mip chain (no pixel work) from (w,h) down to (1,1), FLOOR-halved
        // at every step -- 5 -> 2 -> 1, never 3. Used both to know how many levels exist and to
        // find where a `maxSize` clamp should start.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> BuildDimsChain(std::uint32_t w, std::uint32_t h)
        {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> chain;
            chain.emplace_back(w, h);
            while (!(w == 1 && h == 1))
            {
                w = std::max<std::uint32_t>(1, w >> 1);
                h = std::max<std::uint32_t>(1, h >> 1);
                chain.emplace_back(w, h);
            }
            return chain;
        }

        // HashSourceBytes lives in the ArcaneAssets codec (ArtifactCodec.hpp): the pipeline and
        // the runtime reader share the one function. This call site hashes `pngBytes` alone
        // (a .png has no external buffers), so every texture artifact keeps its on-disk
        // sourceHash.

        // ---- BC7 encode (bc7enc_rdo, plain non-RDO encoder) ----------------------------------
        // Deterministic by construction (spec's determinism ruling, F2b Task 4): bc7enc.cpp has
        // no thread/RNG surface at all (verified on the vendoring desk -- no rand()/thread/omp/
        // clock() hits), and every field of bc7enc_compress_block_params below is a compile-time
        // constant, so the SAME input block always produces the SAME 128-bit output. This is why
        // the RDO post-process (ert.*, rdo_bc_encoder.*) was never vendored -- it exists to trade
        // ratio for size at the cost of a multithreaded path that would need its own determinism
        // proof; plain bc7enc with pinned params sidesteps that proof entirely.

        // bc7enc_compress_block_init() populates process-wide lookup tables and must run exactly
        // once before the first bc7enc_compress_block() call. A function-local static's
        // initializer is guaranteed by the standard to run exactly once even under concurrent
        // first calls (no <mutex> needed) -- ImportTexture has no other synchronization of its
        // own, so this is the only thing standing between it and a data race on g_initialized.
        void EnsureBc7EncInitialized()
        {
            static const bool initialized = (bc7enc_compress_block_init(), true);
            (void)initialized;
        }

        // Pinned encoder params -- highest quality (m_uber_level raised to the max, full
        // partition search), otherwise bc7enc_compress_block_params_init()'s own defaults
        // (perceptual YCbCr weights). Every field is a compile-time constant: no time-based, no
        // thread-count-dependent, no RNG-seeded parameter anywhere in this struct.
        const bc7enc_compress_block_params& Bc7EncParams()
        {
            static const bc7enc_compress_block_params params = []
            {
                bc7enc_compress_block_params p{};
                bc7enc_compress_block_params_init(&p);
                p.m_uber_level = BC7ENC_MAX_UBER_LEVEL;
                return p;
            }();
            return params;
        }

        // BC7-encodes one already-RGBA8 mip level (`rgba8` is (w*h*4) bytes, R first in memory --
        // the same bytes the RGBA8 path would have written). Pads the ENCODED mip to 4x4 blocks
        // by CLAMPING to the source's last row/column (never resizing the source -- same clamp
        // convention BoxDownsampleOnce above already uses for its own edge samples); the caller's
        // MipDesc keeps the true (unpadded) width/height, only the payload bytes are padded.
        // Output is exactly ceil(w/4)*ceil(h/4)*16 bytes, one 128-bit block per 4x4 tile, blocks
        // in row-major tile order.
        std::vector<std::byte> EncodeBc7(std::span<const std::byte> rgba8, std::uint32_t w, std::uint32_t h)
        {
            EnsureBc7EncInitialized();
            const bc7enc_compress_block_params& params = Bc7EncParams();

            const std::uint32_t blocksW = (w + 3u) / 4u;
            const std::uint32_t blocksH = (h + 3u) / 4u;
            std::vector<std::byte> out(static_cast<std::size_t>(blocksW) * blocksH * 16u);

            const auto* src = reinterpret_cast<const std::uint8_t*>(rgba8.data());
            std::uint8_t blockPixels[16 * 4];

            for (std::uint32_t by = 0; by < blocksH; ++by)
            {
                for (std::uint32_t bx = 0; bx < blocksW; ++bx)
                {
                    for (std::uint32_t py = 0; py < 4; ++py)
                    {
                        const std::uint32_t sy = std::min(by * 4 + py, h - 1);
                        for (std::uint32_t px = 0; px < 4; ++px)
                        {
                            const std::uint32_t sx = std::min(bx * 4 + px, w - 1);
                            const std::uint8_t* srcPx = src + (static_cast<std::size_t>(sy) * w + sx) * 4;
                            std::uint8_t* dstPx = blockPixels + (py * 4 + px) * 4;
                            dstPx[0] = srcPx[0];
                            dstPx[1] = srcPx[1];
                            dstPx[2] = srcPx[2];
                            dstPx[3] = srcPx[3];
                        }
                    }

                    void* dstBlock = out.data() + (static_cast<std::size_t>(by) * blocksW + bx) * 16u;
                    bc7enc_compress_block(dstBlock, blockPixels, &params);
                }
            }
            return out;
        }
    }

    std::optional<ImportedTexture> ImportTexture(std::span<const std::byte> pngBytes, const Guid& sourceGuid,
                                                  const TextureMetaSettings& settings)
    {
        int decodedW = 0, decodedH = 0, channelsInFile = 0;
        stbi_uc* decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(pngBytes.data()),
                                                  static_cast<int>(pngBytes.size()),
                                                  &decodedW, &decodedH, &channelsInFile, 4);
        if (decoded == nullptr || decodedW <= 0 || decodedH <= 0)
            return std::nullopt;

        const auto fullW = static_cast<std::uint32_t>(decodedW);
        const auto fullH = static_cast<std::uint32_t>(decodedH);
        const std::size_t fullByteCount = static_cast<std::size_t>(fullW) * fullH * 4;

        std::vector<std::byte> originalBytes(fullByteCount);
        std::memcpy(originalBytes.data(), decoded, fullByteCount);
        stbi_image_free(decoded);

        // Full dims chain (pure integers) so we know both how many mip levels exist and, when
        // `maxSize` is set, which level the artifact's top mip should actually start at.
        const std::vector<std::pair<std::uint32_t, std::uint32_t>> chain = BuildDimsChain(fullW, fullH);

        std::size_t startLevel = 0;
        if (settings.maxSize != 0)
        {
            while (startLevel + 1 < chain.size() &&
                   std::max(chain[startLevel].first, chain[startLevel].second) > settings.maxSize)
                ++startLevel;
        }

        // Decode the full-res image to linear float once, then walk it down to `startLevel` --
        // every intermediate level is a genuine 2x2 box average in linear space, never skipped.
        std::vector<RgbaF> floatImg = ToLinear(originalBytes, fullW, fullH, settings.srgb);
        std::uint32_t curW = fullW, curH = fullH;
        for (std::size_t i = 0; i < startLevel; ++i)
        {
            std::uint32_t nw = 0, nh = 0;
            floatImg = BoxDownsampleOnce(floatImg, curW, curH, nw, nh);
            curW = nw;
            curH = nh;
        }

        struct MipLevel { std::vector<std::byte> bytes; std::uint32_t width, height; };
        std::vector<MipLevel> levels;

        // The artifact's own mip 0: if no clamp was needed (startLevel == 0), reuse the
        // pristine decoded bytes directly rather than round-tripping them through linear float
        // and back -- lossless, and avoids an unnecessary (if deterministic) precision change on
        // an untouched top level. A clamped top level has no such original to fall back to; it
        // IS a resample.
        levels.push_back({ (startLevel == 0) ? originalBytes : FromLinear(floatImg, settings.srgb), curW, curH });

        // The (possibly clamped) top level's own linear-float representation, held aside for the
        // thumbnail -- generating further mips below mutates `floatImg` in place.
        const std::vector<RgbaF> topLevelFloat = floatImg;
        const std::uint32_t topW = curW, topH = curH;

        if (settings.generateMips)
        {
            for (std::size_t lvl = startLevel + 1; lvl < chain.size(); ++lvl)
            {
                std::uint32_t nw = 0, nh = 0;
                floatImg = BoxDownsampleOnce(floatImg, curW, curH, nw, nh);
                curW = nw;
                curH = nh;
                levels.push_back({ FromLinear(floatImg, settings.srgb), curW, curH });
            }
        }

        // Thumbnail: box-downsample the top level (never the original pre-clamp size) until its
        // long edge is <=64, aspect kept by construction (both axes halve together).
        std::vector<RgbaF> thumbFloat = topLevelFloat;
        std::uint32_t thumbW = topW, thumbH = topH;
        while (std::max(thumbW, thumbH) > 64)
        {
            std::uint32_t nw = 0, nh = 0;
            thumbFloat = BoxDownsampleOnce(thumbFloat, thumbW, thumbH, nw, nh);
            thumbW = nw;
            thumbH = nh;
        }
        std::vector<std::byte> thumbBytes = FromLinear(thumbFloat, settings.srgb);

        // BC7 encode: Auto and Bc7 both resolve to BC7 this slice (Rgba8 stays RGBA8 verbatim).
        // The thumbnail above is untouched -- it is always uncompressed RGBA8 regardless of
        // `desc.format`, ArtifactFormat.hpp's own contract. sRGB does NOT gate this branch: the
        // BC7 payload is colorspace-agnostic (it compresses whatever 8-bit values are already in
        // each level's bytes, exactly the bytes the RGBA8 path would have written) -- `srgb`
        // rides the artifact header only, for the runtime to pick a _SRGB vs plain view format.
        const bool encodeBc7 = (settings.format == TextureMetaSettings::Format::Auto ||
                                 settings.format == TextureMetaSettings::Format::Bc7);
        if (encodeBc7)
        {
            for (MipLevel& lvl : levels)
                lvl.bytes = EncodeBc7(lvl.bytes, lvl.width, lvl.height);
        }

        // Assemble the payload: every mip's bytes back to back, in order, each MipDesc addressed
        // by its own offset into `payload` (never resorted -- ArtifactFormat.hpp's contract).
        std::vector<std::byte> payload;
        std::vector<MipDesc> mips;
        std::size_t payloadSize = 0;
        for (const MipLevel& lvl : levels)
            payloadSize += lvl.bytes.size();
        payload.reserve(payloadSize);
        std::uint64_t offset = 0;
        for (const MipLevel& lvl : levels)
        {
            mips.push_back(MipDesc{ offset, static_cast<std::uint64_t>(lvl.bytes.size()), lvl.width, lvl.height });
            payload.insert(payload.end(), lvl.bytes.begin(), lvl.bytes.end());
            offset += lvl.bytes.size();
        }

        TextureArtifactDesc desc{};
        desc.contentKind = ContentKind::Texture;
        desc.sourceGuid = sourceGuid;
        desc.sourceHash = HashSourceBytes(pngBytes);
        desc.importerVersion = kTextureImporterVersion;
        desc.format = encodeBc7 ? ArtifactPixelFormat::BC7 : ArtifactPixelFormat::RGBA8;
        desc.dimension = ArtifactDimension::Tex2D;
        desc.arrayOrDepth = 1;
        desc.width = levels.front().width;
        desc.height = levels.front().height;
        desc.mipCount = static_cast<std::uint32_t>(levels.size());
        desc.srgb = settings.srgb;
        desc.mips = std::move(mips);
        desc.thumbWidth = thumbW;
        desc.thumbHeight = thumbH;

        ImportedTexture result;
        result.desc = std::move(desc);
        result.payload = std::move(payload);
        result.thumbRgba = std::move(thumbBytes);
        return result;
    }
}
