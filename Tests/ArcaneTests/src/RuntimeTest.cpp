// Runtime is the engine facade EngineContext.engine points at. It owns the substrate
// that outlives reloads: TypeContext (installed in Arcane.dll), persistent
// ComponentRegistry, a swappable Registry, schedulers, RunLoop, and the JobSystem.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Render/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Serialization/RegistrySnapshot.hpp>
#include <Arcane/Serialization/ResourceSerialization.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <Astra/Core/TypeID.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace { struct Counter { int value = 0; }; }
namespace { ASTRA_REFLECT_TYPE(Counter) ASTRA_REFLECT_FIELD(Counter, value) ASTRA_END_REFLECT_TYPE() }

// A void(Registry&) LAMBDA does NOT satisfy Astra's LambdaLike (that concept is for
// per-entity lambdas); systems must be NAMED types registered via AddSystem<T>().
namespace { struct NoOpSystem { void operator()(Astra::Registry&) const {} }; }

TEST_CASE("Runtime boots a usable substrate", "[runtime]")
{
    // A ClientRuntime, because the audio device is presentation since the Core-DLL
    // split -- everything else here is the headless substrate, reached through the
    // P5 aliases (which is itself the proof they forward to the owned Runtime).
    Arcane::ClientRuntime rt(Arcane::Test::Process());
    REQUIRE(rt.TypeContext() != nullptr);
    REQUIRE(rt.WorkScheduler() != nullptr);
    REQUIRE(rt.WorkScheduler()->WorkerCount() >= 1);
    REQUIRE(rt.AudioSystem().IsInitialized());
    CHECK(rt.AssetsFacade().Stats().count == 0);
    REQUIRE(rt.TaskExecutor() != nullptr);
    REQUIRE(rt.TaskExecutor()->WorkerCount() >= 1);

    rt.Components()->RegisterComponent<Counter>();
    auto& reg = rt.Registry();
    for (int i = 0; i < 8; ++i) reg.CreateEntityWith(Counter{i});

    int seen = 0;
    reg.CreateView<Counter>().ForEach([&](Astra::Entity, Counter&) { ++seen; });
    CHECK(seen == 8);
}

TEST_CASE("Runtime resets audio without disturbing the engine substrate", "[runtime][audio]")
{
    Arcane::ClientRuntime rt(Arcane::Test::Process());
    REQUIRE(rt.AudioSystem().IsInitialized());

    rt.ResetAudio();

    CHECK(rt.AudioSystem().IsInitialized());
    CHECK(rt.WorkScheduler() != nullptr);
    CHECK(rt.Registry().IsEmpty());
}

TEST_CASE("Runtime snapshot/restore preserves state AND the scheduler", "[runtime]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Counter>();
    auto& reg = rt.Registry();
    constexpr int kN = 2048;
    for (int i = 0; i < kN; ++i) reg.CreateEntityWith(Counter{7});

    auto snapResult = rt.SnapshotRegistry();
    REQUIRE(snapResult.IsOk());
    std::vector<std::byte>& snap = *snapResult.GetValue();
    REQUIRE(!snap.empty());

    // Mutate the live registry, then restore the snapshot.
    reg.CreateView<Counter>().ForEach([](Astra::Entity, Counter& c) { c.value = 0; });
    REQUIRE(rt.RestoreRegistry(snap));

    std::atomic<int> visited{0};
    std::atomic<int> sum{0};
    rt.Registry().CreateView<Counter>().ParallelForEach([&](Astra::Entity, Counter& c) {
        visited.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(c.value, std::memory_order_relaxed);
    });
    CHECK(visited.load() == kN);     // state survived
    CHECK(sum.load() == 7 * kN);     // values survived (== 7, not the mutated 0)
}

