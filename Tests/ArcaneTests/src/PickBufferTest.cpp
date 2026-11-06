// Arcane pickable-collection emitter (Task 2 of the entity-id picking design,
// docs/superpowers/specs/2026-07-19-arcane-entity-id-picking-design.md SS3b/3c).
// CPU-only ([pick]), no GPU: exercises CollectPickables (sprite + collider +
// mesh emitters, WORLD-space since F4 plan 2) and the id<->entity table
// (PickEntityForId).

// THE GRAPH'S PickNode (Render/Nri/nodes/PickOutlineNodes.*) is the only pick
// implementation, and its structural, GPU-free coverage lives in
// RenderGraphTest.cpp. What is in THIS file is the CPU half: the emitter
// numbering and the id<->entity table.
//
// NAMED COVERAGE GAPS, all of them GPU-side and none of them tested anywhere:
//   - Pixel behaviour of the id pass -- front-most-wins id rasterization,
//     1-based id encoding, background/out-of-range handling.
//   - Integer-clear correctness. R32_UINT is the pick format
//     (PickOutlineNodes.hpp) and "0 is background, not a float zero" is
//     explicitly load-bearing there (PickOutlineNodes.cpp).
//   - The 1x1 sub-region readback landing at byte 0, and the hand-rolled
//     row/stride alignment math around it (PickOutlineNodes.cpp).
//   - PickNode::Create/Release's own validation-clean lifecycle
//     (CHECK(RenderErrorCount() == 0) against a real device).
// RenderGraphTest.cpp's PickNode/OutlineNode coverage is entirely structural
// (barriers, pool slots, frame composition against a null context), never a
// real render + readback.

#include <memory>
#include <unordered_map>
#include <utility>   // std::as_const
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/glm.hpp>

#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>        // BuildCube for the mesh drawable case
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/SpriteGeometry.hpp>   // SpriteWorldQuad -- THE corner rule the pick quad must match
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/RenderViewSettings.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>    // SpriteTable / MeshTable -- the resolution the emitter reads
#include <Arcane/Scene/TransformSystems.hpp>

#include "Helpers/TestTypeContext.hpp"

using Catch::Matchers::WithinAbs;

namespace
{
    // The one sprite asset the fixture's SpriteTable resolves: every sprite the
    // helpers spawn names it, so the emitter reads a REAL base size + pivot
    // (the unresolved fallback is 1x1 m at the centre and would hide a pivot
    // bug). The maps are owned HERE, beside the registry, because the tables
    // are transient pointer resources (SceneResources.hpp) that never own.
    constexpr Arcane::Guid kSpriteId{ 5, 5 };

    struct PickFixture
    {
        std::unordered_map<Arcane::Guid, Arcane::SpriteEntry> sprites;
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry>   meshes;
        std::unique_ptr<Astra::Registry>                       reg;

        // The registry, the way the cases spell it.
        Astra::Registry& operator*()  const noexcept { return *reg; }
        Astra::Registry* operator->() const noexcept { return reg.get(); }

        // Publishes `meshes` as the registry's MeshTable, the way the host's
        // MeshCache does (the SpriteTable is published the same way by the
        // maker below).
        void PublishMeshTable() { reg->SetResource(Arcane::MeshTable{ &meshes }); }
    };

