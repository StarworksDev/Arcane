// Arcane Editor play-in-editor: snapshot on Play, restore on Stop. CPU-only ([editor]).
//
// Drives a real Arcane::Runtime bound to the process-wide SharedTypeContext (see
// Helpers/TestTypeContext.hpp) -- the same pattern as RuntimeTest.cpp -- rather than
// a test-local Astra::TypeContext. Reason: Runtime's ctor installs the passed context
// as Arcane.dll's per-module TypeContext (Astra::SetTypeContext runs inside the DLL),
// while this test TU's calls (RegisterSceneComponents/AddComponent/GetComponent, all
// header-only Astra code compiled straight into ArcaneTests.exe) resolve TypeIDs
// against ArcaneTests.exe's own per-module context -- already pinned to
// SharedTypeContext() once, at process start, by test_main.cpp. A test-local
// Astra::TypeContext would only be installed in Arcane.dll's module, leaving the
// exe module on SharedTypeContext(): two DIFFERENT TypeContext instances, disagreeing
// on dense component IDs between the Save() (Arcane.dll) and AddComponent (exe) call
// sites. Passing SharedTypeContext() explicitly keeps both modules on the SAME
// instance, exactly like RuntimeTest.cpp's snapshot/restore cases.

#include <catch2/catch_approx.hpp>
#include "Helpers/ModuleNames.hpp"   // fixture module file names per platform
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Sim/SimSettings.hpp>   // ApplySimStepCap
#include <Arcane/Sim/Time.hpp>
#include <Arcane/Scene/TransformSystems.hpp>   // TransformPropagationSystem -- the engine pair's other half

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"

#include <App/PlayMode.hpp>
#include "Documents/DocumentHost.hpp"
#include "Scene/EditGesture.hpp"
#include "Scene/UndoGate.hpp"

TEST_CASE("Play snapshots and Stop restores the authored registry", "[editor]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());

    // Register Scene components on THIS Runtime's ComponentRegistry (a fresh instance
    // per Runtime) so SnapshotRegistry's Registry::Save() knows how to serialize
    // SpriteRenderer -- mirrors RenderInterpolationTest.cpp's fixture.
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);

    const Astra::Entity e = reg.CreateEntity();
    Arcane::SpriteRenderer sp; sp.sortingLayer = 3;
    reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

    Arcane::Editor::PlaySession play;
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);

    REQUIRE(play.Play(runtime));                  // snapshot + unpause
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    // Mutate during play: bump the field on the (possibly re-fetched) entity.
    {
        Astra::Registry& live = runtime.Registry();
        for (Astra::Entity le : live.GetEntityManager())
            if (Arcane::SpriteRenderer* s = live.GetComponent<Arcane::SpriteRenderer>(le))
                s->sortingLayer = 99;
    }

    REQUIRE(play.Stop(runtime));                  // restore + pause
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());

    // The play-time mutation is gone -- back to the authored value.
    Astra::Registry& restored = runtime.Registry();
    bool found = false;
    for (Astra::Entity le : restored.GetEntityManager())
        if (Arcane::SpriteRenderer* s = restored.GetComponent<Arcane::SpriteRenderer>(le))
        { CHECK(s->sortingLayer == 3); found = true; }
    CHECK(found);
}

namespace
{
    // The component descriptor for `typeName` on `entity`, via the same
    // InspectEntity path the Inspector/CommandStackTest use. TypeMeta::typeName
    // is namespace-qualified (e.g. "Arcane::Transform").
    const Astra::ComponentDescriptor* DescriptorFor(Astra::Registry& reg,
                                                    Astra::Entity e, const char* typeName)
    {
        for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            if (ci.meta && ci.meta->typeName == typeName)
                return ci.descriptor;
        return nullptr;
    }
}