TEST_CASE("Runtime RestoreRegistry keeps the RunLoop object stable", "[runtime]")
{
    // A restore swaps the live registry, which the RunLoop references. It MUST rebind
    // the existing loop in place, NOT destroy + recreate it: consumers cache the
    // RunLoop* handed out by Runtime::Loop() at init (a plugin's SetLoop, a host
    // toolbar), and recreating would leave every such pointer dangling -> a
    // use-after-free the next time they touch it. This pins the "same object survives a
    // restore" invariant.
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Counter>();
    rt.Registry().CreateEntityWith(Counter{7});

    Arcane::RunLoop* before = &rt.Loop();
    rt.Loop().SetPaused(true);           // observable pre-restore state

    auto snapResult = rt.SnapshotRegistry();
    REQUIRE(snapResult.IsOk());
    REQUIRE(rt.RestoreRegistry(*snapResult.GetValue()));

    Arcane::RunLoop* after = &rt.Loop();
    CHECK(before == after);              // SAME object -> a cached RunLoop* stays valid
    // Paused SURVIVES the rebind. It is the HOST'S MODE (Edit vs Play), not
    // per-registry sim state: the accumulator, alpha and a pending single-step
    // belong to the old registry's step backlog and reset, but a registry swap
    // must never silently flip the host into Play. This line used to pin the
    // opposite ("rebind resets to a fresh loop's defaults", paused included),
    // which the editor's Stop masked by re-pausing after its restore -- and
    // which its scene-open (ResetRegistry, no re-pause) did not, so opening a
    // physics scene in Edit mode ran fixedUpdate's Arcane::PhysicsSystem2D and the
    // bodies fell before Play was ever pressed (2026-09-12 desk finding).
    CHECK(after->IsPaused());

    // ResetRegistry holds both invariants.
    rt.ResetRegistry();
    CHECK(&rt.Loop() == before);
    CHECK(rt.Loop().IsPaused());
}

TEST_CASE("Runtime ClearSystems empties the module's systems and re-installs the engine's", "[runtime]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    // Registration must actually succeed, or Empty() below would already be
    // true before ClearSystems() runs and the test would pass vacuously.
    REQUIRE(rt.Schedulers().fixedUpdate.AddSystem<NoOpSystem>().IsOk());   // each scheduler has its own
    REQUIRE(rt.Schedulers().update.AddSystem<NoOpSystem>().IsOk());        // type index, so reusing the
    REQUIRE(rt.Schedulers().render.AddSystem<NoOpSystem>().IsOk());        // same type across them is fine
    rt.ClearSystems();
    // The engine-owned STANDARD systems come back (Arcane::PhysicsSystem2D 2026-09-11;
    // TransformPropagation 2026-09-13, game-module boilerplate spec s4.1;
    // BoundsSystem 2026-09-18, F3 plan 1 T2); the module's NoOpSystems are
    // gone. RenderSubmissionSystem was a fixedUpdate sibling until the
    // Core-DLL split moved it to ClientRuntime, so on a BARE Runtime the
    // render scheduler comes back empty -- the client's half of this is
    // ClientRuntimeTest's "keeps render submission across ClearSystems".
    CHECK(rt.Schedulers().fixedUpdate.Size() == 3);
    CHECK_FALSE(rt.Schedulers().fixedUpdate.HasSystem<NoOpSystem>());
    CHECK(rt.Schedulers().update.Empty());
    CHECK(rt.Schedulers().render.Empty());
}

// ---- 2D physics wiring Plan 1 Task 6: the engine-owned physics facade ------

TEST_CASE("Runtime installs Arcane::PhysicsSystem2D into fixedUpdate and re-installs after ClearSystems", "[runtime][physics]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>());
    CHECK_FALSE(rt.Schedulers().update.HasSystem<Arcane::PhysicsSystem2D>());
    rt.InstallEngineSystems();                                   // idempotent
    CHECK(rt.Schedulers().fixedUpdate.Size() == 3);              // Physics + TransformPropagation + Bounds, all engine-owned (F3 plan 1 T2)
    REQUIRE(rt.Schedulers().fixedUpdate.AddSystem<NoOpSystem>().IsOk());   // "the module's"
    rt.ClearSystems();                                           // what PluginHost does on every unload/reload
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>());
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::BoundsSystem>());   // F3 plan 1 T2: the third engine-owned system
    CHECK_FALSE(rt.Schedulers().fixedUpdate.HasSystem<NoOpSystem>());
    CHECK(rt.Schedulers().update.Empty());
    // Headless: RenderSubmissionSystem is the client's since the Core-DLL split.
    CHECK_FALSE(rt.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
}

