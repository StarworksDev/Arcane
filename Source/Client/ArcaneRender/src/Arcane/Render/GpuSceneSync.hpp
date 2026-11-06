#pragma once

// GpuSceneSync -- the mirror's per-frame reconciliation (F3, spec s5.3 as
// amended by the UE vet: CPU history + re-dirty, no compute), and
// BuildGpuSceneFrame -- the batch table, the indirect args and (plan 1) the
// CPU-written visible-index list (plan 2's MeshCullNode takes over that loop).
//
// Host order per frame: schedulers (TransformPropagation -> Bounds) ->
// BuildVisibleSet -> GpuSceneSync -> BuildGpuSceneFrame -> RenderFrame.
// Sync ADVANCES THE TICK after recording lastSyncTick (TransformPropagation
// System's contract): a write made later in the same tick must compare
// strictly newer next frame. It runs on the host thread with no system in
// flight.
//
// THE G2 CONTRACT (spec s5.3): a row's prevModel is the model it was drawn
// with on the previous frame; a new row, a full rebuild, or a teleported row
// has prev == model. Implemented as UE's FSceneVelocityData: `lastModel` per
// row is the matrix last uploaded; a row uploaded with prev != model is
// re-dirtied next frame so it lands at rest.
#include <Arcane/Math/NormalMatrix.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Arcane
{
    namespace detail
    {
        inline std::uint32_t BatchIdFor(GpuSceneMirror& m, const GpuBatchKey& key)
        {
            if (auto it = m.batchIds.find(key); it != m.batchIds.end())
                return it->second;
            const std::uint32_t id = static_cast<std::uint32_t>(m.batchKeys.size());
            m.batchIds.emplace(key, id);
            m.batchKeys.push_back(key);
            m.batchRowCount.push_back(0);
            return id;
        }

        inline void FreeEntityRows(GpuSceneMirror& m, Astra::Entity e, const GpuSceneMirror::Rows& r,
                                   std::vector<std::uint32_t>& freedRows)
        {
            for (std::uint32_t i = 0; i < r.count; ++i)
            {
                const std::uint32_t rowIndex = r.first + i;
                GpuSceneRow& row = m.rows[rowIndex];
                if (row.live)
                    --m.batchRowCount[row.batch];
                row = GpuSceneRow{};
                freedRows.push_back(rowIndex);
            }
            m.allocator.Free(r.first, r.count);
            m.slots.Erase(e);
        }

        // The material chain (the retired CollectMeshInstances' rule, verbatim):
        // override, if it resolves, wins; else the section slot's material; else white.
        struct RowMaterial
        {
            glm::vec4         baseColor{1.0f};
            std::uint32_t     slot = kGpuInvalidMaterialSlot;
            MaterialBlendMode blend = MaterialBlendMode::Opaque;
            float             alphaCutoff = 0.5f;
            bool              twoSided = false;
        };
        inline RowMaterial ResolveRowMaterial(const MeshMaterialTable* mats, const MeshEntry& entry,
                                              const MeshSection& section, const Guid& override)
        {
            const ResolvedMeshMaterial* mat = mats ? mats->Resolve(override) : nullptr;
            if (!mat)
            {
                Guid slotMat{};
                if (section.slotIndex < entry.slots.size())
                    slotMat = entry.slots[section.slotIndex].material;
                mat = mats ? mats->Resolve(slotMat) : nullptr;
            }
            return mat ? RowMaterial{ mat->baseColor, mat->materialSlot, mat->blend, mat->alphaCutoff, mat->twoSided }
                       : RowMaterial{};
        }
    }

    inline void GpuSceneSync(Astra::Registry& reg, GpuSceneMirror& m,
                             std::uint64_t deviceSyncedGeneration, GpuSceneStage& out)
    {
        out.Clear();
        const MeshTable*         meshes = reg.GetResource<MeshTable>();
        const MeshMaterialTable* mats   = reg.GetResource<MeshMaterialTable>();
        const bool full = (deviceSyncedGeneration != m.generation);
        ++m.syncCounter;

        std::vector<std::uint8_t> dirty(m.rows.size(), 0);
        std::vector<std::uint32_t> freedRows;
        auto markDirty = [&](std::uint32_t row)
        {
            if (row >= dirty.size()) dirty.resize(row + 1, 0);
            dirty[row] = 1;
        };

        // 1. Reconcile: every drawable mesh entity owns a span; a changed mesh or
        //    section count reallocates; everything else is left alone.
        reg.CreateView<const WorldTransform, const WorldBounds, const MeshRenderer, Astra::Not<Hidden>>().ForEach(
            [&](Astra::Entity e, const WorldTransform&, const WorldBounds&, const MeshRenderer& mr)
            {
                const MeshEntry* entry = meshes ? meshes->Resolve(mr.mesh) : nullptr;
                const std::uint32_t sections = entry ? static_cast<std::uint32_t>(entry->data.sections.size()) : 0;
                GpuSceneMirror::Rows* r = m.slots.TryGet(e);
                if (sections == 0)
                {
                    if (r) detail::FreeEntityRows(m, e, *r, freedRows);
                    return;
                }
                if (r && (r->count != sections || m.rows[r->first].mesh != mr.mesh))
                {
                    detail::FreeEntityRows(m, e, *r, freedRows);
                    r = nullptr;
                }
                if (!r)
                {
                    const std::uint32_t first = m.allocator.Allocate(sections);
                    if (m.rows.size() < m.allocator.HighWater())
                        m.rows.resize(m.allocator.HighWater());
                    for (std::uint32_t s = 0; s < sections; ++s)
                    {
                        GpuSceneRow& row = m.rows[first + s];
                        row.entity  = e;
                        row.mesh    = mr.mesh;
                        row.section = s;
                        row.batch   = detail::BatchIdFor(m, GpuBatchKey{ mr.mesh, s, MaterialBlendMode::Opaque, false });
                        row.live    = true;
                        row.lastModel = glm::mat4(0.0f);   // "never uploaded": step 3 sets prev = model
                        ++m.batchRowCount[row.batch];
                        markDirty(first + s);
                    }
                    r = &m.slots[e];
                    *r = GpuSceneMirror::Rows{ first, sections };
                }
                for (std::uint32_t s = 0; s < r->count; ++s)
                    m.rows[r->first + s].touched = m.syncCounter;
            });

        // Removal: spans whose entity the walk did not see (destroyed, Hidden,
        // lost its renderer / transform / bounds).
        {
            std::vector<Astra::Entity> victims;
            for (const auto& kv : m.slots)
                if (m.rows[kv.second.first].touched != m.syncCounter)
                    victims.push_back(kv.first);
            for (Astra::Entity e : victims)
                if (const GpuSceneMirror::Rows* r = m.slots.TryGet(e))
                    detail::FreeEntityRows(m, e, *r, freedRows);
        }

        // Dispatch spans allocator high water, not only live mirror rows. A
        // freed row that remains a hole therefore needs an explicit inactive
        // device value. Defer until reconciliation is complete so a row freed
        // and reused in the same sync receives only its new live upload (two
        // transfer writes to the same bytes would otherwise need a WAW barrier).
        std::vector<std::uint8_t> tombstoned(m.rows.size(), 0);
        const auto stageTombstone = [&](std::uint32_t row)
        {
            if (row >= m.allocator.HighWater() || m.rows[row].live || tombstoned[row])
                return;
            GpuInstance inactive;
            inactive.batch = std::numeric_limits<std::uint32_t>::max();
            inactive.flags = 0;
            out.rows.push_back(row);
            out.values.push_back(inactive);
            tombstoned[row] = 1;
        };
        for (std::uint32_t row : freedRows)
            stageTombstone(row);

        // 2. Dirty: moved (exact -- WorldTransform is change-tracked), a component
        //    write on MeshRenderer, a re-boxed row (WorldBounds is change-tracked:
        //    BoundsSystem rewrote it because the MESH ASSET's bounds changed under
        //    an unmoved entity -- MeshTable::generation -- and the row's
        //    boundsMin/Max must follow; it ran earlier this host frame, so its
        //    write is strictly newer than the previous Sync's lastSyncTick), the
        //    re-dirty list, or every live row on a rebuild.
        if (full)
        {
            for (std::uint32_t row = 0; row < m.rows.size(); ++row)
                if (m.rows[row].live) markDirty(row);
                else stageTombstone(row);
        }
        else
        {
            auto markEntity = [&](Astra::Entity e)
            {
                if (const GpuSceneMirror::Rows* r = m.slots.TryGet(e))
                    for (std::uint32_t s = 0; s < r->count; ++s) markDirty(r->first + s);
            };
            reg.CreateView<const WorldTransform, Astra::Changed<WorldTransform>>().Since(m.lastSyncTick)
                .ForEach([&](Astra::Entity e, const WorldTransform&) { markEntity(e); });
            reg.CreateView<const MeshRenderer, Astra::Changed<MeshRenderer>>().Since(m.lastSyncTick)
                .ForEach([&](Astra::Entity e, const MeshRenderer&) { markEntity(e); });
            reg.CreateView<const WorldBounds, Astra::Changed<WorldBounds>>().Since(m.lastSyncTick)
                .ForEach([&](Astra::Entity e, const WorldBounds&) { markEntity(e); });
            for (std::uint32_t row : m.dirtyLastFrame)
                if (row < m.rows.size() && m.rows[row].live) markDirty(row);
        }
        m.dirtyLastFrame.clear();

        // 3. Re-key then stage. A material re-resolve per live row is the same
        //    hash lookup the retired per-frame sweep did. Blend and two-sided
        //    state define batch membership, so a material-only change must move
        //    the row before its device value is staged -- flags alone are not a
        //    substitute for that membership update.
        for (std::uint32_t row = 0; row < m.rows.size(); ++row)
        {
            GpuSceneRow& r = m.rows[row];
            if (!r.live) continue;
            const MeshEntry* entry = meshes ? meshes->Resolve(r.mesh) : nullptr;
            if (!entry || r.section >= entry->data.sections.size()) continue;   // reconciled away next frame
            const MeshRenderer* mr = std::as_const(reg).GetComponent<MeshRenderer>(r.entity);
            const WorldTransform* wt = std::as_const(reg).GetComponent<WorldTransform>(r.entity);
            const WorldBounds* wb = std::as_const(reg).GetComponent<WorldBounds>(r.entity);
            if (!mr || !wt || !wb) continue;
            const detail::RowMaterial mat =
                detail::ResolveRowMaterial(mats, *entry, entry->data.sections[r.section], mr->materialOverride);
            const GpuBatchKey key{ r.mesh, r.section, mat.blend, mat.twoSided };
            bool rekeyed = false;
            if (m.batchKeys[r.batch] != key)
            {
                --m.batchRowCount[r.batch];
                r.batch = detail::BatchIdFor(m, key);
                ++m.batchRowCount[r.batch];
                rekeyed = true;
            }
            const bool isDirty = (row < dirty.size() && dirty[row])
                              || rekeyed
                              || mat.baseColor != r.lastBaseColor || mat.slot != r.lastSlot
                              || mat.blend != r.lastBlend || mat.alphaCutoff != r.lastAlphaCutoff
                              || mat.twoSided != r.lastTwoSided;
            if (!isDirty) continue;

            const bool neverUploaded = (r.lastModel == glm::mat4(0.0f));
            GpuInstance v;
            v.model     = wt->matrix;
            v.prevModel = (full || neverUploaded) ? v.model : r.lastModel;
            const glm::mat3 n = NormalMatrixFor(v.model);
            v.normal0 = glm::vec4(n[0], 0.0f);
            v.normal1 = glm::vec4(n[1], 0.0f);
            v.normal2 = glm::vec4(n[2], 0.0f);
            v.boundsMin    = glm::vec4(wb->box.min, 0.0f);
            v.boundsMax    = glm::vec4(wb->box.max, mat.alphaCutoff);
            v.baseColor    = mat.baseColor;
            v.materialSlot = mat.slot;
            v.batch        = r.batch;
            v.flags        = kGpuInstanceFlagLive
                           | (static_cast<std::uint32_t>(mat.blend) << kGpuInstanceFlagBlendShift);
            if (mat.twoSided) v.flags |= kGpuInstanceFlagTwoSided;
            if (v.prevModel != v.model)
                m.dirtyLastFrame.push_back(row);   // settle it next frame (the re-dirty)
            r.lastModel     = v.model;
            r.lastBaseColor = v.baseColor;
            r.lastSlot      = v.materialSlot;
            r.lastBlend = mat.blend;
            r.lastAlphaCutoff = mat.alphaCutoff;
            r.lastTwoSided = mat.twoSided;
            r.boundsCenter = (wb->box.min + wb->box.max) * 0.5f;
            r.renderOrder = mr->translucencyRenderOrder;
            r.depthSortBias = mr->translucencyDepthSortBias;
            out.rows.push_back(row);
            out.values.push_back(v);
        }

        out.rowCapacity = m.allocator.HighWater();
        out.fullRebuild = full;
        out.generation  = m.generation;
        m.lastSyncTick  = reg.CurrentTick();
        reg.AdvanceTick();
    }

    // One mesh section's index range, copied out of MeshTable so the frame
    // builder does not keep the table. Inline, like every function here.
    struct MeshSectionRange
    {
        Guid mesh{};
        std::uint32_t section = 0;
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount = 0;
    };

    inline std::vector<MeshSectionRange> MeshSectionRanges(const MeshTable* table)
    {
        std::vector<MeshSectionRange> out;
        if (!table || !table->meshes)
            return out;
        for (const auto& [guid, entry] : *table->meshes)
        {
            std::uint32_t section = 0;
            for (const MeshSection& s : entry.data.sections)
            {
                out.push_back(MeshSectionRange{ guid, section, s.indexOffset, s.indexCount });
                ++section;
            }
        }
        return out;
    }

    inline void BuildGpuSceneFrame(const GpuSceneMirror& m, const VisibleSet* vis,
                                   std::span<const MeshSectionRange> sections,
                                   const ViewTransform& view, GpuSceneFrame& out)
    {
        out.batches.clear();
        out.args.clear();
        out.cullBatches.clear();
        out.transparentDraws.clear();
        out.rowCount = m.allocator.HighWater();
        out.visibleIndices.assign(out.rowCount, 0xFFFFFFFFu);
        out.oracleVisibleIndices.assign(out.rowCount, 0xFFFFFFFFu);
        out.frustum = vis ? vis->frustum : Frustum::From(view).Widened(VisibilitySlackMeters());
        out.stats   = {};

        const std::size_t nb = m.batchKeys.size();
        std::vector<std::uint32_t> firstOutput(nb, 0), cursor(nb, 0);
        std::vector<float> nearDepth(nb, std::numeric_limits<float>::infinity());
        std::uint32_t prefix = 0;
        for (std::size_t b = 0; b < nb; ++b) { firstOutput[b] = prefix; prefix += m.batchRowCount[b]; }
        out.cullBatches.resize(nb);
        for (std::size_t b = 0; b < nb; ++b)
            out.cullBatches[b] = GpuCullBatch{ firstOutput[b], m.batchRowCount[b], 0u, 0u };

        // The coarse pass: every live row whose entity is a member (or every
        // row, no set). Indirect storage is device-owned from this point on:
        // retain CPU answers in oracleVisibleIndices, while transparent rows
        // bypass both lists entirely and become complete direct draw records.
        for (std::uint32_t row = 0; row < m.rows.size(); ++row)
        {
            const GpuSceneRow& r = m.rows[row];
            if (!r.live) continue;
            ++out.stats.total;
            if (vis && !vis->Contains(r.entity)) continue;
            ++out.stats.coarseVisible;
            const GpuBatchKey& key = m.batchKeys[r.batch];
            if (key.blend == MaterialBlendMode::Transparent)
            {
                const MeshSectionRange* section = nullptr;
                for (const MeshSectionRange& candidate : sections)
                    if (candidate.mesh == key.mesh && candidate.section == key.section)
                        section = &candidate;
                if (!section)
                    continue;
                const glm::vec3 viewCenter = glm::vec3(view.view * glm::vec4(r.boundsCenter, 1.0f));
                out.transparentDraws.push_back(TransparentDraw{
                    row, key.mesh, key.section, section->indexOffset, section->indexCount,
                    key.blend, key.twoSided, r.renderOrder,
                    -viewCenter.z + r.depthSortBias, r.entity,
                });
                continue;
            }
            out.oracleVisibleIndices[firstOutput[r.batch] + cursor[r.batch]++] = row;
        }
        // nearDepth per batch = the minimum over its visible entities' VisibleEntry::nearDepth.
        if (vis)
        {
            for (const VisibleEntry& e : vis->entries)
                if (const GpuSceneMirror::Rows* r = m.slots.TryGet(e.entity))
                    for (std::uint32_t s = 0; s < r->count; ++s)
                    {
                        const std::uint32_t b = m.rows[r->first + s].batch;
                        if (m.batchKeys[b].blend != MaterialBlendMode::Transparent)
                            nearDepth[b] = std::min(nearDepth[b], e.nearDepth);
                    }
        }
        else
            std::fill(nearDepth.begin(), nearDepth.end(), 0.0f);

        // Emit: batches with >= 1 visible row, opaque before masked (blend asc), nearest first.
        std::vector<std::uint32_t> emitted;
        for (std::uint32_t b = 0; b < nb; ++b)
            if (cursor[b] > 0 && m.batchKeys[b].blend != MaterialBlendMode::Transparent) emitted.push_back(b);
        std::stable_sort(emitted.begin(), emitted.end(), [&](std::uint32_t a, std::uint32_t b)
        {
            if (m.batchKeys[a].blend != m.batchKeys[b].blend) return m.batchKeys[a].blend < m.batchKeys[b].blend;
            return nearDepth[a] < nearDepth[b];
        });
        for (std::uint32_t b : emitted)
        {
            const GpuBatchKey& key = m.batchKeys[b];
            const MeshSectionRange* section = nullptr;
            for (const MeshSectionRange& candidate : sections)
                if (candidate.mesh == key.mesh && candidate.section == key.section)
                    section = &candidate;
            if (!section)
                continue;
            // The section's range VERBATIM (the retired CollectMeshInstances rule):
            // MeshData's contract (Mesh/MeshBuilder.hpp) has no "empty section
            // draws everything" fallback, and inventing one here would read past
            // the index buffer for a zero-count section with a non-zero offset.
            const std::uint32_t indexCount = section->indexCount;
            GpuBatchDraw d;
            d.mesh = key.mesh; d.section = key.section;
            d.indexOffset = section->indexOffset; d.indexCount = indexCount;
            d.firstOutput = firstOutput[b]; d.capacity = m.batchRowCount[b];
            d.argIndex = static_cast<std::uint32_t>(out.args.size());
            d.blend = key.blend; d.twoSided = key.twoSided; d.nearDepth = nearDepth[b];
            out.batches.push_back(d);
            out.args.push_back(DrawIndexedArgs{ indexCount, 0, section->indexOffset, 0, 0 });
            out.cullBatches[b].argIndex = d.argIndex;
            out.cullBatches[b].emitted  = 1u;
        }
        std::sort(out.transparentDraws.begin(), out.transparentDraws.end(), [](const TransparentDraw& a, const TransparentDraw& b)
        {
            if (a.renderOrder != b.renderOrder) return a.renderOrder < b.renderOrder;
            if (a.projectedDepth != b.projectedDepth) return a.projectedDepth > b.projectedDepth;
            if (a.entity.GetID() != b.entity.GetID()) return a.entity.GetID() < b.entity.GetID();
            if (a.mesh.hi != b.mesh.hi) return a.mesh.hi < b.mesh.hi;
            if (a.mesh.lo != b.mesh.lo) return a.mesh.lo < b.mesh.lo;
            return a.section < b.section;
        });
        out.stats.batches = static_cast<std::uint32_t>(out.batches.size());
        out.stats.draws   = out.stats.batches + static_cast<std::uint32_t>(out.transparentDraws.size());
    }
}
