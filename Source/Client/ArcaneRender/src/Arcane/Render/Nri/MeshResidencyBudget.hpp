#pragma once

// MeshResidencyBudget -- the POLICY half of NriMeshBufferCache (spec s7.2 / R2),
// split out DEVICE-FREE so the eviction rule is provable without an NRI device at
// all. Same split NriTextureCache::Bc7RowPitch keeps for the same reason: the
// arithmetic that decides what happens is testable independently of the API that
// carries it out.
//
// ONE COMBINED CPU+GPU BYTE BUDGET, AND AN HONEST ONE. A resident entry costs its
// GPU buffers PLUS the CPU copy the cache holds beside them, so both halves are
// COUNTED -- a budget that ignored the CPU half would under-report by exactly the
// amount that is easiest to forget.
//
// AND THE COROLLARY, which is what makes the count honest rather than merely
// generous (final-review I2, ruled): EVICTION ERASES THE WHOLE ENTRY, CPU COPY
// INCLUDED. A cache that freed only the GPU half would leave every evicted mesh's
// geometry in memory, uncounted and unshrinkable, while ResidentBytes() reported
// "fine" -- the one hazard s12 raises about residency, dressed as a policy.
// NriMeshBufferCache::ResidentBytes() is therefore EXACTLY the resident CPU+GPU
// bytes, with nothing hiding behind a not-ready entry.
//
// THE CPU COPY THAT SURVIVES AN EVICTION LIVES ONE LAYER UP, in the SUPPLY --
// SceneRenderResolver's in-memory MeshTable (SceneResources.hpp), already populated
// by the per-frame Request sweep. So s7.2's "re-upload after eviction without a
// disk read" still holds exactly: the first draw after an eviction costs one table
// lookup plus one upload, never an artifact read.
//
// THE MESH CAP IS render.mesh.residencyBudgetBytes (RenderBudgetSettings.hpp; 512 MiB
// by default, Restart), latched when the device account is created; the texture
// cap and the reservation percent are the account's other two settings.
//
// THE BUDGET IS THE DEVICE ACCOUNT, NOT A PER-VEHICLE NUMBER (3D-readiness Plan C
// Task 4, superseding final-review I3's "per vehicle" note). Every NriGraphContext
// still creates its own NriMeshBufferCache and NriTextureCache, but all of them
// charge ONE ResidencyAccount owned by the NriDevice (ResidencyAccount.hpp): the
// editor's viewport, its thumbnail harvester and every open Mesh/Shader document
// share the mesh cap and the texture cap, instead of each getting a full 512 MiB.
// The caches call Admit (the device-less policy there) and the account decides
// who is evicted; SelectEvictions below remains the single-cache rule that tests
// reach through the explicit-budget EvictToBudget overload.
//
// A DEDICATED MESH BUDGET HAS FIRST-CLASS UE PRECEDENT, and the earlier reading of
// Decision 7 understated it: alongside r.Streaming.PoolSize
// (StreamingManagerTexture.cpp:455-458) UE ships r.Streaming.PoolSizeForMeshes --
// default -1, meaning "share the texture pool", and any value >= 0 meaning a
// DEDICATED mesh pool of that size (TextureStreamingHelpers.cpp:125-129, consumed
// at AsyncTextureStreaming.cpp:668). So the shared pool is UE's DEFAULT, not UE's
// only shape, and a separate mesh budget is a configuration UE supports rather
// than a divergence we invented. Unification remains the eventual direction and
// the parked cvar arc is where it lands (comparison Decision 7).

#include <Arcane/Guid.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Arcane
{
    // What one resident mesh costs, and when it was last DRAWN (not last resolved --
    // a mesh resolved every frame by the scene sweep but never visible must still be
    // evictable, or the budget protects exactly the wrong entries).
    struct MeshResidencyEntry
    {
        Guid          id;
        std::uint64_t bytes = 0;
        std::uint64_t lastDrawnFrame = 0;
    };

    // Bytes one entry occupies: the CPU copy (vertices + indices + the sections'
    // strings) plus the two GPU buffers, which are the SAME byte counts -- the GPU
    // copy is a verbatim upload of the CPU one, so this is exactly 2x the CPU size
    // plus the section table's own CPU-only cost. Written as one function so the
    // "counted twice, on purpose" fact has a single place to be read.
    [[nodiscard]] inline std::uint64_t MeshResidencyBytes(std::size_t vertexBytes,
                                                          std::size_t indexBytes,
                                                          std::size_t sectionBytes) noexcept
    {
        return 2ull * (static_cast<std::uint64_t>(vertexBytes) + static_cast<std::uint64_t>(indexBytes))
             + static_cast<std::uint64_t>(sectionBytes);
    }

    // Which entries must be evicted, least-recently-drawn first, to bring `entries`
    // back within `budget`. Returns the guids to drop, in eviction order.
    //
    // NEVER EVICTS AN ENTRY DRAWN THIS FRAME (lastDrawnFrame == currentFrame), even
    // when that leaves the cache over budget -- evicting geometry the frame currently
    // being recorded still references is a use-after-free dressed as a policy, and a
    // frame that genuinely needs more than the budget must be allowed to render and be
    // reported, not silently corrupted. When the protected set alone exceeds the
    // budget this returns everything it CAN evict and the caller reports it once.
    //
    // Tie-break on the guid so equal lastDrawnFrame is deterministic -- an unstable
    // eviction order makes a [gpu] regression irreproducible.
    [[nodiscard]] inline std::vector<Guid> SelectEvictions(
        std::span<const MeshResidencyEntry> entries,
        std::uint64_t budget,
        std::uint64_t currentFrame)
    {
        std::uint64_t total = 0;
        for (const MeshResidencyEntry& e : entries)
            total += e.bytes;
        if (total <= budget)
            return {};

        std::vector<MeshResidencyEntry> evictable;
        evictable.reserve(entries.size());
        for (const MeshResidencyEntry& e : entries)
        {
            if (e.lastDrawnFrame != currentFrame)
                evictable.push_back(e);
        }
        std::sort(evictable.begin(), evictable.end(),
                  [](const MeshResidencyEntry& a, const MeshResidencyEntry& b)
                  {
                      if (a.lastDrawnFrame != b.lastDrawnFrame)
                          return a.lastDrawnFrame < b.lastDrawnFrame;
                      return a.id < b.id;
                  });

        std::vector<Guid> out;
        for (const MeshResidencyEntry& e : evictable)
        {
            if (total <= budget)
                break;
            out.push_back(e.id);
            total -= e.bytes;
        }
        return out;
    }
}