TEST_CASE("EnsurePhysics mints a world once and again after RestoreRegistry", "[runtime][physics]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    CHECK(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);
    rt.EnsurePhysics();
    const auto* res = rt.Registry().GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(res != nullptr);
    REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res) != nullptr);
    REQUIRE(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer2D>() != nullptr);
    CHECK_FALSE(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer2D>()->captured);
    const Manifold2D::Physics::PhysicsWorld* first = Arcane::Detail::Physics2D::Access::Solver(*res);
    rt.EnsurePhysics();                                          // same frame, same settings: no re-mint
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*rt.Registry().GetResource<Arcane::PhysicsWorld2D>()) == first);
    CHECK(static_cast<float>(first->Gravity().y) == Catch::Approx(-9.81f));   // the engine default, no project (+Y up, F4)

    auto bytes = rt.SnapshotRegistry();
    REQUIRE(bytes.IsOk());
    REQUIRE(rt.RestoreRegistry(*bytes.GetValue()));              // replaces the registry: resources are gone
    // No world-less zombie of either: both are transient resources
    // (AstraTransientResource), so the snapshot never carried them.
    CHECK(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);
    CHECK(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer2D>() == nullptr);
    rt.EnsurePhysics();
    // Not compared against `first` by address: `first` now points at freed
    // memory, and a same-size allocation immediately after a free routinely
    // reuses that exact address (observed in practice) -- a coincidental match
    // would not mean this is the OLD world. REQUIRE(!= nullptr) above already
    // proves the resource re-minted; that is the invariant this pins.
    REQUIRE(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() != nullptr);
}

TEST_CASE("ResolvedGravity: the engine default, then the scene-root Arcane::PhysicsSettings2D override", "[runtime][physics]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    CHECK(rt.ResolvedGravity().y == Catch::Approx(-9.81f));      // no project open: physics.gravity default (+Y up, F4)

    Astra::Registry& reg = rt.Registry();
    const Astra::Entity root  = reg.CreateEntity();
    const Astra::Entity other = reg.CreateEntity();
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    Arcane::PhysicsSettings2D ps; ps.gravity = glm::vec2(0.0f, 2.0f);
    reg.AddComponent<Arcane::PhysicsSettings2D>(other, ps);        // NOT the root: ignored
    CHECK(rt.ResolvedGravity().y == Catch::Approx(-9.81f));
    reg.AddComponent<Arcane::PhysicsSettings2D>(root, ps);
    CHECK(rt.ResolvedGravity().y == Catch::Approx(2.0f));

    rt.EnsurePhysics();
    const auto* res = rt.Registry().GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(res != nullptr);
    CHECK(static_cast<float>(Arcane::Detail::Physics2D::Access::Solver(*res)->Gravity().y) == Catch::Approx(2.0f));
    // A gravity edit re-mints the world (no SetGravity on the vendored
    // PhysicsWorld; spec s4.3 amended): the next Ensure carries it.
    const auto* before = Arcane::Detail::Physics2D::Access::Solver(*res);
    reg.GetComponent<Arcane::PhysicsSettings2D>(root)->gravity.y = 5.0f;
    rt.EnsurePhysics();
    res = rt.Registry().GetResource<Arcane::PhysicsWorld2D>();
    CHECK(Arcane::Detail::Physics2D::Access::Solver(*res) != before);
    CHECK(static_cast<float>(Arcane::Detail::Physics2D::Access::Solver(*res)->Gravity().y) == Catch::Approx(5.0f));
}

