// 3D-readiness Plan A, Task 8: the sim hook seam. Engine stops naming 2D physics;
// the 2D module registers one SimHook at image load and Runtime calls it in
// SimHook::order. These cases pin the component ids, the frame-event window,
// the include scan, gravity ownership, ResetWorld symmetry and SetFixedHz.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"
#include "Helpers/ModuleNames.hpp"
#include "Helpers/PhysicsV1Baseline.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Platform/Platform.hpp>   // ExecutableFileName
#include <Arcane/Base/Engine.hpp>         // ExecutablePathUtf8
#include <Arcane/Core/ModuleAttach.hpp>
#include <Arcane/Plugin/Module.hpp>       // MappedModules: the ArcaneTests image span
#include <Arcane/Scene/PhysicsQuery.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Sim/SimHook.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <regex>
#include <sstream>
#include <tuple>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>   // WIFEXITED / WEXITSTATUS: pclose's wait status
#endif

namespace
{
    using namespace Arcane::Test;

    // Runs this exe on one case with the 2D hook disabled; returns the exit code
    // and the child's Catch2 report. The exe is addressed by its full path (a
    // bare name is not found when cmd.exe skips the current directory, and a
    // "not recognized" exit 1 would pass for a red child).
#if !defined(_WIN32)
    // POSIX pclose returns the wait status, not the exit code (Catch2's 42
    // arrives as 42 << 8); _pclose on Windows already returns the exit code.
    // A signalled child maps to 128 + signal like HostWitness.cpp's ExitCodeOf (its
    // copy is private to that file's anonymous namespace); -1 stays -1 (pclose failed).
    int ExitCodeOf(int status)
    {
        if (status == -1) return -1;
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return -1;
    }
#endif

    struct NoHookChildRun { int exitCode = -1; std::string output; };
    NoHookChildRun SpawnNoHookChild()
    {
        NoHookChildRun run;
        const std::string exe = Arcane::ExecutablePathUtf8();
#if defined(_WIN32)
        const std::string cmd = "set ARCANE_TEST_NO_PHYSICS_HOOK=1&& \"" + exe +
            "\" \"physics component ids are the three integers after WorldBounds\" --rng-seed 1 2>&1";
        FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
        const std::string cmd = "ARCANE_TEST_NO_PHYSICS_HOOK=1 '" + exe +
            "' \"physics component ids are the three integers after WorldBounds\" --rng-seed 1 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
#endif
        if (!p) return run;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), p)) run.output += buf;
#if defined(_WIN32)
        run.exitCode = _pclose(p);
#else
        run.exitCode = ExitCodeOf(pclose(p));
#endif
        return run;
    }

    bool AnyRows(const Arcane::PhysicsEvents2D& e)
    {
        return !e.contactBegin.empty() || !e.contactEnd.empty() || !e.contactHit.empty()
            || !e.sensorBegin.empty() || !e.sensorEnd.empty();
    }

    // Restores the registered hooks and the seal on scope exit (a REQUIRE may throw).
    struct HookRestore
    {
        std::vector<Arcane::SimHook> saved;
        bool sealed = false;
        HookRestore()
        {
            const auto h = Arcane::SimHooks();
            saved.assign(h.begin(), h.end());
            sealed = Arcane::SimHooksSealed();
        }
        ~HookRestore()
        {
            Arcane::ReplaceSimHooksForTest(saved);
            (void)Arcane::SetSimHooksSealedForTest(sealed);
        }
    };

    void NoopPrewarm() noexcept {}

    // The child (AbiDigestChild --sim-hook) links ArcaneEngine only, with
    // ArcanePhysics2D staged beside it. Returns its exit code and key=value lines.
    struct SimHookChildRun
    {
        int exitCode = -1;
        std::string output;
        std::map<std::string, std::string> fields;
    };

    SimHookChildRun RunSimHookChild(const std::string& module, const std::string& secondModule)
    {
        SimHookChildRun run;
        const std::filesystem::path exeDir = std::filesystem::path(Arcane::ExecutablePathUtf8()).parent_path();
#if defined(_WIN32)
        const std::filesystem::path child = exeDir.parent_path() / "AbiDigestChild" / "AbiDigestChild.exe";
        const std::string line = "\"\"" + child.string() + "\" --sim-hook \"" + module + "\" \"" + secondModule +
                                 "\" 2>NUL\"";
        FILE* pipe = _popen(line.c_str(), "r");
#else
        const std::filesystem::path child = exeDir.parent_path() / "AbiDigestChild" / "AbiDigestChild";
        const std::string line = "'" + child.string() + "' --sim-hook '" + module + "' '" + secondModule + "' 2>/dev/null";
        FILE* pipe = popen(line.c_str(), "r");
#endif
        if (!pipe) return run;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), pipe)) run.output += buf;
