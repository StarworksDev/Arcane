// PhysicsEvents2DTest.cpp -- [physics][events]: the Arcane event surface (spec
// 2026-10-08 s7). Bare registry + Arcane::PhysicsSystem2D, +Y DOWN, g = 10 (the
// PhysicsSystemTest convention).
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <string>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>
#include "Helpers/ReferenceProjectDir.hpp"
#include "Helpers/SettingsSweep.hpp"

namespace
{
    constexpr float kDt = 1.0f / 60.0f;

    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        World()
        {
            Arcane::RegisterSceneComponents(reg);
            Arcane::RegisterPhysicsComponents2D(reg);
            Manifold2D::Physics::WorldDef wd; wd.gravityY = 10.0f;
            reg.SetResource(Arcane::Detail::Physics2D::Adopt(std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));
        }
        Astra::Entity Body(const char* name, Arcane::BodyType2D type, glm::vec2 pos,
                           std::vector<Arcane::Fixture2D> fixtures, glm::vec3 scale = glm::vec3(1.0f))
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Identity id; id.id = Arcane::Guid::Generate(); id.name = name;
            reg.AddComponent<Arcane::Identity>(e, id);
            Arcane::Transform t; t.position = glm::vec3(pos, 0.0f); t.scale = scale;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
            Arcane::RigidBody2D rb; rb.type = type;
            // R8: a dynamic AABB asserts fixedRotation. Every dynamic body here is an Aabb.
            if (type == Arcane::BodyType2D::Dynamic)
                rb.fixedRotation = true;
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);
            Arcane::Collider2D col; col.fixtures = std::move(fixtures);
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});
            return e;
        }
        Arcane::PhysicsWorld2D& Res() { return *reg.GetResource<Arcane::PhysicsWorld2D>(); }
        Arcane::Guid GuidOf(Astra::Entity e) { return std::as_const(reg).GetComponent<Arcane::Identity>(e)->id; }
    };

    Arcane::Fixture2D Box(float hw, float hh)
    {
        Arcane::Fixture2D f; f.kind = Arcane::ShapeKind2D::Aabb; f.halfW = hw; f.halfH = hh; return f;
    }

    int LandingHits()
    {
        World w;
        Arcane::Fixture2D hitty = Box(0.5f, 0.5f); hitty.hitEvents = true;
        w.Body("Ground", Arcane::BodyType2D::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
        w.Body("Crate", Arcane::BodyType2D::Dynamic, { 0.0f, -2.5f }, { hitty });
        Arcane::PhysicsSystem2D physics(kDt);
        int hits = 0;
        for (int i = 0; i < 90; ++i) { physics(w.reg); hits += static_cast<int>(w.Res().StepEvents().contactHit.size()); }
        return hits;
    }
}

TEST_CASE("Arcane::Fixture2D event flags default contact+sensor on, hit off", "[physics][events]")
{
    Arcane::Fixture2D f;
    CHECK(f.contactEvents);
    CHECK(f.sensorEvents);
    CHECK_FALSE(f.hitEvents);
}