TEST_CASE("PhysicsEditPass mints bodies, moves none, captures nothing", "[runtime][physics]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Astra::Registry& reg = rt.Registry();
    const Astra::Entity root = reg.CreateEntity();
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    const Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec3(0.0f, -1.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Arcane::BodyType2D::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Circle; fx.radius = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);

    rt.EnsurePhysics();
    for (int i = 0; i < 5; ++i) rt.PhysicsEditPass();
    const auto* res = rt.Registry().GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(Arcane::Detail::Physics2D::Access::Entities(*res).count(e) == 1);                    // minted (Arcane::PhysicsBodyRef2D auto-added)
    CHECK(reg.GetComponent<Arcane::Transform>(e)->position.y == Catch::Approx(-1.0f));   // did not fall
    CHECK_FALSE(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer2D>()->captured);
}

TEST_CASE("fixedUpdate runs physics BEFORE propagation whichever was added first", "[runtime][physics]")
{
    // Behavioural pin of the Before<> edge: after one fixed step the entity's
    // WorldTransform carries the POST-step position PASS 4 wrote back. If
    // propagation ran first it would lag one step behind.
    Arcane::Runtime rt(Arcane::Test::Process());
    // Both are engine-owned since 2026-09-13 (InstallEngineSystems installs
    // physics then propagation); Arcane::PhysicsSystem2D's Before<> edge is what this
    // pins, so the plan must not depend on insertion order.
    REQUIRE(rt.Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    Astra::Registry& reg = rt.Registry();
    const Astra::Entity root = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
    reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Arcane::BodyType2D::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Circle; fx.radius = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.SetParent(e, root);

    rt.EnsurePhysics();
    rt.Loop().SetPaused(false);
    rt.Loop().Advance(1.0 / 60.0);                               // exactly one fixed step at 60 Hz
    const float y  = reg.GetComponent<Arcane::Transform>(e)->position.y;
    const float wy = reg.GetComponent<Arcane::WorldTransform>(e)->matrix[3].y;
    CHECK(y < 0.0f);                                             // it fell (+Y UP since F4 plan 1 T2)
    CHECK(wy == Catch::Approx(y).margin(1e-6f));                 // propagation saw this step's write-back
}

TEST_CASE("Runtime ResetRegistry empties the registry but keeps the ComponentRegistry", "[runtime]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Counter>();
    rt.Registry().CreateEntityWith(Counter{42});
    REQUIRE(rt.Registry().Size() == 1);

    rt.ResetRegistry();
    CHECK(rt.Registry().IsEmpty());

    // The shared ComponentRegistry still knows Counter -> this must not crash and must land.
    rt.Registry().CreateEntityWith(Counter{1});
    CHECK(rt.Registry().Size() == 1);
}

// E02-4: a Save failure must surface as an actionable Result error, not an
// empty-but-"ok" snapshot. Registry::Save() to memory is infallible, so the
// propagation contract is proven through the pure FinishSnapshot seam and the
// Runtime call site's success path.
TEST_CASE("FinishSnapshot propagates a Save failure instead of masking it", "[runtime][serialization]")
{
    using SR = Arcane::Serialization::SnapshotResult;

    auto failed = Arcane::Serialization::FinishSnapshot(SR::Err(Astra::SerializationError::IOError));
    REQUIRE(failed.IsErr());
    CHECK(*failed.GetError() == Astra::SerializationError::IOError);

    std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
    auto ok = Arcane::Serialization::FinishSnapshot(SR::Ok(bytes));
    REQUIRE(ok.IsOk());
    CHECK(ok.GetValue()->size() == 3);
}

TEST_CASE("Runtime SnapshotRegistry returns an actionable Result", "[runtime][serialization]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Counter>();
    rt.Registry().CreateEntityWith(Counter{7});

    auto snap = rt.SnapshotRegistry();
    REQUIRE(snap.IsOk());
    CHECK_FALSE(snap.GetValue()->empty());
}

// E02-1: a snapshot->restore round-trip must preserve registered serializable
// resources, not just entities/components. SceneRoot is registered by default;
// a second resource type is registered here to prove the seam handles a SET of
// resources (not a SceneRoot special case).
namespace
{
    struct CameraSnapshot { float zoom = 1.0f; float x = 0.0f; float y = 0.0f; };

    bool SaveCamera(const Astra::Registry& reg, Astra::BinaryWriter& w)
    {
        const CameraSnapshot* c = reg.GetResource<CameraSnapshot>();
        if (!c) return false;
        w(c->zoom); w(c->x); w(c->y);
        return true;
    }
    bool LoadCamera(Astra::Registry& reg, Astra::BinaryReader& r)
    {
        CameraSnapshot c;
        r(c.zoom); r(c.x); r(c.y);
        if (r.HasError()) return false;
        reg.SetResource<CameraSnapshot>(std::move(c));
        return true;
    }
}

TEST_CASE("Runtime snapshot/restore round-trips registered serializable resources", "[runtime][serialization][scene]")
{
    // Register the extra resource codec into the process-wide set the engine
    // snapshots with (idempotent -- safe if another test already registered it).
    Arcane::Serialization::SerializableResources().Register(
        Arcane::Serialization::ResourceCodec{
            Astra::TypeID<CameraSnapshot>::Hash(), &SaveCamera, &LoadCamera });

    Arcane::Runtime rt(Arcane::Test::Process());
    Astra::Entity root = rt.Registry().CreateEntity();
    rt.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    rt.Registry().SetResource<CameraSnapshot>(CameraSnapshot{2.5f, 10.0f, 20.0f});

    auto snap = rt.SnapshotRegistry();
    REQUIRE(snap.IsOk());

    // Clobber both resources on the live registry, then restore from the snapshot.
    rt.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{Astra::Entity::Invalid()});
    rt.Registry().SetResource<CameraSnapshot>(CameraSnapshot{});
    REQUIRE(rt.RestoreRegistry(*snap.GetValue()));

    // Both survived (RestoreRegistry swapped the registry, so re-fetch it).
    const Arcane::SceneRoot* sr = rt.Registry().GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    CHECK(sr->entity == root);                 // entity id survived Save/Load
    CHECK(rt.Registry().IsValid(sr->entity));  // and still resolves in the loaded registry

    const CameraSnapshot* cam = rt.Registry().GetResource<CameraSnapshot>();
    REQUIRE(cam != nullptr);
    CHECK(cam->zoom == Catch::Approx(2.5f));
    CHECK(cam->x == Catch::Approx(10.0f));
    CHECK(cam->y == Catch::Approx(20.0f));
}

