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
// UPLOADS ARE DEFERRED (Plan C Task 5, user ruling 2026-11-06). Resolve of a mesh
// with no bytes on the GPU creates the vertex/index buffers (charged to the device
// account at once), QUEUES the upload and answers NOT-READY (null) -- exactly the
// answer a pending mesh already gets, so no consumer ever binds a not-ready pair.
// The graph's `upload` node (nodes/UploadNode.hpp), declared first in the frame,
// takes a SNAPSHOT of the queue (BeginUploadFrame), records the copies in its exec
// fn (RecordUpload -- ring / retry / dedicated staging, classified AFTER
// ring.BeginFrame), and the vehicle calls CommitUploads once the frame submitted:
// only then is an entry ready, so its first consumer is a LATER frame's
// declaration. Timeline: frame N queues, N+1 uploads, N+2 draws.
//
// The buffers are still not graph resources for DRAWING (draws bind them with no
// barrier); the upload frame imports them for its CopyDst write and a no-work
// `upload-settle` node reads them as VertexBuffer/IndexBuffer so the executor
// leaves them in the state the draws bind them in. The pre-Task-5 blocking path
// (UploadData inside Resolve) survives as NriUploadMode::Immediate, for tests only.

// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Nri/MeshResidencyBudget.hpp>
#include <Arcane/Render/Nri/NriUploadMode.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriUploadRing;
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
            // Created and charged, upload queued or in this frame's snapshot, NOT yet
            // recorded and submitted. Neither ready nor memoized-refused: Resolve
            // answers null for it, quietly. Cleared by CommitUploads (-> ready) or
            // by any drop path (eviction / Invalidate / Release cancel the upload).
            bool          queued = false;
        };

        // One entry of an `upload` node's snapshot: the buffers it imports and the
        // byte counts it copies. The CPU bytes stay in the entry (Resident::cpu).
        struct UploadJob
        {
            Guid          id;
            nri::Buffer*  vertexBuffer = nullptr;
            nri::Buffer*  indexBuffer  = nullptr;
            std::uint64_t vertexBytes  = 0;
            std::uint64_t indexBytes   = 0;
        };

        static std::unique_ptr<NriMeshBufferCache> Create(NriDevice& device);
        ~NriMeshBufferCache();
        NriMeshBufferCache(const NriMeshBufferCache&)            = delete;
        NriMeshBufferCache& operator=(const NriMeshBufferCache&) = delete;

        void SetMeshSupply(MeshSupplyFn supply) { m_supply = std::move(supply); }

        // Production is Deferred. Immediate is the old blocking path, TEST ONLY.
        void SetUploadMode(NriUploadMode mode) noexcept { m_mode = mode; }

        // ---- the upload node's seams (Plan C Task 5) --------------------------

        // True from the Resolve that queued an upload until one frame AFTER the
        // entry became ready (the first frame that could draw it has then been
        // recorded). The one-shot renderers' "is this picture final" question.
        [[nodiscard]] bool HasPendingUploads() const noexcept;
        // Entries queued and not yet in a snapshot.
        [[nodiscard]] std::size_t QueuedUploadCount() const noexcept { return m_queue.size(); }
        // The upload node's setup: moves the queue into the frame's snapshot and
        // returns it. Entries queued AFTER this call wait for the next frame.
        [[nodiscard]] std::vector<UploadJob> BeginUploadFrame();
        // The upload node's exec fn, after ring.BeginFrame: classifies the payload
        // (ring / retry / dedicated) and records the copies. Never calls CmdBarrier.
        void RecordUpload(const UploadJob& job, nri::CommandBuffer& cmd, NriUploadRing& ring);
        // After the frame: `submitted` makes every snapshot entry that is still
        // queued ready; false (Execute failed or the frame never ran) puts them back
        // in the queue. Either way, buffers the exec fn buried are handed to
        // `graveyard` at `fence` (this context's lane), and the cache's "settling"
        // flag is advanced. Call exactly once per frame attempt.
        void CommitUploads(bool submitted, Graveyard& graveyard, std::uint64_t fence);

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

        // Just the queue half of the three-argument EvictToBudget (no EndFrame): what
        // another context's boundary runs for THIS cache through
        // NriDevice::DrainOtherContexts, on this cache's own graveyard and fence.
        void DrainPending(Graveyard& graveyard, std::uint64_t fence);

        // Just the clock half: closes this cache's open frame without evicting anything.
        // The context's scope guard calls it on a Skipped/Failed frame.
        void CloseFrame();
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
        // Creates the vertex/index buffers into locals and publishes them (and the
        // entry's byte count) only on full success. Does NOT upload.
        bool CreateObjects(Resident& r, const Guid& id);
        // The old blocking path (NriUploadMode::Immediate): UploadData inside Resolve.
        bool UploadNow(Resident& r);
        // Drops an entry's upload bookkeeping (queue + snapshot membership).
        void CancelUpload(const Guid& id);
        // Memoizes a refusal for an entry whose objects exist: uncharges, parks the
        // buffers for burial, clears the CPU copy.
        void RefuseQueued(const Guid& id, Resident& r);
        void ParkBuffer(nri::Buffer* buffer);

        [[nodiscard]] static std::size_t SectionBytes(const MeshData& mesh) noexcept;

        NriDevice*           m_device = nullptr;
        nri::HelperInterface m_helper{};
        MeshSupplyFn         m_supply;
        std::unordered_map<Guid, Resident> m_entries;
        ResidencyAccount*    m_account = nullptr;   // the device's account (NriDevice::Residency()); not owned
        NriUploadMode        m_mode = NriUploadMode::Deferred;
        std::vector<Guid>    m_queue;       // created, charged, waiting for an upload node
        std::vector<Guid>    m_inflight;    // the current frame's snapshot, not yet committed
        // Destroy thunks the exec fn parked (retry-buried buffers, dedicated staging
        // buffers): they wait for CommitUploads, which knows this lane's fence.
        std::vector<std::function<void()>> m_parked;
        bool                 m_settling = false;   // an entry became ready last frame
        bool m_warnedOverBudget = false;
        bool m_warnedMiss       = false;
        // DebugFailNextUpload's own latch -- see that method's comment. Never read
        // outside Upload().
        std::optional<UploadStage> m_debugFailNextUpload;
    };
}
