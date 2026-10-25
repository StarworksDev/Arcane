#pragma once

// Render budgets (settings arc S6-18; inventory Part 2 "Render graph context
// and budgets", "Batch2D", "Mesh pass", "Post chain", "ImGui NRI backend").
// Every field sizes a pool, a ring, a buffer or a budget that is fixed when
// its owner is created, so every one is Restart: the owner LATCHES its caps
// once, at creation, and a value published later never resizes a live pool.
//
// - render.uploadRingBytesPerFrame  RenderSettings (RenderDeviceSettings.hpp):
//                                   NriGraphContext's per-frame-slot ring.
// - render.batch2d.*    RenderBatch2dSettings: Batch2DNode's material slots,
//                       material textures, sprite textures and material CB
//                       region size.
// - render.gpuScene.*   RenderGpuSceneSettings: GpuScene's first instance
//                       capacity (it grows) and its per-frame scratch rows
//                       (a hard cap: ad-hoc instances past it are dropped).
// - render.mesh.*       RenderMeshSettings: NriMeshBufferCache's residency
//                       budget (the eviction threshold).
// - render.texture.*    RenderTextureSettings: the texture slice of the device
//                       residency account (latched at account creation).
// - render.residency.*  RenderResidencySettings: the reservation percent each
//                       slice keeps from the other.
// - render.texture.*    RenderTextureSettings: the texture slice of the device
//                       residency account (latched at account creation).
// - render.residency.*  RenderResidencySettings: the reservation percent each
//                       slice keeps from the other.
// - render.post.*       RenderPostSettings: PostChainNode's passes, declared
//                       textures and material CB region size.
// - render.imgui.*      RenderImguiSettings: ImGuiNri's descriptor-pool chain.
//
// Every field is Game Dev, Project, Restart, except the two a content author
// can plausibly outgrow: render.batch2d.maxSpriteTextures and
// render.mesh.residencyBudgetBytes (Game).

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>   // RenderSettings (render.uploadRingBytesPerFrame)

#include <cstdint>

namespace Arcane
{
    struct RenderBatch2dSettings
    {
        std::uint32_t maxMaterialSlots    = 8;
        std::uint32_t maxMaterialTextures = 8;
        std::uint32_t maxSpriteTextures   = 64;
        std::uint32_t materialCbBytes     = 256;   // a multiple of 256 (the node rounds down, with one WARN)
    };

