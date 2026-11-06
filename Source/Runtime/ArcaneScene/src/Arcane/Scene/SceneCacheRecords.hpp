#pragma once

// Mesh, sprite and mesh-material cache records, plus the 2D physics
// interpolation buffer. Split out of SceneResources.hpp so a render cache
// translation unit can include them without preprocessing
// Astra/Registry/Registry.hpp (SceneResources.hpp includes Ecs.hpp).

#include <Arcane/Core/Constant.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialBlendMode.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>        // MeshSlot, and MeshBuilder.hpp's MeshData / MeshBounds

#include <Astra/Container/FlatMap.hpp>    // PhysicsInterpBuffer2D::slotOf (fenced below)

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <tuple>                            // std::tuple -- MeshEntry::GeometryIdentity's
                                            // return type. NOT std::tie: it returns the
                                            // identity BY VALUE so the one caller
                                            // (SceneRenderResolver::InvalidateMesh) can
                                            // still compare it after erasing the entry
                                            // it was read from (see GeometryIdentity()).
#include <vector>

namespace Arcane
{
    // ---- render interpolation (Epic 04.2) -----------------------------------
    // Blend a previous fixed-step pose toward the current one by RunLoop alpha so
    // slow-mo renders smoothly instead of snapping. Rotation MUST use AngleLerp
    // (shortest arc), not a matrix-component lerp.
    //
    // These two serve PhysicsInterpPose2D below, which stays a 2D pose
    // (glm::vec2 + a scalar angle) because Manifold2D is a 2D solver. The
    // sprite path (RenderSubmissionSystem) blends through these same two
    // helpers since the Astra adoption (2026-09-11), so overlay and sprite
    // agree to the bit.
    namespace Detail::Physics2D
    {
        [[nodiscard]] inline float Lerp(float a, float b, float t) noexcept
        {
            return a + (b - a) * t;
        }

        // Shortest-arc angle interpolation (radians): wrap the delta into (-pi, pi]
        // before blending, so 350deg->10deg travels +20deg through 0, not -340deg.
        [[nodiscard]] inline float AngleLerp(float a, float b, float t) noexcept
        {
            ARC_CONSTANT("math identity / tolerance: pi")
            constexpr float kPi  = 3.14159265358979323846f;
            ARC_CONSTANT("math identity / tolerance: tau = 2 pi")
            constexpr float kTau = 2.0f * kPi;
            float d = std::fmod(b - a, kTau);
            if (d < -kPi)      d += kTau;
            else if (d >  kPi) d -= kTau;
            return a + d * t;
        }
    }

    // One body's previous fixed-step pose. `generation` mirrors the body handle's
    // generation so a recycled SoA slot (stale prev) is rejected by the consumer.
    struct PhysicsInterpPose2D
    {
        glm::vec2     position{0.0f, 0.0f};
        float         angle      = 0.0f;   // radians
        std::uint32_t generation = 0;      // 0 == dead slot (never matches a live handle)
    };

    // One entity's address into `prev`: the body SLOT it occupied at capture and
    // the handle generation it had then. Manifold2D-free on purpose -- this
    // header is compiled by every game module, whose include surface has no
    // Manifold2D row, so Phys::BodyHandle cannot appear here (and that is why
    // RenderSubmissionSystem reads THIS map rather than PhysicsBodyRef2D).
    struct PhysicsInterpSlot2D
    {
        std::uint32_t index      = 0;
        std::uint32_t generation = 0;
    };

    // Per-body previous-pose buffer, indexed by PhysicsWorld body SLOT index (the
    // same space GatherPhysicsDebug iterates). Populated by PhysicsSystem2D before each
    // world.Step(); read by GatherPhysicsDebug and RenderSubmissionSystem. Transient
    // runtime state: AstraTransientResource, so Registry::Save skips it and a
    // restore never revives a stale history (IN-8; it replaced Runtime::
    // RestoreRegistry's hand-strip). The no-op Serialize still satisfies Astra's
    // HasSerializeMethod so the vector member does not hit the
    // trivially-copyable path when the descriptor is built.
    struct PhysicsInterpBuffer2D
    {
        static constexpr bool AstraTransientResource = true;