#if defined(_WIN32)
        run.exitCode = _pclose(pipe);
#else
        run.exitCode = ExitCodeOf(pclose(pipe));
#endif
        std::istringstream lines(run.output);
        for (std::string l; std::getline(lines, l);)
        {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (const auto eq = l.find('='); eq != std::string::npos) run.fields[l.substr(0, eq)] = l.substr(eq + 1);
        }
        return run;
    }

    std::string Slurp(const std::filesystem::path& p) { return ReadFileBinary(p); }

    // Cases below share this scan's rules with ArcaneSpellingGuardTest's PublicDeclarations
    // mode (that Scan is in an anonymous namespace, so the rules are repeated here):
    // comments, string literals, preprocessor lines (+ continuations) and ARC_INTERNAL
    // fences are exempt. Returns the 1-based lines whose code matches `pattern`.
    std::vector<int> ScanPublicDeclarations(const std::string& text, const char* pattern)
    {
        const std::regex library(pattern);
        std::vector<int> lines;
        std::istringstream in(text);
        std::string raw;
        int lineNo = 0;
        bool inBlock = false, inFence = false, inDefine = false;
        while (std::getline(in, raw))
        {
            ++lineNo;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            if (raw.find("ARC_INTERNAL_BEGIN") != std::string::npos) { inFence = true;  continue; }
            if (raw.find("ARC_INTERNAL_END")   != std::string::npos) { inFence = false; continue; }
            if (inFence) continue;
            if (inDefine) { inDefine = !raw.empty() && raw.back() == '\\'; continue; }

            std::string code;
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                if (inBlock)
                {
                    if (raw.compare(i, 2, "*/") == 0) { inBlock = false; ++i; }
                    continue;
                }
                if (raw.compare(i, 2, "/*") == 0) { inBlock = true; ++i; continue; }
                if (raw.compare(i, 2, "//") == 0) break;
                if (raw[i] == '"')
                {
                    const bool rawStr = i > 0 && raw[i - 1] == 'R';
                    const std::string close = rawStr ? ")\"" : "\"";
                    std::size_t j = i + 1;
                    while (j < raw.size())
                    {
                        if (!rawStr && raw[j] == '\\') { j += 2; continue; }
                        if (raw.compare(j, close.size(), close) == 0) break;
                        ++j;
                    }
                    const std::size_t end = std::min(j + close.size(), raw.size());
                    code += "\"\"";
                    i = end - 1;
                    continue;
                }
                code += raw[i];
            }
            const auto first = code.find_first_not_of(" \t");
            if (first != std::string::npos && code[first] == '#')
            {
                inDefine = !raw.empty() && raw.back() == '\\';
                continue;
            }
            if (std::regex_search(code, library)) lines.push_back(lineNo);
        }
        return lines;
    }

    constexpr const char* kManifoldPattern = R"(\bManifold2D\b)";

    int g_calls = 0;
}

TEST_CASE("physics component ids are the three integers after WorldBounds", "[physics][hook]")
{
    // SimHooks() is undeclared before the hook exists. That compile error is the red.
    if (const char* e = std::getenv("ARCANE_TEST_NO_PHYSICS_HOOK"); e && e[0] == '1')
    {
        CHECK(Arcane::SimHooks().empty());
        Arcane::Runtime rt(Arcane::Test::Process());
        const auto* creg = rt.Registry().GetComponentRegistry();
        const auto wb = Astra::TypeID<Arcane::WorldBounds>::Value();
        for (const auto hash : { Astra::TypeID<Arcane::RigidBody2D>::Hash(),
                                 Astra::TypeID<Arcane::Collider2D>::Hash(),
                                 Astra::TypeID<Arcane::PhysicsBodyRef2D>::Hash() })
        {
            const auto id = creg->GetComponentIDFromHash(hash);
            REQUIRE(id.IsOk()); // the child must fail here
        }
        CHECK(Astra::TypeID<Arcane::RigidBody2D>::Value() == wb + 1);
        CHECK(Astra::TypeID<Arcane::Collider2D>::Value() == wb + 2);
        CHECK(Astra::TypeID<Arcane::PhysicsBodyRef2D>::Value() == wb + 3);
        return;
    }

    REQUIRE_FALSE(Arcane::SimHooks().empty());
    Arcane::Runtime rt(Arcane::Test::Process());
    const auto* creg = rt.Registry().GetComponentRegistry();
    const auto wb = Astra::TypeID<Arcane::WorldBounds>::Value();
    const auto rb = creg->GetComponentIDFromHash(Astra::TypeID<Arcane::RigidBody2D>::Hash());
    const auto col = creg->GetComponentIDFromHash(Astra::TypeID<Arcane::Collider2D>::Hash());
    const auto pref = creg->GetComponentIDFromHash(Astra::TypeID<Arcane::PhysicsBodyRef2D>::Hash());
    REQUIRE(rb.IsOk());
    REQUIRE(col.IsOk());
    REQUIRE(pref.IsOk());
    CHECK(*rb.GetValue() == wb + 1);
    CHECK(*col.GetValue() == wb + 2);
    CHECK(*pref.GetValue() == wb + 3);

    // The child is the only Runtime in its process. Random order in THIS process
    // would already have minted the ids, so the absence check cannot live here.
    // Red for the right reason: Catch2 v3's failed-test exit code (42; a crash is
    // another code), the failure is the id lookup, and the child booted with no hook.
    const NoHookChildRun child = SpawnNoHookChild();
    INFO("child output: " << child.output);
    CHECK(child.exitCode == 42);
    CHECK(child.output.find("REQUIRE( id.IsOk() )") != std::string::npos);
    CHECK(child.output.find("SimHooks().empty()") == std::string::npos);
}

