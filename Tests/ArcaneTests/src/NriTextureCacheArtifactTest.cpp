// F2b Task 7: NriTextureCache uploads COMPILED artifacts -- BC7/RGBA8 mips, the
// pending-cook checkerboard placeholder, the chrome (raw-pixel) path kept.
//
// TWO HALVES, deliberately separated by backend need:
//
//   CPU (tag [nri][artifact][texcache-artifact], NONE backend, part of the
//   required ~[gpu] baseline): the per-key STATE MACHINE -- Resident /
//   PendingCook / Refused -- and the BC7 block-pitch arithmetic, both
//   observable with no real device. Everything here runs on
//   NriDevice::CreateNoneForTests() exactly like NriTextureCacheTest.cpp's own
//   suite (see that file's header banner for why NONE is the sanctioned
//   carve-out and what it does/does not prove) -- the SAME limitation applies
//   here: every NRI handle ImplNONE hands back is the identical dummy
//   pointer, so "one shared checkerboard, not one per pending key" is proven
//   through PlaceholderCount() and supply call-COUNTS, never through pointer
//   identity.
//
//   GPU (tag [gpu][pixel][nri][texcache-artifact], paired per-backend cases
//   following NriGraphPixelTest.cpp's own conventions): a REAL 3-mip BC7
//   artifact, cooked in-test through the pipeline lib (bc7enc_rdo -- the same
//   library arccook links), uploaded through this cache and sampled by an
//   actual textured sprite draw. This is the one thing NONE cannot prove: BC7
//   is a real block-compressed format and ImplNONE never decodes anything it
//   is handed.
//
// Nothing here touches NriTextureCacheTest.cpp's own suite, which stays
// exactly as it was -- it is the "chrome path kept" regression net BY
// CONSTRUCTION: every one of its cases installs ONLY a PixelSupplyFn, never an
// artifact supply, so it continues to exercise the untouched legacy path this
// task's brief promises is "byte for byte" unchanged.

// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h, pulled in by NriDevice.hpp below, declares
// nri::Message::ERROR, and <windows.h> -- reachable through spdlog, which
// Arcane/Base/Log.hpp and much of the rest of the engine pull in -- #defines
// ERROR via wingdi.h. EVERY Render/Nri/ header must therefore parse before
// the first non-NRI Arcane include that could reach spdlog; ArtifactFormat.hpp
// /HostConfig.hpp/etc. below are exactly such headers, which is why they come
// AFTER this block rather than interleaved with it.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>

#undef ERROR

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/TextureImporter.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Assets/ArtifactReader.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Assets/ImageIo.hpp>   // PixelData -- the chrome-path-kept case constructs one
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Render/Nri/AssetsCookSettings.hpp>   // assets.cook.pendingRepollInterval -- the throttle cadence

#include <stb_image_write.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "Helpers/GpuCapability.hpp"
#include "Helpers/SettingsSweep.hpp"

// =============================================================================
// PART 1 -- CPU: the state machine and the pitch arithmetic
// =============================================================================
namespace
{
    const Arcane::Guid kIdA = *Arcane::Guid::FromString("aaaaaaaa-7777-2222-3333-444444444444");
    const Arcane::Guid kIdB = *Arcane::Guid::FromString("bbbbbbbb-7777-6666-3333-888888888888");

    // A minimal, hand-built RGBA8 artifact -- CPU cases never decode a real
    // image, they only drive this cache's OWN validation/upload logic, which
    // does not care whether the bytes came from bc7enc_rdo or a test fixture.
    Arcane::LoadedClientArtifact MakeRgba8Artifact(std::uint32_t w, std::uint32_t h)
    {
        Arcane::LoadedClientArtifact art;
        art.info.width    = w;
        art.info.height   = h;
        art.info.mipCount = 1;
        art.info.srgb     = true;
        art.format = Arcane::ArtifactPixelFormatValue::RGBA8;
        art.mips.push_back(Arcane::MipView{ 0, (std::uint64_t)w * h * 4, w, h });
        art.payload.assign((std::size_t)w * h * 4, std::byte{ 0xCD });
        return art;
    }

