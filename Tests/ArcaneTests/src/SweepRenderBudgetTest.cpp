// Settings arc S6-18: the render budgets -- the upload ring, the Batch2D and
// post-chain pools, the GPU scene's rows, the mesh residency budget and the
// ImGui pool chain -- are Restart cvars. Each node latches its caps once, at
// creation, and every default is the literal it replaces.

// NRI headers first (NriCommon.hpp's include-order rule).
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/ImGui/ImGuiNri.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/nodes/Batch2DNode.hpp>
#include <Arcane/Render/RenderBudgetSettings.hpp>

#include <imgui.h>

#include <cstdint>

using namespace Arcane;

TEST_CASE("sweep: render budget defaults are the pre-sweep literals", "[sweep][render-budget]")
{
    CHECK(RenderSettings{}.uploadRingBytesPerFrame == 4ull * 1024 * 1024);
    CHECK(RenderBatch2dSettings{}.maxMaterialSlots == 8u);
    CHECK(RenderBatch2dSettings{}.maxMaterialTextures == 8u);
    CHECK(RenderBatch2dSettings{}.maxSpriteTextures == 64u);
    CHECK(RenderBatch2dSettings{}.materialCbBytes == 256u);
    CHECK(RenderGpuSceneSettings{}.initialRows == 256u);
    CHECK(RenderGpuSceneSettings{}.scratchRowsPerFrame == 64u);
    CHECK(RenderMeshSettings{}.residencyBudgetBytes == 512ull * 1024 * 1024);
    CHECK(RenderPostSettings{}.maxPasses == 8u);
    CHECK(RenderPostSettings{}.maxTextures == 8u);
    CHECK(RenderPostSettings{}.materialCbBytes == 256u);
    CHECK(RenderImguiSettings{}.firstPoolSets == 64u);
    CHECK(RenderImguiSettings{}.maxPoolSetsPerLink == 1024u);
    Test::RequireDefault("render.batch2d.maxSpriteTextures", CVarValue::UInt32(64u));
    Test::RequireDefault("render.uploadRingBytesPerFrame", CVarValue::UInt64(4ull << 20));
}

TEST_CASE("sweep: a material CB region size is rounded down to a multiple of 256", "[sweep][render-budget]")
{
    STATIC_REQUIRE(MaterialCbRegionBytes(256u) == 256u);
    STATIC_REQUIRE(MaterialCbRegionBytes(511u) == 256u);
    STATIC_REQUIRE(MaterialCbRegionBytes(512u) == 512u);
    STATIC_REQUIRE(MaterialCbRegionBytes(65536u) == 65536u);
    STATIC_REQUIRE(MaterialCbRegionBytes(0u) == 256u);   // never below one region, even outside the Range
}