    // Fresh registry with Scene + Physics components registered, a
    // zero-gravity Arcane::PhysicsWorld2D attached, and a SpriteTable resolving
    // kSpriteId to (`sizeMeters`, `pivot`).
    //
    // Cross-DLL note (mirrors EntityPickTest.cpp's MakeSceneRegistry):
    // CollectPickables is compiled into Arcane.dll (Arcane/Render/PickEmit.cpp),
    // so its registry.CreateView<WorldTransform, SpriteRenderer>() resolves
    // component IDs through Arcane.dll's per-module TypeContext slot, not the
    // one main() installs in the test module. Pin that DLL slot to the shared
    // test context once (a throwaway Runtime installs it in Arcane.dll; the
    // slot persists after the Runtime is destroyed) so both modules agree on
    // component IDs -- otherwise [pick] run in isolation would see 0 sprites.
    PickFixture MakeRegistryWithSpriteTable(glm::vec2 sizeMeters, glm::vec2 pivot)
    {
        // Belt-and-braces: test_main pins Arcane.dll's TypeContext slot once
        // before any test runs, which is the real guarantee (per-type IDs are
        // cached in per-module magic statics and never re-resolve, so a late
        // pin cannot repair an already-cached id). Re-pinning here only keeps
        // the slot pointed at the shared context; never install an unshared
        // one anywhere in this suite.
        Arcane::Runtime pin(Arcane::Test::Process());

        PickFixture fx;
        auto components = std::make_shared<Astra::ComponentRegistry>();
        fx.reg = std::make_unique<Astra::Registry>(components);
        Arcane::RegisterSceneComponents(*fx.reg);
        Arcane::RegisterPhysicsComponents2D(*fx.reg);

        Manifold2D::Physics::WorldDef wd;
        wd.gravityY = 0.0f;
        wd.gravityX = 0.0f;
        fx.reg->SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));

        Arcane::SpriteEntry entry;
        entry.sizeMeters = sizeMeters;
        entry.pivot      = pivot;
        fx.sprites[kSpriteId] = entry;
        fx.reg->SetResource(Arcane::SpriteTable{ &fx.sprites });
        return fx;
    }

    // A sprite naming the fixture's asset, posed by a full TRS: the WorldTransform
    // is written directly (Transform::ToMatrix -- what TransformPropagationSystem
    // would bake for a root entity), so no propagation pass is needed.
    Astra::Entity AddSprite(Astra::Registry& reg, glm::vec3 pos, glm::quat rot, glm::vec3 scale)
    {
        const Astra::Entity e = reg.CreateEntity();
        Arcane::Transform lt;
        lt.position = pos;
        lt.rotation = rot;
        lt.scale    = scale;
        reg.AddComponent<Arcane::Transform>(e, lt);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{ lt.ToMatrix() });
        Arcane::SpriteRenderer sp;
        sp.sprite = kSpriteId;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
        return e;
    }

    // Transform + WorldTransform + MeshRenderer{ mesh } at `pos`, unturned, unit
    // scale -- the MeshRenderer entity GpuSceneSync would stage once the
    // guid resolves through the MeshTable.
    Astra::Entity AddMesh(Astra::Registry& reg, Arcane::Guid mesh, glm::vec3 pos)
    {
        const Astra::Entity e = reg.CreateEntity();
        Arcane::Transform lt;
        lt.position = pos;
        reg.AddComponent<Arcane::Transform>(e, lt);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{ lt.ToMatrix() });
        Arcane::MeshRenderer mr;
        mr.mesh = mesh;
        reg.AddComponent<Arcane::MeshRenderer>(e, mr);
        return e;
    }

    // Mints a kinematic body with a single fixture at `pos`, via the real
    // Arcane::PhysicsSystem2D create pass (stepWorld=false: registers the body and
    // writes it back without stepping, so it stays exactly at `pos`).
    // `scale` is baked into the body's fixtures by the create pass (it sets
    // Arcane::PhysicsBodyRef2D::appliedScale = lt.scale) -- used to prove CollectPickables
    // scales the silhouette to match the drawn collider.
    Astra::Entity AddBody(Astra::Registry& reg, glm::vec2 pos, const Arcane::Fixture2D& fx,
                          glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        const Astra::Entity e = reg.CreateEntity();

        Arcane::Transform lt;
        lt.position = glm::vec3(pos, 0.0f);
        lt.scale    = glm::vec3(scale, 1.0f);
        reg.AddComponent<Arcane::Transform>(e, lt);

        Arcane::RigidBody2D rb;
        rb.type = Arcane::BodyType2D::Kinematic;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);

        Arcane::Collider2D col;
        col.fixtures.push_back(fx);
        reg.AddComponent<Arcane::Collider2D>(e, col);

        reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});

        Arcane::PhysicsSystem2D sys(1.0f / 60.0f, /*stepWorld=*/false);
        sys(reg);

        return e;
    }

    Astra::Entity AddCircleCollider(Astra::Registry& reg, glm::vec2 pos, float radius)
    {
        Arcane::Fixture2D fx;
        fx.kind   = Arcane::ShapeKind2D::Circle;
        fx.radius = radius;
        return AddBody(reg, pos, fx);
    }

    Astra::Entity AddScaledBoxCollider(Astra::Registry& reg, glm::vec2 pos, float halfW, float halfH,
                                       glm::vec2 scale)
    {
        Arcane::Fixture2D fx;
        fx.kind  = Arcane::ShapeKind2D::Aabb;
        fx.halfW = halfW;
        fx.halfH = halfH;
        return AddBody(reg, pos, fx, scale);
    }
}