    // An artifact whose FORMAT byte this reader mirrors but this render cache
    // cannot map to an NRI format -- ArtifactReader.hpp's own forward-compat
    // placeholder values (a future BC5/BC6H importer would populate these; none
    // exists yet). The "refuse, never limp" case: real bytes arrived, this
    // cache just cannot put them on the device.
    Arcane::LoadedClientArtifact MakeUnsupportedFormatArtifact()
    {
        Arcane::LoadedClientArtifact art = MakeRgba8Artifact(4, 4);
        art.format = Arcane::ArtifactPixelFormatValue::BC5_Reserved;
        return art;
    }

    // A supply that COUNTS its calls AND records the count -- the only way to
    // observe "PendingCook re-polls every ask" vs "Refused never re-polls",
    // same idiom NriTextureCacheTest.cpp's own CountingSupply uses for the
    // legacy path.
    struct CountingArtifactSupply
    {
        int calls = 0;
        const Arcane::LoadedClientArtifact* answer = nullptr;

        Arcane::NriTextureCache::ArtifactSupplyFn Fn()
        {
            return [this](const Arcane::Guid&) -> const Arcane::LoadedClientArtifact*
            {
                ++calls;
                return answer;
            };
        }
    };
}

// ---- Bc7RowPitch/Bc7SlicePitch: the exact NPOT numbers the brief pins ------

TEST_CASE("nri texture cache: Bc7RowPitch/Bc7SlicePitch match the 5->2->1 NPOT chain",
          "[nri][artifact][texcache-artifact]")
{
    using Arcane::NriTextureCache;

    // 5x5 (mip 0): ceil(5/4) = 2 blocks/row * 16 bytes = 32; 2 block-rows -> 64.
    CHECK(NriTextureCache::Bc7RowPitch(5) == 32u);
    CHECK(NriTextureCache::Bc7SlicePitch(5, 5) == 64u);

    // 2x2 (mip 1, floor(5/2)): ceil(2/4) = 1 block/row * 16 = 16; 1 block-row -> 16.
    CHECK(NriTextureCache::Bc7RowPitch(2) == 16u);
    CHECK(NriTextureCache::Bc7SlicePitch(2, 2) == 16u);

    // 1x1 (mip 2, floor(2/2)): ceil(1/4) = 1 block/row * 16 = 16; 1 block-row -> 16.
    // The whole point of ceil rather than floor: a floor-derived table would
    // divide 1 by 4 down to zero blocks and describe an empty row.
    CHECK(NriTextureCache::Bc7RowPitch(1) == 16u);
    CHECK(NriTextureCache::Bc7SlicePitch(1, 1) == 16u);
}

// ---- state machine: PendingCook -> Resident, THROTTLED (review fix) --------
//
// Resolve() runs at DECLARATION TIME -- every frame, per on-screen span -- so
// polling the artifact supply on every ask (the pre-fix behaviour) drove a
// fresh Assets::ArtifactFor call, and downstream a full Intermediate/
// Artifacts/** rescan, every single frame for every still-uncooked texture.
// The cases below pin the THROTTLED cadence: assets.cook.pendingRepollInterval
// asks are absorbed (placeholder only, no supply call) between two actual
// polls, and promotion still lands within one throttle window of the
// artifact actually appearing.