// Regression guard for dropping the undo.Clear() on Play (EditorPanels.cpp):
// an Edit-mode CommandStack entry committed BEFORE Play must still Undo/Redo
// correctly AFTER a Play->mutate->Stop round-trip, even though Stop swaps in a
// brand-new Astra::Registry object (Runtime::RestoreRegistry). This only works
// because Registry::Save()/Load() (which SnapshotRegistry/RestoreRegistry ride)
// round-trip the EntityManager and so preserve entity ids/versions -- the same
// property a game plugin's GamePlugin_LoadState relies on to re-resolve SceneRoot
// by saved id after a restore. If this test fails, that assumption is false and
// clearing the undo stack on Play was NOT safe to drop.
TEST_CASE("Edit-mode undo/redo survives a Play/Stop round-trip", "[editor]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);

    const Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt;
    lt.position = glm::vec3(1.0f, 0.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);

    const Astra::ComponentDescriptor* desc = DescriptorFor(reg, e, "Arcane::Transform");
    REQUIRE(desc != nullptr);

    // Resolver-based CommandStack (survives a registry-object swap; see
    // CommandStackTest.cpp's swap regression case), bound to THIS Runtime.
    Arcane::CommandStack stack([&runtime]() -> Astra::Registry& { return runtime.Registry(); });

    // Edit-mode edit, committed BEFORE Play: before={1,0}, after={5,0}.
    const Arcane::TransactionId txn = stack.Begin("edit");
    stack.SnapshotComponent(e, desc);
    reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(5.0f, 0.0f, 0.0f);
    stack.Commit(txn);
    REQUIRE(stack.CanUndo());

    // Play: snapshot captures the authored {5,0} state.
    auto snap = runtime.SnapshotRegistry();
    REQUIRE(snap.IsOk());

    // Play-time mutation: never captured by the stack (the Inspector/gizmo
    // gate capture to Edit mode) -- Stop must discard this, not undo it.
    runtime.Registry().GetComponent<Arcane::Transform>(e)->position = glm::vec3(99.0f, 99.0f, 0.0f);

    // Stop: restore swaps in a NEW registry object built from the snapshot.
    REQUIRE(runtime.RestoreRegistry(*snap.GetValue()));

    // The restore preserved the entity id and its authored {5,0} value.
    Arcane::Transform* lt2 = runtime.Registry().GetComponent<Arcane::Transform>(e);
    REQUIRE(lt2 != nullptr);
    CHECK(lt2->position.x == 5.0f);
    CHECK(lt2->position.y == 0.0f);

    // The core claim: the pre-Play undo entry still resolves and reverts
    // correctly across the registry swap.
    REQUIRE(stack.CanUndo());
    stack.Undo();
    Arcane::Transform* afterUndo = runtime.Registry().GetComponent<Arcane::Transform>(e);
    REQUIRE(afterUndo != nullptr);
    CHECK(afterUndo->position.x == 1.0f);
    CHECK(afterUndo->position.y == 0.0f);

    // Redo survives too.
    REQUIRE(stack.CanRedo());
    stack.Redo();
    Arcane::Transform* afterRedo = runtime.Registry().GetComponent<Arcane::Transform>(e);
    REQUIRE(afterRedo != nullptr);
    CHECK(afterRedo->position.x == 5.0f);
    CHECK(afterRedo->position.y == 0.0f);
}

TEST_CASE("PlaySession Play/Stop are idempotent across repeated calls", "[editor]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);
    reg.CreateEntity();

    Arcane::Editor::PlaySession play;

    // Stop while already in Edit mode: no-op success, no restore attempted.
    CHECK(play.Stop(runtime));
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);

    REQUIRE(play.Play(runtime));
    CHECK(play.IsPlaying());

    // A second Play while already playing is a no-op success (does not re-snapshot).
    CHECK(play.Play(runtime));
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    REQUIRE(play.Stop(runtime));
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());

    // A second Stop while already stopped is a no-op success.
    CHECK(play.Stop(runtime));
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());
}

// Play/Stop route through the hosted module's OWN SaveState/LoadState when one is
// loaded -- the fix for the Stop crash, where the module must re-establish its
// native resources (the physics world) that the raw registry snapshot omits.
//
// DRIVEN THROUGH A REAL PluginHost since the Core-DLL split's Task 7, not the fake
// vtable this case used to build by hand: PlaySession takes the HOST now (it is
// also what attaches/detaches the embedded server world) and reads Vtable() off
// it, so there is no longer a seam a hand-made vtable can be pushed through. The
// module under test is the same HotReloadPluginV1 the [hotreload] suite uses.
//
// WHICH PATH RAN IS MADE DECIDABLE by a TRANSIENT marker resource,
// HotReloadTest::LoadStateMarker, that only the module's OnLoadState sets
// (HotReloadPlugin.cpp). Registry::Save skips transient resources, so no
// snapshot carries the marker and Runtime::RestoreRegistry alone can never
// produce it: a marker present after Stop, counting exactly one load, is a
// statement that the MODULE's LoadState ran -- exactly the property this case
// exists for, since a module's own native resources (the physics world) ride
// that same seam. (The case used SceneRoot as the separator until the input-seam
// gate; SceneRoot rides the registry snapshot, so a plain RestoreRegistry brought
// it back too and it decided nothing -- IN-8. SceneRoot is still checked below,
// as the module re-setting the root it saved, not as the discriminator.)
// On top of that the module's OnLoadState REFUSES (returns false, which Stop
// reports) unless the Pulse entity it re-finds is the saved one, so the REQUIRE on
// Stop's return is a check on the module's extra blob too.
TEST_CASE("PlaySession routes Play/Stop through the hosted module's SaveState/LoadState", "[editor][hotreload]")
{
    using namespace Arcane::HotReloadTest;
    Arcane::Runtime runtime(Arcane::Test::Process());
    runtime.Components()->RegisterComponent<Pulse>();
    runtime.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(runtime));
    REQUIRE(host.Load());                       // OnInit creates the module's Pulse entity

    // The path discriminator (see the comment above) is absent before Play.
    REQUIRE(runtime.Registry().GetResource<LoadStateMarker>() == nullptr);

    // A scene root for the module's SaveState to record and its LoadState to re-set.
    Arcane::RegisterSceneComponents(runtime.Registry());
    const Astra::Entity root = runtime.Registry().CreateEntityWith(Arcane::Transform{});
    runtime.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    const auto readPulse = [&runtime]
    {
        int v = -1;
        runtime.Registry().CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse& p) { v = p.ticks; });
        return v;
    };
    REQUIRE(readPulse() == 0);                  // the AUTHORED value

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime, &host));         // snapshots through the module, then unpauses
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    // Play-time mutation, exactly as a running game would produce.
    CHECK(runtime.Registry().GetResource<LoadStateMarker>() == nullptr);   // Play's SaveState sets nothing
    runtime.Registry().CreateView<Pulse>().ForEach([](Astra::Entity, Pulse& p) { p.ticks = 99; });
    REQUIRE(readPulse() == 99);

    // Stop restores through the module's LoadState and re-pauses. A true here is
    // the module's own verdict on its extra blob (see the comment above).
    REQUIRE(play.Stop(runtime, &host));
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());
    CHECK(readPulse() == 0);                    // the play-time mutation is gone
    // THE discriminator: the module's LoadState ran, exactly once.
    const LoadStateMarker* marker = runtime.Registry().GetResource<LoadStateMarker>();
    REQUIRE(marker != nullptr);
    CHECK(marker->loads == 1);
    // And it re-set the scene root it saved (not a discriminator, see above).
    const Arcane::SceneRoot* sr = runtime.Registry().GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    CHECK(sr->entity == root);

    host.Unload();
}

