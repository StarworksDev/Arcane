// Settings arc S6-8: the fixed-step clock (sim.*), the dedicated server's tick
// (server.tickHz) and the two remaining job-system sizes (jobs.externalThreads,
// jobs.shaderCompileThreads). Every default is the pre-sweep literal, bit for
// bit; the frame clamp and the step cap are read live by both host frames.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Helpers/TestTypeContext.hpp"
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Config/Bindings/JobsBinding.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SimSettings.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include "ServerConfig.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    // Drops this test's Code-rung value on one cvar however the case exits,
    // and leaves every other cvar's Code rung alone.
    struct ClearCodeRung
    {
        CVarHandle handle;
        ~ClearCodeRung()
        {
            CVarRegistry::Get().ClearRung(handle, SetBy::Code);
            CVarRegistry::Get().PublishImmediate();
        }
    };

    Server::ServerConfig::ParseOutcome ParseServer(std::vector<std::string> args)
    {
        args.insert(args.begin(), "ArcaneServer");
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(a.data());
        return Server::ServerConfig::Parse(static_cast<int>(argv.size()), argv.data());
    }
}

TEST_CASE("sweep: sim/server defaults are the pre-sweep literals", "[sweep][sim]")
{
    CHECK(Test::SameBits(SimSettings{}.fixedHz, 60.0));
    CHECK(SimSettings{}.maxStepsPerFrame == 5);
    CHECK(Test::SameBits(SimSettings{}.maxFrameDeltaSeconds, 0.25));
    CHECK(Test::SameBits(ServerSettings{}.tickHz, 60.0));
    CHECK(Test::SameBits(HostConfig{}.fixedDtSeconds, 1.0 / 60.0));        // the old literal, bit for bit
    CHECK(Test::SameBits(1.0 / (1.0 / SimSettings{}.fixedHz), 60.0));      // ServerApp's round trip stays exact
    CHECK(RunLoop::Config{}.maxStepsPerFrame == 5);
    Test::RequireDefault("sim.fixedHz", CVarValue::Float64(60.0));
    Test::RequireDefault("sim.maxStepsPerFrame", CVarValue::Int32(5));
    Test::RequireDefault("sim.maxFrameDeltaSeconds", CVarValue::Float64(0.25));
    Test::RequireDefault("server.tickHz", CVarValue::Float64(60.0));
    const auto e = CVarRegistry::Get().Explain("sim.maxFrameDeltaSeconds");
    REQUIRE(e);
    CHECK(HasFlag(e->flags, CVarFlags::Deterministic));

    CVarRegistry& reg = CVarRegistry::Get();
    const auto hz = reg.Explain("sim.fixedHz");
    REQUIRE(hz);
    CHECK(hz->apply == ApplyMode::NextWorld);
    CHECK(hz->audience == Audience::Game);
    const auto steps = reg.Explain("sim.maxStepsPerFrame");
    REQUIRE(steps);
    CHECK(steps->apply == ApplyMode::Live);
    CHECK(HasFlag(steps->flags, CVarFlags::Deterministic));
    CHECK(e->apply == ApplyMode::Live);
    const auto tick = reg.Explain("server.tickHz");
    REQUIRE(tick);
    CHECK(tick->audience == Audience::Server);
    CHECK(tick->scope == SettingScope::Project);
    CHECK(tick->apply == ApplyMode::Restart);
    CHECK(HasFlag(tick->flags, CVarFlags::Deterministic));
}

