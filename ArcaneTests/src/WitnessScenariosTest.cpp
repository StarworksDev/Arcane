// Witness scenarios: the host-witness harness's OBSERVED half.
//
// Everything here spawns the REAL staged ArcaneRuntime through
// Helpers/HostWitness.hpp and grades what it reported. Nothing in this file
// links a host or reimplements a host's logic -- the whole point is that a
// green ArcaneTests run proves nothing about either host unless a host was
// actually launched (feedback_green_gate_proves_nothing_about_hosts).
//
// FRESH-COPY HYGIENE (spec section 6): every scenario runs against a COPY of
// the staged tree and mutates only the copy. Nothing here may touch
// bin/<Config>-windows-x86_64-md/ArcaneRuntime/ itself.
//
// -----------------------------------------------------------------------
// WHY `--frames 60` AND NOT A SMALLER BUDGET -- load-bearing, measured.
// -----------------------------------------------------------------------
// --settle FREEZES the render clock once the --frames budget is spent
// (RuntimeFrame::AdvanceSim), and ShaderCompiler DISPATCH is gated on that
// same clock by a 200 ms debounce (RuntimeFrame.cpp's own comment: "Collection
// is NOT clock-gated ... only DISPATCH is gated, by ShaderCompiler::Poll's
// readyAt check"). At the headless fixed step of 1/60 s, a budget below 13
// frames freezes the clock BEFORE the debounce elapses in sim time, so the
// queued material compiles never dispatch, IsIdle() never becomes true, and
// the convergence predicate's second conjunct can never be satisfied -- on ANY
// scene, with ANY reference. Measured on this tree: `--frames 10` never
// converges (census spriteBound 0 / postBound false, 38 attempts, bail
// "timeout-bound") while `--frames 60` converges in 3 attempts with everything
// bound. 60 is also exactly what scripts/golden-gate.ps1 passes, so these
// scenarios and the gate exercise the same shape of run.
//
// -----------------------------------------------------------------------
// WHERE W2 IS. NOT FORGOTTEN -- DERIVED UNREACHABLE.
// -----------------------------------------------------------------------
// The planned W2 ("a capture that never lands is recorded as capture-failed")
// has no content-or-CLI lever on this host, so it is deliberately absent
// rather than written against a fake one. `settleCaptureFailed` is
// `!previousCaptureValid` at the bail (RuntimeFrame.cpp:893), and
// previousCaptureValid is set by any settle attempt whose ReadCapture landed.
// ReadCapture refuses at two branches -- the capture NODE never ran
// (NriGraphContext.cpp:1915), or MapBuffer on the capture buffer returns
// null (NriGraphContext.cpp:1933-1936) -- and the predicate that arms the node,
// RenderGraph's `willBeLastFrame` (RuntimeFrame.cpp:619-627), is the SAME
// expression as CaptureTail's `pastBase` (line 701) that decides whether a
// settle attempt happens at all: `frameCount + 1 >= maxFrames` evaluated
// before the increment, and `frameCount >= maxFrames` after it. So on the
// runtime host EVERY settle attempt is, by construction, preceded by a frame
// that armed the readback. (The editor host states the same identity in one
// variable: EditorAppFrame.cpp:2794-2798 assigns `frame.capture = pastBase`
// and then gates its own settle branch on `pastBase`.) What is left --
// EnsureCaptureBuffer failing on a zero-sized surface or a refused
// HOST_READBACK allocation, MapBuffer returning null, a node failing to
// resolve -- is GPU/driver failure, which no scene and no command line can
// express. Reaching capture-failed would take a host code change whose only
// purpose is to be broken, which this harness refuses on principle.

#include "Helpers/HostWitness.hpp"
#include <Arcane/Platform/Platform.hpp>   // ExecutableFileName: .exe on Windows only

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/Verdict.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace Arcane::Test;

