// GpuSceneCullTest.cpp -- THE EXACT ORACLE for the GPU cull pass (F3 plan 2
// T5): what `mesh_cull.hlsl` actually wrote, read back off the device and
// compared ROW SET BY ROW SET against what the CPU expected
// (GpuSceneFrame::oracleVisibleIndices).
//
// ===== WHY A ROW-SET ORACLE AND NOT A COUNT =================================
// Plan 2 moved the visible-index list from the CPU to a compute pass. A count
// alone -- "the batch drew 3 instances" -- passes for a shader that wrote the
// WRONG three rows, wrote one row twice, or left a stale row from the previous
// frame inside the range the draw reads. Every one of those is a wrong picture
// with a right number. So this file asserts, per emitted batch:
//
//   * instanceNum equals the CPU's expected count, and never exceeds the
//     batch's capacity (the shader's InterlockedAdd is unbounded -- only the
//     WRITE is guarded -- so a count past capacity is the observable half of an
//     overflow),
//   * the SET of ids in [firstOutput, firstOutput + instanceNum) equals the
//     CPU's expected set exactly (atomic append order is arbitrary, so the
//     comparison is over sorted sets, never over positions),
//   * every id is in range and belongs to THAT batch,
//   * no id appears twice,
//   * and, across a second phase where the visible set SHRINKS, no leftover
//     from the first phase survives inside the new range -- the stale-tail
//     case, which is the one a single-frame test cannot see at all,
//   * and, across a third phase where NO batch is emitted at all (only
//     transparent rows stay coarse-visible), an EMPTY result lands for that
//     frame -- non-null, zero rows, zero through the hosts' seam -- rather than
//     the previous frame's count standing in for it; a fourth phase then lands
//     a real, nonzero count again once a batch returns.
//
// Transparent rows are asserted ABSENT: their batch keys are never emitted
// (BuildGpuSceneFrame) and MeshNode draws them as direct records, so a
// transparent row inside the compute output would mean the pass ignored
// `emitted`.
//
// ===== WHY THE READBACK IS ASYNCHRONOUS =====================================
// GpuScene's other readback (ReadDebugInstances) calls DeviceWaitIdle. That is
// legitimate for a byte-exactness test and impossible for anything a host may
// arm. The ring this file exercises copies args + visible indices after the
// cull pass and publishes the result from the GRAVEYARD, when the owning
// frame's fence has already retired -- so the test simply renders a few frames
// and finds an answer waiting. Nothing here waits, flushes or idles, and the
// first assertion in each case is that NOTHING is published after one frame.
#include <Arcane/Host/GpuSceneHost.hpp>            // the NRI-free arm/read seam the hosts use
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>             // BuildCube -- the fixture's geometry
#include <Arcane/Render/FramePacing.hpp>           // FramesInFlight() -- how long a publish takes to land
#include <Arcane/Render/GpuSceneSync.hpp>          // GpuSceneSync / BuildGpuSceneFrame -- the CPU half under comparison
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>      // the shared 0/0 latch every [gpu] case guards
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Render/Nri/GpuScene.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>    // MeshSceneDesc
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneCamera.hpp>            // PerspectiveProjection
#include <Arcane/Scene/SceneModule.hpp>            // RegisterSceneComponents
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>            // lookAtRH

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Helpers/GpuCapability.hpp"

namespace
{
    // Not square, deliberately (NriGraphPixelTest's own reasoning): a
    // transposed extent anywhere in the chain shows up as a mismatch rather
    // than as a plausible-looking wrong answer.
    constexpr std::uint32_t kW = 160;
    constexpr std::uint32_t kH = 96;

    constexpr float kEyeZ        = 4.0f;
    constexpr float kFovYDegrees = 60.0f;
    constexpr float kNearZ       = 0.1f;
    constexpr float kFarZ        = 100.0f;