TEST_CASE("BeginFrame clears frameEvents and leaves stepEvents until the next step", "[physics][hook]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(rt.Registry());
    rt.EnsurePhysics();
    auto* world = rt.Registry().GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(world != nullptr);
    // Advance until a step carries rows (the v1 scene makes contacts inside 120 steps).
    bool stepHadRows = false;
    for (int i = 0; i < 120 && !stepHadRows; ++i)
    {
        rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
        stepHadRows = AnyRows(world->StepEvents());
    }
    // A zero-step Advance runs only the frame-begin wiring (the hook's FrameBegin).
    rt.Loop().Advance(0.0, [](double) {}, [](double, double) {});
    const auto frame = world->FrameEvents();
    CHECK(frame.contactBegin.empty());
    CHECK(frame.contactEnd.empty());
    CHECK(frame.contactHit.empty());
    CHECK(frame.sensorBegin.empty());
    CHECK(frame.sensorEnd.empty());
    REQUIRE(stepHadRows);
    CHECK(AnyRows(world->StepEvents())); // BeginFrame must not clear stepEvents
}

TEST_CASE("Engine and Render and Client and Editor and Server include no physics solver header", "[physics][hook]")
{
    const auto root = RepoRoot();
    const std::vector<std::string> dirs = {
        "Source/Runtime/ArcaneEngine", "Source/Client/ArcaneRender", "Source/Client/ArcaneClient",
        "Source/Programs/ArcaneEditor", "Source/Programs/ArcaneServer" };
    const std::vector<std::string> banned = {
        "Physics2DDetail.hpp", "Physics2DHandles.hpp", "PhysicsSystem.hpp",
        "PhysicsDebugGeometry.hpp", "Manifold2D/" };
    // No tree reaches the Physics2D module's public headers, directly or through
    // a chain (Physics2DFwd.hpp, the solver-free forward header, is allowed).
    const std::vector<std::string> bannedTransitive = {
        "Arcane/Physics2D.hpp", "Physics2DWorld.hpp", "PhysicsEvents2D.hpp" };
    // Only #include lines count, so a comment naming a header (PluginABI.hpp's
    // changelog) needs no exemption.
    const std::regex includeLine(R"(^\s*#\s*include\b)");
    // Transitive (final fix I1): every repo-owned header an #include reaches is
    // scanned too, so ServerApp.cpp -> Physics2DWorld.hpp -> Physics2DHandles.hpp
    // -> Manifold2D/ cannot hide behind one hop. Headers are indexed by their path
    // under a src/ folder, the form <Arcane/...> includes use.
    std::map<std::string, std::filesystem::path> headerIndex;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "Source"))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".hpp") continue;
        const std::string full = entry.path().generic_string();
        const auto at = full.find("/src/");
        if (at != std::string::npos) headerIndex.emplace(full.substr(at + 5), entry.path());
    }
    const std::regex includeTarget(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])");
    int scanned = 0;
    for (const auto& dir : dirs)
    {
        REQUIRE(std::filesystem::exists(root / dir));
        std::set<std::filesystem::path> seen;
        std::vector<std::filesystem::path> work;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / dir))
        {
            if (!entry.is_regular_file()) continue;
            const auto ext = entry.path().extension();
            if (ext != ".hpp" && ext != ".cpp") continue;
            if (seen.insert(entry.path()).second) work.push_back(entry.path());
        }
        while (!work.empty())
        {
            const auto file = work.back();
            work.pop_back();
            ++scanned;
            std::istringstream text(Slurp(file));
            for (std::string line; std::getline(text, line);)
            {
                if (!std::regex_search(line, includeLine)) continue;
                for (const auto& ban : banned)
                {
                    INFO(dir << ": " << file.string() << " includes " << ban);
                    CHECK(line.find(ban) == std::string::npos);
                }
                for (const auto& ban : bannedTransitive)
                {
                    INFO(dir << ": " << file.string() << " includes " << ban);
                    CHECK(line.find(ban) == std::string::npos);
                }
                std::smatch m;
                if (!std::regex_search(line, m, includeTarget)) continue;
                const std::string target = m[1].str();
                std::filesystem::path next;
                const auto it = headerIndex.find(target);
                if (it != headerIndex.end()) next = it->second;
                else if (std::filesystem::exists(file.parent_path() / target)) next = file.parent_path() / target;
                else continue; // third party or system header
                // The game-side ABI digest headers reach the Physics2D group only
                // through a __has_include a game build satisfies and the Engine,
                // Render, Client, Editor and Server builds do not (no include dir).
                const std::string leaf = next.filename().string();
                if (leaf == "AbiManifestGame.hpp" || leaf == "AbiManifestPhysics2D.hpp") continue;
                if (seen.insert(next).second) work.push_back(next);
            }
        }
    }
    CHECK(scanned > 50);

    // EngineResourceTypes.cpp is in the walk above; ProjectHost.hpp names a Scene header only.
    CHECK(Slurp(root / "Source/Runtime/ArcaneEngine/src/Arcane/Project/ProjectHost.hpp")
              .find("PhysicsComponents.hpp") != std::string::npos);

    // std::byte needs <cstddef> (final review M7; the header is game-facing).
    CHECK(Slurp(root / "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsComponents.hpp").find("#include <cstddef>") != std::string::npos);

    // Runtime.cpp names no 2D physics type at all (the hook owns them).
    const std::string runtimeCpp = Slurp(root / "Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.cpp");
    REQUIRE_FALSE(runtimeCpp.empty());
    for (const char* bad : { "Manifold2D", "PhysicsSystem2D", "PhysicsWorld2D", "PhysicsInterpBuffer2D", "Physics2DDetail" })
    {
        INFO("Runtime.cpp names " << bad);
        CHECK(runtimeCpp.find(bad) == std::string::npos);
    }

    // Engine's module descriptor and the Scene export header name no Physics2D module.
    CHECK(Slurp(root / "Source/Runtime/ArcaneEngine/ArcaneEngine.arcmodule").find("ArcanePhysics2D") == std::string::npos);
    CHECK(Slurp(root / "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsSystem2D.hpp")
              .find("ArcanePhysics2DApi.hpp") == std::string::npos);

    // Scene's PhysicsSystem2DApi.hpp re-spells ArcanePhysics2DApi.hpp's export
    // switch; both must test the same three build defines.
    const std::string sceneApi = Slurp(root / "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsSystem2DApi.hpp");
    const std::string moduleApi = Slurp(root / "Source/Runtime/ArcanePhysics2D/src/Arcane/Modules/ArcanePhysics2DApi.hpp");
    for (const char* define : { "ARC_MONOLITHIC", "ARC_PHYSICS2D_STATIC", "ARC_PHYSICS2D_EXPORTS" })
    {
        INFO(define);
        CHECK(sceneApi.find(define) != std::string::npos);
        CHECK(moduleApi.find(define) != std::string::npos);
    }
}

