// 3D-readiness Plan A, Tasks 1-2: the v1 physics baselines. The step trace, the
// debug / inspector overlays and the collider pick list are captured once from
// the unmodified tree and checked in under Tests/ArcaneTests/data/physics/.
// Later refactors must reproduce those bytes. The capture cases are hidden
// ([.]) and armed by an environment switch.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"
#include "Helpers/PhysicsV1Baseline.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Scene/PhysicsDebugGeometry.hpp>   // GatherPhysicsDebug (Render no longer carries it)
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Sim/SimSettings.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace Arcane::Test
{
    namespace
    {
        std::string Hex8(float f)
        {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%08x", std::bit_cast<std::uint32_t>(f));
            return buf;
        }

        std::string Hex16(std::uint64_t v)
        {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
            return buf;
        }

        std::string GuidText(const Arcane::Guid& g)
        {
            if (g.IsNil()) return "0";
            std::string s = g.ToString();
            s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        std::string Side(const Arcane::ContactSide2D& s)
        {
            return std::to_string(s.entity.GetID()) + " " + std::to_string(s.entity.GetVersion()) + " "
                 + GuidText(s.guid) + " " + std::to_string(s.fixture);
        }

        template<typename Span, typename Fn>
        void Block(std::string& out, const char* name, const Span& events, Fn&& line)
        {
            out += name; out += ' '; out += std::to_string(events.size()); out += '\n';
            for (const auto& e : events) { out += line(e); out += '\n'; }
        }

        void AddBody(Arcane::Registry& reg, Arcane::Entity e, glm::vec3 pos, Arcane::BodyType2D type,
                     float mass, const Arcane::Fixture2D& fx)
        {
            Arcane::Transform t; t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
            Arcane::RigidBody2D rb; rb.type = type; rb.mass = mass;
            // Solver invariant (R8): a dynamic Aabb shape must be fixedRotation.
            if (type == Arcane::BodyType2D::Dynamic && fx.kind == Arcane::ShapeKind2D::Aabb) rb.fixedRotation = true;
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);
            Arcane::Collider2D col; col.fixtures.push_back(fx);
            reg.AddComponent<Arcane::Collider2D>(e, col);
        }

        Arcane::Fixture2D DynFixture(Arcane::ShapeKind2D kind)
        {
            Arcane::Fixture2D f; f.kind = kind;
            f.isSensor = false; f.contactEvents = true; f.hitEvents = true; f.sensorEvents = true;   // the floor overrides this to false (tangent to the sensor)
            return f;
        }

        std::uint64_t PackFixture(Manifold2D::Physics::FixtureHandle h)
        {
            return (static_cast<std::uint64_t>(h.index) << 32) | h.generation;
        }

        const Arcane::Affine2D kOrdinary{ { 640.0f, 360.0f }, { 100.0f, -100.0f } };
        const Arcane::Affine2D kAlt{ { 100.0f, 80.0f }, { 50.0f, -40.0f } };

        std::string DumpGeometry(const std::string& header, const Arcane::PhysicsDebugGeometry& g)
        {
            std::string out = header; out += '\n';
            for (const auto& p : g.primitives)
            {
                if (p.kind == Arcane::PhysicsDebugPrimitive::Kind::Line)
                    out += "line " + Hex8(p.a.x) + " " + Hex8(p.a.y) + " " + Hex8(p.b.x) + " " + Hex8(p.b.y) + " "
                         + Hex8(p.size) + " " + Hex8(p.color.r) + " " + Hex8(p.color.g) + " " + Hex8(p.color.b) + " "
                         + Hex8(p.color.a) + "\n";
                else
                    out += "circle " + Hex8(p.a.x) + " " + Hex8(p.a.y) + " " + Hex8(p.size) + " "
                         + Hex8(p.color.r) + " " + Hex8(p.color.g) + " " + Hex8(p.color.b) + " " + Hex8(p.color.a) + "\n";
            }
            return out;
        }

        const Manifold2D::Physics::PhysicsWorld& Solver(Arcane::Runtime& rt)
        {
            auto* w = rt.Registry().GetResource<Arcane::PhysicsWorld2D>();
            REQUIRE(w != nullptr);
            const auto* s = Arcane::Detail::Physics2D::Access::Solver(*w);
            REQUIRE(s != nullptr);
            return *s;
        }

        // Compare a checked-in text snapshot: hash file, bytes, live dump.
        // The primitive lines only: drops the header line, so a compare cannot pass on the name alone.
        std::string WithoutHeader(const std::string& dump)
        {
            const auto nl = dump.find('\n');
            return nl == std::string::npos ? std::string() : dump.substr(nl + 1);
        }

        void CompareSnapshot(const char* stem, const std::string& live)
        {
            const auto dir = RepoRoot() / "Tests/ArcaneTests/data/physics";
            const auto path = dir / (std::string(stem) + ".txt");
            const auto hashPath = dir / (std::string(stem) + ".sha256");
            REQUIRE(fs::exists(path));
            REQUIRE(fs::exists(hashPath));
            const std::string disk = ReadFileBinary(path);
            std::string expect = ReadFileBinary(hashPath);
            if (!expect.empty() && expect.back() == '\n') expect.pop_back();
            CHECK(Arcane::Crypto::Sha256Hex(disk) == expect);
            CHECK(live == disk);
        }

        void WriteSnapshot(const char* stem, const std::string& bytes)
        {
            const auto dir = RepoRoot() / "Tests/ArcaneTests/data/physics";
            fs::create_directories(dir);
            WriteFileBinary(dir / (std::string(stem) + ".txt"), bytes);
            WriteFileBinary(dir / (std::string(stem) + ".sha256"), Arcane::Crypto::Sha256Hex(bytes) + "\n");
        }

        bool CaptureArmed()
        {
            const char* arm = std::getenv("ARCANE_CAPTURE_PHYSICS_BASELINE");
            return arm && arm[0] == '1';
        }
    }

    std::string ReadFileBinary(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void WriteFileBinary(const fs::path& path, const std::string& bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    V1PhysicsScene BuildV1PhysicsScene(Arcane::Registry& reg)
    {
        V1PhysicsScene s;
        {   // Floor
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Aabb);
            f.halfW = 5.0f; f.halfH = 0.5f; f.sensorEvents = false;
            s.entities[0] = reg.CreateEntity();
            AddBody(reg, s.entities[0], { 0.0f, -1.0f, 0.0f }, Arcane::BodyType2D::Static, 0.0f, f);
        }
        {   // Circle
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Circle);
            f.radius = 0.5f;
            s.entities[1] = reg.CreateEntity();
            AddBody(reg, s.entities[1], { 0.0f, 2.0f, 0.0f }, Arcane::BodyType2D::Dynamic, 1.0f, f);
        }
        {   // Capsule
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Capsule);
            f.radius = 0.2f; f.halfLen = 0.4f;
            s.entities[2] = reg.CreateEntity();
            AddBody(reg, s.entities[2], { 1.2f, 3.0f, 0.0f }, Arcane::BodyType2D::Dynamic, 1.0f, f);
        }
        {   // Box
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Aabb);
            f.halfW = 0.3f; f.halfH = 0.25f;
            s.entities[3] = reg.CreateEntity();
            AddBody(reg, s.entities[3], { -0.8f, 2.5f, 0.0f }, Arcane::BodyType2D::Dynamic, 1.0f, f);
        }
        {   // Sensor
            Arcane::Fixture2D f = DynFixture(Arcane::ShapeKind2D::Circle);
            f.radius = 0.4f; f.isSensor = true; f.sensorEvents = true; f.contactEvents = false; f.hitEvents = false;
            s.entities[4] = reg.CreateEntity();
            // Above the resting height of every body, so the falling circle enters and then leaves it.
            AddBody(reg, s.entities[4], { 0.0f, 1.5f, 0.0f }, Arcane::BodyType2D::Static, 0.0f, f);
        }
        return s;
    }

    std::string WriteV1StepTrace(Arcane::Runtime& rt, int steps)
    {
        REQUIRE(Arcane::Settings<Arcane::SimSettings>().fixedHz == 60.0);
        std::string out = "arcane-physics-v1-step-trace 1\n";
        out += "steps " + std::to_string(steps) + "\n";
        // The header prints the setting actually in force; the baseline is defined for the default (off).
        const bool parallelSolver = Arcane::Settings<Arcane::PhysicsWorldSettings2D>().parallelSolver;
        REQUIRE_FALSE(parallelSolver);
        out += std::string("parallelSolver ") + (parallelSolver ? "1" : "0") + "\n";
        auto& reg = rt.Registry();
        for (int step = 1; step <= steps; ++step)
        {
            rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
            out += "step " + std::to_string(step) + "\n";

            struct Row { std::uint32_t idx, gen; float x, y, angle; };
            std::vector<Row> rows;
            reg.CreateView<const Arcane::RigidBody2D, const Arcane::Transform>()
                .ForEach([&](Arcane::Entity e, const Arcane::RigidBody2D&, const Arcane::Transform& t) {
                    rows.push_back({ static_cast<std::uint32_t>(e.GetID()), static_cast<std::uint32_t>(e.GetVersion()),
                                     t.position.x, t.position.y, Arcane::RotationZ(t.rotation) });
                });
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                return a.idx != b.idx ? a.idx < b.idx : a.gen < b.gen; });
            out += "bodies " + std::to_string(rows.size()) + "\n";
            for (const Row& r : rows)
                out += std::to_string(r.idx) + " " + std::to_string(r.gen) + " " + Hex8(r.x) + " " + Hex8(r.y) + " " + Hex8(r.angle) + "\n";

            auto* w = reg.GetResource<Arcane::PhysicsWorld2D>();
            REQUIRE(w != nullptr);
            const Arcane::PhysicsEvents2D ev = w->StepEvents();
            const auto two = [](const auto& e) { return Side(e.a) + " " + Side(e.b); };
            Block(out, "contactBegin", ev.contactBegin, two);
            Block(out, "contactEnd", ev.contactEnd, two);
            Block(out, "contactHit", ev.contactHit, [&](const Arcane::ContactHit2D& e) {
                return two(e) + " " + Hex8(e.point.x) + " " + Hex8(e.point.y) + " " + Hex8(e.normal.x) + " "
                     + Hex8(e.normal.y) + " " + Hex8(e.approachSpeed); });
            const auto sv = [](const auto& e) { return Side(e.sensor) + " " + Side(e.visitor); };
            Block(out, "sensorBegin", ev.sensorBegin, sv);
            Block(out, "sensorEnd", ev.sensorEnd, sv);
        }
        return out;
    }

    std::string DumpV1Debug(Arcane::Runtime& rt, const V1PhysicsScene& scene, V1DebugView view)
    {
        auto& reg = rt.Registry();
        auto* phys = reg.GetResource<Arcane::PhysicsWorld2D>();
        REQUIRE(phys != nullptr);
        const auto& solver = Solver(rt);

        Arcane::PhysicsDebugDrawOptions2D opts = Arcane::MakePhysicsDebugDrawOptions();
        const bool alt = view == V1DebugView::AltView || view == V1DebugView::AltViewAlpha0;
        opts.view = alt ? kAlt : kOrdinary;
        opts.alpha = view == V1DebugView::AltView ? 0.25f : 0.0f;
        // EnsurePhysics creates PhysicsInterpBuffer2D (Runtime.cpp:654), so this is a real buffer, read as ExtractFrame does.
        opts.interp = reg.GetResource<Arcane::PhysicsInterpBuffer2D>();
        opts.velocities = false;
        opts.comMarkers = false;
        opts.orientations = false;
        opts.contacts = view != V1DebugView::SelectedOnly;
        const char* name = "wholeworld";
        if (view == V1DebugView::SelectedOnly)
        {
            name = "selected";
            const auto& ents = Arcane::Detail::Physics2D::Access::Entities(*phys);
            const auto it = ents.find(scene.Circle());
            REQUIRE(it != ents.end());
            opts.onlyBody = Arcane::Detail::Physics2D::PackBody(it->second);
        }
        else if (view == V1DebugView::AltView) name = "altview step 20";
        else if (alt) name = "altview-alpha0";

        Arcane::PhysicsDebugGeometry geo;
        Arcane::GatherPhysicsDebug(solver, opts, Arcane::Settings<Arcane::PhysicsDebugStyleSettings2D>(), geo);
        const std::string header = alt
            ? std::string("arcane-physics-v1-debug 1 ") + name + (view == V1DebugView::AltView ? " alpha 0.25" : " alpha 0") + " offset 100 80 scale 50 -40"
            : std::string("arcane-physics-v1-debug 1 ") + name + " alpha 0 offset 640 360 scale 100 -100";
        return DumpGeometry(header, geo);
    }

    std::string DumpV1Inspector(Arcane::Runtime& rt, const V1PhysicsScene& scene)
    {
        auto* phys = rt.Registry().GetResource<Arcane::PhysicsWorld2D>();
        REQUIRE(phys != nullptr);
        const auto& solver = Solver(rt);
        const auto& ents = Arcane::Detail::Physics2D::Access::Entities(*phys);
        const auto fi = ents.find(scene.Floor());
        const auto ci = ents.find(scene.Circle());
        REQUIRE(fi != ents.end());
        REQUIRE(ci != ents.end());
        REQUIRE(solver.FixtureCount(fi->second) >= 1u);
        REQUIRE(solver.FixtureCount(ci->second) >= 1u);
        const auto fa = solver.GetBodyFixture(fi->second, 0u);
        const auto fb = solver.GetBodyFixture(ci->second, 0u);
        Manifold2D::Physics::NarrowphaseTrace trace;
        (void)solver.DebugCollide(fa, fb, trace);
        Arcane::PhysicsDebugGeometry geo;
        Arcane::GatherNarrowphaseWorldOverlay(trace, -1, kOrdinary, std::nullopt, 1.0f,
            Arcane::Settings<Arcane::PhysicsDebugTraceSettings2D>(),
            Arcane::Settings<Arcane::PhysicsDebugColorSettings2D>(),
            Arcane::Settings<Arcane::PhysicsDebugStyleSettings2D>(), geo);
        const std::string header = "arcane-physics-v1-inspector 1 step -1 emphasis 1 thickness none fixtureA "
            + Hex16(PackFixture(fa)) + " fixtureB " + Hex16(PackFixture(fb));
        return DumpGeometry(header, geo);
    }

    std::string DumpV1Pick(Arcane::Runtime& rt)
    {
        std::vector<Arcane::PickDrawable> drawables;
        Arcane::CollectPickables(rt.Registry(), drawables);
        std::string out = "arcane-physics-v1-pick 1\n";
        for (const auto& d : drawables)
        {
            const std::string who = std::to_string(d.entity.GetID()) + " " + std::to_string(d.entity.GetVersion());
            const std::string c = Hex8(d.center.x) + " " + Hex8(d.center.y) + " " + Hex8(d.center.z);
            switch (d.kind)
            {
            case Arcane::PickDrawable::Kind::Circle:
                out += "circle " + who + " " + c + " " + Hex8(d.radius) + " " + Hex8(d.angle) + "\n"; break;
            case Arcane::PickDrawable::Kind::Capsule:
                out += "capsule " + who + " " + c + " " + Hex8(d.halfLen) + " " + Hex8(d.radius) + " " + Hex8(d.angle) + "\n"; break;
            case Arcane::PickDrawable::Kind::Box:
                out += "box " + who + " " + c + " " + Hex8(d.halfExtents.x) + " " + Hex8(d.halfExtents.y) + " " + Hex8(d.angle) + "\n"; break;
            default:
                FAIL("unexpected Quad or Mesh pick drawable in the v1 physics scene");
            }
        }
        return out;
    }

    namespace
    {
        // Rebuild the scene and run the 120 steps; returns the scene.
        V1PhysicsScene StepScene(Arcane::Runtime& rt, int steps = 120)
        {
            const V1PhysicsScene scene = BuildV1PhysicsScene(rt.Registry());
            rt.EnsurePhysics();
            (void)WriteV1StepTrace(rt, steps);
            return scene;
        }
    }
}