TEST_CASE("CollectPickables emits sprites as WORLD quads from SpriteWorldQuad, colliders as world shapes, meshes last", "[pick]")
{
    // A sprite with an off-centre pivot, a mirrored X scale and a Z turn -- the
    // pose PickEmit's old "centre + angle" rule got wrong for a mirrored
    // off-centre pivot (plan 2 handoff). SpriteWorldQuad is the one rule.
    auto reg = MakeRegistryWithSpriteTable(/*sizeMeters=*/{2.0f, 1.0f}, /*pivot=*/{0.25f, 0.0f});
    const Astra::Entity sprite = AddSprite(*reg, glm::vec3(3.0f, 1.0f, 0.5f),
                                           Arcane::RotationAboutZ(0.7f), glm::vec3(-1.5f, 1.0f, 1.0f));
    const Astra::Entity body   = AddCircleCollider(*reg, glm::vec2(-2.0f, 4.0f), /*radius=*/0.5f);
    // The body has a Transform but no WorldTransform (the fixture runs no
    // propagation), so its silhouette z is the emitter's 0 FALLBACK. A second
    // body carries a WorldTransform lifted to z = 0.7 -- the branch that reads
    // the translation column, built the way the sprite helper builds one.
    const Astra::Entity lifted = AddCircleCollider(*reg, glm::vec2(1.0f, 1.0f), /*radius=*/0.25f);
    {
        Arcane::Transform lt;
        lt.position = glm::vec3(1.0f, 1.0f, 0.7f);
        reg->AddComponent<Arcane::WorldTransform>(lifted, Arcane::WorldTransform{ lt.ToMatrix() });
    }

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, out);
    REQUIRE(out.size() == 3);

    // 1. The sprite: kind Quad, corners == SpriteWorldQuad(world, base size, pivot).
    CHECK(out[0].entity == sprite);
    CHECK(out[0].kind == Arcane::PickDrawable::Kind::Quad);
    const glm::mat4 world = std::as_const(*reg).GetComponent<Arcane::WorldTransform>(sprite)->matrix;
    const Arcane::SpriteQuad expected = Arcane::SpriteWorldQuad(world, {2.0f, 1.0f}, {0.25f, 0.0f});
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 3; ++c)
            CHECK_THAT(out[0].corners[i][c], WithinAbs(expected.corners[i][c], 1e-5f));

    // 2. The colliders: world-space circles at the body's pose, in METRES, no
    //    projection anywhere (the id pass projects). Archetype order between
    //    the two bodies is not asserted: find each by entity.
    const auto find = [&](Astra::Entity e) -> const Arcane::PickDrawable&
    {
        for (const auto& d : out)
            if (d.entity == e) return d;
        FAIL("drawable missing");
        return out[0];
    };
    const Arcane::PickDrawable& dBody = find(body);
    CHECK(dBody.kind == Arcane::PickDrawable::Kind::Circle);
    CHECK_THAT(dBody.center.x, WithinAbs(-2.0f, 1e-5f));
    CHECK_THAT(dBody.center.y, WithinAbs(4.0f, 1e-5f));
    CHECK_THAT(dBody.center.z, WithinAbs(0.0f, 1e-5f));   // no WorldTransform: the 0 fallback
    CHECK_THAT(dBody.radius,   WithinAbs(0.5f, 1e-5f));

    const Arcane::PickDrawable& dLifted = find(lifted);
    CHECK(dLifted.kind == Arcane::PickDrawable::Kind::Circle);
    CHECK_THAT(dLifted.center.x, WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(dLifted.center.y, WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(dLifted.center.z, WithinAbs(0.7f, 1e-5f));   // the WorldTransform's translation z
    CHECK_THAT(dLifted.radius,   WithinAbs(0.25f, 1e-5f));
}

TEST_CASE("CollectPickables: a MeshRenderer entity emits ONE Mesh drawable carrying its world matrix and guid, after sprites and colliders", "[pick]")
{
    auto reg = MakeRegistryWithSpriteTable({1.0f, 1.0f}, {0.5f, 0.5f});
    // A MeshTable resource resolving one guid to a cube; an unresolved guid and
    // an empty mesh must NOT emit (nothing is drawn for them either).
    const Arcane::Guid cubeId{ 7, 7 };
    const Arcane::Guid emptyId{ 8, 8 };
    reg.meshes[cubeId].data  = Arcane::BuildCube(1.0f);
    reg.meshes[emptyId].data = Arcane::MeshData{};            // sections empty == empty mesh
    reg.PublishMeshTable();

    const Astra::Entity sprite = AddSprite(*reg, glm::vec3(0.0f), glm::quat(1,0,0,0), glm::vec3(1.0f));
    const Astra::Entity cube   = AddMesh(*reg, cubeId, glm::vec3(1.0f, 2.0f, 3.0f));
    const Astra::Entity empty  = AddMesh(*reg, emptyId, glm::vec3(0.0f));
    const Astra::Entity broken = AddMesh(*reg, Arcane::Guid{ 9, 9 }, glm::vec3(0.0f));
    const Astra::Entity hidden = AddMesh(*reg, cubeId, glm::vec3(5.0f, 0.0f, 0.0f));
    reg->AddComponent<Arcane::Hidden>(hidden, Arcane::Hidden{});

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, out);
    REQUIRE(out.size() == 2);   // the sprite, then the ONE drawable cube
    CHECK(out[0].entity == sprite);
    CHECK(out[1].entity == cube);
    CHECK(out[1].kind == Arcane::PickDrawable::Kind::Mesh);
    CHECK(out[1].mesh == cubeId);
    CHECK_THAT(out[1].world[3].x, WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(out[1].world[3].y, WithinAbs(2.0f, 1e-6f));
    CHECK_THAT(out[1].world[3].z, WithinAbs(3.0f, 1e-6f));
    (void)empty; (void)broken;
    // ...and the id table still inverts through the same k+1 rule.
    CHECK(Arcane::PickPassIdOf(out, cube) == 2u);
    CHECK(Arcane::PickEntityForId(out, 2u) == cube);
}

TEST_CASE("id->entity table maps 1-based, 0 is background", "[pick]")
{
    auto reg = MakeRegistryWithSpriteTable({1.0f, 1.0f}, {0.5f, 0.5f});
    const Astra::Entity e0 = reg->CreateEntity();
    const Astra::Entity e1 = reg->CreateEntity();

    std::vector<Arcane::PickDrawable> table;
    Arcane::PickDrawable d0; d0.entity = e0; table.push_back(d0);
    Arcane::PickDrawable d1; d1.entity = e1; table.push_back(d1);

    CHECK_FALSE(Arcane::PickEntityForId(table, 0).IsValid());   // 0 == background
    CHECK(Arcane::PickEntityForId(table, 1).GetValue() == e0.GetValue());
    CHECK(Arcane::PickEntityForId(table, 2).GetValue() == e1.GetValue());
    CHECK_FALSE(Arcane::PickEntityForId(table, 3).IsValid());   // out of range
}

// The silhouette must match the DRAWN collider, which the physics create pass
// bakes at lt.scale (MakeScaledShape) -- so a scaled body has to pick at its
// scaled size, not its authored size. Aabb scales per-axis (halfW*|sx|,
// halfH*|sy|), mirroring Arcane::PhysicsSystem2D::MakeScaledShape.
TEST_CASE("CollectPickables scales a collider silhouette by the body's baked scale, in metres", "[pick]")
{
    // Arcane::PhysicsBodyRef2D::appliedScale = (2, 3); the box half-extents come out as
    // fixture metres times the baked scale -- no pixels anywhere.
    auto reg = MakeRegistryWithSpriteTable({1.0f, 1.0f}, {0.5f, 0.5f});
    const Astra::Entity body = AddScaledBoxCollider(*reg, glm::vec2(0.0f), /*halfW=*/1.0f, /*halfH=*/0.5f, /*scale=*/{2.0f, 3.0f});
    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].entity == body);
    CHECK(out[0].kind == Arcane::PickDrawable::Kind::Box);
    CHECK_THAT(out[0].halfExtents.x, WithinAbs(2.0f, 1e-5f));
    CHECK_THAT(out[0].halfExtents.y, WithinAbs(1.5f, 1e-5f));
}