TEST_CASE("game-facing headers name no Manifold2D in a public declaration", "[physics][hook]")
{
    // Self-check of the scan rules first.
    CHECK(ScanPublicDeclarations("Manifold2D::Vec2 x;\n", kManifoldPattern).size() == 1);
    CHECK(ScanPublicDeclarations("// Manifold2D\n", kManifoldPattern).empty());
    CHECK(ScanPublicDeclarations("#include <Manifold2D/Foo.hpp>\n", kManifoldPattern).empty());
    CHECK(ScanPublicDeclarations("ARC_INTERNAL_BEGIN\nManifold2D::Vec2 x;\nARC_INTERNAL_END\n", kManifoldPattern).empty());

    const auto root = RepoRoot();
    const char* headers[] = {
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/GameModule.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/GameComponents.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/GameSystems.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/SystemFactory.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Plugin/PluginABI.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Base/ProcessContext.hpp",
        "Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.hpp",
        "Source/Runtime/ArcaneCore/src/Arcane/Base/Assert.hpp",
        "Source/Runtime/ArcaneCore/src/Arcane/Base/Log.hpp",
        "Source/Client/ArcaneClient/src/Arcane/Client/ClientRuntime.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneResources.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCacheRecords.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/Components.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/TransformSystems.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsComponents.hpp",
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsSystem2D.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsEvents2D.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Physics2D.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Physics2DFwd.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DWorld.hpp",
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DHandles.hpp",
    };
    for (const char* rel : headers)
    {
        const auto path = root / rel;
        INFO(path.generic_string());
        REQUIRE(std::filesystem::exists(path));
        CHECK(ScanPublicDeclarations(Slurp(path), kManifoldPattern).empty());
    }
}

