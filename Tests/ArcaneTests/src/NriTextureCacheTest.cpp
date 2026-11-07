// NriTextureCache -- the SHARED Guid -> nri::Texture residency cache, so the
// batch node and the post chain sample the SAME upload instead of each owning
// a copy.
//
// Everything here runs on the NONE backend through
// NriDevice::CreateNoneForTests(), which is the sanctioned carve-out from the
// wrapper-path rule (NriDevice.hpp: NONE has no native device to wrap). What
// NONE buys is exactly what these cases need -- CreateCommittedTexture,
// CreateTextureView and HelperInterface::UploadData all answer SUCCESS with
// dummy-but-non-null objects (ThirdParty/NRI/Source/NONE/ImplNONE.cpp) -- so
// the cache's POLICY (upload once, memoize misses, warn once, bury on release)
// is fully observable with no GPU. What it does not buy is pixels, which is
// why nothing here asserts any.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>   // RenderErrorCount -- the shared validation latch

#undef ERROR

#include <cstdint>
#include <memory>
#include <vector>

namespace
{
    const Arcane::Guid kIdA = *Arcane::Guid::FromString("aaaaaaaa-1111-2222-3333-444444444444");
    const Arcane::Guid kIdB = *Arcane::Guid::FromString("bbbbbbbb-5555-6666-7777-888888888888");

    Arcane::PixelData MakePixels(std::uint32_t w, std::uint32_t h)
    {
        Arcane::PixelData p;
        p.width  = w;
        p.height = h;
        p.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0xAB);
        return p;
    }

    // A pixel supply that COUNTS its calls -- the only way to observe
    // "decoded/resolved once" and "a miss is memoized, not re-attempted".
    struct CountingSupply
    {
        int calls = 0;
        const Arcane::PixelData* answer = nullptr;

        Arcane::NriTextureCache::PixelSupplyFn Fn()
        {
            return [this](const Arcane::Guid&) -> const Arcane::PixelData*
            {
                ++calls;
                return answer;
            };
        }
    };
}