        std::vector<PhysicsInterpPose2D> prev;
        // entity -> its slot at capture. Rebuilt by PhysicsSystem2D PASS 2.5 from
        // PhysicsWorld2D::entityToBody in the same pass that fills `prev`, so the
        // two are exactly as fresh as each other. Read by RenderSubmissionSystem:
        // a miss (no entry, slot past `prev`, generation mismatch) snaps.
        // ARC_INTERNAL_BEGIN: Astra's FlatMap container has no facade alias (engine-side interp bookkeeping)
        Astra::FlatMap<Astra::Entity, PhysicsInterpSlot2D> slotOf;
        // ARC_INTERNAL_END
        bool                    captured = false;   // false until the first capture

        template<typename Archive>
        void Serialize(Archive& /*ar*/) {}
    };

    // One .arcsprite asset, resolved for submission: the source texture's
    // asset Guid, plus ComputeSpriteGeom's output (SpriteAsset.hpp) -- UVs for
    // the pixel sub-rect and the world size in meters -- and the asset's
    // normalized pivot. Precomputed host-side so RenderSubmissionSystem never
    // touches the Assets facade or re-derives rect math per frame.
    struct SpriteEntry
    {
        glm::vec2 uvMin{0.0f, 0.0f};
        glm::vec2 uvMax{1.0f, 1.0f};
        glm::vec2 sizeMeters{1.0f, 1.0f};
        glm::vec2 pivot{0.5f, 0.5f};
        // The SOURCE TEXTURE's asset Guid (.arcsprite's `texture` field), and
        // the record's ONLY texture identity. Device-free by construction:
        // RenderSubmissionSystem hands it to Batcher2D::QuadTextured, it
        // travels into the drained span, and the NRI recorder resolves it
        // through NriTextureCache. Nil when the sprite declares no texture.
        Guid textureId{};
    };

    // One resolved .arcmesh (F2a, Task 4): OWNED geometry, generated once by
    // MeshCache::Request from BuildMeshData, plus its local-space bounds
    // (ComputeMeshBounds). MeshInstance::mesh is the asset Guid (F2c s7.2);
    // NriMeshBufferCache makes `data` resident at declaration time. The table
    // lookup in GpuSceneSync still decides whether the entity is
    // drawable -- MeshCache::Invalidate/Clear erase that entry.
    //
    // `slots` (F2a Task 5; grown from a scalar `material` to a named-slot array in
    // F2c Task 10) is a COPY of the loaded .arcmesh's own `MeshAssetData::slots` --
    // the mesh's default material Guid PER SECTION, resolved through the index the
    // section carries (MeshSection::slotIndex, Mesh/MeshBuilder.hpp), the second
    // link in GpuSceneSync's `materialOverride` -> per-section default ->
    // white chain. It rides along here because GpuSceneSync is
    // host-published-resource-only by design (it reads MeshTable/MeshMaterialTable
    // and never touches a cache pointer, matching RenderSubmissionSystem's rule of
    // never touching the Assets facade). Name and generated topology still re-read
    // the .arcmesh (MeshDocument); MeshCache keeps no copy of the asset (see
    // MeshCache.hpp's "WHAT IT DOES NOT KEEP").
    struct MeshEntry
    {
        MeshData               data;
        MeshBounds             bounds;
        std::vector<MeshSlot>  slots;

        // F2c s7.3: THE GEOMETRY IDENTITY -- what a .arcmesh save must be compared on
        // to decide whether the RESIDENT BUFFERS survive it. A slot reassignment
        // touches none of these and must not re-upload a two-million-triangle prop;
        // anything that changes the vertices must drop them.
        //
        // EVERY FIELD OF MeshAssetData THAT DETERMINES GEOMETRY IS HERE, not just
        // `source` (final-review C3). rings/segments/subdivisions/capsuleLengthRatio
        // are the generators' topology (MeshAsset.hpp's own TOPOLOGY and SHAPE RATIO
        // blocks), and editing one of them while `source` stayed put used to take the
        // KEEP arm: the CPU entry rebuilt with a new index count while the GPU
        // buffers still held the old one, which the per-frame mesh sweep then drew past.
        // MeshAssetData::operator== is the authoritative field list; the only members
        // it carries that are NOT geometry are id, name and slots. A new generator
        // parameter belongs here the day it is added.
        //
        // Copied at MeshCache::Request time beside `slots`, and read NOWHERE ELSE --
        // they exist for exactly this comparison, which is why they say so here.
        MeshSource    source = MeshSource::Cube;
        Guid          importedSource{};
        std::uint32_t rings              = 16;
        std::uint32_t segments           = 32;
        std::uint32_t subdivisions       = 1;
        float         capsuleLengthRatio = 2.0f;

