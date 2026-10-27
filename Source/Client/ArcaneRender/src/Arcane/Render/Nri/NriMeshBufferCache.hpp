#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// NriMeshBufferCache -- ONE Guid -> resident vertex/index buffer pair for the whole
// render path, and the retirement of the per-frame upload ring for mesh geometry.
//
// WHY IT EXISTS (research s3.4-1): MeshNode::Record uploaded every visible mesh's
// streams through the frame's TRANSIENT ring EVERY FRAME -- fine for unit
// primitives, ~3 MB per frame per mesh for a 100k-triangle import. R2's ruling is
// one cache owning CPU+GPU lifetime for ALL meshes, primitives included: the ring
// path for mesh geometry retires outright rather than being kept as a small-mesh
// fast path, because two upload paths is two lifetimes to reason about and the
// resident one is correct for both sizes.
//
// ARCHITECTURE MIRRORS NriTextureCache, deliberately and point for point: an
// injected supply (this class owns no file IO and no Assets knowledge -- a RENDER
// object must not grow a Runtime), failures memoized exactly once, a one-shot WARN
// latch, Release(graveyard, fence) as the sanctioned teardown with the destructor
// as a SAFETY NET that says so at WARN, and Invalidate(id, graveyard, fence) as the
// per-guid escape hatch. Read NriTextureCache.hpp's own banner first; every rule
// there applies here unless this comment says otherwise.
//
// WHAT IS DIFFERENT FROM NriTextureCache, and why:
//   * A BYTE BUDGET AND AN LRU (MeshResidencyBudget.hpp). NriTextureCache has
//     neither -- it never evicts a single entry (NriGraphContext's own note says
//     so). Mesh geometry is where eviction became a real question (F2a's spec
//     assigned it here by name). EVICTION ERASES THE WHOLE ENTRY, the CPU copy
//     with it (final-review I2, ruled), so ResidentBytes() is exactly the
//     resident CPU+GPU bytes and a cold entry can never hide memory behind a
//     `ready == false`. The CPU copy that a re-upload reads after an eviction
//     is NOT kept here -- it lives in the SUPPLY (SceneRenderResolver's
//     in-memory MeshTable), so the next Resolve asks the supply again and that
//     ask is a table lookup, never an artifact read.
//   * NO COLOUR SPACE. A vertex buffer has no sRGB question, so the key is a bare
//     Guid rather than (Guid, space).
//   * NO PLACEHOLDER. A pending texture gets a checkerboard, which is a real,
//     visible, honest stand-in. There is no honest stand-in for a shape (s7.1:
//     "a placeholder cube would fabricate a shape"), so a pending mesh resolves to
//     NOTHING and the draw is skipped -- quietly while pending, loudly when
//     missing or refused.
//
// NO BARRIERS, and none needed -- uploads go through nri::HelperInterface::
// UploadData, which submits and waits internally, so these buffers are NOT graph
// resources. RESOLVE AT DECLARATION TIME ONLY, never inside a node's exec fn: the
// same rule, and the same reason, NriTextureCache states in full.

// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Nri/MeshResidencyBudget.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class ResidencyAccount;

    class ARC_RENDER_API NriMeshBufferCache
    {
    public:
        // Guid -> resolved CPU geometry, or a state saying why not. In production this
        // is the frame driver's forward to SceneRenderResolver's MeshTable (already
        // populated by the per-frame Request sweep); the returned pointer is READ
        // INSIDE the call and never stored.
        struct SupplyResult
        {
            const MeshData*    mesh  = nullptr;   // null unless state == Ready
            MeshResolveState   state = MeshResolveState::Failed;
        };
        using MeshSupplyFn = std::function<SupplyResult(const Guid&)>;

        struct Resident
        {
            nri::Buffer*  vertexBuffer = nullptr;
            nri::Buffer*  indexBuffer  = nullptr;
            std::uint32_t indexCount   = 0;
            std::uint64_t bytes        = 0;
            std::uint64_t lastDrawnFrame = 0;
            // The CPU copy of a RESIDENT mesh, counted in `bytes` alongside the two
            // GPU buffers (MeshResidencyBudget.hpp). It does NOT outlive the entry:
            // eviction erases the whole entry, and a refused upload clears this
            // before memoizing, so `cpu` is non-empty if and only if `ready`.
            MeshData      cpu;
            bool          ready = false;   // upload succeeded; GPU objects are bindable
            // Why this entry is not ready, when it is not. An NRI failure (a refused
            // CreateCommittedBuffer or UploadData) is MEMOIZED and never retried --
            // retrying it per frame is what leaked a buffer a frame before
            // final-review I1. A supply that answered Failed memoizes as an empty
            // entry with this flag CLEAR; the two are distinguished here so a reader
            // (and a test) can tell "the device refused it" from "the asset is
            // broken". Only Invalidate/Release clear either memo. Read by
            // RefusedCount() (debt 12) -- the zero-size refusal (an empty mesh) sets
            // this too, because it is memoized the same way and for the same reason
            // as a real CreateCommittedBuffer/UploadData refusal, even though it
            // never reaches either call.
            bool          uploadRefused = false;
        };

        static std::unique_ptr<NriMeshBufferCache> Create(NriDevice& device);
        ~NriMeshBufferCache();
        NriMeshBufferCache(const NriMeshBufferCache&)            = delete;
        NriMeshBufferCache& operator=(const NriMeshBufferCache&) = delete;

        void SetMeshSupply(MeshSupplyFn supply) { m_supply = std::move(supply); }

        // The resident pair for `id`, uploading on first sight, and stamping
        // `lastDrawnFrame = frameCounter` on every HIT -- which is what makes the LRU
        // mean "least recently DRAWN" rather than "least recently asked about".
        // Null while the mesh is PendingCook (quiet -- retried on the next call, no
        // memo, because a cook queue is expected to promote it) and null-and-memoized
        // when the supply Failed or the upload was refused.
        [[nodiscard]] const Resident* Resolve(const Guid& id, std::uint64_t frameCounter);

        // Frame-boundary eviction (s7.2). Called ONCE per frame by the vehicle, AFTER
        // recording and BEFORE the next frame's declarations, so nothing evicted here
        // can be named by a command buffer still being built. Buries evicted buffers at
        // `fence`, so an ALREADY-RECORDED frame still reading them is never invalidated
        // out from under it -- the same discipline NriTextureCache::Invalidate keeps.
        //
        // THE THREE-ARGUMENT FORM CARRIES OUT THE DEVICE ACCOUNT'S EVICTIONS for this
        // cache (ResidencyAccount.hpp): Resolve charges the account at upload, the
        // account decides who goes (it sees every vehicle's meshes and textures), and
        // this call drops the entries it queued for THIS cache, on this cache's own
        // lane. It also ends this cache's frame on the device clock. The budget is the
        // device account, not a per-cache number.
        //
        // ERASES the evicted entries outright, CPU copy included -- see the banner and
        // MeshResidencyBudget.hpp. The next Resolve of an evicted guid is therefore a
        // full MISS: the supply is asked again (an in-memory MeshTable lookup in
        // production, not a disk read) and the mesh re-uploads.
        //
        // The four-argument form evicts this cache ALONE against an explicit `budget`
        // (SelectEvictions); tests use it so a handful of cubes can exercise the LRU
        // without filling the account.
        void EvictToBudget(std::uint64_t frameCounter, Graveyard& graveyard, std::uint64_t fence);
        void EvictToBudget(std::uint64_t frameCounter, Graveyard& graveyard,
                           std::uint64_t fence, std::uint64_t budget);

        // The mesh cap of the device account this cache charges (latched when the
        // account was created, Restart).
        [[nodiscard]] std::uint64_t Budget() const noexcept;

        // Drops residency for `id` so the NEXT Resolve treats it as brand new --
        // Resident, Pending or Refused, whichever it lands in. The escape hatch a
        // cook-completion callback needs (s7.3), and the un-latch a Refused key would
        // otherwise never get.
        void Invalidate(const Guid& id, Graveyard& graveyard, std::uint64_t fence);

        // Buries every NRI object this cache owns at `fence` and empties it.
        // Idempotent. THE DEVICE-LOSS / RECREATE HOOK TOO: a recreated device's
        // buffers are new objects, so every entry goes and the next Resolve is a
        // clean miss that re-asks the supply -- which, in production, is
        // SceneRenderResolver's in-memory MeshTable, so the re-upload still costs a
        // table lookup rather than an artifact read.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Entries that are READY: uploaded, bindable, drawable. Nothing else is ever
        // counted, and nothing else ever holds bytes -- see Resident::cpu.
        [[nodiscard]] std::size_t   ResidentCount() const noexcept;
        // EXACTLY the resident CPU+GPU bytes -- the number the budget bounds, with no
        // cold CPU copy hiding behind it (final-review I2).
        [[nodiscard]] std::uint64_t ResidentBytes() const noexcept;
        // Entries the DEVICE (or a zero-size mesh, the same memo -- see
        // Resident::uploadRefused) REFUSED: distinct from a Failed supply, which
        // memoizes with this flag clear. Debt 12's reader for a flag that was
        // write-only until now.
        [[nodiscard]] std::size_t   RefusedCount() const noexcept;

        // TEST INSTRUMENT ONLY (debt 13) -- never called by production code. There
        // is no honest way to make a REAL device refuse only the SECOND
        // CreateCommittedBuffer inside Upload (an over-limit index buffer needs a
        // multi-GB CPU vector to reach it), so this forces the next Upload's create
        // call at `stage` to fail as if the device had refused it -- exercising
        // Upload's abandon() arm for real instead of by inspection. One-shot and
        // latched: armed here, consumed by the next Upload call that REACHES that
        // stage (not merely the next Upload call -- an earlier stage failing for a
        // real reason leaves this armed for whichever Upload actually gets there),
        // then cleared either way.
        enum class UploadStage { Vertex, Index };
        void DebugFailNextUpload(UploadStage stage) noexcept { m_debugFailNextUpload = stage; }

    private:
        NriMeshBufferCache() = default;

        void Bury(Resident& r, Graveyard& graveyard, std::uint64_t fence);
        // Uncharges the account, buries, and erases the map node -- the one drop path.
        void Discard(std::unordered_map<Guid, Resident>::iterator it, Graveyard& graveyard, std::uint64_t fence);
        void NoteDraw(const Guid& id, const Resident& r);
        bool Upload(Resident& r, const Guid& id);

        [[nodiscard]] static std::size_t SectionBytes(const MeshData& mesh) noexcept;

        NriDevice*           m_device = nullptr;
        nri::HelperInterface m_helper{};
        MeshSupplyFn         m_supply;
        std::unordered_map<Guid, Resident> m_entries;
        ResidencyAccount*    m_account = nullptr;   // the device's account (NriDevice::Residency()); not owned
        bool m_warnedOverBudget = false;
        bool m_warnedMiss       = false;
        // DebugFailNextUpload's own latch -- see that method's comment. Never read
        // outside Upload().
        std::optional<UploadStage> m_debugFailNextUpload;
    };
}
