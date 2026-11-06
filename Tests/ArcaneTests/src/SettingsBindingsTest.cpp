// The library and system bindings (settings arc S2, spec s5): pure functions,
// pinned field by field, with defaults that ARE the library defaults (the
// fixtures and goldens unchanged), and their read sites.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/DiagnosticsSettings.hpp>
#include <Arcane/Config/Bindings/AstraBinding.hpp>
#include <Arcane/Config/Bindings/JobsBinding.hpp>
#include <Arcane/Config/Bindings/LogBinding.hpp>
#include <Arcane/Config/Bindings/Physics2DBinding.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Mosaic/Log.hpp>
#include <Arcane/Host/EarlyConfig.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SimSettings.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>

#include "Helpers/TestTypeContext.hpp"
#include "Helpers/SettingsSweep.hpp"   // Test::ScopedCodeRung
#include "Settings/EditorSnapshotSettings.hpp"   // astra.snapshot.compression (an Editor setting)

#include <filesystem>
#include <fstream>
#include <system_error>

using namespace Arcane;

TEST_CASE("ToAstraConfig: the default settings ARE Astra's Registry::Config defaults, field by field", "[settings]")
{
    const Astra::Registry::Config lib{};
    const Astra::Registry::Config ours = ToAstraConfig(AstraMemorySettings{}, nullptr);
    CHECK(ours.chunkPoolConfig.chunkSize == lib.chunkPoolConfig.chunkSize);
    CHECK(ours.chunkPoolConfig.chunksPerBlock == lib.chunkPoolConfig.chunksPerBlock);
    CHECK(ours.chunkPoolConfig.maxChunks == lib.chunkPoolConfig.maxChunks);
    CHECK(ours.chunkPoolConfig.initialBlocks == lib.chunkPoolConfig.initialBlocks);
    CHECK(ours.chunkPoolConfig.useHugePages == lib.chunkPoolConfig.useHugePages);
    CHECK(ours.chunkPoolConfig.minChunkBytes == lib.chunkPoolConfig.minChunkBytes);
    CHECK(ours.chunkPoolConfig.maxChunkBytes == lib.chunkPoolConfig.maxChunkBytes);
    CHECK(ours.chunkPoolConfig.growDivisor == lib.chunkPoolConfig.growDivisor);
    const auto& t = ours.entityManagerConfig.tableConfig;
    const auto& lt = lib.entityManagerConfig.tableConfig;
    CHECK(t.entitiesPerSegment == lt.entitiesPerSegment);
    CHECK(t.entitiesPerSegmentShift == lt.entitiesPerSegmentShift);
    CHECK(t.entitiesPerSegmentMask == lt.entitiesPerSegmentMask);
    CHECK(t.releaseThreshold == lt.releaseThreshold);
    CHECK(t.autoRelease == lt.autoRelease);
    CHECK(t.maxEmptySegments == lt.maxEmptySegments);
    CHECK(t.maxPooledSegments == lt.maxPooledSegments);
    CHECK(t.useHugePages == lt.useHugePages);
    CHECK(ours.resourceStorageConfig.initialResourceCapacity == lib.resourceStorageConfig.initialResourceCapacity);
    CHECK(ours.workScheduler == nullptr);
}