using namespace Arcane::Test;

TEST_CASE("v1 step trace matches the checked-in bytes", "[physics][baseline]")
{
    const auto path = RepoRoot() / "Tests/ArcaneTests/data/physics/v1-step-trace.txt";
    const auto hashPath = RepoRoot() / "Tests/ArcaneTests/data/physics/v1-step-trace.sha256";
    REQUIRE(fs::exists(path));
    const std::string disk = ReadFileBinary(path);
    std::string expect = ReadFileBinary(hashPath);
    if (!expect.empty() && expect.back() == '\n') expect.pop_back();
    CHECK(Arcane::Crypto::Sha256Hex(disk) == expect);

    // The trace must pin sensor behavior: at least one sensorBegin span is non-empty.
    bool anySensorBegin = false;
    for (std::size_t at = disk.find("\nsensorBegin "); at != std::string::npos; at = disk.find("\nsensorBegin ", at + 1))
        if (std::atoi(disk.c_str() + at + 13) > 0) { anySensorBegin = true; break; }
    CHECK(anySensorBegin);
    bool anySensorEnd = false;
    for (std::size_t at = disk.find("\nsensorEnd "); at != std::string::npos; at = disk.find("\nsensorEnd ", at + 1))
        if (std::atoi(disk.c_str() + at + 11) > 0) { anySensorEnd = true; break; }
    CHECK(anySensorEnd);

    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(rt.Registry());
    rt.EnsurePhysics();
    const std::string live = WriteV1StepTrace(rt, 120);
    CHECK(live == disk);
}

