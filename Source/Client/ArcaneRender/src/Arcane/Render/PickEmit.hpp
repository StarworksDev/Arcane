#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// PickEmit: the CPU "which entities are pickable" seam for GPU entity-id
// picking (see docs/superpowers/specs/2026-07-19-arcane-entity-id-picking-design.md,
// SS3b/3c). CollectPickables (defined in ExtractFrame.cpp, where the walk lives)
// walks the registry for pickable entities (sprites, physics colliders, meshes)
// and appends one PickDrawable per pickable shape,
// in WORLD space, ready for the id pass. The k-th appended drawable (0-based)
// gets hit-proxy id k+1; PickEntityForId inverts that mapping (id 0 == background).
//
// Pure, device-less-testable: no GPU, no render device, and -- since F4 (spec
// s7.1) -- NO VIEW. The emitter knows nothing about the camera: sprites are the four
// world corners SpriteWorldQuad places, physics silhouettes are world shapes in
// metres at the body pose, meshes are a world matrix + the asset guid. The id
// pass (Render/Nri/nodes/PickOutlineNodes.hpp's PickNode) projects all of it
// through the frame's ViewTransform (NriGraphContext::FrameDesc::pickView), so
// the same drawables pick correctly under an orthographic 2D view, a tilted
// one or a perspective one. The orthographic-only view struct that carried
// plan 1's per-axis pixel mapping into the emitter is gone (ABI 33).

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Guid.hpp>

#include <Astra/Entity/Entity.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Astra { class Registry; }

namespace Arcane
{
    // A single pickable shape in WORLD space. `entity` is the owning entity;
    // `kind` selects which of the geometry fields are meaningful:
    //   Quad     -- corners
    //   Circle   -- center, radius, angle
    //   Capsule  -- center, halfLen, radius, angle
    //   Box      -- center, halfExtents, angle
    //   Mesh     -- world, mesh
    struct PickDrawable
    {
        Astra::Entity entity{};

        enum class Kind : uint8_t { Quad, Circle, Capsule, Box, Mesh };
        Kind kind = Kind::Quad;

        // Quad (sprites): the four WORLD corners, TL, TR, BR, BL (SpriteWorldQuad's
        // order -- Render/SpriteGeometry.hpp, THE corner rule the drawn quad uses).
        std::array<glm::vec3, 4> corners{};

        // Circle / Capsule / Box (physics fixtures): a WORLD-space shape in the XY
        // plane at `center` (metres; z = the entity's world z, 0 without a
        // WorldTransform), turned by `angle` (radians, world sense, about +Z).
        glm::vec3 center{0.0f};
        glm::vec2 halfExtents{0.0f};   // Box
        float     radius  = 0.0f;      // Circle / Capsule
        float     halfLen = 0.0f;      // Capsule
        float     angle   = 0.0f;

        // Mesh: the whole resident mesh, rasterised by the pick node's second
        // pipeline through `world` (the entity's WorldTransform matrix).
        glm::mat4 world{1.0f};
        Guid      mesh{};
    };

    // Collect every pickable entity's silhouette geometry, appended to `out`
    // (NOT cleared -- caller controls accumulation; pass an empty vector to
    // start fresh, matching how tests and the hosts both use it).
    //
    // ORDERING (this IS the hit-proxy id assignment: id = index+1):
    //   1. Sprites   -- View<WorldTransform, SpriteRenderer, Not<Hidden>>: the
    //      DRAWN set, RenderSystems.hpp's own filter, in the registry's view
    //      iteration order (archetype-stable). One Quad per entity whose
    //      corners are SpriteWorldQuad(world matrix, the sprite ASSET's base
    //      size, its pivot) -- the SpriteTable resolves both exactly as
    //      submission does (1x1 m at the centre pivot when unresolved) -- so
    //      the silhouette is the drawn quad under ANY projection, a mirrored
    //      or off-centre pivot included.
    //   2. Colliders -- one PickDrawable per collider fixture, from
    //      FillColliderSilhouettes2D (Scene/PhysicsQuery.hpp): the physics
    //      query proxy walks an archetype-stable View<Arcane::Collider2D,
    //      Arcane::PhysicsBodyRef2D> (DETERMINISTIC: the id assignment
    //      id=index+1 must not depend on unordered_map hash order) and hands
    //      back world-space silhouettes at the live body pose, scaled by the
    //      body's baked appliedScale so a scaled body's silhouette matches its
    //      drawn collider. Render names no solver type. With no physics world
    //      on the registry (or no 2D physics module registered), none are
    //      collected -- not an error.
    //   3. Meshes    -- View<WorldTransform, MeshRenderer, Not<Hidden>>, resolved
    //      through the registry's MeshTable; a nil, unresolved or EMPTY mesh
    //      (no sections) emits nothing, exactly what GpuSceneSync
    //      (Render/GpuSceneSync.hpp) stages. ONE drawable per entity
    //      whatever its section count: sections carry materials, not identity.
    //
    // THE ORDER RULE FOR THE ID PASS (spec s7.1): the 2D kinds draw first with
    // the depth test OFF in submission order (later wins, so a collider picks
    // over an underlying sprite), then the meshes draw depth-tested against a
    // cleared depth -- so a mesh always owns a pixel it shares with a sprite,
    // and meshes resolve among themselves by depth: the main pass's order.
    ARC_RENDER_API void CollectPickables(Astra::Registry& registry, std::vector<PickDrawable>& out);