namespace Arcane::Test::HookBoot
{
    inline int g_dummyRuns = 0;
    struct HzDummy
    {
        void operator()(Astra::Registry&) { ++g_dummyRuns; }
    };
}

TEST_CASE("SetFixedHz keeps a game system and re-rates the physics step", "[physics][hook]")
{
    using namespace Arcane::Test::HookBoot;
    Arcane::Runtime rt(Arcane::Test::Process());
    auto& reg = rt.Registry();
    std::ignore = rt.Schedulers().fixedUpdate.AddSystem<HzDummy>();
    g_dummyRuns = 0;

    rt.SetFixedHz(30.0);
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem2D>());
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<HzDummy>());

    const Astra::Entity box = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(box, Arcane::Transform{ .position = { 0.0f, 10.0f, 0.0f } });
    Arcane::RigidBody2D body; body.type = Arcane::BodyType2D::Dynamic; body.fixedRotation = true;
    reg.AddComponent<Arcane::RigidBody2D>(box, body);
    Arcane::Fixture2D fx; fx.kind = Arcane::ShapeKind2D::Aabb; fx.halfW = 0.5f; fx.halfH = 0.5f;
    Arcane::Collider2D col; col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(box, col);
    rt.EnsurePhysics();
    const float g = rt.ResolvedGravity().y;
    rt.Loop().Advance(1.0 / 30.0);   // exactly one fixed step
    CHECK(g_dummyRuns == 1);
    Arcane::RigidBody2D& rb = *reg.GetComponent<Arcane::RigidBody2D>(box);
    const Arcane::BodyMotion2D m = reg.GetResource<Arcane::PhysicsWorld2D>()->Motion(box, rb);
    REQUIRE(m.bodyReady);
    CHECK(std::abs(m.velocityY - g / 30.0f) < 1e-4f);
}

TEST_CASE("EnsurePhysics mints the world and the resource is non-null after a step", "[physics][hook]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(rt.Registry());
    rt.EnsurePhysics();
    rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
    CHECK(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() != nullptr);
}

TEST_CASE("EnsureWorld resolves its own gravity and Engine passes none", "[physics][hook]")
{
    HookRestore restore;
    Arcane::Runtime rt(Arcane::Test::Process());
    auto& reg = rt.Registry();
    const V1PhysicsScene scene = BuildV1PhysicsScene(reg);

    const Astra::Entity root = reg.CreateEntity();
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
    Arcane::PhysicsSettings2D ps; ps.gravity = glm::vec2(0.0f, 2.0f);
    reg.AddComponent<Arcane::PhysicsSettings2D>(root, ps);

    std::vector<Arcane::SimHook> hooks(Arcane::SimHooks().begin(), Arcane::SimHooks().end());
    Arcane::SimHook recorder;
    recorder.order = 300;
    recorder.EnsureWorld = [](Astra::Registry&, Mosaic::IWorkScheduler*) noexcept { ++g_calls; };
    hooks.push_back(recorder);
    Arcane::ReplaceSimHooksForTest(hooks);

    g_calls = 0;
    rt.EnsurePhysics();
    rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
    CHECK(g_calls == 1);

    auto* world = reg.GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(world != nullptr);
    Arcane::RigidBody2D& rb = *reg.GetComponent<Arcane::RigidBody2D>(scene.Circle());
    const Arcane::BodyMotion2D m = world->Motion(scene.Circle(), rb);
    REQUIRE(m.bodyReady);
    CHECK(m.velocityY > 0.0f);   // the hook resolved the scene root's {0, 2}, not the default
}

TEST_CASE("ResetWorld removes the world and the interp buffer it minted", "[physics][hook]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(rt.Registry());
    rt.EnsurePhysics();
    REQUIRE(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() != nullptr);
    REQUIRE(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer2D>() != nullptr);
    rt.ResetPhysics();
    CHECK(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() == nullptr);
    CHECK(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer2D>() == nullptr);
    rt.EnsurePhysics();
    CHECK(rt.Registry().GetResource<Arcane::PhysicsWorld2D>() != nullptr);
    CHECK(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer2D>() != nullptr);
}