TEST_CASE("v1 whole-world debug matches the checked-in bytes", "[physics][baseline]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    const auto scene = StepScene(rt);
    CompareSnapshot("v1-debug-wholeworld", DumpV1Debug(rt, scene, V1DebugView::WholeWorld));
}

TEST_CASE("v1 selected-only debug matches the checked-in bytes", "[physics][baseline]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    const auto scene = StepScene(rt);
    CompareSnapshot("v1-debug-selected", DumpV1Debug(rt, scene, V1DebugView::SelectedOnly));
}

TEST_CASE("v1 alt-view debug matches the checked-in bytes", "[physics][baseline]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    // Step 20: all three dynamic bodies are in free fall, so the interpolated pose
    // (alpha 0.25) differs from the current one. At rest alpha would pin nothing.
    const auto scene = StepScene(rt, 20);
    const std::string altDump = DumpV1Debug(rt, scene, V1DebugView::AltView);
    CompareSnapshot("v1-debug-altview", altDump);
    const std::string alpha0Dump = DumpV1Debug(rt, scene, V1DebugView::AltViewAlpha0);
    CHECK_FALSE(WithoutHeader(altDump).empty());
    CHECK(WithoutHeader(altDump) != WithoutHeader(alpha0Dump));

    const auto dir = RepoRoot() / "Tests/ArcaneTests/data/physics";
    const auto whole = ReadFileBinary(dir / "v1-debug-wholeworld.txt");
    const auto alt = ReadFileBinary(dir / "v1-debug-altview.txt");
    CHECK(whole != alt);
}

