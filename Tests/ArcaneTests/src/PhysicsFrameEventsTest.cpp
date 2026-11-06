// PhysicsFrameEventsTest.cpp -- [physics][events]: the per-frame window across
// 0, 1 and N fixed steps, and its clearing (spec 2026-10-08 s7.2, s9.2). Real
// Runtime + RunLoop; EnsurePhysics mints a +Y-UP world (default gravity -9.81).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Sim/Time.hpp>

#include <App/PlayMode.hpp>

#include "Helpers/ModuleNames.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr double kFixed = 1.0 / 60.0;

    void AddBody(Astra::Registry& reg, Arcane::BodyType2D type, glm::vec2 pos, float hw, float hh,
                 Arcane::Guid guid = {}, bool sensor = false, bool hitEvents = false)
    {
        if (guid.IsNil()) guid = Arcane::Guid::Generate();
        Astra::Entity e = reg.CreateEntity();
        Arcane::Identity id; id.id = guid; reg.AddComponent<Arcane::Identity>(e, id);
        Arcane::Transform t; t.position = glm::vec3(pos, 0.0f); reg.AddComponent<Arcane::Transform>(e, t);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        Arcane::RigidBody2D rb; rb.type = type;
        // R8: a dynamic AABB asserts fixedRotation. Every dynamic body here is an Aabb.
        if (type == Arcane::BodyType2D::Dynamic)
            rb.fixedRotation = true;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);
        Arcane::Fixture2D f; f.kind = Arcane::ShapeKind2D::Aabb; f.halfW = hw; f.halfH = hh;
        f.isSensor = sensor;
        f.hitEvents = hitEvents;
        Arcane::Collider2D c; c.fixtures.push_back(f); reg.AddComponent<Arcane::Collider2D>(e, c);
        reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, Arcane::PhysicsBodyRef2D{});
    }

    // Ground top at y = 0; a crate whose bottom face starts 0.01 m above it, so it
    // touches within the first step or two.
    void Scene(Arcane::Runtime& rt)
    {
        AddBody(rt.Registry(), Arcane::BodyType2D::Static,  { 0.0f, -0.5f }, 10.0f, 0.5f);
        AddBody(rt.Registry(), Arcane::BodyType2D::Dynamic, { 0.0f, 0.51f }, 0.5f, 0.5f);
        rt.EnsurePhysics();
    }

    // Ground top is y = 0. A resting crate of half-height 0.25 occupies y [0, 0.50].
    // The sensor band y [0.55, 0.75] sits entirely above that, and every crate
    // bottom starts above the band, so each crate enters and leaves the sensor
    // before it lands. Gaps put three landings in step indices 27-29, which is
    // one 3-step frame of the 2,3 alternation. hitEvents is on the crates; the
    // drop clears the 1 m/s hit threshold. Guid{2,1} is teleported to y = 50 at
    // fixed step 35, after those landings, which is the contact End.
    void DropScene(Arcane::Runtime& rt)
    {
        Astra::Registry& reg = rt.Registry();
        AddBody(reg, Arcane::BodyType2D::Static, { 0.0f, -0.5f }, 10.0f, 0.5f, Arcane::Guid{ 1, 1 });
        AddBody(reg, Arcane::BodyType2D::Static, { 0.0f,  0.65f },  8.0f, 0.10f, Arcane::Guid{ 6, 1 }, true);
        const struct { float x; float gap; Arcane::Guid guid; } crates[] = {
            { -4.5f, 1.05f, Arcane::Guid{ 2, 1 } },
            { -1.5f, 1.12f, Arcane::Guid{ 3, 1 } },
            {  1.5f, 1.20f, Arcane::Guid{ 4, 1 } },
            {  4.5f, 1.45f, Arcane::Guid{ 5, 1 } },
        };
        for (const auto& c : crates)
            AddBody(reg, Arcane::BodyType2D::Dynamic, { c.x, c.gap + 0.25f }, 0.25f, 0.25f,
                    c.guid, false, true);
        rt.EnsurePhysics();
        rt.Loop().SetMaxStepsPerFrame(8);   // the 2/3-step frames must not hit the spiral cap
    }

    // Snap a landed crate out of its contact. Both runtimes call this at the same
    // fixed-step boundary, so the End lands on the same step of each sequence.
    void Teleport(Arcane::Runtime& rt, Arcane::Guid guid, glm::vec2 pos)
    {
        Astra::Registry& reg = rt.Registry();
        Arcane::PhysicsWorld2D* res = reg.GetResource<Arcane::PhysicsWorld2D>();
        REQUIRE(res != nullptr);
        REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res) != nullptr);
        bool found = false;
        reg.CreateView<const Arcane::Identity, const Arcane::PhysicsBodyRef2D>().ForEach(
            [&](Astra::Entity, const Arcane::Identity& id, const Arcane::PhysicsBodyRef2D& ref)
            {
                if (id.id != guid || found) return;
                Arcane::Detail::Physics2D::Access::Solver(*res)->SetPosition(Arcane::Detail::Physics2D::UnpackBody(ref.handle), Manifold2D::Physics::Vec2(pos.x, pos.y));
                Arcane::Detail::Physics2D::Access::Solver(*res)->SetVelocity(Arcane::Detail::Physics2D::UnpackBody(ref.handle), Manifold2D::Physics::Vec2(0.0f, 0.0f));
                Arcane::Detail::Physics2D::Access::Solver(*res)->Wake(Arcane::Detail::Physics2D::UnpackBody(ref.handle));
                found = true;
            });
        REQUIRE(found);
    }

    const Arcane::PhysicsWorld2D& Res(Arcane::Runtime& rt) { return *rt.Registry().GetResource<Arcane::PhysicsWorld2D>(); }

    std::uint64_t FixedStep(Arcane::Runtime& rt)
    {
        if (const Arcane::Time* t = rt.Registry().GetResource<Arcane::Time>()) return t->fixedStep;
        return 0;
    }

    bool SameSide(const Arcane::ContactSide2D& a, const Arcane::ContactSide2D& b)
    {
        return a.entity == b.entity && a.guid == b.guid && a.fixture == b.fixture;
    }

    bool SameVec(glm::vec2 a, glm::vec2 b) { return a.x == b.x && a.y == b.y; }

    // Owned copy of one window. The spans from StepEvents/FrameEvents die at the
    // next step, so a zero-step check has to keep the bytes itself.
    struct EventCopy
    {
        std::vector<Arcane::ContactBegin2D> contactBegin;
        std::vector<Arcane::ContactEnd2D>   contactEnd;
        std::vector<Arcane::ContactHit2D>   contactHit;
        std::vector<Arcane::SensorBegin2D>  sensorBegin;
        std::vector<Arcane::SensorEnd2D>    sensorEnd;

        bool operator==(const EventCopy& o) const
        {
            if (contactBegin.size() != o.contactBegin.size() || contactEnd.size() != o.contactEnd.size()
                || contactHit.size() != o.contactHit.size() || sensorBegin.size() != o.sensorBegin.size()
                || sensorEnd.size() != o.sensorEnd.size())
                return false;
            for (std::size_t i = 0; i < contactBegin.size(); ++i)
                if (!SameSide(contactBegin[i].a, o.contactBegin[i].a) || !SameSide(contactBegin[i].b, o.contactBegin[i].b))
                    return false;
            for (std::size_t i = 0; i < contactEnd.size(); ++i)
                if (!SameSide(contactEnd[i].a, o.contactEnd[i].a) || !SameSide(contactEnd[i].b, o.contactEnd[i].b))
                    return false;
            for (std::size_t i = 0; i < contactHit.size(); ++i)
            {
                const Arcane::ContactHit2D& x = contactHit[i];
                const Arcane::ContactHit2D& y = o.contactHit[i];
                if (!SameSide(x.a, y.a) || !SameSide(x.b, y.b) || !SameVec(x.point, y.point)
                    || !SameVec(x.normal, y.normal) || x.approachSpeed != y.approachSpeed)
                    return false;
            }
            for (std::size_t i = 0; i < sensorBegin.size(); ++i)
                if (!SameSide(sensorBegin[i].sensor, o.sensorBegin[i].sensor)
                    || !SameSide(sensorBegin[i].visitor, o.sensorBegin[i].visitor))
                    return false;
            for (std::size_t i = 0; i < sensorEnd.size(); ++i)
                if (!SameSide(sensorEnd[i].sensor, o.sensorEnd[i].sensor)
                    || !SameSide(sensorEnd[i].visitor, o.sensorEnd[i].visitor))
                    return false;
            return true;
        }
    };

    EventCopy CopyEvents(const Arcane::PhysicsEvents2D& e)
    {
        return {
            { e.contactBegin.begin(), e.contactBegin.end() },
            { e.contactEnd.begin(), e.contactEnd.end() },
            { e.contactHit.begin(), e.contactHit.end() },
            { e.sensorBegin.begin(), e.sensorBegin.end() },
            { e.sensorEnd.begin(), e.sensorEnd.end() },
        };
    }

    void RequireFrameEmpty(const Arcane::PhysicsEvents2D& e)
    {
        REQUIRE(e.contactBegin.empty());
        REQUIRE(e.contactEnd.empty());
        REQUIRE(e.contactHit.empty());
        REQUIRE(e.sensorBegin.empty());
        REQUIRE(e.sensorEnd.empty());
    }

    void RequireBothWindowsEmpty(Arcane::Runtime& rt)
    {
        REQUIRE(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() != nullptr);
        RequireFrameEmpty(Res(rt).StepEvents());
        RequireFrameEmpty(Res(rt).FrameEvents());
    }

    // The crate in Scene() touches the ground within the first few steps. Stop
    // at the first non-empty window so the transition under test cannot be a
    // later frame that simply produced nothing.
    void RequireContactBegin(Arcane::Runtime& rt)
    {
        bool saw = false;
        for (int f = 0; f < 30 && !saw; ++f)
        {
            rt.Loop().Advance(kFixed);
            saw = !Res(rt).StepEvents().contactBegin.empty()
               || !Res(rt).FrameEvents().contactBegin.empty();
        }
        REQUIRE(saw);
    }

    // One concatenated window plus the fixed-step index that produced each event.
    struct StampedEvents
    {
        EventCopy events;
        std::vector<int> beginStep;
        std::vector<int> endStep;
        std::vector<int> hitStep;
        std::vector<int> sensorBeginStep;
        std::vector<int> sensorEndStep;
    };

    template <typename T>
    void Take(std::vector<T>& dst, std::vector<int>& steps, const std::vector<T>& src, int step)
    {
        dst.insert(dst.end(), src.begin(), src.end());
        steps.insert(steps.end(), src.size(), step);
    }

    void AppendStamped(StampedEvents& log, const Arcane::PhysicsEvents2D& ev, int step)
    {
        const EventCopy c = CopyEvents(ev);
        Take(log.events.contactBegin, log.beginStep, c.contactBegin, step);
        Take(log.events.contactEnd, log.endStep, c.contactEnd, step);
        Take(log.events.contactHit, log.hitStep, c.contactHit, step);
        Take(log.events.sensorBegin, log.sensorBeginStep, c.sensorBegin, step);
        Take(log.events.sensorEnd, log.sensorEndStep, c.sensorEnd, step);
    }

    void RequireSide(const Arcane::ContactSide2D& a, const Arcane::ContactSide2D& b)
    {
        REQUIRE(a.entity == b.entity);
        REQUIRE(a.guid == b.guid);
        REQUIRE(a.fixture == b.fixture);
    }

    void RequireSameEvents(const EventCopy& got, const EventCopy& oracle)
    {
        REQUIRE(got.contactBegin.size() == oracle.contactBegin.size());
        for (std::size_t i = 0; i < got.contactBegin.size(); ++i)
        {
            INFO("contactBegin[" << i << "]");
            RequireSide(got.contactBegin[i].a, oracle.contactBegin[i].a);
            RequireSide(got.contactBegin[i].b, oracle.contactBegin[i].b);
        }
        REQUIRE(got.contactEnd.size() == oracle.contactEnd.size());
        for (std::size_t i = 0; i < got.contactEnd.size(); ++i)
        {
            INFO("contactEnd[" << i << "]");
            RequireSide(got.contactEnd[i].a, oracle.contactEnd[i].a);
            RequireSide(got.contactEnd[i].b, oracle.contactEnd[i].b);
        }
        REQUIRE(got.contactHit.size() == oracle.contactHit.size());
        for (std::size_t i = 0; i < got.contactHit.size(); ++i)
        {
            INFO("contactHit[" << i << "]");
            const Arcane::ContactHit2D& x = got.contactHit[i];
            const Arcane::ContactHit2D& y = oracle.contactHit[i];
            RequireSide(x.a, y.a);
            RequireSide(x.b, y.b);
            REQUIRE(std::bit_cast<std::uint32_t>(x.point.x) == std::bit_cast<std::uint32_t>(y.point.x));
            REQUIRE(std::bit_cast<std::uint32_t>(x.point.y) == std::bit_cast<std::uint32_t>(y.point.y));
            REQUIRE(std::bit_cast<std::uint32_t>(x.normal.x) == std::bit_cast<std::uint32_t>(y.normal.x));
            REQUIRE(std::bit_cast<std::uint32_t>(x.normal.y) == std::bit_cast<std::uint32_t>(y.normal.y));
            REQUIRE(std::bit_cast<std::uint32_t>(x.approachSpeed) == std::bit_cast<std::uint32_t>(y.approachSpeed));
        }
        REQUIRE(got.sensorBegin.size() == oracle.sensorBegin.size());
        for (std::size_t i = 0; i < got.sensorBegin.size(); ++i)
        {
            INFO("sensorBegin[" << i << "]");
            RequireSide(got.sensorBegin[i].sensor, oracle.sensorBegin[i].sensor);
            RequireSide(got.sensorBegin[i].visitor, oracle.sensorBegin[i].visitor);
        }
        REQUIRE(got.sensorEnd.size() == oracle.sensorEnd.size());
        for (std::size_t i = 0; i < got.sensorEnd.size(); ++i)
        {
            INFO("sensorEnd[" << i << "]");
            RequireSide(got.sensorEnd[i].sensor, oracle.sensorEnd[i].sensor);
            RequireSide(got.sensorEnd[i].visitor, oracle.sensorEnd[i].visitor);
        }
    }

    // `stepOf` records Time::fixedStep (the first step is 1). The frame owns the
    // slice [cursor, cursor + count). Every index in it must sit in the inclusive
    // range that frame actually stepped. Distinct indices are appended to `seen`.
    void AttributeSlice(const std::vector<int>& stepOf, std::size_t cursor, std::size_t count,
                        std::uint64_t rangeLo, std::uint64_t rangeHi, std::vector<int>& seen)
    {
        REQUIRE(cursor + count <= stepOf.size());
        for (std::size_t i = 0; i < count; ++i)
        {
            const int step = stepOf[cursor + i];
            REQUIRE(static_cast<std::uint64_t>(step) >= rangeLo);
            REQUIRE(static_cast<std::uint64_t>(step) <= rangeHi);
            if (std::find(seen.begin(), seen.end(), step) == seen.end())
                seen.push_back(step);
        }
    }
}