    // The pass id assigned to `e` under the k+1 convention CollectPickables emits
    // (the k-th entity in `ordered` gets id k+1; 0 = background). Reverse of the
    // read-back mapping in Pick(). 0 if `e` is absent or Astra::Entity::Invalid().
    ARC_RENDER_API uint32_t PickPassId(const std::vector<Astra::Entity>& ordered, Astra::Entity e);

    // id 0 -> background (invalid entity). id k (k>=1) -> drawables[k-1].entity.
    // Out-of-range k -> invalid entity. Astra::Entity{} is the invalid sentinel
    // (IsValid() == false).
    inline Astra::Entity PickEntityForId(const std::vector<PickDrawable>& drawables, uint32_t id)
    {
        if (id == 0 || id > drawables.size())
            return Astra::Entity{};
        return drawables[id - 1].entity;
    }

    // The ENTITY-ONLY twin of PickEntityForId, same contract, for a consumer
    // that RETAINED the id<->entity table rather than the drawables it came
    // from. That is not a convenience: the NRI graph's pick readback lands
    // FramesInFlight() frames after the id pass that produced it, so
    // the editor's deferred click-pick has to hold the table from the frame
    // that RASTERISED the click -- by which time the live drawables vector has
    // been rebuilt two or more times. Copying entities rather than whole
    // PickDrawables is what makes retaining it cheap.
    //
    // A DISTINCT NAME rather than an overload, deliberately: `PickPassId({}, e)`
    // and `PickEntityForId({}, id)` are both live call shapes in this tree, and
    // a braced empty argument against two container types is ambiguous rather
    // than convenient.
    inline Astra::Entity PickEntityForPassId(std::span<const Astra::Entity> ordered, uint32_t id)
    {
        if (id == 0 || id > ordered.size())
            return Astra::Entity{};
        return ordered[id - 1];
    }

    // PickPassId over the DRAWABLES rather than a separate entity vector -- the
    // form a caller that just ran CollectPickables already has in hand, and
    // therefore the form that cannot disagree with the id pass it just fed.
    // Same k+1 rule and the same FIRST-match tie-break (an entity with several
    // fixtures emits several drawables; the first is the one the outline
    // traces, which is what PickBuffer::PassIdOf reports too -- this is that
    // method's loop, against a span instead of a member).
    inline uint32_t PickPassIdOf(std::span<const PickDrawable> drawables, Astra::Entity e)
    {
        if (e == Astra::Entity::Invalid())
            return 0u;
        for (std::size_t k = 0; k < drawables.size(); ++k)
            if (drawables[k].entity == e)
                return static_cast<uint32_t>(k + 1);
        return 0u;
    }

    // The id-buffer texel to sample for a 1x viewport click at `pixel1x` when the
    // id buffer is supersampled by `ss` (center subsample), clamped to [0, dim).
    ARC_RENDER_API glm::ivec2 PickSampleTexel(glm::vec2 pixel1x, uint32_t ss, uint32_t idW, uint32_t idH);