TEST_CASE("Play lets a body fall; Stop returns it to the authored pose with a fresh world", "[editor][physics]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents2D(reg);
    const Astra::Entity root = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
    reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    const Astra::Entity e = reg.CreateEntity();
    // +Y is UP since F4 plan 1 T2: the authored pose is a metre ABOVE the
    // origin and gravity pulls it to MORE negative y (it used to be the
    // mirror image of this -- y = -1 and a fall towards zero).
    Arcane::Transform lt; lt.position = glm::vec3(0.0f, 1.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Arcane::BodyType2D::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Circle; fx.radius = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.SetParent(e, root);

    // Edit mode: the host's per-frame Ensure + the schedule's edit pass.
    runtime.EnsurePhysics();
    runtime.PhysicsEditPass();

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime));
    for (int i = 0; i < 30; ++i) { runtime.EnsurePhysics(); runtime.Loop().Advance(1.0 / 60.0); }
    {
        Astra::Registry& live = runtime.Registry();
        float y = 1.0f;
        for (Astra::Entity le : live.GetEntityManager())
            if (const auto* rbp = live.GetComponent<Arcane::RigidBody2D>(le))
                if (rbp->type == Arcane::BodyType2D::Dynamic)
                    y = live.GetComponent<Arcane::Transform>(le)->position.y;
        CHECK(y < 0.5f);                            // it fell during Play
    }

    REQUIRE(play.Stop(runtime));                    // restore: the registry is replaced
    CHECK(runtime.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);
    runtime.EnsurePhysics();                        // the next Edit frame
    runtime.PhysicsEditPass();
    // Not proven by comparing world addresses across the restore: the pre-Stop
    // world is freed exactly when RestoreRegistry swaps the registry, and a
    // same-size allocation right after a free routinely reuses that exact
    // address. REQUIRE(res != nullptr) after the CHECK(...== nullptr) above
    // already proves a fresh mint happened -- that is the invariant this pins.
    const auto* res = runtime.Registry().GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(res != nullptr);
    CHECK_FALSE(runtime.Registry().GetResource<Arcane::PhysicsInterpBuffer2D>()->captured);
    Astra::Registry& restored = runtime.Registry();
    float y = 0.0f; int dynamic = 0;
    for (Astra::Entity le : restored.GetEntityManager())
        if (const auto* rbp = restored.GetComponent<Arcane::RigidBody2D>(le))
            if (rbp->type == Arcane::BodyType2D::Dynamic)
            { ++dynamic; y = restored.GetComponent<Arcane::Transform>(le)->position.y; }
    REQUIRE(dynamic == 1);
    CHECK(y == Catch::Approx(1.0f));                // the authored pose
    CHECK(Arcane::Detail::Physics2D::Access::Entities(*res).size() == 1);           // re-minted from it
}