TEST_CASE("v1 inspector overlay matches the checked-in bytes", "[physics][baseline]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    const auto scene = StepScene(rt);
    CompareSnapshot("v1-inspector", DumpV1Inspector(rt, scene));
}

TEST_CASE("v1 collider pick matches the checked-in bytes", "[physics][baseline]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    StepScene(rt);
    CompareSnapshot("v1-pick", DumpV1Pick(rt));
}

TEST_CASE("PhysicsSystem.hpp is gone and the pass body is not a header", "[physics][baseline]")
{
    const auto root = Arcane::Test::RepoRoot();
    CHECK_FALSE(std::filesystem::exists(root / "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem.hpp"));
    const std::string cpp = ReadFileBinary(root / "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem.cpp");
    CHECK(cpp.find("PhysicsSystem2D::operator()") != std::string::npos);
    CHECK(cpp.find("Physics2DDetail.hpp") != std::string::npos);
}

TEST_CASE("capture v1 step trace", "[.][physics-baseline]")
{
    if (!CaptureArmed()) return;
    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(rt.Registry());
    rt.EnsurePhysics();
    const std::string live = WriteV1StepTrace(rt, 120);
    WriteSnapshot("v1-step-trace", live);
}

TEST_CASE("capture v1 debug and pick snapshots", "[.][physics-baseline]")
{
    if (!CaptureArmed()) return;
    Arcane::Runtime rt(Arcane::Test::Process());
    const auto scene = StepScene(rt);
    WriteSnapshot("v1-debug-wholeworld", DumpV1Debug(rt, scene, V1DebugView::WholeWorld));
    WriteSnapshot("v1-debug-selected", DumpV1Debug(rt, scene, V1DebugView::SelectedOnly));
    WriteSnapshot("v1-inspector", DumpV1Inspector(rt, scene));
    WriteSnapshot("v1-pick", DumpV1Pick(rt));

    Arcane::Runtime rtAlt(Arcane::Test::Process());
    const auto sceneAlt = StepScene(rtAlt, 20);
    WriteSnapshot("v1-debug-altview", DumpV1Debug(rtAlt, sceneAlt, V1DebugView::AltView));
}