namespace
{
    // The staged runtime lives beside the test exe dir. Tests run FROM the exe
    // dir, so this resolves relative to cwd. HARD requirement, not a skip: an
    // unbuilt host tree on a capable machine is an incomplete build (spec s7).
    std::filesystem::path StagedRuntimeDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneRuntime");
        INFO("staged ArcaneRuntime not found -- build Arcane.slnx first: " << p.string());
        REQUIRE(std::filesystem::exists(p / Arcane::Platform::ExecutableFileName("ArcaneRuntime")));
        return p;
    }

    // The software-adapter reference sets (ReferenceAdapterSet: "vulkan-lavapipe"
    // and friends) under `refs` that hold `file` -- the extra level a scenario
    // that rewrites or removes "every level" of a reference must also touch.
    std::vector<std::string> AdapterSetsHolding(const std::filesystem::path& refs,
                                                const std::string& file)
    {
        std::vector<std::string> sets;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(refs, ec))
        {
            const std::string dir = e.path().filename().string();
            if (e.is_directory() && dir.rfind("vulkan-", 0) == 0 &&
                std::filesystem::exists(e.path() / file))
                sets.push_back(dir);
        }
        return sets;
    }

    // The invocation shape every scenario shares. --report is passed HERE, by
    // the scenario, not by RunWitness -- the helper only watches the path it
    // was told about (HostWitness.hpp's WitnessInvocation::reportPath).
    WitnessInvocation HostInv(const WitnessScratch& scratch, std::vector<std::string> extraArgs)
    {
        WitnessInvocation inv;
        inv.exePath    = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneRuntime");
        inv.workingDir = scratch.Dir();
        inv.reportPath = scratch.Dir() / "witness-report.json";
        inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "vulkan",
                     "--frames", "60", "--report", inv.reportPath.generic_string() };
        inv.args.insert(inv.args.end(), extraArgs.begin(), extraArgs.end());
        inv.hardCapMs = 120000;
        return inv;
    }
}