TEST_CASE("FrameEvents gathers every step of a frame and empties on a zero-step frame", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Scene(rt);
    std::size_t total = 0;
    for (int f = 0; f < 30 && total == 0; ++f)
    {
        rt.Loop().Advance(2.5 * kFixed);              // two fixed steps per frame
        total = Res(rt).FrameEvents().contactBegin.size();
        CHECK(total <= 1);
    }
    REQUIRE(total == 1);                              // the landing, seen in its frame
    const EventCopy stepBefore = CopyEvents(Res(rt).StepEvents());
    rt.Loop().Advance(0.0);                           // zero fixed steps
    RequireFrameEmpty(Res(rt).FrameEvents());
    REQUIRE(CopyEvents(Res(rt).StepEvents()) == stepBefore);
}

TEST_CASE("FrameEvents matches the per-step oracle in order", "[physics][events]")
{
    // Runtime A: one fixed step per frame. Concatenated StepEvents is the
    // per-step sequence, and each event records the step index that produced
    // it. Runtime B runs the same scene in alternating 2-step and 3-step
    // frames. 2.5 fixed steps does not alternate in double (the leftover is
    // just under one step, so two frames in a row take 2). Passing an integer
    // multiple of the loop's own fixed dt does. Determinism makes A's sequence
    // the oracle for B's FrameEvents.
    constexpr int kSteps = 40;                        // 8 * (2 + 3)
    constexpr int kLiftAt = 35;                       // after the landings; five steps remain
    const Arcane::Guid kLifted{ 2, 1 };
    const glm::vec2 kLiftTo{ -4.5f, 50.0f };

    Arcane::Runtime oracleRt(Arcane::Test::Process());
    DropScene(oracleRt);
    const double step = 1.0 / oracleRt.Loop().FixedHz();
    StampedEvents oracle;
    for (int s = 0; s < kSteps; ++s)
    {
        if (FixedStep(oracleRt) == static_cast<std::uint64_t>(kLiftAt))
            Teleport(oracleRt, kLifted, kLiftTo);
        oracleRt.Loop().Advance(step);
        // Time::fixedStep is 1 on the first step. B's frame range is that same
        // number, so the stamp is the clock after the step, not the loop index.
        const Arcane::Time* clock = oracleRt.Registry().GetResource<Arcane::Time>();
        REQUIRE(clock != nullptr);
        AppendStamped(oracle, Res(oracleRt).StepEvents(), static_cast<int>(clock->fixedStep));
    }
    REQUIRE(FixedStep(oracleRt) == static_cast<std::uint64_t>(kSteps));
    std::string shape;
    for (int s = 1; s <= kSteps; ++s)
    {
        const int b = static_cast<int>(std::count(oracle.beginStep.begin(), oracle.beginStep.end(), s));
        const int e = static_cast<int>(std::count(oracle.endStep.begin(), oracle.endStep.end(), s));
        const int h = static_cast<int>(std::count(oracle.hitStep.begin(), oracle.hitStep.end(), s));
        const int sb = static_cast<int>(std::count(oracle.sensorBeginStep.begin(), oracle.sensorBeginStep.end(), s));
        const int se = static_cast<int>(std::count(oracle.sensorEndStep.begin(), oracle.sensorEndStep.end(), s));
        if (b || e || h || sb || se)
            shape += std::to_string(s) + ":b" + std::to_string(b) + " e" + std::to_string(e)
                + " h" + std::to_string(h) + " sb" + std::to_string(sb) + " se" + std::to_string(se) + " ";
    }
    INFO("per-step events: " << shape);
    REQUIRE(oracle.events.contactBegin.size() >= 4);          // one landing per crate
    REQUIRE_FALSE(oracle.events.contactEnd.empty());
    REQUIRE_FALSE(oracle.events.contactHit.empty());
    REQUIRE_FALSE(oracle.events.sensorBegin.empty());
    REQUIRE_FALSE(oracle.events.sensorEnd.empty());

    Arcane::Runtime frameRt(Arcane::Test::Process());
    DropScene(frameRt);
    const double frameStep = 1.0 / frameRt.Loop().FixedHz();
    // Time is published by Advance. A zero-step frame leaves the clock at 0 so
    // every measured frame can read fixedStep before it steps.
    frameRt.Loop().Advance(0.0);
    REQUIRE(frameRt.Registry().GetResource<Arcane::Time>()->fixedStep == 0);
    EventCopy frames;
    bool sawMultiStepFrame = false;
    std::size_t beginCursor = 0, endCursor = 0, hitCursor = 0, sensorBeginCursor = 0, sensorEndCursor = 0;
    for (int f = 0; f < 16; ++f)
    {
        const int expect = (f % 2 == 0) ? 2 : 3;
        if (FixedStep(frameRt) == static_cast<std::uint64_t>(kLiftAt))
            Teleport(frameRt, kLifted, kLiftTo);
        const std::uint64_t before = frameRt.Registry().GetResource<Arcane::Time>()->fixedStep;
        frameRt.Loop().Advance(expect * frameStep);
        const std::uint64_t after = frameRt.Registry().GetResource<Arcane::Time>()->fixedStep;
        REQUIRE(after - before == static_cast<std::uint64_t>(expect));
        const Arcane::PhysicsEvents2D ev = Res(frameRt).FrameEvents();
        // The slice is the events this frame holds. Each one must name a
        // fixedStep inside [before + 1, after], the steps this Advance took.
        std::vector<int> frameSteps;
        AttributeSlice(oracle.beginStep, beginCursor, ev.contactBegin.size(), before + 1, after, frameSteps);
        AttributeSlice(oracle.endStep, endCursor, ev.contactEnd.size(), before + 1, after, frameSteps);
        AttributeSlice(oracle.hitStep, hitCursor, ev.contactHit.size(), before + 1, after, frameSteps);
        AttributeSlice(oracle.sensorBeginStep, sensorBeginCursor, ev.sensorBegin.size(), before + 1, after, frameSteps);
        AttributeSlice(oracle.sensorEndStep, sensorEndCursor, ev.sensorEnd.size(), before + 1, after, frameSteps);
        if (frameSteps.size() >= 2)
            sawMultiStepFrame = true;
        beginCursor += ev.contactBegin.size();
        endCursor += ev.contactEnd.size();
        hitCursor += ev.contactHit.size();
        sensorBeginCursor += ev.sensorBegin.size();
        sensorEndCursor += ev.sensorEnd.size();
        const EventCopy frameCopy = CopyEvents(ev);
        frames.contactBegin.insert(frames.contactBegin.end(), frameCopy.contactBegin.begin(), frameCopy.contactBegin.end());
        frames.contactEnd.insert(frames.contactEnd.end(), frameCopy.contactEnd.begin(), frameCopy.contactEnd.end());
        frames.contactHit.insert(frames.contactHit.end(), frameCopy.contactHit.begin(), frameCopy.contactHit.end());
        frames.sensorBegin.insert(frames.sensorBegin.end(), frameCopy.sensorBegin.begin(), frameCopy.sensorBegin.end());
        frames.sensorEnd.insert(frames.sensorEnd.end(), frameCopy.sensorEnd.begin(), frameCopy.sensorEnd.end());
    }
    REQUIRE(FixedStep(frameRt) == static_cast<std::uint64_t>(kSteps));
    REQUIRE(beginCursor == oracle.beginStep.size());
    REQUIRE(endCursor == oracle.endStep.size());
    REQUIRE(hitCursor == oracle.hitStep.size());
    REQUIRE(sensorBeginCursor == oracle.sensorBeginStep.size());
    REQUIRE(sensorEndCursor == oracle.sensorEndStep.size());
    REQUIRE(sawMultiStepFrame);                      // one B frame held events from two steps
    RequireSameEvents(frames, oracle.events);
}