    ARC_REFLECT_TYPE(RenderBatch2dSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.batch2d", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(RenderBatch2dSettings, maxMaterialSlots)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 64.0)
            ARC_REFLECT_ATTR(Tooltip, "Distinct registered sprite materials one frame may draw. Past it a material draws with "
                                      "the plain sprite pipeline and one error. Sizes the descriptor pool and the constant-"
                                      "buffer arena.")
        ARC_REFLECT_FIELD(RenderBatch2dSettings, maxMaterialTextures)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 16.0)
            ARC_REFLECT_ATTR(Tooltip, "Declared texture parameters one sprite material may carry. A material over it draws "
                                      "with the plain sprite pipeline and one error. Sizes the descriptor pool.")
        ARC_REFLECT_FIELD(RenderBatch2dSettings, maxSpriteTextures)
            ARC_REFLECT_ATTR(Range, 8.0, 512.0)
            ARC_REFLECT_ATTR(Tooltip, "Distinct sprite textures the 2D renderer can bind. Past it sprites draw as flat tint "
                                      "(the white texel) with one error, and the run still exits 0. Multiplies the descriptor "
                                      "pool: (1 + this) x (1 + maxMaterialSlots x frames in flight) samplers, and a D3D12 "
                                      "shader-visible sampler heap holds at most 2048, so a value past that ceiling is "
                                      "clamped with a warning.")
        ARC_REFLECT_FIELD(RenderBatch2dSettings, materialCbBytes)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 256.0, 65536.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest constant buffer, in bytes, a sprite material's numeric parameters may pack "
                                      "into. A multiple of 256; any other value is rounded down with a warning.")
    ARC_END_REFLECT_TYPE()

    struct RenderGpuSceneSettings
    {
        std::uint32_t initialRows         = 256;
        std::uint32_t scratchRowsPerFrame = 64;
    };

    ARC_REFLECT_TYPE(RenderGpuSceneSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.gpuScene", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderGpuSceneSettings, initialRows)
            ARC_REFLECT_ATTR(Range, 16.0, 65536.0)
            ARC_REFLECT_ATTR(Tooltip, "Instance rows the GPU scene buffer starts with. It doubles when the scene outgrows "
                                      "it, so this only saves the first growths.")
        ARC_REFLECT_FIELD(RenderGpuSceneSettings, scratchRowsPerFrame)
            ARC_REFLECT_ATTR(Range, 16.0, 4096.0)
            ARC_REFLECT_ATTR(Tooltip, "Ad-hoc mesh instances (drawn outside the scene registry, e.g. previews) one frame may "
                                      "draw. Past it the rest are dropped for that frame with one warning.")
    ARC_END_REFLECT_TYPE()

    struct RenderMeshSettings
    {
        std::uint64_t residencyBudgetBytes = 512ull << 20;
    };

    ARC_REFLECT_TYPE(RenderMeshSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.mesh", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(RenderMeshSettings, residencyBudgetBytes)
            ARC_REFLECT_ATTR(Range, 67108864.0, 17179869184.0)
            ARC_REFLECT_ATTR(Tooltip, "Memory budget, in bytes, for resident meshes (CPU copy plus GPU buffers). Past it the "
                                      "least recently drawn meshes are evicted and re-upload when next drawn.")
    ARC_END_REFLECT_TYPE()

    struct RenderTextureSettings
    {
        std::uint64_t residencyBudgetBytes = 512ull << 20;
    };

    ARC_REFLECT_TYPE(RenderTextureSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.texture", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(RenderTextureSettings, residencyBudgetBytes)
            ARC_REFLECT_ATTR(Range, 67108864.0, 17179869184.0)
            ARC_REFLECT_ATTR(Tooltip, "Texture slice of the device residency account, in bytes, latched at account creation.")
    ARC_END_REFLECT_TYPE()

    struct RenderResidencySettings
    {
        std::uint32_t reservationPercent = 50;
    };

    ARC_REFLECT_TYPE(RenderResidencySettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.residency", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderResidencySettings, reservationPercent)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Percent of each slice's own cap that the other slice may not borrow.")
    ARC_END_REFLECT_TYPE()

    struct RenderPostSettings
    {
        std::uint32_t maxPasses       = 8;
        std::uint32_t maxTextures     = 8;
        std::uint32_t materialCbBytes = 256;   // a multiple of 256 (the node rounds down, with one WARN)
    };

    ARC_REFLECT_TYPE(RenderPostSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.post", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderPostSettings, maxPasses)
            ARC_REFLECT_ATTR(Range, 1.0, 32.0)
            ARC_REFLECT_ATTR(Tooltip, "Passes one post-process chain may have. A longer chain is skipped (the frame renders "
                                      "without post) with one error. Sizes the descriptor pool.")
        ARC_REFLECT_FIELD(RenderPostSettings, maxTextures)
            ARC_REFLECT_ATTR(Range, 1.0, 16.0)
            ARC_REFLECT_ATTR(Tooltip, "Declared texture parameters one post-process chain may carry. A chain over it is "
                                      "skipped with one error. Sizes the descriptor pool.")
        ARC_REFLECT_FIELD(RenderPostSettings, materialCbBytes)
            ARC_REFLECT_ATTR(Range, 256.0, 65536.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest constant buffer, in bytes, a post chain's numeric parameters may pack into. "
                                      "A multiple of 256; any other value is rounded down with a warning.")
    ARC_END_REFLECT_TYPE()

    struct RenderImguiSettings
    {
        std::uint32_t firstPoolSets      = 64;
        std::uint32_t maxPoolSetsPerLink = 1024;
    };

    ARC_REFLECT_TYPE(RenderImguiSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.imgui", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderImguiSettings, firstPoolSets)
            ARC_REFLECT_ATTR(Range, 1.0, 1024.0)
            ARC_REFLECT_ATTR(Tooltip, "Textures the first ImGui descriptor pool holds. When it fills, another pool twice "
                                      "the size is added, up to render.imgui.maxPoolSetsPerLink.")
        ARC_REFLECT_FIELD(RenderImguiSettings, maxPoolSetsPerLink)
            ARC_REFLECT_ATTR(Range, 64.0, 2048.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest single ImGui descriptor pool, in textures. 2048 is the ceiling: a D3D12 "
                                      "shader-visible sampler heap holds at most 2048 descriptors.")
    ARC_END_REFLECT_TYPE()

    // A material constant-buffer region size as the nodes use it: rounded
    // DOWN to a multiple of 256 (D3D12's constant-buffer placement alignment
    // and the size a CBV must have), and never below 256. The setting's Range
    // keeps it in 256..65536; this only removes a remainder.
    [[nodiscard]] constexpr std::uint32_t MaterialCbRegionBytes(std::uint32_t requested) noexcept
    {
        const std::uint32_t rounded = requested / 256u * 256u;
        return rounded < 256u ? 256u : rounded;
    }
}