TEST_CASE("nri texture cache: an artifact key with nothing cooked yet is PendingCook, "
          "and re-polls until it is Resident",
          "[nri][artifact][texcache-artifact]")
{
    using Arcane::NriTextureCache;

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    CountingArtifactSupply supply;
    supply.answer = nullptr;   // nothing cooked yet
    cache->SetArtifactSupply(supply.Fn());

    // PendingCook: a real, non-null texture (the checkerboard), but NOT
    // counted as "resident" -- it is a placeholder, not the compiled asset.
    // Creation itself is an unconditional poll (a brand new key deserves an
    // immediate answer), so this alone costs exactly one supply call.
    nri::Texture* pending = cache->Resolve(kIdA);
    CHECK(pending != nullptr);
    CHECK(cache->View(kIdA) != nullptr);
    CHECK(cache->ResidentCount() == 0);
    CHECK(cache->PlaceholderCount() == 1);
    CHECK(supply.calls == 1);

    // THROTTLED, not memoized and not polled every ask: pendingRepollInterval-1
    // more Resolve() calls are all absorbed by the placeholder alone -- the
    // supply is not consulted again yet.
    for (std::uint32_t i = 1; i < Arcane::Settings<Arcane::AssetsCookSettings>().pendingRepollInterval; ++i)
    {
        CHECK(cache->Resolve(kIdA) == pending);
        CHECK(supply.calls == 1);
    }
    CHECK(cache->ResidentCount() == 0);

    // Task 12's (future) cook queue lands the artifact -- but a still-pending
    // ask inside the CURRENT throttle window must not see it early: this is
    // exactly the pendingRepollInterval-th Resolve since the key was
    // created, which IS the throttle boundary, so it polls and promotes.
    Arcane::LoadedClientArtifact artifact = MakeRgba8Artifact(4, 4);
    supply.answer = &artifact;

    nri::Texture* resident = cache->Resolve(kIdA);
    REQUIRE(resident != nullptr);
    CHECK(supply.calls == 2);
    CHECK(cache->ResidentCount() == 1);
    CHECK(cache->View(kIdA) != nullptr);

    // Resident is sticky now, exactly like the legacy path: no further polls,
    // throttled or otherwise.
    CHECK(cache->Resolve(kIdA) == resident);
    CHECK(supply.calls == 2);
    CHECK(cache->ResidentCount() == 1);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

TEST_CASE("nri texture cache: the PendingCook re-poll cadence matches "
          "assets.cook.pendingRepollInterval exactly, across several windows",
          "[nri][artifact][texcache-artifact]")
{
    using Arcane::NriTextureCache;
    const std::uint32_t N = Arcane::Settings<Arcane::AssetsCookSettings>().pendingRepollInterval;
    REQUIRE(N >= 2);   // the test below needs at least one absorbed ask per window

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    CountingArtifactSupply supply;
    supply.answer = nullptr;   // stays pending for the whole test
    cache->SetArtifactSupply(supply.Fn());

    // Drive THREE full throttle windows and assert the supply call count
    // after each one -- not just "it eventually polls again", but the EXACT
    // cadence: one poll per N asks, every window, not just the first.
    int expectedPolls = 1;   // creation itself polls once
    CHECK(cache->Resolve(kIdA) != nullptr);
    CHECK(supply.calls == expectedPolls);

    for (int window = 0; window < 3; ++window)
    {
        // N-1 absorbed asks: no new supply call.
        for (std::uint32_t i = 1; i < N; ++i)
        {
            (void)cache->Resolve(kIdA);
            CHECK(supply.calls == expectedPolls);
        }
        // The Nth ask in this window IS the poll.
        (void)cache->Resolve(kIdA);
        ++expectedPolls;
        CHECK(supply.calls == expectedPolls);
        CHECK(cache->ResidentCount() == 0);   // still pending -- supply.answer never changed
    }

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

// S6-5: the cadence is the Live setting assets.cook.pendingRepollInterval,
// read on every PendingCook ask, so a lowered value shortens the very next
// window.
TEST_CASE("nri texture cache: a lowered assets.cook.pendingRepollInterval re-polls sooner",
          "[nri][artifact][texcache-artifact][sweep][assets]")
{
    Arcane::Test::SkipIfCompiledOut("assets.cook.pendingRepollInterval");
    using Arcane::NriTextureCache;
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const Arcane::CVarHandle h = reg.Find("assets.cook.pendingRepollInterval");
    REQUIRE_FALSE(h.IsStale());
    REQUIRE(reg.Set(h, Arcane::CVarValue::UInt32(4u), Arcane::SetBy::Code) == Arcane::SetResult::Applied);
    reg.PublishImmediate();
    struct Revert
    {
        Arcane::CVarRegistry& reg; Arcane::CVarHandle h;
        ~Revert() { reg.ClearRung(h, Arcane::SetBy::Code); reg.PublishImmediate(); }
    } revert{ reg, h };

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    CountingArtifactSupply supply;
    supply.answer = nullptr;   // stays pending
    cache->SetArtifactSupply(supply.Fn());

    CHECK(cache->Resolve(kIdA) != nullptr);   // creation polls once
    CHECK(supply.calls == 1);
    for (int i = 0; i < 3; ++i)               // three absorbed asks
    {
        (void)cache->Resolve(kIdA);
        CHECK(supply.calls == 1);
    }
    (void)cache->Resolve(kIdA);               // the 4th ask is the poll (32 by default)
    CHECK(supply.calls == 2);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

// ---- state machine: Refused is sticky ---------------------------------------

TEST_CASE("nri texture cache: an artifact this cache cannot upload is Refused, and "
          "Refused never re-polls",
          "[nri][artifact][texcache-artifact]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    Arcane::LoadedClientArtifact bad = MakeUnsupportedFormatArtifact();
    CountingArtifactSupply supply;
    supply.answer = &bad;
    cache->SetArtifactSupply(supply.Fn());

    // Refused: null, exactly like a legacy miss -- the caller binds its own
    // white texel. NEVER the checkerboard: PendingCook and Refused must never
    // render alike (the spec's placeholder rule).
    CHECK(cache->Resolve(kIdA) == nullptr);
    CHECK(cache->View(kIdA) == nullptr);
    CHECK(cache->ResidentCount() == 0);
    CHECK(cache->PlaceholderCount() == 0);   // never even asked for a placeholder
    CHECK(supply.calls == 1);

    // STICKY: unlike PendingCook, a Refused key never re-polls, even though
    // the supply is still installed and would happily answer again.
    CHECK(cache->Resolve(kIdA) == nullptr);
    CHECK(supply.calls == 1);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

TEST_CASE("nri texture cache: an artifact with no mip levels, or dimensions nri::Dim_t "
          "cannot express, is refused",
          "[nri][artifact][texcache-artifact]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    SECTION("no mips")
    {
        Arcane::LoadedClientArtifact art = MakeRgba8Artifact(4, 4);
        art.mips.clear();
        CountingArtifactSupply supply;
        supply.answer = &art;
        cache->SetArtifactSupply(supply.Fn());

        CHECK(cache->Resolve(kIdA) == nullptr);
        CHECK(cache->ResidentCount() == 0);
    }

    SECTION("dims nri::Dim_t cannot express")
    {
        Arcane::LoadedClientArtifact art = MakeRgba8Artifact(70000, 1);
        CountingArtifactSupply supply;
        supply.answer = &art;
        cache->SetArtifactSupply(supply.Fn());

        CHECK(cache->Resolve(kIdA) == nullptr);
        CHECK(cache->ResidentCount() == 0);
    }

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

// ---- the checkerboard is cache-owned, one per colour space -----------------

TEST_CASE("nri texture cache: the pending-cook checkerboard is ONE shared placeholder "
          "per colour space, not one per pending key",
          "[nri][artifact][texcache-artifact]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    CountingArtifactSupply supply;
    supply.answer = nullptr;
    cache->SetArtifactSupply(supply.Fn());

    // Two DIFFERENT guids, both pending in the same (default Srgb) space.
    CHECK(cache->Resolve(kIdA) != nullptr);
    CHECK(cache->Resolve(kIdB) != nullptr);
    // ONE placeholder slot serves both -- PlaceholderCount is what makes this
    // observable on a backend (NONE) where every handle is the same dummy
    // pointer regardless of how many objects were actually created (see this
    // file's header banner).
    CHECK(cache->PlaceholderCount() == 1);
    CHECK(cache->ResidentCount() == 0);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);

    // Release() buries the placeholder too (once, not once per key) and
    // resets the slot -- a PendingCook resolve after Release rebuilds it
    // fresh rather than reusing a buried handle.
    CHECK(cache->PlaceholderCount() == 0);
    CHECK(cache->Resolve(kIdA) != nullptr);
    CHECK(cache->PlaceholderCount() == 1);

    cache->Release(device->Graves(), 2);
    device->Graves().Reap(2);
}

// ---- the chrome path is untouched: Display-space still uses PixelSupplyFn --

TEST_CASE("nri texture cache: installing an artifact supply does not move "
          "Display-space resolution off PixelSupplyFn",
          "[nri][artifact][texcache-artifact]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    Arcane::PixelData chromePixels;
    chromePixels.width  = 2;
    chromePixels.height = 2;
    chromePixels.rgba.assign(2u * 2u * 4u, 0x11);
    int pixelCalls = 0;
    cache->SetPixelSupply([&](const Arcane::Guid&) -> const Arcane::PixelData*
    {
        ++pixelCalls;
        return &chromePixels;
    });

    CountingArtifactSupply artifactSupply;
    artifactSupply.answer = nullptr;   // would drive PendingCook if it were ever asked
    cache->SetArtifactSupply(artifactSupply.Fn());

    using Space = Arcane::NriTextureCache::ColorSpace;

    // Display space: PixelSupplyFn only, exactly as before this task --
    // "the chrome path kept". The artifact supply is NEVER consulted.
    CHECK(cache->Resolve(kIdA, Space::Display) != nullptr);
    CHECK(pixelCalls == 1);
    CHECK(artifactSupply.calls == 0);
    CHECK(cache->ResidentCount() == 1);   // a real (chrome) resident, not a placeholder
    CHECK(cache->PlaceholderCount() == 0);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

// =============================================================================
// PART 2 -- GPU: a real 3-mip BC7 artifact, cooked in-test, sampled end to end
// =============================================================================
namespace
{
    namespace fs = std::filesystem;

    fs::path ArtifactTempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_texcache_artifact_gpu_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    void CollectPngBytes(void* ctx, void* data, int size)
    {
        auto* out = static_cast<std::vector<std::byte>*>(ctx);
        const auto* p = static_cast<std::byte*>(data);
        out->insert(out->end(), p, p + size);
    }

    // A FLAT single-colour PNG, `size` x `size`. Flat on purpose: BC7
    // reproduces a solid-colour block exactly (no gradient to lose), so EVERY
    // mip in the chain -- 5x5, 2x2, 1x1 -- carries the identical known colour
    // regardless of which one the sampler's automatic LOD selection actually
    // picks for a given draw. That is what lets one flat-colour assertion
    // stand in as "the multi-mip BC upload proof" without having to pin an
    // exact mip level.
    std::vector<std::byte> EncodeFlatPng(int size, unsigned char r, unsigned char g,
                                         unsigned char b)
    {
        std::vector<unsigned char> rgba(static_cast<std::size_t>(size) * size * 4);
        for (std::size_t i = 0; i < rgba.size(); i += 4)
        {
            rgba[i + 0] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = 255;
        }
        std::vector<std::byte> out;
        REQUIRE(stbi_write_png_to_func(&CollectPngBytes, &out, size, size, 4, rgba.data(),
                                       size * 4) != 0);
        return out;
    }

    // Cooks a flat-colour, BC7, 3-mip (5->2->1 -- the brief's own NPOT chain)
    // artifact through the REAL pipeline lib (ImportTexture -> bc7enc_rdo ->
    // WriteTextureArtifact, exactly what arccook drives) and reads it back
    // through THIS engine's ArtifactReader -- the same cross-lib byte-contract
    // path ArtifactReaderTest.cpp exercises.
    Arcane::LoadedClientArtifact CookFlatBc7Artifact(const fs::path& artifactPath,
                                                      const Arcane::Guid& guid,
                                                      unsigned char r, unsigned char g,
                                                      unsigned char b)
    {
        const std::vector<std::byte> src = EncodeFlatPng(5, r, g, b);

        Arcane::AssetPipeline::TextureMetaSettings settings;
        settings.format       = Arcane::AssetPipeline::TextureMetaSettings::Format::Bc7;
        settings.srgb         = true;
        settings.generateMips = true;

        auto imported = Arcane::AssetPipeline::ImportTexture(src, guid, settings);
        REQUIRE(imported.has_value());
        REQUIRE(imported->desc.format == Arcane::AssetPipeline::ArtifactPixelFormat::BC7);
        // The brief's own NPOT chain: 5 -> 2 -> 1, three levels.
        REQUIRE(imported->desc.mipCount == 3);
        REQUIRE(imported->desc.mips.size() == 3);
        REQUIRE(imported->desc.mips[0].width == 5);
        REQUIRE(imported->desc.mips[1].width == 2);
        REQUIRE(imported->desc.mips[2].width == 1);

        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
            artifactPath, imported->desc, imported->payload, imported->thumbRgba));

        auto result = Arcane::ReadClientArtifact(artifactPath, src, guid);
        REQUIRE(result.refusal == Arcane::ArtifactRefusal::None);
        REQUIRE(result.artifact.has_value());
        return std::move(*result.artifact);
    }

    // The capture target -- small on purpose, same reasoning as
    // NriGraphPixelTest.cpp's own kW/kH.
    constexpr std::uint32_t kW = 160;
    constexpr std::uint32_t kH = 96;

    struct PixelVehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner> native;
        std::unique_ptr<Arcane::NriDevice>         nri;
        std::unique_ptr<Arcane::NriGraphContext>   ctx;
    };

    PixelVehicle MakeVehicle(Arcane::GraphicsBackend backend)
    {
        PixelVehicle v;
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
#if defined(ARC_BUILD_DEBUG)
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;
#endif
        v.native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(v.native != nullptr);
        v.nri = Arcane::NriDevice::Wrap(*v.native);
        REQUIRE(v.nri != nullptr);

        Arcane::HostConfig cfg;
        cfg.backend = backend;
        v.ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *v.nri, kW, kH, {});
        REQUIRE(v.ctx != nullptr);
        REQUIRE(v.ctx->IsOffscreen());
        return v;
    }

    struct Rgba
    {
        std::uint8_t r = 0, g = 0, b = 0, a = 0;
    };

    Rgba At(const std::vector<unsigned char>& rgba, std::uint32_t w, std::uint32_t x,
            std::uint32_t y)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
        REQUIRE(i + 3u < rgba.size());
        return Rgba{ rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3] };
    }

    void CheckArtifactUploadSamplesItsFlatColour(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        const fs::path dir = ArtifactTempDir("upload_sample");
        const Arcane::Guid guid = Arcane::Guid::Generate();
        // Pure green: separates cleanly from both other channels and from the
        // NriGraphPixelTest.cpp suite's own pure-red fixtures, so a mistaken
        // cross-test bleed would show up as a colour-channel mismatch rather
        // than a coincidental pass.
        Arcane::LoadedClientArtifact artifact =
            CookFlatBc7Artifact(dir / "flat.arcart", guid, 0, 255, 0);

        PixelVehicle v = MakeVehicle(backend);

        // Task 7's own seam: the content supply is ARTIFACT-shaped now, not
        // raw pixels -- see NriGraphContext::SetArtifactSupply.
        v.ctx->SetArtifactSupply(
            [&](const Arcane::Guid& id) -> const Arcane::LoadedClientArtifact*
            {
                return id == guid ? &artifact : nullptr;
            });

        // A rect well inside the canvas, SCALED well past the artifact's own
        // 5x5 texels -- a magnified draw, so this is genuinely sampling a
        // texture rather than coincidentally copying it 1:1.
        constexpr float kX = 30.0f, kY = 20.0f, kSize = 64.0f;
        auto batcher = Arcane::Batcher2D::Create();
        REQUIRE(batcher != nullptr);

        // THE DEFERRED TIMELINE (Plan C Task 5), on the real 5->2->1 NPOT BC7 chain: frame
        // N queues the artifact, N+1 records its three mips into the staging buffer (the
        // aligned-footprint arithmetic UploadStagingTest pins), N+2 samples it. The same
        // frame is submitted until the vehicle reports nothing in flight, so the capture
        // read below is the first frame that CAN show the texture.
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.batch   = batcher.get();
        int frames = 0;
        do
        {
            batcher->Begin(kW, kH);
            batcher->SetLayer(0, 0);
            batcher->QuadTextured(Arcane::Batcher2D::kMaterialSprite, guid,
                                  glm::vec2(kX, kY), glm::vec2(kSize, kSize),
                                  glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f),
                                  glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
            const auto outcome = v.ctx->RenderFrameOffscreen(frame);
            REQUIRE(outcome == Arcane::NriGraphContext::FrameOutcome::Presented);
        } while (v.ctx->HasPendingUploads() && ++frames < 6);
        REQUIRE_FALSE(v.ctx->HasPendingUploads());
        CHECK(frames == 2);   // queue, upload, draw

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));

        const Rgba inside = At(rgba, w, static_cast<std::uint32_t>(kX + kSize * 0.5f),
                               static_cast<std::uint32_t>(kY + kSize * 0.5f));
        const Rgba outside = At(rgba, w, kW - 10u, kH - 10u);

        // THE COLOUR: green dominates both other channels -- structural, not
        // literal, for the same reason NriGraphPixelTest.cpp's own cases are
        // (the tonemap curve sits between the artifact's texel and this
        // capture; see that file's header banner). A wrong row/slice pitch
        // for the sampled mip would show up as noise or a shifted/garbage
        // colour here, not as "green, but the wrong shade of it".
        CHECK(inside.g > inside.r + 60);
        CHECK(inside.g > inside.b + 60);
        CHECK(inside.g > outside.g + 60);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pixel: a 3-mip BC7 artifact uploads through NriTextureCache and samples its "
          "own flat colour (d3d12)",
          "[gpu][pixel][nri][texcache-artifact][d3d12]")
{
    CheckArtifactUploadSamplesItsFlatColour(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: a 3-mip BC7 artifact uploads through NriTextureCache and samples its "
          "own flat colour (vulkan)",
          "[gpu][pixel][nri][texcache-artifact][vulkan]")
{
    CheckArtifactUploadSamplesItsFlatColour(Arcane::GraphicsBackend::Vulkan);
}