TEST_CASE("Play starts from the AUTHORED state, not the Edit world: an authored velocity survives an earlier drag",
          "[editor][physics]")
{
    // 2026-09-12 review finding 3. Play used to only unpause, so the world the
    // Edit passes had been minting and reconciling carried straight into Play.
    // The paused reconcile (PASS 3.5) zeroes a body's velocity on any Transform
    // divergence -- the right call for "don't fling on resume" -- so an author
    // who set Arcane::RigidBody2D.velocity and THEN dragged the entity got a body at
    // rest in Play, while ArcaneRuntime (which mints fresh at boot and applies
    // rb.velocity in PASS 2) moved it. Editor Play must equal the standalone
    // host's boot: Play drops the Edit world, and the first Play frame's
    // EnsurePhysics mints a fresh one from the authored components.
    Arcane::Runtime runtime(Arcane::Test::Process());
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents2D(reg);
    const Astra::Entity root = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
    reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
    Arcane::PhysicsSettings2D zeroG; zeroG.gravity = glm::vec2(0.0f, 0.0f);   // only the authored velocity moves it
    reg.AddComponent<Arcane::PhysicsSettings2D>(root, zeroG);
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    const Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec3(0.0f, -1.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Arcane::BodyType2D::Dynamic;
    rb.velocity = glm::vec2(5.0f, 0.0f);                                     // the authored velocity
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Circle; fx.radius = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.SetParent(e, root);

    // Edit mode: mint (velocity applied to the body), THEN the author drags
    // the entity -- the paused reconcile teleports the body and zeroes its
    // velocity, exactly as designed.
    runtime.EnsurePhysics();
    runtime.PhysicsEditPass();
    reg.GetComponent<Arcane::Transform>(e)->position.x = 1.0f;   // the drag, stamped by Mut
    runtime.PhysicsEditPass();
    {
        const auto* res = reg.GetResource<Arcane::PhysicsWorld2D>();
        REQUIRE(res != nullptr);
        const auto v = Arcane::Detail::Physics2D::Access::Solver(*res)->Velocity(Arcane::Detail::Physics2D::Access::Entities(*res).at(e));
        REQUIRE(static_cast<float>(v.x) == Catch::Approx(0.0f));   // the Edit world's body is at rest
    }
    REQUIRE(reg.GetComponent<Arcane::RigidBody2D>(e)->velocity.x == Catch::Approx(5.0f));   // the AUTHORED value stands

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime));
    // The pin on the mechanism, symmetric with Stop's: the Edit world is gone
    // and the first Play frame's Ensure mints a fresh one.
    CHECK(runtime.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);
    for (int i = 0; i < 30; ++i) { runtime.EnsurePhysics(); runtime.Loop().Advance(1.0 / 60.0); }

    // The pin on the behaviour: half a second at 5 m/s from x = 1 -- the
    // authored velocity was applied, as ArcaneRuntime would have.
    const float x = runtime.Registry().GetComponent<Arcane::Transform>(e)->position.x;
    CHECK(x > 1.5f);

    REQUIRE(play.Stop(runtime));
}

namespace
{
    // The physics demo's shape, built into whatever registry the Runtime holds
    // NOW: a scene root and one dynamic circle a metre above the origin --
    // y = +1 since the +Y flip (F4 plan 1 T2); it falls towards negative y.
    Astra::Entity BuildFallingBody(Arcane::Runtime& runtime)
    {
        Astra::Registry& reg = runtime.Registry();
        Arcane::RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents2D(reg);
        const Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        const Astra::Entity e = reg.CreateEntity();
        Arcane::Transform lt; lt.position = glm::vec3(0.0f, 1.0f, 0.0f);
        reg.AddComponent<Arcane::Transform>(e, lt);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        Arcane::RigidBody2D rb; rb.type = Arcane::BodyType2D::Dynamic;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);
        Arcane::Collider2D col; Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Circle; fx.radius = 0.5f;
        col.fixtures.push_back(fx);
        reg.AddComponent<Arcane::Collider2D>(e, col);
        reg.SetParent(e, root);
        return e;
    }

    // One editor frame's sim advance, exactly as EditorApp::AdvanceSim makes
    // it: the per-frame Ensure, then the loop with the plugin callbacks (none
    // here). Whether fixedUpdate -- and so Arcane::PhysicsSystem2D -- runs is the loop's
    // paused flag's decision alone, which is the whole point of the case below.
    void EditorFrame(Arcane::Runtime& runtime)
    {
        runtime.EnsurePhysics();
        runtime.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
    }
}

