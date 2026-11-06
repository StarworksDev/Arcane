#pragma once

// The GPU scene's CPU-side vocabulary (F3, spec s5). NO NRI here: the mirror
// is a registry resource, Sync and the batch builder are header-only and
// device-free (GpuSceneSync.hpp), and ArcaneTests drives them under ~[gpu].
// Render/Nri/GpuScene.{hpp,cpp} is the device half.
#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Material/MaterialBlendMode.hpp>
#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Scene/Frustum.hpp>

#include <Astra/Container/FlatMap.hpp>
#include <Astra/Core/Tick.hpp>
#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Arcane
{
    // Mirrors BindlessTable::kInvalidSlot (0xFFFFFFFF) without pulling <NRI.h>;
    // GpuScene.cpp static_asserts the two agree.
    ARC_CONSTANT("shader contract: must match data/shaders/gpu_scene.hlsli")
    inline constexpr std::uint32_t kGpuInvalidMaterialSlot = 0xFFFFFFFFu;

    // Row flags: blend and cull are independent pipeline facts. Keep these in
    // lockstep with data/shaders/gpu_scene.hlsli without changing the 240-byte row.
    ARC_CONSTANT("shader contract: must match data/shaders/gpu_scene.hlsli")
    inline constexpr std::uint32_t kGpuInstanceFlagTeleported = 1u << 0;
    ARC_CONSTANT("shader contract: must match data/shaders/gpu_scene.hlsli")
    inline constexpr std::uint32_t kGpuInstanceFlagBlendShift = 1u;
    ARC_CONSTANT("shader contract: must match data/shaders/gpu_scene.hlsli")
    inline constexpr std::uint32_t kGpuInstanceFlagBlendMask  = 0x3u << kGpuInstanceFlagBlendShift;
    ARC_CONSTANT("shader contract: must match data/shaders/gpu_scene.hlsli")
    inline constexpr std::uint32_t kGpuInstanceFlagTwoSided   = 1u << 3;
    ARC_CONSTANT("shader contract: must match data/shaders/gpu_scene.hlsli")
    inline constexpr std::uint32_t kGpuInstanceFlagLive       = 1u << 4;

    // ONE ROW PER (entity, mesh section). 240 bytes, std430; data/shaders/
    // gpu_scene.hlsli carries the same field order -- change both or neither.
    // The normal matrix is NormalMatrixFor(model) computed on the CPU at
    // staging (R8: no per-vertex 3x3 inverse); prevModel is the pose the row
    // was drawn with LAST frame (spec s5.3's G2 contract).
    struct GpuInstance
    {
        glm::mat4     model{1.0f};
        glm::mat4     prevModel{1.0f};
        glm::vec4     normal0{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec4     normal1{0.0f, 1.0f, 0.0f, 0.0f};
        glm::vec4     normal2{0.0f, 0.0f, 1.0f, 0.0f};
        glm::vec4     boundsMin{0.0f};                  // world AABB; w unused
        glm::vec4     boundsMax{0.0f};                  // w = resolved alphaCutoff (stored across blend switches)
        glm::vec4     baseColor{1.0f};
        std::uint32_t materialSlot = kGpuInvalidMaterialSlot;
        std::uint32_t batch        = 0;                 // the batch KEY id (stable per (mesh, section, blend, twoSided))
        std::uint32_t flags        = 0;
        std::uint32_t pad          = 0;
    };
    static_assert(sizeof(GpuInstance) == 240, "GpuInstance must stay 240 bytes -- gpu_scene.hlsli mirrors it");

    struct GpuBatchKey
    {
        Guid          mesh{};
        std::uint32_t     section = 0;
        MaterialBlendMode blend = MaterialBlendMode::Opaque;
        bool              twoSided = false;
        [[nodiscard]] bool operator==(const GpuBatchKey&) const noexcept = default;
    };
    struct GpuBatchKeyHash
    {
        [[nodiscard]] std::size_t operator()(const GpuBatchKey& k) const noexcept
        {
            std::size_t h = std::hash<std::uint64_t>{}(k.mesh.hi);
            h ^= std::hash<std::uint64_t>{}(k.mesh.lo) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            const std::uint64_t state = (std::uint64_t(k.section) << 32)
                                      | (std::uint64_t(static_cast<std::uint8_t>(k.blend)) << 1)
                                      | std::uint64_t(k.twoSided);
            h ^= std::hash<std::uint64_t>{}(state) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        }
    };

    // Contiguous row spans, first-fit, UE's FSpanAllocator shape in miniature:
    // a multi-section entity owns `count` consecutive rows.
    struct RowSpanAllocator
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> freeSpans;   // {first, count}
        std::uint32_t highWater = 0;

        [[nodiscard]] std::uint32_t Allocate(std::uint32_t count)
        {
            for (std::size_t i = 0; i < freeSpans.size(); ++i)
            {
                auto& [first, n] = freeSpans[i];
                if (n >= count)
                {
                    const std::uint32_t out = first;
                    first += count;
                    n     -= count;
                    if (n == 0)
                        freeSpans.erase(freeSpans.begin() + static_cast<std::ptrdiff_t>(i));
                    return out;
                }
            }
            const std::uint32_t out = highWater;
            highWater += count;
            return out;
        }
        void Free(std::uint32_t first, std::uint32_t count) { freeSpans.emplace_back(first, count); }
        [[nodiscard]] std::uint32_t HighWater() const noexcept { return highWater; }
    };

    struct GpuSceneRow
    {
        Astra::Entity entity{};
        Guid          mesh{};
        std::uint32_t section = 0;
        std::uint32_t batch   = 0;
        glm::mat4     lastModel{1.0f};          // the matrix last uploaded -- the prior-pose history (spec s5.3)
        glm::vec4     lastBaseColor{1.0f};      // what the row was last staged with: a resolved material that
        std::uint32_t lastSlot = kGpuInvalidMaterialSlot;   //   changed under it re-stages the row (no generation plumbing)
        MaterialBlendMode lastBlend = MaterialBlendMode::Opaque;
        float             lastAlphaCutoff = 0.5f;
        bool              lastTwoSided = false;
        glm::vec3         boundsCenter{0.0f};   // current WorldBounds centre for CPU transparent sorting
        std::int32_t      renderOrder = 0;
        float             depthSortBias = 0.0f;
        bool          live    = false;
        std::uint64_t touched = 0;              // the sync counter that last saw the entity
    };

    struct GpuSceneMirror
    {
        struct Rows { std::uint32_t first = 0; std::uint32_t count = 0; };

        Astra::FlatMap<Astra::Entity, Rows>                              slots;
        std::vector<GpuSceneRow>                                         rows;          // by row; size == allocator.HighWater()
        RowSpanAllocator                                                 allocator;
        std::unordered_map<GpuBatchKey, std::uint32_t, GpuBatchKeyHash>  batchIds;
        std::vector<GpuBatchKey>                                         batchKeys;     // by id
        std::vector<std::uint32_t>                                       batchRowCount; // by id: live rows with that key
        std::vector<std::uint32_t>                                       dirtyLastFrame;
        std::uint64_t                                                    generation;    // fresh per mirror; the device stamps what it synced
        Astra::Tick                                                      lastSyncTick = 0;
        std::uint64_t                                                    syncCounter  = 0;

        GpuSceneMirror() : generation(NextGeneration()) {}
        // Out of line (Nri/GpuScene.cpp) and exported: ONE counter per process.
        // Mirrors are minted in several images (the hosts' GpuSceneHost.hpp,
        // ArcaneClient, tests); per-image counters could hand two mirrors the
        // same generation and mask a needed device rebuild (P2 header-statics).
        static ARC_RENDER_API std::uint64_t NextGeneration() noexcept;

        // A TRANSIENT registry resource: never serialized, rebuilt on any
        // registry swap (a mirror from a swapped registry has a new
        // generation, and the device side sees the mismatch as a full
        // rebuild). Registry::Save writes it through the no-op Serialize, so
        // a restore default-constructs a mirror with a NEW generation; the
        // no-op Serialize also keeps the container members off Astra's
        // reflected/trivially-copyable auto-serialization path at
        // registration, the same reason SceneVisibility (VisibilitySystem.hpp)
        // and BoundsSystemState (BoundsSystem.hpp) carry one.
        template<typename Archive> void Serialize(Archive& /*ar*/) {}
    };

    // What one Sync hands the device side: the rows to (re)write this frame.
    struct GpuSceneStage
    {
        std::vector<std::uint32_t> rows;
        std::vector<GpuInstance>   values;       // parallel to `rows`
        std::uint32_t              rowCapacity = 0;   // allocator high water: the buffer must hold this many rows
        bool                       fullRebuild = false;
        std::uint64_t              generation  = 0;   // the mirror's; the device stamps it as synced after a successful Apply
        void Clear() { rows.clear(); values.clear(); rowCapacity = 0; fullRebuild = false; generation = 0; }
    };

    // == nri::DrawIndexedDesc, field for field (GpuScene.cpp static_asserts it).
    struct DrawIndexedArgs
    {
        std::uint32_t indexNum     = 0;
        std::uint32_t instanceNum  = 0;
        std::uint32_t baseIndex    = 0;
        std::int32_t  baseVertex   = 0;
        std::uint32_t baseInstance = 0;
    };
    static_assert(sizeof(DrawIndexedArgs) == 20);

    // One record for every stable batch-key id, including keys which are not
    // emitted this view (and transparent keys). Keep this order in sync with
    // mesh_cull.hlsl: the shader indexes it with GpuInstance::batch.
    struct GpuCullBatch
    {
        std::uint32_t firstOutput = 0;
        std::uint32_t capacity    = 0;
        std::uint32_t argIndex    = 0;
        std::uint32_t emitted     = 0;
    };
    static_assert(sizeof(GpuCullBatch) == 16);

    // ONE COMPLETED GPU-CULL READBACK (F3 plan 2 T5): what the compute pass
    // actually wrote for ONE frame, copied off the device and published only
    // after that frame's fence had already retired -- never behind a wait.
    // The CPU's expectation for the same frame is
    // GpuSceneFrame::oracleVisibleIndices, and the oracle test compares the
    // two exactly. NRI-free on purpose: the hosts' observability (VerifyReport,
    // the runtime HUD) reads it through the Host/GpuSceneHost.hpp seam without
    // pulling the device half in.
    struct GpuVisibilityReadback
    {
        std::uint64_t publishCount = 0;   // results published so far; 0 == none has ever landed
        std::uint64_t fence        = 0;   // the frame fence whose retirement published this one
        std::vector<DrawIndexedArgs> args;             // by argIndex, as the compute pass left them
        std::vector<std::uint32_t>   visibleIndices;   // by row slot, the rows it emitted

        // The frame's GPU-visible row count: every emitted batch's
        // instanceNum, summed. Transparent rows are never counted -- they are
        // direct draw records the compute pass never sees (spec s6).
        [[nodiscard]] std::uint32_t VisibleRows() const noexcept
        {
            std::uint32_t n = 0;
            for (const DrawIndexedArgs& a : args)
                n += a.instanceNum;
            return n;
        }
    };

    struct GpuBatchDraw
    {
        Guid          mesh{};
        std::uint32_t section     = 0;
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount  = 0;
        std::uint32_t firstOutput = 0;   // where this batch's visible rows start in visibleIndices
        std::uint32_t capacity    = 0;   // resident rows with this key
        std::uint32_t argIndex    = 0;   // position in `args`
        MaterialBlendMode blend   = MaterialBlendMode::Opaque;
        bool          twoSided    = false;
        float         nearDepth   = 0.0f;
    };

    // One visible transparent row, already fully resolved by the CPU frame
    // builder. MeshNode must draw this record directly; it never needs to
    // look up a mesh section or material state again.
    //
    // `projectedDepth` (a PER-OBJECT projected view-space Z plus the author's
    // bias) is CONVENTIONAL ORDERED TRANSPARENCY -- the F3 BASELINE, and
    // deliberately NOT Arcane's permanent answer (spec R9, the Deadlock-shaped
    // hybrid contract). Sorting whole objects by one depth cannot resolve
    // interpenetration or intra-object face overlap, and the engine's stated
    // route out is MBOIT accumulation/combine plus a refraction pass, which
    // stay NAMED LATER SEAMS in F3 rather than accidental scope. Nothing here
    // may be described as "the transparency solution".
    struct TransparentDraw
    {
        std::uint32_t     row = 0;
        Guid              mesh{};
        std::uint32_t     section = 0;
        std::uint32_t     indexOffset = 0;
        std::uint32_t     indexCount = 0;
        MaterialBlendMode blend = MaterialBlendMode::Transparent;
        bool              twoSided = false;
        std::int32_t      renderOrder = 0;
        float             projectedDepth = 0.0f;
        Astra::Entity     entity{};
    };

    struct GpuSceneFrame
    {
        GpuSceneStage              stage;
        std::vector<GpuBatchDraw>  batches;          // EMITTED, in draw order
        std::vector<DrawIndexedArgs> args;           // by argIndex
        std::vector<GpuCullBatch> cullBatches;        // by stable batch key id; consumed by MeshCullNode
        std::vector<std::uint32_t> visibleIndices;   // device output: rowCapacity entries, initially invalid each frame
        std::vector<std::uint32_t> oracleVisibleIndices; // CPU expectation for later GPU readback comparison
        std::vector<TransparentDraw> transparentDraws; // direct records, ordered far-to-near within render order
        std::uint32_t              rowCount = 0;     // == stage.rowCapacity
        Frustum                    frustum;          // the widened planes the CPU test used (plan 2's cull CB)
        struct Stats { std::uint32_t total = 0, coarseVisible = 0, batches = 0, draws = 0; } stats;

        [[nodiscard]] bool HasDraws() const noexcept { return !batches.empty() || !transparentDraws.empty(); }
    };

    // The figures the HUD and the shutdown report read. Hosts copy this out of
    // the extract packet instead of the whole GpuSceneFrame, so the copy
    // allocates nothing. The editor snapshots it at the viewport extract: the
    // scene-save cover extracts again and must not replace those figures.
    struct GpuSceneFrameStats
    {
        GpuSceneFrame::Stats stats{};
        std::uint32_t transparentDraws = 0;
    };

    // Mutable execution-order state shared by the sync, cull, and mesh graph
    // callbacks for one declared frame. Reserve runs before graph declaration;
    // Apply updates the ready bits inside the sync callback, and later callbacks
    // must read them there rather than snapshotting them during declaration.
    struct GpuSceneFrameReadiness
    {
        bool registryReserved = false;
        bool registryReady    = false;
        bool adHocReady       = false;
    };

    struct GpuSceneApplyResult
    {
        bool registryReady = false;
        bool adHocReady    = false;
    };
}
