// NriTextureCache -- see the header for what this owns, where the pixels come
// from, and why these textures are deliberately NOT graph resources.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "NriTextureCache.hpp"

// The Render/Nri headers FIRST among the Arcane ones, exactly as Batch2DNode
// orders them: NriDevice.hpp pulls Extensions/NRIDeviceCreation.h, whose
// nri::Message enumerator is literally named ERROR, and Arcane/Base/Log.hpp
// reaches <windows.h> through spdlog, which #defines it away via wingdi.h.
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/BindlessTable.hpp>   // AdoptTextureBacking (Invalidate)
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/ResidencyAccount.hpp>

#include <Arcane/Assets/ArtifactReader.hpp>   // LoadedClientArtifact -- the compiled-texture supply (Task 7)
#include <Arcane/Assets/ImageIo.hpp>   // PixelData -- the device-free decode (Task 1)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>   // render.debug.pendingCookChecker*

#undef ERROR

#include <Arcane/Render/Nri/AssetsCookSettings.hpp>   // assets.cook.pendingRepollInterval (the PendingCook throttle)

#include <array>
#include <string>

namespace Arcane
{
    std::unique_ptr<NriTextureCache> NriTextureCache::Create(NriDevice& device)
    {
        std::unique_ptr<NriTextureCache> cache(new NriTextureCache());
        cache->m_device = &device;
        cache->m_account = &device.Residency();   // one account per device; created (and latched) by the first cache

        // Resolved ONCE, here, rather than per upload: nriGetInterface is a
        // table copy, and doing it inside Resolve would repeat it for every
        // image in the project.
        if (!ARC_NRI_CHECK(nriGetInterface(device.Device(), NRI_INTERFACE(nri::HelperInterface),
                                            &cache->m_helper)))
        {
            ARC_ERROR("[nri-graph] NriTextureCache: HelperInterface unavailable -- no image can be "
                      "made resident on the graph device");
            return nullptr;
        }
        return cache;
    }

    NriTextureCache::~NriTextureCache()
    {
        if (m_account)
            m_account->UnchargeOwner(this);
        bool anyPlaceholders = false;
        for (const Resident& ph : m_placeholders)
            anyPlaceholders = anyPlaceholders || ph.texture || ph.view;

        if (!m_device || (m_textures.empty() && !anyPlaceholders))
            return;

        ARC_WARN("[nri-graph] NriTextureCache destroyed with {} live NRI object set(s) -- its owner "
                 "never called Release(). Destroying directly behind a DeviceWaitIdle.",
                 m_textures.size() + (anyPlaceholders ? m_placeholders.size() : 0));
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (auto& [key, resident] : m_textures)
        {
            if (resident.view)    core.DestroyDescriptor(resident.view);
            if (resident.texture) core.DestroyTexture(resident.texture);
        }
        m_textures.clear();
        for (Resident& ph : m_placeholders)
        {
            if (ph.view)    core.DestroyDescriptor(ph.view);
            if (ph.texture) core.DestroyTexture(ph.texture);
            ph = Resident{};
        }
    }

    void NriTextureCache::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view can never outlive its texture.
        for (auto& [key, resident] : m_textures)
        {
            if (resident.view)
                graveyard.Bury(fence, [core, d = resident.view] { core->DestroyDescriptor(d); });
            if (resident.texture)
                graveyard.Bury(fence, [core, t = resident.texture] { core->DestroyTexture(t); });
        }
        m_textures.clear();
        if (m_account)
            m_account->UnchargeOwner(this);