// Fix 4 (review): drive Runtime::RestoreRegistry's resources.IsErr() branch
// end-to-end (the isolated corruption cases for ReadResourceSection itself
// are covered in SerializationNegativeTest.cpp). A perfectly valid registry
// blob paired with a corrupt resource-section tail must fail cleanly and
// leave the live world completely untouched -- RestoreRegistry is documented
// as transactional (load into a local registry first, only swap on full
// success), so this proves the resource-section failure actually aborts the
// swap rather than partially applying it.
TEST_CASE("Runtime RestoreRegistry rejects a valid registry blob with a corrupt resource section", "[runtime][serialization]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Counter>();
    rt.Registry().CreateEntityWith(Counter{42});

    auto snap = rt.SnapshotRegistry();
    REQUIRE(snap.IsOk());

    // Split the valid frame, then re-frame the SAME valid registry blob with a
    // deliberately corrupt resource section (count claims an entry that isn't
    // there -> CorruptedData in ReadResourceSection).
    auto frame = Arcane::Serialization::ParseSnapshot(*snap.GetValue());
    REQUIRE(frame.IsOk());
    const std::vector<std::byte> registryBlob(frame.GetValue()->registry.begin(),
                                               frame.GetValue()->registry.end());

    std::vector<std::byte> corruptSection;
    Astra::BinaryWriter w(corruptSection);
    w(static_cast<uint32_t>(1));   // count = 1, but no entry bytes follow

    const std::vector<std::byte> bytes = Arcane::Serialization::FrameBytes(registryBlob, corruptSection);

    CHECK_FALSE(rt.RestoreRegistry(bytes));

    // World untouched: the live registry still has its original entity/value,
    // proving RestoreRegistry did not swap in the (registry-valid-but-
    // resource-corrupt) loaded registry.
    int seen = 0;
    rt.Registry().CreateView<Counter>().ForEach([&](Astra::Entity, Counter& c)
    {
        ++seen;
        CHECK(c.value == 42);
    });
    CHECK(seen == 1);
}