    // THE VIEWPORT ID PASS'S SUPERSAMPLE FACTOR -- ONE number, read by
    // everything that depends on it. The id target is sized ss*width x ss*height while the
    // world->clip map stays LOGICAL, so the same silhouettes rasterise at ss x
    // density; outline_seed.hlsl then averages the ss*ss subsamples of each 1x
    // pixel into a SUB-PIXEL edge centroid (its `ctr`), which is the seed
    // position the composite measures its distance to.
    //
    // That makes this factor PIXEL-VISIBLE, not a quality knob: at ss=1 every
    // seed sits at its pixel centre, at ss=2 it sits up to a quarter-pixel off
    // it, and the composite's AA ramp is only 1 px wide -- so the two produce
    // visibly different outline edges. It is ONE read, not a literal at each
    // site, for exactly that reason: two copies would drift and the symptom
    // would be a subtly different outline. The factor is the Restart setting
    // render.outline.supersample (default 2), read through the process latch
    // Arcane::PickSupersample() in RenderOutlineSettings.hpp (settings S6-21).

    // =====================================================================
    // THE ID PASS'S 2D GEOMETRY -- ONE emitter.
    //
    // A recorder MUST build these vertices from these drawables in this
    // order, because the 1-based id a vertex carries IS the id<->entity
    // mapping every consumer inverts (PickEntityForId). A second copy of this
    // loop would be a second id assignment that agrees until one of them is
    // edited -- the same reasoning that keeps ONE Batcher2D feeding the 2D
    // path. Mesh drawables emit NO quad here (the pick node rasterises their
    // resident triangles through its second pipeline) but they still OWN their
    // index in the numbering, so the two pipelines share one id space.
    //
    // Pure and device-less: no render device, no graphics API at all.
    // =====================================================================

    // Per-vertex data for the id pass, matching entity_id.hlsl's VSInput. The
    // C++ attribute array order at the recorder MUST match that struct's
    // member order (NRI takes an explicit vk.location, and D3D matches the
    // custom semantic name at SemanticIndex 0).
    //
    // pos is WORLD space; entity_id.hlsl multiplies by the frame's
    // view-projection. local/radius/halfLen are METRES (the PS coverage test
    // is unit-agnostic: it compares a local offset against a radius in
    // whatever unit both arrived in).
    struct PickIdVertex
    {
        glm::vec3 pos;      // world: the rotated bounding-quad corner
        glm::vec2 local;    // shape-local coords (unrotated), metres
        float     radius;   // metres (circle/capsule)
        float     halfLen;  // metres (capsule)
        uint32_t  kind;     // 0=Quad 1=Circle 2=Capsule 3=Box
        uint32_t  id;       // 1-based hit-proxy id
    };
    static_assert(sizeof(PickIdVertex) == 36, "id vertex is the wire format");

    // kind -> the shader code entity_id.hlsl's PS switches on. Mesh -> 4 is
    // never emitted to the 2D path (BuildPickIdGeometry skips the kind); the
    // code exists so the switch is total.
    ARC_RENDER_API uint32_t PickKindCode(PickDrawable::Kind kind);

    // Bounding half-extents (metres) of a drawable's silhouette: the quad the
    // id pass rasterizes. The PS analytically discards fragments outside
    // circle/capsule shapes; Box fills the whole bound. A Quad's bound is half
    // its edge lengths (its corners ARE its geometry); a Mesh has no 2D bound
    // and reports zero.
    ARC_RENDER_API glm::vec2 PickBoundHalfExtents(const PickDrawable& drawable);

    // Build the id-pass vertex + index arrays from `drawables` (both vectors are
    // CLEARED first). One quad (4 verts / 6 indices) per 2D drawable, NONE per
    // Mesh; the k-th drawable (0-based) gets id k+1 either way. Drawables are
    // already ordered back-to-front, so index order = draw order = front-most
    // last (the 2D half draws depth-off, so the output merger's primitive
    // order decides a contested pixel among the 2D silhouettes; the meshes
    // that follow are depth-tested -- see CollectPickables' ORDER RULE).
    ARC_RENDER_API void BuildPickIdGeometry(std::span<const PickDrawable> drawables,
                                        std::vector<PickIdVertex>& outVertices,
                                        std::vector<uint32_t>& outIndices);
}