TEST_CASE("W1: settle spends BOTH bounds when the compare conjunct cannot pass",
          "[witness][gpu]")
{
    WitnessScratch scratch(StagedRuntimeDir(), "w1-bounds-spent");

    // The lever: a wrong reference at EVERY level (Arc A: one level just falls
    // back to the shared one, and the run passes against the level this
    // scenario did not touch). Overwrite both with a PNG that is guaranteed
    // valid and guaranteed wrong -- an existing unrelated reference from the
    // staged tree, so no image is synthesised here.
    const std::filesystem::path refs  = scratch.Dir() / "ReferenceProject" / "Verify" / "References";
    const std::filesystem::path wrong = refs / "editor-ui.png";
    INFO("staged references: " << refs.string());
    REQUIRE(std::filesystem::exists(wrong));
    REQUIRE(std::filesystem::exists(refs / "runtime-scene.png"));
    REQUIRE(std::filesystem::exists(refs / "vulkan" / "runtime-scene.png"));
    std::filesystem::copy_file(wrong, refs / "runtime-scene.png",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(wrong, refs / "vulkan" / "runtime-scene.png",
                               std::filesystem::copy_options::overwrite_existing);
    // ...and every software-adapter set's own image (ReferenceAdapterSet:
    // "vulkan-lavapipe" and friends), which resolves FIRST on such an adapter.
    for (const std::string& set : AdapterSetsHolding(refs, "runtime-scene.png"))
        std::filesystem::copy_file(wrong, refs / set / "runtime-scene.png",
                                   std::filesystem::copy_options::overwrite_existing);

    // 4000, not the plan's 2000: process wall time INCLUDES boot (~2 s for this
    // host on this tree), so a 2000 ms bound is covered by boot alone and the
    // assertion below would read true even on a run that converged immediately
    // -- an assertion that cannot fail is not a test. At 4000 a converging run
    // measures ~2.1 s and a bound-spending run ~6 s, which separates the two
    // by about 2 s in each direction. See the task report's RED evidence.
    const std::uint32_t timeoutMs = 4000;
    WitnessRun run = RunWitness(HostInv(scratch,
        { "--settle", "4", "--settle-timeout", std::to_string(timeoutMs),
          "--compare", "runtime-scene" }));

    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    INFO("exit " << run.exitCode << ", wall " << run.wallMs << " ms, timedOut " << run.timedOut);

    REQUIRE_FALSE(GradeProcessFacts(run).has_value());   // report exists and parsed

    // THE FACT UNDER TEST, asserted FIRST so a run that converged early fails
    // HERE rather than on some downstream consequence: both bounds were
    // genuinely spent. The wall clock is the only time signal a spawned host
    // exposes -- the report carries settleAttemptsUsed but no elapsed ms.
    REQUIRE(run.wallMs >= timeoutMs);
    REQUIRE(run.exitCode != 0);

    REQUIRE(run.report.contains("settleAttemptsUsed"));
    REQUIRE(run.report.contains("settleBailReason"));
    REQUIRE(run.report.contains("exitReason"));
    REQUIRE(run.report["settleAttemptsUsed"].is_number_unsigned());
    REQUIRE(run.report["settleBailReason"].is_string());
    REQUIRE(run.report["exitReason"].is_string());
    // Not a machine-speed race: SettleBailDecision (SettleBound.hpp) only
    // ever bails once BOTH bounds are spent -- it is a CONJUNCTION, `if
    // (attemptsUsed < attempts || elapsedMs < timeoutMs) return Keep;` -- so
    // a bail on this --settle 4 run holds settleAttemptsUsed >= 4 on any
    // host, fast or slow. A bail with fewer attempts would mean the
    // conjunction itself regressed, and red would be the correct verdict.
    REQUIRE(run.report["settleAttemptsUsed"].get<std::uint64_t>() >= 4);
    const std::string bail = run.report["settleBailReason"].get<std::string>();
    // TIGHTENED FROM A DISJUNCTION -- "timeout-bound" is the ONLY healthy
    // label here, because which bound governs is a PURE FUNCTION of this
    // scenario's own arguments, not of the machine it runs on.
    // SettleBailDecision (SettleBound.hpp:101-134) computes
    // attemptsNeeded = ceil(timeoutMs / intervalMs) and returns AttemptsBound
    // iff attempts >= attemptsNeeded, else TimeoutBound. intervalMs is
    // kSettleIntervalMs (SettleBound.hpp:32) = 50, a compile-time constant,
    // not a measurement. With this run's attempts=4 and timeoutMs=4000:
    // attemptsNeeded = ceil(4000 / 50) = 80, and 4 >= 80 is false, so the
    // decision is TimeoutBound on every host, fast or slow -- the 4-attempt
    // budget spends only ~200ms (4 * 50ms) against a 4000ms timeout, nowhere
    // close to outlasting it. A disjunction admitting "attempts-bound" would
    // pass a CONJUNCTION-regressed host that bails attempts-only-but-paced:
    // such a host would report the wrong label here while still incidentally
    // clearing `REQUIRE(run.wallMs >= timeoutMs)` above (~2s boot + ~2.8s of
    // paced attempts >= 4000ms), so that assertion alone cannot catch the
    // regression -- this one closes that hole.
    REQUIRE(bail == "timeout-bound");
    // "compare-failed", NOT "settle-not-converged": the latter is the
    // compare-never-evaluated spelling (RuntimeApp.cpp:1158), and this run
    // evaluated the comparison on every stable, idle attempt.
    REQUIRE(run.report["exitReason"].get<std::string>() == "compare-failed");
    // The mirror half: capture WORKED. Stably wrong is a different fact from a
    // readback that never landed, and this pins that the two do not collapse.
    REQUIRE(bail != "capture-failed");
}

TEST_CASE("W3: a reference missing at EVERY level reports the ordered search space",
          "[witness][gpu]")
{
    WitnessScratch scratch(StagedRuntimeDir(), "w3-missing-reference");
    const auto refs = scratch.Dir() / "ReferenceProject" / "Verify" / "References";
    INFO("staged references: " << refs.string());
    REQUIRE(std::filesystem::exists(refs / "runtime-scene.png"));
    REQUIRE(std::filesystem::exists(refs / "vulkan" / "runtime-scene.png"));
    // Every level of the chain has to go (Arc A: one level just falls back).
    REQUIRE(std::filesystem::remove(refs / "vulkan" / "runtime-scene.png"));
    REQUIRE(std::filesystem::remove(refs / "runtime-scene.png"));
    for (const std::string& set : AdapterSetsHolding(refs, "runtime-scene.png"))
        REQUIRE(std::filesystem::remove(refs / set / "runtime-scene.png"));

    auto run = RunWitness(HostInv(scratch,
        { "--settle", "2", "--settle-timeout", "1000", "--compare", "runtime-scene" }));

    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    INFO("exit " << run.exitCode << ", wall " << run.wallMs << " ms, timedOut " << run.timedOut);

    REQUIRE_FALSE(GradeProcessFacts(run).has_value());   // report exists and parsed

    // EXIT CODE -- derived from a real run against this scratch copy, not
    // guessed (see the task report's evidence). RuntimeApp::MainLoop
    // resolves the --compare reference BEFORE the frame loop starts; a
    // resolution of ReferenceLevel::None with --bless absent is a FAIL-FAST
    // there (RuntimeApp.cpp:567-581): m_graphExit = 4, ShutdownGraphPath(),
    // return -- the settle loop this scenario's --settle/--settle-timeout
    // name never runs a single attempt. main.cpp returns m_graphExit
    // verbatim (`rc = app.Run(); ... return rc;`), so the process exits 4.
    // exitReason is picked from the SAME chain W1 reads, but a branch
    // EARLIER: m_compareMissingFatal is checked before settleFailed
    // (RuntimeApp.cpp:1137-1138), so this is "compare-missing-reference",
    // never W1's "compare-failed" -- a different fact, not a relabeling.
    REQUIRE(run.exitCode == 4);
    REQUIRE(run.report.contains("exitReason"));
    REQUIRE(run.report["exitReason"].is_string());
    REQUIRE(run.report["exitReason"].get<std::string>() == "compare-missing-reference");

    // The settle verdict is never REACHED on this path -- SettleVerdictReached
    // (SettleBound.hpp) names this exact case: no attempt was taken, so
    // VerifyReport::SetSettle is never called and the keys are absent, not
    // zeroed. Pinning the absence keeps W3 from being read as "W1 with a
    // different reference" -- it is a genuinely different code path, one
    // that never reaches the settle loop at all.
    REQUIRE_FALSE(run.report.contains("settleAttemptsUsed"));
    REQUIRE_FALSE(run.report.contains("settleBailReason"));

    REQUIRE(run.report.contains("compare"));
    const auto& compare = run.report["compare"];
    REQUIRE(compare.contains("resolvedLevel"));
    REQUIRE(compare["resolvedLevel"].is_string());
    REQUIRE(compare["resolvedLevel"].get<std::string>() == "none");
    REQUIRE(compare.contains("triedPaths"));
    REQUIRE(compare["triedPaths"].is_array());
    const auto& tried = compare["triedPaths"];
    // Two levels on a hardware adapter; three on a software one, whose own
    // reference set (ReferenceAdapterSet, e.g. "vulkan-lavapipe") is probed
    // before the two below.
    REQUIRE((tried.size() == 2 || tried.size() == 3));
    for (const auto& t : tried) REQUIRE(t.is_string());
    const std::size_t b = tried.size() - 2;
    if (b == 1)
    {
        const std::filesystem::path adapter = tried[0].get<std::string>();
        INFO("adapter-level candidate: " << adapter.string());
        REQUIRE(adapter.filename() == "runtime-scene.png");
        REQUIRE(adapter.parent_path().filename().string().rfind("vulkan-", 0) == 0);
    }
    // Ordered: backend level first, shared second (as Task 1 pinned).
    // ReferenceImages.cpp's ResolveReference pushes the backend-keyed
    // candidate (`root/vulkan/runtime-scene.png`) before the shared one
    // (`root/runtime-scene.png`) -- both literally, and in triedPaths.
    REQUIRE(std::filesystem::path(tried[b].get<std::string>()).parent_path().filename() == "vulkan");
    REQUIRE(tried[b + 1].get<std::string>().find("runtime-scene.png") != std::string::npos);
}

TEST_CASE("W4: a dynamic body authored in physics.arcscene falls under the runtime host",
          "[witness][gpu]")
{
    // The whole chain, observed from OUTSIDE: the v5 scene with Arcane::Collider2D
    // fixtures loads, Runtime installs Arcane::PhysicsSystem2D and mints the world,
    // fixedUpdate steps it 60 times at 1/60 s (--headless pins the sim dt),
    // PASS 4 writes the pose back, propagation composes it, the sprite
    // draws where the body ended. Geometry (physics.arcscene, +Y down,
    // 1280x720, ortho 2.0 -> 180 px/m, origin at (640,360)): the Crate is
    // authored spanning pixels y 90..270 and comes to rest on the Ground
    // spanning 315..495, so a pick at its authored centre finds NOTHING and
    // a pick at its resting centre finds "Crate". No render change is
    // claimed: this scene is not the boot scene and no golden covers it.
    //
    // TWO separate host runs, one probe each -- VerifyReport.cpp's own
    // comment (`only the first pick@ probe in a run is armed`, backed by
    // RuntimeFrame.cpp's RenderGraph arming FrameDesc::pickPixel once from
    // FirstPickProbe) means a second `--probe pick@...` in the same run
    // never gets an "entity" key at all, only an "error" one explaining it
    // was never armed. The plan's snippet asked both in one run; that reads
    // as a plan-snippet defect against the current host, not a product bug.
    WitnessScratch authoredScratch(StagedRuntimeDir(), "w4-physics-falls-authored");
    WitnessRun authoredRun = RunWitness(HostInv(authoredScratch,
        { "--scene", "4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90", "--probe", "pick@640,120" }));
    INFO("host stdout: " << authoredRun.stdoutPath.string());
    INFO("host stderr: " << authoredRun.stderrPath.string());
    INFO("exit " << authoredRun.exitCode << ", wall " << authoredRun.wallMs
                  << " ms, timedOut " << authoredRun.timedOut);
    REQUIRE_FALSE(GradeProcessFacts(authoredRun).has_value());
    REQUIRE(authoredRun.exitCode == 0);
    REQUIRE(authoredRun.report["exitReason"].get<std::string>() == "frames-complete");
    REQUIRE(authoredRun.report.contains("probes"));
    REQUIRE(authoredRun.report["probes"].size() == 1);
    const nlohmann::json& authored = authoredRun.report["probes"][0];
    REQUIRE(authored["kind"] == "pick");
    REQUIRE(authored.contains("entity"));
    CHECK(authored["entity"].is_null());                          // it left

    WitnessScratch restingScratch(StagedRuntimeDir(), "w4-physics-falls-resting");
    WitnessRun restingRun = RunWitness(HostInv(restingScratch,
        { "--scene", "4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90", "--probe", "pick@640,405" }));
    INFO("host stdout: " << restingRun.stdoutPath.string());
    INFO("host stderr: " << restingRun.stderrPath.string());
    INFO("exit " << restingRun.exitCode << ", wall " << restingRun.wallMs
                  << " ms, timedOut " << restingRun.timedOut);
    REQUIRE_FALSE(GradeProcessFacts(restingRun).has_value());
    REQUIRE(restingRun.exitCode == 0);
    REQUIRE(restingRun.report["exitReason"].get<std::string>() == "frames-complete");
    REQUIRE(restingRun.report.contains("probes"));
    REQUIRE(restingRun.report["probes"].size() == 1);
    const nlohmann::json& resting = restingRun.report["probes"][0];
    REQUIRE(resting["kind"] == "pick");
    REQUIRE(resting.contains("entity"));
    REQUIRE(resting["entity"].is_string());
    CHECK(resting["entity"].get<std::string>() == "Crate");       // and landed here
}

TEST_CASE("W7: a game system sees the Crate land (a dynamic-vs-static contact event) and tints it",
          "[witness][gpu]")
{
    // physics.arcscene: the Crate carries ReferenceProject::TintOnContact (red).
    // TintOnContactSystem (Update phase) reads PhysicsWorld2D::FrameEvents() and sets the
    // tint on the Arcane::ContactBegin2D that names it. The Crate rests centred at pixel
    // (640, 405) after 60 frames (W4's resting pick). Authored tint is orange
    // (0.9, 0.6, 0.2); after the event it is red (1, 0, 0). Asserting red-dominant
    // with a crushed green keeps the check robust to the post chain's tone mapping.
    WitnessScratch scratch(StagedRuntimeDir(), "w5-physics-event-tint");
    WitnessRun run = RunWitness(HostInv(scratch,
        { "--scene", "4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90", "--probe", "rgba@640,405" }));
    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report["exitReason"].get<std::string>() == "frames-complete");
    REQUIRE(run.report["probes"].size() == 1);
    const nlohmann::json& v = run.report["probes"][0]["value"];
    const int r = v["r"].get<int>(), g = v["g"].get<int>();
    INFO("rgba " << r << "," << g << "," << v["b"].get<int>());
    CHECK(r > 2 * g);          // orange (0.9 vs 0.6) fails this; red passes
    CHECK(g < 100);
}

