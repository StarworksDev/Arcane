// NriMeshBufferCache -- see the header for what this owns, why the ring path
// for mesh geometry retires, and why these buffers are deliberately NOT graph
// resources.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "NriMeshBufferCache.hpp"

#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/ResidencyAccount.hpp>

#include <Arcane/Base/Log.hpp>

#undef ERROR

#include <string>
#include <vector>

namespace Arcane
{
    std::unique_ptr<NriMeshBufferCache> NriMeshBufferCache::Create(NriDevice& device)
    {
        std::unique_ptr<NriMeshBufferCache> cache(new NriMeshBufferCache());
        cache->m_device = &device;
        cache->m_account = &device.Residency();   // one account per device; created (and latched) by the first cache

        if (!ARC_NRI_CHECK(nriGetInterface(device.Device(), NRI_INTERFACE(nri::HelperInterface),
                                            &cache->m_helper)))
        {
            ARC_ERROR("[nri-graph] NriMeshBufferCache: HelperInterface unavailable -- no mesh can be "
                      "made resident on the graph device");
            return nullptr;
        }
        return cache;
    }

    NriMeshBufferCache::~NriMeshBufferCache()
    {
        if (m_account)
            m_account->UnchargeOwner(this);
        if (!m_device || m_entries.empty())
            return;

        bool any = false;
        for (const auto& [id, r] : m_entries)
            any = any || r.vertexBuffer || r.indexBuffer;
        if (!any)
            return;

        ARC_WARN("[nri-graph] NriMeshBufferCache destroyed with {} live NRI object set(s) -- its owner "
                 "never called Release(). Destroying directly behind a DeviceWaitIdle.",
                 m_entries.size());
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (auto& [id, r] : m_entries)
        {
            if (r.vertexBuffer) core.DestroyBuffer(r.vertexBuffer);
            if (r.indexBuffer)  core.DestroyBuffer(r.indexBuffer);
        }
        m_entries.clear();
    }

    // Buries the GPU half and zeroes the accounting. EVERY caller (Release,
    // Invalidate, EvictToBudget) erases the map node immediately afterwards -- this
    // never leaves a "cold" entry behind, because a cold entry is exactly the
    // uncounted CPU copy final-review I2 rules out. The field resets below are
    // therefore belt-and-braces on a Resident that is about to be destroyed.
    void NriMeshBufferCache::Bury(Resident& r, Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();
        if (r.vertexBuffer)
            graveyard.Bury(fence, [core, b = r.vertexBuffer] { core->DestroyBuffer(b); });
        if (r.indexBuffer)
            graveyard.Bury(fence, [core, b = r.indexBuffer] { core->DestroyBuffer(b); });
        r.vertexBuffer = nullptr;
        r.indexBuffer  = nullptr;
        r.ready        = false;
        r.bytes        = 0;
        r.indexCount   = 0;
        r.cpu          = MeshData{};
    }