        // The checkerboard placeholder(s) -- ONCE each, never once per
        // PendingCook key (which never owned the object; see
        // EnsureCheckerboard/ResolveArtifactKey). Reset to empty afterwards so
        // a PendingCook resolve after this Release() recreates a fresh one
        // rather than reusing a buried handle.
        for (Resident& ph : m_placeholders)
        {
            if (ph.view)
                graveyard.Bury(fence, [core, d = ph.view] { core->DestroyDescriptor(d); });
            if (ph.texture)
                graveyard.Bury(fence, [core, t = ph.texture] { core->DestroyTexture(t); });
            ph = Resident{};
        }
        // The warn latch stays SET across a release: a run that could not
        // resolve an image before a resize should not re-announce it after one.
    }

    void NriTextureCache::Invalidate(const Guid& id, Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        // Both colour spaces -- content is always Srgb, but sweeping Display
        // too costs one extra unordered_map lookup (almost always a miss)
        // and keeps this method correct even if a future caller ever DID
        // resolve the same guid in both spaces. Never touches the shared
        // checkerboard placeholder(s): a PendingCook/Refused key's
        // texture/view are always null (ResolveArtifactKey's own contract),
        // so only a genuinely RESIDENT key's own object is ever buried here.
        for (const ColorSpace space : { ColorSpace::Srgb, ColorSpace::Display })
            DropKey(id, space, graveyard, fence);
    }

    // THE ONE DROP PATH for a key (Invalidate calls it per space; an account
    // eviction calls it for the one space).
    void NriTextureCache::DropKey(const Guid& id, ColorSpace space, Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        const auto it = m_textures.find(Key{ id, space });
        if (it == m_textures.end())
            return;
        if (m_account)
            m_account->Uncharge(this, id, ResidencySlice::Texture, space);
        // A bindless slot (mapped or pending) may still view this texture from
        // ANOTHER context's in-flight submit. Its destruction then joins the
        // slot's retire (spec 3.5: the descriptor and the texture wait on the
        // last-use record together): descriptors first, then this cache's own
        // view, then the texture -- never at this context's fence alone.
        if (it->second.texture)
            if (BindlessTable* bindless = m_device->BindlessIfCreated())
            {
                const nri::Texture* texture = it->second.texture;
                if (bindless->AdoptTextureBacking(
                        texture, fence,
                        [core, d = it->second.view, t = it->second.texture]
                        {
                            if (d) core->DestroyDescriptor(d);
                            core->DestroyTexture(t);
                        }))
                {
                    m_textures.erase(it);
                    return;
                }
            }
        if (it->second.view)
            graveyard.Bury(fence, [core, d = it->second.view] { core->DestroyDescriptor(d); });
        if (it->second.texture)
            graveyard.Bury(fence, [core, t = it->second.texture] { core->DestroyTexture(t); });
        m_textures.erase(it);
    }

    void NriTextureCache::Evict(const Guid& id, ColorSpace space, Graveyard& graveyard, std::uint64_t fence)
    {
        const auto resident = m_textures.find(Key{ id, space });
        if (!m_device || resident == m_textures.end())
            return;
        // The mapping first, so the slot goes pending (or retires at once) and the
        // drop below finds it and joins its retire. Per colour space, unconditional --
        // EXCEPT when the mapped slot views a texture that is not THIS cache's copy:
        // another holder re-created the mapping over its own fresh copy after this
        // eviction was queued, and releasing it would only cost that holder a re-resolve.
        if (BindlessTable* bindless = m_device->BindlessIfCreated())
        {
            const BindlessTable::ColorSpace bspace = space == ColorSpace::Display ? BindlessTable::ColorSpace::Display
                                                                                  : BindlessTable::ColorSpace::Srgb;
            const nri::Texture* mapped = bindless->MappedTexture(id, bspace);
            if (!mapped || mapped == resident->second.texture)
                bindless->ReleaseAll(id, bspace);
        }
        DropKey(id, space, graveyard, fence);
    }

    void NriTextureCache::DrainPending(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_account)
            return;
        for (const ResidencyEviction& ev : m_account->TakePending(this))
            if (ev.slice == ResidencySlice::Texture)
                Evict(ev.id, ev.space, graveyard, fence);
    }

    void NriTextureCache::EvictPending(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_account)
            return;
        DrainPending(graveyard, fence);
        m_account->EndFrame(this);
    }

    void NriTextureCache::CloseFrame()
    {
        if (m_account)
            m_account->EndFrame(this);
    }

    void NriTextureCache::ChargeResident(const Key& key, const Resident& resident)
    {
        if (!m_account || resident.bytes == 0)
            return;
        (void)m_account->Charge(this, ResidencyEntry{ key.id, ResidencySlice::Texture, key.space, resident.bytes, 0 });
    }

    // A draw on the device clock; a key the account no longer holds for this cache
    // (queued for eviction, drop not run yet) is charged again -- it was just drawn.
    void NriTextureCache::NoteDraw(const Key& key, const Resident& resident)
    {
        if (!m_account || resident.bytes == 0)
            return;
        if (!m_account->Touch(this, key.id, ResidencySlice::Texture, key.space))
            ChargeResident(key, resident);
    }

    std::size_t NriTextureCache::ResidentCount() const noexcept
    {
        // Counted on the VIEW, for the same reason Resolve's hit is gated on
        // it: counting TEXTURES over-reports by one for every entry whose
        // upload or view creation failed after the texture itself was created
        // -- entries that are memoized FAILURES, not residents. ALSO gated on
        // state == Resident (Task 7): a PendingCook key has no view of its OWN
        // anyway (it borrows the shared placeholder's, see View() below), but
        // the state check is the honest reason -- a checkerboard is not "the
        // compiled asset made resident" regardless of how it is wired.
        std::size_t count = 0;
        for (const auto& [key, resident] : m_textures)
            if (resident.view && resident.state == ResidentState::Resident)
                ++count;
        return count;
    }

    std::size_t NriTextureCache::PlaceholderCount() const noexcept
    {
        std::size_t count = 0;
        for (const Resident& ph : m_placeholders)
            if (ph.texture)
                ++count;
        return count;
    }

    nri::Descriptor* NriTextureCache::View(const Guid& id, ColorSpace space) const
    {
        const auto found = m_textures.find(Key{ id, space });
        if (found == m_textures.end())
            return nullptr;
        // PendingCook keys never own a view -- Resolve leaves theirs null and
        // hands back the SHARED checkerboard's instead (one per colour space,
        // not one per pending key). Safe to read without creating anything:
        // by the time a caller asks View(), Resolve() has already run for
        // this key at least once (the documented calling convention), which
        // is what put this key into PendingCook and built the placeholder in
        // the first place.
        if (found->second.state == ResidentState::PendingCook)
            return m_placeholders[static_cast<std::size_t>(space)].view;
        return found->second.view;
    }

    nri::Texture* NriTextureCache::Resolve(const Guid& id, ColorSpace space)
    {
        // The ordinary untextured case -- every Rect, Line, Circle, glyph and
        // colored quad. Not a miss, not a warning, not an entry.
        if (!id.IsValid())
            return nullptr;

        const Key key{ id, space };
        // TASK 7 ROUTING RULE -- see ColorSpace's own doc comment: a Srgb-space
        // key with an artifact supply installed goes through the artifact-
        // shaped path; every other key (Display, or Srgb with none installed)
        // takes the legacy pixel-supply path below, byte for byte.
        const bool artifactPath = (space == ColorSpace::Srgb) && static_cast<bool>(m_artifactSupply);

        const auto cached = m_textures.find(key);
        if (cached != m_textures.end())
        {
            // PendingCook is the ONE state this cache re-attempts rather than
            // serving a memo forever -- Task 12's live cook queue may have
            // produced the artifact since the last poll. THROTTLED (review
            // fix): Resolve runs at declaration time, every frame, per
            // on-screen span, so polling on EVERY ask drove a fresh
            // Assets::ArtifactFor call -- and, downstream, a full
            // Intermediate/Artifacts/** rescan -- every single frame for
            // every still-uncooked texture. Absorb
            // assets.cook.pendingRepollInterval asks between polls (read
            // live, here); AssetsCookSettings.hpp has the cost/promptness
            // tradeoff.
            if (cached->second.state == ResidentState::PendingCook)
            {
                if (m_artifactSupply &&
                    ++cached->second.pendingAsksSincePoll >= Settings<AssetsCookSettings>().pendingRepollInterval)
                {
                    cached->second.pendingAsksSincePoll = 0;
                    return ResolveArtifactKey(key, cached->second);
                }
                // Either the throttle window has not elapsed yet, or the
                // supply was pulled mid-run (a vehicle teardown in progress,
                // say) -- either way, keep serving the placeholder rather
                // than silently going to nothing or polling early.
                return m_placeholders[static_cast<std::size_t>(space)].texture;
            }

            // THE VIEW, NOT THE TEXTURE, IS WHAT "RESIDENT" MEANS (whole-branch
            // review, M2). This used to return `texture` with the comment "null
            // for a load this cache already failed", which was false for two of
            // the four failure paths below: an upload failure and a view-create
            // failure both leave `texture` NON-null (it was created before they
            // ran), so a memoized failure was served as a hit and the caller
            // then found View() null. The cache's product is a bindable view --
            // a texture with no view is nothing anyone can use -- so that is
            // what the hit is gated on. The texture object itself stays owned
            // and is destroyed by Release()/ClearImmediate like any other, so
            // this leaks nothing; it just stops lying about what is resident.
            if (cached->second.view)
                NoteDraw(key, cached->second);
            return cached->second.view ? cached->second.texture : nullptr;
        }

        // Inserted BEFORE any early return, so a failure is attempted once
        // rather than re-resolved every frame -- PendingCook is the deliberate
        // exception to "once", handled above on every SUBSEQUENT ask.
        Resident& resident = m_textures[key];

        if (artifactPath)
            return ResolveArtifactKey(key, resident);

        // ===== LEGACY PIXEL-SUPPLY PATH -- unchanged since before Task 7,
        // and reached only for Display-space keys or a Srgb key with no
        // artifact supply installed (see `artifactPath` above). =====

        // THE ONE-SHOT MISS WARN -- moved here from Batch2DNode, and it now
        // covers the SPRITE's own texture as well as a material's declared
        // params, because both reach residency through this one cache.
        const auto reportMiss = [&](const std::string& why)
        {
            if (m_warnedMiss)
                return;
            m_warnedMiss = true;
            ARC_WARN("[nri-graph] NriTextureCache: image {} is not resident on the graph device -- "
                     "{}; the white texel is bound in its place (further occurrences are silent)",
                     id.ToString(), why);
        };

        const PixelData* pixels = m_supply ? m_supply(id) : nullptr;
        if (!pixels)
        {
            reportMiss(m_supply ? "the asset resolved to nothing, or its image could not be decoded"
                                : "no pixel supply is installed on this vehicle");
            return nullptr;
        }
        if (!pixels->Valid())
        {
            reportMiss("the decoded pixel buffer does not match its own dimensions");
            return nullptr;
        }
        // nri::Dim_t is 16-bit and the cast below would silently WRAP -- the
        // same refusal RenderGraph::RealizePool makes for a transient's extent.
        if (pixels->width > 0xFFFFu || pixels->height > 0xFFFFu)
        {
            reportMiss("the image is " + std::to_string(pixels->width) + "x"
                       + std::to_string(pixels->height)
                       + ", which nri::Dim_t (16-bit) cannot express");
            return nullptr;
        }

        const nri::CoreInterface& core = m_device->Core();

        nri::TextureDesc textureDesc = {};
        textureDesc.type      = nri::TextureType::TEXTURE_2D;
        textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
        // THE SPACE DECIDES THE FORMAT (see ColorSpace):
        //   Srgb    -> RGBA8_SRGB. The canvas is LINEAR and the hardware does
        //              the decode, so a UNORM view here renders the same asset
        //              visibly brighter.
        //   Display -> RGBA8_UNORM. The sampled texel composites directly into
        //              a display-referred target (ImGui draws post-tonemap),
        //              and an SRGB view there decodes a second time and reads
        //              dark.
        textureDesc.format    = space == ColorSpace::Display ? nri::Format::RGBA8_UNORM
                                                             : nri::Format::RGBA8_SRGB;
        textureDesc.width     = (nri::Dim_t)pixels->width;
        textureDesc.height    = (nri::Dim_t)pixels->height;
        textureDesc.depth     = 1;
        textureDesc.mipNum    = 1;   // mip 0 only
        textureDesc.layerNum  = 1;
        textureDesc.sampleNum = 1;
        if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                        0.0f, textureDesc, resident.texture))
            || !resident.texture)
        {
            reportMiss("CreateCommittedTexture failed");
            resident.texture = nullptr;
            return nullptr;
        }
        core.SetDebugName(resident.texture,
                          ("asset " + id.ToString()
                           + (space == ColorSpace::Display ? " (display)" : "")).c_str());

        // Through NRI's OWN helper: UploadData submits and waits internally,
        // which is why every caller must reach this at DECLARATION time and
        // never from inside a node's open command buffer. It is also what keeps
        // this file free of any CmdBarrier at all, so the graph's
        // no-hand-barriers rule reads the same from outside as it does inside.
        nri::TextureSubresourceUploadDesc subresource = {};
        subresource.slices     = pixels->rgba.data();
        subresource.sliceNum   = 1;
        subresource.rowPitch   = pixels->width * 4;
        subresource.slicePitch = pixels->width * pixels->height * 4;

        nri::TextureUploadDesc upload = {};
        upload.subresources = &subresource;
        upload.texture      = resident.texture;
        upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                nri::StageBits::FRAGMENT_SHADER };
        upload.planes       = nri::PlaneBits::ALL;
        if (!ARC_NRI_CHECK(m_helper.UploadData(*m_device->GraphicsQueue(), &upload, 1, nullptr, 0)))
        {
            reportMiss("the texture upload failed");
            return nullptr;
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = resident.texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, resident.view)) || !resident.view)
        {
            reportMiss("the shader-resource view could not be created");
            resident.view = nullptr;
            return nullptr;
        }

        ARC_DEBUG("[nri-graph] NriTextureCache: image {} ({}x{}, {}) is resident on the graph device",
                  id.ToString(), pixels->width, pixels->height,
                  space == ColorSpace::Display ? "display-referred" : "sRGB");
        resident.bytes = static_cast<std::uint64_t>(pixels->width) * pixels->height * 4u;   // mip 0, RGBA8
        ChargeResident(key, resident);
        return resident.texture;
    }

    // =====================================================================
    // TASK 7 -- THE ARTIFACT-SHAPED PATH (Resident / PendingCook / Refused)
    // =====================================================================

    void NriTextureCache::ReportIssue(const std::string& why)
    {
        // THE SAME shared one-shot latch the legacy path's own `reportMiss`
        // lambda guards (m_warnedMiss) -- one line per RUN across every
        // failure reason this cache can report, not one per reason. See
        // m_warnedMiss's own comment.
        if (m_warnedMiss)
            return;
        m_warnedMiss = true;
        ARC_WARN("[nri-graph] NriTextureCache: {} (further occurrences are silent)", why);
    }

    NriTextureCache::Resident& NriTextureCache::EnsureCheckerboard(ColorSpace space)
    {
        Resident& placeholder = m_placeholders[static_cast<std::size_t>(space)];
        if (placeholder.texture)
            return placeholder;   // already built -- ONE per space, shared by every pending key in it

        // An 8x8 checkerboard in two saturated, mutually legible colours --
        // deliberately NOT the white texel a Refused/nil-Guid lookup falls
        // back to, and not a plausible real-asset colour either, so a
        // PendingCook sprite reads as "placeholder" at a glance and can never
        // be mistaken for a Refused one (the spec's placeholder rule: the two
        // must never render alike). The colours are
        // render.debug.pendingCookChecker / pendingCookCheckerAlt (default
        // magenta and (16,16,16)), read when the placeholder is built.
        const RenderDebugSettings& debugSettings = Settings<RenderDebugSettings>();
        const std::array<std::uint8_t, 4> lead = Srgb8Bytes(debugSettings.pendingCookChecker);
        const std::array<std::uint8_t, 4> alt  = Srgb8Bytes(debugSettings.pendingCookCheckerAlt);
        ARC_CONSTANT("placeholder shape: an 8x8 checker; only its colours are settings")
        constexpr std::uint32_t kSize = 8;
        std::vector<unsigned char> pixels(static_cast<std::size_t>(kSize) * kSize * 4);
        for (std::uint32_t y = 0; y < kSize; ++y)
        {
            for (std::uint32_t x = 0; x < kSize; ++x)
            {
                unsigned char* p = pixels.data() + (static_cast<std::size_t>(y) * kSize + x) * 4;
                const bool dark = ((x ^ y) & 1u) != 0u;
                const std::array<std::uint8_t, 4>& texel = dark ? alt : lead;
                p[0] = texel[0];
                p[1] = texel[1];
                p[2] = texel[2];
                p[3] = texel[3];
            }
        }

        const nri::CoreInterface& core = m_device->Core();

        nri::TextureDesc textureDesc = {};
        textureDesc.type      = nri::TextureType::TEXTURE_2D;
        textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
        // Same space -> format rule as every other texture this cache
        // uploads (see ColorSpace's own doc) -- the placeholder is a real
        // resident of whichever space asked for it.
        textureDesc.format    = space == ColorSpace::Display ? nri::Format::RGBA8_UNORM
                                                             : nri::Format::RGBA8_SRGB;
        textureDesc.width     = kSize;
        textureDesc.height    = kSize;
        textureDesc.depth     = 1;
        textureDesc.mipNum    = 1;
        textureDesc.layerNum  = 1;
        textureDesc.sampleNum = 1;
        if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                        0.0f, textureDesc, placeholder.texture))
            || !placeholder.texture)
        {
            ReportIssue("the pending-cook checkerboard placeholder could not be created");
            placeholder.texture = nullptr;
            return placeholder;
        }
        core.SetDebugName(placeholder.texture,
                          space == ColorSpace::Display ? "pending-cook placeholder (display)"
                                                        : "pending-cook placeholder");

        nri::TextureSubresourceUploadDesc subresource = {};
        subresource.slices     = pixels.data();
        subresource.sliceNum   = 1;
        subresource.rowPitch   = kSize * 4;
        subresource.slicePitch = kSize * kSize * 4;

        nri::TextureUploadDesc upload = {};
        upload.subresources = &subresource;
        upload.texture      = placeholder.texture;
        upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                nri::StageBits::FRAGMENT_SHADER };
        upload.planes       = nri::PlaneBits::ALL;
        if (!ARC_NRI_CHECK(m_helper.UploadData(*m_device->GraphicsQueue(), &upload, 1, nullptr, 0)))
        {
            ReportIssue("the pending-cook checkerboard placeholder failed to upload");
            return placeholder;   // texture stays (see UploadArtifact's own note on why);
                                    // view stays null, so this placeholder is never handed out
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = placeholder.texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, placeholder.view)) || !placeholder.view)
        {
            ReportIssue("the pending-cook checkerboard placeholder's view could not be created");
            placeholder.view = nullptr;
        }
        return placeholder;
    }

    bool NriTextureCache::UploadArtifact(const LoadedClientArtifact& artifact, Resident& resident)
    {
        if (artifact.mips.empty())
        {
            ReportIssue("a content artifact has no mip levels");
            return false;
        }
        // nri::Dim_t is 16-bit and a silent cast would WRAP -- the same
        // refusal the legacy path makes for a decoded image (and
        // RenderGraph::RealizePool for a transient's extent).
        if (artifact.info.width > 0xFFFFu || artifact.info.height > 0xFFFFu)
        {
            ReportIssue("a content artifact is " + std::to_string(artifact.info.width) + "x"
                        + std::to_string(artifact.info.height)
                        + ", which nri::Dim_t (16-bit) cannot express");
            return false;
        }

        // format + srgb -> the NRI format (Task 7 brief): BC7 uploads as
        // whichever BC7 view variant matches the artifact's own srgb flag,
        // RGBA8 the same choice in the uncompressed format -- srgb decides
        // the VARIANT for either payload shape, never just BC7's. A format
        // byte this reader does not recognise (BC5/BC6H's reserved values --
        // ArtifactReader.hpp's own forward-compat placeholders) is refused,
        // not guessed at.
        nri::Format format;
        switch (artifact.format)
        {
        case ArtifactPixelFormatValue::BC7:
            format = artifact.info.srgb ? nri::Format::BC7_RGBA_SRGB : nri::Format::BC7_RGBA_UNORM;
            break;
        case ArtifactPixelFormatValue::RGBA8:
            format = artifact.info.srgb ? nri::Format::RGBA8_SRGB : nri::Format::RGBA8_UNORM;
            break;
        default:
            ReportIssue("a content artifact's pixel format is not one this render cache can "
                        "upload yet");
            return false;
        }

        const nri::CoreInterface& core = m_device->Core();

        nri::TextureDesc textureDesc = {};
        textureDesc.type      = nri::TextureType::TEXTURE_2D;
        textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
        textureDesc.format    = format;
        textureDesc.width     = (nri::Dim_t)artifact.info.width;
        textureDesc.height    = (nri::Dim_t)artifact.info.height;
        textureDesc.depth     = 1;
        textureDesc.mipNum    = (nri::Dim_t)artifact.mips.size();
        textureDesc.layerNum  = 1;
        textureDesc.sampleNum = 1;
        if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                        0.0f, textureDesc, resident.texture))
            || !resident.texture)
        {
            ReportIssue("CreateCommittedTexture failed for a content artifact");
            resident.texture = nullptr;
            return false;
        }
        core.SetDebugName(resident.texture, "content artifact");

        // One TextureSubresourceUploadDesc per mip, in mip order -- layerNum
        // is 1 here (texture importer v1 is 2D-only), so subresource index ==
        // mip index with no layer-stride ambiguity. BC7's rows are BLOCKS
        // (Bc7RowPitch/Bc7SlicePitch), RGBA8's a straight width*4 passthrough
        // -- see this cache's own doc comment on Bc7RowPitch for the NPOT
        // reasoning (5->2->1 never rounds a row count down to zero: ceil, not
        // floor).
        std::vector<nri::TextureSubresourceUploadDesc> subresources(artifact.mips.size());
        for (std::size_t i = 0; i < artifact.mips.size(); ++i)
        {
            const MipView& mip = artifact.mips[i];
            if (mip.offset > artifact.payload.size() || mip.size > artifact.payload.size() - mip.offset)
            {
                ReportIssue("a content artifact's mip table overruns its own payload");
                return false;
            }
            nri::TextureSubresourceUploadDesc& sub = subresources[i];
            sub.slices   = artifact.payload.data() + mip.offset;
            sub.sliceNum = 1;
            if (artifact.format == ArtifactPixelFormatValue::BC7)
            {
                sub.rowPitch   = Bc7RowPitch(mip.width);
                sub.slicePitch = Bc7SlicePitch(mip.width, mip.height);
            }
            else
            {
                sub.rowPitch   = mip.width * 4u;
                sub.slicePitch = mip.width * mip.height * 4u;
            }
        }

        // Through NRI's OWN helper, exactly like the legacy path -- submits
        // and waits internally, so this must run at DECLARATION time (see the
        // header's NO BARRIERS note). A downstream failure here deliberately
        // does NOT null `resident.texture`: the object was already created,
        // and leaving it in place is what lets Release()/the destructor still
        // find and destroy it -- the same reasoning the legacy path's own
        // upload/view failures already rely on (see Resolve's HIT-check
        // comment above).
        nri::TextureUploadDesc upload = {};
        upload.subresources = subresources.data();
        upload.texture      = resident.texture;
        upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                nri::StageBits::FRAGMENT_SHADER };
        upload.planes       = nri::PlaneBits::ALL;
        if (!ARC_NRI_CHECK(m_helper.UploadData(*m_device->GraphicsQueue(), &upload, 1, nullptr, 0)))
        {
            ReportIssue("a content artifact's texture upload failed");
            return false;
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = resident.texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = format;
        viewDesc.mipNum   = (nri::Dim_t)artifact.mips.size();
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, resident.view)) || !resident.view)
        {
            ReportIssue("a content artifact's shader-resource view could not be created");
            resident.view = nullptr;
            return false;
        }

        // What this key costs the device account: every mip, BC7 by blocks, RGBA8 by texels.
        resident.bytes = 0;
        for (const MipView& mip : artifact.mips)
            resident.bytes += artifact.format == ArtifactPixelFormatValue::BC7
                ? static_cast<std::uint64_t>(Bc7SlicePitch(mip.width, mip.height))
                : static_cast<std::uint64_t>(mip.width) * mip.height * 4u;

        ARC_DEBUG("[nri-graph] NriTextureCache: content artifact ({}x{}, {} mip(s), {}) is resident "
                  "on the graph device",
                  artifact.info.width, artifact.info.height, artifact.mips.size(),
                  artifact.format == ArtifactPixelFormatValue::BC7 ? "BC7" : "RGBA8");
        return true;
    }

    nri::Texture* NriTextureCache::ResolveArtifactKey(const Key& key, Resident& resident)
    {
        const LoadedClientArtifact* artifact = m_artifactSupply(key.id);
        if (!artifact)
        {
            // F2b Task 12: a null answer means EITHER "not cooked yet, a
            // live cook queue will produce it" (genuinely PendingCook) OR
            // "refused -- HashMismatch/VersionNewerThanEngine -- and will
            // NEVER resolve without a user fixing the source" (Task 8 made
            // Assets::ArtifactFor return null for both alike, so this
            // return value alone cannot tell them apart). The cook-pending
            // oracle, when installed, is queue KNOWLEDGE from outside this
            // cache that can: no oracle installed, or the oracle answering
            // true, preserves the pre-Task-12 behaviour (PendingCook,
            // retriable); the oracle answering false means this guid is not
            // queued for a cook, so the null answer is a genuine refusal,
            // not a wait -- memoized Refused, exactly like an upload
            // failure below, so the checkerboard never claims "still
            // cooking" for something that is simply broken (the spec's
            // placeholder rule). A cook that completes LATER for a guid
            // stuck here reaches it through Invalidate(), not through this
            // branch re-polling -- Refused is sticky by design (see
            // ResidentState's own comment).
            if (m_cookPendingOracle && !m_cookPendingOracle(key.id))
            {
                resident.texture = nullptr;
                resident.view    = nullptr;
                resident.state   = ResidentState::Refused;
                return nullptr;
            }

            // Not cooked yet -- retriable (see ResidentState::PendingCook).
            // This key owns NO GPU object of its own: it borrows the shared
            // placeholder's, so Release()/the destructor never double-destroys
            // one checkerboard through N pending keys.
            const Resident& placeholder = EnsureCheckerboard(key.space);
            resident.texture = nullptr;
            resident.view    = nullptr;
            resident.state   = ResidentState::PendingCook;
            return placeholder.texture;
        }

        // A real artifact answered -- build from it. `resident` starts this
        // attempt from a clean slate every time (a re-poll from PendingCook
        // must not inherit a previous, unrelated attempt's partial state).
        resident.texture = nullptr;
        resident.view    = nullptr;
        resident.bytes = 0;
        const bool ok = UploadArtifact(*artifact, resident);
        resident.state = ok ? ResidentState::Resident : ResidentState::Refused;
        if (ok)
            ChargeResident(key, resident);
        return resident.view ? resident.texture : nullptr;
    }
}