TEST_CASE("W5: the F3 cull/blend fixture scene matches its golden and reports the "
          "visibility split the mixed blend modes imply", "[witness][gpu]")
{
    // The RUNTIME half of spec s9.2's "the witness lanes assert the
    // visibility block" (EditorWitnessTest.cpp's E2 is the editor half, on
    // the all-opaque boot scene). This one runs the F3 plan 2 FIXTURE scene
    // -- ReferenceProject/Content/scenes/f3_cull_blend.arcscene, the scene
    // golden-gate.ps1's two `f3-cull-blend` lanes also render -- because the
    // boot scene cannot express what plan 2 shipped: it has no transparent
    // material, no masked one, and no two-sided one, so on it `batches ==
    // draws` and `gpuVisible == coarseVisible` are both true by accident of
    // content rather than by the mechanism being right.
    //
    // EVERY NUMBER BELOW IS DERIVED FROM THE FIXTURE'S OWN CONTENT, not
    // recalled from a previous run:
    //   total 20           -- twenty MeshRenderer entities, each on a
    //                         single-section primitive mesh: fourteen cubes
    //                         (the thirteen original ones plus
    //                         TransBehindMasked, the transparent cube behind
    //                         the masked/opaque pair that proves the masked
    //                         pipeline WRITES depth) and six planes (the
    //                         one-sided/two-sided away-facing twins, one pair
    //                         per blend mode: opaque, masked, transparent).
    //   coarseVisible 20   -- the CPU coarse stage is geometric only. The
    //                         alpha-clipped cube is still coarse-visible (its
    //                         pixels die in the shader, not in the frustum
    //                         test), the edge-straddling cube is admitted by
    //                         the conservative test that case exists to
    //                         prove, and the three one-sided away-facing
    //                         planes are coarse-visible too: back-face culling
    //                         happens in the raster, never in the frustum
    //                         test, so their absence from the picture is not
    //                         an absence from this count.
    //   transparentRows 11 -- eleven entities carry a `transparent` material
    //                         (the far/near pair, the equal-depth pair, the
    //                         render-order pair, the depth-bias pair,
    //                         TransBehindMasked, and the transparent
    //                         one-sided/two-sided plane twins); each is one
    //                         DIRECT draw record, never a batch.
    //   batches 6          -- the emitted (mesh, section, blend, twoSided)
    //                         keys over the nine non-transparent rows:
    //                         (cube, opaque, 1-sided), (cube, masked, 1-sided),
    //                         (plane, opaque, 1-sided), (plane, opaque,
    //                         2-sided), (plane, masked, 1-sided), (plane,
    //                         masked, 2-sided). Transparent keys are excluded
    //                         from the emitted list by construction (spec
    //                         s5.4), so the transparent twins add rows but no
    //                         batch.
    //   draws 17           -- 6 + 11, the identity the report states and this
    //                         case re-derives rather than copies.
    //   gpuVisible 9       -- the CULL PASS's own read-back total: the nine
    //                         rows in emitted batches, all of them inside the
    //                         frustum (the one-sided away-facing opaque and
    //                         masked planes included -- the GPU cull is
    //                         frustum-only and knows nothing of facing).
    //                         STRICTLY LESS than coarseVisible here, which is
    //                         the fact E2's scene cannot show.
    WitnessScratch scratch(StagedRuntimeDir(), "w5-f3-cull-blend");
    WitnessRun run = RunWitness(HostInv(scratch,
        { "--scene", "7e5a0030-0030-4030-8030-000000000030",
          "--settle", "30", "--compare", "f3-cull-blend" }));

    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    INFO("exit " << run.exitCode << ", wall " << run.wallMs << " ms, timedOut " << run.timedOut);

    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("exitReason"));
    CHECK(run.report["exitReason"].get<std::string>() == "frames-complete");

    // THE PICTURE. Without this conjunct the counts below could all be right
    // while the six visual cases rendered wrong -- the golden is what says
    // the cull, the blend modes and the order produced the right pixels.
    REQUIRE(run.report.contains("compare"));
    const auto& compare = run.report["compare"];
    CHECK(compare.at("reference") == "f3-cull-blend");
    CHECK(compare.at("passed") == true);
    CHECK(compare.at("diffCount") == 0);

    REQUIRE(run.report.contains("visibility"));
    const auto& vis = run.report["visibility"];
    REQUIRE(vis.at("total").is_number_unsigned());
    CHECK(vis.at("total") == 20);
    CHECK(vis.at("coarseVisible") == 20);
    CHECK(vis.at("batches") == 6);
    CHECK(vis.at("transparentRows") == 11);
    // The two RELATIONS, asserted as relations and not only as literals: a
    // future edit to the fixture that adds a mesh has to update the literals
    // above, but these two must hold whatever the scene contains.
    CHECK(vis.at("transparentRows").get<std::uint32_t>() > 0u);
    CHECK(vis.at("draws").get<std::uint32_t>()
          == vis.at("batches").get<std::uint32_t>()
           + vis.at("transparentRows").get<std::uint32_t>());
    CHECK(vis.at("draws") == 17);
    // A NUMBER, never null: the delayed readback ring must have retired one
    // frame inside this run's budget. Null here means the budget is too short
    // and the fix is a bigger --frames, never a relaxed assertion.
    REQUIRE(vis.at("gpuVisible").is_number_unsigned());
    CHECK(vis.at("gpuVisible") == 9);
    // The half E2 cannot state: transparent rows never reach the cull, so the
    // GPU's count is STRICTLY below the coarse one on a scene that has any.
    CHECK(vis.at("gpuVisible").get<std::uint32_t>()
          < vis.at("coarseVisible").get<std::uint32_t>());
}