TEST_CASE("SimHooks run in SimHook::order", "[physics][hook]")
{
    HookRestore restore;
    Arcane::SimHook a; a.order = 300;
    Arcane::SimHook b; b.order = 100;
    Arcane::SimHook c; c.order = 100; c.Prewarm = []() noexcept {};   // equal order keeps registration order
    const Arcane::SimHook two[] = { a, b, c };
    Arcane::ReplaceSimHooksForTest(two);
    const auto hooks = Arcane::SimHooks();
    REQUIRE(hooks.size() == 3);
    CHECK(hooks[0].order == 100);
    CHECK(hooks[0].Prewarm == nullptr);
    CHECK(hooks[1].order == 100);
    CHECK(hooks[1].Prewarm != nullptr);
    CHECK(hooks[2].order == 300);

    // RegisterSimHook inserts in ascending order too (before the seal).
    (void)Arcane::SetSimHooksSealedForTest(false);
    Arcane::SimHook mid; mid.order = 200;
    CHECK(Arcane::RegisterSimHook(mid));
    const auto after = Arcane::SimHooks();
    REQUIRE(after.size() == 4);
    CHECK(after[2].order == 200);
    CHECK(after[3].order == 300);
}

TEST_CASE("ProcessContext::Create seals the sim hooks and a later registration is refused", "[physics][hook]")
{
    HookRestore restore;
    (void)Arcane::Test::Process();   // this exe's one ProcessContext exists from here on
    CHECK(Arcane::SimHooksSealed());
    const std::size_t before = Arcane::SimHooks().size();
    Arcane::SimHook late; late.order = 150; late.Prewarm = &NoopPrewarm;
    CHECK_FALSE(Arcane::RegisterSimHook(late));
    CHECK(Arcane::SimHooks().size() == before);

    // The 2D query slot shares the seal: a plugin-loaded copy of the module is refused there too.
    constexpr Arcane::PhysicsSilhouetteFillFn2D fakeSilhouettes =
        [](const Astra::Registry&, std::vector<Arcane::PhysicsColliderSilhouette2D>&) {};
    CHECK_FALSE(Arcane::SetPhysicsQueryFns2D(nullptr, fakeSilhouettes));
}

TEST_CASE("UnregisterSimHooksInRange drops a hook whose function lies inside the image", "[physics][hook]")
{
    // Every platform: the ArcaneTests image is found through Module::MappedModules,
    // which lists ELF objects since PR #20 and dyld's non-cache images since
    // issue #21 (it was Windows-only when this case was written).
    HookRestore restore;
    const auto addr = reinterpret_cast<std::uintptr_t>(&NoopPrewarm);
    Arcane::Module::ImageSpan self{};
    for (const auto& m : Arcane::Module::MappedModules())
    {
        const auto lo = reinterpret_cast<std::uintptr_t>(m.image.base);
        if (addr >= lo && addr < lo + m.image.size) { self = m.image; break; }
    }
    REQUIRE(self.base != nullptr);   // the ArcaneTests image

    std::vector<Arcane::SimHook> hooks(restore.saved.begin(), restore.saved.end());
    Arcane::SimHook mine; mine.order = 900; mine.Prewarm = &NoopPrewarm;
    hooks.push_back(mine);
    Arcane::ReplaceSimHooksForTest(hooks);
    REQUIRE(Arcane::SimHooks().size() == restore.saved.size() + 1);

    CHECK(Arcane::UnregisterSimHooksInRange(nullptr, 0) == 0);
    // One image per module: the saved hooks (Physics2D's) live in their own
    // images, so this exe's image holds exactly the one hook added above.
    // (No ARC_MONOLITHIC branch: premake5.lua returns before the test
    // projects in the monolithic layout, so it never compiled.)
    CHECK(Arcane::UnregisterSimHooksInRange(self.base, self.size) == 1);
    CHECK(Arcane::SimHooks().size() == restore.saved.size());   // the Physics2D hook lives in its own image
    for (const Arcane::SimHook& h : Arcane::SimHooks())
        CHECK(h.Prewarm != &NoopPrewarm);

    // The RAII registrar's path: UnregisterSimHook removes exactly the hook it is given.
    Arcane::ReplaceSimHooksForTest(hooks);
    CHECK(Arcane::UnregisterSimHook(mine));
    CHECK_FALSE(Arcane::UnregisterSimHook(mine));
    CHECK(Arcane::SimHooks().size() == restore.saved.size());
}

