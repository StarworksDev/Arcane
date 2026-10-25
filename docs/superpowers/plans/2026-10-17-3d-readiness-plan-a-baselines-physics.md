# 3D-readiness Plan A — Baselines and Physics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Execution gates (re-vet 2026-10-19).** Tasks 1-7 run now. Task 8 waits for fix-round Tasks 11-12 on main. Run each plan's Servitor sequence once on an unmodified base build first.

Amended 2026-10-20 from arcane-research/box3d/2026-10-20-seam-check.md (Box3D seam check: Q1, Q2, and Q3's hook order).

**Goal:** Check in the physics and `.arcart` baselines from this HEAD, move the 2D sim body into one translation unit, then register 2D physics through a solver-free hook so Engine and Render no longer include Manifold.

**Architecture:** Tasks 1–3 only add checked-in bytes and the tests that compare them. Task 4 moves `PhysicsSystem2D::operator()` into `PhysicsSystem.cpp` and leaves behaviour alone. Tasks 5–8 are coupling 3.1: the component header moves to ArcaneScene, `PhysicsBodyRef2D::handle` becomes the packed `uint64`, Extract asks Physics2D through solver-free proxies, and a `SimHook` recorded at image load (the same shape as `ModuleAttach`, not `SystemFactoryTable`) is what Runtime calls. The step trace from Task 1 is compared again after Task 4 and again after Task 8. It must be bit-identical both times.

**Tech Stack:** C++23, premake5, MSBuild (VS 2026), Catch2 v3, Astra ECS, Manifold2D, PowerShell, Servitor.

**Spec:** `D:/dev/starworks/arcane-readiness/2026-10-17-3d-readiness-arc-design.md` (binding, including the Review log through Revision 4). This plan implements section 4 steps 1, 4, and 5, and the Done lists of sections 3.1 and 3.2. Where this plan and the spec disagree on a line number, the line verified below wins. Where they disagree on behaviour, stop and report; do not invent a behaviour.

**Prerequisite gate:** F5 Plan 1 is in this tree: the arc branch is cut from `main` at `a12f33e6` or later (F5 merged as `ab8b7af9`, PR #17). Do not start from `feat/f5-unified-world` or the `Arcane-f5` worktree. `kGamePluginABIVersion` stays whatever `AbiVersion.hpp` contains when you start (61 at the 2026-10-17 read). This plan does not bump it.

**Sequencing inside this plan, and against Plan B.** Tasks 1–3 are the baselines. **Stop after Task 3.** Plan B Tasks 1 and 2 (the digest tool, `build/abi-digest.txt`, the one epoch bump, and the load gate) merge next, while `PhysicsSystem2D::operator()` is still the inline body in `PhysicsSystem.hpp` and `PhysicsBodyRef2D::handle` is still `Detail::Physics2D::BodyHandle`. Design section 4 step 3 (the paragraph at design lines 443–444) puts ABI identity before the sim-body move and before the handle change: both are measured against the manifest. Then Task 4. Task 4 does not change a game-facing field's declared type, does not `--write`, and does not change `kGamePluginABIDigest`. Its build runs `arcane-abi-digest --check` and that check exits 0, because the class layout stays `m_fixedDt` then `m_stepWorld`. Then Tasks 5–8. Task 5 changes the field's declared type to `std::uint64_t` and, in that same commit, rebuilds `arcane-abi-digest` against the new header, then runs `--write`, then the full desk build. It sets `kGamePluginABIDigest` to the integer that rebuilt tool printed. It does not bump the epoch (design section 7: coupling 3.1 leaves the epoch; the digest updates). Do not start Task 4 in a tree with no `build/abi-digest.txt`. The trace must still match after Task 4 alone, before any hook edit.

## Global Constraints

- **Worktree:** `D:\dev\starworks\Arcane-3dr`, branch cut from `main` `a12f33e6`. `Arcane-f5` is the old F5 branch at `309aa06e`; never build or test there. Never push, never force-push, never merge to `main`. Plans are the files in `D:\dev\starworks\arcane-readiness\`. Do not edit those plan files while implementing.
- **One heavy build at a time across worktrees.** Do not start `build.ps1`, `msbuild`, or `cl.exe` while another worktree on this machine is already compiling.
- **User-at-desk cap.** Unset `CI` and `ARCANE_BUILD_MACHINE` for every local build and test. Compile below normal priority, four MSBuild jobs, `/MP4`:

```powershell
Remove-Item Env:CI -ErrorAction SilentlyContinue
Remove-Item Env:ARCANE_BUILD_MACHINE -ErrorAction SilentlyContinue
$env:_CL_ = '/MP4'
# Windows PowerShell 5.1 Start-Process has no -Priority. -PassThru returns the
# process; PriorityClass is the 5.1 way to lower it. ExitCode is the build's
# code. Start-Process itself does not fail when build.ps1 fails.
$build = Start-Process -FilePath powershell.exe -ArgumentList @(
    '-NoProfile','-ExecutionPolicy','Bypass','-File',
    'D:\dev\starworks\Arcane-3dr\scripts\ci\build.ps1','Debug','-Jobs','4'
) -WorkingDirectory 'D:\dev\starworks\Arcane-3dr' -PassThru -NoNewWindow
if (-not $build.HasExited) { $build.PriorityClass = 'BelowNormal' }
$build.WaitForExit()
if ($build.ExitCode -ne 0) { throw "desk build failed: exit $($build.ExitCode)" }
```

Run that from the repo root. `scripts\ci\build.ps1` is the Windows sequence: generate, engine, `arcbuild build --project ReferenceProject`, hosts (`scripts/ci/build.ps1:39-43`). Logs: `logs\build-{generate,engine,referencegame,tests}.log`. A new `.cpp` is not in `ArcaneTests` until that generate has run: premake globs `src/**.cpp` (`premake5.lua:1316`) into the vcxproj, and an exe built before the new file exists is the old binary. `build.ps1` always regenerates. New code compiles on windows-msvc, linux-gcc-14, linux-clang-19, and macos-appleclang. Windows-only test code sits under `#if defined(_WIN32)`.
- **Tests** run from the exe directory. Record the `Randomness seeded to:` banner. New cases pass `--rng-seed 1` while iterating. Suite gate before every commit:

```powershell
Remove-Item Env:CI -ErrorAction SilentlyContinue
Remove-Item Env:ARCANE_BUILD_MACHINE -ErrorAction SilentlyContinue
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

`run-tests.ps1` pushes to the exe directory (`scripts/run-tests.ps1:66`). After any `Set-Location` into that directory, every script path is root-qualified (`D:\dev\starworks\Arcane-3dr\scripts\...`). A relative `.\scripts\...` or `scripts\...` does not exist there. `run-tests.ps1` is `~[gpu]` plus `scripts\check-baselines.ps1`. A count that rises is fine; a drop fails. Never construct a bare `Arcane::Runtime`. Use `Arcane::Runtime rt(Arcane::Test::Process());` (`RuntimeEngineSystemsTest.cpp:71`).

- **Red steps.** After writing a new test `.cpp`, run the desk build above before `ArcaneTests.exe`. Then say which failure is the red:
  - **Compile error:** the build's `ExitCode` is non-zero and `logs\build-engine.log` or `logs\build-tests.log` names the missing declaration. Do not run the previous exe. That compile error is the red.
  - **Assertion:** the build's `ExitCode` is 0, then the new exe fails the named `CHECK` or `REQUIRE` (a missing checked-in file is this kind only when the test compiled).
  A step that says "FAIL, file missing" without a successful build of the new TU is running the stale binary. Do not write that.
- **D3D12 debug layer.** `Tests/ArcaneTests/src/test_main.cpp` (PR #16, on main) calls `ForceD3D12DebugLayerForProcess` unless `ARCANE_TESTS_NO_D3D12_DEBUG=1`. Its `RenderErrorListener` samples the monotonic `RenderErrorTotal()` around every case and fails a case whose total rose, even when Catch returned 0, unless the case is tagged `[expects-render-errors]`. Do not revert it. Do not set the opt-out. GPU work in later plans stays `RenderErrorCount() == 0` with the layer on. This plan's only GPU gate is the golden gate, and it does not re-bless.
- **Golden gate** runs once, at the end of Task 8, not after Tasks 1–7. Before it, delete `imgui.ini` in `Binaries\Debug-windows-x86_64-md\ArcaneRuntime` and `Binaries\Debug-windows-x86_64-md\ArcaneEditor`. Expected: `scripts\golden-gate.ps1` reports 14 lanes and every lane `diffCount` 0 (`scripts\golden-gate.ps1:326` is the fourteen-lane comment). This arc does not re-bless.
- **Servitor** after every task whose change the running host executes: Tasks 4, 5, 7, and 8. Tasks 1–3 only add tests and checked-in bytes. Task 6 adds proxies `ExtractFrame.cpp` does not call yet; the host check for that path is Task 7. Binary: `Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe`, built from this tree. Launch your own `--headless` host. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way. Never drive an editor the user has open. Never `--bless`. Quit what you launched, including when a later command fails. Record every exit code. Exit codes: 0 ok, 4 no host, 5 retry, 6 failed, 7 host died, 8 check failed. A launched runtime or server is held at frame 0 until `play` or `step` (the server uses the same hold). `EnsurePhysics` runs from the tick (`RuntimeFrame.cpp:338`, `ServerApp.cpp:279`), so the world check is after `step`, not on the held frame. The world check is `resource list`: exit 0 and the envelope's `resources` array contains the name `Arcane::PhysicsWorld2D` (Astra's canonical `TypeID<T>::Name()`, `TypeNameCanonical.hpp`: no `struct` keyword on any compiler). An empty array is a failure. Do not use `resource get PhysicsWorld2D`: the type has no `ARC_REFLECT_TYPE` (`Physics2DWorld.hpp:67-84`), so get returns `reflect.unreadable` (exit 6) even when the instance exists. Do not treat an `ecs query` that returns zero rows as proof the world was minted.
- **ABI:** do not change `kGamePluginABIVersion`, do not edit `PluginABI.hpp`, and do not edit any `.arcproj` `"abi"` field. Task 4 may run `arcane-abi-digest --check` and must not `--write`. Task 5 may replace `kGamePluginABIDigest` and `build/abi-digest.txt` only with the bytes printed by an `arcane-abi-digest.exe` that was rebuilt after the header edit. `--write` from the previous binary records the old mix. It does not bump the epoch.
- **Style:** `/MD`, no `/fp:fast`, MKS, `+Y` up, gravity default `{0, -9.81}` (`Physics2DBinding.hpp:23`), ASCII comments, UTF-8 without BOM. Do not edit `ThirdParty/`. Do not "fix" kinematic velocity, collider remint, or any L09 gap while moving code. Failure is a `Result`, a refusal, or an empty optional. No new listener bus.
- **Every commit message ends with these two lines, exactly:**

```
Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

### Line drift (code wins)

Re-verified 2026-10-19 on `main` `a12f33e6`: no cited file changed since `309aa06e`.

Verified 2026-10-17 at HEAD `309aa06e`. Use these paths and lines, not the spec's when they differ.

| Spec citation | Code |
|---|---|
| `EngineRoster.hpp:22` and `:32-37` | `Source/Runtime/ArcaneEngine/src/Arcane/Scene/EngineRoster.hpp`. Include is line 23. The `using` is lines 34–37. The order comment is lines 32–33. |
| `ProjectHost.hpp:87-88`, `:116-122` | `Source/Runtime/ArcaneEngine/src/Arcane/Project/ProjectHost.hpp`. `IsErr()` return true is lines 87–88. `VerifySharedTypeContext` is lines 120–123. The "thirteen" comment is lines 116–119. |
| `Runtime.cpp` | `Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.cpp`. `RegisterRoster` line 326. Frame-begin hook lines 370–376. `ClearSystems` lines 577–582. `InstallEngineSystems` lines 590–616. `EnsurePhysics` lines 628–655 (`SetExecutor` at 651–652). `PhysicsEditPass` lines 657–661. `ResetPhysics` lines 663–672. |
| `SystemSchedulers.hpp:6-9` | `Source/Runtime/ArcaneSim/src/Arcane/Sim/SystemSchedulers.hpp`. The stale sentence is lines 8–9. |
| `PhysicsSystem.hpp:19` | Body is lines 19–433. The closing brace of `operator()` is line 433. |
| `Physics2DWorld.hpp:106-116` | `struct PhysicsSystem2D` is lines 106–121. |
| `EngineResourceTypes.hpp:13-15` "order is not load-bearing" | That sentence is `EngineResourceTypes.cpp:13-14`. The header's lines 13–15 are the `type_info` comment. `PhysicsSystem.hpp` include is `EngineResourceTypes.cpp:3`. `TypeID<PhysicsWorld2D>` is line 21. |
| `ServerApp.cpp:10` | Matches: `#include <Arcane/Scene/PhysicsSystem.hpp>`. `hasPhysics` is line 332. |
| `DebugCollide` "returns a NarrowphaseTrace" | `ThirdParty/Manifold2D/include/Manifold2D/Physics/PhysicsWorld.hpp:944-945` returns `Manifold` and writes `NarrowphaseTrace& out`. The inspector snapshot uses `out`. Do not edit this vendored declaration. |
| `PhysicsEvents2D.cpp:107` | Matches: `BeginFrame` clears `frameEvents` only. `StepEvents()` is replaced at `CaptureStep`, not cleared by `BeginFrame`. |
| `modules.lua:571` | `ARC_GAME_DEFAULT_MODULES` is five names and does not include `ArcanePhysics2D`. `kDefaultGameModules` is `std::array<std::string_view, 5>` at `Source/Runtime/ArcaneEngine/src/Arcane/Project/GameDescriptor.hpp:23-25`. |
| `ArcaneEngine.arcmodule:8`, `ArcaneRender.arcmodule:8` | Both public lists include `ArcanePhysics2D`. `ArcanePhysics2D.arcmodule:6-8` is Core and Scene. It does not list Engine. |
| `kInvalidBody` | `Physics2DHandles.hpp:23` is `BodyHandle{}`. Manifold's sentinel is `{0u, 0u}` at `PhysicsTypes.hpp:121`. `PackBody` of that value is 0 (`Physics2DDetail.hpp:105-108`). |
| `DrawPhysicsDebug` "inside Physics2D" | The declarations are `Source/Client/ArcaneRender/src/Arcane/Render/PhysicsDebugDraw.hpp:110-114` and `:139-145`. The bodies are `PhysicsDebugDraw.cpp:32` and `:39`. The Line/Circle loop is the file-local `DrawGeometry` at `PhysicsDebugDraw.cpp:21-30`. Task 7 keeps that loop in Render and moves the two Manifold-taking entry points to a test helper. |
| `BodyHandle` layout | `PhysicsTypes.hpp:101-105`: `uint32 index`, `uint32 generation`. Packed `uint64` is `(index << 32) \| generation`, which is **not** the same byte order as the struct on little-endian. `sizeof` of the field stays 8. `alignof(uint64)` is 8; `alignof(BodyHandle)` is 4. After the field change the in-memory struct is 16 bytes and its alignment becomes 8 (`uint64` + `glm::vec2`; this tree does not define `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES`). The version-2 disk form is not a memcpy of that struct. Task 5's traits write `appliedScale` only. The v1 blob is today's `Registry::Save` bytes, not a hand-padded layout. Do not bump the epoch for the field change. |

## Review Focus

These are the five inputs most likely to bite that a task's other tests would miss. Each one has a pinning test in the task named.

1. **A missing physics hook still passes `VerifySharedTypeContext`.** `ProjectHost.hpp:87-88` returns true when `GetComponentIDFromHash` is `IsErr()` ("not registered yet"). Calling `TypeID<RigidBody2D>::Value()` from the test would itself mint the id and hide the miss. The boot test must `REQUIRE` the registry hash lookup `IsOk()` before it compares ids, and a child process with `ARCANE_TEST_NO_PHYSICS_HOOK=1` must fail that `REQUIRE`. Pinned in Task 8.
2. **One step-time debug list cannot satisfy two views.** Ordinary debug is affine `{640,360}` / `{100,-100}`, alpha 0. The alt file uses a different affine and alpha `0.25`. A gather that ignores the request and replays one stored list fails `v1-debug-altview.txt`. Pinned in Task 2 (bytes) and Task 7 (the extract request reproduces both).
3. **`ClearSystems` without `InstantiateModuleSystems` must still have `PhysicsSystem2D`, and the next frame-begin clears `frameEvents` only.** `PhysicsEvents2D.cpp:107` does not clear `stepEvents`. A test that expects `StepEvents()` to be empty after `BeginFrame` and before the next step is wrong: `StepEvents()` still holds the previous step until `CaptureStep` replaces it. `FrameEvents()` is empty after the begin callback. Pinned in Task 8. The existing clear case is `RuntimeEngineSystemsTest.cpp:78-79` and must stay green.
4. **The `uint64` handle round-trips `PackBody` bit-identically, and the binary save drops it.** `PackBody({7,3})` is `(7ull << 32) | 3`. `Serializable false` is the JSON walk only (`SceneSerializer.hpp` skips a field when `IsSerializable()` is false; Astra's `Attribute.hpp` says the built-in binary path is unaffected). `BinaryWriter` writes a trivially copyable component whole (`BinaryWriter.hpp` POD branch) and `BinaryReader` reads it whole. Today's `PhysicsComponentsTest.cpp` expects `{7, 3}` to survive `Registry::Save` / `Load`, and that expectation matches the code. Task 5 adds `SerializationTraits` so a new save writes `appliedScale` and not the handle, and a version-1 blob captured before the change still loads. After either load the handle is 0 and `appliedScale` is the saved value. A store of the raw `BodyHandle` memory bytes is the wrong packed value on little-endian. A stored version below `MinVersion` is `UnsupportedVersion` from Astra before `Migrate`. A stored version above 2 cannot be `UnsupportedVersion`: `Migrate` is void and the reader has no error setter, so that path forces `CorruptedData`. `Registry::Load` collapses both failures to `CorruptedData`. Pinned in Task 5.
5. **Staging the DLL does not run the static registrar.** `modules.lua:544-549` says MSVC and GNU `--as-needed` drop an unreferenced import, so `ARC_SETTINGS` and a static registrar never run. Adding `ArcanePhysics2D` only to a `stage =` list leaves `hasPhysics` false. Server and Runtime must name it in the **linked** consumer list. Pinned in Task 8 by `ServerWitnessTest.cpp:48` (`systems.hasPhysics == true`) and by a runtime whose id assert passes. A stage-only edit fails that census.

## Out of scope

Do not implement any of these. A task that starts one is wrong; stop.

- Plan B's codec move, its epoch bump, and the first digest (written while `operator()` is still inline in `PhysicsSystem.hpp` and `handle` is still `BodyHandle`). Task 4 of this plan only `--check`s that digest. Task 5 rebuilds the tool and then regenerates the digest after the field change. Plan C (bindless, residency, uploads, the fix #11 completion check).
- Fix #11 itself (fix-round Tasks 11-12). On `main` `a12f33e6`, `Runtime::Impl` still constructs one `JobSystem` per world (`Runtime.cpp:200`, initialized at line 239). Task 8 starts only after fix Tasks 11-12 are on `main`. Re-read `Runtime::Impl` then. The hook does not construct a `JobSystem`. It passes the scheduler `EnsurePhysics` hands to `SetExecutor` in that tree (`m_impl->sched.get()` at `Runtime.cpp:651-652` on `a12f33e6`).
- Box3D, a second sim hook, F5 Plans 2 and 3, L09 kinematic/`SetVelocity`/collider-remint changes, L08 artifact security, an ABI bump, a golden re-bless.
- Editing `ThirdParty/Manifold2D`, including `DebugCollide`'s signature.

## File map

| File | Task | Responsibility |
|---|---|---|
| `Tests/ArcaneTests/src/Helpers/PhysicsV1Baseline.hpp` (new) | 1, 2 | Declarations of `BuildV1PhysicsScene`, `WriteV1StepTrace`, and the debug/pick dumpers. Not an anonymous namespace. Task 6 and Task 8 include this header. |
| `Tests/ArcaneTests/src/PhysicsV1BaselineTest.cpp` (new) | 1, 2, 4, 7, 8 | Capture (hidden) and compare of the step trace, debug snapshots, inspector snapshot, and pick snapshot. Includes the helper header. |
| `Tests/ArcaneTests/data/physics/v1-step-trace.txt` + `.sha256` | 1 | 120-step pose and `StepEvents()` bytes. |
| `Tests/ArcaneTests/data/physics/v1-debug-wholeworld.txt`, `v1-debug-selected.txt`, `v1-debug-altview.txt`, `v1-inspector.txt`, `v1-pick.txt`, each with `.sha256` | 2 | Gather and pick bytes from the same scene. |
| `Tests/ArcaneTests/src/ArtifactV1FixtureTest.cpp` (new) | 3 | Writes and compares `v1-texture.arcart` and `v1-mesh.arcart`. |
| `Tests/ArcaneTests/data/artifacts/v1-texture.arcart`, `v1-mesh.arcart`, each `.sha256` | 3 | Pipeline output at this HEAD. |
| `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem.cpp` (new) | 4 | The only definition of `PhysicsSystem2D::operator()`. |
| `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem2D.hpp` (new) | 4 | System declaration, still in Physics2D. `ArcaneScene.arcmodule` has no Physics2D edge, and `PhysicsComponents.hpp` still includes `Physics2DHandles.hpp` until Task 5. |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsComponents.hpp` (moved) | 5 | Solver-free components. `handle` is `uint64`. Include string stays `<Arcane/Scene/PhysicsComponents.hpp>`. `Astra::SerializationTraits<PhysicsBodyRef2D>` lives in this header. `PhysicsSystem2D.hpp` moves here in the same task, after this header no longer includes a solver header. |
| `Tests/ArcaneTests/data/physics/v1-bodyref-save.bin` | 5 | Whole-registry `Registry::Save` of one `PhysicsBodyRef2D` with handle `{7, 3}` and `appliedScale` `{2, 3}`, captured before the field becomes `uint64`. |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugTypes.hpp` (moved) | 7 | Solver-free settings structs. Same include string `<Arcane/Scene/PhysicsDebugTypes.hpp>`. The Physics2D copy is deleted before Render drops its edge. |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugDrawOptions.hpp` (new) | 7 | `PhysicsDebugDrawOptions2D`, moved out of `PhysicsDebugGeometry.hpp` before the edge drop. |
| `Source/Programs/ArcaneServer/src/ServerReport.hpp`, `ServerReport.cpp`, `ServerApp.cpp` | 8 | Additive `systems.hasPhysicsWorld`. `kSchemaVersion` stays 1. |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsQuery.hpp` + `.cpp` (new) | 6 | Solver-free request, silhouettes, `PhysicsDebugPrimitive`, `PhysicsDebugGeometry` (moved), and the forwarders Render calls. No Manifold name in the header. |
| `Source/Runtime/ArcaneCore/src/Arcane/Sim/SimHook.hpp` + `.cpp` (new) | 8 | The hook list. |
| `Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.cpp` | 8 | Calls the hook. Stops naming Manifold, `PhysicsSystem2D`, and `PhysicsWorld2D`. |
| `build/modules.lua`, `GameDescriptor.hpp`, `premake5.lua` | 8 | Linked (not stage-only) `ArcanePhysics2D` on Server, Runtime, and the default game list. |

---

### Task 1: v1 step-trace baseline

**Files:**
- Create: `Tests/ArcaneTests/src/Helpers/PhysicsV1Baseline.hpp`
- Create: `Tests/ArcaneTests/src/PhysicsV1BaselineTest.cpp`
- Create: `Tests/ArcaneTests/data/physics/v1-step-trace.txt`
- Create: `Tests/ArcaneTests/data/physics/v1-step-trace.sha256`
- Modify: `Tests/ArcaneTests/src/MacroPrefixGuardTest.cpp:15-19`: add `"ARCANE_CAPTURE_PHYSICS_BASELINE"` to `kEnvNames`. The guard fails on any `ARCANE_*` token under `Source/` or `Tests/ArcaneTests/` that is not on that list.
- Test: the compare case in that cpp. The capture case is hidden (`[.]`).

`BuildV1PhysicsScene` and `WriteV1StepTrace` are declared in the helper header and defined in the test cpp (or inline in the header). They are not in an anonymous namespace. `ArtifactReaderTest.cpp` keeps its cook helpers TU-local; that is why a later cpp cannot call them. Task 6 and Task 8 include `Helpers/PhysicsV1Baseline.hpp`. Do not copy the functions into those files.

**Interfaces:**
- Consumes: `Arcane::Runtime` constructed with `Arcane::Test::Process()`; `EnsurePhysics()` (`Runtime.cpp:628`); `Loop().Advance`; `PhysicsWorld2D::StepEvents()` (`Physics2DWorld.hpp:76`); `Crypto::Sha256Hex(std::string_view)` (`Crypto.hpp:285`, lowercase 64 hex chars, no newline); `RepoRoot()` (`Helpers/ConstantScan.hpp:34`).
- Produces: the on-disk trace format below. Tasks 4 and 8 compare those exact bytes. Do not add a second writer later.

The file is UTF-8, no BOM, `\n` line endings, one trailing newline after the last record. Floats are raw IEEE-754 bits printed as 8 lowercase hex digits. Entity order is ascending `(index, generation)`. Do not add an `Identity` component (guids stay nil and print as `0`). `physics.parallelSolver` stays at its default `false` (`Physics2DBinding.hpp:33`). Assert `Settings<SimSettings>().fixedHz == 60` before the loop so a later default change fails the capture instead of writing a different step count. Each `Advance(1.0/60.0, …)` is one fixed step.

```
arcane-physics-v1-step-trace 1
steps 120
parallelSolver 0
step N
bodies K
<index> <generation> <posx hex8> <posy hex8> <angle hex8>
contactBegin N
<entityIndex> <entityGen> <guid> <fixture> <entityIndex> <entityGen> <guid> <fixture>
contactEnd N
<same eight fields>
contactHit N
<eight side fields> <pointx hex8> <pointy hex8> <normalx hex8> <normaly hex8> <approach hex8>
sensorBegin N
<entityIndex> <entityGen> <guid> <fixture> <entityIndex> <entityGen> <guid> <fixture>
sensorEnd N
<same eight fields>
```

`guid` is `0` when `Guid::IsNil()`, otherwise `ToString()` with the dashes removed (32 lowercase hex digits). Event spans are written in vector order: `contactBegin`, `contactEnd`, `contactHit`, `sensorBegin`, `sensorEnd` (`PhysicsEvents2D.hpp:26-33`).

Scene, created in this order so entity ids are stable. No `PhysicsBodyRef2D` (the system mints it). No sprite, no mesh, no `Identity`.

- Floor: `Transform.position = {0, -1, 0}`, `RigidBody2D.type = Static`, one `ShapeKind2D::Aabb` fixture `halfW = 5`, `halfH = 0.5`, `isSensor = false`, `contactEvents = true`, `sensorEvents = false`, `hitEvents = true`.
- Circle: position `{0, 2, 0}`, `Dynamic`, `mass = 1`, `ShapeKind2D::Circle`, `radius = 0.5`, `contactEvents = true`, `hitEvents = true`, `sensorEvents = false`.
- Capsule: position `{1.2, 3, 0}`, `Dynamic`, `mass = 1`, `ShapeKind2D::Capsule`, `radius = 0.2`, `halfLen = 0.4`, same event flags as the circle.
- Box: position `{-0.8, 2.5, 0}`, `Dynamic`, `mass = 1`, `ShapeKind2D::Aabb`, `halfW = 0.3`, `halfH = 0.25`, same event flags as the circle.
- Sensor: position `{0, 0.5, 0}`, `Static`, `ShapeKind2D::Circle`, `radius = 1`, `isSensor = true`, `sensorEvents = true`, `contactEvents = false`, `hitEvents = false`.

`RigidBody2D::type` defaults to `Kinematic` (`PhysicsComponents.hpp:51`). Set `Static` or `Dynamic` explicitly. Default fixture `contactEvents` and `sensorEvents` are both `true` (`PhysicsComponents.hpp:80-81`); set the sensor's flags explicitly.

- [ ] **Step 1: Write the failing compare test**

Declare `BuildV1PhysicsScene(Arcane::Registry&)` and `WriteV1StepTrace(Arcane::Runtime&, int steps)` in `Helpers/PhysicsV1Baseline.hpp`. Define them in the test cpp. The compare case reads the checked-in file and does not write it.

```cpp
TEST_CASE("v1 step trace matches the checked-in bytes", "[physics][baseline]")
{
    const auto path = Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/physics/v1-step-trace.txt";
    const auto hashPath = Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/physics/v1-step-trace.sha256";
    REQUIRE(std::filesystem::exists(path));
    const std::string disk = ReadFileBinary(path);
    std::string expect = ReadFileBinary(hashPath);
    if (!expect.empty() && expect.back() == '\n') expect.pop_back();
    CHECK(Arcane::Crypto::Sha256Hex(disk) == expect);

    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(*rt.Registry());
    rt.EnsurePhysics();
    const std::string live = WriteV1StepTrace(rt, 120);
    CHECK(live == disk);
}
```

`ReadFileBinary` reads the file as bytes and keeps the trailing newline. `WriteV1StepTrace` runs 120 times `rt.Loop().Advance(1.0/60.0, [](double){}, [](double, double){})`, and after each advance appends that step's block from `GetResource<PhysicsWorld2D>()->StepEvents()` and from a view of `RigidBody2D` + `Transform` sorted by `(entity.Index(), entity.Generation())`. Position x/y come from `Transform::position`. Angle comes from `RotationZ` of `Transform::rotation` (the same Z the pass writes back). Do not read the solver from this test: the baseline is the authored transform after the pass, which is what the pass stored.

- [ ] **Step 2: Build the new test, then run it and confirm the assertion fails**

Desk build from Global Constraints. The test calls only symbols that exist today (`Runtime`, `EnsurePhysics`, `Sha256Hex`), so the build's `ExitCode` is 0. A compile error here means the helper header was not picked up by the glob; fix that before calling it a red test.

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "v1 step trace matches the checked-in bytes" --rng-seed 1
```

Expected: assertion failure. `REQUIRE(std::filesystem::exists(path))` fails because `v1-step-trace.txt` does not exist. Record the seed banner. This is not a compile error.

- [ ] **Step 3: Add the hidden capture case and write the bytes**

```cpp
TEST_CASE("capture v1 step trace", "[.][physics-baseline]")
{
    const char* arm = std::getenv("ARCANE_CAPTURE_PHYSICS_BASELINE");
    if (!arm || arm[0] != '1') return;
    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(*rt.Registry());
    rt.EnsurePhysics();
    const std::string live = WriteV1StepTrace(rt, 120);
    const auto dir = Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/physics";
    std::filesystem::create_directories(dir);
    WriteFileBinary(dir / "v1-step-trace.txt", live);
    WriteFileBinary(dir / "v1-step-trace.sha256", Arcane::Crypto::Sha256Hex(live) + "\n");
}
```

`[.]` hides it from `~[gpu]` and from tag filters. An explicit name still runs it. Run:

```powershell
$env:ARCANE_CAPTURE_PHYSICS_BASELINE = '1'
.\ArcaneTests.exe "capture v1 step trace" --rng-seed 1
Remove-Item Env:ARCANE_CAPTURE_PHYSICS_BASELINE
```

Expected: PASS, and both files exist. Open the trace. Header is the three lines above. `steps 120`. `parallelSolver 0`. 120 `step` blocks. The floor's y bit pattern does not change across the 120 steps (a static body). At least one `contactBegin` count is non-zero before step 120 (the circle reaches the floor). If the circle never contacts, stop: the scene is wrong; do not paper over it by dropping events from the file.

- [ ] **Step 4: Re-run the compare test**

```powershell
.\ArcaneTests.exe "v1 step trace matches the checked-in bytes" --rng-seed 1
```

Expected: PASS. Then the suite gate from Global Constraints. Expected: the script exits 0.

- [ ] **Step 5: Commit**

```
git add Tests/ArcaneTests/src/Helpers/PhysicsV1Baseline.hpp Tests/ArcaneTests/src/PhysicsV1BaselineTest.cpp Tests/ArcaneTests/src/MacroPrefixGuardTest.cpp Tests/ArcaneTests/data/physics/v1-step-trace.txt Tests/ArcaneTests/data/physics/v1-step-trace.sha256
git commit -m "test: check in the v1 physics step trace"
```

with the two trailer lines from Global Constraints.

---

### Task 2: Debug, inspector, and pick snapshots

**Files:**
- Modify: `Tests/ArcaneTests/src/Helpers/PhysicsV1Baseline.hpp` (declare the debug and pick dumpers Task 6 calls)
- Modify: `Tests/ArcaneTests/src/PhysicsV1BaselineTest.cpp`
- Create: `Tests/ArcaneTests/data/physics/v1-debug-wholeworld.txt` + `.sha256`
- Create: `Tests/ArcaneTests/data/physics/v1-debug-selected.txt` + `.sha256`
- Create: `Tests/ArcaneTests/data/physics/v1-debug-altview.txt` + `.sha256`
- Create: `Tests/ArcaneTests/data/physics/v1-inspector.txt` + `.sha256`
- Create: `Tests/ArcaneTests/data/physics/v1-pick.txt` + `.sha256`

**Interfaces:**
- Consumes: `GatherPhysicsDebug` and `GatherNarrowphaseWorldOverlay` (`PhysicsDebugGeometry.hpp:96-117`). Both take Manifold types. This test is allowed to include `Physics2DDetail.hpp` and `PhysicsDebugDraw.hpp`. `PhysicsDebugPrimitive` is lines 78–84 (`Kind::Line` or `Circle`, `a`, `b`, `size`, `color`). `DebugCollide` (`PhysicsWorld.hpp:944-945`) writes the trace into `NarrowphaseTrace& out` and returns a `Manifold`. `CollectPickables` appends `PickDrawable` (`PickEmit.hpp:47-71`) and does not clear the vector (`PickEmit.hpp:73-75`). `Affine2D` is `offset` + `scale` (`ViewTransform.hpp:39-48`).
- Produces: five text files plus hashes. Task 7's extract request must reproduce the three debug files and the inspector file. The pick file must match the collider pass after the proxy lands.

Ordinary affine: `offset = {640, 360}`, `scale = {100, -100}` (the reference camera, `pixelsPerMeter = 100`, `+Y` up on a y-down canvas), `alpha = 0`. Alt affine: `offset = {100, 80}`, `scale = {50, -40}`, `alpha = 0.25`. Viewport passed to `Begin` is `{1280, 720}` for both. `OverlayBatchCollector::Begin` is empty today; the request still carries the viewport.

Dump one primitive per line, float bits as 8 lowercase hex:

```
line <ax> <ay> <bx> <by> <size> <r> <g> <b> <a>
circle <cx> <cy> <radius> <r> <g> <b> <a>
```

Header line 1 names the request so the files cannot be swapped:

```
arcane-physics-v1-debug 1 wholeworld alpha 0 offset 640 360 scale 100 -100
arcane-physics-v1-debug 1 selected alpha 0 offset 640 360 scale 100 -100
arcane-physics-v1-debug 1 altview alpha 0.25 offset 100 80 scale 50 -40
arcane-physics-v1-inspector 1 step -1 emphasis 1 thickness none fixtureA <hex16> fixtureB <hex16>
arcane-physics-v1-pick 1
```

`fixtureA` / `fixtureB` are the packed fixture handles actually passed to `DebugCollide` (low 32 bits generation, high 32 bits index, same shift as `PackBody`). The inspector is taken after the 120 steps, on fixture 0 of the floor body and fixture 0 of the circle body. `stepIndex = -1`, `emphasis = 1.0f`, `lineThickness = std::nullopt`. If either body has no fixture, the capture fails; do not pick a different pair.

Pick dump, one drawable per line, after the same 120 steps, on a scene with no sprites (so the list is the collider pass). `entity` index and generation, then the fields `PickDrawable` uses for that kind:

```
circle <index> <gen> <cx> <cy> <cz> <radius> <angle>
capsule <index> <gen> <cx> <cy> <cz> <halfLen> <radius> <angle>
box <index> <gen> <cx> <cy> <cz> <hx> <hy> <angle>
```

All floats are hex8 bits. A `Quad` or `Mesh` line fails the capture: this scene has neither.

- [ ] **Step 1: Write the failing compare cases**

Five `TEST_CASE`s, names:

- `"v1 whole-world debug matches the checked-in bytes"`
- `"v1 selected-only debug matches the checked-in bytes"`
- `"v1 alt-view debug matches the checked-in bytes"`
- `"v1 inspector overlay matches the checked-in bytes"`
- `"v1 collider pick matches the checked-in bytes"`

Each requires the txt and the sha256, checks `Sha256Hex`, rebuilds the scene, steps 120, and compares the live dump to the disk bytes. Selected-only uses the circle entity (the second entity created), `playMode` false, `physicsWholeWorld` false, which is the gate at `ExtractFrame.cpp:433-462`: outside play, a resolved selected body, contacts off. Whole-world sets contacts on and no selected body. Alt-view is whole-world with the alt affine and alpha 0.25.

- [ ] **Step 2: Build, then run one compare case**

The new cases are edits to a TU Task 1 already built. Desk build anyway (the vcxproj already lists the cpp; the build compiles the new cases). Expected: `ExitCode` 0. The cases call `GatherPhysicsDebug`, which exists today.

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "v1 whole-world debug matches the checked-in bytes" --rng-seed 1
```

Expected: assertion failure. `exists` fails because `v1-debug-wholeworld.txt` does not exist. This is not a compile error.

- [ ] **Step 3: Hidden capture, then confirm the alt file differs**

Add `TEST_CASE("capture v1 debug and pick snapshots", "[.][physics-baseline]")` guarded by `ARCANE_CAPTURE_PHYSICS_BASELINE=1`, writing all five files and their `.sha256` siblings (hex plus one newline). Run it the same way as Task 1's capture.

Then, in the test cpp, a permanent check inside the alt-view compare case:

```cpp
const auto whole = ReadFileBinary(dir / "v1-debug-wholeworld.txt");
const auto alt = ReadFileBinary(dir / "v1-debug-altview.txt");
CHECK(whole != alt);
```

Expected after capture: the two files differ. If they are equal, the gather ignored affine or alpha. Stop and fix the capture inputs. Do not check in two identical files.

Also confirm `v1-debug-selected.txt` differs from `v1-debug-wholeworld.txt` (contacts and the only-body filter change the list). If they match, stop.

- [ ] **Step 4: Run the five compare cases and the suite gate**

```powershell
.\ArcaneTests.exe "v1 * matches the checked-in bytes" --rng-seed 1
```

Expected: PASS for all five, plus the Task 1 trace case if the filter includes it. Use the explicit names if the wildcard does not. Then `run-tests.ps1 Debug`. Expected: exit 0.

- [ ] **Step 5: Commit**

```
git add Tests/ArcaneTests/src/Helpers/PhysicsV1Baseline.hpp Tests/ArcaneTests/src/PhysicsV1BaselineTest.cpp Tests/ArcaneTests/data/physics/
git commit -m "test: check in v1 physics debug and pick snapshots"
```

with the trailer.

---

### Task 3: v1 `.arcart` fixtures

**Files:**
- Create: `Tests/ArcaneTests/src/ArtifactV1FixtureTest.cpp`
- Create: `Tests/ArcaneTests/data/artifacts/v1-texture.arcart` + `.sha256`
- Create: `Tests/ArcaneTests/data/artifacts/v1-mesh.arcart` + `.sha256`

**Interfaces:**
- Consumes: `CookFixture` is local to `ArtifactReaderTest.cpp` (lines 113–127) and is not linkable. Duplicate the call shape in this test: `EncodePng` + `GradientPixels` + `ImportTexture` + `WriteTextureArtifact`, `DefaultRgba8Settings()` (`ArtifactReaderTest.cpp:106-111`), size 17×9. Do not call `Guid::Generate()` (`ArtifactReaderTest.cpp:135` is why a temp cook is not a baseline). Mesh writer is `WriteMeshArtifact`. The round-trip values are `MeshArtifactFormatTest.cpp:120-141`, **not** the hand-built header helper at lines 100–102. Those aabb helpers (`-1,-2,-3` / `1,2,3`) are a different function. The fixture uses the test's values.
- Produces: two artifact files and two sha256 siblings. Plan B compares a fresh write to these bytes. This task does not move the codec.

Fixed ids, both `constexpr`, neither generated:

```cpp
constexpr Arcane::Guid kV1TextureGuid{0x1111111122224333ull, 0x8444555555555555ull};
constexpr Arcane::Guid kV1MeshGuid{0xAAAAAAAABBBBCCCCull, 0xDDDDEEEEFFFF0000ull};
```

Mesh desc, copied from `MeshArtifactFormatTest.cpp:120-141` with the guid frozen:

```cpp
desc.contentKind = ContentKind::Mesh;
desc.sourceGuid = kV1MeshGuid;
desc.sourceHash = 0x0123456789ABCDEFULL;
desc.importerVersion = 3;
desc.vertexCount = 4;
desc.indexCount = 9;
desc.sectionCount = 2;
desc.indexWidth = 4;
desc.aabbMin[0] = -1.5f;  desc.aabbMin[1] = -2.25f; desc.aabbMin[2] = -3.125f;
desc.aabbMax[0] =  4.5f;  desc.aabbMax[1] =  5.25f; desc.aabbMax[2] =  6.125f;
desc.sections = { { "Metal", 0, 6, 0 }, { "Metal", 6, 3, 0 } };
const std::vector<MeshArtifactVertex> vertices = {
    { 0,0,0,  0,1,0,  0,0 }, { 1,0,0,  0,1,0,  1,0 },
    { 1,0,1,  0,1,0,  1,1 }, { 0,0,1,  0,1,0,  0,1 },
};
const std::vector<std::uint32_t> indices = { 0,1,2, 0,2,3, 1,2,3 };
```

Texture: `ImportTexture(EncodePng(17, 9, GradientPixels(17, 9)), kV1TextureGuid, DefaultRgba8Settings())`, then `WriteTextureArtifact` of `desc`, `payload`, `thumbRgba`. Copy `EncodePng`, `GradientPixels`, and `DefaultRgba8Settings` into this test as file-local functions so the fixture does not depend on an anonymous namespace in another TU. Keep them byte-identical to `ArtifactReaderTest.cpp`.

- [ ] **Step 1: Write the failing compare test**

```cpp
TEST_CASE("v1 texture and mesh artifacts match the checked-in bytes", "[artifact][baseline]")
{
    const auto root = Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/artifacts";
    for (const char* name : { "v1-texture.arcart", "v1-mesh.arcart" })
    {
        const auto path = root / name;
        REQUIRE(std::filesystem::exists(path));
        const std::string disk = ReadFileBinary(path);
        std::string expect = ReadFileBinary(root / (std::string(name) + ".sha256"));
        if (!expect.empty() && expect.back() == '\n') expect.pop_back();
        CHECK(Arcane::Crypto::Sha256Hex(disk) == expect);
        const auto fresh = (std::string(name) == "v1-texture.arcart")
            ? WriteV1TextureToTemp() : WriteV1MeshToTemp();
        CHECK(fresh == disk);
    }
}
```

`WriteV1TextureToTemp` / `WriteV1MeshToTemp` write to a temp directory and return the bytes. They use the fixed guids above.

- [ ] **Step 2: Build the new test, then run it**

Desk build. `ArtifactV1FixtureTest.cpp` is a new TU, so the generate inside the desk build is what adds it. Expected: `ExitCode` 0. The test calls `WriteTextureArtifact` / `WriteMeshArtifact`, which exist. A compile error means a helper copied from `ArtifactReaderTest.cpp` was left in that file's anonymous namespace and is not the duplicate this task told you to paste.

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "v1 texture and mesh artifacts match the checked-in bytes" --rng-seed 1
```

Expected: assertion failure. `exists` fails because `v1-texture.arcart` does not exist. This is not a compile error.

- [ ] **Step 3: Hidden capture**

`TEST_CASE("capture v1 artifacts", "[.][artifact-baseline]")` with `ARCANE_CAPTURE_PHYSICS_BASELINE=1` (reuse the same env so one switch arms every baseline capture). Write the four files. Run the capture, then the compare. Expected: compare PASS. A second capture run writes identical bytes. If it does not, a `Guid::Generate()` call is still in the path. Remove it.

- [ ] **Step 4: Suite gate**

`run-tests.ps1 Debug`. Expected: exit 0. The existing temp-dir round trips in `ArtifactReaderTest.cpp` and `MeshArtifactFormatTest.cpp` stay. They are not the baseline.

- [ ] **Step 5: Commit**

```
git add Tests/ArcaneTests/src/ArtifactV1FixtureTest.cpp Tests/ArcaneTests/data/artifacts/
git commit -m "test: check in v1 texture and mesh artifacts"
```

with the trailer.

---

### Task 4: Sim body in one translation unit

**Prerequisite of this task:** Plan B Tasks 1 and 2 are merged. `build/abi-digest.txt` and `kGamePluginABIDigest` exist. `PhysicsSystem.hpp` still holds the inline `operator()`. `PhysicsBodyRef2D::handle` is still `Detail::Physics2D::BodyHandle`. Do not start this task in a tree that has no digest file.

**Files:**
- Create: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem.cpp`
- Create: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem2D.hpp`
- Delete: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem.hpp` after every includer has moved
- Modify: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DWorld.hpp:4-6` and `:106-121`
- Modify: `Source/Runtime/ArcaneSim/src/Arcane/Sim/SystemSchedulers.hpp:8-9`
- Modify: `Source/Programs/ArcaneServer/src/ServerApp.cpp:10` to include `<Arcane/Scene/PhysicsSystem2D.hpp>`
- Modify: `Tests/ArcaneTests/src/ArcaneSpellingGuardTest.cpp:50` (drop `PhysicsSystem.hpp`, add the Physics2D path of `PhysicsSystem2D.hpp`, count stays 21)
- Modify: every TU that includes `PhysicsSystem.hpp` (`Runtime.cpp` line 26's region, `ExtractFrame.cpp:21`, `EngineResourceTypes.cpp:3`, and the tests the compiler lists). They include `Physics2DWorld.hpp` or `PhysicsSystem2D.hpp` instead. They do not include the pass body.
- Test: `PhysicsV1BaselineTest.cpp` compare case, unchanged.

**Interfaces:**
- Consumes: the inline body at `PhysicsSystem.hpp:19-433`.
- Produces: `void PhysicsSystem2D::operator()(Arcane::Registry& reg)` defined in `PhysicsSystem.cpp` and exported with `ARC_PHYSICS2D_API`. The declaration moves to `PhysicsSystem2D.hpp`, which stays in ArcanePhysics2D for this task. `ArcaneScene.arcmodule` lists ArcaneAssets, ArcaneCore, and ArcaneSim, and not ArcanePhysics2D. `PhysicsComponents.hpp` still includes `Physics2DHandles.hpp` (`PhysicsComponents.hpp:9`), which includes Manifold. A Scene header that includes that component header does not compile on Scene's include path and would be a Physics2D dependency Scene does not have. Task 5 moves `PhysicsSystem2D.hpp` to ArcaneScene after the component header is solver-free. `Physics2DWorld.hpp` includes `PhysicsSystem2D.hpp` and keeps `friend struct PhysicsSystem2D`. Class layout stays `m_fixedDt` then `m_stepWorld` (`Physics2DWorld.hpp:119-120`).

`PhysicsSystem2D.hpp` contains only the struct currently at `Physics2DWorld.hpp:106-121`, plus the includes that struct needs (`SystemTraits`, `TransformPropagationSystem`, `Collider2D`, `PhysicsBodyRef2D`, `RigidBody2D`, `Transform`). Those component types live in the physics component header today. Include `<Arcane/Scene/PhysicsComponents.hpp>` and `<Arcane/Scene/TransformSystems.hpp>`. Do not include `Physics2DDetail.hpp`, `Physics2DHandles.hpp`, or any `Manifold2D/` header directly. The transitive include through `PhysicsComponents.hpp` is why this header is not in Scene yet.

Until Task 8, `Runtime.cpp` may still call the exported `operator()` (`PhysicsEditPass`, `Runtime.cpp:657-661`) and `AddSystem<PhysicsSystem2D>`. It must not include the pass body.

The full include scan of Engine/Render/Client/Editor/Server is Task 8. `ExtractFrame.cpp` still includes `Physics2DDetail.hpp` until Task 7. Do not fail Task 4 for that.

- [ ] **Step 1: Extend the trace compare so a header include of the pass body fails**

Add to `PhysicsV1BaselineTest.cpp`:

```cpp
TEST_CASE("PhysicsSystem.hpp is gone and the pass body is not a header", "[physics][baseline]")
{
    const auto root = Arcane::Test::RepoRoot();
    CHECK_FALSE(std::filesystem::exists(root / "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem.hpp"));
    const std::string cpp = ReadFileBinary(root / "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem.cpp");
    CHECK(cpp.find("PhysicsSystem2D::operator()") != std::string::npos);
    CHECK(cpp.find("Physics2DDetail.hpp") != std::string::npos);
}
```

- [ ] **Step 2: Build, then run it**

The case is an edit to `PhysicsV1BaselineTest.cpp`, which is already in the vcxproj. Desk build so the new case is in the exe. Expected: `ExitCode` 0. The case only calls `filesystem::exists` and reads a file.

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "PhysicsSystem.hpp is gone and the pass body is not a header" --rng-seed 1
```

Expected: assertion failure. `CHECK_FALSE(exists(PhysicsSystem.hpp))` fails because the header still exists. This is not a compile error.

- [ ] **Step 3: Move the body**

Create `PhysicsSystem.cpp`. Paste the function from `PhysicsSystem.hpp:19-433` with these edits and no others:

- Drop `inline`.
- Add `ARC_PHYSICS2D_API` on the definition.
- Includes: `Physics2DWorld.hpp`, `Physics2DDetail.hpp`, and the headers the body already used (`glm`, `cmath`, `utility`, `vector`). Do not include `PhysicsSystem.hpp`.

Create `PhysicsSystem2D.hpp` with the struct. Delete the struct from `Physics2DWorld.hpp` and include the new header instead. Rewrite `Physics2DWorld.hpp:4-6` so it says the pass body is `PhysicsSystem.cpp` and the declaration is `PhysicsSystem2D.hpp`.

Rewrite `SystemSchedulers.hpp:8-9` to: the schedulers stay header-only because they are a thin Astra wrapper and include no solver; `AttachAllModules` (`ModuleAttach.hpp:5-15`) is what puts every module image on one `TypeContext`. Delete the sentence that a registry must be touched by exactly one module.

Rewrite the comment you delete with `PhysicsSystem.hpp` (lines 3–6) into a two-line comment at the top of `PhysicsSystem.cpp`: the body used to be header-only under the pre-attach TypeID rule; `AttachAllModules` makes one translation unit enough.

Update `ArcaneSpellingGuardTest.cpp`'s `kGameFacingHeaders` (the comment on line 12 says 21). Replace the `PhysicsSystem.hpp` entry with `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem2D.hpp`. Do not leave a 22nd entry. Task 5 changes that path when the header moves to Scene.

Point `ServerApp.cpp:10`, `EngineResourceTypes.cpp:3`, `ExtractFrame.cpp:21`, and `Runtime.cpp`'s physics-system include at `PhysicsSystem2D.hpp` or `Physics2DWorld.hpp`. `EngineResourceTypes.cpp` only needs `PhysicsWorld2D` for `TypeID` at line 21; include `Physics2DWorld.hpp` until Task 8 moves that touch. Do not include `Physics2DDetail.hpp` from `EngineResourceTypes.cpp`.

Do not change any statement inside `operator()`. L09 stays as it is.

If `AbiManifest.hpp` includes `PhysicsSystem.hpp`, point that include at `PhysicsSystem2D.hpp`. Do not edit an `offsetof` line. The struct stays `m_fixedDt` then `m_stepWorld`, so the mix does not change. Do not `--write`. Do not change `kGamePluginABIDigest`.

- [ ] **Step 4: Build, trace, suite**

Desk build from Global Constraints. Expected: exit 0. Plan B's `build.ps1` runs `arcane-abi-digest --check` after the engine build, so this exit 0 already means the checked-in digest matches the post-move mix. Run the tool once more and require exit 0. Do not `--write`. If `--check` exits non-zero, the move changed a recorded layout. Stop. Do not update the digest in this task.

```powershell
D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\arcane-abi-digest\arcane-abi-digest.exe --check
```

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "v1 step trace matches the checked-in bytes" --rng-seed 1
.\ArcaneTests.exe "PhysicsSystem.hpp is gone and the pass body is not a header" --rng-seed 1
```

Expected: both PASS. The trace bytes are unchanged. Then, from the exe directory, `powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug`. Expected: exit 0. Do not run the golden gate.

Run this exact Servitor sequence once on an unmodified build of the arc's base commit and record the exit codes. `compare runtime-scene` after `wait settled` reads the held frame 0. Fix-round Tasks 18-19 change what frame 0 presents. If the base tree already exits 8, that is pre-existing and not this task's regression. Report it and do not bless.

Servitor, own headless runtime. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way. Record each exit code. Quit even if a later command fails.

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-a --host runtime wait settled
& $S --project ReferenceProject --client plan-a --host runtime step 30
& $S --project ReferenceProject --client plan-a --host runtime resource list
& $S --project ReferenceProject --client plan-a --host runtime quit
```

Expected: every exit 0. `resource list`'s `resources` array contains `Arcane::PhysicsWorld2D`. An empty array fails the task. The moved `operator()` is what minted that world.

- [ ] **Step 5: Commit**

```
git add -A Source/Runtime/ArcanePhysics2D Source/Runtime/ArcaneSim/src/Arcane/Sim/SystemSchedulers.hpp Source/Programs/ArcaneServer Tests/ArcaneTests/src/ArcaneSpellingGuardTest.cpp Source/Runtime/ArcaneEngine Source/Client/ArcaneRender Source/Runtime/ArcaneCore/src/Arcane/Core/AbiManifest.hpp
git commit -m "refactor: move PhysicsSystem2D's pass body into one translation unit"
```

with the trailer. `git add -A` on those trees only. Do not stage unrelated files. If a test TU was the only other includer, stage it too.

---

### Task 5: Solver-free component header and uint64 handle

**Prerequisite of this task:** Plan B Tasks 1 and 2 are merged, and Task 4 has landed with `arcane-abi-digest --check` exiting 0. `build/abi-digest.txt` and `kGamePluginABIDigest` exist and still describe the pre-`uint64` layout. `operator()` is already in `PhysicsSystem.cpp`. `PhysicsBodyRef2D::handle` is still `Detail::Physics2D::BodyHandle` (`PhysicsComponents.hpp:101`). Do not start this task in a tree that has no digest file.

**Files:**
- Create: `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsComponents.hpp` (move, same include string). This header also specializes `Astra::SerializationTraits<Arcane::PhysicsBodyRef2D>`.
- Delete: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsComponents.hpp`
- Move: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsSystem2D.hpp` to `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsSystem2D.hpp`, after the component header no longer includes a solver header. Retarget the spelling-guard path from the Physics2D copy to this Scene path.
- Modify: `PhysicsSystem.cpp` (the comparisons listed below)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp:200-205`
- Modify: `Tests/ArcaneTests/src/PhysicsComponentsTest.cpp` (the store at line 125 and the load checks at lines 164–172)
- Modify: `Tests/ArcaneTests/src/ArcaneSpellingGuardTest.cpp` path, now under `ArcaneScene` for both `PhysicsComponents.hpp` and `PhysicsSystem2D.hpp`
- Modify: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DDetail.hpp` add `UnpackBody` beside `PackBody` (lines 105–108)
- Modify: `kGamePluginABIDigest` and `build/abi-digest.txt` via `arcane-abi-digest --write`. Do not change `kGamePluginABIVersion`.
- Create: `Tests/ArcaneTests/data/physics/v1-bodyref-save.bin` (captured in Step 1, before the field change)
- Test: `Tests/ArcaneTests/src/PhysicsHandlePackTest.cpp` (new)

**Interfaces:**
- Consumes: `PackBody` (`Physics2DDetail.hpp:105-108`): `(uint64(h.index) << 32) | h.generation`. Today's `Registry::Save` writes a trivially copyable component whole (`BinaryWriter.hpp:507-510`) and `Load` reads it whole (`BinaryReader.hpp:660-662`). `Serializable` does not affect that path (`Attribute.hpp:240`). JSON `SaveJson` (`SceneSerializer.hpp:124`) is the walk that skips `!IsSerializable()`. Both `handle` and `appliedScale` are already `Serializable false` and `Hidden` (`PhysicsComponents.hpp:140-146`). `PhysicsComponentsTest.cpp:164-172` expects `{7, 3}` to survive, and that matches the code. Default `SerializationTraits` (`BinaryArchive.hpp:231-242`) has `HasCustomSerializer = false`, `Version = 1`, `MinVersion = 1`. The version integer is written before the blob (`BinaryWriter.hpp:486`). A mismatch goes to `Migrate` (`BinaryReader.hpp:670-675`). There is no traits specialization under `Source/`. `TransientResourceTraits` (`Component.hpp:93-104`) is for resources. Do not use it for this component. Do not edit `ThirdParty/Astra`.
- Produces: `PhysicsBodyRef2D::handle` is `std::uint64_t`, default `0`. The component header includes `<Arcane/Ecs.hpp>`, `<Arcane/Reflection.hpp>`, `<glm/vec2.hpp>`, `<cstdint>`, `<vector>`, and the Astra header that declares `SerializationTraits` and `BinaryReader`. It does not include `Physics2DHandles.hpp` or any Manifold header. `RegisterPhysicsComponents2D` stays, same two overloads (`PhysicsComponents.hpp:149-158`), same order: `RigidBody2D`, `Collider2D`, `PhysicsBodyRef2D`. `PhysicsSystem2D.hpp` lives in ArcaneScene and includes this header.

```cpp
namespace Arcane::Detail::Physics2D
{
    [[nodiscard]] constexpr Phys::BodyHandle UnpackBody(std::uint64_t packed) noexcept
    {
        Phys::BodyHandle h;
        h.index = static_cast<std::uint32_t>(packed >> 32);
        h.generation = static_cast<std::uint32_t>(packed);
        return h;
    }
}
```

`UnpackBody` stays in the physics detail header. The component header does not declare it.

Reflection of `handle` stays `Serializable false` and `Hidden` (`PhysicsComponents.hpp:140-146`). `appliedScale` stays `Serializable false` and `Hidden`. Those attributes are the JSON walk. They are not the binary gate.

Specialize `Astra::SerializationTraits<Arcane::PhysicsBodyRef2D>` in the component header, in namespace `Astra`, so every TU that includes the component sees it before `Save` or `Load`. `HasCustomSerializer = true`. `Version = 2`. `MinVersion = 1`. `Serialize` writes only `appliedScale.x` and `appliedScale.y` as two floats. On load it sets `handle = 0` and then reads those two floats. `Migrate`, for `storedVersion == 1`, reads this explicit old layout and nothing else:

```cpp
struct PhysicsBodyRef2DDiskV1
{
    std::uint32_t index;
    std::uint32_t generation;
    float scaleX;
    float scaleY;
};
static_assert(sizeof(PhysicsBodyRef2DDiskV1) == 16);
static_assert(alignof(PhysicsBodyRef2DDiskV1) == 4);
```

`Migrate` keeps `scaleX` / `scaleY` as `appliedScale` and sets `handle = 0`. It does not `memcpy` `sizeof(PhysicsBodyRef2D)`: that size is the new in-memory layout and is not the version-1 disk form. The version-2 disk form is the two floats, not a memcpy of the new struct. Do not hardcode padding into the checked-in blob. Capture the bytes `Registry::Save` actually writes.

`Migrate` handles only `storedVersion == 1`. Do not promise `UnsupportedVersion` for every other version, and do not edit `ThirdParty/`. Astra calls `Migrate` for every `storedVersion` that is `>= MinVersion` and not equal to `Version`, and it discards any return value (`BinaryReader.hpp:673-675`). `m_error` is private (`BinaryReader.hpp:854`). `HasError` and `GetError` are const (`:810-811`). There is no setter.

What the code can actually return:

- `storedVersion < MinVersion` (`MinVersion` stays 1): Astra sets `UnsupportedVersion` and does not call `Migrate` (`BinaryReader.hpp:633-637`).
- `storedVersion == 1`: `Migrate` reads `PhysicsBodyRef2DDiskV1`.
- `storedVersion == 2`: `Serialize` on the reader sets `handle = 0` and reads the two floats.
- `storedVersion > 2`, which is the only other value that reaches `Migrate`: do not read the v1 layout. Call `ReadBytes` with a size of `GetRemaining() + 1` (`GetRemaining` is `BinaryReader.hpp:802-805`). The size check sets `SerializationError::CorruptedData` and returns before the copy (`BinaryReader.hpp:103-106`), so pass a one-byte sink. `ReadVersionedComponent` then returns `Err(m_error)` (`:685-687`). Omitting `Migrate` is not this path: without it, version 1 would also be `UnsupportedVersion` (`:677-681`), and the captured blob must load.

`Registry::Load` does not surface either reader code. `Archetype::DeserializeColumn` returns the reader's error (`Archetype.hpp:1655-1657`), then `ArchetypeManager::Deserialize` drops that `Result` and returns `false` (`ArchetypeManager.hpp:1027-1029`), and `Registry::LoadInternal` maps that `false` to `SerializationError::CorruptedData` (`Registry.hpp:1876-1878`). That return is before `VerifyChecksum` (`Registry.hpp:1899`), so a patched byte does not come back as `ChecksumMismatch`. A `Registry::Load` of a patched blob is `Err(CorruptedData)` for both version 0 and version 3. The precise codes are pinned by calling `BinaryReader::ReadVersionedComponent` on a buffer this test builds: little-endian `TypeID<PhysicsBodyRef2D>::Hash()`, then a `uint32` version, then 16 `0xFF` bytes. Version 0 is `Err(UnsupportedVersion)`. Version 3 is `Err(CorruptedData)`, `handle` is still 0, `appliedScale` is still `{1, 1}`, and `GetPosition()` is still `sizeof(uint64) + sizeof(uint32)` (the over-read did not consume the payload). Default save compression is `CompressionMode::None` (`BinaryWriter.hpp:52`, `BinaryArchive.hpp:137`), so the captured file's column is plain (`Archetype.hpp:1280-1297` compresses only LZ4). The per-element record is the hash followed by the traits version (`BinaryWriter.hpp:481-487`). The descriptor record is the hash followed by a `uint64` size (`Archetype.hpp:928-931`); today's `sizeof(PhysicsBodyRef2D)` is 16 (`BodyHandle` is 8, `glm::vec2` is 8, both align 4), so the descriptor's next `uint32` is 16, not 1. Require exactly one occurrence of that hash whose next little-endian `uint32` is 1, and patch that `uint32`. Any other count is a stop, not a guess. Load that copy. Expect `Err(CorruptedData)` for a patch to 0 and for a patch to 3. Do not expect `UnsupportedVersion` from `Registry::Load`.

Sites that treat `handle` as `BodyHandle`, verified in this tree. Update every one. Solver calls take `UnpackBody(ref.handle)`. A comparison against `kInvalidBody` becomes `ref.handle == 0`. An assignment of a fresh solver handle becomes `ref.handle = PackBody(handle)`. `PackBody(ref.handle)` becomes `ref.handle` because it is already packed.

- `PhysicsSystem.cpp` after the move, from today's `PhysicsSystem.hpp`: lines 122–124, 138, 223, 316–317, 328, 333, 349–350, 363–366, 388–389, 408, 410, 418.
- `ExtractFrame.cpp:200-205`.
- `PhysicsComponentsTest.cpp:125`: store `Arcane::Detail::Physics2D::PackBody(Manifold2D::Physics::BodyHandle{7u, 3u})`, and set `appliedScale` to `{2.f, 3.f}` before `Save` (the default is `{1, 1}` at `PhysicsComponents.hpp:102`, which would hide a wiped component). Lines 171–172 today `CHECK` `handle.index == 7` and `handle.generation == 3`. Replace those with `handle == 0`, `appliedScale.x == 2`, and `appliedScale.y == 3`. The zero handle is the traits, not the reflection attribute.

Grep `ref.handle` under `Source/` and `Tests/` after the edit. The only remaining `BodyHandle` stores are inside `ArcanePhysics2D` solver maps (`Physics2DWorld.hpp:97-101`), which stay.

`EngineRoster.hpp:23` still includes `<Arcane/Scene/PhysicsComponents.hpp>`. The file now resolves to ArcaneScene. Leave the roster's thirteen-type `using` (lines 34–37) unchanged until Task 8.

Do not bump `kGamePluginABIVersion`.

- [ ] **Step 1: Capture the version-1 blob against today's API**

`PhysicsHandlePackTest.cpp` in this step uses `BodyHandle` and does not call `UnpackBody`. It builds a registry the way `PhysicsComponentsTest.cpp:90-129` does, sets `handle` to `BodyHandle{7u, 3u}` and `appliedScale` to `{2.f, 3.f}`, calls `Registry::Save`, and writes those bytes to `Tests/ArcaneTests/data/physics/v1-bodyref-save.bin`. It then `Load`s that file and `CHECK`s `handle.index == 7`, `handle.generation == 3`, and both scale components. That is today's behaviour (`PhysicsComponentsTest.cpp:171-172`).

Desk build (the new TU is not in the vcxproj until generate). Expected: `ExitCode` 0. Then, from the exe directory:

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "v1 bodyref save captures the whole component" --rng-seed 1
```

Expected: PASS. The blob is the file `Save` wrote. Do not hand-author its bytes. Do not commit yet.

Capture `v1-bodyref-save.bin` in the arc worktree. Do not copy a blob from `Arcane-f5`: `309aa06e` predates Astra `fa5ff853`, whose canonical `TypeID<T>::Hash()` is the hash the per-element record and the descriptor record carry.

- [ ] **Step 2: Rewrite the test, build, and stop on the compile error**

Replace the capture case with the green case below. It calls `UnpackBody` and assigns a `uint64` to `handle`. Desk build. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names `UnpackBody` as undeclared, and the assignment to `handle` fails because the field is still `BodyHandle`. Do not run `ArcaneTests.exe`. That compile error is the red. It is not an assertion failure.

```cpp
TEST_CASE("PhysicsBodyRef2D handle is the packed uint64 and a v1 blob loads as scale only", "[physics][handle]")
{
    using Arcane::Detail::Physics2D::PackBody;
    Manifold2D::Physics::BodyHandle h{7u, 3u};
    const std::uint64_t packed = PackBody(h);
    CHECK(packed == ((7ull << 32) | 3ull));
    const auto back = Arcane::Detail::Physics2D::UnpackBody(packed);
    CHECK(back.index == 7u);
    CHECK(back.generation == 3u);
    CHECK(PackBody(Manifold2D::Physics::BodyHandle{0u, 0u}) == 0u);

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*components);
    Arcane::RegisterPhysicsComponents2D(*components);
    Astra::Registry reg(components);
    const auto e = reg.CreateEntity();
    Arcane::PhysicsBodyRef2D ref;
    ref.handle = packed;
    ref.appliedScale = glm::vec2{2.f, 3.f};
    reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, ref);
    const auto fresh = std::filesystem::temp_directory_path() / "arcane-handle-pack.arcscene";
    REQUIRE(reg.Save(fresh).IsOk());
    auto loaded = Astra::Registry::Load(fresh, components);
    REQUIRE(loaded.IsOk());
    const auto* out = loaded.GetValue()->GetComponent<Arcane::PhysicsBodyRef2D>(e);
    REQUIRE(out != nullptr);
    CHECK(out->handle == 0u);
    CHECK(out->appliedScale.x == 2.f);
    CHECK(out->appliedScale.y == 3.f);

    const auto v1 = Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/physics/v1-bodyref-save.bin";
    auto fromV1 = Astra::Registry::Load(v1, components);
    REQUIRE(fromV1.IsOk());
    // The captured registry has one PhysicsBodyRef2D. Its handle is 0 and its
    // appliedScale is {2, 3}. Find it by walking entities; do not require the
    // old entity id if Save did not keep it.
}
```

Use the same `Save` / `Load` calls `PhysicsComponentsTest.cpp` already uses if the signatures differ from the sketch. The v1 load must `CHECK` `handle == 0` and `appliedScale == {2, 3}`. A fresh save/load must do the same.

- [ ] **Step 3: Move the headers, switch the field, add the traits**

Move `PhysicsComponents.hpp` to ArcaneScene. Delete the Manifold include. Change the field. Add the traits specialization from the Interfaces section in that same header. Add `UnpackBody`. Update the sites listed above, including `PhysicsComponentsTest.cpp:125` and `:171-172`. Move `PhysicsSystem2D.hpp` to ArcaneScene and retarget the spelling-guard path. The system header includes the Scene component header. Do not run `arcane-abi-digest` in this step. The exe already in `Binaries` was compiled against `BodyHandle` and would `--write` the old mix.

Add the source scan and the version-rejection case in the same test file:

```cpp
TEST_CASE("the component header names no Manifold type", "[physics][handle]")
{
    const std::string text = ReadFileBinary(Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsComponents.hpp");
    CHECK(text.find("Manifold2D") == std::string::npos);
    CHECK(text.find("Physics2DHandles.hpp") == std::string::npos);
    CHECK(text.find("std::uint64_t handle") != std::string::npos);
    CHECK(text.find("SerializationTraits") != std::string::npos);
}

TEST_CASE("a bodyref version other than 1 or 2 returns the code the reader can set", "[physics][handle]")
{
    // Direct BinaryReader::ReadVersionedComponent, not Registry::Load.
    // Buffer: TypeID hash, uint32 version, 16 bytes of 0xFF.
    // version 0 -> Err(UnsupportedVersion), Migrate not required for that code.
    // version 3 -> Err(CorruptedData), handle still 0, appliedScale still {1, 1},
    // GetPosition() still sizeof(uint64) + sizeof(uint32).
    // A copy of v1-bodyref-save.bin with that per-element uint32 patched to 0,
    // and another patched to 3, each Registry::Load as Err(CorruptedData).
}
```

- [ ] **Step 4: Rebuild the digest tool, write, then the full build**

The header edit is already on disk. Rebuild only `arcane-abi-digest` so the compiler re-evaluates `AbiManifestMix`. Do not run `scripts\ci\build.ps1` yet: after Plan B, that script runs `--check` after the engine build and exits when the file or `kGamePluginABIDigest` still holds the pre-`uint64` mix. Premake's `location` for this app is `Source/Programs/arcane-abi-digest` (the same pattern as `arcbuild` at `premake5.lua:580`), so the project file is the path below. `Resolve-MSBuild` is exported from `ArcaneCi.psm1`.

```powershell
Import-Module D:\dev\starworks\Arcane-3dr\scripts\ci\ArcaneCi.psm1 -Force
$msbuild = Resolve-MSBuild
$proj = 'D:\dev\starworks\Arcane-3dr\Source\Programs\arcane-abi-digest\arcane-abi-digest.vcxproj'
$tool = Start-Process -FilePath $msbuild -ArgumentList @(
    $proj, '/p:Configuration=Debug', '/m:4', '/nologo', '/v:minimal'
) -WorkingDirectory 'D:\dev\starworks\Arcane-3dr' -PassThru -NoNewWindow
if (-not $tool.HasExited) { $tool.PriorityClass = 'BelowNormal' }
$tool.WaitForExit()
if ($tool.ExitCode -ne 0) { throw "abi digest tool build failed: exit $($tool.ExitCode)" }
```

Then run that new exe. Do not type a hex literal by hand:

```powershell
D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\arcane-abi-digest\arcane-abi-digest.exe --write
```

`--write` rewrites `build/abi-digest.txt` as the post-`uint64` mix. Copy that integer into `kGamePluginABIDigest`. Do not change `kGamePluginABIVersion`. On Linux and macOS, build this same target, then `--write`, then the full `build.sh`. Do not `--write` with the binary from before the header edit.

Desk build from Global Constraints. Expected: `ExitCode` 0. That script's `--check` is part of the exit code. Run it once more and require exit 0:

```powershell
D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\arcane-abi-digest\arcane-abi-digest.exe --check
```

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "PhysicsBodyRef2D handle is the packed uint64 and a v1 blob loads as scale only" --rng-seed 1
.\ArcaneTests.exe "a bodyref version other than 1 or 2 returns the code the reader can set" --rng-seed 1
.\ArcaneTests.exe "v1 step trace matches the checked-in bytes" --rng-seed 1
```

Expected: all three PASS. The pose bits do not move because `UnpackBody(PackBody(h))` is `h`. Version 0 is `UnsupportedVersion` on the direct reader and `CorruptedData` on `Registry::Load`. Version 3 is `CorruptedData` on both. Then:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Expected: exit 0. `PhysicsComponentsTest`'s load assertion (handle 0, scale `{2, 3}`) is part of that gate. Do not run the golden gate.

Servitor, from the repo root. Client name `plan-a`. Quit even if a later command fails. Record every exit code.

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-a --host runtime wait settled
& $S --project ReferenceProject --client plan-a --host runtime step 30
& $S --project ReferenceProject --client plan-a --host runtime resource list
& $S --project ReferenceProject --client plan-a --host runtime quit
```

Expected: every exit 0. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way. After `step 30`, `resource list`'s `resources` array contains `Arcane::PhysicsWorld2D`. An empty array is a failure. Do not call `resource get`.

- [ ] **Step 5: Commit**

```
git add Source/Runtime/ArcaneScene Source/Runtime/ArcanePhysics2D Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp Tests/ArcaneTests/src/PhysicsHandlePackTest.cpp Tests/ArcaneTests/src/PhysicsComponentsTest.cpp Tests/ArcaneTests/src/ArcaneSpellingGuardTest.cpp Tests/ArcaneTests/data/physics/v1-bodyref-save.bin build/abi-digest.txt Source/Runtime/ArcaneCore/src/Arcane/Core/AbiVersion.hpp
git commit -m "refactor: make PhysicsBodyRef2D's handle a packed uint64 in a solver-free header"
```

with the trailer. Stage `AbiVersion.hpp` only if that is where `kGamePluginABIDigest` lives. The epoch constant in that file must be unchanged.

---

### Task 6: Solver-free debug and pick proxies

**Files:**
- Create: `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsQuery.hpp`
- Create: `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsQuery.cpp` (the function-pointer slot and the forwarders)
- Create: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsQuery.cpp` (the gather implementation and the registrar)
- Modify: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugGeometry.hpp` (move lines 78–94 out; include `PhysicsQuery.hpp`)
- Test: `Tests/ArcaneTests/src/PhysicsQueryBaselineTest.cpp` (new), including the case "a second 2D query registration is refused"
- Do not modify `ExtractFrame.cpp` in this task.

**Interfaces:**
- Consumes: `PhysicsDebugPrimitive` and `PhysicsDebugGeometry` as they are at `PhysicsDebugGeometry.hpp:78-94` before the move. `GatherPhysicsDebug` / `GatherNarrowphaseWorldOverlay` stay in that header (lines 96–117) and still take Manifold types. `DebugCollide` writes `NarrowphaseTrace&`. `PickDrawable` fields (`PickEmit.hpp:47-71`). Today's extract gate (`ExtractFrame.cpp:433-462`): no primitives when the view has no `Affine2D`; alpha from `RenderContext2D` or 0; `onlyBody` from the selected entity; contacts from `physicsWholeWorld`; draw when whole-world, or when outside play and a body resolved.
- Produces: the header below. No identifier in `PhysicsQuery.hpp` is `Manifold`, `PhysicsWorld`, `NarrowphaseTrace`, `BodyHandle`, or `FixtureHandle`. Packed ids are `uint64`. The return type is `PhysicsDebugGeometry`, the spec's name, after the class moves into this header. Render calls the `ARC_SCENE_API` forwarders. It does not call an `ARC_PHYSICS2D_API` symbol, so Task 7 can drop the module edge and still link.

```cpp
namespace Arcane
{
    struct PhysicsDebugExtractRequest2D
    {
        glm::uvec2 viewport{0u, 0u};
        std::optional<Affine2D> view;
        float alpha = 0.0f;
        std::optional<Arcane::Entity> selected;
        bool physicsWholeWorld = false;
        bool playMode = false;
        std::optional<std::uint64_t> inspectorFixtureA;
        std::optional<std::uint64_t> inspectorFixtureB;
        int inspectorStepIndex = -1;
        std::optional<float> inspectorThickness;
        std::optional<float> inspectorEmphasis;
    };

    struct PhysicsColliderSilhouette2D
    {
        Arcane::Entity entity{};
        enum class Kind : std::uint8_t { Circle, Capsule, Box };
        Kind kind = Kind::Circle;
        glm::vec3 center{0.0f};
        glm::vec2 halfExtents{0.0f};
        float radius = 0.0f;
        float halfLen = 0.0f;
        float angle = 0.0f;
    };

    struct PhysicsDebugPrimitive
    {
        enum class Kind { Line, Circle } kind = Kind::Line;
        glm::vec2 a{};
        glm::vec2 b{};
        float size = 0.0f;
        glm::vec4 color{};
    };

    class PhysicsDebugGeometry
    {
    public:
        void Line(glm::vec2 a, glm::vec2 b, float thickness, glm::vec4 color)
        { primitives.push_back({PhysicsDebugPrimitive::Kind::Line, a, b, thickness, color}); }
        void Circle(glm::vec2 center, float radius, glm::vec4 color)
        { primitives.push_back({PhysicsDebugPrimitive::Kind::Circle, center, {}, radius, color}); }
        std::vector<PhysicsDebugPrimitive> primitives;
    };

    using PhysicsDebugFillFn2D =
        PhysicsDebugGeometry (*)(const Astra::Registry&, const PhysicsDebugExtractRequest2D&);
    using PhysicsSilhouetteFillFn2D =
        void (*)(const Astra::Registry&, std::vector<PhysicsColliderSilhouette2D>&);

    [[nodiscard]] ARC_SCENE_API bool SetPhysicsQueryFns2D(PhysicsDebugFillFn2D debug, PhysicsSilhouetteFillFn2D silhouettes) noexcept;
    ARC_SCENE_API PhysicsDebugGeometry FillPhysicsDebug2D(const Astra::Registry& reg, const PhysicsDebugExtractRequest2D& request);
    ARC_SCENE_API void FillColliderSilhouettes2D(const Astra::Registry& reg, std::vector<PhysicsColliderSilhouette2D>& out);
}
```

`PhysicsDebugPrimitive` and `class PhysicsDebugGeometry` move verbatim from `PhysicsDebugGeometry.hpp:78-94`. `PhysicsDebugGeometry.hpp` includes `PhysicsQuery.hpp` and deletes those two definitions. The gather functions stay where they are and still return `PhysicsDebugGeometry`. `PhysicsQuery.hpp` does not include `PhysicsDebugGeometry.hpp`.

The suffix names the dimension. D5 adds a `...3D` set beside it and never calls the 2D setter. `SetPhysicsQueryFns2D` stores the pair and returns true when both slots are null or already hold this pair. When a different non-null pair is stored, it keeps that pair, logs one error, and returns false. The Physics2D registrar casts the result to void with a comment that the test pins the refusal. `PhysicsDebugPrimitive` and `PhysicsDebugGeometry` keep their names.

`Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsQuery.cpp` stores the two pointers. `FillPhysicsDebug2D` returns a default `PhysicsDebugGeometry` when the debug pointer is null. `FillColliderSilhouettes2D` returns without writing when the silhouette pointer is null. `SetPhysicsQueryFns2D` assigns both.

`Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsQuery.cpp` defines the two targets and a namespace-scope registrar, the same shape as `ARC_MODULE_ATTACH` (`ModuleAttach.hpp:18-27`): a struct in an anonymous namespace whose constructor calls `SetPhysicsQueryFns2D`, and a namespace-scope instance of that struct. Not a function-local static. A function-local static is not initialized unless something calls the function, and this plan gives no such call. The targets are not exported. The test calls `SetPhysicsQueryFns2D` only in the refusal case. This registrar does not read `ARCANE_TEST_NO_PHYSICS_HOOK` (that flag is Task 8's component hook only). The targets may include Manifold headers. The Scene cpp may not. `modules.lua:544-549` drops an unreferenced import; once `ArcanePhysics2D` is linked, this namespace-scope initializer runs at load. Empty geometry on the v1 scene after 120 steps is a failure: it means the registrar did not run.

- If `request.view` is nullopt, return an empty geometry. This is the perspective skip (`ExtractFrame.cpp:446-447`).
- Otherwise build `PhysicsDebugDrawOptions2D` the way `ExtractFrame.cpp:433-462` does: `opts.view = *request.view`, `opts.alpha = request.alpha`, `opts.contacts = request.physicsWholeWorld`, `opts.onlyBody` from the selected entity via the entity map (packed `uint64`, which is now `ref.handle`). Apply today's draw gate: if not whole-world and (`playMode` or no body resolved), return empty before the ordinary gather.
- Ordinary path: `GatherPhysicsDebug`.
- If both inspector fixture optionals are set: `DebugCollide` into a local `NarrowphaseTrace`, then `GatherNarrowphaseWorldOverlay` with `request.inspectorStepIndex`, `*request.view`, `request.inspectorThickness`, `request.inspectorEmphasis`, and append those primitives. The trace does not leave this cpp.
- `FillColliderSilhouettes2D` copies the collider pass `CollectPickables` builds (`ExtractFrame.cpp:176` onward, solver via `Access` at 193–195), into `PhysicsColliderSilhouette2D`. Same scale (`appliedScale`) and same pose reads. Do not simplify that math. Append; do not clear `out` first (same contract as `CollectPickables`).

`Kind::Line` stores thickness in `size`. `Kind::Circle` stores radius in `size` and the center in `a`. That is `PhysicsDebugGeometry::Line` / `Circle` (`PhysicsDebugGeometry.hpp:89-92` before the move). There is no second primitive type.

- [ ] **Step 1: Write the failing proxy tests**

`PhysicsQueryBaselineTest.cpp` includes `Tests/ArcaneTests/src/Helpers/PhysicsV1Baseline.hpp` (Task 1). It does not paste `BuildV1PhysicsScene`. It builds that scene, steps 120, and calls `FillPhysicsDebug2D` three times (whole-world, selected, alt) plus one inspector request whose packed fixture ids are parsed from the `fixtureA` / `fixtureB` header of `v1-inspector.txt`. Each result dumped with Task 2's line format equals the checked-in file. A fourth call with `view = nullopt` returns zero primitives. `FillColliderSilhouettes2D` dumped with Task 2's pick format equals `v1-pick.txt`. The test calls `SetPhysicsQueryFns2D` only in the refusal case.

```cpp
TEST_CASE("two debug requests do not share one list", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(*rt.Registry());
    for (int i = 0; i < 120; ++i)
        rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
    const std::string whole = DumpPhysicsDebug(FillPhysicsDebug2D(*rt.Registry(), WholeWorldRequest()));
    const std::string alt = DumpPhysicsDebug(FillPhysicsDebug2D(*rt.Registry(), AltViewRequest()));
    CHECK(whole == ReadFileBinary(Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/physics/v1-debug-wholeworld.txt"));
    CHECK(alt == ReadFileBinary(Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/physics/v1-debug-altview.txt"));
    CHECK(whole != alt);
    CHECK_FALSE(whole.empty()); // empty means the namespace-scope registrar did not run
}
```

```cpp
TEST_CASE("a second 2D query registration is refused", "[physics][proxy]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(*rt.Registry());
    for (int i = 0; i < 120; ++i)
        rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
    constexpr Arcane::PhysicsDebugFillFn2D fake =
        [](const Astra::Registry&, const Arcane::PhysicsDebugExtractRequest2D&) { return Arcane::PhysicsDebugGeometry{}; };
    CHECK_FALSE(Arcane::SetPhysicsQueryFns2D(fake, nullptr));
    CHECK(DumpPhysicsDebug(Arcane::FillPhysicsDebug2D(*rt.Registry(), WholeWorldRequest())) ==
          ReadFileBinary(Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/physics/v1-debug-wholeworld.txt"));
}
```

`BuildV1PhysicsScene(Arcane::Registry&)` is the Task 1 declaration. `DumpPhysicsDebug` is the dumper Task 2 declared in the same header (the line format above). `WholeWorldRequest` and `AltViewRequest` are local to this test and fill `PhysicsDebugExtractRequest2D` with the affines and flags Task 2 used to write those two files. Selected, inspector, null view, and pick are further `TEST_CASE`s in this file with the same shape: call the proxy, compare to the checked-in file, and fail on empty output. Do not invent a second scene builder.

- [ ] **Step 2: Build, then stop on the compile error**

Desk build. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names `FillPhysicsDebug2D` as undeclared. Do not run `ArcaneTests.exe`. That compile error is the red. It is not an assertion, and it is not a missing-file failure from the previous exe.

- [ ] **Step 3: Implement the cpp**

The Physics2D cpp includes `Physics2DDetail.hpp`, `PhysicsDebugGeometry.hpp`, and the Manifold world header. It does not include `PhysicsDebugDraw.hpp` (that header is Render). The Scene cpp and `PhysicsQuery.hpp` include none of those. Add the scan:

```cpp
TEST_CASE("PhysicsQuery.hpp names no solver type", "[physics][proxy]")
{
    const std::string text = ReadFileBinary(Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsQuery.hpp");
    for (const char* bad : { "Manifold2D", "NarrowphaseTrace", "PhysicsWorld", "BodyHandle", "FixtureHandle" })
        CHECK(text.find(bad) == std::string::npos);
}
```

- [ ] **Step 4: Build and run the proxy cases plus the trace**

Desk build. Expected: `ExitCode` 0. Then, from the exe directory, the proxy cases and the trace compare. Expected: the three debug files, the inspector file, and the pick file all match. `view == nullopt` is empty. A non-empty v1 result is required, which is what shows the registrar ran. Trace compare still PASS.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Expected: exit 0. Do not switch `ExtractFrame.cpp` yet. Do not run the golden gate. Do not launch Servitor in this task: `ExtractFrame.cpp` does not call these proxies yet. Task 7 is the host check for this path.

- [ ] **Step 5: Commit**

```
git add Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsQuery.hpp Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsQuery.cpp Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsQuery.cpp Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugGeometry.hpp Tests/ArcaneTests/src/PhysicsQueryBaselineTest.cpp
git commit -m "feat: add solver-free physics debug and pick proxies"
```

with the trailer.

---

### Task 7: Retarget Extract and drop Render's physics edge

**Files:**
- Move: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugTypes.hpp` to `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugTypes.hpp` (same include string). Delete the Physics2D copy.
- Create: `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugDrawOptions.hpp` by moving `struct PhysicsDebugDrawOptions2D` out of `PhysicsDebugGeometry.hpp` (the struct starts at line 16).
- Modify: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugGeometry.hpp` so it includes that Scene header and no longer defines the options struct. Gather functions stay here.
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp` (pick pass 2 around line 176, debug around 433–462, includes at lines 14 and 21)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/PhysicsDebugDraw.hpp` and `PhysicsDebugDraw.cpp` (paint loop stays; the two Manifold entry points leave; the header stops including `PhysicsDebugGeometry.hpp`)
- Modify: `Source/Client/ArcaneRender/ArcaneRender.arcmodule` (remove `ArcanePhysics2D` from the public list; line 8 today)
- Create: `Tests/ArcaneTests/src/Helpers/PhysicsDebugDrawSolver.hpp` (test wrappers that keep today's `DrawPhysicsDebug` / `DrawNarrowphaseWorldOverlay` calls)
- Modify: `Tests/ArcaneTests/src/RenderInterpolationTest.cpp`, `PhysicsDebugTransformTest.cpp`, `PhysicsDebugCapsuleTest.cpp`, `PhysicsDebugRichTest.cpp`, `SweepPhysicsDebugTest.cpp` (include the helper)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/PickEmit.hpp:6` so the comment says the walk lives in `ExtractFrame.cpp:137`, and collider pass 2 calls `FillColliderSilhouettes2D`.
- Test: the Task 2 compare cases still pass, now through Extract's request builder, plus a source scan.

**Interfaces:**
- Consumes: `FillPhysicsDebug2D` and `FillColliderSilhouettes2D` from Task 6. `PhysicsDebugGeometry`.
- Produces: `ExtractFrame.cpp` includes `PhysicsQuery.hpp` and does not include `Physics2DDetail.hpp`, `Physics2DHandles.hpp`, `PhysicsSystem.hpp`, or any `Manifold2D/` header. Collider silhouettes become `PickDrawable` inside Extract (Render already owns `PickDrawable`). Debug primitives are copied into the overlay with the same `Line` / `Circle` loop. No function declared in a Render header takes a `PhysicsWorld` or a `NarrowphaseTrace`.

`DrawPhysicsDebug` and `DrawNarrowphaseWorldOverlay` are Render functions today (`PhysicsDebugDraw.hpp:110` and `:139`, bodies at `PhysicsDebugDraw.cpp:32` and `:39`). They call `GatherPhysicsDebug` / `GatherNarrowphaseWorldOverlay`, which are `ARC_PHYSICS2D_API`. Leaving those calls in the Render cpp after this task drops the module edge is an unresolved import. Extract does not call them. It calls `FillPhysicsDebug2D` and `PaintPhysicsDebug`.

`PaintPhysicsDebug` is today's `DrawGeometry` (`PhysicsDebugDraw.cpp:21-30`) exported:

```cpp
ARC_RENDER_API void PaintPhysicsDebug(Batcher2D& batcher, const PhysicsDebugGeometry& geometry);
```

Delete `DrawPhysicsDebug` and `DrawNarrowphaseWorldOverlay` from the Render header and cpp. Recreate them as inline wrappers in `Tests/ArcaneTests/src/Helpers/PhysicsDebugDrawSolver.hpp`, inside `namespace Arcane`, with the same parameters they have now. Each wrapper gathers into a `PhysicsDebugGeometry` and calls `PaintPhysicsDebug`. That helper may include `PhysicsDebugGeometry.hpp`. The five test cpps listed above include the helper. `MakePhysicsDebugDrawOptions` stays in Render (`PhysicsDebugDraw.hpp:83`, body `PhysicsDebugDraw.cpp:11-18`) and returns `PhysicsDebugDrawOptions2D`.

Do these moves before deleting the arcmodule edge. Dropping the edge while the includes stay does not compile:

- `PhysicsDebugDraw.hpp:31` includes Physics2D's `PhysicsDebugGeometry.hpp`.
- `PhysicsDebugDrawOptions2D` is declared in that header (`PhysicsDebugGeometry.hpp:16`).
- `PhysicsDebugSettings.hpp:17` includes `<Arcane/Scene/PhysicsDebugTypes.hpp>`, and that file is `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugTypes.hpp` until this task moves it. It includes `Settings.hpp` and names no Manifold type.
- `ArcaneRender.arcmodule`'s public list includes `ArcanePhysics2D` and `ArcaneScene`. Physics2D already depends on Scene, so the moved headers resolve for Physics2D without a new edge.

After the move, `PhysicsDebugDraw.hpp` includes `PhysicsDebugDrawOptions.hpp` and `PhysicsQuery.hpp` and does not include `PhysicsDebugGeometry.hpp`. `PhysicsDebugSettings.hpp` keeps its include; the file it names now lives in ArcaneScene. Gather functions stay in Physics2D.

Build `PhysicsDebugExtractRequest2D` from the existing `ExtractRequest` fields (`physicsDebug`, the sprite view's `Affine2D`, `RenderContext2D` alpha or 0, selected entity, `physicsWholeWorld`, `playMode`). Inspector fields stay empty on this path unless `ExtractRequest` already carries them. Do not add an editor inspector UI. The inspector snapshot is pinned by calling `FillPhysicsDebug2D` directly in Task 6; Extract's production path is the ordinary debug path.

After this task `ArcaneRender.arcmodule` has no `ArcanePhysics2D` edge. A private edge is not a substitute (the spec's L13 note: a private edge is still copied onto consumer include paths).

- [ ] **Step 1: Write the failing include scan**

```cpp
TEST_CASE("ArcaneRender includes no physics solver header", "[physics][proxy]")
{
    const auto root = Arcane::Test::RepoRoot() / "Source/Client/ArcaneRender";
    const std::vector<std::string> banned = {
        "Physics2DDetail.hpp", "Physics2DHandles.hpp", "PhysicsSystem.hpp",
        "PhysicsDebugGeometry.hpp", "Manifold2D/"
    };
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension();
        if (ext != ".hpp" && ext != ".cpp" && ext != ".inl") continue;
        const std::string text = ReadFileBinary(entry.path());
        for (const auto& ban : banned)
            INFO(entry.path().string()), CHECK(text.find(ban) == std::string::npos);
    }
    const std::string module = ReadFileBinary(root / "ArcaneRender.arcmodule");
    CHECK(module.find("ArcanePhysics2D") == std::string::npos);
    const auto sceneTypes = Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugTypes.hpp";
    const auto physicsTypes = Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugTypes.hpp";
    CHECK(std::filesystem::exists(sceneTypes));
    CHECK_FALSE(std::filesystem::exists(physicsTypes));
    CHECK(std::filesystem::exists(Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugDrawOptions.hpp"));
}
```

- [ ] **Step 2: Build, then run the scan**

The scan only reads files. It compiles against today's tree. Desk build. Expected: `ExitCode` 0. Then, from the exe directory:

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "ArcaneRender includes no physics solver header" --rng-seed 1
```

Expected: assertion failure, not a compile error. The log names `ExtractFrame.cpp` (a banned include), `PhysicsDebugDraw.hpp` (`PhysicsDebugGeometry.hpp`), and `ArcaneRender.arcmodule` (`ArcanePhysics2D`). `PhysicsDebugTypes.hpp` still exists under ArcanePhysics2D, so `CHECK_FALSE(exists(physicsTypes))` fails too. Do not describe this as a compile failure.

- [ ] **Step 3: Retarget Extract**

Move `PhysicsDebugTypes.hpp` and `PhysicsDebugDrawOptions2D` as the Interfaces section says, and point `PhysicsDebugDraw.hpp` at the Scene headers, before removing the edge.

Replace the `Access::Solver` collider walk with `FillColliderSilhouettes2D`, then convert each `PhysicsColliderSilhouette2D` into the `PickDrawable` the rest of the function already pushes. Keep sprite pass 1 untouched.

Replace the debug block with one `FillPhysicsDebug2D` call and `PaintPhysicsDebug` on that geometry. Delete the Manifold includes and the `Access::Solver` reads in `ExtractFrame.cpp`. Remove `ArcanePhysics2D` from `ArcaneRender.arcmodule`. Desk build so the link fails if a TU still calls `GatherPhysicsDebug` or `DrawPhysicsDebug` from a Render object file. Fix includes until it links. Do not add the edge back. The test helper is the only remaining caller of the gather functions outside ArcanePhysics2D and the solver tests.

- [ ] **Step 4: Scan, baselines, suite**

Desk build. Expected: `ExitCode` 0. The include scan PASS. Task 2's five compare cases PASS (they call gather directly, so they still pass). Add one case that runs `ExtractFrame` on the v1 scene with `physicsDebug` set and checks the packet's overlay physics primitives equal `v1-debug-wholeworld.txt`. Use the existing `ExtractFrame` test harness in `ExtractFrameTest.cpp` for how a packet is built; do not invent a second extract entry point. Trace compare PASS.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Expected: exit 0. No golden gate yet.

Servitor, from the repo root. Client name `plan-a`. Quit even if a later command fails. Record every exit code.

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-a --host runtime wait settled
& $S --project ReferenceProject --client plan-a --host runtime step 30
& $S --project ReferenceProject --client plan-a --host runtime resource list
& $S --project ReferenceProject --client plan-a --host runtime compare runtime-scene --source window
& $S --project ReferenceProject --client plan-a --host runtime quit
```

Expected: every exit 0. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way. `resource list` contains `Arcane::PhysicsWorld2D`. `compare` exit 8 is a pixel miss; stop and do not bless. Exit 7 means the host died.

- [ ] **Step 5: Commit**

```
git add Source/Client/ArcaneRender Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugTypes.hpp Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsDebugDrawOptions.hpp Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugGeometry.hpp Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugTypes.hpp Tests/ArcaneTests/src/Helpers/PhysicsDebugDrawSolver.hpp Tests/ArcaneTests/src/RenderInterpolationTest.cpp Tests/ArcaneTests/src/PhysicsDebugTransformTest.cpp Tests/ArcaneTests/src/PhysicsDebugCapsuleTest.cpp Tests/ArcaneTests/src/PhysicsDebugRichTest.cpp Tests/ArcaneTests/src/SweepPhysicsDebugTest.cpp Tests/ArcaneTests/src/PhysicsQueryBaselineTest.cpp
git commit -m "refactor: extract physics debug and pick through solver-free proxies"
```

with the trailer.

---

### Task 8: SimHook, host link, and the boot census

**Files:**
- Create: `Source/Runtime/ArcaneCore/src/Arcane/Sim/SimHook.hpp`
- Create: `Source/Runtime/ArcaneCore/src/Arcane/Sim/SimHook.cpp`
- Create: `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DSimHook.cpp`
- Modify: `ProcessContext.cpp`, `ProcessContext::Create`, between `AttachAllModules` and `PrewarmEngineResourceTypes` (lines 30-34 on `a12f33e6`; fix Task 12 edits this file, so re-find both calls)
- Create: `Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsGravity2D.hpp` and `PhysicsGravity2D.cpp` (`ARC_SCENE_API glm::vec2 ResolveGravity2D(const Astra::Registry&)`: the body of `Runtime.cpp:618-626`, moved verbatim)
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.cpp:618-626` (`ResolvedGravity` forwards to `ResolveGravity2D`) and `:8` (drop the `Physics2DBinding.hpp` include if nothing else uses it)
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.hpp:389-393` (comment only: the resolution lives in `ResolveGravity2D`; the declaration at `:394` does not change)
- Modify: `Tests/ArcaneTests/src/MacroPrefixGuardTest.cpp` `kEnvNames`: add `"ARCANE_TEST_NO_PHYSICS_HOOK"` (`Physics2DSimHook.cpp` and `PhysicsHookBootTest.cpp` read it).
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.cpp` (roster 326, frame begin 370–376, `InstallEngineSystems` 606–611, `EnsurePhysics` 628–655, `PhysicsEditPass` 657–661, `ResetPhysics` 663–672)
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Scene/EngineRoster.hpp:23` and `:32-37` (drop the three physics types)
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Scene/EngineResourceTypes.cpp:3` and `:21` (drop `PhysicsWorld2D`)
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Project/ProjectHost.hpp:116-123` (walk the concatenation)
- Modify: `Source/Runtime/ArcaneEngine/ArcaneEngine.arcmodule` (drop `ArcanePhysics2D`)
- Modify: `build/modules.lua:571` and `GameDescriptor.hpp:23-25` (array size 6, append `"ArcanePhysics2D"`)
- Modify: `premake5.lua` ArcaneServer consumer (line 959) and ArcaneRuntime consumer (line 1042): add `"ArcanePhysics2D"` to the **names** list, not only `stage`
- Modify: `Tests/ArcaneTests/src/NamespaceFacadeTest.cpp:123-137` to lock the concatenation
- Modify: `Tests/ArcaneTests/src/ProcessContextTest.cpp:61-69` comment: `PhysicsWorld2D` stays in the pin (the physics prewarm touches it) but it is no longer in `EngineResourceTypes.cpp`. The comment on line 61 changes to say that. Presence and stability stay. Id order of resource types is not asserted.
- Modify: `fuzz/scene_fuzz.cpp:42` so it still calls `RegisterPhysicsComponents2D` (the function remains)
- Modify: `Source/Programs/ArcaneServer/src/ServerReport.hpp`, `ServerReport.cpp`, and `ServerApp.cpp` (additive `hasPhysicsWorld`; `kSchemaVersion` stays 1)
- Modify: `Tests/ArcaneTests/src/ServerWitnessTest.cpp:40` and `:48` (schema stays 1; assert `hasPhysicsWorld`)
- Test: `Tests/ArcaneTests/src/PhysicsHookBootTest.cpp` (new), adding "EnsureWorld resolves its own gravity and Engine passes none", "ResetWorld removes the world and the interp buffer it minted", "SimHooks run in SimHook::order". It includes `Helpers/PhysicsV1Baseline.hpp`.

**Interfaces:**
- Consumes: `RegisterModuleAttach` shape (`ModuleAttach.hpp:13-27`). `SystemFactoryTable::Add` drops entries when no owner is open (`SystemFactory.hpp:127-131`). Do not use it for this hook. `ComponentID` is `uint16_t` (`ThirdParty/Astra/include/Astra/Component/Component.hpp:13`), so `TypeID<WorldBounds>::Value() + 1` is a valid id.
- Produces:

```cpp
namespace Arcane
{
    struct SimHook
    {
        std::uint32_t order = 0; // ascending; RegisterComponents runs in this order, so component ids do not depend on DLL load order
        void (*Prewarm)() noexcept = nullptr;
        void (*RegisterComponents)(Astra::ComponentRegistry&) noexcept = nullptr;
        void (*InstallSystems)(Astra::SystemScheduler& fixed, float fixedDt) noexcept = nullptr;
        void (*FrameBegin)(Astra::Registry&) noexcept = nullptr;
        void (*EnsureWorld)(Astra::Registry&, Mosaic::IWorkScheduler* scheduler) noexcept = nullptr;
        void (*EditPass)(Astra::Registry&, float fixedDt) noexcept = nullptr;
        void (*ResetWorld)(Astra::Registry&) noexcept = nullptr;
        void (*ReplaceFixedRate)(Astra::SystemScheduler& fixed, float fixedDt) noexcept = nullptr;
    };

    ARC_CORE_API void RegisterSimHook(SimHook hook) noexcept;
    ARC_CORE_API std::span<const SimHook> SimHooks() noexcept;
    ARC_CORE_API void ReplaceSimHooksForTest(std::span<const SimHook> hooks) noexcept;
}
```

`RegisterSimHook` inserts in ascending `order`. Equal orders keep registration order. `ReplaceSimHooksForTest` sorts the same way. Physics2D registers `order = 100`. D5's 3D hook takes `200`.

`ResetWorld` is on this same record because `ResetPhysics` (`Runtime.cpp:663-672`) names `PhysicsWorld2D`. It is not a second registration mechanism. The spec's three roles are prewarm, component registration, and systems; `EnsureWorld`, `EditPass`, `FrameBegin`, and `ResetWorld` are the system-side calls Runtime already makes.

Physics2D's static registrar (in `Physics2DSimHook.cpp`) registers one hook unless `ARCANE_TEST_NO_PHYSICS_HOOK=1`. The hook functions:

- `Prewarm`: `(void)Astra::TypeID<PhysicsWorld2D>::Value();`
- `RegisterComponents`: `RegisterPhysicsComponents2D(creg);`
- `InstallSystems`: if `!fixed.HasSystem<PhysicsSystem2D>()`, `fixed.AddSystem<PhysicsSystem2D>(fixedDt, true);`
- `FrameBegin`: if `PhysicsWorld2D* res = reg.GetResource<PhysicsWorld2D>()`, `res->BeginFrame();`
- `EnsureWorld`: the body of `EnsurePhysics` from line 631 down, with `const glm::vec2 g = ResolveGravity2D(reg);` in place of `ResolvedGravity()`. Engine passes no gravity. `ToWorldDef` stays in this TU. `SetExecutor(scheduler)` only when `Settings<PhysicsWorldSettings2D>().parallelSolver` is true. The hook does not construct a `JobSystem`. `scheduler` is the work scheduler `EnsurePhysics` passes to `SetExecutor` in the tree you start from (after fix Task 12, the process `JobSystem`'s scheduler), including when the flag is false (the hook decides).
- `EditPass`: `PhysicsSystem2D{fixedDt, false}(reg);`
- `ResetWorld`: `reg.RemoveResource<PhysicsWorld2D>(); reg.RemoveResource<PhysicsInterpBuffer2D>();` The hook removes the pair its `EnsureWorld` mints.
- `ReplaceFixedRate`: `fixed.RemoveSystem<PhysicsSystem2D>()` then `fixed.AddSystem<PhysicsSystem2D>(fixedDt, true)`. This is what `Runtime::SetFixedHz` does today by name (`Runtime.cpp:444-455`): it stores the new `fixedHz`, calls `loop->SetFixedHz`, then `RemoveSystem<PhysicsSystem2D>()` and `InstallEngineSystems()`. `InstallEngineSystems` (`Runtime.cpp:590-616`) only `AddSystem`s when `!HasSystem`, and the system stores `m_fixedDt` privately (`Physics2DWorld.hpp:113-120`) and steps `world.Step(m_fixedDt)`. After this task `SetFixedHz` updates `loopCfg` and the loop, then calls every hook's `ReplaceFixedRate(fixedUpdate, static_cast<float>(1.0 / hz))`. It does not name `PhysicsSystem2D`. It does not need a second `InstallEngineSystems` call to refresh that one system. `SweepSimTest.cpp:187-208` stays green.

Server report, additive. `kSchemaVersion` stays 1 (`ServerReport.hpp:26`). `ServerWitnessTest.cpp:40` keeps `schemaVersion == 1`. Add `bool hasPhysicsWorld = false` next to `hasPhysics` (`ServerReport.hpp:51`). `ToJson` adds `"hasPhysicsWorld"` inside the `systems` object (`ServerReport.cpp:29-34`). `ServerApp.cpp` sets it from `GetResource<PhysicsWorld2D>() != nullptr` after the tick that calls `EnsurePhysics` (`ServerApp.cpp:279` is the tick; `hasPhysics` at line 332 stays `HasSystem` and is not the world check). `ServerWitnessTest` asserts `systems.hasPhysicsWorld == true` on the `--frames 30` run. An absent field or `false` fails. Do not bump the schema.

Call sites:

- `ProcessContext::Create`, after `AttachAllModules` (line 30) and before `PrewarmEngineResourceTypes` (line 34): each hook's `Prewarm`.
- `Runtime` ctor, immediately after `RegisterRoster` (line 326), on the `ComponentRegistry` (the `Registry` does not exist yet; it is created at line 328): each hook's `RegisterComponents`. A secondary world that shares the registry runs the same function. `ClearSystems` does not call it.
- `InstallEngineSystems`: delete the `PhysicsSystem2D` block (lines 607–611) and call each hook's `InstallSystems(fixed, fixedDt)` in that slot, before transform propagation. `PhysicsSystem2D` still declares `Before<TransformPropagationSystem>`.
- Frame-begin lambda (lines 373–376): call each hook's `FrameBegin` instead of naming `PhysicsWorld2D`.
- `EnsurePhysics`: Engine resolves no gravity. For each hook, `EnsureWorld(*registry, that same scheduler)`. `Runtime::ResolvedGravity` stays public (`Runtime.hpp:394`, `ClientRuntime.hpp:172`, the tests at `RuntimeTest.cpp:217-230`, `SweepSimTest.cpp:192`, `SweepProjectStoresTest.cpp:103`) and returns `ResolveGravity2D(*registry)`. The hook and the query read one function. Engine does not mention `Manifold2D`, `Access`, or `ToWorldDef`.
- `PhysicsEditPass`: hook `EditPass` only.
- `ResetPhysics`: each hook's `ResetWorld`. Engine names no interp buffer.

`EngineComponentRoster` becomes the ten scene types, ending at `WorldBounds`. Add `PhysicsComponentRoster2D` in `PhysicsComponents.hpp`:

```cpp
using PhysicsComponentRoster2D = TypeList<RigidBody2D, Collider2D, PhysicsBodyRef2D>;
```

`TypeList` today lives in `EngineRoster.hpp`. Move `TypeList` to `Source/Runtime/ArcaneScene/src/Arcane/Scene/TypeList.hpp` (header-only, no macro) so Scene can name the physics roster without including Engine. `EngineRoster.hpp` includes it. `VerifySharedTypeContext` walks scene roster then `PhysicsComponentRoster2D`, still thirteen types. `NamespaceFacadeTest.cpp:123-137` locks that concatenation, not a single alias that includes the three inside `EngineComponentRoster`.

`ProjectHost.hpp` includes `PhysicsComponents.hpp` (Scene) and does not include a physics module header.

Default game modules, both lists, in this order (append, do not reorder the existing five):

```
"ArcaneClient", "ArcaneImGui", "ImGui", "ArcaneNet", "ArcaneCrypto", "ArcanePhysics2D"
```

`GameContractTest.cpp` asserts they match. `ClassTemplatesTest.cpp:248` iterates the array. ReferenceGame's `.arcmodule` already lists `ArcanePhysics2D`; leave it. `arcbuild` links Engine and does not simulate; do not add Physics2D to arcbuild.

Server (`premake5.lua:959`) and Runtime (`premake5.lua:1042`) add the name to the linked list. Editor and ArcaneTests already consume `engine_modules.order`; confirm the generated project still links `ArcanePhysics2D` after Engine and Render drop the edge. Remove the Manifold **include dir** from ArcaneServer and ArcaneRuntime if the only reason for it was `PhysicsSystem.hpp` (the comments at `premake5.lua:975-979` and `:1059-1066`). Keep the Mosaic include dir. A stray Manifold include then fails the compile, which is the scan's job.

Linking the DLL still adds Manifold include dirs to a consumer of Physics2D. The scan forbids `#include`, not include dirs, on Engine, Render, Client, Editor, and Server.

- [ ] **Step 1: Write the failing boot tests**

```cpp
TEST_CASE("physics component ids are the three integers after WorldBounds", "[physics][hook]")
{
    // SimHooks() is undeclared until Step 3. That compile error is the red.
    if (const char* e = std::getenv("ARCANE_TEST_NO_PHYSICS_HOOK"); e && e[0] == '1')
    {
        CHECK(Arcane::SimHooks().empty());
        Arcane::Runtime rt(Arcane::Test::Process());
        const auto* creg = rt.Registry()->GetComponentRegistry();
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
    const auto* creg = rt.Registry()->GetComponentRegistry();
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

#if defined(_WIN32)
    // Child is the only Runtime in its process. Random order in THIS process
    // would already have minted the ids, so the absence check cannot live here.
    REQUIRE(SpawnNoHookChild() != 0);
#endif
}
```

`SpawnNoHookChild` runs `ArcaneTests.exe "physics component ids are the three integers after WorldBounds" --rng-seed 1` with `ARCANE_TEST_NO_PHYSICS_HOOK=1` and returns the exit code. The child must exit non-zero because `REQUIRE(id.IsOk())` fails. Do not call `TypeID<RigidBody2D>::Value()` before that `REQUIRE` in the child: `Value()` would mint the id. `GetComponentIDFromHash` does not.

On non-Windows the positive `REQUIRE(IsOk())` still runs. The child spawn is the Windows desk proof; linux CI runs the same `#if` only where `popen` is used. Implement the spawn with `popen` everywhere `popen` exists, and `_popen` on Win32. Both compile. The child command uses `Arcane::Platform::ExecutableFileName` if that helper is what `ServerWitnessTest.cpp` uses for the exe name; otherwise `ArcaneTests.exe` on Windows and `ArcaneTests` on POSIX, from the current directory (the suite runs from the exe dir).

Second test, in the same file:

```cpp
TEST_CASE("BeginFrame clears frameEvents and leaves stepEvents until the next step", "[physics][hook]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    BuildV1PhysicsScene(*rt.Registry());
    rt.EnsurePhysics();
    rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
    auto* world = rt.Registry()->GetResource<Arcane::PhysicsWorld2D>();
    REQUIRE(world != nullptr);
    const auto stepBefore = world->StepEvents();
    const bool stepHadRows = !stepBefore.contactBegin.empty() || !stepBefore.contactEnd.empty()
        || !stepBefore.contactHit.empty() || !stepBefore.sensorBegin.empty()
        || !stepBefore.sensorEnd.empty();
    world->BeginFrame();
    const auto frame = world->FrameEvents();
    CHECK(frame.contactBegin.empty());
    CHECK(frame.contactEnd.empty());
    CHECK(frame.contactHit.empty());
    CHECK(frame.sensorBegin.empty());
    CHECK(frame.sensorEnd.empty());
    if (stepHadRows)
    {
        const auto stepAfter = world->StepEvents();
        const bool still = !stepAfter.contactBegin.empty() || !stepAfter.contactEnd.empty()
            || !stepAfter.contactHit.empty() || !stepAfter.sensorBegin.empty()
            || !stepAfter.sensorEnd.empty();
        CHECK(still); // BeginFrame must not clear stepEvents
    }
}
```

The v1 scene produces contacts inside 120 steps (Task 1). One step may be too early for a contact. If `stepHadRows` is false after one step, advance until a step has a non-empty `StepEvents()` span or 120 steps have passed, then `BeginFrame`. The check is on that window. Do not change `BeginFrame` to clear `stepEvents`.

Third test: a directory walk of `Source/Runtime/ArcaneEngine`, `Source/Client/ArcaneRender`, `Source/Client/ArcaneClient`, `Source/Programs/ArcaneEditor`, `Source/Programs/ArcaneServer` fails if a `.hpp`/`.cpp` contains `Physics2DDetail.hpp`, `Physics2DHandles.hpp`, `PhysicsSystem.hpp`, `PhysicsDebugGeometry.hpp`, or `Manifold2D/`. `EngineResourceTypes.cpp` is in that walk.

Fifth test, in the same file: add a dummy system to `fixedUpdate`, call `SetFixedHz(30)`, and `CHECK` that `HasSystem<PhysicsSystem2D>()` is still true and the dummy is still present. Advance one step of a dynamic body under default gravity and `CHECK` the velocity is about `g/30` (the shape `SweepSimTest.cpp:187-208` already uses). `PhysicsHookBootTest.cpp` includes `Helpers/PhysicsV1Baseline.hpp` for `BuildV1PhysicsScene`.

Sixth test: after `EnsurePhysics` and one `Advance`, `GetResource<PhysicsWorld2D>() != nullptr`. A null resource fails. Do not treat an empty entity query as this check.

Fourth test: the same walk of the spelling-guard's `kGameFacingHeaders` fails if a **public declaration** line names `Manifold2D`. Reuse `Scan` from `ArcaneSpellingGuardTest.cpp` (`ScanMode::PublicDeclarations`). A comment or an `ARC_INTERNAL` fence does not count. Calling the existing scan function is required if it is in the same test binary; if it is in an anonymous namespace, duplicate the mode rules in this test (comments, strings, preprocessor lines, and `ARC_INTERNAL` fences stripped) and add one self-check: a fixture string `"Manifold2D::Vec2 x;"` hits, and `"// Manifold2D"` does not.

Seventh test, in the same file, "EnsureWorld resolves its own gravity and Engine passes none": build the v1 scene, then set the scene-root `PhysicsSettings2D::gravity = {0, 2}` (the `RuntimeTest.cpp:222-230` shape). Copy `SimHooks()`, append a recording hook (`order = 300`; `EnsureWorld = [](Astra::Registry&, Mosaic::IWorkScheduler*) noexcept { ++g_calls; }`), and `ReplaceSimHooksForTest`. Then `EnsurePhysics`, then one `Advance`. `CHECK(g_calls == 1)`, and a dynamic body's `velocity.y > 0`. Restore the saved hooks.

Eighth test, "ResetWorld removes the world and the interp buffer it minted": after `EnsurePhysics`, `REQUIRE` both the `PhysicsWorld2D` and `PhysicsInterpBuffer2D` resources. After `ResetPhysics`, `CHECK` both are null. After a second `EnsurePhysics`, both are present.

Ninth test, "SimHooks run in SimHook::order": save the hooks, `ReplaceSimHooksForTest({order 300, order 100})`, `CHECK(SimHooks()[0].order == 100)`, then restore.

- [ ] **Step 2: Build, then stop on the compile error**

The id case calls `Arcane::SimHooks()` before it reads component ids, and the Hz case calls `SetFixedHz` only after it has named `ReplaceFixedRate` through a hook the test installs with `ReplaceSimHooksForTest`. Those two symbols do not exist yet. Desk build. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names `SimHooks` or `ReplaceSimHooksForTest` as undeclared. Do not run `ArcaneTests.exe`. That compile error is the red. The id numbers are already true today via `EngineComponentRoster`, so an id-only test would pass and is not the red. After the implementation the parent passes and the child exits non-zero.

- [ ] **Step 3: Implement the hook and the link**

Write `SimHook.hpp` / `.cpp` and `Physics2DSimHook.cpp`. Rewire `Runtime.cpp` and `ProcessContext.cpp`. Split the roster. Drop the module edges. Add the linked names. Update `NamespaceFacadeTest` and the `ProcessContextTest` comment. `RuntimeEngineSystemsTest.cpp:69-80` keeps including a header that declares `PhysicsSystem2D` (`PhysicsSystem2D.hpp`) and drops its Manifold include if it has one (`PhysicsWorld.hpp` at line 23 of that file today). The `HasSystem<PhysicsSystem2D>` checks stay.

`Runtime.cpp` after this task contains none of the strings `Manifold2D`, `PhysicsSystem2D`, `PhysicsWorld2D`, `PhysicsInterpBuffer2D`, `Physics2DDetail`. Reword the comment at `Runtime.cpp:552` so it no longer names the type. Add that as a `CHECK` on the file text in `PhysicsHookBootTest.cpp`.

- [ ] **Step 4: Build, tests, Servitor, golden gate**

Desk build. Expected: exit 0.

```powershell
.\ArcaneTests.exe "physics component ids are the three integers after WorldBounds" --rng-seed 1
.\ArcaneTests.exe "BeginFrame clears frameEvents and leaves stepEvents until the next step" --rng-seed 1
.\ArcaneTests.exe "EnsureWorld resolves its own gravity and Engine passes none" --rng-seed 1
.\ArcaneTests.exe "ResetWorld removes the world and the interp buffer it minted" --rng-seed 1
.\ArcaneTests.exe "SimHooks run in SimHook::order" --rng-seed 1
.\ArcaneTests.exe "v1 step trace matches the checked-in bytes" --rng-seed 1
.\ArcaneTests.exe "Runtime installs the engine's standard systems and reinstalls them after ClearSystems" --rng-seed 1
```

Expected: all PASS. The child exit is non-zero inside the first case. Record each seed banner.

`run-tests.ps1 Debug`. Expected: exit 0, including `ServerWitnessTest` (`hasPhysics == true`).

`run-tests.ps1` includes `ServerWitnessTest`: `schemaVersion` is still 1, `hasPhysics` is true, and `hasPhysicsWorld` is true. A report with no `hasPhysicsWorld` field, or with that field false, fails. The in-process runtime case's `GetResource<PhysicsWorld2D>()` is non-null. Zero ECS rows are not a pass.

Servitor, from the repo root, on both hosts. Client name `plan-a`. Quit each host even if a later command fails. Record every exit code. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way.

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-a --host runtime wait settled
& $S --project ReferenceProject --client plan-a --host runtime step 30
& $S --project ReferenceProject --client plan-a --host runtime resource list
& $S --project ReferenceProject --client plan-a --host runtime quit
& $S --project ReferenceProject launch server --headless
& $S --project ReferenceProject --client plan-a --host server wait settled
& $S --project ReferenceProject --client plan-a --host server step 30
& $S --project ReferenceProject --client plan-a --host server resource list
& $S --project ReferenceProject --client plan-a --host server quit
```

Expected: every command exits 0. After each `step 30`, that host's `resource list` envelope has a `resources` array that contains the exact name `Arcane::PhysicsWorld2D`. An empty array, or a list that lacks that name, is a failure. Do not call `resource get` (the type is unreflected, so get returns `reflect.unreadable`, exit 6, even when the instance exists). Do not treat an `ecs query` of zero rows as the world check. Do not drive the user's editor. If `launch server` is not a supported verb, `servitor commands` (no `--client`, no host) lists the verb; use that verb and still `step` before `resource list`.

Golden gate, Debug, after deleting both `imgui.ini` files:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\golden-gate.ps1
```

Expected: 14 lanes, every `diffCount` 0. Do not pass `--bless`. If a lane is non-zero, stop. This plan does not re-bless.

- [ ] **Step 5: Commit**

```
git add Source/Runtime/ArcaneCore/src/Arcane/Sim/SimHook.hpp Source/Runtime/ArcaneCore/src/Arcane/Sim/SimHook.cpp Source/Runtime/ArcanePhysics2D Source/Runtime/ArcaneEngine Source/Runtime/ArcaneScene build/modules.lua premake5.lua Tests fuzz/scene_fuzz.cpp Source/Programs/ArcaneServer/src/ServerReport.hpp Source/Programs/ArcaneServer/src/ServerReport.cpp Source/Programs/ArcaneServer/src/ServerApp.cpp
git commit -m "feat: register 2D physics through a sim hook and drop the engine and render edges"
```

with the trailer.

---

## Self-review

- Spec 3.1 Done: include scan (Task 8), module edges and `hasPhysics` plus `hasPhysicsWorld` (Task 8), id order plus the missing-hook child (Task 8), debug/inspector/pick snapshots (Tasks 2, 6, 7), step trace before and after (Tasks 1, 4, 8), `ClearSystems` (existing test plus Task 8), `BeginFrame` window (Task 8), `SetFixedHz` keeps a game system and steps at `g/30` (Task 8), goldens (Task 8), game-facing Manifold scan (Task 8). `FillPhysicsDebug` returns `PhysicsDebugGeometry` through a Scene forwarder (Task 6). Render paints with `PaintPhysicsDebug` and does not import a Physics2D symbol (Task 7). The binary handle is dropped by `SerializationTraits`, and the captured v1 blob still loads (Task 5).
- Spec 3.2 Done: one TU defines `operator()` (Task 4), comments name `AttachAllModules` (Task 4), trace matches after that commit alone (Task 4), no epoch bump. Task 4 runs `--check` and does not `--write`. The digest update in Task 5 rebuilds Plan B's tool before `--write`. It is not an epoch bump.
- `ResetWorld` and `ReplaceFixedRate` are the extra function pointers the spec's "three functions" do not name. Both sit on the same `SimHook`. Documented in Task 8. Not a second mechanism.
- Plan B owns the codec, the epoch, and the first digest, and that digest is written before Task 4. This plan's Task 5 rebuilds the tool and then regenerates the digest after the handle change. Plan C owns fix #11's completion check. This plan does not call the arc complete.

## Review fixes

Codex review `plan-review-a-baselines-physics.md` (NEEDS REVISION). Each finding is fixed in this plan. None rejected.

1. **ABI gate out of order (Critical).** Rev 2 supersedes the "stop after Task 4" sentence. Stop after Task 3. Plan B Tasks 1 and 2 merge while `operator()` is still inline and `handle` is still `BodyHandle`. Task 4 then `--check`s and does not `--write`. Task 5 still changes the field only after that manifest exists, and it rebuilds the tool before `--write`. No epoch bump. Stated in the sequencing paragraph, in Task 4's prerequisite, and in Task 5's prerequisite.
2. **Task 4 Scene header depends on Physics2D (Critical).** Task 4 creates `PhysicsSystem2D.hpp` under ArcanePhysics2D. Task 5 moves it to ArcaneScene after `PhysicsComponents.hpp` no longer includes `Physics2DHandles.hpp`. The spelling-guard path follows the file.
3. **Task 7 cannot drop Render's Physics2D edge (Critical).** Before the edge drop, Task 7 moves `PhysicsDebugTypes.hpp` to ArcaneScene and moves `PhysicsDebugDrawOptions2D` into `PhysicsDebugDrawOptions.hpp`. `PhysicsDebugDraw.hpp` stops including `PhysicsDebugGeometry.hpp`. The include scan also bans that header and checks the Physics2D copy of `PhysicsDebugTypes.hpp` is gone. `MakePhysicsDebugDrawOptions` stays in Render. The test helper may still include the Physics2D gather header.
4. **Serialization assertion contradicts the binary path (Critical).** `Serializable false` is the JSON walk. Task 5 specializes `SerializationTraits` at version 2, migrates an explicit 16-byte v1 layout, and checks a blob captured with today's `Registry::Save` before the field change. After either load, `handle` is 0 and `appliedScale` is the saved value. `PhysicsComponentsTest.cpp:171-172` is updated the same way. `TransientResourceTraits` is not used.
5. **Task 6 registrar does not run (Critical).** Namespace-scope registrar, same shape as `ARC_MODULE_ATTACH`. The proxy test does not call `SetPhysicsQueryFns`. Empty geometry on the v1 scene after 120 steps fails.
6. **`SetFixedHz` missing from the hook (Important).** `SimHook::ReplaceFixedRate` removes and re-adds `PhysicsSystem2D` at the new dt. `Runtime::SetFixedHz` calls that hook and does not name the system. The test keeps a dummy game system and checks velocity about `g/30`. `SweepSimTest.cpp:187-208` stays.
7. **Desk build command invalid on Windows PowerShell 5.1 (Important).** Global Constraints start the build with `-PassThru`, set `PriorityClass` to `BelowNormal`, wait, and throw on `ExitCode`.
8. **Red steps run a stale exe (Important).** Every red step desk-builds first. Compile error means `ExitCode` non-zero and the log names the missing symbol; the exe is not run. Assertion means the build exited 0 and a named `CHECK` failed. Tasks 1–4 already follow this. Tasks 5–8 now do too. Task 5's red is `UnpackBody` undeclared. Task 6's red is `FillPhysicsDebug` undeclared. Task 7's red is the include-scan assertion after a successful build. Task 8's red is `SimHooks` undeclared, because an id-only test would pass on today's roster.
9. **Helpers are TU-local (Important).** Task 1 declares `BuildV1PhysicsScene` and `WriteV1StepTrace` in `Helpers/PhysicsV1Baseline.hpp`. Task 2 adds the dumpers there. Tasks 6 and 8 include that header.
10. **Host gate accepts zero rows (Important).** `ServerReport` gains `systems.hasPhysicsWorld` from `GetResource<PhysicsWorld2D>() != nullptr`. `kSchemaVersion` stays 1. `ServerWitnessTest` asserts the field true. The in-process runtime test asserts the resource is non-null. Servitor, after `step 30`, requires `resource list` to contain `Arcane::PhysicsWorld2D` on both the runtime and the server. An empty array fails. `resource get` and an empty `ecs query` are not the check.

Cross-cutting, applied here even where this review did not name them: script paths after `Set-Location` into the exe dir are root-qualified. Servitor runs after Tasks 4, 5, 7, and 8 (own `--headless` host, `launch` takes no `--client`, every exit code recorded, quit what you launched). Tasks 1–3 only add tests and checked-in bytes, so they have no Servitor step. Task 6's proxies are not called by `ExtractFrame.cpp` yet; Task 7 is that host check. Those two skips are the judgment, not an omission.

### Rev 2

Codex re-review `plan-rereview-a-baselines-physics.md` (NEEDS REVISION). The partial and both new findings are fixed. None rejected. Items the re-review marked Addressed are unchanged in substance.

1. **ABI gate still after the sim-body move (partial).** Design section 4 step 3 (design lines 443–444) measures both 3.2's move of `operator()` and 3.1's handle change against the manifest. Stop after Task 3, not after Task 4. Plan B Tasks 1 and 2 still land before Task 5, and now also before Task 4. `handle` is still `BodyHandle` when that first digest is written. Task 4 retargets `AbiManifest.hpp`'s include if it names `PhysicsSystem.hpp`, then `--check` exits 0. It does not `--write`. The system header stays in ArcanePhysics2D until Task 5 (addressed finding 2).
2. **`--write` before the tool is rebuilt (new).** Task 5 does not run the exe already in `Binaries`. It msbuilds `Source/Programs/arcane-abi-digest/arcane-abi-digest.vcxproj` (`Resolve-MSBuild`, below-normal priority), then `--write`, then the full desk build whose `--check` must exit 0. `build.ps1` is not the first build: Plan B runs `--check` inside that script, and the stale digest would fail it before the new mix was written.
3. **`UnsupportedVersion` has no setter (new).** The call at `BinaryReader.hpp:673-675` discards any return, and `m_error` is private (`:854`). Version `< MinVersion` is Astra's `UnsupportedVersion` (`:633-637`). Version `> 2` calls `ReadBytes` of `GetRemaining() + 1`, which sets `CorruptedData` (`:103-106`) without consuming the payload, and `ReadVersionedComponent` returns that error (`:685-687`). `Registry::Load` maps the archetype failure to `CorruptedData` (`ArchetypeManager.hpp:1027-1029`, `Registry.hpp:1876-1878`) before the checksum (`:1899`). The direct-reader case pins the two codes. The captured v1 blob still loads with handle 0 (addressed finding 4).