TEST_CASE("nri texture cache: a nil Guid resolves to nothing and never asks the supply", "[nri]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    Arcane::PixelData pixels = MakePixels(2, 2);
    CountingSupply supply;
    supply.answer = &pixels;
    cache->SetPixelSupply(supply.Fn());

    // A nil id is the ORDINARY untextured case -- every Rect, Line, Circle and
    // colored quad records one. It must cost nothing and say nothing.
    CHECK(cache->Resolve(Arcane::Guid::Nil()) == nullptr);
    CHECK(cache->View(Arcane::Guid::Nil()) == nullptr);
    CHECK(supply.calls == 0);
    CHECK(cache->ResidentCount() == 0);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

TEST_CASE("nri texture cache: an unsupplied Guid misses once and memoizes the miss", "[nri]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    CountingSupply supply;
    supply.answer = nullptr;   // no pixels for anything -- THE TEXTURE GAP
    cache->SetPixelSupply(supply.Fn());

    CHECK(cache->Resolve(kIdA) == nullptr);
    CHECK(supply.calls == 1);

    // MEMOIZED: a frame that draws this sprite every frame must not re-ask
    // (and must not re-warn) sixty times a second.
    CHECK(cache->Resolve(kIdA) == nullptr);
    CHECK(cache->View(kIdA) == nullptr);
    CHECK(supply.calls == 1);

    // A DIFFERENT id is a separate question and is asked separately.
    CHECK(cache->Resolve(kIdB) == nullptr);
    CHECK(supply.calls == 2);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

TEST_CASE("nri texture cache: with no supply installed at all, Resolve misses quietly", "[nri]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    // The frame driver installs the supply right after Create; a vehicle that
    // has not yet (or a device-less drive) must degrade, not crash.
    CHECK(cache->Resolve(kIdA) == nullptr);
    CHECK(cache->View(kIdA) == nullptr);
    CHECK(cache->ResidentCount() == 0);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

TEST_CASE("nri texture cache: a supplied Guid uploads ONCE and serves one texture + one view",
          "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    Arcane::PixelData pixels = MakePixels(4, 2);
    REQUIRE(pixels.Valid());
    CountingSupply supply;
    supply.answer = &pixels;
    cache->SetPixelSupply(supply.Fn());

    nri::Texture* first = cache->Resolve(kIdA);
    REQUIRE(first != nullptr);
    CHECK(supply.calls == 1);
    CHECK(cache->ResidentCount() == 1);

    nri::Descriptor* view = cache->View(kIdA);
    REQUIRE(view != nullptr);

    // UPLOAD ONCE. The second Resolve is a map lookup: same object, no second
    // trip through the supply and no second HelperInterface::UploadData (which
    // submits and waits, and would be a per-frame stall if it repeated).
    CHECK(cache->Resolve(kIdA) == first);
    CHECK(cache->View(kIdA) == view);
    CHECK(supply.calls == 1);
    CHECK(cache->ResidentCount() == 1);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("nri texture cache: an image nri::Dim_t cannot express is refused, not truncated",
          "[nri]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    // nri::Dim_t is 16-bit. A silent cast would wrap 70000 to 4464 and upload
    // a texture whose row pitch describes a different image -- the same
    // refusal RenderGraph::RealizePool makes for a transient's extent.
    Arcane::PixelData huge = MakePixels(70000, 1);
    REQUIRE(huge.Valid());
    CountingSupply supply;
    supply.answer = &huge;
    cache->SetPixelSupply(supply.Fn());

    CHECK(cache->Resolve(kIdA) == nullptr);
    CHECK(cache->ResidentCount() == 0);
    // ...and the refusal is memoized like any other miss.
    CHECK(cache->Resolve(kIdA) == nullptr);
    CHECK(supply.calls == 1);

    cache->Release(device->Graves(), 1);
    device->Graves().Reap(1);
}

TEST_CASE("nri texture cache: Release BURIES its objects rather than destroying them", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    Arcane::PixelData pixels = MakePixels(2, 2);
    CountingSupply supply;
    supply.answer = &pixels;
    cache->SetPixelSupply(supply.Fn());

    REQUIRE(cache->Resolve(kIdA) != nullptr);
    REQUIRE(cache->Resolve(kIdB) != nullptr);
    CHECK(cache->ResidentCount() == 2);

    Arcane::Graveyard& graves = device->Graves();
    const std::size_t pendingBefore = graves.Pending();

    cache->Release(graves, 7);

    // Two objects per resident texture -- the view and the texture -- and the
    // view is buried FIRST, so a burial can never destroy a texture a live
    // descriptor still names (the graveyard runs burials in order).
    CHECK(graves.Pending() == pendingBefore + 4);
    CHECK(cache->ResidentCount() == 0);

    graves.Reap(7);
    CHECK(graves.Pending() == 0);

    // Idempotent: a second Release buries nothing.
    cache->Release(graves, 8);
    CHECK(graves.Pending() == 0);

    CHECK(Arcane::RenderErrorCount() == before);
}

// ===================================================================
// COLOUR SPACE
// ===================================================================
// THIS CACHE IS THE ONLY PLACE THE DISTINCTION LIVES: sRGB for the linear scene
// canvas, UNORM for ImGui's display-referred target. It keys on (Guid, space),
// and these cases pin the two properties that has to have: the DEFAULT is sRGB
// (every scene caller gets it without asking), and the two spaces are genuinely
// SEPARATE residents rather than one entry serving both.
//
// What NONE cannot show is the FORMAT itself -- ImplNONE hands back a
// dummy-but-non-null texture for any desc and there is no GetTextureDesc to
// read it back through on that backend. The format branch is one ternary at
// the create site, and its consumer (ImGuiNri::EnsureEntry reads the texture's
// ACTUAL format for its view) is a desk item: the logo drawn DARK is what a
// wrong space looks like.

TEST_CASE("nri texture cache: the colour space is part of an image's identity", "[nri]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto cache = Arcane::NriTextureCache::Create(*device);
    REQUIRE(cache != nullptr);
    cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

    Arcane::PixelData pixels = MakePixels(4, 4);
    CountingSupply supply;
    supply.answer = &pixels;
    cache->SetPixelSupply(supply.Fn());

    using Space = Arcane::NriTextureCache::ColorSpace;

    // The default IS sRGB -- the un-suffixed call and the explicit one are the
    // same entry, so no existing caller's behaviour moved.
    nri::Texture* srgb = cache->Resolve(kIdA);
    REQUIRE(srgb != nullptr);
    CHECK(cache->Resolve(kIdA, Space::Srgb) == srgb);
    CHECK(supply.calls == 1);          // one upload, not two
    CHECK(cache->ResidentCount() == 1);

    // ...and the display-referred one is a SECOND resident: a different
    // format is a different texture, because NRI has no typeless resource to
    // hang two views off.
    //
    // OBSERVED AS A SECOND UPLOAD AND A SECOND ENTRY, not as a different
    // pointer: ImplNONE hands back the SAME dummy handle (0x1) for every
    // CreateCommittedTexture and every CreateTextureView, so `display != srgb`
    // is unprovable on this backend -- it fails with 0x1 != 0x1 and would fail
    // exactly the same way if the two spaces shared one entry. The supply call
    // COUNT and ResidentCount are what actually distinguish the two, which is
    // the same reasoning the memoization cases above already run on.
    nri::Texture* display = cache->Resolve(kIdA, Space::Display);
    REQUIRE(display != nullptr);
    CHECK(supply.calls == 2);             // a second upload happened
    CHECK(cache->ResidentCount() == 2);   // ...into a second entry

    // Both are memoized independently: asking again for either uploads nothing.
    CHECK(cache->Resolve(kIdA, Space::Display) == display);
    CHECK(cache->Resolve(kIdA) == srgb);
    CHECK(supply.calls == 2);
    CHECK(cache->ResidentCount() == 2);
    // Each space HAS a view of its own (the lookups do not miss), and the
    // un-suffixed lookup is the sRGB one.
    REQUIRE(cache->View(kIdA, Space::Srgb) != nullptr);
    REQUIRE(cache->View(kIdA, Space::Display) != nullptr);
    CHECK(cache->View(kIdA) == cache->View(kIdA, Space::Srgb));
    // A space nobody resolved is a MISS, not a silent fallback to the other --
    // this is the assertion that would fail if the key ignored the space,
    // because kIdB has only ever been asked for in one of them.
    CHECK(cache->View(kIdB, Space::Display) == nullptr);

    // Release covers BOTH: 2 objects per resident (view + texture), 4 total.
    Arcane::Graveyard& graves = device->Graves();
    const std::size_t before = graves.Pending();
    cache->Release(graves, 1);
    CHECK(graves.Pending() == before + 4);
    CHECK(cache->ResidentCount() == 0);
    device->Graves().Reap(1);
}
