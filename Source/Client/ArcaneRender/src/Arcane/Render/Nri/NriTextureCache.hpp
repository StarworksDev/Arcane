#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// NriTextureCache -- ONE Guid -> nri::Texture residency cache for the whole
// render path.
//
// IT IS SHARED BY EVERY NODE THAT SAMPLES AN ASSET, and that sharing is the
// point: a sprite's own texture (t0, from Batch2DDrawSpan::textureId), a
// registered material's DECLARED params (t1..), and a post material's
// declared params all resolve through THIS cache -- so an image named by a
// sprite and by a post pass gets ONE upload rather than two.
//
// WHERE THE PIXELS COME FROM, and why this class does not decode. The bytes
// arrive through an injected `PixelSupplyFn` -- in production
// `Assets::PixelsFor(Guid)`, the engine's retained decode-once pixel cache,
// which is DEVICE-FREE by charter. So this class owns no file I/O, no stb call
// and no knowledge of the project registry. It is the same injection shape
// (and the same reasoning) as NriGraphContext::AssetResolveFn: a RENDER object
// must not grow a Runtime.
//
// THE SRGB RULE: scene content uploads RGBA8_SRGB, because the canvas is
// LINEAR and the hardware does the decode -- a UNORM view would render the
// same asset visibly brighter. See ColorSpace below for the other half.
//
// FAILURES ARE MEMOIZED, exactly once each: an entry is inserted BEFORE the
// first early return, with null members, so an unresolvable or undecodable
// image is attempted once rather than re-attempted every frame by a sprite
// that is still on screen. The ONE-SHOT "not resident" WARN lives here too and
// fires only on a Resolve that MISSES -- a frame full of colored quads (every
// span carrying a nil Guid) says nothing at all.
//
// NO BARRIERS, and none needed. Uploads go through NRI's own
// HelperInterface::UploadData, which submits and waits internally and leaves
// the texture in SHADER_RESOURCE -- so these textures are NOT graph resources
// (they are persistent, never written by a node, and their state never
// changes), which is why nothing about them appears in a compiled frame. They
// must therefore be resolved at DECLARATION time, never from inside a node's
// exec fn: UploadData submitting while the frame's command buffer is open is
// exactly the shape the graph's no-hand-barriers rule exists to prevent.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR and
// <windows.h>, via spdlog, #defines ERROR through wingdi.h).
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Guid.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class ResidencyAccount;
    struct PixelData;
    struct LoadedClientArtifact;

    class ARC_RENDER_API NriTextureCache
    {
    public:
        // Guid -> decoded RGBA8 pixels, or null when the id names nothing this
        // process can decode. `Assets::PixelsFor` in production; the returned
        // pointer is READ INSIDE the call and never stored, so the supply owns
        // the buffer's lifetime and may evict it afterwards.
        using PixelSupplyFn = std::function<const PixelData*(const Guid&)>;

        // Guid -> a compiled texture artifact (BC7 or RGBA8 mips + the format/
        // srgb metadata to upload them), or null when no cooked artifact exists
        // for this id YET (not a permanent miss -- see PendingCook below).
        // `Assets::ArtifactFor` in production (Task 7); same "read inside the
        // call, never stored" contract as PixelSupplyFn.
        //
        // THIS IS THE CONTENT SEAM going forward (Task 7): Resolve/View for
        // ColorSpace::Srgb prefer this supply, when one is installed, over
        // PixelSupplyFn -- see ColorSpace's own doc below for the full routing
        // rule and why Display/chrome is deliberately untouched.
        using ArtifactSupplyFn = std::function<const LoadedClientArtifact*(const Guid&)>;

        // ===== WHICH COLOUR SPACE THE VIEW SAMPLES IN ====================
        // TWO SPACES, because the two have DIFFERENT consumers and there is no
        // one format that serves both:
        //
        //   Srgb    -- the SCENE. Uploads RGBA8_SRGB, so the sampler decodes
        //              to linear for the linear canvas and the hardware does
        //              the work. THE DEFAULT, and what every existing caller
        //              (Batch2DNode's sprites, PostChainNode's declared
        //              params) gets without asking.
        //   Display -- ImGui. Uploads RGBA8_UNORM, so the sampled texel goes
        //              STRAIGHT to a display-referred target. ImGuiNri draws
        //              after the tonemap and its own font atlas is UNORM for
        //              this reason; an SRGB view under it decodes a second
        //              time and the image renders visibly dark.
        //
        // THE SAME ASSET MAY BE RESIDENT IN BOTH, and that is deliberate
        // rather than a leak: the entries are keyed on (Guid, space) and a
        // second space is a second upload. NRI has no typeless textures to
        // hang two views off, and an asset wanted in both spaces is rare (one
        // is scene content, the other is chrome).
        //
        // TASK 7 ROUTING RULE (which supply answers a given key): a Srgb-space
        // resolve prefers ArtifactSupplyFn -- IF one is installed
        // (SetArtifactSupply) -- over PixelSupplyFn; every other case (Display,
        // or Srgb with no artifact supply installed) uses PixelSupplyFn exactly
        // as before ABI 21. This is what "the chrome path kept" means concretely:
        // the editor's toolbar mark (ColorSpace::Display, a PixelSupplyFn lambda,
        // no artifact supply ever installed on that vehicle) is byte-for-byte
        // unaffected, while a content vehicle that installs BOTH supplies still
        // answers Display-space lookups (if it ever has any) through
        // PixelSupplyFn -- only Srgb is artifact-shaped.
        enum class ColorSpace : std::uint8_t { Srgb, Display };

        // Resolves the HelperInterface once and takes a borrowed reference to
        // the device, which must outlive this object (the vehicle owns both).
        // Null, already logged, if the helper is unavailable.
        static std::unique_ptr<NriTextureCache> Create(NriDevice& device);

        // SAFETY NET, NOT THE PATH -- the same shape as ~Batch2DNode. The
        // sanctioned release is Release() at a fence the owner knows; if this
        // still holds objects it destroys them directly behind a
        // DeviceWaitIdle and says so at WARN, because there is no fence value
        // to bury against here and burying at 0 would violate Graveyard's
        // nondecreasing rule on a device the graph has been burying against
        // all run.
        ~NriTextureCache();

        NriTextureCache(const NriTextureCache&)            = delete;
        NriTextureCache& operator=(const NriTextureCache&) = delete;

        // Installed once by the frame driver, right after Create. Without it
        // every Resolve misses (loudly, once) and every texture slot falls back
        // to its node's white texel. Copied, not borrowed.
        void SetPixelSupply(PixelSupplyFn supply) { m_supply = std::move(supply); }

        // Installed once by the frame driver beside SetPixelSupply (Task 7).
        // Without it every Srgb-space Resolve falls back to PixelSupplyFn
        // exactly as before this task -- see ColorSpace's own routing-rule
        // comment. Copied, not borrowed.
        void SetArtifactSupply(ArtifactSupplyFn supply) { m_artifactSupply = std::move(supply); }

        // Guid -> "is a cook currently queued or in flight for this asset".
        // F2b Task 12: without this, ResolveArtifactKey cannot tell "not
        // cooked YET" (genuinely PendingCook -- the cook queue will produce
        // it) apart from "answered null and NEVER WILL without user action"
        // (HashMismatch/VersionNewerThanEngine -- Assets::ArtifactFor
        // refuses both identically to Missing as of Task 8, returning
        // nullptr either way), because the artifact supply's return value
        // alone cannot distinguish them. Installed ONLY on a vehicle that
        // also owns a live cook queue (the editor's viewport graph); left
        // unset (the default) preserves the EXACT pre-Task-12 behaviour --
        // every null answer is PendingCook, as ResolveArtifactKey's own doc
        // comment already describes -- so RuntimeApp and every existing
        // [texcache-artifact] case are unaffected. See ResolveArtifactKey's
        // own comment for exactly where this is consulted.
        using CookPendingOracle = std::function<bool(const Guid&)>;
        void SetCookPendingOracle(CookPendingOracle oracle) { m_cookPendingOracle = std::move(oracle); }

        // The texture for `id` on this device, uploading it on first sight.
        //
        // THREE OUTCOMES on the artifact-shaped path (Srgb, ArtifactSupplyFn
        // installed) that the legacy pixel path does not have:
        //   Resident    -- the artifact supply answered and the upload/view
        //                  succeeded. Returns the real texture, exactly like
        //                  the legacy "hit" case.
        //   PendingCook -- the artifact supply answered null (no cooked
        //                  artifact YET, not a permanent miss): returns the
        //                  cache-owned 8x8 checkerboard placeholder for this
        //                  colour space (lazily created, shared by every
        //                  pending key in that space), and RE-POLLS the
        //                  supply every assets.cook.pendingRepollInterval-th
        //                  Resolve for this key (throttled, not every ask --
        //                  Resolve runs at declaration time, per frame, per
        //                  span, and polling on every ask reaches all the way
        //                  down to a directory scan; see AssetsCookSettings.hpp)
        //                  -- the placeholder is a stand-in for
        //                  "still cooking", not a memoized failure, because
        //                  Task 12's live cook queue is expected to promote
        //                  it later.
        //   Refused     -- the supply answered with a real artifact but this
        //                  cache could not put it on the device (an
        //                  unsupported/unmapped pixel format, dimensions
        //                  nri::Dim_t cannot express, a malformed mip table,
        //                  or an NRI failure): returns null -- the SAME "bind
        //                  the white texel" null the legacy path already
        //                  means -- and is memoized (sticky): never
        //                  re-attempted. PendingCook and Refused therefore
        //                  never render alike: a checkerboard is a real,
        //                  visible texture, while a Refused key binds nothing
        //                  and falls through to the caller's own white texel,
        //                  the spec's placeholder rule.
        //
        // Everything else is the legacy contract, unchanged: null -- meaning
        // "bind the white texel" -- for a nil Guid (the ordinary untextured
        // case, silent), for an id the pixel supply has no pixels for, for an
        // image nri::Dim_t cannot express, and for an NRI refusal. Every null
        // but the nil-Guid one and PendingCook's placeholder is reported once
        // per cache and then memoized.
        //
        // CALL AT DECLARATION TIME ONLY -- see NO BARRIERS above.
        //
        // `space` defaults to Srgb, which is what every scene caller means and
        // is what keeps this signature's old behaviour byte for byte.
        [[nodiscard]] nri::Texture* Resolve(const Guid& id, ColorSpace space = ColorSpace::Srgb);

        // The SHADER_RESOURCE view over Resolve(id, space), created with it.
        // Null under exactly the same conditions, and it does NOT trigger a
        // resolve: a caller that wants residency asks Resolve first (which is
        // what the nodes' Prepare passes do), so nothing uploads from a lookup.
        // For a PendingCook key this is the shared checkerboard's own view --
        // safe to read without re-polling because Resolve always runs first.
        [[nodiscard]] nri::Descriptor* View(const Guid& id,
                                            ColorSpace space = ColorSpace::Srgb) const;

        // Buries every NRI object this cache owns at `fence` and empties it --
        // views before the textures they view, so the graveyard's in-order
        // reaping can never destroy a texture a live descriptor still names.
        // Idempotent. The caller picks the fence for the same reason
        // NriPipelineCache::Clear does: only it knows which timeline the
        // cache's users submitted on. Covers the lazily-created checkerboard
        // placeholder(s) too, exactly once each -- never once per PendingCook
        // key, which share the one object per colour space.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // F2b Task 12: drops residency for `id`, in BOTH colour spaces (a
        // content guid is always Srgb; Display is swept too, for symmetry
        // and at negligible cost -- an unordered_map lookup that almost
        // always misses), so the NEXT Resolve() treats it as a brand new
        // key -- Resident, PendingCook, or Refused, whichever this key was
        // left in, since a cook-completion callback does not know which.
        // This is the ESCAPE HATCH the cook-pending oracle above needs: a
        // key the oracle steered into Refused (because nothing was queued
        // for it YET) would otherwise be stuck there forever once a cook
        // finally lands, since Resolve()'s own re-poll is PendingCook-only
        // (Refused is sticky by design). A genuine RESIDENT texture/view --
        // never the shared checkerboard placeholder, which no key owns --
        // is buried at `fence`, exactly like Release()'s own bulk sweep, so
        // an already-recorded command buffer still reading the OLD texture
        // is never invalidated out from under it. A no-op for a guid this
        // cache has never resolved.
        void Invalidate(const Guid& id, Graveyard& graveyard, std::uint64_t fence);

        // An ACCOUNT EVICTION of ONE (id, space) entry (Plan C Task 4): releases that
        // space's bindless mappings unconditionally (BindlessTable::ReleaseAll -- an
        // eviction ends residency regardless of holders, so never the refcount
        // Release), then drops ONLY that key through the same drop path Invalidate
        // uses (a texture a bindless slot views is destroyed through the slot
        // backing, never directly). The other space's entry, slot and texture stay.
        // A no-op when this cache does not hold the key.
        void Evict(const Guid& id, ColorSpace space, Graveyard& graveyard, std::uint64_t fence);

        // The frame-boundary half of the device residency account for THIS cache:
        // carries out the evictions the account queued for it (Evict, on this cache's
        // own lane and fence) and ends its frame on the device clock. The vehicle
        // calls it where it calls NriMeshBufferCache::EvictToBudget.
        void EvictPending(Graveyard& graveyard, std::uint64_t fence);

        // How many images are actually RESIDENT (uploaded and viewable) --
        // i.e. in the Resident state. PendingCook keys (a real, viewable
        // checkerboard, just not the real asset) and Refused/memoized
        // failures (no GPU object) are both excluded -- neither is "the
        // compiled asset made resident", which is this counter's whole
        // meaning. Public because upload-once is otherwise unobservable from
        // outside, and it is the property that makes this a cache rather than
        // a loader.
        [[nodiscard]] std::size_t ResidentCount() const noexcept;

        // How many checkerboard placeholders actually exist right now (0, 1,
        // or 2 -- one slot per ColorSpace, lazily created on first
        // PendingCook). TEST-ONLY introspection: on the NONE backend every
        // NRI handle this cache ever creates is the SAME dummy pointer (see
        // NriTextureCacheTest.cpp's own ColorSpace-case comment), so pointer
        // identity cannot prove "one shared placeholder, not one per pending
        // key" the way it would on a real device -- this count is what makes
        // that property observable without one.
        [[nodiscard]] std::size_t PlaceholderCount() const noexcept;

        // BC-block row/slice pitch for a WxH mip. BC7 (and any future BCn
        // sharing its 4x4/16-byte block shape) packs texels into 4x4 blocks
        // regardless of the mip's own TRUE (possibly NPOT) dimensions -- see
        // TextureImporter.hpp's own padding note -- so a row of blocks covers
        // ceil(W/4) blocks and a mip covers ceil(H/4) rows of them. Exposed
        // (not file-local) so the pitch arithmetic is unit-testable
        // independent of any NRI device or artifact fixture: the exact
        // numbers Task 7's brief pins for a 5->2->1 NPOT mip chain are
        // Bc7RowPitch(5)==32, Bc7RowPitch(2)==Bc7RowPitch(1)==16.
        [[nodiscard]] static std::uint32_t Bc7RowPitch(std::uint32_t width) noexcept
        {
            return ((width + 3u) / 4u) * 16u;
        }
        [[nodiscard]] static std::uint32_t Bc7SlicePitch(std::uint32_t width,
                                                         std::uint32_t height) noexcept
        {
            return Bc7RowPitch(width) * ((height + 3u) / 4u);
        }

    private:
        NriTextureCache() = default;

        // Which of the three outcomes Resolve's doc comment describes a given
        // key is currently in. Meaningful ONLY for a key resolved through the
        // artifact-shaped path (ResolveArtifactKey below) -- a legacy
        // pixel-path entry never reads this field and leaves it at its
        // default, which is deliberately the same value a legacy "resident"
        // entry would want (see Resident's own comment).
        enum class ResidentState : std::uint8_t
        {
            Resident,      // texture/view are this key's OWN uploaded object.
            PendingCook,   // texture/view are left null; the shared checkerboard
                           // for this key's colour space is what Resolve/View
                           // actually hand back (see m_placeholders).
            Refused,       // texture/view are null (or texture non-null with a
                           // null view after a partial create -- same shape the
                           // legacy path's own failures already use), memoized.
        };

        // One image made resident here. A FAILED load is kept with null
        // members: attempted once, not once per frame -- EXCEPT PendingCook,
        // which is deliberately retried, THROTTLED (see ResidentState and
        // assets.cook.pendingRepollInterval).
        struct Resident
        {
            nri::Texture*    texture = nullptr;
            nri::Descriptor* view    = nullptr;
            ResidentState    state   = ResidentState::Resident;
            // PendingCook ONLY: Resolve asks absorbed since the last actual
            // poll of the artifact supply. Reset to 0 every time a poll
            // happens (whether it lands on PendingCook again or promotes/
            // refuses); meaningless -- and untouched -- in every other state.
            std::uint32_t    pendingAsksSincePoll = 0;
            // The GPU bytes this key charges the device residency account (Resident
            // state only; 0 for PendingCook / Refused, which hold no object of their own).
            std::uint64_t    bytes = 0;
        };

        // (asset, colour space) -- see ColorSpace. The SPACE is part of the
        // identity because it decides the uploaded FORMAT, so two spaces are
        // two textures and a lookup that ignored it would hand a caller the
        // other one's.
        struct Key
        {
            Guid       id;
            ColorSpace space = ColorSpace::Srgb;
            bool operator==(const Key&) const noexcept = default;
        };
        struct KeyHash
        {
            std::size_t operator()(const Key& k) const noexcept
            {
                // The Guid's own hash, mixed with the space through the
                // 32-BIT golden-ratio constant (0x9E3779B9; the 64-bit one is
                // 0x9E3779B97F4A7C15) -- two spaces of one asset must not land
                // in the same bucket chain by construction.
                const std::size_t h = std::hash<Guid>{}(k.id);
                return h ^ (static_cast<std::size_t>(k.space) + 0x9E3779B9u + (h << 6) + (h >> 2));
            }
        };

        // Resolves `key` through m_artifactSupply, driving it into PendingCook
        // (supply answered null), Resident (answered, uploaded), or Refused
        // (answered, but this cache could not upload it) -- see ResidentState
        // and Resolve's own doc comment. Called both for a BRAND NEW key and,
        // while `resident.state == PendingCook`, to RE-POLL an existing one --
        // the one asymmetry against every other entry in m_textures, which is
        // resolved exactly once.
        nri::Texture* ResolveArtifactKey(const Key& key, Resident& resident);

        // The one drop path for a resident key: a slot-backed destroy when a bindless
        // slot views the texture, otherwise the graveyard; uncharges the account.
        void DropKey(const Guid& id, ColorSpace space, Graveyard& graveyard, std::uint64_t fence);
        // Charge the account for a freshly resident key / note a draw of a hit.
        void ChargeResident(const Key& key, const Resident& resident);
        void NoteDraw(const Key& key, const Resident& resident);

        // The upload half of ResolveArtifactKey's Resident/Refused branch:
        // format+srgb -> nri::Format, textureDesc sized to the artifact's OWN
        // mip table, one TextureSubresourceUploadDesc per mip (BC7's via
        // Bc7RowPitch/Bc7SlicePitch, RGBA8's a straight width*4 passthrough),
        // one CreateTextureView spanning every mip. Writes into `resident`
        // exactly like the legacy path does on a partial failure (see
        // Resident's own comment: texture may stay non-null with a null view,
        // so Release()/the destructor still finds it and destroys it -- this
        // function never leaks the object it created even when it fails
        // partway through). Returns whether `resident.view` ended up non-null.
        bool UploadArtifact(const LoadedClientArtifact& artifact, Resident& resident);

        // The shared 8x8 checkerboard for `space` -- created on the FIRST
        // PendingCook resolve in that space, reused (never recreated) by
        // every other pending key in it, until Release()/the destructor buries
        // it. Never returns a null texture/view once NRI itself is healthy;
        // on an NRI failure it reports once (the shared miss latch) and hands
        // back whatever partial state resulted (possibly all-null), which
        // Resolve then returns verbatim -- the same "degrade to no texture,
        // never crash" posture the rest of this cache already has.
        Resident& EnsureCheckerboard(ColorSpace space);

        // The shared one-shot diagnostic used by every failure path in this
        // cache, artifact-shaped or legacy -- see m_warnedMiss's own comment.
        void ReportIssue(const std::string& why);

        NriDevice*          m_device = nullptr;
        ResidencyAccount*   m_account = nullptr;   // the device's account (NriDevice::Residency()); not owned
        nri::HelperInterface m_helper{};
        PixelSupplyFn       m_supply;
        ArtifactSupplyFn    m_artifactSupply;
        CookPendingOracle   m_cookPendingOracle;   // F2b Task 12 -- see SetCookPendingOracle
        std::unordered_map<Key, Resident, KeyHash> m_textures;
        // Index 0 == ColorSpace::Srgb, index 1 == ColorSpace::Display -- see
        // EnsureCheckerboard. Both start empty (texture == nullptr); nothing
        // creates one until a PendingCook resolve actually needs it.
        std::array<Resident, 2> m_placeholders{};
        // THE ONE-SHOT MISS WARN (moved here from Batch2DNode): one line per
        // run, not one per span per frame. Shared across EVERY failure reason
        // this cache can report, legacy or artifact-shaped -- exactly the
        // existing legacy contract already extended to more reasons, not a
        // new latch per reason.
        bool m_warnedMiss = false;
    };
}
