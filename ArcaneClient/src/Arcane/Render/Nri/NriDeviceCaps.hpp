#pragma once

// NRI substrate (Phase 4, Task 1): a one-time snapshot of the wrapped
// device's tiers/features, taken at NriDevice::FinishWrap and read back
// through NriDevice::Caps() for the rest of the process's lifetime.
//
// Why this exists: GetDeviceDesc is already called in six places in this
// tree, every one of them for alignment values -- nothing reads
// tiers/features. A later consumer gating on a feature it never checked
// (bindless indexing, ray tracing) becomes a hard crash on the first
// machine without it, arbitrarily far from a snapshot that could have
// caught it at wrap time. This struct is that snapshot: plain data, queried
// once, no live nri::Device reference to keep alive.

#include <cstdint>
#include <string>

namespace Arcane
{
    struct NriDeviceCaps
    {
        std::uint8_t bindlessTier   = 0;   // nri::DeviceDesc::tiers.bindless
        std::uint8_t rayTracingTier = 0;   // tiers.rayTracing
        bool         meshShader     = false;                // features.meshShader
        std::uint32_t maxDescriptorSetTextures = 0;          // descriptorSet.textureMaxNum
        std::uint32_t maxPerStageTextures = 0;                // shaderStage.descriptorTextureMaxNum
        std::uint32_t maxDescriptorSetUpdateAfterSetTextures = 0; // descriptorSet.updateAfterSet.textureMaxNum
        std::uint32_t maxPerStageUpdateAfterSetTextures = 0; // shaderStage.updateAfterSet.descriptorTextureMaxNum

        // Adapter identity (adapterDesc.name / architecture). Read by the hosts'
        // --compare to pick a software adapter's own reference set
        // (Arcane::ReferenceAdapterSet) -- not a capability gate.
        std::string  adapterName;
        bool         softwareAdapter = false;               // adapterDesc.architecture == SOFTWARE

        [[nodiscard]] bool SupportsBindless() const noexcept { return bindlessTier > 0; }
        // A table the device can build as update-after-set (MeshNode adds
        // ALLOW_UPDATE_AFTER_SET exactly when SupportsTextureUpdateAfterSet
        // holds) counts against the update-after-bind limits, not the plain
        // ones (Vulkan: maxDescriptorSetSampledImages covers only sets WITHOUT
        // the update-after-bind pool bit). MoltenVK is the case that tells
        // them apart: 640/128 plain, 1000000/1000000 update-after-bind.
        [[nodiscard]] bool SupportsBindlessTextures(std::uint32_t required) const noexcept
        {
            return SupportsBindless()
                && (SupportsTextureUpdateAfterSet(required)
                    || (maxDescriptorSetTextures >= required && maxPerStageTextures >= required));
        }
        [[nodiscard]] bool SupportsTextureUpdateAfterSet(std::uint32_t required) const noexcept
        {
            return maxDescriptorSetUpdateAfterSetTextures >= required
                && maxPerStageUpdateAfterSetTextures >= required;
        }
    };
}