TEST_CASE("sweep: ClampFrameDelta reads sim.maxFrameDeltaSeconds", "[sweep][sim]")
{
    CHECK(ClampFrameDelta(1.0) == 0.25);
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("sim.maxFrameDeltaSeconds");
    ClearCodeRung restore{ h };
    REQUIRE(reg.Set(h, CVarValue::Float64(0.5), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();
    CHECK(ClampFrameDelta(1.0) == 0.5);
    CHECK(ClampFrameDelta(0.1) == 0.1);
}

TEST_CASE("sweep: ApplySimStepCap is Live -- a RunLoop takes sim.maxStepsPerFrame on the next frame", "[sweep][sim]")
{
    Astra::Registry registry;
    SystemSchedulers schedulers(nullptr);   // sequential executor
    RunLoop loop(registry, schedulers, RunLoop::Config{ .fixedHz = 60.0, .maxStepsPerFrame = 2 });
    CHECK(loop.MaxStepsPerFrame() == 2);
    ApplySimStepCap(loop);
    CHECK(loop.MaxStepsPerFrame() == 5);   // the published default

    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("sim.maxStepsPerFrame");
    ClearCodeRung restore{ h };
    REQUIRE(reg.Set(h, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();
    ApplySimStepCap(loop);
    CHECK(loop.MaxStepsPerFrame() == 1);
    int fixed = 0;
    loop.Advance(0.25, [&](double) { ++fixed; }, [](double, double) {});
    CHECK(fixed == 1);                     // the cap holds a 15-step hitch to one step
}

TEST_CASE("sweep: server.tickHz paces the dedicated server unless --fixed-dt is supplied", "[sweep][sim][server]")
{
    const auto byDefault = ParseServer({ "--project", "P" });
    REQUIRE(byDefault.config);
    CHECK_FALSE(byDefault.config->fixedDtSupplied);
    CHECK(Test::SameBits(byDefault.config->FixedHz(), 60.0));

    const auto flag = ParseServer({ "--project", "P", "--fixed-dt", "0.05" });
    REQUIRE(flag.config);
    CHECK(flag.config->fixedDtSupplied);
    CHECK(flag.config->FixedHz() == 1.0 / 0.05);

    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("server.tickHz");
    ClearCodeRung restore{ h };
    REQUIRE(reg.Set(h, CVarValue::Float64(30.0), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();
    CHECK(byDefault.config->FixedHz() == 30.0);
    CHECK(flag.config->FixedHz() == 1.0 / 0.05);   // the flag still wins
}

TEST_CASE("sweep: jobs.externalThreads / jobs.shaderCompileThreads defaults are today's", "[sweep][sim]")
{
    CHECK(JobsSettings{}.externalThreads == 0u);
    CHECK(JobsSettings{}.shaderCompileThreads == 1u);
    Test::RequireDefault("jobs.externalThreads", CVarValue::UInt32(0));
    Test::RequireDefault("jobs.shaderCompileThreads", CVarValue::UInt32(1));
    for (const char* name : { "jobs.externalThreads", "jobs.shaderCompileThreads" })
    {
        INFO(name);
        if (!Test::InThisBuild(name)) continue;   // Dev: compiled out of Dist
        const auto e = CVarRegistry::Get().Explain(name);
        REQUIRE(e);
        CHECK(e->apply == ApplyMode::Restart);
        CHECK(HasFlag(e->flags, CVarFlags::Dev));
    }
}

namespace
{
    // One free-falling dynamic box, stepped exactly once at `hz`; returns its
    // vertical velocity after that step (gravity * the step physics took).
    float FallVelocityAfterOneStep(double hz, bool rerate)
    {
        Runtime rt(Test::Process());
        if (rerate) rt.SetFixedHz(hz);
        auto& reg = rt.Registry();
        RegisterSceneComponents(reg);
        const Astra::Entity box = reg.CreateEntity();
        reg.AddComponent<Transform>(box, Transform{ .position = { 0.0f, 10.0f, 0.0f } });
        Arcane::RigidBody2D body; body.type = Arcane::BodyType2D::Dynamic; body.fixedRotation = true;
        reg.AddComponent<Arcane::RigidBody2D>(box, body);
        Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Aabb; fx.halfW = 0.5f; fx.halfH = 0.5f;
        Arcane::Collider2D col; col.fixtures.push_back(fx);
        reg.AddComponent<Arcane::Collider2D>(box, col);
        rt.EnsurePhysics();
        REQUIRE(rt.Loop().FixedHz() == hz);
        rt.Loop().Advance(1.0 / hz);   // exactly one fixed step
        Arcane::RigidBody2D& rb = *reg.GetComponent<Arcane::RigidBody2D>(box);
        const Arcane::BodyMotion2D m = reg.GetResource<Arcane::PhysicsWorld2D>()->Motion(box, rb);
        REQUIRE(m.bodyReady);
        return m.velocityY;
    }
}

TEST_CASE("sweep: Runtime::SetFixedHz re-rates the physics step with the loop (server.tickHz != sim.fixedHz)", "[sweep][sim][server]")
{
    // S6-8 deferral, owned by S6-GATE: ArcaneServer re-rated only the RunLoop,
    // so with server.tickHz = 30 each tick still stepped physics by
    // 1/sim.fixedHz and the simulation ran at half speed.
    const float g = Runtime(Test::Process()).ResolvedGravity().y;
    REQUIRE(g < 0.0f);
    const float at60 = FallVelocityAfterOneStep(60.0, false);
    const float at30 = FallVelocityAfterOneStep(30.0, true);
    INFO("g " << g << ", one step at 60 Hz " << at60 << ", at 30 Hz " << at30);
    CHECK(std::abs(at60 - g / 60.0f) < 1e-4f);
    CHECK(std::abs(at30 - g / 30.0f) < 1e-4f);   // the step the loop ran, not 1/sim.fixedHz

    // A ClearSystems reinstall (module unload) keeps the re-rated step.
    Runtime rt(Test::Process());
    rt.SetFixedHz(30.0);
    rt.ClearSystems();
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>());
    CHECK(rt.Loop().FixedHz() == 30.0);
    rt.SetFixedHz(0.0);                       // refused
    rt.SetFixedHz(std::nan(""));              // refused
    CHECK(rt.Loop().FixedHz() == 30.0);
}