TEST_CASE("opening a scene in Edit mode does not simulate it: bodies hold their authored pose until Play, and Stop returns them to it",
          "[editor][physics]")
{
    // 2026-09-12 desk finding: open physics.arcscene from the Asset Browser
    // and the bodies fell at once, in Edit mode, and never came back. The
    // editor opens a scene through Runtime::ResetRegistry (DoOpenScene), and
    // ResetRegistry rebinds the RunLoop, which used to reset `paused` to a
    // fresh loop's default -- RUNNING. The next AdvanceSim ran fixedUpdate's
    // Arcane::PhysicsSystem2D(stepWorld=true) in "Edit" mode, so the bodies fell; Play
    // then snapshotted the fallen poses and Stop restored them. The boot scene
    // never showed it because boot pauses AFTER loading it.
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::Editor::PlaySession play;

    // The editor's boot: a scene loaded, then Edit mode (paused) -- the state
    // the golden lanes see, which is why they never caught this.
    BuildFallingBody(runtime);
    runtime.Loop().SetPaused(true);
    REQUIRE(runtime.Loop().IsPaused());

    // The user opens ANOTHER scene: DoOpenScene's ResetRegistry, then the
    // scene's content. Edit mode must survive the swap.
    runtime.ResetRegistry();
    const Astra::Entity e = BuildFallingBody(runtime);
    CHECK(runtime.Loop().IsPaused());

    // Thirty Edit-mode frames: EnsurePhysics mints the world (the Edit pass
    // would mint the body; AdvanceSim's loop must not step it).
    for (int i = 0; i < 30; ++i) EditorFrame(runtime);
    const float yEdit = std::as_const(runtime.Registry()).GetComponent<Arcane::Transform>(e)->position.y;
    CHECK(yEdit == Catch::Approx(1.0f));            // it did NOT fall in Edit mode

    // Play: NOW it falls.
    REQUIRE(play.Play(runtime));
    for (int i = 0; i < 30; ++i) EditorFrame(runtime);
    const float yPlay = std::as_const(runtime.Registry()).GetComponent<Arcane::Transform>(e)->position.y;
    CHECK(yPlay < 0.5f);

    // Stop: back to the AUTHORED pose, not the fallen one -- and paused.
    REQUIRE(play.Stop(runtime));
    CHECK(runtime.Loop().IsPaused());
    float yStop = 0.0f; int dynamic = 0;
    Astra::Registry& restored = runtime.Registry();
    for (Astra::Entity le : restored.GetEntityManager())
        if (const auto* rbp = std::as_const(restored).GetComponent<Arcane::RigidBody2D>(le))
            if (rbp->type == Arcane::BodyType2D::Dynamic)
            { ++dynamic; yStop = std::as_const(restored).GetComponent<Arcane::Transform>(le)->position.y; }
    REQUIRE(dynamic == 1);
    CHECK(yStop == Catch::Approx(1.0f));
    // And it STAYS put across further Edit-mode frames after the restore.
    for (int i = 0; i < 30; ++i) EditorFrame(runtime);
    yStop = 0.0f;
    for (Astra::Entity le : runtime.Registry().GetEntityManager())
        if (const auto* rbp = std::as_const(runtime.Registry()).GetComponent<Arcane::RigidBody2D>(le))
            if (rbp->type == Arcane::BodyType2D::Dynamic)
                yStop = std::as_const(runtime.Registry()).GetComponent<Arcane::Transform>(le)->position.y;
    CHECK(yStop == Catch::Approx(1.0f));
}

// ---- Core-DLL split, plan 1 Task 7: the three play TOPOLOGIES ---------------
// PlaySession no longer has one shape. Standalone is what every case above
// drives; ListenServer re-roles the ONE world; EmbeddedServer stands a SECOND
// DedicatedServer world up beside it on the SAME ProcessContext (spec s7), and
// ClientOnly is that same world under a separate SERVER PROCESS (whose spawn is
// ServerLaunchTest's/desk territory, not this file's).

namespace
{
    std::size_t CountEntities(Arcane::Runtime& rt)
    { std::size_t n = 0; for (Astra::Entity e : rt.Registry().GetEntityManager()) { (void)e; ++n; } return n; }
}

TEST_CASE("Play as embedded server stands up a second DedicatedServer world on the same ProcessContext with the same scene; Stop tears it down", "[editor][netmode]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::RegisterSceneComponents(runtime.Registry());
    const Astra::Entity root = runtime.Registry().CreateEntityWith(Arcane::Transform{});
    runtime.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    runtime.Registry().CreateEntityWith(Arcane::Transform{});
    const std::size_t authored = CountEntities(runtime);

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime, nullptr, Arcane::Editor::PlayTopology::EmbeddedServer));
    Arcane::Runtime* server = play.ServerWorld();
    REQUIRE(server != nullptr);
    CHECK(server->Mode() == Arcane::NetMode::DedicatedServer);
    CHECK(server->HasAuthority());
    CHECK(&server->Process() == &runtime.Process());          // shares the ProcessContext and nothing else
    CHECK(server != &runtime);
    CHECK(CountEntities(*server) == authored);                // the same scene, restored registry-only
    CHECK(runtime.Mode() == Arcane::NetMode::Client);
    CHECK_FALSE(runtime.HasAuthority());
    CHECK_FALSE(server->Loop().IsPaused());
    for (int i = 0; i < 5; ++i) play.TickServer(1.0 / 60.0);  // ticks independently of the editor's world
    CHECK(server->Loop().IsPaused() == false);

    REQUIRE(play.Stop(runtime));
    CHECK(play.ServerWorld() == nullptr);
    CHECK(runtime.Mode() == Arcane::NetMode::Standalone);
    CHECK(runtime.Loop().IsPaused());
    CHECK(CountEntities(runtime) == authored);
}