TEST_CASE("Re-minting and restoring clear both windows", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Scene(rt);
    for (int f = 0; f < 30; ++f) rt.Loop().Advance(kFixed);
    auto bytes = rt.SnapshotRegistry();                          // RuntimeTest's RestoreRegistry path
    REQUIRE(bytes.IsOk());
    REQUIRE(rt.RestoreRegistry(*bytes.GetValue()));
    CHECK(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);   // windows gone with the world
    rt.EnsurePhysics();
    CHECK(Res(rt).StepEvents().contactBegin.empty());
    CHECK(Res(rt).FrameEvents().contactBegin.empty());

    // The hook is host policy on the RunLoop Rebind keeps. A stepping frame
    // after the restore still clears on the next zero-step frame.
    std::size_t produced = 0;
    for (int f = 0; f < 30 && produced == 0; ++f)
    {
        rt.Loop().Advance(kFixed);
        produced = Res(rt).FrameEvents().contactBegin.size();
    }
    REQUIRE(produced >= 1);
    rt.Loop().Advance(0.0);
    RequireFrameEmpty(Res(rt).FrameEvents());

    // Gravity edit on the scene root re-mints (RuntimeTest's Arcane::PhysicsSettings2D
    // pattern). The replacement resource's windows start empty.
    Astra::Registry& reg = rt.Registry();
    const Astra::Entity root = reg.CreateEntity();
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    Arcane::PhysicsSettings2D ps; ps.gravity = glm::vec2(0.0f, 2.0f);
    reg.AddComponent<Arcane::PhysicsSettings2D>(root, ps);
    const Manifold2D::Physics::PhysicsWorld* before = Arcane::Detail::Physics2D::Access::Solver(Res(rt));
    rt.EnsurePhysics();
    CHECK(Arcane::Detail::Physics2D::Access::Solver(Res(rt)) != before);
    CHECK(Res(rt).StepEvents().contactBegin.empty());
    CHECK(Res(rt).FrameEvents().contactBegin.empty());
}