TEST_CASE("two Runtimes share one JobSystem and the survivor can Submit", "[runtime][jobs]")
{
    // With private pools a's destructor drains a's task (n == 1 after a's scope)
    // and &a.Jobs() != &b.Jobs(). Shared, a's drop leaves the pool and its
    // gated task alone, so n is still 0 until the last Runtime drops.
    std::atomic<int>  n{0};
    std::atomic<bool> release{false};
    {
        Arcane::Runtime b(Arcane::Test::Process(), Arcane::NetMode::Standalone);
        {
            Arcane::Runtime a(Arcane::Test::Process(), Arcane::NetMode::Standalone);
            CHECK(&a.Jobs() == &b.Jobs());
            a.Jobs().Submit([&]
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!release.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                n.fetch_add(1);
            });
        }   // a is gone; the pool lives on through b
        CHECK(n.load() == 0);   // a's destructor did not drain the shared pool
        release.store(true);
        b.Jobs().Submit([&] { n.fetch_add(1); });
    }       // b's drop is the last: ~JobSystem drains both tasks
    REQUIRE(n.load() == 2);
}

// Astra adoption Task 3 (residency, spec s5). The shape that crashed on
// 2026-08-10: several Runtimes against ONE TypeContext, each tearing down its
// own engine ComponentModule -- the last one used to erase the shared TypeMeta.
// Two assertions, and the second is the load-bearing one: GetMeta<Transform>
// would survive even an un-pinned release here, because this test exe drains
// its OWN baseline binder for Transform at test_main.cpp:22 and never drops it.
// BinderCount does not: Arcane.dll's binder stays on the stack ONLY because
// Runtime declared Resident, so its Reset reports Retained instead of dropping
// the binder (MetaRegistry::Release). A module-owned roster without Resident
// makes the count fall by one -- that is the RED this case exists to show.
TEST_CASE("Runtime: two Runtimes against the shared context leave the engine metas pinned",
          "[runtime][residency]")
{
    const std::uint64_t hash = Astra::TypeID<Arcane::Transform>::Hash();
    Astra::MetaRegistry& meta = Arcane::Test::SharedTypeContext().Meta();
    const std::size_t before = meta.BinderCount(hash);
    REQUIRE(before >= 2);   // this exe's baseline + Arcane.dll's (pinned at test_main's throwaway pin)

    {
        Arcane::Runtime a(Arcane::Test::Process());
        Arcane::Runtime b(Arcane::Test::Process());
        // The roster is present in BOTH registries (GetComponentDescriptor is the
        // registry's presence query: null when the slot is empty).
        CHECK(a.Components()->GetComponentDescriptor(Astra::TypeID<Arcane::Transform>::Value()) != nullptr);
        CHECK(b.Components()->GetComponentDescriptor(Astra::TypeID<Arcane::PhysicsBodyRef2D>::Value()) != nullptr);
        CHECK(meta.BinderCount(hash) == before);   // registration ACQUIRES on an existing binder, adds none
    }

    CHECK(meta.BinderCount(hash) == before);       // Retained: the pinned binder did not leave
    const Astra::TypeMeta* still = Astra::GetMeta<Arcane::Transform>();
    REQUIRE(still != nullptr);
    CHECK(still->typeName == "Arcane::Transform");
}