TEST_CASE("ToAstraConfig: every field reaches its Astra field, and the scheduler is passed through", "[settings]")
{
    AstraMemorySettings s;
    s.chunkSize = 32768;
    s.chunksPerBlock = 64;
    s.maxChunks = 8192;
    s.initialBlocks = 2;
    s.chunkHugePages = false;
    s.minChunkBytes = 8192;
    s.maxChunkBytes = 262144;
    s.growDivisor = 4;
    s.entitiesPerSegment = 4096;
    s.entityReleaseThreshold = 0.25f;
    s.entityAutoRelease = false;
    s.maxEmptySegments = 3;
    s.maxPooledSegments = 7;
    s.entityHugePages = false;
    s.initialResourceCapacity = 64;
    JobSystem jobs(1);
    const std::shared_ptr<Astra::IWorkScheduler> sched = jobs.WorkScheduler();
    const Astra::Registry::Config cfg = ToAstraConfig(s, sched);
    CHECK(cfg.chunkPoolConfig.chunkSize == 32768u);
    CHECK(cfg.chunkPoolConfig.chunksPerBlock == 64u);
    CHECK(cfg.chunkPoolConfig.maxChunks == 8192u);
    CHECK(cfg.chunkPoolConfig.initialBlocks == 2u);
    CHECK_FALSE(cfg.chunkPoolConfig.useHugePages);
    CHECK(cfg.chunkPoolConfig.minChunkBytes == 8192u);
    CHECK(cfg.chunkPoolConfig.maxChunkBytes == 262144u);
    CHECK(cfg.chunkPoolConfig.growDivisor == 4u);
    const auto& t = cfg.entityManagerConfig.tableConfig;
    CHECK(t.entitiesPerSegment == 4096u);
    CHECK(t.entitiesPerSegmentShift == 12u);                    // derived by EntityManager::Config's ctor
    CHECK(t.entitiesPerSegmentMask == 4095u);
    CHECK(t.releaseThreshold == 0.25f);
    CHECK_FALSE(t.autoRelease);
    CHECK(t.maxEmptySegments == 3u);
    CHECK(t.maxPooledSegments == 7u);
    CHECK_FALSE(t.useHugePages);
    CHECK(cfg.resourceStorageConfig.initialResourceCapacity == 64u);
    CHECK(cfg.workScheduler == sched);
}

TEST_CASE("astra.memory.* are registered with the inventory's metadata and defaults", "[settings]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const auto maxChunks = reg.Explain("astra.memory.maxChunks");
    REQUIRE(maxChunks);
    CHECK(maxChunks->type == CVarType::UInt64);
    CHECK(maxChunks->published.AsUInt64() == 4096u);
    CHECK_FALSE(HasFlag(maxChunks->flags, CVarFlags::Dev));     // the one Game (not Dev) row: a world-size ceiling
    CHECK(maxChunks->scope == SettingScope::Project);
    CHECK(maxChunks->apply == ApplyMode::NextWorld);
#if !defined(ARC_BUILD_DIST)
    const auto chunkSize = reg.Explain("astra.memory.chunkSize");
    REQUIRE(chunkSize);
    CHECK(HasFlag(chunkSize->flags, CVarFlags::Dev));
    CHECK(chunkSize->published.AsUInt64() == 16384u);
    CHECK(reg.Explain("astra.memory.entitiesPerSegment")->type == CVarType::UInt32);
    CHECK(reg.Explain("astra.memory.entityReleaseThreshold")->type == CVarType::Float32);
#endif
    CHECK(Settings<AstraMemorySettings>().maxChunks == 4096u);
}

namespace
{
    // Drops a test's Code record on scope exit, so a failed CHECK cannot leak a value into later cases.
    struct ClearCodeOnExit
    {
        Arcane::CVarHandle handle;
        ~ClearCodeOnExit()
        {
            Arcane::CVarRegistry::Get().ClearRung(handle, Arcane::SetBy::Code);
            Arcane::CVarRegistry::Get().Publish();
        }
    };
}