        // The whole identity as ONE value, so no caller can compare a subset by
        // accident -- which is exactly how C3 shipped. BY VALUE, not std::tie: the
        // one caller (SceneRenderResolver::InvalidateMesh) captures it, ERASES the
        // entry, re-resolves and only then compares, so a tuple of references would
        // dangle across the erase.
        using GeometryId = std::tuple<MeshSource, Guid, std::uint32_t, std::uint32_t,
                                      std::uint32_t, float>;
        [[nodiscard]] GeometryId GeometryIdentity() const noexcept
        {
            return { source, importedSource, rings, segments, subdivisions,
                     capsuleLengthRatio };
        }
    };

    // One resolved mesh material (F2a, Task 4; `albedo`/`materialSlot` added
    // F2b Task 11): CONSTANTS ONLY. Neither MeshCache nor MeshMaterialCache
    // may touch MaterialSource or ShaderCompiler (that would open a second
    // compile-drain site alongside SceneRenderResolver's one, Host/
    // SceneRenderResolver.hpp:22-28), so there is no compiled pipeline
    // riding along here -- just the values MeshInstance::baseColor/
    // materialSlot (Render/Nri/nodes/MeshNode.hpp) copy into the per-instance
    // root constant. Default (1,1,1,1) is exactly what a nil material at the
    // end of the materialOverride -> mesh-default chain resolves to
    // directly, with no lookup at all.
    struct ResolvedMeshMaterial
    {
        glm::vec4 baseColor{1.0f};
        MaterialBlendMode blend = MaterialBlendMode::Opaque;
        float alphaCutoff = 0.5f;
        bool twoSided = false;

        // The mesh material's declared "albedo" param, if any (F2b Task 11)
        // -- a Texture-typed value read off the .arcmat chain by
        // MeshMaterialCache::Request exactly like baseColor. Nil (the
        // default) is legal and means "no texture, the flat baseColor path"
        // -- the resolver needs this only to know WHICH texture
        // `materialSlot` below names (GpuSceneSync copies the slot per row); nothing
        // reads it directly at draw time.
        Guid albedo{};

        // `albedo` resolved into a slot in the render device's bindless
        // material table (Render/Nri/BindlessTable.hpp) via
        // NriGraphContext::ResolveMeshAlbedoSlot -> BindlessTable::Add, or
        // the default below when there is nothing to resolve (nil albedo,
        // no device attached, or the resolve failed). Populated by
        // MeshMaterialCache::Request through its injected `resolveAlbedoSlot` seam
        // (Render/MeshMaterialCache.hpp) -- this struct stays CONSTANTS
        // ONLY, still: it holds a slot NUMBER a device produced elsewhere,
        // never a device object of its own.
        //
        // 0xFFFFFFFF, restated as a LITERAL rather than referencing
        // `BindlessTable::kInvalidSlot` by name: that header pulls <NRI.h>
        // in (Render/Nri/BindlessTable.hpp's own include-order note), and
        // this file is deliberately render-backend-agnostic -- it is shared
        // by the 2D path above (SpriteEntry, SpriteTable) and by dozens of
        // NRI-free consumers (Base/Runtime.hpp, Plugin/PluginABI.hpp, most
        // of Scene/ and every Astra-registry CPU test that never touches a
        // device). The two literals MUST stay numerically identical --
        // Render/Nri/GpuScene.cpp's static_assert, where both BindlessTable
        // and kGpuInvalidMaterialSlot are visible together, is the compiled half of
        // that contract; this comment is the other half, matching the
        // discipline mesh.hlsl's own kMeshInvalidMaterialSlot restatement
        // already uses for the identical reason.
        std::uint32_t materialSlot = 0xFFFFFFFFu;
    };
}