    void NriMeshBufferCache::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        for (auto& [id, r] : m_entries)
            Bury(r, graveyard, fence);
        m_entries.clear();
        if (m_account)
            m_account->UnchargeOwner(this);
        // The warn latch stays SET across a release: a run that could not
        // resolve a mesh before a resize should not re-announce it after one.
    }

    void NriMeshBufferCache::Invalidate(const Guid& id, Graveyard& graveyard, std::uint64_t fence)
    {
        const auto it = m_entries.find(id);
        if (it == m_entries.end())
            return;
        Discard(it, graveyard, fence);
    }

    void NriMeshBufferCache::Discard(std::unordered_map<Guid, Resident>::iterator it,
                                     Graveyard& graveyard, std::uint64_t fence)
    {
        if (m_account)
            m_account->Uncharge(this, it->first, ResidencySlice::Mesh, NriTextureCache::ColorSpace::Srgb);
        Bury(it->second, graveyard, fence);
        m_entries.erase(it);
    }

    std::uint64_t NriMeshBufferCache::Budget() const noexcept
    {
        return m_account ? m_account->Caps().meshCap : 0u;
    }

    std::size_t NriMeshBufferCache::ResidentCount() const noexcept
    {
        std::size_t n = 0;
        for (const auto& [id, r] : m_entries)
            if (r.ready)
                ++n;
        return n;
    }

    std::uint64_t NriMeshBufferCache::ResidentBytes() const noexcept
    {
        std::uint64_t n = 0;
        for (const auto& [id, r] : m_entries)
            if (r.ready)
                n += r.bytes;
        return n;
    }

    std::size_t NriMeshBufferCache::RefusedCount() const noexcept
    {
        std::size_t n = 0;
        for (const auto& [id, r] : m_entries)
            if (r.uploadRefused)
                ++n;
        return n;
    }

    std::size_t NriMeshBufferCache::SectionBytes(const MeshData& mesh) noexcept
    {
        std::size_t n = mesh.sections.size() * sizeof(MeshSection);
        for (const MeshSection& s : mesh.sections)
            n += s.name.size();
        return n;
    }

    // CREATES INTO LOCALS AND PUBLISHES ONLY ON FULL SUCCESS (final-review I1).
    // The pre-fix shape passed r.vertexBuffer/r.indexBuffer straight to
    // CreateCommittedBuffer as out-parameters, which meant a second call on an entry
    // that already owned buffers OVERWROTE the live handles -- a leaked nri::Buffer
    // per call, forever, on any path that retried. Locals make that unrepresentable
    // rather than merely guarded: `r`'s handles are written once, at the end, when
    // both buffers exist AND the copy has landed; every failure destroys exactly what
    // this call created and leaves `r` with the null handles it came in with, so
    // Release/Invalidate find nothing dangling and nothing to double-free.
    bool NriMeshBufferCache::Upload(Resident& r, const Guid& id)
    {
        const nri::CoreInterface& core = m_device->Core();
        const std::uint64_t vertexBytes = r.cpu.vertices.size() * sizeof(MeshVertex);
        const std::uint64_t indexBytes  = r.cpu.indices.size() * sizeof(std::uint32_t);

        nri::Buffer* vb = nullptr;
        nri::Buffer* ib = nullptr;
        // `waitIdle` only on the UploadData arm: CreateCommittedBuffer failing means
        // nothing was ever submitted, while UploadData submits and waits internally
        // and may have failed after a partial submit -- the destructor's own
        // DeviceWaitIdle-then-destroy discipline, applied to this one cold path.
        const auto abandon = [&](bool waitIdle) -> bool
        {
            if (waitIdle)
                (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
            if (vb) core.DestroyBuffer(vb);
            if (ib) core.DestroyBuffer(ib);
            return false;
        };

        // DebugFailNextUpload's latch (debt 13): reset ONLY when this call's current
        // stage equals the currently-armed stage -- an unrelated real failure at a
        // DIFFERENT stage leaves the latch untouched, still armed, for whichever
        // Upload call next actually reaches the matching stage.
        const bool debugFailVertex = (m_debugFailNextUpload == UploadStage::Vertex);
        if (debugFailVertex)
            m_debugFailNextUpload.reset();

        nri::BufferDesc vbDesc = {};
        vbDesc.size  = vertexBytes;
        vbDesc.usage = nri::BufferUsageBits::VERTEX_BUFFER;
        if (debugFailVertex
            || !ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                       0.0f, vbDesc, vb))
            || !vb)
        {
            vb = nullptr;
            return abandon(false);
        }
        core.SetDebugName(vb, ("mesh vb " + id.ToString()).c_str());

        const bool debugFailIndex = (m_debugFailNextUpload == UploadStage::Index);
        if (debugFailIndex)
            m_debugFailNextUpload.reset();

        nri::BufferDesc ibDesc = {};
        ibDesc.size  = indexBytes;
        ibDesc.usage = nri::BufferUsageBits::INDEX_BUFFER;
        if (debugFailIndex
            || !ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                       0.0f, ibDesc, ib))
            || !ib)
        {
            ib = nullptr;
            return abandon(false);
        }
        core.SetDebugName(ib, ("mesh ib " + id.ToString()).c_str());

        nri::BufferUploadDesc uploads[2] = {};
        uploads[0].data   = r.cpu.vertices.data();
        uploads[0].buffer = vb;
        uploads[0].after  = { nri::AccessBits::VERTEX_BUFFER, nri::StageBits::VERTEX_SHADER };
        uploads[1].data   = r.cpu.indices.data();
        uploads[1].buffer = ib;
        uploads[1].after  = { nri::AccessBits::INDEX_BUFFER, nri::StageBits::INDEX_INPUT };

        if (!ARC_NRI_CHECK(m_helper.UploadData(*m_device->GraphicsQueue(), nullptr, 0, uploads, 2)))
            return abandon(true);

        r.vertexBuffer = vb;
        r.indexBuffer  = ib;
        r.indexCount = static_cast<std::uint32_t>(r.cpu.indices.size());
        r.bytes      = MeshResidencyBytes(static_cast<std::size_t>(vertexBytes),
                                          static_cast<std::size_t>(indexBytes),
                                          SectionBytes(r.cpu));
        r.ready      = true;
        return true;
    }

    const NriMeshBufferCache::Resident* NriMeshBufferCache::Resolve(const Guid& id,
                                                                    std::uint64_t frameCounter)
    {
        if (id.IsNil())
            return nullptr;

        if (auto it = m_entries.find(id); it != m_entries.end())
        {
            Resident& r = it->second;
            if (r.ready)
            {
                r.lastDrawnFrame = frameCounter;
                NoteDraw(id, r);
                return &r;
            }
            // NOT READY AND PRESENT == MEMOIZED, with no third case: a supply that
            // answered Failed, or an upload the device refused (Resident::
            // uploadRefused). There is deliberately no "cold CPU copy, re-upload
            // without the supply" arm any more -- EvictToBudget ERASES, so an
            // evicted guid is not found here at all and falls through to the miss
            // path below, where the supply is asked again (final-review I2).
            // Retrying a refusal here is precisely what leaked a buffer per frame
            // before final-review I1.
            return nullptr;
        }

        const auto reportMiss = [&](const std::string& why)
        {
            if (m_warnedMiss)
                return;
            m_warnedMiss = true;
            ARC_WARN("[nri-graph] NriMeshBufferCache: mesh {} is not resident on the graph device -- "
                     "{}; the draw is skipped (further occurrences are silent)",
                     id.ToString(), why);
        };

        if (!m_supply)
        {
            m_entries[id];   // memoize the miss
            reportMiss("no mesh supply is installed on this vehicle");
            return nullptr;
        }

        // Inserted BEFORE the supply consult so a Failed answer is attempted
        // once rather than every frame. PendingCook erases this entry below.
        Resident& resident = m_entries[id];
        const SupplyResult supplied = m_supply(id);
        if (supplied.state == MeshResolveState::PendingCook)
        {
            m_entries.erase(id);
            return nullptr;   // quiet, retryable -- not a miss
        }
        if (supplied.state != MeshResolveState::Ready || !supplied.mesh)
        {
            reportMiss("the asset resolved to nothing, or its mesh could not be decoded");
            return nullptr;   // empty entry stays -- memoized failure
        }
        if (supplied.mesh->vertices.empty() || supplied.mesh->indices.empty())
        {
            // Debt 12: the SAME memo as an Upload refusal below, taken before ever
            // reaching CreateCommittedBuffer -- a zero-size nri::Buffer is precisely
            // the shape of thing the device would refuse, so it counts in
            // RefusedCount() exactly like one.
            resident.uploadRefused = true;
            reportMiss("the mesh has no indices or no vertices -- a zero-size nri::Buffer is an NRI error, not a mesh");
            return nullptr;
        }

        resident.cpu = *supplied.mesh;
        if (!Upload(resident, id))
        {
            // MEMOIZED, EXPLICITLY, AND NEVER RETRIED (final-review I1): the flag is
            // what makes the refusal legible beside a Failed supply, and dropping the
            // CPU copy is what keeps ResidentBytes() honest -- an entry that will
            // never upload must not hold a mesh's worth of memory the budget cannot
            // see. Upload left no GPU objects behind, so there is nothing to bury.
            // Invalidate(id) is the only un-latch, exactly as for a Failed supply.
            resident.uploadRefused = true;
            resident.cpu = MeshData{};
            reportMiss("CreateCommittedBuffer or UploadData failed");
            return nullptr;
        }
        resident.lastDrawnFrame = frameCounter;
        if (m_account)
            (void)m_account->Charge(this, ResidencyEntry{ id, ResidencySlice::Mesh,
                                                         NriTextureCache::ColorSpace::Srgb, resident.bytes, 0 });
        return &resident;
    }

    // A draw on the device clock. A key the account no longer holds for this cache (it
    // queued the entry for eviction, and the drop has not run yet) is charged again:
    // it was just drawn, so it is hot after all.
    void NriMeshBufferCache::NoteDraw(const Guid& id, const Resident& r)
    {
        if (!m_account)
            return;
        if (!m_account->Touch(this, id, ResidencySlice::Mesh, NriTextureCache::ColorSpace::Srgb))
            (void)m_account->Charge(this, ResidencyEntry{ id, ResidencySlice::Mesh,
                                                         NriTextureCache::ColorSpace::Srgb, r.bytes, 0 });
    }

    void NriMeshBufferCache::CloseFrame()
    {
        if (m_account)
            m_account->EndFrame(this);
    }

    void NriMeshBufferCache::EvictToBudget(std::uint64_t frameCounter, Graveyard& graveyard,
                                           std::uint64_t fence)
    {
        (void)frameCounter;   // the device account keeps its own clock (R2); see ResidencyAccount.hpp
        if (!m_account)
            return;
        DrainPending(graveyard, fence);
        m_account->EndFrame(this);
    }

    void NriMeshBufferCache::DrainPending(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_account)
            return;
        for (const ResidencyEviction& ev : m_account->TakePending(this))
        {
            if (ev.slice != ResidencySlice::Mesh)
                continue;
            const auto it = m_entries.find(ev.id);
            if (it != m_entries.end())
                Discard(it, graveyard, fence);
        }
    }

    void NriMeshBufferCache::EvictToBudget(std::uint64_t frameCounter, Graveyard& graveyard,
                                           std::uint64_t fence, std::uint64_t budget)
    {
        std::vector<MeshResidencyEntry> live;
        live.reserve(m_entries.size());
        for (const auto& [id, r] : m_entries)
        {
            if (!r.ready)
                continue;
            live.push_back(MeshResidencyEntry{ id, r.bytes, r.lastDrawnFrame });
        }

        const std::vector<Guid> drop = SelectEvictions(live, budget, frameCounter);
        for (const Guid& id : drop)
        {
            const auto it = m_entries.find(id);
            if (it == m_entries.end())
                continue;
            // THE WHOLE ENTRY GOES, CPU copy included (final-review I2, ruled; plan
            // Task 2 Step 6). Burying only the GPU half and keeping the map node
            // would leave the CPU geometry resident and UNCOUNTED -- ResidentBytes()
            // would report a number the process was not honouring. The next Resolve
            // of this guid is a clean miss that re-asks the supply, which in
            // production is SceneRenderResolver's in-memory MeshTable.
            Discard(it, graveyard, fence);
        }

        if (ResidentBytes() > budget && !m_warnedOverBudget)
        {
            m_warnedOverBudget = true;
            ARC_WARN("[nri-graph] NriMeshBufferCache: protected set is {} bytes over the {}-byte "
                     "budget -- the in-flight frame is kept intact (further occurrences are silent)",
                     ResidentBytes(), budget);
        }
    }
}