    // The vehicle, in destruction order (the graph context borrows the
    // NriDevice, which wraps the native one). Mirrors NriGraphPixelTest's
    // PixelVehicle; that file's helpers live in its own anonymous namespace and
    // cannot be shared, so the minimum is restated here.
    struct CullVehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner> native;
        std::unique_ptr<Arcane::NriDevice>         nri;
        std::unique_ptr<Arcane::NriGraphContext>   ctx;
    };

    CullVehicle MakeVehicle(Arcane::GraphicsBackend backend)
    {
        CullVehicle v;
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
#if defined(ARC_BUILD_DEBUG)
        // The same validation set the [gpu][pixel] vehicle turns on: this case
        // adds a copy node reading the args buffer the mesh pass consumed as
        // indirect arguments, which is exactly the barrier-placement class
        // Vulkan sync validation catches and core validation does not.
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;
#endif
        v.native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(v.native != nullptr);
        v.nri = Arcane::NriDevice::Wrap(*v.native);
        REQUIRE(v.nri != nullptr);

        Arcane::HostConfig cfg;
        cfg.backend = backend;
        v.ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *v.nri, kW, kH, {});
        REQUIRE(v.ctx != nullptr);
        return v;
    }

    void RenderOne(Arcane::NriGraphContext& ctx, const Arcane::NriGraphContext::FrameDesc& frame)
    {
        REQUIRE(ctx.RenderFrameOffscreen(frame) == Arcane::NriGraphContext::FrameOutcome::Presented);
    }

    // The fixture's world: a registry with a mesh table, a material table and
    // the two schedulers the GPU scene reads from. Adapted from
    // GpuSceneSyncTest.cpp's World (multi-section meshes, material overrides)
    // with the device-facing half this file needs.
    struct CullWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry>            meshes;
        std::unordered_map<Arcane::Guid, Arcane::ResolvedMeshMaterial> materials;
        std::uint64_t meshGeneration = 1;
        Astra::Entity root{};
        Arcane::GpuSceneMirror mirror;

        CullWorld()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes, &meshGeneration });
            reg.SetResource<Arcane::MeshMaterialTable>(Arcane::MeshMaterialTable{ &materials });
        }

        // A cube whose index range is split into `sections` equal sections --
        // one GPU-scene ROW each, which is what "multiple sections" buys the
        // oracle: two rows of the same entity land in two different batches.
        Arcane::Guid AddMesh(Arcane::Guid id, float size, std::uint32_t sections)
        {
            Arcane::MeshEntry entry;
            entry.data   = Arcane::BuildCube(size);
            entry.bounds = Arcane::ComputeMeshBounds(entry.data);
            const std::uint32_t per = static_cast<std::uint32_t>(entry.data.indices.size()) / sections;
            entry.data.sections.clear();
            for (std::uint32_t s = 0; s < sections; ++s)
            {
                entry.data.sections.push_back(Arcane::MeshSection{ std::string(), s * per, per, s });
                entry.slots.push_back(Arcane::MeshSlot{});
            }
            meshes.emplace(id, entry);
            return id;
        }

        Arcane::Guid AddMaterial(Arcane::Guid id, Arcane::MaterialBlendMode blend, bool twoSided)
        {
            Arcane::ResolvedMeshMaterial m;
            m.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, blend == Arcane::MaterialBlendMode::Transparent ? 0.5f : 1.0f);
            m.blend     = blend;
            m.twoSided  = twoSided;
            materials.emplace(id, m);
            return id;
        }

        Astra::Entity Spawn(glm::vec3 position, Arcane::Guid mesh, Arcane::Guid material = {})
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t;
            t.position = position;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ mesh, material });
            return e;
        }

        // The mutable accessor is the change-tracked write GpuSceneSync's
        // Changed<WorldTransform> view sees (GpuSceneSyncTest.cpp's own idiom).
        void Move(Astra::Entity e, glm::vec3 position)
        {
            Arcane::Transform* t = reg.GetComponent<Arcane::Transform>(e);
            REQUIRE(t != nullptr);
            t->position = position;
        }

        void Schedulers()
        {
            Arcane::TransformPropagationSystem{}(reg);
            Arcane::BoundsSystem{}(reg);
        }
    };

    Arcane::ViewTransform FixtureView()
    {
        Arcane::ViewTransform view;
        view.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, kEyeZ), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        view.projection = Arcane::PerspectiveProjection(kFovYDegrees,
                                                        static_cast<float>(kW) / static_cast<float>(kH),
                                                        kNearZ, kFarZ);
        view.viewport = glm::uvec2{ kW, kH };
        return view;
    }

    void FillScene(Arcane::MeshSceneDesc& scene, const Arcane::ViewTransform& view)
    {
        scene.view           = view.view;
        scene.projection     = view.projection;
        scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
        scene.lightColor     = glm::vec3(1.0f);
        scene.ambient        = glm::vec3(0.08f);
    }

    // Every mesh this fixture registers, by Guid -- MeshNode resolves its
    // vertex/index buffers through this seam.
    Arcane::NriMeshBufferCache::MeshSupplyFn SupplyFrom(const CullWorld& world)
    {
        return [&world](const Arcane::Guid& g) -> Arcane::NriMeshBufferCache::SupplyResult
        {
            const auto it = world.meshes.find(g);
            if (it == world.meshes.end())
                return { nullptr, Arcane::MeshResolveState::Failed };
            return { &it->second.data, Arcane::MeshResolveState::Ready };
        };
    }

    // The CPU's expectation for one emitted batch: its slice of
    // oracleVisibleIndices, which BuildGpuSceneFrame filled with exactly the
    // rows that passed the coarse test and belong to this batch.
    std::vector<std::uint32_t> ExpectedRows(const Arcane::GpuSceneFrame& frame, const Arcane::GpuBatchDraw& batch)
    {
        std::vector<std::uint32_t> rows;
        for (std::uint32_t i = 0; i < batch.capacity; ++i)
        {
            const std::uint32_t id = frame.oracleVisibleIndices[batch.firstOutput + i];
            if (id != 0xFFFFFFFFu)
                rows.push_back(id);
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    }

    // The stable batch-key id behind an emitted draw: cullBatches is indexed by
    // that id and carries the argIndex the draw was given, so this inverts it
    // without the test having to re-derive the key.
    std::uint32_t BatchKeyIdOf(const Arcane::GpuSceneFrame& frame, const Arcane::GpuBatchDraw& batch)
    {
        for (std::uint32_t b = 0; b < frame.cullBatches.size(); ++b)
            if (frame.cullBatches[b].emitted != 0 && frame.cullBatches[b].argIndex == batch.argIndex
                && frame.cullBatches[b].firstOutput == batch.firstOutput)
                return b;
        FAIL("no cull batch carries this draw's argIndex");
        return 0;
    }

    // THE ORACLE ITSELF. `phase` names the frame in failure output, since both
    // phases run the same comparison over different expectations.
    void CheckCullMatchesOracle(const Arcane::GpuVisibilityReadback& result,
                                const Arcane::GpuSceneFrame& frame,
                                const Arcane::GpuSceneMirror& mirror,
                                const char* phase)
    {
        INFO("phase: " << phase);
        REQUIRE(result.args.size() == frame.args.size());
        REQUIRE(result.visibleIndices.size() == frame.rowCount);

        std::unordered_set<std::uint32_t> emittedIds;
        for (const Arcane::GpuBatchDraw& batch : frame.batches)
        {
            INFO("batch argIndex " << batch.argIndex << " firstOutput " << batch.firstOutput
                                   << " capacity " << batch.capacity);
            const std::vector<std::uint32_t> expected = ExpectedRows(frame, batch);
            const Arcane::DrawIndexedArgs& args = result.args[batch.argIndex];

            // THE COUNT: exactly the CPU's, and never past the batch's own
            // capacity -- the shader's atomic increment is unbounded even
            // though its write is guarded, so a count past capacity is how an
            // overflow becomes visible at all.
            CHECK(args.instanceNum == static_cast<std::uint32_t>(expected.size()));
            CHECK(args.instanceNum <= batch.capacity);
            // The rest of the record is the CPU's, untouched by the pass.
            CHECK(args.indexNum == batch.indexCount);
            CHECK(args.baseIndex == batch.indexOffset);

            // THE SET: sorted, because the atomic append order is arbitrary.
            const std::uint32_t written = std::min(args.instanceNum, batch.capacity);
            std::vector<std::uint32_t> actual;
            for (std::uint32_t i = 0; i < written; ++i)
                actual.push_back(result.visibleIndices[batch.firstOutput + i]);
            std::vector<std::uint32_t> sorted = actual;
            std::sort(sorted.begin(), sorted.end());
            CHECK(sorted == expected);
            // No id twice -- stated separately from the set comparison so a
            // duplicate reports as a duplicate rather than as a set mismatch.
            CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

            for (std::uint32_t id : actual)
            {
                INFO("emitted row id " << id);
                REQUIRE(id < frame.rowCount);                  // in range: never a stale 0xFFFFFFFF or a wild index
                REQUIRE(id < mirror.rows.size());
                CHECK(mirror.rows[id].live);
                CHECK(mirror.rows[id].batch == BatchKeyIdOf(frame, batch));   // belongs to THIS batch
                emittedIds.insert(id);
            }
        }

        // TRANSPARENT ROWS ARE ABSENT. Their batch keys are never emitted, so
        // the pass must skip them on `emitted == 0`; MeshNode draws them as
        // direct records instead.
        for (const Arcane::TransparentDraw& t : frame.transparentDraws)
        {
            INFO("transparent row " << t.row);
            CHECK(emittedIds.find(t.row) == emittedIds.end());
        }

        // The whole-frame count the hosts report is the same sum.
        std::uint32_t expectedTotal = 0;
        for (const Arcane::GpuBatchDraw& batch : frame.batches)
            expectedTotal += static_cast<std::uint32_t>(ExpectedRows(frame, batch).size());
        CHECK(result.VisibleRows() == expectedTotal);
    }

    void CheckGpuCullAgainstTheCpuOracle(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        // ---- THE FIXTURE (the plan's own list) -------------------------
        // TWO MESHES, the first with TWO SECTIONS, so an entity owns two rows
        // in two different batches; TWO entities per material variant, so every
        // shared batch carries at least two rows; boxes inside, outside and
        // straddling all six frustum planes; and masked, transparent and
        // two-sided material variants beside the plain opaque one.
        CullWorld world;
        const Arcane::Guid meshA{ 1, 1 };
        const Arcane::Guid meshB{ 2, 2 };
        world.AddMesh(meshA, 2.0f, /*sections*/ 2);
        world.AddMesh(meshB, 1.0f, /*sections*/ 1);
        const Arcane::Guid masked      = world.AddMaterial(Arcane::Guid{ 10, 1 }, Arcane::MaterialBlendMode::Masked, false);
        const Arcane::Guid transparent = world.AddMaterial(Arcane::Guid{ 10, 2 }, Arcane::MaterialBlendMode::Transparent, false);
        const Arcane::Guid twoSided    = world.AddMaterial(Arcane::Guid{ 10, 3 }, Arcane::MaterialBlendMode::Opaque, true);

        // Every entity whose rows can reach an INDIRECT batch (everything but
        // the transparent pair), kept so phase 3 below can move them all out
        // of the frustum at once.
        std::vector<Astra::Entity> indirectEntities;
        const auto spawnIndirect = [&](glm::vec3 position, Arcane::Guid mesh, Arcane::Guid material = {})
        {
            const Astra::Entity e = world.Spawn(position, mesh, material);
            indirectEntities.push_back(e);
            return e;
        };

        // Inside, sharing both of meshA's batches (2 rows each).
        const Astra::Entity insideA = spawnIndirect(glm::vec3(-0.6f, 0.2f, 0.0f), meshA);
        const Astra::Entity insideB = spawnIndirect(glm::vec3(0.6f, -0.2f, 0.0f), meshA);
        // Outside each of the six planes. The frustum at the origin plane is
        // ~7.7 x 4.6 m (60 degrees, 4 m away), near 0.1 and far 100 from the
        // eye at +4 -- so these are outside by orders of magnitude, never by a
        // rounding error the CPU and the GPU could resolve differently.
        spawnIndirect(glm::vec3( 1000.0f,     0.0f,     0.0f), meshA);   // right
        spawnIndirect(glm::vec3(-1000.0f,     0.0f,     0.0f), meshA);   // left
        spawnIndirect(glm::vec3(     0.0f,  1000.0f,    0.0f), meshA);   // top
        spawnIndirect(glm::vec3(     0.0f, -1000.0f,    0.0f), meshA);   // bottom
        spawnIndirect(glm::vec3(     0.0f,     0.0f,  1000.0f), meshA);  // behind the eye (near)
        spawnIndirect(glm::vec3(     0.0f,     0.0f, -1000.0f), meshA);  // past the far plane
        // Straddling: each box crosses one plane with roughly half of its
        // 2 m extent on either side, so both predicates accept it and neither
        // sits on a tangency.
        spawnIndirect(glm::vec3( 3.8f,  0.0f,   0.0f), meshA);   // across the right plane
        spawnIndirect(glm::vec3(-3.8f,  0.0f,   0.0f), meshA);   // across the left plane
        spawnIndirect(glm::vec3( 0.0f,  2.2f,   0.0f), meshA);   // across the top plane
        spawnIndirect(glm::vec3( 0.0f, -2.2f,   0.0f), meshA);   // across the bottom plane
        spawnIndirect(glm::vec3( 0.0f,  0.0f,   3.9f), meshA);   // across the near plane
        spawnIndirect(glm::vec3( 0.0f,  0.0f, -95.5f), meshA);   // across the far plane
        // The material variants, two entities each: masked and two-sided are
        // their own opaque-pass batches; transparent is never emitted at all.
        spawnIndirect(glm::vec3(-0.3f, 0.9f, 0.5f), meshB, masked);
        spawnIndirect(glm::vec3( 0.3f, 0.9f, 0.5f), meshB, masked);
        spawnIndirect(glm::vec3(-0.3f, -0.9f, 0.5f), meshB, twoSided);
        spawnIndirect(glm::vec3( 0.3f, -0.9f, 0.5f), meshB, twoSided);
        world.Spawn(glm::vec3(-0.9f, 0.0f, 1.2f), meshB, transparent);
        world.Spawn(glm::vec3( 0.9f, 0.0f, 1.2f), meshB, transparent);
        world.Schedulers();

        const Arcane::ViewTransform view = FixtureView();
        Arcane::VisibleSet visible;
        Arcane::BuildVisibleSet(world.reg, view, visible);

        Arcane::GpuSceneFrame frame;
        Arcane::GpuSceneSync(world.reg, world.mirror, /*deviceSyncedGeneration*/ 0u, frame.stage);
        Arcane::BuildGpuSceneFrame(world.mirror, &visible,
                                   Arcane::MeshSectionRanges(world.reg.GetResource<Arcane::MeshTable>()), view, frame);

        // The fixture is worth nothing if it does not actually produce the
        // shapes the oracle is meant to discriminate.
        REQUIRE(frame.rowCount > 0);
        REQUIRE(frame.batches.size() >= 3);            // meshA's two sections + the variants
        REQUIRE(frame.transparentDraws.size() == 2);   // never emitted, always direct
        REQUIRE(frame.stats.coarseVisible < frame.stats.total);   // something really is culled
        // THE IDENTITY THE WITNESS LANES ASSERT, on a LIVE frame: a frame's
        // draws are its indirect batches plus its direct transparent records,
        // and nothing else. This is the fact `visibility.transparentRows`
        // exists to let a report consumer check -- asserted here, where
        // BuildGpuSceneFrame's own numbers can disagree, rather than against a
        // report's literals, where it could only ever be arithmetic.
        CHECK(frame.stats.draws == frame.stats.batches
                                     + static_cast<std::uint32_t>(frame.transparentDraws.size()));
        {
            bool sharedBatch = false, maskedBatch = false, twoSidedBatch = false;
            for (const Arcane::GpuBatchDraw& b : frame.batches)
            {
                if (ExpectedRows(frame, b).size() >= 2) sharedBatch = true;
                if (b.blend == Arcane::MaterialBlendMode::Masked) maskedBatch = true;
                if (b.twoSided) twoSidedBatch = true;
            }
            REQUIRE(sharedBatch);
            REQUIRE(maskedBatch);
            REQUIRE(twoSidedBatch);
        }
        for (const Arcane::DrawIndexedArgs& a : frame.args)
            REQUIRE(a.instanceNum == 0u);   // compute is the sole counter writer

        // ---- THE VEHICLE, WITH THE RING ARMED --------------------------
        CullVehicle v = MakeVehicle(backend);
        v.ctx->SetMeshSupply(SupplyFrom(world));
        Arcane::GpuScene* device = v.ctx->Scene();
        REQUIRE(device != nullptr);
        REQUIRE(Arcane::GpuSceneArmVisibilityReadback(device));
        CHECK(device->VisibilityReadbackEnabled());
        CHECK(device->LatestVisibility() == nullptr);              // armed is not landed
        CHECK_FALSE(Arcane::GpuSceneVisibleRows(device).has_value());

        Arcane::MeshSceneDesc scene;
        FillScene(scene, view);
        scene.scene = &frame;
        Arcane::NriGraphContext::FrameDesc fd;
        fd.mesh = &scene;

        // Every frame that carries the fixture records ONE copy, so this count
        // is the ceiling on how many results may ever be published -- the pin
        // on the two publication paths not counting a frame twice.
        std::uint32_t recordedFrames = 0;
        const auto renderFixtureFrame = [&]
        {
            RenderOne(*v.ctx, fd);
            ++recordedFrames;
        };

        // ONE frame: the copy is recorded and its publish is parked against the
        // fence this frame signals. Nothing has reaped it and the slot has not
        // been reused, so the answer is still UNAVAILABLE -- the state a host
        // must report rather than a CPU count.
        renderFixtureFrame();
        CHECK(device->LatestVisibility() == nullptr);
        CHECK_FALSE(Arcane::GpuSceneVisibleRows(device).has_value());

        // Past the frames in flight, frame 1's fence has been observed -- by
        // the offscreen pacing wait ahead of declaration, which makes this
        // Execute's reap publish it, and failing that by the slot-reuse publish
        // inside the record callback. No wait, no flush, no idle anywhere.
        for (std::uint32_t i = 0; i < Arcane::FramesInFlight() + 1u; ++i)
            renderFixtureFrame();

        const Arcane::GpuVisibilityReadback* landed = device->LatestVisibility();
        REQUIRE(landed != nullptr);
        CHECK(landed->publishCount > 0);
        // NO DOUBLE PUBLICATION: the slot-reuse path and the graveyard path can
        // both come due for the same frame, and `publishedSeq` is what makes the
        // second one a no-op. More results than recorded copies would mean one
        // frame was counted twice.
        CHECK(landed->publishCount <= recordedFrames);
        CheckCullMatchesOracle(*landed, frame, world.mirror, "steady state");
        const std::optional<std::uint32_t> seam = Arcane::GpuSceneVisibleRows(device);
        REQUIRE(seam.has_value());
        CHECK(*seam == landed->VisibleRows());

        // ARMING IS IDEMPOTENT, and that matters now that hosts arm without
        // asking whether they already did: the runtime arms at boot on every
        // run, and the editor arms again every time it rebuilds its viewport
        // context. A second arm must not discard what has already landed.
        REQUIRE(Arcane::GpuSceneArmVisibilityReadback(device));
        REQUIRE(device->LatestVisibility() == landed);
        CHECK(Arcane::GpuSceneVisibleRows(device) == seam);

        // ---- PHASE 2: THE VISIBLE SET SHRINKS --------------------------
        // One of the two rows sharing meshA's batches leaves the frustum. The
        // device's visible-index buffer still holds the previous frame's ids in
        // the positions the batch no longer uses, so this is the case that
        // catches a stale tail entry being counted or drawn.
        const std::uint64_t publishedBefore = landed->publishCount;
        const std::uint64_t fenceBefore     = landed->fence;
        world.Move(insideB, glm::vec3(2000.0f, 0.0f, 0.0f));
        world.Schedulers();
        Arcane::VisibleSet shrunk;
        Arcane::BuildVisibleSet(world.reg, view, shrunk);
        Arcane::GpuSceneFrame frame2;
        Arcane::GpuSceneSync(world.reg, world.mirror, Arcane::GpuSceneSyncedGeneration(device), frame2.stage);
        Arcane::BuildGpuSceneFrame(world.mirror, &shrunk,
                                   Arcane::MeshSectionRanges(world.reg.GetResource<Arcane::MeshTable>()), view, frame2);
        REQUIRE(frame2.stats.coarseVisible < frame.stats.coarseVisible);
        // ...and the identity again, on a DIFFERENT live frame: the rebuild
        // dropped rows from shared batches while the transparent records stayed,
        // which is exactly the shape that would expose `draws` being counted
        // from the wrong side.
        CHECK(frame2.stats.draws == frame2.stats.batches
                                      + static_cast<std::uint32_t>(frame2.transparentDraws.size()));

        scene.scene = &frame2;
        for (std::uint32_t i = 0; i < Arcane::FramesInFlight() + 2u; ++i)
            renderFixtureFrame();

        const Arcane::GpuVisibilityReadback* shrunkResult = device->LatestVisibility();
        REQUIRE(shrunkResult != nullptr);
        REQUIRE(shrunkResult->publishCount > publishedBefore);
        // PUBLICATION MOVES FORWARD, NEVER BACKWARDS: whichever path published
        // this one, it came from a LATER frame than the previous result -- the
        // `publishedSeq >= seq` guard is what rules out an older pending entry
        // overwriting a newer answer.
        CHECK(shrunkResult->fence > fenceBefore);
        CHECK(shrunkResult->publishCount <= recordedFrames);
        CheckCullMatchesOracle(*shrunkResult, frame2, world.mirror, "after the visible set shrank");

        // ---- PHASE 3: NO EMITTED BATCH AT ALL --------------------------
        // Every entity that can reach an indirect batch leaves the frustum;
        // only the two transparent rows stay coarse-visible. Such a frame
        // emits no batch, so the compute pass has NOTHING it could have
        // incremented: the GPU's answer is determined -- zero -- before any
        // dispatch. The ring must publish that EMPTY result for THIS frame
        // rather than leave the shrunk frame's count standing. This is the
        // frame the runtime HUD shows while a camera orbits away from every
        // mesh, and the frame a --report's `gpuVisible` is read from when it
        // is the run's last; a count left over from an earlier frame is a
        // measurement attributed to the wrong frame. (A REFUSED frame is
        // different and keeps the last real result: it has no GPU answer.)
        const std::uint64_t publishedBeforeEmpty = shrunkResult->publishCount;
        const std::uint64_t fenceBeforeEmpty     = shrunkResult->fence;
        for (const Astra::Entity e : indirectEntities)
            world.Move(e, glm::vec3(5000.0f, 0.0f, 0.0f));
        world.Schedulers();
        Arcane::VisibleSet onlyTransparent;
        Arcane::BuildVisibleSet(world.reg, view, onlyTransparent);
        Arcane::GpuSceneFrame frame3;
        Arcane::GpuSceneSync(world.reg, world.mirror, Arcane::GpuSceneSyncedGeneration(device), frame3.stage);
        Arcane::BuildGpuSceneFrame(world.mirror, &onlyTransparent,
                                   Arcane::MeshSectionRanges(world.reg.GetResource<Arcane::MeshTable>()), view, frame3);
        REQUIRE(frame3.rowCount > 0);                    // every row is still resident -- nothing was freed
        REQUIRE(frame3.batches.empty());                 // ...and no indirect batch is emitted
        REQUIRE(frame3.args.empty());
        REQUIRE(frame3.transparentDraws.size() == 2);    // the direct records still draw
        CHECK(frame3.stats.coarseVisible == 2);
        CHECK(frame3.stats.draws == 2);                  // the identity, on a batch-less frame: 0 + 2

        scene.scene = &frame3;
        for (std::uint32_t i = 0; i < Arcane::FramesInFlight() + 2u; ++i)
            renderFixtureFrame();

        const Arcane::GpuVisibilityReadback* emptyResult = device->LatestVisibility();
        REQUIRE(emptyResult != nullptr);                          // an answer LANDED -- this is not "nothing completed"
        CHECK(emptyResult->publishCount > publishedBeforeEmpty);  // ...a NEW one, for these frames
        CHECK(emptyResult->fence > fenceBeforeEmpty);             // sequenced after the shrunk result, never before it
        CHECK(emptyResult->publishCount <= recordedFrames);       // still at most one publication per frame
        CHECK(emptyResult->args.empty());
        CHECK(emptyResult->visibleIndices.empty());
        CHECK(emptyResult->VisibleRows() == 0u);
        // THE HOSTS' SEAM: zero -- not null, and not the shrunk frame's count.
        CHECK(Arcane::GpuSceneVisibleRows(device) == std::optional<std::uint32_t>{ 0u });

        // ---- PHASE 4: A BATCH COMES BACK -------------------------------
        // The empty result must not be sticky either. One shared-batch entity
        // returns, and a real, nonzero count lands again -- one that matches
        // the oracle for its own frame.
        const std::uint64_t publishedBeforeReturn = emptyResult->publishCount;
        const std::uint64_t fenceBeforeReturn     = emptyResult->fence;
        world.Move(insideA, glm::vec3(-0.6f, 0.2f, 0.0f));
        world.Schedulers();
        Arcane::VisibleSet returned;
        Arcane::BuildVisibleSet(world.reg, view, returned);
        Arcane::GpuSceneFrame frame4;
        Arcane::GpuSceneSync(world.reg, world.mirror, Arcane::GpuSceneSyncedGeneration(device), frame4.stage);
        Arcane::BuildGpuSceneFrame(world.mirror, &returned,
                                   Arcane::MeshSectionRanges(world.reg.GetResource<Arcane::MeshTable>()), view, frame4);
        REQUIRE(frame4.batches.size() == 2);             // meshA's two sections, one row each
        REQUIRE(frame4.transparentDraws.size() == 2);

        scene.scene = &frame4;
        for (std::uint32_t i = 0; i < Arcane::FramesInFlight() + 2u; ++i)
            renderFixtureFrame();

        const Arcane::GpuVisibilityReadback* returnedResult = device->LatestVisibility();
        REQUIRE(returnedResult != nullptr);
        REQUIRE(returnedResult->publishCount > publishedBeforeReturn);
        CHECK(returnedResult->fence > fenceBeforeReturn);
        CHECK(returnedResult->publishCount <= recordedFrames);
        CHECK(returnedResult->VisibleRows() == 2u);
        CheckCullMatchesOracle(*returnedResult, frame4, world.mirror, "after a batch returned");
        CHECK(Arcane::GpuSceneVisibleRows(device) == std::optional<std::uint32_t>{ returnedResult->VisibleRows() });

        // A frame with no mesh scene, so the vehicle tears down with nothing
        // pending but what every frame leaves.
        {
            Arcane::NriGraphContext::FrameDesc empty;
            RenderOne(*v.ctx, empty);
        }
        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("GpuSceneCull: the GPU cull emits exactly the CPU oracle's row set (d3d12)",
          "[gpu][gpuscene][cull][nri][d3d12]")
{
    CheckGpuCullAgainstTheCpuOracle(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("GpuSceneCull: the GPU cull emits exactly the CPU oracle's row set (vulkan)",
          "[gpu][gpuscene][cull][nri][vulkan]")
{
    CheckGpuCullAgainstTheCpuOracle(Arcane::GraphicsBackend::Vulkan);
}

// ---- The device-free half: the seam's own states -------------------------
// The two facts every host depends on and no device is needed to state: a null
// device is not an armed one, and "nothing has landed" is nullopt rather than
// zero. A host that read zero here would report gpuVisible 0 for a frame the
// GPU may have drawn in full.

TEST_CASE("GpuSceneCull: the visibility seam refuses a null device and reports nothing landed",
          "[gpuscene][cull]")
{
    CHECK_FALSE(Arcane::GpuSceneArmVisibilityReadback(nullptr));
    CHECK_FALSE(Arcane::GpuSceneVisibleRows(nullptr).has_value());
}

TEST_CASE("GpuSceneCull: a readback's visible-row count sums every batch's instanceNum",
          "[gpuscene][cull]")
{
    Arcane::GpuVisibilityReadback result;
    CHECK(result.VisibleRows() == 0u);           // an empty result counts nothing
    result.args.push_back(Arcane::DrawIndexedArgs{ 36u, 3u, 0u, 0, 0u });
    result.args.push_back(Arcane::DrawIndexedArgs{ 36u, 0u, 36u, 0, 0u });   // an emitted batch the cull emptied
    result.args.push_back(Arcane::DrawIndexedArgs{ 12u, 5u, 72u, 0, 0u });
    CHECK(result.VisibleRows() == 8u);
}