TEST_CASE("a plugin-loaded SimHook is refused and nothing dangles after unload", "[physics][hook]")
{
    // This one child run also carries the PR #20 re-review witnesses: M-P1
    // (a failed load keeps a pinned dependency's cvars), M-P2 (a second stem
    // layers its own rungs), M-P5 (the game's image unmaps, never pinned) and,
    // since issue #21, which branch the leftover count ran on (M-R3). Every
    // platform runs them; the unmap branch of a DEPENDENCY is reachable on
    // Linux and macOS only when PinMapped fails (pin-for-process-life), which
    // a healthy loader never does, so only Windows witnesses it here.
    // HotReloadPluginV1 imports ArcanePhysics2D (its descriptor names it); the
    // child links ArcaneEngine only, so the module maps Physics2D after the seal.
    // Addressed from the exe, not the cwd (another case may move the cwd).
    const std::string module = (std::filesystem::path(Arcane::ExecutablePathUtf8()).parent_path().parent_path()
                                / "HotReloadPluginV1" / Arcane::Test::ModuleFile("HotReloadPluginV1")).string();
    REQUIRE(std::filesystem::exists(module));
    // The second session loads HotReloadPluginV2: a DIFFERENT stem (issue #21
    // M-P2) that imports ArcanePhysics2D too.
    const std::string secondModule = (std::filesystem::path(Arcane::ExecutablePathUtf8()).parent_path().parent_path()
                                      / "HotReloadPluginV2" / Arcane::Test::ModuleFile("HotReloadPluginV2")).string();
    REQUIRE(std::filesystem::exists(secondModule));
    const SimHookChildRun run = RunSimHookChild(module, secondModule);
    INFO("child output: " << run.output);
    CHECK(run.exitCode == 0);
    REQUIRE(run.fields.count("loaded") == 1);
    CHECK(run.fields.at("loaded") == "1");
    CHECK(run.fields.at("sealed") == "1");
    CHECK(run.fields.at("hooksAfterLoad") == run.fields.at("hooksBefore"));
    CHECK(run.fields.at("hooksAfterUnload") == run.fields.at("hooksBefore"));
    REQUIRE(run.fields.count("physics2dMappedAfterUnload") == 1);
    REQUIRE(run.fields.count("physics2dAttachSecondSession") == 1);
    REQUIRE(run.fields.count("physics2dCVarsSecondSession") == 1);
    REQUIRE(run.fields.count("pluginMappedAfterUnload") == 1);
    REQUIRE(run.fields.count("physics2dCVarAfterFailedLoad") == 1);
    REQUIRE(run.fields.count("physics2dLayeredSecondSession") == 1);
    REQUIRE(run.fields.count("physics2dSecondSessionBranch") == 1);
    // Every platform since issue #21 (Module::MappedModules lists dyld's
    // non-cache images on macOS, so the child's witness sees Physics2D there).
    CHECK(run.fields.at("physics2dMappedBefore") == "0");   // the child does not link it
    CHECK(run.fields.at("physics2dMappedAtLoad") == "1");   // the module really imports it
#if defined(_WIN32)
    // Windows: the host's load session pinned the fresh dependency, disowned it
    // and let go; FreeLibrary dropped the last reference (the fresh Runtime
    // below is not vacuous).
    CHECK(run.fields.at("physics2dMappedAfterUnload") == "0");
#else
    // Linux and macOS: a game load pins its fresh engine dependencies FOR
    // PROCESS LIFE (Module::PinsForProcessLife, user decision 2026-10-24;
    // macOS since issue #21): engine libraries are never hot-reloaded, only
    // the game module is. It stays mapped by rule (RTLD_NODELETE on ELF, a
    // kept dlopen reference on Mach-O, plus Module::Load's beside-exe retry
    // reference), not by what the loader happens to do with a released
    // object, and its registrations are never dropped.
    CHECK(run.fields.at("physics2dMappedAfterUnload") == "1");
#endif
    // The game's OWN image is never pinned, on any platform (PR #20 re-review
    // M-P5): its temp copy is mapped while loaded and gone after Unload. A
    // regression that pinned it would stay green everywhere else, since every
    // load uses a fresh temp path.
    CHECK(run.fields.at("pluginMappedAtLoad") == "1");
    CHECK(run.fields.at("pluginMappedAfterUnload") == "0");
    // Physics2D's settings are layered at the load (--set ...=0.25), whatever
    // name they are registered under.
    CHECK(run.fields.at("physics2dLayeredAtLoad") == "0.25");
    CHECK(run.fields.at("failedLoadFailed") == "1");
#if defined(_WIN32)
    // Windows: a dependency's statics run inside the game's LoadLibrary, under
    // its CVarModuleScope, and leave with the load session (EndLoadSession).
    CHECK(run.fields.at("physics2dCVarModule") == "HotReloadPluginV1");
    // ...and a FAILED load of the game's stem finds Physics2D already unmapped
    // and its registrations dropped, so the cvar is stale (issue #21 Task 2
    // review (d)).
    CHECK(run.fields.at("physics2dCVarAfterFailedLoad") == "0");
    // Physics2D unmapped with the first session, so the second maps it afresh
    // (the label comes from the unmap, not from the base address).
    CHECK(run.fields.at("physics2dSecondSessionBranch") == "remapped");
#else
    // Linux and macOS (issue #21 M-P1/M-P2): the dependency is pinned for the
    // life of the process, and Plugin::Load re-attributes what its statics
    // registered under the game's scope to its own module, the name it has in
    // a host that links it...
    CHECK(run.fields.at("physics2dCVarModule") == "ArcanePhysics2D");
    // ...so a FAILED load of the game's stem (its name drop) leaves it (M-P1:
    // it stays mapped and its statics never run again)...
    CHECK(run.fields.at("physics2dCVarAfterFailedLoad") == "1");
    // The leftover count ran against the pinned first-session mapping.
    CHECK(run.fields.at("physics2dSecondSessionBranch") == "same-base");
#endif
    // ...and a second session that loads a DIFFERENT stem layers it with ITS
    // rungs (--set ...=0.75), not the first game's (M-P2).
    CHECK(run.fields.at("physics2dLayeredSecondSession") == "0.75");
    // A second load session has Physics2D's registrations EXACTLY once: one
    // module-attach callback (re-registered by a fresh map on Windows, never
    // dropped on Linux and macOS -- a dependency disowned but kept mapped
    // shows 0, a double registration 2) and its cvars.
    CHECK(run.fields.at("loadedSecondSession") == "1");
    CHECK(run.fields.at("physics2dAttachSecondSession") == "1");
    CHECK(run.fields.at("physics2dCVarsSecondSession") != "0");
    CHECK(run.fields.count("advanced") == 1);   // the fresh Runtime stepped after the unload
}