// Settings arc S6-8 (fix round 1): sim.maxStepsPerFrame is Live, and the
// editor's primary loop takes it each frame (EditorAppFrame). The embedded
// server world advances on the SAME clamped dt, so it must take the same cap
// or a hitch steps the client world N times and the authority M times. The
// server Runtime's ctor read the cap at Play; a change DURING Play has to
// reach it on the next TickServer.
TEST_CASE("Play as embedded server: the server world takes the live sim.maxStepsPerFrame each tick like the primary loop", "[editor][netmode][sim]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::RegisterSceneComponents(runtime.Registry());
    const Astra::Entity root = runtime.Registry().CreateEntityWith(Arcane::Transform{});
    runtime.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime, nullptr, Arcane::Editor::PlayTopology::EmbeddedServer));
    Arcane::Runtime* server = play.ServerWorld();
    REQUIRE(server != nullptr);
    REQUIRE(server->Loop().FixedHz() == 60.0);
    REQUIRE(server->Loop().MaxStepsPerFrame() == 5);   // the published default, read at construction

    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const Arcane::CVarHandle h = reg.Find("sim.maxStepsPerFrame");
    struct ClearCodeRung
    {
        Arcane::CVarHandle handle;
        ~ClearCodeRung()
        {
            Arcane::CVarRegistry::Get().ClearRung(handle, Arcane::SetBy::Code);
            Arcane::CVarRegistry::Get().PublishImmediate();
        }
    } restore{ h };
    REQUIRE(reg.Set(h, Arcane::CVarValue::Int32(10), Arcane::SetBy::Code) == Arcane::SetResult::Applied);
    reg.PublishImmediate();

    // What the editor frame does to the primary loop before its Advance.
    Arcane::ApplySimStepCap(runtime.Loop());
    CHECK(runtime.Loop().MaxStepsPerFrame() == 10);

    // A 0.2 s hitch owes 12 steps at 60 Hz: both worlds run exactly 10.
    play.TickServer(0.2);
    CHECK(server->Loop().MaxStepsPerFrame() == 10);
    const Arcane::Time* t = server->Registry().GetResource<Arcane::Time>();
    REQUIRE(t != nullptr);
    CHECK(t->fixedStep == 10);   // was 5 when the server loop kept its ctor-read cap

    REQUIRE(play.Stop(runtime));
}

TEST_CASE("Play as listen server flips the ONE world to ListenServer; Stop restores Standalone", "[editor][netmode]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime, nullptr, Arcane::Editor::PlayTopology::ListenServer));
    CHECK(runtime.Mode() == Arcane::NetMode::ListenServer);
    CHECK(runtime.HasAuthority());
    CHECK(play.ServerWorld() == nullptr);                     // one world, dual role (spec R3)
    REQUIRE(play.Stop(runtime));
    CHECK(runtime.Mode() == Arcane::NetMode::Standalone);
}