// The drawable index IS the hit-proxy id (id = index+1), so the collection order
// must be DETERMINISTIC and must not depend on unordered_map hash layout. Pins
// the documented contract: sprites, then colliders, then meshes, stable across
// repeated collections.
TEST_CASE("CollectPickables orders sprites, then colliders, then meshes, deterministically", "[pick]")
{
    // Two of each, collected twice: identical order both times, and the three
    // groups in that sequence (id = index+1 is the contract every consumer inverts).
    auto reg = MakeRegistryWithSpriteTable({1.0f, 1.0f}, {0.5f, 0.5f});
    const Arcane::Guid cubeId{ 7, 7 };
    reg.meshes[cubeId].data = Arcane::BuildCube(1.0f);
    reg.PublishMeshTable();
    AddMesh(*reg, cubeId, glm::vec3(0.0f)); AddSprite(*reg, glm::vec3(1.0f), glm::quat(1,0,0,0), glm::vec3(1.0f));
    AddCircleCollider(*reg, glm::vec2(2.0f), 0.5f); AddMesh(*reg, cubeId, glm::vec3(3.0f));
    AddSprite(*reg, glm::vec3(4.0f), glm::quat(1,0,0,0), glm::vec3(1.0f)); AddCircleCollider(*reg, glm::vec2(5.0f), 0.5f);
    std::vector<Arcane::PickDrawable> a, b;
    Arcane::CollectPickables(*reg, a); Arcane::CollectPickables(*reg, b);
    REQUIRE(a.size() == 6); REQUIRE(b.size() == 6);
    for (std::size_t i = 0; i < 6; ++i) CHECK(a[i].entity == b[i].entity);
    CHECK(a[0].kind == Arcane::PickDrawable::Kind::Quad);   CHECK(a[1].kind == Arcane::PickDrawable::Kind::Quad);
    CHECK(a[2].kind == Arcane::PickDrawable::Kind::Circle); CHECK(a[3].kind == Arcane::PickDrawable::Kind::Circle);
    CHECK(a[4].kind == Arcane::PickDrawable::Kind::Mesh);   CHECK(a[5].kind == Arcane::PickDrawable::Kind::Mesh);
}