TEST_CASE("sweep: Batch2D's sprite-texture cap is clamped to the 2048-sampler heap ceiling",
          "[sweep][render-budget]")
{
    // Device-less: CapsFrom and PoolSizes take the frame depth explicitly, so
    // every depth render.framesInFlight allows (2..3) is checked here without
    // a node. D3D12 creates the pool's one shader-visible sampler heap at
    // exactly samplerMaxNum, and that heap holds at most 2048 descriptors.
    using Node = Batch2DNode;
    STATIC_REQUIRE(Node::kMaxPoolSamplers == 2048u);
    const auto samplers = [](const Node::Caps& caps, std::uint32_t fif) {
        return Node::PoolSizes(caps, fif).samplerMaxNum;
    };

    for (const std::uint32_t fif : { 2u, 3u })
    {
        CAPTURE(fif);

        // The defaults come through UNCLAMPED (65 * 17 = 1105 at 2, 65 * 25 =
        // 1625 at 3), so the default pool is byte-identical to before.
        const RenderBatch2dSettings defaults{};
        const Node::Caps byDefault = Node::CapsFrom(defaults, fif);
        CHECK(byDefault.spriteTextures   == defaults.maxSpriteTextures);
        CHECK(byDefault.materialSlots    == defaults.maxMaterialSlots);
        CHECK(byDefault.materialTextures == defaults.maxMaterialTextures);
        CHECK(byDefault.materialCbBytes  == defaults.materialCbBytes);
        CHECK(samplers(byDefault, fif) <= Node::kMaxPoolSamplers);

        // The top of the Game-audience range (512) at the default 8 slots:
        // clamped to the LARGEST count that fits, slots untouched.
        RenderBatch2dSettings top;
        top.maxSpriteTextures = 512;
        const Node::Caps topCaps = Node::CapsFrom(top, fif);
        CHECK(topCaps.materialSlots == 8u);
        CHECK(samplers(topCaps, fif) <= Node::kMaxPoolSamplers);
        Node::Caps oneMore = topCaps;
        ++oneMore.spriteTextures;
        CHECK(samplers(oneMore, fif) > Node::kMaxPoolSamplers);

        // maxMaterialSlots at its Dev max (64) overflows even at the default
        // sprite count and at the range top; slots are never clamped.
        for (const std::uint32_t sprite : { 64u, 512u })
        {
            CAPTURE(sprite);
            RenderBatch2dSettings wide;
            wide.maxMaterialSlots  = 64;
            wide.maxSpriteTextures = sprite;
            const Node::Caps wideCaps = Node::CapsFrom(wide, fif);
            CHECK(wideCaps.materialSlots == 64u);
            CHECK(wideCaps.spriteTextures < sprite);
            CHECK(samplers(wideCaps, fif) <= Node::kMaxPoolSamplers);
        }

        // The floor (8) with 64 slots always fits -- why clamping spriteTextures
        // alone suffices: 9 * 193 = 1737 at 3 frames.
        RenderBatch2dSettings atFloor;
        atFloor.maxMaterialSlots  = 64;
        atFloor.maxSpriteTextures = 8;
        CHECK(Node::CapsFrom(atFloor, fif).spriteTextures == 8u);
    }

    // The exact clamp points: 2048 / 17 = 120 variants at 2 frames, 2048 / 25
    // = 81 at 3 (variant 0 is the nil texture, so one less sprite texture).
    RenderBatch2dSettings top;
    top.maxSpriteTextures = 512;
    CHECK(Node::CapsFrom(top, 2).spriteTextures == 119u);
    CHECK(Node::CapsFrom(top, 3).spriteTextures == 80u);
    RenderBatch2dSettings atEdge;
    atEdge.maxSpriteTextures = 119;
    CHECK(Node::CapsFrom(atEdge, 2).spriteTextures == 119u);   // fits exactly: untouched
}

TEST_CASE("sweep: the ImGui pool chain and the mesh cache latch their budgets at creation (Restart)",
          "[sweep][render-budget]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    // Device-less (the NONE backend), so this runs without a GPU: the pool
    // chain's bookkeeping and the cache's budget are CPU state.
    CVarRegistry& reg = CVarRegistry::Get();
    const auto set = [&reg](std::string_view name, const CVarValue& value)
    {
        REQUIRE(reg.Set(reg.Find(name), value, SetBy::Code) == SetResult::Applied);
        reg.PublishImmediate();
    };
    // render.imgui.firstPoolSets is a Dev row: compiled out of Dist, where only the mesh cache's half runs.
    const bool poolRow = Test::InThisBuild("render.imgui.firstPoolSets");
    if (poolRow) set("render.imgui.firstPoolSets", CVarValue::UInt32(16u));
    set("render.mesh.residencyBudgetBytes", CVarValue::UInt64(128ull << 20));

    auto device = NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGuiContext* const context  = ImGui::CreateContext();
    REQUIRE(context != nullptr);

    Graveyard lane;
    {
        NriPipelineCache pipelines;
        pipelines.Bind(*device);
        const std::uint8_t vsBytes[4] = { 1, 2, 3, 4 };
        const std::uint8_t psBytes[4] = { 5, 6, 7, 8 };
        ImGuiNri backend;
        REQUIRE(backend.Init(*device, pipelines, vsBytes, psBytes));
        auto cache = NriMeshBufferCache::Create(*device);
        REQUIRE(cache != nullptr);
        cache->SetUploadMode(Arcane::NriUploadMode::Immediate);   // these cases assert on a resident entry straight after Resolve (Plan C Task 5: the deferred path is UploadStagingTest's)

        // The published values, not the defaults.
        if (poolRow)
        {
            CHECK(backend.LinkCapacity(0) == 16u);
            CHECK(backend.LinkCapacity(1) == 32u);
        }
        CHECK(cache->Budget() == (128ull << 20));

        // A later publish never resizes what already exists.
        if (poolRow) set("render.imgui.firstPoolSets", CVarValue::UInt32(256u));
        set("render.mesh.residencyBudgetBytes", CVarValue::UInt64(1ull << 30));
        if (poolRow) CHECK(backend.LinkCapacity(0) == 16u);
        CHECK(cache->Budget() == (128ull << 20));

        backend.Release(lane, 1);
        cache->Release(lane, 1);
        pipelines.Clear(lane, 1);
        lane.Drain();
    }

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    reg.RevertLayer(SetBy::Code);
    reg.PublishImmediate();
}