TEST_CASE("Play as embedded server with a loaded module: the server world gets the Server-masked system, the editor world the Client-masked", "[editor][netmode][hotreload]")
{
    using namespace Arcane::HotReloadTest;
    Arcane::Runtime runtime(Arcane::Test::Process());
    runtime.Components()->RegisterComponent<Pulse>(); runtime.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    host.AttachRuntime(runtime);
    REQUIRE(host.Load());
    // The BEHAVIOUR probe both worlds stamp into (RoleCounters, HotReloadShared.hpp):
    // one entity carrying it, in the AUTHORED scene, so the registry-only seed
    // carries it into the server world too.
    runtime.Registry().CreateEntityWith(RoleCounters{});

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime, &host, Arcane::Editor::PlayTopology::EmbeddedServer));
    REQUIRE(play.ServerWorld() != nullptr);
    CHECK(host.Runtimes().size() == 2);
    CHECK(play.ServerWorld()->Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK_FALSE(play.ServerWorld()->Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK(runtime.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK_FALSE(runtime.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());

    // PRESENCE IS NOT EXECUTION (final-review fix wave, minor 13). TickServer drives
    // the server world's OWN loop; the Server-masked system must actually run there,
    // and the Client-masked one must not -- which is what separates "the right
    // factories were instantiated" from "the right systems are doing the work".
    for (int i = 0; i < 5; ++i) play.TickServer(1.0 / 60.0);
    const auto serverCounters = [&]
    {
        RoleCounters c{};
        play.ServerWorld()->Registry().CreateView<RoleCounters>()
            .ForEach([&](Astra::Entity, RoleCounters& r) { c = r; });
        return c;
    }();
    CHECK(serverCounters.serverTicks > 0);
    CHECK(serverCounters.clientTicks == 0);

    REQUIRE(play.Stop(runtime, &host));
    CHECK(host.Runtimes().size() == 1);
    // Back to Standalone, so the editor world's fixedUpdate carries BOTH masks again
    // -- the module's pair, alongside the engine's own Arcane::PhysicsSystem2D and
    // TransformPropagationSystem (Runtime::InstallEngineSystems, ABI 29), which the
    // SetNetMode clear-and-reinstantiate must not have dropped.
    CHECK(runtime.Mode() == Arcane::NetMode::Standalone);
    CHECK(runtime.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK(runtime.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK(runtime.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>());
    CHECK(runtime.Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    host.Unload();
}

TEST_CASE("a Play topology flip moves the NETMODE the MODULE sees, not only the world's", "[editor][netmode][hotreload]")
{
    // Final-review fix wave, I1. Runtime::SetNetMode moves the WORLD's mode, but a
    // module branches on EngineContext::netMode -- the struct PluginHost fills, and
    // used to fill ONLY on its load/reload/attach paths. PlaySession flips the
    // primary's mode on every Play and Stop, so without a refresh a module asked "do
    // I have authority?" answered Standalone (yes) while its world was a Client (no):
    // the whole point of the field, inverted, silently.
    //
    // PluginHost::Context() is the read-only/diagnostic accessor this reads through.
    using namespace Arcane::HotReloadTest;
    Arcane::Runtime runtime(Arcane::Test::Process());
    runtime.Components()->RegisterComponent<Pulse>();
    runtime.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(runtime));
    REQUIRE(host.Load());

    const Arcane::EngineContext* ctx = host.Context();
    REQUIRE(ctx != nullptr);
    REQUIRE(ctx->engine == &runtime);
    CHECK(ctx->netMode == Arcane::NetMode::Standalone);      // the load-path value

    Arcane::Editor::PlaySession play;

    // ListenServer: one world, both roles.
    REQUIRE(play.Play(runtime, &host, Arcane::Editor::PlayTopology::ListenServer));
    CHECK(runtime.Mode() == Arcane::NetMode::ListenServer);
    CHECK(ctx->netMode == Arcane::NetMode::ListenServer);    // and the module sees it
    REQUIRE(play.Stop(runtime, &host));
    CHECK(ctx->netMode == Arcane::NetMode::Standalone);      // and sees it come back

    // ClientOnly: the world loses authority, and so must the module's view of it --
    // this is the pairing that used to read "Standalone" over a Client world.
    REQUIRE(play.Play(runtime, &host, Arcane::Editor::PlayTopology::ClientOnly));
    CHECK(runtime.Mode() == Arcane::NetMode::Client);
    CHECK_FALSE(runtime.HasAuthority());
    CHECK(ctx->netMode == Arcane::NetMode::Client);
    REQUIRE(play.Stop(runtime, &host));
    CHECK(ctx->netMode == Arcane::NetMode::Standalone);

    // EmbeddedServer re-roles the PRIMARY to Client too (the authority is the second
    // world), so the module's view follows it there as well.
    REQUIRE(play.Play(runtime, &host, Arcane::Editor::PlayTopology::EmbeddedServer));
    CHECK(ctx->netMode == Arcane::NetMode::Client);
    CHECK(ctx->engine == &runtime);                          // still the PRIMARY, not the server world
    REQUIRE(play.Stop(runtime, &host));
    CHECK(ctx->netMode == Arcane::NetMode::Standalone);

    host.Unload();
}

TEST_CASE("Play as embedded server is REFUSED when the host will not take the server world: no half-built world is left behind", "[editor][netmode]")
{
    // The binding invariant's own refusal path (spec s4; review round 1, I1).
    // PlaySession builds the server world on `runtime.Components()`, so a Play
    // driven against a world that is NOT the host's primary hands AttachRuntime a
    // secondary with a ComponentRegistry the primary does not share -- which it
    // refuses. A refused attach must unwind the whole Play: a server world the
    // module does not serve has no gameplay in it, and leaving one standing would
    // be a silently inert second world.
    using namespace Arcane::HotReloadTest;
    Arcane::Runtime primary(Arcane::Test::Process());
    Arcane::Runtime other(Arcane::Test::Process());            // its OWN ComponentRegistry
    REQUIRE(other.Components() != primary.Components());
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(primary));                      // primary = the first attach

    Arcane::Editor::PlaySession play;
    CHECK_FALSE(play.Play(other, &host, Arcane::Editor::PlayTopology::EmbeddedServer));
    CHECK(play.ServerWorld() == nullptr);                      // nothing half-built survives
    CHECK(play.Topology() == Arcane::Editor::PlayTopology::Standalone);
    CHECK_FALSE(play.IsPlaying());                             // the session stayed in Edit
    CHECK(host.Runtimes().size() == 1);                        // the host is unchanged
    CHECK(other.Mode() == Arcane::NetMode::Standalone);        // and so is the would-be client
}

TEST_CASE("an embedded-server session Stopped before its PlaySession dies leaves the host exactly one world, and Unload runs clean", "[editor][netmode][hotreload]")
{
    // The EXIT-PATH contract, device-free (review round 1, C1). PluginHost holds a
    // RAW pointer to every attached world, and PlaySession now OWNS one -- so the
    // session must be Stopped while the host is still alive, which is precisely
    // what EditorApp::Shutdown does (ahead of the member teardown that would
    // otherwise free the server world out from under ~PluginHost). This case pins
    // the ORDER that makes that safe: Stop detaches and destroys the server world,
    // and the host is then left with exactly the one world it began with, so its
    // own teardown touches nothing that is gone. The EditorApp exit path itself is
    // only reachable through a real editor process -- E1 ([witness][gpu],
    // --play-as embedded-server, no explicit Stop) is the witness for that half.
    using namespace Arcane::HotReloadTest;
    Arcane::Runtime runtime(Arcane::Test::Process());
    runtime.Components()->RegisterComponent<Pulse>();
    runtime.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(runtime));
    REQUIRE(host.Load());
    {
        Arcane::Editor::PlaySession play;
        REQUIRE(play.Play(runtime, &host, Arcane::Editor::PlayTopology::EmbeddedServer));
        REQUIRE(host.Runtimes().size() == 2);
        REQUIRE(play.Stop(runtime, &host));                    // Shutdown's call, in Shutdown's position
        CHECK(play.ServerWorld() == nullptr);
        CHECK(host.Runtimes().size() == 1);
    }                                                          // ~PlaySession: nothing left to free
    CHECK(host.Runtimes().size() == 1);
    CHECK(host.Runtimes()[0] == &runtime);
    host.Unload();                                             // clean: no freed world in the loop
    CHECK_FALSE(host.IsLoaded());
}

namespace
{
    struct DocStepNote final : Arcane::ICommand
    {
        void Undo() override {}
        void Redo() override {}
        const char* Label() const override { return "joined"; }
        bool AffectsScene() const override { return false; }
    };

    struct GestureDoc final : Arcane::Editor::EditorDocument
    {
        std::string title = "gesture.arcmat";
        Arcane::Editor::UndoResolver undo;
        Arcane::Editor::EditGesture::GestureState gesture;
        const std::string& Title() const override { return title; }
        Arcane::Guid AssetGuid() const override { return {}; }
        bool Dirty() const override { return false; }
        bool Save() override { return true; }
        void Draw(bool&) override {}
        void NoteMoved(const std::filesystem::path&) override {}
        void FlushGesture() override   // the documents' real override, verbatim
        {
            if (Arcane::CommandStack* s = undo ? undo() : nullptr)
                Arcane::Editor::EditGesture::ClosePending(*s, gesture);
        }
    };
}

TEST_CASE("Play entry flushes an open document drag into ONE Edit-mode step first", "[editor][undo]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::RegisterSceneComponents(runtime.Registry());
    Arcane::CommandStack stack([&runtime]() -> Astra::Registry& { return runtime.Registry(); });
    Arcane::Editor::PlaySession play;
    Arcane::Editor::DocumentHost host;
    auto* doc = static_cast<GestureDoc*>(host.Add(std::make_unique<GestureDoc>()));
    doc->undo = [&]() { return Arcane::Editor::ResolveDocumentUndo(play.IsPlaying(), &stack); };

    // A drag mid-flight: the gesture owns the open transaction and parks its step.
    doc->gesture.slots.txn = stack.Begin("Edit Roughness");
    doc->gesture.pendingCommit = [&stack] { stack.Push(std::make_unique<DocStepNote>()); };
    REQUIRE(stack.InTransaction());

    host.FlushGestures();                       // the toolbar's beforePlay, BEFORE play.Play
    CHECK_FALSE(stack.InTransaction());
    REQUIRE(stack.CanUndo());
    CHECK(std::string(stack.UndoLabel()) == "Edit Roughness");
    REQUIRE(play.Play(runtime));
    CHECK(doc->undo() == nullptr);              // during Play nothing is pushed

    const std::uint64_t id = stack.StateId();
    host.FlushGestures();                       // nothing open: no new step
    CHECK(stack.StateId() == id);
    REQUIRE(play.Stop(runtime));
    CHECK(doc->undo() == &stack);
}