// s9.2: Play drops the world (ResetPhysics), Stop restores a snapshot that
// cannot carry the transient resource, and File > Open Scene swaps the
// registry (ResetRegistry -- EditorPlayModeTest / ResourceSwapTest). Each
// path is checked with a contact begin already in the windows, then with
// the replacement resource's windows empty before any new step.
TEST_CASE("Play, Stop and scene open clear both windows", "[physics][events]")
{
    {
        Arcane::Runtime rt(Arcane::Test::Process());
        Scene(rt);
        RequireContactBegin(rt);

        Arcane::Editor::PlaySession play;
        REQUIRE(play.Play(rt));
        CHECK(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);
        rt.EnsurePhysics();
        RequireBothWindowsEmpty(rt);

        RequireContactBegin(rt);
        REQUIRE(play.Stop(rt));
        CHECK(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);
        rt.EnsurePhysics();
        RequireBothWindowsEmpty(rt);
    }

    Arcane::Runtime opened(Arcane::Test::Process());
    Scene(opened);
    RequireContactBegin(opened);
    opened.ResetRegistry();
    CHECK(opened.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);
    opened.EnsurePhysics();
    RequireBothWindowsEmpty(opened);
}

// PluginHost::ForceReload is the headless module-reload seam (ResourceSwapTest,
// PluginHostTest). Teardown resets every attached registry before LoadState
// restores one that cannot carry Arcane::PhysicsWorld2D.
TEST_CASE("a module hot reload clears both windows", "[physics][events]")
{
    const std::filesystem::path dll = Arcane::Test::ModuleFile("PhysicsEventsReload");
    std::error_code ec;
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), dll,
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);

    Arcane::Runtime rt(Arcane::Test::Process());
    Arcane::PluginHost host(Arcane::Test::Process(), dll);
    REQUIRE(host.AttachRuntime(rt));
    REQUIRE(host.Load());
    Scene(rt);
    RequireContactBegin(rt);

    REQUIRE(host.ForceReload());
    CHECK(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);
    rt.EnsurePhysics();
    RequireBothWindowsEmpty(rt);

    host.Unload();
    std::filesystem::remove(dll, ec);
}

TEST_CASE("the frame hook tolerates a registry without Arcane::PhysicsWorld2D", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Loop().Advance(kFixed);                        // no EnsurePhysics: the hook finds nothing
    rt.ResetPhysics();
    rt.Loop().Advance(kFixed);
    SUCCEED();
}