TEST_CASE("ToWorldDef: the default settings ARE Manifold2D's WorldDef defaults; gravity is left to EnsurePhysics", "[settings]")
{
    const Manifold2D::Physics::WorldDef lib{};
    const Manifold2D::Physics::WorldDef ours = Arcane::Detail::Physics2D::ToWorldDef(Arcane::PhysicsWorldSettings2D{});
    CHECK(ours.broadphase == lib.broadphase);
    CHECK(ours.hashCellSize == lib.hashCellSize);
    CHECK(ours.passability == nullptr);
    CHECK(ours.tileCellSize == lib.tileCellSize);
    CHECK(ours.tileOrigin.x == lib.tileOrigin.x);
    CHECK(ours.tileOrigin.y == lib.tileOrigin.y);
    CHECK(ours.gravityX == lib.gravityX);                       // DERIVED: Runtime::ResolvedGravity sets it
    CHECK(ours.gravityY == lib.gravityY);
    CHECK(ours.substepCount == lib.substepCount);
    CHECK(ours.contactHertz == lib.contactHertz);
    CHECK(ours.contactDampingRatio == lib.contactDampingRatio);
    CHECK(ours.restitutionThreshold == lib.restitutionThreshold);
    CHECK(ours.contactPushMaxVelocity == lib.contactPushMaxVelocity);
    CHECK(ours.maxLinearVelocity == lib.maxLinearVelocity);
    CHECK(ours.sleepThreshold == lib.sleepThreshold);
    CHECK_FALSE(Arcane::PhysicsWorldSettings2D{}.parallelSolver);
}

TEST_CASE("ToWorldDef: every field reaches its WorldDef field; every broadphase maps", "[settings]")
{
    Arcane::PhysicsWorldSettings2D s;
    s.broadphase = Arcane::PhysicsBroadphase2D::Hash;
    s.hashCellSize = 2.5f;
    s.substepCount = 8;
    s.contactHertz = 60.0f;
    s.contactDampingRatio = 5.0f;
    s.restitutionThreshold = 0.5f;
    s.contactPushMaxVelocity = 6.0f;
    s.maxLinearVelocity = 100.0f;
    s.sleepThreshold = 0.1f;
    const Manifold2D::Physics::WorldDef wd = Arcane::Detail::Physics2D::ToWorldDef(s);
    CHECK(wd.broadphase == Manifold2D::Physics::BroadphaseKind::Hash);
    CHECK(wd.hashCellSize == 2.5f);
    CHECK(wd.substepCount == 8u);
    CHECK(wd.contactHertz == 60.0f);
    CHECK(wd.contactDampingRatio == 5.0f);
    CHECK(wd.restitutionThreshold == 0.5f);
    CHECK(wd.contactPushMaxVelocity == 6.0f);
    CHECK(wd.maxLinearVelocity == 100.0f);
    CHECK(wd.sleepThreshold == 0.1f);
    CHECK(Arcane::Detail::Physics2D::ToBroadphaseKind(Arcane::PhysicsBroadphase2D::Tree) == Manifold2D::Physics::BroadphaseKind::Tree);
    CHECK(Arcane::Detail::Physics2D::ToBroadphaseKind(Arcane::PhysicsBroadphase2D::Sap) == Manifold2D::Physics::BroadphaseKind::Sap);
    const auto e = CVarRegistry::Get().Explain("physics.broadphase");
    REQUIRE(e);
    CHECK(e->type == CVarType::Enum);
    CHECK(e->published.AsEnum() == 0);                           // Tree
    CHECK(HasFlag(CVarRegistry::Get().Explain("physics.substepCount")->flags, CVarFlags::Deterministic));
    CHECK(CVarRegistry::Get().Explain("physics.contactHertz")->apply == ApplyMode::NextWorld);
}