// F3 plan 1 T3: CollectPickables reads MainVisibleSet() the same way the
// sprite sweep does -- nullptr (no SceneVisibility resource) culls nothing,
// which is why the FIRST collection below (before any resource is set) still
// emits both sprites. The fixture needs a SceneRoot + parenting for
// TransformPropagationSystem (VisibilityTest.cpp's World shape); WorldTransform
// is also written directly at spawn (matching what propagation over an
// identity-root subtree would compute) so the first, pre-propagation
// CollectPickables call already sees it -- CollectPickables requires one.
TEST_CASE("CollectPickables: with a SceneVisibility resource only members are emitted; without one everything is", "[pick]")
{
    // Cross-DLL pin (see MakeRegistryWithSpriteTable's note above):
    // CollectPickables/BuildVisibleSet's CreateView<> calls resolve component
    // ids through Arcane.dll's per-module TypeContext slot.
    Arcane::Runtime pin(Arcane::Test::Process());

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{ components };
    Arcane::RegisterSceneComponents(reg);

    std::unordered_map<Arcane::Guid, Arcane::SpriteEntry> sprites;
    Arcane::SpriteEntry entry;
    entry.sizeMeters = { 1.0f, 1.0f };
    entry.pivot      = { 0.5f, 0.5f };
    sprites[kSpriteId] = entry;
    reg.SetResource(Arcane::SpriteTable{ &sprites });

    const Astra::Entity root = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });

    const auto addSprite = [&](glm::vec3 pos) -> Astra::Entity
    {
        const Astra::Entity e = reg.CreateEntity();
        Arcane::Transform t; t.position = pos;
        reg.AddComponent<Arcane::Transform>(e, t);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{ t.ToMatrix() });
        reg.SetParent(e, root);
        Arcane::SpriteRenderer sp;
        sp.sprite = kSpriteId;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
        return e;
    };

    // One sprite at the origin, one far outside any reasonable frustum.
    const Astra::Entity nearEntity = addSprite(glm::vec3(0.0f));
    addSprite(glm::vec3(1000.0f, 0.0f, 0.0f));

    std::vector<Arcane::PickDrawable> all;
    Arcane::CollectPickables(reg, all);
    REQUIRE(all.size() == 2);

    Arcane::TransformPropagationSystem{}(reg);
    Arcane::BoundsSystem{}(reg);
    Arcane::SceneVisibility* sv = reg.EmplaceResource<Arcane::SceneVisibility>();
    REQUIRE(sv);
    sv->views.emplace_back();
    Arcane::BuildVisibleSet(reg, Arcane::Ortho2DView(glm::vec2(0.0f), 5.0f, glm::uvec2{ 800, 600 }), sv->views[0]);

    std::vector<Arcane::PickDrawable> some;
    Arcane::CollectPickables(reg, some);
    REQUIRE(some.size() == 1);
    CHECK(some[0].entity == nearEntity);
}