TEST_CASE("StepEvents reports a crate landing on static ground, by entity + GUID + fixture", "[physics][events]")
{
    World w;
    const Astra::Entity ground = w.Body("Ground", Arcane::BodyType2D::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate  = w.Body("Crate",  Arcane::BodyType2D::Dynamic, { 0.0f, -2.0f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem2D physics(kDt);
    int begins = 0;
    for (int i = 0; i < 120; ++i)
    {
        physics(w.reg);
        for (const Arcane::ContactBegin2D& e : w.Res().StepEvents().contactBegin)
        {
            ++begins;
            CHECK(e.a.entity == crate);           // A = the dynamic side
            CHECK(e.a.guid == w.GuidOf(crate));
            CHECK(e.a.fixture == 0u);
            CHECK(e.b.entity == ground);
            CHECK(e.b.guid == w.GuidOf(ground));
        }
    }
    CHECK(begins == 1);
}

TEST_CASE("fixture indices map through the auto-fixture and AddFixture paths", "[physics][events]")
{
    World w;
    Arcane::Fixture2D sensor = Box(2.0f, 2.0f); sensor.isSensor = true;    // fixture 0: the auto-fixture path
    Arcane::Fixture2D solid  = Box(0.3f, 0.3f); solid.localPos = { 0.0f, 5.0f };   // fixture 1: AddFixture
    const Astra::Entity zone = w.Body("Zone", Arcane::BodyType2D::Static, { 0.0f, -6.0f }, { sensor, solid });
    const Astra::Entity crate = w.Body("Crate", Arcane::BodyType2D::Dynamic, { 0.0f, -12.0f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem2D physics(kDt);
    int sensorBegins = 0;
    for (int i = 0; i < 90; ++i)
    {
        physics(w.reg);
        for (const Arcane::SensorBegin2D& e : w.Res().StepEvents().sensorBegin)
        {
            ++sensorBegins;
            CHECK(e.sensor.entity == zone);
            CHECK(e.sensor.fixture == 0u);
            CHECK(e.visitor.entity == crate);
        }
    }
    CHECK(sensorBegins == 1);
}

TEST_CASE("an AddFixture surface reports fixture index 1", "[physics][events]")
{
    World w;
    Arcane::Fixture2D away = Box(0.2f, 0.2f); away.localPos = { 30.0f, 0.0f };   // fixture 0: auto-fixture, off the fall
    Arcane::Fixture2D surface = Box(10.0f, 0.5f);                                 // fixture 1: AddFixture, the landing
    const Astra::Entity ground = w.Body("Ground", Arcane::BodyType2D::Static, { 0.0f, 0.5f }, { away, surface });
    const Astra::Entity crate  = w.Body("Crate",  Arcane::BodyType2D::Dynamic, { 0.0f, -2.0f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem2D physics(kDt);
    int begins = 0;
    for (int i = 0; i < 120; ++i)
    {
        physics(w.reg);
        for (const Arcane::ContactBegin2D& e : w.Res().StepEvents().contactBegin)
        {
            ++begins;
            CHECK(e.a.entity == crate);
            CHECK(e.b.entity == ground);
            CHECK(e.b.guid == w.GuidOf(ground));
            CHECK(e.b.fixture == 1u);
        }
    }
    CHECK(begins == 1);
}

TEST_CASE("Opting one fixture out silences the pair (both-fixtures rule)", "[physics][events]")
{
    World w;
    Arcane::Fixture2D quiet = Box(10.0f, 0.5f); quiet.contactEvents = false;
    w.Body("Ground", Arcane::BodyType2D::Static, { 0.0f, 0.5f }, { quiet });
    w.Body("Crate", Arcane::BodyType2D::Dynamic, { 0.0f, -2.0f }, { Box(0.5f, 0.5f) });   // default on
    Arcane::PhysicsSystem2D physics(kDt);
    int begins = 0;
    for (int i = 0; i < 120; ++i) { physics(w.reg); begins += static_cast<int>(w.Res().StepEvents().contactBegin.size()); }
    CHECK(begins == 0);
}

TEST_CASE("A destroyed entity's End still carries its GUID", "[physics][events]")
{
    World w;
    w.Body("Ground", Arcane::BodyType2D::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate = w.Body("Crate", Arcane::BodyType2D::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    const Arcane::Guid guid = w.GuidOf(crate);
    Arcane::PhysicsSystem2D physics(kDt);
    for (int i = 0; i < 10; ++i) physics(w.reg);
    w.reg.DestroyEntity(crate);
    physics(w.reg);                                // PASS 1 removes + retires; the step delivers the End
    const auto ends = w.Res().StepEvents().contactEnd;
    REQUIRE(ends.size() == 1);
    CHECK(ends[0].a.guid == guid);
    CHECK_FALSE(w.reg.IsValid(ends[0].a.entity));
    physics(w.reg);
    CHECK(Arcane::Detail::Physics2D::Access::Records(w.Res()).size() == 1);        // retired record erased after its delivery
}

TEST_CASE("a recycled body slot never resolves to the retired record", "[physics][events]")
{
    World w;
    w.Body("Ground", Arcane::BodyType2D::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity first = w.Body("First", Arcane::BodyType2D::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem2D physics(kDt);
    for (int i = 0; i < 5; ++i) physics(w.reg);
    const Arcane::Guid firstGuid = w.GuidOf(first);
    w.reg.DestroyEntity(first);
    const Astra::Entity second = w.Body("Second", Arcane::BodyType2D::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    physics(w.reg);                                // PASS 1 retires First, PASS 2 mints Second (same slot)
    bool sawFirstEnd = false, sawSecondBegin = false;
    for (const auto& e : w.Res().StepEvents().contactEnd)   if (e.a.guid == firstGuid) sawFirstEnd = true;
    for (const auto& e : w.Res().StepEvents().contactBegin) if (e.a.entity == second && e.a.guid == w.GuidOf(second)) sawSecondBegin = true;
    CHECK(sawFirstEnd);
    CHECK(sawSecondBegin);
}

TEST_CASE("a paused rescale retires the old fixture until the next capture", "[physics][events]")
{
    World w;
    w.Body("Ground", Arcane::BodyType2D::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate = w.Body("Crate", Arcane::BodyType2D::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    const Arcane::Guid guid = w.GuidOf(crate);
    Arcane::PhysicsSystem2D physics(kDt);
    for (int i = 0; i < 10; ++i) physics(w.reg);   // resting contact, Begin already delivered

    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld*/ false);
    w.reg.GetComponent<Arcane::Transform>(crate)->scale = glm::vec3(2.0f, 2.0f, 1.0f);
    paused(w.reg);                                 // rebuild: old handle retired, new handle current

    bool sawEnd = false, sawBegin = false;
    for (int i = 0; i < 8; ++i)
    {
        physics(w.reg);
        for (const Arcane::ContactEnd2D& e : w.Res().StepEvents().contactEnd)
            if (e.a.entity == crate && e.a.fixture == 0u && e.a.guid == guid) sawEnd = true;
        for (const Arcane::ContactBegin2D& e : w.Res().StepEvents().contactBegin)
            if (e.a.entity == crate && e.a.fixture == 0u && e.a.guid == guid) sawBegin = true;
    }
    CHECK(sawEnd);                                 // destroy-time End of the dropped fixture
    CHECK(sawBegin);                               // Begin of the rebuilt fixture
}

TEST_CASE("consecutive paused rescales keep every retired fixture resolvable", "[physics][events]")
{
    World w;
    w.Body("Ground", Arcane::BodyType2D::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate = w.Body("Crate", Arcane::BodyType2D::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    const Arcane::Guid guid = w.GuidOf(crate);
    Arcane::PhysicsSystem2D physics(kDt);
    for (int i = 0; i < 10; ++i) physics(w.reg);

    auto& records = Arcane::Detail::Physics2D::Access::Records(w.Res());
    const auto key = Arcane::Detail::Physics2D::PackBody(
        Arcane::Detail::Physics2D::Access::Entities(w.Res()).at(crate));
    const auto gen0 = records.at(key).fixtures.at(0);

    Arcane::PhysicsSystem2D paused(kDt, /*stepWorld*/ false);
    w.reg.GetComponent<Arcane::Transform>(crate)->scale = glm::vec3(2.0f, 2.0f, 1.0f);
    paused(w.reg);
    const auto gen1 = records.at(key).fixtures.at(0);

    w.reg.GetComponent<Arcane::Transform>(crate)->scale = glm::vec3(3.0f, 3.0f, 1.0f);
    paused(w.reg);                                 // second rescale before any capture
    const auto gen2 = records.at(key).fixtures.at(0);
    const auto& retired = records.at(key).retiredFixtures;
    REQUIRE(retired.size() == 2u);
    CHECK(retired[0].handle == gen0);
    CHECK(retired[0].index == 0u);
    CHECK(retired[1].handle == gen1);
    CHECK(retired[1].index == 0u);

    // R10: DropFixture queues an End iff that fixture's Begin was delivered.
    // gen1 is added and dropped with no Step between the two paused rescales,
    // so it never beginReported and the world queues no End for it. Both
    // handles stay in retiredFixtures until this capture (the REQUIRE above),
    // so Side() resolves whichever generation the world does end. The queued
    // End is gen0; gen2's Begin is the fixture that is current now.
    bool sawGen0End = false, sawGen1End = false, sawBegin = false;
    for (int i = 0; i < 8; ++i)
    {
        physics(w.reg);
        const auto* solver = Arcane::Detail::Physics2D::Access::Solver(w.Res());
        const auto raw = solver->GetContactEvents();
        const auto sen = solver->GetSensorEvents();
        const auto ev  = w.Res().StepEvents();
        // A dropped Side() shrinks the translated array. Equal sizes means
        // every event the world queued was kept.
        CHECK(ev.contactBegin.size() == raw.begin.size());
        CHECK(ev.contactEnd.size() == raw.end.size());
        CHECK(ev.contactHit.size() == raw.hit.size());
        CHECK(ev.sensorBegin.size() == sen.begin.size());
        CHECK(ev.sensorEnd.size() == sen.end.size());

        for (std::size_t n = 0; n < raw.end.size(); ++n)
        {
            const auto& src = raw.end[n];
            const bool aOld = src.a == gen0 || src.a == gen1;
            const bool bOld = src.b == gen0 || src.b == gen1;
            if (!aOld && !bOld) continue;
            const auto& side = aOld ? ev.contactEnd[n].a : ev.contactEnd[n].b;
            CHECK(side.entity == crate);
            CHECK(side.guid == guid);
            CHECK(side.fixture == 0u);
            if (src.a == gen0 || src.b == gen0) sawGen0End = true;
            if (src.a == gen1 || src.b == gen1) sawGen1End = true;
        }
        for (std::size_t n = 0; n < raw.begin.size(); ++n)
        {
            const auto& src = raw.begin[n];
            const bool aNew = src.a == gen2;
            const bool bNew = src.b == gen2;
            if (!aNew && !bNew) continue;
            const auto& side = aNew ? ev.contactBegin[n].a : ev.contactBegin[n].b;
            CHECK(side.entity == crate);
            CHECK(side.guid == guid);
            CHECK(side.fixture == 0u);
            sawBegin = true;
        }
    }
    CHECK(sawGen0End);                             // first rescale's queued End
    CHECK_FALSE(sawGen1End);                       // R10: gen1 never began, so no End
    CHECK(sawBegin);                               // the fixture that is current now
    CHECK(records.at(key).retiredFixtures.empty());
}

TEST_CASE("ContactsOf lists a resting crate's ground contact by entity", "[physics][events]")
{
    World w;
    const Astra::Entity ground = w.Body("Ground", Arcane::BodyType2D::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate  = w.Body("Crate",  Arcane::BodyType2D::Dynamic, { 0.0f, -0.5f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem2D physics(kDt);
    // kSleepTime is 0.5 s once the crate is idle. 300 steps is the usual rest;
    // keep going up to 1200 (20 s) and require the body actually slept.
    auto& entities = Arcane::Detail::Physics2D::Access::Entities(w.Res());
    for (int i = 0; i < 1200; ++i)
    {
        physics(w.reg);
        const auto* solver = Arcane::Detail::Physics2D::Access::Solver(w.Res());
        const auto it = entities.find(crate);
        if (it != entities.end() && solver && !solver->IsAwake(it->second))
            break;
    }
    const auto slept = entities.find(crate);
    REQUIRE(slept != entities.end());
    const auto* solver = Arcane::Detail::Physics2D::Access::Solver(w.Res());
    REQUIRE(solver);
    REQUIRE_FALSE(solver->IsAwake(slept->second));   // sleepers-included path
    std::vector<Arcane::ContactPoint2D> out;
    w.Res().ContactsOf(crate, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].self.entity == crate);
    CHECK(out[0].other.entity == ground);
    CHECK(out[0].other.guid == w.GuidOf(ground));
    CHECK(out[0].normal.y > 0.99f);                   // from the crate down to the ground (+Y down here)
    CHECK(out[0].pointCount >= 1u);
    w.Res().ContactsOf(Astra::Entity::Invalid(), out);
    CHECK(out.empty());
}

TEST_CASE("physics.events.hitThreshold reaches the world", "[physics][events]")
{
    {
        // Above the ~6 m/s landing. The guard publishes, and clears the Code
        // rung on the way out -- including when a REQUIRE fails.
        const Arcane::Test::ScopedCodeRung high("physics.events.hitThreshold", Arcane::CVarValue::Float32(50.0f));
        CHECK(LandingHits() == 0);
    }
    CHECK(LandingHits() >= 1);                     // default 1 m/s, restored
}

TEST_CASE("PhysicsEvents2D.hpp includes no Manifold2D header", "[physics][events][guard]")
{
    const auto path = Arcane::Test::FindReferenceProjectDir().parent_path() / "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsEvents2D.hpp";
    std::ifstream in(path);
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("#include", 0) == 0) CHECK(line.find("Manifold2D") == std::string::npos);
}

TEST_CASE("Arcane::Fixture2D event flags round-trip through scene JSON; absent keys keep the defaults", "[physics][events][json]")
{
    nlohmann::json doc;
    {
        World w;
        Arcane::Fixture2D f = Box(0.5f, 0.5f); f.contactEvents = false; f.hitEvents = true;
        const Astra::Entity root = w.reg.CreateEntity();
        w.reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        Arcane::Collider2D col; col.fixtures.push_back(f);
        w.reg.AddComponent<Arcane::Collider2D>(root, col);
        w.reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
        doc = Arcane::Scene::SaveJson(w.reg);
    }
    auto& fx = doc["entities"][0]["components"]["Arcane::Collider2D"]["fixtures"][0];
    CHECK(fx["contactEvents"] == false);
    CHECK(fx["hitEvents"] == true);
    fx.erase("sensorEvents");                      // a pre-spec scene has no key
    World loaded;
    REQUIRE(Arcane::Scene::LoadJson(loaded.reg, doc));
    loaded.reg.CreateView<Arcane::Collider2D>().ForEach([&](Astra::Entity, Arcane::Collider2D& c)
    {
        REQUIRE(c.fixtures.size() == 1);
        CHECK_FALSE(c.fixtures[0].contactEvents);
        CHECK(c.fixtures[0].hitEvents);
        CHECK(c.fixtures[0].sensorEvents);         // default kept
    });
}