namespace
{
    int g_attachProbeCalls = 0;
    void AttachProbe(Astra::TypeContext*) { ++g_attachProbeCalls; }
}

TEST_CASE("UnregisterModuleAttach and the range net drop exactly the callback they are given", "[physics][hook]")
{
    // Task 8 fix round 2: ARC_MODULE_ATTACH's registrar destructor calls
    // UnregisterModuleAttach; Plugin::Load's refusal net and EndLoadSession call
    // UnregisterModuleAttachInRange. RegisterModuleAttach may call the probe at
    // once (a context is installed); the probe only counts.
    Arcane::RegisterModuleAttach("AttachProbe", &AttachProbe);
    CHECK(Arcane::UnregisterModuleAttach(&AttachProbe));
    CHECK_FALSE(Arcane::UnregisterModuleAttach(&AttachProbe));   // exactly one registration left
    CHECK_FALSE(Arcane::UnregisterModuleAttach(nullptr));
#if !defined(ARC_MONOLITHIC)   // one image per module: the probe is alone in this one (every platform since issue #21 M5)
    const auto addr = reinterpret_cast<std::uintptr_t>(&AttachProbe);
    Arcane::Module::ImageSpan self{};
    for (const auto& m : Arcane::Module::MappedModules())
    {
        const auto lo = reinterpret_cast<std::uintptr_t>(m.image.base);
        if (addr >= lo && addr < lo + m.image.size) { self = m.image; break; }
    }
    REQUIRE(self.base != nullptr);   // the ArcaneTests image
    Arcane::RegisterModuleAttach("AttachProbe", &AttachProbe);
    CHECK(Arcane::UnregisterModuleAttachInRange(nullptr, 0) == 0);
    CHECK(Arcane::UnregisterModuleAttachInRange(self.base, self.size) == 1);   // the engine modules' callbacks live in their own images
    CHECK_FALSE(Arcane::UnregisterModuleAttach(&AttachProbe));
    // The 2D query pair lives in ArcanePhysics2D's image, not this one: the range clear leaves it.
    CHECK_FALSE(Arcane::ClearPhysicsQueryFns2DInRange(nullptr, 0));
    CHECK_FALSE(Arcane::ClearPhysicsQueryFns2DInRange(self.base, self.size));
#endif
}