#include <fstream>

TEST_CASE("W6: a headless runtime neither reads nor writes the slot's imgui.ini",
          "[witness][gpu]")
{
    // THE LEVER: the imgui.ini a windowed session leaves beside the exe. On
    // 2026-09-28 the golden gate's two runtime lanes were red by ~2600 px and
    // never settled ("timeout-bound") while the same compare passed from a
    // fresh scratch copy -- the slot held an ini with the HUD at Pos=59,60,
    // one pixel off the default, and the headless host read it. The editor
    // pins io.IniFilename = nullptr under --headless (EditorApp.cpp, "THE
    // FLAG BEATS THE INI"); the runtime must do the same, or every golden
    // depends on whatever ini the slot happens to hold.
    WitnessScratch scratch(StagedRuntimeDir(), "w6-imgui-ini-ignored");
    const std::filesystem::path ini = scratch.Dir() / "imgui.ini";
    const std::string poisoned = R"ini([Window][ArcaneRuntime]
Pos=59,60
Size=416,116
Collapsed=0
)ini";
    {
        std::ofstream out(ini, std::ios::binary);
        REQUIRE(out.good());
        out << poisoned;
    }

    WitnessRun run = RunWitness(HostInv(scratch, { "--settle", "30", "--compare", "runtime-scene" }));
    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);

    // Neither read (the HUD sits where the golden has it, and the frame
    // settles) ...
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("passed") == true);
    CHECK(run.report["compare"].at("diffCount") == 0);
    CHECK(run.report.at("settleBailReason") == "converged");

    // ... nor written: the file is byte-for-byte what this case seeded.
    std::ifstream in(ini, std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(after == poisoned);
}