TEST_CASE("physics.parallelSolver: off by default (the serial solver, as before); on hands the Runtime's job pool to the next world", "[settings]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("physics.parallelSolver");
#if defined(ARC_BUILD_DIST)
    if (h.IsStale()) return;   // Dev: compiled out in Dist, so the default (serial) stands
#endif
    REQUIRE_FALSE(h.IsStale());
    ClearCodeOnExit restore{ h };
    {
        Runtime rt(Test::Process());
        rt.EnsurePhysics();
        const Arcane::PhysicsWorld2D* res = rt.Registry().GetResource<Arcane::PhysicsWorld2D>();
        REQUIRE(res);
        REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res));
        CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->Executor() != rt.WorkScheduler());
    }
    REQUIRE(reg.Set(h, CVarValue::Bool(true), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    {
        Runtime rt(Test::Process());
        rt.EnsurePhysics();
        const Arcane::PhysicsWorld2D* res = rt.Registry().GetResource<Arcane::PhysicsWorld2D>();
        REQUIRE(res);
        REQUIRE(Arcane::Detail::Physics2D::Access::Solver(*res));
        CHECK(Arcane::Detail::Physics2D::Access::Solver(*res)->Executor() == rt.WorkScheduler());
    }
}

TEST_CASE("SimSettings: today's 60 Hz step and 0.25 s frame clamp; sim.fixedHz is read when a Runtime is built", "[settings]")
{
    CHECK(SimSettings{}.fixedHz == 60.0);
    CHECK(SimSettings{}.maxFrameDeltaSeconds == 0.25);
    CHECK(RunLoop::Config{}.fixedHz == SimSettings{}.fixedHz);
    CVarRegistry& reg = CVarRegistry::Get();
    const auto hz = reg.Explain("sim.fixedHz");
    REQUIRE(hz);
    CHECK(hz->type == CVarType::Float64);
    CHECK(HasFlag(hz->flags, CVarFlags::Deterministic));
    CHECK(hz->apply == ApplyMode::NextWorld);
    const auto clamp = reg.Explain("sim.maxFrameDeltaSeconds");
    REQUIRE(clamp);
    CHECK(clamp->apply == ApplyMode::Live);
    CHECK(HasFlag(clamp->flags, CVarFlags::Deterministic));

    Runtime before(Test::Process());
    const CVarHandle h = reg.Find("sim.fixedHz");
    ClearCodeOnExit restore{ h };
    REQUIRE(reg.Set(h, CVarValue::Float64(30.0), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(Settings<SimSettings>().fixedHz == 30.0);
    Runtime after(Test::Process());
    CHECK(after.Loop().FixedHz() == 30.0);
    CHECK(before.Loop().FixedHz() == 60.0);                     // NextWorld: a live Runtime keeps its step
}

TEST_CASE("ResolveWorkerThreads: 0 is enkiTS' hardware default and N is N; a Runtime reads a published Code-rung jobs.workerThreads", "[settings]")
{
    CHECK(JobsSettings{}.workerThreads == 0u);
    {
        JobSystem byDefault(0);
        JobSystem resolved(ResolveWorkerThreads(JobsSettings{}));
        CHECK(resolved.WorkerCount() == byDefault.WorkerCount());   // identical pool
    }
    CHECK(ResolveWorkerThreads(JobsSettings{ .workerThreads = 3 }) == 3u);
    CHECK(ResolveWorkerThreads(JobsSettings{ .workerThreads = 1 }) == 1u);

    CVarRegistry& reg = CVarRegistry::Get();
    const auto e = reg.Explain("jobs.workerThreads");
    REQUIRE(e);
    CHECK(e->type == CVarType::UInt32);
    CHECK(e->apply == ApplyMode::Restart);
    CHECK(e->scope == SettingScope::PreferencesProject);
    CHECK(HasFlag(e->flags, CVarFlags::Archive));

    const CVarHandle h = reg.Find("jobs.workerThreads");
    ClearCodeOnExit restore{ h };
    REQUIRE(reg.Set(h, CVarValue::UInt32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    JobSystem one(1);
    Runtime serial(Test::Process());
    CHECK(serial.Jobs().WorkerCount() == one.WorkerCount());
}

TEST_CASE("AcquireJobSystem: once every Runtime is gone the pool is rebuilt with the CURRENT jobs.workerThreads", "[settings][jobs]")
{
    auto& reg = Arcane::CVarRegistry::Get();
    const CVarHandle h = reg.Find("jobs.workerThreads");
    ClearCodeOnExit restore{ h };
    std::uint32_t firstCount = 0;
    REQUIRE(reg.Set(h, CVarValue::UInt32(2), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    {
        Runtime first(Test::Process());
        firstCount = first.Jobs().WorkerCount();
        // A live sibling keeps the pool: a changed setting does not resize it.
        REQUIRE(reg.Set(h, CVarValue::UInt32(1), SetBy::Code) == SetResult::Applied);
        reg.Publish();
        Runtime sibling(Test::Process());
        CHECK(&sibling.Jobs() == &first.Jobs());
    }   // both gone: the weak expired
    JobSystem two(2);
    JobSystem one(1);
    CHECK(firstCount == two.WorkerCount());
    Runtime later(Test::Process());
    CHECK(later.Jobs().WorkerCount() == one.WorkerCount());
    CHECK(later.Jobs().WorkerCount() != firstCount);
}

TEST_CASE("ApplyEarlyConfigRungs: jobs.workerThreads in <project>/Config/jobs.json sizes the first Runtime",
          "[project][settings]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_s2_10_jobs_json";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Config", ec);
    std::ofstream(dir / "P.arcproj", std::ios::binary) <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":5},"gameModule":"","plugins":[],"bootScene":""})";
    std::ofstream(dir / "Config" / "jobs.json", std::ios::binary) << R"({ "workerThreads": 1 })";

    struct RestoreProjectRungs
    {
        ~RestoreProjectRungs()
        {
            CVarRegistry& r = CVarRegistry::Get();
            r.RevertLayer(SetBy::Project);
            r.RevertLayer(SetBy::User);
            r.RevertLayer(SetBy::EditorUser);
            r.RevertLayer(SetBy::CommandLine);
            r.Publish();
        }
    } restore;

    HostConfig cfg;
    cfg.projectPath = dir.string();
    HostBoot::ApplyEarlyConfigRungs(cfg, CommandLineCVarContext(), /*editor*/ false);

    JobSystem one(1);
    Runtime serial(Test::Process());
    CHECK(serial.Jobs().WorkerCount() == one.WorkerCount());
    fs::remove_all(dir, ec);
}

namespace
{
    Mosaic::LogLevel g_probeLevel = Mosaic::LogLevel::Info;
    void ProbeMosaicLevel(Mosaic::LogLevel level) noexcept { g_probeLevel = level; }
}

TEST_CASE("ApplyLogSettings sets spdlog and Mosaic's level in Core, this exe and every registered module", "[settings]")
{
    Log::Init();
    Log::InstallMosaicSink();                                    // registers THIS exe's Mosaic level setter
    Log::RegisterMosaicLevelTarget(&ProbeMosaicLevel);
    CHECK(g_probeLevel == Log::CoreMosaicLevel());               // a new target receives the current level at once

    ApplyLogSettings(LogSettings{ .level = 3 });
    CHECK(Log::Engine()->level() == spdlog::level::warn);
    CHECK(Mosaic::GetLogLevel() == Mosaic::LogLevel::Warn);       // ArcaneTests.exe's copy
    CHECK(Log::CoreMosaicLevel() == Mosaic::LogLevel::Warn);      // ArcaneCore.dll's copy
    CHECK(g_probeLevel == Mosaic::LogLevel::Warn);

    Log::UnregisterMosaicLevelTarget(&ProbeMosaicLevel);
    ApplyLogSettings(LogSettings{ .level = 1 });
    CHECK(g_probeLevel == Mosaic::LogLevel::Warn);               // unregistered: untouched
    CHECK(Mosaic::GetLogLevel() == Mosaic::LogLevel::Debug);

    ApplyLogSettings(LogSettings{});                             // back to info
    CHECK(Log::Engine()->level() == spdlog::level::info);
    CHECK(Mosaic::GetLogLevel() == Mosaic::LogLevel::Info);
}

TEST_CASE("UnregisterModuleRange drops Mosaic level setters whose address lies in the image", "[settings]")
{
    Log::Init();
    Log::RegisterMosaicLevelTarget(&ProbeMosaicLevel);
    g_probeLevel = Mosaic::LogLevel::Info;
    const auto* p = reinterpret_cast<const unsigned char*>(&ProbeMosaicLevel);
    (void)CVarRegistry::Get().UnregisterModuleRange(p, 1);
    ApplyLogSettings(LogSettings{ .level = 4 });
    CHECK(g_probeLevel == Mosaic::LogLevel::Info);               // range-dropped: untouched
    CHECK(Log::Engine()->level() == spdlog::level::err);
    ApplyLogSettings(LogSettings{});                             // back to info
}

TEST_CASE("log.level is LogSettings' field: same flags and scope as before, and its publish reaches Mosaic", "[settings]")
{
    Log::Init();
    Log::InstallMosaicSink();
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("log.level");
#if defined(ARC_BUILD_DIST)
    if (h.IsStale()) return;   // Dev: compiled out in Dist
#endif
    REQUIRE_FALSE(h.IsStale());
    const auto e = reg.Explain("log.level");
    REQUIRE(e);
    CHECK(e->flags == (CVarFlags::Archive | CVarFlags::Dev));   // byte-identical to the registration it replaces
    CHECK(e->scope == SettingScope::PreferencesProject);
    CHECK(e->help.find("0 trace") != std::string::npos);
    CHECK(LogSettings{}.level == static_cast<std::int32_t>(spdlog::level::info));
    {
        ClearCodeOnExit restore{ h };
        REQUIRE(reg.Set(h, CVarValue::Int32(4), SetBy::Code) == SetResult::Applied);
        reg.Publish();
        CHECK(Log::Engine()->level() == spdlog::level::err);
        CHECK(Mosaic::GetLogLevel() == Mosaic::LogLevel::Error);
        CHECK(Log::CoreMosaicLevel() == Mosaic::LogLevel::Error);
    }
    ApplyLogSettings(Settings<LogSettings>());                   // whatever the rungs hold now
}

TEST_CASE("render.meshCull and diagnostics.drawMarkers keep their names, types, defaults, flags and help as settings-struct fields", "[settings]")
{
    CHECK(RenderSettings{}.meshCull == true);
    CHECK(DiagnosticsSettings{}.drawMarkers == false);
    CVarRegistry& reg = CVarRegistry::Get();
#if defined(ARC_BUILD_DIST)
    CHECK(reg.Find("render.meshCull").IsStale());                // Dev: compiled out, so the defaults stand
    CHECK(MeshCullFrustumEnabled());
    CHECK_FALSE(GpuDrawMarkersEnabled());
#else
    const auto cull = reg.Explain("render.meshCull");
    REQUIRE(cull);
    CHECK(cull->type == CVarType::Bool);
    CHECK(cull->flags == CVarFlags::Dev);
    CHECK(cull->scope == SettingScope::Project);
    CHECK(cull->help == "Frustum-cull mesh instances on the GPU.");
    const auto markers = reg.Explain("diagnostics.drawMarkers");
    REQUIRE(markers);
    CHECK(markers->type == CVarType::Bool);
    CHECK(markers->flags == CVarFlags::Dev);
    CHECK(markers->help == "Per-draw GPU markers for PIX/RenderDoc. Pass-level scopes stay on.");

    const CVarHandle h = reg.Find("diagnostics.drawMarkers");
    ClearCodeOnExit restore{ h };
    CHECK_FALSE(GpuDrawMarkersEnabled());
    REQUIRE(reg.Set(h, CVarValue::Bool(true), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(GpuDrawMarkersEnabled());
    CHECK(Settings<DiagnosticsSettings>().drawMarkers);
#endif
}


// S6-45: astra.snapshot.compression -- the registry snapshot's Save config. An
// Editor setting, so ArcaneEditor declares it (spec s3.2); Core owns only the
// Save path's process-wide SaveConfig, which the editor's publish callback sets.
namespace
{
    struct SnapshotFill { std::int32_t a = 7, b = 7, c = 7, d = 7; };
    ASTRA_REFLECT_TYPE(SnapshotFill)
        ASTRA_REFLECT_FIELD(SnapshotFill, a) ASTRA_REFLECT_FIELD(SnapshotFill, b)
        ASTRA_REFLECT_FIELD(SnapshotFill, c) ASTRA_REFLECT_FIELD(SnapshotFill, d)
    ASTRA_END_REFLECT_TYPE()
}

TEST_CASE("ToAstraSaveConfig: the default is Astra's SaveConfig; None turns compression off", "[settings]")
{
    using Editor::AstraSnapshotCompression;
    using Editor::AstraSnapshotSettings;
    const Astra::Registry::SaveConfig lib{};
    const Astra::Registry::SaveConfig ours = Editor::ToAstraSaveConfig(AstraSnapshotSettings{});
    CHECK(ours.compressionMode == lib.compressionMode);
    CHECK(ours.compressionLevel == lib.compressionLevel);
    CHECK(ours.compressionThreshold == lib.compressionThreshold);

    AstraSnapshotSettings off;
    off.compression = AstraSnapshotCompression::None;
    CHECK(Editor::ToAstraSaveConfig(off).compressionMode == Astra::CompressionMode::None);

    if (!Test::InThisBuild("astra.snapshot.compression")) return;   // Dev: compiled out of Dist
    const std::optional<CVarDescInfo> d = CVarRegistry::Get().Describe("astra.snapshot.compression");
    REQUIRE(d.has_value());
    CHECK(d->type == CVarType::Enum);
    CHECK(d->enumNames == std::vector<std::string>{ "None", "LZ4" });
    CHECK(d->defaultValue == CVarValue::Enum(1));   // LZ4, by ordinal (I7)
    CHECK(d->audience == Audience::Editor);
    CHECK(d->scope == SettingScope::PreferencesProject);
    CHECK(HasFlag(d->flags, CVarFlags::Dev));
    CHECK(d->module != "ArcaneCore");   // declared by an editor TU (here compiled into the test exe), never Core (spec s3.2)
}

TEST_CASE("astra.snapshot.compression reaches Runtime::SnapshotRegistry through the editor's publish callback", "[settings]")
{
    Arcane::Test::SkipIfCompiledOut("astra.snapshot.compression");
    // Core's own default, before and after: Astra's SaveConfig{}.
    REQUIRE(Runtime::SnapshotSaveConfig().compressionMode == Astra::Registry::SaveConfig{}.compressionMode);

    Runtime rt(Test::Process());
    rt.Components()->RegisterComponent<SnapshotFill>();
    for (int i = 0; i < 4096; ++i)
        rt.Registry().CreateEntityWith(SnapshotFill{});

    auto lz4 = rt.SnapshotRegistry();
    REQUIRE(lz4.IsOk());
    std::size_t noneSize = 0;
    {
        const Test::ScopedCodeRung none("astra.snapshot.compression", CVarValue::Enum(0));
        CHECK(Runtime::SnapshotSaveConfig().compressionMode == Astra::CompressionMode::None);
        auto raw = rt.SnapshotRegistry();
        REQUIRE(raw.IsOk());
        noneSize = raw.GetValue()->size();
        CHECK(rt.RestoreRegistry(*raw.GetValue()));   // an uncompressed snapshot still restores
    }
    // The rung cleared: the callback pushed LZ4 back.
    CHECK(Runtime::SnapshotSaveConfig().compressionMode == Astra::CompressionMode::LZ4);
    INFO("LZ4 " << lz4.GetValue()->size() << " bytes, None " << noneSize << " bytes");
    CHECK(noneSize > lz4.GetValue()->size());
}
